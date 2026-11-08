// SPDX-License-Identifier: MIT
/*
 * pascal/uapi.c - validation for the nouveau CS uapi subset NVK needs.
 *
 * Every rule cites its Linux kernel origin (drivers/gpu/drm/nouveau,
 * Linux 7.4-rc1 d48723711bae) here, in pascal/uapi.h and in
 * research/evidence/NOUVEAU-UAPI.md. MIT, like uapi.h (see the licence
 * note and the nouveau_drm.h notice there).
 *
 * Pure C11, libc only, no hardware access.
 */
#include "pascal/uapi.h"

#define PG106_UAPI_PAGE_MASK ((uint64_t)PG106_UAPI_PAGE_SIZE - 1)

static int pg106_uapi_page_aligned(uint64_t v)
{
    return (v & PG106_UAPI_PAGE_MASK) == 0;
}

/* Half-open [a, a+alen) vs [b, b+blen) overlap; callers have already
 * ruled out overflow of both ends. */
static int pg106_uapi_ranges_overlap(uint64_t a, uint64_t alen,
                                     uint64_t b, uint64_t blen)
{
    return a + alen > b && b + blen > a;
}

static int pg106_uapi_add_overflows(uint64_t a, uint64_t b)
{
    return a > UINT64_MAX - b;
}

uint32_t pg106_uapi_exec_push_max(uint32_t ib_max)
{
    /* nouveau_exec_push_max_from_ib_max (nouveau_exec.h:51-58):
     * half the GPFIFO ring, minus one slot for the HW fence. */
    return ib_max > 1 ? ib_max / 2 - 1 : 0;
}

static int pg106_uapi_sync_type_ok(const struct pg106_uapi_sync *s)
{
    uint32_t stype = s->flags & PG106_UAPI_SYNC_TYPE_MASK;

    return stype == PG106_UAPI_SYNC_SYNCOBJ ||
           stype == PG106_UAPI_SYNC_TIMELINE_SYNCOBJ;
}

int pg106_uapi_sync_check(const struct pg106_uapi_sync *s)
{
    if (s == NULL)
        return PG106_UAPI_EINVAL;
    /* sync_find_fence (nouveau_sched.c:147-149). */
    return pg106_uapi_sync_type_ok(s) ? 0 : PG106_UAPI_EOPNOTSUPP;
}

/* Waits: -EOPNOTSUPP per bad type; signals: -EINVAL
 * (nouveau_job_fence_attach_prepare, nouveau_sched.c:215-219). */
static int pg106_uapi_syncs_check(const struct pg106_uapi_sync *waits,
                                  uint32_t wait_count,
                                  const struct pg106_uapi_sync *sigs,
                                  uint32_t sig_count)
{
    uint32_t i;
    int ret;

    for (i = 0; i < wait_count; i++)
        if ((ret = pg106_uapi_sync_check(&waits[i])) != 0)
            return ret;
    for (i = 0; i < sig_count; i++)
        if (!pg106_uapi_sync_type_ok(&sigs[i]))
            return PG106_UAPI_EINVAL;
    return 0;
}

int pg106_uapi_vm_init_validate(const struct pg106_uapi_vm_init *init)
{
    if (init == NULL)
        return PG106_UAPI_EINVAL;

    /* nouveau_uvmm_ioctl_vm_init (nouveau_uvmm.c:1912-1918):
     * check_add_overflow(kernel_managed_addr, kernel_managed_size)
     * and kernel_managed_end <= NOUVEAU_VA_SPACE_END. */
    if (pg106_uapi_add_overflows(init->kernel_managed_addr,
                                 init->kernel_managed_size))
        return PG106_UAPI_EINVAL;

    if (init->kernel_managed_addr + init->kernel_managed_size >
        PG106_UAPI_VA_SPACE_END)
        return PG106_UAPI_EINVAL;

    return 0;
}

int pg106_uapi_vm_bind_op_is_bo_map(const struct pg106_uapi_vm_bind_op *op)
{
    return op->op == PG106_UAPI_VM_BIND_OP_MAP &&
           (op->flags & PG106_UAPI_VM_BIND_SPARSE) == 0;
}

static int pg106_uapi_vm_bind_op_check(const struct pg106_uapi_vm_init *vm,
                                       const struct pg106_uapi_vm_bind_op *op)
{
    /* Only the two documented ops. The kernel would run op 2/3 as
     * MAP_SPARSE/UNMAP_SPARSE through its internal enum (uapi.h,
     * "bridge-only"); op >= 4 is -EINVAL there too (:1426-1428). */
    if (op->op != PG106_UAPI_VM_BIND_OP_MAP &&
        op->op != PG106_UAPI_VM_BIND_OP_UNMAP)
        return PG106_UAPI_EINVAL;

    if (pg106_uapi_vm_bind_op_is_bo_map(op)) {
        /* Non-sparse MAP looks its BO up first
         * (nouveau_uvmm_bind_job_submit, :1278-1283): handle 0 is
         * never a GEM handle -> -ENOENT. Then bind_validate_op
         * (:1068-1069): bo_offset page-aligned. */
        if (op->handle == 0)
            return PG106_UAPI_ENOENT;
        if (!pg106_uapi_page_aligned(op->bo_offset))
            return PG106_UAPI_EINVAL;
    }

    /* nouveau_uvmm_validate_range (nouveau_uvmm.c:1009-1021), for every
     * op: addr & ~PAGE_MASK, range & ~PAGE_MASK, then
     * drm_gpuvm_range_valid (no overflow, inside [0, 2^47), not
     * touching the kernel-managed node; bounds from drm_gpuvm_init,
     * :1944-1949). */
    if (!pg106_uapi_page_aligned(op->addr) ||
        !pg106_uapi_page_aligned(op->range))
        return PG106_UAPI_EINVAL;
    if (pg106_uapi_add_overflows(op->addr, op->range))
        return PG106_UAPI_EINVAL;
    if (op->addr + op->range > PG106_UAPI_VA_SPACE_END)
        return PG106_UAPI_EINVAL;
    if (vm->kernel_managed_size != 0 &&
        pg106_uapi_ranges_overlap(op->addr, op->range,
                                  vm->kernel_managed_addr,
                                  vm->kernel_managed_size))
        return PG106_UAPI_EINVAL;

    /* Bridge-only: a zero-length op (see uapi.h). */
    if (op->range == 0)
        return PG106_UAPI_EINVAL;

    return 0;
}

int pg106_uapi_vm_bind_op_check_map(const struct pg106_uapi_vm_bind_op *op,
                                    uint64_t bo_size)
{
    /* bind_validate_op MAP branch (nouveau_uvmm.c:1067-1076):
     *   op->gem.offset & ~PAGE_MASK               -> -EINVAL
     *   obj->size <= op->gem.offset               -> -EINVAL
     *   op->va.range > obj->size - op->gem.offset -> -EINVAL */
    if (op == NULL || !pg106_uapi_vm_bind_op_is_bo_map(op))
        return PG106_UAPI_EINVAL;
    if (!pg106_uapi_page_aligned(op->bo_offset))
        return PG106_UAPI_EINVAL;
    if (bo_size <= op->bo_offset)
        return PG106_UAPI_EINVAL;
    if (op->range > bo_size - op->bo_offset)
        return PG106_UAPI_EINVAL;
    return 0;
}

int pg106_uapi_vm_bind_validate(const struct pg106_uapi_vm_init *vm,
                                const struct pg106_uapi_vm_bind *req,
                                const struct pg106_uapi_vm_bind_op *ops,
                                const struct pg106_uapi_sync *waits,
                                const struct pg106_uapi_sync *sigs)
{
    uint32_t i;
    int ret;

    if (req == NULL)
        return PG106_UAPI_EINVAL;

    if ((req->op_count != 0 && ops == NULL) ||
        (req->wait_count != 0 && waits == NULL) ||
        (req->sig_count != 0 && sigs == NULL))
        return PG106_UAPI_EINVAL;

    /* nouveau_job_init (nouveau_sched.c:43-46, 56-59): a synchronous
     * job (no RUN_ASYNC -> args.sync, nouveau_uvmm.c:1708) may not
     * carry in_syncs or out_syncs. Other req->flags bits are not read
     * by the kernel. */
    if (!(req->flags & PG106_UAPI_VM_BIND_RUN_ASYNC) &&
        (req->wait_count != 0 || req->sig_count != 0))
        return PG106_UAPI_EINVAL;

    ret = pg106_uapi_syncs_check(waits, req->wait_count, sigs,
                                 req->sig_count);
    if (ret != 0)
        return ret;

    if (req->op_count != 0 && vm == NULL)
        return PG106_UAPI_EINVAL; /* no VM_INIT: nothing to bind into */
    for (i = 0; i < req->op_count; i++)
        if ((ret = pg106_uapi_vm_bind_op_check(vm, &ops[i])) != 0)
            return ret;

    return 0;
}

int pg106_uapi_exec_validate(uint32_t ib_max,
                             const struct pg106_uapi_exec *req,
                             const struct pg106_uapi_exec_push *pushes,
                             const struct pg106_uapi_sync *waits,
                             const struct pg106_uapi_sync *sigs)
{
    uint32_t i;

    if (req == NULL || (req->push_count != 0 && pushes == NULL) ||
        (req->wait_count != 0 && waits == NULL) ||
        (req->sig_count != 0 && sigs == NULL))
        return PG106_UAPI_EINVAL;

    /* nouveau_exec_ioctl_exec (nouveau_exec.c:385-389). */
    if (req->push_count > pg106_uapi_exec_push_max(ib_max))
        return PG106_UAPI_EINVAL;

    for (i = 0; i < req->push_count; i++) {
        const struct pg106_uapi_exec_push *p = &pushes[i];

        /* nouveau_exec_job_init (nouveau_exec.c:211-219):
         * va_len > NV50_DMA_PUSH_MAX_LENGTH -> -EINVAL. */
        if (p->va_len > PG106_UAPI_DMA_PUSH_MAX_LENGTH)
            return PG106_UAPI_EINVAL;
        /* Bridge-only GPFIFO-encoding rules (uapi.h): non-empty,
         * dword-aligned address and length. Unknown push flag bits
         * are ignored, as by the kernel. */
        if (p->va_len == 0)
            return PG106_UAPI_EINVAL;
        if ((p->va & 3u) != 0 || (p->va_len & 3u) != 0)
            return PG106_UAPI_EINVAL;
    }

    return pg106_uapi_syncs_check(waits, req->wait_count, sigs,
                                  req->sig_count);
}
