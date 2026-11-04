// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
/*
 * pascal/uapi.h - the nouveau CS uapi subset NVK (Mesa) needs, as plain
 * C11 structs plus a validation API for the macOS bridge.
 *
 * Struct layouts are byte-identical to Linux
 * include/uapi/drm/nouveau_drm.h (torvalds/linux master, fetched to
 * /tmp/pascal-ref/uapi/nouveau_drm.h; _Static_asserts below pin the
 * sizes/offsets). Only what NVK actually issues is modelled; the full
 * inventory with file+line citations lives in
 * research/evidence/NOUVEAU-UAPI.md (Phase 4 gate document).
 *
 * Licensing: the struct layouts and constants are copied from a Linux
 * uapi header, which is GPL-2.0 WITH the Linux-syscall-note exception
 * (the note allows unrestricted userspace use of the syscall ABI). The
 * core stays GPL-2.0-only like the rest of the kext tree; the exception
 * note in the SPDX line keeps userspace (NVK is MIT) free to speak this
 * shape through the bridge.
 *
 * Everything here is pure C11, libc only: these functions validate
 * request arrays in host memory and never touch hardware. Pointer
 * fields of the wire structs (wait_ptr/sig_ptr/op_ptr/push_ptr) are
 * NOT dereferenced by the core - the bridge copies the arrays in and
 * passes them as plain arguments, mirroring what the Linux kernel does
 * with u_memcpya() (drivers/gpu/drm/nouveau/nouveau_exec.c).
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

/* drm_nouveau_vm_bind_op @flags (nouveau_drm.h:311). */
#define PG106_UAPI_VM_BIND_SPARSE           (1u << 8)

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
 *   PAGE_MASK (nouveau_uvmm.c:1008-1017, 1053); on the reference
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
#define PG106_UAPI_ENOSYS     (-38)  /* uapi not initialised       */
#define PG106_UAPI_EOPNOTSUPP (-95)  /* unsupported sync type      */

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
 * it (drm_gpuvm_init, nouveau_uvmm.c:1943-1948). */
struct pg106_uapi_vm_init {
    uint64_t kernel_managed_addr;
    uint64_t kernel_managed_size;
};

/* drm_nouveau_vm_bind_op (nouveau_drm.h:282-334). */
struct pg106_uapi_vm_bind_op {
    uint32_t op;        /* PG106_UAPI_VM_BIND_OP_*                 */
    uint32_t flags;     /* PG106_UAPI_VM_BIND_SPARSE or 0          */
    uint32_t handle;    /* GEM BO handle (MAP only)                */
    uint32_t pad;       /* uapi doc: "should be 0"; bridge enforces */
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
 * (drivers/gpu/drm/nouveau/nouveau_exec.h:45-52). ib_max is the
 * channel's GPFIFO ring capacity; the bridge reports the same number
 * through NOUVEAU_GETPARAM_EXEC_PUSH_MAX.
 */
uint32_t pg106_uapi_exec_push_max(uint32_t ib_max);

/* Sync type gate shared by both validators: type must be SYNCOBJ or
 * TIMELINE_SYNCOBJ (kernel: nouveau_sched.c rejects other types with
 * -EOPNOTSUPP). Returns PG106_UAPI_OK (0) or an error. */
int pg106_uapi_sync_check(const struct pg106_uapi_sync *s);

/*
 * DRM_IOCTL_NOUVEAU_VM_INIT: check_add_overflow(addr, size) and
 * kernel_managed_end <= NOUVEAU_VA_SPACE_END
 * (nouveau_uvmm.c:1908-1916).
 */
int pg106_uapi_vm_init_validate(const struct pg106_uapi_vm_init *init);

/*
 * One MAP op against its BO's size, per bind_validate_op
 * (nouveau_uvmm.c:1048-1062): bo_offset page-aligned, offset < bo_size,
 * range <= bo_size - offset. Returns 0 or PG106_UAPI_EINVAL.
 */
int pg106_uapi_vm_bind_op_check_map(const struct pg106_uapi_vm_bind_op *op,
                                    uint64_t bo_size);

/*
 * DRM_IOCTL_NOUVEAU_VM_BIND, stateless rules. `vm` is the VM_INIT
 * request for the same client (needed for the kernel-managed-region
 * exclusion); it may be NULL only when `req` has no ops. `ops`,
 * `waits` and `sigs` are the arrays the bridge copied in; each pointer
 * must be non-NULL when the corresponding count is non-zero (and may
 * be NULL otherwise).
 *
 * Enforced (kernel rule unless noted):
 *  - op flags 0 or RUN_ASYNC; anything else is rejected by the bridge
 *    (the Linux kernel currently ignores unknown bits - stricter here,
 *    see NOUVEAU-UAPI.md section 2.3)
 *  - without RUN_ASYNC no waits/sigs are accepted (nouveau_sched.c
 *    nouveau_job_init: in_sync/out_sync with job->sync -> -EINVAL)
 *  - per-op: op is MAP or UNMAP; pad == 0 (bridge policy, uapi doc);
 *    addr/range page-aligned and range > 0 (nouveau_uvmm_validate_range,
 *    nouveau_uvmm.c:1008-1017); addr..addr+range inside the VA space
 *    and not overlapping the kernel-managed region (drm_gpuvm_range_valid
 *    via drm_gpuvm_init bounds, nouveau_uvmm.c:1943-1948)
 *  - MAP: handle != 0 and pg106_uapi_vm_bind_op_check_map rules
 *    (bo-size cross-check is the caller's job, see above)
 *
 * NOT enforced here (needs per-client state, bridge-side):
 *  - UNMAP+SPARSE must match a sparse region exactly, and unmap/sparse
 *    overlaps between queued jobs (nouveau_uvmm.c:34-56 doc comment,
 *    bind_validate_map_sparse / bind_validate_map_common)
 *  - handle -> BO existence (-ENOENT on the kernel)
 */
int pg106_uapi_vm_bind_validate(const struct pg106_uapi_vm_init *vm,
                                const struct pg106_uapi_vm_bind *req,
                                const struct pg106_uapi_vm_bind_op *ops,
                                const struct pg106_uapi_sync *waits,
                                const struct pg106_uapi_sync *sigs);

/*
 * DRM_IOCTL_NOUVEAU_EXEC, stateless rules. `ib_max` is the channel's
 * GPFIFO ring capacity (as on Linux: nouveau_exec_ioctl_exec ->
 * push_max from chan->chan.gpfifo.max, nouveau_exec.c:384-390).
 *
 * Enforced (kernel rule unless noted):
 *  - push_count <= pg106_uapi_exec_push_max(ib_max)
 *    (nouveau_exec.c:385-390)
 *  - per push: va_len <= PG106_UAPI_DMA_PUSH_MAX_LENGTH
 *    (nouveau_exec.c:211-219); va_len > 0 (NVK never sends empty
 *    pushes - nvk_queue.c skips range == 0; bridge rejects them);
 *    va and va_len 4-byte aligned (PBDMA fetches dwords; Mesa asserts
 *    exactly this in nvkmd_nouveau_ctx.c:199); flags only
 *    EXEC_PUSH_NO_PREFETCH (bridge policy; kernel ignores unknown bits)
 *  - waits/sigs: pg106_uapi_sync_check per entry
 *
 * Semantics the bridge must implement (not validatable statelessly):
 *  - exec runs asynchronously on the channel's scheduler; all waits
 *    complete before the pushes are submitted to the PBDMA and all
 *    signals fire after the job's done fence
 *    (nouveau_sched.c nouveau_job_submit -> nouveau_job_add_deps /
 *    nouveau_job_fence_attach)
 *  - channel must have been created through this uapi
 *    (kernel: -ENOENT for unknown channel, -ENODEV killed,
 *    nouveau_exec.c:373-381)
 */
int pg106_uapi_exec_validate(uint32_t ib_max,
                             const struct pg106_uapi_exec *req,
                             const struct pg106_uapi_exec_push *pushes,
                             const struct pg106_uapi_sync *waits,
                             const struct pg106_uapi_sync *sigs);

#endif /* PASCAL_UAPI_H */
