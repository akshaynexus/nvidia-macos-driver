// SPDX-License-Identifier: GPL-2.0-only
// pascal/tests/test_kdisp.c — the bridge wire protocol end to end on Linux:
//
//   libpg106_bridge (pascal/bridge, MIT)
//     -> loopback transport (same header||payload buffers the IOKit
//        transport hands IOConnectCallStructMethod)
//     -> pg106_kd_handle (core/src/kdisp.c, the kext's dispatcher)
//     -> mock GPU: host memory as "system memory", the core MMU model for
//        VA -> PA, the core PBDMA model (core/src/pbdma.c) to consume the
//        GPFIFO the dispatcher wrote, and SEMAPHOREA-D RELEASE executed
//        into memory (the methods nvc0_fence_emit32 and NVK use).
//
// SIMULATION. The "GPU" here only executes channel semaphore releases; it
// says nothing about how a GP106 runs these pushes. What it does prove is
// the protocol: every op's bytes, the kernel-side validation (requests the
// bridge would never send are injected as raw frames), the GPFIFO and
// fence the kernel builds, and that completion flows back to syncobjs.

#include <pascal/kdisp.h>
#include <pascal/mmu.h>
#include <pascal/pbdma.h>
#include <pascal/pushbuf.h>
#include <pascal/uapi.h>

#include "pg106_br_bridge.h"
#include "pg106_br_loopback.h"
#include "pg106_br_wire.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

#define CHECK(c)                                                         \
	do {                                                             \
		if (!(c)) {                                              \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			g_fail++;                                        \
		}                                                        \
	} while (0)
#define CHECK_EQ(a, b)                                                   \
	do {                                                             \
		long long a_ = (long long)(a), b_ = (long long)(b);      \
		if (a_ != b_) {                                          \
			printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n",  \
			       __FILE__, __LINE__, #a, #b, a_, b_);      \
			g_fail++;                                        \
		}                                                        \
	} while (0)

#define IB_MAX 64u
#define KERN_VA (1ull << 39) // NOUVEAU_WS_DEVICE_KERNEL_RESERVATION_START

// ---- host "system memory" -------------------------------------------------

struct hmem {
	uint8_t *p;
	uint64_t size;
	bool imported;
	uint32_t cookie_handle;
};

struct sys {
	struct pg106_kd *kd;
	size_t allocs, frees, imports;
	struct hmem *mapped[256]; // cookie -> mem
	struct pg106_kd_chan ch;
	bool chan_bound;
	bool ready;
	bool async;         // GPU runs on delay_us instead of at the kick
	uint32_t put, get;  // GP_PUT seen, GP_GET consumed
	uint64_t now_ns;
	size_t kicks, releases, user_methods, gpu_errors;
	int fail_alloc_after; // -1 = never
	int chan_rc;          // what chan_bind answers (0 = bind)
	size_t waits;         // env->wait_event calls (SYNCOBJ_WAIT sleeps)
	uint64_t wait_us;     // what they asked for
};

static int
m_alloc(void *u, uint64_t size, uint64_t align, uint32_t flags, void **mem)
{
	struct sys *s = u;
	struct hmem *h;

	(void)flags;
	if (s->fail_alloc_after == 0)
		return -ENOMEM;
	if (s->fail_alloc_after > 0)
		s->fail_alloc_after--;
	h = calloc(1, sizeof(*h));
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
	struct sys *s = u;
	struct hmem *h = calloc(1, sizeof(*h));

	(void)flags;
	if (!h)
		return -ENOMEM;
	h->p = (uint8_t *)(uintptr_t)addr; // "wired" for the BO lifetime
	h->size = size;
	h->imported = true;
	s->imports++;
	s->allocs++;
	*mem = h;
	return 0;
}

static void
m_free(void *u, void *mem)
{
	struct sys *s = u;
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
	struct sys *s = u;

	if (handle >= 256)
		return -ENOSPC;
	s->mapped[handle] = mem;
	*cookie = handle; // IOConnectMapMemory64 memory type
	return 0;
}

static void
m_unmap(void *u, void *mem, uint64_t cookie)
{
	struct sys *s = u;

	(void)mem;
	if (cookie < 256)
		s->mapped[cookie] = NULL;
}

static int
m_chan_bind(void *u, const struct pg106_kd_chan *ch)
{
	struct sys *s = u;

	if (s->chan_rc)
		return s->chan_rc;
	s->ch = *ch;
	s->chan_bound = true;
	s->put = s->get = 0;
	return 0;
}

static void
m_chan_unbind(void *u)
{
	((struct sys *)u)->chan_bound = false;
}

// ---- the mock GPU ---------------------------------------------------------

static void *
va_ptr(struct sys *s, uint64_t va)
{
	struct pg106_pte_query q;

	if (!pg106_mmu_walk(pg106_kd_mmu(s->kd), va, &q))
		return NULL;
	return (void *)(uintptr_t)(q.pa + (va & ((1ull << q.page_shift) - 1)));
}

static int
rd_dword(void *ctx, uint64_t dword_addr, uint32_t *out)
{
	uint32_t *p = va_ptr(ctx, dword_addr * 4);

	if (!p)
		return -1; // PBDMA MEMREQ fault
	*out = *p;
	return 0;
}

// NVC06F semaphore state per run (methods 0x10..0x1c).
static void
gpu_run(struct sys *s)
{
	static pg106_pbdma_entry ent[4096];
	static uint32_t pool[65536];
	pg106_pbdma_trace tr;
	pg106_pbdma_reader rd = { rd_dword, s };
	const uint64_t *ring;
	uint32_t sa = 0, sb = 0, sc = 0;
	size_t i;

	if (!s->chan_bound || s->get == s->put)
		return;
	ring = va_ptr(s, s->ch.ring_va);
	pg106_pbdma_trace_init(&tr, ent, 4096, pool, 65536);
	if (!ring || pg106_pbdma_run_getput(ring, s->ch.ring_entries, s->get,
					    s->put, &rd, &tr) != PG106_PBDMA_OK ||
	    tr.error) {
		s->gpu_errors++;
		s->get = s->put;
		return;
	}
	for (i = 0; i < tr.len; i++) {
		const pg106_pbdma_entry *e = &tr.entries[i];
		uint32_t k;

		if (e->data_len == 0)
			continue;
		if (e->subch != 0) { // the class methods a real engine would run
			s->user_methods += e->data_len;
			continue;
		}
		for (k = 0; k < e->data_len; k++) {
			// INC methods advance by 4 per data word.
			const uint32_t m = e->mthd + (e->secop == PG106_SEC_OP_INC_METHOD ? 4 * k : 0);
			const uint32_t d = tr.data_pool[e->data_offset + k];

			if (m == 0x10) {
				sa = d;
			} else if (m == 0x14) {
				sb = d;
			} else if (m == 0x18) {
				sc = d;
			} else if (m == 0x1c) {
				if ((d & 0x1f) == 2) { // RELEASE, 16-byte
					uint32_t *dst = va_ptr(s, ((uint64_t)(sa & 0xff) << 32) | sb);

					if (dst) {
						dst[0] = sc;
						dst[1] = 0;
						s->releases++;
					} else {
						s->gpu_errors++;
					}
				}
			}
		}
	}
	s->get = s->put;
}

static void
m_kick(void *u, uint32_t put)
{
	struct sys *s = u;

	s->kicks++;
	s->put = put;
	if (!s->async)
		gpu_run(s);
}

static void
m_delay(void *u, uint32_t us)
{
	struct sys *s = u;

	s->now_ns += (uint64_t)us * 1000;
	if (s->async)
		gpu_run(s);
}

// The kext sleeps here until the fence IRQ; the mock lets time pass.
static void
m_wait(void *u, uint32_t us)
{
	struct sys *s = u;

	s->waits++;
	s->wait_us += us;
	m_delay(u, us > 100 ? 100 : us);
}

static uint64_t
m_now(void *u)
{
	return ((struct sys *)u)->now_ns;
}

static bool
m_ready(void *u)
{
	return ((struct sys *)u)->ready;
}

static struct pg106_kd_env
env_of(struct sys *s)
{
	struct pg106_kd_env e;

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
	e.pci_vendor = 0x10de;
	e.pci_device = 0x1c20;
	e.vram_bar_size = 256ull << 20;
	e.vram_size = 6ull << 30;
	e.boot0 = 0x136000a1u;
	e.graph_units = 2u | (10u << 8) | (48ull << 32);
	e.ib_max = IB_MAX;
	e.drm_version = 0x01000301u;
	e.abi_major = PG106_BR_ABI_MAJOR;
	e.abi_minor = PG106_BR_ABI_MINOR;
	e.mmu_cfg.table_target = PG106_TARGET_VRAM;
	e.mmu_cfg.table_base = 0x1000000ull;
	return e;
}

// ---- loopback server ------------------------------------------------------

static int
serve(void *srv, const void *req, size_t req_len, void *resp, size_t cap,
      size_t *len)
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

// Raw frame straight to the dispatcher (what a hostile or buggy client
// could send): returns the status, or the transport error.
static int
raw(struct sys *s, uint8_t op, const void *pl, size_t n)
{
	static uint8_t req[64 * 1024], resp[256];
	struct pg106_br_frame h, o;
	size_t rl = 0;
	int rc;

	memset(&h, 0, sizeof(h));
	h.magic = PG106_BR_WIRE_MAGIC;
	h.op = op;
	h.wire_version = PG106_BR_WIRE_VERSION;
	h.payload_len = (uint32_t)n;
	h.seq = 0x77;
	memcpy(req, &h, sizeof(h));
	if (n)
		memcpy(req + sizeof(h), pl, n);
	rc = pg106_kd_handle(s->kd, req, sizeof(h) + n, resp, sizeof(resp), &rl);
	if (rc)
		return rc;
	memcpy(&o, resp, sizeof(o));
	CHECK_EQ(o.seq, 0x77);
	CHECK_EQ(o.op, op);
	CHECK_EQ(rl, sizeof(o) + o.payload_len);
	return o.status;
}

// One user push: SEMAPHORE release of `payload` at `va` (what NVK's own
// control pushes do) plus a SET_OBJECT the mock counts as a user method.
static size_t
user_push(uint32_t *w, uint64_t va, uint32_t payload)
{
	uint32_t d[4] = { (uint32_t)(va >> 32) & 0xff, (uint32_t)va, payload, 2 };
	uint32_t nop = 0;
	pg106_pb pb;

	pg106_pb_init(&pb, w, 64);
	pg106_pb_set_object(&pb, 1, PG106_CLASS_PASCAL_COMPUTE_B);
	pg106_pb_inc(&pb, 1, 0x0200, &nop, 1); // a compute-class method
	pg106_pb_inc(&pb, 0, 0x0010, d, 4);
	return pb.len;
}

static struct sys *
sys_new(void)
{
	struct sys *s = calloc(1, sizeof(*s));

	s->ready = true;
	s->fail_alloc_after = -1;
	return s;
}

// ---- tests ----------------------------------------------------------------

static void
test_end_to_end(void)
{
	struct sys *s = sys_new();
	struct pg106_kd_env env = env_of(s);
	struct pg106_br_loopback lb;
	struct pg106_br_loopback_server srv = { serve, lb_map, NULL, s };
	struct pg106_br_transport tr;
	struct pg106_br_dev *dev = NULL;
	struct pg106_br_ctx *ctx = NULL;
	uint32_t pbo, sbo, so, i;
	uint64_t v, pva, sva;
	uint32_t *pmap, *smap;
	size_t len;

	CHECK_EQ(pg106_kd_create(&env, &s->kd), 0);
	CHECK_EQ(pg106_br_loopback_open(&lb, &srv, &tr), 0);
	CHECK_EQ(pg106_br_dev_open(&tr, &dev), 0);
	if (!dev)
		return;

	// GETPARAM answers (nouveau_abi16.c:299-327 + private ids).
	CHECK_EQ(pg106_br_getparam(dev, PG106_UAPI_GETPARAM_PCI_DEVICE, &v), 0);
	CHECK_EQ(v, 0x1c20);
	CHECK_EQ(pg106_br_getparam(dev, PG106_UAPI_GETPARAM_EXEC_PUSH_MAX, &v), 0);
	CHECK_EQ(v, IB_MAX / 2 - 1);
	CHECK_EQ(pg106_br_getparam(dev, PG106_UAPI_GETPARAM_VRAM_BAR_SIZE, &v), 0);
	CHECK_EQ(v, 256ull << 20);
	CHECK_EQ(pg106_br_getparam(dev, PG106_BR_GETPARAM_VRAM_SIZE, &v), 0);
	CHECK_EQ(v, 6ull << 30);
	CHECK_EQ(pg106_br_getparam(dev, PG106_BR_GETPARAM_BOOT0, &v), 0);
	CHECK_EQ(v, 0x136000a1u);
	CHECK_EQ(pg106_br_getparam(dev, PG106_UAPI_GETPARAM_GRAPH_UNITS, &v), 0);
	CHECK_EQ(v & 0xff, 2);
	CHECK_EQ(pg106_br_getparam(dev, PG106_BR_GETPARAM_READY, &v), 0);
	CHECK_EQ(v, 1);
	CHECK_EQ(pg106_br_getparam(dev, 0x1234, &v), -EINVAL);

	// EXEC before VM_INIT has no channel.
	CHECK_EQ(pg106_br_ctx_create(dev, 0, &ctx), 0);
	CHECK_EQ(pg106_br_vm_init(dev, KERN_VA, (1ull << 40) - KERN_VA), 0);
	CHECK(s->chan_bound);
	CHECK_EQ(s->ch.ring_entries, IB_MAX);
	CHECK_EQ(s->ch.ring_va, KERN_VA);

	// Push buffer + semaphore BO, CPU-mapped through the cookie path.
	CHECK_EQ(pg106_br_bo_alloc(dev, 0x10000, 0x1000,
				   PG106_BR_BO_GART | PG106_BR_BO_CAN_MAP, &pbo), 0);
	CHECK_EQ(pg106_br_bo_alloc(dev, 0x1000, 0x1000,
				   PG106_BR_BO_GART | PG106_BR_BO_CAN_MAP, &sbo), 0);
	CHECK_EQ(pg106_br_bo_map(dev, pbo, 0, (void **)&pmap), 0);
	CHECK_EQ(pg106_br_bo_map(dev, sbo, 0, (void **)&smap), 0);
	CHECK_EQ(pg106_br_va_alloc(dev, 0x10000, 0x10000, &pva), 0);
	CHECK_EQ(pg106_br_va_alloc(dev, 0x1000, 0x1000, &sva), 0);
	{
		struct pg106_uapi_vm_bind_op op = { PG106_UAPI_VM_BIND_OP_MAP, 0, pbo, 0, pva, 0, 0x10000 };
		struct pg106_uapi_vm_bind_op op2 = { PG106_UAPI_VM_BIND_OP_MAP, 0, sbo, 0, sva, 0, 0x1000 };

		CHECK_EQ(pg106_br_ctx_bind(ctx, &op), 0);
		CHECK_EQ(pg106_br_ctx_bind(ctx, &op2), 0);
		CHECK_EQ(pg106_br_ctx_flush(ctx), 0);
	}
	{
		uint32_t h;
		uint64_t off;

		CHECK(pg106_kd_va_lookup(s->kd, pva + 0x40, &h, &off));
		CHECK_EQ(h, pbo);
		CHECK_EQ(off, 0x40);
	}

	// One job with a syncobj signal; the "GPU" runs it at the kick.
	len = user_push(pmap, sva, 0xc0ffee01u);
	CHECK_EQ(pg106_br_syncobj_create(dev, 0, &so), 0);
	{
		struct pg106_uapi_exec_push p = { pva, (uint32_t)(len * 4), 0 };
		struct pg106_uapi_sync sig = { PG106_UAPI_SYNC_SYNCOBJ, so, 0 };

		CHECK_EQ(pg106_br_ctx_exec(ctx, &p), 0);
		CHECK_EQ(pg106_br_ctx_exec_signal(ctx, &sig), 0);
		CHECK_EQ(pg106_br_ctx_flush(ctx), 0);
	}
	CHECK_EQ(pg106_br_syncobj_wait(dev, so, 0, 1000000000ull), 0);
	CHECK_EQ(smap[0], 0xc0ffee01u);
	CHECK_EQ(s->releases, 2); // the user's + the kernel fence
	CHECK_EQ(s->user_methods, 2);
	CHECK_EQ(s->gpu_errors, 0);
	CHECK_EQ(pg106_kd_seqno_emitted(s->kd), 1);
	CHECK_EQ(pg106_kd_poll(s->kd), 1);
	printf("PASS e2e: handshake, getparams, VM_INIT (channel bound), BO "
	       "alloc+map, VM_BIND, EXEC -> GPFIFO -> PBDMA model -> semaphore "
	       "release, fence -> syncobj wait\n");

	// Async GPU: work completes only as time passes; the wait polls.
	s->async = true;
	len = user_push(pmap + 64, sva + 16, 0x2u);
	{
		struct pg106_uapi_exec_push p = { pva + 256, (uint32_t)(len * 4), 0 };
		struct pg106_uapi_sync sig = { PG106_UAPI_SYNC_SYNCOBJ, so, 0 };

		CHECK_EQ(pg106_br_ctx_exec(ctx, &p), 0);
		CHECK_EQ(pg106_br_ctx_exec_signal(ctx, &sig), 0);
		CHECK_EQ(pg106_br_ctx_flush(ctx), 0);
	}
	CHECK_EQ(smap[4], 0);
	CHECK_EQ(pg106_br_syncobj_wait(dev, so, 0, 0), -ETIME); // not yet
	{
		size_t w0 = s->waits;

		CHECK_EQ(pg106_br_syncobj_wait(dev, so, 0, 5000000ull), 0);
		// SYNCOBJ_WAIT slept through wait_event (no busy polling), each
		// sleep bounded by 10 ms and by the time left.
		CHECK(s->waits > w0);
		CHECK(s->wait_us <= (s->waits) * 10000ull);
	}
	CHECK_EQ(smap[4], 2);

	// Ring wrap under back-pressure: 3 jobs x 2 entries per kick round,
	// 200 jobs through a 64-entry ring.
	for (i = 0; i < 200; i++) {
		struct pg106_uapi_exec_push p = { pva + 256, (uint32_t)(len * 4), 0 };

		user_push(pmap + 64, sva + 32, 100 + i);
		CHECK_EQ(pg106_br_ctx_exec(ctx, &p), 0);
		CHECK_EQ(pg106_br_ctx_flush(ctx), 0);
		if (i % 3 == 2) {
			struct pg106_uapi_sync sig = { PG106_UAPI_SYNC_SYNCOBJ, so, 0 };

			CHECK_EQ(pg106_br_ctx_exec_signal(ctx, &sig), 0);
			CHECK_EQ(pg106_br_ctx_flush(ctx), 0); // empty exec + signal
			CHECK_EQ(pg106_br_syncobj_wait(dev, so, 0, 5000000000ull), 0);
		}
	}
	CHECK_EQ(pg106_br_syncobj_wait(dev, so, 0, 5000000000ull), 0);
	CHECK(s->gpu_errors == 0);
	CHECK(pg106_kd_seqno_emitted(s->kd) > 200);
	printf("PASS ring: 200 jobs + %u sync-only execs through a %u-entry "
	       "GPFIFO with an async GPU (%zu kicks)\n", 200 / 3, IB_MAX, s->kicks);
	s->async = false;

	// Timeline syncobj points.
	{
		uint32_t tl;
		struct pg106_uapi_sync sig = { PG106_UAPI_SYNC_TIMELINE_SYNCOBJ, 0, 5 };

		CHECK_EQ(pg106_br_syncobj_create(dev, 0, &tl), 0);
		CHECK_EQ(pg106_br_syncobj_wait(dev, tl, 1, 0), -EINVAL); // nothing yet
		sig.handle = tl;
		CHECK_EQ(pg106_br_ctx_exec_signal(ctx, &sig), 0);
		CHECK_EQ(pg106_br_ctx_flush(ctx), 0);
		CHECK_EQ(pg106_br_syncobj_wait(dev, tl, 3, 1000), 0);
		CHECK_EQ(pg106_br_syncobj_wait(dev, tl, 6, 1000), -EINVAL);
		CHECK_EQ(pg106_br_syncobj_destroy(dev, tl), 0);
		CHECK_EQ(pg106_br_syncobj_wait(dev, tl, 3, 1000), -ENOENT);
	}

	// Host import (the offload path's IOSurface pages).
	{
		uint8_t *host = aligned_alloc(4096, 0x2000);
		uint32_t ih;
		uint64_t iva;
		void *bad;
		struct pg106_uapi_vm_bind_op op;

		memset(host, 0, 0x2000);
		CHECK_EQ(pg106_br_bo_import_host(dev, host + 1, 0x1000, 0, &ih), -EINVAL);
		CHECK_EQ(pg106_br_bo_import_host(dev, host, 0x1001, 0, &ih), -EINVAL);
		CHECK_EQ(pg106_br_bo_import_host(dev, host, 0x2000, PG106_BR_BO_GART, &ih), 0);
		CHECK_EQ(pg106_br_bo_map(dev, ih, 0, &bad), -EINVAL);
		CHECK_EQ(pg106_br_va_alloc(dev, 0x2000, 0x1000, &iva), 0);
		op = (struct pg106_uapi_vm_bind_op){ PG106_UAPI_VM_BIND_OP_MAP, 0, ih, 0, iva, 0, 0x2000 };
		CHECK_EQ(pg106_br_ctx_bind(ctx, &op), 0);
		CHECK_EQ(pg106_br_ctx_flush(ctx), 0);
		len = user_push(pmap + 128, iva + 0x1000, 0x5eed);
		{
			struct pg106_uapi_exec_push p = { pva + 512, (uint32_t)(len * 4), 0 };
			struct pg106_uapi_sync sig = { PG106_UAPI_SYNC_SYNCOBJ, so, 0 };

			CHECK_EQ(pg106_br_ctx_exec(ctx, &p), 0);
			CHECK_EQ(pg106_br_ctx_exec_signal(ctx, &sig), 0);
			CHECK_EQ(pg106_br_ctx_flush(ctx), 0);
		}
		CHECK_EQ(pg106_br_syncobj_wait(dev, so, 0, 1000000000ull), 0);
		CHECK_EQ(((uint32_t *)host)[0x1000 / 4], 0x5eed);
		CHECK_EQ(pg106_br_bo_free(dev, ih), -EBUSY); // still bound
		op.op = PG106_UAPI_VM_BIND_OP_UNMAP;
		op.handle = 0;
		CHECK_EQ(pg106_br_ctx_bind(ctx, &op), 0);
		CHECK_EQ(pg106_br_ctx_flush(ctx), 0);
		CHECK_EQ(pg106_br_bo_free(dev, ih), 0);
		CHECK_EQ(s->imports, 1);
		free(host);
	}
	printf("PASS timeline points, host import (bind, GPU write, -EBUSY while "
	       "bound, unbind, free)\n");

	pg106_br_ctx_destroy(ctx);
	CHECK_EQ(pg106_br_dev_close(dev), 0);
	pg106_br_loopback_close(&lb);
	pg106_kd_destroy(s->kd);
	CHECK_EQ(s->allocs, s->frees); // every BO + kernel object released
	CHECK(!s->chan_bound);
	free(s);
}

static void
test_kernel_validation(void)
{
	struct sys *s = sys_new();
	struct pg106_kd_env env = env_of(s);
	struct pg106_br_bo_alloc ba;
	struct pg106_uapi_vm_init vi = { KERN_VA, (1ull << 40) - KERN_VA };
	uint8_t buf[4096];
	uint32_t h;
	size_t rl;

	CHECK_EQ(pg106_kd_create(&env, &s->kd), 0);

	// Not frames at all: the transport call fails.
	memset(buf, 0, sizeof(buf));
	CHECK_EQ(pg106_kd_handle(s->kd, buf, 10, buf + 1024, 256, &rl), -EPROTO);
	CHECK_EQ(pg106_kd_handle(s->kd, buf, 24, buf + 1024, 256, &rl), -EPROTO);
	// Unknown op and wrong payload sizes are answered -EPROTO.
	CHECK_EQ(raw(s, 99, NULL, 0), -EPROTO);
	CHECK_EQ(raw(s, PG106_BR_OP_GETPARAM, buf, 15), -EPROTO);

	// Before VM_INIT: binds -ENOSYS, execs -ENODEV.
	{
		struct pg106_uapi_vm_bind vb = { 0 };
		struct pg106_uapi_exec ex = { 0 };

		CHECK_EQ(raw(s, PG106_BR_OP_VM_BIND, &vb, sizeof(vb)), -ENOSYS);
		CHECK_EQ(raw(s, PG106_BR_OP_EXEC, &ex, sizeof(ex)), -ENODEV);
	}
	// A kernel region too small for the kernel's own objects.
	{
		struct pg106_uapi_vm_init small = { KERN_VA, 0x1000 };

		CHECK_EQ(raw(s, PG106_BR_OP_VM_INIT, &small, sizeof(small)), -EINVAL);
	}
	CHECK_EQ(raw(s, PG106_BR_OP_VM_INIT, &vi, sizeof(vi)), 0);
	CHECK_EQ(raw(s, PG106_BR_OP_VM_INIT, &vi, sizeof(vi)), -EEXIST);

	// BO_ALLOC rules: nonzero handle in the request, bad alignment.
	memset(&ba, 0, sizeof(ba));
	ba.size_B = 0x2000;
	ba.align_B = 0x1000;
	ba.handle = 5;
	CHECK_EQ(raw(s, PG106_BR_OP_BO_ALLOC, &ba, sizeof(ba)), -EINVAL);
	ba.handle = 0;
	ba.align_B = 0x1800;
	CHECK_EQ(raw(s, PG106_BR_OP_BO_ALLOC, &ba, sizeof(ba)), -EINVAL);
	ba.align_B = 0x1000;
	ba.flags = PG106_BR_BO_GART; // not CAN_MAP
	{
		static uint8_t req[128], resp[256];
		struct pg106_br_frame f = { PG106_BR_WIRE_MAGIC, PG106_BR_OP_BO_ALLOC,
					    0, 0, sizeof(ba), 1, 0, 0 };
		struct pg106_br_frame o;

		memcpy(req, &f, sizeof(f));
		memcpy(req + sizeof(f), &ba, sizeof(ba));
		CHECK_EQ(pg106_kd_handle(s->kd, req, sizeof(f) + sizeof(ba), resp,
					 sizeof(resp), &rl), 0);
		memcpy(&o, resp, sizeof(o));
		CHECK_EQ(o.status, 0);
		CHECK_EQ(o.payload_len, sizeof(ba));
		memcpy(&ba, resp + sizeof(o), sizeof(ba));
		h = ba.handle;
		CHECK(h != 0);
	}
	{
		struct pg106_br_bo_map bm = { h, 0, 0 };

		CHECK_EQ(raw(s, PG106_BR_OP_BO_MAP, &bm, sizeof(bm)), -EACCES);
	}

	// VM_BIND, kernel-side rules (the bridge never sends these).
	{
		struct {
			struct pg106_uapi_vm_bind h;
			struct pg106_uapi_vm_bind_op op;
		} f;

		memset(&f, 0, sizeof(f));
		f.h.op_count = 1;
		f.op.op = PG106_UAPI_VM_BIND_OP_MAP;
		f.op.handle = 0x999; // unknown BO
		f.op.addr = 0x100000;
		f.op.range = 0x1000;
		CHECK_EQ(raw(s, PG106_BR_OP_VM_BIND, &f, sizeof(f)), -ENOENT);
		f.op.handle = h;
		f.op.range = 0x3000; // larger than the BO
		CHECK_EQ(raw(s, PG106_BR_OP_VM_BIND, &f, sizeof(f)), -EINVAL);
		f.op.range = 0x1000;
		f.op.addr = KERN_VA + 0x1000; // inside the kernel-managed region
		CHECK_EQ(raw(s, PG106_BR_OP_VM_BIND, &f, sizeof(f)), -EINVAL);
		f.op.addr = 0x100800; // misaligned
		CHECK_EQ(raw(s, PG106_BR_OP_VM_BIND, &f, sizeof(f)), -EINVAL);
		f.op.addr = 0x100000;
		f.op.flags = 0xff; // invalid PTE kind
		CHECK_EQ(raw(s, PG106_BR_OP_VM_BIND, &f, sizeof(f)), -EINVAL);
		f.op.flags = 0;
		f.h.op_ptr = 0x1234; // user pointers are not allowed on this wire
		CHECK_EQ(raw(s, PG106_BR_OP_VM_BIND, &f, sizeof(f)), -EPROTO);
		f.h.op_ptr = 0;
		CHECK_EQ(raw(s, PG106_BR_OP_VM_BIND, &f, sizeof(f) - 8), -EPROTO);
		CHECK_EQ(raw(s, PG106_BR_OP_VM_BIND, &f, sizeof(f)), 0);
		// Free while bound: refused by the kernel too.
		{
			struct pg106_br_bo_free bf = { h, 0 };

			CHECK_EQ(raw(s, PG106_BR_OP_BO_FREE, &bf, sizeof(bf)), -EBUSY);
		}
		// Sparse region rules.
		f.op.flags = PG106_UAPI_VM_BIND_SPARSE;
		f.op.handle = 0;
		f.op.addr = 0x200000;
		f.op.range = 0x10000;
		CHECK_EQ(raw(s, PG106_BR_OP_VM_BIND, &f, sizeof(f)), 0);
		CHECK_EQ(raw(s, PG106_BR_OP_VM_BIND, &f, sizeof(f)), -ENOSPC);
		f.op.flags = 0;
		f.op.handle = h;
		f.op.addr = 0x20f000;
		f.op.range = 0x2000; // straddles the region's end
		CHECK_EQ(raw(s, PG106_BR_OP_VM_BIND, &f, sizeof(f)), -ENOSPC);
		f.op.op = PG106_UAPI_VM_BIND_OP_UNMAP;
		f.op.flags = PG106_UAPI_VM_BIND_SPARSE;
		f.op.handle = 0;
		f.op.addr = 0x200000;
		f.op.range = 0x8000; // must match exactly
		CHECK_EQ(raw(s, PG106_BR_OP_VM_BIND, &f, sizeof(f)), -ENOENT);
		// UNMAP of the mapped page releases the BO.
		f.op.flags = 0;
		f.op.addr = 0x100000;
		f.op.range = 0x1000;
		CHECK_EQ(raw(s, PG106_BR_OP_VM_BIND, &f, sizeof(f)), 0);
		{
			struct pg106_pte_query q;

			CHECK(!pg106_mmu_walk(pg106_kd_mmu(s->kd), 0x100000, &q));
		}
	}

	// EXEC, kernel-side rules.
	{
		struct {
			struct pg106_uapi_exec h;
			struct pg106_uapi_exec_push p;
			struct pg106_uapi_sync w;
		} e;

		memset(&e, 0, sizeof(e));
		e.h.channel = 1;
		e.h.push_count = 1;
		e.p.va = 0x100000;
		e.p.va_len = 16;
		CHECK_EQ(raw(s, PG106_BR_OP_EXEC, &e, sizeof(e) - sizeof(e.w)), -ENOENT);
		e.h.channel = 0;
		e.p.va_len = 6; // not dword-aligned
		CHECK_EQ(raw(s, PG106_BR_OP_EXEC, &e, sizeof(e) - sizeof(e.w)), -EINVAL);
		e.p.va_len = 16;
		e.h.wait_count = 1;
		e.w.handle = 0x42; // no such syncobj
		CHECK_EQ(raw(s, PG106_BR_OP_EXEC, &e, sizeof(e)), -ENOENT);
		e.h.push_count = IB_MAX; // counts that do not match the payload
		CHECK_EQ(raw(s, PG106_BR_OP_EXEC, &e, sizeof(e)), -EPROTO);
		{
			// More pushes than EXEC_PUSH_MAX (IB_MAX/2 - 1), well formed.
			static uint8_t big[sizeof(struct pg106_uapi_exec) +
					  IB_MAX * sizeof(struct pg106_uapi_exec_push)];
			struct pg106_uapi_exec ex = { 0, IB_MAX, 0, 0, 0, 0, 0 };
			uint32_t k;

			memcpy(big, &ex, sizeof(ex));
			for (k = 0; k < IB_MAX; k++) {
				struct pg106_uapi_exec_push pp = { 0x100000, 16, 0 };

				memcpy(big + sizeof(ex) + k * sizeof(pp), &pp, sizeof(pp));
			}
			CHECK_EQ(raw(s, PG106_BR_OP_EXEC, big, sizeof(big)), -EINVAL);
		}
	}

	// Bring-up not finished (or GPU powered off): only HANDSHAKE and
	// GETPARAM are served.
	s->ready = false;
	{
		struct pg106_uapi_getparam gp = { PG106_UAPI_GETPARAM_PCI_VENDOR, 0 };
		struct pg106_br_syncobj so = { 0, 0 };

		CHECK_EQ(raw(s, PG106_BR_OP_GETPARAM, &gp, sizeof(gp)), 0);
		CHECK_EQ(raw(s, PG106_BR_OP_SYNCOBJ_CREATE, &so, sizeof(so)), -ENODEV);
		gp.param = PG106_BR_GETPARAM_READY;
		CHECK_EQ(raw(s, PG106_BR_OP_GETPARAM, &gp, sizeof(gp)), 0);
	}
	s->ready = true;
	pg106_kd_destroy(s->kd);
	CHECK_EQ(s->allocs, s->frees);
	free(s);
	printf("PASS kernel-side validation: framing, VM_INIT, BO, VM_BIND "
	       "(handle, size, kernel region, alignment, kind, sparse), EXEC, "
	       "readiness gate\n");

	// Allocation failure inside VM_INIT leaves nothing behind.
	s = sys_new();
	env = env_of(s);
	CHECK_EQ(pg106_kd_create(&env, &s->kd), 0);
	s->fail_alloc_after = 1; // ring ok, fence page fails
	CHECK_EQ(raw(s, PG106_BR_OP_VM_INIT, &vi, sizeof(vi)), -ENOMEM);
	CHECK_EQ(s->allocs, s->frees);
	CHECK(!s->chan_bound);
	s->fail_alloc_after = -1;
	CHECK_EQ(raw(s, PG106_BR_OP_VM_INIT, &vi, sizeof(vi)), 0);
	pg106_kd_destroy(s->kd);
	CHECK_EQ(s->allocs, s->frees);
	{
		struct pg106_kd_env bad = env_of(s);
		struct pg106_kd *kd;

		bad.ib_max = 48; // not a power of two
		CHECK_EQ(pg106_kd_create(&bad, &kd), -EINVAL);
	}
	free(s);
	// No hardware channel (-ENOSYS): VM works, EXEC is -ENODEV. A
	// channel owned by another client (-EBUSY) fails VM_INIT.
	s = sys_new();
	env = env_of(s);
	CHECK_EQ(pg106_kd_create(&env, &s->kd), 0);
	s->chan_rc = -EBUSY;
	CHECK_EQ(raw(s, PG106_BR_OP_VM_INIT, &vi, sizeof(vi)), -EBUSY);
	s->chan_rc = -ENOSYS;
	CHECK_EQ(raw(s, PG106_BR_OP_VM_INIT, &vi, sizeof(vi)), 0);
	{
		struct pg106_uapi_exec ex = { 0 };

		CHECK_EQ(raw(s, PG106_BR_OP_EXEC, &ex, sizeof(ex)), -ENODEV);
	}
	pg106_kd_destroy(s->kd);
	CHECK_EQ(s->allocs, s->frees);
	free(s);
	printf("PASS VM_INIT allocation failure unwinds; bad env refused; "
	       "no-channel (-ENOSYS) and busy-channel (-EBUSY) binds\n");
}

int
main(void)
{
	printf("== kdisp: SIMULATION — bridge -> loopback -> kernel dispatcher -> mock GPU ==\n");
	test_end_to_end();
	test_kernel_validation();
	if (g_fail) {
		printf("%d failure(s)\n", g_fail);
		return 1;
	}
	printf("PASS test_kdisp\n");
	return 0;
}
