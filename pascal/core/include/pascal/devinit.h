// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/devinit.h — VBIOS DEVINIT script interpreter
// (register-write trace producer).
//
// Derives from nouveau, file
// drivers/gpu/drm/nouveau/nvkm/subdev/bios/init.c (linux master, fetched to
// /tmp/pascal-ref/nouveau/bios-init/init.c; identical copy at
// /tmp/pascal-ref/nouveau/nvkm-subdev-bios/init.c). Every opcode constant
// below cites its master case label / handler in the dispatch table
// init.c:2230-2299 ([opcode] = { handler }). Struct/flag semantics mirror
// include/nvkm/subdev/bios/init.h (struct nvbios_init: execute-bit dance,
// repeat/repend, head/or/link context), reference copy at
// /tmp/pascal-ref/nouveau/bios-init/init.h.
//
// This is a *simulation* component (HANDOFF-PASCAL.md §6 Phase 1): it
// executes DEVINIT scripts from a raw BIOS image against a caller-supplied
// bus model and records every side effect. It is not hardware evidence; the
// Phase 1 gate (identical trace on a second run) is applied to a synthetic
// script in tests/test_devinit.c until a real GP106 VBIOS is available
// (research/evidence/BLOCKERS.md B1).
//
// Scope notes (Phase 1, part B contract):
//   - The API takes a script offset *within a raw BIOS image*; the BIT 'I'
//     init-table parsing that locates that offset lives in the sibling
//     vbios_bit module and is wired in later, outside this file.
//   - init.c resolves subtables (script/condition/macro/...) by reading u16
//     pointers out of the BIT 'I' init data table (init.c:399-406). Here the
//     caller supplies those offsets directly in struct pg106_init_env so this
//     module does not depend on the BIT parser.
//   - Opcodes that need structures this interpreter does not model (I2C/AUX
//     buses, TMDS tables, PLL programming, DCB connector context, BMP-era
//     memory tables) are NOT silently skipped: they stop execution with
//     PG106_INIT_ERR_OPCODE naming the opcode (master instead prints a
//     warning and limps on; a simulator must fail loudly).

#ifndef PASCAL_DEVINIT_H
#define PASCAL_DEVINIT_H

#include <stddef.h>
#include <stdint.h>

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
#define PG106_INIT_IO_RESTRICT_PLL    0x34u // init_io_restrict_pll (NOT implemented: PLL)
#define PG106_INIT_END_REPEAT         0x36u // init_end_repeat
#define PG106_INIT_COPY               0x37u // init_copy
#define PG106_INIT_NOT                0x38u // init_not
#define PG106_INIT_IO_FLAG_CONDITION  0x39u // init_io_flag_condition
#define PG106_INIT_GENERIC_CONDITION  0x3au // init_generic_condition (NOT implemented: DCB/DP)
#define PG106_INIT_IO_MASK_OR         0x3bu // init_io_mask_or
#define PG106_INIT_IO_OR              0x3cu // init_io_or
#define PG106_INIT_ANDN_REG           0x47u // init_andn_reg
#define PG106_INIT_OR_REG             0x48u // init_or_reg
#define PG106_INIT_INDEX_ADDRESS_LATCHED 0x49u // init_idx_addr_latched
#define PG106_INIT_IO_RESTRICT_PLL2   0x4au // init_io_restrict_pll2 (NOT implemented: PLL)
#define PG106_INIT_PLL2               0x4bu // init_pll2 (NOT implemented: PLL)
#define PG106_INIT_I2C_BYTE           0x4cu // init_i2c_byte (NOT implemented: I2C)
#define PG106_INIT_ZM_I2C_BYTE        0x4du // init_zm_i2c_byte (NOT implemented: I2C)
#define PG106_INIT_ZM_I2C             0x4eu // init_zm_i2c (NOT implemented: I2C)
#define PG106_INIT_TMDS               0x4fu // init_tmds (NOT implemented: TMDS table)
#define PG106_INIT_ZM_TMDS_GROUP      0x50u // init_zm_tmds_group (NOT implemented: TMDS)
#define PG106_INIT_CR_INDEX_ADDRESS_LATCHED 0x51u // init_cr_idx_adr_latch
#define PG106_INIT_CR                 0x52u // init_cr
#define PG106_INIT_ZM_CR              0x53u // init_zm_cr
#define PG106_INIT_ZM_CR_GROUP        0x54u // init_zm_cr_group
#define PG106_INIT_CONDITION_TIME     0x56u // init_condition_time
#define PG106_INIT_LTIME              0x57u // init_ltime
#define PG106_INIT_ZM_REG_SEQUENCE    0x58u // init_zm_reg_sequence
#define PG106_INIT_PLL_INDIRECT       0x59u // init_pll_indirect (NOT implemented: PLL)
#define PG106_INIT_ZM_REG_INDIRECT    0x5au // init_zm_reg_indirect
#define PG106_INIT_SUB_DIRECT         0x5bu // init_sub_direct
#define PG106_INIT_JUMP               0x5cu // init_jump
#define PG106_INIT_I2C_IF             0x5eu // init_i2c_if (NOT implemented: I2C)
#define PG106_INIT_COPY_NV_REG        0x5fu // init_copy_nv_reg
#define PG106_INIT_ZM_INDEX_IO        0x62u // init_zm_index_io
#define PG106_INIT_COMPUTE_MEM        0x63u // init_compute_mem (NOT implemented: meminit)
#define PG106_INIT_RESET              0x65u // init_reset
#define PG106_INIT_CONFIGURE_MEM      0x66u // init_configure_mem (NOT implemented: BMP-era)
#define PG106_INIT_CONFIGURE_CLK      0x67u // init_configure_clk (NOT implemented: BMP-era)
#define PG106_INIT_CONFIGURE_PREINIT  0x68u // init_configure_preinit (NOT implemented: BMP-era)
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
#define PG106_INIT_PLL                0x79u // init_pll (NOT implemented: PLL)
#define PG106_INIT_ZM_REG             0x7au // init_zm_reg
#define PG106_INIT_RAM_RESTRICT_PLL   0x87u // init_ram_restrict_pll (NOT implemented: PLL)
#define PG106_INIT_RESET_BEGUN        0x8cu // init_reset_begun
#define PG106_INIT_RESET_END          0x8du // init_reset_end
#define PG106_INIT_GPIO               0x8eu // init_gpio
#define PG106_INIT_RAM_RESTRICT_ZM_GROUP 0x8fu // init_ram_restrict_zm_reg_group
#define PG106_INIT_COPY_ZM_REG        0x90u // init_copy_zm_reg
#define PG106_INIT_ZM_REG_GROUP       0x91u // init_zm_reg_group
#define PG106_INIT_RESERVED           0x92u // init_reserved (1-byte reserved stub)
#define PG106_INIT_XLAT               0x96u // init_xlat
#define PG106_INIT_ZM_MASK_ADD        0x97u // init_zm_mask_add
#define PG106_INIT_AUXCH              0x98u // init_auxch (NOT implemented: AUX channel)
#define PG106_INIT_ZM_AUXCH           0x99u // init_zm_auxch (NOT implemented: AUX channel)
#define PG106_INIT_I2C_LONG_IF        0x9au // init_i2c_long_if (NOT implemented: I2C)
#define PG106_INIT_GPIO_NE            0xa9u // init_gpio_ne (NOT implemented: DCB gpio table)
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
	PG106_INIT_ERR_OPCODE   = -1,    // unknown or unimplemented opcode (named in error_msg)
	PG106_INIT_ERR_OVERRUN  = -2,    // operand/subtable read past end of BIOS image
	PG106_INIT_ERR_TRACE_FULL = -3,  // trace record buffer exhausted
	PG106_INIT_ERR_NO_TABLE = -4,    // required BIOS subtable pointer not supplied
	PG106_INIT_ERR_DEPTH    = -5,    // SUB/SUB_DIRECT nesting deeper than PG106_INIT_MAX_NESTING
	PG106_INIT_ERR_STEPS    = -6,    // step budget exceeded (runaway-loop guard)
	PG106_INIT_ERR_BADARG   = -7,    // NULL vtable/image
	PG106_INIT_ERR_RANGE    = -8,    // condition/macro index outside supplied table
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
// writes and simulated delays are also recorded so a whole run is
// reproducible byte-for-byte.
#define PG106_INIT_TRACE_MMIO 0u
#define PG106_INIT_TRACE_IO   1u
#define PG106_INIT_TRACE_TIME 2u

struct pg106_init_trace_rec {
	uint32_t address;       // MMIO address (MMIO) or IO port base (IO); 0 for TIME
	uint32_t value;         // value written; delay in microseconds for TIME records
	uint16_t index;         // indexed-IO index register (IO records; 0 otherwise)
	uint16_t script_offset; // BIOS offset of the opcode that produced the record
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

	// BIT 'I' init-data subtable offsets within image (init.c:399-406).
	// 0 = absent; an opcode needing an absent table fails loudly.
	uint16_t table_script;            // +0x00 script table (INIT_SUB 0x6b)
	uint16_t table_macro;             // +0x04 macro table (INIT_MACRO 0x6f)
	uint16_t table_condition;         // +0x06 condition table (0x75)
	uint16_t table_io_condition;      // +0x08 io condition table (0x76)
	uint16_t table_io_flag_condition; // +0x0a io flag condition table (0x39)
	uint16_t table_xlat;              // +0x10 xlat table (0x96)
	// init.c also defines init_macro_index_table (+0x02) but no master
	// handler reads it; not modelled.

	// Entry counts for the tables above (master reads them unbounded; a
	// simulator bounds-checks). Entries per table: condition 12 bytes
	// (init.c:484), io condition 5 bytes (init.c:500), io flag condition
	// 9 bytes (init.c:517), macro 8 bytes (init.c:1737).
	uint8_t condition_count;
	uint8_t io_condition_count;
	uint8_t io_flag_condition_count;
	uint8_t macro_count;

	// RAM restrict strap selection (init.c init_ram_restrict line 444 and
	// init_ram_restrict_group_count line 439): master derives both from
	// the ramcfg/strap tables; here the caller supplies them (deterministic
	// stand-in until the strap parser exists).
	uint8_t ram_restrict_groups; // nvbios_ramcfg_count equivalent
	uint8_t ramcfg_index;        // selected ramcfg group

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
	uint16_t error_offset; // script offset of the failing opcode
	uint8_t error_opcode;  // failing opcode byte
	uint32_t warnings;     // non-fatal deviations master only prints about

	// --- internal interpreter state (nvbios_init equivalents) ---
	uint16_t offset;    // program counter; 0 terminates (init.c:2307)
	uint8_t execute;    // nvkm execute-bit dance, init.c:58-82
	uint32_t nested;    // recursion depth
	uint16_t repeat;    // INIT_REPEAT body start (init.c:657-677)
	uint16_t repend;    // INIT_REPEAT body end
	uint32_t steps;     // opcodes executed this run
	uint16_t step_offset; // offset of opcode currently dispatched
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
int pg106_init_run(struct pg106_init_env *env, uint16_t script_offset);

// Human-readable status string ("OK", "unimplemented opcode", ...).
const char *pg106_init_strerr(int status);

// Master handler name for an opcode byte ("INIT_ZM_REG" for 0x7a), or
// "UNKNOWN" for bytes absent from master's dispatch table.
const char *pg106_init_opcode_name(uint8_t opcode);

// Nonzero when the opcode byte is implemented by this interpreter.
int pg106_init_opcode_implemented(uint8_t opcode);

#ifdef __cplusplus
}
#endif

#endif // PASCAL_DEVINIT_H
