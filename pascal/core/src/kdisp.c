// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/src/kdisp.c — kernel side of the bridge wire protocol. See
// kdisp.h for what each op does and where the rules come from.

#include <pascal/kdisp.h>
#include <pascal/pushbuf.h>
#include <pascal/uapi.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define PAGE 4096ull
#define KD_MAX_BO_SIZE (1ull << 32)
#define KD_TL_POINTS 16u
// Kernel objects at the start of the kernel-managed region.
#define KD_RING_OFF  0x000000ull
#define KD_FENCE_OFF 0x080000ull // ring <= 64K entries (512 KiB)
#define KD_PUSH_OFF  0x081000ull
// Bounded waits (microseconds).
#define KD_SPACE_WAIT_US 1000000u
#define KD_DRAIN_WAIT_US 1000000u
#define KD_POLL_STEP_US  1000u

// clc06f.h: SEMAPHOREA 0x10 .. SEMAPHORED 0x1c, NON_STALL_INTERRUPT 0x20;
// SEMAPHORED OPERATION_RELEASE = 2, RELEASE_WFI_EN = 0 (bit 20),
// RELEASE_SIZE_16BYTE = 0 (bit 24).
#define C06F_SEMAPHOREA 0x0010u
#define C06F_SEMAPHORED_RELEASE_WFI_16B 0x00000002u

struct kd_bo {
	uint32_t handle; // 0 = free slot
	void *mem;
	uint64_t size;
	uint32_t flags;
	bool imported;
	uint64_t cookie; // non-zero while userspace may map it
};

struct kd_piece {
	uint32_t handle;
	uint64_t addr, range, bo_offset;
	uint8_t kind; // PTE kind of the MAP that made it (op flags [7:0])
};

struct kd_region {
	uint64_t addr, range;
};

struct kd_vt {
	struct kd_piece *b;
	size_t nb, capb;
	struct kd_region *r;
	size_t nr, capr;
};

struct kd_point {
	uint64_t point;
	uint32_t seqno; // 0 = already signaled
};

struct kd_sync {
	uint32_t handle; // 0 = free slot
	struct kd_point pt[KD_TL_POINTS];
	size_t npt;
};

struct pg106_kd {
	struct pg106_kd_env env;
	struct kd_bo *bo;
	size_t nbo, capbo;
	uint32_t next_handle;
	struct kd_sync *sy;
	size_t nsy, capsy;
	uint32_t next_sync;

	bool vm_ready;
	struct pg106_uapi_vm_init vm;
	struct pg106_mmu *mmu;
	struct kd_vt vt;

	void *ring_mem, *fence_mem, *push_mem;
	uint64_t *ring;
	volatile uint32_t *fence;
	uint32_t *push;
	uint64_t ring_va, fence_va, push_va;
	uint32_t push_slots;
	uint32_t *job_put; // put index after job seqno (seqno % ib_max)
	bool chan_bound;
	uint32_t put;      // free-running GPFIFO entry counter
	uint32_t emitted, completed;
};

// ---- small helpers --------------------------------------------------------

static int
grow(void **arr, size_t *cap, size_t need, size_t elem)
{
	size_t n;
	void *p;

	if (need <= *cap)
		return 0;
	n = *cap ? *cap : 16;
	while (n < need) {
		if (n > SIZE_MAX / 2 / elem)
			return -ENOMEM;
		n *= 2;
	}
	p = realloc(*arr, n * elem);
	if (!p)
		return -ENOMEM;
	memset((uint8_t *)p + *cap * elem, 0, (n - *cap) * elem);
	*arr = p;
	*cap = n;
	return 0;
}

static bool
seq_done(uint32_t completed, uint32_t seq)
{
	return seq == 0 || (int32_t)(completed - seq) >= 0;
}

static struct kd_bo *
bo_find(struct pg106_kd *kd, uint32_t h)
{
	size_t i;

	if (!h)
		return NULL;
	for (i = 0; i < kd->nbo; i++)
		if (kd->bo[i].handle == h)
			return &kd->bo[i];
	return NULL;
}

static struct kd_sync *
sy_find(struct pg106_kd *kd, uint32_t h)
{
	size_t i;

	if (!h)
		return NULL;
	for (i = 0; i < kd->nsy; i++)
		if (kd->sy[i].handle == h)
			return &kd->sy[i];
	return NULL;
}

uint32_t
pg106_kd_poll(struct pg106_kd *kd)
{
	if (kd->fence) {
		uint32_t v = kd->fence[0];

		// Only move forward, and never past what was emitted.
		if ((int32_t)(v - kd->completed) > 0 &&
		    (int32_t)(kd->emitted - v) >= 0)
			kd->completed = v;
	}
	return kd->completed;
}

// ---- VM table (drm_gpuvm split/merge, nouveau sparse regions) ---------------

static void
vt_free(struct kd_vt *t)
{
	free(t->b);
	free(t->r);
	memset(t, 0, sizeof(*t));
}

static int
vt_clone(struct kd_vt *d, const struct kd_vt *s)
{
	memset(d, 0, sizeof(*d));
	if (grow((void **)&d->b, &d->capb, s->nb ? s->nb : 1, sizeof(*d->b)) ||
	    grow((void **)&d->r, &d->capr, s->nr ? s->nr : 1, sizeof(*d->r))) {
		vt_free(d);
		return -ENOMEM;
	}
	if (s->nb)
		memcpy(d->b, s->b, s->nb * sizeof(*s->b));
	if (s->nr)
		memcpy(d->r, s->r, s->nr * sizeof(*s->r));
	d->nb = s->nb;
	d->nr = s->nr;
	return 0;
}

static int
vt_push(struct kd_vt *t, const struct kd_piece *p)
{
	if (grow((void **)&t->b, &t->capb, t->nb + 1, sizeof(*t->b)))
		return -ENOMEM;
	t->b[t->nb++] = *p;
	return 0;
}

// drm_gpuvm_sm_unmap: cut [addr, addr+range) out of every piece.
static int
vt_cut(struct kd_vt *t, uint64_t addr, uint64_t range)
{
	const uint64_t end = addr + range;
	size_t i = 0;

	while (i < t->nb) {
		struct kd_piece *b = &t->b[i];
		const uint64_t bend = b->addr + b->range;
		const bool head = b->addr < addr, tail = bend > end;

		if (bend <= addr || b->addr >= end) {
			i++;
		} else if (head && tail) {
			struct kd_piece rest = *b;

			rest.addr = end;
			rest.range = bend - end;
			rest.bo_offset = b->bo_offset + (end - b->addr);
			b->range = addr - b->addr;
			if (vt_push(t, &rest))
				return -ENOMEM;
			i++;
		} else if (head) {
			b->range = addr - b->addr;
			i++;
		} else if (tail) {
			b->bo_offset += end - b->addr;
			b->range = bend - end;
			b->addr = end;
			i++;
		} else {
			t->b[i] = t->b[--t->nb];
		}
	}
	return 0;
}

static const struct kd_region *
vt_region(const struct kd_vt *t, uint64_t addr, uint64_t range)
{
	size_t i;

	for (i = 0; i < t->nr; i++)
		if (addr < t->r[i].addr + t->r[i].range &&
		    t->r[i].addr < addr + range)
			return &t->r[i];
	return NULL;
}

// bind_validate_map_common (nouveau_uvmm.c:1147-1155): a MAP is outside
// every region or inside one.
static int
vt_map_rule(const struct kd_vt *t, uint64_t addr, uint64_t range)
{
	const struct kd_region *r = vt_region(t, addr, range);

	if (r && (r->addr > addr || r->addr + r->range < addr + range))
		return -ENOSPC;
	return 0;
}

static int
vt_apply(struct kd_vt *t, const struct pg106_uapi_vm_bind_op *op)
{
	const bool sparse = (op->flags & PG106_UAPI_VM_BIND_SPARSE) != 0;
	size_t i;
	int rc;

	if (op->op == PG106_UAPI_VM_BIND_OP_MAP && sparse) {
		if (vt_region(t, op->addr, op->range))
			return -EEXIST; // __nouveau_uvma_region_insert :280-292
		if (grow((void **)&t->r, &t->capr, t->nr + 1, sizeof(*t->r)))
			return -ENOMEM;
		t->r[t->nr].addr = op->addr;
		t->r[t->nr].range = op->range;
		t->nr++;
		return 0;
	}
	if (op->op == PG106_UAPI_VM_BIND_OP_UNMAP && sparse) {
		for (i = 0; i < t->nr; i++) {
			if (t->r[i].addr == op->addr && t->r[i].range == op->range) {
				rc = vt_cut(t, op->addr, op->range);
				if (rc)
					return rc;
				t->r[i] = t->r[--t->nr];
				return 0;
			}
		}
		return -ENOENT; // nouveau_uvma_region_find :1326-1333
	}
	if (op->op == PG106_UAPI_VM_BIND_OP_MAP) {
		struct kd_piece p = { op->handle, op->addr, op->range, op->bo_offset,
				      (uint8_t)(op->flags & PG106_UAPI_VM_BIND_KIND_MASK) };

		rc = vt_map_rule(t, op->addr, op->range);
		if (rc)
			return rc;
		rc = vt_cut(t, op->addr, op->range);
		return rc ? rc : vt_push(t, &p);
	}
	return vt_cut(t, op->addr, op->range);
}

static bool
vt_bo_bound(const struct kd_vt *t, uint32_t h)
{
	size_t i;

	for (i = 0; i < t->nb; i++)
		if (t->b[i].handle == h)
			return true;
	return false;
}

// ---- page tables ------------------------------------------------------------

// Map [va, va+len) to mem's pages starting at off, run by contiguous run.
static int
mmu_map_mem(struct pg106_kd *kd, void *mem, uint64_t off, uint64_t va,
	    uint64_t len, uint8_t kind)
{
	uint64_t done = 0;

	while (done < len) {
		uint64_t pa = 0, contig = 0, n;
		struct pg106_map m;
		int rc;

		rc = kd->env.mem_dma(kd->env.user, mem, off + done, &pa, &contig);
		if (rc)
			return rc;
		if ((pa & (PAGE - 1)) || contig < PAGE)
			return -EFAULT;
		n = contig & ~(PAGE - 1);
		if (n > len - done)
			n = len - done;
		memset(&m, 0, sizeof(m));
		m.va = va + done;
		m.pa = pa;
		m.size = n;
		m.page_shift = PG106_PAGE_4K_SHIFT;
		m.target = PG106_TARGET_SYS_COHERENT;
		m.kind = kind;
		rc = pg106_mmu_map(kd->mmu, &m);
		if (rc)
			return rc;
		done += n;
	}
	return 0;
}

// Make the page tables of `t` real again over [addr, addr+range): unmap
// the range, then map every piece of t that intersects it.
static int
mmu_sync_range(struct pg106_kd *kd, const struct kd_vt *t, uint64_t addr,
	       uint64_t range)
{
	const uint64_t end = addr + range;
	size_t i;
	int rc;

	pg106_mmu_unmap(kd->mmu, addr, range, PG106_PAGE_4K_SHIFT);
	for (i = 0; i < t->nb; i++) {
		const struct kd_piece *p = &t->b[i];
		uint64_t a = p->addr > addr ? p->addr : addr;
		uint64_t e = p->addr + p->range < end ? p->addr + p->range : end;
		struct kd_bo *bo;

		if (a >= e)
			continue;
		bo = bo_find(kd, p->handle);
		if (!bo)
			return -ENOENT;
		rc = mmu_map_mem(kd, bo->mem, p->bo_offset + (a - p->addr), a,
				 e - a, p->kind);
		if (rc)
			return rc;
	}
	return 0;
}

// ---- syncobjs ----------------------------------------------------------------

// The seqno a wait entry refers to; -ENOENT unknown, -EINVAL no fence yet
// (drm_syncobj_find_fence without WAIT_FOR_SUBMIT).
static int
sy_fence(struct pg106_kd *kd, uint32_t h, bool timeline, uint64_t point,
	 uint32_t *seq)
{
	struct kd_sync *s = sy_find(kd, h);
	size_t i;

	if (!s)
		return -ENOENT;
	if (!s->npt)
		return -EINVAL;
	if (!timeline) {
		*seq = s->pt[s->npt - 1].seqno;
		return 0;
	}
	for (i = 0; i < s->npt; i++) {
		if (s->pt[i].point >= point) {
			*seq = s->pt[i].seqno;
			return 0;
		}
	}
	return -EINVAL;
}

static void
sy_signal(struct pg106_kd *kd, struct kd_sync *s, bool timeline,
	  uint64_t point, uint32_t seq)
{
	if (!timeline) {
		// drm_syncobj_replace_fence.
		s->pt[0].point = 0;
		s->pt[0].seqno = seq;
		s->npt = 1;
		return;
	}
	// drm_syncobj_add_point; drop the oldest signaled entry when full.
	if (s->npt == KD_TL_POINTS) {
		pg106_kd_poll(kd);
		memmove(&s->pt[0], &s->pt[1], (KD_TL_POINTS - 1) * sizeof(s->pt[0]));
		s->npt--;
	}
	s->pt[s->npt].point = point;
	s->pt[s->npt].seqno = seq;
	s->npt++;
}

static int
check_syncs(struct pg106_kd *kd, const struct pg106_uapi_sync *w, uint32_t nw,
	    const struct pg106_uapi_sync *g, uint32_t ng)
{
	uint32_t i, seq;
	int rc;

	for (i = 0; i < nw; i++) {
		rc = sy_fence(kd, w[i].handle,
			      (w[i].flags & PG106_UAPI_SYNC_TYPE_MASK) ==
				      PG106_UAPI_SYNC_TIMELINE_SYNCOBJ,
			      w[i].timeline_value, &seq);
		if (rc)
			return rc;
	}
	for (i = 0; i < ng; i++)
		if (!sy_find(kd, g[i].handle))
			return -ENOENT;
	return 0;
}

static void
signal_all(struct pg106_kd *kd, const struct pg106_uapi_sync *g, uint32_t ng,
	   uint32_t seq)
{
	uint32_t i;

	for (i = 0; i < ng; i++)
		sy_signal(kd, sy_find(kd, g[i].handle),
			  (g[i].flags & PG106_UAPI_SYNC_TYPE_MASK) ==
				  PG106_UAPI_SYNC_TIMELINE_SYNCOBJ,
			  g[i].timeline_value, seq);
}

// ---- kernel objects + channel --------------------------------------------------

static int
kobj_alloc(struct pg106_kd *kd, uint64_t size, void **mem, void **cpu)
{
	int rc = kd->env.mem_alloc(kd->env.user, size, PAGE, PG106_BR_BO_GART, mem);

	if (rc)
		return rc;
	*cpu = kd->env.mem_cpu(kd->env.user, *mem);
	if (!*cpu) {
		kd->env.mem_free(kd->env.user, *mem);
		*mem = NULL;
		return -ENOMEM;
	}
	memset(*cpu, 0, size);
	return 0;
}

static uint64_t
pg_round(uint64_t v)
{
	return (v + PAGE - 1) & ~(PAGE - 1);
}

static int
vm_setup(struct pg106_kd *kd)
{
	const uint64_t k = kd->vm.kernel_managed_addr;
	const uint64_t ring_sz = pg_round((uint64_t)kd->env.ib_max * 8);
	uint64_t push_sz;
	void *cpu;
	int rc;

	kd->push_slots = kd->env.ib_max / 2;
	push_sz = pg_round((uint64_t)kd->push_slots * PG106_KD_FENCE_PUSH_WORDS * 4);
	if (ring_sz > KD_FENCE_OFF - KD_RING_OFF ||
	    KD_PUSH_OFF + push_sz > PG106_KD_KERNEL_VA_SPAN)
		return -EINVAL;
	kd->job_put = calloc(kd->env.ib_max, sizeof(*kd->job_put));
	if (!kd->job_put)
		return -ENOMEM;
	rc = pg106_mmu_init(&kd->mmu, &kd->env.mmu_cfg);
	if (rc)
		return rc;
	if ((rc = kobj_alloc(kd, ring_sz, &kd->ring_mem, &cpu)))
		return rc;
	kd->ring = cpu;
	if ((rc = kobj_alloc(kd, PAGE, &kd->fence_mem, &cpu)))
		return rc;
	kd->fence = cpu;
	if ((rc = kobj_alloc(kd, push_sz, &kd->push_mem, &cpu)))
		return rc;
	kd->push = cpu;
	kd->ring_va = k + KD_RING_OFF;
	kd->fence_va = k + KD_FENCE_OFF;
	kd->push_va = k + KD_PUSH_OFF;
	if ((rc = mmu_map_mem(kd, kd->ring_mem, 0, kd->ring_va, ring_sz, 0)) ||
	    (rc = mmu_map_mem(kd, kd->fence_mem, 0, kd->fence_va, PAGE, 0)) ||
	    (rc = mmu_map_mem(kd, kd->push_mem, 0, kd->push_va, push_sz, 0)))
		return rc;
	if (kd->env.vm_flush && (rc = kd->env.vm_flush(kd->env.user, kd->mmu)))
		return rc;
	if (kd->env.chan_bind) {
		struct pg106_kd_chan ch = {
			.pdb_addr = pg106_mmu_root_addr(kd->mmu),
			.pdb_target = kd->env.mmu_cfg.table_target,
			.vm_limit = PG106_MMU_VA_LIMIT,
			.ring_va = kd->ring_va,
			.ring_entries = kd->env.ib_max,
			.fence_va = kd->fence_va,
		};

		rc = kd->env.chan_bind(kd->env.user, &ch);
		// -ENOSYS: the host has no hardware channel to give (the
		// kext until channel bring-up is ported). The VM still works;
		// EXEC answers -ENODEV.
		if (rc == -ENOSYS)
			return 0;
		if (rc)
			return rc;
		kd->chan_bound = true;
	}
	return 0;
}

static void
vm_teardown(struct pg106_kd *kd)
{
	if (kd->chan_bound && kd->env.chan_unbind)
		kd->env.chan_unbind(kd->env.user);
	kd->chan_bound = false;
	if (kd->mmu)
		pg106_mmu_destroy(kd->mmu); // all PTEs go with the tables
	kd->mmu = NULL;
	if (kd->ring_mem)
		kd->env.mem_free(kd->env.user, kd->ring_mem);
	if (kd->fence_mem)
		kd->env.mem_free(kd->env.user, kd->fence_mem);
	if (kd->push_mem)
		kd->env.mem_free(kd->env.user, kd->push_mem);
	kd->ring_mem = kd->fence_mem = kd->push_mem = NULL;
	kd->ring = NULL;
	kd->fence = NULL;
	kd->push = NULL;
	free(kd->job_put);
	kd->job_put = NULL;
	vt_free(&kd->vt);
	kd->vm_ready = false;
}

// ---- ops ---------------------------------------------------------------------

struct reply {
	void *p;     // response payload buffer
	size_t len;  // bytes written on success
};

static int
op_handshake(struct pg106_kd *kd, const uint8_t *pl, size_t n, struct reply *r)
{
	struct pg106_br_handshake hs;

	// The client's version is the bridge's to judge (it refuses a kext
	// it does not speak, pg106_br_dev_open); the kernel only answers.
	(void)pl;
	if (n != sizeof(hs))
		return -EPROTO;
	memset(&hs, 0, sizeof(hs));
	hs.kext_major = kd->env.abi_major;
	hs.kext_minor = kd->env.abi_minor;
	hs.ib_max = kd->env.ib_max;
	hs.drm_version = kd->env.drm_version;
	hs.va_space_end = PG106_UAPI_VA_SPACE_END;
	memcpy(r->p, &hs, sizeof(hs));
	r->len = sizeof(hs);
	return 0;
}

static int
op_getparam(struct pg106_kd *kd, const uint8_t *pl, size_t n, struct reply *r)
{
	struct pg106_uapi_getparam gp;

	if (n != sizeof(gp))
		return -EPROTO;
	memcpy(&gp, pl, sizeof(gp));
	switch (gp.param) { // nouveau_abi16.c:299-327
	case PG106_UAPI_GETPARAM_PCI_VENDOR: gp.value = kd->env.pci_vendor; break;
	case PG106_UAPI_GETPARAM_PCI_DEVICE: gp.value = kd->env.pci_device; break;
	case PG106_UAPI_GETPARAM_GRAPH_UNITS: gp.value = kd->env.graph_units; break;
	case PG106_UAPI_GETPARAM_PTIMER_TIME:
		gp.value = kd->env.ptimer_ns ? kd->env.ptimer_ns(kd->env.user) : 0;
		break;
	case PG106_UAPI_GETPARAM_EXEC_PUSH_MAX:
		gp.value = pg106_uapi_exec_push_max(kd->env.ib_max);
		break;
	case PG106_UAPI_GETPARAM_VRAM_BAR_SIZE: gp.value = kd->env.vram_bar_size; break;
	case PG106_UAPI_GETPARAM_VRAM_USED: gp.value = 0; break;
	case PG106_UAPI_GETPARAM_HAS_VMA_TILEMODE: gp.value = 1; break;
	case PG106_BR_GETPARAM_VRAM_SIZE: gp.value = kd->env.vram_size; break;
	case PG106_BR_GETPARAM_BOOT0: gp.value = kd->env.boot0; break;
	case PG106_BR_GETPARAM_READY:
		gp.value = kd->env.ready ? kd->env.ready(kd->env.user) : 1;
		break;
	default:
		return -EINVAL; // nouveau_abi16.c default case
	}
	memcpy(r->p, &gp, sizeof(gp));
	r->len = sizeof(gp);
	return 0;
}

static int
op_vm_init(struct pg106_kd *kd, const uint8_t *pl, size_t n)
{
	struct pg106_uapi_vm_init vi;
	int rc;

	if (n != sizeof(vi))
		return -EPROTO;
	memcpy(&vi, pl, sizeof(vi));
	if (kd->vm_ready)
		return -EEXIST;
	rc = pg106_uapi_vm_init_validate(&vi);
	if (rc)
		return rc;
	if (vi.kernel_managed_size < PG106_KD_KERNEL_VA_SPAN ||
	    (vi.kernel_managed_addr & (PAGE - 1)))
		return -EINVAL; // the kernel's objects must fit
	kd->vm = vi;
	rc = vm_setup(kd);
	if (rc) {
		vm_teardown(kd);
		return rc;
	}
	kd->vm_ready = true;
	return 0;
}

static struct kd_bo *
bo_slot(struct pg106_kd *kd)
{
	size_t i;

	for (i = 0; i < kd->nbo; i++)
		if (!kd->bo[i].handle)
			return &kd->bo[i];
	if (grow((void **)&kd->bo, &kd->capbo, kd->nbo + 1, sizeof(*kd->bo)))
		return NULL;
	return &kd->bo[kd->nbo++];
}

static uint32_t
new_handle(struct pg106_kd *kd)
{
	do {
		if (++kd->next_handle == 0)
			kd->next_handle = 1;
	} while (bo_find(kd, kd->next_handle));
	return kd->next_handle;
}

static int
op_bo_alloc(struct pg106_kd *kd, const uint8_t *pl, size_t n, struct reply *r)
{
	struct pg106_br_bo_alloc a;
	struct kd_bo *bo;
	void *mem = NULL;
	int rc;

	if (n != sizeof(a))
		return -EPROTO;
	memcpy(&a, pl, sizeof(a));
	if (a.handle || !a.size_B || a.size_B > KD_MAX_BO_SIZE || !a.align_B ||
	    (a.align_B & (a.align_B - 1)))
		return -EINVAL;
	a.size_B = pg_round(a.size_B);
	bo = bo_slot(kd);
	if (!bo)
		return -ENOMEM;
	rc = kd->env.mem_alloc(kd->env.user, a.size_B,
			       a.align_B < PAGE ? PAGE : a.align_B, a.flags, &mem);
	if (rc)
		return rc;
	memset(bo, 0, sizeof(*bo));
	bo->handle = new_handle(kd);
	bo->mem = mem;
	bo->size = a.size_B;
	bo->flags = a.flags & 0x00ffffffu;
	a.handle = bo->handle;
	memcpy(r->p, &a, sizeof(a));
	r->len = sizeof(a);
	return 0;
}

static int
op_bo_import(struct pg106_kd *kd, const uint8_t *pl, size_t n, struct reply *r)
{
	struct pg106_br_bo_import_host im;
	struct kd_bo *bo;
	void *mem = NULL;
	int rc;

	if (n != sizeof(im))
		return -EPROTO;
	memcpy(&im, pl, sizeof(im));
	if (im.handle || !im.size_B || im.size_B > KD_MAX_BO_SIZE ||
	    (im.host_addr & (PAGE - 1)) || (im.size_B & (PAGE - 1)) ||
	    im.host_addr + im.size_B < im.host_addr)
		return -EINVAL;
	if (!kd->env.mem_import)
		return -EOPNOTSUPP;
	bo = bo_slot(kd);
	if (!bo)
		return -ENOMEM;
	rc = kd->env.mem_import(kd->env.user, im.host_addr, im.size_B, im.flags,
				&mem);
	if (rc)
		return rc;
	memset(bo, 0, sizeof(*bo));
	bo->handle = new_handle(kd);
	bo->mem = mem;
	bo->size = im.size_B;
	bo->flags = im.flags & 0x00ffffffu;
	bo->imported = true;
	im.handle = bo->handle;
	memcpy(r->p, &im, sizeof(im));
	r->len = sizeof(im);
	return 0;
}

static void
bo_release(struct pg106_kd *kd, struct kd_bo *bo)
{
	if (bo->cookie && kd->env.mem_unmap)
		kd->env.mem_unmap(kd->env.user, bo->mem, bo->cookie);
	kd->env.mem_free(kd->env.user, bo->mem); // unwire after the PTEs
	memset(bo, 0, sizeof(*bo));
}

static int
op_bo_free(struct pg106_kd *kd, const uint8_t *pl, size_t n)
{
	struct pg106_br_bo_free f;
	struct kd_bo *bo;

	if (n != sizeof(f))
		return -EPROTO;
	memcpy(&f, pl, sizeof(f));
	if (f.pad)
		return -EINVAL;
	bo = bo_find(kd, f.handle);
	if (!bo)
		return -ENOENT;
	// The pinning rule (NOUVEAU-UAPI.md section 6.1 rule 3): pages under
	// live PTEs are never released.
	if (vt_bo_bound(&kd->vt, f.handle))
		return -EBUSY;
	bo_release(kd, bo);
	return 0;
}

static int
op_bo_map(struct pg106_kd *kd, const uint8_t *pl, size_t n, struct reply *r)
{
	struct pg106_br_bo_map m;
	struct kd_bo *bo;
	int rc;

	if (n != sizeof(m))
		return -EPROTO;
	memcpy(&m, pl, sizeof(m));
	if (m.map_cookie)
		return -EINVAL;
	bo = bo_find(kd, m.handle);
	if (!bo)
		return -ENOENT;
	if (bo->imported)
		return -EINVAL; // the caller already has the pages
	if (!(bo->flags & PG106_BR_BO_CAN_MAP))
		return -EACCES;
	if (bo->cookie)
		return -EEXIST;
	if (!kd->env.mem_map)
		return -EOPNOTSUPP;
	rc = kd->env.mem_map(kd->env.user, bo->mem, bo->handle, &bo->cookie);
	if (rc) {
		bo->cookie = 0;
		return rc;
	}
	if (!bo->cookie)
		return -EIO;
	m.map_cookie = bo->cookie;
	memcpy(r->p, &m, sizeof(m));
	r->len = sizeof(m);
	return 0;
}

static int
op_bo_unmap(struct pg106_kd *kd, const uint8_t *pl, size_t n)
{
	struct pg106_br_bo_unmap u;
	struct kd_bo *bo;

	if (n != sizeof(u))
		return -EPROTO;
	memcpy(&u, pl, sizeof(u));
	if (u.pad)
		return -EINVAL;
	bo = bo_find(kd, u.handle);
	if (!bo)
		return -ENOENT;
	if (!bo->cookie)
		return -EINVAL;
	if (kd->env.mem_unmap)
		kd->env.mem_unmap(kd->env.user, bo->mem, bo->cookie);
	bo->cookie = 0;
	return 0;
}

static int
op_vm_bind(struct pg106_kd *kd, const uint8_t *pl, size_t n)
{
	struct pg106_uapi_vm_bind req;
	const struct pg106_uapi_vm_bind_op *ops;
	const struct pg106_uapi_sync *waits, *sigs;
	struct kd_vt next;
	uint64_t need;
	uint32_t i;
	int rc;

	if (n < sizeof(req))
		return -EPROTO;
	memcpy(&req, pl, sizeof(req));
	need = sizeof(req) + (uint64_t)req.op_count * sizeof(*ops) +
	       ((uint64_t)req.wait_count + req.sig_count) * sizeof(*waits);
	if (need != n || req.op_ptr || req.wait_ptr || req.sig_ptr)
		return -EPROTO;
	if (!kd->vm_ready)
		return -ENOSYS; // VM_INIT first (PG106_UAPI_ENOSYS on the bridge)
	// The wire carries the arrays packed after the header; they are
	// naturally aligned (40/16-byte elements after a 40-byte header).
	ops = (const void *)(pl + sizeof(req));
	waits = (const void *)(pl + sizeof(req) + (size_t)req.op_count * sizeof(*ops));
	sigs = waits + req.wait_count;

	rc = pg106_uapi_vm_bind_validate(&kd->vm, &req, req.op_count ? ops : NULL,
					 req.wait_count ? waits : NULL,
					 req.sig_count ? sigs : NULL);
	if (rc)
		return rc;
	for (i = 0; i < req.op_count; i++) {
		struct kd_bo *bo;

		if (!pg106_uapi_vm_bind_op_is_bo_map(&ops[i]))
			continue;
		bo = bo_find(kd, ops[i].handle);
		if (!bo)
			return -ENOENT;
		rc = pg106_uapi_vm_bind_op_check_map(&ops[i], bo->size);
		if (rc)
			return rc;
		if (!pg106_mmu_kind_valid((uint8_t)(ops[i].flags &
						    PG106_UAPI_VM_BIND_KIND_MASK)))
			return -EINVAL;
	}
	rc = check_syncs(kd, waits, req.wait_count, sigs, req.sig_count);
	if (rc)
		return rc;
	// bind_validate_region (nouveau_uvmm.c:1158-1188) against the state
	// before the job.
	for (i = 0; i < req.op_count; i++) {
		if (ops[i].op != PG106_UAPI_VM_BIND_OP_MAP)
			continue;
		if (ops[i].flags & PG106_UAPI_VM_BIND_SPARSE) {
			if (vt_region(&kd->vt, ops[i].addr, ops[i].range))
				return -ENOSPC;
		} else if ((rc = vt_map_rule(&kd->vt, ops[i].addr, ops[i].range))) {
			return rc;
		}
	}
	// The whole job against a copy; the live table changes only if all
	// ops apply (the kernel unwinds a failed job, nouveau_uvmm.c:1309-1313).
	rc = vt_clone(&next, &kd->vt);
	if (rc)
		return rc;
	for (i = 0; i < req.op_count; i++) {
		rc = vt_apply(&next, &ops[i]);
		if (rc) {
			vt_free(&next);
			return rc;
		}
	}
	// Page tables: every op's range is re-derived from the new table.
	// The PTE kind of a piece is its op's; kept per BO-piece below.
	for (i = 0; i < req.op_count; i++) {
		const struct pg106_uapi_vm_bind_op *op = &ops[i];

		pg106_mmu_unmap(kd->mmu, op->addr, op->range, PG106_PAGE_4K_SHIFT);
		if (pg106_uapi_vm_bind_op_is_bo_map(op)) {
			struct kd_bo *bo = bo_find(kd, op->handle);

			rc = mmu_map_mem(kd, bo->mem, op->bo_offset, op->addr,
					 op->range,
					 (uint8_t)(op->flags & PG106_UAPI_VM_BIND_KIND_MASK));
			if (rc)
				break;
		}
	}
	if (rc) {
		// Roll the page tables back to the old table over every
		// range this job touched.
		for (i = 0; i < req.op_count; i++)
			(void)mmu_sync_range(kd, &kd->vt, ops[i].addr, ops[i].range);
		vt_free(&next);
		return rc;
	}
	vt_free(&kd->vt);
	kd->vt = next;
	if (kd->env.vm_flush && (rc = kd->env.vm_flush(kd->env.user, kd->mmu)))
		return rc;
	// Binds run synchronously: their fences are already signaled.
	signal_all(kd, sigs, req.sig_count, 0);
	return 0;
}

static uint32_t
ring_free(struct pg106_kd *kd)
{
	const uint32_t n = kd->env.ib_max;
	uint32_t get;

	pg106_kd_poll(kd);
	// GP_GET as seen through completed jobs: everything up to the end of
	// the last completed job has been consumed.
	get = kd->completed ? kd->job_put[kd->completed % n] : 0;
	if (kd->emitted == kd->completed)
		get = kd->put;
	return n - 1 - (kd->put - get);
}

static int
op_exec(struct pg106_kd *kd, const uint8_t *pl, size_t n)
{
	struct pg106_uapi_exec req;
	const struct pg106_uapi_exec_push *pushes;
	const struct pg106_uapi_sync *waits, *sigs;
	const uint32_t N = kd->env.ib_max;
	uint32_t i, seq, waited = 0, slot;
	uint32_t data[5];
	pg106_pb pb;
	uint64_t need;
	int rc;

	if (n < sizeof(req))
		return -EPROTO;
	memcpy(&req, pl, sizeof(req));
	need = sizeof(req) + (uint64_t)req.push_count * sizeof(*pushes) +
	       ((uint64_t)req.wait_count + req.sig_count) * sizeof(*waits);
	if (need != n || req.push_ptr || req.wait_ptr || req.sig_ptr)
		return -EPROTO;
	pushes = (const void *)(pl + sizeof(req));
	waits = (const void *)(pl + sizeof(req) + (size_t)req.push_count * sizeof(*pushes));
	sigs = waits + req.wait_count;

	rc = pg106_uapi_exec_validate(N, &req, req.push_count ? pushes : NULL,
				      req.wait_count ? waits : NULL,
				      req.sig_count ? sigs : NULL);
	if (rc)
		return rc;
	if (req.channel != 0)
		return -ENOENT; // nouveau_exec.c:373-381
	if (!kd->vm_ready || !kd->chan_bound)
		return -ENODEV;
	rc = check_syncs(kd, waits, req.wait_count, sigs, req.sig_count);
	if (rc)
		return rc;

	// Room for every push plus the fence push.
	while (ring_free(kd) < req.push_count + 1) {
		if (waited >= KD_SPACE_WAIT_US)
			return -EBUSY;
		kd->env.delay_us(kd->env.user, KD_POLL_STEP_US);
		waited += KD_POLL_STEP_US;
	}
	for (i = 0; i < req.push_count; i++) {
		kd->ring[kd->put % N] = pg106_gpfifo_pack(
			pushes[i].va, pushes[i].va_len / 4, PG106_GP_LEVEL_SUBROUTINE,
			(pushes[i].flags & PG106_UAPI_EXEC_PUSH_NO_PREFETCH) ? 1 : 0,
			PG106_GP_FETCH_UNCONDITIONAL);
		kd->put++;
	}
	seq = kd->emitted + 1;
	if (seq == 0)
		seq = 1; // 0 means "signaled" to the syncobj code
	// nvc0_fence_emit32 (nvc0_fence.c:35-55).
	slot = seq % kd->push_slots;
	data[0] = (uint32_t)(kd->fence_va >> 32) & 0xffu; // SEMAPHOREA OFFSET_UPPER 7:0
	data[1] = (uint32_t)kd->fence_va;                 // SEMAPHOREB
	data[2] = seq;                                    // SEMAPHOREC payload
	data[3] = C06F_SEMAPHORED_RELEASE_WFI_16B;        // SEMAPHORED
	data[4] = 0;                                      // NON_STALL_INTERRUPT
	pg106_pb_init(&pb, kd->push + (size_t)slot * PG106_KD_FENCE_PUSH_WORDS,
		      PG106_KD_FENCE_PUSH_WORDS);
	pg106_pb_inc(&pb, 0, C06F_SEMAPHOREA, data, 5);
	if (pb.error)
		return -EIO;
	kd->ring[kd->put % N] = pg106_gpfifo_pack(
		kd->push_va + (uint64_t)slot * PG106_KD_FENCE_PUSH_WORDS * 4,
		(uint32_t)pb.len, PG106_GP_LEVEL_MAIN, PG106_GP_SYNC_PROCEED,
		PG106_GP_FETCH_UNCONDITIONAL);
	kd->put++;
	kd->emitted = seq;
	kd->job_put[seq % N] = kd->put;
	// GP_PUT at USERD + 0x8c (nvif/chan506f.c gpfifo_kick); the hook must
	// order the ring writes before the doorbell.
	kd->env.kick(kd->env.user, kd->put % N);
	signal_all(kd, sigs, req.sig_count, seq);
	return 0;
}

static int
op_syncobj_create(struct pg106_kd *kd, const uint8_t *pl, size_t n,
		  struct reply *r)
{
	struct pg106_br_syncobj so;
	struct kd_sync *s = NULL;
	size_t i;

	if (n != sizeof(so))
		return -EPROTO;
	memcpy(&so, pl, sizeof(so));
	if (so.handle || (so.flags & ~PG106_BR_SYNCOBJ_CREATE_SIGNALED))
		return -EINVAL;
	for (i = 0; i < kd->nsy; i++)
		if (!kd->sy[i].handle) {
			s = &kd->sy[i];
			break;
		}
	if (!s) {
		if (grow((void **)&kd->sy, &kd->capsy, kd->nsy + 1, sizeof(*kd->sy)))
			return -ENOMEM;
		s = &kd->sy[kd->nsy++];
	}
	memset(s, 0, sizeof(*s));
	do {
		if (++kd->next_sync == 0)
			kd->next_sync = 1;
	} while (sy_find(kd, kd->next_sync));
	s->handle = kd->next_sync;
	if (so.flags & PG106_BR_SYNCOBJ_CREATE_SIGNALED) {
		s->pt[0].point = 0;
		s->pt[0].seqno = 0;
		s->npt = 1;
	}
	so.handle = s->handle;
	so.flags = 0;
	memcpy(r->p, &so, sizeof(so));
	r->len = sizeof(so);
	return 0;
}

static int
op_syncobj_destroy(struct pg106_kd *kd, const uint8_t *pl, size_t n)
{
	struct pg106_br_syncobj so;
	struct kd_sync *s;

	if (n != sizeof(so))
		return -EPROTO;
	memcpy(&so, pl, sizeof(so));
	if (so.flags)
		return -EINVAL;
	s = sy_find(kd, so.handle);
	if (!s)
		return -ENOENT;
	memset(s, 0, sizeof(*s));
	return 0;
}

static int
op_syncobj_wait(struct pg106_kd *kd, const uint8_t *pl, size_t n)
{
	struct pg106_br_syncobj_wait w;
	struct kd_sync *s;
	uint64_t start;
	uint32_t seq;
	int rc;

	if (n != sizeof(w))
		return -EPROTO;
	memcpy(&w, pl, sizeof(w));
	if (w.flags)
		return -EINVAL;
	s = sy_find(kd, w.handle);
	if (!s)
		return -ENOENT;
	rc = sy_fence(kd, w.handle, w.point != 0, w.point, &seq);
	if (rc)
		return rc;
	start = kd->env.now_ns(kd->env.user);
	for (;;) {
		if (seq_done(pg106_kd_poll(kd), seq))
			return 0;
		{
			const uint64_t spent = kd->env.now_ns(kd->env.user) - start;
			uint64_t left_us;

			if (spent >= w.timeout_ns)
				return -ETIME;
			if (!kd->env.wait_event) {
				kd->env.delay_us(kd->env.user, KD_POLL_STEP_US);
				continue;
			}
			// Sleep for the interrupt; wake at least every 10 ms
			// in case a release landed without one.
			left_us = (w.timeout_ns - spent + 999) / 1000;
			kd->env.wait_event(kd->env.user,
					   left_us > 10000 ? 10000u : (uint32_t)left_us);
		}
	}
}

int
pg106_kd_handle(struct pg106_kd *kd, const void *reqv, size_t req_len,
		void *respv, size_t resp_cap, size_t *resp_len)
{
	const uint8_t *req = reqv;
	uint8_t *resp = respv;
	struct pg106_br_frame h, o;
	struct reply r;
	const uint8_t *pl;
	size_t n;
	int st;

	if (!kd || !req || !resp || !resp_len ||
	    resp_cap < sizeof(o) + PG106_BR_WIRE_MAX_RESP)
		return -EINVAL;
	*resp_len = 0;
	if (req_len < sizeof(h))
		return -EPROTO;
	memcpy(&h, req, sizeof(h));
	if (h.magic != PG106_BR_WIRE_MAGIC ||
	    h.wire_version != PG106_BR_WIRE_VERSION || h.flags || h.reserved ||
	    h.payload_len != req_len - sizeof(h) ||
	    h.payload_len > PG106_BR_WIRE_MAX_PAYLOAD)
		return -EPROTO;
	pl = req + sizeof(h);
	n = h.payload_len;
	r.p = resp + sizeof(o);
	r.len = 0;

	if (kd->env.ready && !kd->env.ready(kd->env.user) &&
	    h.op != PG106_BR_OP_HANDSHAKE && h.op != PG106_BR_OP_GETPARAM) {
		st = -ENODEV;
	} else {
		switch (h.op) {
		case PG106_BR_OP_HANDSHAKE: st = op_handshake(kd, pl, n, &r); break;
		case PG106_BR_OP_GETPARAM: st = op_getparam(kd, pl, n, &r); break;
		case PG106_BR_OP_VM_INIT: st = op_vm_init(kd, pl, n); break;
		case PG106_BR_OP_BO_ALLOC: st = op_bo_alloc(kd, pl, n, &r); break;
		case PG106_BR_OP_BO_FREE: st = op_bo_free(kd, pl, n); break;
		case PG106_BR_OP_BO_MAP: st = op_bo_map(kd, pl, n, &r); break;
		case PG106_BR_OP_BO_UNMAP: st = op_bo_unmap(kd, pl, n); break;
		case PG106_BR_OP_VM_BIND: st = op_vm_bind(kd, pl, n); break;
		case PG106_BR_OP_EXEC: st = op_exec(kd, pl, n); break;
		case PG106_BR_OP_BO_IMPORT_HOST: st = op_bo_import(kd, pl, n, &r); break;
		case PG106_BR_OP_SYNCOBJ_CREATE: st = op_syncobj_create(kd, pl, n, &r); break;
		case PG106_BR_OP_SYNCOBJ_DESTROY: st = op_syncobj_destroy(kd, pl, n); break;
		case PG106_BR_OP_SYNCOBJ_WAIT: st = op_syncobj_wait(kd, pl, n); break;
		default: st = -EPROTO; break;
		}
	}
	if (st)
		r.len = 0; // error responses carry no payload
	memset(&o, 0, sizeof(o));
	o.magic = PG106_BR_WIRE_MAGIC;
	o.op = h.op;
	o.wire_version = PG106_BR_WIRE_VERSION;
	o.seq = h.seq;
	o.status = st;
	o.payload_len = (uint32_t)r.len;
	memcpy(resp, &o, sizeof(o));
	*resp_len = sizeof(o) + r.len;
	return 0;
}

// ---- lifecycle -----------------------------------------------------------------

int
pg106_kd_create(const struct pg106_kd_env *env, struct pg106_kd **out)
{
	struct pg106_kd *kd;

	if (!env || !out || !env->mem_alloc || !env->mem_free || !env->mem_dma ||
	    !env->mem_cpu || !env->kick || !env->delay_us || !env->now_ns ||
	    env->ib_max < 4 || (env->ib_max & (env->ib_max - 1)) ||
	    env->ib_max > 0x10000)
		return -EINVAL;
	kd = calloc(1, sizeof(*kd));
	if (!kd)
		return -ENOMEM;
	kd->env = *env;
	*out = kd;
	return 0;
}

void
pg106_kd_destroy(struct pg106_kd *kd)
{
	uint32_t waited = 0;
	size_t i;

	if (!kd)
		return;
	// 1. In-flight work drains (bounded): pages stay wired until then.
	while (kd->fence && !seq_done(pg106_kd_poll(kd), kd->emitted) &&
	       waited < KD_DRAIN_WAIT_US) {
		kd->env.delay_us(kd->env.user, KD_POLL_STEP_US);
		waited += KD_POLL_STEP_US;
	}
	// 2-3. Channel off, page tables gone, then 4. unwire + free BOs.
	vm_teardown(kd);
	for (i = 0; i < kd->nbo; i++)
		if (kd->bo[i].handle)
			bo_release(kd, &kd->bo[i]);
	free(kd->bo);
	free(kd->sy);
	free(kd);
}

const struct pg106_mmu *
pg106_kd_mmu(const struct pg106_kd *kd)
{
	return kd->mmu;
}

uint32_t
pg106_kd_seqno_emitted(const struct pg106_kd *kd)
{
	return kd->emitted;
}

size_t
pg106_kd_bo_count(const struct pg106_kd *kd)
{
	size_t i, c = 0;

	for (i = 0; i < kd->nbo; i++)
		c += kd->bo[i].handle != 0;
	return c;
}

bool
pg106_kd_va_lookup(const struct pg106_kd *kd, uint64_t va, uint32_t *handle,
		   uint64_t *bo_offset)
{
	size_t i;

	for (i = 0; i < kd->vt.nb; i++) {
		const struct kd_piece *p = &kd->vt.b[i];

		if (va >= p->addr && va - p->addr < p->range) {
			if (handle)
				*handle = p->handle;
			if (bo_offset)
				*bo_offset = p->bo_offset + (va - p->addr);
			return true;
		}
	}
	return false;
}
