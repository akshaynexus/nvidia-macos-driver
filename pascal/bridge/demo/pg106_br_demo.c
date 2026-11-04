// SPDX-License-Identifier: MIT
/*
 * pg106_br_demo.c - a tiny NVK-shaped sequence over the mock
 * transport, exercising the whole pg106_br_ surface in order:
 *
 *   dev open (handshake) -> VM_INIT -> GETPARAM -> BO alloc + map
 *   -> VA alloc -> pack pushbuffer words with the pascal/core
 *   encoders -> VM_BIND MAP (RUN_ASYNC + syncs) -> EXEC (wait/signal)
 *   -> flush -> sync.
 *
 * Mirrors what nvkmd_nouveau does on a queue tick:
 * alloc_mem -> alloc_va -> va_bind_mem -> ctx_exec + signal
 * (mesa src/nouveau/vulkan/nvkmd/nouveau/).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pg106_br_bridge.h"
#include "pg106_br_mock.h"
#include "pascal/pushbuf.h"

#define BO_SIZE   0x10000ull /* 64 KiB */
#define VA_SIZE   0x10000ull

int main(void)
{
    struct pg106_br_mock *mock;
    struct pg106_br_transport tr;
    struct pg106_br_dev *dev = NULL;
    struct pg106_br_ctx *ctx = NULL;
    struct pg106_uapi_vm_bind_op op;
    struct pg106_uapi_exec_push push;
    struct pg106_uapi_sync wait, sig;
    uint32_t handle = 0;
    uint64_t va = 0, value = 0;
    uint32_t words[8];
    pg106_pb pb;
    void *map = NULL;
    int rc;

    mock = pg106_br_mock_create();
    if (mock == NULL) {
        return 1;
    }
    pg106_br_mock_transport(mock, &tr);

    /* 1. Open: OP_HANDSHAKE round trip, drm floor gate. */
    rc = pg106_br_dev_open(&tr, &dev);
    printf("dev_open          -> %d (ib_max via GETPARAM next)\n", rc);
    if (rc != 0) {
        return 1;
    }

    /* 2. VM_INIT: reserve the kernel-managed region, 1 GiB @ 1 GiB
     * (same shape as the winsys, nouveau_device.c:510-515). */
    rc = pg106_br_vm_init(dev, 0x40000000ull, 0x40000000ull);
    printf("vm_init           -> %d\n", rc);
    if (rc != 0) {
        return 1;
    }

    rc = pg106_br_getparam(dev, PG106_UAPI_GETPARAM_EXEC_PUSH_MAX, &value);
    printf("GETPARAM EXEC_PUSH_MAX -> %d (value %llu)\n", rc,
           (unsigned long long)value);
    if (rc != 0 || value != pg106_uapi_exec_push_max(PG106_BR_MOCK_IB_MAX)) {
        return 1;
    }

    /* 3. BO alloc + CPU map (mock backs it with host memory; the kext
     * path is IOConnectMapMemory64, NOUVEAU-UAPI.md section 4.1). */
    rc = pg106_br_bo_alloc(dev, BO_SIZE, 0x1000,
                           PG106_BR_BO_LOCAL | PG106_BR_BO_CAN_MAP, &handle);
    printf("bo_alloc          -> %d (handle %u)\n", rc, handle);
    if (rc != 0) {
        return 1;
    }
    rc = pg106_br_bo_map(dev, handle, 0, &map);
    printf("bo_map            -> %d (cpu %p)\n", rc, map);
    if (rc != 0 || map == NULL) {
        return 1;
    }

    /* 4. VA alloc and pack a 5-word push into the mapped BO with the
     * core encoders - the same words as the pascal/tests/test_uapi.c
     * layering vector: SET_OBJECT PASCAL_COMPUTE_B on subchannel 4,
     * SEND_PCAS_A non-incremental, IMMD, END_PB_SEGMENT. */
    rc = pg106_br_va_alloc(dev, VA_SIZE, 0x1000, &va);
    printf("va_alloc          -> %d (va 0x%llx)\n", rc,
           (unsigned long long)va);
    if (rc != 0) {
        return 1;
    }

    pg106_pb_init(&pb, words, 8);
    if (pg106_pb_set_object(&pb, 4, PG106_CLASS_PASCAL_COMPUTE_B) != PG106_PB_OK ||
        pg106_pb_noninc(&pb, 4, PG106_NVC1C0_SEND_PCAS_A, NULL, 0) != PG106_PB_OK ||
        pg106_pb_immd(&pb, 4, PG106_NVC1C0_SEND_PCAS_B, 0x0000000Fu) != PG106_PB_OK ||
        pg106_pb_endseg(&pb) != PG106_PB_OK) {
        printf("pushbuffer pack failed (error %d)\n", pb.error);
        return 1;
    }
    memcpy(map, words, pb.len * sizeof(uint32_t));
    printf("push words        -> %zu packed into BO\n", pb.len);

    /* 5. Queue the MAP that exposes the BO at va, with one timeline
     * wait and one syncobj signal (RUN_ASYNC set by the bridge). */
    if (pg106_br_ctx_create(dev, 0, &ctx) != 0) {
        return 1;
    }
    memset(&op, 0, sizeof(op));
    op.op = PG106_UAPI_VM_BIND_OP_MAP;
    op.handle = handle;
    op.addr = va;
    op.range = 0x1000;
    rc = pg106_br_ctx_bind(ctx, &op);
    printf("ctx bind MAP      -> %d\n", rc);
    memset(&wait, 0, sizeof(wait));
    wait.flags = PG106_UAPI_SYNC_TIMELINE_SYNCOBJ;
    wait.handle = 0x11;
    wait.timeline_value = 7;
    rc = pg106_br_ctx_bind_wait(ctx, &wait);
    printf("ctx bind wait     -> %d\n", rc);
    memset(&sig, 0, sizeof(sig));
    sig.handle = 0x22;
    rc = pg106_br_ctx_bind_signal(ctx, &sig);
    printf("ctx bind signal   -> %d\n", rc);

    /* 6. Queue the EXEC push at the bound va + the exec-side wait and
     * signal, then flush: one OP_VM_BIND frame, then one OP_EXEC
     * frame (waits before pushes, signals after). */
    memset(&push, 0, sizeof(push));
    push.va = va;
    push.va_len = (uint32_t)(pb.len * sizeof(uint32_t));
    rc = pg106_br_ctx_exec(ctx, &push);
    printf("ctx exec push     -> %d (va 0x%llx, %u B)\n", rc,
           (unsigned long long)push.va, push.va_len);
    rc = pg106_br_ctx_exec_wait(ctx, &wait);
    printf("ctx exec wait     -> %d\n", rc);
    rc = pg106_br_ctx_exec_signal(ctx, &sig);
    printf("ctx exec signal   -> %d\n", rc);
    rc = pg106_br_ctx_sync(ctx);
    printf("ctx sync (flush)  -> %d\n", rc);
    if (rc != 0) {
        return 1;
    }

    /* 7. Show the wire trace. */
    printf("\nwire trace (%zu frames):\n", pg106_br_mock_frame_count(mock));
    for (size_t i = 0; i < pg106_br_mock_frame_count(mock); i++) {
        const struct pg106_br_mock_rec *r = pg106_br_mock_frame(mock, i);
        printf("  [%zu] op=%u seq=%u len=%zu status=%d\n",
               i, r->op, r->seq, r->len, r->status);
    }

    pg106_br_ctx_destroy(ctx);
    pg106_br_dev_close(dev);
    pg106_br_mock_destroy(mock);

    printf("pg106_br_demo: OK\n");
    return 0;
}
