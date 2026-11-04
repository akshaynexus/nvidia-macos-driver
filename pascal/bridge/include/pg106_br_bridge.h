// SPDX-License-Identifier: MIT
/*
 * pg106_br_bridge.h - the userspace-side bridge backend NVK links
 * against: a pg106_br_ API shaped like Mesa's nvkmd interface
 * (mesa src/nouveau/vulkan/nvkmd/nvkmd.h) but transporting the
 * nouveau CS uapi subset over an abstract channel instead of ioctls.
 *
 * Object mapping vs nvkmd (file:line in mesa 17ca6174):
 *   nvkmd_pdev  -> (none; the kext is discovered by the future
 *                  UserClient transport, not by drmDevice scanning)
 *   nvkmd_dev   -> pg106_br_dev      (open = version handshake)
 *   nvkmd_dev_ops.alloc_mem    -> pg106_br_bo_alloc
 *   nvkmd_mem_ops.map/unmap    -> pg106_br_bo_map/unmap
 *   nvkmd_dev_ops.alloc_va     -> pg106_br_va_alloc
 *   nvkmd_va_ops.bind_mem/unbind -> pg106_br_ctx_bind (MAP/UNMAP ops,
 *                  batched + flushed like nvkmd_nouveau_bind_ctx,
 *                  nvkmd_nouveau_ctx.c:306-365)
 *   nvkmd_ctx_ops.exec/flush/signal/wait -> pg106_br_ctx_exec /
 *                  pg106_br_ctx_flush / pg106_br_ctx_*_signal / wait
 *   nvkmd_dev_ops.create_ctx   -> pg106_br_ctx_create
 *
 * Return convention: 0 or negative errno, mirroring the core
 * validators (PG106_UAPI_*), so validator failures surface as ordinary
 * op errors. A validator rejection never reaches the transport.
 *
 * C11, libc only. MIT licensed (bridge to Mesa-side code); it speaks
 * GPL-2.0 syscal-note shapes from pascal/core headers at arm's length.
 */
#ifndef PG106_BR_BRIDGE_H
#define PG106_BR_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#include "pg106_br_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Transport: the only kext-facing seam. The mock records frames in
 * host memory; the future UserClient transport will implement call()
 * with IOConnectCallMethod (NOUVEAU-UAPI.md section 4.1) and
 * map_cookie() with IOConnectMapMemory64.
 */
struct pg106_br_transport {
    /* One request/response round trip. Implementations must fill
     * *resp (header; seq/op/status) and at most resp_cap bytes of
     * response payload (*resp_len_out). Returns 0 or negative errno
     * for channel-level failures. */
    int (*call)(struct pg106_br_transport *tr,
                const struct pg106_br_frame *req, const void *req_payload,
                struct pg106_br_frame *resp, void *resp_payload,
                size_t resp_cap, size_t *resp_len_out);

    /* Optional: materialize the CPU mapping for a BO map cookie
     * returned by OP_BO_MAP. NULL = mapping unsupported. */
    int (*map_cookie)(struct pg106_br_transport *tr, uint64_t cookie,
                      uint64_t size_B, void **ptr_out);

    /* Optional: tear down a mapping made by map_cookie (IOConnectMap-
     * then-unmap). NULL = teardown happens at transport close. */
    void (*unmap_cookie)(struct pg106_br_transport *tr, uint64_t cookie,
                         void *ptr);

    void *user;
};

struct pg106_br_dev;
struct pg106_br_ctx;

/* Limits, taken from the nvkmd/nouveau backend so batch behaviour
 * matches (nvkmd_nouveau.h:99-100). */
#define PG106_BR_MAX_SYNCS  256u
#define PG106_BR_MAX_BINDS  4096u
#define PG106_BR_MAX_BOS    1024u

/* ------------------------------------------------------------------ */
/* Device                                                              */
/* ------------------------------------------------------------------ */

/*
 * Open a device over `tr`: performs the OP_HANDSHAKE round trip and
 * gates on it (kext major must match, minor >= ours; drm_version >=
 * PG106_BR_DRM_VERSION_MIN like the Mesa winsys refuses below
 * 0x01000301, nouveau_device.c:500-505).
 *
 * PG106_BR_ABI_MAJOR/MINOR below is what the bridge was built against.
 */
#define PG106_BR_ABI_MAJOR 0u
#define PG106_BR_ABI_MINOR 1u

int pg106_br_dev_open(struct pg106_br_transport *tr,
                      struct pg106_br_dev **dev_out);
int pg106_br_dev_close(struct pg106_br_dev *dev);

/* DRM_IOCTL_NOUVEAU_GETPARAM transport. */
int pg106_br_getparam(struct pg106_br_dev *dev, uint64_t param,
                      uint64_t *value_out);

/* DRM_IOCTL_NOUVEAU_VM_INIT: reserve the kernel-managed VA region
 * (winsys nouveau_device.c:510-515). Must succeed before any VM_BIND
 * flush; the region is remembered for the validator's exclusion
 * check. */
int pg106_br_vm_init(struct pg106_br_dev *dev, uint64_t kernel_addr,
                     uint64_t kernel_size);

/* BO lifecycle (the GEM_NEW/GEM_INFO/CPU_PREP stand-in,
 * NOUVEAU-UAPI.md section 2.5). */
int pg106_br_bo_alloc(struct pg106_br_dev *dev, uint64_t size_B,
                      uint64_t align_B, uint32_t flags, uint32_t *handle_out);
int pg106_br_bo_free(struct pg106_br_dev *dev, uint32_t handle);
int pg106_br_bo_map(struct pg106_br_dev *dev, uint32_t handle,
                    uint32_t flags, void **map_out);
int pg106_br_bo_unmap(struct pg106_br_dev *dev, uint32_t handle);

/* GPU VA allocation, userspace-side like nvkmd's util_vma_heap
 * (nvkmd_nouveau_dev.c). Skeleton policy: bump allocator starting at
 * 4096 (NVKMD_NOUVEAU_HEAP_START), no reuse on free - see README
 * "intentionally missing". */
int pg106_br_va_alloc(struct pg106_br_dev *dev, uint64_t size_B,
                      uint64_t align_B, uint64_t *addr_out);
int pg106_br_va_free(struct pg106_br_dev *dev, uint64_t addr);

/* ------------------------------------------------------------------ */
/* Context (channel)                                                   */
/* ------------------------------------------------------------------ */

/*
 * One ctx == one channel. Channel creation (CHANNEL_ALLOC) is out of
 * skeleton scope: the mock provisions channel 0 at handshake and any
 * other channel id is rejected with -ENOENT, exactly like an unknown
 * channel on Linux (nouveau_exec.c:373-381).
 */
int pg106_br_ctx_create(struct pg106_br_dev *dev, uint32_t channel,
                        struct pg106_br_ctx **ctx_out);
void pg106_br_ctx_destroy(struct pg106_br_ctx *ctx);

/* Queue a VM_BIND op (MAP or UNMAP). Batched; hits the wire on flush
 * like nvkmd_nouveau_bind_ctx (nvkmd_nouveau_ctx.c:306-365). */
int pg106_br_ctx_bind(struct pg106_br_ctx *ctx,
                      const struct pg106_uapi_vm_bind_op *op);

/* Queue pushes (EXEC) and sync entries. The bind queue and the exec
 * queue each carry their own wait/signal lists; syncs on the bind side
 * force RUN_ASYNC (kernel rule: syncs without RUN_ASYNC are -EINVAL,
 * nouveau_sched.c:43-46). */
int pg106_br_ctx_bind_wait(struct pg106_br_ctx *ctx,
                           const struct pg106_uapi_sync *s);
int pg106_br_ctx_bind_signal(struct pg106_br_ctx *ctx,
                             const struct pg106_uapi_sync *s);
int pg106_br_ctx_exec(struct pg106_br_ctx *ctx,
                      const struct pg106_uapi_exec_push *push);
int pg106_br_ctx_exec_wait(struct pg106_br_ctx *ctx,
                           const struct pg106_uapi_sync *s);
int pg106_br_ctx_exec_signal(struct pg106_br_ctx *ctx,
                             const struct pg106_uapi_sync *s);

/*
 * Flush: validates everything pending through the pg106_uapi_*
 * validators, serializes and sends one OP_VM_BIND frame (if bind ops
 * are queued) and then one OP_EXEC frame (if pushes or exec syncs are
 * queued - the sync-only "empty exec" flush NVK uses,
 * nvkmd_nouveau_ctx.c:269-276). On any validator rejection the error
 * is returned and nothing is sent.
 *
 * Semantics carried by the wire layout: waits precede ops/pushes,
 * signals follow (all waits complete before submission, all signals
 * fire after the job's done fence, nouveau_sched.c nouveau_job_submit).
 */
int pg106_br_ctx_flush(struct pg106_br_ctx *ctx);

/* Flush + wait for completion (nvkmd_ctx_ops::sync). The mock channel
 * completes inline, so this is flush(); a real UserClient transport
 * will block on the event wait selector (NOUVEAU-UAPI.md section 4.1). */
int pg106_br_ctx_sync(struct pg106_br_ctx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* PG106_BR_BRIDGE_H */
