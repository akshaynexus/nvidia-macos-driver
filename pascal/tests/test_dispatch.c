// SPDX-License-Identifier: GPL-2.0-only
/*
 * pascal/tests/test_dispatch.c - golden-vector test for the GP106
 * compute-dispatch submission model (pascal/core dispatch.h/.c).
 *
 * Golden values cross-checked against:
 *   - third_party/open-gpu-kernel-modules:src/common/sdk/nvidia/inc/class/clc06f.h (NVC06F_DMA_*, NVC06F_GP_ENTRY_*)
 *   - third_party/open-gpu-kernel-modules:src/common/sdk/nvidia/inc/class/
 *     clc1c0.h method offsets (SET_OBJECT 0x0000, SET_SHADER_SHARED_
 *     MEMORY_WINDOW 0x0214, SEND_PCAS_A 0x02b4, SEND_SIGNALING_PCAS_B
 *     0x02bc, SET_SHADER_LOCAL_MEMORY_NON_THROTTLED_A 0x02e4,
 *     _THROTTLED_A 0x02f0, SET_SHADER_LOCAL_MEMORY_WINDOW 0x077c,
 *     SET_SHADER_LOCAL_MEMORY_A 0x0790, SET_PROGRAM_REGION_A 0x1608)
 *     and clc06f.h:62 (USERD GPPut at 0x8c)
 *   - nouveau runlist/RAMFC/join layout (GPL-2.0): nvkm/engine/fifo/
 *     gp100.c gp100_runl_insert_chan, gk110.c gk110_runl_insert_cgrp,
 *     gk104.c gk104_chan_ramfc_write; nvkm/subdev/mmu/vmmgp100.c
 *     gp100_vmm_join -> vmmgf100.c gf100_vmm_join_; nvif/chan506f.c
 *     gpfifo_push (PUT mask) and gpfifo_kick (USERD+0x8c)
 *   - Mesa NVK: nvk_cmd_dispatch.c nvk_push_dispatch_state_init (:32-89)
 *     and nvk_CmdDispatchBase (:340-349), nvk_queue.c:224-238 (SLM)
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
    .crs_size           = 0x800u,
    .release0_addr      = 0x00000000DEAD0000ull,
    .release0_payload   = 42u,
    .cbuf               = { { .addr = 0x12345600ull, .size = 256u } },
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
    .pdb_addr   = 0x0000000108000000ull, /* above 4 GiB: exercises 0x204 */
    .pdb_target = PG106_TARGET_VRAM,
    .vm_limit   = 1ull << 49,            /* vmm.c:1153 1 << bits, GP100 49 */
};
#define RING_ENTRIES 512u                /* chan.gpfifo_len / 8 */

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

        /* VMM join (gf100_vmm_join_ with gp100_vmm_join's base):
         * 0x200 = pd addr | VRAM target 0 | BIT(10) VER2 | BIT(11) 64KiB,
         * 0x208 = vmm->limit - 1 = 2^49 - 1. */
        CHECK(inst[0x200 / 4u] == (0x08000000u | 0x400u | 0x800u));
        CHECK(inst[0x204 / 4u] == 0x00000001u);
        CHECK(inst[0x208 / 4u] == 0xFFFFFFFFu);
        CHECK(inst[0x20c / 4u] == 0x0001FFFFu);
        /* join first, like nvkm_chan_new_ (chan.c:421 before :475) */
        CHECK(steps[0].type == PG106_SUB_STEP_VMM_JOIN &&
              steps[0].addr == 0x200u);
        CHECK(steps[4].type == PG106_SUB_STEP_RAMFC);

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
        CHECK(rec.len == 4u + 15u);
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

        /* Join targets (gf100_vmm_join_ :346-355): coherent HOST is
         * 2 | VOL BIT(2), NCOH is 3. Bad join inputs are rejected. */
        bad = chan;
        bad.pdb_target = PG106_TARGET_SYS_COHERENT;
        CHECK(pg106_sub_vmm_join(&bad, inst, NULL) == PG106_SUB_OK);
        CHECK(inst[0x200 / 4u] == (0x08000000u | 0xC00u | 0x2u | 0x4u));
        bad.pdb_target = PG106_TARGET_SYS_NONCOHERENT;
        CHECK(pg106_sub_vmm_join(&bad, inst, NULL) == PG106_SUB_OK);
        CHECK(inst[0x200 / 4u] == (0x08000000u | 0xC00u | 0x3u));
        bad = chan;
        bad.pdb_addr = 0;
        CHECK(pg106_sub_ramfc_build(&bad, inst, NULL) == PG106_SUB_ERR_RANGE);
        bad.pdb_addr = 0x08000800ull;            /* not 4K-aligned */
        CHECK(pg106_sub_ramfc_build(&bad, inst, NULL) == PG106_SUB_ERR_RANGE);
        bad = chan;
        bad.vm_limit = (1ull << 49) + 1u;        /* beyond GP100 VA */
        CHECK(pg106_sub_ramfc_build(&bad, inst, NULL) == PG106_SUB_ERR_RANGE);
    }

    /* ---- 3. The launch golden vector --------------------------------
     * Compute state + one launch, NVK's sequence; header words are
     * computed from the clc06f.h fields (SEC_OP 31:29, COUNT 28:16,
     * SUBCH 15:13, ADDR 11:0 = mthd >> 2) and clc1c0.h offsets. */
    {
        pg106_qmd qmd;
        CHECK(pg106_qmd_build(&sim_qmd, &qmd) == PG106_QMD_OK);
        CHECK(pg106_qmd_validate(&qmd) == PG106_QMD_OK);

        const uint64_t qmd_addr  = 0x0000000000A10000ull; /* 256-aligned */
        const uint64_t push_addr = 0x0000000000B00000ull; /* 4-aligned  */
        const pg106_sub_compute_state cs = {
            .program_region       = 0x0000000123400000ull,
            .local_memory_window  = 0xff000000u, /* nvk_cmd_dispatch.c:82 */
            .shared_memory_window = 0xfe000000u, /* nvk_cmd_dispatch.c:85 */
            .slm_addr             = 0x0000000140000000ull,
            .slm_per_tpc          = 0x0000000100008000ull, /* 0x8000-aligned */
        };
#define HDR(op, n, m) (((op) << 29) | ((n) << 16) | (1u << 13) | ((m) >> 2))
        static const uint32_t want[] = {
            HDR(1u, 1u, 0x0000u), 0x0000C1C0u,          /* SET_OBJECT */
            HDR(1u, 2u, 0x1608u), 0x1u, 0x23400000u,    /* PROGRAM_REGION_A/B */
            HDR(1u, 1u, 0x077cu), 0xff000000u,          /* LOCAL_MEMORY_WINDOW */
            HDR(1u, 1u, 0x0214u), 0xfe000000u,          /* SHARED_MEMORY_WINDOW */
            HDR(1u, 2u, 0x0790u), 0x1u, 0x40000000u,    /* SHADER_LOCAL_MEMORY_A/B */
            HDR(1u, 3u, 0x02e4u), 0x1u, 0x8000u, 0xffu, /* NON_THROTTLED_A/B/C */
            HDR(1u, 3u, 0x02f0u), 0x1u, 0x8000u, 0xffu, /* THROTTLED_A/B/C */
            HDR(1u, 1u, 0x02b4u), 0x0000A100u,          /* SEND_PCAS_A: qmd >> 8 */
            HDR(4u, 3u, 0x02bcu),                       /* SEND_SIGNALING_PCAS_B */
        };
#undef HDR
        const size_t nwant = sizeof(want) / sizeof(want[0]);
        CHECK(want[0] == 0x20012000u && want[nwant - 1] == 0x800320AFu);

        uint32_t pb[128];
        static uint64_t ring[RING_ENTRIES];
        memset(ring, 0, sizeof(ring));
        pg106_sub_step steps[128];
        memset(steps, 0, sizeof(steps)); /* zero padding for memcmp below */
        pg106_sub_rec rec;
        pg106_sub_rec_init(&rec, steps, 128);

        pg106_sub_launch_params lp = {
            .chan = &chan, .subch = 1u, .qmd = &qmd,
            .qmd_addr = qmd_addr, .push_addr = push_addr, .init = &cs,
        };
        pg106_sub_result res;
        CHECK(pg106_sub_launch(&lp, pb, 128, ring, RING_ENTRIES, 0, &res,
                               &rec) == PG106_SUB_OK);
        CHECK(res.pb_words == nwant);
        CHECK(memcmp(pb, want, sizeof(want)) == 0);
        /* The QMD is not inlined (NVK uploads it to memory): no
         * SET_INLINE_QMD_ADDRESS (0x0318) / LOAD_INLINE_QMD_DATA (0x0320). */
        for (size_t i = 0; i < res.pb_words; i++) {
            uint32_t m;
            pg106_pb_decode(pb[i], NULL, NULL, NULL, &m);
            if ((pb[i] >> 29) == 1u && (m == 0x0318u || m == 0x0320u)) {
                checks++; failures++;
                printf("FAIL inline QMD method at word %zu\n", i);
            }
        }

        /* Without init: just the two launch methods. */
        uint32_t pb_l[8];
        pg106_sub_launch_params lp_noinit = lp;
        lp_noinit.init = NULL;
        pg106_sub_result res_l;
        CHECK(pg106_sub_launch(&lp_noinit, pb_l, 8, ring, RING_ENTRIES, 1,
                               &res_l, NULL) == PG106_SUB_OK);
        CHECK(res_l.pb_words == 3u &&
              memcmp(pb_l, &want[nwant - 3], 3 * sizeof(uint32_t)) == 0);
        /* No SLM: the SLM methods are skipped (nvk_queue.c:216 qss->mem). */
        pg106_sub_compute_state cs_noslm = cs;
        cs_noslm.slm_addr = 0;
        pg106_pb pbn;
        uint32_t wn[16];
        pg106_pb_init(&pbn, wn, 16);
        CHECK(pg106_sub_compute_init(&pbn, 1u, &cs_noslm) == PG106_PB_OK);
        CHECK(pbn.len == 9u && memcmp(wn, want, 9 * sizeof(uint32_t)) == 0);

        /* GPFIFO entry (nvif_chan506f_gpfifo_push): lo = push_addr,
         * hi = upper | (dwords << 10); LEVEL MAIN, no SYNC. */
        CHECK(res.gpfifo_entry ==
              (((uint64_t)(nwant << 10)) << 32 | 0x00B00000ull));
        CHECK(ring[0] == res.gpfifo_entry);
        CHECK(res.new_put == 1u);

        /* Ordered steps, each with a citation. */
        CHECK(rec.len == 2u + res.pb_words + 2u);   /* QMD+REL0+PB+GP+PUT */
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
        /* Kick = GPPut at USERD + 0x8c (clc06f.h:62, chan506f.c:11);
         * never BAR0 + 0x90 (Volta usermode, nvif/userc361.c:40). */
        CHECK(steps[rec.len - 1].type == PG106_SUB_STEP_GP_PUT &&
              steps[rec.len - 1].addr == chan.userd_addr + 0x8cu &&
              steps[rec.len - 1].value == 1u);

        /* ---- 4. Determinism: run twice, diff clean ---------------- */
        uint32_t pb2[128];
        static uint64_t ring2[RING_ENTRIES];
        memset(ring2, 0, sizeof(ring2));
        pg106_sub_step steps2[128];
        memset(steps2, 0, sizeof(steps2));
        pg106_sub_rec rec2;
        pg106_sub_rec_init(&rec2, steps2, 128);
        pg106_sub_result res2;
        memset(ring, 0, sizeof(ring));
        CHECK(pg106_sub_launch(&lp, pb, 128, ring, RING_ENTRIES, 0, &res,
                               NULL) == PG106_SUB_OK);
        CHECK(pg106_sub_launch(&lp, pb2, 128, ring2, RING_ENTRIES, 0, &res2,
                               &rec2) == PG106_SUB_OK);
        CHECK(res2.pb_words == res.pb_words &&
              res2.gpfifo_entry == res.gpfifo_entry &&
              res2.new_put == res.new_put);
        CHECK(memcmp(pb2, pb, res.pb_words * sizeof(uint32_t)) == 0);
        CHECK(memcmp(ring2, ring, sizeof(ring)) == 0);
        CHECK(rec2.len == rec.len);
        CHECK(memcmp(steps2, steps, rec.len * sizeof(pg106_sub_step)) == 0);

        /* ---- 5. Ring wraparound: power-of-two mask, chan506f.c:28 - */
        pg106_sub_result res3;
        CHECK(pg106_sub_launch(&lp, pb2, 128, ring2, RING_ENTRIES,
                               RING_ENTRIES - 1u, &res3, NULL) ==
              PG106_SUB_OK);
        CHECK(ring2[RING_ENTRIES - 1u] == res3.gpfifo_entry);
        CHECK(res3.new_put == 0u);
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
        static uint64_t ring[RING_ENTRIES];
        pg106_sub_result res;
#define LAUNCH(lpp, pbp, cap) \
        pg106_sub_launch((lpp), (pbp), (cap), ring, RING_ENTRIES, 0, &res, NULL)

        CHECK(pg106_sub_launch(NULL, pb, 128, ring, RING_ENTRIES, 0, &res,
                               NULL) == PG106_SUB_ERR_ARG);

        pg106_sub_launch_params lp2 = lp;
        lp2.qmd = NULL;
        CHECK(LAUNCH(&lp2, pb, 128) ==
              PG106_SUB_ERR_ARG);

        lp2 = lp;
        lp2.qmd_addr = 0xA10008ull;                  /* not 256-aligned */
        CHECK(LAUNCH(&lp2, pb, 128) ==
              PG106_SUB_ERR_RANGE);

        lp2 = lp;
        lp2.subch = 8u;                              /* SUBCHANNEL 15:13 */
        CHECK(LAUNCH(&lp2, pb, 128) ==
              PG106_SUB_ERR_RANGE);

        pg106_qmd zero;
        memset(&zero, 0, sizeof(zero));              /* not v02_01 */
        lp2 = lp;
        lp2.qmd = &zero;
        CHECK(LAUNCH(&lp2, pb, 128) ==
              PG106_SUB_ERR_QMD);

        /* v02_01 but RELEASE0 unarmed: no semaphore release to arm. */
        pg106_qmd_params norel = sim_qmd;
        norel.release0_addr = 0;
        pg106_qmd q2;
        CHECK(pg106_qmd_build(&norel, &q2) == PG106_QMD_OK);
        lp2 = lp;
        lp2.qmd = &q2;
        CHECK(LAUNCH(&lp2, pb, 128) ==
              PG106_SUB_ERR_QMD);

        /* Pushbuffer too small. */
        lp2 = lp;
        lp2.qmd = &qmd;
        uint32_t tiny[8];
        lp2.init = NULL;
        CHECK(LAUNCH(&lp2, tiny, 2) == PG106_PB_ERR_SPACE);

        /* A release whose ENABLE0 bit was cleared is inert on hardware
         * (clc1c0qmd.h:484): rejected even with an address present. */
        pg106_qmd noen = qmd;
        noen.w[4] &= ~(1u << 10);
        lp2 = lp;
        lp2.qmd = &noen;
        CHECK(LAUNCH(&lp2, pb, 128) == PG106_SUB_ERR_QMD);

        /* push_addr beyond GET 31:2 + GET_HI 7:0 (40 bits) */
        lp2 = lp;
        lp2.push_addr = 1ull << 40;
        CHECK(LAUNCH(&lp2, pb, 128) == PG106_SUB_ERR_RANGE);
        lp2.push_addr = (1ull << 40) - 4u;           /* last dword: fine */
        CHECK(LAUNCH(&lp2, pb, 128) == PG106_SUB_OK);
        lp2.push_addr = 0xB00002ull;                  /* not dword-aligned */
        CHECK(LAUNCH(&lp2, pb, 128) == PG106_SUB_ERR_RANGE);

        /* qmd_addr >> 8 must fit SEND_PCAS_A's 32 bits */
        lp2 = lp;
        lp2.qmd_addr = 1ull << 40;
        CHECK(LAUNCH(&lp2, pb, 128) == PG106_SUB_ERR_RANGE);

        /* Ring must be the RAMFC-described one (gpfifo_len / 8 entries)
         * and put inside it. */
        CHECK(pg106_sub_launch(&lp, pb, 128, ring, 8, 0, &res, NULL) ==
              PG106_SUB_ERR_RANGE);
        CHECK(pg106_sub_launch(&lp, pb, 128, ring, RING_ENTRIES,
                               RING_ENTRIES, &res, NULL) ==
              PG106_SUB_ERR_RANGE);
        CHECK(pg106_sub_launch(&lp, pb, 128, NULL, RING_ENTRIES, 0, &res,
                               NULL) == PG106_SUB_ERR_ARG);

        /* Compute state: SLM per TPC must be 0x8000-aligned
         * (nvk_queue.c:221); program region inside the 49-bit VA. */
        pg106_sub_compute_state cs = {
            .program_region = 0x1000, .local_memory_window = 0xff000000u,
            .shared_memory_window = 0xfe000000u,
            .slm_addr = 0x40000000ull, .slm_per_tpc = 0x8001ull,
        };
        lp2 = lp;
        lp2.init = &cs;
        CHECK(LAUNCH(&lp2, pb, 128) == PG106_SUB_ERR_RANGE);
        cs.slm_per_tpc = 0x8000ull;
        cs.program_region = 1ull << 49;
        CHECK(LAUNCH(&lp2, pb, 128) == PG106_SUB_ERR_RANGE);
#undef LAUNCH
    }

    printf("%s %d checks, %d failures\n", failures ? "FAIL" : "PASS",
           checks, failures);
    return failures ? 1 : 0;
}
