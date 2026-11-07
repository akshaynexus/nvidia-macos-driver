// SPDX-License-Identifier: GPL-2.0-only
// pascal/tests/test_gr_boot.c — GR bring-up SIMULATION test.
//
// **********************************************************************
// **  SIMULATION, NOT HARDWARE PROOF.                                   **
// **  pg106_grboot_plan_build()'s step list is replayed against a MOCK  **
// **  GR register file. Nothing here runs on, reads, or proves          **
// **  anything about a real GP106.                                      **
// **********************************************************************
//
// The mock is a register file with nouveau's register semantics, not an
// echo of the plan's expectations:
//   * fuse/strap registers hold a [HW-VALUE] stand-in "chip" (1 GPC x 5
//     TPC); discovery READs are checked against it;
//   * MASK steps are real read-modify-writes, so pre-set bits outside the
//     mask survive (and a plain write would lose them — mutation test);
//   * 0x40060c reads busy for a while before going idle;
//   * the FECS mailbox 0x409800 answers a size method (0x10/0x16/0x25 at
//     0x409504) only after a few reads, like a running ucode would — a
//     single read (the old model) sees 0 (mutation test);
//   * FECS reports ready (0x409800 bit 0) only if the SEC2 bootstrap ran
//     before the falcons were started (info.bootstrap_before).
// The ACR/SEC2 side itself is covered by test_acr_boot.c.
//
// Run: cd pascal && make test

#include <pascal/gr_boot.h>

#include <errno.h>
#include <stdbool.h>
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

// --- mock GR register file -------------------------------------------------------

#define MOCK_NREG 4096
#define FECS_ANSWER_READS 3 // SIM: FECS needs 3 polls to answer a method
#define IDLE_READS 4        // SIM: GR busy for 4 reads of 0x40060c

struct gr_mock {
	const struct pg106_grboot_config *cfg;
	uint64_t addr[MOCK_NREG];
	uint32_t val[MOCK_NREG];
	size_t n;
	// FECS mailbox model
	uint32_t pending_mthd;
	int answer_in;
	bool bootstrapped, fecs_started, gpccs_started;
	int idle_left;
	// counters
	size_t pmc_writes, tpc_48c_writes, ltc_index_masks, writes;
};

static uint32_t *
slot(struct gr_mock *m, uint64_t addr, bool create)
{
	size_t i;

	for (i = 0; i < m->n; i++)
		if (m->addr[i] == addr)
			return &m->val[i];
	if (!create || m->n == MOCK_NREG)
		return NULL;
	m->addr[m->n] = addr;
	m->val[m->n] = 0;
	return &m->val[m->n++];
}

static uint32_t
get(struct gr_mock *m, uint64_t addr)
{
	uint32_t *p = slot(m, addr, false);

	return p ? *p : 0;
}

static void
put(struct gr_mock *m, uint64_t addr, uint32_t v)
{
	uint32_t *p = slot(m, addr, true);

	if (p)
		*p = v;
}

static uint64_t
gpc_reg(const struct pg106_grboot_config *c, uint32_t g, uint32_t off)
{
	return c->gr_base + PG106_GRBOOT_GPC_BASE +
	       (uint64_t)g * PG106_GRBOOT_GPC_STRIDE + off;
}

// Seed the stand-in chip: what the fuses/straps of a 1x5 GP106 would read.
static void
mock_init(struct gr_mock *m, const struct pg106_grboot_config *c)
{
	uint32_t g, pp;

	memset(m, 0, sizeof(*m));
	m->cfg = c;
	m->idle_left = IDLE_READS;
	put(m, PG106_GRBOOT_PMC_ENABLE, 0x40003120u | PG106_GRBOOT_PMC_GR);
	put(m, c->fecs_base + PG106_GRBOOT_FECSC_OFF_409604, c->gpc_nr);
	put(m, PG106_GRBOOT_PFB_12006C, c->rop_config);
	put(m, PG106_GRBOOT_PFB_100800, c->ltc_active);
	put(m, PG106_GRBOOT_PFB_100804, c->ltc_magma);
	put(m, PG106_GRBOOT_PFB_100C80, c->pri_mmu_rsvd);
	put(m, PG106_GRBOOT_PFB_100CC4, c->mmu_wr);
	put(m, PG106_GRBOOT_PFB_100CC8, c->mmu_wr_rd);
	put(m, PG106_GRBOOT_PFB_100CCC, c->mmu_rd);
	for (g = 0; g < c->gpc_nr; g++) {
		put(m, gpc_reg(c, g, PG106_GRBOOT_GPC_OFF_2608), c->tpc_nr[g]);
		put(m, gpc_reg(c, g, PG106_GRBOOT_GPC_OFF_0C50),
		    c->swdx_pes[g]);
		for (pp = 0; pp < 3; pp++)
			put(m, gpc_reg(c, g, PG106_GRBOOT_GPC_OFF_0C30 + pp * 4),
			    c->ppc_tpc_mask[g][pp]);
	}
	// Bits outside the nvkm_mask fields (SIM) that must survive RMW.
	put(m, c->gr_base + PG106_GRBOOT_OFF_408850, 0x00000a50u);
	put(m, c->gr_base + PG106_GRBOOT_OFF_419CC0, 0x00000001u);
	// A stale format in an unused ZBC slot (index 5) that init clears.
	put(m, c->gr_base + PG106_GRBOOT_OFF_418100 + 4, 0x7fu);
}

static uint32_t
mrd(struct gr_mock *m, uint64_t addr)
{
	const struct pg106_grboot_config *c = m->cfg;

	if (addr == c->gr_base + PG106_GRBOOT_OFF_40060C) {
		if (m->idle_left > 0) {
			m->idle_left--;
			return 1; // busy
		}
		return 0;
	}
	if (addr == c->fecs_base + 0x800u) {
		uint32_t v = get(m, addr);

		if (m->pending_mthd && --m->answer_in <= 0) {
			switch (m->pending_mthd) {
			case 0x10: v = c->ctx_size; break;
			case 0x16: v = c->size_zcull; break;
			case 0x25: v = c->size_pm; break;
			}
			m->pending_mthd = 0;
			put(m, addr, v);
		}
		return v;
	}
	return get(m, addr);
}

static void
mwr(struct gr_mock *m, uint64_t addr, uint32_t v)
{
	const struct pg106_grboot_config *c = m->cfg;

	m->writes++;
	if (addr == PG106_GRBOOT_PMC_ENABLE)
		m->pmc_writes++;
	if (addr == PG106_GRBOOT_LTC_ZBC_INDEX)
		m->ltc_index_masks++;
	if (addr >= c->gr_base + PG106_GRBOOT_TPC_BASE &&
	    ((addr - c->gr_base - PG106_GRBOOT_TPC_BASE) % PG106_GRBOOT_GPC_STRIDE) %
			    PG106_GRBOOT_TPC_STRIDE == 0x48c)
		m->tpc_48c_writes++;
	if (addr == c->fecs_base + PG106_GRBOOT_FECSC_OFF_409504 &&
	    (v == 0x10 || v == 0x16 || v == 0x25)) {
		m->pending_mthd = v;
		m->answer_in = FECS_ANSWER_READS;
	}
	if (addr == c->fecs_base + PG106_GRBOOT_FALCON_CPUCTL && (v & 2))
		m->fecs_started = true;
	if (addr == c->gpccs_base + PG106_GRBOOT_FALCON_CPUCTL && (v & 2))
		m->gpccs_started = true;
	if (m->fecs_started && m->gpccs_started && m->bootstrapped &&
	    addr == c->fecs_base + PG106_GRBOOT_FALCON_CPUCTL) {
		put(m, c->fecs_base + 0x800u, 1); // ucode up: mailbox ready
		return;
	}
	if ((addr == c->fecs_base + PG106_GRBOOT_FALCON_CPUCTL ||
	     addr == c->gpccs_base + PG106_GRBOOT_FALCON_CPUCTL))
		return; // STARTCPU is write-only
	put(m, addr, v);
}

struct gr_replay {
	size_t poll_timeouts, read_mismatches;
	uint32_t sizes[3];
	size_t nsizes;
};

// Replay; `bootstrap_at` is where the SEC2 bootstrap "happens" (the mock
// flag stands in for test_acr_boot.c's pg106_acr_bootstrap_falcons run).
static struct gr_replay
replay(struct gr_mock *m, const struct pg106_grboot_plan *p,
       size_t bootstrap_at)
{
	struct gr_replay r = { 0 };
	size_t i;

	for (i = 0; i < p->nsteps; i++) {
		const struct pg106_grboot_step *s = &p->steps[i];
		uint32_t v, n, tries;

		if (i == bootstrap_at)
			m->bootstrapped = true;
		switch (s->kind) {
		case PG106_GRBOOT_WRITE_REG:
			mwr(m, s->addr, s->value);
			break;
		case PG106_GRBOOT_WRITE_BLOB:
			break; // sw_nonctx pack: firmware data, not modelled
		case PG106_GRBOOT_MASK_REG:
			v = mrd(m, s->addr);
			mwr(m, s->addr, (v & ~s->mask) | s->value);
			break;
		case PG106_GRBOOT_READ_REG:
			v = mrd(m, s->addr);
			if ((v & s->mask) != (s->value & s->mask))
				r.read_mismatches++;
			break;
		case PG106_GRBOOT_POLL_REG:
			tries = s->timeout_us / 10 ? s->timeout_us / 10 : 1;
			if (tries > 1000)
				tries = 1000; // SIM: bounded replay
			for (n = 0; n < tries; n++) {
				bool ok;

				v = mrd(m, s->addr);
				ok = s->poll_op == PG106_GRBOOT_POLL_EQ
					     ? (v & s->mask) == s->value
					     : (v & s->mask) != 0;
				if (ok)
					break;
			}
			if (n == tries)
				r.poll_timeouts++;
			else if (s->mask == 0xffffffffu && r.nsizes < 3)
				r.sizes[r.nsizes++] = v; // the size query result
			break;
		}
	}
	return r;
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
			      sizeof(s->blob_len) + sizeof(s->timeout_us) +
			      strlen(s->why) + 1 +
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
		memcpy(buf + len, &s->timeout_us, sizeof(s->timeout_us));
		len += sizeof(s->timeout_us);
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

static struct pg106_grboot_plan g_gr_plan, g_gr_plan2;
static struct pg106_grboot_info g_gr_info;

static void
test_replay(void)
{
	struct pg106_grboot_config cfg;
	struct gr_mock *m = malloc(sizeof(*m));
	struct gr_replay r;
	size_t i, n_unload = 0;

	printf("\n== SIMULATION, NOT HARDWARE PROOF: GR bring-up replayed on "
	       "the mock register file ==\n");
	gr_config_init(&cfg, 0x00400000ull, 0x00409000, 0x0041a000);
	CHECK_EQ(pg106_grboot_plan_build(&cfg, &g_gr_plan, &g_gr_info), 0);
	mock_init(m, &cfg);
	r = replay(m, &g_gr_plan, g_gr_info.bootstrap_before);
	CHECK_EQ(r.poll_timeouts, 0);
	CHECK_EQ(r.read_mismatches, 0);
	// FECS size queries: polled until nonzero, results are the sizes.
	CHECK_EQ(r.nsizes, 3);
	CHECK_EQ(r.sizes[0], cfg.ctx_size);
	CHECK_EQ(r.sizes[1], cfg.size_zcull);
	CHECK_EQ(r.sizes[2], cfg.size_pm);
	printf("fecs sizes: ctx 0x%x zcull 0x%x pm 0x%x (after %d polls "
	       "each)\n",
	       r.sizes[0], r.sizes[1], r.sizes[2], FECS_ANSWER_READS);

	// GP106 never writes PMC_ENABLE (no NvGrResetWar on 0x136, and
	// gf100_gr_fini writes nothing).
	CHECK_EQ(m->pmc_writes, 0);
	for (i = 0; i < g_gr_plan.nsteps; i++)
		if (g_gr_plan.steps[i].group == PG106_GRBOOT_G_UNLOAD)
			n_unload++;
	CHECK_EQ(n_unload, 0);

	// Golden group order, non-decreasing through the plan.
	for (i = 1; i < g_gr_plan.nsteps; i++)
		CHECK(g_gr_plan.steps[i].group >= g_gr_plan.steps[i - 1].group);
	CHECK_EQ(first_of_group(&g_gr_plan, PG106_GRBOOT_G_RESET), 0);
	CHECK(first_of_group(&g_gr_plan, PG106_GRBOOT_G_DISCOVERY) <
	      first_of_group(&g_gr_plan, PG106_GRBOOT_G_ZBC));
	CHECK(first_of_group(&g_gr_plan, PG106_GRBOOT_G_ZBC) <
	      first_of_group(&g_gr_plan, PG106_GRBOOT_G_FECS_BOOT));
	CHECK_EQ(g_gr_plan.steps[0].kind, PG106_GRBOOT_MASK_REG); // 0x400500
	// The bootstrap slot sits between unk260(0) and unk260(1), before
	// any falcon start.
	CHECK(g_gr_info.bootstrap_before > 0);
	CHECK_EQ(g_gr_plan.steps[g_gr_info.bootstrap_before - 1].addr,
		 PG106_GRBOOT_PMC_UNK260);
	CHECK_EQ(g_gr_plan.steps[g_gr_info.bootstrap_before - 1].value, 0);
	CHECK_EQ(g_gr_plan.steps[g_gr_info.bootstrap_before].addr,
		 PG106_GRBOOT_PMC_UNK260);
	CHECK_EQ(g_gr_plan.steps[g_gr_info.bootstrap_before].value, 1);

	// RMW preserved bits outside the masks.
	CHECK_EQ(get(m, cfg.gr_base + PG106_GRBOOT_OFF_408850),
		 0x00000a50u | cfg.fbp_count);
	CHECK_EQ(get(m, cfg.gr_base + PG106_GRBOOT_OFF_419CC0), 0x9u);
	// 0x419cc0's per-TPC 0x48c clears are emitted (gf100.c:2245-2248).
	CHECK_EQ(m->tpc_48c_writes, g_gr_info.tpc_total);

	// ZBC: formats per gf100_gr_zbc_init (1, 2, 4, 4 color; 1, 1 depth;
	// 1, 1, 1 stencil) at 7-bit fields, index 1 at bit 0.
	CHECK_EQ(get(m, cfg.gr_base + PG106_GRBOOT_OFF_418100),
		 1u | (2u << 7) | (4u << 14) | (4u << 21));
	CHECK_EQ(get(m, cfg.gr_base + PG106_GRBOOT_OFF_418100 + 4), 0);
	CHECK_EQ(get(m, cfg.gr_base + PG106_GRBOOT_OFF_41814C), 1u | (1u << 7));
	CHECK_EQ(get(m, cfg.gr_base + PG106_GRBOOT_OFF_418198),
		 1u | (1u << 7) | (1u << 14));
	CHECK_EQ(get(m, cfg.gr_base + PG106_GRBOOT_OFF_418010 + 4), 0x3f800000);
	CHECK_EQ(get(m, cfg.gr_base + PG106_GRBOOT_OFF_418110 + 4), 0x3f800000);
	CHECK_EQ(get(m, cfg.gr_base + PG106_GRBOOT_OFF_41815C + 8), 0xff);
	CHECK_EQ(m->ltc_index_masks, 9);
	CHECK_EQ(get(m, PG106_GRBOOT_LTC_ZBC_STENCIL), 0xff);
	CHECK_EQ(get(m, PG106_GRBOOT_LTC_ZBC_INDEX), 3);
	CHECK_EQ(get(m, PG106_GRBOOT_LTC_ZBC_COLOR0), 0x3f800000); // f32_1 l2
	free(m);
}

static void
test_mutations(void)
{
	struct pg106_grboot_config cfg;
	struct pg106_grboot_plan mp;
	struct gr_mock *m = malloc(sizeof(*m));
	struct gr_replay r;
	size_t i, n;

	printf("\n== GR mutation tests ==\n");
	gr_config_init(&cfg, 0x00400000ull, 0x00409000, 0x0041a000);

	// (10) size queries as single reads (the old model) see 0.
	mp.steps = malloc(g_gr_plan.nsteps * sizeof(*mp.steps));
	memcpy(mp.steps, g_gr_plan.steps, g_gr_plan.nsteps * sizeof(*mp.steps));
	mp.nsteps = g_gr_plan.nsteps;
	for (i = n = 0; i < mp.nsteps; i++)
		if (mp.steps[i].kind == PG106_GRBOOT_POLL_REG &&
		    mp.steps[i].mask == 0xffffffffu) {
			mp.steps[i].kind = PG106_GRBOOT_READ_REG;
			mp.steps[i].value = n == 0 ? cfg.ctx_size
					    : n == 1 ? cfg.size_zcull
						     : cfg.size_pm;
			n++;
		}
	CHECK_EQ(n, 3);
	mock_init(m, &cfg);
	r = replay(m, &mp, g_gr_info.bootstrap_before);
	printf("single-read size query mutant: %zu mismatches\n",
	       r.read_mismatches);
	CHECK_EQ(r.read_mismatches, 3);

	// (8) RMW as plain writes loses the pre-set bits.
	memcpy(mp.steps, g_gr_plan.steps, g_gr_plan.nsteps * sizeof(*mp.steps));
	for (i = 0; i < mp.nsteps; i++)
		if (mp.steps[i].kind == PG106_GRBOOT_MASK_REG)
			mp.steps[i].kind = PG106_GRBOOT_WRITE_REG;
	mock_init(m, &cfg);
	r = replay(m, &mp, g_gr_info.bootstrap_before);
	CHECK(get(m, cfg.gr_base + PG106_GRBOOT_OFF_408850) !=
	      (0x00000a50u | cfg.fbp_count));
	CHECK_EQ(get(m, cfg.gr_base + PG106_GRBOOT_OFF_419CC0), 0x8u);
	free(mp.steps);

	// (1) starting FECS/GPCCS without the SEC2 bootstrap: FECS never
	// reports ready.
	mock_init(m, &cfg);
	r = replay(m, &g_gr_plan, g_gr_plan.nsteps); // bootstrap never runs
	CHECK(r.poll_timeouts > 0);
	printf("no-bootstrap mutant: %zu poll timeout(s)\n", r.poll_timeouts);
	free(m);
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
			// GP106 (no reset WAR) never writes PMC_ENABLE
			// (wait_idle only reads it, gf100.c:1064).
			CHECK(s->addr != PG106_GRBOOT_PMC_ENABLE ||
			      s->kind == PG106_GRBOOT_READ_REG);
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
	CHECK_EQ(p.nsteps, g_gr_plan.nsteps + 4);
	// gf100.c:2090-2094: RMW of the GR bit only, each followed by a read.
	CHECK_EQ(p.steps[0].kind, PG106_GRBOOT_MASK_REG);
	CHECK_EQ(p.steps[0].addr, PG106_GRBOOT_PMC_ENABLE);
	CHECK_EQ(p.steps[0].mask, PG106_GRBOOT_PMC_GR);
	CHECK_EQ(p.steps[0].value, 0);
	CHECK_EQ(p.steps[1].kind, PG106_GRBOOT_READ_REG);
	CHECK_EQ(p.steps[2].kind, PG106_GRBOOT_MASK_REG);
	CHECK_EQ(p.steps[2].value, PG106_GRBOOT_PMC_GR);
	for (size_t k = 0; k < p.nsteps; k++)
		CHECK(!(p.steps[k].addr == PG106_GRBOOT_PMC_ENABLE &&
			p.steps[k].kind == PG106_GRBOOT_WRITE_REG));
	pg106_grboot_plan_free(&p);

	// fbp_count and rop_nr come from one register (0x12006c).
	gr_config_init(&cfg, 0x00400000ull, 0x00900000, 0x00a00000);
	cfg.fbp_count = 2;
	CHECK_EQ(pg106_grboot_plan_build(&cfg, &p, &info), -EINVAL);
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
		if (s->kind == PG106_GRBOOT_MASK_REG)
			printf(" &~0x%08x |0x%08x", s->mask, s->value);
		if (s->kind == PG106_GRBOOT_POLL_REG)
			printf(" mask=0x%08x %s 0x%08x (%u us)", s->mask,
			       pg106_grboot_poll_op_name(s->poll_op),
			       s->value, s->timeout_us);
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
	printf("== gr_boot: SIMULATION, NOT HARDWARE PROOF "
	       "(mock register file, no hardware claims)\n");
	test_replay();
	test_mutations();
	test_citations_and_hwvalue(&g_gr_plan);
	test_determinism();
	test_info_and_validation();
	test_trace_dump();
	pg106_grboot_plan_free(&g_gr_plan);
	pg106_grboot_plan_free(&g_gr_plan2);
	if (g_failures) {
		printf("== gr_boot: FAIL (%d check(s))\n", g_failures);
		return 1;
	}
	printf("== gr_boot: PASS (simulation only — no hardware claims)\n");
	return 0;
}
