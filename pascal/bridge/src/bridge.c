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
    uint64_t map_cookie;
    void    *map;
    bool     alive;
};

/* One active GPU-VA binding (a MAP op that has been flushed). Bridge-
 * side mirror of the kext's region table: enough to enforce the
 * unbind-before-release ordering of the pinning rule
 * (NOUVEAU-UAPI.md section 6) and to reject double MAPs at the same
 * address. */
struct pg106_br_binding {
    uint32_t handle;
    uint64_t addr;
    uint64_t range;
};

struct pg106_br_dev {
    struct pg106_br_transport *tr;
    uint32_t ib_max;
    uint32_t drm_version;
    uint64_t va_space_end;

    bool vm_ready;
    struct pg106_uapi_vm_init vm;

    struct pg106_br_bo bos[PG106_BR_MAX_BOS];
    size_t n_bos;

    struct pg106_br_binding bindings[PG106_BR_MAX_BINDS];
    size_t n_bindings;

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

/* One request/response round trip (steps b+c). */
static int br_call(struct pg106_br_dev *dev, uint8_t op,
                   const void *payload, size_t payload_len,
                   void *resp_payload, size_t resp_cap, size_t *resp_len_out)
{
    struct pg106_br_frame req;
    struct pg106_br_frame resp;
    size_t resp_len = 0;
    int rc;

    memset(&req, 0, sizeof(req));
    req.magic = PG106_BR_WIRE_MAGIC;
    req.op = op;
    req.wire_version = PG106_BR_WIRE_VERSION;
    req.payload_len = (uint32_t)payload_len;
    req.seq = br_next_seq(dev);

    rc = dev->tr->call(dev->tr, &req, payload, &resp,
                       resp_payload, resp_cap, &resp_len);
    if (rc < 0) {
        return rc;
    }
    if (resp.magic != PG106_BR_WIRE_MAGIC ||
        resp.wire_version != PG106_BR_WIRE_VERSION ||
        resp.op != op || resp.seq != req.seq) {
        return -EPROTO;
    }
    if (resp.payload_len > resp_cap) {
        return -EPROTO;
    }
    if (resp.status < 0) {
        return resp.status; /* kext-side error surfaces as op error */
    }
    if (resp_len_out) {
        *resp_len_out = resp_len;
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
                 &hs, sizeof(hs), NULL);
    if (rc != 0) {
        free(dev);
        return rc;
    }

    if (hs.kext_major != PG106_BR_ABI_MAJOR || hs.kext_minor < PG106_BR_ABI_MINOR) {
        free(dev);
        return -EPROTONOSUPPORT;
    }
    /* The Mesa winsys refuses to run below 0x01000301
     * (nouveau_device.c:500-505); gate at the same point. */
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
                 &gp, sizeof(gp), NULL);
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
    rc = br_call(dev, PG106_BR_OP_VM_INIT, &init, sizeof(init), NULL, 0, NULL);
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
    struct pg106_br_bo *bo;
    int rc;

    if (dev == NULL || handle_out == NULL) {
        return -EINVAL;
    }
    if (size_B == 0 || align_B == 0 || (align_B & (align_B - 1)) != 0) {
        return -EINVAL;
    }
    if (dev->n_bos >= PG106_BR_MAX_BOS) {
        return -ENOMEM;
    }
    memset(&req, 0, sizeof(req));
    req.size_B = size_B;
    req.align_B = align_B;
    req.flags = flags;
    rc = br_call(dev, PG106_BR_OP_BO_ALLOC, &req, sizeof(req),
                 &req, sizeof(req), NULL);
    if (rc != 0) {
        return rc;
    }
    if (req.handle == 0) {
        return -EPROTO; /* 0 is never a valid handle (nouveau_drm.h) */
    }
    bo = &dev->bos[dev->n_bos++];
    memset(bo, 0, sizeof(*bo));
    bo->handle = req.handle;
    bo->flags = flags;
    bo->size_B = size_B;
    bo->alive = true;
    *handle_out = req.handle;
    return 0;
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
    for (size_t i = 0; i < dev->n_bindings; i++) {
        if (dev->bindings[i].handle == handle) {
            return -EBUSY;
        }
    }
    memset(&req, 0, sizeof(req));
    req.handle = handle;
    rc = br_call(dev, PG106_BR_OP_BO_FREE, &req, sizeof(req), NULL, 0, NULL);
    if (rc != 0) {
        return rc;
    }
    if (bo->map != NULL && dev->tr->unmap_cookie != NULL) {
        dev->tr->unmap_cookie(dev->tr, bo->map_cookie, bo->map);
    }
    bo->alive = false;
    return 0;
}

int pg106_br_bo_map(struct pg106_br_dev *dev, uint32_t handle,
                    uint32_t flags, void **map_out)
{
    struct pg106_br_bo *bo;
    struct pg106_br_bo_map req;
    int rc;

    if (dev == NULL || map_out == NULL) {
        return -EINVAL;
    }
    bo = br_bo_lookup(dev, handle);
    if (bo == NULL) {
        return PG106_UAPI_ENOENT;
    }
    if ((bo->flags & PG106_BR_BO_CAN_MAP) == 0) {
        return -EACCES; /* nvkmd requires NVKMD_MEM_CAN_MAP, nvkmd.h:51 */
    }
    if (bo->map != NULL) {
        return -EEXIST; /* one mapping at a time (nvkmd.h:70-74) */
    }
    memset(&req, 0, sizeof(req));
    req.handle = handle;
    req.flags = flags;
    rc = br_call(dev, PG106_BR_OP_BO_MAP, &req, sizeof(req),
                 &req, sizeof(req), NULL);
    if (rc != 0) {
        return rc;
    }
    bo->map_cookie = req.map_cookie;
    if (dev->tr->map_cookie != NULL) {
        rc = dev->tr->map_cookie(dev->tr, req.map_cookie, bo->size_B,
                                 &bo->map);
        if (rc != 0) {
            return rc;
        }
    }
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
    memset(&req, 0, sizeof(req));
    req.handle = handle;
    rc = br_call(dev, PG106_BR_OP_BO_UNMAP, &req, sizeof(req), NULL, 0, NULL);
    if (rc != 0) {
        return rc;
    }
    bo->map = NULL;
    bo->map_cookie = 0;
    return 0;
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
 * arrays are inline, see pg106_br_wire.h). */
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

static int br_flush_vm_bind(struct pg106_br_ctx *ctx)
{
    struct pg106_br_dev *dev = ctx->dev;
    struct pg106_uapi_vm_bind req;
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
        return PG106_UAPI_ENOSYS; /* VM_INIT first (uapi.h:190-199) */
    }

    /* Bridge-side state the stateless validator cannot check: handle
     * -> BO existence (-ENOENT), the per-BO size cross-check
     * (pg106_uapi_vm_bind_op_check_map, nouveau_uvmm.c:1048-1062),
     * and MAP overlap against already-applied bindings
     * (bind_validate_map_common, nouveau_uvmm.c:34-56 doc comment). */
    for (size_t i = 0; i < ctx->n_binds; i++) {
        const struct pg106_uapi_vm_bind_op *op = &ctx->binds[i];
        struct pg106_br_bo *bo;

        if (op->op != PG106_UAPI_VM_BIND_OP_MAP) {
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
        for (size_t j = 0; j < dev->n_bindings; j++) {
            const struct pg106_br_binding *b = &dev->bindings[j];

            if (op->addr < b->addr + b->range &&
                b->addr < op->addr + op->range) {
                return PG106_UAPI_EINVAL; /* overlaps an active MAP */
            }
        }
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

    rc = pg106_uapi_vm_bind_validate(&dev->vm, &req, ctx->binds,
                                     ctx->n_bind_waits ? ctx->bind_waits : NULL,
                                     ctx->n_bind_sigs ? ctx->bind_sigs : NULL);
    if (rc != 0) {
        return rc;
    }

    len = br_vm_bind_payload_len(ctx);
    buf = malloc(len);
    if (buf == NULL) {
        return -ENOMEM;
    }
    w.buf = buf;
    w.len = 0;
    w.cap = len;
    br_vm_bind_serialize(ctx, &req, &w);
    rc = br_call(dev, PG106_BR_OP_VM_BIND, buf, w.len, NULL, 0, NULL);
    free(buf);
    if (rc != 0) {
        return rc;
    }
    /* Success: mirror the ops into the binding table (the kext owns
     * the real region table; this is the bridge-side view the pinning
     * rule's unbind-before-release check reads). */
    for (size_t i = 0; i < ctx->n_binds; i++) {
        const struct pg106_uapi_vm_bind_op *op = &ctx->binds[i];

        if (op->op == PG106_UAPI_VM_BIND_OP_MAP) {
            dev->bindings[dev->n_bindings++] = (struct pg106_br_binding){
                op->handle, op->addr, op->range,
            };
        } else { /* UNMAP: drop the matching binding(s) */
            for (size_t j = 0; j < dev->n_bindings; j++) {
                if (dev->bindings[j].addr == op->addr) {
                    dev->bindings[j] = dev->bindings[dev->n_bindings - 1];
                    dev->n_bindings--;
                    break;
                }
            }
        }
    }
    ctx->n_binds = 0;
    ctx->n_bind_waits = 0;
    ctx->n_bind_sigs = 0;
    return 0;
}

/* EXEC payload: header + waits + sigs + pushes (waits before pushes,
 * signals after - the scheduler contract, NOUVEAU-UAPI.md section 2.4). */
static size_t br_exec_payload_len(const struct pg106_br_ctx *ctx)
{
    return sizeof(struct pg106_uapi_exec) +
           (ctx->n_exec_waits + ctx->n_exec_sigs) *
               sizeof(struct pg106_uapi_sync) +
           ctx->n_pushes * sizeof(struct pg106_uapi_exec_push);
}

static void br_exec_serialize(const struct pg106_br_ctx *ctx,
                              const struct pg106_uapi_exec *req,
                              struct br_writer *w)
{
    br_wr_put(w, req, sizeof(*req));
    for (size_t i = 0; i < ctx->n_exec_waits; i++) {
        br_wr_put(w, &ctx->exec_waits[i], sizeof(struct pg106_uapi_sync));
    }
    for (size_t i = 0; i < ctx->n_exec_sigs; i++) {
        br_wr_put(w, &ctx->exec_sigs[i], sizeof(struct pg106_uapi_sync));
    }
    for (size_t i = 0; i < ctx->n_pushes; i++) {
        br_wr_put(w, &ctx->pushes[i], sizeof(struct pg106_uapi_exec_push));
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
    rc = br_call(dev, PG106_BR_OP_EXEC, buf, w.len, NULL, 0, NULL);
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
     * mappings; nvkmd_nouveau_ctx.c:306-365). */
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
