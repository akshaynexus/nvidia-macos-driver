// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/gr_boot.h — Phase GR SIMULATOR: builds the
// ordered PGRAPH (GR engine) bring-up step list that nouveau's nvkm GR
// executes on GP10x AFTER the ACR layer has booted the FECS/GPCCS falcons,
// up to and including a minimal COMPUTE-capable GR state (class 0xC1C0).
// Driven by the real gp106/gr/{fecs,gpccs}_sig.bin blobs already handled by
// pascal/core/src/acr.c + acr_boot.c; this layer models what runs next.
//
// SIMULATION, NOT HARDWARE PROOF (HANDOFF-PASCAL.md gate wording): this
// module produces a MODEL of the GR bring-up sequence, replayed against a
// mock bus (pascal/tests/test_gr_boot.c) that merely records writes and
// returns canned readback values. Nothing here touches, claims, or proves
// anything about real hardware.
//
// Every step carries a `why` string naming its nouveau source origin
// (file:function + line, from the reference copies of linux master under
// /tmp/pascal-ref/upstream/, fetched 2026-11-05 from torvalds/master):
//   nvkm/engine/gr/{gf100.c,gm200.c,gp100.c,gp102.c,gp104.c}
//   nvkm/engine/gr/{ctxgf100.c,ctxgp102.c,ctxgp104.c}
//   nvkm/engine/gr/{gf117.c,gk104.c,gm107.c}   (init helpers GP10x inherits)
//   nvkm/engine/gr/gf100.h                      (GPC/TPC unit-address macros)
//   nvkm/falcon/{gm200.c,v1.c}                  (falcon enable/start, unload)
//
// Register offsets: master writes these as raw numbers through nvkm_wr32/
// nvkm_mask (no NV_PGRAPH_* macros in nvkm/engine/gr). The macros below
// therefore attach nouveau-style names to each offset and cite the master
// function+line that uses it. Addresses are baked into steps as
// cfg->gr_base + off (top.h does not exist yet, so — like acr_boot.h's
// falcon_base — the GR PRI base is a caller-supplied stand-in for the value
// nvkm_top discovers at runtime).
//
// FALCONLESS VERDICT (researched 2026-11-05): there is NO "falconless" GR
// mode in mainline nouveau for GP10x. gp102.c's fwif table offers exactly
// two entries: gm200_gr_load (signed fecs/gpccs sigs via ACR —
// nvkm/engine/gr/gp102.c:152-154) and gm200_gr_nofw, which just warns
// "firmware unavailable" and returns -ENODEV (gm200.c:34-39, gp102.c:153).
// The no-firmware internal-ucode path (gf100_gr_init_ctxctl_int,
// gf100.c:1831-1880) requires gr->func->fecs.ucode, which the gp102_gr func
// never sets, so it fails -ENOSYS at gf100.c:1838-1840. (Kepler/GM10x could
// run in-tree fuc3 microcode — gf100_gr_nofw, gf100.c:2562-2566 — but that
// generational path ends before gm200.) Laptops without signed GR firmware
// get no GR at all in mainline. We HAVE committed fecs/gpccs sigs, so this
// model follows the signed path: ACR boots FECS/GPCCS (previous layer), and
// gr_boot begins with the post-ACR GR init that polls 0x409800 bit 0 for
// FECS to confirm the boot (gf100_gr_init_ctxctl_ext, gf100.c:1793-1797).
//
// C11, libc only, single-threaded. Errors are errno-style (0 or -EINVAL).
// All symbols are prefixed pg106_grboot_ (no clashes with the rest of core).

#ifndef PASCAL_GR_BOOT_H
#define PASCAL_GR_BOOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// GR unit-address layout (cfg->gr_base-relative offsets). Absolute BAR0
// numbers in master; the gr_base indirection is this model's parameterization
// for the value nvkm_top discovers at runtime.
//
// Unit-address macros — nvkm/engine/gr/gf100.h:41-45:
//   ROP_UNIT(u,r) = 0x410000 + u*0x400 + r
//   GPC_BCAST(r)  = 0x418000 + r
//   GPC_UNIT(t,r) = 0x500000 + t*0x8000 + r
//   PPC_UNIT(t,m,r)= 0x503000 + t*0x8000 + m*0x200 + r
//   TPC_UNIT(t,m,r)= 0x504000 + t*0x8000 + m*0x800 + r
// ---------------------------------------------------------------------------

// PMC engine-enable register (BAR0 root, NOT gr_base-relative — master uses
// the raw 0x200; nvkm engine mask for GR is bit 12 = 0x1000, polled in
// gf100_gr_wait_idle, nvkm/engine/gr/gf100.c:1064, and driven by
// nvkm_mc_enable/Disable in the reset path gf100.c:2334,2338).
#define PG106_GRBOOT_PMC_ENABLE		 0x00000200ull
#define PG106_GRBOOT_PMC_GR		 0x00001000u

// GR interrupt / master control (gf100.c gf100_gr_init 2348,2387).
#define PG106_GRBOOT_OFF_400500		 0x000500ull
// GR exception/status registers cleared+enabled in gf100_gr_init (2389-2453).
#define PG106_GRBOOT_OFF_400100		 0x000100ull
#define PG106_GRBOOT_OFF_400108		 0x000108ull
#define PG106_GRBOOT_OFF_40011C		 0x00011cull
#define PG106_GRBOOT_OFF_400118		 0x000118ull
#define PG106_GRBOOT_OFF_400124		 0x000124ull
#define PG106_GRBOOT_OFF_400130		 0x000130ull
#define PG106_GRBOOT_OFF_400134		 0x000134ull
#define PG106_GRBOOT_OFF_400138		 0x000138ull
#define PG106_GRBOOT_OFF_40013C		 0x00013cull
// GR busy status polled by gf100_gr_wait_idle — gf100.c:1050-1076.
// (0x400700 is read first to refresh FIFO_ENGINE_STATUS, gf100.c:1062.)
#define PG106_GRBOOT_OFF_400700		 0x000700ull
#define PG106_GRBOOT_OFF_40060C		 0x00060cull
// FECS host interface (BAR0 abs 0x409xxx = fecs_base + off in master):
// ctrl/mailbox pair used by every gf100_gr_fecs_* helper (gf100.c:751-980).
#define PG106_GRBOOT_FECSC_OFF_409500	 0x000500ull // mailbox data in
#define PG106_GRBOOT_FECSC_OFF_409504	 0x000504ull // mailbox method
#define PG106_GRBOOT_FECSC_OFF_409800	 0x000800ull // mailbox ctrl/return
// FECS falcon interrupt-enable regs — gp100_gr_init_fecs_exceptions,
// nvkm/engine/gr/gp100.c:88-91 (wr32 0x409c24) and gf100.c:1571-1602 (ISR).
#define PG106_GRBOOT_FECSC_OFF_409C24	 0x000c24ull
// FECS reset — gf100_gr_fecs_reset, gf100.c:2467-2476.
#define PG106_GRBOOT_FECSC_OFF_409614	 0x000614ull
// GR PRI region offsets (gr_base-relative) touched by the init sequence.
#define PG106_GRBOOT_OFF_404000		 0x004000ull // DISPATCH trap clear
#define PG106_GRBOOT_OFF_404490		 0x004490ull // MACRO trap clear
#define PG106_GRBOOT_OFF_404600		 0x004600ull // M2MF trap clear
#define PG106_GRBOOT_OFF_405840		 0x005840ull // SHADER trap clear
#define PG106_GRBOOT_OFF_405844		 0x005844ull // SHADER hww esr mask
#define PG106_GRBOOT_OFF_405848		 0x005848ull // DS hww esr 2 — gm200.c:101
#define PG106_GRBOOT_OFF_40584C		 0x00584cull // — gm200.c:102
#define PG106_GRBOOT_OFF_406018		 0x006018ull // UNK6 trap clear
#define PG106_GRBOOT_OFF_407020		 0x007020ull // SKED hww esr — gk104.c:393
#define PG106_GRBOOT_OFF_408030		 0x008030ull // CCACHE trap clear
#define PG106_GRBOOT_OFF_408850		 0x008850ull // ZROP active fbps — gp100.c:99
#define PG106_GRBOOT_OFF_408958		 0x008958ull // CROP active fbps — gp100.c:100
#define PG106_GRBOOT_OFF_4181D0		 0x0181d0ull // SWDX PES mask — gp102.c:96
#define PG106_GRBOOT_OFF_418880		 0x018880ull // GPC MMU — gm200.c:118
#define PG106_GRBOOT_OFF_4188B0		 0x0188b0ull // — gm200.c:124
#define PG106_GRBOOT_OFF_4188B4		 0x0188b4ull // — gm200.c:122
#define PG106_GRBOOT_OFF_4188B8		 0x0188b8ull // — gm200.c:123
#define PG106_GRBOOT_OFF_418890		 0x018890ull // — gm200.c:119
#define PG106_GRBOOT_OFF_418894		 0x018894ull // — gm200.c:120
#define PG106_GRBOOT_OFF_419CC0		 0x019cc0ull // L1C — gf100.c:2243
#define PG106_GRBOOT_OFF_418010		 0x018010ull // ZBC color ds[0] — gp100.c:40
#define PG106_GRBOOT_OFF_41804C		 0x01804cull // ZBC color ds[1] — gp100.c:41
#define PG106_GRBOOT_OFF_418088		 0x018088ull // ZBC color ds[2] — gp100.c:42
#define PG106_GRBOOT_OFF_4180C4		 0x0180c4ull // ZBC color ds[3] — gp100.c:43
#define PG106_GRBOOT_OFF_418100		 0x018100ull // ZBC color format — gp100.c:46
#define PG106_GRBOOT_OFF_418110		 0x018110ull // ZBC depth ds — gp100.c:59
#define PG106_GRBOOT_OFF_41814C		 0x01814cull // ZBC depth format — gp100.c:60
#define PG106_GRBOOT_OFF_41815C		 0x01815cull // ZBC stencil ds — gp102.c:37
#define PG106_GRBOOT_OFF_418198		 0x018198ull // ZBC stencil format — gp102.c:38
// LTC ZBC table (absolute BAR0, not gr_base-relative):
// gm107_ltc_zbc_clear_color/_depth (nvkm/subdev/ltc/gm107.c:53-69) and
// gp102_ltc_zbc_clear_stencil (nvkm/subdev/ltc/gp102.c:26-32), wired for
// GP106 by gp102_ltc (ltc/gp102.c:35-46). 0x17e338 is the index register
// (nvkm_mask(.., 0xf, i)); the rest are data.
#define PG106_GRBOOT_LTC_ZBC_INDEX	 0x0017e338ull
#define PG106_GRBOOT_LTC_ZBC_COLOR0	 0x0017e33cull // .. 0x17e348
#define PG106_GRBOOT_LTC_ZBC_DEPTH	 0x0017e34cull
#define PG106_GRBOOT_LTC_ZBC_STENCIL	 0x0017e204ull
// ZBC table bounds: nvkm_ltc_new_ sets zbc_*_min = 1 ("reserve 0 for
// disabled") and max = min(func->zbc_*, NVKM_LTC_MAX_ZBC_*_CNT) - 1
// (nvkm/subdev/ltc/base.c:142-145); gp102_ltc .zbc_color = .zbc_depth = 16
// (ltc/gp102.c:41-42) -> indices 1..15.
#define PG106_GRBOOT_ZBC_MIN		 1u
#define PG106_GRBOOT_ZBC_MAX		 15u
// PMC 0x260 — nvkm_mc_unk260 -> gf100_mc_unk260 = nvkm_wr32(0x260, data)
// (nvkm/subdev/mc/gf100.c:61-65; gp100_mc wires it). Absolute BAR0.
#define PG106_GRBOOT_PMC_UNK260		 0x00000260ull

// Unit-region bases (gr_base-relative), from gf100.h:41-45.
#define PG106_GRBOOT_ROP_BASE		 0x010000ull
#define PG106_GRBOOT_GPC_BCAST_BASE	 0x018000ull
#define PG106_GRBOOT_PPC_BASE		 0x103000ull
#define PG106_GRBOOT_GPC_BASE		 0x100000ull
#define PG106_GRBOOT_TPC_BASE		 0x104000ull
#define PG106_GRBOOT_GPC_STRIDE	 0x008000ull
#define PG106_GRBOOT_TPC_STRIDE	 0x000800ull
#define PG106_GRBOOT_PPC_STRIDE	 0x000200ull
#define PG106_GRBOOT_ROP_STRIDE	 0x000400ull

// Discovery read addresses (BAR0 abs in master; gr_base-relative only where
// they live inside the GR PRI — 0x409604 is FECS-falcon-relative).
// GPC/ROP count report — gf100_gr_oneinit gf100.c:2010 (gpc_nr) and
// gf100_gr_rops gf100.c:999-1003 (rops, [23:16]); gm200_gr_rops gm200.c:92-95.
#define PG106_GRBOOT_FECSC_OFF_409604	 0x000604ull
// FBPA/L2 config in the FB (PFB) region — outside gr_base, absolute BAR0:
#define PG106_GRBOOT_PFB_12006C		 0x0012006cull // fbp count — gp100.c:98
#define PG106_GRBOOT_PFB_100800		 0x00100800ull // active LTCs — gm200.c:109
#define PG106_GRBOOT_PFB_100804		 0x00100804ull // — gm200.c:110
#define PG106_GRBOOT_PFB_100C80		 0x00100c80ull // GPC MMU rsvd — gm200.c:118
#define PG106_GRBOOT_PFB_100CC4		 0x00100cc4ull // mmu_rd/wr — gm200.c:124
#define PG106_GRBOOT_PFB_100CC8		 0x00100cc8ull // — gm200.c:122
#define PG106_GRBOOT_PFB_100CCC		 0x00100cccull // — gm200.c:123
// Per-GPC discovery (GPC_UNIT offsets):
#define PG106_GRBOOT_GPC_OFF_2608	 0x002608ull // TPC count — gf100.c:2012
#define PG106_GRBOOT_GPC_OFF_0C30	 0x000c30ull // PPC TPC masks — gf100.c:2017
#define PG106_GRBOOT_GPC_OFF_0C50	 0x000c50ull // SWDX PES — gp102.c:92

// Falcon registers (falcon_base-relative), duplicated from acr_boot.h's
// citations — the GR falcons use the same v1 falcon block:
// FECS falcon base 0x409000, GPCCS 0x41a000 — gf100_gr_new_,
// nvkm/engine/gr/gf100.c:2641-2643,2648-2650.
// CPUCTL start — nvkm_falcon_v1_start, nvkm/falcon/v1.c:93-101 (wr32 0x100=2).
#define PG106_GRBOOT_FALCON_CPUCTL	 0x00000100u
// Disable path — gm200_flcn_disable, nvkm/falcon/gm200.c:184-215:
// mask 0x048 bits 1:0 -> 0 (195) and wr32 0x014 = 0xffffffff (196).
#define PG106_GRBOOT_FALCON_RM		 0x00000048u
#define PG106_GRBOOT_FALCON_INTR_EN	 0x00000014u
// NOTE: the GR falcon func (gf100_gr_flcn, gf100.c:2162-2167) has NO
// .reset_eng — GP10x's 0x3c0 engine reset (falcon/gp102.c:77-79) does NOT
// apply to FECS/GPCCS; the unload path below deliberately omits it.

// ---------------------------------------------------------------------------
// Compute class binding + context-buffer geometry constants (ctxgp102).
// ---------------------------------------------------------------------------

// PASCAL_COMPUTE_B — the compute class GP102/GP104/GP106 GR exposes,
// gp102.c sclass list nvkm/engine/gr/gp102.c:130-135 (also nvkm/engine/gr/
// gp104.c:85-90, same struct for GP106 per MODULE_FIRMWARE gp104.c:78-89).
// Matches pascal/pushbuf.h PASCAL_COMPUTE_B 0xC1C0 / pascal/dispatch.h.
#define PG106_GRBOOT_COMPUTE_CLASS	 0x0000c1c0u

// gf100_grctx_func constants for GP102/GP106 — nvkm/engine/gr/ctxgp102.c:98-122
// (gp102_grctx; GP106 uses gp104_grctx with the identical numbers —
// ctxgp104.c:37-60):
#define PG106_GRCTX_BUNDLE_SIZE		 0x00003000u // ctxgp102.c:103
#define PG106_GRCTX_BUNDLE_MIN_GPM_FIFO 0x00000180u // ctxgp102.c:104
#define PG106_GRCTX_BUNDLE_TOKEN_LIMIT	 0x00000900u // ctxgp102.c:105
#define PG106_GRCTX_PAGEPOOL_SIZE	 0x00020000u // ctxgp102.c:107
#define PG106_GRCTX_ATTRIB_NR_MAX	 0x000004b0u // ctxgp102.c:111
#define PG106_GRCTX_ATTRIB_NR		 0x00000320u // ctxgp102.c:112
#define PG106_GRCTX_ALPHA_NR_MAX	 0x00000c00u // ctxgp102.c:113
#define PG106_GRCTX_ALPHA_NR		 0x00000800u // ctxgp102.c:114
#define PG106_GRCTX_GFXP_NR		 0x00000ba8u // ctxgp102.c:115

// GR unit limits — nvkm/engine/gr/gf100.h:36-38,107,112 (GPC_MAX 32,
// TPC_MAX_PER_GPC 8, ppc_tpc_mask[GPC_MAX][4]; gf100_gr_func .gpc_nr=6 cap,
// gp102.c:124).
#define PG106_GRBOOT_GPC_MAX		 32u
#define PG106_GRBOOT_TPC_MAX_PER_GPC	 8u
#define PG106_GRBOOT_PPC_MAX		 4u

// ---------------------------------------------------------------------------
// Step model
// ---------------------------------------------------------------------------

// Address space a step acts on. GR steps are BAR0 MMIO; the SW_NONCTX
// register-pack step is modeled as an opaque blob (its contents come from
// the signed sw_nonctx.bin firmware, not from nouveau source).
enum pg106_grboot_space {
	PG106_GRBOOT_MMIO = 0,
	PG106_GRBOOT_FW   = 1, // firmware-provided byte stream (sw_nonctx pack)
};

enum pg106_grboot_kind {
	// Single 32-bit register write at addr (value & mask composed by the
	// model; `value` is the final word written).
	PG106_GRBOOT_WRITE_REG = 0,
	// Opaque firmware register-pack (sw_nonctx.bin applied through
	// gf100_gr_mmio, gf100.c:2352-2362): blob==NULL, blob_len = pack size.
	PG106_GRBOOT_WRITE_BLOB = 1,
	// Spin until (rd32(addr) & mask) satisfies `op` vs value, for at most
	// timeout_us (the nvkm_msec budget); replayers must re-read. The
	// FECS size queries are ANY-polls whose satisfying read IS the
	// result (gf100.c:932-935: `if ((*psize = nvkm_rd32(..)))`).
	PG106_GRBOOT_POLL_REG = 2,
	// Register read-back with expected (value & mask) — used for the
	// discovery reads and the FECS mailbox size queries.
	PG106_GRBOOT_READ_REG = 3,
	// nvkm_mask() (include/nvkm/core/device.h:131-136): old = rd32(addr);
	// wr32(addr, (old & ~mask) | value).
	PG106_GRBOOT_MASK_REG = 4,
	// nvkm_falcon_v1_start (falcon/v1.c:93-101), decided at run time:
	// cpuctl = rd32(addr); wr32((cpuctl & BIT(6)) ? addr + 0x30 : addr,
	// value). addr = falcon CPUCTL (0x100), value = 2. FECS/GPCCS are LS
	// falcons once SEC2 bootstrapped them, and ALIAS_EN is their state at
	// that point (on hardware SEC2 had it set after the ACR).
	PG106_GRBOOT_FALCON_START = 5,
};

// Poll comparison: ANY = stop when (rd32 & mask) != 0; EQ = stop when
// (rd32 & mask) == value.
enum pg106_grboot_poll_op {
	PG106_GRBOOT_POLL_ANY = 0,
	PG106_GRBOOT_POLL_EQ  = 1,
};

// Step groups, in mandatory bring-up order (golden ordering below).
enum pg106_grboot_group {
	PG106_GRBOOT_G_RESET = 0,     // engine reset / 0x400500 quiesce
	PG106_GRBOOT_G_DISCOVERY = 1, // fuse/strap/GPC/TPC/PPC/LTC reads
	PG106_GRBOOT_G_GPC_MMU = 2,   // init_gpc_mmu (gm200)
	PG106_GRBOOT_G_MMIO_PACK = 3, // sw_nonctx register pack
	PG106_GRBOOT_G_ENGINE_CFG = 4,// wait_idle + zcull/ltc/fbp/swdx config
	PG106_GRBOOT_G_EXCEPTIONS = 5,// GR exception register init
	PG106_GRBOOT_G_ZBC = 6,       // zero-bandwidth-clear table init
	PG106_GRBOOT_G_FECS_BOOT = 7, // falcon start + boot poll + ctx queries
	// gf100_gr_fini (gf100.c:2115-2126) only blocks the GR interrupt and
	// nvkm_falcon_put()s both falcons — falcon_put (nvkm/falcon/
	// base.c:285-297) just drops the user pointer. No register is
	// written, so this group is EMPTY; it stays in the enum so traces
	// can name it.
	PG106_GRBOOT_G_UNLOAD = 8,
	PG106_GRBOOT_G_COUNT
};

// One modeled bus transaction with its nouveau provenance.
struct pg106_grboot_step {
	// Transaction kind / address space / poll op (enums above).
	uint8_t kind;
	uint8_t space;
	uint8_t poll_op; // PG106_GRBOOT_POLL_* when kind == POLL_REG
	// [HW-VALUE] flag: 1 when the value (or read-back) comes from
	// fuses/straps/firmware state that cannot be simulated — the mock
	// supplies a stand-in and real hardware must provide the true value.
	uint8_t hw_value;
	// PG106_GRBOOT_G_* group (drives the golden-ordering assertions).
	uint16_t group;
	uint16_t reserved_;
	// Address (absolute BAR0 for MMIO; 0 for FW steps).
	uint64_t addr;
	// WRITE_REG: the 32-bit value. POLL_REG: `value` for POLL_EQ.
	// READ_REG: expected (value & mask). MASK_REG: bits to set.
	uint32_t value;
	// POLL_REG/READ_REG: mask applied to the read value. MASK_REG: bits
	// cleared before `value` is OR'd in.
	uint32_t mask;
	// WRITE_BLOB source bytes (NULL = firmware-provided, not simulated).
	const uint8_t *blob;
	uint32_t blob_len;
	// POLL_REG budget in microseconds (nvkm_msec / jiffies timeout).
	uint32_t timeout_us;
	// Nouveau provenance: "file:line function — note". Never NULL/empty
	// for steps produced by pg106_grboot_plan_build.
	const char *why;
};

// Caller-supplied simulated hardware/firmware geometry. Real GP106 values
// come from fuses/straps and the FECS firmware at runtime; the test supplies
// plausible stand-ins (a 1-GPC/5-TPC GT 1060-class config).
struct pg106_grboot_config {
	// Simulated GR PRI base and falcon bases (real values come from
	// nvkm_top at runtime; master hardcodes 0x400000 / 0x409000 /
	// 0x41a000 — gf100.c:2641-2650).
	uint64_t gr_base;
	uint32_t fecs_base;
	uint32_t gpccs_base;
	// GP107/GP108-only reset workaround (gf100.c:2070,2088-2095): pulse
	// PMC bit 12 off/on before init. GP106 (chipset 0x136) leaves this
	// false. When set, the plan emits the two PMC writes first.
	bool reset_war;
	// Current PMC enable value the model composes the pulse from
	// (only used when reset_war is set; must contain the GR bit).
	uint32_t pmc_enable;
	// HW discovery stand-ins ([HW-VALUE] — fuses/straps on real silicon):
	uint32_t gpc_nr;             // 0x409604 [4:0] — gf100.c:2010
	uint8_t tpc_nr[32];          // GPC_UNIT(g,0x2608) — gf100.c:2012
	uint8_t ppc_tpc_mask[32][4]; // GPC_UNIT(g,0x0c30+4p) — gf100.c:2017
	// rop_nr = gm200_gr_rops = the whole 0x12006c word (gm200.c:91-95;
	// gp104_gr .rops = gm200_gr_rops, gp104.c — GP106 uses gp104_gr,
	// nvkm/engine/device/base.c nv136_chipset). NOT 0x409604[23:16]
	// (that is gf100_gr_rops, gf100.c:999-1003, which GP10x does not use).
	uint32_t rop_config;         // 0x12006c full word
	// fbp_count = 0x12006c & 0xf (gp100.c:98) — the SAME register as
	// rop_config, so it must equal rop_config & 0xf (validated).
	uint32_t fbp_count;
	uint32_t ltc_active;         // 0x100800 — gm200.c:109
	uint32_t ltc_magma;          // 0x100804 — gm200.c:110
	uint32_t pri_mmu_rsvd;       // 0x100c80 — gm200.c:118
	uint32_t mmu_wr;             // 0x100cc4 — gm200.c:124
	uint32_t mmu_wr_rd;          // 0x100cc8 — gm200.c:122
	uint32_t mmu_rd;             // 0x100ccc — gm200.c:123
	uint8_t swdx_pes[32];        // GPC_UNIT(g,0x0c50)[3:0] — gp102.c:92
	// FECS mailbox query results ([HW-VALUE] — reported by the booted FECS
	// firmware, not by fuses; the mock stands in):
	uint32_t ctx_size;    // mthd 0x10 golden-context image — gf100.c:956-970
	uint32_t size_zcull;  // mthd 0x16 ZCULL image — gf100.c:940-954
	uint32_t size_pm;     // mthd 0x25 PerfMon image — gf100.c:924-938
	// sw_nonctx.bin register-pack size in bytes (contents are firmware
	// data — modeled as an opaque zero-fill blob; gf100.c:2352-2362).
	uint32_t sw_nonctx_size;
	// Model the ZBC default-table init (gf100.c:2458 -> 1005-1042) in
	// full: 4 color + 2 depth + 3 stencil defaults (each programming the
	// LTC table then the GR table), then clear_* of the remaining
	// indices up to PG106_GRBOOT_ZBC_MAX.
	bool zbc_init;
};

// Derived discovery + context-buffer geometry, mirroring
// gf100_gr_oneinit (gf100.c:1989-2062) and the gp102 grctx constants.
struct pg106_grboot_info {
	// Discovered totals (recomputed from the config stand-ins exactly
	// like gf100_gr_oneinit does — gf100.c:2009-2033).
	uint32_t tpc_total;
	uint32_t ppc_total;
	uint8_t ppc_tpc_max;
	uint8_t ppc_tpc_min;
	uint32_t rop_nr;
	// ZCULL magic — DIV_ROUND_UP(0x00800000, tpc_total) (gf117.c:127).
	uint32_t magicgpc918;
	// Context buffer sizes the GR layer allocates (gf100.c:2035-2049):
	uint32_t pagepool_size; // PG106_GRCTX_PAGEPOOL_SIZE
	uint32_t bundle_size;   // PG106_GRCTX_BUNDLE_SIZE
	// attrib_cb = gp102_grctx_generate_attrib_cb_size (ctxgp102.c:86-95):
	//   ((alpha_nr_max * tpc_total
	//     + gfxp_nr * ppc_nr * ppc_tpc_max) * 0x20 + 127) & ~127
	uint64_t attrib_cb_size;
	// Golden-context image size discovered from FECS (cfg->ctx_size
	// echoed; consumed by gf100_gr_chan_bind, gf100.c:327-334).
	uint32_t ctx_size;
	// Step index the SEC2 BOOTSTRAP_FALCON(FECS, GPCCS) exchange
	// (pg106_acr_bootstrap_falcons, acr_boot.h) must run before: the
	// nvkm_mc_unk260(1) write that follows nvkm_acr_bootstrap_falcons in
	// gf100_gr_init_ctxctl_ext (gf100.c:1777-1783).
	size_t bootstrap_before;
	// Compute class binding available after this bring-up.
	uint32_t compute_class; // 0xC1C0 PASCAL_COMPUTE_B (gp102.c:130-135)
	// Number of steps in the plan.
	size_t nsteps;
};

// Built plan: owns its step array.
struct pg106_grboot_plan {
	struct pg106_grboot_step *steps;
	size_t nsteps;
	// PRIVATE — scratch allocations the steps may point into. Currently
	// unused (all `why` strings are literals; the sw_nonctx pack is a
	// zero-fill) but kept for symmetry with pg106_acrboot_plan.
	void **scratch_;
	size_t nscratch_;
};

// Build the ordered step list for GP106 GR bring-up after the ACR layer has
// booted FECS/GPCCS (see the group enum for the order; each step's `why`
// cites its source). The discovery group reads the fuse/strap stand-ins from
// the config and later groups derive their written values from them —
// exactly the data flow gf100_gr_oneinit -> gf100_gr_init has in master.
//
// Returns 0, or -EINVAL on bad arguments/inconsistent inputs (gpc_nr out of
// range, TPC count > 8 per GPC, a PPC mask above 8 bits, a sw_nonctx size
// that is not a multiple of 4, reset_war without the PMC GR bit set,
// fbp_count != rop_config & 0xf).
int pg106_grboot_plan_build(const struct pg106_grboot_config *cfg,
			    struct pg106_grboot_plan *plan,
			    struct pg106_grboot_info *info);

// Release the plan's step array and scratch buffers.
void pg106_grboot_plan_free(struct pg106_grboot_plan *plan);

// Human-readable names for the enums above (for trace dumps). Never NULL.
const char *pg106_grboot_kind_name(uint8_t kind);
const char *pg106_grboot_space_name(uint8_t space);
const char *pg106_grboot_poll_op_name(uint8_t op);
const char *pg106_grboot_group_name(uint8_t group);

#ifdef __cplusplus
}
#endif

#endif // PASCAL_GR_BOOT_H
