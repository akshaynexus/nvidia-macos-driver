// SPDX-License-Identifier: MIT
/*
 * pascal/tests/test_uapi.c - golden vectors for the nouveau CS uapi
 * subset NVK needs (pascal/core uapi.h/uapi.c), Phase 4 gate.
 *
 * Golden values are cross-checked against:
 *   - include/uapi/drm/nouveau_drm.h (Mesa's vendored copy,
 *     third_party/mesa/include/drm-uapi/) - sizes/offsets/constants
 *   - drivers/gpu/drm/nouveau/{nouveau_exec,nouveau_uvmm,nouveau_sched}.c
 *     (Linux 7.4-rc1 d48723711bae) - the kernel-side validation rules
 *   - mesa 759935be (third_party/mesa) src/nouveau/vulkan/nvkmd/nouveau/
 *     and src/nouveau/winsys/ - what NVK actually sends
 *   - pascal/core pushbuf API - the pushbuffer words inside the EXEC
 *     vector are built with the existing pg106_pb_* encoders, showing
 *     the bridge->core layering: a bridge packs NVK pushes into a
 *     pushbuffer via the core, then hands the uapi request to the core
 *     validator.
 *
 * This is a simulation/host-side test only; it touches no hardware.
 * Build/run via the pascal Makefile.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "pascal/pushbuf.h"
#include "pascal/uapi.h"

static int failures;

#define CHECK(cond) do { \
    if (cond) { printf("PASS %s\n", #cond); } \
    else { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

/* The reference VM for the sequence below, shaped like NVK's
 * (winsys nouveau_device.c:508-515 reserves the low region for
 * the kernel and gives UMD everything above it): */
#define KERN_MANAGED_ADDR  0x40000000ull  /* 1 GiB */
#define KERN_MANAGED_SIZE  0x40000000ull  /* 1 GiB */
#define UMD_VA_BASE        0x100000000ull /* 4 GiB: first UMD region */

static const struct pg106_uapi_vm_init kVM = {
    KERN_MANAGED_ADDR, KERN_MANAGED_SIZE,
};

int main(void)
{
    /* ---- 1. Wire-layout golden vectors (nouveau_drm.h) -------------
     * The bridge will marshal these structs verbatim between NVK and
     * the kext, so sizes and key offsets must match the Linux uapi
     * exactly. (Sizes are also pinned by _Static_assert in uapi.h.) */
    {
        struct pg106_uapi_sync s = { PG106_UAPI_SYNC_TIMELINE_SYNCOBJ, 7, 42 };
        struct pg106_uapi_vm_bind_op op = {
            PG106_UAPI_VM_BIND_OP_MAP, 0, 0x11, 0, 0x1000, 0x2000, 0x3000,
        };
        CHECK(sizeof(struct pg106_uapi_sync) == 16);
        CHECK(sizeof(struct pg106_uapi_vm_bind_op) == 40);
        CHECK(sizeof(struct pg106_uapi_vm_bind) == 40);
        CHECK(sizeof(struct pg106_uapi_exec) == 40);
        CHECK(offsetof(struct pg106_uapi_sync, handle) == 4);
        CHECK(offsetof(struct pg106_uapi_sync, timeline_value) == 8);
        CHECK(offsetof(struct pg106_uapi_vm_bind_op, handle) == 8);
        CHECK(offsetof(struct pg106_uapi_vm_bind_op, addr) == 16);
        CHECK(offsetof(struct pg106_uapi_vm_bind, op_ptr) == 32);
        CHECK(offsetof(struct pg106_uapi_exec, push_ptr) == 32);
        CHECK(s.timeline_value == 42 && op.handle == 0x11);
        /* Constant golden values, nouveau_drm.h:237-239, 300-311,
         * 357, 399. */
        CHECK(PG106_UAPI_SYNC_SYNCOBJ == 0x0);
        CHECK(PG106_UAPI_SYNC_TIMELINE_SYNCOBJ == 0x1);
        CHECK(PG106_UAPI_SYNC_TYPE_MASK == 0xf);
        CHECK(PG106_UAPI_VM_BIND_OP_MAP == 0x0);
        CHECK(PG106_UAPI_VM_BIND_OP_UNMAP == 0x1);
        CHECK(PG106_UAPI_VM_BIND_SPARSE == (1 << 8));
        CHECK(PG106_UAPI_VM_BIND_RUN_ASYNC == 0x1);
        CHECK(PG106_UAPI_EXEC_PUSH_NO_PREFETCH == 0x1);
        CHECK(PG106_UAPI_DMA_PUSH_MAX_LENGTH == 0x7fffff);
    }

    /* ---- 2. EXEC push limit (nouveau_exec.h:51-58) ------------------
     * half the GPFIFO ring minus one slot for the HW fence. */
    CHECK(pg106_uapi_exec_push_max(0) == 0);
    CHECK(pg106_uapi_exec_push_max(1) == 0);
    CHECK(pg106_uapi_exec_push_max(2) == 0);
    CHECK(pg106_uapi_exec_push_max(8) == 3);
    CHECK(pg106_uapi_exec_push_max(940) == 469);

    /* ---- 3. VM_INIT (nouveau_uvmm.c:1912-1918) ---------------------- */
    CHECK(pg106_uapi_vm_init_validate(&kVM) == 0);
    {
        struct pg106_uapi_vm_init bad_overflow = { UINT64_MAX, 1 };
        struct pg106_uapi_vm_init bad_toobig = { 0, PG106_UAPI_VA_SPACE_END + 4096 };
        CHECK(pg106_uapi_vm_init_validate(&bad_overflow) == PG106_UAPI_EINVAL);
        CHECK(pg106_uapi_vm_init_validate(&bad_toobig) == PG106_UAPI_EINVAL);
    }

    /* ---- 4. VM_BIND accept: the NVK-shaped sequence ------------------
     * NVK binds each BO into its VA space with MAP ops and releases
     * with UNMAP, then kicks work with EXEC (mesa-nvkmd_nouveau_va.c,
     * _ctx.c). This is the golden accept vector: MAP 64 KiB at the
     * first UMD VA, then UNMAP it, run asynchronously with one
     * timeline wait and one plain syncobj signal. */
    {
        struct pg106_uapi_vm_bind_op ops[2] = {
            { PG106_UAPI_VM_BIND_OP_MAP, 0, 0x11, 0,
              UMD_VA_BASE, 0, 0x10000 },
            { PG106_UAPI_VM_BIND_OP_UNMAP, 0, 0, 0,
              UMD_VA_BASE, 0, 0x10000 },
        };
        struct pg106_uapi_sync waits[1] = {
            { PG106_UAPI_SYNC_TIMELINE_SYNCOBJ, 0x21, 5 },
        };
        struct pg106_uapi_sync sigs[1] = {
            { PG106_UAPI_SYNC_SYNCOBJ, 0x22, 0 },
        };
        struct pg106_uapi_vm_bind req = {
            .op_count = 2,
            .flags = PG106_UAPI_VM_BIND_RUN_ASYNC,
            .wait_count = 1,
            .sig_count = 1,
        };
        CHECK(pg106_uapi_vm_bind_validate(&kVM, &req, ops, waits, sigs) == 0);

        /* MAP against its 64 KiB BO: accept. */
        CHECK(pg106_uapi_vm_bind_op_check_map(&ops[0], 0x10000) == 0);
        /* Sync mode is legal too when there are no syncs at all. */
        {
            struct pg106_uapi_vm_bind sync_req = { 2, 0, 0, 0, 0, 0, 0 };
            CHECK(pg106_uapi_vm_bind_validate(&kVM, &sync_req, ops,
                                              NULL, NULL) == 0);
        }
        /* ...but syncs without RUN_ASYNC are -EINVAL
         * (nouveau_sched.c nouveau_job_init). */
        {
            struct pg106_uapi_vm_bind bad = req;
            bad.flags = 0;
            CHECK(pg106_uapi_vm_bind_validate(&kVM, &bad, ops,
                                              waits, sigs) ==
                  PG106_UAPI_EINVAL);
        }
        /* Request flag bits other than RUN_ASYNC are not read by the
         * kernel (nouveau_uvmm.c:1708): accepted. */
        {
            struct pg106_uapi_vm_bind other = req;
            other.flags = PG106_UAPI_VM_BIND_RUN_ASYNC | 0x2;
            CHECK(pg106_uapi_vm_bind_validate(&kVM, &other, ops,
                                              waits, sigs) == 0);
        }
        /* Unknown sync type: a wait is -EOPNOTSUPP (sync_find_fence,
         * nouveau_sched.c:147-149), a signal is -EINVAL
         * (nouveau_job_fence_attach_prepare, :215-219). */
        {
            struct pg106_uapi_sync bad = { 2, 0x21, 0 }; /* type 2 */
            CHECK(pg106_uapi_vm_bind_validate(&kVM, &req, ops,
                                              &bad, sigs) ==
                  PG106_UAPI_EOPNOTSUPP);
            CHECK(pg106_uapi_vm_bind_validate(&kVM, &req, ops,
                                              waits, &bad) ==
                  PG106_UAPI_EINVAL);
        }
    }

    /* ---- 5. VM_BIND rejects, one per kernel rule --------------------- */
    {
        struct pg106_uapi_vm_bind req = {
            .op_count = 1, .flags = PG106_UAPI_VM_BIND_RUN_ASYNC,
        };
        struct pg106_uapi_vm_bind_op good = {
            PG106_UAPI_VM_BIND_OP_MAP, 0, 0x11, 0,
            UMD_VA_BASE, 0, 0x10000,
        };
        CHECK(pg106_uapi_vm_bind_validate(&kVM, &req, &good, NULL, NULL) == 0);

        {
            struct pg106_uapi_vm_bind_op bad = good;
            bad.addr = UMD_VA_BASE + 0x1001; /* not page-aligned */
            CHECK(pg106_uapi_vm_bind_validate(&kVM, &req, &bad,
                                              NULL, NULL) == PG106_UAPI_EINVAL);
        }
        {
            struct pg106_uapi_vm_bind_op bad = good;
            bad.range = 0x10001; /* not page-aligned */
            CHECK(pg106_uapi_vm_bind_validate(&kVM, &req, &bad,
                                              NULL, NULL) == PG106_UAPI_EINVAL);
        }
        {
            struct pg106_uapi_vm_bind_op bad = good;
            bad.range = 0; /* empty op */
            CHECK(pg106_uapi_vm_bind_validate(&kVM, &req, &bad,
                                              NULL, NULL) == PG106_UAPI_EINVAL);
        }
        {
            struct pg106_uapi_vm_bind_op bad = good;
            bad.bo_offset = 0x1001; /* unaligned (MAP) */
            CHECK(pg106_uapi_vm_bind_validate(&kVM, &req, &bad,
                                              NULL, NULL) == PG106_UAPI_EINVAL);
        }
        {
            /* Not a documented op. The kernel would alias 2/3 to its
             * internal OP_MAP_SPARSE/OP_UNMAP_SPARSE (uapi.h,
             * bridge-only); >= 4 it rejects too. */
            struct pg106_uapi_vm_bind_op bad = good;
            bad.op = 2;
            CHECK(pg106_uapi_vm_bind_validate(&kVM, &req, &bad,
                                              NULL, NULL) == PG106_UAPI_EINVAL);
            bad.op = 4;
            CHECK(pg106_uapi_vm_bind_validate(&kVM, &req, &bad,
                                              NULL, NULL) == PG106_UAPI_EINVAL);
        }
        {
            /* pad is never read by the kernel. */
            struct pg106_uapi_vm_bind_op odd = good;
            odd.pad = 0xdeadbeef;
            CHECK(pg106_uapi_vm_bind_validate(&kVM, &req, &odd,
                                              NULL, NULL) == 0);
        }
        {
            /* Non-sparse MAP without a BO: the GEM lookup of handle 0
             * fails -> -ENOENT (nouveau_uvmm.c:1278-1283). */
            struct pg106_uapi_vm_bind_op bad = good;
            bad.handle = 0;
            CHECK(pg106_uapi_vm_bind_validate(&kVM, &req, &bad,
                                              NULL, NULL) == PG106_UAPI_ENOENT);
        }
        {
            struct pg106_uapi_vm_bind_op bad = good;
            bad.addr = KERN_MANAGED_ADDR; /* inside the kernel region */
            CHECK(pg106_uapi_vm_bind_validate(&kVM, &req, &bad,
                                              NULL, NULL) == PG106_UAPI_EINVAL);
        }
        {
            struct pg106_uapi_vm_bind_op bad = good;
            bad.addr = KERN_MANAGED_ADDR + KERN_MANAGED_SIZE - 0x1000;
            bad.range = 0x2000; /* straddles the kernel region end */
            CHECK(pg106_uapi_vm_bind_validate(&kVM, &req, &bad,
                                              NULL, NULL) == PG106_UAPI_EINVAL);
        }
        {
            struct pg106_uapi_vm_bind_op bad = good;
            bad.addr = PG106_UAPI_VA_SPACE_END - 0x1000;
            bad.range = 0x2000; /* past the VA space end (1<<47) */
            CHECK(pg106_uapi_vm_bind_validate(&kVM, &req, &bad,
                                              NULL, NULL) == PG106_UAPI_EINVAL);
        }
        /* NVK's real op shapes (mesa 759935be). */
        {
            /* alloc_va with NVKMD_VA_SPARSE: MAP|SPARSE, handle 0,
             * bo_offset 0 (nvkmd_nouveau_va.c:144-151) -> kernel
             * OP_MAP_SPARSE, which never looks a BO up. */
            struct pg106_uapi_vm_bind_op sparse = {
                PG106_UAPI_VM_BIND_OP_MAP, PG106_UAPI_VM_BIND_SPARSE, 0, 0,
                UMD_VA_BASE, 0, 0x100000,
            };
            CHECK(pg106_uapi_vm_bind_validate(&kVM, &req, &sparse,
                                              NULL, NULL) == 0);
            CHECK(pg106_uapi_vm_bind_op_is_bo_map(&sparse) == 0);
            /* handle/bo_offset of a sparse MAP are ignored. */
            sparse.handle = 0x77;
            sparse.bo_offset = 0x123;
            CHECK(pg106_uapi_vm_bind_validate(&kVM, &req, &sparse,
                                              NULL, NULL) == 0);
            /* va_free's sparse release: UNMAP|SPARSE
             * (nvkmd_nouveau_va.c:192-200). */
            sparse.op = PG106_UAPI_VM_BIND_OP_UNMAP;
            CHECK(pg106_uapi_vm_bind_validate(&kVM, &req, &sparse,
                                              NULL, NULL) == 0);
        }
        {
            /* bind_mem / bind ctx put the PTE kind in flags[7:0]
             * (nvkmd_nouveau_va.c:229, nvkmd_nouveau_ctx.c:395); the
             * kernel reads flags & 0xff (nouveau_uvmm.c:1399). */
            struct pg106_uapi_vm_bind_op kind = good;
            kind.flags = 0xfe;
            CHECK(pg106_uapi_vm_bind_validate(&kVM, &req, &kind,
                                              NULL, NULL) == 0);
            CHECK(pg106_uapi_vm_bind_op_is_bo_map(&kind) == 1);
            CHECK(pg106_uapi_vm_bind_op_check_map(&kind, 0x10000) == 0);
            /* ...and on the UNMAP side of the bind ctx too. */
            kind.op = PG106_UAPI_VM_BIND_OP_UNMAP;
            kind.handle = 0;
            CHECK(pg106_uapi_vm_bind_validate(&kVM, &req, &kind,
                                              NULL, NULL) == 0);
            /* Bits above SPARSE are ignored by the kernel. */
            kind = good;
            kind.flags = 0x80000000u | 0x06;
            CHECK(pg106_uapi_vm_bind_validate(&kVM, &req, &kind,
                                              NULL, NULL) == 0);
        }
        {
            /* An unaligned range is still checked for sparse ops
             * (validate_range runs for every op). */
            struct pg106_uapi_vm_bind_op bad = {
                PG106_UAPI_VM_BIND_OP_MAP, PG106_UAPI_VM_BIND_SPARSE, 0, 0,
                UMD_VA_BASE, 0, 0x1800,
            };
            CHECK(pg106_uapi_vm_bind_validate(&kVM, &req, &bad,
                                              NULL, NULL) == PG106_UAPI_EINVAL);
            /* check_map is only for non-sparse MAPs. */
            bad.range = 0x1000;
            CHECK(pg106_uapi_vm_bind_op_check_map(&bad, 0x10000) ==
                  PG106_UAPI_EINVAL);
        }
        /* BO cross-check (bind_validate_op, nouveau_uvmm.c:1070-1079). */
        {
            struct pg106_uapi_vm_bind_op off_end = good;
            off_end.bo_offset = 0x10000; /* offset == bo size */
            CHECK(pg106_uapi_vm_bind_op_check_map(&off_end, 0x10000) ==
                  PG106_UAPI_EINVAL);
            CHECK(pg106_uapi_vm_bind_op_check_map(&good, 0x8000) ==
                  PG106_UAPI_EINVAL); /* range 0x10000 > 0x8000 - 0 */
            CHECK(pg106_uapi_vm_bind_op_check_map(&good, 0x20000) == 0);
        }
        /* Pointer/count agreement. */
        CHECK(pg106_uapi_vm_bind_validate(&kVM, &req, NULL, NULL, NULL) ==
              PG106_UAPI_EINVAL);
        /* No VM_INIT known -> any op is invalid. */
        CHECK(pg106_uapi_vm_bind_validate(NULL, &req, &good, NULL, NULL) ==
              PG106_UAPI_EINVAL);
    }

    /* ---- 6. EXEC accept: pushbuffer built with the core encoders -----
     * This is the layering vector: the bridge packs NVK's push words
     * with the existing pg106_pb_* API (same encoders the kext will
     * run), then hands the uapi request to the validator. The kernel
     * never parses push contents - it only checks the per-push
     * address/length limits (nouveau_exec.c:211-219) - so a 5-word
     * SET_OBJECT / SEND_PCAS_A / IMMD / END_PB_SEGMENT stream is a
     * valid, realistic GP106 compute submission body.
     */
    {
        uint32_t words[16];
        pg106_pb pb;
        struct pg106_uapi_exec_push pushes[1];
        struct pg106_uapi_sync waits[1] = {
            { PG106_UAPI_SYNC_TIMELINE_SYNCOBJ, 0x21, 6 },
        };
        struct pg106_uapi_sync sigs[1] = {
            { PG106_UAPI_SYNC_SYNCOBJ, 0x22, 0 },
        };
        struct pg106_uapi_exec req = {
            .channel = 0,             /* first channel created */
            .push_count = 1,
            .wait_count = 1,
            .sig_count = 1,
        };

        CHECK(pg106_pb_init(&pb, words, 16) == PG106_PB_OK);
        /* Subchannel 1 = compute: NVK's fixed assignment
         * (SUBC_NV90C0 1, mesa src/nouveau/headers/nv_push.h:81). */
        CHECK(pg106_pb_set_object(&pb, 1, PG106_CLASS_PASCAL_COMPUTE_B)
              == PG106_PB_OK);
        CHECK(pg106_pb_noninc(&pb, 1, PG106_NVC1C0_SEND_PCAS_A, NULL, 0)
              == PG106_PB_OK);
        CHECK(pg106_pb_immd(&pb, 1, PG106_NVC1C0_SEND_PCAS_B, 0x1234)
              == PG106_PB_OK);
        CHECK(pg106_pb_endseg(&pb) == PG106_PB_OK);
        CHECK(pb.len == 5);
        /* Golden header words (clc06f.h layout, subchannel 1). */
        CHECK(words[0] == 0x20012000u); /* INC, count 1, subch 1, mthd 0 */
        CHECK(words[0] == ((1u << 29) | (1u << 16) | (1u << 13) | 0u));
        CHECK(words[1] == PG106_CLASS_PASCAL_COMPUTE_B);
        CHECK(words[2] == 0x600020ADu); /* NONINC, count 0, subch 1 */
        CHECK(words[2] == ((3u << 29) | (0u << 16) | (1u << 13) |
                           (0x02B4u >> 2)));

        pushes[0] = (struct pg106_uapi_exec_push){
            .va = UMD_VA_BASE + 0x200000, /* where the bridge mapped it */
            .va_len = (uint32_t)pb.len * 4,
            .flags = 0,
        };
        CHECK(pushes[0].va_len == 20);
        /* ib_max 8 -> max 3 pushes (section 2). */
        CHECK(pg106_uapi_exec_validate(8, &req, pushes, waits, sigs) == 0);

        /* Sync-only flush: NVK signals a syncobj with an empty exec
         * (nvkmd_nouveau_ctx.c:242-292, ioctl at :275). */
        {
            struct pg106_uapi_exec empty = {
                .channel = 0, .push_count = 0,
                .wait_count = 0, .sig_count = 1,
            };
            CHECK(pg106_uapi_exec_validate(8, &empty, NULL, NULL, sigs)
                  == 0);
        }
    }

    /* ---- 7. EXEC rejects, one per kernel rule ------------------------ */
    {
        struct pg106_uapi_exec_push good = {
            UMD_VA_BASE + 0x200000, 20, 0,
        };
        struct pg106_uapi_exec req = {
            .channel = 0, .push_count = 1,
        };
        CHECK(pg106_uapi_exec_validate(8, &req, &good, NULL, NULL) == 0);

        {
            struct pg106_uapi_exec_push bad = good;
            bad.va_len = 0x800000; /* > NV50_DMA_PUSH_MAX_LENGTH */
            CHECK(pg106_uapi_exec_validate(8, &req, &bad, NULL, NULL) ==
                  PG106_UAPI_EINVAL);
        }
        {
            struct pg106_uapi_exec_push bad = good;
            bad.va_len = 21; /* not 4-aligned */
            CHECK(pg106_uapi_exec_validate(8, &req, &bad, NULL, NULL) ==
                  PG106_UAPI_EINVAL);
        }
        {
            struct pg106_uapi_exec_push bad = good;
            bad.va = UMD_VA_BASE + 0x200002; /* not 4-aligned */
            CHECK(pg106_uapi_exec_validate(8, &req, &bad, NULL, NULL) ==
                  PG106_UAPI_EINVAL);
        }
        {
            struct pg106_uapi_exec_push bad = good;
            bad.va_len = 0; /* empty push */
            CHECK(pg106_uapi_exec_validate(8, &req, &bad, NULL, NULL) ==
                  PG106_UAPI_EINVAL);
        }
        {
            /* Only NO_PREFETCH is read; other bits are ignored. */
            struct pg106_uapi_exec_push odd = good;
            odd.flags = 0x2;
            CHECK(pg106_uapi_exec_validate(8, &req, &odd, NULL, NULL) == 0);
        }
        {
            struct pg106_uapi_exec bad = req;
            struct pg106_uapi_exec_push four[4] = { good, good, good, good };
            bad.push_count = 4; /* > exec_push_max(8) == 3 */
            CHECK(pg106_uapi_exec_validate(8, &bad, four, NULL, NULL) ==
                  PG106_UAPI_EINVAL);
            /* ...but legal on a bigger ring: */
            CHECK(pg106_uapi_exec_validate(16, &bad, four, NULL, NULL) ==
                  0);
        }
        {
            struct pg106_uapi_sync bad_wait = { 2, 0x21, 0 }; /* type 2 */
            struct pg106_uapi_exec with_wait = req;
            with_wait.wait_count = 1;
            CHECK(pg106_uapi_exec_validate(8, &with_wait, &good, &bad_wait,
                                           NULL) == PG106_UAPI_EOPNOTSUPP);
            /* Same type as a signal: -EINVAL. */
            struct pg106_uapi_exec with_sig = req;
            with_sig.sig_count = 1;
            CHECK(pg106_uapi_exec_validate(8, &with_sig, &good, NULL,
                                           &bad_wait) == PG106_UAPI_EINVAL);
        }
        CHECK(pg106_uapi_exec_validate(8, &req, NULL, NULL, NULL) ==
              PG106_UAPI_EINVAL);
    }

    printf("%s: %d failure(s)\n", "test_uapi", failures);
    return failures != 0;
}
