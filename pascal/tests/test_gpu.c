// SPDX-License-Identifier: GPL-2.0-only
// pascal/tests/test_gpu.c — pg106_gpu: two-pass bring-up, VRAM sizing,
// BAR2/BAR1, FIFO, interrupt routing, and the GPU-work self-test (host
// semaphore + CE copy) end to end on tests/mockgpu.h.
//
// SIMULATION. devboot is stopped after PTOP here (the ACR/GR stages have
// their own falcon mocks in test_acr_boot/test_gr_boot); the copy-engine
// path needs neither (gk104_engn_ce has no context: fifo/gk104.c:280-286).

#include "mockgpu.h"

#include <pascal/gpu.h>
#include <pascal/pmc.h>

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

static const uint32_t HELIOS_PTOP[] = {
	0x8006183e, 0x80000003, 0x00400005, 0x9c028c3e, 0x8000004f,
	0x001040ad, 0xa0030e3e, 0x8000004f, 0x041040b5, 0x94a3aa3e,
	0x8000004f, 0x081040dd, 0x84289e3e, 0x80000043, 0x00084015,
	0x00000000, 0x00000000, 0x00000000, 0x8c679c3e, 0x80000037,
	0x00087095, 0x8848243e, 0x8000003b, 0x001c80cd, 0x00000000,
	0x00000000, 0x00000000, 0x98c52c3e, 0x8000004f, 0x0c1040e5,
};

static struct mockgpu g_m;

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

static void
setup_chip(void)
{
	unsigned i;

	mg_init(&g_m, 0x180000000ull);
	mg_set(&g_m, 0x000000, 0x136000a1u); // BOOT_0 GP106 A1
	mg_set(&g_m, 0x02240c, 0x2);         // posted
	for (i = 0; i < sizeof(HELIOS_PTOP) / 4; i++)
		mg_set(&g_m, 0x022700 + i * 4, HELIOS_PTOP[i]);
	// 3 FBPs x 2 FBPAs x 1 GiB = 6 GiB (gm200/gp100 FBP walk).
	mg_set(&g_m, 0x022438, 3);
	mg_set(&g_m, 0x022458, 2);
	for (i = 0; i < 6; i++)
		mg_set(&g_m, 0x90020c + i * 0x4000, 1024);
	mg_set(&g_m, 0x002004, 4);
	g_m.runl[5].is_ce = true;
	g_m.runl[6].is_ce = true;
}

static unsigned g_ns;

static void
on_ns(void *ctx)
{
	(void)ctx;
	g_ns++;
}

int
main(void)
{
	struct pg106_hw_bus bus;
	struct pg106_gpu_cfg cfg;
	struct pg106_gpu *g = calloc(1, sizeof(*g));
	struct pg106_gpu_selftest st;

	printf("== SIMULATION: pg106_gpu bring-up + self-test on tests/mockgpu.h ==\n");
	setup_chip();
	bus = (struct pg106_hw_bus){ &g_m, mg_rd32, mg_wr32, mg_delay, NULL, 0 };
	memset(&cfg, 0, sizeof(cfg));
	cfg.bus = &bus;
	cfg.bar1_size = 0x10000000;
	cfg.bar2_size = 0x2000000;
	cfg.bar1_io = (struct pg106_bar1_io){ &g_m, io_wr, io_rd };
	cfg.stop_after = PG106_DB_TOP;
	CHECK_EQ(pg106_gpu_boot(g, &cfg), 0);
	CHECK_EQ(g->stage, PG106_DB_TOP);
	CHECK_EQ(g->vram_size, 0x180000000ull);
	CHECK(g->bar2_up && g->bar1_up && g->fifo_up && g->intr_up);
	CHECK(!pg106_gpu_ready(g)); // stopped before READY
	CHECK_EQ(g->bar2.inst, 0x17feff000ull); // first tail object, as nouveau
	CHECK_EQ(mg_get(&g_m, 0x1714), 0x80000000u | (uint32_t)(g->bar2.inst >> 12));
	CHECK_EQ(mg_get(&g_m, 0x1704), 0x80000000u | (uint32_t)(g->bar1.inst >> 12));
	CHECK_EQ(g_m.intr_allow, PG106_PMC_INTR_FIFO);
	CHECK_EQ(g_m.pfifo_en & 0x80000000u, 0x80000000u); // non-stall on
	pg106_gpu_set_nonstall_cb(g, on_ns, NULL);

	CHECK_EQ(pg106_gpu_selftest(g, 100000, &st), 0);
	CHECK(st.failed_at == NULL);
	if (st.failed_at)
		printf("selftest failed at: %s (%d); mock: %s\n", st.failed_at, st.ret,
		       g_m.nerr ? g_m.err[0] : "-");
	CHECK_EQ(st.runlist, 5);
	CHECK_EQ(st.sem_value, 0x5a5a0001u);
	CHECK_EQ(st.ce_sem_value, 0x5a5a0002u);
	CHECK_EQ(st.ce_mismatch, 0);
	CHECK_EQ(st.ce_bytes, 0x10000);
	CHECK_EQ(st.gp_get, 2);
	CHECK_EQ(st.nonstall_events, 1);
	CHECK_EQ(g_ns, 1);
	CHECK_EQ(g_m.nerr, 0);
	CHECK_EQ(g_m.kicks, 2);
	printf("self-test: runlist %d chid %u, host semaphore 0x%08x, CE sem 0x%08x, "
	       "%u bytes copied, %u mismatches, non-stall events %llu\n",
	       st.runlist, st.chid, st.sem_value, st.ce_sem_value, st.ce_bytes,
	       st.ce_mismatch, (unsigned long long)st.nonstall_events);

	// A PBDMA that never runs: the self-test times out and says where.
	g_m.runl[5].is_ce = true;
	{
		unsigned i;

		for (i = 0; i < MG_NCHAN; i++)
			g_m.chan[i].enabled = false;
	}
	g_m.wr_hook = NULL;
	{
		// break the kick: point USERD window elsewhere
		uint32_t saved = mg_get(&g_m, 0x2254);

		mg_set(&g_m, 0x2254, 0);
		g_m.nerr = 0;
		// chan_create re-enables via PCCSR; but the kick misses USERD
		// once 0x2254 lacks its valid bit.
		CHECK_EQ(pg106_gpu_selftest(g, 1000, &st), -ETIMEDOUT);
		CHECK(st.failed_at != NULL);
		mg_set(&g_m, 0x2254, saved);
	}
	{
		uint64_t before = g->vram.mm.free_bytes;

		CHECK_EQ(pg106_gpu_selftest(g, 100000, &st), 0);
		CHECK_EQ(g->vram.mm.free_bytes, before); // everything freed
	}
	pg106_gpu_fini(g);
	CHECK_EQ(g_m.intr_allow, 0);
	mg_free(&g_m);

	// Unposted GPU without a VBIOS: stops at POST, nothing else is set up.
	setup_chip();
	mg_set(&g_m, 0x02240c, 0);
	CHECK(pg106_gpu_boot(g, &cfg) != 0);
	CHECK_EQ(g->stage, PG106_DB_POST);
	CHECK(!g->vram_up && !g->fifo_up);
	pg106_gpu_fini(g);
	mg_free(&g_m);
	free(g);
	printf("%s: %d checks, %d failures\n", g_fail ? "FAILED" : "PASSED",
	       g_checks, g_fail);
	return g_fail ? 1 : 0;
}
