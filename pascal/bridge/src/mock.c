// SPDX-License-Identifier: MIT
/*
 * pg106_br_mock.c - recording mock transport, the "kext" side of the
 * bridge in tests and the demo (see include/pg106_br_mock.h).
 *
 * The mock implements just enough server semantics to make the
 * demo/tests end-to-end (handles, GETPARAM answers, handshake fields,
 * CPU backing for BO maps, one-shot failure injection). It does NOT
 * re-validate uapi ops: validation is the bridge's job (through the
 * pg106_uapi_* validators) plus the kext's job later; the mock only
 * proves what bytes the bridge put on the channel and in which order.
 */
#include "pg106_br_mock.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "pg106_br_bridge.h" /* PG106_BR_ABI_* defaults for the handshake */

struct pg106_br_mock_bo {
    uint32_t handle;
    uint64_t size_B;
    uint64_t map_cookie;
    bool     mapped;
    void    *backing;
};

struct pg106_br_mock {
    struct pg106_br_transport tr;

    struct pg106_br_mock_rec *recs; /* grows; every frame is kept */
    size_t n_recs, cap_recs;

    struct pg106_br_mock_bo bos[PG106_BR_MOCK_MAX_BOS];
    size_t n_bos;
    uint32_t next_handle;
    uint64_t next_cookie;

    uint32_t kext_major, kext_minor;
    uint32_t drm_version;
    uint32_t ib_max;

    bool    fail_armed;
    uint8_t fail_op;
    int32_t fail_status;

    size_t  n_unmap_cookie; /* transport unmap_cookie() calls */

    /* Response-length fault injection (pg106_br_mock_short_reply). */
    bool    short_armed;
    uint8_t short_op;
};

/* ---- Test hooks ---------------------------------------------------- */

void pg106_br_mock_transport(struct pg106_br_mock *m,
                             struct pg106_br_transport *tr_out)
{
    if (tr_out != NULL) {
        *tr_out = m->tr;
    }
}

void pg106_br_mock_set_kext_version(struct pg106_br_mock *m,
                                    uint32_t major, uint32_t minor)
{
    m->kext_major = major;
    m->kext_minor = minor;
}

void pg106_br_mock_set_drm_version(struct pg106_br_mock *m,
                                   uint32_t drm_version)
{
    m->drm_version = drm_version;
}

void pg106_br_mock_set_ib_max(struct pg106_br_mock *m, uint32_t ib_max)
{
    m->ib_max = ib_max;
}

void pg106_br_mock_fail_next(struct pg106_br_mock *m, uint8_t op,
                             int32_t status)
{
    m->fail_armed = true;
    m->fail_op = op;
    m->fail_status = status;
}

void pg106_br_mock_short_reply(struct pg106_br_mock *m, uint8_t op)
{
    m->short_armed = true;
    m->short_op = op;
}

size_t pg106_br_mock_frame_count(const struct pg106_br_mock *m)
{
    return m->n_recs;
}

const struct pg106_br_mock_rec *pg106_br_mock_frame(
    const struct pg106_br_mock *m, size_t index)
{
    if (index >= m->n_recs) {
        return NULL;
    }
    return &m->recs[index];
}

void *pg106_br_mock_bo_backing(const struct pg106_br_mock *m, uint32_t handle)
{
    for (size_t i = 0; i < m->n_bos; i++) {
        if (m->bos[i].handle == handle) {
            return m->bos[i].backing;
        }
    }
    return NULL;
}

/* ---- Server semantics ---------------------------------------------- */

static struct pg106_br_mock_bo *mock_bo_by_handle(struct pg106_br_mock *m,
                                                  uint32_t handle)
{
    for (size_t i = 0; i < m->n_bos; i++) {
        if (m->bos[i].handle == handle) {
            return &m->bos[i];
        }
    }
    return NULL;
}

static struct pg106_br_mock_bo *mock_bo_by_cookie(struct pg106_br_mock *m,
                                                  uint64_t cookie)
{
    for (size_t i = 0; i < m->n_bos; i++) {
        if (m->bos[i].map_cookie == cookie) {
            return &m->bos[i];
        }
    }
    return NULL;
}

static int mock_getparam(const struct pg106_br_mock *m,
                         struct pg106_uapi_getparam *gp)
{
    switch (gp->param) {
    case PG106_UAPI_GETPARAM_PCI_VENDOR:
        gp->value = PG106_BR_MOCK_PCI_VENDOR;
        break;
    case PG106_UAPI_GETPARAM_PCI_DEVICE:
        gp->value = PG106_BR_MOCK_PCI_DEVICE;
        break;
    case PG106_UAPI_GETPARAM_EXEC_PUSH_MAX:
        /* Must equal gpfifo_ring/2 - 1 (nouveau_exec.h:45-52,
         * NOUVEAU-UAPI.md section 1). */
        gp->value = pg106_uapi_exec_push_max(m->ib_max);
        break;
    case PG106_UAPI_GETPARAM_VRAM_BAR_SIZE:
        gp->value = 6ull * 1024 * 1024 * 1024; /* Helios 300: 6 GiB */
        break;
    default:
        gp->value = 0; /* GRAPH_UNITS/PTIMER/VRAM_USED/etc: zeros fine
                          for a skeleton; the kext will fill these. */
        break;
    }
    return 0;
}

static int mock_handle_req(struct pg106_br_mock *m,
                           const struct pg106_br_frame *req,
                           const void *payload, size_t payload_len,
                           struct pg106_br_frame *resp, void *resp_payload,
                           size_t resp_cap, size_t *resp_len_out)
{
    memset(resp, 0, sizeof(*resp));
    resp->magic = PG106_BR_WIRE_MAGIC;
    resp->op = req->op;
    resp->wire_version = PG106_BR_WIRE_VERSION;
    resp->seq = req->seq;
    *resp_len_out = 0;

    switch (req->op) {
    case PG106_BR_OP_HANDSHAKE: {
        struct pg106_br_handshake hs;

        if (payload_len != sizeof(hs) || resp_cap < sizeof(hs)) {
            return -EPROTO;
        }
        memcpy(&hs, payload, sizeof(hs));
        memset(&hs, 0, sizeof(hs));
        hs.kext_major = m->kext_major;
        hs.kext_minor = m->kext_minor;
        hs.ib_max = m->ib_max;
        hs.drm_version = m->drm_version;
        hs.va_space_end = PG106_UAPI_VA_SPACE_END; /* 2^47 */
        memcpy(resp_payload, &hs, sizeof(hs));
        *resp_len_out = sizeof(hs);
        return 0;
    }
    case PG106_BR_OP_GETPARAM: {
        struct pg106_uapi_getparam gp;

        if (payload_len != sizeof(gp) || resp_cap < sizeof(gp)) {
            return -EPROTO;
        }
        memcpy(&gp, payload, sizeof(gp));
        mock_getparam(m, &gp);
        memcpy(resp_payload, &gp, sizeof(gp));
        *resp_len_out = sizeof(gp);
        return 0;
    }
    case PG106_BR_OP_VM_INIT:
        return 0; /* recorded; the kext will own the region table */

    case PG106_BR_OP_BO_ALLOC: {
        struct pg106_br_bo_alloc ba;
        struct pg106_br_mock_bo *bo;

        if (payload_len != sizeof(ba) || resp_cap < sizeof(ba)) {
            return -EPROTO;
        }
        memcpy(&ba, payload, sizeof(ba));
        if (m->n_bos >= PG106_BR_MOCK_MAX_BOS) {
            return -ENOMEM;
        }
        bo = &m->bos[m->n_bos++];
        memset(bo, 0, sizeof(*bo));
        bo->handle = ++m->next_handle; /* GEM handles start at 1 */
        bo->size_B = ba.size_B;
        bo->backing = calloc(1, ba.size_B);
        if (bo->backing == NULL) {
            m->n_bos--;
            return -ENOMEM;
        }
        memset(&ba, 0, sizeof(ba));
        ba.handle = bo->handle;
        memcpy(resp_payload, &ba, sizeof(ba));
        *resp_len_out = sizeof(ba);
        return 0;
    }
    case PG106_BR_OP_BO_FREE: {
        struct pg106_br_bo_free bf;
        struct pg106_br_mock_bo *bo;

        if (payload_len != sizeof(bf)) {
            return -EPROTO;
        }
        memcpy(&bf, payload, sizeof(bf));
        bo = mock_bo_by_handle(m, bf.handle);
        if (bo == NULL) {
            return PG106_UAPI_ENOENT;
        }
        free(bo->backing);
        *bo = m->bos[m->n_bos - 1];
        m->n_bos--;
        return 0;
    }
    case PG106_BR_OP_BO_MAP: {
        struct pg106_br_bo_map bm;
        struct pg106_br_mock_bo *bo;

        if (payload_len != sizeof(bm) || resp_cap < sizeof(bm)) {
            return -EPROTO;
        }
        memcpy(&bm, payload, sizeof(bm));
        bo = mock_bo_by_handle(m, bm.handle);
        if (bo == NULL) {
            return PG106_UAPI_ENOENT;
        }
        if (bo->mapped) {
            return -EEXIST; /* one mapping at a time (nvkmd.h:68-76) */
        }
        bo->mapped = true;
        bo->map_cookie = ++m->next_cookie;
        memset(&bm, 0, sizeof(bm));
        bm.map_cookie = bo->map_cookie;
        memcpy(resp_payload, &bm, sizeof(bm));
        *resp_len_out = sizeof(bm);
        return 0;
    }
    case PG106_BR_OP_BO_UNMAP: {
        struct pg106_br_bo_unmap bu;
        struct pg106_br_mock_bo *bo;

        if (payload_len != sizeof(bu)) {
            return -EPROTO;
        }
        memcpy(&bu, payload, sizeof(bu));
        bo = mock_bo_by_handle(m, bu.handle);
        if (bo == NULL) {
            return PG106_UAPI_ENOENT;
        }
        if (!bo->mapped) {
            return -EEXIST;
        }
        bo->mapped = false;
        bo->map_cookie = 0;
        return 0;
    }
    case PG106_BR_OP_VM_BIND:
    case PG106_BR_OP_EXEC:
        return 0; /* recorded only; see file comment */
    default:
        return -EPROTO;
    }
}

/* ---- Transport vtable ---------------------------------------------- */

static int mock_call(struct pg106_br_transport *tr,
                     const struct pg106_br_frame *req, const void *req_payload,
                     struct pg106_br_frame *resp, void *resp_payload,
                     size_t resp_cap, size_t *resp_len_out)
{
    struct pg106_br_mock *m = (struct pg106_br_mock *)tr->user;
    size_t payload_len, total, resp_len = 0;
    uint8_t *copy;
    int32_t status;

    if (req->magic != PG106_BR_WIRE_MAGIC ||
        req->wire_version != PG106_BR_WIRE_VERSION ||
        req->flags != 0 || req->reserved != 0) {
        return -EPROTO;
    }
    payload_len = req->payload_len;
    total = sizeof(*req) + payload_len;
    if (req_payload == NULL && payload_len != 0) {
        return -EPROTO;
    }

    /* Response header is always well-formed, even on an injected
     * failure path (status is the only field that changes). */
    memset(resp, 0, sizeof(*resp));
    resp->magic = PG106_BR_WIRE_MAGIC;
    resp->op = req->op;
    resp->wire_version = PG106_BR_WIRE_VERSION;
    resp->seq = req->seq;

    /* Record the exact request bytes. */
    if (m->n_recs == m->cap_recs) {
        size_t ncap = m->cap_recs ? m->cap_recs * 2 : 64;
        struct pg106_br_mock_rec *n = realloc(m->recs, ncap * sizeof(*n));

        if (n == NULL) {
            return -ENOMEM;
        }
        m->recs = n;
        m->cap_recs = ncap;
    }
    copy = malloc(total);
    if (copy == NULL) {
        return -ENOMEM;
    }
    memcpy(copy, req, sizeof(*req));
    if (payload_len != 0) {
        memcpy(copy + sizeof(*req), req_payload, payload_len);
    }
    m->recs[m->n_recs].op = req->op;
    m->recs[m->n_recs].seq = req->seq;
    m->recs[m->n_recs].bytes = copy;
    m->recs[m->n_recs].len = total;

    /* One-shot failure injection (transport-level). */
    if (m->fail_armed && m->fail_op == req->op) {
        m->fail_armed = false;
        status = m->fail_status;
    } else {
        status = (int32_t)mock_handle_req(m, req, req_payload, payload_len,
                                          resp, resp_payload, resp_cap,
                                          &resp_len);
    }

    m->recs[m->n_recs].status = status;
    m->n_recs++;

    /* Error responses carry no payload. */
    if (status != 0) {
        resp_len = 0;
    }
    if (m->short_armed && m->short_op == req->op && resp_len > 0) {
        m->short_armed = false;
        resp_len -= 1; /* a truncated reply, as from a buggy kext */
    }
    resp->status = status;
    resp->payload_len = (uint32_t)resp_len;
    if (resp_len_out != NULL) {
        *resp_len_out = resp_len;
    }
    return 0;
}

static int mock_map_cookie(struct pg106_br_transport *tr, uint64_t cookie,
                           uint64_t size_B, void **ptr_out)
{
    struct pg106_br_mock *m = (struct pg106_br_mock *)tr->user;
    struct pg106_br_mock_bo *bo = mock_bo_by_cookie(m, cookie);

    if (bo == NULL || bo->backing == NULL) {
        return -ENOENT;
    }
    (void)size_B; /* the mock's backing is already the full BO */
    *ptr_out = bo->backing;
    return 0;
}

static void mock_unmap_cookie(struct pg106_br_transport *tr, uint64_t cookie,
                              void *ptr)
{
    struct pg106_br_mock *m = (struct pg106_br_mock *)tr->user;

    (void)cookie;
    (void)ptr; /* backing is freed with the BO */
    m->n_unmap_cookie++;
}

size_t pg106_br_mock_unmap_cookie_count(const struct pg106_br_mock *m)
{
    return m->n_unmap_cookie;
}

/* ---- Lifecycle ----------------------------------------------------- */

struct pg106_br_mock *pg106_br_mock_create(void)
{
    struct pg106_br_mock *m = calloc(1, sizeof(*m));

    if (m == NULL) {
        return NULL;
    }
    m->kext_major = PG106_BR_ABI_MAJOR;
    m->kext_minor = PG106_BR_ABI_MINOR;
    m->drm_version = PG106_BR_MOCK_DRM_VERSION;
    m->ib_max = PG106_BR_MOCK_IB_MAX;
    m->tr.call = mock_call;
    m->tr.map_cookie = mock_map_cookie;
    m->tr.unmap_cookie = mock_unmap_cookie;
    m->tr.user = m;
    return m;
}

void pg106_br_mock_destroy(struct pg106_br_mock *m)
{
    if (m == NULL) {
        return;
    }
    for (size_t i = 0; i < m->n_recs; i++) {
        free((void *)m->recs[i].bytes);
    }
    free(m->recs);
    for (size_t i = 0; i < m->n_bos; i++) {
        free(m->bos[i].backing);
    }
    free(m);
}
