// SPDX-License-Identifier: GPL-2.0-only
/*
 * pascal/tests/test_dispatch.c - golden-vector test for the GP106
 * compute-dispatch submission model (pascal/core dispatch.h/.c).
 *
 * Golden values cross-checked against:
 *   - third_party/open-gpu-kernel-modules:src/common/sdk/nvidia/inc/class/clc06f.h (NVC06F_DMA_*, NVC06F_GP_ENTRY_*)
 *   - third_party/open-gpu-kernel-modules:src/common/sdk/nvidia/inc/class/clc1c0.h (NVC1C0_SET_OBJECT 0x0000,
 *     SET_INLINE_QMD_ADDRESS_A 0x0318 / _B 0x031C,
 *     LOAD_INLINE_QMD_DATA(i) 0x0320, SEND_PCAS_A 0x02B4,
 *     SEND_SIGNALING_PCAS_B 0x02BC)
 *   - third_party/mesa:src/nouveau/headers/nvidia/classes/clc1c0qmd.h via pascal/qmd.h (QMD words,
 *     same numbers as helios300-gp106-simtest.py section 5)
 *   - nouveau runlist/RAMFC layout (GPL-2.0): nvkm/engine/fifo/
 *     gp100.c gp100_runl_insert_chan, gk110.c gk110_runl_insert_cgrp,
 *     gk104.c gk104_chan_ramfc_write, chan/gf100.c gf100_chan_inst
 *   - Mesa NVK dispatch sequence: src/nouveau/vulkan/nvk_cmd_dispatch.c
 *     (SEND_PCAS_A + SEND_SIGNALING_PCAS_B)
 *
 * SIMULATION ONLY: this test packs words and compares them; it touches
 * no hardware. Grid/block are the simtest's numbers (129x64x1 / 8x8x1).
 */
#include <stdio.h>
#include <string.h>

#include "pascal/dispatch.h"

static int failures;
static int checks;

#define CHECK(cond) do { \
    checks++; \
    if (cond) { printf("PASS %s\n", #cond); } \
    else { printf("FAIL test_dispatch.c:%d %s\n", __LINE__, #cond); \
           failures++; } \
} while (0)

/* Simtest section-5 QMD parameters (grid 129x64x1, block 8x8x1). */
static const pg106_qmd_params sim_qmd = {
    .program_offset     = 0x00A00000u,
    .raster_width       = 129u,
    .raster_height      = 64u,
    .raster_depth       = 1u,
    .thread_dim         = { 8u, 8u, 1u },
    .shared_memory_size = 1024u,
    .register_count     = 168u,
    .barrier_count      = 1u,
    .local_mem_low_size = 32u,
    .release0_addr      = 0x00000000DEAD0000ull,
    .release0_payload   = 42u,
    .cb0_addr           = 0x0000000012345678ull,
    .cb0_size           = 256u,
};

/* The channel every launch runs on (values chosen to make the RAMFC
 * and runlist words recognizable; layout is what matters). */
static const pg106_sub_chan chan = {
    .chid       = 42u,
    .runq       = 1u,
    .inst_addr  = 0x0000000010000000ull,
    .userd_addr = 0x0000000020000000ull,
    .gpfifo_off = 0x0000000030000000ull,
    .gpfifo_len = 0x1000u,   /* 512 GPFIFO entries */
    .devm       = 0xFFFu,
    .priv       = false,
};

int main(void)
{
    /* ---- 1. Runlist entry (nouveau gp100_runl_insert_chan) ----------
     * word0 = chid | runq << 14; word1 = inst_addr >> 12. */
    {
        uint32_t rl[2];
        CHECK(pg106_sub_runlist_chan(&chan, rl) == PG106_SUB_OK);
        CHECK(rl[0] == (42u | (1u << 14)));           /* 0x0000402A */
        CHECK(rl[0] == 0x402Au);
        CHECK(rl[1] == (uint32_t)(0x10000000ull >> 12)); /* 0x00010000 */
        CHECK(rl[1] == 0x10000u);

        /* TSG entry (nouveau gk110_runl_insert_cgrp), chan_nr=1: */
        uint32_t cg[2];
        pg106_sub_runlist_cgrp(1u, 42u, cg);
        CHECK(cg[0] == ((1u << 26) | (128u << 18) | (3u << 14) |
                        0x00002000u | 42u));
        CHECK(cg[0] == 0x0600E02Au);
        CHECK(cg[1] == 0u);
    }

    /* ---- 2. Channel instance RAMFC (nouveau gk104_chan_ramfc_write) -
     * GP106 wires fifo/gp100.c .chan = &gm107_chan, .ramfc =
     * gk104_chan_ramfc; instance block 0x1000 bytes (gf100_chan_inst). */
    {
        uint32_t inst[PG106_SUB_INST_WORDS];
        pg106_sub_rec rec;
        pg106_sub_step steps[32];
        CHECK(pg106_sub_rec_init(&rec, steps, 32) == PG106_SUB_OK);
        CHECK(pg106_sub_ramfc_build(&chan, inst, &rec) == PG106_SUB_OK);

        const uint32_t limit2 = 9u;   /* ilog2(0x1000 / 8) = ilog2(512) */
        CHECK(inst[0x08 / 4u] == 0x20000000u); /* USERD lo */
        CHECK(inst[0x0c / 4u] == 0x00000000u); /* USERD hi */
        CHECK(inst[0x10 / 4u] == 0x0000faceu);
        CHECK(inst[0x30 / 4u] == 0xfffff902u);
        CHECK(inst[0x48 / 4u] == 0x30000000u); /* GPFIFO offset lo */
        CHECK(inst[0x4c / 4u] ==
              (uint32_t)(0x30000000ull >> 32) + (limit2 << 16));
        CHECK(inst[0x4c / 4u] == 0x00090000u);
        CHECK(inst[0x84 / 4u] == 0x20400000u);
        CHECK(inst[0x94 / 4u] == (0x30000000u | 0xFFFu)); /* 0x30FFFFFF */
        CHECK(inst[0x9c / 4u] == 0x00000100u);
        CHECK(inst[0xac / 4u] == 0x0000001fu);
        CHECK(inst[0xb8 / 4u] == 0xf8000000u);
        CHECK(inst[0xe4 / 4u] == 0u);          /* not privileged */
        CHECK(inst[0xe8 / 4u] == 42u);         /* chid */
        CHECK(inst[0xf8 / 4u] == 0x10003080u);
        CHECK(inst[0xfc / 4u] == 0x10000010u);
        /* Everything else stays zero (gf100_chan_inst .zero = true). */
        CHECK(inst[0] == 0u && inst[500] == 0u && inst[1023] == 0u);
        /* Every recorded step carries a citation. */
        CHECK(rec.len == 15u);
        for (size_t i = 0; i < rec.len; i++)
            if (steps[i].src == NULL || steps[i].src[0] == '\0') {
                checks++;
                printf("FAIL step %zu missing citation\n", i);
                failures++;
            }
        checks++;
        printf("PASS ramfc steps all cited (%zu steps)\n", rec.len);

        /* Range checks: bad runq, unaligned instance, non-pow2 ring. */
        pg106_sub_chan bad = chan;
        bad.runq = 2u;
        CHECK(pg106_sub_ramfc_build(&bad, inst, NULL) ==
              PG106_SUB_ERR_RANGE);
        bad = chan;
        bad.inst_addr = 0x10001234ull;
        CHECK(pg106_sub_ramfc_build(&bad, inst, NULL) ==
              PG106_SUB_ERR_RANGE);
        bad = chan;
        bad.gpfifo_len = 0x1800u;
        CHECK(pg106_sub_ramfc_build(&bad, inst, NULL) ==
              PG106_SUB_ERR_RANGE);
    }

    /* ---- 3. The launch golden vector --------------------------------
     * One pushbuffer program, one GPFIFO entry, doorbell ring. */
    {
        pg106_qmd qmd;
        CHECK(pg106_qmd_build(&sim_qmd, &qmd) == PG106_QMD_OK);
        CHECK(pg106_qmd_validate(&qmd) == PG106_QMD_OK);
        /* Simtest section-5 cross-checks: */
        CHECK(pg106_qmd_extract(qmd.w, PG106_QMD_CTA_RASTER_WIDTH) == 129u);
        CHECK(pg106_qmd_extract(qmd.w, PG106_QMD_CTA_RASTER_HEIGHT) == 64u);
        CHECK(pg106_qmd_extract(qmd.w, PG106_QMD_CTA_RASTER_DEPTH) == 1u);
        CHECK(qmd.w[12] == 129u);                       /* grid X alone */
        CHECK(qmd.w[18] == ((8u << 16) | (1u << 4) | 7u)); /* dim0+major+ver */

        const uint64_t qmd_addr  = 0x0000000000A10000ull; /* 256-aligned */
        const uint64_t push_addr = 0x0000000000B00000ull; /* 4-aligned  */

        uint32_t pb[128];
        uint64_t ring[8];
        memset(ring, 0, sizeof(ring));
        pg106_sub_step steps[128];
        memset(steps, 0, sizeof(steps)); /* zero padding for memcmp below */
        pg106_sub_rec rec;
        pg106_sub_rec_init(&rec, steps, 128);

        pg106_sub_launch_params lp = {
            .chan = &chan, .subch = 1u, .qmd = &qmd,
            .qmd_addr = qmd_addr, .push_addr = push_addr,
        };
        pg106_sub_result res;
        CHECK(pg106_sub_launch(&lp, pb, 128, ring, 8, 0, &res, &rec) ==
              PG106_SUB_OK);

        /* Exact method word sequence. Words 6..69 are the QMD image. */
        CHECK(res.pb_words == 74u);
        CHECK(pb[0] == ((1u << 29) | (1u << 16) | (1u << 13) |
                        (0x0000u >> 2)));            /* SET_OBJECT hdr  */
        CHECK(pb[0] == 0x20012000u);
        CHECK(pb[1] == 0x0000C1C0u);                 /* PASCAL_COMPUTE_B */
        CHECK(pb[2] == ((1u << 29) | (2u << 16) | (1u << 13) |
                        (0x0318u >> 2)));            /* QMD_ADDRESS_A hdr */
        CHECK(pb[2] == 0x200220C6u);
        CHECK(pb[3] == 0x00000000u);                 /* shifted8 upper */
        CHECK(pb[4] == 0x0000A100u);                 /* shifted8 lower */
        CHECK(pb[5] == ((3u << 29) | (64u << 16) | (1u << 13) |
                        (0x0320u >> 2)));            /* LOAD_QMD noninc */
        CHECK(pb[5] == 0x604020C8u);
        CHECK(memcmp(&pb[6], qmd.w, PG106_QMD_WORDS * sizeof(uint32_t))
              == 0);                                 /* the QMD image */
        CHECK(pb[70] == ((1u << 29) | (1u << 16) | (1u << 13) |
                         (0x02B4u >> 2)));           /* SEND_PCAS_A hdr */
        CHECK(pb[70] == 0x200120ADu);
        CHECK(pb[71] == 0x0000A100u);                /* qmd_addr >> 8 */
        CHECK(pb[72] == ((4u << 29) | (0x3u << 16) | (1u << 13) |
                         (0x02BCu >> 2)));           /* SIGNALING_PCAS_B */
        CHECK(pb[72] == 0x800320AFu);                /* IMMD invalidate|
                                                        schedule = 0x3 */
        CHECK(pb[73] == (7u << 29));                 /* END_PB_SEGMENT */
        CHECK(pb[73] == 0xE0000000u);

        /* Decode every header back through the clc06f.h fields. */
        unsigned sec_op, count, subch;
        uint32_t mthd;
        pg106_pb_decode(pb[0], &sec_op, &count, &subch, &mthd);
        CHECK(sec_op == PG106_SEC_OP_INC_METHOD && count == 1u &&
              subch == 1u && mthd == PG106_NVC1C0_SET_OBJECT);
        pg106_pb_decode(pb[2], &sec_op, &count, &subch, &mthd);
        CHECK(sec_op == PG106_SEC_OP_INC_METHOD && count == 2u &&
              subch == 1u && mthd == 0x0318u);
        pg106_pb_decode(pb[5], &sec_op, &count, &subch, &mthd);
        CHECK(sec_op == PG106_SEC_OP_NON_INC_METHOD && count == 64u &&
              subch == 1u && mthd == 0x0320u);
        pg106_pb_decode(pb[70], &sec_op, &count, &subch, &mthd);
        CHECK(sec_op == PG106_SEC_OP_INC_METHOD && count == 1u &&
              subch == 1u && mthd == PG106_NVC1C0_SEND_PCAS_A);
        pg106_pb_decode(pb[72], &sec_op, &count, &subch, &mthd);
        CHECK(sec_op == PG106_SEC_OP_IMMD_DATA_METHOD && count == 0x3u &&
              subch == 1u && mthd == 0x02BCu);
        pg106_pb_decode(pb[73], &sec_op, &count, &subch, &mthd);
        CHECK(sec_op == PG106_SEC_OP_END_PB_SEGMENT && mthd == 0u);

        /* GPFIFO entry: 74 dwords = 296 bytes of pushbuffer segment
         * (LENGTH field is a dword count per kernel chan506f.c). */
        const uint64_t want_entry =
            pg106_gpfifo_pack(push_addr, 74u, PG106_GP_LEVEL_MAIN,
                              PG106_GP_SYNC_PROCEED,
                              PG106_GP_FETCH_UNCONDITIONAL);
        CHECK(res.gpfifo_entry == want_entry);
        CHECK(res.gpfifo_entry == 0x0001280000B00000ull);
        CHECK(ring[0] == res.gpfifo_entry);
        CHECK(res.new_put == 1u);

        /* clc06f.h field round-trip. */
        uint64_t addr;
        uint32_t len;
        unsigned level, sync, fetch, priv;
        pg106_gpfifo_unpack(res.gpfifo_entry, &addr, &len, &level, &sync,
                            &fetch, &priv);
        CHECK(addr == push_addr && len == 74u && level == 0u &&
              sync == 0u && fetch == 0u && priv == 0u);

        /* Ordered steps, each with a citation. */
        CHECK(rec.len == 2u + res.pb_words + 2u);   /* QMD+REL0+PB+GP+bell */
        for (size_t i = 0; i < rec.len; i++)
            if (steps[i].src == NULL || steps[i].src[0] == '\0') {
                checks++;
                printf("FAIL launch step %zu missing citation\n", i);
                failures++;
            }
        checks++;
        printf("PASS launch steps all cited (%zu steps)\n", rec.len);
        CHECK(steps[0].type == PG106_SUB_STEP_QMD &&
              steps[0].addr == qmd_addr);
        CHECK(steps[1].type == PG106_SUB_STEP_RELEASE0 &&
              steps[1].addr == 0xDEAD0000ull && steps[1].value == 42u);
        CHECK(steps[rec.len - 2].type == PG106_SUB_STEP_GPFIFO &&
              steps[rec.len - 2].value == res.gpfifo_entry);
        CHECK(steps[rec.len - 1].type == PG106_SUB_STEP_DOORBELL &&
              steps[rec.len - 1].addr == PG106_SUB_DOORBELL_OFFSET &&
              steps[rec.len - 1].value == 1u);

        /* ---- 4. Determinism: run twice, diff clean ---------------- */
        uint32_t pb2[128];
        uint64_t ring2[8];
        pg106_sub_step steps2[128];
        memset(steps2, 0, sizeof(steps2)); /* zero padding, same as run 1 */
        pg106_sub_rec rec2;
        pg106_sub_rec_init(&rec2, steps2, 128);
        pg106_sub_result res2;
        CHECK(pg106_sub_launch(&lp, pb2, 128, ring2, 8, 0, &res2, &rec2) ==
              PG106_SUB_OK);
        CHECK(res2.pb_words == res.pb_words &&
              res2.gpfifo_entry == res.gpfifo_entry &&
              res2.new_put == res.new_put);
        CHECK(memcmp(pb2, pb, res.pb_words * sizeof(uint32_t)) == 0);
        CHECK(memcmp(ring2, ring, sizeof(ring)) == 0);
        CHECK(rec2.len == rec.len);
        CHECK(memcmp(steps2, steps, rec.len * sizeof(pg106_sub_step)) == 0);
        checks++;
        printf("PASS second run diffs clean\n");

        /* ---- 5. Ring wraparound ---------------------------------- */
        pg106_sub_result res3;
        CHECK(pg106_sub_launch(&lp, pb2, 128, ring2, 8, 7, &res3, NULL) ==
              PG106_SUB_OK);
        CHECK(ring2[7] == res3.gpfifo_entry);
        CHECK(res3.new_put == 0u);                   /* (7+1) % 8 */
    }

    /* ---- 6. Error paths ------------------------------------------ */
    {
        pg106_qmd qmd;
        CHECK(pg106_qmd_build(&sim_qmd, &qmd) == PG106_QMD_OK);

        pg106_sub_launch_params lp = {
            .chan = &chan, .subch = 1u, .qmd = &qmd,
            .qmd_addr = 0x0000000000A10000ull,
            .push_addr = 0x0000000000B00000ull,
        };
        uint32_t pb[128];
        uint64_t ring[8];
        pg106_sub_result res;

        CHECK(pg106_sub_launch(NULL, pb, 128, ring, 8, 0, &res, NULL) ==
              PG106_SUB_ERR_ARG);

        pg106_sub_launch_params lp2 = lp;
        lp2.qmd = NULL;
        CHECK(pg106_sub_launch(&lp2, pb, 128, ring, 8, 0, &res, NULL) ==
              PG106_SUB_ERR_ARG);

        lp2 = lp;
        lp2.qmd_addr = 0xA10008ull;                  /* not 256-aligned */
        CHECK(pg106_sub_launch(&lp2, pb, 128, ring, 8, 0, &res, NULL) ==
              PG106_SUB_ERR_RANGE);

        lp2 = lp;
        lp2.subch = 8u;                              /* SUBCHANNEL 15:13 */
        CHECK(pg106_sub_launch(&lp2, pb, 128, ring, 8, 0, &res, NULL) ==
              PG106_SUB_ERR_RANGE);

        pg106_qmd zero;
        memset(&zero, 0, sizeof(zero));              /* not v01_07 */
        lp2 = lp;
        lp2.qmd = &zero;
        CHECK(pg106_sub_launch(&lp2, pb, 128, ring, 8, 0, &res, NULL) ==
              PG106_SUB_ERR_QMD);

        /* v01_07 but RELEASE0 unarmed: no semaphore release to arm. */
        pg106_qmd_params norel = sim_qmd;
        norel.release0_addr = 0;
        pg106_qmd q2;
        CHECK(pg106_qmd_build(&norel, &q2) == PG106_QMD_OK);
        lp2 = lp;
        lp2.qmd = &q2;
        CHECK(pg106_sub_launch(&lp2, pb, 128, ring, 8, 0, &res, NULL) ==
              PG106_SUB_ERR_QMD);

        /* Pushbuffer too small. */
        lp2 = lp;
        lp2.qmd = &qmd;
        uint32_t tiny[8];
        CHECK(pg106_sub_launch(&lp2, tiny, 8, ring, 8, 0, &res, NULL) ==
              PG106_PB_ERR_SPACE);

        /* GPFIFO ring full / bad put. */
        CHECK(pg106_sub_launch(&lp, pb, 128, ring, 8, 8, &res, NULL) ==
              PG106_SUB_ERR_RANGE);
    }

    printf("%s %d checks, %d failures\n", failures ? "FAIL" : "PASS",
           checks, failures);
    return failures ? 1 : 0;
}
