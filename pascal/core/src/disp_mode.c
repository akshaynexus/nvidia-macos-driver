// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/src/disp_mode.c — Pascal display-engine mode-set planner.
//
// SIMULATION ONLY: builds a paper plan of EVO-method / BAR0 writes for one
// 1080p HDMI head on a GP106 (GP102 disp glue). See disp_mode.h for the full
// provenance list; every step's `source` field names file + function.
//
// The timing arithmetic below is nouveau's own, verbatim from
// nv50-head.c nv50_head_atomic_check_mode:
//   m->h.active = mode->crtc_htotal;
//   m->h.synce  = mode->crtc_hsync_end - mode->crtc_hsync_start - 1;
//   m->h.blanke = mode->crtc_hblank_end - mode->crtc_hsync_start - 1;
//   m->h.blanks = m->h.blanke + mode->crtc_hdisplay;
// (same for v). drm_mode_set_crtcinfo sets crtc_hblank_end == crtc_htotal for
// a progressive mode, so hblank_end/vblank_end are the totals here. The
// comment in nv50-head.c explains why the "- 1" bias exists: the hardware
// raster counter repeats over an interval starting one unit into the sync
// pulse, not at the start of the active area.

#include <pascal/disp_mode.h>

// cl907d.h field encodings used below.
#define PG106_SOR_PROTO_SINGLE_TMDS_A 0x00000001u // cl907d.h:91 (decode: gf119_sor_state case 1 -> TMDS link 1)
#define PG106_SOR_OWNER_NONE          0x00000000u // cl907d.h:85
#define PG106_SOR_OWNER_HEAD0         0x00000001u // cl907d.h:86

#define PG106_PIXDEPTH_BPP_18_444 0x00000002u // cl907d.h:120 (readback: gf119.c gf119_head_state case 2 -> depth 18)
#define PG106_PIXDEPTH_BPP_24_444 0x00000005u // cl907d.h:123 (case 5 -> depth 24)
#define PG106_PIXDEPTH_BPP_30_444 0x00000006u // cl907d.h:124 (case 6 -> depth 30)

#define PG106_PCLOCK_MODE_CLK_CUSTOM 0x00000002u // cl907d.h:281, at bits 21:20

// head907d_or writes HEAD_SET_CONTROL = 0x31ec6000 | head << 25 | STRUCTURE
// (nv50-head907d.c head907d_or); the 0x31EC6000 base is verbatim nouveau.
#define PG106_HEAD_CONTROL_BASE 0x31EC6000u

// gf119_sor_clock: TMDS divider speed 0x14 when tmds.high_speed else 0x0a,
// written at bits 22:18 of 0x612300 + soff. high_speed is a >165 MHz TMDS
// clock property; 148.5 MHz stays single-link low speed.
#define PG106_TMDS_SPEED_LOW  0x0000000Au
#define PG106_TMDS_SPEED_HIGH 0x00000014u
#define PG106_TMDS_HIGH_SPEED_KHZ_LIMIT 165000u

static uint32_t
pixel_depth_field(uint8_t bpc)
{
	switch (bpc) {
	case 18: return PG106_PIXDEPTH_BPP_18_444;
	case 24: return PG106_PIXDEPTH_BPP_24_444;
	case 30: return PG106_PIXDEPTH_BPP_30_444;
	default: return PG106_PIXDEPTH_BPP_24_444;
	}
}

const char *
pg106_disp_kind_str(uint8_t kind)
{
	switch (kind) {
	case PG106_EVO_MTHD:  return "evo-method";
	case PG106_MMIO_BAR0: return "mmio-bar0";
	case PG106_BLOCK:     return "block";
	default:              return "?";
	}
}

size_t
pg106_disp_modeset_plan(const struct pg106_disp_mode *mode,
			struct pg106_disp_step *out, size_t max)
{
	// nouveau conversions (nv50-head.c nv50_head_atomic_check_mode).
	const uint32_t htotal = mode->htotal, vtotal = mode->vtotal;
	const uint32_t hsynce = mode->hsync_end - mode->hsync_start - 1;
	const uint32_t hblanke = htotal - mode->hsync_start - 1;
	const uint32_t hblanks = hblanke + mode->hdisplay;
	const uint32_t vsynce = mode->vsync_end - mode->vsync_start - 1;
	const uint32_t vblanke = vtotal - mode->vsync_start - 1;
	const uint32_t vblanks = vblanke + mode->vdisplay;

	const uint32_t khz_hz = mode->pixel_clock_khz * 1000u;
	const uint32_t head = mode->head, sor = mode->sor;
	const uint32_t tmds_speed =
		mode->pixel_clock_khz > PG106_TMDS_HIGH_SPEED_KHZ_LIMIT ?
			PG106_TMDS_SPEED_HIGH : PG106_TMDS_SPEED_LOW;
	const uint32_t pixdepth = pixel_depth_field(mode->bpc);
	size_t i = 0;

	if (max < PG106_DISP_STEPS_MAX)
		return 0;
	if (mode->hsync_end <= mode->hsync_start ||
	    mode->vsync_end <= mode->vsync_start)
		return 0;

#define EMIT(nam, knd, rg, val, cite) do {                              \
	out[i].name = (nam); out[i].kind = (knd);                       \
	out[i].reg = (rg);   out[i].value = (uint32_t)(val);            \
	out[i].source = (cite); i++;                                    \
} while (0)

	// ---- Phase 1: head disable (must precede any reprogramming) ----
	// Unhook the SOR from the head: owner mask NONE, protocol kept.
	// SOR_SET_CONTROL is written by the core channel (nv50-sor907d.c
	// sor907d_ctrl); gf119_sor_state decodes OWNER_MASK at bits 3:0 and
	// PROTOCOL at bits 11:8.
	EMIT("sor0.owner-none", PG106_EVO_MTHD,
	     PG106_NV907D_SOR_SET_CONTROL(sor),
	     PG106_SOR_OWNER_NONE | (PG106_SOR_PROTO_SINGLE_TMDS_A << 8),
	     "nvkm/engine/disp/nv50-sor907d.c sor907d_ctrl + nvhw/class/cl907d.h NV907D_SOR_SET_CONTROL_* (decode: nvkm/engine/disp/gf119.c gf119_sor_state)");
	// Power down the SOR: nv50_sor_power(sor, normal=true, pu=false, ...)
	// writes state = 0x80000000 | (1*!!pu) << shift at 0x61C004 + soff.
	EMIT("sor0.power-down", PG106_MMIO_BAR0, PG106_SOR_POWER(sor),
	     0x80000000u,
	     "nvkm/engine/disp/nv50.c nv50_sor_power (wired as gf119_sor.power in nvkm/engine/disp/gf119.c gf119_sor)");
	// Latch the disable with an UPDATE carrying the base-channel interlock
	// (core507d_update emits NV507D UPDATE = interlock[BASE] | interlock[OVLY]).
	EMIT("core.update-disable", PG106_EVO_MTHD, PG106_NV907D_UPDATE,
	     0x1u << 1, // NV507D_UPDATE_INTERLOCK_WITH_BASE0_ENABLE, cl507d.h:49
	     "nvkm/engine/disp/nv50-core507d.c core507d_update + nvhw/class/cl507d.h NV507D_UPDATE_INTERLOCK_WITH_BASE0");

	// ---- Phase 2: raster/timing setup ----
	// VPLL: supervisor step 2.1 programs PLL_VPLL0 + head->id to
	// head->asy.hz / 1000 (kHz) before the IOR is clocked.
	EMIT("vpll0.clock", PG106_BLOCK, PG106_BLOCK_VPLL0,
	     mode->pixel_clock_khz,
	     "nvkm/engine/disp/nv50.c nv50_disp_super_2_1 (invoked from nvkm/engine/disp/gf119.c gf119_disp_super, super pending bit 2)");
	// RASTER_SIZE = HEIGHT(active) << 16 | WIDTH(active); head907d_mode
	// writes m->v.active / m->h.active into fields 30:16 / 14:0
	// (cl907d.h NV907D_HEAD_SET_RASTER_SIZE_{HEIGHT,WIDTH}). The 0x640414
	// readback in gf119_head_state is the runtime state view, not the
	// method's semantics.
	EMIT("head0.raster-size", PG106_EVO_MTHD,
	     PG106_NV907D_HEAD_SET_RASTER_SIZE(head),
	     (mode->vdisplay << 16) | mode->hdisplay,
	     "nvkm/engine/disp/nv50-head907d.c head907d_mode (HEAD_SET_RASTER_SIZE = h.active | v.active << 16) + nvhw/class/cl907d.h NV907D_HEAD_SET_RASTER_SIZE_WIDTH 14:0 / _HEIGHT 30:16");
	// SYNC_END = VSYNCE << 16 | HSYNCE (readback 0x640418, gf119.c).
	EMIT("head0.raster-sync-end", PG106_EVO_MTHD,
	     PG106_NV907D_HEAD_SET_RASTER_SYNC_END(head), (vsynce << 16) | hsynce,
	     "nvkm/engine/disp/nv50-head907d.c head907d_mode (HEAD_SET_RASTER_SYNC_END); readback: nvkm/engine/disp/gf119.c gf119_head_state (0x640418)");
	// BLANK_END = VBLANKE << 16 | HBLANKE (readback 0x64041c, gf119.c).
	EMIT("head0.raster-blank-end", PG106_EVO_MTHD,
	     PG106_NV907D_HEAD_SET_RASTER_BLANK_END(head), (vblanke << 16) | hblanke,
	     "nvkm/engine/disp/nv50-head907d.c head907d_mode (HEAD_SET_RASTER_BLANK_END); readback: nvkm/engine/disp/gf119.c gf119_head_state (0x64041c)");
	// BLANK_START = VBLANKS << 16 | HBLANKS (readback 0x640420, gf119.c).
	EMIT("head0.raster-blank-start", PG106_EVO_MTHD,
	     PG106_NV907D_HEAD_SET_RASTER_BLANK_START(head), (vblanks << 16) | hblanks,
	     "nvkm/engine/disp/nv50-head907d.c head907d_mode (HEAD_SET_RASTER_BLANK_START); readback: nvkm/engine/disp/gf119.c gf119_head_state (0x640420)");
	// VERT_BLANK2 is for interlaced sub-fields only; progressive -> 0
	// (head907d_mode writes m->v.blank2s/blank2e, 0 in the atomic check
	// when !interlace).
	EMIT("head0.raster-vert-blank2", PG106_EVO_MTHD,
	     PG106_NV907D_HEAD_SET_RASTER_VERT_BLANK2(head), 0u,
	     "nvkm/engine/disp/nv50-head907d.c head907d_mode (HEAD_SET_RASTER_VERT_BLANK2; blank2s/blank2e are 0 for progressive, nv50-head.c nv50_head_atomic_check_mode)");
	// PIXEL_CLOCK_FREQUENCY = HERTZ (m->clock * 1000); the same register
	// 0x640450 is what gf119_head_state reads back into state->hz.
	EMIT("head0.pixel-clock-frequency", PG106_EVO_MTHD,
	     PG106_NV907D_HEAD_SET_PIXEL_CLOCK_FREQUENCY(head), khz_hz,
	     "nvkm/engine/disp/nv50-head907d.c head907d_mode (HEAD_SET_PIXEL_CLOCK_FREQUENCY); readback: nvkm/engine/disp/gf119.c gf119_head_state (state->hz from 0x640450)");
	// PIXEL_CLOCK_CONFIGURATION: MODE CLK_CUSTOM, NOT_DRIVER FALSE,
	// ENABLE_HOPPING FALSE (head907d_mode).
	EMIT("head0.pixel-clock-configuration", PG106_EVO_MTHD,
	     PG106_NV907D_HEAD_SET_PIXEL_CLOCK_CONFIGURATION(head),
	     PG106_PCLOCK_MODE_CLK_CUSTOM << 20,
	     "nvkm/engine/disp/nv50-head907d.c head907d_mode + nvhw/class/cl907d.h NV907D_HEAD_SET_PIXEL_CLOCK_CONFIGURATION_MODE_CLK_CUSTOM");
	EMIT("head0.pixel-clock-frequency-max", PG106_EVO_MTHD,
	     PG106_NV907D_HEAD_SET_PIXEL_CLOCK_FREQUENCY_MAX(head), khz_hz,
	     "nvkm/engine/disp/nv50-head907d.c head907d_mode (HEAD_SET_PIXEL_CLOCK_FREQUENCY_MAX)");

	// ---- Phase 3: head control (depth, sync polarity, structure) ----
	// OUTPUT_RESOURCE: CRC_MODE ACTIVE_RASTER(0), HSYNC/VSYNC polarity
	// (POSITIVE_TRUE = 0), PIXEL_DEPTH at bits 9:6 (head907d_or). The
	// depth field is what gf119_head_state decodes back from 0x640404
	// bits 9:6 (case 5 -> 24bpp).
	EMIT("head0.output-resource", PG106_EVO_MTHD,
	     PG106_NV907D_HEAD_SET_CONTROL_OUTPUT_RESOURCE(head),
	     (uint32_t)(pixdepth << 6) |
	     (mode->hsync_positive ? 0u : 1u << 3) |
	     (mode->vsync_positive ? 0u : 1u << 4),
	     "nvkm/engine/disp/nv50-head907d.c head907d_or (HEAD_SET_CONTROL_OUTPUT_RESOURCE) + nvhw/class/cl907d.h PIXEL_DEPTH/HSYNC_POLARITY/VSYNC_POLARITY; readback: nvkm/engine/disp/gf119.c gf119_head_state (or.depth from 0x640404 bits 9:6)");
	// HEAD_SET_CONTROL = 0x31ec6000 | head << 25 | STRUCTURE (0 =
	// progressive).
	EMIT("head0.control", PG106_EVO_MTHD, PG106_NV907D_HEAD_SET_CONTROL(head),
	     PG106_HEAD_CONTROL_BASE | ((uint32_t)head << 25) |
	     (mode->interlace ? 1u : 0u),
	     "nvkm/engine/disp/nv50-head907d.c head907d_or (HEAD_SET_CONTROL 0x31ec6000 | index << 25 | STRUCTURE) + nvhw/class/cl907d.h NV907D_HEAD_SET_CONTROL_STRUCTURE");

	// ---- Phase 4: SOR/IOR config (HDMI over TMDS, gf119 routing) ----
	// Owner assignment: SOR 0 -> head 0, single-link TMDS (proto 1; the
	// mirror is gf119_sor_state: proto_evo 1 -> TMDS, link 1, head bits
	// 3:0). Written via the core channel by sor907d_ctrl.
	EMIT("sor0.owner", PG106_EVO_MTHD, PG106_NV907D_SOR_SET_CONTROL(sor),
	     PG106_SOR_OWNER_HEAD0 + (PG106_SOR_PROTO_SINGLE_TMDS_A << 8),
	     "nvkm/engine/disp/nv50-sor907d.c sor907d_ctrl + nvhw/class/cl907d.h NV907D_SOR_SET_CONTROL_OWNER_MASK_HEAD0/PROTOCOL_SINGLE_TMDS_A (routing decode: nvkm/engine/disp/gf119.c gf119_sor_state; supervisor: nvkm/engine/disp/nv50.c nv50_disp_super_2_2)");
	// RG clock divider: gf119_head_rgclk(head, rgdiv) masks 0xf at
	// 0x612200 + head*0x800. rgdiv is 1 for TMDS (super_2_2 calls
	// head->func->rgclk with ior->asy.rgdiv).
	EMIT("head0.rgclk", PG106_MMIO_BAR0, PG106_HEAD_RGCLK(head), 1u,
	     "nvkm/engine/disp/gf119.c gf119_head_rgclk (called from nvkm/engine/disp/nv50.c nv50_disp_super_2_2)");
	// SOR clock: TMDS speed 0x0a (148.5 MHz < 165 MHz -> not high_speed)
	// at bits 22:18, link dividers 0 (link != 3) at 0x00000707.
	EMIT("sor0.clock", PG106_MMIO_BAR0, PG106_SOR_CLOCK(sor),
	     tmds_speed << 18,
	     "nvkm/engine/disp/gf119.c gf119_sor_clock (invoked from nvkm/engine/disp/nv50.c nv50_disp_super_2_2 via ior->func->clock)");
	// HDMI block: gf119_sor_hdmi_ctrl gates the infoframe path, writes
	// 0x10 to 0x6167AC, then sets HDMI_CTRL = 0x40000000 (enable) |
	// max_ac_packet << 16 | rekey.
	EMIT("sor0.hdmi-infoframe-gate-off", PG106_MMIO_BAR0,
	     PG106_HEAD_HDMI_UNK_A4(mode->head), 0u,
	     "nvkm/engine/disp/gf119.c gf119_sor_hdmi_ctrl (mask 0x6167A4 bit0 -> 0)");
	EMIT("sor0.hdmi-infoframe-unk10", PG106_MMIO_BAR0,
	     PG106_HEAD_HDMI_UNK_AC(mode->head), 0x00000010u,
	     "nvkm/engine/disp/gf119.c gf119_sor_hdmi_ctrl (wr 0x00000010 to 0x6167AC, \"??? InfoFrame?\")");
	EMIT("sor0.hdmi-infoframe-gate-on", PG106_MMIO_BAR0,
	     PG106_HEAD_HDMI_UNK_A4(mode->head), 1u,
	     "nvkm/engine/disp/gf119.c gf119_sor_hdmi_ctrl (mask 0x6167A4 bit0 -> 1)");
	EMIT("sor0.hdmi-ctrl", PG106_MMIO_BAR0, PG106_HEAD_HDMI_CTRL(mode->head),
	     0x40000000u | ((uint32_t)mode->hdmi_max_ac_packet << 16) |
	     mode->hdmi_rekey,
	     "nvkm/engine/disp/gf119.c gf119_sor_hdmi_ctrl (HDMI_CTRL at 0x616798, mask 0x401f007f)");
	// Power the SOR back up: nv50_sor_power with pu=true -> 0x80000001,
	// then waits on 0x61C030 bit 28 (wait not modeled).
	EMIT("sor0.power-on", PG106_MMIO_BAR0, PG106_SOR_POWER(sor),
	     0x80000001u,
	     "nvkm/engine/disp/nv50.c nv50_sor_power (wired as gf119_sor.power in nvkm/engine/disp/gf119.c gf119_sor)");

	// ---- Phase 5: head enable (always last) ----
	EMIT("core.update-enable", PG106_EVO_MTHD, PG106_NV907D_UPDATE,
	     0x1u << 1,
	     "nvkm/engine/disp/nv50-core507d.c core507d_update + nvhw/class/cl507d.h NV507D_UPDATE_INTERLOCK_WITH_BASE0");

#undef EMIT
	return i;
}

void
pg106_disp_modeset_apply(const struct pg106_disp_step *steps, size_t n,
			 pg106_disp_bus_write_fn fn, void *user)
{
	for (size_t i = 0; i < n; i++)
		fn(user, &steps[i]);
}

int
pg106_disp_modeset_diff(const struct pg106_disp_step *a, size_t na,
			const struct pg106_disp_step *b, size_t nb,
			size_t *first_diff)
{
	if (first_diff)
		*first_diff = 0;
	if (na != nb) {
		if (first_diff)
			*first_diff = na < nb ? na : nb;
		return -1;
	}
	for (size_t i = 0; i < na; i++) {
		if (a[i].name != b[i].name || a[i].kind != b[i].kind ||
		    a[i].reg != b[i].reg || a[i].value != b[i].value) {
			if (first_diff)
				*first_diff = i;
			return -1;
		}
	}
	return 0;
}
