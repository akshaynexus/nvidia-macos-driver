// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/disp_mode.h — GP102-class (core917d/head917d)
// mode set as nouveau builds it: DRM mode -> head timing, then the CORE
// CHANNEL PUSHBUFFER (method stream) plus the BASE channel image update.
//
// SIMULATION ONLY. Produces method streams and register traces; nothing
// here has been executed by a GPU.
//
// What changed vs the earlier plan (and why): the earlier module wrote
// cl907d method offsets to BAR0 0x640000+ as if EVO methods were PDISP
// registers. On GP102 the core channel is a DMA channel
// (gp102_disp_core_init, nvkm/engine/disp/gp102.c:102-127: pushbuffer
// address at 0x611494, PUT at 0x640000); methods only execute when the
// channel FETCHES them from the pushbuffer after a PUT. 0x640000+ is the
// USER/PUT page and the ARMED-state mirror (gf119_head_state reads ARM at
// 0x640414 and ASY at 0x660414, gf119.c:423-464) — a BAR0 write there runs
// nothing. This module therefore emits the pushbuffer words nouveau's
// PUSH_MTHD would (include/nvif/push507c.h: header = count << 18 |
// method, NV507C_DMA_METHOD_COUNT 27:18 / OFFSET 11:2), and disp_core.h
// models the BAR0 side (channel init, PUT kick, supervisor).
//
// Upstream (linux master, /tmp/review-ref/linux d48723711bae):
//   dispnv50/core.c:46-56            GP102_DISP_CORE_CHANNEL_DMA ->
//                                    core917d_new (head917d, sor907d)
//   dispnv50/head.c:283-326          nv50_head_atomic_check_mode
//   dispnv50/head.c:69-82            nv50_head_flush_set order
//   dispnv50/head907d.c:322-410      head907d_mode / head907d_view
//   dispnv50/head907d.c:36-75,209-244 head907d_or / procamp / core_set
//   dispnv50/head917d.c:30-75        head917d_dither / head917d_base
//   dispnv50/head907d.c:95-122       head907d_ovly
//   dispnv50/head507d.c:247-276      head507d_core_calc
//   dispnv50/head.c:97-124           nv50_head_atomic_check_dither
//   nouveau_connector.c:264-269      connector defaults (dither AUTO,
//                                    scaler NONE, underscan OFF,
//                                    vibrance 150, hue 90)
//   dispnv50/sor907d.c:31-43         sor907d_ctrl (SOR_SET_CONTROL)
//   dispnv50/disp.c:1539-1556        nv50_sor_update (ctrl |= proto<<8 |
//                                    BIT(head); disable clears to 0 when
//                                    no owner is left)
//   dispnv50/disp.c:1743-1838        nv50_sor_atomic_enable: TMDS proto
//                                    SINGLE_TMDS_A/B, depth DEFAULT
//   dispnv50/disp.c:777-850          nv50_hdmi_enable: rekey 56,
//                                    max_ac_packet = (htotal - hdisplay
//                                    - 56 - 18) / 32
//   dispnv50/disp.c:2168-2340        nv50_disp_atomic_commit_tail order
//   dispnv50/core507d.c:34-59,131-141 core507d_update / core507d_init
//   dispnv50/base907c.c:29-61        base907c_image_set
//   dispnv50/base507c.c:36-46        base507c_update
//   dispnv50/base917c.c:49           base interlock 0x2 << (head * 4)
//   drm_modes.c:1348-1408            drm_mode_set_crtcinfo

#ifndef PASCAL_DISP_MODE_H
#define PASCAL_DISP_MODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- core917d methods (open-gpu-kernel-modules
// src/common/sdk/nvidia/inc/class/cl917d.h; head stride 0x300) -------------
#define PG106_917D_UPDATE                   0x0080u
#define PG106_917D_SET_NOTIFIER_CONTROL     0x0084u
#define PG106_917D_SET_CONTEXT_DMA_NOTIFIER 0x0088u
#define PG106_917D_SOR_SET_CONTROL(s)       (0x0200u + (s) * 0x20u)
#define PG106_917D_HEAD(h, m)               ((m) + (h) * 0x300u)
#define PG106_917D_HEAD_SET_CONTROL_OUTPUT_RESOURCE 0x0404u
#define PG106_917D_HEAD_SET_CONTROL         0x0408u
#define PG106_917D_HEAD_SET_OVERSCAN_COLOR  0x0410u
#define PG106_917D_HEAD_SET_RASTER_SIZE     0x0414u
#define PG106_917D_HEAD_SET_RASTER_SYNC_END 0x0418u
#define PG106_917D_HEAD_SET_RASTER_BLANK_END 0x041cu
#define PG106_917D_HEAD_SET_RASTER_BLANK_START 0x0420u
#define PG106_917D_HEAD_SET_RASTER_VERT_BLANK2 0x0424u
#define PG106_917D_HEAD_SET_DEFAULT_BASE_COLOR 0x042cu
#define PG106_917D_HEAD_SET_PIXEL_CLOCK_FREQUENCY 0x0450u
#define PG106_917D_HEAD_SET_PIXEL_CLOCK_CONFIGURATION 0x0454u
#define PG106_917D_HEAD_SET_PIXEL_CLOCK_FREQUENCY_MAX 0x0458u
#define PG106_917D_HEAD_SET_OFFSET          0x0460u
#define PG106_917D_HEAD_SET_SIZE            0x0468u
#define PG106_917D_HEAD_SET_STORAGE         0x046cu
#define PG106_917D_HEAD_SET_PARAMS          0x0470u
#define PG106_917D_HEAD_SET_CONTEXT_DMAS_ISO 0x0474u
#define PG106_917D_HEAD_SET_CONTROL_OUTPUT_SCALER 0x0494u
#define PG106_917D_HEAD_SET_PROCAMP         0x0498u
#define PG106_917D_HEAD_SET_DITHER_CONTROL  0x04a0u
#define PG106_917D_HEAD_SET_VIEWPORT_POINT_IN 0x04b0u
#define PG106_917D_HEAD_SET_VIEWPORT_SIZE_IN 0x04b8u
#define PG106_917D_HEAD_SET_VIEWPORT_SIZE_OUT 0x04c0u
#define PG106_917D_HEAD_SET_VIEWPORT_SIZE_OUT_MIN 0x04c4u
#define PG106_917D_HEAD_SET_VIEWPORT_SIZE_OUT_MAX 0x04c8u
#define PG106_917D_HEAD_SET_BASE_CHANNEL_USAGE_BOUNDS 0x04d0u
#define PG106_917D_HEAD_SET_OVERLAY_USAGE_BOUNDS 0x04d4u

// SOR_SET_CONTROL protocols (cl917d.h).
#define PG106_SOR_PROTO_SINGLE_TMDS_A 0x1u
#define PG106_SOR_PROTO_SINGLE_TMDS_B 0x2u
#define PG106_SOR_PROTO_DUAL_TMDS     0x5u
#define PG106_SOR_PROTO_DP_A          0x8u
#define PG106_SOR_PROTO_DP_B          0x9u
#define PG106_SOR_PROTO_CUSTOM        0xfu

// --- base907c methods (nouveau include/nvhw/class/cl907c.h) ---------------
#define PG106_907C_UPDATE                0x0080u
#define PG106_907C_SET_PRESENT_CONTROL   0x0084u
#define PG106_907C_SET_CONTEXT_DMAS_ISO0 0x00c0u
#define PG106_907C_SURFACE_SET_OFFSET0   0x0400u
#define PG106_907C_SURFACE_SET_SIZE0     0x0408u
#define PG106_907C_SURFACE_SET_STORAGE0  0x040cu
#define PG106_907C_SURFACE_SET_PARAMS0   0x0410u

// HDMI constants from nv50_hdmi_enable (disp.c:785).
#define PG106_HDMI_REKEY 56u

// A mode in DRM terms (absolute sync/total values).
struct pg106_disp_mode {
	uint32_t pixel_clock_khz;
	uint16_t hdisplay, hsync_start, hsync_end, htotal;
	uint16_t vdisplay, vsync_start, vsync_end, vtotal;
	bool interlace;
	bool hsync_positive, vsync_positive; // !NHSYNC / !NVSYNC
	uint8_t bpc;  // asyh->or.bpc (bits per component: 6/8/10)
	uint8_t head; // head index
};

// struct nv50_head_mode (dispnv50/atom.h) as nv50_head_atomic_check_mode
// fills it.
struct pg106_head_mode {
	uint32_t h_active, h_synce, h_blanke, h_blanks;
	uint32_t v_active, v_synce, v_blanke, v_blanks;
	uint32_t v_blank2s, v_blank2e, v_blankus;
	uint32_t clock_khz;
	bool interlace;
};

// nv50_head_atomic_check_mode (head.c:283-326) after drm_mode_set_crtcinfo
// (CRTC_INTERLACE_HALVE_V). Returns 0, -EINVAL on a degenerate mode.
int pg106_head_mode_calc(const struct pg106_disp_mode *mode,
			 struct pg106_head_mode *m);

// nv50_hdmi_enable's max_ac_packet (disp.c:791-794), clamped the way
// nvkm_uoutp_mthd_hdmi rejects (> 0x1f -> -EINVAL): returns -1 then.
int pg106_hdmi_max_ac_packet(const struct pg106_disp_mode *mode);

// --- pushbuffer -----------------------------------------------------------------

struct pg106_push {
	uint32_t *buf;
	size_t cap;     // dwords
	size_t len;     // dwords written (== nv50_dmac cur offset)
	bool overflow;
};

// One PUSH_MTHD group: header (count << 18 | mthd) + `n` data words for
// consecutive methods mthd, mthd+4, ... (push507c.h PUSH_HDR).
void pg106_push_mthd(struct pg106_push *p, uint32_t mthd,
		     const uint32_t *data, unsigned n);

// Decoded method write (for tests / the mock channel executor).
struct pg106_mthd { uint32_t mthd, data; };
// Walk a method stream (incrementing METHOD opcode only, which is all
// nouveau's display code emits). Returns writes decoded, or -1 on an
// opcode this decoder does not model.
int pg106_push_decode(const uint32_t *buf, size_t len, struct pg106_mthd *out,
		      size_t max);

// --- commit ----------------------------------------------------------------------

struct pg106_disp_surface {
	uint32_t handle;   // ctxdma handle (SET_CONTEXT_DMAS_ISO)
	uint64_t offset;   // byte offset in the ctxdma (>> 8 in methods; 40-bit)
	uint16_t w, h;
	uint32_t pitch;    // bytes (>> 8 in STORAGE.PITCH)
	uint8_t format;    // NV907C_SURFACE_SET_PARAMS_FORMAT (0xcf A8R8G8B8)
	uint8_t cpp;       // bytes per pixel of the base plane
};

struct pg106_disp_commit {
	struct pg106_disp_mode mode;
	uint8_t sor;           // acquired SOR (nv_encoder->outp.or.id)
	uint8_t proto;         // PG106_SOR_PROTO_*
	uint8_t sor_depth;     // asyh->or.depth from nv50_sor_update
	uint32_t core_vram_handle; // head507d_core_calc: core.handle
	struct pg106_disp_surface base; // primary plane (base channel)
};

// core507d_init (core507d.c:131-141).
void pg106_disp_push_core_init(struct pg106_push *p, uint32_t ntfy_handle);

// Core channel stream for enabling one head (commit_tail "update output
// path" + "update head" + core507d_update with notifier). Order:
//   SOR_SET_CONTROL(sor) = proto << 8 | BIT(head)
//   head907d_view, head907d_mode, head907d_core_set, head917d_base,
//   head907d_ovly, head917d_dither, head907d_procamp, head907d_or
//   SET_NOTIFIER_CONTROL(WRITE|NOTIFY) ; UPDATE(base interlock) +
//   SET_NOTIFIER_CONTROL(NOTIFY DISABLE)
// The UPDATE carries INTERLOCK_WITH_BASE<head> only because the base
// channel stream below exists and is kicked first (commit_wndw before
// commit_core, disp.c:2310-2320).
void pg106_disp_push_core_modeset(struct pg106_push *p,
				  const struct pg106_disp_commit *c);

// Base channel stream: base907c_image_set then base507c_update with
// INTERLOCK_WITH_CORE (interlock[CORE] = 1 during a modeset).
void pg106_disp_push_base_image(struct pg106_push *p,
				const struct pg106_disp_commit *c);

// Core stream for disabling the head: SOR_SET_CONTROL with the head bit
// cleared — 0 once no owner remains (nv50_sor_update, disp.c:1545-1548) —
// head907d_core_clr, then UPDATE without interlock.
void pg106_disp_push_core_disable(struct pg106_push *p, uint8_t head,
				  uint8_t sor, uint32_t sor_ctrl_before);

#ifdef __cplusplus
}
#endif

#endif // PASCAL_DISP_MODE_H
