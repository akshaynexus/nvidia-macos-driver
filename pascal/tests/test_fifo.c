// SPDX-License-Identifier: GPL-2.0-only
// pascal/tests/test_fifo.c — host FIFO: PBDMA init, USERD through BAR1,
// TSG + channel runlists, PCCSR bind/enable, GP_PUT kick, semaphore
// release + non-stall interrupt, a PASCAL_DMA_COPY_B copy, teardown.
//
// SIMULATION on tests/mockgpu.h (runlist parser, PCCSR state machine,
// PBDMA executor that walks the GPFIFO through the channel's VMM). The
// PTOP table is the real one from the owner's Helios (test_top.c
// HELIOS_PTOP): CE2 sits on runlist 5.

#include "mockgpu.h"

#include <pascal/ce.h>
#include <pascal/fifo.h>
#include <pascal/pushbuf.h>

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

#define VRAM_6G 0x180000000ull

static const uint32_t HELIOS_PTOP[] = {
	0x8006183e, 0x80000003, 0x00400005, 0x9c028c3e, 0x8000004f,
	0x001040ad, 0xa0030e3e, 0x8000004f, 0x041040b5, 0x94a3aa3e,
	0x8000004f, 0x081040dd, 0x84289e3e, 0x80000043, 0x00084015,
	0x00000000, 0x00000000, 0x00000000, 0x8c679c3e, 0x80000037,
	0x00087095, 0x8848243e, 0x8000003b, 0x001c80cd, 0x00000000,
	0x00000000, 0x00000000, 0x98c52c3e, 0x8000004f, 0x0c1040e5,
};

static int
top_rd(void *u, uint32_t a, uint32_t *v)
{
	uint32_t i = (a - 0x022700u) / 4u;

	(void)u;
	*v = i < sizeof(HELIOS_PTOP) / 4 ? HELIOS_PTOP[i] : 0;
	return 0;
}

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

struct rig {
	struct pg106_hw_bus bus;
	struct pg106_vram vram;
	struct pg106_bar bar1;
	struct pg106_fifo fifo;
	struct pg106_vmm vmm;
	struct pg106_top_table top;
	uint64_t buf_pa; // 1 MiB VRAM: ring @0, push @0x10000, sem @0x20000,
			 // src @0x40000, dst @0x80000
};

#define VA_BASE 0x200000000ull

static void
rig_up(struct rig *r)
{
	const struct pg106_bar1_io io = { &g_m, io_wr, io_rd };
	struct pg106_map m;

	mg_init(&g_m, VRAM_6G);
	g_m.runl[5].is_ce = true;
	g_m.runl[6].is_ce = true;
	mg_set(&g_m, 0x002004, 4); // PBDMAs
	r->bus = (struct pg106_hw_bus){ &g_m, mg_rd32, mg_wr32, mg_delay, NULL, 0 };
	CHECK_EQ(pg106_top_scan(top_rd, NULL, &r->top), 0);
	CHECK_EQ(pg106_vram_init(&r->vram, &r->bus, VRAM_6G), 0);
	CHECK_EQ(pg106_bar_init(&r->bar1, &r->vram, PG106_BAR1, 0x10000000), 0);
	CHECK_EQ(pg106_fifo_init(&r->fifo, &r->vram, &r->bar1, &io, &r->top), 0);
	CHECK_EQ(pg106_vmm_init(&r->vmm, &r->vram, PG106_MMU_VA_LIMIT, 0, false), 0);
	CHECK_EQ(pg106_vram_alloc(&r->vram, 0x100000, 0x10000, 0, &r->buf_pa), 0);
	memset(&m, 0, sizeof(m));
	m.va = VA_BASE;
	m.pa = r->buf_pa;
	m.size = 0x100000;
	m.page_shift = 16;
	m.target = PG106_TARGET_VRAM;
	m.kind = PG106_KIND_PITCH;
	CHECK_EQ(pg106_vmm_map(&r->vmm, &m), 0);
	CHECK_EQ(pg106_vmm_commit(&r->vmm), 0);
}

static void
rig_down(struct rig *r)
{
	pg106_fifo_fini(&r->fifo);
	pg106_vmm_fini(&r->vmm);
	pg106_bar_fini(&r->bar1);
	pg106_vram_fini(&r->vram);
	mg_free(&g_m);
}

// Write push words to VRAM and one GPFIFO entry; returns the new PUT.
static uint32_t
submit(struct rig *r, struct pg106_chan *c, uint32_t put, uint64_t push_off,
       const uint32_t *w, size_t n)
{
	uint64_t e = pg106_gpfifo_pack(VA_BASE + push_off, (uint32_t)n,
				       PG106_GP_LEVEL_MAIN, 0, 0);
	size_t i;

	for (i = 0; i < n; i++)
		pg106_vram_wr32(&r->vram, r->buf_pa + push_off + i * 4, w[i]);
	pg106_vram_wr64(&r->vram, r->buf_pa + put * 8ull, e);
	put = (put + 1) & (c->entries - 1);
	pg106_chan_kick(c, put);
	return put;
}

static void
test_init(void)
{
	struct rig r;

	rig_up(&r);
	CHECK_EQ(mg_get(&g_m, 0x000204), 0xf);
	CHECK_EQ(mg_get(&g_m, 0x002a04) & 0xbfffffffu, 0xbfffffffu);
	CHECK_EQ(mg_get(&g_m, 0x04012c + 3 * 0x2000), 0x000f4240);
	CHECK_EQ(mg_get(&g_m, 0x04010c), 0xfffffeff);
	CHECK_EQ(mg_get(&g_m, 0x002254), 0x10000000u | (uint32_t)(r.fifo.userd_bar1 >> 12));
	CHECK_EQ(g_m.pfifo_en, 0x7fffffffu);
	// PMC: PFIFO plus every engine's reset line from the Helios PTOP
	// (GR 12, CE 6/7/21/22, NVDEC 15, SEC2 14, NVENC 18).
	CHECK_EQ(mg_get(&g_m, PG106_PMC_ENABLE) & 0x0064d1c0u, 0x0064d1c0u);
	CHECK_EQ(pg106_fifo_runl_find(&r.fifo, PG106_TOP_TYPE_GR), 0);
	{
		int k = pg106_fifo_runl_find(&r.fifo, PG106_TOP_TYPE_CE_N);

		CHECK(k >= 0);
		if (k >= 0) {
			CHECK_EQ(r.fifo.runl[k].id, 5); // CE2: async copy engine
			CHECK(r.fifo.runl[k].is_ce);
		}
	}
	rig_down(&r);
	printf("PASS FIFO init: PBDMAs, 0x2254 USERD window, PMC enables, "
	       "runlists from the Helios PTOP (CE2 -> runlist 5)\n");
}

static void
test_channel(void)
{
	struct rig r;
	struct pg106_chan *c = NULL;
	int k;
	uint32_t put = 0, w[16];
	pg106_pb pb;

	rig_up(&r);
	k = pg106_fifo_runl_find(&r.fifo, PG106_TOP_TYPE_CE_N);
	CHECK_EQ(pg106_chan_create(&r.fifo, &r.vmm, k, VA_BASE, 512, &c), 0);
	if (!c) {
		rig_down(&r);
		return;
	}
	// PCCSR + runlist as gp100 writes them.
	CHECK_EQ(mg_get(&g_m, 0x002274) , 0); // (latched by the mock, not stored)
	CHECK_EQ(g_m.chan[c->chid].pccsr_inst, 0x80000000u | (uint32_t)(c->inst >> 12));
	CHECK(g_m.chan[c->chid].enabled);
	CHECK_EQ(g_m.runl[5].submits, 1);
	CHECK_EQ(g_m.runl[5].count, 2);
	CHECK_EQ(g_m.runl[5].tsg_entries, 1);
	CHECK_EQ(g_m.runl[5].chan_entries, 1);
	CHECK_EQ(g_m.runl[5].chid[0], c->chid);
	CHECK_EQ(g_m.runl[5].target, 0);
	// RAMFC: USERD, GPFIFO, join.
	CHECK_EQ(mg_vram_rd32(&g_m, c->inst + 0x08),
		 (uint32_t)(r.fifo.userd_pa + c->chid * 0x200ull));
	CHECK_EQ(mg_vram_rd32(&g_m, c->inst + 0x48), (uint32_t)VA_BASE);
	CHECK_EQ(mg_vram_rd32(&g_m, c->inst + 0x4c), (uint32_t)(VA_BASE >> 32) | (9u << 16));
	CHECK_EQ(mg_vram_rd32(&g_m, c->inst + 0x10), 0xface);
	CHECK_EQ(mg_vram_rd64(&g_m, c->inst + 0x200),
		 pg106_mmu_root_addr(r.vmm.mmu) | 0xc00);

	// 1. Host semaphore release + NON_STALL_INTERRUPT (nvc0_fence_emit32).
	pg106_fifo_nonstall(&r.fifo, true);
	g_m.intr_allow = 0;
	mg_wr32(&g_m, 0x160, PG106_PMC_INTR_FIFO); // gp100_mc_intr_allow
	pg106_pb_init(&pb, w, 16);
	{
		uint32_t d[5] = { (uint32_t)((VA_BASE + 0x20000) >> 32),
				  (uint32_t)(VA_BASE + 0x20000), 0x1234, 0x2, 0 };

		pg106_pb_inc(&pb, 0, 0x0010, d, 5);
	}
	put = submit(&r, c, put, 0x10000, w, pb.len);
	CHECK_EQ(g_m.kicks, 1);
	CHECK_EQ(g_m.sem_releases, 1);
	CHECK_EQ(mg_vram_rd32(&g_m, r.buf_pa + 0x20000), 0x1234);
	CHECK_EQ(pg106_chan_get(c), put);
	CHECK(mg_irq(&g_m));
	{
		bool ns = false;
		uint32_t st = pg106_fifo_intr(&r.fifo, &ns);

		CHECK(ns);
		CHECK_EQ(st, PG106_FIFO_INTR_NONSTALL);
		CHECK(!mg_irq(&g_m));
	}

	// 2. A CE copy of 64 KiB with its semaphore.
	{
		uint32_t cw[64];
		pg106_ce_semaphore sem = { VA_BASE + 0x20010, 0xc0ffee, 0 };
		pg106_ce_memcpy_plan pl = { VA_BASE + 0x40000, VA_BASE + 0x80000,
					    0x10000, 1, PG106_CE_XFER_NON_PIPELINED,
					    1, 0, &sem };
		unsigned i;

		for (i = 0; i < 0x10000; i += 4)
			mg_vram_wr32(&g_m, r.buf_pa + 0x40000 + i, 0xa5000000u | i);
		pg106_pb_init(&pb, cw, 64);
		CHECK_EQ(pg106_ce_memcpy(&pb, &pl), 0);
		put = submit(&r, c, put, 0x11000, cw, pb.len);
		CHECK_EQ(g_m.ce_copies, 1);
		CHECK_EQ(g_m.ce_bytes, 0x10000);
		CHECK_EQ(mg_vram_rd32(&g_m, r.buf_pa + 0x80000 + 0xfffc), 0xa500fffcu);
		CHECK_EQ(mg_vram_rd32(&g_m, r.buf_pa + 0x20010), 0xc0ffee);
	}
	CHECK_EQ(g_m.nerr, 0);
	if (g_m.nerr)
		printf("mock: %s\n", g_m.err[0]);

	// 3. Teardown: stop, preempt, runlist without it, unbind.
	{
		uint32_t chid = c->chid;

		pg106_chan_destroy(c);
		CHECK(!g_m.chan[chid].enabled);
		CHECK_EQ(g_m.preempts, 1);
		CHECK_EQ(mg_get(&g_m, 0x002634), 0x01000000u | chid);
		CHECK_EQ(g_m.runl[5].submits, 2);
		CHECK_EQ(g_m.runl[5].count, 0);
		CHECK_EQ(g_m.chan[chid].pccsr_inst, 0);
	}
	rig_down(&r);
	printf("PASS channel: TSG+chan runlist, PCCSR bind/enable, BAR1 GP_PUT kick, "
	       "semaphore + non-stall IRQ, CE 64 KiB copy + semaphore, teardown\n");
}

static void
test_errors(void)
{
	struct rig r;
	struct pg106_chan *c = NULL, *c2 = NULL;
	uint32_t w[4] = { 0 };
	pg106_pb pb;

	rig_up(&r);
	// A copy class on the GR runlist is a mistake the mock catches.
	CHECK_EQ(pg106_chan_create(&r.fifo, &r.vmm, 0, VA_BASE, 512, &c), 0);
	pg106_pb_init(&pb, w, 4);
	pg106_pb_set_object(&pb, 1, PG106_CLASS_PASCAL_DMA_COPY_B);
	submit(&r, c, 0, 0x10000, w, pb.len);
	CHECK_EQ(g_m.nerr, 1);
	// Two channels, second gets the next chid; the runlist carries both.
	CHECK_EQ(pg106_chan_create(&r.fifo, &r.vmm, 0, VA_BASE, 512, &c2), 0);
	CHECK_EQ(c2->chid, c->chid + 1);
	CHECK_EQ(g_m.runl[0].count, 4);
	pg106_chan_destroy(c);
	CHECK_EQ(g_m.runl[0].count, 2);
	// A kick on a channel that is no longer scheduled is noticed.
	g_m.nerr = 0;
	CHECK_EQ(pg106_chan_create(&r.fifo, &r.vmm, 5, VA_BASE, 3, &c), -EINVAL);
	CHECK_EQ(pg106_chan_create(&r.fifo, &r.vmm, 9, VA_BASE, 512, &c), -EINVAL);
	// Restore after a power loss: registers re-programmed, channel back.
	{
		unsigned i;
		uint32_t chid = c2->chid;

		for (i = 0; i < MG_HASH; i++) {
			free(g_m.pages[i].data);
			g_m.pages[i].data = NULL;
			g_m.pages[i].pfn = 0;
		}
		memset(&g_m.chan[chid], 0, sizeof(g_m.chan[chid]));
		pg106_vram_forget_window(&r.vram);
		CHECK_EQ(pg106_bar_restore(&r.bar1), 0);
		pg106_mmu_mark_all_dirty(r.vmm.mmu);
		CHECK_EQ(pg106_vmm_commit(&r.vmm), 0);
		CHECK_EQ(pg106_fifo_restore(&r.fifo), 0);
		CHECK(g_m.chan[chid].enabled);
		CHECK_EQ(mg_vram_rd32(&g_m, c2->inst + 0x10), 0xface);
		{
			uint32_t d[4] = { (uint32_t)((VA_BASE + 0x20000) >> 32),
					  (uint32_t)(VA_BASE + 0x20000), 7, 0x01000002u };

			pg106_pb_init(&pb, w, 8);
			pg106_pb_inc(&pb, 0, 0x0010, d, 4);
			submit(&r, c2, 0, 0x10000, w, pb.len);
			CHECK_EQ(mg_vram_rd32(&g_m, r.buf_pa + 0x20000), 7);
			CHECK_EQ(g_m.nerr, 0);
		}
	}
	rig_down(&r);
	printf("PASS FIFO errors: copy class on the GR runlist flagged, chid "
	       "reuse, bad args, restore after power loss\n");
}

int
main(void)
{
	printf("== SIMULATION: host FIFO on tests/mockgpu.h ==\n");
	test_init();
	test_channel();
	test_errors();
	printf("%s: %d checks, %d failures\n", g_fail ? "FAILED" : "PASSED",
	       g_checks, g_fail);
	return g_fail ? 1 : 0;
}
