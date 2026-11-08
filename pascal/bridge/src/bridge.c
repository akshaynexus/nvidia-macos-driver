// SPDX-License-Identifier: MIT
/*
 * pg106_br_bridge.c - implementation of the pg106_br_ API (see
 * include/pg106_br_bridge.h).
 *
 * Every op follows the same three steps mandated by the mission
 * shape:
 *   (a) validate through the pg106_uapi_* validators (pascal/core) or
 *       the bridge-side state rules (BO handles, channel, VM_INIT);
 *   (b) serialize the op into a pg106_br_frame + payload that matches
 *       research/evidence/NOUVEAU-UAPI.md section 2 byte-for-byte;
 *   (c) hand it to the transport vtable (mock in tests, the kext
 *       UserClient later).
 *
 * A validator rejection returns before step (c): transport recordings
 * prove nothing was sent.
 */
#include "pg106_br_bridge.h"

#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Byte writer (step b)                                                */
/* ------------------------------------------------------------------ */

struct br_writer {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
};

static void br_wr_put(struct br_writer *w, const void *p, size_t n)
{
    if (w->len + n > w->cap) {
        return; /* sized up front; br_frame_send re-checks */
    }
    memcpy(w->buf + w->len, p, n);
    w->len += n;
}

/* ------------------------------------------------------------------ */
/* Device state                                                        */
/* ------------------------------------------------------------------ */

struct pg106_br_bo {
    uint32_t handle;
    uint32_t flags;
    uint64_t size_B;
    uint64_t map_cookie; /* non-zero while the kext holds a mapping  */
    void    *map;        /* non-NULL while the CPU mapping exists     */
    bool     alive;
    bool     imported;   /* OP_BO_IMPORT_HOST: caller-owned pages     */
};

/* One applied piece of a non-sparse MAP. Pieces follow drm_gpuvm's
 * split/merge rules (drm_gpuvm_sm_map / drm_gpuvm_sm_unmap): an UNMAP
 * may cut a piece in two, a MAP over existing pieces replaces the
 * overlapped part. This is the bridge's mirror of the kext's VA table:
 * enough to enforce unbind-before-release (NOUVEAU-UAPI.md section 6)
 * and to know what each UNMAP actually releases. */
struct pg106_br_binding {
    uint32_t handle;
    uint64_t addr;
    uint64_t range;
    uint64_t bo_offset;
};

/* A sparse region created by MAP|SPARSE (nouveau_uvma_region). */
struct pg106_br_region {
    uint64_t addr;
    uint64_t range;
};

struct br_vm_table {
    struct pg106_br_binding *b;
    size_t nb, capb;
    struct pg106_br_region *r;
    size_t nr, capr;
};

struct pg106_br_dev {
    struct pg106_br_transport *tr;
    uint32_t ib_max;
    uint32_t drm_version;
    uint64_t va_space_end;

    bool vm_ready;
    struct pg106_uapi_vm_init vm;

    /* Slots are reused after free; the array grows on demand. */
    struct pg106_br_bo *bos;
    size_t n_bos, cap_bos;

    struct br_vm_table va;

    uint32_t next_seq;
    uint64_t va_next;
};

static uint32_t br_next_seq(struct pg106_br_dev *dev)
{
    return dev->next_seq++;
}

static struct pg106_br_bo *br_bo_lookup(struct pg106_br_dev *dev,
                                        uint32_t handle)
{
    for (size_t i = 0; i < dev->n_bos; i++) {
        if (dev->bos[i].alive && dev->bos[i].handle == handle) {
            return &dev->bos[i];
        }
    }
    return NULL;
}

/* Grow *arr (element size elem) so that it holds at least need
 * elements. */
static int br_reserve(void **arr, size_t *cap, size_t need, size_t elem)
{
    size_t ncap;
    void *n;

    if (need <= *cap) {
        return 0;
    }
    ncap = *cap ? *cap : 16;
    while (ncap < need) {
        if (ncap > SIZE_MAX / 2 / elem) {
            return -ENOMEM;
        }
        ncap *= 2;
    }
    n = realloc(*arr, ncap * elem);
    if (n == NULL) {
        return -ENOMEM;
    }
    *arr = n;
    *cap = ncap;
    return 0;
}

/* One request/response round trip (steps b+c). `resp_len` is the exact
 * response payload size the op defines: a successful response with any
 * other length is a protocol error, so a short reply can never be read
 * as a zero-filled success. */
static int br_call(struct pg106_br_dev *dev, uint8_t op,
                   const void *payload, size_t payload_len,
                   void *resp_payload, size_t resp_len)
{
    struct pg106_br_frame req;
    struct pg106_br_frame resp;
    size_t got = 0;
    int rc;

    if (payload_len > UINT32_MAX) {
        return -E2BIG;
    }
    memset(&req, 0, sizeof(req));
    req.magic = PG106_BR_WIRE_MAGIC;
    req.op = op;
    req.wire_version = PG106_BR_WIRE_VERSION;
    req.payload_len = (uint32_t)payload_len;
    req.seq = br_next_seq(dev);

    memset(&resp, 0, sizeof(resp));
    rc = dev->tr->call(dev->tr, &req, payload, &resp,
                       resp_payload, resp_len, &got);
    if (rc < 0) {
        return rc;
    }
    if (resp.magic != PG106_BR_WIRE_MAGIC ||
        resp.wire_version != PG106_BR_WIRE_VERSION ||
        resp.op != op || resp.seq != req.seq ||
        resp.flags != 0 || resp.reserved != 0) {
        return -EPROTO;
    }
    if (resp.status > 0) {
        return -EPROTO; /* status is 0 or a negative errno */
    }
    if (resp.status < 0) {
        return resp.status; /* kext-side error surfaces as op error */
    }
    if (resp.payload_len != resp_len || got != resp_len) {
        return -EPROTO;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Open / close / handshake                                            */
/* ------------------------------------------------------------------ */

int pg106_br_dev_open(struct pg106_br_transport *tr,
                      struct pg106_br_dev **dev_out)
{
    struct pg106_br_dev *dev;
    struct pg106_br_handshake hs;
    int rc;

    if (tr == NULL || tr->call == NULL || dev_out == NULL) {
        return -EINVAL;
    }

    dev = calloc(1, sizeof(*dev));
    if (dev == NULL) {
        return -ENOMEM;
    }
    dev->tr = tr;
    dev->va_next = 0x1000; /* NVKMD_NOUVEAU_HEAP_START, nvkmd_nouveau.h */

    memset(&hs, 0, sizeof(hs));
    hs.client_major = PG106_BR_ABI_MAJOR;
    hs.client_minor = PG106_BR_ABI_MINOR;
    rc = br_call(dev, PG106_BR_OP_HANDSHAKE, &hs, sizeof(hs),
                 &hs, sizeof(hs));
    if (rc != 0) {
        free(dev);
        return rc;
    }

    if (hs.kext_major != PG106_BR_ABI_MAJOR || hs.kext_minor < PG106_BR_ABI_MINOR) {
        free(dev);
        return -EPROTONOSUPPORT;
    }
    /* The Mesa winsys refuses to run below 0x01000301
     * (nouveau_device.c:504-505); gate at the same point. */
    if (hs.drm_version < PG106_BR_DRM_VERSION_MIN) {
        free(dev);
        return -EINVAL;
    }
    if (hs.va_space_end == 0) {
        free(dev);
        return -EPROTO;
    }

    dev->ib_max = hs.ib_max;
    dev->drm_version = hs.drm_version;
    dev->va_space_end = hs.va_space_end;
    *dev_out = dev;
    return 0;
}

int pg106_br_dev_close(struct pg106_br_dev *dev)
{
    if (dev == NULL) {
        return -EINVAL;
    }
    /* CPU mappings are the transport's resources; release them. The
     * kext drops its side of every object when the connection closes. */
    for (size_t i = 0; i < dev->n_bos; i++) {
        struct pg106_br_bo *bo = &dev->bos[i];

        if (bo->alive && bo->map != NULL && dev->tr->unmap_cookie != NULL) {
            dev->tr->unmap_cookie(dev->tr, bo->map_cookie, bo->map);
        }
    }
    free(dev->bos);
    free(dev->va.b);
    free(dev->va.r);
    free(dev);
    return 0;
}

int pg106_br_getparam(struct pg106_br_dev *dev, uint64_t param,
                      uint64_t *value_out)
{
    struct pg106_uapi_getparam gp;
    int rc;

    if (dev == NULL || value_out == NULL) {
        return -EINVAL;
    }
    memset(&gp, 0, sizeof(gp));
    gp.param = param;
    rc = br_call(dev, PG106_BR_OP_GETPARAM, &gp, sizeof(gp),
                 &gp, sizeof(gp));
    if (rc != 0) {
        return rc;
    }
    *value_out = gp.value;
    return 0;
}

int pg106_br_vm_init(struct pg106_br_dev *dev, uint64_t kernel_addr,
                     uint64_t kernel_size)
{
    struct pg106_uapi_vm_init init;
    int rc;

    if (dev == NULL) {
        return -EINVAL;
    }
    if (dev->vm_ready) {
        return -EEXIST; /* one VM per client (nouveau_uvmm.c) */
    }
    memset(&init, 0, sizeof(init));
    init.kernel_managed_addr = kernel_addr;
    init.kernel_managed_size = kernel_size;
    rc = pg106_uapi_vm_init_validate(&init);
    if (rc != 0) {
        return rc;
    }
    rc = br_call(dev, PG106_BR_OP_VM_INIT, &init, sizeof(init), NULL, 0);
    if (rc != 0) {
        return rc;
    }
    dev->vm = init;
    dev->vm_ready = true;
    return 0;
}

/* ------------------------------------------------------------------ */
/* BO lifecycle                                                        */
/* ------------------------------------------------------------------ */

int pg106_br_bo_alloc(struct pg106_br_dev *dev, uint64_t size_B,
                      uint64_t align_B, uint32_t flags, uint32_t *handle_out)
{
    struct pg106_br_bo_alloc req;
    struct pg106_br_bo *bo = NULL;
    size_t slot;
    int rc;

    if (dev == NULL || handle_out == NULL) {
        return -EINVAL;
    }
    if (size_B == 0 || align_B == 0 || (align_B & (align_B - 1)) != 0) {
        return -EINVAL;
    }
    /* Reuse a freed slot, else append (growing the array first, so a
     * failed allocation cannot leave a BO the bridge cannot track). */
    for (slot = 0; slot < dev->n_bos; slot++) {
        if (!dev->bos[slot].alive) {
            break;
        }
    }
    if (slot == dev->n_bos) {
        rc = br_reserve((void **)&dev->bos, &dev->cap_bos, dev->n_bos + 1,
                        sizeof(*dev->bos));
        if (rc != 0) {
            return rc;
        }
    }
    memset(&req, 0, sizeof(req));
    req.size_B = size_B;
    req.align_B = align_B;
    req.flags = flags;
    rc = br_call(dev, PG106_BR_OP_BO_ALLOC, &req, sizeof(req),
                 &req, sizeof(req));
    if (rc != 0) {
        return rc;
    }
    if (req.handle == 0 || br_bo_lookup(dev, req.handle) != NULL) {
        return -EPROTO; /* 0 is never a handle; live handles are unique */
    }
    if (slot == dev->n_bos) {
        dev->n_bos++;
    }
    bo = &dev->bos[slot];
    memset(bo, 0, sizeof(*bo));
    bo->handle = req.handle;
    bo->flags = flags;
    bo->size_B = size_B;
    bo->alive = true;
    *handle_out = req.handle;
    return 0;
}

static bool br_bo_bound(const struct pg106_br_dev *dev, uint32_t handle)
{
    for (size_t i = 0; i < dev->va.nb; i++) {
        if (dev->va.b[i].handle == handle) {
            return true;
        }
    }
    return false;
}

/* Tear down the CPU mapping (IOConnectUnmapMemory64 on the real
 * transport) before the kext is told to drop or free the memory behind
 * it, so userspace never holds a pointer into released pages. */
static void br_bo_cpu_unmap(struct pg106_br_dev *dev, struct pg106_br_bo *bo)
{
    if (bo->map != NULL && dev->tr->unmap_cookie != NULL) {
        dev->tr->unmap_cookie(dev->tr, bo->map_cookie, bo->map);
    }
    bo->map = NULL;
}

int pg106_br_bo_free(struct pg106_br_dev *dev, uint32_t handle)
{
    struct pg106_br_bo *bo;
    struct pg106_br_bo_free req;
    int rc;

    if (dev == NULL) {
        return -EINVAL;
    }
    bo = br_bo_lookup(dev, handle);
    if (bo == NULL) {
        return PG106_UAPI_ENOENT;
    }
    /* Pinning rule, enforcement side: a BO whose pages are still bound
     * into the GPU VM must not be released - unbind first
     * (NOUVEAU-UAPI.md section 6, rule 3). */
    if (br_bo_bound(dev, handle)) {
        return -EBUSY;
    }
    br_bo_cpu_unmap(dev, bo);
    memset(&req, 0, sizeof(req));
    req.handle = handle;
    rc = br_call(dev, PG106_BR_OP_BO_FREE, &req, sizeof(req), NULL, 0);
    if (rc != 0) {
        return rc; /* still alive; the kext mapping (if any) is kept */
    }
    bo->alive = false;
    bo->map_cookie = 0;
    return 0;
}

int pg106_br_bo_map(struct pg106_br_dev *dev, uint32_t handle,
                    uint32_t flags, void **map_out)
{
    struct pg106_br_bo *bo;
    struct pg106_br_bo_map req;
    struct pg106_br_bo_unmap undo;
    int rc;

    if (dev == NULL || map_out == NULL) {
        return -EINVAL;
    }
    bo = br_bo_lookup(dev, handle);
    if (bo == NULL) {
        return PG106_UAPI_ENOENT;
    }
    if (bo->imported) {
        return -EINVAL; /* the caller owns the pages already */
    }
    if ((bo->flags & PG106_BR_BO_CAN_MAP) == 0) {
        return -EACCES; /* nvkmd requires NVKMD_MEM_CAN_MAP, nvkmd.h:51 */
    }
    if (bo->map_cookie != 0) {
        /* One client mapping at a time (nvkmd.h:68-76; nvkmd refcounts
         * internal maps itself, above this layer). */
        return -EEXIST;
    }
    if (dev->tr->map_cookie == NULL) {
        return -EOPNOTSUPP; /* transport cannot make CPU mappings */
    }
    memset(&req, 0, sizeof(req));
    req.handle = handle;
    req.flags = flags;
    rc = br_call(dev, PG106_BR_OP_BO_MAP, &req, sizeof(req),
                 &req, sizeof(req));
    if (rc != 0) {
        return rc;
    }
    if (req.map_cookie == 0) {
        return -EPROTO;
    }
    rc = dev->tr->map_cookie(dev->tr, req.map_cookie, bo->size_B, &bo->map);
    if (rc != 0) {
        /* The kext mapped it but userspace could not: undo kext-side so
         * the BO is not left half-mapped. */
        bo->map = NULL;
        memset(&undo, 0, sizeof(undo));
        undo.handle = handle;
        (void)br_call(dev, PG106_BR_OP_BO_UNMAP, &undo, sizeof(undo), NULL, 0);
        return rc;
    }
    bo->map_cookie = req.map_cookie;
    *map_out = bo->map;
    return 0;
}

int pg106_br_bo_unmap(struct pg106_br_dev *dev, uint32_t handle)
{
    struct pg106_br_bo *bo;
    struct pg106_br_bo_unmap req;
    int rc;

    if (dev == NULL) {
        return -EINVAL;
    }
    bo = br_bo_lookup(dev, handle);
    if (bo == NULL) {
        return PG106_UAPI_ENOENT;
    }
    if (bo->map_cookie == 0) {
        return -EINVAL; /* not mapped */
    }
    br_bo_cpu_unmap(dev, bo);
    memset(&req, 0, sizeof(req));
    req.handle = handle;
    rc = br_call(dev, PG106_BR_OP_BO_UNMAP, &req, sizeof(req), NULL, 0);
    if (rc != 0) {
        return rc; /* CPU side is gone; cookie kept so unmap can retry */
    }
    bo->map_cookie = 0;
    return 0;
}

int pg106_br_bo_import_host(struct pg106_br_dev *dev, void *host_ptr,
                            uint64_t size_B, uint32_t flags,
                            uint32_t *handle_out)
{
    struct pg106_br_bo_import_host req;
    struct pg106_br_bo *bo;
    const uint64_t addr = (uint64_t)(uintptr_t)host_ptr;
    size_t slot;
    int rc;

    if (dev == NULL || handle_out == NULL || host_ptr == NULL || size_B == 0 ||
        (addr & (PG106_UAPI_PAGE_SIZE - 1)) != 0 ||
        (size_B & (PG106_UAPI_PAGE_SIZE - 1)) != 0 || addr + size_B < addr) {
        return -EINVAL;
    }
    for (slot = 0; slot < dev->n_bos; slot++) {
        if (!dev->bos[slot].alive) {
            break;
        }
    }
    if (slot == dev->n_bos) {
        rc = br_reserve((void **)&dev->bos, &dev->cap_bos, dev->n_bos + 1,
                        sizeof(*dev->bos));
        if (rc != 0) {
            return rc;
        }
    }
    memset(&req, 0, sizeof(req));
    req.host_addr = addr;
    req.size_B = size_B;
    req.flags = flags;
    rc = br_call(dev, PG106_BR_OP_BO_IMPORT_HOST, &req, sizeof(req),
                 &req, sizeof(req));
    if (rc != 0) {
        return rc;
    }
    if (req.handle == 0 || br_bo_lookup(dev, req.handle) != NULL) {
        return -EPROTO;
    }
    if (slot == dev->n_bos) {
        dev->n_bos++;
    }
    bo = &dev->bos[slot];
    memset(bo, 0, sizeof(*bo));
    bo->handle = req.handle;
    bo->flags = flags;
    bo->size_B = size_B;
    bo->alive = true;
    bo->imported = true;
    *handle_out = req.handle;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Syncobjs                                                            */
/* ------------------------------------------------------------------ */

int pg106_br_syncobj_create(struct pg106_br_dev *dev, uint32_t flags,
                            uint32_t *handle_out)
{
    struct pg106_br_syncobj so;
    int rc;

    if (dev == NULL || handle_out == NULL ||
        (flags & ~PG106_BR_SYNCOBJ_CREATE_SIGNALED) != 0) {
        return -EINVAL;
    }
    memset(&so, 0, sizeof(so));
    so.flags = flags;
    rc = br_call(dev, PG106_BR_OP_SYNCOBJ_CREATE, &so, sizeof(so),
                 &so, sizeof(so));
    if (rc != 0) {
        return rc;
    }
    if (so.handle == 0) {
        return -EPROTO;
    }
    *handle_out = so.handle;
    return 0;
}

int pg106_br_syncobj_destroy(struct pg106_br_dev *dev, uint32_t handle)
{
    struct pg106_br_syncobj so;

    if (dev == NULL || handle == 0) {
        return -EINVAL;
    }
    memset(&so, 0, sizeof(so));
    so.handle = handle;
    return br_call(dev, PG106_BR_OP_SYNCOBJ_DESTROY, &so, sizeof(so), NULL, 0);
}

int pg106_br_syncobj_wait(struct pg106_br_dev *dev, uint32_t handle,
                          uint64_t point, uint64_t timeout_ns)
{
    struct pg106_br_syncobj_wait w;

    if (dev == NULL || handle == 0) {
        return -EINVAL;
    }
    memset(&w, 0, sizeof(w));
    w.handle = handle;
    w.point = point;
    w.timeout_ns = timeout_ns;
    return br_call(dev, PG106_BR_OP_SYNCOBJ_WAIT, &w, sizeof(w), NULL, 0);
}

/* ------------------------------------------------------------------ */
/* VA allocator (bump; see README "intentionally missing")             */
/* ------------------------------------------------------------------ */

static uint64_t br_align_up(uint64_t v, uint64_t align)
{
    return (v + align - 1) & ~(align - 1);
}

int pg106_br_va_alloc(struct pg106_br_dev *dev, uint64_t size_B,
                      uint64_t align_B, uint64_t *addr_out)
{
    uint64_t addr;

    if (dev == NULL || addr_out == NULL) {
        return -EINVAL;
    }
    if (size_B == 0 || align_B == 0 || (align_B & (align_B - 1)) != 0) {
        return -EINVAL;
    }
    addr = br_align_up(dev->va_next, align_B);
    if (addr > dev->va_space_end || size_B > dev->va_space_end - addr) {
        return -ENOMEM;
    }
    dev->va_next = addr + size_B;
    *addr_out = addr;
    return 0;
}

int pg106_br_va_free(struct pg106_br_dev *dev, uint64_t addr)
{
    (void)dev;
    (void)addr;
    return 0; /* bump allocator: no reuse (documented) */
}

/* ------------------------------------------------------------------ */
/* Context                                                             */
/* ------------------------------------------------------------------ */

struct pg106_br_ctx {
    struct pg106_br_dev *dev;
    uint32_t channel;
    uint32_t max_pushes;

    struct pg106_uapi_vm_bind_op binds[PG106_BR_MAX_BINDS];
    size_t n_binds;
    struct pg106_uapi_sync bind_waits[PG106_BR_MAX_SYNCS];
    struct pg106_uapi_sync bind_sigs[PG106_BR_MAX_SYNCS];
    size_t n_bind_waits, n_bind_sigs;

    struct pg106_uapi_exec_push *pushes;
    size_t n_pushes;
    struct pg106_uapi_sync exec_waits[PG106_BR_MAX_SYNCS];
    struct pg106_uapi_sync exec_sigs[PG106_BR_MAX_SYNCS];
    size_t n_exec_waits, n_exec_sigs;
};

int pg106_br_ctx_create(struct pg106_br_dev *dev, uint32_t channel,
                        struct pg106_br_ctx **ctx_out)
{
    struct pg106_br_ctx *ctx;
    uint32_t max_pushes;

    if (dev == NULL || ctx_out == NULL) {
        return -EINVAL;
    }
    /* Channel creation (CHANNEL_ALLOC) is out of skeleton scope: the
     * mock provisions channel 0 at the handshake; anything else is as
     * unknown as it would be on Linux (nouveau_exec.c:373-381). */
    if (channel != 0) {
        return PG106_UAPI_ENOENT;
    }
    ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return -ENOMEM;
    }
    max_pushes = pg106_uapi_exec_push_max(dev->ib_max);
    ctx->pushes = calloc(max_pushes ? max_pushes : 1,
                         sizeof(struct pg106_uapi_exec_push));
    if (ctx->pushes == NULL) {
        free(ctx);
        return -ENOMEM;
    }
    ctx->dev = dev;
    ctx->channel = channel;
    ctx->max_pushes = max_pushes;
    *ctx_out = ctx;
    return 0;
}

void pg106_br_ctx_destroy(struct pg106_br_ctx *ctx)
{
    if (ctx == NULL) {
        return;
    }
    free(ctx->pushes);
    free(ctx);
}

#define BR_CTX_QUEUE_BODY(what, arr, count, max, elem_t)                     \
    do {                                                                     \
        if (ctx == NULL || what == NULL) {                                   \
            return -EINVAL;                                                  \
        }                                                                    \
        if (ctx->count >= (max)) {                                           \
            return -EINVAL; /* nvkmd overflows to flush; skeleton errors */  \
        }                                                                    \
        ctx->arr[ctx->count++] = *what;                                      \
        return 0;                                                            \
    } while (0)

int pg106_br_ctx_bind(struct pg106_br_ctx *ctx,
                      const struct pg106_uapi_vm_bind_op *op)
{
    BR_CTX_QUEUE_BODY(op, binds, n_binds, PG106_BR_MAX_BINDS,
                      struct pg106_uapi_vm_bind_op);
}

int pg106_br_ctx_bind_wait(struct pg106_br_ctx *ctx,
                           const struct pg106_uapi_sync *s)
{
    BR_CTX_QUEUE_BODY(s, bind_waits, n_bind_waits, PG106_BR_MAX_SYNCS,
                      struct pg106_uapi_sync);
}

int pg106_br_ctx_bind_signal(struct pg106_br_ctx *ctx,
                             const struct pg106_uapi_sync *s)
{
    BR_CTX_QUEUE_BODY(s, bind_sigs, n_bind_sigs, PG106_BR_MAX_SYNCS,
                      struct pg106_uapi_sync);
}

int pg106_br_ctx_exec(struct pg106_br_ctx *ctx,
                      const struct pg106_uapi_exec_push *push)
{
    BR_CTX_QUEUE_BODY(push, pushes, n_pushes, ctx->max_pushes,
                      struct pg106_uapi_exec_push);
}

int pg106_br_ctx_exec_wait(struct pg106_br_ctx *ctx,
                           const struct pg106_uapi_sync *s)
{
    BR_CTX_QUEUE_BODY(s, exec_waits, n_exec_waits, PG106_BR_MAX_SYNCS,
                      struct pg106_uapi_sync);
}

int pg106_br_ctx_exec_signal(struct pg106_br_ctx *ctx,
                             const struct pg106_uapi_sync *s)
{
    BR_CTX_QUEUE_BODY(s, exec_sigs, n_exec_sigs, PG106_BR_MAX_SYNCS,
                      struct pg106_uapi_sync);
}

/* ---- Wire serialization of the batched uapi ops ------------------- */

/* VM_BIND payload: header + ops + waits + sigs (ptr fields zero; the
 * arrays are inline in the kernel's copy order, nouveau_uvmm_vm_bind_ucopy,
 * nouveau_uvmm.c:1749-1800; see pg106_br_wire.h). */
static size_t br_vm_bind_payload_len(const struct pg106_br_ctx *ctx)
{
    return sizeof(struct pg106_uapi_vm_bind) +
           ctx->n_binds * sizeof(struct pg106_uapi_vm_bind_op) +
           (ctx->n_bind_waits + ctx->n_bind_sigs) *
               sizeof(struct pg106_uapi_sync);
}

static void br_vm_bind_serialize(const struct pg106_br_ctx *ctx,
                                 const struct pg106_uapi_vm_bind *req,
                                 struct br_writer *w)
{
    br_wr_put(w, req, sizeof(*req));
    for (size_t i = 0; i < ctx->n_binds; i++) {
        br_wr_put(w, &ctx->binds[i], sizeof(struct pg106_uapi_vm_bind_op));
    }
    for (size_t i = 0; i < ctx->n_bind_waits; i++) {
        br_wr_put(w, &ctx->bind_waits[i], sizeof(struct pg106_uapi_sync));
    }
    for (size_t i = 0; i < ctx->n_bind_sigs; i++) {
        br_wr_put(w, &ctx->bind_sigs[i], sizeof(struct pg106_uapi_sync));
    }
}

/* ---- VA table: drm_gpuvm split/merge + nouveau sparse regions ------ */

static void br_vm_table_free(struct br_vm_table *t)
{
    free(t->b);
    free(t->r);
    memset(t, 0, sizeof(*t));
}

static int br_vm_table_clone(struct br_vm_table *dst,
                             const struct br_vm_table *src)
{
    memset(dst, 0, sizeof(*dst));
    if (br_reserve((void **)&dst->b, &dst->capb, src->nb ? src->nb : 1,
                   sizeof(*dst->b)) != 0 ||
        br_reserve((void **)&dst->r, &dst->capr, src->nr ? src->nr : 1,
                   sizeof(*dst->r)) != 0) {
        br_vm_table_free(dst);
        return -ENOMEM;
    }
    if (src->nb) {
        memcpy(dst->b, src->b, src->nb * sizeof(*src->b));
    }
    if (src->nr) {
        memcpy(dst->r, src->r, src->nr * sizeof(*src->r));
    }
    dst->nb = src->nb;
    dst->nr = src->nr;
    return 0;
}

static int br_vm_push_binding(struct br_vm_table *t,
                              const struct pg106_br_binding *nb)
{
    if (br_reserve((void **)&t->b, &t->capb, t->nb + 1, sizeof(*t->b)) != 0) {
        return -ENOMEM;
    }
    t->b[t->nb++] = *nb;
    return 0;
}

/* Remove [addr, addr+range) from every binding, splitting a binding
 * that straddles either end; the kept tail of a split piece advances
 * its bo_offset by the cut, like drm_gpuvm's REMAP prev/next
 * (drm_gpuvm_sm_unmap). Ranges with nothing mapped are a no-op, as on
 * the kernel. Callers have validated addr+range for overflow. */
static int br_vm_unmap_range(struct br_vm_table *t, uint64_t addr,
                             uint64_t range)
{
    const uint64_t end = addr + range;
    size_t i = 0;

    while (i < t->nb) {
        struct pg106_br_binding *b = &t->b[i];
        const uint64_t bend = b->addr + b->range;
        const bool keep_head = b->addr < addr;
        const bool keep_tail = bend > end;

        if (bend <= addr || b->addr >= end) {
            i++;
            continue;
        }
        if (keep_head && keep_tail) {
            struct pg106_br_binding tail = *b;

            tail.addr = end;
            tail.range = bend - end;
            tail.bo_offset = b->bo_offset + (end - b->addr);
            b->range = addr - b->addr;
            /* b may move on growth; it is not used after this. */
            if (br_vm_push_binding(t, &tail) != 0) {
                return -ENOMEM;
            }
            i++;
        } else if (keep_head) {
            b->range = addr - b->addr;
            i++;
        } else if (keep_tail) {
            b->bo_offset += end - b->addr;
            b->range = bend - end;
            b->addr = end;
            i++;
        } else {
            t->b[i] = t->b[t->nb - 1]; /* fully covered: drop */
            t->nb--;
        }
    }
    return 0;
}

static const struct pg106_br_region *
br_vm_region_overlapping(const struct br_vm_table *t, uint64_t addr,
                         uint64_t range)
{
    for (size_t i = 0; i < t->nr; i++) {
        const struct pg106_br_region *r = &t->r[i];

        if (addr < r->addr + r->range && r->addr < addr + range) {
            return r;
        }
    }
    return NULL;
}

/* A non-sparse MAP must be outside every region or fully inside one
 * (bind_validate_map_common, nouveau_uvmm.c:1147-1155; re-checked in the
 * locked loop, :1356-1385). */
static int br_vm_map_region_rule(const struct br_vm_table *t, uint64_t addr,
                                 uint64_t range)
{
    const struct pg106_br_region *r = br_vm_region_overlapping(t, addr, range);

    if (r != NULL && (r->addr > addr || r->addr + r->range < addr + range)) {
        return PG106_UAPI_ENOSPC;
    }
    return 0;
}

/* Apply one validated op to t, in job order (the kernel's locked loop,
 * nouveau_uvmm_bind_job_submit, nouveau_uvmm.c:1316-1430). */
static int br_vm_apply(struct br_vm_table *t,
                       const struct pg106_uapi_vm_bind_op *op)
{
    const bool sparse = (op->flags & PG106_UAPI_VM_BIND_SPARSE) != 0;
    int rc;

    if (op->op == PG106_UAPI_VM_BIND_OP_MAP && sparse) {
        /* nouveau_uvma_region_insert: any overlap is -EEXIST
         * (__nouveau_uvma_region_insert, nouveau_uvmm.c:280-292). */
        if (br_vm_region_overlapping(t, op->addr, op->range) != NULL) {
            return PG106_UAPI_EEXIST;
        }
        if (br_reserve((void **)&t->r, &t->capr, t->nr + 1,
                       sizeof(*t->r)) != 0) {
            return -ENOMEM;
        }
        t->r[t->nr++] = (struct pg106_br_region){ op->addr, op->range };
        return 0;
    }
    if (op->op == PG106_UAPI_VM_BIND_OP_UNMAP && sparse) {
        /* The region must match exactly (nouveau_uvma_region_find,
         * :1326-1333 -> -ENOENT); everything mapped inside it goes. */
        for (size_t i = 0; i < t->nr; i++) {
            if (t->r[i].addr == op->addr && t->r[i].range == op->range) {
                rc = br_vm_unmap_range(t, op->addr, op->range);
                if (rc != 0) {
                    return rc;
                }
                t->r[i] = t->r[t->nr - 1];
                t->nr--;
                return 0;
            }
        }
        return PG106_UAPI_ENOENT;
    }
    if (op->op == PG106_UAPI_VM_BIND_OP_MAP) {
        struct pg106_br_binding nb = {
            op->handle, op->addr, op->range, op->bo_offset,
        };

        rc = br_vm_map_region_rule(t, op->addr, op->range);
        if (rc != 0) {
            return rc;
        }
        /* drm_gpuvm_sm_map: a new mapping replaces whatever it overlaps. */
        rc = br_vm_unmap_range(t, op->addr, op->range);
        if (rc != 0) {
            return rc;
        }
        return br_vm_push_binding(t, &nb);
    }
    return br_vm_unmap_range(t, op->addr, op->range); /* plain UNMAP */
}

static int br_flush_vm_bind(struct pg106_br_ctx *ctx)
{
    struct pg106_br_dev *dev = ctx->dev;
    struct pg106_uapi_vm_bind req;
    struct br_vm_table next;
    struct br_writer w;
    uint8_t *buf;
    size_t len;
    int rc;

    if (ctx->n_binds == 0) {
        /* Kernel rule: syncs only with RUN_ASYNC ops (a sync job with
         * syncs but no ops is -EINVAL, nouveau_sched.c:43-46). */
        if (ctx->n_bind_waits + ctx->n_bind_sigs > 0) {
            return PG106_UAPI_EINVAL;
        }
        return 0; /* nothing queued on the bind side */
    }
    if (!dev->vm_ready) {
        return PG106_UAPI_ENOSYS; /* VM_INIT first */
    }

    memset(&req, 0, sizeof(req));
    req.op_count = (uint32_t)ctx->n_binds;
    req.wait_count = (uint32_t)ctx->n_bind_waits;
    req.sig_count = (uint32_t)ctx->n_bind_sigs;
    /* Kernel rule: syncs only with RUN_ASYNC (nouveau_sched.c:43-46);
     * the bridge sets the bit itself so UMD code cannot forget it. */
    if (ctx->n_bind_waits + ctx->n_bind_sigs > 0) {
        req.flags = PG106_UAPI_VM_BIND_RUN_ASYNC;
    }

    /* (1) Request-level and sync rules (nouveau_job_init): the
     * validator with no ops. */
    {
        struct pg106_uapi_vm_bind syncs_only = req;

        syncs_only.op_count = 0;
        rc = pg106_uapi_vm_bind_validate(&dev->vm, &syncs_only, NULL,
                                         ctx->n_bind_waits ? ctx->bind_waits : NULL,
                                         ctx->n_bind_sigs ? ctx->bind_sigs : NULL);
        if (rc != 0) {
            return rc;
        }
    }

    /* (2) Per op, in order, like nouveau_uvmm_bind_job_submit
     * (nouveau_uvmm.c:1271-1297): stateless checks, then for a
     * non-sparse MAP the handle -> BO lookup (-ENOENT) and the BO-size
     * cross-check (bind_validate_op). */
    for (size_t i = 0; i < ctx->n_binds; i++) {
        const struct pg106_uapi_vm_bind_op *op = &ctx->binds[i];
        const struct pg106_uapi_vm_bind one = { .op_count = 1 };
        struct pg106_br_bo *bo;

        rc = pg106_uapi_vm_bind_validate(&dev->vm, &one, op, NULL, NULL);
        if (rc != 0) {
            return rc;
        }
        if (!pg106_uapi_vm_bind_op_is_bo_map(op)) {
            continue;
        }
        bo = br_bo_lookup(dev, op->handle);
        if (bo == NULL) {
            return PG106_UAPI_ENOENT;
        }
        rc = pg106_uapi_vm_bind_op_check_map(op, bo->size_B);
        if (rc != 0) {
            return rc;
        }
    }

    /* (3) Region pre-check against the state before this job
     * (bind_validate_region, nouveau_uvmm.c:1158-1188): a MAP|SPARSE
     * overlapping an existing region, or a MAP partly inside one, is
     * -ENOSPC. */
    for (size_t i = 0; i < ctx->n_binds; i++) {
        const struct pg106_uapi_vm_bind_op *op = &ctx->binds[i];

        if (op->op != PG106_UAPI_VM_BIND_OP_MAP) {
            continue;
        }
        if (op->flags & PG106_UAPI_VM_BIND_SPARSE) {
            if (br_vm_region_overlapping(&dev->va, op->addr, op->range)) {
                return PG106_UAPI_ENOSPC;
            }
        } else if ((rc = br_vm_map_region_rule(&dev->va, op->addr,
                                               op->range)) != 0) {
            return rc;
        }
    }

    /* (4) Apply the ops in order to a copy. The job is all-or-nothing on
     * the kernel (it unwinds on failure, nouveau_uvmm.c:1309-1313), so
     * the live table changes only after the kext accepted the frame;
     * later ops see earlier ones (an UNMAP then a MAP of the same range
     * in one job is fine). */
    rc = br_vm_table_clone(&next, &dev->va);
    if (rc != 0) {
        return rc;
    }
    for (size_t i = 0; i < ctx->n_binds; i++) {
        rc = br_vm_apply(&next, &ctx->binds[i]);
        if (rc != 0) {
            br_vm_table_free(&next);
            return rc;
        }
    }

    len = br_vm_bind_payload_len(ctx);
    buf = malloc(len);
    if (buf == NULL) {
        br_vm_table_free(&next);
        return -ENOMEM;
    }
    w.buf = buf;
    w.len = 0;
    w.cap = len;
    br_vm_bind_serialize(ctx, &req, &w);
    rc = br_call(dev, PG106_BR_OP_VM_BIND, buf, w.len, NULL, 0);
    free(buf);
    if (rc != 0) {
        br_vm_table_free(&next);
        return rc;
    }
    br_vm_table_free(&dev->va);
    dev->va = next;
    ctx->n_binds = 0;
    ctx->n_bind_waits = 0;
    ctx->n_bind_sigs = 0;
    return 0;
}

/* EXEC payload: header + pushes + waits + sigs, the order the kernel
 * copies them in (nouveau_exec_ucopy, nouveau_exec.c:291-339). */
static size_t br_exec_payload_len(const struct pg106_br_ctx *ctx)
{
    return sizeof(struct pg106_uapi_exec) +
           ctx->n_pushes * sizeof(struct pg106_uapi_exec_push) +
           (ctx->n_exec_waits + ctx->n_exec_sigs) *
               sizeof(struct pg106_uapi_sync);
}

static void br_exec_serialize(const struct pg106_br_ctx *ctx,
                              const struct pg106_uapi_exec *req,
                              struct br_writer *w)
{
    br_wr_put(w, req, sizeof(*req));
    for (size_t i = 0; i < ctx->n_pushes; i++) {
        br_wr_put(w, &ctx->pushes[i], sizeof(struct pg106_uapi_exec_push));
    }
    for (size_t i = 0; i < ctx->n_exec_waits; i++) {
        br_wr_put(w, &ctx->exec_waits[i], sizeof(struct pg106_uapi_sync));
    }
    for (size_t i = 0; i < ctx->n_exec_sigs; i++) {
        br_wr_put(w, &ctx->exec_sigs[i], sizeof(struct pg106_uapi_sync));
    }
}

static int br_flush_exec(struct pg106_br_ctx *ctx)
{
    struct pg106_br_dev *dev = ctx->dev;
    struct pg106_uapi_exec req;
    struct br_writer w;
    uint8_t *buf;
    size_t len;
    int rc;

    if (ctx->n_pushes == 0 &&
        ctx->n_exec_waits == 0 && ctx->n_exec_sigs == 0) {
        return 0; /* nothing on the exec side */
    }
    if (ctx->channel != 0) {
        return PG106_UAPI_ENOENT; /* unknown channel, nouveau_exec.c:373-381 */
    }

    memset(&req, 0, sizeof(req));
    req.channel = ctx->channel;
    req.push_count = (uint32_t)ctx->n_pushes;
    req.wait_count = (uint32_t)ctx->n_exec_waits;
    req.sig_count = (uint32_t)ctx->n_exec_sigs;

    rc = pg106_uapi_exec_validate(dev->ib_max, &req, ctx->pushes,
                                  ctx->n_exec_waits ? ctx->exec_waits : NULL,
                                  ctx->n_exec_sigs ? ctx->exec_sigs : NULL);
    if (rc != 0) {
        return rc;
    }

    len = br_exec_payload_len(ctx);
    buf = malloc(len);
    if (buf == NULL) {
        return -ENOMEM;
    }
    w.buf = buf;
    w.len = 0;
    w.cap = len;
    br_exec_serialize(ctx, &req, &w);
    rc = br_call(dev, PG106_BR_OP_EXEC, buf, w.len, NULL, 0);
    free(buf);
    if (rc != 0) {
        return rc;
    }
    ctx->n_pushes = 0;
    ctx->n_exec_waits = 0;
    ctx->n_exec_sigs = 0;
    return 0;
}

int pg106_br_ctx_flush(struct pg106_br_ctx *ctx)
{
    int rc;

    if (ctx == NULL) {
        return -EINVAL;
    }
    /* Binds first, then exec: NVK orders them the same way (the bind
     * queue flushes before the exec queue so pushes see the new
     * mappings; nvkmd_nouveau_ctx.c:353-376). */
    rc = br_flush_vm_bind(ctx);
    if (rc != 0) {
        return rc;
    }
    return br_flush_exec(ctx);
}

int pg106_br_ctx_sync(struct pg106_br_ctx *ctx)
{
    /* The mock channel completes inline; a real UserClient transport
     * blocks on the event wait selector here
     * (NOUVEAU-UAPI.md section 4.1, "Event wait"). */
    return pg106_br_ctx_flush(ctx);
}
