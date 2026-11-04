// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/src/devinit.c — VBIOS DEVINIT script interpreter.
//
// Derives from nouveau, file
// drivers/gpu/drm/nouveau/nvkm/subdev/bios/init.c (linux master; reference
// copy at /tmp/pascal-ref/nouveau/bios-init/init.c — line references in the
// comments point there). The opcode handlers are faithful ports of the
// master handlers named in each function; deliberate deviations are flagged
// "SIM DEVIATION" inline:
//   1. unimplemented opcodes abort loudly (master prints a warning and
//      continues with garbage),
//   2. missing/out-of-range BIOS subtables abort loudly (master reads
//      unbounded),
//   3. a step budget and a recursion cap guard against runaway scripts
//      (master trusts the VBIOS),
//   4. delays go through a fake clock instead of udelay/mdelay, and every
//      side effect is appended to a trace so runs are diffable.
//
// The INIT_PLL family additionally ports three more master files (reference
// copies fetched under /tmp/pascal-ref/nouveau/):
//   bios-pll/pll.c        nvbios_pll_parse + the pll limits table walk
//   devinit/clk-pllgt215.c  gt215_pll_calc (the M/N/fN/P search)
//   devinit/gf100.c       gf100_devinit_pll_set, the .pll_set hook GP10x
//                         inherits via gm200_devinit (gm200.c:178-184)
//
// This is simulation, not hardware evidence (HANDOFF-PASCAL.md §6 Phase 1,
// §7 evidence rules).

#include <pascal/devinit.h>

#include <stddef.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Dispatch table (init.c:2227-2299). exec == NULL: opcode known to master
// but not implemented here — loud failure. name == NULL: byte absent from
// master's table entirely — different loud failure.
// ---------------------------------------------------------------------------

struct pg106_init_op {
	const char *name;
	void (*exec)(struct pg106_init_env *env);
};

static void op_io_restrict_prog(struct pg106_init_env *env);   // 0x32
static void op_repeat(struct pg106_init_env *env);             // 0x33
static void op_io_restrict_pll(struct pg106_init_env *env);    // 0x34
static void op_end_repeat(struct pg106_init_env *env);         // 0x36
static void op_copy(struct pg106_init_env *env);               // 0x37
static void op_not(struct pg106_init_env *env);                // 0x38
static void op_io_flag_condition(struct pg106_init_env *env);  // 0x39
static void op_io_mask_or(struct pg106_init_env *env);         // 0x3b
static void op_io_or(struct pg106_init_env *env);              // 0x3c
static void op_andn_reg(struct pg106_init_env *env);           // 0x47
static void op_or_reg(struct pg106_init_env *env);             // 0x48
static void op_idx_addr_latched(struct pg106_init_env *env);   // 0x49
static void op_io_restrict_pll2(struct pg106_init_env *env);   // 0x4a
static void op_pll2(struct pg106_init_env *env);               // 0x4b
static void op_i2c_byte(struct pg106_init_env *env);           // 0x4c
static void op_zm_i2c_byte(struct pg106_init_env *env);        // 0x4d
static void op_zm_i2c(struct pg106_init_env *env);             // 0x4e
static void op_cr_idx_adr_latch(struct pg106_init_env *env);   // 0x51
static void op_cr(struct pg106_init_env *env);                 // 0x52
static void op_zm_cr(struct pg106_init_env *env);              // 0x53
static void op_zm_cr_group(struct pg106_init_env *env);        // 0x54
static void op_condition_time(struct pg106_init_env *env);     // 0x56
static void op_ltime(struct pg106_init_env *env);              // 0x57
static void op_zm_reg_sequence(struct pg106_init_env *env);    // 0x58
static void op_pll_indirect(struct pg106_init_env *env);       // 0x59
static void op_zm_reg_indirect(struct pg106_init_env *env);    // 0x5a
static void op_sub_direct(struct pg106_init_env *env);         // 0x5b
static void op_jump(struct pg106_init_env *env);               // 0x5c
static void op_i2c_if(struct pg106_init_env *env);             // 0x5e
static void op_copy_nv_reg(struct pg106_init_env *env);        // 0x5f
static void op_zm_index_io(struct pg106_init_env *env);        // 0x62
static void op_compute_mem(struct pg106_init_env *env);        // 0x63
static void op_reset(struct pg106_init_env *env);              // 0x65
static void op_configure_mem(struct pg106_init_env *env);      // 0x66
static void op_configure_clk(struct pg106_init_env *env);      // 0x67
static void op_configure_preinit(struct pg106_init_env *env);  // 0x68
static void op_io(struct pg106_init_env *env);                 // 0x69
static void op_sub(struct pg106_init_env *env);                // 0x6b
static void op_ram_condition(struct pg106_init_env *env);      // 0x6d
static void op_nv_reg(struct pg106_init_env *env);             // 0x6e
static void op_macro(struct pg106_init_env *env);              // 0x6f
static void op_done(struct pg106_init_env *env);               // 0x71
static void op_resume(struct pg106_init_env *env);             // 0x72
static void op_strap_condition(struct pg106_init_env *env);    // 0x73
static void op_time(struct pg106_init_env *env);               // 0x74
static void op_condition(struct pg106_init_env *env);          // 0x75
static void op_io_condition(struct pg106_init_env *env);       // 0x76
static void op_zm_reg16(struct pg106_init_env *env);           // 0x77
static void op_index_io(struct pg106_init_env *env);           // 0x78
static void op_pll(struct pg106_init_env *env);                // 0x79
static void op_zm_reg(struct pg106_init_env *env);             // 0x7a
static void op_ram_restrict_pll(struct pg106_init_env *env);   // 0x87
static void op_reset_begun(struct pg106_init_env *env);        // 0x8c
static void op_reset_end(struct pg106_init_env *env);          // 0x8d
static void op_gpio(struct pg106_init_env *env);               // 0x8e
static void op_ram_restrict_zm_reg_group(struct pg106_init_env *env); // 0x8f
static void op_copy_zm_reg(struct pg106_init_env *env);        // 0x90
static void op_zm_reg_group(struct pg106_init_env *env);       // 0x91
static void op_reserved(struct pg106_init_env *env);           // 0x92
static void op_xlat(struct pg106_init_env *env);               // 0x96
static void op_zm_mask_add(struct pg106_init_env *env);        // 0x97
static void op_i2c_long_if(struct pg106_init_env *env);        // 0x9a
static void op_reserved4(struct pg106_init_env *env);          // 0xaa

static const struct pg106_init_op pg106_init_ops[256] = {
	[PG106_INIT_IO_RESTRICT_PROG]      = {"INIT_IO_RESTRICT_PROG", op_io_restrict_prog},
	[PG106_INIT_REPEAT]                = {"INIT_REPEAT", op_repeat},
	[PG106_INIT_IO_RESTRICT_PLL]       = {"INIT_IO_RESTRICT_PLL", op_io_restrict_pll},
	[PG106_INIT_END_REPEAT]            = {"INIT_END_REPEAT", op_end_repeat},
	[PG106_INIT_COPY]                  = {"INIT_COPY", op_copy},
	[PG106_INIT_NOT]                   = {"INIT_NOT", op_not},
	[PG106_INIT_IO_FLAG_CONDITION]     = {"INIT_IO_FLAG_CONDITION", op_io_flag_condition},
	[PG106_INIT_GENERIC_CONDITION]     = {"INIT_GENERIC_CONDITION", NULL},
	[PG106_INIT_IO_MASK_OR]            = {"INIT_IO_MASK_OR", op_io_mask_or},
	[PG106_INIT_IO_OR]                 = {"INIT_IO_OR", op_io_or},
	[PG106_INIT_ANDN_REG]              = {"INIT_ANDN_REG", op_andn_reg},
	[PG106_INIT_OR_REG]                = {"INIT_OR_REG", op_or_reg},
	[PG106_INIT_INDEX_ADDRESS_LATCHED] = {"INIT_INDEX_ADDRESS_LATCHED", op_idx_addr_latched},
	[PG106_INIT_IO_RESTRICT_PLL2]      = {"INIT_IO_RESTRICT_PLL2", op_io_restrict_pll2},
	[PG106_INIT_PLL2]                  = {"INIT_PLL2", op_pll2},
	[PG106_INIT_I2C_BYTE]              = {"INIT_I2C_BYTE", op_i2c_byte},
	[PG106_INIT_ZM_I2C_BYTE]           = {"INIT_ZM_I2C_BYTE", op_zm_i2c_byte},
	[PG106_INIT_ZM_I2C]                = {"INIT_ZM_I2C", op_zm_i2c},
	[PG106_INIT_TMDS]                  = {"INIT_TMDS", NULL},
	[PG106_INIT_ZM_TMDS_GROUP]         = {"INIT_ZM_TMDS_GROUP", NULL},
	[PG106_INIT_CR_INDEX_ADDRESS_LATCHED] = {"INIT_CR_INDEX_ADDRESS_LATCHED", op_cr_idx_adr_latch},
	[PG106_INIT_CR]                    = {"INIT_CR", op_cr},
	[PG106_INIT_ZM_CR]                 = {"INIT_ZM_CR", op_zm_cr},
	[PG106_INIT_ZM_CR_GROUP]           = {"INIT_ZM_CR_GROUP", op_zm_cr_group},
	[PG106_INIT_CONDITION_TIME]        = {"INIT_CONDITION_TIME", op_condition_time},
	[PG106_INIT_LTIME]                 = {"INIT_LTIME", op_ltime},
	[PG106_INIT_ZM_REG_SEQUENCE]       = {"INIT_ZM_REG_SEQUENCE", op_zm_reg_sequence},
	[PG106_INIT_PLL_INDIRECT]          = {"INIT_PLL_INDIRECT", op_pll_indirect},
	[PG106_INIT_ZM_REG_INDIRECT]       = {"INIT_ZM_REG_INDIRECT", op_zm_reg_indirect},
	[PG106_INIT_SUB_DIRECT]            = {"INIT_SUB_DIRECT", op_sub_direct},
	[PG106_INIT_JUMP]                  = {"INIT_JUMP", op_jump},
	[PG106_INIT_I2C_IF]                = {"INIT_I2C_IF", op_i2c_if},
	[PG106_INIT_COPY_NV_REG]           = {"INIT_COPY_NV_REG", op_copy_nv_reg},
	[PG106_INIT_ZM_INDEX_IO]           = {"INIT_ZM_INDEX_IO", op_zm_index_io},
	[PG106_INIT_COMPUTE_MEM]           = {"INIT_COMPUTE_MEM", op_compute_mem},
	[PG106_INIT_RESET]                 = {"INIT_RESET", op_reset},
	[PG106_INIT_CONFIGURE_MEM]         = {"INIT_CONFIGURE_MEM", op_configure_mem},
	[PG106_INIT_CONFIGURE_CLK]         = {"INIT_CONFIGURE_CLK", op_configure_clk},
	[PG106_INIT_CONFIGURE_PREINIT]     = {"INIT_CONFIGURE_PREINIT", op_configure_preinit},
	[PG106_INIT_IO]                    = {"INIT_IO", op_io},
	[PG106_INIT_SUB]                   = {"INIT_SUB", op_sub},
	[PG106_INIT_RAM_CONDITION]         = {"INIT_RAM_CONDITION", op_ram_condition},
	[PG106_INIT_NV_REG]                = {"INIT_NV_REG", op_nv_reg},
	[PG106_INIT_MACRO]                 = {"INIT_MACRO", op_macro},
	[PG106_INIT_DONE]                  = {"INIT_DONE", op_done},
	[PG106_INIT_RESUME]                = {"INIT_RESUME", op_resume},
	[PG106_INIT_STRAP_CONDITION]       = {"INIT_STRAP_CONDITION", op_strap_condition},
	[PG106_INIT_TIME]                  = {"INIT_TIME", op_time},
	[PG106_INIT_CONDITION]             = {"INIT_CONDITION", op_condition},
	[PG106_INIT_IO_CONDITION]          = {"INIT_IO_CONDITION", op_io_condition},
	[PG106_INIT_ZM_REG16]              = {"INIT_ZM_REG16", op_zm_reg16},
	[PG106_INIT_INDEX_IO]              = {"INIT_INDEX_IO", op_index_io},
	[PG106_INIT_PLL]                   = {"INIT_PLL", op_pll},
	[PG106_INIT_ZM_REG]                = {"INIT_ZM_REG", op_zm_reg},
	[PG106_INIT_RAM_RESTRICT_PLL]      = {"INIT_RAM_RESTRICT_PLL", op_ram_restrict_pll},
	[PG106_INIT_RESET_BEGUN]           = {"INIT_RESET_BEGUN", op_reset_begun},
	[PG106_INIT_RESET_END]             = {"INIT_RESET_END", op_reset_end},
	[PG106_INIT_GPIO]                  = {"INIT_GPIO", op_gpio},
	[PG106_INIT_RAM_RESTRICT_ZM_GROUP] = {"INIT_RAM_RESTRICT_ZM_GROUP", op_ram_restrict_zm_reg_group},
	[PG106_INIT_COPY_ZM_REG]           = {"INIT_COPY_ZM_REG", op_copy_zm_reg},
	[PG106_INIT_ZM_REG_GROUP]          = {"INIT_ZM_REG_GROUP", op_zm_reg_group},
	[PG106_INIT_RESERVED]              = {"INIT_RESERVED", op_reserved},
	[PG106_INIT_XLAT]                  = {"INIT_XLAT", op_xlat},
	[PG106_INIT_ZM_MASK_ADD]           = {"INIT_ZM_MASK_ADD", op_zm_mask_add},
	[PG106_INIT_AUXCH]                 = {"INIT_AUXCH", NULL},
	[PG106_INIT_ZM_AUXCH]              = {"INIT_ZM_AUXCH", NULL},
	[PG106_INIT_I2C_LONG_IF]           = {"INIT_I2C_LONG_IF", op_i2c_long_if},
	[PG106_INIT_GPIO_NE]               = {"INIT_GPIO_NE", NULL},
	[PG106_INIT_RESERVED4]             = {"INIT_RESERVED4", op_reserved4},
};

const char *
pg106_init_opcode_name(uint8_t opcode)
{
	const char *name = pg106_init_ops[opcode].name;
	return name ? name : "UNKNOWN";
}

int
pg106_init_opcode_implemented(uint8_t opcode)
{
	return pg106_init_ops[opcode].exec != NULL;
}

const char *
pg106_init_strerr(int status)
{
	switch (status) {
	case PG106_INIT_OK:            return "OK";
	case PG106_INIT_ERR_OPCODE:    return "unknown/unimplemented opcode";
	case PG106_INIT_ERR_OVERRUN:   return "read past end of BIOS image";
	case PG106_INIT_ERR_TRACE_FULL: return "trace buffer full";
	case PG106_INIT_ERR_NO_TABLE:  return "required BIOS subtable absent";
	case PG106_INIT_ERR_DEPTH:     return "sub-script nesting too deep";
	case PG106_INIT_ERR_STEPS:     return "step budget exceeded";
	case PG106_INIT_ERR_BADARG:    return "bad argument (NULL vtable/image)";
	case PG106_INIT_ERR_RANGE:     return "table index out of range";
	default:                       return "unknown status";
	}
}

// ---------------------------------------------------------------------------
// Error reporting. First failure wins; later attempts are no-ops so handlers
// can stay straight-line (mirrors nvbios_exec's bail-on-error, minus the
// kernel printks).
//
// Messages are static literals: the portable core must not pull in <stdio.h>
// or <stdarg.h> (no printf family in a kext; enforced by
// pascal/kext/scripts/symbol-audit.sh). The dynamic facts a message would
// have carried are already structured: env->error_offset / error_opcode /
// status, and handlers store the offending table index in the trace or the
// error code itself.
// ---------------------------------------------------------------------------

// Bounded copy (C11 has no strlcpy; this is the whole thing we need).
static void
strlcpy_lite(char *dst, size_t cap, const char *src)
{
	size_t o = 0;

	if (cap == 0)
		return;
	while (src[o] != '\0' && o + 1 < cap) {
		dst[o] = src[o];
		o++;
	}
	dst[o] = '\0';
}

static void
fail(struct pg106_init_env *env, int code, const char *msg)
{
	if (env->status != PG106_INIT_OK)
		return;
	env->status = code;
	env->error_offset = env->step_offset;
	env->error_opcode = env->step_opcode;
	strlcpy_lite(env->error_msg, sizeof(env->error_msg), msg);
}

static void
warn(struct pg106_init_env *env, const char *msg)
{
	env->warnings++;
	if (env->error_msg[0] == '\0')
		strlcpy_lite(env->error_msg, sizeof(env->error_msg), msg);
}

// ---------------------------------------------------------------------------
// BIOS image reads (nvbios_rd08/16/32, bounds-checked). Offsets are u16 the
// way master's are; size_t arithmetic prevents wrap at 0xffff.
// ---------------------------------------------------------------------------

static uint8_t
bi_rd08(struct pg106_init_env *env, uint16_t off)
{
	if (env->status != PG106_INIT_OK)
		return 0;
	if ((size_t)off >= env->image_size) {
		fail(env, PG106_INIT_ERR_OVERRUN,
		     "BIOS read past image end");
		return 0;
	}
	return env->image[off];
}

static uint16_t
bi_rd16(struct pg106_init_env *env, uint16_t off)
{
	return (uint16_t)(bi_rd08(env, off) |
			  ((uint16_t)bi_rd08(env, (uint16_t)(off + 1)) << 8));
}

static uint32_t
bi_rd32(struct pg106_init_env *env, uint16_t off)
{
	return (uint32_t)bi_rd08(env, off) |
	       ((uint32_t)bi_rd08(env, (uint16_t)(off + 1)) << 8) |
	       ((uint32_t)bi_rd08(env, (uint16_t)(off + 2)) << 16) |
	       ((uint32_t)bi_rd08(env, (uint16_t)(off + 3)) << 24);
}

// ---------------------------------------------------------------------------
// Parser control flow (init.c:58-82, verbatim bit semantics).
// ---------------------------------------------------------------------------

static int
init_exec(const struct pg106_init_env *env)
{
	return (env->execute == 1) || ((env->execute & 5) == 5);
}

static void
init_exec_set(struct pg106_init_env *env, int exec)
{
	if (exec)
		env->execute &= 0xfd;
	else
		env->execute |= 0x02;
}

static void
init_exec_inv(struct pg106_init_env *env)
{
	env->execute ^= 0x02;
}

static void
init_exec_force(struct pg106_init_env *env, int exec)
{
	if (exec)
		env->execute |= 0x04;
	else
		env->execute &= 0xfb;
}

// ---------------------------------------------------------------------------
// Display context accessors (init.c:88-119). master prints "script needs
// OR!!"/"script needs head" and continues with 0; we count a warning.
// ---------------------------------------------------------------------------

static int
init_or(struct pg106_init_env *env)
{
	if (init_exec(env) && env->or < 0) {
		warn(env, "script needs OR link set first");
		return 0;
	}
	return env->or < 0 ? 0 : env->or;
}

static int
init_link(struct pg106_init_env *env)
{
	if (init_exec(env) && !env->link) {
		warn(env, "script needs OR link set first");
		return 0;
	}
	return env->link == 2;
}

static int
init_head(struct pg106_init_env *env)
{
	if (init_exec(env) && env->head < 0) {
		warn(env, "script needs a head assignment first");
		return 0;
	}
	return env->head < 0 ? 0 : env->head;
}

// ---------------------------------------------------------------------------
// Trace recorder. Records are attributed to the opcode being dispatched
// (env->step_offset/step_opcode, kept by the main loop); nested sub-scripts
// attribute to the nested opcode the way a real bus snooper would.
// ---------------------------------------------------------------------------

static void
trace_append(struct pg106_init_env *env, uint8_t kind, uint32_t address,
	     uint16_t index, uint32_t value)
{
	if (env->trace_len >= env->trace_cap) {
		fail(env, PG106_INIT_ERR_TRACE_FULL,
		     "trace buffer full");
		return;
	}
	struct pg106_init_trace_rec *rec = &env->trace[env->trace_len++];
	rec->address = address;
	rec->value = value;
	rec->index = index;
	rec->script_offset = env->step_offset;
	rec->opcode = env->step_opcode;
	rec->kind = kind;
}

// ---------------------------------------------------------------------------
// Register access wrappers (init.c:143-211).
// ---------------------------------------------------------------------------

// init_nvreg (init.c:143-179): clear low two bits, then mangle head/or/link
// selector bits. GP106 is card_type GP100 >= NV_50, so the NV_50+ mangling
// always applies (master gates on card_type; this core only targets GP10x).
static uint32_t
init_nvreg(struct pg106_init_env *env, uint32_t reg)
{
	reg &= ~0x00000003u;

	if (reg & 0x80000000u) {
		reg += (uint32_t)init_head(env) * 0x800;
		reg &= ~0x80000000u;
	}

	if (reg & 0x40000000u) {
		reg += (uint32_t)init_or(env) * 0x800;
		reg &= ~0x40000000u;
		if (reg & 0x20000000u) {
			reg += (uint32_t)init_link(env) * 0x80;
			reg &= ~0x20000000u;
		}
	}

	if (reg & ~0x00fffffcu) {
		// master: warn("unknown bits in register 0x%08x") and continue.
		warn(env, "unknown bits in register value");
	}

	return reg;
}

// init_rd32 (init.c:181-189).
static uint32_t
init_rd32(struct pg106_init_env *env, uint32_t reg)
{
	reg = init_nvreg(env, reg);
	if (reg != ~0u && init_exec(env))
		return env->vt->rd32(env, reg);
	return 0x00000000;
}

// init_wr32 (init.c:191-198), plus the trace record.
static void
init_wr32(struct pg106_init_env *env, uint32_t reg, uint32_t val)
{
	reg = init_nvreg(env, reg);
	if (reg != ~0u && init_exec(env)) {
		env->vt->wr32(env, reg, val);
		trace_append(env, PG106_INIT_TRACE_MMIO, reg, 0, val);
	}
}

// init_mask (init.c:200-211): tmp = rd; wr = (tmp & ~mask) | val; return tmp.
static uint32_t
init_mask(struct pg106_init_env *env, uint32_t reg, uint32_t mask, uint32_t val)
{
	reg = init_nvreg(env, reg);
	if (reg != ~0u && init_exec(env)) {
		uint32_t tmp = env->vt->rd32(env, reg);
		uint32_t out = (tmp & ~mask) | val;
		env->vt->wr32(env, reg, out);
		trace_append(env, PG106_INIT_TRACE_MMIO, reg, 0, out);
		return tmp;
	}
	return 0x00000000;
}

// init_rdvgai/init_wrvgai (init.c:228-260). The pre-NV_50 CR44 head-swapping
// there does not apply (GP106 is NV_50+).
static uint8_t
init_rdvgai(struct pg106_init_env *env, uint16_t port, uint8_t index)
{
	if (init_exec(env))
		return env->vt->rd8(env, port, index);
	return 0x00;
}

static void
init_wrvgai(struct pg106_init_env *env, uint16_t port, uint8_t index,
	    uint8_t value)
{
	if (init_exec(env)) {
		env->vt->wr8(env, port, index, value);
		trace_append(env, PG106_INIT_TRACE_IO, port, index, value);
	}
}

// init_rdport/init_wrport (init.c:213-226).
static uint8_t
init_rdport(struct pg106_init_env *env, uint16_t port)
{
	if (init_exec(env))
		return env->vt->rdport(env, port);
	return 0x00;
}

static void
init_wrport(struct pg106_init_env *env, uint16_t port, uint8_t value)
{
	if (init_exec(env)) {
		env->vt->wrport(env, port, value);
		trace_append(env, PG106_INIT_TRACE_IO, port, 0, value);
	}
}

// init_i2c (init.c:262-282): bus-index remap only. 0xff follows
// init->outp->i2c_upper_default — the DCB output context is not modelled, so
// the PRI bus is used unconditionally (SIM note on the one outp-dependent
// branch); 0x80 -> PRI, 0x81 -> SEC (NVKM_I2C_BUS_PRI = 0 / SEC = 1).
static uint8_t
init_i2c_bus(struct pg106_init_env *env, uint8_t index)
{
	(void)env;
	if (index == 0xff || index == 0x80)
		return 0; // NVKM_I2C_BUS_PRI
	if (index == 0x81)
		return 1; // NVKM_I2C_BUS_SEC
	return index;
}

// init_rdi2cr (init.c:284-291): adap == NULL -> -ENODEV, reproduced by the
// NULL hook. The returned int is what INIT_I2C_IF compares (bug-compatible,
// including the negative errno truncation).
static int
init_rdi2cr(struct pg106_init_env *env, uint8_t index, uint8_t addr, uint8_t reg)
{
	if (env->vt->rd_i2cr && init_exec(env))
		return env->vt->rd_i2cr(env, init_i2c_bus(env, index), addr, reg);
	return -19; // -ENODEV, the errno master's init_rdi2cr returns
}

// init_wri2cr (init.c:293-300), plus the trace record.
static void
init_wri2cr(struct pg106_init_env *env, uint8_t index, uint8_t addr,
	    uint8_t reg, uint8_t val)
{
	if (env->vt->wr_i2cr && init_exec(env)) {
		env->vt->wr_i2cr(env, init_i2c_bus(env, index), addr, reg, val);
		trace_append(env, PG106_INIT_TRACE_I2C,
			     ((uint32_t)init_i2c_bus(env, index) << 8) | addr,
			     reg, val);
	}
}

// init_shift (init.c:530-536): shift < 0x80 means right, 0x100 - shift left.
static uint32_t
init_shift(uint32_t data, uint8_t shift)
{
	if (shift < 0x80)
		return data >> shift;
	return data << (0x100 - shift);
}

// Fake-clock delay: mdelay(n) rounds up to whole milliseconds (see
// init_time, init.c:1789-1794); udelay is used verbatim. No-op when
// execution is disabled, matching master's init_exec() guards.
static void
init_delay(struct pg106_init_env *env, uint32_t usec)
{
	if (init_exec(env) && env->vt->time) {
		env->vt->time(env, usec);
		// Delays are side effects too — record them so a whole run
		// (INIT_TIME 0x74, INIT_LTIME 0x57, INIT_CONDITION_TIME 0x56
		// polls, INIT_RESET 0x65's udelay, INIT_IO 0x69's mdeltas)
		// is reproducible end to end.
		trace_append(env, PG106_INIT_TRACE_TIME, 0, 0, usec);
	}
}

// ---------------------------------------------------------------------------
// Condition evaluation (init.c:478-528).
// ---------------------------------------------------------------------------

// init_condition_met (init.c:478-492). 12-byte entries {reg, mask, value}.
static int
init_condition_met(struct pg106_init_env *env, uint8_t cond)
{
	uint16_t table = env->table_condition;
	uint32_t reg, msk, val;

	if (!table) {
		fail(env, PG106_INIT_ERR_NO_TABLE,
		     "condition table absent");
		return 0;
	}
	if (cond >= env->condition_count) {
		fail(env, PG106_INIT_ERR_RANGE,
		     "condition index out of range");
		return 0;
	}
	reg = bi_rd32(env, (uint16_t)(table + cond * 12 + 0));
	msk = bi_rd32(env, (uint16_t)(table + cond * 12 + 4));
	val = bi_rd32(env, (uint16_t)(table + cond * 12 + 8));
	return (init_rd32(env, reg) & msk) == val;
}

// init_io_condition_met (init.c:494-509). 5-byte entries
// {port, index, mask, value}.
static int
init_io_condition_met(struct pg106_init_env *env, uint8_t cond)
{
	uint16_t table = env->table_io_condition;
	uint16_t port;
	uint8_t index, mask, value;

	if (!table) {
		fail(env, PG106_INIT_ERR_NO_TABLE,
		     "io condition table absent");
		return 0;
	}
	if (cond >= env->io_condition_count) {
		fail(env, PG106_INIT_ERR_RANGE,
		     "io condition index out of range");
		return 0;
	}
	port = bi_rd16(env, (uint16_t)(table + cond * 5 + 0));
	index = bi_rd08(env, (uint16_t)(table + cond * 5 + 2));
	mask = bi_rd08(env, (uint16_t)(table + cond * 5 + 3));
	value = bi_rd08(env, (uint16_t)(table + cond * 5 + 4));
	return (init_rdvgai(env, port, index) & mask) == value;
}

// init_io_flag_condition_met (init.c:511-528). 9-byte entries
// {port, index, mask, shift, data offset, dmask, value}.
static int
init_io_flag_condition_met(struct pg106_init_env *env, uint8_t cond)
{
	uint16_t table = env->table_io_flag_condition;
	uint16_t port, data;
	uint8_t index, mask, shift, dmask, value, ioval;

	if (!table) {
		fail(env, PG106_INIT_ERR_NO_TABLE,
		     "io flag condition table absent");
		return 0;
	}
	if (cond >= env->io_flag_condition_count) {
		fail(env, PG106_INIT_ERR_RANGE,
		     "io flag condition index out of range");
		return 0;
	}
	port = bi_rd16(env, (uint16_t)(table + cond * 9 + 0));
	index = bi_rd08(env, (uint16_t)(table + cond * 9 + 2));
	mask = bi_rd08(env, (uint16_t)(table + cond * 9 + 3));
	shift = bi_rd08(env, (uint16_t)(table + cond * 9 + 4));
	data = bi_rd16(env, (uint16_t)(table + cond * 9 + 5));
	dmask = bi_rd08(env, (uint16_t)(table + cond * 9 + 7));
	value = bi_rd08(env, (uint16_t)(table + cond * 9 + 8));
	ioval = (init_rdvgai(env, port, index) & mask) >> shift;
	return (bi_rd08(env, (uint16_t)(data + ioval)) & dmask) == value;
}

// init_script (init.c:408-427), BIT path only: script table entry `index`.
// The BMP (< 0x0510) fallback is not modelled — GP106 VBIOSes are BIT-era.
static uint16_t
init_script(struct pg106_init_env *env, uint8_t index)
{
	if (!env->table_script) {
		fail(env, PG106_INIT_ERR_NO_TABLE,
		     "script table absent");
		return 0;
	}
	return bi_rd16(env, (uint16_t)(env->table_script + index * 2));
}

// init_ram_restrict_group_count / init_ram_restrict (init.c:438-458): the
// caller-supplied strap stand-in (see header comment on the env fields).
static uint8_t
init_ram_restrict_group_count(const struct pg106_init_env *env)
{
	return env->ram_restrict_groups;
}

static uint8_t
init_ram_restrict(const struct pg106_init_env *env)
{
	return env->ramcfg_index;
}

// init_xlat_ (init.c:460-472): xlat table entry `index` is a u16 pointer to
// a byte table; return byte at +offset. master warns and yields 0 for a bad
// pointer; that becomes a warning + 0 here too.
static uint8_t
init_xlat_(struct pg106_init_env *env, uint8_t index, uint8_t offset)
{
	if (!env->table_xlat) {
		warn(env, "xlat table absent");
		return 0x00;
	}
	uint16_t data = bi_rd16(env,
		(uint16_t)(env->table_xlat + (uint16_t)(index * 2)));
	if (!data) {
		warn(env, "xlat table pointer invalid");
		return 0x00;
	}
	return bi_rd08(env, (uint16_t)(data + offset));
}

// ---------------------------------------------------------------------------
// PLL programming (INIT_PLL family 0x34/0x4a/0x4b/0x59/0x79/0x87).
//
// Master chain: init.c init_prog_pll (init.c:343-352) -> nvkm_devinit_pll_set
// (devinit-base.c:38-40) -> gf100_devinit_pll_set (devinit/gf100.c:31-64 —
// the .pll_set GP10x inherits through gm200_devinit, gm200.c:178-184) ->
// nvbios_pll_parse (pll.c:229-440) + gt215_pll_calc (clk/pllgt215.c:29-87).
// All three stages are ported below; the SIM DEVIATIONs are flagged inline:
//   - BIT-era pll limits tables only (versions 0x30/0x40/0x50; GP10x ROMs
//     carry 0x40 — verified against research/firmware/vbios/*.rom): the
//     BMP-era fallbacks and the nv51 sel_clk refclk hack (pll.c:388-397)
//     are dead code on Pascal and not ported;
//   - a missing/out-of-range limits table aborts loudly (master's
//     nvbios_pll_parse failure propagates to init_prog_pll, which warns
//     "failed to prog pll" and limps on — deviation 2 applies instead);
//   - refclk/vco1 input frequency of zero would divide-by-zero in master;
//     a simulator fails loudly instead.
// ---------------------------------------------------------------------------

// enum nvbios_pll_type values the gf100/gt215 write path switches on
// (include/nvkm/subdev/bios/pll.h:28-42).
#define PLL_VPLL0 0x80u
#define PLL_VPLL1 0x81u
#define PLL_VPLL2 0x82u
#define PLL_VPLL3 0x83u
#define PLL_MAX_  0xffu

// struct nvbios_pll, pll.h:44-73, trimmed to the fields the ported code
// reads (gt215_pll_calc and the gf100 switch only touch vco1).
struct pg106_pll {
	uint8_t type;
	uint32_t reg;
	uint32_t refclk;
	uint8_t min_p;
	uint8_t max_p;
	uint8_t bias_p;
	struct {
		uint32_t min_freq;
		uint32_t max_freq;
		uint32_t min_inputfreq;
		uint32_t max_inputfreq;
		uint8_t min_m;
		uint8_t max_m;
		uint8_t min_n;
		uint8_t max_n;
	} vco1;
};

// pll_limits_table (pll.c:81-113), BIT path only: the caller supplies the
// 'C'-entry pointer (env->table_pll), here it is only decoded.
static int
pll_limits_table(struct pg106_init_env *env, uint16_t *data, uint8_t *ver,
		 uint8_t *hdr, uint8_t *cnt, uint8_t *len)
{
	if (!env->table_pll) {
		fail(env, PG106_INIT_ERR_NO_TABLE,
		     "pll limits table absent");
		return -1;
	}
	*data = env->table_pll;
	*ver = bi_rd08(env, (uint16_t)(*data + 0));
	*hdr = bi_rd08(env, (uint16_t)(*data + 1));
	*len = bi_rd08(env, (uint16_t)(*data + 2));
	*cnt = bi_rd08(env, (uint16_t)(*data + 3));
	return env->status == PG106_INIT_OK ? 0 : -1;
}

// pll_map_reg (pll.c:142-160), version >= 0x30 path: scan entries for
// u32 at entry+3 == reg; the type byte is entry+0.
static uint16_t
pll_map_reg(struct pg106_init_env *env, uint32_t reg, uint8_t *type,
	    uint8_t *ver)
{
	uint16_t data, base;
	uint8_t hdr, cnt, len;

	if (pll_limits_table(env, &data, ver, &hdr, &cnt, &len))
		return 0;
	if (*ver < 0x30) {
		// pre-0x30 tables need the static per-chipset pll_map —
		// BMP-era, unmodelled (pll.c:115-140).
		fail(env, PG106_INIT_ERR_RANGE,
		     "unsupported pll limits version");
		return 0;
	}
	base = (uint16_t)(data + hdr);
	data = base;
	while (cnt--) {
		if (bi_rd32(env, (uint16_t)(data + 3)) == reg) {
			*type = bi_rd08(env, data);
			return data;
		}
		if (env->status != PG106_INIT_OK)
			return 0;
		data = (uint16_t)(data + len);
	}
	fail(env, PG106_INIT_ERR_RANGE,
	     "pll register not in limits table");
	return 0;
}

// pll_map_type (pll.c:184-205), version >= 0x30 path: scan entries for the
// type byte; reg comes from entry+3 below 0x50 and is 0 from 0x50 on.
static uint16_t
pll_map_type(struct pg106_init_env *env, uint8_t type, uint32_t *reg,
	     uint8_t *ver)
{
	uint16_t data;
	uint8_t hdr, cnt, len;

	if (pll_limits_table(env, &data, ver, &hdr, &cnt, &len))
		return 0;
	if (*ver < 0x30) {
		fail(env, PG106_INIT_ERR_RANGE,
		     "unsupported pll limits version");
		return 0;
	}
	data = (uint16_t)(data + hdr);
	while (cnt--) {
		if (bi_rd08(env, data) == type) {
			if (*ver < 0x50)
				*reg = bi_rd32(env, (uint16_t)(data + 3));
			else
				*reg = 0;
			return data;
		}
		if (env->status != PG106_INIT_OK)
			return 0;
		data = (uint16_t)(data + len);
	}
	fail(env, PG106_INIT_ERR_RANGE,
	     "pll type not in limits table");
	return 0;
}

// nvbios_pll_parse (pll.c:229-440). Returns 0 on success; on failure env
// carries the loud status.
static int
pll_parse(struct pg106_init_env *env, uint32_t type, struct pg106_pll *info)
{
	uint32_t reg = type;
	uint16_t data;
	uint8_t ver = 0;

	memset(info, 0, sizeof(*info));
	info->type = (uint8_t)type; // pll.c:249

	if (type > PLL_MAX_)
		data = pll_map_reg(env, reg, &info->type, &ver);
	else
		data = pll_map_type(env, (uint8_t)type, &reg, &ver);
	if (env->status != PG106_INIT_OK)
		return -1;

	info->reg = reg;

	switch (ver) {
	case 0x30:
		data = bi_rd16(env, (uint16_t)(data + 1));
		info->vco1.min_freq = (uint32_t)bi_rd16(env, (uint16_t)(data + 0)) * 1000;
		info->vco1.max_freq = (uint32_t)bi_rd16(env, (uint16_t)(data + 2)) * 1000;
		info->vco1.min_inputfreq = (uint32_t)bi_rd16(env, (uint16_t)(data + 8)) * 1000;
		info->vco1.max_inputfreq = (uint32_t)bi_rd16(env, (uint16_t)(data + 12)) * 1000;
		info->vco1.min_n = bi_rd08(env, (uint16_t)(data + 16));
		info->vco1.max_n = bi_rd08(env, (uint16_t)(data + 17));
		info->vco1.min_m = bi_rd08(env, (uint16_t)(data + 18));
		info->vco1.max_m = bi_rd08(env, (uint16_t)(data + 19));
		info->max_p = bi_rd08(env, (uint16_t)(data + 25));
		info->bias_p = bi_rd08(env, (uint16_t)(data + 27));
		info->refclk = bi_rd32(env, (uint16_t)(data + 28));
		break;
	case 0x40:
		info->refclk = (uint32_t)bi_rd16(env, (uint16_t)(data + 9)) * 1000;
		data = bi_rd16(env, (uint16_t)(data + 1));
		info->vco1.min_freq = (uint32_t)bi_rd16(env, (uint16_t)(data + 0)) * 1000;
		info->vco1.max_freq = (uint32_t)bi_rd16(env, (uint16_t)(data + 2)) * 1000;
		info->vco1.min_inputfreq = (uint32_t)bi_rd16(env, (uint16_t)(data + 4)) * 1000;
		info->vco1.max_inputfreq = (uint32_t)bi_rd16(env, (uint16_t)(data + 6)) * 1000;
		info->vco1.min_m = bi_rd08(env, (uint16_t)(data + 8));
		info->vco1.max_m = bi_rd08(env, (uint16_t)(data + 9));
		info->vco1.min_n = bi_rd08(env, (uint16_t)(data + 10));
		info->vco1.max_n = bi_rd08(env, (uint16_t)(data + 11));
		info->min_p = bi_rd08(env, (uint16_t)(data + 12));
		info->max_p = bi_rd08(env, (uint16_t)(data + 13));
		break;
	case 0x50:
		info->refclk = (uint32_t)bi_rd16(env, (uint16_t)(data + 1)) * 1000;
		info->vco1.min_freq = (uint32_t)bi_rd16(env, (uint16_t)(data + 5)) * 1000;
		info->vco1.max_freq = (uint32_t)bi_rd16(env, (uint16_t)(data + 7)) * 1000;
		info->vco1.min_inputfreq = (uint32_t)bi_rd16(env, (uint16_t)(data + 9)) * 1000;
		info->vco1.max_inputfreq = (uint32_t)bi_rd16(env, (uint16_t)(data + 11)) * 1000;
		info->vco1.min_m = bi_rd08(env, (uint16_t)(data + 13));
		info->vco1.max_m = bi_rd08(env, (uint16_t)(data + 14));
		info->vco1.min_n = bi_rd08(env, (uint16_t)(data + 15));
		info->vco1.max_n = bi_rd08(env, (uint16_t)(data + 16));
		info->min_p = bi_rd08(env, (uint16_t)(data + 17));
		info->max_p = bi_rd08(env, (uint16_t)(data + 18));
		break;
	default:
		// master: nvkm_error("unknown pll limits version") + -EINVAL
		// (pll.c:381-383). A simulator aborts (deviation 2).
		fail(env, PG106_INIT_ERR_RANGE,
		     "unknown pll limits version");
		return -1;
	}
	if (env->status != PG106_INIT_OK)
		return -1;

	if (!info->refclk) {
		// pll.c:386-398 — master falls back to device->crystal. 0
		// here would divide-by-zero in gt215_pll_calc.
		if (!env->crystal_khz) {
			fail(env, PG106_INIT_ERR_RANGE,
			     "pll refclk unknown and no crystal set");
			return -1;
		}
		info->refclk = env->crystal_khz;
	}

	return 0;
}

// gt215_pll_calc (clk/pllgt215.c:29-87), verbatim arithmetic including the
// u32 wrap in `tmp = freq * *P * M` and the early return in the pfN branch
// (gf100 always passes pfN, so the first valid M wins).
static int
gt215_pll_calc(struct pg106_init_env *env, const struct pg106_pll *info,
	       uint32_t freq, int *pN, int *pfN, int *pM, int *P)
{
	uint32_t best_err = ~0u, err;
	int M, lM, hM, N, fN;

	if (!freq || !info->vco1.min_inputfreq ||
	    !info->vco1.max_inputfreq) {
		// master divides by zero in these cases (clk/pllgt215.c:36,
		// :42, :44); a simulator fails loudly instead.
		fail(env, PG106_INIT_ERR_RANGE,
		     "pll frequency limits zero");
		return -1;
	}

	*P = (int)(info->vco1.max_freq / freq);
	if (*P > info->max_p)
		*P = info->max_p;
	if (*P < info->min_p)
		*P = info->min_p;

	lM = (int)((info->refclk + info->vco1.max_inputfreq) /
		   info->vco1.max_inputfreq);
	lM = lM > (int)info->vco1.min_m ? lM : (int)info->vco1.min_m;
	hM = (int)((info->refclk + info->vco1.min_inputfreq) /
		   info->vco1.min_inputfreq);
	hM = hM < (int)info->vco1.max_m ? hM : (int)info->vco1.max_m;
	lM = lM < hM ? lM : hM;

	for (M = lM; M <= hM; M++) {
		uint32_t tmp = freq * (uint32_t)*P * (uint32_t)M;
		N = (int)(tmp / info->refclk);
		fN = (int)(tmp % info->refclk);

		if (!pfN) {
			if (fN >= (int)(info->refclk / 2))
				N++;
		} else {
			if (fN < (int)(info->refclk / 2))
				N--;
			fN = (int)(tmp - (uint32_t)N * info->refclk);
		}

		if (N < (int)info->vco1.min_n)
			continue;
		if (N > (int)info->vco1.max_n)
			break;

		int diff = (int)freq -
			   info->refclk * N / M / *P;
		err = (uint32_t)(diff < 0 ? -diff : diff);
		if (err < best_err) {
			best_err = err;
			*pN = N;
			*pM = M;
		}

		if (pfN) {
			*pfN = (int)(((uint32_t)fN << 13) +
				     info->refclk / 2) / (int)info->refclk;
			*pfN = (*pfN - 4096) & 0xffff;
			return (int)freq;
		}
	}

	if (best_err == ~0u) {
		// master: nvkm_error("unable to find matching pll values")
		// + -EINVAL -> init_prog_pll warns and CONTINUES.
		warn(env, "unable to find matching pll values");
		return -1;
	}

	return (int)(info->refclk * *pN / *pM / *P);
}

// init_prog_pll (init.c:343-352) + gf100_devinit_pll_set (devinit/gf100.c:
// 31-64): parse the limits entry for `id` (a register address or a type
// byte), compute N/fN/M/P, and program the VPLL register block. Master's
// "failed to prog pll" warn-and-continue is kept for the calc/no-write
// paths; missing/unknown-table data aborts loudly (deviation 2).
static void
init_prog_pll(struct pg106_init_env *env, uint32_t id, uint32_t freq)
{
	struct pg106_pll info;
	int N = 0, fN = 0, M = 0, P = 0;
	int ret;

	if (!init_exec(env))
		return;

	if (pll_parse(env, id, &info))
		return; // loud failure already recorded

	ret = gt215_pll_calc(env, &info, freq, &N, &fN, &M, &P);
	if (ret < 0) {
		// master: gf100_devinit_pll_set returns the error and
		// init_prog_pll warns "failed to prog pll" + continues.
		return;
	}

	switch (info.type) {
	case PLL_VPLL0:
	case PLL_VPLL1:
	case PLL_VPLL2:
	case PLL_VPLL3:
		// devinit/gf100.c:53-55.
		init_mask(env, info.reg + 0x0c, 0x00000000, 0x00000100);
		init_wr32(env, info.reg + 0x04,
			  ((uint32_t)P << 16) | ((uint32_t)N << 8) |
			  (uint32_t)M);
		init_wr32(env, info.reg + 0x10, (uint32_t)fN << 16);
		break;
	default:
		// master: nvkm_warn("%08x/%dKhz unimplemented") + -EINVAL
		// (devinit/gf100.c:57-59) -> warn-and-continue.
		warn(env, "pll type unimplemented (gf100 default case)");
		break;
	}
}

// ---------------------------------------------------------------------------
// Script execution (init.c:2301-2319 nvbios_exec and the call sites inside
// INIT_SUB / INIT_SUB_DIRECT / INIT_REPEAT).
// ---------------------------------------------------------------------------

static int exec_script(struct pg106_init_env *env);

// Shared body of INIT_SUB (0x6b) / INIT_SUB_DIRECT (0x5b): run the script at
// addr to its INIT_DONE, then restore the caller's PC. master's error path
// ("error parsing sub-table", init.c:1355-1359/1675-1679) leaks the inner PC
// and keeps parsing; we let the error propagate and abort the whole run —
// SIM DEVIATION (a simulator must not limp).
static void
exec_sub_script(struct pg106_init_env *env, uint16_t addr)
{
	if (!addr || !init_exec(env))
		return;

	uint16_t save = env->offset;
	env->offset = addr;
	exec_script(env);
	env->offset = save;
}

static int
exec_script(struct pg106_init_env *env)
{
	if (env->nested >= PG106_INIT_MAX_NESTING) {
		fail(env, PG106_INIT_ERR_DEPTH,
		     "sub-script nesting limit exceeded");
		return env->status;
	}
	env->nested++;

	while (env->offset) {
		const struct pg106_init_op *op;

		if (env->status != PG106_INIT_OK)
			break;
		// SIM DEVIATION: runaway guard (master has none).
		if (env->step_limit && env->steps >= env->step_limit) {
			fail(env, PG106_INIT_ERR_STEPS,
			     "step budget exceeded");
			break;
		}
		env->steps++;

		uint8_t opcode = bi_rd08(env, env->offset);
		if (env->status != PG106_INIT_OK)
			break;

		env->step_offset = env->offset;
		env->step_opcode = opcode;
		op = &pg106_init_ops[opcode];

		if (!op->exec) {
			// master init.c:2309-2312 prints "unknown opcode" and
			// returns -EINVAL. Distinguish "byte not in master's
			// table" from "master knows it, we don't implement it".
			if (op->name)
				// The dispatch table's name IS the message;
				// tests assert it (e.g. "INIT_I2C_BYTE").
				fail(env, PG106_INIT_ERR_OPCODE, op->name);
			else
				fail(env, PG106_INIT_ERR_OPCODE,
				     "opcode not in nvkm dispatch");
			break;
		}

		op->exec(env);
	}

	env->nested--;
	return env->status;
}

// ---------------------------------------------------------------------------
// Opcode handlers, in dispatch-table order. Each cites its master function.
// ---------------------------------------------------------------------------

// init_io_restrict_prog (init.c:617-651), INIT_IO_RESTRICT_PROG 0x32.
static void
op_io_restrict_prog(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint16_t port = bi_rd16(env, (uint16_t)(at + 1));
	uint8_t index = bi_rd08(env, (uint16_t)(at + 3));
	uint8_t mask = bi_rd08(env, (uint16_t)(at + 4));
	uint8_t shift = bi_rd08(env, (uint16_t)(at + 5));
	uint8_t count = bi_rd08(env, (uint16_t)(at + 6));
	uint32_t reg = bi_rd32(env, (uint16_t)(at + 7));
	uint8_t conf, i;

	env->offset = (uint16_t)(at + 11);
	conf = (init_rdvgai(env, port, index) & mask) >> shift;
	for (i = 0; i < count; i++) {
		uint32_t data = bi_rd32(env, env->offset);
		if (i == conf)
			init_wr32(env, reg, data);
		env->offset = (uint16_t)(env->offset + 4);
	}
}

// init_repeat (init.c:654-677), INIT_REPEAT 0x33.
static void
op_repeat(struct pg106_init_env *env)
{
	uint8_t count = bi_rd08(env, (uint16_t)(env->offset + 1));
	uint16_t repeat = env->repeat;

	env->offset = (uint16_t)(env->offset + 2);
	env->repeat = env->offset;
	env->repend = env->offset;
	while (count--) {
		env->offset = env->repeat;
		exec_script(env);
		if (env->status != PG106_INIT_OK)
			break;
	}
	env->offset = env->repend;
	env->repeat = repeat;
}

// init_io_restrict_pll (init.c:684-717), INIT_IO_RESTRICT_PLL 0x34.
// `iofc` is a signed byte; only a positive index consults the io flag
// condition table (init.c:707), and the doubled frequency feeds the same
// init_prog_pll path.
static void
op_io_restrict_pll(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint16_t port = bi_rd16(env, (uint16_t)(at + 1));
	uint8_t index = bi_rd08(env, (uint16_t)(at + 3));
	uint8_t mask = bi_rd08(env, (uint16_t)(at + 4));
	uint8_t shift = bi_rd08(env, (uint16_t)(at + 5));
	int8_t iofc = (int8_t)bi_rd08(env, (uint16_t)(at + 6));
	uint8_t count = bi_rd08(env, (uint16_t)(at + 7));
	uint32_t reg = bi_rd32(env, (uint16_t)(at + 8));
	uint8_t conf, i;

	env->offset = (uint16_t)(at + 12);
	conf = (uint8_t)((init_rdvgai(env, port, index) & mask) >> shift);
	for (i = 0; i < count; i++) {
		uint32_t freq = (uint32_t)bi_rd16(env, env->offset) * 10;

		if (i == conf) {
			if (iofc > 0 &&
			    init_io_flag_condition_met(env, (uint8_t)iofc))
				freq *= 2;
			init_prog_pll(env, reg, freq);
		}
		env->offset = (uint16_t)(env->offset + 2);
	}
}

// init_end_repeat (init.c:720-733), INIT_END_REPEAT 0x36. Setting offset to
// 0 ends the inner nvbios_exec started by INIT_REPEAT.
static void
op_end_repeat(struct pg106_init_env *env)
{
	env->offset = (uint16_t)(env->offset + 1);
	if (env->repeat) {
		env->repend = env->offset;
		env->offset = 0;
	}
}

// init_copy (init.c:736-760), INIT_COPY 0x37.
static void
op_copy(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint32_t reg = bi_rd32(env, (uint16_t)(at + 1));
	uint8_t shift = bi_rd08(env, (uint16_t)(at + 5));
	uint8_t smask = bi_rd08(env, (uint16_t)(at + 6));
	uint16_t port = bi_rd16(env, (uint16_t)(at + 7));
	uint8_t index = bi_rd08(env, (uint16_t)(at + 9));
	uint8_t mask = bi_rd08(env, (uint16_t)(at + 10));
	uint8_t data;

	env->offset = (uint16_t)(at + 11);
	data = init_rdvgai(env, port, index) & mask;
	data |= (uint8_t)(init_shift(init_rd32(env, reg), shift) & smask);
	init_wrvgai(env, port, index, data);
}

// init_not (init.c:763-772), INIT_NOT 0x38.
static void
op_not(struct pg106_init_env *env)
{
	env->offset = (uint16_t)(env->offset + 1);
	init_exec_inv(env);
}

// init_io_flag_condition (init.c:775-789), INIT_IO_FLAG_CONDITION 0x39.
static void
op_io_flag_condition(struct pg106_init_env *env)
{
	uint8_t cond = bi_rd08(env, (uint16_t)(env->offset + 1));

	env->offset = (uint16_t)(env->offset + 2);
	if (!init_io_flag_condition_met(env, cond))
		init_exec_set(env, 0);
}

// init_io_mask_or (init.c:844-860), INIT_IO_MASK_OR 0x3b.
static void
op_io_mask_or(struct pg106_init_env *env)
{
	uint8_t index = bi_rd08(env, (uint16_t)(env->offset + 1));
	uint8_t or = (uint8_t)init_or(env);
	uint8_t data;

	env->offset = (uint16_t)(env->offset + 2);
	data = init_rdvgai(env, 0x03d4, index);
	init_wrvgai(env, 0x03d4, index, (uint8_t)(data & ~(1u << or)));
}

// init_io_or (init.c:863-879), INIT_IO_OR 0x3c.
static void
op_io_or(struct pg106_init_env *env)
{
	uint8_t index = bi_rd08(env, (uint16_t)(env->offset + 1));
	uint8_t or = (uint8_t)init_or(env);
	uint8_t data;

	env->offset = (uint16_t)(env->offset + 2);
	data = init_rdvgai(env, 0x03d4, index);
	init_wrvgai(env, 0x03d4, index, (uint8_t)(data | (1u << or)));
}

// init_andn_reg (init.c:882-896), INIT_ANDN_REG 0x47.
static void
op_andn_reg(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint32_t reg = bi_rd32(env, (uint16_t)(at + 1));
	uint32_t mask = bi_rd32(env, (uint16_t)(at + 5));

	env->offset = (uint16_t)(at + 9);
	init_mask(env, reg, mask, 0);
}

// init_or_reg (init.c:899-913), INIT_OR_REG 0x48.
static void
op_or_reg(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint32_t reg = bi_rd32(env, (uint16_t)(at + 1));
	uint32_t mask = bi_rd32(env, (uint16_t)(at + 5));

	env->offset = (uint16_t)(at + 9);
	init_mask(env, reg, 0, mask);
}

// init_idx_addr_latched (init.c:916-943), INIT_INDEX_ADDRESS_LATCHED 0x49.
static void
op_idx_addr_latched(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint32_t creg = bi_rd32(env, (uint16_t)(at + 1));
	uint32_t dreg = bi_rd32(env, (uint16_t)(at + 5));
	uint32_t mask = bi_rd32(env, (uint16_t)(at + 9));
	uint32_t data = bi_rd32(env, (uint16_t)(at + 13));
	uint8_t count = bi_rd08(env, (uint16_t)(at + 17));

	env->offset = (uint16_t)(at + 18);
	while (count--) {
		uint8_t iaddr = bi_rd08(env, env->offset);
		uint8_t idata = bi_rd08(env, (uint16_t)(env->offset + 1));

		env->offset = (uint16_t)(env->offset + 2);
		init_wr32(env, dreg, idata);
		init_mask(env, creg, ~mask, data | iaddr);
	}
}

// init_io_restrict_pll2 (init.c:950-978), INIT_IO_RESTRICT_PLL2 0x4a.
static void
op_io_restrict_pll2(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint16_t port = bi_rd16(env, (uint16_t)(at + 1));
	uint8_t index = bi_rd08(env, (uint16_t)(at + 3));
	uint8_t mask = bi_rd08(env, (uint16_t)(at + 4));
	uint8_t shift = bi_rd08(env, (uint16_t)(at + 5));
	uint8_t count = bi_rd08(env, (uint16_t)(at + 6));
	uint32_t reg = bi_rd32(env, (uint16_t)(at + 7));
	uint8_t conf, i;

	env->offset = (uint16_t)(at + 11);
	conf = (uint8_t)((init_rdvgai(env, port, index) & mask) >> shift);
	for (i = 0; i < count; i++) {
		uint32_t freq = bi_rd32(env, env->offset);
		if (i == conf)
			init_prog_pll(env, reg, freq);
		env->offset = (uint16_t)(env->offset + 4);
	}
}

// init_pll2 (init.c:985-995), INIT_PLL2 0x4b.
static void
op_pll2(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint32_t reg = bi_rd32(env, (uint16_t)(at + 1));
	uint32_t freq = bi_rd32(env, (uint16_t)(at + 5));

	env->offset = (uint16_t)(at + 9);
	init_prog_pll(env, reg, freq);
}

// init_i2c_byte (init.c:1002-1026), INIT_I2C_BYTE 0x4c. A failed read
// (no adapter) skips the write — master's `if (val < 0) continue`.
static void
op_i2c_byte(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint8_t index = bi_rd08(env, (uint16_t)(at + 1));
	uint8_t addr = (uint8_t)(bi_rd08(env, (uint16_t)(at + 2)) >> 1);
	uint8_t count = bi_rd08(env, (uint16_t)(at + 3));

	env->offset = (uint16_t)(at + 4);
	while (count--) {
		uint8_t reg = bi_rd08(env, env->offset);
		uint8_t mask = bi_rd08(env, (uint16_t)(env->offset + 1));
		uint8_t data = bi_rd08(env, (uint16_t)(env->offset + 2));
		int val;

		env->offset = (uint16_t)(env->offset + 3);
		val = init_rdi2cr(env, index, addr, reg);
		if (val < 0)
			continue;
		init_wri2cr(env, index, addr, reg,
			    (uint8_t)((val & mask) | data));
	}
}

// init_zm_i2c_byte (init.c:1033-1052), INIT_ZM_I2C_BYTE 0x4d.
static void
op_zm_i2c_byte(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint8_t index = bi_rd08(env, (uint16_t)(at + 1));
	uint8_t addr = (uint8_t)(bi_rd08(env, (uint16_t)(at + 2)) >> 1);
	uint8_t count = bi_rd08(env, (uint16_t)(at + 3));

	env->offset = (uint16_t)(at + 4);
	while (count--) {
		uint8_t reg = bi_rd08(env, env->offset);
		uint8_t data = bi_rd08(env, (uint16_t)(env->offset + 1));

		env->offset = (uint16_t)(env->offset + 2);
		init_wri2cr(env, index, addr, reg, data);
	}
}

// init_zm_i2c (init.c:1059-1086), INIT_ZM_I2C 0x4e. One write i2c_msg over
// the block hook; no hook = master's adap == NULL silent skip, a failed
// transfer = master's warn("i2c wr failed").
static void
op_zm_i2c(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint8_t index = bi_rd08(env, (uint16_t)(at + 1));
	uint8_t addr = (uint8_t)(bi_rd08(env, (uint16_t)(at + 2)) >> 1);
	uint8_t count = bi_rd08(env, (uint16_t)(at + 3));
	uint8_t data[256], i;

	env->offset = (uint16_t)(at + 4);
	for (i = 0; i < count; i++) {
		data[i] = bi_rd08(env, env->offset);
		env->offset = (uint16_t)(env->offset + 1);
	}

	if (!init_exec(env))
		return;
	if (!env->vt->wr_i2c_block)
		return;
	uint8_t bus = init_i2c_bus(env, index);
	int ret = env->vt->wr_i2c_block(env, bus, addr, data, count);
	if (ret == 1) {
		for (i = 0; i < count; i++)
			trace_append(env, PG106_INIT_TRACE_I2C,
				     ((uint32_t)bus << 8) | addr, i, data[i]);
	} else {
		warn(env, "i2c wr failed");
	}
}

// init_cr_idx_adr_latch (init.c:1142-1169),
// INIT_CR_INDEX_ADDRESS_LATCHED 0x51.
static void
op_cr_idx_adr_latch(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint8_t addr0 = bi_rd08(env, (uint16_t)(at + 1));
	uint8_t addr1 = bi_rd08(env, (uint16_t)(at + 2));
	uint8_t base = bi_rd08(env, (uint16_t)(at + 3));
	uint8_t count = bi_rd08(env, (uint16_t)(at + 4));
	uint8_t save0;

	env->offset = (uint16_t)(at + 5);
	save0 = init_rdvgai(env, 0x03d4, addr0);
	while (count--) {
		uint8_t data = bi_rd08(env, env->offset);

		env->offset = (uint16_t)(env->offset + 1);
		init_wrvgai(env, 0x03d4, addr0, base++);
		init_wrvgai(env, 0x03d4, addr1, data);
	}
	init_wrvgai(env, 0x03d4, addr0, save0);
}

// init_cr (init.c:1172-1189), INIT_CR 0x52.
static void
op_cr(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint8_t addr = bi_rd08(env, (uint16_t)(at + 1));
	uint8_t mask = bi_rd08(env, (uint16_t)(at + 2));
	uint8_t data = bi_rd08(env, (uint16_t)(at + 3));
	uint8_t val;

	env->offset = (uint16_t)(at + 4);
	val = (uint8_t)(init_rdvgai(env, 0x03d4, addr) & mask);
	init_wrvgai(env, 0x03d4, addr, (uint8_t)(val | data));
}

// init_zm_cr (init.c:1192-1206), INIT_ZM_CR 0x53.
static void
op_zm_cr(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint8_t addr = bi_rd08(env, (uint16_t)(at + 1));
	uint8_t data = bi_rd08(env, (uint16_t)(at + 2));

	env->offset = (uint16_t)(at + 3);
	init_wrvgai(env, 0x03d4, addr, data);
}

// init_zm_cr_group (init.c:1209-1230), INIT_ZM_CR_GROUP 0x54.
static void
op_zm_cr_group(struct pg106_init_env *env)
{
	uint8_t count = bi_rd08(env, (uint16_t)(env->offset + 1));

	env->offset = (uint16_t)(env->offset + 2);
	while (count--) {
		uint8_t addr = bi_rd08(env, env->offset);
		uint8_t data = bi_rd08(env, (uint16_t)(env->offset + 1));

		env->offset = (uint16_t)(env->offset + 2);
		init_wrvgai(env, 0x03d4, addr, data);
	}
}

// init_condition_time (init.c:1233-1257), INIT_CONDITION_TIME 0x56.
// master polls the condition every 20 ms up to min(retry * 50, 100) times
// (mdelay(20)); the fake clock keeps that deterministic. Note master's
// `u8 wait` wraps for retry = 0: wait = 0 and `while (wait--)` tests 0
// first, so no poll happens and execution is disabled unconditionally —
// reproduced here, wrap and all.
static void
op_condition_time(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint8_t cond = bi_rd08(env, (uint16_t)(at + 1));
	uint8_t retry = bi_rd08(env, (uint16_t)(at + 2));
	uint8_t wait = (uint8_t)(((uint16_t)retry * 50) < 100 ?
				 ((uint16_t)retry * 50) : 100);

	env->offset = (uint16_t)(at + 3);
	if (!init_exec(env))
		return;

	while (wait--) {
		if (init_condition_met(env, cond))
			return;
		init_delay(env, 20 * 1000);
	}

	init_exec_set(env, 0);
}

// init_ltime (init.c:1260-1274), INIT_LTIME 0x57.
static void
op_ltime(struct pg106_init_env *env)
{
	uint16_t msec = bi_rd16(env, (uint16_t)(env->offset + 1));

	env->offset = (uint16_t)(env->offset + 3);
	init_delay(env, msec * 1000u);
}

// init_zm_reg_sequence (init.c:1277-1299), INIT_ZM_REG_SEQUENCE 0x58.
static void
op_zm_reg_sequence(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint32_t base = bi_rd32(env, (uint16_t)(at + 1));
	uint8_t count = bi_rd08(env, (uint16_t)(at + 5));

	env->offset = (uint16_t)(at + 6);
	while (count--) {
		uint32_t data = bi_rd32(env, env->offset);

		env->offset = (uint16_t)(env->offset + 4);
		init_wr32(env, base, data);
		base += 4;
	}
}

// init_pll_indirect (init.c:1306-1318), INIT_PLL_INDIRECT 0x59.
static void
op_pll_indirect(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint32_t reg = bi_rd32(env, (uint16_t)(at + 1));
	uint16_t addr = bi_rd16(env, (uint16_t)(at + 5));
	uint32_t freq = (uint32_t)bi_rd16(env, addr) * 1000;

	env->offset = (uint16_t)(at + 7);
	init_prog_pll(env, reg, freq);
}

// init_zm_reg_indirect (init.c:1321-1337), INIT_ZM_REG_INDIRECT 0x5a.
// NOTE: master writes to `addr` (the BIOS offset) rather than `reg`, unlike
// the trace line and INIT_PLL_INDIRECT right above it — almost certainly an
// upstream bug, reproduced verbatim so traces match nvkm. (VBIOSes in the
// wild appear to avoid the opcode; kept for dispatch completeness.)
static void
op_zm_reg_indirect(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint32_t reg = bi_rd32(env, (uint16_t)(at + 1));
	uint16_t addr = bi_rd16(env, (uint16_t)(at + 5));
	uint32_t data = bi_rd32(env, addr);

	env->offset = (uint16_t)(at + 7);
	// upstream: init_wr32(init, addr, data) — bug-compatible on purpose.
	init_wr32(env, addr, data);
	(void)reg;
}

// init_sub_direct (init.c:1340-1363), INIT_SUB_DIRECT 0x5b.
static void
op_sub_direct(struct pg106_init_env *env)
{
	uint16_t addr = bi_rd16(env, (uint16_t)(env->offset + 1));

	exec_sub_script(env, addr);
	env->offset = (uint16_t)(env->offset + 3);
}

// init_jump (init.c:1366-1381), INIT_JUMP 0x5c. A jump to 0 terminates the
// script, exactly like master (the assignment simply ends the while loop).
static void
op_jump(struct pg106_init_env *env)
{
	uint16_t offset = bi_rd16(env, (uint16_t)(env->offset + 1));

	if (init_exec(env))
		env->offset = offset;
	else
		env->offset = (uint16_t)(env->offset + 3);
}

// init_i2c_if (init.c:1388-1408), INIT_I2C_IF 0x5e. Forced execution around
// the read; the comparison uses the truncated u8 read value — including the
// (u8)-ENODEV = 0xed a missing adapter yields (bug-compatible).
static void
op_i2c_if(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint8_t index = bi_rd08(env, (uint16_t)(at + 1));
	uint8_t addr = bi_rd08(env, (uint16_t)(at + 2));
	uint8_t reg = bi_rd08(env, (uint16_t)(at + 3));
	uint8_t mask = bi_rd08(env, (uint16_t)(at + 4));
	uint8_t data = bi_rd08(env, (uint16_t)(at + 5));
	uint8_t value;

	env->offset = (uint16_t)(at + 6);
	init_exec_force(env, 1);

	value = (uint8_t)init_rdi2cr(env, index, addr, reg);
	if ((uint8_t)(value & mask) != data)
		init_exec_set(env, 0);

	init_exec_force(env, 0);
}

// init_copy_nv_reg (init.c:1411-1434), INIT_COPY_NV_REG 0x5f.
static void
op_copy_nv_reg(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint32_t sreg = bi_rd32(env, (uint16_t)(at + 1));
	uint8_t shift = bi_rd08(env, (uint16_t)(at + 5));
	uint32_t smask = bi_rd32(env, (uint16_t)(at + 6));
	uint32_t sxor = bi_rd32(env, (uint16_t)(at + 10));
	uint32_t dreg = bi_rd32(env, (uint16_t)(at + 14));
	uint32_t dmask = bi_rd32(env, (uint16_t)(at + 18));
	uint32_t data;

	env->offset = (uint16_t)(at + 22);
	data = init_shift(init_rd32(env, sreg), shift);
	init_mask(env, dreg, ~dmask, (data & smask) ^ sxor);
}

// init_zm_index_io (init.c:1437-1452), INIT_ZM_INDEX_IO 0x62.
static void
op_zm_index_io(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint16_t port = bi_rd16(env, (uint16_t)(at + 1));
	uint8_t index = bi_rd08(env, (uint16_t)(at + 3));
	uint8_t data = bi_rd08(env, (uint16_t)(at + 4));

	env->offset = (uint16_t)(at + 5);
	init_wrvgai(env, port, index, data);
}

// init_compute_mem (init.c:1458-1470), INIT_COMPUTE_MEM 0x63. Master calls
// nvkm_devinit_meminit; the gm200/GP10x devinit func table has no .meminit
// (devinit/gm200.c:178-184), so nvkm_devinit_meminit is a no-op there —
// the exec_force dance with nothing inside is the faithful port.
static void
op_compute_mem(struct pg106_init_env *env)
{
	env->offset = (uint16_t)(env->offset + 1);
	init_exec_force(env, 1);
	// nvkm_devinit_meminit(devinit): gm200+ has no .meminit — no-op.
	init_exec_force(env, 0);
}

// init_reset (init.c:1473-1497), INIT_RESET 0x65. Runs even while execution
// is disabled (init_exec_force); the 10 us udelay goes through the fake
// clock.
static void
op_reset(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint32_t reg = bi_rd32(env, (uint16_t)(at + 1));
	uint32_t data1 = bi_rd32(env, (uint16_t)(at + 5));
	uint32_t data2 = bi_rd32(env, (uint16_t)(at + 9));
	uint32_t savepci19;

	env->offset = (uint16_t)(at + 13);
	init_exec_force(env, 1);

	savepci19 = init_mask(env, 0x00184c, 0x00000f00, 0x00000000);
	init_wr32(env, reg, data1);
	init_delay(env, 10);
	init_wr32(env, reg, data2);
	init_wr32(env, 0x00184c, savepci19);
	init_mask(env, 0x001850, 0x00000001, 0x00000000);

	init_exec_force(env, 0);
}

// init_configure_mem (init.c:1512-1556), INIT_CONFIGURE_MEM 0x66; likewise
// INIT_CONFIGURE_CLK 0x67 (init.c:1562-1590) and INIT_CONFIGURE_PREINIT 0x68
// (init.c:1596-1616). All three take the BMP memory-init tables and, when
// bios->version.major > 2 (every BIT-era VBIOS, incl. all GP10x), reduce to
// init_done (init.c:1522-1525, :1571-1574, :1605-1608). This core only ever
// sees BIT-era images, so the BMP table walk below that gate is not ported
// (SIM DEVIATION: a BMP-era image would terminate here instead of doing
// BMP meminit).
static void
op_configure_mem(struct pg106_init_env *env)
{
	env->offset = 0x0000; // version.major > 2 -> init_done
}

static void
op_configure_clk(struct pg106_init_env *env)
{
	env->offset = 0x0000; // version.major > 2 -> init_done
}

static void
op_configure_preinit(struct pg106_init_env *env)
{
	env->offset = 0x0000; // version.major > 2 -> init_done
}

// init_io (init.c:1619-1656), INIT_IO 0x69. master reads the mask/data bytes
// with 16-bit loads truncated to u8 (offset +3 and +4 overlap the same
// words); the effect is plain byte loads at +3/+4, reproduced literally.
// The NV_50+ port-0x03c3/data-0x01 special case (init.c:1634-1652) is the
// display-restore hack — GP106 is always NV_50+, so it always applies.
static void
op_io(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint16_t port = bi_rd16(env, (uint16_t)(at + 1));
	uint8_t mask = bi_rd08(env, (uint16_t)(at + 3));
	uint8_t data = bi_rd08(env, (uint16_t)(at + 4));
	uint8_t value;

	env->offset = (uint16_t)(at + 5);

	if (port == 0x03c3 && data == 0x01) {
		// init.c:1640-1651, verbatim (mdelay(10) -> fake clock).
		init_mask(env, 0x614100, 0xf0800000, 0x00800000);
		init_mask(env, 0x00e18c, 0x00020000, 0x00020000);
		init_mask(env, 0x614900, 0xf0800000, 0x00800000);
		init_mask(env, 0x000200, 0x40000000, 0x00000000);
		init_delay(env, 10 * 1000);
		init_mask(env, 0x00e18c, 0x00020000, 0x00000000);
		init_mask(env, 0x000200, 0x40000000, 0x40000000);
		init_wr32(env, 0x614100, 0x00800018);
		init_wr32(env, 0x614900, 0x00800018);
		init_delay(env, 10 * 1000);
		init_wr32(env, 0x614100, 0x10000018);
		init_wr32(env, 0x614900, 0x10000018);
	}

	value = (uint8_t)(init_rdport(env, port) & mask);
	init_wrport(env, port, (uint8_t)(data | value));
}

// init_sub (init.c:1659-1683), INIT_SUB 0x6b. master also has a BMP-era
// fallback inside init_script(); BIT-era scripts (everything Pascal) take
// the table path modelled here.
static void
op_sub(struct pg106_init_env *env)
{
	uint8_t index = bi_rd08(env, (uint16_t)(env->offset + 1));
	uint16_t addr = init_script(env, index);

	exec_sub_script(env, addr);
	env->offset = (uint16_t)(env->offset + 2);
}

// init_ram_condition (init.c:1686-1702), INIT_RAM_CONDITION 0x6d.
static void
op_ram_condition(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint8_t mask = bi_rd08(env, (uint16_t)(at + 1));
	uint8_t value = bi_rd08(env, (uint16_t)(at + 2));

	env->offset = (uint16_t)(at + 3);
	if ((init_rd32(env, 0x100000) & mask) != value)
		init_exec_set(env, 0);
}

// init_nv_reg (init.c:1705-1720), INIT_NV_REG 0x6e. "R &= mask |= data":
// init_mask(reg, ~mask, data) computes (rd & mask) | data.
static void
op_nv_reg(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint32_t reg = bi_rd32(env, (uint16_t)(at + 1));
	uint32_t mask = bi_rd32(env, (uint16_t)(at + 5));
	uint32_t data = bi_rd32(env, (uint16_t)(at + 9));

	env->offset = (uint16_t)(at + 13);
	init_mask(env, reg, ~mask, data);
}

// init_macro (init.c:1723-1744), INIT_MACRO 0x6f. 8-byte entries
// {address, data} in the macro table.
static void
op_macro(struct pg106_init_env *env)
{
	uint8_t macro = bi_rd08(env, (uint16_t)(env->offset + 1));
	uint16_t table = env->table_macro;

	env->offset = (uint16_t)(env->offset + 2);
	if (!table) {
		fail(env, PG106_INIT_ERR_NO_TABLE,
		     "macro table absent");
		return;
	}
	if (macro >= env->macro_count) {
		fail(env, PG106_INIT_ERR_RANGE,
		     "macro index out of range");
		return;
	}
	uint32_t addr = bi_rd32(env, (uint16_t)(table + macro * 8 + 0));
	uint32_t data = bi_rd32(env, (uint16_t)(table + macro * 8 + 4));
	init_wr32(env, addr, data);
}

// init_done (init.c:606-614), INIT_DONE 0x71 (mission alias INIT_END).
static void
op_done(struct pg106_init_env *env)
{
	env->offset = 0x0000;
}

// init_resume (init.c:1747-1756), INIT_RESUME 0x72.
static void
op_resume(struct pg106_init_env *env)
{
	env->offset = (uint16_t)(env->offset + 1);
	init_exec_set(env, 1);
}

// init_strap_condition (init.c:1759-1774), INIT_STRAP_CONDITION 0x73.
static void
op_strap_condition(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint32_t mask = bi_rd32(env, (uint16_t)(at + 1));
	uint32_t value = bi_rd32(env, (uint16_t)(at + 5));

	env->offset = (uint16_t)(at + 9);
	if ((init_rd32(env, 0x101000) & mask) != value)
		init_exec_set(env, 0);
}

// init_time (init.c:1777-1795), INIT_TIME 0x74. Deterministic stand-in for
// udelay/mdelay: recorded as a TIME trace record. > 1 ms delays round up to
// whole milliseconds exactly like mdelay((usec + 900) / 1000).
static void
op_time(struct pg106_init_env *env)
{
	uint16_t usec = bi_rd16(env, (uint16_t)(env->offset + 1));

	env->offset = (uint16_t)(env->offset + 3);
	if (!init_exec(env))
		return;
	if (usec < 1000)
		init_delay(env, usec);
	else
		init_delay(env, ((usec + 900) / 1000) * 1000u);
}

// init_condition (init.c:1798-1812), INIT_CONDITION 0x75
// (mission alias INIT_CONDITION_SET).
static void
op_condition(struct pg106_init_env *env)
{
	uint8_t cond = bi_rd08(env, (uint16_t)(env->offset + 1));

	env->offset = (uint16_t)(env->offset + 2);
	if (!init_condition_met(env, cond))
		init_exec_set(env, 0);
}

// init_io_condition (init.c:1815-1829), INIT_IO_CONDITION 0x76.
static void
op_io_condition(struct pg106_init_env *env)
{
	uint8_t cond = bi_rd08(env, (uint16_t)(env->offset + 1));

	env->offset = (uint16_t)(env->offset + 2);
	if (!init_io_condition_met(env, cond))
		init_exec_set(env, 0);
}

// init_zm_reg16 (init.c:1832-1846), INIT_ZM_REG16 0x77.
static void
op_zm_reg16(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint32_t addr = bi_rd32(env, (uint16_t)(at + 1));
	uint16_t data = bi_rd16(env, (uint16_t)(at + 5));

	env->offset = (uint16_t)(at + 7);
	init_wr32(env, addr, data);
}

// init_index_io (init.c:1849-1868), INIT_INDEX_IO 0x78 (mission alias
// INIT_INDEXED_IO_REG). master loads the index with a 16-bit read truncated
// to u8; identical to a byte load at +3.
static void
op_index_io(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint16_t port = bi_rd16(env, (uint16_t)(at + 1));
	uint8_t index = bi_rd08(env, (uint16_t)(at + 3));
	uint8_t mask = bi_rd08(env, (uint16_t)(at + 4));
	uint8_t data = bi_rd08(env, (uint16_t)(at + 5));
	uint8_t value;

	env->offset = (uint16_t)(at + 6);
	value = (uint8_t)(init_rdvgai(env, port, index) & mask);
	init_wrvgai(env, port, index, (uint8_t)(data | value));
}

// init_pll (init.c:1873-1885), INIT_PLL 0x79. freq = u16 operand * 10 kHz.
static void
op_pll(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint32_t reg = bi_rd32(env, (uint16_t)(at + 1));
	uint32_t freq = (uint32_t)bi_rd16(env, (uint16_t)(at + 5)) * 10;

	env->offset = (uint16_t)(at + 7);
	init_prog_pll(env, reg, freq);
}

// init_zm_reg (init.c:1888-1905), INIT_ZM_REG 0x7a. Writing PMC 0x000200
// always has bit 0 set (trigger bit), hence the data |= 1 special case.
static void
op_zm_reg(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint32_t addr = bi_rd32(env, (uint16_t)(at + 1));
	uint32_t data = bi_rd32(env, (uint16_t)(at + 5));

	env->offset = (uint16_t)(at + 9);
	if (addr == 0x000200)
		data |= 0x00000001;
	init_wr32(env, addr, data);
}

// init_ram_restrict_pll (init.c:1911-1935), INIT_RAM_RESTRICT_PLL 0x87.
// The operand is a pll TYPE byte (enum nvbios_pll_type, pll.h:28-42), not a
// register address; pll_map_type resolves it against the limits table.
static void
op_ram_restrict_pll(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint8_t type = bi_rd08(env, (uint16_t)(at + 1));
	uint8_t count = init_ram_restrict_group_count(env);
	uint8_t strap = init_ram_restrict(env);
	uint8_t cconf;

	env->offset = (uint16_t)(at + 2);
	for (cconf = 0; cconf < count; cconf++) {
		uint32_t freq = bi_rd32(env, env->offset);

		if (cconf == strap)
			init_prog_pll(env, type, freq);
		env->offset = (uint16_t)(env->offset + 4);
	}
}

// init_reset_begun (init.c:1938-1946), INIT_RESET_BEGUN 0x8c.
static void
op_reset_begun(struct pg106_init_env *env)
{
	env->offset = (uint16_t)(env->offset + 1);
}

// init_reset_end (init.c:1949-1957), INIT_RESET_END 0x8d.
static void
op_reset_end(struct pg106_init_env *env)
{
	env->offset = (uint16_t)(env->offset + 1);
}

// init_gpio (init.c:1960-1973), INIT_GPIO 0x8e. nvkm_gpio_reset walks the
// DCB gpio table; this module hands the event to the gpio_reset stub hook
// instead. A missing hook counts a warning (master would deref NULL).
static void
op_gpio(struct pg106_init_env *env)
{
	env->offset = (uint16_t)(env->offset + 1);
	if (!init_exec(env))
		return;
	if (env->vt->gpio_reset)
		env->vt->gpio_reset(env);
	else
		warn(env, "no gpio_reset hook provided");
}

// init_ram_restrict_zm_reg_group (init.c:1976-2011),
// INIT_RAM_RESTRICT_ZM_GROUP 0x8f (mission alias INIT_RAM_RESTRICT_ZONE).
// num registers, each with ram_restrict_groups candidate values; the group
// selected by the strap (ramcfg_index) is the one written.
static void
op_ram_restrict_zm_reg_group(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint32_t addr = bi_rd32(env, (uint16_t)(at + 1));
	uint8_t incr = bi_rd08(env, (uint16_t)(at + 5));
	uint8_t num = bi_rd08(env, (uint16_t)(at + 6));
	uint8_t count = init_ram_restrict_group_count(env);
	uint8_t index = init_ram_restrict(env);
	uint8_t i, j;

	env->offset = (uint16_t)(at + 7);
	for (i = 0; i < num; i++) {
		for (j = 0; j < count; j++) {
			uint32_t data = bi_rd32(env, env->offset);
			if (j == index)
				init_wr32(env, addr, data);
			env->offset = (uint16_t)(env->offset + 4);
		}
		addr += incr;
	}
}

// init_copy_zm_reg (init.c:2014-2028), INIT_COPY_ZM_REG 0x90.
static void
op_copy_zm_reg(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint32_t sreg = bi_rd32(env, (uint16_t)(at + 1));
	uint32_t dreg = bi_rd32(env, (uint16_t)(at + 5));

	env->offset = (uint16_t)(at + 9);
	init_wr32(env, dreg, init_rd32(env, sreg));
}

// init_zm_reg_group (init.c:2031-2050), INIT_ZM_REG_GROUP 0x91.
static void
op_zm_reg_group(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint32_t addr = bi_rd32(env, (uint16_t)(at + 1));
	uint8_t count = bi_rd08(env, (uint16_t)(at + 5));

	env->offset = (uint16_t)(at + 6);
	while (count--) {
		uint32_t data = bi_rd32(env, env->offset);
		env->offset = (uint16_t)(env->offset + 4);
		init_wr32(env, addr, data);
	}
}

// init_reserved (init.c:578-603), INIT_RESERVED 0x92 (1 byte) and
// INIT_RESERVED4 0xaa (4 bytes).
static void
op_reserved(struct pg106_init_env *env)
{
	env->offset = (uint16_t)(env->offset + 1);
}

static void
op_reserved4(struct pg106_init_env *env)
{
	env->offset = (uint16_t)(env->offset + 4);
}

// init_xlat (init.c:2053-2078), INIT_XLAT 0x96.
static void
op_xlat(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint32_t saddr = bi_rd32(env, (uint16_t)(at + 1));
	uint8_t sshift = bi_rd08(env, (uint16_t)(at + 5));
	uint8_t smask = bi_rd08(env, (uint16_t)(at + 6));
	uint8_t index = bi_rd08(env, (uint16_t)(at + 7));
	uint32_t daddr = bi_rd32(env, (uint16_t)(at + 8));
	uint32_t dmask = bi_rd32(env, (uint16_t)(at + 12));
	uint8_t shift = bi_rd08(env, (uint16_t)(at + 16));
	uint32_t data;

	env->offset = (uint16_t)(at + 17);
	data = init_shift(init_rd32(env, saddr), sshift) & smask;
	data = (uint32_t)init_xlat_(env, index, (uint8_t)data) << shift;
	init_mask(env, daddr, ~dmask, data);
}

// init_zm_mask_add (init.c:2081-2099), INIT_ZM_MASK_ADD 0x97.
static void
op_zm_mask_add(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint32_t addr = bi_rd32(env, (uint16_t)(at + 1));
	uint32_t mask = bi_rd32(env, (uint16_t)(at + 5));
	uint32_t add = bi_rd32(env, (uint16_t)(at + 9));
	uint32_t data;

	env->offset = (uint16_t)(at + 13);
	data = init_rd32(env, addr);
	data = (data & mask) | ((data + add) & ~mask);
	init_wr32(env, addr, data);
}

// init_i2c_long_if (init.c:2151-2184), INIT_I2C_LONG_IF 0x9a. Two-message
// i2c_transfer (write the 16-bit register offset, read one byte) behind the
// reg16 hook; no adapter or a failed transfer or an unmet comparison all
// disable execution exactly like master's fall-through to
// init_exec_set(init, false).
static void
op_i2c_long_if(struct pg106_init_env *env)
{
	uint16_t at = env->offset;
	uint8_t index = bi_rd08(env, (uint16_t)(at + 1));
	uint8_t addr = (uint8_t)(bi_rd08(env, (uint16_t)(at + 2)) >> 1);
	uint8_t reglo = bi_rd08(env, (uint16_t)(at + 3));
	uint8_t reghi = bi_rd08(env, (uint16_t)(at + 4));
	uint8_t mask = bi_rd08(env, (uint16_t)(at + 5));
	uint8_t data = bi_rd08(env, (uint16_t)(at + 6));
	uint8_t value = 0;
	int met = 0;

	env->offset = (uint16_t)(at + 7);
	// master performs the transfer whenever the adapter exists — no
	// init_exec() gate (init.c:2168-2183), reproduced.
	if (env->vt->rd_i2c_reg16) {
		int ret = env->vt->rd_i2c_reg16(env, init_i2c_bus(env, index),
						addr, reghi, reglo, &value);
		if (ret == 0 && (uint8_t)(value & mask) == data)
			met = 1;
	}
	if (!met)
		init_exec_set(env, 0);
}

// ---------------------------------------------------------------------------
// Public entry points.
// ---------------------------------------------------------------------------

void
pg106_init_env_init(struct pg106_init_env *env,
		    const struct pg106_init_vt *vt,
		    const uint8_t *image, size_t image_size,
		    struct pg106_init_trace_rec *trace, size_t trace_cap)
{
	memset(env, 0, sizeof(*env));
	env->vt = vt;
	env->image = image;
	env->image_size = image_size;
	env->trace = trace;
	env->trace_cap = trace_cap;
	env->head = -1;
	env->or = -1;
	env->link = 0;
	env->execute = 1;
	env->status = PG106_INIT_OK;
	env->step_limit = PG106_INIT_STEP_LIMIT_DEFAULT;
}

int
pg106_init_run(struct pg106_init_env *env, uint16_t script_offset)
{
	if (!env || !env->vt || !env->image || !env->image_size)
		return PG106_INIT_ERR_BADARG;
	if (!env->vt->rd32 || !env->vt->wr32 || !env->vt->rd8 ||
	    !env->vt->wr8 || !env->vt->rdport || !env->vt->wrport ||
	    !env->vt->time)
		return PG106_INIT_ERR_BADARG;

	env->offset = script_offset;
	env->execute = 1;
	env->nested = 0;
	env->repeat = 0;
	env->repend = 0;
	env->steps = 0;
	env->trace_len = 0;
	env->status = PG106_INIT_OK;

	exec_script(env);
	return env->status;
}
