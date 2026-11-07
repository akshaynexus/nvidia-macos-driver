// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/src/disp_mode.c — core917d/head917d method streams for a
// GP102-class mode set. SIMULATION; see disp_mode.h for provenance.

#include <pascal/disp_mode.h>

#include <string.h>

// --- head timing (head.c:283-326 + drm_modes.c:1348-1408) --------------------

int
pg106_head_mode_calc(const struct pg106_disp_mode *mode,
		     struct pg106_head_mode *m)
{
	uint32_t vdisplay, vsync_start, vsync_end, vtotal;
	uint32_t hblank_end, vblank_end;

	if (!mode || !m || !mode->pixel_clock_khz ||
	    mode->hsync_end <= mode->hsync_start ||
	    mode->vsync_end <= mode->vsync_start ||
	    mode->hsync_start < mode->hdisplay ||
	    mode->vsync_start < mode->vdisplay ||
	    mode->htotal < mode->hsync_end || mode->vtotal < mode->vsync_end)
		return -22; // -EINVAL
	memset(m, 0, sizeof(*m));

	// drm_mode_set_crtcinfo(mode, CRTC_INTERLACE_HALVE_V | ...).
	vdisplay = mode->vdisplay;
	vsync_start = mode->vsync_start;
	vsync_end = mode->vsync_end;
	vtotal = mode->vtotal;
	if (mode->interlace) {
		vdisplay /= 2;
		vsync_start /= 2;
		vsync_end /= 2;
		vtotal /= 2;
	}
	vblank_end = vsync_end > vtotal ? vsync_end : vtotal;
	hblank_end = mode->hsync_end > mode->htotal ? mode->hsync_end :
						     mode->htotal;

	// "The hardware modes are defined in terms of a repeating interval
	// starting one unit into the sync pulse" — the -1 bias. ACTIVE is
	// the TOTAL (m->h.active = mode->crtc_htotal, head.c:298), which
	// head907d_mode writes to HEAD_SET_RASTER_SIZE (head907d.c:338-340).
	m->h_active = mode->htotal;
	m->h_synce = mode->hsync_end - mode->hsync_start - 1u;
	m->h_blanke = hblank_end - mode->hsync_start - 1u;
	m->h_blanks = m->h_blanke + mode->hdisplay;

	m->v_active = vtotal;
	m->v_synce = vsync_end - vsync_start - 1u;
	m->v_blanke = vblank_end - vsync_start - 1u;
	m->v_blanks = m->v_blanke + vdisplay;

	m->v_blankus = (uint32_t)((uint64_t)(m->v_active - vdisplay - 2) *
				  m->h_active * 1000 / mode->pixel_clock_khz);

	if (mode->interlace) {				// head.c:313-317
		m->v_blank2e = m->v_active + m->v_blanke;
		m->v_blank2s = m->v_blank2e + vdisplay;
		m->v_active = (m->v_active * 2) + 1;
		m->interlace = true;
	} else {					// head.c:318-321
		m->v_blank2e = 0;
		m->v_blank2s = 1;
		m->interlace = false;
	}
	m->clock_khz = mode->pixel_clock_khz;
	return 0;
}

int
pg106_hdmi_max_ac_packet(const struct pg106_disp_mode *mode)
{
	uint32_t v = (uint32_t)mode->htotal - mode->hdisplay;	// disp.c:791-794
	v -= PG106_HDMI_REKEY;
	v -= 18;
	v /= 32;
	return v > 0x1f ? -1 : (int)v;	// uoutp.c:254-257 rejects > 0x1f
}

// --- pushbuffer (push507c.h) ---------------------------------------------------

void
pg106_push_mthd(struct pg106_push *p, uint32_t mthd, const uint32_t *data,
		unsigned n)
{
	if (p->len + 1 + n > p->cap) {
		p->overflow = true;
		return;
	}
	// NV507C_DMA: OPCODE 31:29 = METHOD (0), COUNT 27:18, OFFSET 11:2.
	p->buf[p->len++] = ((uint32_t)(n & 0x3ff) << 18) | (mthd & 0x0ffcu);
	for (unsigned i = 0; i < n; i++)
		p->buf[p->len++] = data[i];
}

int
pg106_push_decode(const uint32_t *buf, size_t len, struct pg106_mthd *out,
		  size_t max)
{
	size_t i = 0, n = 0;

	while (i < len) {
		uint32_t h = buf[i++];
		uint32_t cnt = (h >> 18) & 0x3ff, mthd = h & 0x0ffc;
		if ((h >> 29) != 0)
			return -1; // JUMP / NONINC / SUBDEVICE: not emitted here
		for (uint32_t k = 0; k < cnt; k++) {
			if (i >= len)
				return -1;
			if (n < max) {
				out[n].mthd = mthd + 4 * k;
				out[n].data = buf[i];
			}
			n++;
			i++;
		}
	}
	return (int)n;
}

#define HM(h, m) PG106_917D_HEAD(h, m)

void
pg106_disp_push_core_init(struct pg106_push *p, uint32_t ntfy_handle)
{
	pg106_push_mthd(p, PG106_917D_SET_CONTEXT_DMA_NOTIFIER, &ntfy_handle, 1);
}

void
pg106_disp_push_core_modeset(struct pg106_push *p,
			     const struct pg106_disp_commit *c)
{
	const struct pg106_disp_mode *md = &c->mode;
	const uint32_t i = md->head;
	struct pg106_head_mode m;
	uint32_t d[6];

	if (pg106_head_mode_calc(md, &m)) {
		p->overflow = true;
		return;
	}

	// 1. Output path: nv50_sor_atomic_enable -> nv50_sor_update ->
	// sor907d_ctrl.
	d[0] = ((uint32_t)c->proto << 8) | (1u << i);
	pg106_push_mthd(p, PG106_917D_SOR_SET_CONTROL(c->sor), d, 1);

	// 2. nv50_head_flush_set: view.
	d[0] = 0; // TAPS_1/TAPS_1, bias 0
	pg106_push_mthd(p, HM(i, PG106_917D_HEAD_SET_CONTROL_OUTPUT_SCALER), d, 1);
	d[0] = md->hdisplay | (uint32_t)md->vdisplay << 16;	// iW/iH
	pg106_push_mthd(p, HM(i, PG106_917D_HEAD_SET_VIEWPORT_SIZE_IN), d, 1);
	d[0] = d[1] = d[2] = md->hdisplay | (uint32_t)md->vdisplay << 16;
	pg106_push_mthd(p, HM(i, PG106_917D_HEAD_SET_VIEWPORT_SIZE_OUT), d, 3);

	// mode (head907d.c:333-374).
	d[0] = 0;					// OVERSCAN_COLOR
	d[1] = m.h_active | m.v_active << 16;		// RASTER_SIZE = totals
	d[2] = m.h_synce | m.v_synce << 16;
	d[3] = m.h_blanke | m.v_blanke << 16;
	d[4] = m.h_blanks | m.v_blanks << 16;
	d[5] = m.v_blank2s | m.v_blank2e << 16;		// YSTART | YEND
	pg106_push_mthd(p, HM(i, PG106_917D_HEAD_SET_OVERSCAN_COLOR), d, 6);
	d[0] = 0;
	pg106_push_mthd(p, HM(i, PG106_917D_HEAD_SET_DEFAULT_BASE_COLOR), d, 1);
	d[0] = (m.clock_khz * 1000u) & 0x7fffffffu;	// HERTZ, ADJ1000DIV1001 0
	d[1] = 0x2u << 20;				// MODE CLK_CUSTOM
	d[2] = d[0];
	pg106_push_mthd(p, HM(i, PG106_917D_HEAD_SET_PIXEL_CLOCK_FREQUENCY), d, 3);

	// core_set with head507d_core_calc's values (base visible).
	{
		const uint32_t w = c->base.w, h = c->base.h;
		const uint32_t pitch = ((w + 63u) & ~63u) * 4;	// ALIGN(w,64)*4
		d[0] = 0;					// ORIGIN offset 0 >> 8
		pg106_push_mthd(p, HM(i, PG106_917D_HEAD_SET_OFFSET), d, 1);
		d[0] = w | h << 16;
		d[1] = 0 /*ONE_GOB*/ | ((pitch >> 8) | 0) << 8 | 1u << 24 /*PITCH*/;
		d[2] = 0xcfu << 8;				// A8R8G8B8, X1_AA, LINEAR
		d[3] = c->core_vram_handle;
		pg106_push_mthd(p, HM(i, PG106_917D_HEAD_SET_SIZE), d, 4);
		d[0] = 0;					// VIEWPORT_POINT_IN 0,0
		pg106_push_mthd(p, HM(i, PG106_917D_HEAD_SET_VIEWPORT_POINT_IN), d, 1);
	}

	// head917d_base: cpp 4 -> USABLE | BPP_32 | BASE_LUT USAGE_1025.
	d[0] = 0;
	if (c->base.cpp) {
		uint32_t bpp = c->base.cpp == 8 ? 5 : c->base.cpp == 4 ? 3 :
			       c->base.cpp == 2 ? 1 : 0;
		d[0] = 1u | bpp << 8 | 2u << 16;
	}
	pg106_push_mthd(p, HM(i, PG106_917D_HEAD_SET_BASE_CHANNEL_USAGE_BOUNDS), d, 1);
	// head907d_ovly: no overlay -> PIXEL_DEPTH BPP_16, not usable.
	d[0] = 1u << 8;
	pg106_push_mthd(p, HM(i, PG106_917D_HEAD_SET_OVERLAY_USAGE_BOUNDS), d, 1);

	// head917d_dither with nv50_head_atomic_check_dither under the
	// connector defaults (mode/depth AUTO).
	{
		uint32_t base_depth = c->base.cpp == 4 ? 24 : c->base.cpp * 8u;
		uint32_t mode = 0;
		if (base_depth > (uint32_t)md->bpc * 3)
			mode = 0x1u | 0x2u << 3;	// DITHERING_MODE_DYNAMIC2X2
		if (md->bpc >= 8)
			mode |= 0x1u << 1;		// DITHERING_DEPTH_8BPC
		d[0] = mode;	// ENABLE 0:0, BITS 2:1, MODE 6:3, PHASE 0
	}
	pg106_push_mthd(p, HM(i, PG106_917D_HEAD_SET_DITHER_CONTROL), d, 1);

	// head907d_procamp: vibrance 150 / hue 90 -> sat.cos = (50*2047 +
	// 50) / 100 = 1024, sin 0 (head.c:84-95).
	{
		const int vib = 150 - 100, hue = 90 - 90;
		const int adj = vib > 0 ? 50 : 0;
		uint32_t cosv = (uint32_t)((vib * 2047 + adj) / 100) & 0xfff;
		uint32_t sinv = (uint32_t)((hue * 2047) / 100) & 0xfff;
		d[0] = cosv << 8 | sinv << 20;	// RGB, LPF AUTO, VESA, no compr.
	}
	pg106_push_mthd(p, HM(i, PG106_917D_HEAD_SET_PROCAMP), d, 1);

	// head907d_or: OUTPUT_RESOURCE + HEAD_SET_CONTROL.
	d[0] = 0 /*CRC ACTIVE_RASTER*/ |
	       (md->hsync_positive ? 0u : 1u) << 3 |
	       (md->vsync_positive ? 0u : 1u) << 4 |
	       ((uint32_t)c->sor_depth & 0xf) << 6;
	d[1] = 0x31ec6000u | i << 25 | (m.interlace ? 1u : 0u);
	pg106_push_mthd(p, HM(i, PG106_917D_HEAD_SET_CONTROL_OUTPUT_RESOURCE), d, 2);

	// core507d_update(core, interlock, ntfy = true).
	d[0] = 0x80000000u;	// NOTIFY ENABLE | MODE WRITE | OFFSET NTFY(0)
	pg106_push_mthd(p, PG106_917D_SET_NOTIFIER_CONTROL, d, 1);
	d[0] = 0x2u << (i * 4);	// INTERLOCK_WITH_BASE<head> (base917c.c:49)
	d[1] = 0;		// SET_NOTIFIER_CONTROL NOTIFY DISABLE
	pg106_push_mthd(p, PG106_917D_UPDATE, d, 2);
}

void
pg106_disp_push_base_image(struct pg106_push *p,
			   const struct pg106_disp_commit *c)
{
	uint32_t d[5];

	// nv50_wndw_flush_set: interlock[CORE] set -> NON_TEARING, interval 1.
	d[0] = 0u << 8 | 0u << 2 | 1u << 4;
	pg106_push_mthd(p, PG106_907C_SET_PRESENT_CONTROL, d, 1);
	d[0] = c->base.handle;
	pg106_push_mthd(p, PG106_907C_SET_CONTEXT_DMAS_ISO0, d, 1);
	d[0] = (uint32_t)(c->base.offset >> 8);
	d[1] = 0;
	d[2] = c->base.w | (uint32_t)c->base.h << 16;
	d[3] = 0 /*ONE_GOB*/ | ((c->base.pitch >> 8) & 0x1fff) << 8 |
	       1u << 24 /*PITCH layout*/;
	d[4] = (uint32_t)c->base.format << 8;	// X1_AA, LINEAR, FRM
	pg106_push_mthd(p, PG106_907C_SURFACE_SET_OFFSET0, d, 5);
	// base507c_update: UPDATE = interlock[CORE].
	d[0] = 1;
	pg106_push_mthd(p, PG106_907C_UPDATE, d, 1);
}

void
pg106_disp_push_core_disable(struct pg106_push *p, uint8_t head, uint8_t sor,
			     uint32_t before)
{
	uint32_t d[2];

	d[0] = before & ~(1u << head);
	if ((d[0] & 0xf) == 0)		// OWNER == NONE -> ctrl = 0
		d[0] = 0;
	pg106_push_mthd(p, PG106_917D_SOR_SET_CONTROL(sor), d, 1);
	d[0] = 0;			// head907d_core_clr
	pg106_push_mthd(p, HM(head, PG106_917D_HEAD_SET_CONTEXT_DMAS_ISO), d, 1);
	d[0] = 0x80000000u;
	pg106_push_mthd(p, PG106_917D_SET_NOTIFIER_CONTROL, d, 1);
	d[0] = 0;
	d[1] = 0;
	pg106_push_mthd(p, PG106_917D_UPDATE, d, 2);
}
