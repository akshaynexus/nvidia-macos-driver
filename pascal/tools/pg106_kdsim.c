// SPDX-License-Identifier: GPL-2.0-only
// pascal/tools/pg106_kdsim.c - NVK's pg106 backend served by the kext's own dispatcher, on Linux.
//
// Linked into a Linux build of NVK in place of nvk/pascal/tests/{sim_icd,loopback_gpu}.c + the bridge mock
// (nvk/pascal/scripts/build-nvk-linux.sh makes <workdir>/sim/libvulkan_pg106_kdsim.so), this is the path
//
//   Vulkan app (plugin/tests/nvmtl_vk_dryrun) -> NVK + NAK -> nvkmd pg106 -> libpg106_bridge
//     -> loopback transport (the bytes IOConnectCallStructMethod carries) -> pg106_kd_handle (core/src/kdisp.c,
//        what PG106UserClient runs) -> a host-memory "GPU": kdisp's own page tables walked for every address,
//        core/src/pbdma.c consuming the GPFIFO kdisp wrote, host-class SEMAPHORE releases and acquires executed
//
// i.e. what pg106_hwtest --render ran on the GP106 under Linux (bridge -> loopback -> kdisp), with NVK as the client
// instead of the harness. One kdisp instance per connection, as one PG106UserClient per IOServiceOpen; each one's
// VM_INIT binds a channel, up to PG106_KDSIM_CHANS at once (default 4, the kext's pg106_render_chans default; 1 =
// the single-channel kext, where a second VM_INIT answers -EBUSY).
//
// It counts, per connection, every frame kdisp answered with an error (op and errno), the kicks, the releases and
// acquires it ran, PBDMA errors, and prints them to stderr at exit ("kdsim: ..."). A request NVK makes that kdisp
// refuses is a failure the first macOS run would hit the same way.
//
// SIMULATION ONLY: no 3D/compute/copy class method is executed (they are counted), and memory is host memory.
#include <pascal/kdisp.h>
#include <pascal/mmu.h>
#include <pascal/pbdma.h>
#include <pascal/uapi.h>

#include "pg106_br_bridge.h"
#include "pg106_br_loopback.h"
#include "pg106_br_wire.h"
#include "nvkmd_pg106.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KDSIM_MAX 32
#define KDSIM_IB_MAX 1024u

struct hmem {
	uint8_t *p;
	uint64_t size;
	bool imported;
};

struct kdsim {
	struct pg106_kd *kd;
	struct pg106_br_loopback lb;
	struct pg106_br_transport *tr;
	struct hmem *mapped[4096];
	struct pg106_kd_chan ch;
	bool chan_bound;
	uint32_t put, get;
	uint64_t now_ns;
	uint32_t sem_a, sem_b, sem_c;
	unsigned id;
	// what happened
	unsigned long frames, refused, kicks, releases, acquires, acquires_unmet, user_methods, gpu_errors, bad_sem;
	unsigned long imports, allocs, frees;
	char first_refusals[8][96];
	unsigned nfirst;
};

static struct kdsim *g_sims[KDSIM_MAX];
static unsigned g_nsims, g_bound, g_maxchans;
static unsigned long g_busy;
static int g_trace; // PG106_KDSIM_TRACE=1: one line per kick

static int
m_alloc(void *u, uint64_t size, uint64_t align, uint32_t flags, void **mem)
{
	struct kdsim *s = u;
	struct hmem *h = calloc(1, sizeof(*h));

	(void)flags;
	if (!h)
		return -ENOMEM;
	h->p = aligned_alloc(align < 4096 ? 4096 : align, (size + 4095) & ~4095ull);
	if (!h->p) {
		free(h);
		return -ENOMEM;
	}
	memset(h->p, 0, size);
	h->size = size;
	s->allocs++;
	*mem = h;
	return 0;
}

static int
m_import(void *u, uint64_t addr, uint64_t size, uint32_t flags, void **mem)
{
	struct kdsim *s = u;
	struct hmem *h = calloc(1, sizeof(*h));

	(void)flags;
	if (!h)
		return -ENOMEM;
	h->p = (uint8_t *)(uintptr_t)addr;
	h->size = size;
	h->imported = true;
	s->imports++;
	*mem = h;
	return 0;
}

static void
m_free(void *u, void *mem)
{
	struct kdsim *s = u;
	struct hmem *h = mem;

	if (!h->imported)
		free(h->p);
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
	*addr = (uint64_t)(uintptr_t)(h->p + off); // bus address == host VA
	*contig = h->size - off;
	return 0;
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
	struct kdsim *s = u;

	if (handle >= 4096)
		return -ENOSPC;
	s->mapped[handle] = mem;
	*cookie = handle;
	return 0;
}

static void
m_unmap(void *u, void *mem, uint64_t cookie)
{
	struct kdsim *s = u;

	(void)mem;
	if (cookie < 4096)
		s->mapped[cookie] = NULL;
}

// PG106::claimChannel: one channel per connection, PG106_KDSIM_CHANS at once
static int
m_chan_bind(void *u, const struct pg106_kd_chan *ch)
{
	struct kdsim *s = u;

	if (g_bound >= g_maxchans) {
		g_busy++;
		fprintf(stderr, "kdsim: connection %u: VM_INIT -> -EBUSY (all %u render channels in use, as the kext "
				"with pg106_render_chans=%u)\n", s->id, g_maxchans, g_maxchans);
		return -EBUSY;
	}
	s->ch = *ch;
	s->chan_bound = true;
	s->put = s->get = 0;
	g_bound++;
	return 0;
}

static void
m_chan_unbind(void *u)
{
	struct kdsim *s = u;

	if (s->chan_bound && g_bound)
		g_bound--;
	s->chan_bound = false;
}

static void *
va_ptr(struct kdsim *s, uint64_t va, size_t len)
{
	struct pg106_pte_query q;
	uint64_t pg;

	if (!pg106_mmu_walk(pg106_kd_mmu(s->kd), va, &q))
		return NULL;
	pg = 1ull << q.page_shift;
	if ((va & (pg - 1)) + len > pg)
		return NULL; // straddles a page: not needed for words
	return (void *)(uintptr_t)(q.pa + (va & (pg - 1)));
}

static int
rd_dword(void *ctx, uint64_t dword_addr, uint32_t *out)
{
	uint32_t *p = va_ptr(ctx, dword_addr * 4, 4);

	if (!p)
		return -1;
	*out = *p;
	return 0;
}

// clc06f.h host methods (any subchannel's 0x10..0x1c on subchannel 0 here; NVK and kdisp put them on 0)
static void
semaphore(struct kdsim *s, uint32_t d)
{
	const uint64_t va = ((uint64_t)(s->sem_a & 0xff) << 32) | (s->sem_b & ~3u);
	uint32_t *w = va_ptr(s, va, (d & (1u << 24)) ? 4 : 16);

	if (!w) {
		s->bad_sem++;
		return;
	}
	switch (d & 0x1f) {
	case 2: // RELEASE
		s->releases++;
		w[0] = s->sem_c;
		if (!(d & (1u << 24))) {
			w[1] = 0;
			w[2] = (uint32_t)s->now_ns;
			w[3] = (uint32_t)(s->now_ns >> 32);
		}
		break;
	case 4: // ACQ_GEQ: the real PBDMA would stall; every channel runs to completion here, in submission order
		s->acquires++;
		if ((int32_t)(w[0] - s->sem_c) < 0)
			s->acquires_unmet++;
		break;
	case 1: // ACQUIRE (equal)
		s->acquires++;
		if (w[0] != s->sem_c)
			s->acquires_unmet++;
		break;
	default:
		break;
	}
}

static void
gpu_run(struct kdsim *s)
{
	static pg106_pbdma_entry ent[16384];
	static uint32_t pool[1u << 18];
	pg106_pbdma_trace tr;
	pg106_pbdma_reader rd = { rd_dword, s };
	const uint64_t *ring;
	size_t i;

	if (!s->chan_bound || s->get == s->put)
		return;
	// kdisp allocates the ring in one piece (kobj_alloc), so its first entry's host address covers all of it
	ring = va_ptr(s, s->ch.ring_va, 8);
	pg106_pbdma_trace_init(&tr, ent, 16384, pool, 1u << 18);
	if (!ring || pg106_pbdma_run_getput(ring, s->ch.ring_entries, s->get, s->put, &rd, &tr) != PG106_PBDMA_OK ||
	    tr.error) {
		s->gpu_errors++;
		s->get = s->put;
		return;
	}
	for (i = 0; i < tr.len; i++) {
		const pg106_pbdma_entry *e = &tr.entries[i];
		uint32_t k;

		for (k = 0; k < e->data_len; k++) {
			const uint32_t m = e->mthd + (e->secop == PG106_SEC_OP_INC_METHOD ? 4 * k : 0);
			const uint32_t d = tr.data_pool[e->data_offset + k];

			if (m >= 0x100) { // a class method: what the engine would run
				s->user_methods++;
				continue;
			}
			if (m == 0x10)
				s->sem_a = d;
			else if (m == 0x14)
				s->sem_b = d;
			else if (m == 0x18)
				s->sem_c = d;
			else if (m == 0x1c)
				semaphore(s, d);
		}
	}
	s->get = s->put;
}

static void
m_kick(void *u, uint32_t put)
{
	struct kdsim *s = u;

	const unsigned long r0 = s->releases, a0 = s->acquires, e0 = s->gpu_errors + s->bad_sem, u0 = s->user_methods;

	s->kicks++;
	s->put = put;
	gpu_run(s);
	if (g_trace)
		fprintf(stderr, "kdsim: connection %u: kick %lu GP_PUT %u: %lu release(s), %lu acquire(s), %lu class method(s), "
				"%lu error(s)\n", s->id, s->kicks, put, s->releases - r0, s->acquires - a0, s->user_methods - u0,
			s->gpu_errors + s->bad_sem - e0);
}

static void
m_delay(void *u, uint32_t us)
{
	((struct kdsim *)u)->now_ns += (uint64_t)us * 1000;
}

static void
m_wait(void *u, uint32_t us)
{
	m_delay(u, us > 100 ? 100 : us);
}

static uint64_t
m_now(void *u)
{
	return ((struct kdsim *)u)->now_ns;
}

static bool
m_ready(void *u)
{
	(void)u;
	return true;
}

static const char *
op_name(uint8_t op)
{
	switch (op) {
	case PG106_BR_OP_HANDSHAKE: return "HANDSHAKE";
	case PG106_BR_OP_GETPARAM: return "GETPARAM";
	case PG106_BR_OP_BO_ALLOC: return "BO_ALLOC";
	case PG106_BR_OP_BO_FREE: return "BO_FREE";
	case PG106_BR_OP_BO_MAP: return "BO_MAP";
	case PG106_BR_OP_VM_INIT: return "VM_INIT";
	case PG106_BR_OP_VM_BIND: return "VM_BIND";
	case PG106_BR_OP_EXEC: return "EXEC";
	default: return "op";
	}
}

static int
serve(void *srv, const void *req, size_t req_len, void *resp, size_t cap, size_t *len)
{
	struct kdsim *s = srv;
	struct pg106_br_frame in, out;
	int rc = pg106_kd_handle(s->kd, req, req_len, resp, cap, len);

	s->frames++;
	if (rc == 0 && *len >= sizeof(out) && req_len >= sizeof(in)) {
		memcpy(&in, req, sizeof(in));
		memcpy(&out, resp, sizeof(out));
		if (out.status != 0) {
			s->refused++;
			if (s->nfirst < 8) {
				snprintf(s->first_refusals[s->nfirst++], sizeof s->first_refusals[0], "%s(op %u) -> %d",
					 op_name(in.op), in.op, out.status);
				fprintf(stderr, "kdsim: connection %u: kdisp refused %s\n", s->id, s->first_refusals[s->nfirst - 1]);
			}
		}
	}
	return rc;
}

static int
lb_map(void *srv, uint64_t cookie, uint64_t size, void **ptr)
{
	struct kdsim *s = srv;

	if (cookie >= 4096 || !s->mapped[cookie] || s->mapped[cookie]->size < size)
		return -ENOENT;
	*ptr = s->mapped[cookie]->p;
	return 0;
}

static void
lb_unmap(void *srv, uint64_t cookie, void *ptr)
{
	(void)srv; (void)cookie; (void)ptr;
}

static int
kdsim_open(struct pg106_br_transport *tr)
{
	struct kdsim *s;
	struct pg106_kd_env e;
	struct pg106_br_loopback_server srv;
	int rc;

	if (g_nsims >= KDSIM_MAX)
		return -ENOSPC;
	s = calloc(1, sizeof(*s));
	if (!s)
		return -ENOMEM;
	s->id = g_nsims;
	memset(&e, 0, sizeof(e));
	e.user = s;
	e.mem_alloc = m_alloc;
	e.mem_import = m_import;
	e.mem_free = m_free;
	e.mem_dma = m_dma;
	e.mem_cpu = m_cpu;
	e.mem_map = m_map;
	e.mem_unmap = m_unmap;
	e.chan_bind = m_chan_bind;
	e.chan_unbind = m_chan_unbind;
	e.kick = m_kick;
	e.delay_us = m_delay;
	e.wait_event = m_wait;
	e.now_ns = m_now;
	e.ready = m_ready;
	// what PG106::kdEnvBase + PG106UserClient give it on the Helios
	e.pci_vendor = 0x10de;
	e.pci_device = 0x1c20;
	e.vram_bar_size = 256ull << 20;
	e.vram_size = 6ull << 30;
	e.boot0 = 0x136000a1u;
	e.graph_units = 2u | (10u << 8) | (48ull << 32);
	e.ib_max = KDSIM_IB_MAX;
	e.drm_version = 0x01000301u;
	e.abi_major = PG106_BR_ABI_MAJOR;
	e.abi_minor = PG106_BR_ABI_MINOR;
	e.mmu_cfg.table_target = PG106_TARGET_VRAM;
	e.mmu_cfg.table_base = 0x1000000ull;
	rc = pg106_kd_create(&e, &s->kd);
	if (rc) {
		free(s);
		return rc;
	}
	srv.serve = serve;
	srv.map = lb_map;
	srv.unmap = lb_unmap;
	srv.srv = s;
	rc = pg106_br_loopback_open(&s->lb, &srv, tr);
	if (rc) {
		pg106_kd_destroy(s->kd);
		free(s);
		return rc;
	}
	s->tr = tr;
	g_sims[g_nsims++] = s;
	return 0;
}

static void
kdsim_close(struct pg106_br_transport *tr)
{
	for (unsigned i = 0; i < g_nsims; i++) {
		struct kdsim *s = g_sims[i];

		if (s && s->tr == tr && s->kd) {
			pg106_kd_destroy(s->kd); // teardown as PG106UserClient::stop: drain, channel off, unmap, free
			s->kd = NULL;
			pg106_br_loopback_close(&s->lb);
			s->tr = NULL;
		}
	}
}

static void
kdsim_report(void)
{
	unsigned long refused = 0, gpu = 0;

	for (unsigned i = 0; i < g_nsims; i++) {
		struct kdsim *s = g_sims[i];

		fprintf(stderr, "kdsim: connection %u: %lu frames, %lu refused, %lu kicks, %lu releases, %lu acquires "
				"(%lu unmet), %lu class methods, %lu PBDMA errors, %lu bad semaphore VAs, %lu/%lu kernel "
				"allocations freed, %lu host imports%s\n",
			i, s->frames, s->refused, s->kicks, s->releases, s->acquires, s->acquires_unmet, s->user_methods,
			s->gpu_errors, s->bad_sem, s->frees, s->allocs + s->imports, s->imports,
			s->kd ? " (still open)" : "");
		for (unsigned k = 0; k < s->nfirst; k++)
			fprintf(stderr, "kdsim:   refused %s\n", s->first_refusals[k]);
		refused += s->refused;
		gpu += s->gpu_errors + s->bad_sem;
	}
	fprintf(stderr, "kdsim: %s: %u connection(s), %lu refused frame(s), %lu GPU-model error(s), %lu VM_INIT -EBUSY\n",
		refused || gpu ? "FINDINGS" : "CLEAN", g_nsims, refused, gpu, g_busy);
}

// nvkmd_nvrm_ctx.c calls this macOS-only helper; the NVRM backend is never used here (NVK_KMD=pg106).
int64_t nvrm_xnu_wait_event(int fd, uint32_t seen, uint32_t timeout_us);
int64_t
nvrm_xnu_wait_event(int fd, uint32_t seen, uint32_t timeout_us)
{
	(void)fd; (void)seen; (void)timeout_us;
	return -1;
}

static void __attribute__((constructor))
pg106_kdsim_init(void)
{
	const char *c = getenv("PG106_KDSIM_CHANS");

	g_maxchans = c ? (unsigned)strtoul(c, NULL, 0) : 4;
	g_trace = getenv("PG106_KDSIM_TRACE") != NULL;
	if (g_maxchans < 1)
		g_maxchans = 1;
	setenv("NVK_KMD", "pg106", 0);
	nvkmd_pg106_set_transport_hook(kdsim_open, kdsim_close);
	atexit(kdsim_report);
	fprintf(stderr, "pg106-kdsim: NVK with the pg106 backend over the kext's dispatcher (kdisp), %u render channel(s) "
			"(SIMULATION: host memory, host methods executed, class methods counted)\n", g_maxchans);
}
