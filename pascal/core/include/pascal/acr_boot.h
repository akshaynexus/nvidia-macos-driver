// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/acr_boot.h — Phase 2 ACR secure-boot SIMULATOR:
// builds the ordered falcon-loading step list (reset, IMEM/DMEM PIO writes,
// mailbox handshake, WPR shadow setup) that nouveau's nvkm ACR executes when
// booting the HS "load" firmware on GP10x, driven by the real firmware blobs
// parsed by pascal/core/src/acr.c.
//
// SIMULATION, NOT HARDWARE PROOF (HANDOFF-PASCAL.md Phase 2 gate wording):
// this module produces a MODEL of the boot sequence. The tests replay the
// step list (and run the SEC2 queue code below) against a mock falcon with
// nouveau's register semantics (pascal/tests/test_acr_boot.c); nothing here
// has run on, or proves anything about, real hardware.
//
// Every step carries a `why` string naming its nouveau source origin
// (file:line function). Line numbers are torvalds/linux master at commit
// d48723711bae9b8cdc56719c448ba42fef28cbee (2026-11-06), paths relative to
// drivers/gpu/drm/nouveau/. Key upstream sources:
//   nvkm/falcon/{base.c,gm200.c,gp102.c,v1.c,fw.c,cmdq.c,msgq.c,qmgr.c}
//   nvkm/subdev/acr/{base.c,gm200.c,gp102.c,lsfw.c}
//   nvkm/engine/sec2/{base.c,gp102.c}, include/nvfw/{sec2.h,flcn.h}
// Register offsets: current master touches these offsets through
// nvkm_falcon_wr32()/nvkm_falcon_mask() with *raw numbers* — there are no
// NV_FALCON_* macros in master's include/nvkm/engine/falcon.h or anywhere in
// nvkm. The macros below therefore attach nouveau-style names to each offset
// and cite the master function(s)+lines that use it.
//
// C11, libc only, single-threaded. Errors are errno-style (0 or -EINVAL).
// All symbols are prefixed pg106_acrboot_ (no clashes with the rest of core).

#ifndef PASCAL_ACR_BOOT_H
#define PASCAL_ACR_BOOT_H

#include <pascal/acr.h>
#include <pascal/pmc.h>
#include <pascal/top.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Falcon register offsets (falcon-relative; MMIO addr = falcon_base + off).
// Master has no NV_FALCON_* macros: each macro cites the raw-offset uses it
// mirrors (nvkm/falcon/*.c of linux master, /tmp/pascal-ref/upstream/).
// ---------------------------------------------------------------------------

// Falcon interrupt acknowledge / IRQSCLR — nvkm/falcon/gm200.c:243
// (gm200_flcn_fw_boot irqsclr), nvkm/engine/falcon.c:76,83,89 (intr ack).
#define PG106_FALCON_INTR		 0x004u
// Falcon interrupt status — nvkm/falcon/gm200.c:125 (gm200_flcn_bind_stat).
#define PG106_FALCON_INTR_STAT		 0x008u
// "disable all interrupts" — nvkm/engine/falcon.c:113,187; gm200.c:196.
#define PG106_FALCON_INTR_EN		 0x014u
// Host mailbox 0 (command/return code) — nvkm/falcon/gm200.c:224,237
// (gm200_flcn_fw_boot write + read-back).
#define PG106_FALCON_MBOX0		 0x040u
// Host mailbox 1 — nvkm/falcon/gm200.c:225-226,238.
#define PG106_FALCON_MBOX1		 0x044u
// Memory-binding / app enable — nvkm/falcon/gm200.c:195 (disable, clear
// 0x3) and 257 (fw_load, set bit 0 before binding the instance).
#define PG106_FALCON_RM			 0x048u
// Instance-block bind address — nvkm/falcon/gm200.c:135
// (gm200_flcn_bind_inst: (1<<30)|(target<<28)|(addr>>12)).
#define PG106_FALCON_BINDINST		 0x054u
// Memory bind control — nvkm/falcon/gm200.c:136 (bind_inst, set bit 16).
#define PG106_FALCON_MEMBIND_CTL	 0x090u
// Memory bind control (set bit 3) — nvkm/falcon/gm200.c:137 (bind_inst).
#define PG106_FALCON_MEMBIND_CTL2	 0x0a4u
// Bind completion / error status — nvkm/falcon/gm200.c:128
// (gm200_flcn_bind_stat: (rd32 & 0x7000) >> 12).
#define PG106_FALCON_BINDSTAT		 0x0dcu
// Bind/unbind clear — nvkm/falcon/gm200.c:276 (fw_load, set bit 3 in INTR).
#define PG106_FALCON_BIND_INTR_CLR	 PG106_FALCON_INTR
// Post-bind trigger — nvkm/falcon/gm200.c:277 (fw_load, set bit 1).
#define PG106_FALCON_BIND_TRIGGER	 0x058u
// Written with nvkm_rd32(device, 0x000000) — PMC_BOOT_0, the chip id — by
// gm200_flcn_enable (nvkm/falcon/gm200.c:179). Not a PMC-enable mirror.
#define PG106_FALCON_ENGINE_EN		 0x084u
// CPU control: START (0x2) / halted (bit 4) / boot-vec-from-IMEM bit 6 —
// nvkm/falcon/v1.c:95-100 (nvkm_falcon_v1_start), gm200.c:229,232
// (gm200_flcn_fw_boot start + halted poll).
#define PG106_FALCON_CPUCTL		 0x100u
// Boot vector (entry IMEM address) — nvkm/falcon/gm200.c:228
// (gm200_flcn_fw_boot: nvkm_falcon_wr32(0x104, fw->boot_addr)).
#define PG106_FALCON_BOOTVEC		 0x104u
// IMEM/DMEM limits caps (code.limit=(rd&0x1ff)<<8, data.limit=(rd&0x3fe00)>>1)
// — nvkm/falcon/base.c:273-275 (nvkm_falcon_oneinit).
#define PG106_FALCON_MEM_CAPS		 0x108u
// Memory-scrubbing status (bits 1:2 = IMEM/DMEM scrub BUSY) —
// gm200_flcn_reset_wait_mem_scrubbing waits for the bits to CLEAR:
// `if (!(nvkm_falcon_rd32(falcon, 0x10c) & 0x00000006)) break;`
// (nvkm/falcon/gm200.c:145-147, 10 ms budget). :286 (no-inst fw_load) also
// writes it to 0.
#define PG106_FALCON_MEM_SCRUB		 0x10cu
// Falcon version/secret/ports caps — nvkm/falcon/base.c:267-270 (oneinit).
#define PG106_FALCON_VERSION		 0x12cu
// Alternate start register for bit-6 (secret) falcons — nvkm/falcon/v1.c:98.
#define PG106_FALCON_CPUCTL_ALIAS	 0x130u
// IMEM PIO port 0: control/data/tag (stride 0x10 per port) —
// nvkm/falcon/gm200.c:100 (gm200_flcn_pio_imem_wr_init: BIT(24)|base, BIT(28)
// if secure), :104-112 (gm200_flcn_pio_imem_wr: tag, then u32 data words).
#define PG106_FALCON_IMEMC		 0x180u
#define PG106_FALCON_IMEMD		 0x184u
#define PG106_FALCON_IMEMT		 0x188u
// DMEM PIO port 0: control/data (stride 8 per port) — nvkm/falcon/gm200.c:84
// (gm200_flcn_pio_dmem_wr_init: BIT(24)|base), :70-79 (wr: u32 data words).
#define PG106_FALCON_DMEMC		 0x1c0u
#define PG106_FALCON_DMEMD		 0x1c4u
// SEC2 falcon debug register: bit 20 set = debug-fused part, selects the
// debug HS signature (gm200_flcn_fw_signature, nvkm/falcon/gm200.c:335;
// falcon->debug at oneinit, nvkm/falcon/base.c:277-280; gp102_sec2_flcn
// .debug = 0x408, nvkm/engine/sec2/gp102.c:205).
#define PG106_FALCON_SEC2_DEBUG		 0x408u
#define PG106_FALCON_SEC2_DEBUG_DBG	 0x00100000u
// Interrupt destination (read by gp102_sec2_intr, sec2/gp102.c:158).
#define PG106_FALCON_IRQDEST		 0x01cu
// SEC2 queue head/tail registers: gp102_sec2_flcn .cmdq = {0xa00, 0xa04,
// 8}, .msgq = {0xa30, 0xa34, 8} (sec2/gp102.c:214-215).
#define PG106_SEC2_CMDQ_HEAD		 0xa00u
#define PG106_SEC2_CMDQ_TAIL		 0xa04u
#define PG106_SEC2_MSGQ_HEAD		 0xa30u
#define PG106_SEC2_MSGQ_TAIL		 0xa34u
#define PG106_SEC2_Q_STRIDE		 8u
// EMEM PIO port 0 control/data — gp102_flcn_emem_pio (nvkm/falcon/
// gp102.c:34-54: 0xac0 = BIT(24)|base to write, BIT(25)|base to read).
#define PG106_FALCON_EMEMC		 0xac0u
#define PG106_FALCON_EMEMD		 0xac4u
// GP10x falcon engine reset — nvkm/falcon/gp102.c:77-79
// (gp102_flcn_reset_eng: set bit 0, 10us delay, clear bit 0).
#define PG106_FALCON_GP102_RESET	 0x3c0u
// DMA context select (DMAIDX_VIRT = 0) — nvkm/falcon/gm200.c:134
// (gm200_flcn_bind_inst).
#define PG106_FALCON_DMAIDX		 0x604u
// DMA transfer control — nvkm/falcon/gm200.c:285 (fw_load no-inst branch,
// set bit 7). Not exercised by the inst-bound ACR path modeled here.
#define PG106_FALCON_DMACTL		 0x624u

// PFB WPR-check window register (full BAR0 address, not falcon-relative)
// — nvkm/subdev/acr/gm200.c:57-61 (gm200_acr_wpr_check: write selector
// 2/3, read window in [31:8]).
#define PG106_PFB_WPR_CHECK		 0x100cd4ull

// PIO transfer granularity: gm200_flcn_imem_pio.min/.max = 0x100 and
// gm200_flcn_dmem_pio.max = 0x100 (nvkm/falcon/gm200.c:89-90,114-120).
#define PG106_FALCON_PIO_CHUNK		 0x100u

// PMC registers the reset path touches (BAR0 absolute): PMC_BOOT_0 is read
// for the 0x084 write (gm200.c:179); PMC_ENABLE is RMW'd by
// nvkm_mc_disable/enable -> nv04_mc_device_disable/enable
// (nvkm/subdev/mc/nv04.c:34-46; gp100_mc .device = &nv04_mc_device,
// nvkm/subdev/mc/gp100.c:96).
// (PG106_PMC_BOOT_0 / PG106_PMC_ENABLE live in pascal/pmc.h.)

// SEC2 falcon PRI base on GP106. nouveau resolves it from the nvkm_top table
// (nvkm/falcon/base.c:261-264: nvkm_top_addr(SEC2, 0)); the value MEASURED
// on a real GP106 is 0x087000 ("SEC2 @ 0x87000: SCTL 0x3000 (LS+L3), IMEM
// 64KB visible", research/prior-art/pascal-egpu/FINDINGS.md:160, submodule
// commit 6dfeb2f8). Use it only as the fallback for
// pg106_acrboot_falcon_base(); the earlier 0x3c0000 stand-in had no source
// and is gone.
#define PG106_ACRBOOT_SEC2_BASE_GP106	 0x00087000u

// Decode of FALCON_HWCFG (0x108), nvkm_falcon_oneinit
// (nvkm/falcon/base.c:273-275): code.limit = (reg & 0x1ff) << 8,
// data.limit = (reg & 0x3fe00) >> 1. Cross-check against real GP106
// captures (FINDINGS.md:60,66): FECS 0x20202060 -> 24 KB IMEM / 4 KB
// DMEM, PMU 0x400e0100 -> 64 KB / 64 KB; the labels in the capture agree.
#define PG106_FALCON_HWCFG_CODE_LIMIT(r) (((r) & 0x1ffu) << 8)
#define PG106_FALCON_HWCFG_DATA_LIMIT(r) (((r) & 0x3fe00u) >> 1)

// ---------------------------------------------------------------------------
// Step model
// ---------------------------------------------------------------------------

// Address space a step acts on. MMIO = BAR0 registers; WPR = the
// shadow-buffer object built by gp102_acr_wpr_build (offsets are
// object-relative); IMG = the HS ucode image buffer that gets DMA-mapped
// (offsets are image-relative — the host patches it before load).
enum pg106_acrboot_space {
	PG106_ACRBOOT_MMIO = 0,
	PG106_ACRBOOT_WPR  = 1,
	PG106_ACRBOOT_IMG  = 2,
};

enum pg106_acrboot_kind {
	// Single register write: addr = falcon_base + off (MMIO) or
	// object-relative (WPR/IMG); value written as one 32-bit word.
	PG106_ACRBOOT_WRITE_REG = 0,
	// Streaming write of blob_len bytes starting at addr: expands the
	// nvkm_wobj() data phase. blob may be NULL (= zero fill; used where
	// the caller supplied no LS image bytes).
	PG106_ACRBOOT_WRITE_BLOB = 1,
	// Spin until (rd32(addr) & mask) satisfies `poll_op` vs value, for at
	// most timeout_us (the nvkm_msec budget). Replayers must RE-READ until
	// the condition holds or the budget is spent.
	PG106_ACRBOOT_POLL_REG = 2,
	// Register read-back with expected (value & mask): mailbox results,
	// WPR window check. mask 0 = read for the record, nothing expected.
	PG106_ACRBOOT_READ_REG = 3,
	// nvkm_mask()/nvkm_falcon_mask() read-modify-write (include/nvkm/core/
	// device.h:131-136): old = rd32(addr); wr32(addr, (old & ~mask) |
	// value). Always writes, even when the value is unchanged.
	PG106_ACRBOOT_MASK_REG = 4,
	// udelay(value) — e.g. gp102_flcn_reset_eng's 10 us (gp102.c:78).
	PG106_ACRBOOT_DELAY_US = 5,
};

// Poll comparison: ANY = stop when (rd32 & mask) != 0; EQ = stop when
// (rd32 & mask) == value (EQ with value 0 = "wait for bits to clear").
enum pg106_acrboot_poll_op {
	PG106_ACRBOOT_POLL_ANY = 0,
	PG106_ACRBOOT_POLL_EQ  = 1,
};

// One modeled bus/memory transaction with its nouveau provenance.
struct pg106_acrboot_step {
	// Transaction kind / address space (enums above).
	uint8_t kind;
	uint8_t space;
	// Poll comparison op (PG106_ACRBOOT_POLL_* when kind == POLL_REG).
	uint8_t poll_op;
	uint8_t reserved_;
	// Address: falcon_base + offset for falcon MMIO steps (the plan
	// bakes falcon_base in), absolute BAR0 for PMC/PFB steps,
	// object-relative offset for WPR/IMG steps.
	uint64_t addr;
	// WRITE_REG: the 32-bit value. POLL_REG: `value` for POLL_EQ.
	// READ_REG: expected (value & mask). MASK_REG: bits to set.
	// DELAY_US: microseconds.
	uint32_t value;
	// POLL_REG/READ_REG: mask applied to the read value. MASK_REG: bits
	// cleared before `value` is OR'd in.
	uint32_t mask;
	// WRITE_BLOB source bytes (may be NULL = zero-fill) and length.
	const uint8_t *blob;
	uint32_t blob_len;
	// POLL_REG: nvkm_msec budget in microseconds.
	uint32_t timeout_us;
	// Nouveau provenance: "file:line function — note". Never NULL/empty
	// for steps produced by pg106_acrboot_plan_build.
	const char *why;
};

// Caller-supplied simulated hardware/firmware geometry. Fields marked
// [READBACK] are what the plan EXPECTS the hardware to return at that point
// (emitted as READ_REG steps so a replay against a real or mock bus checks
// them); on real hardware they must come from the device.
struct pg106_acrboot_config {
	// SEC2 falcon MMIO base. Resolve it with pg106_acrboot_falcon_base()
	// from the nvkm_top table (nvkm/falcon/base.c:261-264); fallback
	// PG106_ACRBOOT_SEC2_BASE_GP106 (measured, see above).
	uint32_t falcon_base;
	// [READBACK] FALCON_HWCFG (0x108) at nvkm_falcon_oneinit
	// (base.c:273-275): code.limit = (hwcfg & 0x1ff) << 8 — the BL is
	// PIO-loaded at code.limit - boot_size (gm200.c:291). SEC2 IMEM was
	// measured at 64 KB on GP106 (FINDINGS.md:160) — use a value whose
	// decode gives 0x10000; the exact register word for SEC2 was not
	// captured, so the full word is a SIM stand-in.
	uint32_t hwcfg;
	// [READBACK] SEC2 0x408 (gp102_sec2_flcn .debug): bit 20 selects the
	// debug HS signature (gm200.c:335-337).
	uint32_t debug_reg;
	// VA the HS image is DMA-mapped at (fw->vma->addr, gm200.c:229).
	uint64_t vma_addr;
	// Simulated base address of the WPR+shadow allocation
	// (gp102_acr_wpr_alloc: shadow at base, WPR at base + half —
	// nvkm/subdev/acr/gp102.c:121-129).
	uint64_t wpr_alloc_base;
	// WPR header fields: bootstrap_owner (gp102.c:89 NVKM_ACR_LSF_SEC2)
	// and the rtos falcon id for lazy_bootstrap (gp102.c:90). On GP106
	// the rtos is SEC2: nvkm_acr_rtos() returns the first LSF whose func
	// has .bootstrap_falcon (acr/base.c:59-72), and only gp102_sec2_acr_*
	// have one (sec2/gp102.c:118,279). UINT32_MAX = "no rtos".
	uint32_t bootstrap_owner;
	uint32_t rtos_id;
	// Expected MBOX0 after boot (gp102_acr_load_fwif boot_mbox0 = 0,
	// nvkm/subdev/acr/gp102.c:239) and IRQSCLR mask (intr_clear = 0x10).
	uint32_t boot_mbox0;
	uint32_t intr_clear;
	// [READBACK] PMC_BOOT_0 (0x000000), copied into falcon 0x084 by
	// gm200_flcn_enable (gm200.c:179). GP106 A1 reads 0x136000a1
	// (FINDINGS.md:50).
	uint32_t pmc_boot0;
	// SEC2's PMC_ENABLE bit(s): nvkm_mc_reset_mask -> BIT(top reset
	// line) (nvkm/subdev/mc/base.c:51-69, top/base.c:61-74). Get it from
	// pg106_top_find(..)->reset. 0 = no PMC reset line known (nouveau
	// then skips mc_disable/enable, mc/base.c:85,93).
	uint32_t pmc_mask;
	// BINDINST fields (gm200.c:135): memory target (0 = VRAM) and
	// instance address (simulated).
	uint32_t bind_target;
	uint64_t bind_addr;
	// [READBACK] SEC2 CPUCTL bit 6 (ALIAS_EN) when nvkm_sec2_init starts
	// the RTOS: nvkm_falcon_v1_start writes 0x130 instead of 0x100 when
	// set (nvkm/falcon/v1.c:93-101).
	bool cpuctl_alias_en;
};

// Derived geometry.
struct pg106_acrboot_info {
	// Total shadow-copy size from pg106_acr_wpr_layout.
	uint32_t wpr_size;
	// Allocation size = ALIGN(wpr_size, 0x40000) << 1 (gp102.c:122).
	uint64_t alloc_size;
	// shadow_start = wpr_alloc_base, wpr_start/end = second half.
	uint64_t shadow_start;
	uint64_t wpr_start;
	uint64_t wpr_end;
	// HWCFG decode (base.c:273-275).
	uint32_t code_limit;
	uint32_t data_limit;
	// Debug part (0x408 bit 20) -> debug HS signature patched.
	bool debug_sig;
	// Number of steps in the plan.
	size_t nsteps;
};

// Built plan: owns its step array and private scratch buffers.
struct pg106_acrboot_plan {
	struct pg106_acrboot_step *steps;
	size_t nsteps;
	// PRIVATE — scratch allocations the steps may point into (serialized
	// WPR/LSB headers, bootloader data blocks, the BL DMEM argument
	// block). Owned by the plan; freed by plan_free.
	void **scratch_;
	size_t nscratch_;
};

// Falcon-base resolution. nouveau derives a falcon's PRI base from the
// nvkm_top table at falcon oneinit (nvkm/falcon/base.c:261-264:
// falcon->addr = nvkm_top_addr(device, type, inst)). `top` is a table
// scanned by pg106_top_scan(); the SEC2 instance-0 entry's base is
// returned. When the table is NULL, holds no SEC2/0 entry, or its base row
// never arrived (addr 0), `fallback` is returned instead — upstream has no
// fallback (base.c:263-264 WARNs and returns -ENODEV); callers should pass
// PG106_ACRBOOT_SEC2_BASE_GP106.
uint32_t pg106_acrboot_falcon_base(const struct pg106_top_table *top,
				   uint32_t fallback);

// Build the ordered step list for the gp102 ACR secure boot, from SEC2
// oneinit to the SEC2 RTOS start. Inputs are the already acr.c-parsed blob
// views plus the LS falcon descriptions (lsfws, in nouveau's acr->lsfw
// order: GR registers FECS then GPCCS before SEC2 registers itself, since
// NVKM_ENGINE_GR precedes NVKM_ENGINE_SEC2 in include/nvkm/core/layout.h:
// 38,51) and their signatures. `lsfws` is mutated exactly like
// pg106_acr_wpr_layout does.
//
// Group order mirrors nouveau end to end (each step's `why` cites it):
//   1. SEC2 oneinit reads: 0x12c, HWCFG 0x108, debug 0x408 (base.c:267-280)
//   2. WPR shadow build + wpr_patch      (acr/base.c:283-306; gp102.c:33-116)
//   3. HS signature select + patch: nvkm_falcon_enable then 0x408 bit 20
//                                        (fw.c:84-90 -> gm200.c:324-342)
//   4. Falcon reset: disable (0x048 RMW, 0x014, mc_disable, reset_eng),
//      enable (reset_eng, mc_enable, scrub wait, 0x084)
//                                        (base.c:242-252, gm200.c:154-215,
//                                         gp102.c:66-82)
//   5. ACR desc setup in the image DMEM  (gp102.c:196-213)
//   6. Load: inst bind (bind_intr: 0x008 bit 3, then 0x0dc==5), BL PIO to
//      IMEM top, BL args to DMEM 0 — and nothing else: with fw->boot set
//      gm200_flcn_fw_load returns after load_bld (gm200.c:289-297)
//   7. Boot: mailbox, BOOTVEC, CPUCTL start, halt poll, read-back, IRQSCLR
//                                        (gm200.c:217-246)
//   8. WPR window check                  (acr/base.c:105-112, gm200.c:52-62)
//   9. SEC2 RTOS start, nvkm_sec2_init via nvkm_subdev_ref(rtos owner)
//                                        (acr/base.c:116-121,
//                                         sec2/base.c:74-93, v1.c:93-101)
// The SEC2 init message and the BOOTSTRAP_FALCON commands that follow are
// data-dependent (queue offsets come from the firmware) and are executed by
// the pg106_sec2_* functions below rather than planned.
//
// Returns 0, -EINVAL on bad arguments/inconsistent inputs, -ENOMEM.
int pg106_acrboot_plan_build(const struct pg106_acrboot_config *cfg,
			     const struct pg106_acr_bl_ucode *bl,
			     const struct pg106_acr_hs_ucode *hs,
			     struct pg106_acr_lsfw *lsfws, size_t nlsfw,
			     const struct pg106_acr_lsf_signature_v1 *const *sigs,
			     struct pg106_acrboot_plan *plan,
			     struct pg106_acrboot_info *info);

// Release the plan's step array and scratch buffers.
void pg106_acrboot_plan_free(struct pg106_acrboot_plan *plan);

// Human-readable names for the enums above (for trace dumps). Never NULL.
const char *pg106_acrboot_kind_name(uint8_t kind);
const char *pg106_acrboot_space_name(uint8_t space);
const char *pg106_acrboot_poll_op_name(uint8_t op);

// ---------------------------------------------------------------------------
// SEC2 RTOS message queues: init message + ACR BOOTSTRAP_FALCON
// ---------------------------------------------------------------------------
//
// Executable port of the nouveau falcon queue manager as GP10x SEC2 uses it:
// nvkm/falcon/{cmdq.c,msgq.c,qmgr.c,qmgr.h}, nvkm/engine/sec2/gp102.c
// (initmsg, intr, acr_bootstrap_falcon), include/nvfw/sec2.h and
// include/nvkm/core/falcon.h:180-188 (nvfw_falcon_msg). It runs against a
// caller-provided register bus, so the same code drives a mock in tests and
// would drive BAR0 on hardware. Deviations from nouveau, all because the
// portable core has no IRQs, threads or jiffies:
//   * interrupt delivery is polled: waits call pg106_sec2_intr() (a direct
//     port of gp102_sec2_intr) up to `max_polls` times instead of sleeping
//     on a completion;
//   * every timeout (cmdq ready 1000 ms, queue space 2000 ms, reply
//     1000 ms) becomes that poll budget.

// Register bus. Addresses are absolute BAR0 byte offsets.
struct pg106_flcn_bus {
	void *ctx;
	uint32_t (*rd32)(void *ctx, uint32_t addr);
	void (*wr32)(void *ctx, uint32_t addr, uint32_t val);
};

// nvfw_falcon_msg/cmd header size (include/nvkm/core/falcon.h:180-188).
#define PG106_FLCN_HDR_SIZE		4u
// qmgr.h:7-9 QUEUE_ALIGNMENT / MSG_BUF_SIZE; qmgr.h:42 SEQ_NUM.
#define PG106_FLCN_QUEUE_ALIGNMENT	4u
#define PG106_FLCN_MSG_BUF_SIZE		128u
#define PG106_FLCN_QMGR_SEQ_NUM		16u
// NV_FALCON_CMD_UNIT_ID_REWIND (include/nvkm/core/falcon.h:188).
#define PG106_FLCN_UNIT_ID_REWIND	0x00u
// cmdq.c:118-121 CMD_FLAGS_STATUS | CMD_FLAGS_INTR.
#define PG106_FLCN_CMD_FLAGS_STATUS	0x01u
#define PG106_FLCN_CMD_FLAGS_INTR	0x02u
// include/nvfw/sec2.h:12-14,18,28-29,62,73-74.
#define PG106_SEC2_UNIT_INIT		0x01u
#define PG106_SEC2_UNIT_UNLOAD		0x06u
#define PG106_SEC2_UNIT_ACR		0x08u
#define PG106_SEC2_INIT_MSG_INIT	0x00u
#define PG106_SEC2_QUEUE_ID_CMDQ	0x00u
#define PG106_SEC2_QUEUE_ID_MSGQ	0x01u
#define PG106_SEC2_ACR_CMD_BOOTSTRAP_FALCON 0x00u
#define PG106_SEC2_ACR_BOOTSTRAP_FALCON_FLAGS_RESET_YES 0x00000000u
// sizeof(struct nv_sec2_init_msg) with natural alignment
// (include/nvfw/sec2.h:16-35): hdr 0, msg_type 4, num_queues 5,
// os_debug_entry_point 6, queue_info[0] 8 {offset u32 +0, size u16 +4,
// index u8 +6, id u8 +7}, queue_info[1] 16, sw_managed_area_offset 24,
// sw_managed_area_size 28, padded to 32.
#define PG106_SEC2_INIT_MSG_SIZE	32u
// sizeof(struct nv_sec2_acr_bootstrap_falcon_cmd/_msg) (sec2.h:60-83):
// nv_sec2_acr_cmd is 5 bytes of u8 (hdr + cmd_type), so flags lands at 8
// and falcon_id (msg: error_code @8, falcon_id @12) at 12; size 16.
#define PG106_SEC2_ACR_BOOTSTRAP_CMD_SIZE 16u
#define PG106_SEC2_ACR_BOOTSTRAP_MSG_SIZE 16u
// gp102_sec2_acr_{0,1}.bootstrap_falcons (sec2/gp102.c:115-117,276-278).
#define PG106_SEC2_BOOTSTRAP_FALCONS					   \
	((1ull << PG106_ACR_LSF_FECS) | (1ull << PG106_ACR_LSF_GPCCS) |	   \
	 (1ull << PG106_ACR_LSF_SEC2))

// nvkm_falcon_cmdq / nvkm_falcon_msgq state (qmgr.h:59-83).
struct pg106_sec2_queue {
	uint32_t head_reg; // falcon-relative (func->cmdq.head + index*stride)
	uint32_t tail_reg;
	uint32_t offset;   // DMEM/EMEM address of the ring
	uint32_t size;     // cmdq only (msgq never uses it, msgq.c:177-188)
	uint32_t position;
	bool ready;        // cmdq->ready completion (cmdq.c:131,184)
};

// nvkm_falcon_qmgr_seq (qmgr.h:23-36).
enum pg106_sec2_seq_state {
	PG106_SEC2_SEQ_FREE = 0,
	PG106_SEC2_SEQ_PENDING,
	PG106_SEC2_SEQ_USED,
	PG106_SEC2_SEQ_CANCELLED,
};
struct pg106_sec2_seq {
	uint8_t state;
	bool done;     // seq->done completion
	int result;    // seq->result (callback return)
	// BOOTSTRAP_FALCON reply fields kept for the caller's log.
	uint32_t msg_falcon_id;
	uint32_t msg_error_code;
};

// Parsed nv_sec2_init_msg.
struct pg106_sec2_init_msg {
	uint8_t unit_id, size, ctrl_flags, seq_id;
	uint8_t msg_type, num_queues;
	uint16_t os_debug_entry_point;
	struct {
		uint32_t offset;
		uint16_t size;
		uint8_t index;
		uint8_t id;
	} queue_info[2];
	uint32_t sw_managed_area_offset;
	uint16_t sw_managed_area_size;
};

struct pg106_sec2 {
	struct pg106_flcn_bus bus;
	uint32_t base;      // SEC2 falcon PRI base
	uint32_t max_polls; // SIM stand-in for every jiffies timeout
	// sec2->initmsg (sec2/gp102.c:162-168): 0 = not yet, 1 = parsed,
	// <0 = parse error.
	int initmsg;
	struct pg106_sec2_init_msg init;
	struct pg106_sec2_queue cmdq, msgq;
	struct pg106_sec2_seq seq[PG106_FLCN_QMGR_SEQ_NUM];
	uint32_t unhandled_intr; // OR of intr bits gp102_sec2_intr reports
};

// nvkm_sec2_new_ + qmgr/cmdq/msgq_new: zeroed state for a falcon at base.
void pg106_sec2_ctx_init(struct pg106_sec2 *s, const struct pg106_flcn_bus *bus,
			 uint32_t base, uint32_t max_polls);

// One pass of gp102_sec2_intr (sec2/gp102.c:152-196): reads IRQDEST and
// IRQSTAT, handles SWGEN0 (0x40: init message first, then msgq_recv) and
// HALT (0x10), acks with 0x004 writes. Returns the handled bit set.
uint32_t pg106_sec2_intr(struct pg106_sec2 *s);

// nvkm_falcon_msgq_recv_initmsg + gp102_sec2_initmsg (msgq.c:153-174,
// sec2/gp102.c:121-150). Normally reached from pg106_sec2_intr.
int pg106_sec2_initmsg(struct pg106_sec2 *s);

// nvkm_falcon_cmdq_send (cmdq.c:123-166) of NV_SEC2_ACR_CMD_BOOTSTRAP_FALCON
// with FLAGS_RESET_YES for `falcon_id` (gp102_sec2_acr_bootstrap_falcon,
// sec2/gp102.c:58-75), waiting for the reply; the reply's error_code != 0
// gives -EINVAL (callback, sec2/gp102.c:39-56). -ETIMEDOUT when the queue
// never became ready or no reply came within max_polls.
int pg106_sec2_bootstrap_falcon(struct pg106_sec2 *s, uint32_t falcon_id);

// nvkm_acr_bootstrap_falcons with the SEC2 rtos (acr/base.c:134-169):
// -ENOSYS unless every bit of mask is in PG106_SEC2_BOOTSTRAP_FALCONS, then
// one BOOTSTRAP_FALCON per set bit in ascending id order (for_each_set_bit).
// GR asks for FECS|GPCCS (gf100_gr_init_ctxctl_ext,
// nvkm/engine/gr/gf100.c:1761-1781).
int pg106_acr_bootstrap_falcons(struct pg106_sec2 *s, uint64_t mask);

#endif // PASCAL_ACR_BOOT_H
