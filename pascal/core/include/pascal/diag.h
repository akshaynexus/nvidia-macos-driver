// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/diag.h — first-contact failure decoder + bus
// liveness probe (PRIOR-ART-AUDIT.md gaps P0-2 and P0-3).
//
// Two halves:
//   1. pg106_diag_decode_readback(): classify a raw BAR0 readback against the
//      failure-signature table TheTom/pascal-egpu measured on real GP106
//      (research/prior-art/pascal-egpu/FINDINGS.md:29-44) plus the PRI
//      error-code meanings NVIDIA publishes in
//      open-gpu-kernel-modules .../maxwell/gm107/dev_pri_ringstation_sys.h
//      (lines 28-42). These are PRIOR-ART-DERIVED HEURISTICS, not nouveau
//      behavior: nouveau has no such decoder.
//   2. pg106_diag_probe_liveness(): a read-only first-contact sequence over a
//      caller-supplied rd32 function that classifies the GPU as
//      {absent, clock-gated, alive-unposted, alive-posted, alive-unverified}.
//      The already-POSTed evidence is register 0x02240c bit 1, the readback
//      nouveau's nvkm_devinit uses to decide whether devinit must run
//      (drivers/gpu/drm/nouveau/nvkm/subdev/devinit/gf100.c,
//      gf100_devinit_preinit; gm200.c wires the same function into every
//      GP10x chipset via gm200_devinit.preinit — see
//      research/evidence/POSTED-DETECTION.md).
//
// The probe only ever READS registers. On a GPU whose priv ring is in the
// fragile states pascal-egpu documented, writes can lock it up permanently
// (FINDINGS.md:120-122, 200-210) — the probe must stay read-only by design.

#ifndef PASCAL_DIAG_H
#define PASCAL_DIAG_H

#include <stdbool.h>
#include <stdint.h>

// Register offsets used by the probe. PTIMER: on GP106 silicon the TIME
// registers live at 0x9400/0x9410 (open-gpu-kernel-modules maxwell/gm107/
// dev_timer.h:26-27; measured on real GP106 by
// research/prior-art/pascal-egpu/hw/mmio.py:72-73). core/include/pascal/
// mmio.h maps the PTIMER block at 0x009000 accordingly (the pre-2026-11
// envytools 0x020000 mapping was NV50-era and wrong on GF100+;
// research/evidence/MMIO-MAP-CORRECTIONS.md).
#define PG106_DIAG_PTIMER_TIME_0	0x00009400u
#define PG106_DIAG_PTIMER_TIME_1	0x00009410u

// Devinit "already ran" latch, bits [31:0] at BAR0 0x02240c; bit 1 is the
// evidence bit (nouveau gf100.c gf100_devinit_preinit: `base->post =
// ((nvkm_rd32(device, 0x2240c) & BIT(1)) == 0);` — bit SET means devinit
// already ran and post is NOT needed; the bit clears on power-off/suspend).
#define PG106_DIAG_DEVINIT_DONE		0x0002240cu
#define PG106_DIAG_DEVINIT_DONE_BIT	0x00000002u

// Read-only liveness readbacks (recorded, never gated on):
// PBUS_BAR0_WINDOW (open-gpu-kernel-modules maxwell/gm107/dev_bus.h:43),
// PFB WPR2 address (offsets measured on real GP106,
// research/prior-art/pascal-egpu/hw/mmio.py:76-77; sticky value example
// FINDINGS.md:54), board straps (offset per pascal-egpu PLAN.md:149).
#define PG106_DIAG_PBUS_BAR0_WINDOW	0x00001700u
#define PG106_DIAG_WPR2_ADDR_LO		0x00100ce0u
#define PG106_DIAG_WPR2_ADDR_HI		0x00100ce4u
#define PG106_DIAG_PSTRAPS		0x00101000u

// Classification of one readback word.
enum pg106_diag_class {
	PG106_DIAG_OK = 0,	// no failure signature in the value
	PG106_DIAG_PRI_ERRCODE,	// 0xbad____: PRI error, code in bits [31:8]
	PG106_DIAG_SEC_REJECT,	// 0xdead*__: falcon security rejection
	PG106_DIAG_ALL_FF,	// 0xffffffff: absent device / dead station
	PG106_DIAG_ALL_ZERO,	// 0x00000000: unclocked or genuinely zero
};

struct pg106_diag_read {
	// Which failure-signature class the value falls into.
	enum pg106_diag_class cls;
	// value >> 8 for PRI_ERRCODE (the dev_pri_ringstation_sys.h
	// NV_PPRIV_SYS_PRI_ERROR_CODE field, bits [31:8]); undefined otherwise.
	uint32_t code;
	// Static strings with program lifetime; meaning is never NULL.
	const char *meaning;
	// Static citation string, e.g. "pascal-egpu FINDINGS.md:31" or
	// "open-gpu-kernel-modules gm107/dev_pri_ringstation_sys.h:29".
	const char *source;
};

// Decode a raw BAR0 readback. Pure function of (addr, value); no hardware
// access, no allocation. addr only refines the wording (e.g. PMC_BOOT_1
// legitimately reads 0 on a little-endian GPU, per pascal/pmc.h
// PG106_PMC_BOOT_1_LE).
struct pg106_diag_read pg106_diag_decode_readback(uint32_t addr, uint32_t value);

// Human-readable one-line diagnosis into buf (NUL-terminated). Returns the
// length written (excluding NUL); truncates at cap-1. Uses the mmio.h block
// map for context ("PMC", "PFB", ...) and "(unmapped)" outside all blocks.
int pg106_diag_format(char *buf, uint32_t cap, uint32_t addr, uint32_t value,
		      const struct pg106_diag_read *r);

// Falcon execution verdict, per the pascal-egpu UCODE_LEVEL latch warning
// (FINDINGS.md:164-180): CPUCTL HALTED (bit 4) after STARTCPU (bit 1) with
// TRACEPC stuck is a SECURITY EXCEPTION, not a successful run, while
// SCTL UCODE_LEVEL = 3. That latch is hardware-enforced and cannot be
// cleared from the host (SCTL writes silently dropped, FINDINGS.md:169-180).
enum pg106_diag_falcon_verdict {
	PG106_FALCON_HALTED = 0,	// HALTED without a pending STARTCPU
	PG106_FALCON_RUNNING,		// started, not (yet) halted
	PG106_FALCON_SECURITY_FAULT,	// STARTCPU stuck + HALTED: rejected
};

struct pg106_diag_falcon {
	enum pg106_diag_falcon_verdict verdict;
	// true when SCTL's UCODE_LEVEL field reads 3 (0x3000/0x3002 captures,
	// FINDINGS.md:59,68) — the latch that makes execution refusal final.
	bool ucode_level3_latched;
	// Static explanation string.
	const char *note;
};

// STARTCPU and HALTED are CPUCTL bits 1 and 4 (FINDINGS.md:57-58, 116:
// 0x10 = HALTED, 0x42 = ALIAS_EN|STARTCPU, 0x52 = HALTED|ALIAS_EN|STARTCPU).
#define PG106_FALCON_CPUCTL_STARTCPU	0x00000002u
#define PG106_FALCON_CPUCTL_HALTED	0x00000010u
// SCTL: bits [13:12] = UCODE_LEVEL (3 in every pascal-egpu capture), bit 1 =
// HSMODE (0x3002 capture, FINDINGS.md:68).
#define PG106_FALCON_SCTL_UCODE_LEVEL(s)	(((s) >> 12) & 0xfu)
#define PG106_FALCON_SCTL_HSMODE	0x00000002u

struct pg106_diag_falcon
pg106_diag_falcon_exec(uint32_t ctlctl, uint32_t sctl, uint32_t tracepc);

// Host-side read function for the liveness probe. Return the BAR0 word at
// addr (byte offset). May be a real MMIO reader or a synthetic bus in tests.
typedef uint32_t (*pg106_diag_rd32_fn)(void *ctx, uint32_t addr);

enum pg106_diag_state {
	// Identity OK and the 0x02240c bit-1 latch is SET: firmware already
	// ran devinit (Optimus system firmware at POST). Our devinit
	// interpreter must NOT run.
	PG106_DIAG_ALIVE_POSTED = 0,
	// Identity OK, latch CLEAR: a cold, un-POSTed GPU (the pascal-egpu
	// eGPU situation, FINDINGS.md:87). Only here may devinit run.
	PG106_DIAG_ALIVE_UNPOSTED,
	// Identity OK but the latch readback itself failed (0xbad*/ff):
	// cannot decide. Stop and collect logs; do not run devinit.
	PG106_DIAG_ALIVE_UNVERIFIED,
	// The PRI port answers with 0xbad* error codes instead of data
	// (clock-gated domains, FINDINGS.md:31).
	PG106_DIAG_CLOCK_GATED,
	// All-ones or undecodable identity: device not responding
	// (FINDINGS.md:202 whole-GPU 0xffffffff state) or not our chip.
	PG106_DIAG_ABSENT,
};

struct pg106_diag_liveness {
	enum pg106_diag_state state;
	uint32_t boot0;
	uint32_t boot1;
	bool boot0_ok;		// pg106_pmc_probe: GP106 identity decoded
	bool timer_ticked;	// PTIMER advanced between two samples
	uint32_t bus_window;	// PBUS_BAR0_WINDOW raw readback
	uint32_t devinit_done;	// 0x02240c raw readback
	bool devinit_latch_readable; // latch read returned no failure signature
	bool devinit_ran;	// latch bit set; meaningful only for ALIVE_POSTED
	uint32_t wpr2_lo;	// corroboration only, never gates a decision
	uint32_t wpr2_hi;
	uint32_t pstraps;
	// Rationale for state, NUL-terminated, for the kext log.
	char note[112];
};

// Read-only first-contact sequence (PMC identity, bus window, PTIMER tick,
// devinit latch, WPR2/strap record). Never writes a register. Deterministic
// for a given bus state.
struct pg106_diag_liveness
pg106_diag_probe_liveness(void *ctx, pg106_diag_rd32_fn rd32);

// "ALIVE_POSTED", "CLOCK_GATED", ... for logs. Static string, never NULL.
const char *pg106_diag_state_name(enum pg106_diag_state state);

#endif // PASCAL_DIAG_H
