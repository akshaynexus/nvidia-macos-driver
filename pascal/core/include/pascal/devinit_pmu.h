// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/devinit_pmu.h — GP106 POST model: posted
// detection + the gm200 VBIOS-PMU DEVINIT upload/run sequence.
//
// SIMULATION. This replays nouveau's GP10x POST path over a caller bus and
// records every register access as a step trace. It is a model of what the
// driver would do, not evidence the GPU accepts it.
//
// Upstream chain (linux master, /tmp/review-ref/linux d48723711bae):
//   nvkm/engine/device/base.c:2177  nv136_chipset (GP106): .devinit =
//                                   gm200_devinit_new, .pmu = gp102_pmu_new
//   nvkm/engine/device/base.c:3016  nvkm_device_preinit ->
//                                   nvkm_devinit_post; failure is fatal
//                                   (goto fail)
//   nvkm/subdev/devinit/base.c:59-96 nvkm_devinit_post / _preinit
//                                   (NvForcePost override l.87-91)
//   nvkm/subdev/devinit/gf100.c:90-102 gf100_devinit_preinit — the posted
//                                   bit: post = !(rd32(0x2240c) & BIT(1))
//   nvkm/subdev/devinit/gm200.c:32-176 pmu_code / pmu_data / pmu_args /
//                                   pmu_exec / pmu_load /
//                                   gm200_devinit_preos /
//                                   gm200_devinit_post
//   nvkm/subdev/bios/pmu.c:74-102   nvbios_pmuRm (vbios_bit.h)
//   nvkm/falcon/base.c:243-252      nvkm_falcon_reset = disable + enable
//   nvkm/subdev/pmu/gp102.c:28-42   gp102_pmu_flcn: gm200_flcn_disable /
//                                   gm200_flcn_enable, reset_eng =
//                                   gp102_flcn_reset_eng, no reset_pmc
//   nvkm/falcon/gm200.c:140-215     gm200_flcn_reset_wait_mem_scrubbing,
//                                   _enable, _disable
//   nvkm/falcon/gp102.c:66-82       gp102_flcn_reset_eng (0x3c0 toggle)
//
// What the POST does (gm200.c:123-176):
//   1. BIT 'I' must be version 1 with length >= 0x1c, else -EINVAL
//      "VBIOS PMU init data not found".
//   2. pmu_load(0x04 DEVINIT): nvbios_pmuRm must find the app, else
//      -EINVAL "VBIOS PMU/DEVINIT not found" (checked even when !post).
//      With post: falcon reset (pg106_pmu_falcon_reset below; on the
//      first hardware run it was skipped and DEVINIT never ran), IMEM boot (0x10a180/0x10a188/0x10a184),
//      IMEM secure code, DMEM data (0x10a1c0/0x10a1c4).
//   3. pmu_args(args+8, 0x08) -> DMEM address for the init TABLES
//      ('I'+0x14 ptr / +0x16 len); pmu_data upload.
//   4. pmu_args(args+8, 0x10) -> DMEM address for the BOOT SCRIPTS
//      ('I'+0x18 ptr / +0x1a len); pmu_data upload.
//   5. 0x10a040 = 0x5000, pmu_exec(init_addr) (0x10a104/0x10a10c/
//      0x10a100 = 2), poll 0x10a040 bit 13 for 2 s -> -ETIMEDOUT.
//   6. gm200_devinit_preos: pmu_load(0x01 PRE_OS) and exec; failure
//      ignored.

#ifndef PASCAL_DEVINIT_PMU_H
#define PASCAL_DEVINIT_PMU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pascal/vbios_bit.h>

#ifdef __cplusplus
extern "C" {
#endif

// Posted detection (gf100.c:101). Set by devinit, cleared on suspend.
#define PG106_DEVINIT_POSTED_REG 0x0002240cu
#define PG106_DEVINIT_POSTED_BIT 0x00000002u

// PMU falcon registers used by gm200.c (PMU falcon base 0x10a000).
#define PG106_PMU_FALCON_CPUCTL   0x0010a100u // pmu_exec: = 2 (start)
#define PG106_PMU_FALCON_BOOTVEC  0x0010a104u // pmu_exec: init address
#define PG106_PMU_FALCON_DMACTL   0x0010a10cu // pmu_exec: = 0
#define PG106_PMU_FALCON_MAILBOX  0x0010a040u // 0x5000 / poll bit 13
#define PG106_PMU_FALCON_IMEMC0   0x0010a180u
#define PG106_PMU_FALCON_IMEMD0   0x0010a184u
#define PG106_PMU_FALCON_IMEMT0   0x0010a188u
#define PG106_PMU_FALCON_DMEMC0   0x0010a1c0u
#define PG106_PMU_FALCON_DMEMD0   0x0010a1c4u
#define PG106_PMU_DEVINIT_DONE    0x00002000u // gm200.c:168
#define PG106_PMU_FALCON_MAILBOX1 0x0010a044u // diagnostics only
// nvkm_falcon_reset registers (falcon/gm200.c, falcon/gp102.c).
#define PG106_PMU_FALCON_IRQMCLR  0x0010a014u // disable: = 0xffffffff
#define PG106_PMU_FALCON_RM       0x0010a048u // disable: mask 0x3 -> 0
#define PG106_PMU_FALCON_BOOT0    0x0010a084u // enable: = PMC_BOOT_0
#define PG106_PMU_FALCON_ENGCTL   0x0010a3c0u // reset_eng: bit 0 1 -> 0
#define PG106_PMU_SCRUB_BUSY      0x00000006u // 0x10c IMEM|DMEM scrubbing
// nvkm_msec(device, 10, ...) of the scrub wait, as 10000 x 1 us polls.
#define PG106_PMU_SCRUB_POLLS     10000u

// nvkm_device_preinit writes before the POST that nouveau makes on GP106
// and this port replays (devboot's POST stage), from the owner's Helios
// mmiotraces (research/hardware/helios300/raw/posttrace*-2026-11-07):
#define PG106_PCI_MIRROR_0x50     0x00088050u // nvkm_pci_rom_shadow
#define PG106_MC_INTR_BLOCK(leaf) (0x00000180u + 4u * (leaf)) // gp100 mc

// Poll budget of nvkm_msec(device, 2000, ...) (gm200.c:167), modelled as
// 2000 polls of 1 ms each on the bus clock hook.
#define PG106_DEVINIT_POST_POLLS 2000u

// gf100_devinit_preinit + the NvForcePost override (devinit/base.c:84-91).
static inline bool
pg106_devinit_needs_post(uint32_t r2240c, bool force_post)
{
	bool post = (r2240c & PG106_DEVINIT_POSTED_BIT) == 0;
	return force_post ? true : post;
}

enum pg106_pmu_step_kind {
	PG106_PMU_STEP_WR = 0,   // nvkm_wr32
	PG106_PMU_STEP_RD,       // nvkm_rd32 (value = what the bus returned)
	PG106_PMU_STEP_RESET,    // nvkm_falcon_reset (value = return code)
	PG106_PMU_STEP_POLL,     // nvkm_msec poll summary (value = polls)
};

struct pg106_pmu_step {
	uint8_t kind;
	uint8_t phase;      // enum pg106_post_phase
	uint32_t addr;
	uint32_t val;
};

enum pg106_post_phase {
	PG106_POST_PH_DEVINIT_LOAD = 0, // pmu_load(0x04)
	PG106_POST_PH_TABLES,           // 'I'+0x14/+0x16 -> DMEM
	PG106_POST_PH_SCRIPTS,          // 'I'+0x18/+0x1a -> DMEM
	PG106_POST_PH_EXEC,             // 0x10a040 + pmu_exec + poll
	PG106_POST_PH_PREOS,            // gm200_devinit_preos
	PG106_POST_PH_PREINIT,          // pg106_devinit_preinit
};

// Bus underneath the model. rd32/wr32 required; falcon_reset/time
// optional. NULL falcon_reset = the register-level nvkm_falcon_reset of
// the GP102 PMU (pg106_pmu_falcon_reset; what the hardware path uses);
// a non-NULL hook replaces it (tests that model the reset themselves).
// NULL time = no clock (polls still bounded by count).
struct pg106_pmu_bus {
	uint32_t (*rd32)(void *user, uint32_t addr);
	void (*wr32)(void *user, uint32_t addr, uint32_t val);
	int (*falcon_reset)(void *user);
	void (*time)(void *user, uint32_t usec);
	void *user;
};

// Everything the run decided, for evidence output.
struct pg106_post_result {
	int ret;              // 0 / -EINVAL / -ETIMEDOUT / reset error
	const char *error;    // upstream nvkm_error text, NULL on success
	bool post;            // the post flag the run used
	struct pg106_vbios_bit_entry bit_I;
	bool have_devinit_app;
	struct pg106_vbios_pmu_app devinit; // pmu_load(0x04) app
	uint32_t exec_addr, args_addr;      // *init_addr_pmu / *args_addr_pmu
	// pmu_args results + the ROM ranges uploaded behind them.
	uint32_t tables_dmem, tables_img, tables_len;
	uint32_t scripts_dmem, scripts_img, scripts_len;
	uint32_t polls;
	bool have_preos_app;
	struct pg106_vbios_pmu_app preos;
	size_t steps;          // steps recorded (or that would have been)
	bool trace_truncated;  // cap exhausted (the run itself continues)
	uint32_t imem_words, dmem_words; // data words written per memory
	// Falcon reset (built-in path): 0x10c polls per scrub wait (enable
	// path = the last one) and the failing wait's 0x10c value.
	uint32_t resets, scrub_polls_max, scrub_stuck;
	// Read after a DEVINIT timeout, for the hardware log (0 otherwise).
	bool have_diag;
	uint32_t diag_cpuctl, diag_mbox0, diag_mbox1, diag_dmactl,
		 diag_engctl, diag_pmc_enable, diag_posted;
};

// Run gm200_devinit_post(post) + preos over `bus`. `has_pmu` mirrors
// device->pmu != NULL (gp102_pmu_new returns -ENODEV under GSP-RM; on the
// nouveau-native path it exists). Steps go to trace[0..cap) (may be NULL).
// Returns res->ret.
int pg106_devinit_gm200_post(const struct pg106_vbios_bit *vb, bool post,
			     bool has_pmu, const struct pg106_pmu_bus *bus,
			     struct pg106_pmu_step *trace, size_t cap,
			     struct pg106_post_result *res);

// nvkm_falcon_reset of the GP102/GP106 PMU falcon (0x10a000), register
// for register as nouveau issues it (falcon/base.c:243-252):
//   gm200_flcn_disable  (gm200.c:194-215): mask(0x048, 3, 0);
//       wr(0x014, 0xffffffff); [no reset_pmc] reset_eng
//   gp102_flcn_reset_eng (gp102.c:66-82): mask(0x3c0, 1, 1); udelay(10);
//       mask(0x3c0, 1, 0); reset_wait_mem_scrubbing
//   gm200_flcn_reset_wait_mem_scrubbing (gm200.c:140-151):
//       mask(0x040, 0, 0); poll 0x10c & 6 == 0 for 10 ms -> -ETIMEDOUT
//   gm200_flcn_enable   (gm200.c:153-180): reset_eng; [no select, no
//       reset_pmc] reset_wait_mem_scrubbing; wr(0x084, rd(0x000000))
// Nouveau's trace on this GPU (posttrace-2026-11-07) shows exactly these
// writes, and 0x10c reading 0x7 for ~1.5 ms after each 0x3c0 toggle.
// Steps are recorded with `phase` when trace is non-NULL; `steps` counts
// them (may be NULL). Returns 0 or -ETIMEDOUT (scrubbing never finished).
int pg106_pmu_falcon_reset(const struct pg106_pmu_bus *bus,
			   struct pg106_pmu_step *trace, size_t cap,
			   size_t *steps, uint8_t phase,
			   struct pg106_post_result *res);

// The nvkm_device_preinit writes nouveau makes before gm200_devinit_post
// that this port replays (device/base.c:2992-3020), in nouveau's order:
//   nvkm_bios (bios/shadow.c via nvkm_pci_rom_shadow, pci/base.c:66-74):
//       0x088050 bit 0 cleared for the PROM read, then set again — the
//       net state the DEVINIT app starts with is bit 0 = 1 (trace: 0 ->
//       0 -> 1). This port reads the VBIOS from a file, so only the pair
//       of read-modify-writes is replayed.
//   nvkm_intr_unarm -> gp100_mc_intr_unarm (mc/gp100.c:57-68): 0x180 and
//       0x184 = 0xffffffff (both interrupt leaves blocked).
// Not replayed (and why), all seen in the same trace before the reset:
//   nvkm_lockvgac(false) (devinit/base.c:94): 8-bit 0x6013d4/0x6013d5
//       CR3F = 0x57 — host VGA register unlock; the bus is 32-bit only
//       and DEVINIT runs on the PMU, not through the host VGA ports.
//   nvkm_i2c_preinit pad/bus init (0x00db0c.. |= 1, 0x00d034.. = 7):
//       DCB-derived, owned by the display side.
//   nvkm_fb_preinit 0x100c10 (sysmem flush page): needs a DMA page;
//       gpu.c's FB init writes it when the platform provides one.
//   nvkm_fifo_preinit PMC_ENABLE bit 8 reset: fifo.c does it at FIFO init.
size_t pg106_devinit_preinit(const struct pg106_pmu_bus *bus,
			     struct pg106_pmu_step *trace, size_t cap);

const char *pg106_post_phase_name(uint8_t phase);

#ifdef __cplusplus
}
#endif

#endif // PASCAL_DEVINIT_PMU_H
