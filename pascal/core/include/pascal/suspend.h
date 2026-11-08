// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/suspend.h — system sleep and wake for the
// GP106: the suspend half of nouveau's nvkm_device_fini(suspend = true) for
// the units this driver brings up, and the checks the wake path makes
// before it boots the GPU again.
//
// nouveau (drivers/gpu/drm/nouveau, torvalds/linux d48723711bae):
//   nouveau_do_suspend (nouveau_drm.c:953) idles the channels, then
//   nvif_client_suspend -> nvkm_device_fini(device, suspend = true)
//   (nvkm/engine/device/base.c), which runs every subdev's fini in reverse
//   order. For the parts this driver has:
//     FIFO  nvkm_fifo_fini (engine/fifo/base.c): nvkm_inth_block, then
//           nvkm_runl_fini -> gk104_runl_fini (fifo/gk104.c):
//           mask(0x002630, BIT(runl->id), BIT(runl->id)) — runlist
//           scheduling off; every channel object's fini first stops it
//           (gk104_chan_stop: 0x800004 bit 11) and preempts it
//           (gk110_cgrp_preempt: 0x002634 = 0x01000000 | tsg, then
//           gf100_runl_preempt_pending: bit 20 clears).
//     GR    gf100_gr_fini; idle is gf100_gr_wait_idle (gr/gf100.c): GR
//           enabled (0x000200 bit 12), no context switch in progress
//           (0x002640 bit 15) and PGRAPH not busy (0x40060c bit 0).
//     CE    no state (gk104_engn_ce has no context): idle is its engine
//           status, 0x002640 + engine * 8 bit 31 (gk104_engn_status).
//     ACR   nvkm_acr_fini runs the HS "unload" ucode, which releases the
//           WPR. linux-firmware has gp106/acr/ucode_unload.bin, but this
//           driver does not carry it: it relies on power being cut across
//           sleep (the Helios's S3 and D3cold both do) and checks on wake
//           that the WPR is gone (pg106_resume_probe), the way
//           nvkm_acr_load compares gm200_acr_wpr_check with its own WPR.
//     DEVINIT nvkm_devinit_fini: "force full reinit on resume" (post =
//           true). Here devboot reads 0x2240c again on every boot and POSTs
//           an unposted GPU from the VBIOS PMU DEVINIT path.
//     MC    interrupts blocked (nvkm_mc_intr_unarm).
//   nouveau_do_resume (:1016) -> nvkm_device_init: devinit POST, mc init,
//   every subdev init again (FIFO, ACR + SEC2, GR with the golden context
//   gr->data kept from the first boot). This driver's resume is the
//   ordinary bring-up (pg106_gpu_boot) on the GPU the probe found cold.
//
// Every wait has a budget; pg106_gpu_suspend never returns early on a
// timeout (it still disables scheduling and blocks the interrupts) and
// reports what did not go idle.
//
// C11, libc only. SIMULATION status: tests/test_suspend.c (10 suspend/resume
// cycles on tests/mockgpu.h); not run on the GPU.

#ifndef PASCAL_SUSPEND_H
#define PASCAL_SUSPEND_H

#include <pascal/gpu.h>
#include <pascal/hwexec.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Default total time pg106_gpu_suspend may spend waiting (all polls).
#define PG106_SUSPEND_BUDGET_US 500000u

#define PG106_FIFO_SCHED_DISABLE 0x002630u
#define PG106_FIFO_PREEMPT       0x002634u
#define PG106_FIFO_ENGINE_STATUS(e) (0x002640u + (uint32_t)(e) * 8u)
#define PG106_PGRAPH_STATUS      0x40060cu
#define PG106_PFB_WPR_INFO       0x100cd4u // gm200_acr_wpr_check index/data

struct pg106_suspend_res {
	int ret;                   // 0, or the first failure (-ETIMEDOUT)
	const char *failed_at;     // first step that did not complete, NULL = all
	uint32_t chans_preempted;  // live channels stopped + preempted
	uint32_t preempt_timeouts; // ... of which the preempt did not finish
	uint32_t runl_mask;        // bits set in 0x002630
	uint32_t gr_status;        // last 0x40060c read
	uint32_t gr_ctxsw;         // last 0x002640 read
	uint32_t engines_busy;     // bit per PTOP engine id still busy at the end
	uint64_t waited_us;        // total time in polls
	uint32_t pmc_intr;         // PMC_INTR_0 (0x000100) at the end
};

// Quiesce a booted GPU before its power goes: block PMC interrupts, stop
// and preempt every channel still on the FIFO, disable scheduling on every
// runlist, wait for GR and every PTOP engine to go idle, turn the FIFO
// interrupts off. The FIFO is marked suspended, so pg106_gpu_fini (which
// the caller runs next) frees the channels without preempting them again.
// Safe on a GPU that never booted (returns 0, does nothing but report).
int pg106_gpu_suspend(struct pg106_gpu *g, uint32_t budget_us,
		      struct pg106_suspend_res *res);

// ---- wake ------------------------------------------------------------------

// gm200_acr_wpr_check (nvkm/subdev/acr/gm200.c): index 2 = WPR start,
// index 3 = WPR end (minus 0x20000), each the value & 0xffffff00 << 8.
// *valid is false when either read is a PRI error or all ones (a PFB that
// is not clocked: an unposted GPU). Locked = valid and start != 0.
void pg106_wpr_query(const struct pg106_hw_bus *bus, uint64_t *start,
		     uint64_t *end, bool *valid);

struct pg106_resume_probe {
	uint32_t boot0;          // read now
	uint32_t boot0_expected; // read at kext start
	bool boot0_readable;     // not all ones
	bool posted;             // 0x2240c bit 1
	bool wpr_valid, wpr_locked;
	uint64_t wpr_start, wpr_end;
	bool acpi_power;         // the platform can cut / restore power (PG00)
	unsigned attempt;        // power operations already made for this wake
};

enum pg106_resume_plan {
	PG106_RESUME_BOOT = 0,      // cold (or never locked): full bring-up
	PG106_RESUME_POWER_ON,      // BOOT_0 reads all ones: PG00._ON, probe again
	PG106_RESUME_POWER_CYCLE,   // power was not lost (WPR still locked):
				    // PG00._OFF + _ON for a cold GPU, probe again
	PG106_RESUME_FAIL_NO_POWER, // all ones, and no (more) way to power it
	PG106_RESUME_FAIL_WARM,     // WPR locked and power cannot be cut (no
				    // ACR unload firmware in this driver)
	PG106_RESUME_FAIL_IDENTITY, // readable but not the GPU of this load
};

// Read BOOT_0, the posted bit and the WPR. Reads only, except the two
// 0x100cd4 index writes of gm200_acr_wpr_check (made only when BOOT_0 is
// readable).
void pg106_resume_probe(const struct pg106_hw_bus *bus, uint32_t boot0_expected,
			bool acpi_power, unsigned attempt,
			struct pg106_resume_probe *out);
// Pure decision. At most two power operations per wake (attempt < 2).
uint8_t pg106_resume_decide(const struct pg106_resume_probe *p);
const char *pg106_resume_plan_name(uint8_t plan);

// Before booting a GPU whose power was not cut (posted, WPR not locked):
// undo pg106_gpu_suspend's scheduling disable (0x002630 = 0). A cold GPU
// comes out of reset with it clear, so the cold path never calls this.
void pg106_resume_warm_fixup(const struct pg106_hw_bus *bus);

#ifdef __cplusplus
}
#endif

#endif // PASCAL_SUSPEND_H
