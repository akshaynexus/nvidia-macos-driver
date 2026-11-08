// SPDX-License-Identifier: GPL-2.0-only
// pascal/tests/test_grctx.c — the GR golden context and a render channel
// end to end on tests/mockgpu.h:
//
//   pg106_gpu_boot (to PTOP) -> the real GR stage (pg106_devboot_gr: the
//   GR plan on the mock, the SEC2 bootstrap replaced by a hook, the mock
//   FECS answering the size queries) -> pg106_gr_init + pg106_gr_golden
//   (FE power, FECS reset, BIND_POINTER, sw_ctx / bundle / method lists
//   from the real gp106 firmware files, WFI_GOLDEN_SAVE through the bound
//   instance block) -> libpg106_bridge over loopback -> kdisp: VM_INIT
//   binds a channel on GR's runlist with a per-channel context (the
//   global buffers, the mmio list and the golden copy mapped into the
//   client VM by pg106_kd_map_vram), BO_ALLOC in VRAM, VM_BIND, EXEC of a
//   PASCAL_COMPUTE_B dispatch (SET_OBJECT, NVK's compute state, a v02_01
//   QMD with RELEASE0) -> the mock loads the context on the kick, applies
//   the mmio list, runs the QMD's semaphore release, then the kernel
//   fence -> syncobj wait.
//
// SIMULATION. The mock FECS checks register semantics (what is bound,
// what the context pointer translates to, the image header); it does not
// know what the real ucode saves, so the golden image here is the mock's.

#include "mockgpu.h"

#include <pascal/dispatch.h>
#include <pascal/gpu.h>
#include <pascal/gr_ctx.h>
#include <pascal/kdisp.h>
#include <pascal/pushbuf.h>
#include <pascal/qmd.h>
#include <pascal/uapi.h>

#include "pg106_br_bridge.h"
#include "pg106_br_loopback.h"
#include "pg106_br_wire.h"

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

#define IB_MAX 64u
#define KERN_VA (1ull << 39)
#define CTX_SIZE 0x00012000u // the mock FECS's image size (real: ~MiB)
#define FW_DIR "../research/firmware/gp106/gr/"

static const uint32_t HELIOS_PTOP[] = {
	0x8006183e, 0x80000003, 0x00400005, 0x9c028c3e, 0x8000004f,
	0x001040ad, 0xa0030e3e, 0x8000004f, 0x041040b5, 0x94a3aa3e,
	0x8000004f, 0x081040dd, 0x84289e3e, 0x80000043, 0x00084015,
	0x00000000, 0x00000000, 0x00000000, 0x8c679c3e, 0x80000037,
	0x00087095, 0x8848243e, 0x8000003b, 0x001c80cd, 0x00000000,
	0x00000000, 0x00000000, 0x98c52c3e, 0x8000004f, 0x0c1040e5,
};

static struct mockgpu g_m;

// PG106_GRCTX_DUMP=<file>: every BAR0 write during the golden generation
// as "addr value", for scripts/compare_grctx_trace.py against nouveau's
// mmiotrace of the same GPU (research/hardware/helios300/raw, gitignored).
static FILE *g_dump;

static bool
dump_wr(struct mockgpu *m, uint32_t a, uint32_t v)
{
	(void)m;
	if (g_dump)
		fprintf(g_dump, "%06x 0x%x\n", a, v);
	return false; // the mock still processes the write
}

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

#define GPC_UNIT(g, r) (0x500000u + (g) * 0x8000u + (r))

// The Helios GP106: 2 GPCs x 5 TPCs, PPC masks 0x9 / 0x12 / 0x4 per GPC
// (hardware run 3, posttrace2: 0x502608 = 0x40005, 0x500c30.. = 9/0x12/4).
static void
setup_chip(void)
{
	unsigned i, g;

	mg_init(&g_m, 0x180000000ull);
	mg_set(&g_m, 0x000000, 0x136000a1u); // BOOT_0 GP106 A1
	mg_set(&g_m, 0x02240c, 0x2);         // posted
	for (i = 0; i < sizeof(HELIOS_PTOP) / 4; i++)
		mg_set(&g_m, 0x022700 + i * 4, HELIOS_PTOP[i]);
	mg_set(&g_m, 0x022438, 3);
	mg_set(&g_m, 0x022458, 2);
	for (i = 0; i < 6; i++)
		mg_set(&g_m, 0x90020c + i * 0x4000, 1024);
	mg_set(&g_m, 0x002004, 4);
	mg_set(&g_m, 0x000200, 0x40003120u | 0x1000); // PMC_ENABLE with GR
	// GR topology (gf100_gr_oneinit reads).
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
}

static int
bootstrap_hook(void *u, uint64_t mask)
{
	(void)u;
	CHECK_EQ(mask, (1ull << 2) | (1ull << 3)); // FECS | GPCCS
	g_m.gr.bootstrapped = true;
	return 0;
}

static uint8_t *
load_blob(const char *name, size_t *n)
{
	char path[256];
	FILE *f;
	uint8_t *b;
	long sz;

	snprintf(path, sizeof(path), FW_DIR "%s", name);
	f = fopen(path, "rb");
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

// ---- the kdisp host: VRAM + host BOs, a GR channel with a context ----------------

struct hmem {
	uint8_t *p;      // host pages (NULL for VRAM)
	uint64_t pa;     // VRAM address
	uint64_t size;
};

struct sys {
	struct pg106_gpu *g;
	struct pg106_gr *gr;
	struct pg106_kd *kd;
	int runl_gr;
	uint64_t tables, tables_size;
	struct pg106_grctx_chan gc;
	struct pg106_chan *chan;
	struct pg106_chan *bare; // a GR channel without a context (negative)
	size_t allocs, frees, vram_allocs, kicks, flushes;
	struct hmem *mapped[256];
};

static int
m_alloc(void *u, uint64_t size, uint64_t align, uint32_t flags, void **mem)
{
	struct sys *s = u;
	struct hmem *h = calloc(1, sizeof(*h));

	if (!h)
		return -ENOMEM;
	h->size = size;
	if (flags & PG106_BR_BO_VRAM) {
		int rc = pg106_vram_alloc(&s->g->vram, size, align < 0x1000 ? 0x1000 : align,
					  PG106_VRAM_ZERO, &h->pa);

		if (rc) {
			free(h);
			return rc;
		}
		s->vram_allocs++;
	} else {
		h->p = aligned_alloc(4096, (size + 4095) & ~4095ull);
		if (!h->p) {
			free(h);
			return -ENOMEM;
		}
		memset(h->p, 0, size);
		mg_add_host(&g_m, (uint64_t)(uintptr_t)h->p, h->p, size);
	}
	s->allocs++;
	*mem = h;
	return 0;
}

static void
m_free(void *u, void *mem)
{
	struct sys *s = u;
	struct hmem *h = mem;

	if (h->p)
		free(h->p);
	else
		pg106_vram_free(&s->g->vram, h->pa, h->size);
	free(h);
	s->frees++;
}

static int
m_dma(void *u, void *mem, uint64_t off, uint64_t *addr, uint64_t *contig)
{
	struct hmem *h = mem;

	(void)u;
	if (off >= h->size)
		return -EFAULT;
	*addr = (h->p ? (uint64_t)(uintptr_t)h->p : h->pa) + off;
	*contig = h->size - off;
	return 0;
}

static int
m_target(void *u, void *mem)
{
	(void)u;
	return ((struct hmem *)mem)->p ? PG106_TARGET_SYS_COHERENT : PG106_TARGET_VRAM;
}

static void *
m_cpu(void *u, void *mem)
{
	(void)u;
	return ((struct hmem *)mem)->p;
}

static int
m_map(void *u, void *mem, uint32_t handle, uint64_t *cookie)
{
	struct sys *s = u;
	struct hmem *h = mem;

	if (handle >= 256 || !h->p)
		return -ENODEV; // VRAM BOs would need BAR1 here
	s->mapped[handle] = h;
	*cookie = handle;
	return 0;
}

static void
m_unmap(void *u, void *mem, uint64_t cookie)
{
	(void)mem;
	if (cookie < 256)
		((struct sys *)u)->mapped[cookie] = NULL;
}

static int
gm_map(void *u, uint64_t pa, uint64_t size, bool priv, uint64_t *va)
{
	return pg106_kd_map_vram(((struct sys *)u)->kd, pa, size, priv, va);
}

static void
gm_unmap(void *u, uint64_t va, uint64_t size)
{
	pg106_kd_unmap_vram(((struct sys *)u)->kd, va, size);
}

// What the kext's claimChannel does once the GR context is ported.
static int
m_chan_bind(void *u, const struct pg106_kd_chan *ch)
{
	struct sys *s = u;
	const struct pg106_grctx_map map = { s, gm_map, gm_unmap };
	struct pg106_chan_desc d;
	int rc;

	rc = pg106_grctx_chan_create(s->gr, &map, &s->gc);
	if (rc)
		return rc;
	memset(&d, 0, sizeof(d));
	d.pdb_addr = ch->pdb_addr;
	d.pdb_target = ch->pdb_target;
	d.vm_limit = ch->vm_limit;
	d.gpfifo_va = ch->ring_va;
	d.entries = ch->ring_entries;
	d.ectx = pg106_grctx_chan_ectx(&s->gc);
	rc = pg106_chan_create_desc(&s->g->fifo, s->runl_gr, &d, &s->chan);
	if (rc) {
		pg106_grctx_chan_destroy(s->gr, &map, &s->gc);
		return rc;
	}
	return 0;
}

static void
m_chan_unbind(void *u)
{
	struct sys *s = u;
	const struct pg106_grctx_map map = { s, gm_map, gm_unmap };

	pg106_chan_destroy(s->chan);
	s->chan = NULL;
	pg106_grctx_chan_destroy(s->gr, &map, &s->gc);
}

static int
m_vm_flush(void *u, const struct pg106_mmu *mmu)
{
	struct sys *s = u;

	s->flushes++;
	return pg106_mmu_upload(&s->g->vram, (struct pg106_mmu *)mmu, s->tables,
				s->tables_size, true);
}

static void
m_kick(void *u, uint32_t put)
{
	struct sys *s = u;

	s->kicks++;
	pg106_bar_flush(s->g->cfg.bus);
	pg106_chan_kick(s->chan, put);
}

static void
m_delay(void *u, uint32_t us)
{
	(void)u;
	mg_delay(&g_m, us);
}

static uint64_t
m_now(void *u)
{
	(void)u;
	return g_m.t_us * 1000;
}

static int
serve(void *srv, const void *req, size_t req_len, void *resp, size_t cap, size_t *len)
{
	return pg106_kd_handle(((struct sys *)srv)->kd, req, req_len, resp, cap, len);
}

static int
lb_map(void *srv, uint64_t cookie, uint64_t size, void **ptr)
{
	struct sys *s = srv;

	if (cookie >= 256 || !s->mapped[cookie] || s->mapped[cookie]->size < size)
		return -ENOENT;
	*ptr = s->mapped[cookie]->p;
	return 0;
}

int
main(void)
{
	struct pg106_hw_bus bus;
	struct pg106_gpu_cfg cfg;
	struct pg106_devboot_cfg dc;
	struct pg106_devboot_fw fw;
	struct pg106_gpu *g = calloc(1, sizeof(*g));
	struct pg106_gr gr;
	struct pg106_gr_cfg gcfg;
	struct pg106_gr_fw gfw;
	struct sys s;
	uint8_t *nonctx, *swctx, *bundle, *method;
	size_t n_nonctx, n_swctx, n_bundle, n_method, i;
	int k, ret;

	printf("== SIMULATION: GR golden context + render channel on tests/mockgpu.h ==\n");
	nonctx = load_blob("sw_nonctx.bin", &n_nonctx);
	swctx = load_blob("sw_ctx.bin", &n_swctx);
	bundle = load_blob("sw_bundle_init.bin", &n_bundle);
	method = load_blob("sw_method_init.bin", &n_method);
	if (!nonctx || !swctx || !bundle || !method) {
		printf("FAIL: " FW_DIR " firmware lists missing\n");
		return 1;
	}
	CHECK_EQ(n_swctx % 12, 0);
	CHECK_EQ(n_bundle % 8, 0);
	CHECK_EQ(n_method % 8, 0);

	// 1. Bring-up to PTOP + FIFO, then the GR stage on the mock.
	setup_chip();
	bus = (struct pg106_hw_bus){ &g_m, mg_rd32, mg_wr32, mg_delay, NULL, 0 };
	memset(&cfg, 0, sizeof(cfg));
	cfg.bus = &bus;
	cfg.bar1_size = 0x10000000;
	cfg.bar2_size = 0x2000000;
	cfg.bar1_io = (struct pg106_bar1_io){ &g_m, io_wr, io_rd };
	cfg.stop_after = PG106_DB_TOP;
	CHECK_EQ(pg106_gpu_boot(g, &cfg), 0);
	CHECK(g->fifo_up);
	memset(&fw, 0, sizeof(fw));
	fw.sw_nonctx.data = nonctx;
	fw.sw_nonctx.size = n_nonctx;
	memset(&dc, 0, sizeof(dc));
	dc.bus = &bus;
	dc.fw = &fw;
	dc.bootstrap = bootstrap_hook;
	ret = pg106_devboot_gr(&g->db, &dc);
	CHECK_EQ(ret, 0);
	if (ret)
		printf("GR stage failed at step %zu: %s\n", g->db.exec.step,
		       g->db.exec.why ? g->db.exec.why : "?");
	CHECK_EQ(g->db.stage, PG106_DB_READY);
	CHECK_EQ(g->db.gr_nsizes, 3);
	CHECK_EQ(g->db.gr_sizes[0], CTX_SIZE);
	CHECK_EQ(g->db.gr_info.tpc_total, 10);
	CHECK(g_m.gr.mbox_ready);
	CHECK_EQ(g_m.nerr, 0);
	printf("GR stage: %u GPCs, %u TPCs, FECS ctx size 0x%x (%zu plan steps)\n",
	       g->db.gr_cfg.gpc_nr, g->db.gr_info.tpc_total, g->db.gr_sizes[0],
	       g->db.gr_plan.nsteps);

	// 2. The engine + the golden context.
	memset(&gfw, 0, sizeof(gfw));
	gfw.sw_ctx = swctx;
	gfw.sw_ctx_len = n_swctx;
	gfw.sw_bundle_init = bundle;
	gfw.sw_bundle_init_len = n_bundle;
	gfw.sw_method_init = method;
	gfw.sw_method_init_len = n_method;
	memset(&gcfg, 0, sizeof(gcfg));
	gcfg.bus = &bus;
	gcfg.vram = &g->vram;
	gcfg.topo = &g->db.gr_cfg;
	gcfg.ctx_size = g->db.gr_sizes[0];
	gcfg.fw = &gfw;
	for (i = 0; i < g->db.top.ndev; i++)
		if (g->db.top.dev[i].type == PG106_TOP_TYPE_GR)
			gcfg.gr_engine = (unsigned)g->db.top.dev[i].engine;
	CHECK_EQ(pg106_gr_init(&gr, &gcfg), 0);
	CHECK_EQ(gr.gpc_nr, 2);
	CHECK_EQ(gr.tpc_total, 10);
	CHECK_EQ(gr.tpc_max, 5);
	CHECK_EQ(gr.ppc_nr[0], 3);
	CHECK_EQ(gr.ppc_mask[0], 0x7);
	CHECK_EQ(gr.ppc_tpc_nr[0][0], 2);
	CHECK_EQ(gr.ppc_tpc_nr[0][1], 2);
	CHECK_EQ(gr.ppc_tpc_nr[0][2], 1);
	CHECK_EQ(gr.ppc_tpc_min, 1);
	CHECK_EQ(gr.ppc_tpc_max, 2);
	CHECK_EQ(gr.sm_nr, 10);
	// gf100_gr_oneinit_sm_id: SM n alternates GPCs (tpc-major order).
	CHECK_EQ(gr.sm[0].gpc, 0);
	CHECK_EQ(gr.sm[1].gpc, 1);
	CHECK_EQ(gr.sm[2].tpc, 1);
	// 10 TPCs: screen_tile_row_offset 6 (gf100_gr_oneinit_tiles table).
	CHECK_EQ(gr.screen_tile_row_offset, 6);
	CHECK_EQ(gr.tile[0], 0);
	CHECK(gr.tile[9] < 2 && gr.tile[10] == 0xff);
	CHECK_EQ(gr.sw_ctx_n, n_swctx / 12);
	CHECK_EQ(gr.bundle_n, n_bundle / 8);
	CHECK_EQ(gr.method_n, n_method / 8);
	// gp102 attrib_cb size: (0xc00*10 + 2 * 0xba8*3*2) * 0x20, 128-aligned.
	CHECK_EQ(gr.attrib_size, (((0xc00u * 10 + 2 * (0xba8u * 3 * 2)) * 0x20) + 127) & ~127u);
	CHECK(gr.pagepool_pa && gr.bundle_pa && gr.attrib_pa);

	if (getenv("PG106_GRCTX_DUMP")) {
		g_dump = fopen(getenv("PG106_GRCTX_DUMP"), "w");
		g_m.wr_hook = dump_wr;
	}
	ret = pg106_gr_golden(&gr);
	g_m.wr_hook = NULL;
	if (g_dump) {
		fclose(g_dump);
		g_dump = NULL;
	}
	CHECK_EQ(ret, 0);
	if (ret)
		printf("golden failed: %s (FECS 0x409800 = 0x%08x); mock: %s\n",
		       gr.failed_at ? gr.failed_at : "?", gr.fecs_stat,
		       g_m.nerr ? g_m.err[0] : "-");
	CHECK(gr.golden != NULL);
	CHECK_EQ(g_m.nerr, 0);
	CHECK(g_m.gr.golden_saved);
	CHECK_EQ(g_m.gr.fecs_resets, 1);
	CHECK_EQ(g_m.gr.bind_errors, 0);
	CHECK_EQ(g_m.gr.save_errors, 0);
	CHECK(g_m.gr.scc_init);
	CHECK_EQ(mg_get(&g_m, 0x000260), 0);      // gp100_mc: no unk260 writes
	CHECK_EQ(g_m.gr.golden_icmds, gr.bundle_n); // every bundle entry went to 0x400200
	CHECK_EQ(g_m.gr.golden_mthds, gr.method_n);
	CHECK(g_m.gr.golden_mmio > gr.sw_ctx_n + gr.bundle_n + gr.method_n);
	if (gr.golden) {
		CHECK_EQ(gr.golden[0], MG_GR_MAGIC);
		CHECK_EQ(gr.golden[CTX_SIZE / 4 - 1], MG_GR_MAGIC ^ CTX_SIZE);
		CHECK_EQ(gr.golden[0x1c / 4], 0); // the header is per channel
	}
	CHECK_EQ(mg_get(&g_m, 0x409b00) & 0x80000000u, 0); // cleared after the save
	CHECK_EQ(mg_get(&g_m, 0x4064c8), (0x180u << 16) | 0x900); // gm107 bundle limits
	CHECK_EQ(mg_get(&g_m, 0x405b00), (10u << 8) | 2);         // gpc_tpc_nr
	CHECK_EQ(mg_get(&g_m, 0x4041c4), 0x1f | (0x1f << 5));     // tpc_mask
	CHECK_EQ(mg_get(&g_m, 0x405830), 0x320);                 // attrib_nr
	CHECK_EQ(mg_get(&g_m, 0x4181e4), 0x100);
	CHECK_EQ(mg_get(&g_m, 0x418bb8), (10u << 8) | 6);         // rop_mapping
	CHECK_EQ(mg_get(&g_m, 0x405870), 0x55);                   // 5 TPCs x 2 GPCs (ds)
	CHECK_EQ(mg_get(&g_m, 0x406028), 0x55);                   // (pd)
	CHECK_EQ(mg_get(&g_m, 0x40802c), 0);                      // not a plain register in the mock
	printf("golden: image 0x%x bytes, %llu register writes, %llu icmds, %llu "
	       "methods, %llu idle waits\n", gr.size, (unsigned long long)gr.mmio_writes,
	       (unsigned long long)gr.icmds, (unsigned long long)gr.mthds,
	       (unsigned long long)gr.idle_waits);

	// 3. A render client through the bridge: channel on GR with a context.
	memset(&s, 0, sizeof(s));
	s.g = g;
	s.gr = &gr;
	k = pg106_fifo_runl_find(&g->fifo, PG106_TOP_TYPE_GR);
	CHECK(k >= 0);
	s.runl_gr = k;
	g_m.runl[g->fifo.runl[k].id].is_gr = true;
	s.tables_size = 0x400000;
	CHECK_EQ(pg106_vram_alloc(&g->vram, s.tables_size, 0x1000, PG106_VRAM_TAIL, &s.tables), 0);
	{
		struct pg106_kd_env e;
		struct pg106_br_loopback lb;
		struct pg106_br_loopback_server srv = { serve, lb_map, NULL, &s };
		struct pg106_br_transport tr;
		struct pg106_br_dev *dev = NULL;
		struct pg106_br_ctx *ctx = NULL;
		uint32_t pbo, vbo, so, *pmap;
		uint64_t v, pva, vva, vbo_pa;
		pg106_pb pb;
		pg106_qmd qmd;
		pg106_qmd_params qp;
		pg106_sub_compute_state st;
		const uint32_t payload = 0xd15bad01u;
		const uint64_t QMD_OFF = 0x0000, PROG_OFF = 0x1000, SEM_OFF = 0x8000;

		memset(&e, 0, sizeof(e));
		e.user = &s;
		e.mem_alloc = m_alloc;
		e.mem_free = m_free;
		e.mem_dma = m_dma;
		e.mem_target = m_target;
		e.mem_cpu = m_cpu;
		e.mem_map = m_map;
		e.mem_unmap = m_unmap;
		e.chan_bind = m_chan_bind;
		e.chan_unbind = m_chan_unbind;
		e.vm_flush = m_vm_flush;
		e.kick = m_kick;
		e.delay_us = m_delay;
		e.now_ns = m_now;
		e.pci_vendor = 0x10de;
		e.pci_device = 0x1c20;
		e.vram_bar_size = 256ull << 20;
		e.vram_size = g->vram_size;
		e.boot0 = 0x136000a1u;
		e.graph_units = 2u | (10u << 8) | (3ull << 32);
		e.ib_max = IB_MAX;
		e.drm_version = 0x01000301u;
		e.abi_major = PG106_BR_ABI_MAJOR;
		e.abi_minor = PG106_BR_ABI_MINOR;
		e.mmu_cfg.table_target = PG106_TARGET_VRAM;
		e.mmu_cfg.table_base = s.tables;
		e.mmu_cfg.table_limit = s.tables + s.tables_size;
		CHECK_EQ(pg106_kd_create(&e, &s.kd), 0);
		CHECK_EQ(pg106_br_loopback_open(&lb, &srv, &tr), 0);
		CHECK_EQ(pg106_br_dev_open(&tr, &dev), 0);
		if (!dev)
			return 1;
		CHECK_EQ(pg106_br_ctx_create(dev, 0, &ctx), 0);
		CHECK_EQ(pg106_br_vm_init(dev, KERN_VA, (1ull << 40) - KERN_VA), 0);
		CHECK(s.chan != NULL);
		CHECK(s.gc.up);
		CHECK_EQ(s.gc.pagepool_va, KERN_VA + PG106_KD_KERNEL_VA_SPAN);
		CHECK(s.gc.ctx_va > s.gc.mmio_va);
		CHECK_EQ(s.gc.mmio_nr, 2 * (4 + 5 + 5 + 3 + 2 * 3 * 7 + 2)); // pagepool, bundle, attrib_cb, gp102 attrib (7 per PPC)
		if (s.chan) {
			CHECK_EQ(s.chan->ectx, s.gc.ctx_va | 4);
			CHECK_EQ(mg_vram_rd64(&g_m, s.chan->inst + 0x210), s.gc.ctx_va | 4);
			CHECK_EQ(g->fifo.runl[s.chan->runl].id, 0); // GR's runlist on the Helios
		}
		CHECK(s.flushes >= 2); // kernel objects, then the context mappings
		CHECK_EQ(g_m.gr.loads, 0); // nothing ran yet
		CHECK_EQ(g_m.nerr, 0);

		// BOs: the push buffer in host memory (mapped), QMD + program +
		// semaphore in VRAM.
		CHECK_EQ(pg106_br_bo_alloc(dev, 0x10000, 0x1000,
					   PG106_BR_BO_GART | PG106_BR_BO_CAN_MAP, &pbo), 0);
		CHECK_EQ(pg106_br_bo_alloc(dev, 0x10000, 0x1000, PG106_BR_BO_VRAM, &vbo), 0);
		CHECK_EQ(s.vram_allocs, 1);
		CHECK_EQ(pg106_br_getparam(dev, PG106_UAPI_GETPARAM_VRAM_USED, &v), 0);
		CHECK_EQ(v, 0x10000);
		CHECK_EQ(pg106_br_bo_map(dev, pbo, 0, (void **)&pmap), 0);
		CHECK_EQ(pg106_br_bo_map(dev, vbo, 0, (void **)&v), -EACCES); // not CAN_MAP
		CHECK_EQ(pg106_br_va_alloc(dev, 0x10000, 0x10000, &pva), 0);
		CHECK_EQ(pg106_br_va_alloc(dev, 0x10000, 0x10000, &vva), 0);
		{
			struct pg106_uapi_vm_bind_op op = { PG106_UAPI_VM_BIND_OP_MAP, 0, pbo, 0, pva, 0, 0x10000 };
			struct pg106_uapi_vm_bind_op op2 = { PG106_UAPI_VM_BIND_OP_MAP, 0, vbo, 0, vva, 0, 0x10000 };

			CHECK_EQ(pg106_br_ctx_bind(ctx, &op), 0);
			CHECK_EQ(pg106_br_ctx_bind(ctx, &op2), 0);
			CHECK_EQ(pg106_br_ctx_flush(ctx), 0);
		}
		// The VRAM BO's PTEs point at VRAM (the mock walks them).
		{
			struct pg106_pte_query q;

			CHECK(pg106_mmu_walk(pg106_kd_mmu(s.kd), vva, &q));
			CHECK_EQ(q.aperture, 0); // NV_MMU_VER2_PTE_APERTURE_VIDEO_MEMORY
			vbo_pa = q.pa;
		}
		// The QMD: one CTA of one thread, program at PROG_OFF, RELEASE0
		// of `payload` to the semaphore word.
		memset(&qp, 0, sizeof(qp));
		qp.program_offset = (uint32_t)PROG_OFF;
		qp.raster_width = qp.raster_height = qp.raster_depth = 1;
		qp.thread_dim[0] = qp.thread_dim[1] = qp.thread_dim[2] = 1;
		qp.register_count = 8;
		qp.crs_size = 0; // no SLM on the channel (gr_ctx.c st_dispatch)
		qp.release0_addr = vva + SEM_OFF;
		qp.release0_payload = payload;
		CHECK_EQ(pg106_qmd_build(&qp, &qmd), 0);
		CHECK_EQ(pg106_vram_write(&g->vram, vbo_pa + QMD_OFF, qmd.w, sizeof(qmd.w)), 0);
		CHECK_EQ(pg106_vram_wr64(&g->vram, vbo_pa + PROG_OFF, 0xe30000000007000full), 0); // EXIT
		// The push: SET_OBJECT, NVK's compute state, SEND_PCAS.
		pg106_pb_init(&pb, pmap, 256);
		memset(&st, 0, sizeof(st));
		st.program_region = vva;
		st.local_memory_window = PG106_SUB_NVK_LOCAL_MEMORY_WINDOW;
		st.shared_memory_window = PG106_SUB_NVK_SHARED_MEMORY_WINDOW;
		CHECK_EQ(pg106_sub_compute_init(&pb, 1, &st), 0);
		{
			uint32_t a = (uint32_t)((vva + QMD_OFF) >> 8), b = 3;

			pg106_pb_inc(&pb, 1, PG106_NVC1C0_SEND_PCAS_A, &a, 1);
			pg106_pb_inc(&pb, 1, 0x02bc, &b, 1); // SEND_SIGNALING_PCAS_B
		}
		CHECK_EQ(pb.error, 0);
		CHECK_EQ(pg106_br_syncobj_create(dev, 0, &so), 0);
		{
			struct pg106_uapi_exec_push p = { pva, (uint32_t)(pb.len * 4), 0 };
			struct pg106_uapi_sync sig = { PG106_UAPI_SYNC_SYNCOBJ, so, 0 };

			CHECK_EQ(pg106_br_ctx_exec(ctx, &p), 0);
			CHECK_EQ(pg106_br_ctx_exec_signal(ctx, &sig), 0);
			CHECK_EQ(pg106_br_ctx_flush(ctx), 0);
		}
		CHECK_EQ(s.kicks, 1);
		CHECK_EQ(pg106_br_syncobj_wait(dev, so, 0, 1000000000ull), 0);
		if (g_m.nerr)
			printf("mock: %s\n", g_m.err[0]);
		CHECK_EQ(g_m.nerr, 0);
		// The context was loaded on the kick with this channel's mmio
		// list, the QMD ran and released the semaphore in VRAM, the
		// kernel fence followed.
		CHECK_EQ(g_m.gr.loads, 1);
		CHECK_EQ(g_m.gr.load_errors, 0);
		CHECK_EQ(g_m.gr.last_loaded_chid, s.chan ? s.chan->chid : 999);
		CHECK_EQ(g_m.gr.mmio_applied, s.gc.mmio_nr / 2);
		CHECK_EQ(g_m.gr.last_pagepool, s.gc.pagepool_va);
		CHECK_EQ(g_m.gr.last_bundle, s.gc.bundle_va);
		CHECK_EQ(g_m.gr.last_attrib, s.gc.attrib_va);
		CHECK_EQ(g_m.gr.dispatches, 1);
		CHECK_EQ(g_m.gr.releases, 1);
		CHECK_EQ(g_m.gr.last_qmd, vva + QMD_OFF);
		CHECK_EQ(pg106_vram_rd32(&g->vram, vbo_pa + SEM_OFF), payload);
		CHECK_EQ(g_m.sem_releases, 1); // the kernel fence
		CHECK_EQ(pg106_kd_poll(s.kd), 1);
		printf("PASS e2e: VM_INIT bound a GR channel (chid %u, ctx image 0x%x at "
		       "VA 0x%llx, mmio list %u pairs), BO in VRAM at 0x%llx, EXEC -> "
		       "context load -> compute dispatch -> RELEASE0 0x%08x -> fence\n",
		       s.chan ? s.chan->chid : 0, gr.size,
		       (unsigned long long)s.gc.ctx_va, s.gc.mmio_nr / 2,
		       (unsigned long long)vbo_pa, payload);

		// A second dispatch on the same channel: no reload.
		{
			struct pg106_uapi_exec_push p = { pva, (uint32_t)(pb.len * 4), 0 };
			struct pg106_uapi_sync sig = { PG106_UAPI_SYNC_SYNCOBJ, so, 0 };

			CHECK_EQ(pg106_vram_wr32(&g->vram, vbo_pa + SEM_OFF, 0), 0);
			CHECK_EQ(pg106_br_ctx_exec(ctx, &p), 0);
			CHECK_EQ(pg106_br_ctx_exec_signal(ctx, &sig), 0);
			CHECK_EQ(pg106_br_ctx_flush(ctx), 0);
			CHECK_EQ(pg106_br_syncobj_wait(dev, so, 0, 1000000000ull), 0);
			CHECK_EQ(g_m.gr.loads, 1);
			CHECK_EQ(g_m.gr.dispatches, 2);
			CHECK_EQ(pg106_vram_rd32(&g->vram, vbo_pa + SEM_OFF), payload);
		}

		// Negative: a GR channel without a context faults at its first
		// kick (what the unported kext would have done).
		{
			struct pg106_chan_desc d;
			unsigned nerr = g_m.nerr;

			memset(&d, 0, sizeof(d));
			d.pdb_addr = pg106_mmu_root_addr(pg106_kd_mmu(s.kd));
			d.pdb_target = PG106_TARGET_VRAM;
			d.vm_limit = PG106_MMU_VA_LIMIT;
			d.gpfifo_va = pva;
			d.entries = 8;
			CHECK_EQ(pg106_chan_create_desc(&g->fifo, s.runl_gr, &d, &s.bare), 0);
			if (s.bare) {
				CHECK_EQ(mg_vram_rd64(&g_m, s.bare->inst + 0x210), 0);
				pg106_chan_kick(s.bare, 0);
				CHECK_EQ(g_m.nerr, nerr + 1);
				CHECK_EQ(g_m.gr.load_errors, 1);
				pg106_chan_destroy(s.bare);
				g_m.nerr = nerr;
			}
		}

		// A second render client at the same time (the kext's claimChannel
		// slots, one GR channel per user-client connection): its own kdisp
		// VM, page tables, channel and context image on the same GR runlist.
		// Both channels are in the runlist, the second context loads on its
		// own first kick, and closing it leaves the first channel running.
		{
			struct sys s2;
			struct pg106_kd_env e2 = e;
			struct pg106_br_loopback lb2;
			struct pg106_br_loopback_server srv2 = { serve, lb_map, NULL, &s2 };
			struct pg106_br_transport tr2;
			struct pg106_br_dev *dev2 = NULL;
			struct pg106_br_ctx *ctx2 = NULL;
			const struct mg_runl *rl = &g_m.runl[g->fifo.runl[s.runl_gr].id];
			uint32_t pbo2 = 0, so2 = 0, *pmap2 = NULL;
			uint64_t pva2 = 0;
			pg106_pb pb2;
			pg106_sub_compute_state st2;
			const unsigned loads0 = g_m.gr.loads, lerr0 = g_m.gr.load_errors, mmio0 = g_m.gr.mmio_applied;

			memset(&s2, 0, sizeof(s2));
			s2.g = g;
			s2.gr = &gr;
			s2.runl_gr = s.runl_gr;
			s2.tables_size = s.tables_size;
			CHECK_EQ(pg106_vram_alloc(&g->vram, s2.tables_size, 0x1000, PG106_VRAM_TAIL, &s2.tables), 0);
			e2.user = &s2;
			e2.mmu_cfg.table_base = s2.tables;
			e2.mmu_cfg.table_limit = s2.tables + s2.tables_size;
			CHECK_EQ(pg106_kd_create(&e2, &s2.kd), 0);
			CHECK_EQ(pg106_br_loopback_open(&lb2, &srv2, &tr2), 0);
			CHECK_EQ(pg106_br_dev_open(&tr2, &dev2), 0);
			if (dev2) {
				CHECK_EQ(pg106_br_ctx_create(dev2, 0, &ctx2), 0);
				CHECK_EQ(pg106_br_vm_init(dev2, KERN_VA, (1ull << 40) - KERN_VA), 0);
			}
			CHECK(s2.chan != NULL);
			CHECK(s2.gc.up);
			if (dev2 && ctx2 && s2.chan && s.chan) {
				CHECK(s2.chan->chid != s.chan->chid);
				CHECK(s2.gc.ctx_pa != s.gc.ctx_pa);
				CHECK(s2.gc.mmio_pa != s.gc.mmio_pa);
				CHECK_EQ(s2.gc.pagepool_va, s.gc.pagepool_va); // shared buffers, same place in each VM
				CHECK_EQ(rl->chan_entries, 2);
				CHECK_EQ(rl->tsg_entries, 2);
				CHECK_EQ(pg106_br_bo_alloc(dev2, 0x10000, 0x1000,
							   PG106_BR_BO_GART | PG106_BR_BO_CAN_MAP, &pbo2), 0);
				CHECK_EQ(pg106_br_bo_map(dev2, pbo2, 0, (void **)&pmap2), 0);
				CHECK_EQ(pg106_br_va_alloc(dev2, 0x10000, 0x10000, &pva2), 0);
				{
					struct pg106_uapi_vm_bind_op op = { PG106_UAPI_VM_BIND_OP_MAP, 0, pbo2, 0, pva2, 0, 0x10000 };

					CHECK_EQ(pg106_br_ctx_bind(ctx2, &op), 0);
					CHECK_EQ(pg106_br_ctx_flush(ctx2), 0);
				}
				if (pmap2) {
					pg106_pb_init(&pb2, pmap2, 256);
					memset(&st2, 0, sizeof(st2));
					st2.program_region = pva2;
					st2.local_memory_window = PG106_SUB_NVK_LOCAL_MEMORY_WINDOW;
					st2.shared_memory_window = PG106_SUB_NVK_SHARED_MEMORY_WINDOW;
					CHECK_EQ(pg106_sub_compute_init(&pb2, 1, &st2), 0);
					CHECK_EQ(pb2.error, 0);
					CHECK_EQ(pg106_br_syncobj_create(dev2, 0, &so2), 0);
					{
						struct pg106_uapi_exec_push p = { pva2, (uint32_t)(pb2.len * 4), 0 };
						struct pg106_uapi_sync sig = { PG106_UAPI_SYNC_SYNCOBJ, so2, 0 };

						CHECK_EQ(pg106_br_ctx_exec(ctx2, &p), 0);
						CHECK_EQ(pg106_br_ctx_exec_signal(ctx2, &sig), 0);
						CHECK_EQ(pg106_br_ctx_flush(ctx2), 0);
					}
					CHECK_EQ(s2.kicks, 1);
					CHECK_EQ(pg106_br_syncobj_wait(dev2, so2, 0, 1000000000ull), 0);
					if (g_m.nerr)
						printf("mock: %s\n", g_m.err[0]);
					CHECK_EQ(g_m.nerr, 0);
					CHECK_EQ(g_m.gr.loads, loads0 + 1); // the second channel's own context
					CHECK_EQ(g_m.gr.load_errors, lerr0);
					CHECK_EQ(g_m.gr.last_loaded_chid, s2.chan->chid);
					CHECK_EQ(g_m.gr.mmio_applied, mmio0 + s2.gc.mmio_nr / 2);
				}
				printf("PASS two render clients: chids %u and %u on GR runlist %d, "
				       "context images at 0x%llx and 0x%llx, second context loaded on "
				       "its first kick\n", s.chan->chid, s2.chan->chid,
				       g->fifo.runl[s.runl_gr].id, (unsigned long long)s.gc.ctx_pa,
				       (unsigned long long)s2.gc.ctx_pa);
			}
			if (ctx2)
				pg106_br_ctx_destroy(ctx2);
			if (dev2)
				pg106_br_dev_close(dev2);
			pg106_kd_destroy(s2.kd);
			CHECK(s2.chan == NULL);
			CHECK(!s2.gc.up);
			CHECK_EQ(s2.allocs, s2.frees);
			CHECK_EQ(rl->chan_entries, 1); // the first client's channel stays scheduled
			CHECK(s.chan != NULL && s.gc.up);
			pg106_br_loopback_close(&lb2);
			pg106_vram_free(&g->vram, s2.tables, s2.tables_size);
		}

		// The first client still runs after the second left.
		{
			struct pg106_uapi_exec_push p = { pva, (uint32_t)(pb.len * 4), 0 };
			struct pg106_uapi_sync sig = { PG106_UAPI_SYNC_SYNCOBJ, so, 0 };
			const unsigned d0 = g_m.gr.dispatches;

			CHECK_EQ(pg106_vram_wr32(&g->vram, vbo_pa + SEM_OFF, 0), 0);
			CHECK_EQ(pg106_br_ctx_exec(ctx, &p), 0);
			CHECK_EQ(pg106_br_ctx_exec_signal(ctx, &sig), 0);
			CHECK_EQ(pg106_br_ctx_flush(ctx), 0);
			CHECK_EQ(pg106_br_syncobj_wait(dev, so, 0, 1000000000ull), 0);
			CHECK_EQ(g_m.gr.dispatches, d0 + 1);
			CHECK_EQ(pg106_vram_rd32(&g->vram, vbo_pa + SEM_OFF), payload);
			CHECK_EQ(g_m.nerr, 0);
		}

		// Teardown frees everything: BOs, the context objects, the channel.
		CHECK_EQ(pg106_br_bo_free(dev, pbo), -EBUSY); // still bound
		pg106_br_ctx_destroy(ctx);
		pg106_br_dev_close(dev);
		{
			uint64_t before = g->vram.mm.free_bytes;

			pg106_kd_destroy(s.kd);
			CHECK(s.chan == NULL);
			CHECK(!s.gc.up);
			CHECK_EQ(s.allocs, s.frees);
			CHECK(g->vram.mm.free_bytes > before);
		}
		pg106_br_loopback_close(&lb);
	}
	pg106_vram_free(&g->vram, s.tables, s.tables_size);
	{
		uint64_t before = g->vram.mm.free_bytes;

		pg106_gr_fini(&gr);
		CHECK(g->vram.mm.free_bytes > before);
	}

	// 4. The hardware harness's GR self-test on the mock: its own golden
	// generation (a second one: the mock FECS resets its counts), a
	// channel with a context, the EXIT kernel's RELEASE0 and the host
	// semaphore. The mock cannot run SASS, so the store kernel's result
	// stays 0 and the test reports it (what the harness prints).
	{
		struct pg106_gr_selftest gs;
		uint64_t before = g->vram.mm.free_bytes;
		int rc;

		g_m.nerr = 0;
		rc = pg106_gpu_gr_selftest(g, &gfw, 100000, &gs);
		CHECK_EQ(rc, -EIO);
		CHECK(gs.failed_at && !strncmp(gs.failed_at, "store kernel result", 19));
		CHECK_EQ(gs.golden_size, CTX_SIZE);
		CHECK(gs.golden_failed_at == NULL);
		CHECK_EQ(gs.runlist, 0);
		CHECK_EQ(gs.sem_value, PG106_GR_SELFTEST_SEM);
		CHECK_EQ(gs.host_sem, PG106_GR_SELFTEST_HOST);
		CHECK_EQ(gs.store_value, 0);
		CHECK_EQ(gs.mmio_pairs, 61);
		CHECK_EQ(gs.gp_get, 3); // class bind, EXIT dispatch, store
		CHECK_EQ(gs.step, 3);
		CHECK_EQ(gs.mmu_faults, 0);
		// cumulative: 3 dispatches on the first render client, 2 here; loads:
		// the two render clients' contexts and the self-test's channel
		CHECK_EQ(g_m.gr.dispatches, 5);
		CHECK_EQ(g_m.gr.releases, 5);
		CHECK_EQ(g_m.gr.loads, 3);
		CHECK_EQ(g_m.gr.fecs_resets, 2);
		CHECK_EQ(g_m.nerr, 0);
		CHECK_EQ(g->vram.mm.free_bytes, before); // everything freed
		printf("PASS gr selftest on the mock: golden regenerated, chid %u, QMD "
		       "release 0x%08x, host sem 0x%08x, store kernel reported\n",
		       gs.chid, gs.sem_value, gs.host_sem);
	}
	pg106_gpu_fini(g);
	CHECK_EQ(g_m.nerr, 0);
	mg_free(&g_m);
	free(g);
	free(nonctx);
	free(swctx);
	free(bundle);
	free(method);
	printf("%s: %d checks, %d failures\n", g_fail ? "FAILED" : "PASSED",
	       g_checks, g_fail);
	return g_fail ? 1 : 0;
}
