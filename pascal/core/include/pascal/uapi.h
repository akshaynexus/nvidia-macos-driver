// SPDX-License-Identifier: MIT
/*
 * pascal/uapi.h - the nouveau CS uapi subset NVK (Mesa) needs, as plain
 * C11 structs plus a validation API for the macOS bridge.
 *
 * Struct layouts and constants are byte-identical to Linux
 * include/uapi/drm/nouveau_drm.h (as vendored in Mesa,
 * third_party/mesa/include/drm-uapi/nouveau_drm.h); the _Static_asserts
 * below pin the sizes/offsets. The validation rules are the kernel's
 * (drivers/gpu/drm/nouveau/nouveau_{uvmm,exec,sched}.c, Linux 7.4-rc1
 * d48723711bae). Full inventory with citations:
 * research/evidence/NOUVEAU-UAPI.md.
 *
 * Licensing: MIT. nouveau_drm.h and the nouveau kernel files the rules
 * come from are MIT-licensed (SPDX MIT / the notice below), and this
 * file and uapi.c are linked into the MIT userspace bridge
 * (pascal/bridge), so they carry the same licence. The struct layouts
 * and constants are copied from nouveau_drm.h, whose notice is:
 *
 *   Copyright 2005 Stephane Marchesin.
 *   All Rights Reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a
 *   copy of this software and associated documentation files (the "Software"),
 *   to deal in the Software without restriction, including without limitation
 *   the rights to use, copy, modify, merge, publish, distribute, sublicense,
 *   and/or sell copies of the Software, and to permit persons to whom the
 *   Software is furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice (including the next
 *   paragraph) shall be included in all copies or substantial portions of the
 *   Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 *   VA LINUX SYSTEMS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 *   OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 *   ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 *   OTHER DEALINGS IN THE SOFTWARE.
 *
 * Everything here is pure C11, libc only: these functions validate
 * request arrays in host memory and never touch hardware. Pointer
 * fields of the wire structs (wait_ptr/sig_ptr/op_ptr/push_ptr) are
 * NOT dereferenced by the core - the bridge copies the arrays in and
 * passes them as plain arguments, as the kernel does with u_memcpya()
 * (nouveau_exec.c nouveau_exec_ucopy, nouveau_uvmm.c
 * nouveau_uvmm_vm_bind_ucopy).
 */
#ifndef PASCAL_UAPI_H
#define PASCAL_UAPI_H

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Constants, verbatim from include/uapi/drm/nouveau_drm.h             */
/* ------------------------------------------------------------------ */

/* GETPARAM ids NVK queries (nouveau_drm.h:36-76). The bridge must
 * answer these; which ones and what values are documented in
 * NOUVEAU-UAPI.md section 4. */
#define PG106_UAPI_GETPARAM_PCI_VENDOR       3u
#define PG106_UAPI_GETPARAM_PCI_DEVICE       4u
#define PG106_UAPI_GETPARAM_GRAPH_UNITS     13u
#define PG106_UAPI_GETPARAM_PTIMER_TIME     14u
#define PG106_UAPI_GETPARAM_EXEC_PUSH_MAX   17u
#define PG106_UAPI_GETPARAM_VRAM_BAR_SIZE   18u
#define PG106_UAPI_GETPARAM_VRAM_USED       19u
#define PG106_UAPI_GETPARAM_HAS_VMA_TILEMODE 20u

/* drm_nouveau_sync @flags (nouveau_drm.h:237-239). */
#define PG106_UAPI_SYNC_SYNCOBJ             0x0u
#define PG106_UAPI_SYNC_TIMELINE_SYNCOBJ    0x1u
#define PG106_UAPI_SYNC_TYPE_MASK           0xFu

/* drm_nouveau_vm_bind_op @op (nouveau_drm.h:300-301). */
#define PG106_UAPI_VM_BIND_OP_MAP           0x0u
#define PG106_UAPI_VM_BIND_OP_UNMAP         0x1u

/* drm_nouveau_vm_bind_op @flags (nouveau_drm.h:311). Bits [7:0] carry
 * the PTE kind for a non-sparse MAP: NVK puts va->pte_kind there
 * (mesa nvkmd_nouveau_va.c:229, nvkmd_nouveau_ctx.c:395) and the kernel
 * reads op->flags & 0xff (nouveau_uvmm.c:1399). The kernel ignores all
 * other bits. */
#define PG106_UAPI_VM_BIND_SPARSE           (1u << 8)
#define PG106_UAPI_VM_BIND_KIND_MASK        0xFFu

/* drm_nouveau_vm_bind @flags (nouveau_drm.h:357). */
#define PG106_UAPI_VM_BIND_RUN_ASYNC        0x1u

/* drm_nouveau_exec_push @flags (nouveau_drm.h:399). */
#define PG106_UAPI_EXEC_PUSH_NO_PREFETCH    0x1u

/*
 * Kernel-side limits the validators enforce. Sources:
 * - VA space: NOUVEAU_VA_SPACE_BITS 47 / START 0x0 / END 1<<47
 *   (drivers/gpu/drm/nouveau/nouveau_uvmm.c:35-37).
 * - Single push length: NV50_DMA_PUSH_MAX_LENGTH 0x7fffff
 *   (drivers/gpu/drm/nouveau/nouveau_dma.h:48), checked per push in
 *   nouveau_exec_job_init (nouveau_exec.c:211-219).
 * - Page size: the kernel validates addr/range/bo_offset against
 *   PAGE_MASK (nouveau_uvmm.c:1009-1021, 1068); on the reference
 *   x86-64 platform that is 4 KiB, which this model assumes.
 */
#define PG106_UAPI_VA_SPACE_END             (1ULL << 47)
#define PG106_UAPI_DMA_PUSH_MAX_LENGTH      0x7FFFFFu
#define PG106_UAPI_PAGE_SHIFT               12
#define PG106_UAPI_PAGE_SIZE                (1u << PG106_UAPI_PAGE_SHIFT)

/*
 * Error returns: negative errno-style values, mirroring what the
 * Linux kernel ioctl would return (so the bridge can map 1:1). Aliased
 * here so the header does not depend on <errno.h>.
 */
#define PG106_UAPI_EINVAL     (-22)  /* invalid argument           */
#define PG106_UAPI_ENOENT     (-2)   /* unknown handle/channel     */
#define PG106_UAPI_EEXIST     (-17)  /* sparse region overlap      */
#define PG106_UAPI_ENOSPC     (-28)  /* MAP crosses a sparse region */
#define PG106_UAPI_ENOSYS     (-38)  /* uapi not initialised       */
#define PG106_UAPI_EOPNOTSUPP (-95)  /* unsupported wait sync type */

/* ------------------------------------------------------------------ */
/* Wire structs, byte-identical to include/uapi/drm/nouveau_drm.h      */
/* ------------------------------------------------------------------ */

/* drm_nouveau_getparam (nouveau_drm.h:78-81). */
struct pg106_uapi_getparam {
    uint64_t param;
    uint64_t value;
};

/* drm_nouveau_sync (nouveau_drm.h:230-251). */
struct pg106_uapi_sync {
    uint32_t flags;          /* PG106_UAPI_SYNC_* / TYPE_MASK      */
    uint32_t handle;         /* drm syncobj handle                 */
    uint64_t timeline_value; /* only for TIMELINE_SYNCOBJ          */
};

/* drm_nouveau_vm_init (nouveau_drm.h:263-274). Kernel-managed region
 * is reserved for kernel allocations; UMD VM_BIND ops must stay out of
 * it (drm_gpuvm_init, nouveau_uvmm.c:1944-1949). */
struct pg106_uapi_vm_init {
    uint64_t kernel_managed_addr;
    uint64_t kernel_managed_size;
};

/* drm_nouveau_vm_bind_op (nouveau_drm.h:282-334). */
struct pg106_uapi_vm_bind_op {
    uint32_t op;        /* PG106_UAPI_VM_BIND_OP_*                 */
    uint32_t flags;     /* SPARSE bit 8; kind in [7:0] (MAP)       */
    uint32_t handle;    /* GEM BO handle (non-sparse MAP only)     */
    uint32_t pad;       /* "should be 0"; kernel never reads it    */
    uint64_t addr;      /* GPU VA, page-aligned                    */
    uint64_t bo_offset; /* offset into the BO, page-aligned (MAP)  */
    uint64_t range;     /* bytes, page-aligned, > 0                */
};

/* drm_nouveau_vm_bind (nouveau_drm.h:339-378). The *_ptr fields are
 * user pointers on the wire; the validators take the arrays directly. */
struct pg106_uapi_vm_bind {
    uint32_t op_count;
    uint32_t flags;      /* PG106_UAPI_VM_BIND_RUN_ASYNC or 0      */
    uint32_t wait_count;
    uint32_t sig_count;
    uint64_t wait_ptr;
    uint64_t sig_ptr;
    uint64_t op_ptr;
};

/* drm_nouveau_exec_push (nouveau_drm.h:386-400). */
struct pg106_uapi_exec_push {
    uint64_t va;     /* GPU VA of the pushbuffer mapping, 4-aligned */
    uint32_t va_len; /* bytes, 4-aligned, <= DMA_PUSH_MAX_LENGTH    */
    uint32_t flags;  /* EXEC_PUSH_NO_PREFETCH or 0                  */
};

/* drm_nouveau_exec (nouveau_drm.h:405-434). */
struct pg106_uapi_exec {
    uint32_t channel;
    uint32_t push_count;
    uint32_t wait_count;
    uint32_t sig_count;
    uint64_t wait_ptr;
    uint64_t sig_ptr;
    uint64_t push_ptr;
};

/* Size/offset pins: if a compiler ever pads differently from the Linux
 * uapi, these fail at compile time before a bridge can ship. */
/* C11 spelling: _Static_assert (static_assert without a header is C23). */
#define PG106_UAPI_STATICASSERT(cond, name) \
    _Static_assert(cond, name)
PG106_UAPI_STATICASSERT(sizeof(struct pg106_uapi_getparam) == 16, "getparam layout");
PG106_UAPI_STATICASSERT(sizeof(struct pg106_uapi_sync) == 16, "sync layout");
PG106_UAPI_STATICASSERT(sizeof(struct pg106_uapi_vm_init) == 16, "vm_init layout");
PG106_UAPI_STATICASSERT(sizeof(struct pg106_uapi_vm_bind_op) == 40, "vm_bind_op layout");
PG106_UAPI_STATICASSERT(sizeof(struct pg106_uapi_vm_bind) == 40, "vm_bind layout");
PG106_UAPI_STATICASSERT(sizeof(struct pg106_uapi_exec_push) == 16, "exec_push layout");
PG106_UAPI_STATICASSERT(sizeof(struct pg106_uapi_exec) == 40, "exec layout");
PG106_UAPI_STATICASSERT(offsetof(struct pg106_uapi_sync, handle) == 4, "sync.handle @4");
PG106_UAPI_STATICASSERT(offsetof(struct pg106_uapi_vm_bind, op_ptr) == 32, "vm_bind.op_ptr @32");
PG106_UAPI_STATICASSERT(offsetof(struct pg106_uapi_exec, push_ptr) == 32, "exec.push_ptr @32");
#undef PG106_UAPI_STATICASSERT

/* ------------------------------------------------------------------ */
/* Validation API - the kernel-side rules, executable                  */
/* ------------------------------------------------------------------ */

/*
 * Max EXEC pushes per submission: ib_max/2 - 1 (or 0 if ib_max <= 1),
 * from nouveau_exec_push_max_from_ib_max
 * (drivers/gpu/drm/nouveau/nouveau_exec.h:51-58). ib_max is the
 * channel's GPFIFO ring capacity; the bridge reports the same number
 * through NOUVEAU_GETPARAM_EXEC_PUSH_MAX.
 */
uint32_t pg106_uapi_exec_push_max(uint32_t ib_max);

/* Sync type gate for a WAIT entry: type must be SYNCOBJ or
 * TIMELINE_SYNCOBJ, else -EOPNOTSUPP (sync_find_fence,
 * nouveau_sched.c:147-149). Signal entries with a bad type are -EINVAL
 * instead (nouveau_job_fence_attach_prepare, nouveau_sched.c:215-219);
 * the validators below apply each rule to its own array. */
int pg106_uapi_sync_check(const struct pg106_uapi_sync *s);

/*
 * DRM_IOCTL_NOUVEAU_VM_INIT: check_add_overflow(addr, size) and
 * kernel_managed_end <= NOUVEAU_VA_SPACE_END
 * (nouveau_uvmm_ioctl_vm_init, nouveau_uvmm.c:1912-1918).
 */
int pg106_uapi_vm_init_validate(const struct pg106_uapi_vm_init *init);

/*
 * One non-sparse MAP op against its BO's size, per bind_validate_op
 * (nouveau_uvmm.c:1064-1082): bo_offset page-aligned, offset < bo_size,
 * range <= bo_size - offset. Returns 0 or PG106_UAPI_EINVAL (also for an
 * op that is not a non-sparse MAP).
 */
int pg106_uapi_vm_bind_op_check_map(const struct pg106_uapi_vm_bind_op *op,
                                    uint64_t bo_size);

/* True for a MAP without SPARSE: the only op kind that references a BO
 * (handle/bo_offset) and carries a PTE kind (nouveau_uvmm.c:1640-1641
 * bind_job_op_from_uop, :1278-1283 lookup). */
int pg106_uapi_vm_bind_op_is_bo_map(const struct pg106_uapi_vm_bind_op *op);

/*
 * DRM_IOCTL_NOUVEAU_VM_BIND, stateless rules. `vm` is the VM_INIT
 * request for the same client (needed for the kernel-managed-region
 * exclusion); it may be NULL only when `req` has no ops. `ops`,
 * `waits` and `sigs` are the arrays the bridge copied in; each pointer
 * must be non-NULL when the corresponding count is non-zero.
 *
 * Enforced, kernel rule (Linux 7.4-rc1):
 *  - without RUN_ASYNC no waits/sigs (nouveau_job_init,
 *    nouveau_sched.c:43-46, 56-59: -EINVAL); other request flag bits
 *    are ignored, as by the kernel (only RUN_ASYNC is read,
 *    nouveau_uvmm.c:1708)
 *  - wait type -EOPNOTSUPP, signal type -EINVAL (see sync_check)
 *  - per op, in order (nouveau_uvmm_bind_job_submit, :1271-1297):
 *    non-sparse MAP with handle 0 -> -ENOENT (drm_gem_object_lookup
 *    never finds handle 0); non-sparse MAP bo_offset page-aligned
 *    (bind_validate_op); addr and range page-aligned, inside the 2^47
 *    VA space, no overflow, not touching the kernel-managed region
 *    (nouveau_uvmm_validate_range, :1009-1021, drm_gpuvm_range_valid
 *    with the bounds from drm_gpuvm_init, :1944-1949)
 *  - op flags: SPARSE selects MAP_SPARSE/UNMAP_SPARSE; [7:0] is the PTE
 *    kind; nothing else is checked (kernel reads only these). Sparse
 *    MAPs ignore handle/bo_offset, which NVK leaves 0
 *    (nvkmd_nouveau_va.c:144-151). pad is never read by the kernel and
 *    is not checked.
 *
 * Enforced, bridge-only (kernel accepts, documented divergence):
 *  - op other than MAP (0) / UNMAP (1) is -EINVAL. The kernel copies an
 *    unknown op into its internal enum unchanged (bind_job_op_from_uop
 *    default case, :1647-1649), where 2 and 3 alias OP_MAP_SPARSE and
 *    OP_UNMAP_SPARSE (:46-51), so it runs uapi op 2/3 as sparse ops and
 *    rejects only op >= 4 (:1426-1428). That aliasing is not part of
 *    the documented uapi (nouveau_drm.h:300-301 defines 0 and 1 only).
 *  - range == 0 is -EINVAL. The kernel's range check passes a zero
 *    range, but a zero-length gpuva has no defined meaning (drm_gpuva
 *    interval nodes end at addr + range - 1) and NVK never sends one.
 *
 * NOT enforced here (per-client state, pascal/bridge does these):
 *  - handle -> BO existence (-ENOENT) and the BO-size cross-check
 *    (pg106_uapi_vm_bind_op_check_map)
 *  - sparse-region rules: MAP_SPARSE overlapping a region -ENOSPC/-EEXIST,
 *    UNMAP_SPARSE not matching a region exactly -ENOENT, a MAP partly
 *    inside a region -ENOSPC (bind_validate_map_common :1105-1157,
 *    region insert :280-292, UNMAP_SPARSE find :1326-1333)
 *  - the PTE kind's validity for the chip (checked by nvkm when the job
 *    runs, not at ioctl time)
 */
int pg106_uapi_vm_bind_validate(const struct pg106_uapi_vm_init *vm,
                                const struct pg106_uapi_vm_bind *req,
                                const struct pg106_uapi_vm_bind_op *ops,
                                const struct pg106_uapi_sync *waits,
                                const struct pg106_uapi_sync *sigs);

/*
 * DRM_IOCTL_NOUVEAU_EXEC, stateless rules. `ib_max` is the channel's
 * GPFIFO ring capacity (nouveau_exec_ioctl_exec computes push_max from
 * chan->chan.gpfifo.max, nouveau_exec.c:385).
 *
 * Enforced, kernel rule:
 *  - push_count <= pg106_uapi_exec_push_max(ib_max)
 *    (nouveau_exec.c:385-389)
 *  - per push va_len <= PG106_UAPI_DMA_PUSH_MAX_LENGTH
 *    (nouveau_exec_job_init, nouveau_exec.c:211-219)
 *  - wait type -EOPNOTSUPP, signal type -EINVAL (see sync_check)
 *  - push flags: only NO_PREFETCH is read; other bits are ignored, as by
 *    the kernel (nouveau_exec_job_run)
 *
 * Enforced, bridge-only (the kernel does not check; these protect the
 * GPFIFO encoding rather than mirror an ioctl rule):
 *  - va and va_len 4-byte aligned: a GPFIFO entry stores the address
 *    from bit 2 and the length as a dword count (clc06f.h
 *    NVC06F_GP_ENTRY0_GET, NVC06F_GP_ENTRY1_LENGTH), so low bits would
 *    be dropped silently; Mesa asserts the same (nvkmd_nouveau_ctx.c:202)
 *  - va_len > 0: with LENGTH 0 the entry is a control entry whose
 *    OPCODE shares bits [7:0] with GET_HI (clc06f.h
 *    NVC06F_GP_ENTRY1_OPCODE), not an empty fetch; NVK never submits
 *    one (nvk_queue.c:304-305 skips range == 0)
 *
 * Semantics the bridge must implement (not validatable statelessly):
 *  - exec runs asynchronously on the channel's scheduler; all waits
 *    complete before the pushes run and all signals fire after the
 *    job's done fence (nouveau_sched.c nouveau_job_submit)
 *  - the channel must exist (kernel: -ENOENT unknown, -ENODEV killed,
 *    nouveau_exec.c:373-381)
 */
int pg106_uapi_exec_validate(uint32_t ib_max,
                             const struct pg106_uapi_exec *req,
                             const struct pg106_uapi_exec_push *pushes,
                             const struct pg106_uapi_sync *waits,
                             const struct pg106_uapi_sync *sigs);

#endif /* PASCAL_UAPI_H */
