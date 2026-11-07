// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/devinit.h — VBIOS DEVINIT script interpreter
// (register-write trace producer).
//
// Derives from nouveau, linux master nvkm/subdev/bios/init.c (sparse clone
// /tmp/review-ref/linux/drivers/gpu/drm/nouveau, torvalds/linux d48723711bae).
// Every opcode constant cites its master handler; the dispatch table is
// init.c:2227-2299 and is fully implemented. Struct/flag semantics mirror
// include/nvkm/subdev/bios/init.h (struct nvbios_init: u32 offset,
// execute-bit dance, repeat/repend, outp/or/link/head context).
//
// SIMULATION component (HANDOFF-PASCAL.md §6 Phase 1): executes VBIOS init
// scripts against a caller-supplied bus model and records every side
// effect. Not hardware evidence.
//
// GP10x scope — read this before wiring it into a POST path:
//   On GP106 nouveau does NOT host-interpret the BIT 'I' boot scripts.
//   gm200_devinit_post (devinit/gm200.c:123-176) uploads them, with the
//   init tables, to the VBIOS PMU DEVINIT application, which runs them on
//   the PMU falcon (modelled in devinit_pmu.h). nouveau uses THIS
//   interpreter on GP10x for: DCB encoder scripts after a POST
//   (devinit/nv50.c:118-140), the display supervisor IED scripts
//   (engine/disp/nv50.c:1029-1108), the DP Info scripts around link
//   training (engine/disp/dp.c:369-471), and ramgp100's nvbios_init calls.
//   Those run with init.outp/or/link/head set — env->outp/or/link/head here.
//
// Upstream semantics kept (no deviation): unknown opcode -> stop with
// -EINVAL (init.c:2309-2313, PG106_INIT_ERR_OPCODE); missing sub-tables ->
// helpers return false / no-op with a warning (init.c:376-406, 478-528,
// 1727-1744); PLL failures warn and continue (init.c:343-352).
// Deviations: OOB image reads abort (PG106_INIT_ERR_OVERRUN; master reads
// 0), step budget + nesting cap, divide-by-zero PLL inputs abort, fake
// clock + trace recorder.

#ifndef PASCAL_DEVINIT_H
#define PASCAL_DEVINIT_H

#include <stddef.h>
#include <stdint.h>

#include <pascal/vbios_bit.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Opcode constants (init.c dispatch table, lines 2230-2299).
//
// master init.c does not define INIT_* macros; each opcode is identified by
// its handler function (init_<name>). Constants below use those handler
// names. Where the Phase 1 mission names an opcode differently, the alias is
// noted; names with no counterpart in master are listed at the bottom.
// ---------------------------------------------------------------------------

#define PG106_INIT_IO_RESTRICT_PROG   0x32u // init_io_restrict_prog
#define PG106_INIT_REPEAT             0x33u // init_repeat
#define PG106_INIT_IO_RESTRICT_PLL    0x34u // init_io_restrict_pll (init.c:684)
#define PG106_INIT_END_REPEAT         0x36u // init_end_repeat
#define PG106_INIT_COPY               0x37u // init_copy
#define PG106_INIT_NOT                0x38u // init_not
#define PG106_INIT_IO_FLAG_CONDITION  0x39u // init_io_flag_condition
#define PG106_INIT_GENERIC_CONDITION  0x3au // init_generic_condition (init.c:796; outp/conn/AUX)
#define PG106_INIT_IO_MASK_OR         0x3bu // init_io_mask_or
#define PG106_INIT_IO_OR              0x3cu // init_io_or
#define PG106_INIT_ANDN_REG           0x47u // init_andn_reg
#define PG106_INIT_OR_REG             0x48u // init_or_reg
#define PG106_INIT_INDEX_ADDRESS_LATCHED 0x49u // init_idx_addr_latched
#define PG106_INIT_IO_RESTRICT_PLL2   0x4au // init_io_restrict_pll2 (init.c:950)
#define PG106_INIT_PLL2               0x4bu // init_pll2 (init.c:985)
#define PG106_INIT_I2C_BYTE           0x4cu // init_i2c_byte (init.c:1002)
#define PG106_INIT_ZM_I2C_BYTE        0x4du // init_zm_i2c_byte (init.c:1033)
#define PG106_INIT_ZM_I2C             0x4eu // init_zm_i2c (init.c:1059)
#define PG106_INIT_TMDS               0x4fu // init_tmds (init.c:1093; outp->or)
#define PG106_INIT_ZM_TMDS_GROUP      0x50u // init_zm_tmds_group (init.c:1119)
#define PG106_INIT_CR_INDEX_ADDRESS_LATCHED 0x51u // init_cr_idx_adr_latch
#define PG106_INIT_CR                 0x52u // init_cr
#define PG106_INIT_ZM_CR              0x53u // init_zm_cr
#define PG106_INIT_ZM_CR_GROUP        0x54u // init_zm_cr_group
#define PG106_INIT_CONDITION_TIME     0x56u // init_condition_time
#define PG106_INIT_LTIME              0x57u // init_ltime
#define PG106_INIT_ZM_REG_SEQUENCE    0x58u // init_zm_reg_sequence
#define PG106_INIT_PLL_INDIRECT       0x59u // init_pll_indirect (init.c:1306)
#define PG106_INIT_ZM_REG_INDIRECT    0x5au // init_zm_reg_indirect
#define PG106_INIT_SUB_DIRECT         0x5bu // init_sub_direct
#define PG106_INIT_JUMP               0x5cu // init_jump
#define PG106_INIT_I2C_IF             0x5eu // init_i2c_if (init.c:1388)
#define PG106_INIT_COPY_NV_REG        0x5fu // init_copy_nv_reg
#define PG106_INIT_ZM_INDEX_IO        0x62u // init_zm_index_io
#define PG106_INIT_COMPUTE_MEM        0x63u // init_compute_mem (no-op: gm200+ devinit has no .meminit)
#define PG106_INIT_RESET              0x65u // init_reset
#define PG106_INIT_CONFIGURE_MEM      0x66u // init_configure_mem (init_done on BIT-era, init.c:1522-1525)
#define PG106_INIT_CONFIGURE_CLK      0x67u // init_configure_clk (init_done on BIT-era, init.c:1571-1574)
#define PG106_INIT_CONFIGURE_PREINIT  0x68u // init_configure_preinit (init_done on BIT-era, init.c:1605-1608)
#define PG106_INIT_IO                 0x69u // init_io
#define PG106_INIT_SUB                0x6bu // init_sub
#define PG106_INIT_RAM_CONDITION      0x6du // init_ram_condition
#define PG106_INIT_NV_REG             0x6eu // init_nv_reg
#define PG106_INIT_MACRO              0x6fu // init_macro
#define PG106_INIT_DONE               0x71u // init_done (terminates; mission alias INIT_END)
#define PG106_INIT_RESUME             0x72u // init_resume
#define PG106_INIT_STRAP_CONDITION    0x73u // init_strap_condition
#define PG106_INIT_TIME               0x74u // init_time
#define PG106_INIT_CONDITION          0x75u // init_condition (mission alias INIT_CONDITION_SET)
#define PG106_INIT_IO_CONDITION       0x76u // init_io_condition
#define PG106_INIT_ZM_REG16           0x77u // init_zm_reg16
#define PG106_INIT_INDEX_IO           0x78u // init_index_io (mission alias INIT_INDEXED_IO_REG)
#define PG106_INIT_PLL                0x79u // init_pll (init.c:1875)
#define PG106_INIT_ZM_REG             0x7au // init_zm_reg
#define PG106_INIT_RAM_RESTRICT_PLL   0x87u // init_ram_restrict_pll (init.c:1912)
#define PG106_INIT_RESET_BEGUN        0x8cu // init_reset_begun
#define PG106_INIT_RESET_END          0x8du // init_reset_end
#define PG106_INIT_GPIO               0x8eu // init_gpio
#define PG106_INIT_RAM_RESTRICT_ZM_GROUP 0x8fu // init_ram_restrict_zm_reg_group
#define PG106_INIT_COPY_ZM_REG        0x90u // init_copy_zm_reg
#define PG106_INIT_ZM_REG_GROUP       0x91u // init_zm_reg_group
#define PG106_INIT_RESERVED           0x92u // init_reserved (1-byte reserved stub)
#define PG106_INIT_XLAT               0x96u // init_xlat
#define PG106_INIT_ZM_MASK_ADD        0x97u // init_zm_mask_add
#define PG106_INIT_AUXCH              0x98u // init_auxch (init.c:2106; outp AUX)
#define PG106_INIT_ZM_AUXCH           0x99u // init_zm_auxch (init.c:2130)
#define PG106_INIT_I2C_LONG_IF        0x9au // init_i2c_long_if (init.c:2152)
#define PG106_INIT_GPIO_NE            0xa9u // init_gpio_ne (init.c:2191; DCB gpio table)
#define PG106_INIT_RESERVED4          0xaau // init_reserved (4-byte reserved stub)

// Mission-listed names with NO counterpart in master init.c — encountering
// the concept is reported as "not present upstream"; any byte value is
// dispatched only by the table above:
//   INIT_NOP               master has no 0x00 handler; byte 0x00 hits the
//                          "unknown opcode" error path (init.c:2309-2312),
//                          which this interpreter reproduces.
//   INIT_PROGRAM_CRITICAL  not present in master dispatch.
//   INIT_MEM               not present (closest is 0x63 INIT_COMPUTE_MEM).
//   INIT_COMPARATOR        not present (closest is 0x56 INIT_CONDITION_TIME).
//   INIT_CONDITION_DONE    not present (0x75/0x72 form the set/resume pair).
//   INIT_CASE              not present in master dispatch.
//   INIT_FINITE_END        not present (0x71 INIT_DONE is the only terminator).
//   INIT_INDEXED_IO conditioned variants: covered by 0x76 INIT_IO_CONDITION,
//                          0x39 INIT_IO_FLAG_CONDITION, 0x3b/0x3c IO_MASK_OR/
//                          IO_OR, and strap-selecting 0x32/0x8f.
//   INIT_RAM_RESTRICT_SEL  the ramcfg strap *selection* is data, not an
//                          opcode: caller supplies pg106_init_env.ramcfg_index
//                          (init.c init_ram_restrict, line 444).

// ---------------------------------------------------------------------------
// Status codes.
// ---------------------------------------------------------------------------

enum pg106_init_status {
	PG106_INIT_OK           = 0,     // script ran to INIT_DONE
	PG106_INIT_ERR_OPCODE   = -1,    // byte not in nvkm's dispatch (master: -EINVAL stop)
	PG106_INIT_ERR_OVERRUN  = -2,    // operand/subtable read past end of BIOS image
	PG106_INIT_ERR_TRACE_FULL = -3,  // trace record buffer exhausted
	PG106_INIT_ERR_NO_TABLE = -4,    // reserved (missing tables now warn, as upstream)
	PG106_INIT_ERR_DEPTH    = -5,    // SUB/SUB_DIRECT nesting deeper than PG106_INIT_MAX_NESTING
	PG106_INIT_ERR_STEPS    = -6,    // step budget exceeded (runaway-loop guard)
	PG106_INIT_ERR_BADARG   = -7,    // NULL vtable/image
	PG106_INIT_ERR_RANGE    = -8,    // PLL calc input that would divide by zero
};

// Default opcode-step budget per run. init.c has no such guard (the kernel
// trusts the VBIOS); a simulator needs one to turn a runaway script into an
// error instead of a hang. Override via env.step_limit (0 = unlimited).
#define PG106_INIT_STEP_LIMIT_DEFAULT (1u << 20)

// SUB/SUB_DIRECT recursion cap. init.c relies on nvbios_exec's plain
// recursion with no limit; a self-sub script would blow the C stack.
#define PG106_INIT_MAX_NESTING 16

// ---------------------------------------------------------------------------
// Trace recorder.
// ---------------------------------------------------------------------------

// Record kinds. Every MMIO write produces a PG106_INIT_TRACE_MMIO record
// (mission shape {address, value, script_offset, opcode}); indexed/direct IO
// writes, simulated delays and I2C writes are also recorded so a whole run
// is reproducible byte-for-byte.
#define PG106_INIT_TRACE_MMIO 0u
#define PG106_INIT_TRACE_IO   1u
#define PG106_INIT_TRACE_TIME 2u
#define PG106_INIT_TRACE_I2C  3u
#define PG106_INIT_TRACE_AUX  4u // address = DPCD addr, index = aux (i2c_index)
#define PG106_INIT_TRACE_GPIO 5u // address = DCB gpio func, index = line

struct pg106_init_trace_rec {
	uint32_t address;       // MMIO address (MMIO), IO port base (IO), or
				// (bus << 8) | device address (I2C); 0 for TIME
	uint32_t value;         // value written; delay in microseconds for TIME
	uint16_t index;         // indexed-IO index register (IO), I2C register
				// offset (I2C_BYTE/byte writes) or block byte
				// position (block writes); 0 otherwise
	uint32_t script_offset; // BIOS offset of the opcode that produced the record
	uint8_t  opcode;        // opcode byte at script_offset
	uint8_t  kind;          // PG106_INIT_TRACE_*
};

// ---------------------------------------------------------------------------
// Bus/time stub-hook vtable (the "hardware" underneath the interpreter).
//
// A test or simulator supplies these; production wiring (Phase 3 kext) would
// back rd32/wr32 with BAR0 and the *8/*port hooks with VGA IO ports. All
// hooks may read pg106_init_env for context (head/or/link). Reads returning
// without touching anything must be deterministic for traces to be.
// ---------------------------------------------------------------------------

struct pg106_init_env; // defined below (the vtable's hooks receive it)

struct pg106_init_vt {
	// BAR0 MMIO (nvkm_rd32/nvkm_wr32 equivalents, init.c:182-198).
	uint32_t (*rd32)(struct pg106_init_env *env, uint32_t addr);
	void (*wr32)(struct pg106_init_env *env, uint32_t addr, uint32_t val);

	// Indexed IO (nvkm_rdvgai/nvkm_wrvgai equivalents, init.c:228-260):
	// write index register `index` at `port`, then access the data port.
	uint8_t (*rd8)(struct pg106_init_env *env, uint16_t port, uint8_t index);
	void (*wr8)(struct pg106_init_env *env, uint16_t port, uint8_t index,
		    uint8_t val);

	// Direct byte IO ports (nvkm_rdport/nvkm_wrport, init.c:213-226),
	// used by INIT_IO (0x69).
	uint8_t (*rdport)(struct pg106_init_env *env, uint16_t port);
	void (*wrport)(struct pg106_init_env *env, uint16_t port, uint8_t val);

	// 32-bit IO port read. No master opcode issues one; reserved for the
	// bus stub-hook interface so host tooling can probe through the same
	// vtable. May be NULL.
	uint32_t (*rd32_io)(struct pg106_init_env *env, uint16_t port);

	// Fake clock: advance by delay_us and return the resulting time.
	// Backs udelay/mdelay in init.c (INIT_TIME 0x74, INIT_LTIME 0x57,
	// INIT_CONDITION_TIME 0x56, INIT_RESET 0x65, INIT_IO 0x69 special
	// case). Deterministic environments just accumulate.
	uint64_t (*time)(struct pg106_init_env *env, uint32_t delay_us);

	// INIT_GPIO (0x8e) side effect: nvkm_gpio_reset(gpio, DCB_GPIO_UNUSED)
	// in init.c:1963-1973. The full reset walks the DCB gpio table, which
	// this module does not model — the hook receives the event instead.
	// May be NULL (counts as a warning).
	void (*gpio_reset)(struct pg106_init_env *env);

	// I2C transactions backing init_rdi2cr/init_wri2cr (init.c:284-300)
	// and the i2c_transfer calls in INIT_ZM_I2C (init.c:1076-1085) /
	// INIT_I2C_LONG_IF (init.c:2168-2183). The `bus` argument is the
	// NVKM_I2C_BUS_* index after master's 0xff/0x80/0x81 remap
	// (init.c:268-278, see init_i2c_bus in devinit.c); `addr` is the 7-bit
	// device address (the script byte >> 1). All optional — NULL means
	// "no adapter", which reproduces master's degradation exactly:
	//   rd_i2cr / wr_i2cr: nvkm_rdi2cr/nvkm_wri2cr (adap == NULL ->
	//     -ENODEV, INIT_I2C_BYTE skips the write, INIT_I2C_IF compares
	//     against the truncated error code — bug-compatible);
	//   wr_i2c_block: one write i2c_msg (INIT_ZM_I2C); return the number
	//     of messages transferred (1 on success). NULL -> master's
	//     adap == NULL silent skip; non-1 return -> warn and continue.
	//   rd_i2c_reg16: write-2-bytes-then-read-1 i2c_transfer pair
	//     (INIT_I2C_LONG_IF); return 0 on success. NULL -> adap == NULL
	//     -> execution disabled, exactly like master.
	// Every completed I2C write appends a PG106_INIT_TRACE_I2C record.
	// (Kept at the tail of the struct so positional initializers in
	// existing callers keep binding to the original nine hooks.)
	int (*rd_i2cr)(struct pg106_init_env *env, uint8_t bus, uint8_t addr,
		       uint8_t reg);
	void (*wr_i2cr)(struct pg106_init_env *env, uint8_t bus, uint8_t addr,
			uint8_t reg, uint8_t val);
	int (*wr_i2c_block)(struct pg106_init_env *env, uint8_t bus,
			    uint8_t addr, const uint8_t *buf, uint8_t len);
	int (*rd_i2c_reg16)(struct pg106_init_env *env, uint8_t bus,
			    uint8_t addr, uint8_t reg_hi, uint8_t reg_lo,
			    uint8_t *val);

	// DPCD access on the script outp's AUX channel (nvkm_rdaux/
	// nvkm_wraux of init_rdauxr/init_wrauxr, init.c:314-341); `aux` is
	// outp->i2c_index (nvkm_i2c_aux_find). Return 0 on success. Both NULL
	// = no AUX channel found (master: aux == NULL). Optional.
	int (*aux_rd)(struct pg106_init_env *env, uint8_t aux, uint32_t addr,
		      uint8_t *data);
	int (*aux_wr)(struct pg106_init_env *env, uint8_t aux, uint32_t addr,
		      uint8_t data);
	// INIT_GPIO_NE side effect: nvkm_gpio_reset(gpio, func)
	// (init.c:2217-2219). Optional (NULL counts a warning).
	void (*gpio_reset_func)(struct pg106_init_env *env, uint8_t func);
};

// ---------------------------------------------------------------------------
// Interpreter environment (mirrors struct nvbios_init + the BIOS subtable
// pointers init.c resolves from the BIT 'I' init data table).
// ---------------------------------------------------------------------------

struct pg106_init_env {
	// --- caller-supplied configuration ---
	const struct pg106_init_vt *vt;
	const uint8_t *image;   // raw BIOS image (script offsets index into this)
	size_t image_size;

	// nvbios_addr remap (base.c:31-39): addresses >= image0_size go to
	// imaged_addr + (addr - image0_size) when imaged_addr != 0. Filled by
	// pg106_init_env_from_bios; 0/0 = flat image.
	uint32_t image0_size;
	uint32_t imaged_addr;

	// BIT 'I' init-data subtable pointers (init.c:399-406, read through
	// init_table_). 0 = absent -> upstream degradation (warning + false/
	// no-op), never an abort. Master bounds no entry index either, so
	// there are no counts.
	uint32_t table_script;            // +0x00 script table (INIT_SUB 0x6b)
	uint32_t table_macro;             // +0x04 macro table (INIT_MACRO 0x6f)
	uint32_t table_condition;         // +0x06 condition table (0x75/0x56)
	uint32_t table_io_condition;      // +0x08 io condition table (0x76)
	uint32_t table_io_flag_condition; // +0x0a io flag condition table (0x39/0x34)
	uint32_t table_xlat;              // +0x10 xlat table (0x96)

	// PLL limits table header pointer: BIT 'C' v1 rd16(+8), v2 rd32(+0)
	// (pll.c:81-98). u32 — the v2 pointer is a full u32.
	uint32_t table_pll;

	// Crystal frequency in kHz (nvkm device->crystal, pll.c:386-398).
	uint32_t crystal_khz;

	// RAM restrict strap selection (init.c:438-458): group count is
	// nvbios_ramcfg_count (BIT 'M'); the strap index needs hardware.
	uint8_t ram_restrict_groups;
	uint8_t ramcfg_index;

	// Optional parsed BIOS (DCB connector/GPIO tables for init_conn and
	// INIT_GPIO_NE). NULL = no tables, as on a BIOS without them.
	const struct pg106_vbios_bit *bios;

	// Script output context (struct nvbios_init.outp). NULL = none
	// (boot scripts). DCB encoder/IED/DP scripts set it.
	const struct pg106_dcb_outp *outp;
	// nvbios_dpout_match(DCB_OUTPUT_DP, or | sorconf.link << 6) flags
	// for INIT_GENERIC_CONDITION 1/2; -1 = no DP output table data.
	int16_t dpout_flags;

	// Display script context (init.h struct nvbios_init): -1 = unset.
	// Register addresses with bit 31/30/29 set are mangled with these
	// (init.c init_nvreg, lines 143-179).
	int32_t head;
	int32_t or;
	int32_t link;

	// Trace recorder: caller-owned array; full buffer aborts with
	// PG106_INIT_ERR_TRACE_FULL.
	struct pg106_init_trace_rec *trace;
	size_t trace_cap;
	size_t trace_len;

	// Runaway guard; 0 = unlimited (see PG106_INIT_STEP_LIMIT_DEFAULT).
	uint32_t step_limit;

	// --- diagnostics (valid after pg106_init_run) ---
	int32_t status;        // enum pg106_init_status
	char error_msg[160];   // first failure, names opcode + offset
	uint32_t error_offset; // script offset of the failing opcode
	uint8_t error_opcode;  // failing opcode byte
	uint32_t warnings;     // non-fatal deviations master only prints about

	// --- internal interpreter state (nvbios_init equivalents) ---
	uint32_t offset;    // program counter; 0 terminates (init.c:2307)
	uint8_t execute;    // nvkm execute-bit dance, init.c:58-82
	uint32_t nested;    // recursion depth
	uint32_t repeat;    // INIT_REPEAT body start (init.c:657-677)
	uint32_t repend;    // INIT_REPEAT body end
	uint32_t steps;     // opcodes executed this run
	uint32_t step_offset; // offset of opcode currently dispatched
	uint8_t step_opcode;  // opcode currently dispatched
};

// Fill env with defaults (caller then overrides the configuration fields it
// cares about). Does not allocate.
void pg106_init_env_init(struct pg106_init_env *env,
			 const struct pg106_init_vt *vt,
			 const uint8_t *image, size_t image_size,
			 struct pg106_init_trace_rec *trace, size_t trace_cap);

// Execute the script at script_offset within env->image until INIT_DONE
// (0x71, which sets the master-style 0 terminator) or an error. Returns
// env->status (enum pg106_init_status). Reentrant across independent envs;
// the same script + same bus presets produce an identical trace.
int pg106_init_run(struct pg106_init_env *env, uint32_t script_offset);

// Wire env from a parsed BIOS the way nvbios_init resolves things lazily:
// remap inputs, 'I' sub-table pointers (init_table_, init.c:376-406), the
// PLL limits table from BIT 'C' (pll.c:81-98), the RAM-restrict group count
// from BIT 'M' (nvbios_ramcfg_count, ramcfg.c), and env->bios. Missing
// tokens/pointers leave the field 0 (upstream degradation). Returns 0, or
// -ENOENT when the BIOS has no 'I' token.
int pg106_init_env_from_bios(struct pg106_init_env *env,
			     const struct pg106_vbios_bit *vb);

// Human-readable status string ("OK", "unimplemented opcode", ...).
const char *pg106_init_strerr(int status);

// Master handler name for an opcode byte ("INIT_ZM_REG" for 0x7a), or
// "UNKNOWN" for bytes absent from master's dispatch table.
const char *pg106_init_opcode_name(uint8_t opcode);

// Nonzero when the opcode byte has a handler (== present in nvkm's table).
int pg106_init_opcode_implemented(uint8_t opcode);

#ifdef __cplusplus
}
#endif

#endif // PASCAL_DEVINIT_H
