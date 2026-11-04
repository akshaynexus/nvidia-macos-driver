// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/disp_mode.h — Pascal (GF119-family) display-engine
// mode-set sequence planner for one HDMI head.
//
// SIMULATION ONLY. This module *plans* an ordered register/method sequence on
// paper (and over a mock bus in the test); it never touches hardware. Every
// emitted step carries a nouveau source citation. This is not a hardware
// proof (HANDOFF-PASCAL.md §7 evidence rules).
//
// Provenance: derived from nouveau, linux master, fetched to /tmp/pascal-ref/disp/:
//   nvkm/engine/disp/gf119.c       CPU-driven display base: head/SOR readback
//                                  registers, rgclk, SOR clock, HDMI ctrl,
//                                  supervisor (owner assignment).
//   nvkm/engine/disp/nv50-head907d.c  head907d_mode/head907d_or — the EVO
//                                  pushbuffer methods that program head timing,
//                                  depth and sync (the write side of the
//                                  registers gf119_head_state reads back).
//   nvkm/engine/disp/nv50-sor907d.c   sor907d_ctrl — SOR_SET_CONTROL (owner +
//                                  protocol) method.
//   nvkm/engine/disp/nv50.c        nv50_disp_super_2_1/2_2 — supervisor: VPLL
//                                  clock, rgclk and IOR clock invocation.
//   nvkm/engine/disp/nv50-core507d.c  core507d_update — UPDATE method that
//                                  latches a batch of head/IOR state.
//   nvkm/engine/disp/gp102.c       GP106 disp glue: GP102_DISP core channel,
//                                  gf119 head/SOR count+new, gf119_disp_init.
//   nvhw/class/cl907d.h, cl507d.h  method/register offsets and field encodings.
//
// Key structural fact (verified across those files): on GF119+ the EVO core
// channel methods ARE the PDISP head registers. cl907d.h gives
// NV907D_HEAD_SET_RASTER_SIZE(a) = 0x0414 + a*0x300, and gf119_head_state
// reads state back from 0x640414 + head_id*0x300 (gf119.c). Same for the SOR:
// NV907D_SOR_SET_CONTROL(a) = 0x0200 + a*0x20 vs 0x640200 + sor_id*0x20
// (gf119_sor_state). We therefore model each step as a write to the method/
// register address directly, which is exact for the readback path and the
// hardware's latched view of the pushbuffer method.

#ifndef PASCAL_DISP_MODE_H
#define PASCAL_DISP_MODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Modeled on a GP106 (GP102 disp): head count comes from gf119_head_cnt
// (gf119.c: (0x612004 & 0xf0) >> 4 -> 4 heads); we drive head 0 through SOR 0
// (HDMI connector), the only wiring that exists on the Helios 300 (research
// doc §2: HDMI/miniDP are wired to the dGPU; internal panel is on the iGPU).
#define PG106_DISP_HEAD 0u
#define PG106_DISP_SOR  0u

// EVO core-channel method addresses == PDISP head/SOR register offsets
// (nvhw/class/cl907d.h, head stride 0x300, SOR stride 0x20; head/SOR 0).
#define PG106_NV907D_UPDATE                       0x00000080u // cl507d.h:41
#define PG106_NV907D_SOR_SET_CONTROL(sor)         (0x00000200u + (sor) * 0x20u) // cl907d.h:83
#define PG106_NV907D_HEAD_SET_CONTROL_OUTPUT_RESOURCE(head) \
	(0x00000404u + (head) * 0x300u) // cl907d.h:106
#define PG106_NV907D_HEAD_SET_CONTROL(head)       (0x00000408u + (head) * 0x300u) // cl907d.h:128
#define PG106_NV907D_HEAD_SET_RASTER_SIZE(head)   (0x00000414u + (head) * 0x300u) // cl907d.h:136
#define PG106_NV907D_HEAD_SET_RASTER_SYNC_END(head)  (0x00000418u + (head) * 0x300u) // cl907d.h:139
#define PG106_NV907D_HEAD_SET_RASTER_BLANK_END(head) (0x0000041Cu + (head) * 0x300u) // cl907d.h:142
#define PG106_NV907D_HEAD_SET_RASTER_BLANK_START(head) \
	(0x00000420u + (head) * 0x300u) // cl907d.h:145
#define PG106_NV907D_HEAD_SET_RASTER_VERT_BLANK2(head) \
	(0x00000424u + (head) * 0x300u) // cl907d.h:148
#define PG106_NV907D_HEAD_SET_PIXEL_CLOCK_FREQUENCY(head) \
	(0x00000450u + (head) * 0x300u) // cl907d.h:272
#define PG106_NV907D_HEAD_SET_PIXEL_CLOCK_CONFIGURATION(head) \
	(0x00000454u + (head) * 0x300u) // cl907d.h:277
#define PG106_NV907D_HEAD_SET_PIXEL_CLOCK_FREQUENCY_MAX(head) \
	(0x00000458u + (head) * 0x300u) // cl907d.h:291

// BAR0 (PDISP block) registers programmed outside the EVO method stream.
#define PG106_HEAD_RGCLK(head)     (0x612200u + (head) * 0x800u) // gf119.c gf119_head_rgclk
#define PG106_SOR_CLOCK(sor)       (0x612300u + (sor) * 0x800u)  // gf119.c gf119_sor_clock
#define PG106_SOR_POWER(sor)       (0x61C004u + (sor) * 0x800u)  // nv50.c nv50_sor_power
// The gf119+ HDMI block is indexed by HEAD, not SOR: gf119_sor_hdmi_ctrl
// computes hoff = head * 0x800 (gf119.c:259).
#define PG106_HEAD_HDMI_CTRL(head)   (0x616798u + (head) * 0x800u) // gf119.c gf119_sor_hdmi_ctrl (HDMI_CTRL)
#define PG106_HEAD_HDMI_UNK_A4(head) (0x6167A4u + (head) * 0x800u) // gf119.c gf119_sor_hdmi_ctrl (infoframe gate)
#define PG106_HEAD_HDMI_UNK_AC(head) (0x6167ACu + (head) * 0x800u) // gf119.c gf119_sor_hdmi_ctrl ("??? InfoFrame?")

// Pseudo-block identifiers for operations that are not a single register
// write (modeled only; the real path goes through nvkm_devinit PLL code).
#define PG106_BLOCK_VPLL0 0x00000000u // nv50.c nv50_disp_super_2_1 (PLL_VPLL0 + head->id)

enum pg106_disp_kind {
	PG106_EVO_MTHD = 0, // write into the EVO core-channel method space
	PG106_MMIO_BAR0,    // direct BAR0/PDISP register write
	PG106_BLOCK,        // pseudo-block op (VPLL), not MMIO
};

// One ordered step of the mode-set sequence.
struct pg106_disp_step {
	const char *name;   // stable identifier, e.g. "head0.raster-size"
	uint8_t kind;       // enum pg106_disp_kind
	uint32_t reg;       // method offset (EVO), BAR0 offset (MMIO), block id
	uint32_t value;     // full 32-bit word to write
	const char *source; // nouveau citation: file + function, never NULL
};

// A mode in DRM/CEA terms (absolute hsync/hblank values). Fill it from a real
// EDID DTD; the sibling parser pascal/core/src/edid.c extracts exactly these
// fields from a CTA-861 detailed timing descriptor.
struct pg106_disp_mode {
	uint32_t pixel_clock_khz;
	uint16_t hdisplay, hsync_start, hsync_end, htotal;
	uint16_t vdisplay, vsync_start, vsync_end, vtotal;
	uint8_t bpc; // 18, 24 or 30 (BPP_18_444 / BPP_24_444 / BPP_30_444)
	uint8_t head; // head id (0..3)
	uint8_t sor;  // SOR id (0..3)
	uint8_t hdmi_max_ac_packet; // gf119_sor_hdmi_ctrl max_ac_packet
	uint8_t hdmi_rekey;         // gf119_sor_hdmi_ctrl rekey
	bool interlace;
	bool hsync_positive, vsync_positive; // CEA-861 flags
};

// Upper bound on steps emitted by pg106_disp_modeset_plan().
#define PG106_DISP_STEPS_MAX 24

// Build the ordered mode-set sequence for one head. Returns the step count,
// or 0 if `max` is too small or mode is invalid. Deterministic: two calls
// with the same mode produce byte-identical plans (diff gate in the test).
//
// Sequence shape (each phase cited in the emitted steps):
//   1. disable: SOR owner -> NONE, SOR power down, UPDATE to latch
//   2. setup:   VPLL, raster/timing, pixel clock
//   3. control: output resource (depth/sync polarity), head control
//   4. IOR:     SOR owner/protocol, rgclk, SOR clock, HDMI ctrl, SOR power up
//   5. enable:  final UPDATE (always the last step)
size_t pg106_disp_modeset_plan(const struct pg106_disp_mode *mode,
			       struct pg106_disp_step *out, size_t max);

// Replay the plan over a caller-supplied bus. SIMULATION: `fn` is expected to
// be a recorder in the tests, not real MMIO.
typedef void (*pg106_disp_bus_write_fn)(void *user,
					const struct pg106_disp_step *step);
void pg106_disp_modeset_apply(const struct pg106_disp_step *steps, size_t n,
			      pg106_disp_bus_write_fn fn, void *user);

// Deterministic diff gate. Returns 0 if both plans are identical
// (count, name, kind, reg, value — citations are informational), or -1 with
// *first_diff set to the index of the first difference.
int pg106_disp_modeset_diff(const struct pg106_disp_step *a, size_t na,
			    const struct pg106_disp_step *b, size_t nb,
			    size_t *first_diff);

const char *pg106_disp_kind_str(uint8_t kind);

#endif // PASCAL_DISP_MODE_H
