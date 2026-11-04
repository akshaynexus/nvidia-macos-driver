// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/acr_boot.h — Phase 2 ACR secure-boot SIMULATOR:
// builds the ordered falcon-loading step list (reset, IMEM/DMEM PIO writes,
// mailbox handshake, WPR shadow setup) that nouveau's nvkm ACR executes when
// booting the HS "load" firmware on GP10x, driven by the real firmware blobs
// parsed by pascal/core/src/acr.c.
//
// SIMULATION, NOT HARDWARE PROOF (HANDOFF-PASCAL.md Phase 2 gate wording):
// this module produces a MODEL of the boot sequence. The step list is meant
// to be replayed against a mock bus (see pascal/tests/test_acr_boot.c) that
// merely records writes; nothing here touches, claims, or proves anything
// about real hardware.
//
// Every step carries a `why` string naming its nouveau source origin
// (file:function + line, from the reference copies of linux master under
// /tmp/pascal-ref/upstream/, fetched 2026-11-05). Key upstream sources:
//   nvkm/falcon/falcon? no — nvkm/falcon/{base.c,gm200.c,gp102.c,v1.c,fw.c}
//   nvkm/subdev/acr/{base.c,gm200.c,gp102.c}
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
// Engine clock-enable mirror (value = PMC enable reg read-back) —
// nvkm/falcon/gm200.c:179 (gm200_flcn_enable).
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
// Memory-scrubbing status (bits 1:2 = code/data busy) — nvkm/falcon/gm200.c
// :143-148 (gm200_flcn_reset_wait_mem_scrubbing), :286 (fw_load clears it).
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
// GP10x falcon engine reset — nvkm/falcon/gp102.c:77-79
// (gp102_flcn_reset_eng: set bit 0, 10us delay, clear bit 0).
#define PG106_FALCON_GP102_RESET	 0x3c0u
// DMA context select (DMAIDX_VIRT = 0) — nvkm/falcon/gm200.c:134
// (gm200_flcn_bind_inst).
#define PG106_FALCON_DMAIDX		 0x604u
// DMA transfer control — nvkm/falcon/gm200.c:285 (fw_load no-inst branch,
// set bit 7). Not exercised by the inst-bound ACR path modeled here.
#define PG106_FALCON_DMACTL		 0x624u

// PFB WPR-check window register (falcon_base-relative? NO — full BAR0
// address; handled specially below) — nvkm/subdev/acr/gm200.c:57-60
// (gm200_acr_wpr_check: write selector 2/3, read window in [31:8]).
#define PG106_PFB_WPR_CHECK		 0x100cd4ull

// PIO transfer granularity: gm200_flcn_imem_pio.min/.max = 0x100 and
// gm200_flcn_dmem_pio.max = 0x100 (nvkm/falcon/gm200.c:89-90,114-120).
#define PG106_FALCON_PIO_CHUNK		 0x100u

// ---------------------------------------------------------------------------
// Step model
// ---------------------------------------------------------------------------

// Address space a step acts on. MMIO = BAR0 falcon registers; WPR = the
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
	// nvkm_falcon_pio_wr()/nvkm_wobj() data phase. blob may be NULL
	// (= zero fill; used where the sim lacks the LS ucode image bytes).
	PG106_ACRBOOT_WRITE_BLOB = 1,
	// Spin until (rd32(addr) & mask) satisfies `op` vs value (see below).
	PG106_ACRBOOT_POLL_REG = 2,
	// Register read-back with expected (value & mask): mailbox results,
	// WPR window check.
	PG106_ACRBOOT_READ_REG = 3,
};

// Poll comparison: ANY = stop when (rd32 & mask) != 0; EQ = stop when
// (rd32 & mask) == value.
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
	// Address: falcon_base + offset for MMIO steps (the plan bakes
	// falcon_base in), object-relative offset for WPR/IMG steps.
	uint64_t addr;
	// WRITE_REG: the 32-bit value. POLL_REG: `value` for POLL_EQ.
	// READ_REG: expected (value & mask).
	uint32_t value;
	// POLL_REG/READ_REG: mask applied to the read value.
	uint32_t mask;
	// WRITE_BLOB source bytes (may be NULL = zero-fill) and length.
	const uint8_t *blob;
	uint32_t blob_len;
	// Nouveau provenance: "file:line function — note". Never NULL/empty
	// for steps produced by pg106_acrboot_plan_build.
	const char *why;
};

// Caller-supplied simulated hardware/firmware geometry. The falcon base
// address and IMEM limit are runtime-discovered on real hardware (nvkm_top,
// FALCON_MEM_CAPS); the test supplies plausible stand-ins.
struct pg106_acrboot_config {
	// Simulated falcon MMIO base (real GP106 SEC2 base comes from
	// nvkm_top at runtime: nvkm/falcon/base.c:261-265 — stand-in here).
	uint32_t falcon_base;
	// Falcon IMEM limit from FALCON_MEM_CAPS (base.c:273-275): the BL is
	// loaded at code_limit - boot_size (gm200.c:291).
	uint32_t code_limit;
	// VA the HS image is DMA-mapped at (fw->vma->addr, gm200.c:229).
	uint64_t vma_addr;
	// Simulated base address of the WPR+shadow allocation
	// (gp102_acr_wpr_alloc: shadow at base, WPR at base + half —
	// nvkm/subdev/acr/gp102.c:121-129).
	uint64_t wpr_alloc_base;
	// WPR header fields: bootstrap_owner (gp102.c:89 NVKM_ACR_LSF_SEC2)
	// and the rtos falcon id for lazy_bootstrap (gp102.c:90); pass
	// UINT32_MAX for "no rtos".
	uint32_t bootstrap_owner;
	uint32_t rtos_id;
	// Expected MBOX0 after boot (gp102_acr_load_fwif boot_mbox0 = 0,
	// nvkm/subdev/acr/gp102.c:239) and IRQSCLR mask (intr_clear = 0x10).
	uint32_t boot_mbox0;
	uint32_t intr_clear;
	// Value written to FALCON_ENGINE_EN: master copies the PMC enable
	// register there (gm200.c:179). Simulated stand-in.
	uint32_t pmc_readback;
	// BINDINST fields (gm200.c:135): memory target (0 = VRAM) and
	// instance address (simulated).
	uint32_t bind_target;
	uint64_t bind_addr;
};

// Derived WPR geometry, mirroring gp102_acr_wpr_alloc (gp102.c:118-131) on
// top of the pg106_acr_wpr_layout total.
struct pg106_acrboot_info {
	// Total shadow-copy size from pg106_acr_wpr_layout.
	uint32_t wpr_size;
	// Allocation size = ALIGN(wpr_size, 0x40000) << 1 (gp102.c:122).
	uint64_t alloc_size;
	// shadow_start = wpr_alloc_base, wpr_start/end = second half.
	uint64_t shadow_start;
	uint64_t wpr_start;
	uint64_t wpr_end;
	// Number of steps in the plan.
	size_t nsteps;
};

// Built plan: owns its step array and private scratch buffers.
struct pg106_acrboot_plan {
	struct pg106_acrboot_step *steps;
	size_t nsteps;
	// PRIVATE — scratch allocations the steps may point into (annotated
	// register-stream strings, serialized WPR/LSB headers, the BL DMEM
	// argument block). Owned by the plan; freed by plan_free.
	void **scratch_;
	size_t nscratch_;
};

// Build the ordered step list for the gp102 (r352) ACR secure boot of the
// real blobs: HS "load" firmware on the SEC2 falcon. Inputs are the already
// acr.c-parsed blob views (the caller validates the blobs first — structural
// signature/header validation is the Phase 2 gate and lives in acr.c) plus
// the LS falcon descriptions and their real gr/{fecs,gpccs}_sig.bin
// signatures for the WPR shadow build. `lsfws` is mutated exactly like
// pg106_acr_wpr_layout does (offsets/bl_data_size filled in).
//
// Group order mirrors nouveau end to end (each group's `why` cites its
// source):
//   1. HS signature patch into the image   (nvkm_falcon_fw_patch, fw.c:31-64)
//   2. Falcon reset                        (gm200_flcn_disable/enable,
//                                           gp102_flcn_reset_eng, scrub poll)
//   3. ACR desc setup in the image DMEM    (gp102_acr_load_setup, gp102.c:196)
//   4. Load: instance bind + BL PIO IMEM stream + BL DMEM args + the HS
//      image PIO stream modeled from the same loader's no-boot branch
//                                           (gm200_flcn_fw_load, gm200.c:248)
//   5. Boot: mailbox args, BOOTVEC, CPUCTL start, halted poll, mailbox
//      read-back, IRQSCLR                  (gm200_flcn_fw_boot, gm200.c:217)
//   6. WPR shadow build + window check     (gp102_acr_wpr_build, gp102.c:76;
//                                           gm200_acr_wpr_check, gm200.c:52;
//                                           nvkm_acr_load, acr/base.c:88)
//
// Returns 0, or -EINVAL on bad arguments/inconsistent inputs.
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

#endif // PASCAL_ACR_BOOT_H
