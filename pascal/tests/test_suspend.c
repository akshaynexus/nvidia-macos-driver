// SPDX-License-Identifier: GPL-2.0-only
// pascal/tests/test_suspend.c — system sleep / wake (suspend.h, pm.h) end to
// end on tests/mockgpu.h, 10 suspend/resume cycles:
//
//   cold GPU -> wake probe says BOOT -> pg106_gpu_boot (POST from the
//   Helios VBIOS through the PMU DEVINIT path when the ROM is present, mc
//   init, VRAM, FB, BAR2/BAR1, FIFO) -> the GR stage on the mock FECS
//   (BOOTSTRAP_FALCON through a hook, as test_grctx) -> CE self-test (host
//   semaphore + 64 KiB copy) -> GR engine with the golden image (generated
//   in cycle 1, reused from the host copy afterwards, as the kext keeps
//   fGolden) -> a live channel on GR's runlist -> pg106_gpu_suspend
//   (interrupts blocked, channel stopped + preempted, runlist scheduling
//   off, GR + engines idle, FIFO interrupts off) -> pg106_gpu_fini (no
//   second preempt) -> power lost (the mock is re-created unposted) -> the
//   next cycle's wake.
//
// Plus: the pm state machine's suspend/resume transitions (sessions kept,
// D3cold kept, a failed wake retried on the next one), the wake decision
// table, a GPU that kept its power (WPR still locked -> power cycle, or a
// clean failure without ACPI power), a GR that never goes idle (the suspend
// still blocks interrupts and disables scheduling, within its budget), and
// BOOT_0 all ones.
//
// SIMULATION. The ACR/SEC2 half of every wake is test_acr_boot's
// "10 wake cycles" test (the toy ACR in that file); here devboot's GR stage
// runs on the mock with the SEC2 exchange replaced by a hook.

#include "mockgpu.h"

#include <pascal/gpu.h>
#include <pascal/gr_ctx.h>
#include <pascal/pm.h>
#include <pascal/pmc.h>
#include <pascal/suspend.h>
#include <pascal/uclient.h>

#include <errno.h>

static int g_fail, g_checks;

#define CHECK(c)                                                         \
	do {                                                             \
		g_checks++;                                              \
		if (!(c)) {                                              \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			g_fail++;                                        \
		}                                                        \
	} while (0)
#define CHECK_EQ(a, b)                                                   \
	do {                                                             \
		unsigned long long a_ = (unsigned long long)(a);         \
		unsigned long long b_ = (unsigned long long)(b);         \
		g_checks++;                                              \
		if (a_ != b_) {                                          \
			printf("FAIL %s:%d: %s == %s (0x%llx vs 0x%llx)\n", \
			       __FILE__, __LINE__, #a, #b, a_, b_);      \
			g_fail++;                                        \
		}                                                        \
	} while (0)

#define CYCLES 10
#define CTX_SIZE 0x00012000u
#define FW_DIR "../research/firmware/gp106/gr/"
#define BOOT0 0x136000a1u

static const uint32_t HELIOS_PTOP[] = {
	0x8006183e, 0x80000003, 0x00400005, 0x9c028c3e, 0x8000004f,
	0x001040ad, 0xa0030e3e, 0x8000004f, 0x041040b5, 0x94a3aa3e,
	0x8000004f, 0x081040dd, 0x84289e3e, 0x80000043, 0x00084015,
	0x00000000, 0x00000000, 0x00000000, 0x8c679c3e, 0x80000037,
	0x00087095, 0x8848243e, 0x8000003b, 0x001c80cd, 0x00000000,
	0x00000000, 0x00000000, 0x98c52c3e, 0x8000004f, 0x0c1040e5,
};

static struct mockgpu g_m;
static unsigned g_devinit_runs;
static bool g_gr_busy;      // 0x40060c bit 0 stuck
static uint32_t g_wpr[2];   // 0x100cd4 index 2/3 answers (0 = cold)
static bool g_dead;         // BOOT_0 and everything read all ones

#define GPC_UNIT(g, r) (0x500000u + (g) * 0x8000u + (r))

static void
io_wr(void *u, uint64_t off, uint32_t v)
{
	mg_bar1_wr32(u, off, v);
}

static uint32_t
io_rd(void *u, uint64_t off)
{
	return mg_bar1_rd32(u, off);
}

// The PMU answers DEVINIT (test_gpu.c's model) and leaves 0x2240c alone.
static bool
hook_wr(struct mockgpu *m, uint32_t a, uint32_t v)
{
	if (a == 0x10a100 && v == 2 && mg_get(m, 0x10a040) == 0x5000) {
		g_devinit_runs++;
		mg_set(m, 0x10a040, 0x5000 | 0x2000);
		return true;
	}
	return false;
}

static bool
hook_rd(struct mockgpu *m, uint32_t a, uint32_t *v)
{
	if (g_dead) {
		*v = 0xffffffffu;
		return true;
	}
	if (a == PG106_PGRAPH_STATUS && g_gr_busy) {
		*v = 1;
		return true;
	}
	if (a == PG106_PFB_WPR_INFO) {
		uint32_t idx = mg_get(m, a);

		*v = idx == 2 ? (g_wpr[0] | 2) : idx == 3 ? (g_wpr[1] | 3) : idx;
		return true;
	}
	return false;
}

// A cold GP106 (power just applied): unposted when the ROM can POST it.
static void
setup_chip(bool posted)
{
	unsigned i, g;

	mg_init(&g_m, 0x180000000ull);
	mg_set(&g_m, 0x000000, BOOT0);
	mg_set(&g_m, 0x02240c, posted ? 0x2 : 0);
	for (i = 0; i < sizeof(HELIOS_PTOP) / 4; i++)
		mg_set(&g_m, 0x022700 + i * 4, HELIOS_PTOP[i]);
	mg_set(&g_m, 0x022438, 3);
	mg_set(&g_m, 0x022458, 2);
	for (i = 0; i < 6; i++)
		mg_set(&g_m, 0x90020c + i * 0x4000, 1024);
	mg_set(&g_m, 0x002004, 4);
	mg_set(&g_m, 0x000200, 0x40003120u | 0x1000);
	mg_set(&g_m, 0x409604, 2);
	for (g = 0; g < 2; g++) {
		mg_set(&g_m, GPC_UNIT(g, 0x2608), 5);
		mg_set(&g_m, GPC_UNIT(g, 0x0c30), 0x09);
		mg_set(&g_m, GPC_UNIT(g, 0x0c34), 0x12);
		mg_set(&g_m, GPC_UNIT(g, 0x0c38), 0x04);
		mg_set(&g_m, GPC_UNIT(g, 0x0c50), 0x3);
	}
	mg_set(&g_m, 0x12006c, 0x3);
	mg_set(&g_m, 0x100800, 0x3);
	mg_set(&g_m, 0x100804, 0x1);
	g_m.gr.ctx_size = CTX_SIZE;
	g_m.gr.zcull_size = 0x4000;
	g_m.gr.pm_size = 0x1000;
	g_m.runl[5].is_ce = true;
	g_m.runl[6].is_ce = true;
	g_m.wr_hook = hook_wr;
	g_m.rd_hook = hook_rd;
	g_gr_busy = false;
	g_dead = false;
	g_wpr[0] = g_wpr[1] = 0;
}

static int
bootstrap_hook(void *u, uint64_t mask)
{
	(void)u;
	CHECK_EQ(mask, (1ull << 2) | (1ull << 3));
	g_m.gr.bootstrapped = true;
	return 0;
}

static uint8_t *
load_file(const char *path, size_t *n)
{
	FILE *f = fopen(path, "rb");
	uint8_t *b;
	long sz;

	*n = 0;
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = sz > 0 ? malloc((size_t)sz) : NULL;
	if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) {
		free(b);
		b = NULL;
	}
	fclose(f);
	*n = b ? (size_t)sz : 0;
	return b;
}

// ---- the pm state machine ------------------------------------------------------

static void
test_pm(void)
{
	struct pg106_pm pm;
	uint8_t a;
	unsigned k;

	pg106_pm_init(&pm, 5000, false);
	CHECK_EQ(pg106_pm_start(&pm), PG106_PM_ACT_BOOT);
	CHECK_EQ(pg106_pm_done(&pm, PG106_PM_ACT_BOOT, 0, 0), PG106_PM_ACT_NONE);
	CHECK_EQ(pg106_pm_open(&pm, 0, &a), PG106_PM_OPEN_READY); // PG106FB's session
	for (k = 0; k < CYCLES; k++) {
		CHECK_EQ(pg106_pm_suspend(&pm, 10), PG106_PM_ACT_SUSPEND);
		CHECK_EQ(pm.state, PG106_PM_SUSPENDING);
		CHECK(!pg106_pm_ready(&pm));
		CHECK_EQ(pg106_pm_suspend(&pm, 10), PG106_PM_ACT_NONE); // one at a time
		CHECK_EQ(pg106_pm_done(&pm, PG106_PM_ACT_SUSPEND, k == 3 ? -ETIMEDOUT : 0, 10),
			 PG106_PM_ACT_NONE);
		CHECK_EQ(pm.state, PG106_PM_SUSPENDED); // never refused
		CHECK(pg106_pm_asleep(&pm));
		CHECK_EQ(pm.sessions, 1);
		CHECK_EQ(pg106_pm_tick(&pm, 100000), PG106_PM_ACT_NONE);
		// an open while asleep waits for the wake
		CHECK_EQ(pg106_pm_open(&pm, 20, &a), PG106_PM_OPEN_WAIT);
		CHECK_EQ(a, PG106_PM_ACT_NONE);
		CHECK_EQ(pg106_pm_resume(&pm, 30), PG106_PM_ACT_RESUME);
		CHECK_EQ(pm.state, PG106_PM_RESUMING);
		CHECK_EQ(pg106_pm_done(&pm, PG106_PM_ACT_RESUME, 0, 30), PG106_PM_ACT_BOOT);
		CHECK_EQ(pg106_pm_done(&pm, PG106_PM_ACT_BOOT, 0, 40), PG106_PM_ACT_NONE);
		CHECK(pg106_pm_ready(&pm));
		CHECK_EQ(pm.state, PG106_PM_ACTIVE);
		CHECK_EQ(pm.sessions, 2);
		pg106_pm_close(&pm, 50);
	}
	CHECK_EQ(pm.suspends, CYCLES);
	CHECK_EQ(pm.resumes, CYCLES);
	CHECK_EQ(pm.generation, CYCLES + 1);
	CHECK_EQ(pm.resume_failures, 0);

	// A wake whose boot fails: FAILED, sessions refused; the next sleep and
	// wake try again.
	CHECK_EQ(pg106_pm_suspend(&pm, 0), PG106_PM_ACT_SUSPEND);
	pg106_pm_done(&pm, PG106_PM_ACT_SUSPEND, 0, 0);
	CHECK_EQ(pg106_pm_resume(&pm, 0), PG106_PM_ACT_RESUME);
	CHECK_EQ(pg106_pm_done(&pm, PG106_PM_ACT_RESUME, 0, 0), PG106_PM_ACT_BOOT);
	CHECK_EQ(pg106_pm_done(&pm, PG106_PM_ACT_BOOT, -EIO, 0), PG106_PM_ACT_NONE);
	CHECK_EQ(pm.state, PG106_PM_FAILED);
	CHECK_EQ(pm.resume_failures, 1);
	CHECK_EQ(pg106_pm_open(&pm, 0, &a), PG106_PM_OPEN_FAILED);
	CHECK_EQ(pg106_pm_suspend(&pm, 0), PG106_PM_ACT_SUSPEND);
	pg106_pm_done(&pm, PG106_PM_ACT_SUSPEND, 0, 0);
	CHECK_EQ(pg106_pm_resume(&pm, 0), PG106_PM_ACT_RESUME);
	CHECK_EQ(pg106_pm_done(&pm, PG106_PM_ACT_RESUME, -ENODEV, 0), PG106_PM_ACT_NONE);
	CHECK_EQ(pm.state, PG106_PM_FAILED);
	CHECK_EQ(pm.resume_failures, 2);
	CHECK_EQ(pg106_pm_suspend(&pm, 0), PG106_PM_ACT_SUSPEND);
	pg106_pm_done(&pm, PG106_PM_ACT_SUSPEND, 0, 0);
	CHECK_EQ(pg106_pm_resume(&pm, 0), PG106_PM_ACT_RESUME);
	pg106_pm_done(&pm, PG106_PM_ACT_RESUME, 0, 0);
	CHECK_EQ(pg106_pm_done(&pm, PG106_PM_ACT_BOOT, 0, 0), PG106_PM_ACT_NONE);
	CHECK(pg106_pm_ready(&pm));

	// D3cold (OFF) at sleep: nothing to do, still OFF after the wake; a
	// session that opened meanwhile powers it on.
	pg106_pm_init(&pm, 0, true);
	pg106_pm_start(&pm);
	pg106_pm_done(&pm, PG106_PM_ACT_BOOT, 0, 0);
	CHECK_EQ(pg106_pm_tick(&pm, 1), PG106_PM_ACT_QUIESCE);
	CHECK_EQ(pg106_pm_done(&pm, PG106_PM_ACT_QUIESCE, 0, 1), PG106_PM_ACT_POWER_OFF);
	pg106_pm_done(&pm, PG106_PM_ACT_POWER_OFF, 0, 1);
	CHECK_EQ(pm.state, PG106_PM_OFF);
	CHECK_EQ(pg106_pm_suspend(&pm, 2), PG106_PM_ACT_NONE);
	CHECK_EQ(pm.state, PG106_PM_SUSPENDED);
	CHECK_EQ(pg106_pm_resume(&pm, 3), PG106_PM_ACT_NONE);
	CHECK_EQ(pm.state, PG106_PM_OFF);
	pg106_pm_suspend(&pm, 4);
	CHECK_EQ(pg106_pm_open(&pm, 5, &a), PG106_PM_OPEN_WAIT);
	CHECK_EQ(pg106_pm_resume(&pm, 6), PG106_PM_ACT_POWER_ON);
	CHECK_EQ(pg106_pm_done(&pm, PG106_PM_ACT_POWER_ON, 0, 6), PG106_PM_ACT_BOOT);
	pg106_pm_done(&pm, PG106_PM_ACT_BOOT, 0, 7);
	CHECK_EQ(pm.state, PG106_PM_ACTIVE);

	// A sleep that arrives while an action is outstanding goes right after
	// it (the kext serialises both on its gate; the machine still copes).
	pg106_pm_init(&pm, 0, false);
	CHECK_EQ(pg106_pm_start(&pm), PG106_PM_ACT_BOOT);
	CHECK_EQ(pg106_pm_suspend(&pm, 0), PG106_PM_ACT_NONE);
	CHECK(pm.suspend_pending);
	CHECK_EQ(pg106_pm_done(&pm, PG106_PM_ACT_BOOT, 0, 0), PG106_PM_ACT_SUSPEND);
	pg106_pm_done(&pm, PG106_PM_ACT_SUSPEND, 0, 0);
	CHECK_EQ(pm.state, PG106_PM_SUSPENDED);
	// A sleep before the first boot ran: the wake boots it.
	pg106_pm_init(&pm, 0, false);
	CHECK_EQ(pg106_pm_suspend(&pm, 0), PG106_PM_ACT_SUSPEND);
	pg106_pm_done(&pm, PG106_PM_ACT_SUSPEND, 0, 0);
	CHECK_EQ(pg106_pm_start(&pm), PG106_PM_ACT_NONE); // deferred to the wake
	CHECK_EQ(pg106_pm_resume(&pm, 0), PG106_PM_ACT_RESUME);
	CHECK_EQ(pg106_pm_done(&pm, PG106_PM_ACT_RESUME, 0, 0), PG106_PM_ACT_BOOT);
	CHECK_EQ(pg106_pm_state_name(PG106_PM_SUSPENDED)[0], 's');
	CHECK_EQ(pg106_pm_action_name(PG106_PM_ACT_RESUME)[0], 'r');
	printf("PASS pm: %d sleep/wake cycles with a session kept, a failed wake retried "
	       "by the next one, D3cold kept across sleep, sleep during an action\n",
	       CYCLES);
}

// ---- the wake decision ------------------------------------------------------------

static void
test_decide(void)
{
	struct pg106_resume_probe p;

	memset(&p, 0, sizeof(p));
	p.boot0 = p.boot0_expected = BOOT0;
	p.boot0_readable = true;
	CHECK_EQ(pg106_resume_decide(&p), PG106_RESUME_BOOT);
	p.posted = true; // posted but no WPR: a warm GPU ACR never ran on
	CHECK_EQ(pg106_resume_decide(&p), PG106_RESUME_BOOT);
	p.wpr_locked = true;
	CHECK_EQ(pg106_resume_decide(&p), PG106_RESUME_FAIL_WARM);
	p.acpi_power = true;
	CHECK_EQ(pg106_resume_decide(&p), PG106_RESUME_POWER_CYCLE);
	p.attempt = 2;
	CHECK_EQ(pg106_resume_decide(&p), PG106_RESUME_FAIL_WARM);
	memset(&p, 0, sizeof(p));
	p.boot0 = 0xffffffffu;
	p.boot0_expected = BOOT0;
	CHECK_EQ(pg106_resume_decide(&p), PG106_RESUME_FAIL_NO_POWER);
	p.acpi_power = true;
	CHECK_EQ(pg106_resume_decide(&p), PG106_RESUME_POWER_ON);
	p.attempt = 2;
	CHECK_EQ(pg106_resume_decide(&p), PG106_RESUME_FAIL_NO_POWER);
	p.boot0 = 0x134000a1u; // a GP104 answering: not ours
	p.boot0_readable = true;
	CHECK_EQ(pg106_resume_decide(&p), PG106_RESUME_FAIL_IDENTITY);
	CHECK(strcmp(pg106_resume_plan_name(PG106_RESUME_FAIL_WARM), "?") != 0);

	// The probe on the mock: cold, warm, PRI errors (unclocked PFB), dead.
	setup_chip(false);
	{
		struct pg106_hw_bus bus = { &g_m, mg_rd32, mg_wr32, mg_delay, NULL, 0 };

		pg106_resume_probe(&bus, BOOT0, true, 0, &p);
		CHECK(p.boot0_readable && !p.posted && p.wpr_valid && !p.wpr_locked);
		CHECK_EQ(pg106_resume_decide(&p), PG106_RESUME_BOOT);
		g_wpr[0] = 0x017fe400u << 0; // start 0x17fe40000 >> 8
		g_wpr[1] = 0x017fe600u;
		pg106_resume_probe(&bus, BOOT0, true, 0, &p);
		CHECK(p.wpr_locked);
		CHECK_EQ(p.wpr_start, 0x17fe40000ull);
		CHECK_EQ(p.wpr_end, 0x17fe60000ull + 0x20000);
		CHECK_EQ(pg106_resume_decide(&p), PG106_RESUME_POWER_CYCLE);
		g_wpr[0] = 0xbadf1200u;
		g_wpr[1] = 0xbadf1200u;
		pg106_resume_probe(&bus, BOOT0, true, 0, &p);
		CHECK(!p.wpr_valid && !p.wpr_locked);
		CHECK_EQ(pg106_resume_decide(&p), PG106_RESUME_BOOT);
		g_dead = true;
		pg106_resume_probe(&bus, BOOT0, false, 0, &p);
		CHECK(!p.boot0_readable);
		CHECK_EQ(pg106_resume_decide(&p), PG106_RESUME_FAIL_NO_POWER);
		g_dead = false;
	}
	mg_free(&g_m);
	printf("PASS wake decision: cold -> boot, WPR still locked -> power cycle (or a "
	       "clean failure without PG00), all ones -> PG00._ON (at most two power "
	       "operations), PRI errors -> cold, another BOOT_0 -> refused\n");
}

// ---- 10 cycles on the mock GPU ----------------------------------------------------

struct fwset {
	uint8_t *rom, *nonctx, *swctx, *bundle, *method;
	size_t n_rom, n_nonctx, n_swctx, n_bundle, n_method;
};

// A render client's BAR1 window mapped across the sleeps (uclient.h): the
// kext's registry, fed to pg106_gpu_boot as cfg.bar1_hold.
struct uobj {
	int maps;
	bool blocked;
};
static struct pg106_uwins g_uw;
static struct pg106_gpu *g_uw_gpu;
static bool
uw_mapped(void *u, void *o)
{
	(void)u;
	return ((struct uobj *)o)->maps > 0;
}
static void
uw_give_back(void *u, const struct pg106_uwin *w)
{
	(void)u;
	pg106_bar_unmap(&g_uw_gpu->bar1, w->bar1_off, w->size);
	pg106_vram_free(&g_uw_gpu->vram, w->vram_pa, w->size);
}
static void
uw_block(void *u, void *o)
{
	(void)u;
	((struct uobj *)o)->blocked = true;
}
static const struct pg106_uwin_ops g_uw_ops = { NULL, uw_mapped, uw_give_back, uw_block, NULL };

static int
boot_to_ready(struct pg106_gpu *g, struct pg106_hw_bus *bus, struct fwset *fw)
{
	struct pg106_gpu_cfg cfg;
	struct pg106_devboot_cfg dc;
	struct pg106_devboot_fw dfw;
	int ret;

	memset(&cfg, 0, sizeof(cfg));
	cfg.bus = bus;
	cfg.vbios = fw->rom;
	cfg.vbios_size = (uint32_t)fw->n_rom;
	cfg.bar1_size = 0x10000000;
	cfg.bar2_size = 0x2000000;
	cfg.bar1_io = (struct pg106_bar1_io){ &g_m, io_wr, io_rd };
	cfg.stop_after = PG106_DB_TOP;
	cfg.bar1_hold = &g_uw;
	cfg.bar1_hold_ops = &g_uw_ops;
	g_uw_gpu = g;
	ret = pg106_gpu_boot(g, &cfg);
	if (ret)
		return ret;
	memset(&dfw, 0, sizeof(dfw));
	dfw.sw_nonctx.data = fw->nonctx;
	dfw.sw_nonctx.size = fw->n_nonctx;
	memset(&dc, 0, sizeof(dc));
	dc.bus = bus;
	dc.fw = &dfw;
	dc.bootstrap = bootstrap_hook;
	return pg106_devboot_gr(&g->db, &dc);
}

static void
test_cycles(struct fwset *fw)
{
	struct pg106_gpu *g = calloc(1, sizeof(*g));
	struct pg106_gr *gr = calloc(1, sizeof(*gr));
	struct pg106_hw_bus bus = { &g_m, mg_rd32, mg_wr32, mg_delay, NULL, 0 };
	uint32_t *golden = NULL, golden_size = 0;
	unsigned k, posts = 0, golden_runs = 0;
	struct uobj uo = { 0 };
	uint64_t uo_off = 0, uo_pa = 0;
	bool uw_checked = false;

	for (k = 0; k < CYCLES; k++) {
		struct pg106_resume_probe p;
		struct pg106_gpu_selftest st;
		struct pg106_suspend_res sr;
		struct pg106_gr_cfg gc;
		struct pg106_gr_fw gfw;
		struct pg106_chan_desc d;
		struct pg106_chan *ch = NULL;
		uint64_t pdb = 0, t0;
		uint32_t preempts;
		unsigned devinit0 = g_devinit_runs, i;
		int runl;

		// wake: power is back, the GPU is cold (unposted if the ROM
		// can POST it)
		setup_chip(fw->rom == NULL);
		pg106_resume_probe(&bus, BOOT0, true, 0, &p);
		CHECK_EQ(pg106_resume_decide(&p), PG106_RESUME_BOOT);

		CHECK_EQ(boot_to_ready(g, &bus, fw), 0);
		CHECK(pg106_gpu_ready(g));
		CHECK_EQ(g->db.stage, PG106_DB_READY);
		if (fw->rom) {
			CHECK_EQ(g_devinit_runs, devinit0 + 1); // POSTed every wake
			posts++;
		}
		CHECK_EQ(mg_get(&g_m, 0x200), 0xffffffffu); // mc init every wake
		CHECK_EQ(g_m.intr_allow, PG106_PMC_INTR_FIFO);

		CHECK_EQ(pg106_gpu_selftest(g, 100000, &st), 0);
		CHECK_EQ(st.ce_mismatch, 0);
		CHECK_EQ(st.ce_sem_value, 0x5a5a0002u);

		// BAR1 user window (uclient.h) through the real boot: mapped in
		// cycle 1 and freed while mapped; still mapped at the wakes of
		// cycles 2 and 3, where pg106_gpu_boot reserved it in the new
		// BAR1 allocator before FIFO init or anyone else mapped; the
		// stale task unmaps in cycle 3 and the window is free again.
		if (k == 0) {
			CHECK_EQ(pg106_vram_alloc(&g->vram, 0x10000, 0x1000, 0, &uo_pa), 0);
			CHECK_EQ(pg106_bar_map_ex(&g->bar1, uo_pa, 0x10000, PG106_TARGET_VRAM,
						  false, &uo_off), 0);
			CHECK_EQ(pg106_uwins_add(&g_uw, &uo, uo_off, 0x10000, uo_pa), 0);
			uo.maps = 1;
			CHECK_EQ(pg106_uwins_free(&g_uw, &uo, &g_uw_ops), 0); // held
		} else if (k <= 2) {
			uint64_t off2 = 0, pa2 = 0;

			CHECK(uo.blocked);
			CHECK_EQ(g->bar1_hold_failed, 0);
			CHECK(pg106_range_is_allocated(&g->bar1.va, uo_off, 0x10000));
			CHECK_EQ(pg106_vram_alloc(&g->vram, 0x10000, 0x1000, 0, &pa2), 0);
			CHECK_EQ(pg106_bar_map_ex(&g->bar1, pa2, 0x10000, PG106_TARGET_VRAM,
						  false, &off2), 0);
			CHECK(off2 + 0x10000 <= uo_off || uo_off + 0x10000 <= off2);
			CHECK_EQ(pg106_bar_unmap(&g->bar1, off2, 0x10000), 0);
			pg106_vram_free(&g->vram, pa2, 0x10000);
			if (k == 2) {
				uo.maps = 0;
				CHECK_EQ(pg106_uwins_reap(&g_uw, &g_uw_ops), 1);
				CHECK(!pg106_range_is_allocated(&g->bar1.va, uo_off, 0x10000));
				CHECK_EQ(g_uw.n, 0);
				uw_checked = true;
			}
		}

		// GR engine; the golden image from the first cycle is reused
		memset(&gfw, 0, sizeof(gfw));
		gfw.sw_ctx = fw->swctx;
		gfw.sw_ctx_len = fw->n_swctx;
		gfw.sw_bundle_init = fw->bundle;
		gfw.sw_bundle_init_len = fw->n_bundle;
		gfw.sw_method_init = fw->method;
		gfw.sw_method_init_len = fw->n_method;
		memset(&gc, 0, sizeof(gc));
		gc.bus = &bus;
		gc.vram = &g->vram;
		gc.topo = &g->db.gr_cfg;
		gc.ctx_size = g->db.gr_sizes[0];
		gc.fw = &gfw;
		for (i = 0; i < g->db.top.ndev; i++)
			if (g->db.top.dev[i].type == PG106_TOP_TYPE_GR)
				gc.gr_engine = (unsigned)g->db.top.dev[i].engine;
		if (golden && golden_size == gc.ctx_size)
			gc.golden = golden;
		CHECK_EQ(pg106_gr_init(gr, &gc), 0);
		if (!gr->golden) {
			CHECK_EQ(pg106_gr_golden(gr), 0);
			golden_runs++;
			CHECK(gr->golden != NULL);
			if (gr->golden) {
				golden = malloc(gr->size);
				memcpy(golden, gr->golden, gr->size);
				golden_size = gr->size;
			}
		} else {
			CHECK_EQ(memcmp(gr->golden, golden, golden_size), 0);
		}

		// a live channel on GR's runlist at the moment of the sleep
		runl = pg106_fifo_runl_find(&g->fifo, PG106_TOP_TYPE_GR);
		CHECK(runl >= 0);
		CHECK_EQ(pg106_vram_alloc(&g->vram, 0x1000, 0x1000, 0, &pdb), 0);
		memset(&d, 0, sizeof(d));
		d.pdb_addr = pdb;
		d.pdb_target = PG106_TARGET_VRAM;
		d.vm_limit = (1ull << 40) - 1;
		d.gpfifo_va = 0x100000;
		d.entries = 64;
		CHECK_EQ(pg106_chan_create_desc(&g->fifo, runl, &d, &ch), 0);

		// sleep: the kext drops GR first (grDrop), then quiesces
		pg106_gr_fini(gr);
		t0 = g_m.t_us;
		preempts = g_m.preempts;
		if (k == 4)
			g_gr_busy = true; // a GR that never goes idle
		CHECK_EQ(pg106_gpu_suspend(g, 0, &sr), k == 4 ? -ETIMEDOUT : 0);
		CHECK_EQ(sr.chans_preempted, 1);
		CHECK_EQ(sr.preempt_timeouts, 0);
		CHECK_EQ(g_m.preempts, preempts + 1);
		CHECK(sr.runl_mask != 0);
		CHECK_EQ(mg_get(&g_m, PG106_FIFO_SCHED_DISABLE) & sr.runl_mask, sr.runl_mask);
		CHECK_EQ(g_m.intr_allow, 0);          // PMC interrupts blocked
		CHECK_EQ(g_m.pfifo_en, 0);            // FIFO interrupts off
		CHECK(g->fifo.suspended);
		CHECK(!g_m.chan[ch->chid].enabled);   // stopped
		CHECK(g_m.t_us - t0 <= PG106_SUSPEND_BUDGET_US + 1000); // bounded
		if (k == 4) {
			CHECK(sr.failed_at && strstr(sr.failed_at, "GR idle"));
			CHECK(sr.waited_us >= PG106_SUSPEND_BUDGET_US);
		} else {
			CHECK(sr.failed_at == NULL);
		}
		g_gr_busy = false;
		pg106_uwins_lost(&g_uw, &g_uw_ops); // the kext's memLost
		pg106_gpu_fini(g); // frees the channel without a second preempt
		CHECK_EQ(g_m.preempts, preempts + 1);
		CHECK_EQ(g_m.nerr, 0);
		if (g_m.nerr)
			printf("cycle %u: mock: %s\n", k + 1, g_m.err[0]);
		CHECK_EQ(g->vram.mm.free_bytes, 0); // fini zeroed the object
		// power lost: the next cycle starts from a fresh, cold mock
		mg_free(&g_m);
	}
	CHECK_EQ(golden_runs, 1);
	CHECK(uw_checked);
	pg106_uwins_fini(&g_uw, &g_uw_ops);
	printf("PASS BAR1 user window freed while mapped: held, blocked at every sleep, "
	       "reserved by pg106_gpu_boot in the new BAR1 allocator at 2 wakes (a new "
	       "mapping never overlaps it), free again after the stale unmap\n");
	printf("PASS %d suspend/resume cycles on the mock: cold wake probe, %s, mc init, "
	       "FIFO, GR stage (BOOTSTRAP_FALCON hook) to READY, CE self-test, golden image "
	       "generated once and reused %d times, a live GR-runlist channel preempted at "
	       "every sleep, scheduling off, interrupts blocked, fini without a second "
	       "preempt; cycle 5's stuck GR timed out within the %u us budget\n",
	       CYCLES, fw->rom ? "POST from the Helios VBIOS every wake" : "posted GPU (no ROM)",
	       CYCLES - 1, PG106_SUSPEND_BUDGET_US);
	(void)posts;

	// A GPU that kept its power: the WPR of the last ACR boot is still
	// there. With PG00 the wake power-cycles it; without, it fails cleanly.
	setup_chip(true);
	CHECK_EQ(boot_to_ready(g, &bus, fw), 0);
	CHECK_EQ(pg106_gpu_suspend(g, 0, NULL), 0);
	pg106_gpu_fini(g);
	g_wpr[0] = 0x017fe400u;
	g_wpr[1] = 0x017fe600u;
	{
		struct pg106_resume_probe p;

		pg106_resume_probe(&bus, BOOT0, true, 0, &p);
		CHECK_EQ(pg106_resume_decide(&p), PG106_RESUME_POWER_CYCLE);
		pg106_resume_probe(&bus, BOOT0, false, 0, &p);
		CHECK_EQ(pg106_resume_decide(&p), PG106_RESUME_FAIL_WARM);
		// after the power cycle the WPR is gone: boot, and the warm
		// fixup clears the scheduling disable a cold GPU never has
		g_wpr[0] = g_wpr[1] = 0;
		pg106_resume_probe(&bus, BOOT0, true, 1, &p);
		CHECK_EQ(pg106_resume_decide(&p), PG106_RESUME_BOOT);
		CHECK(mg_get(&g_m, PG106_FIFO_SCHED_DISABLE) != 0);
		pg106_resume_warm_fixup(&bus);
		CHECK_EQ(mg_get(&g_m, PG106_FIFO_SCHED_DISABLE), 0);
	}
	mg_free(&g_m);
	// Suspend of an object that never booted touches nothing.
	{
		struct pg106_gpu z;

		memset(&z, 0, sizeof(z));
		CHECK_EQ(pg106_gpu_suspend(&z, 0, NULL), 0);
	}
	free(golden);
	free(gr);
	free(g);
	printf("PASS warm wake: WPR still locked -> power cycle with PG00, a clean "
	       "failure without; never-booted suspend is a no-op\n");
}

int
main(void)
{
	struct fwset fw;

	printf("== SIMULATION: system sleep / wake on tests/mockgpu.h ==\n");
	memset(&fw, 0, sizeof(fw));
	fw.rom = load_file("../research/firmware/vbios/helios300-gp106.rom", &fw.n_rom);
	fw.nonctx = load_file(FW_DIR "sw_nonctx.bin", &fw.n_nonctx);
	fw.swctx = load_file(FW_DIR "sw_ctx.bin", &fw.n_swctx);
	fw.bundle = load_file(FW_DIR "sw_bundle_init.bin", &fw.n_bundle);
	fw.method = load_file(FW_DIR "sw_method_init.bin", &fw.n_method);
	if (!fw.nonctx || !fw.swctx || !fw.bundle || !fw.method) {
		printf("FAIL: " FW_DIR " firmware lists missing\n");
		return 1;
	}
	if (!fw.rom)
		printf("note: Helios ROM not present: cycles start posted (no POST)\n");
	test_pm();
	test_decide();
	test_cycles(&fw);
	free(fw.rom);
	free(fw.nonctx);
	free(fw.swctx);
	free(fw.bundle);
	free(fw.method);
	printf("%s: %d checks, %d failures\n", g_fail ? "FAILED" : "PASSED",
	       g_checks, g_fail);
	return g_fail ? 1 : 0;
}
