// SPDX-License-Identifier: GPL-2.0-only
// pascal/tests/test_disp_mode.c — Phase 5B gate: simulated Pascal display
// mode-set sequence for one 1080p60 HDMI head (GP106M Helios 300, HDMI/miniDP
// wired to the dGPU, HELIOS300-PASCAL-RESEARCH.md §2).
//
// SIMULATION ONLY: the plan runs over a mock bus (a recorder array below).
// Nothing here touches hardware; a PASS is a plan-shape check, not a device
// claim (HANDOFF-PASCAL.md §7).
//
// NOTE: the CEA-861 detailed-timing numbers below are hard-coded on purpose
// (briefing rule: no dependency on sibling modules). A sibling EDID parser
// exists (pascal/core/src/edid.c) and its extracted DTD for a 1080p60 panel
// must yield exactly these numbers (pixel clock 148500 kHz = DTD bytes 0..1
// 0x3903? -> 14850 x 10kHz, hactive 1920, hblank 280, hsync offset 88 width
// 44, vactive 1080, vblank 45, vsync offset 4 width 5, +hsync +vsync).
// Cross-check test_edid.c against this block later.

#include <pascal/disp_mode.h>

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do {                                           \
	if (!(cond)) {                                                  \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);    \
		failures++;                                             \
	}                                                               \
} while (0)

static void
report(const char *name)
{
	if (failures == 0)
		printf("PASS %s\n", name);
}

// CEA-861 1080p60 detailed timing (see note above re: edid.c cross-check).
static struct pg106_disp_mode
cea1080p60(void)
{
	struct pg106_disp_mode m = {
		.pixel_clock_khz = 148500,
		.hdisplay = 1920,
		.hsync_start = 1920 + 88,   // hsync offset 88
		.hsync_end = 1920 + 88 + 44, // hsync width 44
		.htotal = 2200,              // hactive + hblank(280)
		.vdisplay = 1080,
		.vsync_start = 1080 + 4,     // vsync offset 4
		.vsync_end = 1080 + 4 + 5,   // vsync width 5
		.vtotal = 1125,              // vactive + vblank(45)
		.bpc = 24,
		.head = PG106_DISP_HEAD,
		.sor = PG106_DISP_SOR,
		.hdmi_max_ac_packet = 0x1f, // matches gf119_sor_hdmi_ctrl mask
		.hdmi_rekey = 0,
		.interlace = false,
		.hsync_positive = true,      // CEA-861 DTD flags: positive
		.vsync_positive = true,
	};
	return m;
}

// Mock bus: SIMULATION recorder. Every apply() write lands here.
struct mock_bus {
	struct pg106_disp_step writes[PG106_DISP_STEPS_MAX];
	size_t n;
};

static void
bus_write(void *user, const struct pg106_disp_step *step)
{
	struct mock_bus *bus = user;
	bus->writes[bus->n++] = *step;
}

static size_t
find_step(const struct pg106_disp_step *steps, size_t n, const char *name)
{
	for (size_t i = 0; i < n; i++) {
		if (strcmp(steps[i].name, name) == 0)
			return i;
	}
	return (size_t)-1;
}

static const struct pg106_disp_step *
step_of(const struct pg106_disp_step *steps, size_t n, const char *name)
{
	size_t i = find_step(steps, n, name);
	static const struct pg106_disp_step none = { 0 };
	return i == (size_t)-1 ? &none : &steps[i];
}

static void
test_plan_shape_and_order(void)
{
	struct pg106_disp_step steps[PG106_DISP_STEPS_MAX];
	size_t n = pg106_disp_modeset_plan(&(struct pg106_disp_mode){ 0 },
					   steps, PG106_DISP_STEPS_MAX);

	CHECK(n == 0, "zeroed mode must be rejected (sync_end <= sync_start)");

	struct pg106_disp_mode mode = cea1080p60();
	n = pg106_disp_modeset_plan(&mode, steps, PG106_DISP_STEPS_MAX);
	CHECK(n == 23, "plan emits exactly 23 steps");

	// Ordering invariant 1: disable before reprogram.
	size_t dis = find_step(steps, n, "core.update-disable");
	size_t ras = find_step(steps, n, "head0.raster-size");
	CHECK(dis != (size_t)-1 && ras != (size_t)-1 && dis < ras,
	      "disable UPDATE precedes raster reprogramming");

	// Ordering invariant 2: enable is the very last step.
	size_t en = find_step(steps, n, "core.update-enable");
	CHECK(en == n - 1, "enable UPDATE is last");

	// Ordering invariant 3: SOR unhooked (owner-none) before re-owned,
	// and powered down before powered up.
	size_t onone = find_step(steps, n, "sor0.owner-none");
	size_t ohead = find_step(steps, n, "sor0.owner");
	size_t pdown = find_step(steps, n, "sor0.power-down");
	size_t pup = find_step(steps, n, "sor0.power-on");
	CHECK(onone < ohead && pdown < pup && ohead < pup && onone < pdown,
	      "SOR disable/enable lifecycle is ordered");

	// Ordering invariant 4: head control (depth/sync) before owner
	// assignment, mirroring head907d_or -> sor907d_ctrl ordering.
	CHECK(find_step(steps, n, "head0.output-resource") < ohead,
	      "head output-resource precedes SOR owner assignment");

	// Ordering invariant 5: VPLL before pixel clock programming.
	CHECK(find_step(steps, n, "vpll0.clock") < ras,
	      "VPLL set before raster programming");

	// Every step carries a non-empty source citation.
	int cited = 1;
	for (size_t i = 0; i < n; i++) {
		if (!steps[i].source || !steps[i].source[0] ||
		    !steps[i].name || !steps[i].name[0])
			cited = 0;
	}
	CHECK(cited, "every step has name + source citation");

	report("plan-shape-and-order");
}

static void
test_timing_roundtrip(void)
{
	struct pg106_disp_step steps[PG106_DISP_STEPS_MAX];
	struct pg106_disp_mode mode = cea1080p60();
	size_t n = pg106_disp_modeset_plan(&mode, steps, PG106_DISP_STEPS_MAX);
	CHECK(n == 23, "plan built");

	// RASTER_SIZE round-trips htotal/vtotal (HEIGHT<<16|WIDTH).
	const struct pg106_disp_step *rs =
		step_of(steps, n, "head0.raster-size");
	CHECK(rs->reg == 0x0414, "RASTER_SIZE at method 0x0414 (cl907d.h:136; readback 0x640414 gf119.c)");
	CHECK((rs->value & 0xffff) == 2200, "htotal 2200 round-trips");
	CHECK((rs->value >> 16) == 1125, "vtotal 1125 round-trips");

	// SYNC_END: hsynce = 2052-2008-1 = 43, vsynce = 1089-1084-1 = 4
	// (nv50-head.c nv50_head_atomic_check_mode math over CEA numbers).
	const struct pg106_disp_step *se =
		step_of(steps, n, "head0.raster-sync-end");
	CHECK(se->reg == 0x0418, "SYNC_END at 0x0418");
	CHECK((se->value & 0xffff) == 43, "hsynce 43 round-trips");
	CHECK((se->value >> 16) == 4, "vsynce 4 round-trips");

	// BLANK_END = total - sync_start - 1 (crtc_hblank_end == crtc_htotal):
	// hblanke = 2200-2008-1 = 191, vblanke = 1125-1084-1 = 40.
	const struct pg106_disp_step *be =
		step_of(steps, n, "head0.raster-blank-end");
	CHECK(be->reg == 0x041C, "BLANK_END at 0x041C");
	CHECK((be->value & 0xffff) == 191, "hblanke 191 round-trips");
	CHECK((be->value >> 16) == 40, "vblanke 40 round-trips");

	// BLANK_START = blanke + display: hblanks = 191+1920 = 2111,
	// vblanks = 40+1080 = 1120.
	const struct pg106_disp_step *bs =
		step_of(steps, n, "head0.raster-blank-start");
	CHECK(bs->reg == 0x0420, "BLANK_START at 0x0420");
	CHECK((bs->value & 0xffff) == 2111, "hblanks 2111 round-trips");
	CHECK((bs->value >> 16) == 1120, "vblanks 1120 round-trips");

	// Sanity of nouveau's "-1 into the sync pulse" counter bias: blank
	// start must land exactly at display-1 in absolute terms
	// (2008 + 2111 = 4119 = 2*2200 + 1919, hdisplay-1 = 1919).
	const uint32_t hblanks = bs->value & 0xffff;
	CHECK(((2008u + hblanks) % 2200u) == 1919u,
	      "hblank start lands at hdisplay-1 (1919) in absolute terms");

	// Pixel clock: 148500 kHz -> 148500000 Hz (0x08D9EE20) in HERTZ fields.
	const uint32_t expect_hz = 148500u * 1000u;
	const struct pg106_disp_step *pc =
		step_of(steps, n, "head0.pixel-clock-frequency");
	CHECK(pc->reg == 0x0450 && pc->value == expect_hz,
	      "pixel clock 148500000 Hz round-trips at 0x0450");
	CHECK(expect_hz == 0x08D9EE20u, "148500000 encodes as 0x08D9EE20");
	const struct pg106_disp_step *pcm =
		step_of(steps, n, "head0.pixel-clock-frequency-max");
	CHECK(pcm->reg == 0x0458 && pcm->value == expect_hz,
	      "pixel clock max matches at 0x0458");
	const struct pg106_disp_step *vpll = step_of(steps, n, "vpll0.clock");
	CHECK(vpll->kind == PG106_BLOCK && vpll->value == 148500,
	      "VPLL block op carries 148500 kHz (super_2_1 units)");

	// Depth: 24bpp -> BPP_24_444 (5) at bits 9:6; positive syncs -> 0.
	const struct pg106_disp_step *ores =
		step_of(steps, n, "head0.output-resource");
	CHECK(ores->reg == 0x0404, "OUTPUT_RESOURCE at 0x0404");
	CHECK(((ores->value >> 6) & 0xf) == 5, "PIXEL_DEPTH field 5 = BPP_24_444 (gf119.c readback case 5 -> 24)");
	CHECK((ores->value & 0x18) == 0,
	      "positive HSYNC/VSYNC polarity bits clear");

	// Head control magic + progressive + head 0 (head907d_or).
	const struct pg106_disp_step *hc = step_of(steps, n, "head0.control");
	CHECK(hc->reg == 0x0408 && hc->value == 0x31EC6000u,
	      "HEAD_SET_CONTROL = 0x31ec6000 | head0 << 25 | progressive");

	// SOR owner: HEAD0 + SINGLE_TMDS_A (gf119_sor_state proto_evo 1).
	const struct pg106_disp_step *own = step_of(steps, n, "sor0.owner");
	CHECK(own->reg == 0x0200 && own->value == 0x00000101u,
	      "SOR_SET_CONTROL = owner head0 + single-link TMDS-A");

	// SOR clock: low-speed TMDS 0x0a << 18 (gf119_sor_clock).
	const struct pg106_disp_step *sc = step_of(steps, n, "sor0.clock");
	CHECK(sc->reg == 0x612300 && sc->value == (0x0au << 18),
	      "SOR clock = TMDS speed 0x0a, dividers 0");

	// HDMI ctrl: enable bit + max_ac_packet<<16 | rekey.
	const struct pg106_disp_step *hdmic =
		step_of(steps, n, "sor0.hdmi-ctrl");
	CHECK(hdmic->reg == 0x616798 && hdmic->value == 0x401F0000u,
	      "HDMI_CTRL enable | max_ac_packet 0x1f | rekey 0");

	// UPDATE value: base0 interlock only (cl507d.h bit 1).
	const struct pg106_disp_step *upd =
		step_of(steps, n, "core.update-enable");
	CHECK(upd->reg == 0x0080 && upd->value == 0x2u,
	      "UPDATE carries BASE0 interlock bit");

	report("timing-roundtrip-cea1080p");
}

static void
test_mock_bus_and_diff(void)
{
	struct pg106_disp_step plan_a[PG106_DISP_STEPS_MAX];
	struct pg106_disp_step plan_b[PG106_DISP_STEPS_MAX];
	struct pg106_disp_mode mode = cea1080p60();
	size_t na = pg106_disp_modeset_plan(&mode, plan_a,
					    PG106_DISP_STEPS_MAX);
	size_t nb = pg106_disp_modeset_plan(&mode, plan_b,
					    PG106_DISP_STEPS_MAX);
	CHECK(na == nb && na == 23, "two runs produce same step count");

	// Determinism gate: two independent plans must diff clean.
	size_t first = (size_t)-1;
	CHECK(pg106_disp_modeset_diff(plan_a, na, plan_b, nb, &first) == 0,
	      "two runs of the same mode diff clean");

	// Mock bus replay: recorder sees every write, in order, intact.
	struct mock_bus bus = { 0 };
	pg106_disp_modeset_apply(plan_a, na, bus_write, &bus);
	CHECK(bus.n == na, "mock bus recorded all steps");
	int intact = 1;
	for (size_t i = 0; i < bus.n; i++) {
		if (bus.writes[i].reg != plan_a[i].reg ||
		    bus.writes[i].value != plan_a[i].value ||
		    bus.writes[i].kind != plan_a[i].kind)
			intact = 0;
	}
	CHECK(intact, "mock bus values round-trip the plan");

	// A one-value perturbation must be caught, at the right index.
	struct pg106_disp_step plan_c[PG106_DISP_STEPS_MAX];
	memcpy(plan_c, plan_a, na * sizeof(plan_c[0]));
	plan_c[7].value ^= 0x1u; // head0.raster-vert-blank2 vicinity
	CHECK(pg106_disp_modeset_diff(plan_a, na, plan_c, na, &first) == -1 &&
	      first == 7,
	      "perturbed plan diffs at the changed step");

	struct pg106_disp_step plan_d[PG106_DISP_STEPS_MAX];
	size_t nd = pg106_disp_modeset_plan(&mode, plan_d,
					    PG106_DISP_STEPS_MAX - 1);
	CHECK(nd == 0, "undersized buffer is rejected, not truncated");

	report("mock-bus-and-diff-gate");
}

int
main(void)
{
	test_plan_shape_and_order();
	test_timing_roundtrip();
	test_mock_bus_and_diff();

	if (failures) {
		printf("%d check(s) FAILED\n", failures);
		return 1;
	}
	printf("all disp mode-set checks passed\n");
	return 0;
}
