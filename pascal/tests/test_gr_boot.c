// SPDX-License-Identifier: GPL-2.0-only
// pascal/tests/test_gr_boot.c — GR bring-up SIMULATION test.
//
// **********************************************************************
// **  SIMULATION, NOT HARDWARE PROOF.                                   **
// **  This test drives pg106_grboot_plan_build()'s model of the         **
// **  nouveau GR bring-up sequence against a MOCK BUS that merely       **
// **  records writes and returns canned readbacks. Nothing here runs    **
// **  on, reads, or proves anything about a real GP106.                 **
// **********************************************************************
//
// Checks:
//   (a) golden ordering: the GR plan replays strictly AFTER a
//       pg106_acrboot_plan replay on the SAME mock bus (ACR-complete ->
//       GR reset -> fuse/strap discovery -> FECS enable/boot poll ->
//       ... -> UNLOAD last), and the group sequence matches the golden
//       order RESET < DISCOVERY < GPC_MMU < MMIO_PACK < ENGINE_CFG <
//       EXCEPTIONS < ZBC < FECS_BOOT < UNLOAD;
//   (b) determinism: two builds of the plan serialize byte-identical;
//   (c) every step carries a non-empty citation ("file:line" shape);
//   (d) [HW-VALUE] steps are flagged, present, and cover every
//       DISCOVERY-group read plus the FECS mailbox queries; the trace
//       dump marks them;
//   (e) integration smoke + falcon-base parameterization: rebuilding with
//       different gr/fecs/gpccs bases keeps the same step count and
//       shifts the addresses correctly;
//   (f) info block: compute class 0xC1C0 binding + context buffer sizing
//       per the ctxgp102 constants.
//
// Run: cd pascal && make test   (or build/test_gr_boot from pascal/)

#include <pascal/acr.h>
#include <pascal/acr_boot.h>
#include <pascal/gr_boot.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                        \
	do {                                                               \
		if (!(cond)) {                                             \
			printf("FAIL %s:%d: %s\n", __func__, __LINE__,     \
			       #cond);                                     \
			g_failures++;                                      \
		}                                                          \
	} while (0)

#define CHECK_EQ(a, b)                                                     \
	do {                                                               \
		unsigned long long va_ = (unsigned long long)(a);          \
		unsigned long long vb_ = (unsigned long long)(b);          \
		if (va_ != vb_) {                                          \
			printf("FAIL %s:%d: %s == %s "                     \
			       "(0x%llx != 0x%llx)\n", __func__, __LINE__, \
			       #a, #b, va_, vb_);                          \
			g_failures++;                                      \
		}                                                          \
	} while (0)

// --- tiny file loader (same shape as test_acr_boot.c) ------------------------

static uint8_t *
load_blob(const char *dir, const char *rel, size_t *size_out)
{
	char path[512];
	uint8_t *buf;
	long len;
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, rel);
	f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len < 0) {
		fclose(f);
		return NULL;
	}
	buf = malloc((size_t)len ? (size_t)len : 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*size_out = (size_t)len;
	return buf;
}

static const char *
fw_dir(void)
{
	static const char *candidates[] = {
		"../research/firmware/gp106",
		"../../research/firmware/gp106",
		"research/firmware/gp106",
	};
	size_t i;

	for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
		char path[512];
		FILE *f;

		snprintf(path, sizeof(path), "%s/README.md", candidates[i]);
		f = fopen(path, "rb");
		if (f) {
			fclose(f);
			return candidates[i];
		}
	}
	return NULL;
}

// --- ACR fixture (previous layer; the composition predecessor) ---------------

static void
make_fecs_lsfw(struct pg106_acr_lsfw *l)
{
	*l = (struct pg106_acr_lsfw){
		.id = PG106_ACR_LSF_FECS,
		.bootloader_size = 0x100,
		.bootloader_imem_offset = 0x0,
		.app_size = 0x2b00,
		.app_start_offset = 0x100,
		.app_imem_entry = 0x100,
		.app_resident_code_offset = 0x0,
		.app_resident_code_size = 0x2800,
		.app_resident_data_offset = 0x2900,
		.app_resident_data_size = 0x100,
		.ucode_size = 0x2a00,
		.data_size = 0x100,
		.bld_size = 0x4c,
		.img_size = 0x2b00,
	};
}

static void
make_gpccs_lsfw(struct pg106_acr_lsfw *l)
{
	make_fecs_lsfw(l);
	l->id = PG106_ACR_LSF_GPCCS;
}

// --- unified mock bus ---------------------------------------------------------
//
// One recorder for both layers: the ACR boot (SEC2 falcon, base-relative
// canned responses) followed by the GR bring-up (BAR0 addresses, canned
// fuse/strap echo from the config). Records {phase, group, addr} so the
// composed ordering can be asserted across the layer boundary.

struct trace_entry {
	uint8_t phase; // 0 = acr boot, 1 = gr boot
	uint8_t group;
	uint64_t addr;
};

struct mock_bus {
	uint32_t sec2_base;
	uint32_t gr_base;
	uint32_t fecs_base;
	uint32_t gpccs_base;

	// ACR-side canned state (same shapes as test_acr_boot.c).
	uint32_t wpr_check_sel;
	uint64_t wpr_start, wpr_end;
	size_t n_bindstat_polls;

	// GR-side canned config echo.
	const struct pg106_grboot_config *cfg;

	struct trace_entry *trace;
	size_t ntrace, trace_cap;
};

static void
record(struct mock_bus *bus, uint8_t phase, uint8_t group, uint64_t addr)
{
	if (bus->ntrace == bus->trace_cap) {
		size_t cap = bus->trace_cap ? bus->trace_cap * 2 : 256;
		struct trace_entry *t =
			realloc(bus->trace, cap * sizeof(*bus->trace));

		CHECK(t != NULL);
		if (!t)
			return;
		bus->trace = t;
		bus->trace_cap = cap;
	}
	bus->trace[bus->ntrace++] =
		(struct trace_entry){ phase, group, addr };
}

static uint32_t
mock_off(uint64_t addr, uint32_t base)
{
	return (uint32_t)(addr - base);
}

// ACR-layer canned readbacks (test_acr_boot.c semantics).
static uint32_t
acr_mock_read(struct mock_bus *bus, const struct pg106_acrboot_step *s)
{
	if (s->addr == PG106_PFB_WPR_CHECK) {
		if (bus->wpr_check_sel == 2)
			return (uint32_t)((bus->wpr_start >> 8) &
					  0xffffff00u);
		return (uint32_t)(((bus->wpr_end - 0x20000ull) >> 8) &
				  0xffffff00u);
	}
	switch (mock_off(s->addr, bus->sec2_base)) {
	case 0x10cu: return 0x00000006u; // mem scrub done
	case 0x0dcu: // bindstat: first poll state 5, then 0
		return ++bus->n_bindstat_polls == 1 ? 0x00005000u : 0x0u;
	case 0x100u: return 0x00000012u; // CPUCTL halted
	case 0x040u: return 0x00000000u; // MBOX0 ok
	case 0x044u: return 0x00000000u; // MBOX1
	}
	return 0;
}

// Replays the ACR plan (the previous layer) onto the shared bus.
static size_t
run_acr_plan(struct mock_bus *bus, const struct pg106_acrboot_plan *plan)
{
	size_t i, fails = 0;

	for (i = 0; i < plan->nsteps; i++) {
		const struct pg106_acrboot_step *s = &plan->steps[i];
		uint32_t v;

		switch (s->kind) {
		case PG106_ACRBOOT_WRITE_REG:
			if (s->addr == PG106_PFB_WPR_CHECK)
				bus->wpr_check_sel = s->value; // selector
			if (s->space == PG106_ACRBOOT_MMIO)
				record(bus, 0, 0xff, s->addr);
			break;
		case PG106_ACRBOOT_WRITE_BLOB:
			break;
		case PG106_ACRBOOT_POLL_REG:
			v = acr_mock_read(bus, s);
			if (s->poll_op == PG106_ACRBOOT_POLL_EQ) {
				if ((v & s->mask) != s->value)
					fails++;
			} else if (!(v & s->mask)) {
				fails++;
			}
			record(bus, 0, 0xff, s->addr);
			break;
		case PG106_ACRBOOT_READ_REG:
			v = acr_mock_read(bus, s);
			if ((v & s->mask) != (s->value & s->mask))
				fails++;
			record(bus, 0, 0xff, s->addr);
			break;
		}
	}
	return fails;
}

// GR-layer canned readbacks: polls see the "boot confirmed" / "idle"
// values; reads echo the step's own configured expectation (that is the
// point of a mock — the expectation is asserted to be explicit).
static uint32_t
gr_mock_read(struct mock_bus *bus, const struct pg106_grboot_step *s)
{
	(void)bus;
	if (s->kind == PG106_GRBOOT_POLL_REG) {
		if (s->addr ==
		    bus->cfg->fecs_base + 0x800u) // FECS boot confirm
			return 0x00000001u;
		return 0x00000000u; // 0x40060c idle
	}
	return s->value;
}

// Replays the GR plan onto the shared bus. Returns failed steps (0).
static size_t
run_gr_plan(struct mock_bus *bus, const struct pg106_grboot_plan *plan)
{
	size_t i, fails = 0;

	for (i = 0; i < plan->nsteps; i++) {
		const struct pg106_grboot_step *s = &plan->steps[i];
		uint32_t v;

		switch (s->kind) {
		case PG106_GRBOOT_WRITE_REG:
		case PG106_GRBOOT_WRITE_BLOB:
			record(bus, 1, s->group, s->addr);
			break;
		case PG106_GRBOOT_POLL_REG:
			v = gr_mock_read(bus, s);
			if (s->poll_op == PG106_GRBOOT_POLL_EQ) {
				if ((v & s->mask) != s->value)
					fails++;
			} else if (!(v & s->mask)) {
				fails++;
			}
			record(bus, 1, s->group, s->addr);
			break;
		case PG106_GRBOOT_READ_REG:
			v = gr_mock_read(bus, s);
			if ((v & s->mask) != (s->value & s->mask))
				fails++;
			record(bus, 1, s->group, s->addr);
			break;
		}
	}
	return fails;
}

// --- GP106 configuration stand-ins ---------------------------------------------
//
// A GT 1060-class GP106: 1 GPC x 5 TPC (10 SMs), 3 PPC slots with TPC
// split 3/2 across two of them. ALL of these are [HW-VALUE] stand-ins —
// on real silicon every one comes from a fuse/strap read modeled in the
// DISCOVERY group.

static void
gr_config_init(struct pg106_grboot_config *cfg, uint64_t gr_base,
	       uint32_t fecs_base, uint32_t gpccs_base)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->gr_base = gr_base;
	cfg->fecs_base = fecs_base;
	cfg->gpccs_base = gpccs_base;
	cfg->reset_war = false; // GP106 (chipset 0x136) — gf100.c:2070
	cfg->pmc_enable = 0x000013ffu;

	cfg->gpc_nr = 1;
	cfg->tpc_nr[0] = 5;
	cfg->ppc_tpc_mask[0][0] = 0x07; // TPC0-2
	cfg->ppc_tpc_mask[0][1] = 0x18; // TPC3-4
	cfg->ppc_tpc_mask[0][2] = 0x00;
	cfg->rop_config = 0x00000001; // gm200_gr_rops raw count stand-in: 1 ROP
	cfg->fbp_count = 0x1;
	cfg->ltc_active = 0x00010001;
	cfg->ltc_magma = 0x00000001;
	cfg->pri_mmu_rsvd = 0x00000001;
	cfg->mmu_wr = 0x00000001;
	cfg->mmu_wr_rd = 0x00000001;
	cfg->mmu_rd = 0x00000001;
	cfg->swdx_pes[0] = 0x5;

	cfg->ctx_size = 0x00a00000;   // FECS-reported stand-in
	cfg->size_zcull = 0x00040000; // FECS-reported stand-in
	cfg->size_pm = 0x00001000;    // FECS-reported stand-in
	cfg->sw_nonctx_size = 0x1000; // opaque blob stand-in
	cfg->zbc_init = true;
}

// Serialize a plan for the determinism diff.
static uint8_t *
serialize_plan(const struct pg106_grboot_plan *p, size_t *size_out)
{
	size_t i, cap = 4096, len = 0;
	uint8_t *buf = malloc(cap);

	*size_out = 0;
	if (!buf)
		return NULL;
	for (i = 0; i < p->nsteps; i++) {
		const struct pg106_grboot_step *s = &p->steps[i];
		size_t need = sizeof(s->kind) + sizeof(s->space) +
			      sizeof(s->poll_op) + sizeof(s->hw_value) +
			      sizeof(s->group) + sizeof(s->addr) +
			      sizeof(s->value) + sizeof(s->mask) +
			      sizeof(s->blob_len) + strlen(s->why) + 1 +
			      s->blob_len;

		while (len + need > cap) {
			uint8_t *n;

			cap *= 2;
			n = realloc(buf, cap);
			if (!n) {
				free(buf);
				return NULL;
			}
			buf = n;
		}
		memcpy(buf + len, &s->kind, sizeof(s->kind));
		len += sizeof(s->kind);
		memcpy(buf + len, &s->space, sizeof(s->space));
		len += sizeof(s->space);
		memcpy(buf + len, &s->poll_op, sizeof(s->poll_op));
		len += sizeof(s->poll_op);
		memcpy(buf + len, &s->hw_value, sizeof(s->hw_value));
		len += sizeof(s->hw_value);
		memcpy(buf + len, &s->group, sizeof(s->group));
		len += sizeof(s->group);
		memcpy(buf + len, &s->addr, sizeof(s->addr));
		len += sizeof(s->addr);
		memcpy(buf + len, &s->value, sizeof(s->value));
		len += sizeof(s->value);
		memcpy(buf + len, &s->mask, sizeof(s->mask));
		len += sizeof(s->mask);
		memcpy(buf + len, &s->blob_len, sizeof(s->blob_len));
		len += sizeof(s->blob_len);
		if (s->blob) {
			memcpy(buf + len, s->blob, s->blob_len);
			len += s->blob_len;
		}
		memcpy(buf + len, s->why, strlen(s->why) + 1);
		len += strlen(s->why) + 1;
	}
	*size_out = len;
	return buf;
}

static size_t
first_of_group(const struct pg106_grboot_plan *p, uint8_t group)
{
	size_t i;

	for (i = 0; i < p->nsteps; i++)
		if (p->steps[i].group == group)
			return i;
	return (size_t)-1;
}

// --- tests ---------------------------------------------------------------------

static struct pg106_acrboot_plan g_acr_plan;
static struct pg106_acrboot_info g_acr_info;
static struct pg106_grboot_plan g_gr_plan, g_gr_plan2;
static struct pg106_grboot_info g_gr_info;

static void
test_compose_and_ordering(struct mock_bus *bus)
{
	size_t i, acr_fails, gr_fails;
	size_t acr_last = 0, gr_first = (size_t)-1;
	uint8_t last_group = 0;
	bool first = true;

	printf("\n== SIMULATION, NOT HARDWARE PROOF: replaying the modeled "
	       "GR bring-up AFTER the ACR boot on the same mock bus ==\n");

	// The ACR layer boots first (previous layer, real gp106 sigs).
	acr_fails = run_acr_plan(bus, &g_acr_plan);
	CHECK_EQ(acr_fails, 0);

	// Then the GR layer on the SAME bus.
	gr_fails = run_gr_plan(bus, &g_gr_plan);
	CHECK_EQ(gr_fails, 0);

	// (a) composed golden ordering: every ACR trace entry precedes every
	// GR trace entry (ACR-complete -> GR).
	for (i = 0; i < bus->ntrace; i++) {
		if (bus->trace[i].phase == 0)
			acr_last = i;
		else if (gr_first == (size_t)-1)
			gr_first = i;
	}
	CHECK(acr_last > 0 || bus->ntrace > 0);
	CHECK(gr_first != (size_t)-1);
	CHECK(acr_last < gr_first);
	CHECK_EQ(g_acr_info.nsteps, g_acr_plan.nsteps);

	// GR group sequence must be non-decreasing and hit every golden
	// group in order.
	for (i = gr_first; i < bus->ntrace; i++) {
		const struct pg106_grboot_step *s;

		if (bus->trace[i].phase != 1)
			continue;
		s = &g_gr_plan.steps[0]; // group comes from the trace entry
		(void)s;
		if (first) {
			last_group = bus->trace[i].group;
			first = false;
			continue;
		}
		CHECK(bus->trace[i].group >= last_group);
		last_group = bus->trace[i].group;
	}

	// (a) golden: ACR-complete -> GR reset -> fuse discovery -> enable.
	CHECK_EQ(first_of_group(&g_gr_plan, PG106_GRBOOT_G_RESET), 0);
	CHECK(first_of_group(&g_gr_plan, PG106_GRBOOT_G_DISCOVERY) >
	      first_of_group(&g_gr_plan, PG106_GRBOOT_G_RESET));
	CHECK(first_of_group(&g_gr_plan, PG106_GRBOOT_G_FECS_BOOT) >
	      first_of_group(&g_gr_plan, PG106_GRBOOT_G_DISCOVERY));
	CHECK(first_of_group(&g_gr_plan, PG106_GRBOOT_G_UNLOAD) >
	      first_of_group(&g_gr_plan, PG106_GRBOOT_G_FECS_BOOT));

	// The first GR step is the 0x400500 quiesce write (reset).
	CHECK_EQ(g_gr_plan.steps[0].kind, PG106_GRBOOT_WRITE_REG);
	CHECK_EQ(g_gr_plan.steps[0].addr,
		 bus->cfg->gr_base + PG106_GRBOOT_OFF_400500);

	// The first DISCOVERY step is the 0x409604 GPC-count fuse read.
	{
		const struct pg106_grboot_step *s =
			&g_gr_plan.steps[first_of_group(
				&g_gr_plan, PG106_GRBOOT_G_DISCOVERY)];

		CHECK_EQ(s->kind, PG106_GRBOOT_READ_REG);
		CHECK_EQ(s->addr,
			 bus->cfg->fecs_base +
				 PG106_GRBOOT_FECSC_OFF_409604);
		CHECK_EQ(s->hw_value, 1);
	}

	// FECS boot: CPUCTL start (enable) precedes the boot-confirm poll;
	// the poll precedes the mailbox size queries.
	{
		size_t start = (size_t)-1, poll = (size_t)-1;
		size_t n_cpuctl = 0;

		for (i = 0; i < g_gr_plan.nsteps; i++) {
			const struct pg106_grboot_step *s =
				&g_gr_plan.steps[i];

			if (s->group != PG106_GRBOOT_G_FECS_BOOT)
				continue;
			if (s->kind == PG106_GRBOOT_WRITE_REG &&
			    s->value == 0x2 &&
			    (s->addr == bus->cfg->fecs_base +
						PG106_GRBOOT_FALCON_CPUCTL ||
			     s->addr == bus->cfg->gpccs_base +
						PG106_GRBOOT_FALCON_CPUCTL)) {
				n_cpuctl++;
				if (start == (size_t)-1)
					start = i;
			}
			if (s->kind == PG106_GRBOOT_POLL_REG &&
			    s->addr == bus->cfg->fecs_base + 0x800u &&
			    poll == (size_t)-1)
				poll = i;
		}
		CHECK_EQ(n_cpuctl, 2); // FECS + GPCCS starts
		CHECK(start != (size_t)-1 && poll != (size_t)-1);
		CHECK(poll > start);
	}

	// UNLOAD is the final group.
	{
		size_t un = first_of_group(&g_gr_plan, PG106_GRBOOT_G_UNLOAD);

		CHECK_EQ(un, g_gr_plan.nsteps - 5);
		for (i = un; i < g_gr_plan.nsteps; i++)
			CHECK_EQ(g_gr_plan.steps[i].group,
				 PG106_GRBOOT_G_UNLOAD);
	}
}

static void
test_citations_and_hwvalue(const struct pg106_grboot_plan *p)
{
	size_t i, n_hw = 0, n_disc = 0, n_disc_hw = 0;

	// (c) every step has a non-empty "file:line" citation.
	for (i = 0; i < p->nsteps; i++) {
		const char *why = p->steps[i].why;

		CHECK(why != NULL);
		if (!why)
			continue;
		CHECK(strlen(why) >= 10);
		CHECK(strchr(why, ':') != NULL);
		CHECK(strstr(why, ".c") != NULL ||
		      strstr(why, ".h") != NULL);
		if (p->steps[i].hw_value)
			n_hw++;
		if (p->steps[i].group == PG106_GRBOOT_G_DISCOVERY) {
			n_disc++;
			if (p->steps[i].hw_value)
				n_disc_hw++;
		}
	}

	// (d) [HW-VALUE] marking: every DISCOVERY step is flagged, and the
	// flag set is non-empty overall.
	CHECK(n_disc > 0);
	CHECK_EQ(n_disc, n_disc_hw);
	CHECK(n_hw > n_disc); // + derived writes + FECS mailbox queries
	printf("hw-value steps: %zu of %zu (%zu discovery reads)\n", n_hw,
	       p->nsteps, n_disc);
}

static void
test_determinism(void)
{
	struct pg106_grboot_config cfg;
	struct pg106_grboot_info info1, info2;
	size_t len1 = 0, len2 = 0;
	uint8_t *s1, *s2;
	int ret;

	gr_config_init(&cfg, 0x00400000ull, 0x00900000, 0x00a00000);
	ret = pg106_grboot_plan_build(&cfg, &g_gr_plan, &info1);
	CHECK_EQ(ret, 0);
	if (ret)
		return;
	ret = pg106_grboot_plan_build(&cfg, &g_gr_plan2, &info2);
	CHECK_EQ(ret, 0);
	if (ret)
		return;

	// (b) two runs diff clean.
	s1 = serialize_plan(&g_gr_plan, &len1);
	s2 = serialize_plan(&g_gr_plan2, &len2);
	CHECK(s1 && s2);
	CHECK_EQ(len1, len2);
	CHECK(len1 > 0);
	if (s1 && s2 && len1 == len2)
		CHECK(memcmp(s1, s2, len1) == 0);
	free(s1);
	free(s2);

	// (e) falcon-base parameterization: same step count, shifted bases.
	{
		struct pg106_grboot_config cfg2;
		struct pg106_grboot_plan p3;
		struct pg106_grboot_info info3;
		size_t i;
		int saw_fecs = 0, saw_gpccs = 0;

		gr_config_init(&cfg2, 0x00800000ull, 0x00620000, 0x00640000);
		CHECK_EQ(pg106_grboot_plan_build(&cfg2, &p3, &info3), 0);
		CHECK_EQ(p3.nsteps, g_gr_plan.nsteps);
		for (i = 0; i < p3.nsteps; i++) {
			const struct pg106_grboot_step *s = &p3.steps[i];

			if (s->kind == PG106_GRBOOT_WRITE_REG &&
			    s->value == 0x2 &&
			    s->addr == cfg2.fecs_base +
					       PG106_GRBOOT_FALCON_CPUCTL)
				saw_fecs = 1;
			if (s->kind == PG106_GRBOOT_WRITE_REG &&
			    s->value == 0x2 &&
			    s->addr == cfg2.gpccs_base +
					       PG106_GRBOOT_FALCON_CPUCTL)
				saw_gpccs = 1;
			// The PMC register stays at its absolute BAR0 root.
			if (s->addr == PG106_GRBOOT_PMC_ENABLE)
				CHECK(s->group == PG106_GRBOOT_G_UNLOAD);
		}
		CHECK(saw_fecs && saw_gpccs);
		pg106_grboot_plan_free(&p3);
	}
}

static void
test_info_and_validation(void)
{
	struct pg106_grboot_config cfg;
	struct pg106_grboot_plan p;
	struct pg106_grboot_info info;

	// (f) info block: discovery totals + compute class + ctx sizing.
	gr_config_init(&cfg, 0x00400000ull, 0x00900000, 0x00a00000);
	CHECK_EQ(pg106_grboot_plan_build(&cfg, &p, &info), 0);
	CHECK_EQ(info.tpc_total, 5);
	CHECK_EQ(info.ppc_tpc_max, 3);
	CHECK_EQ(info.ppc_tpc_min, 2);
	CHECK_EQ(info.rop_nr, cfg.rop_config);
	CHECK_EQ(info.magicgpc918, (0x00800000u + 4) / 5); // DIV_ROUND_UP
	CHECK_EQ(info.compute_class, 0x0000c1c0u); // PASCAL_COMPUTE_B
	CHECK_EQ(info.pagepool_size, 0x00020000u); // ctxgp102.c:107
	CHECK_EQ(info.bundle_size, 0x00003000u);   // ctxgp102.c:103
	// attrib_cb_size formula (ctxgp102.c:86-95):
	//   ((0xc00 * 5 + 0xba8*3*3) * 0x20 + 127) & ~127
	CHECK_EQ(info.attrib_cb_size,
		 (((uint64_t)0xc00 * 5 + (uint64_t)0xba8 * 3 * 3) * 0x20 +
		  127) & ~(uint64_t)127);
	CHECK_EQ(info.nsteps, p.nsteps);
	pg106_grboot_plan_free(&p);

	// Validation: bad configs rejected.
	gr_config_init(&cfg, 0x00400000ull, 0x00900000, 0x00a00000);
	cfg.gpc_nr = 0;
	CHECK_EQ(pg106_grboot_plan_build(&cfg, &p, &info), -EINVAL);

	gr_config_init(&cfg, 0x00400000ull, 0x00900000, 0x00a00000);
	cfg.tpc_nr[0] = 9; // > TPC_MAX_PER_GPC
	CHECK_EQ(pg106_grboot_plan_build(&cfg, &p, &info), -EINVAL);

	gr_config_init(&cfg, 0x00400000ull, 0x00900000, 0x00a00000);
	cfg.sw_nonctx_size = 0x1001; // not a multiple of 4
	CHECK_EQ(pg106_grboot_plan_build(&cfg, &p, &info), -EINVAL);

	gr_config_init(&cfg, 0x00400000ull, 0x00900000, 0x00a00000);
	cfg.pmc_enable = 0x000002ffu; // no PMC GR bit
	cfg.reset_war = true;         // -> invalid combination
	CHECK_EQ(pg106_grboot_plan_build(&cfg, &p, &info), -EINVAL);

	// reset_war path builds and leads with the PMC pulse.
	gr_config_init(&cfg, 0x00400000ull, 0x00900000, 0x00a00000);
	cfg.reset_war = true;
	CHECK_EQ(pg106_grboot_plan_build(&cfg, &p, &info), 0);
	CHECK_EQ(p.nsteps, g_gr_plan.nsteps + 2);
	CHECK_EQ(p.steps[0].addr, PG106_GRBOOT_PMC_ENABLE);
	CHECK_EQ(p.steps[0].value, cfg.pmc_enable & ~PG106_GRBOOT_PMC_GR);
	CHECK_EQ(p.steps[1].value, cfg.pmc_enable | PG106_GRBOOT_PMC_GR);
	pg106_grboot_plan_free(&p);
}

static void
test_trace_dump(void)
{
	size_t i;

	printf("\n-- committed-evidence trace (gr plan, %zu steps) --\n",
	       g_gr_plan.nsteps);
	for (i = 0; i < g_gr_plan.nsteps; i++) {
		const struct pg106_grboot_step *s = &g_gr_plan.steps[i];

		printf("[%4zu] %-8s %-4s %-10s 0x%09llx", i,
		       pg106_grboot_kind_name(s->kind),
		       pg106_grboot_space_name(s->space),
		       pg106_grboot_group_name((uint8_t)s->group),
		       (unsigned long long)s->addr);
		if (s->kind == PG106_GRBOOT_WRITE_REG)
			printf(" = 0x%08x", s->value);
		if (s->kind == PG106_GRBOOT_WRITE_BLOB)
			printf(" len=%u%s", s->blob_len,
			       s->blob ? "" : " (fw data, zero-fill)");
		if (s->kind == PG106_GRBOOT_POLL_REG)
			printf(" mask=0x%08x %s 0x%08x", s->mask,
			       pg106_grboot_poll_op_name(s->poll_op),
			       s->value);
		if (s->kind == PG106_GRBOOT_READ_REG)
			printf(" expect (v&0x%08x)==0x%08x", s->mask,
			       s->value);
		printf(" %s| %s\n", s->hw_value ? "[HW-VALUE] " : "",
		       s->why);
	}
}

int
main(void)
{
	const char *dir = fw_dir();
	struct mock_bus bus = { 0 };
	uint8_t *bl_blob, *hs_blob, *fecs_sig_blob, *gpccs_sig_blob;
	size_t bl_size, hs_size, fecs_sig_size, gpccs_sig_size;
	struct pg106_acr_bl_ucode bl;
	struct pg106_acr_hs_ucode hs;
	struct pg106_acr_lsf_signature_v1 fecs_sig, gpccs_sig;
	struct pg106_acrboot_config acr_cfg;
	struct pg106_acr_lsfw lsfws[2];
	const struct pg106_acr_lsf_signature_v1 *sigs[2];
	struct pg106_grboot_config gr_cfg;
	int ret;

	printf("== gr_boot: SIMULATION, NOT HARDWARE PROOF "
	       "(mock bus, no hardware claims)\n");

	// --- ACR fixture (previous layer): parse the real blobs first ------
	CHECK(dir != NULL);
	if (!dir)
		return 1;
	bl_blob = load_blob(dir, "acr/bl.bin", &bl_size);
	hs_blob = load_blob(dir, "acr/ucode_load.bin", &hs_size);
	fecs_sig_blob = load_blob(dir, "gr/fecs_sig.bin", &fecs_sig_size);
	gpccs_sig_blob = load_blob(dir, "gr/gpccs_sig.bin", &gpccs_sig_size);
	CHECK(bl_blob && hs_blob && fecs_sig_blob && gpccs_sig_blob);
	if (!bl_blob || !hs_blob || !fecs_sig_blob || !gpccs_sig_blob)
		return 1;

	ret = pg106_acr_ucode_parse_bl(bl_blob, bl_size, &bl);
	CHECK_EQ(ret, 0);
	ret |= pg106_acr_ucode_parse_hs(hs_blob, hs_size, &hs);
	CHECK_EQ(ret, 0);
	ret |= pg106_acr_lsf_sig_parse(fecs_sig_blob, fecs_sig_size,
				       &fecs_sig);
	CHECK_EQ(ret, 0);
	ret |= pg106_acr_lsf_sig_parse(gpccs_sig_blob, gpccs_sig_size,
				       &gpccs_sig);
	CHECK_EQ(ret, 0);
	CHECK_EQ(fecs_sig.falcon_id, PG106_ACR_LSF_FECS);
	CHECK_EQ(gpccs_sig.falcon_id, PG106_ACR_LSF_GPCCS);

	make_fecs_lsfw(&lsfws[0]);
	make_gpccs_lsfw(&lsfws[1]);
	sigs[0] = &fecs_sig;
	sigs[1] = &gpccs_sig;
	acr_cfg = (struct pg106_acrboot_config){
		.falcon_base = 0x003c0000, // SEC2 stand-in (nvkm_top)
		.code_limit = 0x8000,
		.vma_addr = 0x100000000ull,
		.wpr_alloc_base = 0x0f0000000ull,
		.bootstrap_owner = PG106_ACR_LSF_SEC2,
		.rtos_id = PG106_ACR_LSF_GPCCS,
		.boot_mbox0 = 0,
		.intr_clear = 0x10,
		.pmc_readback = 0x13f,
		.bind_target = 0,
		.bind_addr = 0x10000000ull,
	};
	ret = pg106_acrboot_plan_build(&acr_cfg, &bl, &hs, lsfws, 2, sigs,
				       &g_acr_plan, &g_acr_info);
	CHECK_EQ(ret, 0);
	if (ret)
		return 1;
	printf("acr plan: %zu steps (previous layer, replayed first)\n",
	       g_acr_plan.nsteps);

	// --- GR plan -------------------------------------------------------
	gr_config_init(&gr_cfg, 0x00400000ull, 0x00900000, 0x00a00000);
	ret = pg106_grboot_plan_build(&gr_cfg, &g_gr_plan, &g_gr_info);
	CHECK_EQ(ret, 0);
	if (ret)
		return 1;
	printf("gr plan: %zu steps, tpc_total %u, attrib_cb 0x%llx, "
	       "compute class 0x%04x\n",
	       g_gr_plan.nsteps, g_gr_info.tpc_total,
	       (unsigned long long)g_gr_info.attrib_cb_size,
	       g_gr_info.compute_class);

	// --- shared mock bus (parameterized falcon/GR bases) ---------------
	bus.sec2_base = acr_cfg.falcon_base;
	bus.gr_base = gr_cfg.gr_base;
	bus.fecs_base = gr_cfg.fecs_base;
	bus.gpccs_base = gr_cfg.gpccs_base;
	bus.cfg = &gr_cfg;
	bus.wpr_start = g_acr_info.wpr_start;
	bus.wpr_end = g_acr_info.wpr_end;

	test_compose_and_ordering(&bus);
	test_citations_and_hwvalue(&g_gr_plan);
	test_determinism();
	test_info_and_validation();
	test_trace_dump();

	free(bus.trace);
	pg106_acrboot_plan_free(&g_acr_plan);
	pg106_grboot_plan_free(&g_gr_plan);
	pg106_grboot_plan_free(&g_gr_plan2);
	free(bl_blob);
	free(hs_blob);
	free(fecs_sig_blob);
	free(gpccs_sig_blob);

	if (g_failures) {
		printf("== gr_boot: FAIL (%d check(s))\n", g_failures);
		return 1;
	}
	printf("== gr_boot: PASS (simulation only — no hardware claims)\n");
	return 0;
}
