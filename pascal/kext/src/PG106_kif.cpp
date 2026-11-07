// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/src/PG106_kif.cpp — PG106's side of pg106_kif.h (ABI 1.0,
// agreed with PG106Accel and PG106FB 2026-11-08; MIT header in
// pascal/bridge/include): VRAM allocations, the accelerator VM, a
// copy-engine channel, COPY / RELEASE / FENCE, and PROGRESS / LOST events.
//
// Only the MIT header crosses the licence boundary; PG106Accel reaches
// this through IOService::callPlatformFunction (PG106.cpp).
//
// The accelerator VM is a pg106_vmm (tables in VRAM). VA_ALLOC hands out
// [KIF_VA_BASE, KIF_VA_BASE + KIF_VA_BYTES); PG106's own objects for each
// channel (GPFIFO ring, push slots, fence word) are mapped below
// KIF_VA_BASE. A channel lives on the copy engine's runlist (CE2 on the
// Helios: runlist 5), which needs no engine context. Each COPY / RELEASE
// is one push in its own slot:
//   COPY     PASCAL_DMA_COPY_B 2D pitch copy (pascal/ce.h), then
//   RELEASE  SEMAPHOREA-D 4-byte release of `value` to gpu_va (WFI), then
//   both end with a host release of the channel's seqno to its fence word
//   (WFI: after the engine is idle) and NON_STALL_INTERRUPT, which wakes
//   FENCE WAIT and sends PROGRESS (PG106::sNonstall -> kifEvent).
//
// SIMULATION status: compiles and links (cross-build); the FIFO, VMM and
// CE encoders it uses are tested on Linux (test_fifo, test_gpu); never run
// on a Mac or a GPU.

#include "PG106.hpp"

#include <IOKit/IOLib.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <sys/errno.h>

#include <pascal/ce.h>
#include <pascal/pushbuf.h>

#include "pg106_kif.h"
#include "pg106_klibc.h"

#define KIF_VA_BASE   0x0000001000000000ull // 64 GiB; + epoch * window
#define KIF_VA_BYTES  0x0000001000000000ull // 64 GiB window
#define KIF_KVA_BASE  0x0000000100000000ull // PG106's objects
#define KIF_MAX_VRAM  128u
#define KIF_MAX_MAP   256u
#define KIF_MAX_CHAN  4u
#define KIF_RING_N    512u                  // GPFIFO entries
#define KIF_SLOTS     64u                   // push slots
#define KIF_SLOT_SZ   0x400u                // bytes per push slot
#define KIF_CHAN_SZ   0x20000ull            // ring 4K + fence 4K + slots 64K
#define KIF_OFF_RING  0x00000u
#define KIF_OFF_FENCE 0x01000u
#define KIF_OFF_PUSH  0x10000u

struct kif_vram {
	bool used;
	uint64_t handle, pa, size, bar1_off, gpu_va, cookie;
	bool cpu, gva;
};

struct kif_map {
	bool used;
	uint64_t va, size;
	struct pg106_dma *dma; // MAP_SYSMEM
};

struct kif_chan {
	bool used;
	uint64_t id;
	struct pg106_chan *hw;
	IOService *client;
	uint64_t tag;
	uint64_t mem_pa, mem_va; // KIF_CHAN_SZ of VRAM, mapped at mem_va
	uint32_t put;
	uint64_t emitted, completed_seen;
	bool session;
};

struct pg106_kif_state {
	bool vm_up;
	struct pg106_vmm vm;
	struct pg106_range va;    // VA_ALLOC window
	struct pg106_range kva;   // PG106's objects
	struct kif_vram vram[KIF_MAX_VRAM];
	struct kif_map map[KIF_MAX_MAP];
	struct kif_chan chan[KIF_MAX_CHAN];
	uint64_t next_handle, next_cookie;
	uint32_t generation;
	uint64_t va_base; // this epoch's VA_ALLOC window
};

// Each epoch (bumped by LOST) gets its own 64 GiB VA window, so a late
// VA_FREE / UNMAP of an old-epoch address can never hit a range that was
// re-allocated after the VM was rebuilt: it falls outside the current
// window and is a no-op. 256 windows fit below 2^49.
static uint64_t
epochBase(const struct pg106_kif_state *k)
{
	return KIF_VA_BASE + (uint64_t)(k->generation & 0xffu) * KIF_VA_BYTES;
}

// ---- helpers ---------------------------------------------------------------------

static bool
kifVm(PG106 *p, struct pg106_kif_state *k)
{
	struct pg106_gpu *g = p->gpu();

	if (k->vm_up)
		return true;
	if (!g || !p->memUp())
		return false;
	if (pg106_vmm_init(&k->vm, &g->vram, PG106_MMU_VA_LIMIT, 0x400000, false))
		return false;
	k->va_base = epochBase(k);
	if (pg106_range_init(&k->va, k->va_base, KIF_VA_BYTES, 0x1000)) {
		pg106_vmm_fini(&k->vm);
		return false;
	}
	if (pg106_range_init(&k->kva, KIF_KVA_BASE, 0x40000000ull, 0x10000)) {
		pg106_range_fini(&k->va);
		pg106_vmm_fini(&k->vm);
		return false;
	}
	k->vm_up = true;
	return true;
}

static int
kifMapRange(struct pg106_kif_state *k, uint64_t va, uint64_t pa, uint64_t size,
	    enum pg106_target target, bool ro)
{
	struct pg106_map m;

	memset(&m, 0, sizeof(m));
	m.va = va;
	m.pa = pa;
	m.size = size;
	m.page_shift = PG106_PAGE_4K_SHIFT;
	m.target = target;
	m.kind = PG106_KIND_PITCH;
	m.vol = target == PG106_TARGET_SYS_COHERENT;
	m.ro = ro;
	return pg106_vmm_map(&k->vm, &m);
}

static struct kif_vram *
findVram(struct pg106_kif_state *k, uint64_t h)
{
	for (unsigned i = 0; i < KIF_MAX_VRAM; i++)
		if (k->vram[i].used && k->vram[i].handle == h)
			return &k->vram[i];
	return nullptr;
}

static struct kif_chan *
findChan(struct pg106_kif_state *k, uint64_t id)
{
	for (unsigned i = 0; i < KIF_MAX_CHAN; i++)
		if (k->chan[i].used && k->chan[i].id == id)
			return &k->chan[i];
	return nullptr;
}

static IOReturn
ioret(int e)
{
	switch (e) {
	case 0: return kIOReturnSuccess;
	case -ENODEV: return kIOReturnNotReady;
	case -ENOSPC:
	case -ENOMEM: return kIOReturnNoResources;
	case -ETIMEDOUT: return kIOReturnTimeout;
	case -EBUSY: return kIOReturnBusy;
	default: return kIOReturnBadArgument;
	}
}

// ---- channel submission ---------------------------------------------------------------

static uint32_t
chanCompleted(PG106 *p, struct kif_chan *c)
{
	return p->vramRd32(c->mem_pa + KIF_OFF_FENCE);
}

// Append the fence release + non-stall to `pb` and submit it.
static int
chanSubmit(PG106 *p, struct kif_chan *c, pg106_pb *pb, uint64_t *seqno)
{
	const uint64_t fva = c->mem_va + KIF_OFF_FENCE;
	uint32_t slot, d[5];
	uint64_t e;
	unsigned spins = 0;

	// Free push slot: at most KIF_SLOTS submissions in flight.
	while ((uint32_t)(c->emitted - chanCompleted(p, c)) >= KIF_SLOTS - 1) {
		if (++spins > 200)
			return -EBUSY;
		p->fenceWait(5000);
	}
	c->emitted++;
	// nvc0_fence_emit32 shape (nvc0_fence.c:35-55): 4-byte release with
	// WFI (SEMAPHORED RELEASE 2 | RELEASE_SIZE_4BYTE bit 24), then
	// NON_STALL_INTERRUPT.
	d[0] = (uint32_t)(fva >> 32) & 0xffu;
	d[1] = (uint32_t)fva;
	d[2] = (uint32_t)c->emitted;
	d[3] = 0x01000002u;
	d[4] = 0;
	pg106_pb_inc(pb, 0, 0x0010, d, 5);
	if (pb->error || pb->len * 4 > KIF_SLOT_SZ) {
		c->emitted--;
		return -EINVAL;
	}
	slot = (uint32_t)(c->emitted % KIF_SLOTS);
	p->vramWrite(c->mem_pa + KIF_OFF_PUSH + slot * KIF_SLOT_SZ, pb->words,
		     (uint32_t)(pb->len * 4));
	e = pg106_gpfifo_pack(c->mem_va + KIF_OFF_PUSH + slot * KIF_SLOT_SZ,
			      (uint32_t)pb->len, PG106_GP_LEVEL_MAIN, 0, 0);
	p->vramWrite(c->mem_pa + KIF_OFF_RING + (c->put % KIF_RING_N) * 8ull, &e, 8);
	pg106_bar_flush(p->gpu()->vram.bus);
	c->put = (c->put + 1) % KIF_RING_N;
	pg106_chan_kick(c->hw, c->put);
	*seqno = c->emitted;
	return 0;
}

// ---- the functions -----------------------------------------------------------------------

static IOReturn
kifInfo(PG106 *p, struct pg106_kif_state *k, struct pg106_kif_info *r)
{
	struct pg106_gpu *g = p->gpu();
	bool mem = p->memUp(), fifo = g && g->fifo_up;
	bool ce = fifo && pg106_fifo_runl_find(&g->fifo, PG106_TOP_TYPE_CE_N) >= 0;

	r->caps = 0;
	if (mem)
		r->caps |= PG106_KIF_CAP_VRAM | PG106_KIF_CAP_BAR1 | PG106_KIF_CAP_VM;
	if (mem && ce)
		r->caps |= PG106_KIF_CAP_COPY | PG106_KIF_CAP_RELEASE;
	r->ready = p->gpuReady() ? 1 : 0;
	r->vram_bytes = g ? g->vram_size : 0;
	r->vram_avail_bytes = mem ? g->vram.mm.free_bytes : 0;
	r->bar1_phys = p->bar1Phys();
	r->bar1_bytes = p->bar1Len();
	r->bar1_avail_bytes = mem ? g->bar1.va.free_bytes : 0;
	r->va_base = k->vm_up ? k->va_base : epochBase(k);
	r->va_bytes = k->vm_up ? k->va.free_bytes : KIF_VA_BYTES;
	r->page_bytes = 4096;
	return kIOReturnSuccess;
}

static IOReturn
kifVramAlloc(PG106 *p, struct pg106_kif_state *k, struct pg106_kif_vram *r)
{
	struct kif_vram *v = nullptr;
	uint64_t align = r->align ? r->align : 0x1000, size;
	int ret;

	if (!p->memUp())
		return kIOReturnNotReady;
	if (!r->bytes || (align & (align - 1)))
		return kIOReturnBadArgument;
	for (unsigned i = 0; i < KIF_MAX_VRAM; i++)
		if (!k->vram[i].used) {
			v = &k->vram[i];
			break;
		}
	if (!v)
		return kIOReturnNoResources;
	if (align < 0x1000)
		align = 0x1000;
	if ((r->flags & PG106_KIF_VRAM_SCANOUT) && align < 0x10000)
		align = 0x10000;
	size = (r->bytes + 0xfff) & ~0xfffull;
	memset(v, 0, sizeof(*v));
	// Every allocation is physically contiguous (one range of the VRAM
	// allocator), so CONTIG / SCANOUT need nothing extra.
	ret = p->vramAlloc(size, align, PG106_VRAM_ZERO, &v->pa);
	if (ret)
		return ioret(ret);
	v->size = size;
	if (r->flags & PG106_KIF_VRAM_CPU) {
		ret = p->bar1Map(v->pa, size, &v->bar1_off);
		if (ret) {
			p->vramFree(v->pa, size);
			return ioret(ret);
		}
		v->cpu = true;
	}
	if (r->flags & PG106_KIF_VRAM_GPU_VA) {
		if (!kifVm(p, k) ||
		    pg106_range_alloc(&k->va, size, align, false, &v->gpu_va) ||
		    kifMapRange(k, v->gpu_va, v->pa, size, PG106_TARGET_VRAM, false) ||
		    pg106_vmm_commit(&k->vm)) {
			if (v->gpu_va && k->vm_up) {
				pg106_vmm_unmap(&k->vm, v->gpu_va, size, 12);
				pg106_range_free(&k->va, v->gpu_va, size);
			}
			if (v->cpu)
				p->bar1Unmap(v->bar1_off, size);
			p->vramFree(v->pa, size);
			return kIOReturnNoResources;
		}
		v->gva = true;
	}
	v->used = true;
	v->handle = ++k->next_handle;
	v->cookie = ++k->next_cookie;
	r->handle = v->handle;
	r->vram_offset = v->pa;
	r->actual_bytes = size;
	r->bar1_phys = v->cpu ? p->bar1Phys() + v->bar1_off : 0;
	r->kva = v->cpu ? (uint64_t)(uintptr_t)(p->bar1Kva() + v->bar1_off) : 0;
	r->gpu_va = v->gva ? v->gpu_va : 0;
	r->cookie = v->cookie;
	return kIOReturnSuccess;
}

static void
vramRelease(PG106 *p, struct pg106_kif_state *k, struct kif_vram *v, bool hw)
{
	if (hw) {
		if (v->gva && k->vm_up) {
			pg106_vmm_unmap(&k->vm, v->gpu_va, v->size, 12);
			pg106_vmm_commit(&k->vm);
			pg106_range_free(&k->va, v->gpu_va, v->size);
		}
		if (v->cpu)
			p->bar1Unmap(v->bar1_off, v->size);
		p->vramFree(v->pa, v->size);
	}
	memset(v, 0, sizeof(*v));
}

static IOReturn
kifVramFree(PG106 *p, struct pg106_kif_state *k, struct pg106_kif_vram *r)
{
	struct kif_vram *v = findVram(k, r->handle);

	if (v)
		vramRelease(p, k, v, true);
	return kIOReturnSuccess; // dead handles after LOST are accepted
}

static IOReturn
kifVa(PG106 *p, struct pg106_kif_state *k, struct pg106_kif_va *r, bool alloc)
{
	uint64_t align = r->align ? r->align : 0x1000;

	if (!alloc && !k->vm_up)
		return kIOReturnSuccess; // after LOST: a harmless no-op
	if (!kifVm(p, k))
		return kIOReturnNotReady;
	if (alloc) {
		if (!r->bytes || (align & (align - 1)))
			return kIOReturnBadArgument;
		return pg106_range_alloc(&k->va, r->bytes, align < 0x1000 ? 0x1000 : align,
					 false, &r->gpu_va)
			       ? kIOReturnNoResources : kIOReturnSuccess;
	}
	// A range that is not (or no longer) allocated, or of an older epoch
	// (outside this window): no-op, as after LOST.
	if (pg106_range_is_allocated(&k->va, r->gpu_va, r->bytes))
		pg106_range_free(&k->va, r->gpu_va, r->bytes);
	return kIOReturnSuccess;
}

static IOReturn
kifMap(PG106 *p, struct pg106_kif_state *k, struct pg106_kif_map *r, bool map)
{
	struct kif_map *m = nullptr;
	const bool ro = r->flags & PG106_KIF_MAP_READONLY;
	int ret = 0;

	if (!map && !k->vm_up)
		return kIOReturnSuccess; // after LOST: a harmless no-op
	if (!kifVm(p, k))
		return kIOReturnNotReady;
	if (!r->bytes || (r->gpu_va | r->bytes) & 0xfff)
		return kIOReturnBadArgument;
	if (!map) {
		for (unsigned i = 0; i < KIF_MAX_MAP; i++)
			if (k->map[i].used && k->map[i].va == r->gpu_va &&
			    k->map[i].size == r->bytes) {
				pg106_vmm_unmap(&k->vm, r->gpu_va, r->bytes, 12);
				pg106_vmm_commit(&k->vm);
				if (k->map[i].dma)
					pg106_dma_free(k->map[i].dma);
				memset(&k->map[i], 0, sizeof(k->map[i]));
				return kIOReturnSuccess;
			}
		return kIOReturnSuccess; // unknown mapping: no-op
	}
	if (!pg106_range_is_allocated(&k->va, r->gpu_va, r->bytes))
		return kIOReturnBadArgument; // outside a VA_ALLOC range
	for (unsigned i = 0; i < KIF_MAX_MAP; i++)
		if (!k->map[i].used) {
			m = &k->map[i];
			break;
		}
	if (!m)
		return kIOReturnNoResources;
	memset(m, 0, sizeof(*m));
	if (r->kind == PG106_KIF_MAP_VRAM) {
		struct kif_vram *v = findVram(k, r->vram_handle);

		if (!v || r->offset & 0xfff || r->offset + r->bytes > v->size)
			return kIOReturnBadArgument;
		ret = kifMapRange(k, r->gpu_va, v->pa + r->offset, r->bytes,
				  PG106_TARGET_VRAM, ro);
	} else if (r->kind == PG106_KIF_MAP_SYSMEM) {
		uint64_t off;

		if (!r->md || r->offset & 0xfff)
			return kIOReturnBadArgument;
		ret = pg106_dma_wrap(static_cast<IOMemoryDescriptor *>(r->md), &m->dma);
		if (ret)
			return ioret(ret);
		if (r->offset + r->bytes > m->dma->size) {
			pg106_dma_free(m->dma);
			return kIOReturnBadArgument;
		}
		for (off = 0; off < r->bytes && !ret; off += 0x1000) {
			uint64_t bus = 0;

			ret = pg106_dma_addr(m->dma, r->offset + off, &bus, nullptr);
			if (!ret)
				ret = kifMapRange(k, r->gpu_va + off, bus & ~0xfffull, 0x1000,
						  PG106_TARGET_SYS_COHERENT, ro);
		}
		if (ret) {
			pg106_vmm_unmap(&k->vm, r->gpu_va, r->bytes, 12);
			pg106_dma_free(m->dma);
			m->dma = nullptr;
		}
	} else {
		return kIOReturnBadArgument;
	}
	if (!ret)
		ret = pg106_vmm_commit(&k->vm); // sync + TLB invalidate
	if (ret) {
		pg106_vmm_unmap(&k->vm, r->gpu_va, r->bytes, 12);
		pg106_vmm_commit(&k->vm);
		if (m->dma)
			pg106_dma_free(m->dma);
		memset(m, 0, sizeof(*m));
		return ioret(ret);
	}
	m->used = true;
	m->va = r->gpu_va;
	m->size = r->bytes;
	return kIOReturnSuccess;
}

static void
chanRelease(PG106 *p, struct pg106_kif_state *k, struct kif_chan *c, bool hw)
{
	if (hw && c->hw)
		pg106_chan_destroy(c->hw);
	if (hw && c->mem_pa) {
		pg106_vmm_unmap(&k->vm, c->mem_va, KIF_CHAN_SZ, 12);
		pg106_vmm_commit(&k->vm);
		pg106_range_free(&k->kva, c->mem_va, KIF_CHAN_SZ);
		p->vramFree(c->mem_pa, KIF_CHAN_SZ);
	}
	if (c->session)
		p->closeSession();
	OSSafeReleaseNULL(c->client);
	memset(c, 0, sizeof(*c));
}

static IOReturn
kifChanOpen(PG106 *p, struct pg106_kif_state *k, struct pg106_kif_chan *r)
{
	struct pg106_gpu *g;
	struct kif_chan *c = nullptr;
	int runl, ret;

	if (r->kind != PG106_KIF_CHAN_COPY || !r->client)
		return kIOReturnBadArgument;
	for (unsigned i = 0; i < KIF_MAX_CHAN; i++)
		if (!k->chan[i].used) {
			c = &k->chan[i];
			break;
		}
	if (!c)
		return kIOReturnNoResources;
	memset(c, 0, sizeof(*c));
	// Keep the GPU booted while the channel is open (pg106_kif.h).
	if (p->openSession())
		return kIOReturnNotReady;
	c->session = true;
	g = p->gpu();
	if (!g || !g->fifo_up || !kifVm(p, k)) {
		chanRelease(p, k, c, false);
		return kIOReturnNotReady;
	}
	runl = pg106_fifo_runl_find(&g->fifo, PG106_TOP_TYPE_CE_N);
	if (runl < 0) {
		chanRelease(p, k, c, false);
		return kIOReturnUnsupported;
	}
	ret = p->vramAlloc(KIF_CHAN_SZ, 0x10000, PG106_VRAM_ZERO, &c->mem_pa);
	if (!ret)
		ret = pg106_range_alloc(&k->kva, KIF_CHAN_SZ, 0x10000, false, &c->mem_va);
	if (!ret)
		ret = kifMapRange(k, c->mem_va, c->mem_pa, KIF_CHAN_SZ,
				  PG106_TARGET_VRAM, false);
	if (!ret)
		ret = pg106_vmm_commit(&k->vm);
	if (!ret)
		ret = pg106_chan_create(&g->fifo, &k->vm, runl, c->mem_va + KIF_OFF_RING,
					KIF_RING_N, &c->hw);
	if (ret) {
		chanRelease(p, k, c, true);
		return ioret(ret);
	}
	c->used = true;
	c->id = ++k->next_handle;
	c->client = static_cast<IOService *>(r->client);
	c->client->retain();
	c->tag = r->client_tag;
	r->chan = c->id;
	IOLog("PG106: kif: copy channel %llu open (chid %u, runlist %d)\n",
	      (unsigned long long)c->id, c->hw->chid, g->fifo.runl[runl].id);
	return kIOReturnSuccess;
}

static IOReturn
kifCopy(PG106 *p, struct pg106_kif_state *k, struct pg106_kif_copy *r)
{
	struct kif_chan *c = findChan(k, r->chan);
	pg106_ce_memcpy2d_plan pl;
	uint32_t w[KIF_SLOT_SZ / 4];
	pg106_pb pb;

	if (!c || !c->hw)
		return kIOReturnNotReady;
	if (!r->width_bytes || !r->height || r->flags)
		return kIOReturnBadArgument;
	memset(&pl, 0, sizeof(pl));
	pl.src_addr = r->src_va;
	pl.dst_addr = r->dst_va;
	pl.pitch_in = r->src_pitch ? r->src_pitch : r->width_bytes;
	pl.pitch_out = r->dst_pitch ? r->dst_pitch : r->width_bytes;
	pl.line_length = r->width_bytes;
	pl.line_count = r->height;
	pl.subch = 4;
	pl.transfer_type = PG106_CE_XFER_NON_PIPELINED;
	pl.flush_enable = PG106_NVC1B5_LAUNCH_DMA_FLUSH_ENABLE_TRUE;
	pl.multi_line = r->height > 1 ? PG106_NVC1B5_LAUNCH_DMA_MULTI_LINE_ENABLE_TRUE : 0;
	pg106_pb_init(&pb, w, KIF_SLOT_SZ / 4);
	if (pg106_ce_memcpy2d(&pb, &pl) || pb.error)
		return kIOReturnBadArgument;
	return ioret(chanSubmit(p, c, &pb, &r->seqno));
}

static IOReturn
kifRelease(PG106 *p, struct pg106_kif_state *k, struct pg106_kif_release *r)
{
	struct kif_chan *c = findChan(k, r->chan);
	uint32_t w[KIF_SLOT_SZ / 4], d[4];
	pg106_pb pb;

	if (!c || !c->hw)
		return kIOReturnNotReady;
	if (r->gpu_va & 3)
		return kIOReturnBadArgument;
	pg106_pb_init(&pb, w, KIF_SLOT_SZ / 4);
	d[0] = (uint32_t)(r->gpu_va >> 32) & 0xffu;
	d[1] = (uint32_t)r->gpu_va;
	d[2] = r->value;
	d[3] = 0x01000002u; // RELEASE, 4 bytes, WFI
	pg106_pb_inc(&pb, 0, 0x0010, d, 4);
	return ioret(chanSubmit(p, c, &pb, &r->seqno));
}

static IOReturn
kifFence(PG106 *p, struct pg106_kif_state *k, struct pg106_kif_fence *r)
{
	struct kif_chan *c = findChan(k, r->chan);
	uint64_t waited = 0;

	if (!c || !c->hw)
		return kIOReturnNotReady;
	for (;;) {
		// 32-bit fence word, extended against what was emitted.
		uint32_t lo = chanCompleted(p, c);
		uint64_t done = (c->emitted & ~0xffffffffull) | lo;

		if (done > c->emitted)
			done -= 0x100000000ull;
		r->completed = done;
		if (r->op == PG106_KIF_FENCE_QUERY || done >= r->seqno)
			return kIOReturnSuccess;
		if (r->op != PG106_KIF_FENCE_WAIT)
			return kIOReturnBadArgument;
		if (waited >= r->timeout_us)
			return kIOReturnTimeout;
		p->fenceWait(r->timeout_us - waited < 10000 ? r->timeout_us - waited
							   : 10000);
		waited += 10000;
	}
}

// ---- dispatch + events -----------------------------------------------------------------

struct kif_req {
	const char *name;
	void *req;
	IOReturn ret;
};

static IOReturn
kifGated(OSObject *owner, void *a0, void *, void *, void *)
{
	PG106 *p = OSDynamicCast(PG106, owner);
	struct kif_req *q = static_cast<struct kif_req *>(a0);
	const char *n = q->name + 24; // after "org.helios300.pg106.kif."
	struct pg106_kif_state *k;

	if (!p)
		return kIOReturnBadArgument;
	if (!p->fKif) {
		p->fKif = static_cast<struct pg106_kif_state *>(
			pg106_kcalloc(1, sizeof(*p->fKif)));
		if (!p->fKif)
			return q->ret = kIOReturnNoMemory;
	}
	k = p->fKif;
	if (!strcmp(n, "info"))
		q->ret = kifInfo(p, k, static_cast<struct pg106_kif_info *>(q->req));
	else if (!strcmp(n, "vram_alloc"))
		q->ret = kifVramAlloc(p, k, static_cast<struct pg106_kif_vram *>(q->req));
	else if (!strcmp(n, "vram_free"))
		q->ret = kifVramFree(p, k, static_cast<struct pg106_kif_vram *>(q->req));
	else if (!strcmp(n, "va_alloc"))
		q->ret = kifVa(p, k, static_cast<struct pg106_kif_va *>(q->req), true);
	else if (!strcmp(n, "va_free"))
		q->ret = kifVa(p, k, static_cast<struct pg106_kif_va *>(q->req), false);
	else if (!strcmp(n, "map"))
		q->ret = kifMap(p, k, static_cast<struct pg106_kif_map *>(q->req), true);
	else if (!strcmp(n, "unmap"))
		q->ret = kifMap(p, k, static_cast<struct pg106_kif_map *>(q->req), false);
	else if (!strcmp(n, "chan_open"))
		q->ret = kifChanOpen(p, k, static_cast<struct pg106_kif_chan *>(q->req));
	else if (!strcmp(n, "chan_close")) {
		struct kif_chan *c =
			findChan(k, static_cast<struct pg106_kif_chan *>(q->req)->chan);

		if (c)
			chanRelease(p, k, c, true);
		q->ret = kIOReturnSuccess;
	} else if (!strcmp(n, "copy"))
		q->ret = kifCopy(p, k, static_cast<struct pg106_kif_copy *>(q->req));
	else if (!strcmp(n, "release"))
		q->ret = kifRelease(p, k, static_cast<struct pg106_kif_release *>(q->req));
	else if (!strcmp(n, "fence"))
		q->ret = kifFence(p, k, static_cast<struct pg106_kif_fence *>(q->req));
	else
		q->ret = kIOReturnUnsupported;
	return kIOReturnSuccess;
}

static uint32_t
kifReqSize(const char *n)
{
	n += 24;
	if (!strcmp(n, "info")) return sizeof(struct pg106_kif_info);
	if (!strcmp(n, "vram_alloc") || !strcmp(n, "vram_free"))
		return sizeof(struct pg106_kif_vram);
	if (!strcmp(n, "va_alloc") || !strcmp(n, "va_free"))
		return sizeof(struct pg106_kif_va);
	if (!strcmp(n, "map") || !strcmp(n, "unmap"))
		return sizeof(struct pg106_kif_map);
	if (!strcmp(n, "chan_open") || !strcmp(n, "chan_close"))
		return sizeof(struct pg106_kif_chan);
	if (!strcmp(n, "copy")) return sizeof(struct pg106_kif_copy);
	if (!strcmp(n, "release")) return sizeof(struct pg106_kif_release);
	if (!strcmp(n, "fence")) return sizeof(struct pg106_kif_fence);
	return 0;
}

IOReturn
PG106::kifCall(const char *name, void *req, uintptr_t abi)
{
	uint32_t *hdr = static_cast<uint32_t *>(req);
	uint32_t need = kifReqSize(name);
	struct kif_req q = { name, req, kIOReturnSuccess };

	// pg106_kif.h versioning: same major, caller minor <= ours.
	if (!req || !PG106_KIF_ABI_ACCEPTS(PG106_KIF_ABI, (uint32_t)abi) ||
	    !PG106_KIF_ABI_ACCEPTS(PG106_KIF_ABI, hdr[0]))
		return kIOReturnUnsupported;
	if (!need)
		return kIOReturnUnsupported;
	if (hdr[1] < need) // ABI 1.0 has no shorter request forms
		return kIOReturnBadArgument;
	hdr[0] = PG106_KIF_ABI;
	if (runOnGate(kifGated, &q) != kIOReturnSuccess)
		return kIOReturnNotReady;
	return q.ret;
}

static void
sendEvent(struct kif_chan *c, uint32_t event, uint64_t completed)
{
	struct pg106accel_kif_event ev;
	const OSSymbol *fn = OSSymbol::withCStringNoCopy(PG106ACCEL_KIF_FN_EVENT);

	if (!fn || !c->client)
		return;
	memset(&ev, 0, sizeof(ev));
	ev.abi = PG106_KIF_ABI;
	ev.size = sizeof(ev);
	ev.event = event;
	ev.client_tag = c->tag;
	ev.chan = c->id;
	ev.completed = completed;
	c->client->callPlatformFunction(fn, false, &ev,
					(void *)(uintptr_t)PG106_KIF_ABI, nullptr,
					nullptr);
	fn->release();
}

void
PG106::kifEvent(void)
{
	struct pg106_kif_state *k = fKif;

	if (!k)
		return;
	for (unsigned i = 0; i < KIF_MAX_CHAN; i++) {
		struct kif_chan *c = &k->chan[i];
		uint32_t lo;

		if (!c->used || !c->hw)
			continue;
		lo = chanCompleted(this, c);
		if (lo != (uint32_t)c->completed_seen) {
			c->completed_seen = (c->emitted & ~0xffffffffull) | lo;
			if (c->completed_seen > c->emitted)
				c->completed_seen -= 0x100000000ull;
			sendEvent(c, PG106_KIF_EVENT_PROGRESS, c->completed_seen);
		}
	}
}

// The GPU goes away (power-off, re-boot, stop): LOST once per channel,
// every handle dead. Hardware objects are dropped without touching the
// GPU (pg106_gpu_fini frees VRAM and the FIFO wholesale).
void
PG106::kifLost(void)
{
	struct pg106_kif_state *k = fKif;

	if (!k)
		return;
	for (unsigned i = 0; i < KIF_MAX_CHAN; i++) {
		struct kif_chan *c = &k->chan[i];

		if (!c->used)
			continue;
		sendEvent(c, PG106_KIF_EVENT_LOST, c->completed_seen);
		if (c->hw)
			pg106_chan_destroy(c->hw); // FIFO bookkeeping only if powered
		c->hw = nullptr;
		c->mem_pa = 0;
		chanRelease(this, k, c, false);
	}
	for (unsigned i = 0; i < KIF_MAX_MAP; i++)
		if (k->map[i].used) {
			if (k->map[i].dma)
				pg106_dma_free(k->map[i].dma);
			memset(&k->map[i], 0, sizeof(k->map[i]));
		}
	for (unsigned i = 0; i < KIF_MAX_VRAM; i++)
		if (k->vram[i].used)
			vramRelease(this, k, &k->vram[i], false);
	if (k->vm_up) {
		pg106_range_fini(&k->va);
		pg106_range_fini(&k->kva);
		pg106_vmm_fini(&k->vm);
		k->vm_up = false;
	}
	k->generation++;
}

void
PG106::kifTeardown(void)
{
	if (!fKif)
		return;
	kifLost();
	pg106_kfree(fKif);
	fKif = nullptr;
}
