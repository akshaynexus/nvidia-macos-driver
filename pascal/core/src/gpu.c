// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/src/gpu.c — bring-up + memory + FIFO as one object, and the
// GPU-work self-test. See gpu.h.

#include <pascal/ce.h>
#include <pascal/gpu.h>
#include <pascal/pushbuf.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static void
fifo_intr(void *ctx, uint32_t pending)
{
	struct pg106_gpu *g = ctx;
	bool ns = false;

	(void)pending;
	pg106_fifo_intr(&g->fifo, &ns);
	if (ns) {
		g->nonstall_events++;
		if (g->nonstall_cb)
			g->nonstall_cb(g->nonstall_ctx);
	}
}

void
pg106_gpu_set_nonstall_cb(struct pg106_gpu *g, void (*cb)(void *), void *ctx)
{
	g->nonstall_cb = cb;
	g->nonstall_ctx = ctx;
}

static void
fill_db_cfg(const struct pg106_gpu_cfg *cfg, struct pg106_devboot_cfg *c)
{
	memset(c, 0, sizeof(*c));
	c->bus = cfg->bus;
	c->vbios = cfg->vbios;
	c->vbios_size = cfg->vbios_size;
	c->fw = cfg->fw;
	c->event = cfg->event;
	c->event_user = cfg->event_user;
	c->sec2_max_polls = cfg->sec2_max_polls;
	c->bootstrap = cfg->bootstrap;
	c->bootstrap_user = cfg->bootstrap_user;
}

int
pg106_gpu_boot(struct pg106_gpu *g, const struct pg106_gpu_cfg *cfg)
{
	struct pg106_devboot_cfg c;
	uint8_t stop;
	int ret;

	if (!g || !cfg || !cfg->bus || !cfg->bar1_size || !cfg->bar1_io.wr32 ||
	    !cfg->bar1_io.rd32)
		return -EINVAL;
	memset(g, 0, sizeof(*g));
	g->cfg = *cfg;
	stop = cfg->stop_after > PG106_DB_READY ? PG106_DB_READY : cfg->stop_after;

	// 1. identify, posted, POST.
	fill_db_cfg(cfg, &c);
	c.stop_after = stop < PG106_DB_POST ? stop : PG106_DB_POST;
	ret = pg106_devboot_run(&g->db, &c);
	g->db_valid = true;
	g->stage = g->db.stage;
	if (ret || stop <= PG106_DB_POST)
		return g->ret = ret;
	// 1b. nvkm_mc_init -> nv50_mc_init (mc/nv50.c:41-45; gp100_mc .init,
	// mc/gp100.c): wr32(0x200, 0xffffffff), "everything on". Nouveau does
	// it on every init, after preinit/POST and the subdev resets and
	// before FB/GR (owner's Helios trace posttrace2 L129391; PMC_ENABLE
	// then reads 0x5c6cf1e1). Without it PMC_ENABLE stayed 0x4064f1e1
	// (bits 19 and 26-28 off) and GR's GPC registers answered 0xbadf1200
	// to discovery while FECS (0x409604) answered (hardware run 4).
	{
		uint32_t before = cfg->bus->rd32(cfg->bus->user, PG106_PMC_ENABLE);
		uint32_t after;

		cfg->bus->wr32(cfg->bus->user, PG106_PMC_ENABLE, 0xffffffffu);
		after = cfg->bus->rd32(cfg->bus->user, PG106_PMC_ENABLE);
		if (cfg->event) {
			struct pg106_devboot_event e = {
				PG106_DB_POST, PG106_DB_EV_INFO, 0,
				"mc init (nv50_mc_init: 0x200 = 0xffffffff): a = "
				"PMC_ENABLE before, b = after", before, after };

			cfg->event(cfg->event_user, &e);
		}
	}
	// 2. VRAM.
	ret = pg106_fb_vram_size(cfg->bus, &g->vram_size);
	if (!ret)
		ret = pg106_vram_init(&g->vram, cfg->bus, g->vram_size);
	if (ret)
		return g->ret = ret;
	g->vram_up = true;
	// 3. nouveau's subdev order: FB init (fb oneinit + init), then the
	// BARs (instmem/bar oneinit: BAR2 first, gf100_bar_oneinit), all
	// before the ACR. The top-down VRAM layout then follows nouveau's:
	// mmu_rd/mmu_wr, BAR2 inst, BAR2 page tables, BAR1 inst, WPR.
	ret = pg106_fb_init(&g->fb, &g->vram, cfg->sysmem_flush_bus);
	if (!ret && cfg->bar2_size) {
		ret = pg106_bar_init(&g->bar2, &g->vram, PG106_BAR2, cfg->bar2_size);
		g->bar2_up = !ret;
	}
	if (!ret) {
		ret = pg106_bar_init(&g->bar1, &g->vram, PG106_BAR1, cfg->bar1_size);
		g->bar1_up = !ret;
	}
	if (ret)
		return g->ret = ret;
	// 4. the whole sequence with the ACR memory in VRAM (POST is not
	// repeated: devboot.h posted_this_power_cycle).
	c.posted_this_power_cycle = g->db.posted;
	pg106_devboot_fini(&g->db);
	c.stop_after = stop;
	c.vram = &g->vram;
	c.hostmem = cfg->hostmem;
	c.acr_vma = cfg->acr_vma;
	c.sysmem_flush_bus = cfg->sysmem_flush_bus;
	c.fb_ready = true;
	ret = pg106_devboot_run(&g->db, &c);
	g->stage = g->db.stage;
	g->ret = ret;
	// 5. FIFO + interrupts whenever PTOP was read.
	if (g->db.stage < PG106_DB_TOP || (ret && g->db.stage == PG106_DB_TOP))
		return ret;
	{
		int r2 = 0;

		if (!r2) {
			r2 = pg106_fifo_init(&g->fifo, &g->vram, &g->bar1,
					     &cfg->bar1_io, &g->db.top);
			g->fifo_up = !r2;
		}
		if (!r2) {
			if (cfg->intr) {
				g->intr = cfg->intr;
			} else {
				g->intr = &g->intr_own;
				pg106_intr_init(g->intr, cfg->bus);
			}
			g->intr_up = true;
			pg106_intr_set(g->intr, PG106_PMC_INTR_FIFO, NULL, NULL);
			r2 = pg106_intr_set(g->intr, PG106_PMC_INTR_FIFO, fifo_intr, g);
			pg106_fifo_nonstall(&g->fifo, true);
		}
		if (!ret)
			g->ret = ret = r2;
	}
	return ret;
}

bool
pg106_gpu_ready(const struct pg106_gpu *g)
{
	return g && g->db_valid && g->ret == 0 && g->db.stage == PG106_DB_READY &&
	       g->fifo_up;
}

void
pg106_gpu_fini(struct pg106_gpu *g)
{
	if (!g)
		return;
	if (g->intr_up) {
		if (g->intr == &g->intr_own)
			pg106_intr_init(g->intr, g->cfg.bus); // block everything
		else
			pg106_intr_set(g->intr, PG106_PMC_INTR_FIFO, NULL, NULL);
	}
	if (g->fifo_up)
		pg106_fifo_fini(&g->fifo);
	if (g->bar1_up)
		pg106_bar_fini(&g->bar1);
	if (g->bar2_up)
		pg106_bar_fini(&g->bar2);
	if (g->db_valid)
		pg106_devboot_fini(&g->db);
	if (g->vram_up)
		pg106_vram_fini(&g->vram);
	memset(g, 0, sizeof(*g));
}

// ---- self-test ------------------------------------------------------------------------

#define ST_VA	   0x200000000ull
#define ST_SIZE	   0x100000ull
#define ST_RING	   0x00000u // 512 entries
#define ST_PUSH	   0x10000u
#define ST_SEM	   0x20000u
#define ST_SRC	   0x40000u
#define ST_DST	   0x80000u
#define ST_COPY	   0x10000u
#define ST_MAGIC_1 0x5a5a0001u
#define ST_MAGIC_2 0x5a5a0002u

static int
poll_vram(struct pg106_gpu *g, uint64_t pa, uint32_t want, uint32_t budget,
	  uint32_t *got, uint64_t *waited)
{
	const struct pg106_hw_bus *b = g->cfg.bus;
	uint64_t t = 0;

	for (;;) {
		*got = pg106_vram_rd32(&g->vram, pa);
		if (*got == want) {
			*waited = t;
			return 0;
		}
		if (t >= budget) {
			*waited = t;
			return -ETIMEDOUT;
		}
		b->delay_us(b->user, 10);
		t += 10;
	}
}

static uint32_t
st_submit(struct pg106_gpu *g, struct pg106_chan *c, uint64_t buf, uint32_t put,
	  uint32_t push_off, const uint32_t *w, size_t n)
{
	uint64_t e = pg106_gpfifo_pack(ST_VA + push_off, (uint32_t)n,
				       PG106_GP_LEVEL_MAIN, 0, 0);

	pg106_vram_write(&g->vram, buf + push_off, w, (uint32_t)(n * 4));
	pg106_vram_wr64(&g->vram, buf + ST_RING + put * 8ull, e);
	// VRAM writes through PRAMIN are posted by the time the BAR1 kick
	// lands (same BAR0 path, in order); nouveau orders them with
	// nvkm_done + a BAR flush.
	pg106_bar_flush(g->cfg.bus);
	put = (put + 1) & (c->entries - 1);
	pg106_chan_kick(c, put);
	return put;
}

int
pg106_gpu_selftest(struct pg106_gpu *g, uint32_t timeout_us,
		   struct pg106_gpu_selftest *res)
{
	struct pg106_vmm vmm;
	struct pg106_chan *c = NULL;
	struct pg106_map m;
	uint64_t buf = 0, ns0;
	uint32_t put = 0, w[64], i;
	pg106_pb pb;
	int k, ret;
	bool vmm_up = false;

	if (!g || !res)
		return -EINVAL;
	memset(res, 0, sizeof(*res));
	ns0 = g->nonstall_events;
	res->runlist = -1;
	if (!g->fifo_up) {
		res->failed_at = "FIFO not up";
		return res->ret = -ENODEV;
	}
	k = pg106_fifo_runl_find(&g->fifo, PG106_TOP_TYPE_CE_N);
	if (k < 0)
		k = pg106_fifo_runl_find(&g->fifo, PG106_TOP_TYPE_CE2);
	if (k < 0) {
		res->failed_at = "no copy-engine runlist in PTOP";
		return res->ret = -ENOENT;
	}
	res->runlist = g->fifo.runl[k].id;
	ret = pg106_vram_alloc(&g->vram, ST_SIZE, 0x10000, PG106_VRAM_ZERO, &buf);
	if (ret) {
		res->failed_at = "VRAM for the buffers";
		goto out;
	}
	ret = pg106_vmm_init(&vmm, &g->vram, PG106_MMU_VA_LIMIT, 0x10000, false);
	if (ret) {
		res->failed_at = "channel VMM";
		goto out;
	}
	vmm_up = true;
	memset(&m, 0, sizeof(m));
	m.va = ST_VA;
	m.pa = buf;
	m.size = ST_SIZE;
	m.page_shift = PG106_PAGE_64K_SHIFT;
	m.target = PG106_TARGET_VRAM;
	m.kind = PG106_KIND_PITCH;
	ret = pg106_vmm_map(&vmm, &m);
	if (!ret)
		ret = pg106_vmm_commit(&vmm);
	if (ret) {
		res->failed_at = "channel VMM map/upload/flush";
		goto out;
	}
	ret = pg106_chan_create(&g->fifo, &vmm, k, ST_VA + ST_RING, 512, &c);
	if (ret) {
		res->failed_at = "channel create (runlist commit / bind)";
		goto out;
	}
	res->chid = c->chid;
	res->sem_pa = buf + ST_SEM;

	// 1. Host semaphore release, 4 bytes, WFI (clc06f.h SEMAPHORED:
	// OPERATION_RELEASE 2, RELEASE_SIZE_4BYTE bit 24), then
	// NON_STALL_INTERRUPT.
	pg106_pb_init(&pb, w, 64);
	{
		const uint64_t sva = ST_VA + ST_SEM;
		uint32_t d[5] = { (uint32_t)(sva >> 32) & 0xffu, (uint32_t)sva,
				  ST_MAGIC_1, 0x01000002u, 0 };

		pg106_pb_inc(&pb, 0, 0x0010, d, 5);
	}
	put = st_submit(g, c, buf, put, ST_PUSH, w, pb.len);
	ret = poll_vram(g, buf + ST_SEM, ST_MAGIC_1, timeout_us, &res->sem_value,
			&res->sem_wait_us);
	if (ret) {
		res->failed_at = "host semaphore release never landed";
		goto out;
	}

	// 2. CE copy + CE semaphore.
	for (i = 0; i < ST_COPY; i += 4)
		pg106_vram_wr32(&g->vram, buf + ST_SRC + i, 0xc0de0000u ^ i);
	{
		pg106_ce_semaphore sem = { ST_VA + ST_SEM + 0x10, ST_MAGIC_2, 0 };
		pg106_ce_memcpy_plan pl;

		memset(&pl, 0, sizeof(pl));
		pl.src_addr = ST_VA + ST_SRC;
		pl.dst_addr = ST_VA + ST_DST;
		pl.size = ST_COPY;
		pl.subch = 4; // nouveau puts its copy class on subchannel 4
		pl.transfer_type = PG106_CE_XFER_NON_PIPELINED;
		pl.flush_enable = PG106_NVC1B5_LAUNCH_DMA_FLUSH_ENABLE_TRUE;
		pl.sem = &sem;
		pg106_pb_init(&pb, w, 64);
		if (pg106_ce_memcpy(&pb, &pl) || pb.error) {
			ret = -EINVAL;
			res->failed_at = "CE push encode";
			goto out;
		}
	}
	put = st_submit(g, c, buf, put, ST_PUSH + 0x1000, w, pb.len);
	ret = poll_vram(g, buf + ST_SEM + 0x10, ST_MAGIC_2, timeout_us,
			&res->ce_sem_value, &res->ce_wait_us);
	if (ret) {
		res->failed_at = "CE semaphore never landed";
		goto out;
	}
	res->ce_bytes = ST_COPY;
	for (i = 0; i < ST_COPY; i += 4)
		if (pg106_vram_rd32(&g->vram, buf + ST_DST + i) != (0xc0de0000u ^ i))
			res->ce_mismatch++;
	if (res->ce_mismatch) {
		ret = -EIO;
		res->failed_at = "CE copy contents differ";
	}
out:
	if (c)
		res->gp_get = pg106_chan_get(c);
	res->pfifo_intr = g->cfg.bus->rd32(g->cfg.bus->user, 0x002100);
	if (g->intr_up && pg106_intr_pending(g->intr))
		pg106_intr_service(g->intr);
	res->pfifo_intr_after = g->cfg.bus->rd32(g->cfg.bus->user, 0x002100);
	res->nonstall_events = g->nonstall_events;
	// Our NON_STALL_INTERRUPT raises PFIFO_INTR bit 31 (gk104_fifo_intr
	// 0x80000000, the engine non-stall line); the service acknowledges it
	// and counts one event. Anything left pending afterwards is an error
	// of its own (an unacknowledged or foreign interrupt).
	if (!ret && (res->pfifo_intr_after || res->nonstall_events != ns0 + 1)) {
		ret = -EIO;
		res->failed_at = res->pfifo_intr_after
			? "PFIFO_INTR still pending after the service"
			: "NON_STALL_INTERRUPT did not raise exactly one non-stall event";
	}
	if (c)
		pg106_chan_destroy(c);
	if (vmm_up)
		pg106_vmm_fini(&vmm);
	if (buf)
		pg106_vram_free(&g->vram, buf, ST_SIZE);
	return res->ret = ret;
}
