// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
/*
 * pascal/uapi.c - validation for the nouveau CS uapi subset NVK needs.
 *
 * Every rule implemented here cites its Linux kernel origin in the
 * header (pascal/uapi.h) and in research/evidence/NOUVEAU-UAPI.md.
 * The reference kernel sources live in /tmp/pascal-ref/linux-nouveau/
 * (torvalds/linux master, fetched 2026-11-05) and the uapi header in
 * /tmp/pascal-ref/uapi/nouveau_drm.h.
 *
 * Pure C11, libc only, no hardware access.
 */
#include "pascal/uapi.h"

#define PG106_UAPI_PAGE_MASK ((uint64_t)PG106_UAPI_PAGE_SIZE - 1)

static int pg106_uapi_page_aligned(uint64_t v)
{
    return (v & PG106_UAPI_PAGE_MASK) == 0;
}

/* Half-open [a, a+alen) vs [b, b+blen) overlap, overflow-safe. */
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
    /* nouveau_exec_push_max_from_ib_max (nouveau_exec.h:45-52):
     * half the GPFIFO ring, minus one slot for the HW fence. */
    return ib_max > 1 ? ib_max / 2 - 1 : 0;
}

int pg106_uapi_sync_check(const struct pg106_uapi_sync *s)
{
    uint32_t stype;

    if (s == NULL)
        return PG106_UAPI_EINVAL;

    stype = s->flags & PG106_UAPI_SYNC_TYPE_MASK;
    if (stype != PG106_UAPI_SYNC_SYNCOBJ &&
        stype != PG106_UAPI_SYNC_TIMELINE_SYNCOBJ)
        return PG106_UAPI_EOPNOTSUPP; /* nouveau_sched.c ~line 145 */

    return 0;
}

int pg106_uapi_vm_init_validate(const struct pg106_uapi_vm_init *init)
{
    if (init == NULL)
        return PG106_UAPI_EINVAL;

    /* nouveau_uvmm_ioctl_vm_init (nouveau_uvmm.c:1908-1916):
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

static int pg106_uapi_vm_bind_op_check(const struct pg106_uapi_vm_init *vm,
                                       const struct pg106_uapi_vm_bind_op *op)
{
    uint64_t end;

    switch (op->op) {
    case PG106_UAPI_VM_BIND_OP_MAP:
        if (op->handle == 0)
            return PG106_UAPI_EINVAL; /* bridge stand-in for -ENOENT */
        if (!pg106_uapi_page_aligned(op->bo_offset))
            return PG106_UAPI_EINVAL; /* bind_validate_op,
                                         nouveau_uvmm.c:1053-1054 */
        break;
    case PG106_UAPI_VM_BIND_OP_UNMAP:
        break;
    default:
        return PG106_UAPI_EINVAL;
    }

    /* drm_nouveau_vm_bind_op @pad: "32 bit padding, should be 0"
     * (nouveau_drm.h:316-319). The Linux kernel does not check; the
     * bridge does, so garbage from a confused UMD cannot sneak into a
     * future flag. */
    if (op->pad != 0)
        return PG106_UAPI_EINVAL;

    if (op->flags != 0 && op->flags != PG106_UAPI_VM_BIND_SPARSE)
        return PG106_UAPI_EINVAL;

    /* nouveau_uvmm_validate_range (nouveau_uvmm.c:1008-1017):
     * addr & ~PAGE_MASK and range & ~PAGE_MASK are rejected. range 0
     * is rejected as well: an empty op can never satisfy the sparse
     * exact-match rules and NVK never sends one. */
    if (!pg106_uapi_page_aligned(op->addr) ||
        !pg106_uapi_page_aligned(op->range) || op->range == 0)
        return PG106_UAPI_EINVAL;

    if (pg106_uapi_add_overflows(op->addr, op->range))
        return PG106_UAPI_EINVAL;
    end = op->addr + op->range;

    /* drm_gpuvm_range_valid via the drm_gpuvm_init bounds
     * (nouveau_uvmm.c:1943-1948): ops stay inside [0, VA_SPACE_END)
     * and out of the kernel-managed region. */
    if (end > PG106_UAPI_VA_SPACE_END)
        return PG106_UAPI_EINVAL;
    if (vm != NULL && vm->kernel_managed_size != 0 &&
        pg106_uapi_ranges_overlap(op->addr, op->range,
                                  vm->kernel_managed_addr,
                                  vm->kernel_managed_size))
        return PG106_UAPI_EINVAL;

    return 0;
}

int pg106_uapi_vm_bind_op_check_map(const struct pg106_uapi_vm_bind_op *op,
                                    uint64_t bo_size)
{
    /* bind_validate_op MAP branch (nouveau_uvmm.c:1051-1061):
     *   op->gem.offset & ~PAGE_MASK           -> -EINVAL
     *   obj->size <= op->gem.offset           -> -EINVAL
     *   op->va.range > obj->size - op->gem.offset -> -EINVAL */
    if (op == NULL || op->op != PG106_UAPI_VM_BIND_OP_MAP)
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

    if (req->flags != 0 && req->flags != PG106_UAPI_VM_BIND_RUN_ASYNC)
        return PG106_UAPI_EINVAL; /* see header note on unknown bits */

    if ((req->op_count != 0 && ops == NULL) ||
        (req->wait_count != 0 && waits == NULL) ||
        (req->sig_count != 0 && sigs == NULL))
        return PG106_UAPI_EINVAL;

    /* nouveau_job_init (nouveau_sched.c:43-46, 56-59): a synchronous
     * job (VM_BIND without RUN_ASYNC -> job->sync) may not carry
     * in_syncs or out_syncs (-EINVAL). */
    if (!(req->flags & PG106_UAPI_VM_BIND_RUN_ASYNC) &&
        (req->wait_count != 0 || req->sig_count != 0))
        return PG106_UAPI_EINVAL;

    for (i = 0; i < req->wait_count; i++)
        if ((ret = pg106_uapi_sync_check(&waits[i])) != 0)
            return ret;
    for (i = 0; i < req->sig_count; i++)
        if ((ret = pg106_uapi_sync_check(&sigs[i])) != 0)
            return ret;

    for (i = 0; i < req->op_count; i++) {
        if (vm == NULL)
            return PG106_UAPI_EINVAL;
        if ((ret = pg106_uapi_vm_bind_op_check(vm, &ops[i])) != 0)
            return ret;
    }

    return 0;
}

int pg106_uapi_exec_validate(uint32_t ib_max,
                             const struct pg106_uapi_exec *req,
                             const struct pg106_uapi_exec_push *pushes,
                             const struct pg106_uapi_sync *waits,
                             const struct pg106_uapi_sync *sigs)
{
    uint32_t i;
    int ret;

    if (req == NULL || (req->push_count != 0 && pushes == NULL) ||
        (req->wait_count != 0 && waits == NULL) ||
        (req->sig_count != 0 && sigs == NULL))
        return PG106_UAPI_EINVAL;

    /* nouveau_exec_ioctl_exec (nouveau_exec.c:384-390). */
    if (req->push_count > pg106_uapi_exec_push_max(ib_max))
        return PG106_UAPI_EINVAL;

    for (i = 0; i < req->push_count; i++) {
        const struct pg106_uapi_exec_push *p = &pushes[i];

        /* nouveau_exec_job_init (nouveau_exec.c:211-219):
         * va_len > NV50_DMA_PUSH_MAX_LENGTH -> -EINVAL. */
        if (p->va_len > PG106_UAPI_DMA_PUSH_MAX_LENGTH)
            return PG106_UAPI_EINVAL;
        /* NVK never submits empty pushes (nvk_queue.c skips
         * push->range == 0); the bridge rejects them outright. */
        if (p->va_len == 0)
            return PG106_UAPI_EINVAL;
        /* "This is the hardware limit on all current GPUs" - Mesa
         * asserts 4-byte alignment of va and va_len
         * (nvkmd_nouveau_ctx.c:199); the PBDMA fetches dwords. */
        if ((p->va & 3u) != 0 || (p->va_len & 3u) != 0)
            return PG106_UAPI_EINVAL;
        if (p->flags != 0 && p->flags != PG106_UAPI_EXEC_PUSH_NO_PREFETCH)
            return PG106_UAPI_EINVAL; /* bridge policy */
    }

    for (i = 0; i < req->wait_count; i++)
        if ((ret = pg106_uapi_sync_check(&waits[i])) != 0)
            return ret;
    for (i = 0; i < req->sig_count; i++)
        if ((ret = pg106_uapi_sync_check(&sigs[i])) != 0)
            return ret;

    return 0;
}
