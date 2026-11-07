// SPDX-License-Identifier: MIT
/*
 * pascal/bridge/tests/test_bridge.c - mock-transport tests for the
 * pg106 bridge skeleton:
 *
 *   1. wire-layout goldens (frame header + bridge payloads)
 *   2. version handshake (accept, drm floor, kext version mismatch,
 *      handshake frames actually recorded)
 *   3. bind -> exec ordering on the wire
 *   4. sync semantics (waits before pushes, signals after; sync-only
 *      empty exec flush; syncs force RUN_ASYNC; bind syncs without
 *      ops rejected)
 *   5. validator rejections surface as op errors without a frame
 *   6. transport failure injection surfaces as op error
 *   7. byte-golden frames: HANDSHAKE, VM_BIND, EXEC (embedded arrays)
 *   8. VA table: drm_gpuvm split/replace, batch order, sparse regions
 *   9. resource lifetime: BO slot reuse, unmap_cookie, response length
 *
 * Byte-goldens are little-endian x86-64 (the target platform, Helios
 * 300) and pin the wire format against accidental change.
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pg106_br_bridge.h"
#include "pg106_br_mock.h"

static int failures;

#define CHECK(cond) do { \
    if (cond) { printf("PASS %s\n", #cond); } \
    else { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

/* The NVK-shaped VM for the sequences below (same shape as
 * pascal/tests/test_uapi.c: winsys reserves the low region and gives
 * UMD the rest, nouveau_device.c:508-515). */
#define KERN_MANAGED_ADDR  0x40000000ull /* 1 GiB */
#define KERN_MANAGED_SIZE  0x40000000ull /* 1 GiB */

struct env {
    struct pg106_br_mock *mock;
    struct pg106_br_transport tr;
    struct pg106_br_dev *dev;
    struct pg106_br_ctx *ctx;
};

static int env_setup(struct env *e)
{
    memset(e, 0, sizeof(*e));
    e->mock = pg106_br_mock_create();
    if (e->mock == NULL) {
        return -1;
    }
    pg106_br_mock_transport(e->mock, &e->tr);
    if (pg106_br_dev_open(&e->tr, &e->dev) != 0) {
        return -1;
    }
    if (pg106_br_vm_init(e->dev, KERN_MANAGED_ADDR, KERN_MANAGED_SIZE) != 0) {
        return -1;
    }
    if (pg106_br_ctx_create(e->dev, 0, &e->ctx) != 0) {
        return -1;
    }
    return 0;
}

static void env_teardown(struct env *e)
{
    pg106_br_ctx_destroy(e->ctx);
    pg106_br_dev_close(e->dev);
    pg106_br_mock_destroy(e->mock);
}

/* A failed flush leaves its batch queued (see test_transport_failure),
 * so exec-side rejection cases each start from a fresh ctx. */
static int ctx_renew(struct env *e)
{
    pg106_br_ctx_destroy(e->ctx);
    return pg106_br_ctx_create(e->dev, 0, &e->ctx);
}

/* ------------------------------------------------------------------ */
/* 1. Wire-layout goldens                                              */
/* ------------------------------------------------------------------ */

static void test_layout(void)
{
    CHECK(sizeof(struct pg106_br_frame) == 24);
    CHECK(offsetof(struct pg106_br_frame, payload_len) == 8);
    CHECK(offsetof(struct pg106_br_frame, seq) == 12);
    CHECK(offsetof(struct pg106_br_frame, reserved) == 16);
    CHECK(offsetof(struct pg106_br_frame, status) == 20);
    CHECK(sizeof(struct pg106_br_handshake) == 32);
    CHECK(sizeof(struct pg106_br_bo_alloc) == 24);
    CHECK(sizeof(struct pg106_br_bo_map) == 16);
    /* BO flags mirror nvkmd_mem_flags (nvkmd.h:35-58). */
    CHECK(PG106_BR_BO_LOCAL == (1u << 0));
    CHECK(PG106_BR_BO_GART == (1u << 1));
    CHECK(PG106_BR_BO_VRAM == (1u << 2));
    CHECK(PG106_BR_BO_CAN_MAP == (1u << 3));
    CHECK(PG106_BR_BO_SHARED == (1u << 4));
    CHECK(PG106_BR_BO_COHERENT == (1u << 5));
    /* Magic is "PG10" read sequentially off the wire. */
    {
        const uint8_t magic[4] = { 0x50, 0x47, 0x31, 0x30 }; /* P G 1 0 */
        uint32_t m = PG106_BR_WIRE_MAGIC;
        CHECK(memcmp(&m, magic, 4) == 0);
    }
}

/* ------------------------------------------------------------------ */
/* 2. Handshake                                                        */
/* ------------------------------------------------------------------ */

static void test_handshake(void)
{
    struct pg106_br_mock *mock = pg106_br_mock_create();
    struct pg106_br_transport tr;
    struct pg106_br_dev *dev = NULL;
    const struct pg106_br_mock_rec *rec;
    uint64_t value = 0;

    CHECK(mock != NULL);
    pg106_br_mock_transport(mock, &tr);

    /* drm floor: Mesa refuses below 0x01000301 (nouveau_device.c:504). */
    pg106_br_mock_set_drm_version(mock, 0x01000300u);
    CHECK(pg106_br_dev_open(&tr, &dev) == -EINVAL);
    CHECK(dev == NULL);
    pg106_br_mock_set_drm_version(mock, PG106_BR_MOCK_DRM_VERSION);

    /* kext version mismatch. */
    pg106_br_mock_set_kext_version(mock, 1u, 0u);
    CHECK(pg106_br_dev_open(&tr, &dev) == -EPROTONOSUPPORT);
    pg106_br_mock_set_kext_version(mock, PG106_BR_ABI_MAJOR,
                                   PG106_BR_ABI_MINOR);

    /* Older minor than the bridge was built against. */
    pg106_br_mock_set_kext_version(mock, PG106_BR_ABI_MAJOR, 0u);
    CHECK(pg106_br_dev_open(&tr, &dev) == -EPROTONOSUPPORT);
    pg106_br_mock_set_kext_version(mock, PG106_BR_ABI_MAJOR,
                                   PG106_BR_ABI_MINOR);

    /* Good open: the three rejected attempts also recorded their
     * handshake frames (open is an op like any other). */
    CHECK(pg106_br_dev_open(&tr, &dev) == 0);
    CHECK(pg106_br_mock_frame_count(mock) == 4);
    rec = pg106_br_mock_frame(mock, 3);
    CHECK(rec != NULL && rec->op == PG106_BR_OP_HANDSHAKE);
    CHECK(rec != NULL && rec->status == 0);
    CHECK(rec != NULL && rec->seq == 0); /* seq counter is per-dev */

    /* GETPARAM answers flow back through the response payload. */
    CHECK(pg106_br_getparam(dev, PG106_UAPI_GETPARAM_EXEC_PUSH_MAX,
                            &value) == 0);
    CHECK(value == pg106_uapi_exec_push_max(PG106_BR_MOCK_IB_MAX));
    CHECK(pg106_br_getparam(dev, PG106_UAPI_GETPARAM_PCI_VENDOR,
                            &value) == 0);
    CHECK(value == PG106_BR_MOCK_PCI_VENDOR);
    CHECK(pg106_br_mock_frame_count(mock) == 6);

    pg106_br_dev_close(dev);
    pg106_br_mock_destroy(mock);
}

/* ------------------------------------------------------------------ */
/* 3. bind -> exec ordering                                            */
/* ------------------------------------------------------------------ */

static void test_ordering(void)
{
    struct env e;
    struct pg106_uapi_vm_bind_op op;
    struct pg106_uapi_exec_push push;
    struct pg106_uapi_sync sig;
    uint32_t handle = 0;
    uint64_t va = 0;
    void *map = NULL;

    CHECK(env_setup(&e) == 0);

    CHECK(pg106_br_bo_alloc(e.dev, 0x10000, 0x1000,
                            PG106_BR_BO_LOCAL | PG106_BR_BO_CAN_MAP,
                            &handle) == 0);
    CHECK(pg106_br_bo_map(e.dev, handle, 0, &map) == 0);
    CHECK(map != NULL);
    CHECK(pg106_br_va_alloc(e.dev, 0x10000, 0x1000, &va) == 0);

    /* Queue a MAP and a push in one flush; the VM_BIND frame must hit
     * the wire before the EXEC frame (pushes must see the mapping). */
    memset(&op, 0, sizeof(op));
    op.op = PG106_UAPI_VM_BIND_OP_MAP;
    op.handle = handle;
    op.addr = va;
    op.range = 0x1000;
    CHECK(pg106_br_ctx_bind(e.ctx, &op) == 0);

    memset(&push, 0, sizeof(push));
    push.va = va;
    push.va_len = 8;
    CHECK(pg106_br_ctx_exec(e.ctx, &push) == 0);

    memset(&sig, 0, sizeof(sig));
    sig.flags = PG106_UAPI_SYNC_SYNCOBJ;
    sig.handle = 0x22;
    CHECK(pg106_br_ctx_exec_signal(e.ctx, &sig) == 0);

    CHECK(pg106_br_ctx_flush(e.ctx) == 0);

    /* Frames: handshake(0), vm_init(1), bo_alloc(2), bo_map(3),
     * vm_bind(4), exec(5). */
    CHECK(pg106_br_mock_frame_count(e.mock) == 6);
    CHECK(pg106_br_mock_frame(e.mock, 4)->op == PG106_BR_OP_VM_BIND);
    CHECK(pg106_br_mock_frame(e.mock, 5)->op == PG106_BR_OP_EXEC);

    /* A second flush with nothing queued sends nothing. */
    CHECK(pg106_br_ctx_flush(e.ctx) == 0);
    CHECK(pg106_br_mock_frame_count(e.mock) == 6);

    env_teardown(&e);
}

/* ------------------------------------------------------------------ */
/* 4. Sync semantics                                                   */
/* ------------------------------------------------------------------ */

static void test_sync_semantics(void)
{
    struct env e;
    struct pg106_uapi_vm_bind_op op;
    struct pg106_uapi_exec_push push;
    struct pg106_uapi_sync wait, sig;
    struct pg106_uapi_vm_bind wb;
    struct pg106_uapi_exec we;
    uint32_t handle = 0;
    uint64_t va = 0;
    const uint8_t *bytes;
    size_t len;

    CHECK(env_setup(&e) == 0);

    CHECK(pg106_br_bo_alloc(e.dev, 0x10000, 0x1000, PG106_BR_BO_LOCAL,
                            &handle) == 0);
    CHECK(pg106_br_va_alloc(e.dev, 0x10000, 0x1000, &va) == 0);

    /* Bind syncs without ops: the kernel rejects a sync job with
     * syncs but no RUN_ASYNC ops (-EINVAL, nouveau_sched.c:43-46);
     * the bridge refuses before serializing. */
    memset(&sig, 0, sizeof(sig));
    sig.handle = 0x22;
    CHECK(pg106_br_ctx_bind_signal(e.ctx, &sig) == 0);
    CHECK(pg106_br_ctx_flush(e.ctx) == -EINVAL);
    CHECK(pg106_br_mock_frame_count(e.mock) == 3); /* nothing new: setup,
                                       bo_alloc only */

    memset(&op, 0, sizeof(op));
    op.op = PG106_UAPI_VM_BIND_OP_MAP;
    op.handle = handle;
    op.addr = va;
    op.range = 0x1000;
    CHECK(pg106_br_ctx_bind(e.ctx, &op) == 0);
    memset(&wait, 0, sizeof(wait));
    wait.flags = PG106_UAPI_SYNC_TIMELINE_SYNCOBJ;
    wait.handle = 0x11;
    wait.timeline_value = 7;
    CHECK(pg106_br_ctx_bind_wait(e.ctx, &wait) == 0);
    /* syncs force RUN_ASYNC on the wire (bridge sets the bit). */
    CHECK(pg106_br_ctx_flush(e.ctx) == 0);
    bytes = pg106_br_mock_frame(e.mock, 3)->bytes;
    len = pg106_br_mock_frame(e.mock, 3)->len;
    CHECK(len == PG106_BR_WIRE_FRAME_LEN(112));
    memcpy(&wb, bytes + sizeof(struct pg106_br_frame), sizeof(wb));
    CHECK(wb.flags == PG106_UAPI_VM_BIND_RUN_ASYNC);
    CHECK(wb.op_count == 1 && wb.wait_count == 1 && wb.sig_count == 1);
    /* waits precede ops, sigs follow (array order on the wire). */
    CHECK(memcmp(bytes + PG106_BR_WIRE_FRAME_LEN(0) +
                     sizeof(struct pg106_uapi_vm_bind) +
                     wb.op_count * sizeof(struct pg106_uapi_vm_bind_op),
                 &wait, sizeof(wait)) == 0);

    /* Sync-only empty exec flush (nvkmd_nouveau_ctx.c:242-292). */
    memset(&sig, 0, sizeof(sig));
    sig.handle = 0x22;
    CHECK(pg106_br_ctx_exec_signal(e.ctx, &sig) == 0);
    CHECK(pg106_br_ctx_flush(e.ctx) == 0);
    CHECK(pg106_br_mock_frame(e.mock, 4)->op == PG106_BR_OP_EXEC);
    bytes = pg106_br_mock_frame(e.mock, 4)->bytes;
    memcpy(&we, bytes + sizeof(struct pg106_br_frame), sizeof(we));
    CHECK(we.push_count == 0 && we.wait_count == 0 && we.sig_count == 1);

    /* Exec with wait+push+signal: arrays in the kernel's copy order,
     * pushes, waits, sigs (nouveau_exec_ucopy). */
    memset(&wait, 0, sizeof(wait));
    wait.flags = PG106_UAPI_SYNC_TIMELINE_SYNCOBJ;
    wait.handle = 0x11;
    wait.timeline_value = 7;
    CHECK(pg106_br_ctx_exec_wait(e.ctx, &wait) == 0);
    memset(&push, 0, sizeof(push));
    push.va = va;
    push.va_len = 8;
    CHECK(pg106_br_ctx_exec(e.ctx, &push) == 0);
    CHECK(pg106_br_ctx_exec_signal(e.ctx, &sig) == 0);
    CHECK(pg106_br_ctx_flush(e.ctx) == 0);
    CHECK(pg106_br_mock_frame(e.mock, 5)->op == PG106_BR_OP_EXEC);
    bytes = pg106_br_mock_frame(e.mock, 5)->bytes;
    len = pg106_br_mock_frame(e.mock, 5)->len;
    CHECK(len == PG106_BR_WIRE_FRAME_LEN(
                      sizeof(struct pg106_uapi_exec) + 3 * 16));
    {
        size_t base = PG106_BR_WIRE_FRAME_LEN(0);
        CHECK(memcmp(bytes + base + sizeof(struct pg106_uapi_exec),
                     &push, sizeof(push)) == 0);
        CHECK(memcmp(bytes + base + sizeof(struct pg106_uapi_exec) +
                         sizeof(struct pg106_uapi_exec_push),
                     &wait, sizeof(wait)) == 0);
        CHECK(memcmp(bytes + base + sizeof(struct pg106_uapi_exec) +
                         sizeof(struct pg106_uapi_exec_push) +
                         sizeof(struct pg106_uapi_sync),
                     &sig, sizeof(sig)) == 0);
    }

    env_teardown(&e);
}

/* ------------------------------------------------------------------ */
/* 5. Validator rejections surface as op errors, no frame sent         */
/* ------------------------------------------------------------------ */

static void test_validator_rejections(void)
{
    struct env e;
    struct pg106_uapi_vm_bind_op op;
    struct pg106_uapi_exec_push push;
    uint32_t handle = 0;
    uint64_t va = 0;

    CHECK(env_setup(&e) == 0);
    CHECK(pg106_br_bo_alloc(e.dev, 0x10000, 0x1000, PG106_BR_BO_LOCAL,
                            &handle) == 0);
    CHECK(pg106_br_va_alloc(e.dev, 0x10000, 0x1000, &va) == 0);

    /* Unaligned range -> -EINVAL from the validator. */
    memset(&op, 0, sizeof(op));
    op.op = PG106_UAPI_VM_BIND_OP_MAP;
    op.handle = handle;
    op.addr = va;
    op.range = 0x800; /* not page-aligned */
    CHECK(pg106_br_ctx_bind(e.ctx, &op) == 0);
    CHECK(pg106_br_ctx_flush(e.ctx) == PG106_UAPI_EINVAL);
    CHECK(pg106_br_mock_frame_count(e.mock) == 3); /* nothing new */

    /* Unknown BO handle -> -ENOENT (bridge-side state). Fresh ctx: ops
     * are checked in job order, so the queued unaligned op would fail
     * first, as on the kernel. */
    CHECK(ctx_renew(&e) == 0);
    op.range = 0x1000;
    op.handle = 0xdead;
    CHECK(pg106_br_ctx_bind(e.ctx, &op) == 0);
    CHECK(pg106_br_ctx_flush(e.ctx) == PG106_UAPI_ENOENT);
    CHECK(pg106_br_mock_frame_count(e.mock) == 3);

    /* MAP range past the end of the BO (bo-size cross-check). */
    CHECK(ctx_renew(&e) == 0);
    op.handle = handle;
    op.bo_offset = 0xF000;
    op.range = 0x2000;
    CHECK(pg106_br_ctx_bind(e.ctx, &op) == 0);
    CHECK(pg106_br_ctx_flush(e.ctx) == PG106_UAPI_EINVAL);
    CHECK(pg106_br_mock_frame_count(e.mock) == 3);

    /* Exec-side cases: fresh ctx per case (a failed flush leaves the
     * batch queued, which would mask the intended rejection). */

    /* Push longer than NV50_DMA_PUSH_MAX_LENGTH. */
    CHECK(ctx_renew(&e) == 0);
    memset(&push, 0, sizeof(push));
    push.va = va;
    push.va_len = PG106_UAPI_DMA_PUSH_MAX_LENGTH + 4;
    CHECK(pg106_br_ctx_exec(e.ctx, &push) == 0);
    CHECK(pg106_br_ctx_flush(e.ctx) == PG106_UAPI_EINVAL);
    CHECK(pg106_br_mock_frame_count(e.mock) == 3);

    /* Unknown sync type -> -EOPNOTSUPP. */
    CHECK(ctx_renew(&e) == 0);
    CHECK(pg106_br_ctx_exec_wait(e.ctx,
        &(struct pg106_uapi_sync){ 2, 0x21, 0 }) == 0);
    push.va_len = 8;
    CHECK(pg106_br_ctx_exec(e.ctx, &push) == 0);
    CHECK(pg106_br_ctx_flush(e.ctx) == PG106_UAPI_EOPNOTSUPP);
    CHECK(pg106_br_mock_frame_count(e.mock) == 3);

    /* Push count over the ring limit: ib_max 940 -> max 469, the 470th
     * queue attempt is the op error. */
    CHECK(ctx_renew(&e) == 0);
    {
        struct pg106_uapi_exec_push many[470];
        memset(many, 0, sizeof(many));
        for (size_t i = 0; i < 470; i++) {
            many[i].va = va;
            many[i].va_len = 4;
        }
        for (size_t i = 0; i < 469; i++) {
            CHECK(pg106_br_ctx_exec(e.ctx, &many[i]) == 0);
        }
        CHECK(pg106_br_ctx_exec(e.ctx, &many[469]) == -EINVAL);
        CHECK(pg106_br_ctx_flush(e.ctx) == 0); /* the 469 do go out */
        CHECK(pg106_br_mock_frame_count(e.mock) == 4);
    }

    /* Unknown channel id -> -ENOENT at ctx creation. */
    {
        struct pg106_br_ctx *c = NULL;
        CHECK(pg106_br_ctx_create(e.dev, 7, &c) == PG106_UAPI_ENOENT);
        CHECK(c == NULL);
    }

    env_teardown(&e);
}

/* ------------------------------------------------------------------ */
/* 6. Transport failure injection                                      */
/* ------------------------------------------------------------------ */

static void test_transport_failure(void)
{
    struct env e;
    struct pg106_uapi_vm_bind_op op;
    uint32_t handle = 0;
    uint64_t va = 0;

    CHECK(env_setup(&e) == 0);
    CHECK(pg106_br_bo_alloc(e.dev, 0x10000, 0x1000, PG106_BR_BO_LOCAL,
                            &handle) == 0);
    CHECK(pg106_br_va_alloc(e.dev, 0x10000, 0x1000, &va) == 0);

    /* Injected VM_BIND failure comes back as the op error and clears. */
    memset(&op, 0, sizeof(op));
    op.op = PG106_UAPI_VM_BIND_OP_MAP;
    op.handle = handle;
    op.addr = va;
    op.range = 0x1000;
    CHECK(pg106_br_ctx_bind(e.ctx, &op) == 0);
    pg106_br_mock_fail_next(e.mock, PG106_BR_OP_VM_BIND, -ENOMEM);
    CHECK(pg106_br_ctx_flush(e.ctx) == -ENOMEM);
    CHECK(pg106_br_mock_frame(e.mock, 3)->status == -ENOMEM);

    /* Queue drained? No: on failure the batch stays queued, so a retry
     * re-sends the same op. */
    CHECK(pg106_br_ctx_flush(e.ctx) == 0);
    CHECK(pg106_br_mock_frame(e.mock, 4)->op == PG106_BR_OP_VM_BIND);
    CHECK(pg106_br_mock_frame(e.mock, 4)->status == 0);

    env_teardown(&e);
}

/* ------------------------------------------------------------------ */
/* 6b. Pinning rule: unbind-before-release (NOUVEAU-UAPI.md section 6) */
/* ------------------------------------------------------------------ */

static void test_pinning_rule(void)
{
    struct env e;
    struct pg106_uapi_vm_bind_op op;
    uint32_t handle = 0;
    uint64_t va = 0, va2 = 0;

    CHECK(env_setup(&e) == 0);
    CHECK(pg106_br_bo_alloc(e.dev, 0x10000, 0x1000, PG106_BR_BO_LOCAL,
                            &handle) == 0);
    CHECK(pg106_br_va_alloc(e.dev, 0x10000, 0x1000, &va) == 0);
    CHECK(pg106_br_va_alloc(e.dev, 0x10000, 0x1000, &va2) == 0);

    /* Free while bound is refused: pages stay protected for the bind
     * lifetime; release ordering is UNMAP first. */
    memset(&op, 0, sizeof(op));
    op.op = PG106_UAPI_VM_BIND_OP_MAP;
    op.handle = handle;
    op.addr = va;
    op.range = 0x1000;
    CHECK(pg106_br_ctx_bind(e.ctx, &op) == 0);
    CHECK(pg106_br_ctx_flush(e.ctx) == 0);
    CHECK(pg106_br_bo_free(e.dev, handle) == -EBUSY);
    CHECK(pg106_br_mock_frame(e.mock, 3)->op == PG106_BR_OP_VM_BIND);
    CHECK(pg106_br_mock_frame_count(e.mock) == 4); /* no BO_FREE frame */

    /* UNMAP, then free goes through (and the address is reusable). */
    memset(&op, 0, sizeof(op));
    op.op = PG106_UAPI_VM_BIND_OP_UNMAP;
    op.addr = va;
    op.range = 0x1000;
    CHECK(pg106_br_ctx_bind(e.ctx, &op) == 0);
    CHECK(pg106_br_ctx_flush(e.ctx) == 0);
    CHECK(pg106_br_bo_free(e.dev, handle) == 0);
    CHECK(pg106_br_mock_frame_count(e.mock) == 6);

    /* After the UNMAP, mapping a fresh VA does not overlap. */
    CHECK(pg106_br_bo_alloc(e.dev, 0x10000, 0x1000, PG106_BR_BO_LOCAL,
                            &handle) == 0);
    memset(&op, 0, sizeof(op));
    op.op = PG106_UAPI_VM_BIND_OP_MAP;
    op.handle = handle;
    op.addr = va2;
    op.range = 0x1000;
    CHECK(pg106_br_ctx_bind(e.ctx, &op) == 0);
    CHECK(pg106_br_ctx_flush(e.ctx) == 0);

    env_teardown(&e);
}

/* ------------------------------------------------------------------ */
/* 7. Byte-golden frames (embedded expected arrays)                    */
/* ------------------------------------------------------------------ */

/*
 * Golden VM_BIND frame: fresh dev -> handshake(0), vm_init(1),
 * bo_alloc(2) [64 KiB, LOCAL|CAN_MAP, handle 1], va_alloc(0x1000),
 * MAP op {handle 1, addr 0x1000, range 0x1000}, one timeline wait
 * {handle 0x11, value 7}, one syncobj signal {handle 0x22}, flush.
 * seq = 3, payload_len = 40 + 40 + 16 + 16 = 112.
 */
static const uint8_t kGoldenVmBind[] = {
    /* pg106_br_frame */
    0x50, 0x47, 0x31, 0x30,          /* magic "PG10"            */
    0x07,                            /* op = VM_BIND            */
    0x00,                            /* wire_version            */
    0x00, 0x00,                      /* flags                   */
    0x70, 0x00, 0x00, 0x00,          /* payload_len = 112       */
    0x03, 0x00, 0x00, 0x00,          /* seq = 3                 */
    0x00, 0x00, 0x00, 0x00,          /* reserved                */
    0x00, 0x00, 0x00, 0x00,          /* status = 0              */
    /* struct pg106_uapi_vm_bind */
    0x01, 0x00, 0x00, 0x00,          /* op_count = 1            */
    0x01, 0x00, 0x00, 0x00,          /* flags = RUN_ASYNC       */
    0x01, 0x00, 0x00, 0x00,          /* wait_count = 1          */
    0x01, 0x00, 0x00, 0x00,          /* sig_count = 1           */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* wait_ptr = 0 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* sig_ptr = 0  */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* op_ptr = 0   */
    /* struct pg106_uapi_vm_bind_op[0] */
    0x00, 0x00, 0x00, 0x00,          /* op = MAP                */
    0x00, 0x00, 0x00, 0x00,          /* flags = 0               */
    0x01, 0x00, 0x00, 0x00,          /* handle = 1              */
    0x00, 0x00, 0x00, 0x00,          /* pad = 0                 */
    0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* addr = 0x1000 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* bo_offset = 0 */
    0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* range = 0x1000 */
    /* struct pg106_uapi_sync (wait) */
    0x01, 0x00, 0x00, 0x00,          /* flags = TIMELINE_SYNCOBJ */
    0x11, 0x00, 0x00, 0x00,          /* handle = 0x11           */
    0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* timeline = 7 */
    /* struct pg106_uapi_sync (signal) */
    0x00, 0x00, 0x00, 0x00,          /* flags = SYNCOBJ         */
    0x22, 0x00, 0x00, 0x00,          /* handle = 0x22           */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* timeline = 0 */
};

/*
 * Golden EXEC frame: same session, one push {va 0x1000, va_len 8,
 * flags 0}, same wait/signal pair, in the kernel's copy order (pushes,
 * waits, sigs). seq = 4, payload_len = 40 + 16 + 16 + 16 = 88.
 */
static const uint8_t kGoldenExec[] = {
    /* pg106_br_frame */
    0x50, 0x47, 0x31, 0x30,          /* magic "PG10"            */
    0x08,                            /* op = EXEC               */
    0x00,                            /* wire_version            */
    0x00, 0x00,                      /* flags                   */
    0x58, 0x00, 0x00, 0x00,          /* payload_len = 88        */
    0x04, 0x00, 0x00, 0x00,          /* seq = 4                 */
    0x00, 0x00, 0x00, 0x00,          /* reserved                */
    0x00, 0x00, 0x00, 0x00,          /* status = 0              */
    /* struct pg106_uapi_exec */
    0x00, 0x00, 0x00, 0x00,          /* channel = 0             */
    0x01, 0x00, 0x00, 0x00,          /* push_count = 1          */
    0x01, 0x00, 0x00, 0x00,          /* wait_count = 1          */
    0x01, 0x00, 0x00, 0x00,          /* sig_count = 1           */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* wait_ptr = 0 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* sig_ptr = 0  */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* push_ptr = 0 */
    /* struct pg106_uapi_exec_push[0] */
    0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* va = 0x1000  */
    0x08, 0x00, 0x00, 0x00,          /* va_len = 8              */
    0x00, 0x00, 0x00, 0x00,          /* flags = 0               */
    /* struct pg106_uapi_sync (wait) */
    0x01, 0x00, 0x00, 0x00,
    0x11, 0x00, 0x00, 0x00,
    0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    /* struct pg106_uapi_sync (signal) */
    0x00, 0x00, 0x00, 0x00,
    0x22, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static void test_byte_goldens(void)
{
    struct env e;
    struct pg106_uapi_vm_bind_op op;
    struct pg106_uapi_exec_push push;
    struct pg106_uapi_sync wait, sig;
    uint32_t handle = 0;
    uint64_t va = 0;
    const struct pg106_br_mock_rec *rec;

    CHECK(env_setup(&e) == 0);
    CHECK(pg106_br_bo_alloc(e.dev, 0x10000, 0x1000,
                            PG106_BR_BO_LOCAL | PG106_BR_BO_CAN_MAP,
                            &handle) == 0);
    CHECK(pg106_br_va_alloc(e.dev, 0x10000, 0x1000, &va) == 0);
    CHECK(va == 0x1000);

    memset(&op, 0, sizeof(op));
    op.op = PG106_UAPI_VM_BIND_OP_MAP;
    op.handle = handle;
    op.addr = va;
    op.range = 0x1000;
    CHECK(pg106_br_ctx_bind(e.ctx, &op) == 0);
    memset(&wait, 0, sizeof(wait));
    wait.flags = PG106_UAPI_SYNC_TIMELINE_SYNCOBJ;
    wait.handle = 0x11;
    wait.timeline_value = 7;
    CHECK(pg106_br_ctx_bind_wait(e.ctx, &wait) == 0);
    memset(&sig, 0, sizeof(sig));
    sig.handle = 0x22;
    CHECK(pg106_br_ctx_bind_signal(e.ctx, &sig) == 0);
    CHECK(pg106_br_ctx_flush(e.ctx) == 0);

    memset(&push, 0, sizeof(push));
    push.va = va;
    push.va_len = 8;
    CHECK(pg106_br_ctx_exec(e.ctx, &push) == 0);
    CHECK(pg106_br_ctx_exec_wait(e.ctx, &wait) == 0);
    CHECK(pg106_br_ctx_exec_signal(e.ctx, &sig) == 0);
    CHECK(pg106_br_ctx_flush(e.ctx) == 0);

    rec = pg106_br_mock_frame(e.mock, 3);
    CHECK(rec != NULL && rec->op == PG106_BR_OP_VM_BIND);
    CHECK(rec != NULL && rec->len == sizeof(kGoldenVmBind));
    CHECK(rec != NULL &&
          memcmp(rec->bytes, kGoldenVmBind, sizeof(kGoldenVmBind)) == 0);

    rec = pg106_br_mock_frame(e.mock, 4);
    CHECK(rec != NULL && rec->op == PG106_BR_OP_EXEC);
    CHECK(rec != NULL && rec->len == sizeof(kGoldenExec));
    CHECK(rec != NULL &&
          memcmp(rec->bytes, kGoldenExec, sizeof(kGoldenExec)) == 0);

    env_teardown(&e);
}


/* ------------------------------------------------------------------ */
/* 8. VA table: split/replace, batch order, sparse regions             */
/* ------------------------------------------------------------------ */

static struct pg106_uapi_vm_bind_op mk_op(uint32_t opc, uint32_t flags,
                                          uint32_t handle, uint64_t addr,
                                          uint64_t bo_offset, uint64_t range)
{
    struct pg106_uapi_vm_bind_op op;

    memset(&op, 0, sizeof(op));
    op.op = opc;
    op.flags = flags;
    op.handle = handle;
    op.addr = addr;
    op.bo_offset = bo_offset;
    op.range = range;
    return op;
}

static int bind1(struct env *e, struct pg106_uapi_vm_bind_op op)
{
    int rc = pg106_br_ctx_bind(e->ctx, &op);

    if (rc != 0) {
        return rc;
    }
    rc = pg106_br_ctx_flush(e->ctx);
    if (rc != 0) {
        ctx_renew(e); /* drop the rejected batch */
    }
    return rc;
}

#define MAP_   PG106_UAPI_VM_BIND_OP_MAP
#define UNMAP_ PG106_UAPI_VM_BIND_OP_UNMAP
#define SPARSE_ PG106_UAPI_VM_BIND_SPARSE

static void test_va_table(void)
{
    struct env e;
    uint32_t h1 = 0, h2 = 0;
    size_t frames;
    const uint64_t A = 0x10000;

    CHECK(env_setup(&e) == 0);
    CHECK(pg106_br_bo_alloc(e.dev, 0x10000, 0x1000, PG106_BR_BO_LOCAL,
                            &h1) == 0);
    CHECK(pg106_br_bo_alloc(e.dev, 0x10000, 0x1000, PG106_BR_BO_LOCAL,
                            &h2) == 0);

    /* Partial UNMAP splits a binding; the BO stays busy until every
     * piece is gone. The old code matched UNMAPs by exact start only,
     * so the middle cut left a stale entry and bo_free -EBUSY forever. */
    CHECK(bind1(&e, mk_op(MAP_, 0x06, h1, A, 0, 0x4000)) == 0); /* kind 6 */
    CHECK(bind1(&e, mk_op(UNMAP_, 0, 0, A + 0x1000, 0, 0x1000)) == 0);
    CHECK(pg106_br_bo_free(e.dev, h1) == -EBUSY);
    CHECK(bind1(&e, mk_op(UNMAP_, 0, 0, A, 0, 0x1000)) == 0);
    CHECK(pg106_br_bo_free(e.dev, h1) == -EBUSY);
    CHECK(bind1(&e, mk_op(UNMAP_, 0, 0, A + 0x2000, 0, 0x2000)) == 0);
    CHECK(pg106_br_bo_free(e.dev, h1) == 0);
    CHECK(pg106_br_bo_alloc(e.dev, 0x10000, 0x1000, PG106_BR_BO_LOCAL,
                            &h1) == 0);

    /* One UNMAP over several bindings and holes (nvkmd va_free unmaps
     * the whole VA, nvkmd_nouveau_va.c:183-190). */
    CHECK(bind1(&e, mk_op(MAP_, 0, h1, A, 0, 0x1000)) == 0);
    CHECK(bind1(&e, mk_op(MAP_, 0, h2, A + 0x3000, 0, 0x1000)) == 0);
    CHECK(bind1(&e, mk_op(UNMAP_, 0, 0, A, 0, 0x8000)) == 0);
    CHECK(pg106_br_bo_free(e.dev, h1) == 0);
    CHECK(pg106_br_bo_free(e.dev, h2) == 0);
    /* UNMAP of a range with nothing mapped is a no-op, as on Linux. */
    CHECK(bind1(&e, mk_op(UNMAP_, 0, 0, A, 0, 0x8000)) == 0);
    CHECK(pg106_br_bo_alloc(e.dev, 0x10000, 0x1000, PG106_BR_BO_LOCAL,
                            &h1) == 0);
    CHECK(pg106_br_bo_alloc(e.dev, 0x10000, 0x1000, PG106_BR_BO_LOCAL,
                            &h2) == 0);

    /* MAP over an existing mapping replaces it (drm_gpuvm_sm_map): the
     * replaced BO is no longer bound. */
    CHECK(bind1(&e, mk_op(MAP_, 0, h1, A, 0, 0x2000)) == 0);
    CHECK(bind1(&e, mk_op(MAP_, 0, h2, A, 0, 0x2000)) == 0);
    CHECK(pg106_br_bo_free(e.dev, h1) == 0);
    CHECK(pg106_br_bo_free(e.dev, h2) == -EBUSY);
    CHECK(bind1(&e, mk_op(UNMAP_, 0, 0, A, 0, 0x2000)) == 0);

    /* Same batch: UNMAP then MAP of the same range is one valid job; the
     * old overlap check ignored the earlier UNMAP and refused it. */
    CHECK(bind1(&e, mk_op(MAP_, 0, h2, A, 0, 0x1000)) == 0);
    {
        struct pg106_uapi_vm_bind_op u = mk_op(UNMAP_, 0, 0, A, 0, 0x1000);
        struct pg106_uapi_vm_bind_op m = mk_op(MAP_, 0, h2, A, 0x1000, 0x1000);
        CHECK(pg106_br_ctx_bind(e.ctx, &u) == 0);
        CHECK(pg106_br_ctx_bind(e.ctx, &m) == 0);
        CHECK(pg106_br_ctx_flush(e.ctx) == 0);
    }
    CHECK(bind1(&e, mk_op(UNMAP_, 0, 0, A, 0, 0x1000)) == 0);
    CHECK(pg106_br_bo_free(e.dev, h2) == 0);

    /* Sparse regions: NVK's alloc_va shape is MAP|SPARSE with handle 0
     * (nvkmd_nouveau_va.c:144-151). It needs no BO. */
    CHECK(pg106_br_bo_alloc(e.dev, 0x10000, 0x1000, PG106_BR_BO_LOCAL,
                            &h1) == 0);
    CHECK(bind1(&e, mk_op(MAP_, SPARSE_, 0, 0x100000, 0, 0x10000)) == 0);
    frames = pg106_br_mock_frame_count(e.mock);
    /* MAP straddling the region edge: -ENOSPC, nothing sent. */
    CHECK(bind1(&e, mk_op(MAP_, 0, h1, 0x10F000, 0, 0x2000)) ==
          PG106_UAPI_ENOSPC);
    /* Overlapping region: -ENOSPC (pre-check against existing regions). */
    CHECK(bind1(&e, mk_op(MAP_, SPARSE_, 0, 0x108000, 0, 0x10000)) ==
          PG106_UAPI_ENOSPC);
    /* UNMAP|SPARSE must match a region exactly. */
    CHECK(bind1(&e, mk_op(UNMAP_, SPARSE_, 0, 0x100000, 0, 0x8000)) ==
          PG106_UAPI_ENOENT);
    CHECK(pg106_br_mock_frame_count(e.mock) == frames);
    /* MAP inside the region is fine; UNMAP|SPARSE releases it too. */
    CHECK(bind1(&e, mk_op(MAP_, 0x06, h1, 0x104000, 0, 0x4000)) == 0);
    CHECK(pg106_br_bo_free(e.dev, h1) == -EBUSY);
    CHECK(bind1(&e, mk_op(UNMAP_, SPARSE_, 0, 0x100000, 0, 0x10000)) == 0);
    CHECK(pg106_br_bo_free(e.dev, h1) == 0);
    /* Two overlapping regions created in one job: the second insert
     * collides with the first -> -EEXIST (region insert). */
    {
        struct pg106_uapi_vm_bind_op r1 = mk_op(MAP_, SPARSE_, 0, 0x200000, 0, 0x10000);
        struct pg106_uapi_vm_bind_op r2 = mk_op(MAP_, SPARSE_, 0, 0x208000, 0, 0x10000);
        CHECK(pg106_br_ctx_bind(e.ctx, &r1) == 0);
        CHECK(pg106_br_ctx_bind(e.ctx, &r2) == 0);
        CHECK(pg106_br_ctx_flush(e.ctx) == PG106_UAPI_EEXIST);
        CHECK(ctx_renew(&e) == 0);
    }
    /* ...and nothing of the rejected job was applied: the range is free. */
    CHECK(bind1(&e, mk_op(MAP_, SPARSE_, 0, 0x200000, 0, 0x10000)) == 0);

    /* Non-sparse MAP with handle 0 is -ENOENT, not -EINVAL. */
    CHECK(bind1(&e, mk_op(MAP_, 0, 0, 0x300000, 0, 0x1000)) ==
          PG106_UAPI_ENOENT);

    env_teardown(&e);
}

/* More bindings than one batch holds (PG106_BR_MAX_BINDS), spread over
 * several flushes: the device-wide table must grow, not overflow. */
static void test_many_bindings(void)
{
    struct env e;
    uint32_t h = 0;
    int ok = 1;
    const unsigned n = PG106_BR_MAX_BINDS + 1000;

    CHECK(env_setup(&e) == 0);
    CHECK(pg106_br_bo_alloc(e.dev, 0x1000, 0x1000, PG106_BR_BO_LOCAL,
                            &h) == 0);
    for (unsigned i = 0; i < n && ok; i++) {
        struct pg106_uapi_vm_bind_op op =
            mk_op(MAP_, 0, h, 0x10000000ull + (uint64_t)i * 0x2000, 0, 0x1000);
        ok = pg106_br_ctx_bind(e.ctx, &op) == 0;
        if (ok && (i % 1000) == 999) {
            ok = pg106_br_ctx_flush(e.ctx) == 0;
        }
    }
    CHECK(ok);
    CHECK(pg106_br_ctx_flush(e.ctx) == 0);
    CHECK(pg106_br_bo_free(e.dev, h) == -EBUSY);
    CHECK(bind1(&e, mk_op(UNMAP_, 0, 0, 0x10000000ull, 0,
                          (uint64_t)n * 0x2000)) == 0);
    CHECK(pg106_br_bo_free(e.dev, h) == 0);
    env_teardown(&e);
}

/* ------------------------------------------------------------------ */
/* 9. Resource lifetime and response checks                            */
/* ------------------------------------------------------------------ */

static void test_lifetime(void)
{
    struct env e;
    uint32_t h = 0;
    uint64_t value = 0;
    void *map = NULL;
    int ok = 1;

    CHECK(env_setup(&e) == 0);

    /* Freed BO slots are reused: far more alloc/free cycles than the
     * old fixed 1024-slot table allowed. */
    for (unsigned i = 0; i < 3000 && ok; i++) {
        ok = pg106_br_bo_alloc(e.dev, 0x1000, 0x1000, PG106_BR_BO_LOCAL,
                               &h) == 0 &&
             pg106_br_bo_free(e.dev, h) == 0;
    }
    CHECK(ok);

    /* bo_unmap tears down the CPU mapping through the transport. */
    CHECK(pg106_br_bo_alloc(e.dev, 0x1000, 0x1000,
                            PG106_BR_BO_LOCAL | PG106_BR_BO_CAN_MAP, &h) == 0);
    CHECK(pg106_br_bo_map(e.dev, h, 0, &map) == 0 && map != NULL);
    CHECK(pg106_br_bo_map(e.dev, h, 0, &map) == -EEXIST);
    CHECK(pg106_br_mock_unmap_cookie_count(e.mock) == 0);
    CHECK(pg106_br_bo_unmap(e.dev, h) == 0);
    CHECK(pg106_br_mock_unmap_cookie_count(e.mock) == 1);
    CHECK(pg106_br_bo_unmap(e.dev, h) == -EINVAL); /* not mapped */
    /* Remap works, and freeing a mapped BO unmaps it first. */
    CHECK(pg106_br_bo_map(e.dev, h, 0, &map) == 0);
    CHECK(pg106_br_bo_free(e.dev, h) == 0);
    CHECK(pg106_br_mock_unmap_cookie_count(e.mock) == 2);
    /* A mapping still live at close is released too. */
    CHECK(pg106_br_bo_alloc(e.dev, 0x1000, 0x1000,
                            PG106_BR_BO_LOCAL | PG106_BR_BO_CAN_MAP, &h) == 0);
    CHECK(pg106_br_bo_map(e.dev, h, 0, &map) == 0);

    /* A response one byte short is a protocol error, not a success
     * with a zeroed tail. */
    pg106_br_mock_short_reply(e.mock, PG106_BR_OP_GETPARAM);
    CHECK(pg106_br_getparam(e.dev, PG106_UAPI_GETPARAM_PCI_VENDOR,
                            &value) == -EPROTO);
    CHECK(pg106_br_getparam(e.dev, PG106_UAPI_GETPARAM_PCI_VENDOR,
                            &value) == 0 && value == PG106_BR_MOCK_PCI_VENDOR);
    pg106_br_mock_short_reply(e.mock, PG106_BR_OP_BO_ALLOC);
    CHECK(pg106_br_bo_alloc(e.dev, 0x1000, 0x1000, PG106_BR_BO_LOCAL,
                            &h) == -EPROTO);

    /* A rejected VM_BIND frame leaves the VA table as it was. */
    CHECK(pg106_br_bo_alloc(e.dev, 0x1000, 0x1000, PG106_BR_BO_LOCAL,
                            &h) == 0);
    {
        struct pg106_uapi_vm_bind_op op = mk_op(MAP_, 0, h, 0x10000, 0, 0x1000);
        CHECK(pg106_br_ctx_bind(e.ctx, &op) == 0);
        pg106_br_mock_fail_next(e.mock, PG106_BR_OP_VM_BIND, -ENOMEM);
        CHECK(pg106_br_ctx_flush(e.ctx) == -ENOMEM);
        CHECK(pg106_br_bo_free(e.dev, h) == 0); /* not bound */
    }

    {
        size_t before = pg106_br_mock_unmap_cookie_count(e.mock);
        pg106_br_ctx_destroy(e.ctx);
        pg106_br_dev_close(e.dev);
        CHECK(pg106_br_mock_unmap_cookie_count(e.mock) == before + 1);
        pg106_br_mock_destroy(e.mock);
    }
}

/* ------------------------------------------------------------------ */
/* ABI 0.2 ops: host import, syncobjs, private GETPARAMs               */
/* ------------------------------------------------------------------ */

static void test_abi_0_2(void)
{
    struct env e;
    static uint8_t host[3 * 4096] __attribute__((aligned(4096)));
    const struct pg106_br_mock_rec *rec;
    uint32_t h = 0, so = 0;
    uint64_t v = 0;
    size_t before;
    void *map;

    CHECK(env_setup(&e) == 0);
    CHECK(sizeof(struct pg106_br_bo_import_host) == 24);
    CHECK(sizeof(struct pg106_br_syncobj_wait) == 24);
    CHECK(PG106_BR_ABI_MINOR == 2u);
    /* Alignment is checked before anything is sent. */
    before = pg106_br_mock_frame_count(e.mock);
    CHECK(pg106_br_bo_import_host(e.dev, host + 1, 4096, 0, &h) == -EINVAL);
    CHECK(pg106_br_bo_import_host(e.dev, host, 100, 0, &h) == -EINVAL);
    CHECK(pg106_br_bo_import_host(e.dev, NULL, 4096, 0, &h) == -EINVAL);
    CHECK(pg106_br_mock_frame_count(e.mock) == before);
    CHECK(pg106_br_bo_import_host(e.dev, host, 2 * 4096, PG106_BR_BO_GART, &h) == 0);
    rec = pg106_br_mock_frame(e.mock, pg106_br_mock_frame_count(e.mock) - 1);
    CHECK(rec->op == PG106_BR_OP_BO_IMPORT_HOST);
    CHECK(rec->len == 24 + 24);
    {
        struct pg106_br_bo_import_host im;

        memcpy(&im, rec->bytes + 24, sizeof(im));
        CHECK(im.host_addr == (uint64_t)(uintptr_t)host);
        CHECK(im.size_B == 2 * 4096 && im.handle == 0);
    }
    CHECK(pg106_br_bo_map(e.dev, h, 0, &map) == -EINVAL);
    CHECK(pg106_br_bo_free(e.dev, h) == 0);

    CHECK(pg106_br_syncobj_create(e.dev, 2, &so) == -EINVAL);
    CHECK(pg106_br_syncobj_create(e.dev, PG106_BR_SYNCOBJ_CREATE_SIGNALED, &so) == 0);
    CHECK(so != 0);
    CHECK(pg106_br_syncobj_wait(e.dev, so, 0, 1000) == 0);
    rec = pg106_br_mock_frame(e.mock, pg106_br_mock_frame_count(e.mock) - 1);
    CHECK(rec->op == PG106_BR_OP_SYNCOBJ_WAIT && rec->len == 24 + 24);
    CHECK(pg106_br_syncobj_destroy(e.dev, so) == 0);
    CHECK(pg106_br_syncobj_destroy(e.dev, 0) == -EINVAL);

    CHECK(pg106_br_getparam(e.dev, PG106_UAPI_GETPARAM_VRAM_BAR_SIZE, &v) == 0);
    CHECK(v == 256ull * 1024 * 1024); /* BAR1, not VRAM */
    CHECK(pg106_br_getparam(e.dev, PG106_BR_GETPARAM_VRAM_SIZE, &v) == 0);
    CHECK(v == 6ull * 1024 * 1024 * 1024);
    CHECK(pg106_br_getparam(e.dev, PG106_BR_GETPARAM_BOOT0, &v) == 0);
    CHECK(v == 0x136000a1u);
    env_teardown(&e);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    test_layout();
    test_handshake();
    test_ordering();
    test_sync_semantics();
    test_validator_rejections();
    test_transport_failure();
    test_pinning_rule();
    test_byte_goldens();
    test_va_table();
    test_many_bindings();
    test_lifetime();
    test_abi_0_2();

    printf("%s: %d failure(s)\n", "test_bridge", failures);
    return failures != 0;
}
