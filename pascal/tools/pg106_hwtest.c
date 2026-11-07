// SPDX-License-Identifier: GPL-2.0-only
// pascal/tools/pg106_hwtest.c — Linux userspace hardware harness: runs the
// portable bring-up (pascal/core gpu.c: devboot with the ACR memory in
// VRAM, BAR2/BAR1, FIFO) on the real GP106 through
// /sys/bus/pci/devices/<bdf>/resource{0,1,3} mmaps, with no kernel driver
// bound, and ends with GPU work: a host semaphore release and a
// copy-engine copy (pg106_gpu_selftest).
//
//   pg106_hwtest --probe [--dev 0000:01:00.0]          read-only
//   pg106_hwtest --go --fw <research/firmware> [--vbios rom] [--stop-after N]
//                [--gr]   after READY: the GR golden context, a channel on
//                         GR's runlist and two compute dispatches
//                         (pg106_gpu_gr_selftest, core gr_ctx.h)
//                [--render] after READY: the kext's render path, bridge ->
//                         kdisp -> GPU (VM_INIT binds a GR channel, BOs,
//                         VM_BIND, two EXECs with kernel fences; every
//                         object in VRAM behind BAR1)
//
// Safety rules it enforces before touching anything:
//   - the function must be 10de:1c20 (GTX 1060 Mobile; --any-gp106 widens
//     it to the kext's gate list);
//   - no driver may be bound (<dev>/driver must not exist): nvidia or
//     nouveau would be fighting it;
//   - --probe only reads registers (BOOT_0, 0x2240c, PTOP, the FBP walk,
//     BAR binds, PMC_ENABLE, SEC2 HWCFG);
//   - every write needs --go; it may enable the function's memory decode
//     (<dev>/enable = 1) and nothing else in PCI config space.
// The HS ACR image is placed in VRAM (no host DMA is needed, so VT-d and
// bus mastering do not matter); the copy runs VRAM to VRAM.
//
// Recovery if the GPU wedges: stop the display manager first (the dGPU is
// not driving a panel on the Helios), unbind nvidia before the run, and a
// reboot restores the GPU (the ACR WPR lock and the FIFO state do not
// survive a cold boot). NOT run on the laptop without the coordinator's go.
//
// Status (HARDWARE, 2026-11-08, Helios, owner-approved): hw-run6 at
// bbdbca6 passed --stop-after 2 (POST), --stop-after 3 (FIFO + CE
// selftest) and the full bring-up to READY + selftest, each from a GPU
// woken out of D3cold. Runs 1-5 and their fixes:
// research/hardware/helios300/hw-runs/README.md. Stage and run with
// scripts/stage-hw-run.sh + scripts/hw-run.sh.

#define _GNU_SOURCE
#include <pascal/devboot.h>
#include <pascal/dispatch.h>
#include <pascal/gpu.h>
#include <pascal/gr_ctx.h>
#include <pascal/kdisp.h>
#include <pascal/pmc.h>
#include <pascal/pushbuf.h>
#include <pascal/qmd.h>
#include <pascal/top.h>
#include <pascal/uapi.h>
#include "pg106_br_bridge.h"
#include "pg106_br_loopback.h"
#include "pg106_br_wire.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct bar {
	volatile uint8_t *p;
	uint64_t len;
};

struct hw {
	struct bar bar0, bar1, bar3;
	uint64_t writes, reads;
	bool go;
};

static struct hw g_hw;

static uint32_t
h_rd32(void *u, uint32_t a)
{
	struct hw *h = u;

	if ((uint64_t)a + 4 > h->bar0.len || (a & 3))
		return 0xffffffffu;
	h->reads++;
	return *(volatile uint32_t *)(h->bar0.p + a);
}

static void
h_wr32(void *u, uint32_t a, uint32_t v)
{
	struct hw *h = u;

	if (!h->go) {
		fprintf(stderr, "BUG: write 0x%06x = 0x%08x without --go\n", a, v);
		abort();
	}
	if ((uint64_t)a + 4 > h->bar0.len || (a & 3))
		return;
	h->writes++;
	*(volatile uint32_t *)(h->bar0.p + a) = v;
	__sync_synchronize();
}

static void
h_delay(void *u, uint32_t us)
{
	struct timespec ts = { us / 1000000u, (long)(us % 1000000u) * 1000L };

	(void)u;
	if (us < 50) { // spin: nanosleep granularity is too coarse
		struct timespec a, b;

		clock_gettime(CLOCK_MONOTONIC, &a);
		do
			clock_gettime(CLOCK_MONOTONIC, &b);
		while ((b.tv_sec - a.tv_sec) * 1000000000L + (b.tv_nsec - a.tv_nsec) <
		       (long)us * 1000L);
		return;
	}
	nanosleep(&ts, NULL);
}

static void
b1_wr(void *u, uint64_t off, uint32_t v)
{
	struct hw *h = u;

	if (!h->go)
		abort();
	if (off + 4 > h->bar1.len)
		return;
	*(volatile uint32_t *)(h->bar1.p + off) = v;
	__sync_synchronize();
}

static uint32_t
b1_rd(void *u, uint64_t off)
{
	struct hw *h = u;

	if (off + 4 > h->bar1.len)
		return 0xffffffffu;
	return *(volatile uint32_t *)(h->bar1.p + off);
}

static int
read_file(const char *path, char *buf, size_t cap)
{
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return -errno;
	n = read(fd, buf, cap - 1);
	close(fd);
	if (n < 0)
		return -errno;
	buf[n] = 0;
	return (int)n;
}

static uint8_t *
slurp(const char *path, size_t *len)
{
	struct stat st;
	uint8_t *b;
	int fd = open(path, O_RDONLY);

	if (fd < 0)
		return NULL;
	if (fstat(fd, &st) || st.st_size <= 0 || !(b = malloc((size_t)st.st_size))) {
		close(fd);
		return NULL;
	}
	if (read(fd, b, (size_t)st.st_size) != st.st_size) {
		free(b);
		close(fd);
		return NULL;
	}
	close(fd);
	*len = (size_t)st.st_size;
	return b;
}

static int
map_bar(const char *dev, int idx, bool rw, struct bar *b)
{
	char path[512];
	struct stat st;
	int fd;
	void *p;

	snprintf(path, sizeof(path), "%s/resource%d", dev, idx);
	fd = open(path, rw ? O_RDWR : O_RDONLY);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &st) || st.st_size <= 0) {
		close(fd);
		return -ENODEV;
	}
	p = mmap(NULL, (size_t)st.st_size, PROT_READ | (rw ? PROT_WRITE : 0),
		 MAP_SHARED, fd, 0);
	close(fd);
	if (p == MAP_FAILED)
		return -errno;
	b->p = p;
	b->len = (uint64_t)st.st_size;
	return 0;
}

static void
on_event(void *u, const struct pg106_devboot_event *e)
{
	(void)u;
	printf("boot %-14s %-5s ret=%d a=0x%llx b=0x%llx %s\n",
	       pg106_devboot_stage_name(e->stage), pg106_devboot_ev_name(e->ev),
	       e->ret, (unsigned long long)e->a, (unsigned long long)e->b,
	       e->detail ? e->detail : "");
	fflush(stdout);
}

static int
load_fw(const char *root, struct pg106_devboot_fw *fw, uint8_t **bufs)
{
	size_t i;
	int missing = 0;

	memset(fw, 0, sizeof(*fw));
	for (i = 0; i < pg106_devboot_fw_count; i++) {
		const struct pg106_fw_name *n = &pg106_devboot_fw_names[i];
		struct pg106_fw_blob *b =
			(struct pg106_fw_blob *)((uint8_t *)fw + n->offset);
		char path[1024];
		size_t len = 0;

		snprintf(path, sizeof(path), "%s/%s", root, n->path);
		bufs[i] = slurp(path, &len);
		if (!bufs[i]) {
			printf("fw: %s missing (stage %s will fail)\n", path,
			       pg106_devboot_stage_name(n->required_for));
			missing++;
			continue;
		}
		b->data = bufs[i];
		b->size = len;
	}
	return missing;
}

static int
top_rd(void *u, uint32_t a, uint32_t *v)
{
	*v = h_rd32(u, a);
	return 0;
}

static void
probe(struct hw *h)
{
	struct pg106_hw_bus bus = { h, h_rd32, h_wr32, h_delay, NULL, 0 };
	struct pg106_top_table t;
	uint32_t boot0 = h_rd32(h, 0);
	uint64_t vram = 0;
	size_t i;

	printf("probe (read-only): BOOT_0 0x%08x (%s)\n", boot0,
	       pg106_pmc_probe(boot0).is_gp106 ? "GP106" : "NOT a GP106");
	printf("posted: 0x2240c = 0x%08x (bit 1 %s)\n", h_rd32(h, 0x2240c),
	       (h_rd32(h, 0x2240c) & 2) ? "set" : "clear");
	printf("PMC_ENABLE 0x%08x  PMC_INTR_0 0x%08x\n", h_rd32(h, 0x200),
	       h_rd32(h, 0x100));
	if (pg106_fb_vram_size(&bus, &vram) == 0)
		printf("VRAM (FBP walk) %llu MiB; 0x100ce0 says %llu MiB\n",
		       (unsigned long long)(vram >> 20),
		       (unsigned long long)(pg106_fb_vidmem_size(&bus) >> 20));
	printf("BAR1 bind 0x1704 = 0x%08x, BAR2 bind 0x1714 = 0x%08x\n",
	       h_rd32(h, 0x1704), h_rd32(h, 0x1714));
	printf("SEC2 HWCFG 0x%08x, debug 0x%08x, CPUCTL 0x%08x, BINDINST 0x%08x\n",
	       h_rd32(h, 0x87108), h_rd32(h, 0x87408), h_rd32(h, 0x87100),
	       h_rd32(h, 0x87054));
	printf("FIFO: 0x2004 = 0x%x PBDMAs, 0x2008 = %u channels, 0x2254 = 0x%08x\n",
	       h_rd32(h, 0x2004) & 0xff, h_rd32(h, 0x2008), h_rd32(h, 0x2254));
	if (pg106_top_scan(top_rd, h, &t) == 0)
		for (i = 0; i < t.ndev; i++)
			printf("PTOP: type 0x%02x inst %d addr 0x%06x runlist %d "
			       "reset %d intr %d\n",
			       t.dev[i].type, t.dev[i].inst, t.dev[i].addr,
			       t.dev[i].runlist, t.dev[i].reset, t.dev[i].intr);
	printf("probe: %llu reads, %llu writes\n", (unsigned long long)h->reads,
	       (unsigned long long)h->writes);
}

// ---- --render: the kext's render path on the hardware ------------------------
//
// What PG106::claimChannel + PG106UserClient do for NVK, minus IOKit: the
// bridge (the NVK backend's client side) over the in-process loopback into
// the kernel dispatcher (pascal/core/kdisp.c, the kext's), whose
// environment is this harness: every object (kdisp's ring, fence page
// and fence pushes, the client's BOs, the VM tables) lives in VRAM and
// is reached by the CPU through a BAR1 window (what the kext does for
// CAN_MAP VRAM BOs), since there is no host DMA without a kernel
// driver. chan_bind is the kext's: the GR context from gr_ctx.c and a
// channel on GR's runlist. Then the NVK-shaped flow: VM_INIT, BO_ALLOC,
// VM_BIND, EXEC of a compute dispatch, a syncobj wait on the kernel
// fence, twice (EXIT kernel with a QMD release; the store kernel).

struct rmem {
	uint64_t pa, size, b1; // VRAM address, bytes (page-rounded), BAR1 offset
	void *cpu;             // CPU view through BAR1
};

struct rsys {
	struct pg106_gpu *g;
	struct pg106_gr *gr;
	struct pg106_kd *kd;
	int runl_gr;
	uint64_t tables, tables_size;
	struct pg106_grctx_chan gc;
	struct pg106_chan *chan;
	struct rmem *mapped[64];
	unsigned allocs, frees, kicks, flushes;
	int bind_rc;
};

static int
r_alloc(void *u, uint64_t size, uint64_t align, uint32_t flags, void **mem)
{
	struct rsys *s = u;
	struct rmem *h = calloc(1, sizeof(*h));
	int rc;

	(void)flags; // GART or VRAM: all VRAM here
	if (!h)
		return -ENOMEM;
	h->size = (size + 0xfff) & ~0xfffull;
	rc = pg106_vram_alloc(&s->g->vram, h->size, align < 0x1000 ? 0x1000 : align,
			      PG106_VRAM_ZERO, &h->pa);
	if (rc) {
		free(h);
		return rc;
	}
	rc = pg106_bar_map_ex(&s->g->bar1, h->pa, h->size, PG106_TARGET_VRAM, false, &h->b1);
	if (rc) {
		pg106_vram_free(&s->g->vram, h->pa, h->size);
		free(h);
		return rc;
	}
	if (h->b1 + h->size > g_hw.bar1.len) {
		pg106_bar_unmap(&s->g->bar1, h->b1, h->size);
		pg106_vram_free(&s->g->vram, h->pa, h->size);
		free(h);
		return -ERANGE;
	}
	h->cpu = (void *)(g_hw.bar1.p + h->b1);
	s->allocs++;
	*mem = h;
	return 0;
}

static void
r_free(void *u, void *mem)
{
	struct rsys *s = u;
	struct rmem *h = mem;

	pg106_bar_unmap(&s->g->bar1, h->b1, h->size);
	pg106_vram_free(&s->g->vram, h->pa, h->size);
	free(h);
	s->frees++;
}

static int
r_dma(void *u, void *mem, uint64_t off, uint64_t *addr, uint64_t *contig)
{
	struct rmem *h = mem;

	(void)u;
	if (off >= h->size)
		return -EFAULT;
	*addr = h->pa + off;
	*contig = h->size - off;
	return 0;
}

static int
r_target(void *u, void *mem)
{
	(void)u;
	(void)mem;
	return PG106_TARGET_VRAM;
}

static void *
r_cpu(void *u, void *mem)
{
	(void)u;
	return ((struct rmem *)mem)->cpu;
}

static int
r_map(void *u, void *mem, uint32_t handle, uint64_t *cookie)
{
	struct rsys *s = u;

	if (handle >= 64)
		return -ENODEV;
	s->mapped[handle] = mem;
	*cookie = handle + 1;
	return 0;
}

static void
r_unmap(void *u, void *mem, uint64_t cookie)
{
	(void)mem;
	if (cookie >= 1 && cookie <= 64)
		((struct rsys *)u)->mapped[cookie - 1] = NULL;
}

static int
r_lb_map(void *srv, uint64_t cookie, uint64_t size, void **ptr)
{
	struct rsys *s = srv;

	if (cookie < 1 || cookie > 64 || !s->mapped[cookie - 1] ||
	    s->mapped[cookie - 1]->size < size)
		return -ENOENT;
	*ptr = s->mapped[cookie - 1]->cpu;
	return 0;
}

static int
r_gm_map(void *u, uint64_t pa, uint64_t size, bool priv, uint64_t *va)
{
	return pg106_kd_map_vram(((struct rsys *)u)->kd, pa, size, priv, va);
}

static void
r_gm_unmap(void *u, uint64_t va, uint64_t size)
{
	pg106_kd_unmap_vram(((struct rsys *)u)->kd, va, size);
}

// PG106::claimChannel.
static int
r_chan_bind(void *u, const struct pg106_kd_chan *ch)
{
	struct rsys *s = u;
	const struct pg106_grctx_map map = { s, r_gm_map, r_gm_unmap };
	struct pg106_chan_desc d;
	int rc;

	rc = pg106_grctx_chan_create(s->gr, &map, &s->gc);
	if (rc)
		return s->bind_rc = rc;
	memset(&d, 0, sizeof(d));
	d.pdb_addr = ch->pdb_addr;
	d.pdb_target = ch->pdb_target;
	d.vm_limit = ch->vm_limit;
	d.gpfifo_va = ch->ring_va;
	d.entries = ch->ring_entries;
	d.ectx = pg106_grctx_chan_ectx(&s->gc);
	rc = pg106_chan_create_desc(&s->g->fifo, s->runl_gr, &d, &s->chan);
	if (rc)
		pg106_grctx_chan_destroy(s->gr, &map, &s->gc);
	return s->bind_rc = rc;
}

static void
r_chan_unbind(void *u)
{
	struct rsys *s = u;
	const struct pg106_grctx_map map = { s, r_gm_map, r_gm_unmap };

	if (s->chan)
		pg106_chan_destroy(s->chan);
	s->chan = NULL;
	pg106_grctx_chan_destroy(s->gr, &map, &s->gc);
}

static int
r_vm_flush(void *u, const struct pg106_mmu *mmu)
{
	struct rsys *s = u;

	s->flushes++;
	return pg106_mmu_upload(&s->g->vram, (struct pg106_mmu *)mmu, s->tables,
				s->tables_size, true);
}

static void
r_kick(void *u, uint32_t put)
{
	struct rsys *s = u;

	s->kicks++;
	__sync_synchronize();
	pg106_bar_flush(s->g->cfg.bus);
	pg106_chan_kick(s->chan, put);
}

static uint64_t
r_now(void *u)
{
	struct timespec ts;

	(void)u;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static bool
r_ready(void *u)
{
	return pg106_gpu_ready(((struct rsys *)u)->g);
}

static int
r_serve(void *srv, const void *req, size_t req_len, void *resp, size_t cap, size_t *len)
{
	return pg106_kd_handle(((struct rsys *)srv)->kd, req, req_len, resp, cap, len);
}

// 32-bit stores into a BAR1 window (no wide/unaligned accesses to MMIO).
static void
b1_copy(void *dst, const void *src, size_t n)
{
	volatile uint32_t *d = dst;
	const uint32_t *s = src;
	size_t i;

	for (i = 0; i < n / 4; i++)
		d[i] = s[i];
	__sync_synchronize();
}

static uint32_t
b1_rd32p(const void *p)
{
	return *(const volatile uint32_t *)p;
}

static void
print_trap(const char *what, unsigned step, const struct pg106_gr_trap *t)
{
	unsigned gp, tp;

	printf("%s trap (step %u): INTR 0x%08x TRAP 0x%08x [%s%s%s%s%s%s%s%s%s] "
	       "mthd 0x%08x (subc %u mthd 0x%04x) data 0x%08x code 0x%08x "
	       "class 0x%04x; DISPATCH 0x%08x M2MF 0x%08x CCACHE 0x%08x "
	       "SHADER 0x%08x UNK6 0x%08x MACRO 0x%08x pc 0x%08x op 0x%08x "
	       "SKED 0x%08x GPC_MASK 0x%08x; ctxsw cur 0x%08x next 0x%08x; "
	       "FECS mbox 0x%08x 0x%08x 0x%08x 0x%08x; FE status 0x%08x "
	       "FIFO access 0x%08x ENGINE_STATUS 0x%08x\n",
	       what, step, t->intr, t->trap,
	       t->trap & 0x1 ? "DISPATCH " : "", t->trap & 0x2 ? "M2MF " : "",
	       t->trap & 0x8 ? "CCACHE " : "", t->trap & 0x10 ? "SHADER " : "",
	       t->trap & 0x40 ? "UNK6 " : "", t->trap & 0x80 ? "MACRO " : "",
	       t->trap & 0x100 ? "SKED " : "", t->trap & 0x1000000 ? "GPC " : "",
	       t->trap & 0x2000000 ? "ROP " : "",
	       t->mthd, (t->mthd >> 16) & 7, t->mthd & 0x3ffc, t->data, t->code,
	       t->class0, t->dispatch, t->m2mf, t->ccache, t->shader, t->unk6,
	       t->macro, t->macro_pc, t->macro_op, t->sked, t->gpc_mask,
	       t->ctxsw_cur, t->ctxsw_next, t->fecs_mbox[0], t->fecs_mbox[1],
	       t->fecs_mbox[2], t->fecs_mbox[3], t->fe_status, t->fifo_access,
	       t->engine_status);
	for (gp = 0; gp < t->gpc_nr; gp++) {
		printf("%s trap GPC%u 0x502c90 0x%08x:", what, gp, t->gpc[gp]);
		for (tp = 0; tp < t->tpc_nr[gp]; tp++)
			printf(" TPC%u 0x%08x/MP warp 0x%08x global 0x%08x", tp,
			       t->tpc[gp][tp], t->mp_warp[gp][tp], t->mp_global[gp][tp]);
		printf("\n");
	}
}

#define R_KERN_VA   (1ull << 39)
#define R_IB_MAX    64u
#define R_BO_SIZE   0x10000u
#define R_QMD1_OFF  0x0000u  // data BO layout (VRAM, CAN_MAP)
#define R_QMD2_OFF  0x0100u
#define R_CODE1_OFF 0x1000u  // EXIT kernel
#define R_CODE2_OFF 0x2000u  // store kernel
#define R_CBUF_OFF  0x3000u  // c[0]: +0x140 = store VA
#define R_SEM1_OFF  0x8000u  // QMD1 RELEASE0
#define R_SEM2_OFF  0x8010u  // QMD2 RELEASE0
#define R_STORE_OFF 0x9000u
#define R_PAYLOAD1  0x7e5d0001u
#define R_PAYLOAD2  0x7e5d0002u

static int
render_poll(const void *p, uint32_t want, uint32_t budget_us, uint32_t *got, uint64_t *us)
{
	uint64_t t = 0;

	for (;;) {
		*got = b1_rd32p(p);
		if (*got == want) {
			*us = t;
			return 0;
		}
		if (t >= budget_us) {
			*us = t;
			return -ETIMEDOUT;
		}
		h_delay(NULL, 10);
		t += 10;
	}
}

// Returns 0 on success; prints one "render: ..." line either way.
static int
run_render(struct pg106_gpu *g, const struct pg106_gr_fw *gfw, unsigned timeout_ms)
{
	struct rsys s;
	struct pg106_gr *gr = calloc(1, sizeof(*gr));
	struct pg106_gr_cfg gc;
	struct pg106_kd_env e;
	struct pg106_br_loopback lb;
	struct pg106_br_loopback_server srv = { r_serve, r_lb_map, NULL, &s };
	struct pg106_br_transport tr;
	struct pg106_br_dev *dev = NULL;
	struct pg106_br_ctx *ctx = NULL;
	uint32_t pbo = 0, dbo = 0, so = 0, w[256], *pmap = NULL, *dmap = NULL;
	uint64_t pva = 0, dva = 0, v, wait1 = 0, wait2 = 0;
	uint32_t sem1 = 0, sem2 = 0, store = 0, seqno = 0, gp_get = 0, gr_intr, fifo_intr;
	const char *failed_at = NULL;
	unsigned step = 0;
	size_t n;
	int ret, rc;
	bool kd_up = false, lb_up = false, gr_up = false, tables_up = false;
	uint32_t ns0 = (uint32_t)g->nonstall_events;
	pg106_pb pb;

	memset(&s, 0, sizeof(s));
	s.g = g;
	s.gr = gr;
	if (!gr) {
		printf("render: ret -12 at: memory\n");
		return -ENOMEM;
	}
	// 1. The GR engine and the golden context (what claimChannel does
	// at the first render client's VM_INIT).
	step = 1;
	memset(&gc, 0, sizeof(gc));
	gc.bus = g->cfg.bus;
	gc.vram = &g->vram;
	gc.topo = &g->db.gr_cfg;
	gc.ctx_size = g->db.gr_nsizes ? g->db.gr_sizes[0] : 0;
	gc.fw = gfw;
	for (n = 0; n < g->db.top.ndev; n++)
		if (g->db.top.dev[n].type == PG106_TOP_TYPE_GR && g->db.top.dev[n].engine >= 0)
			gc.gr_engine = (unsigned)g->db.top.dev[n].engine;
	ret = pg106_gr_init(gr, &gc);
	if (ret) {
		failed_at = "GR engine init";
		goto out;
	}
	gr_up = true;
	ret = pg106_gr_golden(gr);
	if (ret) {
		failed_at = "golden context";
		goto out;
	}
	s.runl_gr = pg106_fifo_runl_find(&g->fifo, PG106_TOP_TYPE_GR);
	if (s.runl_gr < 0) {
		ret = -ENOENT;
		failed_at = "no GR runlist";
		goto out;
	}
	// 2. The dispatcher with a VRAM/BAR1 environment (PG106UserClient's).
	step = 2;
	s.tables_size = 0x400000;
	ret = pg106_vram_alloc(&g->vram, s.tables_size, 0x1000, PG106_VRAM_TAIL, &s.tables);
	if (ret) {
		failed_at = "VRAM for the VM tables";
		goto out;
	}
	tables_up = true;
	memset(&e, 0, sizeof(e));
	e.user = &s;
	e.mem_alloc = r_alloc;
	e.mem_free = r_free;
	e.mem_dma = r_dma;
	e.mem_target = r_target;
	e.mem_cpu = r_cpu;
	e.mem_map = r_map;
	e.mem_unmap = r_unmap;
	e.chan_bind = r_chan_bind;
	e.chan_unbind = r_chan_unbind;
	e.vm_flush = r_vm_flush;
	e.kick = r_kick;
	e.delay_us = h_delay;
	e.now_ns = r_now;
	e.ready = r_ready;
	e.pci_vendor = 0x10de;
	e.pci_device = 0x1c20;
	e.vram_bar_size = g_hw.bar1.len;
	e.vram_size = g->vram_size;
	e.boot0 = h_rd32(&g_hw, 0x000000);
	e.graph_units = (uint64_t)gr->gpc_nr | ((uint64_t)gr->tpc_total << 8) |
			((uint64_t)g->db.gr_info.rop_nr << 32);
	e.ib_max = R_IB_MAX;
	e.drm_version = 0x01000301u;
	e.abi_major = PG106_BR_ABI_MAJOR;
	e.abi_minor = PG106_BR_ABI_MINOR;
	e.mmu_cfg.table_target = PG106_TARGET_VRAM;
	e.mmu_cfg.table_base = s.tables;
	e.mmu_cfg.table_limit = s.tables + s.tables_size;
	ret = pg106_kd_create(&e, &s.kd);
	if (ret) {
		failed_at = "kdisp create";
		goto out;
	}
	kd_up = true;
	ret = pg106_br_loopback_open(&lb, &srv, &tr);
	if (ret) {
		failed_at = "loopback";
		goto out;
	}
	lb_up = true;
	// 3. The client: HANDSHAKE, a context, VM_INIT (binds the channel).
	step = 3;
	ret = pg106_br_dev_open(&tr, &dev);
	if (ret || !dev) {
		ret = ret ? ret : -EIO;
		failed_at = "bridge dev open (handshake / getparams)";
		goto out;
	}
	ret = pg106_br_ctx_create(dev, 0, &ctx);
	if (ret) {
		failed_at = "bridge ctx create";
		goto out;
	}
	ret = pg106_br_vm_init(dev, R_KERN_VA, (1ull << 40) - R_KERN_VA);
	if (ret) {
		failed_at = "VM_INIT (kernel objects, context mappings, channel bind)";
		goto out;
	}
	if (!s.chan || !s.gc.up) {
		ret = -ENODEV;
		failed_at = "VM_INIT did not bind the channel";
		goto out;
	}
	// 4. BOs: the pushbuffer (what NVK asks: GART, mappable) and the
	// data BO (VRAM, mappable), both bound in the client VM.
	step = 4;
	if ((ret = pg106_br_bo_alloc(dev, R_BO_SIZE, 0x1000,
				     PG106_BR_BO_GART | PG106_BR_BO_CAN_MAP, &pbo)) ||
	    (ret = pg106_br_bo_alloc(dev, R_BO_SIZE, 0x1000,
				     PG106_BR_BO_VRAM | PG106_BR_BO_CAN_MAP, &dbo)) ||
	    (ret = pg106_br_bo_map(dev, pbo, 0, (void **)&pmap)) ||
	    (ret = pg106_br_bo_map(dev, dbo, 0, (void **)&dmap)) ||
	    (ret = pg106_br_va_alloc(dev, R_BO_SIZE, 0x10000, &pva)) ||
	    (ret = pg106_br_va_alloc(dev, R_BO_SIZE, 0x10000, &dva))) {
		failed_at = "BO alloc / map / VA alloc";
		goto out;
	}
	{
		struct pg106_uapi_vm_bind_op op = { PG106_UAPI_VM_BIND_OP_MAP, 0, pbo, 0, pva, 0,
						    R_BO_SIZE };
		struct pg106_uapi_vm_bind_op op2 = { PG106_UAPI_VM_BIND_OP_MAP, 0, dbo, 0, dva, 0,
						     R_BO_SIZE };

		if ((ret = pg106_br_ctx_bind(ctx, &op)) || (ret = pg106_br_ctx_bind(ctx, &op2)) ||
		    (ret = pg106_br_ctx_flush(ctx))) {
			failed_at = "VM_BIND";
			goto out;
		}
	}
	// 5. Code, constant buffer, QMDs through the BAR1 maps.
	step = 5;
	{
		pg106_qmd_params qp;
		pg106_qmd qmd;
		uint64_t cb[2] = { 0, 0 };
		uint32_t zero[8] = { 0 };

		b1_copy((uint8_t *)dmap + R_CODE1_OFF, pg106_gr_sm61_exit, sizeof(pg106_gr_sm61_exit));
		b1_copy((uint8_t *)dmap + R_CODE2_OFF, pg106_gr_sm61_store,
			sizeof(pg106_gr_sm61_store));
		cb[0] = dva + R_STORE_OFF;
		b1_copy((uint8_t *)dmap + R_CBUF_OFF + 0x140, cb, 8);
		b1_copy((uint8_t *)dmap + R_SEM1_OFF, zero, 32);
		b1_copy((uint8_t *)dmap + R_STORE_OFF, zero, 32);
		memset(&qp, 0, sizeof(qp));
		qp.program_offset = R_CODE1_OFF;
		qp.raster_width = qp.raster_height = qp.raster_depth = 1;
		qp.thread_dim[0] = qp.thread_dim[1] = qp.thread_dim[2] = 1;
		qp.register_count = PG106_GR_SM61_REGS;
		qp.crs_size = 0; // no local memory on the channel (grctx-run5)
		qp.release0_addr = dva + R_SEM1_OFF;
		qp.release0_payload = R_PAYLOAD1;
		if (pg106_qmd_build(&qp, &qmd)) {
			ret = -EINVAL;
			failed_at = "QMD build";
			goto out;
		}
		b1_copy((uint8_t *)dmap + R_QMD1_OFF, qmd.w, sizeof(qmd.w));
		qp.program_offset = R_CODE2_OFF;
		qp.release0_addr = dva + R_SEM2_OFF;
		qp.release0_payload = R_PAYLOAD2;
		qp.cbuf[0].addr = dva + R_CBUF_OFF;
		qp.cbuf[0].size = 0x200;
		if (pg106_qmd_build(&qp, &qmd)) {
			ret = -EINVAL;
			failed_at = "QMD build (store)";
			goto out;
		}
		b1_copy((uint8_t *)dmap + R_QMD2_OFF, qmd.w, sizeof(qmd.w));
	}
	// 6. EXEC 1: SET_OBJECT + NVK's compute state + INVALIDATE_SKED_CACHES
	// + the EXIT dispatch; the kernel fence follows; syncobj wait.
	step = 6;
	{
		pg106_sub_compute_state st;
		uint32_t a = (uint32_t)((dva + R_QMD1_OFF) >> 8), b = 3, z = 0;
		struct pg106_uapi_exec_push p;
		struct pg106_uapi_sync sig;

		pg106_pb_init(&pb, w, sizeof(w) / 4);
		pg106_pb_set_object(&pb, 1, PG106_CLASS_PASCAL_COMPUTE_B);
		memset(&st, 0, sizeof(st));
		st.program_region = dva;
		st.local_memory_window = PG106_SUB_NVK_LOCAL_MEMORY_WINDOW;
		st.shared_memory_window = PG106_SUB_NVK_SHARED_MEMORY_WINDOW;
		if (pg106_sub_compute_init(&pb, 1, &st) || pb.error) {
			ret = -EINVAL;
			failed_at = "push encode";
			goto out;
		}
		pg106_pb_inc(&pb, 1, 0x0298, &z, 1); // INVALIDATE_SKED_CACHES
		pg106_pb_inc(&pb, 1, PG106_NVC1C0_SEND_PCAS_A, &a, 1);
		pg106_pb_inc(&pb, 1, 0x02bc, &b, 1); // SEND_SIGNALING_PCAS_B
		if (pb.error) {
			ret = -EINVAL;
			failed_at = "push encode (dispatch)";
			goto out;
		}
		b1_copy(pmap, w, pb.len * 4);
		p.va = pva;
		p.va_len = (uint32_t)(pb.len * 4);
		p.flags = 0;
		sig.flags = PG106_UAPI_SYNC_SYNCOBJ;
		sig.timeline_value = 0;
		if ((ret = pg106_br_syncobj_create(dev, 0, &so))) {
			failed_at = "syncobj create";
			goto out;
		}
		sig.handle = so;
		if ((ret = pg106_br_ctx_exec(ctx, &p)) || (ret = pg106_br_ctx_exec_signal(ctx, &sig)) ||
		    (ret = pg106_br_ctx_flush(ctx))) {
			failed_at = "EXEC 1";
			goto out;
		}
		ret = pg106_br_syncobj_wait(dev, so, 0, (uint64_t)timeout_ms * 1000000ull);
		if (ret) {
			failed_at = "EXEC 1: the kernel fence never signalled (syncobj wait)";
			goto out;
		}
		rc = render_poll((uint8_t *)dmap + R_SEM1_OFF, R_PAYLOAD1, 100000, &sem1, &wait1);
		if (rc) {
			ret = rc;
			failed_at = "EXEC 1: QMD RELEASE0 missing although the fence signalled";
			goto out;
		}
	}
	// 7. EXEC 2: the store kernel on the bound channel (no state replay).
	step = 7;
	{
		uint32_t a = (uint32_t)((dva + R_QMD2_OFF) >> 8), b = 3;
		struct pg106_uapi_exec_push p;
		struct pg106_uapi_sync sig = { PG106_UAPI_SYNC_SYNCOBJ, so, 0 };

		pg106_pb_init(&pb, w, sizeof(w) / 4);
		pg106_pb_inc(&pb, 1, PG106_NVC1C0_SEND_PCAS_A, &a, 1);
		pg106_pb_inc(&pb, 1, 0x02bc, &b, 1);
		b1_copy((uint8_t *)pmap + 0x1000, w, pb.len * 4);
		p.va = pva + 0x1000;
		p.va_len = (uint32_t)(pb.len * 4);
		p.flags = 0;
		if ((ret = pg106_br_ctx_exec(ctx, &p)) || (ret = pg106_br_ctx_exec_signal(ctx, &sig)) ||
		    (ret = pg106_br_ctx_flush(ctx))) {
			failed_at = "EXEC 2";
			goto out;
		}
		ret = pg106_br_syncobj_wait(dev, so, 0, (uint64_t)timeout_ms * 1000000ull);
		if (ret) {
			failed_at = "EXEC 2: the kernel fence never signalled (syncobj wait)";
			goto out;
		}
		rc = render_poll((uint8_t *)dmap + R_SEM2_OFF, R_PAYLOAD2, 100000, &sem2, &wait2);
		store = b1_rd32p((uint8_t *)dmap + R_STORE_OFF);
		if (rc) {
			ret = rc;
			failed_at = "EXEC 2: QMD RELEASE0 missing although the fence signalled";
			goto out;
		}
		if (store != PG106_GR_SELFTEST_STORE) {
			ret = -EIO;
			failed_at = "EXEC 2: store kernel result differs";
			goto out;
		}
	}
	step = 8;
	seqno = pg106_kd_poll(s.kd);
	if (seqno != 2) {
		ret = -EIO;
		failed_at = "fence seqno is not 2";
		goto out;
	}
	if (pg106_br_getparam(dev, PG106_UAPI_GETPARAM_VRAM_USED, &v) || v != R_BO_SIZE) {
		ret = -EIO;
		failed_at = "GETPARAM VRAM_USED is not the data BO";
		goto out;
	}
	step = 0;
out:
	if (s.chan)
		gp_get = pg106_chan_get(s.chan);
	gr_intr = h_rd32(&g_hw, 0x400100);
	if (g->intr_up && pg106_intr_pending(g->intr))
		pg106_intr_service(g->intr);
	fifo_intr = h_rd32(&g_hw, 0x002100);
	printf("render: ret %d%s%s; chid %u, ctx VA 0x%llx, mmio list %u pairs, golden 0x%x; "
	       "kdisp allocs %u (VRAM through BAR1), flushes %u, kicks %u; EXEC 1: fence + "
	       "RELEASE0 0x%08x (%llu us); EXEC 2: RELEASE0 0x%08x (%llu us), store 0x%08x "
	       "(want 0x%08x); fence seqno %u; GP_GET %u; GR_INTR 0x%08x PFIFO_INTR 0x%08x; "
	       "non-stall events +%u; MMU faults %u, PBDMA errors %u\n",
	       ret, failed_at ? " at: " : "", failed_at ? failed_at : "",
	       s.chan ? s.chan->chid : 0, (unsigned long long)s.gc.ctx_va, s.gc.mmio_nr / 2,
	       gr->size, s.allocs, s.flushes, s.kicks, sem1, (unsigned long long)wait1, sem2,
	       (unsigned long long)wait2, store, PG106_GR_SELFTEST_STORE, seqno, gp_get, gr_intr,
	       fifo_intr, (unsigned)(g->nonstall_events - ns0), g->fifo.mmu_faults,
	       g->fifo.pbdma_errors);
	if (ret && gr_up && gr->up) {
		struct pg106_gr_trap t;

		pg106_gr_read_trap(g, gr, &t);
		print_trap("render", step, &t);
	}
	if (so && dev)
		pg106_br_syncobj_destroy(dev, so);
	if (ctx)
		pg106_br_ctx_destroy(ctx);
	if (dev)
		pg106_br_dev_close(dev);
	if (kd_up)
		pg106_kd_destroy(s.kd); // unbinds the channel, frees every object
	if (lb_up)
		pg106_br_loopback_close(&lb);
	if (tables_up)
		pg106_vram_free(&g->vram, s.tables, s.tables_size);
	if (gr_up)
		pg106_gr_fini(gr);
	free(gr);
	if (!ret && s.allocs != s.frees)
		printf("render: WARNING %u allocs, %u frees\n", s.allocs, s.frees);
	return ret;
}

static void
usage(void)
{
	fprintf(stderr,
		"usage: pg106_hwtest --probe [--dev BDF] [--sysfs DIR]\n"
		"       pg106_hwtest --go --fw DIR [--vbios FILE] [--stop-after N]\n"
		"                    [--timeout-ms N] [--gr] [--render] [--dev BDF]\n"
		"                    [--sysfs DIR]\n");
}

int
main(int argc, char **argv)
{
	const char *bdf = "0000:01:00.0", *sysfs = "/sys/bus/pci/devices";
	const char *fwdir = NULL, *vbios_path = NULL;
	bool do_probe = false, go = false, any = false, do_gr = false, do_render = false;
	unsigned stop_after = PG106_DB_READY, timeout_ms = 1000;
	char dev[512], path[600], buf[64];
	struct stat st;
	int i, ret;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--probe"))
			do_probe = true;
		else if (!strcmp(argv[i], "--go"))
			go = true;
		else if (!strcmp(argv[i], "--any-gp106"))
			any = true;
		else if (!strcmp(argv[i], "--gr"))
			do_gr = true;
		else if (!strcmp(argv[i], "--render"))
			do_render = true;
		else if (!strcmp(argv[i], "--dev") && i + 1 < argc)
			bdf = argv[++i];
		else if (!strcmp(argv[i], "--sysfs") && i + 1 < argc)
			sysfs = argv[++i];
		else if (!strcmp(argv[i], "--fw") && i + 1 < argc)
			fwdir = argv[++i];
		else if (!strcmp(argv[i], "--vbios") && i + 1 < argc)
			vbios_path = argv[++i];
		else if (!strcmp(argv[i], "--stop-after") && i + 1 < argc)
			stop_after = (unsigned)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--timeout-ms") && i + 1 < argc)
			timeout_ms = (unsigned)strtoul(argv[++i], NULL, 0);
		else {
			usage();
			return 64;
		}
	}
	if (do_probe == go || (go && !fwdir)) {
		usage();
		return 64;
	}
	snprintf(dev, sizeof(dev), "%s/%s", sysfs, bdf);
	// Identity from sysfs (no config-space access needed).
	snprintf(path, sizeof(path), "%s/vendor", dev);
	if (read_file(path, buf, sizeof(buf)) < 0 || strtoul(buf, NULL, 0) != 0x10de) {
		fprintf(stderr, "refusing: %s is not an NVIDIA function\n", dev);
		return 2;
	}
	snprintf(path, sizeof(path), "%s/device", dev);
	if (read_file(path, buf, sizeof(buf)) < 0) {
		fprintf(stderr, "refusing: no device id at %s\n", path);
		return 2;
	}
	{
		unsigned long id = strtoul(buf, NULL, 0);
		bool ok = id == 0x1c20 ||
			  (any && (id == 0x1c21 || id == 0x1c22 || id == 0x1c23 ||
				   id == 0x1c60 || id == 0x1c61 || id == 0x1c62));

		if (!ok) {
			fprintf(stderr, "refusing: device 0x%04lx is not the GTX 1060 "
				"Mobile (10de:1c20)\n", id);
			return 2;
		}
	}
	snprintf(path, sizeof(path), "%s/driver", dev);
	if (lstat(path, &st) == 0) {
		char tgt[512];
		ssize_t n = readlink(path, tgt, sizeof(tgt) - 1);

		tgt[n > 0 ? n : 0] = 0;
		fprintf(stderr, "refusing: a driver is bound (%s); unbind it first\n",
			tgt);
		return 2;
	}
	snprintf(path, sizeof(path), "%s/enable", dev);
	if (read_file(path, buf, sizeof(buf)) > 0 && buf[0] == '0') {
		if (!go) {
			fprintf(stderr, "refusing: the function's decode is off "
				"(%s = 0); --probe does not turn it on\n", path);
			return 2;
		}
		{
			int fd = open(path, O_WRONLY);

			if (fd < 0 || write(fd, "1", 1) != 1) {
				fprintf(stderr, "cannot enable %s\n", path);
				return 2;
			}
			close(fd);
			printf("enabled memory decode (%s = 1)\n", path);
		}
	}
	g_hw.go = go;
	ret = map_bar(dev, 0, go, &g_hw.bar0);
	if (ret) {
		fprintf(stderr, "resource0: %s\n", strerror(-ret));
		return 3;
	}
	{
		uint32_t boot0 = h_rd32(&g_hw, 0);

		if (!pg106_pmc_probe(boot0).is_gp106) {
			fprintf(stderr, "refusing: BOOT_0 0x%08x is not a GP106 "
				"(BAR0 not decoding, or D3cold)\n", boot0);
			return 2;
		}
	}
	if (do_probe) {
		probe(&g_hw);
		return 0;
	}
	ret = map_bar(dev, 1, true, &g_hw.bar1);
	if (ret) {
		fprintf(stderr, "resource1 (BAR1): %s\n", strerror(-ret));
		return 3;
	}
	if (map_bar(dev, 3, true, &g_hw.bar3))
		g_hw.bar3.len = 0; // BAR2 VMM skipped
	{
		struct pg106_hw_bus bus = { &g_hw, h_rd32, h_wr32, h_delay, NULL, 10 };
		struct pg106_devboot_fw fw;
		uint8_t *bufs[32] = { 0 };
		uint8_t *vbios = NULL;
		size_t vbios_len = 0;
		struct pg106_gpu_cfg cfg;
		struct pg106_gpu *g = calloc(1, sizeof(*g));
		struct pg106_gpu_selftest res;
		int missing = load_fw(fwdir, &fw, bufs);

		if (vbios_path && !(vbios = slurp(vbios_path, &vbios_len))) {
			fprintf(stderr, "cannot read %s\n", vbios_path);
			return 3;
		}
		printf("firmware: %zu files, %d missing; BAR1 %llu MiB, BAR3 %llu MiB\n",
		       pg106_devboot_fw_count, missing,
		       (unsigned long long)(g_hw.bar1.len >> 20),
		       (unsigned long long)(g_hw.bar3.len >> 20));
		memset(&cfg, 0, sizeof(cfg));
		cfg.bus = &bus;
		cfg.vbios = vbios;
		cfg.vbios_size = (uint32_t)vbios_len;
		cfg.fw = &fw;
		cfg.hostmem = NULL; // HS image in VRAM: no host DMA
		cfg.bar1_size = g_hw.bar1.len;
		cfg.bar2_size = g_hw.bar3.len;
		cfg.bar1_io = (struct pg106_bar1_io){ &g_hw, b1_wr, b1_rd };
		cfg.stop_after = (uint8_t)stop_after;
		cfg.event = on_event;
		cfg.sec2_max_polls = 1000;
		// Read-only snapshot of the PMU falcon before anything is
		// written (run 1 had no reset; this shows the state it saw).
		printf("pre-boot: PMC_ENABLE 0x%08x 0x2240c 0x%08x PMU CPUCTL "
		       "0x%08x DMACTL 0x%08x ENGCTL 0x%08x MAILBOX0 0x%08x\n",
		       h_rd32(&g_hw, 0x200), h_rd32(&g_hw, 0x2240c),
		       h_rd32(&g_hw, 0x10a100), h_rd32(&g_hw, 0x10a10c),
		       h_rd32(&g_hw, 0x10a3c0), h_rd32(&g_hw, 0x10a040));
		ret = pg106_gpu_boot(g, &cfg);
		printf("gpu_boot: ret %d, stage %s, VRAM %llu MiB, fifo %s\n", ret,
		       pg106_devboot_stage_name(g->stage),
		       (unsigned long long)(g->vram_size >> 20),
		       g->fifo_up ? "up" : "down");
		{
			const struct pg106_post_result *p = &g->db.post;

			if (p->resets)
				printf("post: %u PMU falcon reset(s), max %u "
				       "0x10a10c polls per scrub wait%s; %u polls "
				       "for 0x10a040 bit 13; %zu steps\n",
				       p->resets, p->scrub_polls_max,
				       p->scrub_stuck ? " (STUCK)" : "", p->polls,
				       p->steps);
			if (p->have_diag)
				printf("post diag: CPUCTL 0x%08x MAILBOX0 0x%08x "
				       "MAILBOX1 0x%08x DMACTL 0x%08x ENGCTL "
				       "0x%08x PMC_ENABLE 0x%08x 0x2240c 0x%08x\n",
				       p->diag_cpuctl, p->diag_mbox0,
				       p->diag_mbox1, p->diag_dmactl,
				       p->diag_engctl, p->diag_pmc_enable,
				       p->diag_posted);
		}
		printf("after boot: PMC_ENABLE 0x%08x 0x2240c 0x%08x\n",
		       h_rd32(&g_hw, 0x200), h_rd32(&g_hw, 0x2240c));
		if (g->fifo_up) {
			int r2 = pg106_gpu_selftest(g, timeout_ms * 1000u, &res);

			printf("selftest: ret %d%s%s; runlist %d chid %u; host sem 0x%08x "
			       "(%llu us); CE sem 0x%08x (%llu us); copy %u bytes, %u "
			       "mismatched words; GP_GET %u; PFIFO_INTR 0x%08x -> 0x%08x after "
			       "the service; non-stall events %llu\n",
			       r2, res.failed_at ? " at: " : "",
			       res.failed_at ? res.failed_at : "", res.runlist, res.chid,
			       res.sem_value, (unsigned long long)res.sem_wait_us,
			       res.ce_sem_value, (unsigned long long)res.ce_wait_us,
			       res.ce_bytes, res.ce_mismatch, res.gp_get, res.pfifo_intr,
			       res.pfifo_intr_after, (unsigned long long)res.nonstall_events);
			if (!ret)
				ret = r2;
		}
		if (do_gr && pg106_gpu_ready(g)) {
			struct pg106_gr_fw gfw = {
				fw.sw_ctx.data, fw.sw_ctx.size,
				fw.sw_bundle_init.data, fw.sw_bundle_init.size,
				fw.sw_method_init.data, fw.sw_method_init.size,
			};
			struct pg106_gr_selftest gs;
			int r3 = pg106_gpu_gr_selftest(g, &gfw, timeout_ms * 1000u, &gs);

			printf("gr selftest: ret %d%s%s; runlist %d chid %u; golden image 0x%x "
			       "bytes%s%s (FECS 0x409800 0x%08x); ctx VA 0x%llx, mmio list %u "
			       "pairs; QMD release 0x%08x (%llu us); host sem 0x%08x; store "
			       "0x%08x (want 0x%08x, %llu us); GP_GET %u; GR_INTR 0x%08x FECS_INTR "
			       "0x%08x PFIFO_INTR 0x%08x -> 0x%08x GR_STATUS 0x%08x; MMU faults %u, PBDMA "
			       "errors %u; non-stall events %llu\n",
			       r3, gs.failed_at ? " at: " : "", gs.failed_at ? gs.failed_at : "",
			       gs.runlist, gs.chid, gs.golden_size,
			       gs.golden_failed_at ? " FAILED: " : "",
			       gs.golden_failed_at ? gs.golden_failed_at : "", gs.fecs_stat,
			       (unsigned long long)gs.ctx_va, gs.mmio_pairs, gs.sem_value,
			       (unsigned long long)gs.sem_wait_us, gs.host_sem, gs.store_value,
			       PG106_GR_SELFTEST_STORE, (unsigned long long)gs.store_wait_us,
			       gs.gp_get, gs.gr_intr, gs.fecs_intr, gs.fifo_intr, gs.fifo_intr_after,
			       gs.gr_status,
			       gs.mmu_faults, gs.pbdma_errors,
			       (unsigned long long)gs.nonstall_events);
			if (r3 == -ETIMEDOUT)
				print_trap("gr", gs.step, &gs.trap);
			if (!ret)
				ret = r3;
		} else if (do_gr) {
			printf("gr selftest: skipped (GPU not READY)\n");
		}
		if (do_render && pg106_gpu_ready(g)) {
			struct pg106_gr_fw gfw = {
				fw.sw_ctx.data, fw.sw_ctx.size,
				fw.sw_bundle_init.data, fw.sw_bundle_init.size,
				fw.sw_method_init.data, fw.sw_method_init.size,
			};
			int r4 = run_render(g, &gfw, timeout_ms);

			if (!ret)
				ret = r4;
		} else if (do_render) {
			printf("render: skipped (GPU not READY)\n");
		}
		pg106_gpu_fini(g);
		printf("done: %llu MMIO reads, %llu MMIO writes\n",
		       (unsigned long long)g_hw.reads, (unsigned long long)g_hw.writes);
		free(g);
		free(vbios);
		for (i = 0; i < 32; i++)
			free(bufs[i]);
	}
	return ret ? 1 : 0;
}
