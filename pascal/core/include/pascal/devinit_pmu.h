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
//   nvkm/falcon/base.c:243-252      nvkm_falcon_reset (bus hook here)
//
// What the POST does (gm200.c:123-176):
//   1. BIT 'I' must be version 1 with length >= 0x1c, else -EINVAL
//      "VBIOS PMU init data not found".
//   2. pmu_load(0x04 DEVINIT): nvbios_pmuRm must find the app, else
//      -EINVAL "VBIOS PMU/DEVINIT not found" (checked even when !post).
//      With post: falcon reset, IMEM boot (0x10a180/0x10a188/0x10a184),
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
};

// Bus underneath the model. rd32/wr32 required; falcon_reset/time
// optional (NULL reset = success, NULL time = no clock).
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
};

// Run gm200_devinit_post(post) + preos over `bus`. `has_pmu` mirrors
// device->pmu != NULL (gp102_pmu_new returns -ENODEV under GSP-RM; on the
// nouveau-native path it exists). Steps go to trace[0..cap) (may be NULL).
// Returns res->ret.
int pg106_devinit_gm200_post(const struct pg106_vbios_bit *vb, bool post,
			     bool has_pmu, const struct pg106_pmu_bus *bus,
			     struct pg106_pmu_step *trace, size_t cap,
			     struct pg106_post_result *res);

const char *pg106_post_phase_name(uint8_t phase);

#ifdef __cplusplus
}
#endif

#endif // PASCAL_DEVINIT_PMU_H
