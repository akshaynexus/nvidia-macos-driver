// SPDX-License-Identifier: MIT
/*
 * pascal/tests/test_pushbuf.c - golden vectors for the GP106 pushbuffer
 * and GPFIFO encoders (pascal/core).
 *
 * Golden values are cross-checked against:
 *   - helios300-gp106-simtest.py sections 3-4 (same numbers)
 *   - third_party/open-gpu-kernel-modules:src/common/sdk/nvidia/inc/class/clc06f.h (NVC06F_DMA_*, NVC06F_GP_ENTRY_*)
 *   - third_party/open-gpu-kernel-modules:src/common/sdk/nvidia/inc/class/clc1c0.h (NVC1C0_SET_OBJECT, PASCAL_COMPUTE_B)
 *   - research/evidence/ENCODING-RESOLUTION.md
 *
 * This is a simulation/host-side test only; it touches no hardware.
 * Build/run via the pascal Makefile, or directly:
 *   cc -std=c11 -Wall -Wextra -I ../core/include test_pushbuf.c ../core/src/pushbuf.c -o /tmp/opencode/test_pushbuf
 */
#include <stdio.h>
#include "pascal/pushbuf.h"

static int failures;

#define CHECK(cond) do { \
    if (cond) { printf("PASS %s\n", #cond); } \
    else { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

int main(void)
{
    /* ---- 1. SET_OBJECT golden vector (simtest section 3) -------------
     * NVC1C0_SET_OBJECT (0x0000), class PASCAL_COMPUTE_B (0xC1C0),
     * subchannel 1: header word must be exactly 0x20012000.
     *   NVC06F_DMA_INCR_OPCODE_VALUE = 1 -> SEC_OP[31:29] = 1
     *   NVC06F_DMA_INCR_COUNT 28:16 = 1 data word
     *   NVC06F_DMA_INCR_SUBCHANNEL 15:13 = 1
     *   NVC06F_DMA_INCR_ADDRESS 11:0 = 0x0000 >> 2 = 0 */
    {
        uint32_t buf[8];
        pg106_pb pb;
        CHECK(pg106_pb_init(&pb, buf, 8) == PG106_PB_OK);
        CHECK(pg106_pb_set_object(&pb, 1, PG106_CLASS_PASCAL_COMPUTE_B)
              == PG106_PB_OK);
        CHECK(pb.len == 2);
        CHECK(buf[0] == 0x20012000u);
        CHECK(buf[1] == 0xC1C0u);
        unsigned sec_op, count, subch;
        uint32_t mthd;
        pg106_pb_decode(buf[0], &sec_op, &count, &subch, &mthd);
        CHECK(sec_op == PG106_SEC_OP_INC_METHOD);   /* NVC06F_DMA_SEC_OP_INC_METHOD */
        CHECK(count == 1);                          /* NVC06F_DMA_INCR_COUNT */
        CHECK(subch == 1);                          /* NVC06F_DMA_INCR_SUBCHANNEL */
        CHECK(mthd == PG106_NVC1C0_SET_OBJECT);     /* clc1c0.h 0x0000 */
    }

    /* ---- 2. SEND_PCAS_A as a non-increasing method (simtest §3) ------
     * NVC1C0_SEND_PCAS_A = 0x02B4, zero data words (the QMD address
     * arrives as a separate pushbuffer entry):
     *   NVC06F_DMA_NONINCR_OPCODE_VALUE = 3 -> SEC_OP[31:29] = 3. */
    {
        uint32_t buf[8];
        pg106_pb pb;
        pg106_pb_init(&pb, buf, 8);
        CHECK(pg106_pb_noninc(&pb, 1, PG106_NVC1C0_SEND_PCAS_A, NULL, 0)
              == PG106_PB_OK);
        CHECK(pb.len == 1);
        CHECK(buf[0] == ((3u << 29) | (0u << 16) | (1u << 13) | (0x02B4u >> 2)));
        CHECK(buf[0] == 0x600020ADu);
        unsigned sec_op, count, subch;
        uint32_t mthd;
        pg106_pb_decode(buf[0], &sec_op, &count, &subch, &mthd);
        CHECK(sec_op == PG106_SEC_OP_NON_INC_METHOD);
        CHECK(count == 0 && subch == 1 && mthd == 0x02B4u);
    }

    /* ---- 3. tinygrad-compat decode (ENCODING-RESOLUTION.md) ----------
     * typ<<28 with typ=2 == 1<<29: SEC_OP=1 (INC_METHOD). typ=1 lands on
     * SEC_OP=0 (legacy TERT) and sets COUNT's MSB - never emit it. */
    {
        unsigned sec_op, count, subch;
        uint32_t mthd;
        pg106_pb_decode((2u << 28) | (1u << 13) | (0x02B4u >> 2),
                        &sec_op, &count, &subch, &mthd);
        CHECK(sec_op == PG106_SEC_OP_INC_METHOD);
        CHECK(count == 0 && subch == 1 && mthd == 0x02B4u);
        pg106_pb_decode((1u << 28) | (1u << 13) | (0x02B4u >> 2),
                        &sec_op, &count, &subch, &mthd);
        CHECK(sec_op == 0);            /* GRP0_USE_TERT, not a real op */
        CHECK(count == 0x1000u);       /* the typ bit leaked into COUNT */
    }

    /* ---- 4. IMMD_DATA method -----------------------------------------
     * NVC06F_DMA_IMMD_OPCODE_VALUE = 4, NVC06F_DMA_IMMD_DATA 28:16;
     * example: SEND_PCAS_B (0x02B8) with data 0x42, subchannel 1. */
    {
        uint32_t buf[8];
        pg106_pb pb;
        pg106_pb_init(&pb, buf, 8);
        CHECK(pg106_pb_immd(&pb, 1, PG106_NVC1C0_SEND_PCAS_B, 0x42u)
              == PG106_PB_OK);
        CHECK(pb.len == 1);
        CHECK(buf[0] == ((4u << 29) | (0x42u << 16) | (1u << 13) | (0x02B8u >> 2)));
        CHECK(buf[0] == 0x804220AEu);
        unsigned sec_op, count, subch;
        uint32_t mthd;
        pg106_pb_decode(buf[0], &sec_op, &count, &subch, &mthd);
        CHECK(sec_op == PG106_SEC_OP_IMMD_DATA_METHOD);
        CHECK(count == 0x42u);         /* data rides in the COUNT field */
        CHECK(mthd == PG106_NVC1C0_SEND_PCAS_B);
    }

    /* ---- 5. END_PB_SEGMENT -------------------------------------------
     * NVC06F_DMA_SEC_OP_END_PB_SEGMENT = 7, single word, rest zero. */
    {
        uint32_t buf[8];
        pg106_pb pb;
        pg106_pb_init(&pb, buf, 8);
        CHECK(pg106_pb_endseg(&pb) == PG106_PB_OK);
        CHECK(pb.len == 1);
        CHECK(buf[0] == 0xE0000000u);
        unsigned sec_op;
        pg106_pb_decode(buf[0], &sec_op, NULL, NULL, NULL);
        CHECK(sec_op == PG106_SEC_OP_END_PB_SEGMENT);
    }

    /* ---- 6. whole-segment golden: SET_OBJECT, SEND_PCAS_A, ENDSEG ----
     * the inc/noninc/endseg sequence a compute submission opens with. */
    {
        uint32_t buf[16];
        pg106_pb pb;
        pg106_pb_init(&pb, buf, 16);
        CHECK(pg106_pb_set_object(&pb, 1, PG106_CLASS_PASCAL_COMPUTE_B)
              == PG106_PB_OK);
        CHECK(pg106_pb_noninc(&pb, 1, PG106_NVC1C0_SEND_PCAS_A, NULL, 0)
              == PG106_PB_OK);
        CHECK(pg106_pb_endseg(&pb) == PG106_PB_OK);
        CHECK(pb.len == 4);
        const uint32_t golden[4] = {
            0x20012000u,   /* SET_OBJECT header, simtest section 3 */
            0x0000C1C0u,   /* PASCAL_COMPUTE_B class id (clc1c0.h) */
            0x600020ADu,   /* SEND_PCAS_A noninc header */
            0xE0000000u,   /* END_PB_SEGMENT */
        };
        for (size_t i = 0; i < 4; i++)
            CHECK(buf[i] == golden[i]);
    }

    /* ---- 7. GPFIFO entry round-trip (simtest section 4) --------------
     * NVC06F_GP_ENTRY0_GET 31:2, ENTRY1_GET_HI 7:0, _LEVEL 9:9
     * (LEVEL_MAIN = 0), _LENGTH 30:10, _SYNC 31:31, ENTRY0_FETCH 0:0. */
    {
        uint64_t e = pg106_gpfifo_pack(0x1234000ull, 257,
                                       PG106_GP_LEVEL_MAIN,
                                       PG106_GP_SYNC_PROCEED,
                                       PG106_GP_FETCH_UNCONDITIONAL);
        CHECK(((e >> 2) & 0x3FFFFFFFull) == (0x1234000ull >> 2));
        CHECK(((e >> 42) & PG106_GP_ENTRY1_LENGTH_MAX) == 257u);
        CHECK(((e >> 41) & 1u) == PG106_GP_LEVEL_MAIN);      /* 0, not 1 */
        CHECK(((e >> 63) & 1u) == PG106_GP_SYNC_PROCEED);
        CHECK((e & 1u) == PG106_GP_FETCH_UNCONDITIONAL);

        uint64_t addr;
        uint32_t len;
        unsigned level, sync, fetch, priv;
        pg106_gpfifo_unpack(e, &addr, &len, &level, &sync, &fetch, &priv);
        CHECK(addr == 0x1234000ull);
        CHECK(len == 257u);
        CHECK(level == PG106_GP_LEVEL_MAIN);
        CHECK(sync == PG106_GP_SYNC_PROCEED);
        CHECK(fetch == PG106_GP_FETCH_UNCONDITIONAL);
        CHECK(priv == 0u);             /* NVC06F_GP_ENTRY1_PRIV_USER */

        /* Subroutine level, wait sync, conditional fetch, and the
         * GET_HI path for addresses above 4 GiB. */
        e = pg106_gpfifo_pack(0x123456780ull, 3, PG106_GP_LEVEL_SUBROUTINE,
                              PG106_GP_SYNC_WAIT, PG106_GP_FETCH_CONDITIONAL);
        CHECK(((e >> 41) & 1u) == PG106_GP_LEVEL_SUBROUTINE);
        CHECK(((e >> 63) & 1u) == PG106_GP_SYNC_WAIT);
        CHECK((e & 1u) == PG106_GP_FETCH_CONDITIONAL);
        pg106_gpfifo_unpack(e, &addr, &len, &level, &sync, &fetch, &priv);
        CHECK(addr == 0x123456780ull);   /* GET_HI carried bits 39:32 */
        CHECK(len == 3u && level == 1u && sync == 1u && fetch == 1u);
    }

    /* ---- 8. bounds and error paths ---------------------------------- */
    {
        uint32_t buf[4];
        pg106_pb pb;

        /* Overflowing pushbuffer: 2 words needed, 1 left -> ERR_SPACE,
         * and the error is sticky for later writes. */
        pg106_pb_init(&pb, buf, 1);
        CHECK(pg106_pb_inc(&pb, 1, 0x0100u, (uint32_t[]){1u, 2u}, 2)
              == PG106_PB_ERR_SPACE);
        CHECK(pg106_pb_endseg(&pb) == PG106_PB_ERR_SPACE);
        CHECK(pb.len == 0);            /* nothing was written */
        CHECK(pb.error == PG106_PB_ERR_SPACE);

        /* count > 0x1FFF does not fit NVC06F_DMA_METHOD_COUNT 28:16. */
        pg106_pb_init(&pb, buf, 16);
        uint32_t big[0x2000u];
        CHECK(pg106_pb_inc(&pb, 1, 0x0100u, big, 0x2000u)
              == PG106_PB_ERR_RANGE);

        /* inc with count = 0 mirrors the simtest's assert len(args) >= 1. */
        pg106_pb_init(&pb, buf, 16);
        CHECK(pg106_pb_inc(&pb, 1, 0x0100u, NULL, 0) == PG106_PB_ERR_ARG);

        /* clc06f.h:233 NVC06F_DMA_METHOD_ADDRESS is 11:0 = 12 bits of
         * mthd >> 2: 0x3FFC is the last encodable method, 0x4000 the
         * first that is not (it would alias into header bit 12). */
        pg106_pb_init(&pb, buf, 16);
        CHECK(pg106_pb_inc(&pb, 1, 0x3FFCu, (uint32_t[]){0u}, 1)
              == PG106_PB_OK);
        CHECK((buf[0] & 0x1FFFu) == 0x0FFFu);   /* bit 12 stays clear */
        pg106_pb_init(&pb, buf, 16);
        CHECK(pg106_pb_inc(&pb, 1, 0x4000u, (uint32_t[]){0u}, 1)
              == PG106_PB_ERR_RANGE);
        pg106_pb_init(&pb, buf, 16);
        CHECK(pg106_pb_immd(&pb, 1, 0x7FFCu, 1u) == PG106_PB_ERR_RANGE);
        pg106_pb_init(&pb, buf, 16);
        CHECK(pg106_pb_inc(&pb, 1, 0x8000u, (uint32_t[]){0u}, 1)
              == PG106_PB_ERR_RANGE);
        /* A non-dword method would lose its low bits in mthd >> 2. */
        pg106_pb_init(&pb, buf, 16);
        CHECK(pg106_pb_inc(&pb, 1, 0x0102u, (uint32_t[]){0u}, 1)
              == PG106_PB_ERR_RANGE);
        /* Decoder ignores header bit 12 (not part of ADDRESS 11:0). */
        {
            uint32_t m;
            pg106_pb_decode(0x20011000u | 0x0FFFu, NULL, NULL, NULL, &m);
            CHECK(m == 0x3FFCu);
        }

        /* Subchannel does not fit 15:13. */
        pg106_pb_init(&pb, buf, 16);
        CHECK(pg106_pb_inc(&pb, 8, 0x0100u, (uint32_t[]){0u}, 1)
              == PG106_PB_ERR_RANGE);

        /* immd data does not fit NVC06F_DMA_IMMD_DATA 28:16. */
        pg106_pb_init(&pb, buf, 16);
        CHECK(pg106_pb_immd(&pb, 1, 0x0100u, 0x2000u)
              == PG106_PB_ERR_RANGE);

        /* NULL arguments. */
        pg106_pb_init(&pb, buf, 16);
        CHECK(pg106_pb_inc(NULL, 1, 0x0100u, buf, 1) == PG106_PB_ERR_ARG);
        CHECK(pg106_pb_inc(&pb, 1, 0x0100u, NULL, 1) == PG106_PB_ERR_ARG);
        CHECK(pg106_pb_endseg(NULL) == PG106_PB_ERR_ARG);
        CHECK(pg106_pb_init(NULL, buf, 4) == PG106_PB_ERR_ARG);

        /* noninc allows count = 0 (SEND_PCAS_A), but not count with NULL. */
        pg106_pb_init(&pb, buf, 16);
        CHECK(pg106_pb_noninc(&pb, 1, 0x02B4u, NULL, 0) == PG106_PB_OK);
        CHECK(pg106_pb_noninc(&pb, 1, 0x02B4u, NULL, 1) == PG106_PB_ERR_ARG);

        /* exact-fit capacity must succeed (1 header + 3 data in 4). */
        pg106_pb_init(&pb, buf, 4);
        CHECK(pg106_pb_inc(&pb, 1, 0x0100u, (uint32_t[]){7u, 8u, 9u}, 3)
              == PG106_PB_OK);
        CHECK(pb.len == 4 && buf[1] == 7u && buf[3] == 9u);
    }

    if (failures == 0) {
        printf("PASS test_pushbuf: all checks green\n");
        return 0;
    }
    printf("FAIL test_pushbuf: %d check(s) failed\n", failures);
    return 1;
}
