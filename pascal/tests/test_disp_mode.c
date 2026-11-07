// SPDX-License-Identifier: GPL-2.0-only
// pascal/tests/test_disp_mode.c — GP102-class mode set: head timing math,
// core917d/base907c method streams, and the interrupt-driven supervisor
// over a MOCK display engine.
//
// SIMULATION ONLY. The mock engine below stands in for hardware: on a
// core PUT it fetches the pushbuffer from fake VRAM, latches methods into
// the ASSEMBLY mirror (0x660000 + mthd), and on UPDATE raises supervisor
// 1/2/3 one after another; after supervisor 3 it copies ASY into the ARM
// mirror (0x640000 + mthd). That sequencing is the documented contract
// nouveau's gf119_disp_intr/gf119_disp_super code is written against, not
// a measurement of a GPU.
//
// Expected values are derived from upstream formulas (cited) by hand:
//   1080p60 (CTA VIC 16): htotal 2200, hsync 2008..2052, vtotal 1125,
//   vsync 1084..1089, 148.5 MHz ->
//     RASTER_SIZE  = 2200 | 1125 << 16   (h/v.active = crtc_*total,
//                                         head.c:298/303)
//     SYNC_END     = 43 | 4 << 16, BLANK_END = 191 | 40 << 16,
//     BLANK_START  = 2111 | 1120 << 16, VERT_BLANK2 = 1 | 0 << 16
//                                        (blank2s = 1 progressive,
//                                         head.c:318-321)
//   1080i (VIC 5, 74.25 MHz, vsync 1084..1094) halved by
//     drm_mode_set_crtcinfo: 540/542/547/562 -> v.synce 4, v.blanke 19,
//     v.blanks 559, blank2e 581, blank2s 1121, v.active 2*562+1 = 1125.

#include <pascal/disp_core.h>
#include <pascal/disp_mode.h>
#include <pascal/edid.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define REQUIRE(c)                                                         \
	do {                                                               \
		if (!(c)) {                                                \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			failures++;                                        \
		}                                                          \
	} while (0)

static const struct pg106_disp_mode M1080P = {
	.pixel_clock_khz = 148500,
	.hdisplay = 1920, .hsync_start = 2008, .hsync_end = 2052, .htotal = 2200,
	.vdisplay = 1080, .vsync_start = 1084, .vsync_end = 1089, .vtotal = 1125,
	.hsync_positive = true, .vsync_positive = true, .bpc = 8, .head = 0,
};

// The 1080p mode the plan sets must come from a REAL EDID, not a table:
// parse the Acer ED320QR S dump (research/reference/edid/, provenance in its
// README), turn its preferred DTD into a mode the way drm_mode_from_dtd /
// drm_edid.c do it (sync start = active + offset, end = start + width,
// total = active + blank), and require it to equal M1080P and to produce
// the same nv50_head_atomic_check_mode result. Skips cleanly if the dump is
// missing (it is tracked in git, so a miss means a broken checkout).
static void
test_mode_from_real_edid(void)
{
	static const char *const paths[] = {
		"../research/reference/edid/acer_ed320qr_s.bin",
		"research/reference/edid/acer_ed320qr_s.bin",
	};
	static unsigned char buf[1024];
	struct pg106_edid edid;
	struct pg106_head_mode a, b;
	size_t n = 0;
	FILE *f = NULL;

	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]) && !f; i++)
		f = fopen(paths[i], "rb");
	if (!f) {
		printf("FAIL edid-mode: acer_ed320qr_s.bin not found (run from "
		       "pascal/)\n");
		failures++;
		return;
	}
	n = fread(buf, 1, sizeof(buf), f);
	fclose(f);
	REQUIRE(pg106_edid_parse(buf, n, &edid) == PG106_EDID_OK);

	const struct pg106_edid_timing *t = &edid.preferred_timing;
	REQUIRE(t->sync == PG106_EDID_SYNC_DIGITAL_SEPARATE && !t->interlaced);
	const struct pg106_disp_mode m = {
		.pixel_clock_khz = t->pixel_clock_khz,
		.hdisplay = t->hactive,
		.hsync_start = (uint16_t)(t->hactive + t->hsync_offset),
		.hsync_end = (uint16_t)(t->hactive + t->hsync_offset +
					t->hsync_width),
		.htotal = t->htotal,
		.vdisplay = t->vactive,
		.vsync_start = (uint16_t)(t->vactive + t->vsync_offset),
		.vsync_end = (uint16_t)(t->vactive + t->vsync_offset +
					t->vsync_width),
		.vtotal = t->vtotal,
		.hsync_positive = t->hsync_positive,
		.vsync_positive = t->vsync_positive,
		.bpc = 8, .head = 0,
	};
	// Field by field: struct padding makes memcmp meaningless here.
	REQUIRE((m.pixel_clock_khz == M1080P.pixel_clock_khz &&
		 m.hdisplay == M1080P.hdisplay &&
		 m.hsync_start == M1080P.hsync_start &&
		 m.hsync_end == M1080P.hsync_end && m.htotal == M1080P.htotal &&
		 m.vdisplay == M1080P.vdisplay &&
		 m.vsync_start == M1080P.vsync_start &&
		 m.vsync_end == M1080P.vsync_end && m.vtotal == M1080P.vtotal &&
		 m.hsync_positive == M1080P.hsync_positive &&
		 m.vsync_positive == M1080P.vsync_positive));
	REQUIRE(pg106_head_mode_calc(&m, &a) == 0);
	REQUIRE(pg106_head_mode_calc(&M1080P, &b) == 0);
	REQUIRE(a.h_active == b.h_active && a.h_synce == b.h_synce &&
		a.h_blanke == b.h_blanke && a.h_blanks == b.h_blanks &&
		a.v_active == b.v_active && a.v_synce == b.v_synce &&
		a.v_blanke == b.v_blanke && a.v_blanks == b.v_blanks &&
		a.v_blank2s == b.v_blank2s && a.v_blank2e == b.v_blank2e &&
		a.clock_khz == b.clock_khz);
	printf("PASS edid-mode: Acer ED320QR S preferred DTD -> %ux%u, "
	       "%u kHz, totals %ux%u == the 1080p60 mode the plan sets\n",
	       m.hdisplay, m.vdisplay, m.pixel_clock_khz, m.htotal, m.vtotal);
}

static void
test_head_mode(void)
{
	struct pg106_head_mode m;

	REQUIRE(pg106_head_mode_calc(&M1080P, &m) == 0);
	REQUIRE(m.h_active == 2200 && m.v_active == 1125);
	REQUIRE(m.h_synce == 43 && m.v_synce == 4);
	REQUIRE(m.h_blanke == 191 && m.v_blanke == 40);
	REQUIRE(m.h_blanks == 2111 && m.v_blanks == 1120);
	REQUIRE(m.v_blank2s == 1 && m.v_blank2e == 0 && !m.interlace);

	struct pg106_disp_mode i = M1080P;
	i.pixel_clock_khz = 74250;
	i.vsync_end = 1094;
	i.interlace = true;
	REQUIRE(pg106_head_mode_calc(&i, &m) == 0);
	REQUIRE(m.v_synce == 4 && m.v_blanke == 19 && m.v_blanks == 559);
	REQUIRE(m.v_blank2e == 581 && m.v_blank2s == 1121);
	REQUIRE(m.v_active == 1125 && m.interlace);

	struct pg106_disp_mode bad = M1080P;
	bad.hsync_end = bad.hsync_start;
	REQUIRE(pg106_head_mode_calc(&bad, &m) != 0);

	// nv50_hdmi_enable: (2200 - 1920 - 56 - 18) / 32 = 6.
	REQUIRE(pg106_hdmi_max_ac_packet(&M1080P) == 6);
	printf("PASS disp: head timing = nv50_head_atomic_check_mode (TOTALS "
	       "in RASTER_SIZE, blank2s=1, interlace halving)\n");
}

// --- method stream --------------------------------------------------------------

static const struct pg106_disp_commit C1080 = {
	.mode = {
		.pixel_clock_khz = 148500,
		.hdisplay = 1920, .hsync_start = 2008, .hsync_end = 2052,
		.htotal = 2200, .vdisplay = 1080, .vsync_start = 1084,
		.vsync_end = 1089, .vtotal = 1125, .hsync_positive = true,
		.vsync_positive = true, .bpc = 8, .head = 0,
	},
	.sor = 0,
	.proto = PG106_SOR_PROTO_SINGLE_TMDS_A,
	.sor_depth = 0, // NV837D_SOR_SET_CONTROL_PIXEL_DEPTH_DEFAULT (TMDS)
	.core_vram_handle = 0xf0000000,
	.base = { .handle = 0xfb000000, .offset = 0x00100000, .w = 1920,
		  .h = 1080, .pitch = 7680, .format = 0xcf, .cpp = 4 },
};

static uint32_t pbuf[512];
static struct pg106_mthd mth[256];

static uint32_t
mval(int n, uint32_t mthd)
{
	for (int i = n - 1; i >= 0; i--)
		if (mth[i].mthd == mthd)
			return mth[i].data;
	return 0xdeadbeef;
}

static void
test_push(void)
{
	struct pg106_push p = { pbuf, 512, 0, false };

	pg106_disp_push_core_modeset(&p, &C1080);
	REQUIRE(!p.overflow);
	// First header: SOR_SET_CONTROL(0), 1 word; data proto<<8 | BIT(0).
	REQUIRE(pbuf[0] == (1u << 18 | 0x200) && pbuf[1] == 0x00000101);
	int n = pg106_push_decode(pbuf, p.len, mth, 256);
	REQUIRE(n > 0);
	REQUIRE(mval(n, 0x414) == (2200u | 1125u << 16));	// RASTER_SIZE
	REQUIRE(mval(n, 0x418) == (43u | 4u << 16));
	REQUIRE(mval(n, 0x41c) == (191u | 40u << 16));
	REQUIRE(mval(n, 0x420) == (2111u | 1120u << 16));
	REQUIRE(mval(n, 0x424) == 1u);				// blank2s = 1
	REQUIRE(mval(n, 0x450) == 148500000u);
	REQUIRE(mval(n, 0x454) == 0x00200000u);			// CLK_CUSTOM
	REQUIRE(mval(n, 0x4b8) == (1920u | 1080u << 16));	// VIEWPORT_IN
	REQUIRE(mval(n, 0x4c8) == (1920u | 1080u << 16));
	REQUIRE(mval(n, 0x46c) == (((7680u >> 8) << 8) | 1u << 24)); // core pitch
	REQUIRE(mval(n, 0x474) == 0xf0000000u);
	REQUIRE(mval(n, 0x4d0) == 0x00020301u);	// base bounds BPP_32 LUT1025
	REQUIRE(mval(n, 0x4d4) == 0x00000100u);	// ovly BPP_16
	REQUIRE(mval(n, 0x4a0) == 0x00000002u);	// dither 8bpc, disabled
	REQUIRE(mval(n, 0x498) == 0x00040000u);	// procamp sat.cos 1024
	REQUIRE(mval(n, 0x404) == 0x00000000u);	// +sync, depth DEFAULT
	REQUIRE(mval(n, 0x408) == 0x31ec6000u);
	REQUIRE(mval(n, 0x080) == 0x00000002u);	// UPDATE: base0 interlock
	REQUIRE(mth[n - 1].mthd == 0x084 && mth[n - 1].data == 0);
	// The old plan's VERT_BLANK2 = 0 and RASTER_SIZE = actives are gone.
	REQUIRE(mval(n, 0x414) != (1920u | 1080u << 16));

	// Base channel stream.
	struct pg106_push b = { pbuf, 512, 0, false };
	pg106_disp_push_base_image(&b, &C1080);
	n = pg106_push_decode(pbuf, b.len, mth, 256);
	REQUIRE(n == 8);
	REQUIRE(mval(n, 0x084) == 0x10);		// NON_TEARING, interval 1
	REQUIRE(mval(n, 0x0c0) == 0xfb000000u);
	REQUIRE(mval(n, 0x400) == 0x1000u);		// offset >> 8
	REQUIRE(mval(n, 0x408) == (1920u | 1080u << 16));
	REQUIRE(mval(n, 0x40c) == (30u << 8 | 1u << 24));
	REQUIRE(mval(n, 0x410) == 0xcf00u);
	REQUIRE(mth[n - 1].mthd == 0x080 && mth[n - 1].data == 1);

	// Disable: last owner removed -> SOR ctrl 0 (disp.c:1545-1548).
	struct pg106_push d = { pbuf, 512, 0, false };
	pg106_disp_push_core_disable(&d, 0, 0, 0x00000101);
	n = pg106_push_decode(pbuf, d.len, mth, 256);
	REQUIRE(mval(n, 0x200) == 0);
	REQUIRE(mval(n, 0x474) == 0);
	printf("PASS disp: core917d/base907c method streams (PUSH_HDR "
	       "encoding, head flush order, BASE interlock only with a base "
	       "stream, disable clears SOR ctrl to 0)\n");
}

// --- mock display engine ---------------------------------------------------------

#define NREG 4096
struct mock {
	uint32_t addr[NREG], val[NREG];
	unsigned n;
	uint8_t vram[0x10000]; // pushbuffers live here
	uint32_t core_push, base_push; // chan->push words (addr >> 8 | 1)
	uint32_t core_get;     // dwords consumed so far
	int pending_super;     // 0 none, else next stage bit
	unsigned pll_calls;
	uint32_t pll_type, pll_khz;
	unsigned ied_calls;
};
static struct mock MK;

static uint32_t *
slot(uint32_t a)
{
	for (unsigned i = 0; i < MK.n; i++)
		if (MK.addr[i] == a)
			return &MK.val[i];
	if (MK.n >= NREG)
		return NULL;
	MK.addr[MK.n] = a;
	MK.val[MK.n] = 0;
	return &MK.val[MK.n++];
}

static uint32_t
m_peek(uint32_t a)
{
	for (unsigned i = 0; i < MK.n; i++)
		if (MK.addr[i] == a)
			return MK.val[i];
	return 0;
}

static void
m_set(uint32_t a, uint32_t v)
{
	uint32_t *s = slot(a);
	if (s)
		*s = v;
}

static void
core_fetch(uint32_t put_bytes)
{
	// Hardware stand-in: execute methods [get, put) of the core
	// pushbuffer into the ASSEMBLY mirror; UPDATE raises supervisor 1.
	// chan->push = addr >> 8 | target (nv50.c:694-701).
	uint32_t base = (MK.core_push & ~0x3u) << 8;
	const uint32_t *pb = (const uint32_t *)&MK.vram[base & 0xffff];
	uint32_t put = put_bytes >> 2;
	static struct pg106_mthd ms[256];
	int n = pg106_push_decode(pb + MK.core_get, put - MK.core_get, ms, 256);
	for (int i = 0; i < n && i < 256; i++) {
		if (ms[i].mthd == PG106_917D_UPDATE) {
			m_set(PG106_DISP_INTR, 0x00100000);
			m_set(PG106_DISP_SUPER_STAT, 0x1);
			m_set(PG106_DISP_SUPER_HEAD(0), 0x00011000);
			MK.pending_super = 1;
		} else {
			m_set(PG106_DISP_ASY_BASE + ms[i].mthd, ms[i].data);
		}
	}
	MK.core_get = put;
}

static uint32_t
m_rd(void *u, uint32_t a)
{
	(void)u;
	if (a == 0x612004)
		return 0x00000f0f;	// 4 heads, SORs 0-3 (gf119_*_cnt)
	if (a == 0x070000 || a == 0x61c034)
		return 0;
	return m_peek(a);
}

static void
m_wr(void *u, uint32_t a, uint32_t v)
{
	(void)u;
	m_set(a, v);
	if (a == PG106_DISP_CHAN_PUSH(0))
		MK.core_push = v;
	if (a == PG106_DISP_CHAN_PUT(0) && MK.core_push && v)
		core_fetch(v);
	if (a == PG106_DISP_SUPER_STAT)	// ack
		m_set(PG106_DISP_INTR, 0);
	if (a == PG106_DISP_SUPER_ACK && MK.pending_super) {
		// Next supervisor stage; after 3, ASY -> ARM.
		if (MK.pending_super < 4) {
			MK.pending_super <<= 1;
			m_set(PG106_DISP_INTR, 0x00100000);
			m_set(PG106_DISP_SUPER_STAT, (uint32_t)MK.pending_super);
			m_set(PG106_DISP_SUPER_HEAD(0), 0x00011000);
		} else {
			for (unsigned i = 0; i < MK.n; i++) {
				uint32_t x = MK.addr[i];
				if (x >= PG106_DISP_ASY_BASE &&
				    x < PG106_DISP_ASY_BASE + 0x1000)
					m_set(x - 0x20000, MK.val[i]);
			}
			MK.pending_super = 0;
		}
	}
}

static int
m_pll(void *u, uint32_t type, uint32_t khz)
{
	(void)u;
	MK.pll_calls++;
	MK.pll_type = type;
	MK.pll_khz = khz;
	return 0;
}

static void
m_ied(void *u, int on, int id, int head, int sor, uint32_t khz)
{
	(void)u; (void)on; (void)id; (void)head; (void)sor; (void)khz;
	MK.ied_calls++;
}

static const struct pg106_dbus BUS = { m_rd, m_wr, m_pll, m_ied, NULL };

static struct pg106_dreg tr[8192];

static size_t
count_wr(const struct pg106_dtrace *t, uint32_t addr, uint32_t val)
{
	size_t c = 0;
	for (size_t i = 0; i < t->len && i < t->cap; i++)
		if ((t->rec[i].kind == PG106_DREG_WR ||
		     t->rec[i].kind == PG106_DREG_MASK) &&
		    t->rec[i].addr == addr && t->rec[i].val == val)
			c++;
	return c;
}

// The whole HDMI head-0 modeset, upstream order.
static size_t
run_modeset(struct pg106_dtrace *t, struct pg106_disp_super_result *sr)
{
	struct pg106_disp_init_cfg cfg = { .inst_addr = 0x00200000 };
	struct pg106_disp_sor_sw sw[8];

	memset(&MK, 0, sizeof(MK));
	memset(sr, 0, sizeof(*sr));
	memset(sw, 0, sizeof(sw));
	REQUIRE(pg106_disp_init(&BUS, &cfg, t) == 0);
	// Core pushbuffer at VRAM 0x1000 (target VRAM = 1), base at 0x2000.
	REQUIRE(pg106_disp_chan_init(&BUS, 0, 0, (0x1000u >> 8) | 1, 0, true, t) == 0);
	REQUIRE(pg106_disp_chan_init(&BUS, 1, 1, (0x2000u >> 8) | 1, 0, false, t) == 0);

	struct pg106_push core = { (uint32_t *)&MK.vram[0x1000], 0x400, 0, false };
	struct pg106_push base = { (uint32_t *)&MK.vram[0x2000], 0x400, 0, false };
	pg106_disp_push_core_init(&core, 0xf0000001);
	pg106_disp_kick(&BUS, 0, (uint32_t)core.len, true, t);

	// Acquire: DCB OR mask 0x1 (macro link A), sublink 1 -> SOR 0.
	pg106_disp_sor_route(&BUS, 0x1, 1, 0, 1, t);
	// nv50_hdmi_enable -> nvif_outp_hdmi (before the core stream).
	sw[0].high_speed = pg106_disp_hdmi_enable(&BUS, 0, 0,
		(uint8_t)pg106_hdmi_max_ac_packet(&C1080.mode), PG106_HDMI_REKEY,
		C1080.mode.pixel_clock_khz, false, false, false, t);
	sw[0].outp_attached = true;
	sw[0].sor = 0;

	pg106_disp_push_core_modeset(&core, &C1080);
	pg106_disp_push_base_image(&base, &C1080);
	// commit_wndw (base UPDATE) is kicked before commit_core.
	pg106_disp_kick(&BUS, 1, (uint32_t)base.len, true, t);
	pg106_disp_kick(&BUS, 0, (uint32_t)core.len, true, t);

	// Interrupt-driven supervisors.
	unsigned stages = 0;
	for (int guard = 0; guard < 8; guard++) {
		uint32_t pend = pg106_disp_intr(&BUS, t);
		if (!pend)
			break;
		pg106_disp_super(&BUS, pend, 0x1, 0x1, sw, sr, t);
		stages++;
	}
	return stages;
}

static void
test_modeset(void)
{
	static struct pg106_dreg tr2[8192];
	struct pg106_dtrace t = { tr, 8192, 0, false, 0 };
	struct pg106_dtrace t2 = { tr2, 8192, 0, false, 0 };
	struct pg106_disp_super_result sr, sr2;

	size_t stages = run_modeset(&t, &sr);
	REQUIRE(stages == 3 && !t.overflow);
	// gf119_disp_init landmarks.
	REQUIRE(count_wr(&t, 0x610010, (0x00200000u >> 8) | 9) == 1);
	REQUIRE(count_wr(&t, 0x6100b0, 0x307) == 1);
	// IOR power at init only: 0x61c004 = 0x80000001 for SOR 0..3.
	REQUIRE(count_wr(&t, 0x61c004, 0x80000001) == 1);
	// Channel init: pushbuffer word + core ctrl 0x01000013.
	REQUIRE(count_wr(&t, 0x611494, 0x11) == 1);
	REQUIRE(count_wr(&t, 0x610490, 0x01000013) == 1);
	REQUIRE(count_wr(&t, 0x6104a0, 0x00000013) == 1);	// base ctrl 1
	// Route: SOR 0 + 1, link A -> 0x612308[4:0] = 0x01.
	REQUIRE(count_wr(&t, 0x612308, 0x01) == 1);
	// gk104 HDMI path (not gf119's 0x6167a4/ac).
	REQUIRE(count_wr(&t, 0x6900cc, 0x10) == 1);
	REQUIRE(count_wr(&t, 0x690080, 0x82000000) == 1);
	REQUIRE(count_wr(&t, 0x616798, 0x40000000u | 6u << 16 | 56u) == 1);
	REQUIRE(count_wr(&t, 0x6167ac, 0x10) == 0);
	// Supervisor 1 latched ASY from the fetched pushbuffer.
	REQUIRE(sr.asy[0].htotal == 2200 && sr.asy[0].vtotal == 1125);
	REQUIRE(sr.asy[0].hz == 148500000 && sr.asy[0].depth == 18);
	REQUIRE(sr.sor_asy[0].proto == 1 && sr.sor_asy[0].head == 1);
	// 2.1 VPLL0 148500 kHz; 2.2 rgclk 0 (rgdiv never set for SORs);
	// gf119_sor_clock: 148.5 MHz <= 340 MHz -> speed 0x0a, div 0.
	REQUIRE(MK.pll_calls == 1 && MK.pll_type == 0x80 && MK.pll_khz == 148500);
	REQUIRE(count_wr(&t, 0x612200, 0) == 1);
	REQUIRE(count_wr(&t, 0x612300, 0x0a << 18) == 2); // speed then 0x707
	REQUIRE(MK.ied_calls == 2); // OnInt2 (2.2) + OnInt3 (3.0)
	// After supervisor 3 the ARM mirror holds the mode (gf119_head_state
	// reads ARM at 0x640414).
	REQUIRE(m_peek(0x640414) == (2200u | 1125u << 16));
	REQUIRE(m_peek(0x640200) == 0x101);
	REQUIRE(count_wr(&t, PG106_DISP_SUPER_ACK, 0x80000000) == 3);
	// Never: methods written straight into BAR0 0x640000+ by the driver.
	for (size_t i = 0; i < t.len; i++)
		if (tr[i].kind == PG106_DREG_WR && tr[i].addr > 0x640000 &&
		    tr[i].addr < 0x641000)
			REQUIRE(!"driver wrote a method offset into the ARM page");

	// Determinism.
	run_modeset(&t2, &sr2);
	REQUIRE(t2.len == t.len);
	int same = t2.len == t.len;
	for (size_t i = 0; same && i < t.len; i++)
		same = tr[i].kind == tr2[i].kind && tr[i].addr == tr2[i].addr &&
		       tr[i].val == tr2[i].val && tr[i].mask == tr2[i].mask;
	REQUIRE(same);
	printf("PASS disp: HDMI modeset over mock engine — init, core/base DMA "
	       "channels, route 0x612308, gk104 HDMI, PUT kick, supervisor "
	       "1/2/3 (VPLL, rgclk 0, SOR clock), ARM readback; %zu steps, "
	       "deterministic\n", t.len);
}

static void
test_tmds_high_speed(void)
{
	struct pg106_dtrace t = { tr, 8192, 0, false, 0 };

	memset(&MK, 0, sizeof(MK));
	// gm200_sor_hdmi_scdc: khz > 340000 -> high_speed (gm200.c:70).
	REQUIRE(!pg106_disp_hdmi_enable(&BUS, 0, 0, 6, 56, 340000, true, true,
					false, &t));
	REQUIRE(m_peek(0x61c5bc) == 0);
	REQUIRE(pg106_disp_hdmi_enable(&BUS, 0, 0, 6, 56, 594000, true, true,
				       false, &t));
	REQUIRE(m_peek(0x61c5bc) == 3);
	// gf119_sor_clock: speed 0x14 and div2 = 1 (gf119.c:293-300).
	m_set(0x612300, 0);
	pg106_disp_sor_clock(&BUS, 0, true, 1, true, &t);
	REQUIRE(m_peek(0x612300) == (0x14u << 18 | 0x100));
	m_set(0x612300, 0);
	pg106_disp_sor_clock(&BUS, 0, true, 1, false, &t);
	REQUIRE(m_peek(0x612300) == (0x0au << 18));
	printf("PASS disp: TMDS high_speed > 340 MHz (gm200) with div2 = 1 "
	       "(gf119_sor_clock)\n");
}

static void
test_dp(void)
{
	struct pg106_dtrace t = { tr, 8192, 0, false, 0 };
	static struct pg106_dp_sink sink;
	const struct pg106_dp_link l = { .sor = 1, .link = 1, .bw = 0x0a,
					 .nr = 2, .ef = true };

	memset(&MK, 0, sizeof(MK));
	memset(&sink, 0, sizeof(sink));
	sink.dpcd[0x000] = 0x12; // DPCD 1.2
	sink.dpcd[0x002] = 0x80 | 0x40 | 2; // EF cap, TPS3, 2 lanes
	sink.dpcd[0x206] = 0x11; // request vswing 1 on lanes 0/1
	sink.cr_after = 2;
	sink.eq_after = 1;
	REQUIRE(pg106_dp_train(&BUS, &sink, &l, &t) == 0);
	REQUIRE(sink.dpcd[0x100] == 0x0a && sink.dpcd[0x101] == 0x82);
	REQUIRE((sink.dpcd[0x102] & 0x0f) == 0); // ended on pattern 0
	REQUIRE(sink.dpcd[0x103] == 0x01);       // lane 0 vswing 1
	// gf119_sor_dp_links: clksor bw << 18 at 0x612300 + 0x800.
	REQUIRE((m_peek(0x612b00) & 0x007c0000) == (0x0au << 18));
	REQUIRE((m_peek(0x61c90c) & 0x401f4000) == (0x3u << 16 | 0x4000));
	// TPS3 used for EQ (chipset >= 0xd0 keeps it): 0x61c910 = 0x03...
	int saw_tps3 = 0;
	for (size_t i = 0; i < t.len; i++)
		if (tr[i].kind == PG106_DREG_MASK && tr[i].addr == 0x61c910 &&
		    tr[i].val == 0x03030303)
			saw_tps3 = 1;
	REQUIRE(saw_tps3);

	// A sink that never reaches CR: 5 tries then failure, pattern 0.
	memset(&sink, 0, sizeof(sink));
	sink.dpcd[0x000] = 0x12;
	sink.cr_after = 99;
	sink.eq_after = 99;
	t.len = 0;
	REQUIRE(pg106_dp_train(&BUS, &sink, &l, &t) == -1);
	REQUIRE(sink.drive_writes == 5);
	REQUIRE((sink.dpcd[0x102] & 0x0f) == 0);

	// nv50_disp_super_2_2_dp for 1080p60 on 2 x HBR (hand-derived):
	// h = (191 + 2200 - 2111 - 7) * 270000 / 148500 - 3 - 6 = 487
	// v = (1120 - 40 - 25) * 270000 / 148500 - (18 + 3) - 1 = 1896
	// ratio = (148500 * 24 / 8 / 2) * 1e5 / 270000 = 82500,
	// watermark = (1e5 - 82500) * 64 * 82500 / 1e5 / 1e5 + 6 = 15.
	{
		struct pg106_disp_sor_sw sw[8];
		struct pg106_disp_super_result sr;
		memset(sw, 0, sizeof(sw));
		memset(&sr, 0, sizeof(sr));
		memset(&MK, 0, sizeof(MK));
		sw[0].dp_bw = 0x0a; sw[0].dp_nr = 2; sw[0].dp_ef = true;
		// ASY state as a DP head would latch it.
		m_set(0x660414, 2200 | 1125u << 16);
		m_set(0x660418, 43 | 4u << 16);
		m_set(0x66041c, 191 | 40u << 16);
		m_set(0x660420, 2111 | 1120u << 16);
		m_set(0x660450, 148500000);
		m_set(0x660404, 5u << 6);              // BPP_24_444
		m_set(0x660200, 0x801);                // DP_A, head 0
		m_set(PG106_DISP_SUPER_HEAD(0), 0x00011000);
		t.len = 0;
		pg106_disp_super(&BUS, 1, 1, 1, sw, &sr, &t);
		// gf119_disp_super cleared 0x6101d4; the engine re-arms it.
		m_set(PG106_DISP_SUPER_HEAD(0), 0x00011000);
		pg106_disp_super(&BUS, 2, 1, 1, sw, &sr, &t);
		printf("   super 2.2 dp: h %u v %u watermark %u\n", sr.dp_h,
		       sr.dp_v, sr.dp_watermark);
		REQUIRE(sr.dp_h == 487 && sr.dp_v == 1896 && sr.dp_watermark == 15);
		REQUIRE(m_peek(0x616610) == (0x08000000u | 15));
	}
	printf("PASS disp: DP model — dp.c CR/EQ loops over a simulated sink, "
	       "gf119/gm107/gm200 SOR DP registers, super 2.2 DP config "
	       "(h 487, v 1896, watermark 15)\n");
}

int
main(void)
{
	printf("disp_mode tests — SIMULATION ONLY (mock display engine; not "
	       "hardware evidence)\n");
	test_head_mode();
	test_mode_from_real_edid();
	test_push();
	test_modeset();
	test_tmds_high_speed();
	test_dp();
	if (failures) {
		printf("%d check(s) FAILED\n", failures);
		return EXIT_FAILURE;
	}
	printf("all checks passed\n");
	return EXIT_SUCCESS;
}
