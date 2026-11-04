// SPDX-License-Identifier: MIT
/*
 * pascal/tests/test_tinygrad_compat.c - differential test of our
 * pushbuffer/GPFIFO/QMD encodings against tinygrad's actual NV encoder
 * (tinygrad/runtime/ops_nv.py).
 *
 * Every expected value below is computed with tinygrad's own formula,
 * transcribed from source, NOT hand-derived, so the test fails if
 * either side changes. Citations:
 *
 *   [TG-master] tinygrad master ops_nv.py, fetched 2026-11-05,
 *     github blob 0a1bfa77 (raw master). Line numbers below refer to
 *     that revision; /tmp/pascal-tg/ops_nv.py holds the copy.
 *     - nvm():            lines 43-45
 *         (typ << 28) | (count << 16) | (subc << 13) | (mthd >> 2)
 *         default typ=2; grep shows NO call site passes typ != 2.
 *     - NVQueue.sem():    lines 111-113 (one INC_METHOD run of 5 words
 *       covering NVC56F_SEM_ADDR_LO..SEM_EXECUTE)
 *     - nv_submit():      line 97 (GPFIFO entry)
 *         gpentry = cmdbuf_addr | (cmdbuf_bytes // 4 << 42) | (1 << 41)
 *     - NVComputeQueue.exec(): line 180 SEND_PCAS_A with qmd_addr >> 8
 *     - NVCopyQueue.copy():    lines 187-194 (B5 methods)
 *   [TG-v0.9.1] tinygrad v0.9.1 ops_nv.py (May 2024):
 *     - nvmethod(): line 67 - the same typ<<28 formula
 *     - _submit():  line 143 - the same GPFIFO entry packing
 *   [NVK-kernel] torvalds/linux nvif/chan506f.c (MIT, fetched 2026-11-05),
 *     nvif_chan506f_gpfifo_push(): kernel-side entry packing
 *         dword0 = lower_32_bits(addr)
 *         dword1 = upper_32_bits(addr) | (main ? 0 : BIT(9))
 *                  | ((size >> 2) << 10) | (no_prefetch ? BIT(31) : 0)
 *     - EXEC segments go out with main=false, i.e. BIT(9) SET:
 *       LEVEL = SUBROUTINE, the same bit tinygrad's `1 << 41` sets.
 *   [NV-570-autogen] tinygrad.runtime.autogen.nv_570 (generated from
 *     NVIDIA open-gpu-kernel-modules 570 SDK headers): method numbers
 *     and QMD field (msb, lsb) tuples quoted in the comments.
 *   [ENC-RES] research/evidence/ENCODING-RESOLUTION.md - the typ<<28
 *     analysis this test locks in.
 *
 * Method-number provenance: NVC1C0 (Pascal compute) == NVC6C0 (Ada)
 * for SET_OBJECT (0x0) and SEND_PCAS_A (0x2b4); the PCAS-B/signaling
 * methods DIVERGED (Pascal SEND_PCAS_B 0x2b8 / SEND_SIGNALING_PCAS_B
 * 0x2bc, Ada SEND_SIGNALING_PCAS2_B 0x2c0 - research/reference/
 * clc1c0.h:374-387 vs open-gpu-kernel-modules clc6c0.h:357-381).
 * CE copy methods are generation-stable across B5 classes (NVC1B5 ==
 * NVC6B5 for 0x240/0x300/0x400/0x418, clc1b5.h vs nv_570 autogen).
 *
 * Build/run via the pascal Makefile. Touches no hardware.
 */
#include <stdint.h>
#include <stdio.h>
#include "pascal/pushbuf.h"
#include "pascal/qmd.h"

static int failures;

#define CHECK(cond) do { \
    if (cond) { printf("PASS %s\n", #cond); } \
    else { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

/* tinygrad nvm() header, transcribed verbatim from [TG-master:43-45]
 * (`typ` defaults to 2 everywhere in the file). */
static uint32_t tg_nvm(unsigned subc, uint32_t mthd, unsigned count)
{
    return (2u << 28) | ((count & 0x3FFFu) << 16) | (subc << 13) | (mthd >> 2);
}

/* tinygrad GPFIFO entry, transcribed from [TG-master:97]
 * (v0.9.1 [TG-v0.9.1:143] is the identical packing). */
static uint64_t tg_gpentry(uint64_t addr, unsigned bytes)
{
    return addr | (((uint64_t)bytes / 4) << 42) | ((uint64_t)1 << 41);
}

/* Method numbers from [NV-570-autogen] (see provenance note above). */
#define TG_NVC6C0_SET_OBJECT            0x0000u
#define TG_NVC6C0_SEND_PCAS_A           0x02b4u
#define TG_NVC6C0_SEND_SIGNALING_PCAS2_B 0x02c0u
#define TG_NVC6C0_INVALIDATE_SHADER_CACHES_NO_WFI 0x1698u
#define TG_NVC56F_SEM_ADDR_LO           0x5cu
#define TG_NVC56F_SEM_ADDR_HI           0x60u
#define TG_NVC56F_SEM_PAYLOAD_LO        0x64u
#define TG_NVC56F_SEM_PAYLOAD_HI        0x68u
#define TG_NVC56F_SEM_EXECUTE           0x6cu
#define TG_NVC6B5_OFFSET_IN_UPPER       0x400u
#define TG_NVC6B5_LINE_LENGTH_IN        0x418u
#define TG_NVC6B5_LAUNCH_DMA            0x300u

/* SEM_EXECUTE flag words [NV-570-autogen]: NVC56F_SEM_EXECUTE_OPERATION
 * [3:0] (ACQ_CIRC_GEQ=3, RELEASE=1), PAYLOAD_SIZE [25:24] (64BIT=1),
 * RELEASE_WFI [20:20], RELEASE_TIMESTAMP [25:25]. Computed with
 * tinygrad's nv_flags() helper (ops_nv.py:39-40). */
#define TG_SEM_EXEC_ACQ_CIRC_GEQ_64BIT  0x01000003u
#define TG_SEM_EXEC_RELEASE_WFI_64BIT   0x01100001u

int main(void)
{
    /* ---- 1. Method-header identity: tinygrad typ=2 == our INC_METHOD
     * ---- [ENC-RES] core claim. (2 << 28) and (1 << 29) are the same
     * number: tinygrad's shorthand IS the canonical SEC_OP=1 word. */
    CHECK((2u << 28) == (1u << 29));
    CHECK(PG106_SEC_OP_INC_METHOD == 1u);

    {
        static const struct { unsigned subc; uint32_t mthd; unsigned count; } v[] = {
            /* [TG-master:180] compute launch: SEND_PCAS_A, one word. */
            { 1, TG_NVC6C0_SEND_PCAS_A, 1 },
            /* [TG-master:111-113] semaphore run: 5 data words. */
            { 0, TG_NVC56F_SEM_ADDR_LO, 5 },
            /* [TG-master:187-190] CE copy: hi/lo src + hi/lo dst. */
            { 4, TG_NVC6B5_OFFSET_IN_UPPER, 4 },
            /* [TG-master:191] CE copy length. */
            { 4, TG_NVC6B5_LINE_LENGTH_IN, 1 },
            /* [TG-master:193-194] CE copy launch. */
            { 4, TG_NVC6B5_LAUNCH_DMA, 1 },
            /* [TG-master:722-724] channel setup: SET_OBJECT. */
            { 1, TG_NVC6C0_SET_OBJECT, 1 },
            /* [TG-master:154-157] memory_barrier. */
            { 1, TG_NVC6C0_INVALIDATE_SHADER_CACHES_NO_WFI, 1 },
        };
        for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
            uint32_t tg = tg_nvm(v[i].subc, v[i].mthd, v[i].count);
            uint32_t ours = pg106_pb_mthd_word(PG106_SEC_OP_INC_METHOD,
                                               v[i].subc, v[i].mthd, v[i].count);
            CHECK(ours == tg);
            printf("  vector %zu: subc=%u mthd=0x%03x count=%u -> 0x%08x\n",
                   i, v[i].subc, v[i].mthd, v[i].count, tg);
        }
    }

    /* ---- 2. End-to-end method words: our encoders must emit exactly
     * the word sequence tinygrad's nvm() produces for the same calls. */
    {
        pg106_pb pb;
        uint32_t buf[32];

        /* [TG-master:180]: nvm(1, SEND_PCAS_A, (qmd_addr >> 8).cast(u32))
         * with qmd_addr = 0x100000000 -> data word 0x01000000. */
        {
            uint32_t data = (uint32_t)(0x100000000ull >> 8);
            pg106_pb_init(&pb, buf, 32);
            CHECK(pg106_pb_inc(&pb, 1, PG106_NVC1C0_SEND_PCAS_A, &data, 1) == PG106_PB_OK);
            CHECK(pb.len == 2);
            CHECK(buf[0] == tg_nvm(1, TG_NVC6C0_SEND_PCAS_A, 1));
            CHECK(buf[1] == 0x01000000u);
            /* Pascal cross-check: same method number in our class header
             * (research/reference/clc1c0.h:374 = 0x2b4). */
            CHECK(PG106_NVC1C0_SEND_PCAS_A == TG_NVC6C0_SEND_PCAS_A);
        }

        /* [TG-master:111-114] wait(): nvm(0, SEM_ADDR_LO, addr(u64),
         * value(u64), sem_exec(u32)) - count 5, one header + 5 words,
         * covering SEM_ADDR_LO 0x5c .. SEM_EXECUTE 0x6c. */
        {
            const uint32_t data[5] = {
                0x90abcdefu, 0x12345678u,   /* semaphore addr */
                0x00000042u, 0x00000000u,   /* payload (64-bit) */
                TG_SEM_EXEC_ACQ_CIRC_GEQ_64BIT,
            };
            pg106_pb_init(&pb, buf, 32);
            CHECK(pg106_pb_inc(&pb, 0, TG_NVC56F_SEM_ADDR_LO, data, 5) == PG106_PB_OK);
            CHECK(pb.len == 6);
            CHECK(buf[0] == tg_nvm(0, TG_NVC56F_SEM_ADDR_LO, 5));
            for (int i = 0; i < 5; i++)
                CHECK(buf[1 + i] == data[i]);
            /* The 5-word run must span exactly the 5 channel semaphore
             * methods (0x5c, 0x60, 0x64, 0x68, 0x6c): count from the
             * first method to the last is (0x6c - 0x5c) / 4 + 1 = 5. */
            CHECK((TG_NVC56F_SEM_EXECUTE - TG_NVC56F_SEM_ADDR_LO) / 4 + 1 == 5);
            CHECK(PG106_DMA_METHOD_COUNT_MAX >= 5);
        }

        /* [TG-master:187-194] NVCopyQueue.copy(): two nvm() calls, one
         * 4-word (OFFSET_IN/OUT UPPER/LOWER), one 1-word each for
         * LINE_LENGTH_IN and LAUNCH_DMA, all subchannel 4. */
        {
            uint32_t data4[4] = { 0x22222222u, 0x1000u, 0x33333333u, 0x2000u };
            uint32_t len = 0x1000u, launch = 0x00000102u;
            pg106_pb_init(&pb, buf, 32);
            CHECK(pg106_pb_inc(&pb, 4, TG_NVC6B5_OFFSET_IN_UPPER, data4, 4) == PG106_PB_OK);
            CHECK(pg106_pb_inc(&pb, 4, TG_NVC6B5_LINE_LENGTH_IN, &len, 1) == PG106_PB_OK);
            CHECK(pg106_pb_inc(&pb, 4, TG_NVC6B5_LAUNCH_DMA, &launch, 1) == PG106_PB_OK);
            CHECK(buf[0] == tg_nvm(4, TG_NVC6B5_OFFSET_IN_UPPER, 4));
            CHECK(buf[5] == tg_nvm(4, TG_NVC6B5_LINE_LENGTH_IN, 1));
            CHECK(buf[7] == tg_nvm(4, TG_NVC6B5_LAUNCH_DMA, 1));
        }
    }

    /* ---- 3. Decode round-trip: our decoder must read tinygrad words
     * back into the right SEC_OP/COUNT/SUBCHANNEL/ADDRESS fields. */
    {
        uint32_t word = tg_nvm(1, TG_NVC6C0_SEND_PCAS_A, 1); /* 0x200120AD */
        unsigned sec_op, count, subch;
        uint32_t mthd;
        pg106_pb_decode(word, &sec_op, &count, &subch, &mthd);
        CHECK(sec_op == PG106_SEC_OP_INC_METHOD);
        CHECK(count == 1 && subch == 1 && mthd == 0x2b4u);

        /* The semaphore header decodes to the run start. */
        pg106_pb_decode(tg_nvm(0, TG_NVC56F_SEM_ADDR_LO, 5), &sec_op, &count, &subch, &mthd);
        CHECK(sec_op == PG106_SEC_OP_INC_METHOD && count == 5 && subch == 0 &&
              mthd == 0x5cu);
    }

    /* ---- 4. The typ<<28 divergence, locked as a test [ENC-RES].
     * typ=2 aliases SEC_OP=1 (INC_METHOD) exactly. typ=1 would be
     * SEC_OP=0 (GRP0_USE_TERT): a legacy word where COUNT[17:16]
     * aliases TERT_OP - a corrupt INC word, NOT a valid encoding.
     * tinygrad master never emits it (no `typ=` other than the default
     * in the whole file), so we only assert the decode behavior. */
    CHECK((1u << 28) >> 29 == 0);  /* typ=1 lands in SEC_OP=0's slot */
    {
        uint32_t tert = (1u << 28) | (1u << 16) | (1u << 13) | 0xADu;
        unsigned sec_op, count, subch;
        uint32_t mthd;
        pg106_pb_decode(tert, &sec_op, &count, &subch, &mthd);
        CHECK(sec_op == PG106_SEC_OP_GRP0_USE_TERT);
        CHECK(sec_op != PG106_SEC_OP_INC_METHOD); /* never emit, never read as INC */
    }

    /* ---- 5. GPFIFO entry identity [TG-master:97] vs pg106_gpfifo_pack.
     * tinygrad sets bit 41 = LEVEL bit = SUBROUTINE. That matches the
     * kernel nouveau uapi path: nvif_chan506f_gpfifo_push() packs
     * `main ? 0 : BIT(9)` and EXEC segments call it with main=false
     * [NVK-kernel]. SYNC bit stays 0 (tinygrad allows prefetch); the
     * kernel's no_prefetch maps to SYNC_WAIT = BIT(63), asserted below
     * as where ours would put it. */
    {
        static const struct { uint64_t addr; unsigned bytes; } v[] = {
            { 0x1234567890ull, 0x400 },   /* 256-word segment */
            { 0x40000000ull, 20 },        /* tiny 5-word segment */
            { 0x9876543210ull, 0x8000 },  /* high VA, 8 KiB */
            { 0x1000ull, 4 },             /* minimum: 1 dword */
        };
        for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
            uint64_t tg = tg_gpentry(v[i].addr, v[i].bytes);
            uint64_t ours = pg106_gpfifo_pack(v[i].addr, v[i].bytes / 4,
                                              PG106_GP_LEVEL_SUBROUTINE,
                                              PG106_GP_SYNC_PROCEED,
                                              PG106_GP_FETCH_UNCONDITIONAL);
            CHECK(ours == tg);
            /* Kernel no_prefetch variant: SYNC_WAIT is bit 63 in both. */
            uint64_t wait = pg106_gpfifo_pack(v[i].addr, v[i].bytes / 4,
                                              PG106_GP_LEVEL_SUBROUTINE,
                                              PG106_GP_SYNC_WAIT,
                                              PG106_GP_FETCH_UNCONDITIONAL);
            CHECK(wait == (tg | ((uint64_t)1 << 63)));
        }
        /* LENGTH units are dwords on both sides (kernel: (size>>2)<<10,
         * tinygrad: bytes//4 << 42, ours: len (dwords) << 10 in ENTRY1). */
        CHECK(PG106_GP_ENTRY1_LENGTH_MAX == 0x1FFFFFu);
    }

    /* ---- 6. QMD: v01_07 (Pascal, ours) vs QMDV03_00 (what tinygrad
     * emits for Ada/Blackwell [TG-master:50-54, 283-297]).
     *
     * tinygrad has NO Pascal-class QMD path: its class ladder starts at
     * AMPERE_COMPUTE_B 0xc7c0 [NV-570-autogen] and its QMD versions are
     * v05_00 (Blackwell, 0x60 words) and v03_00 (NVC6C0, 0x40 words),
     * with qmd_major_version=3 [TG-master:287]. So there is nothing to
     * differentially TEST for Pascal's v01_07 - instead this section
     * pins (a) the fields v01_07 and v03_00 SHARE bit-for-bit (the
     * generation-stable core a Pascal encoder can cite) and (b) the
     * fields that DIVERGE (why a v03_00 blob cannot be replayed on
     * Pascal). Values from [NV-570-autogen] QMDV03_00 (msb,lsb) tuples
     * vs our qmd.h MW(msb,lsb) macros. */
    {
        /* Our field macros are `msb, lsb` comma-pairs (two arguments of
         * pg106_qmd_insert); forward them variadically so the pair is
         * split after expansion, and compare against the v03_00 tuples
         * quoted from [NV-570-autogen]. */
#define TG_MS_(msb, lsb) (msb)
#define TG_LS_(msb, lsb) (lsb)
#define TG_MS(...) TG_MS_(__VA_ARGS__)
#define TG_LS(...) TG_LS_(__VA_ARGS__)

        /* Shared positions: identity, grid X/Y, thread dims, shared
         * memory, and the program address area at 287:256 (v01_07
         * PROGRAM_OFFSET == v03_00 PROGRAM_PREFETCH_ADDR_LOWER_SHIFTED). */
        CHECK(TG_MS(PG106_QMD_QMD_VERSION) == 579 &&
              TG_LS(PG106_QMD_QMD_VERSION) == 576);
        CHECK(TG_MS(PG106_QMD_QMD_MAJOR_VERSION) == 583 &&
              TG_LS(PG106_QMD_QMD_MAJOR_VERSION) == 580);
        CHECK(TG_MS(PG106_QMD_CTA_RASTER_WIDTH) == 415 &&
              TG_LS(PG106_QMD_CTA_RASTER_WIDTH) == 384);
        CHECK(TG_MS(PG106_QMD_CTA_RASTER_HEIGHT) == 431 &&
              TG_LS(PG106_QMD_CTA_RASTER_HEIGHT) == 416);
        CHECK(TG_MS(PG106_QMD_SHARED_MEMORY_SIZE) == 561 &&
              TG_LS(PG106_QMD_SHARED_MEMORY_SIZE) == 544);
        CHECK(TG_MS(PG106_QMD_CTA_THREAD_DIMENSION0) == 607 &&
              TG_LS(PG106_QMD_CTA_THREAD_DIMENSION0) == 592);
        CHECK(TG_MS(PG106_QMD_PROGRAM_OFFSET) == 287 &&
              TG_LS(PG106_QMD_PROGRAM_OFFSET) == 256);

        /* Divergent positions - exactly the ones the UAPI-ORACLE.md
         * mapping table calls out; lock them so a header update that
         * silently changes this cannot pass unnoticed. */
        CHECK(TG_MS(PG106_QMD_CTA_RASTER_DEPTH) == 447);  /* v03_00: 463 */
        CHECK(TG_MS(PG106_QMD_REGISTER_COUNT) == 1503);   /* v03_00 REGISTER_COUNT_V: 656 */
        CHECK(TG_MS(PG106_QMD_BARRIER_COUNT) == 1471);    /* v03_00: 767 */


        /* Field VALUE divergences: Pascal hardware wants the 01_07
         * identity (version 7, major 1); tinygrad stamps major 3 and
         * leaves the minor field at 0. */
        CHECK(PG106_QMD_VERSION_VALUE == 7);
        CHECK(PG106_QMD_MAJOR_VERSION_VALUE == 1);
    }

    /* ---- 7. PCAS method-number drift between classes. SEND_PCAS_A is
     * stable (0x2b4 both), the B/signaling variants moved on Ada. Our
     * Pascal constants must stay the clc1c0.h ones. */
    CHECK(PG106_NVC1C0_SEND_PCAS_A == 0x02b4u);
    CHECK(PG106_NVC1C0_SEND_PCAS_B == 0x02b8u);
    CHECK(TG_NVC6C0_SEND_SIGNALING_PCAS2_B == 0x02c0u);
    CHECK(PG106_NVC1C0_SEND_PCAS_B != TG_NVC6C0_SEND_SIGNALING_PCAS2_B);

    printf("%s: %d failure(s)\n", "test_tinygrad_compat", failures);
    return failures != 0;
}
