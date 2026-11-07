// SPDX-License-Identifier: GPL-2.0-only
/*
 * pascal/tests/test_pbdma.c - the PBDMA consumption-side model under
 * test (pascal/core pbdma.h/.c): our encoders' output must survive a
 * hardware-faithful walk of GPFIFO + pushbuffer.
 *
 * Fixtures are built with the EXISTING encoders only (pg106_sub_*,
 * pg106_pb_*, pg106_qmd_*, pg106_gpfifo_pack); the decoder is validated
 * against their outputs, never against duplicated encodings.
 *
 * References (semantics the assertions pin):
 *   - third_party/open-gpu-kernel-modules:src/common/sdk/nvidia/inc/class/clc06f.h (NVC06F_DMA_*, NVC06F_GP_ENTRY_*)
 *   - third_party/open-gpu-kernel-modules:src/common/sdk/nvidia/inc/class/clc1c0.h (NVC1C0_SET_OBJECT, LOAD_INLINE_QMD_ *,
 *     SEND_PCAS_A, SEND_SIGNALING_PCAS_B)
 *   - NVIDIA open-gpu-doc manuals/volta/gv100/dev_ram.ref.txt "Host
 *     Pushbuffer Format (FIFO_DMA)" (SEC_OP/TERT table, COUNT=0 no-op,
 *     ENDSEG, data-across-segments)
 *   - NVIDIA open-gpu-doc manuals/volta/gv100/dev_pbdma.ref.txt
 *     (GP_ENTRY control entries, INTR_0 PBENTRY/GPENTRY/PBSEG)
 *   - tinygrad ops_nv.py nvm(): the typ=1 corrupt-word legacy vector
 *     (test_tinygrad_compat.c section 4, ENCODING-RESOLUTION.md)
 *
 * SIMULATION ONLY: packs/unpacks words over a mock VM; no hardware.
 */
#include <stdio.h>
#include <string.h>

#include "pascal/dispatch.h"
#include "pascal/pbdma.h"

static int failures;
static int checks;

/* clc06f.h NVC06F_GP_ENTRY1_OPCODE_ILLEGAL (LENGTH==0 GP control entries). */
#define GP_OPCODE_ILLEGAL 0x1u

#define CHECK(cond) do { \
    checks++; \
    if (cond) { printf("PASS %s\n", #cond); } \
    else { printf("FAIL test_pbdma.c:%d %s\n", __LINE__, #cond); \
           failures++; } \
} while (0)

/* ------------------------------------------------------------------ */
/* Mock VM: a flat dword array; pushbuffer "addresses" are indexes.    */

#define VM_WORDS 8192

typedef struct mock_vm {
    uint32_t *mem;
    uint64_t limit;   /* reads at or beyond this dword index fault */
} mock_vm;

static int vm_read(void *ctx, uint64_t dword_addr, uint32_t *out)
{
    mock_vm *vm = ctx;
    if (dword_addr >= vm->limit)
        return -1; /* fetch fault */
    *out = vm->mem[dword_addr];
    return 0;
}

/* The simtest section-5 QMD parameters (same as test_dispatch.c). */
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

static const pg106_sub_chan chan = {
    .chid       = 42u,
    .runq       = 1u,
    .inst_addr  = 0x0000000010000000ull,
    .userd_addr = 0x0000000020000000ull,
    .gpfifo_off = 0x0000000030000000ull,
    .gpfifo_len = 0x1000u,
    .devm       = 0xFFFu,
    .priv       = false,
};

int main(void)
{
    static uint32_t vm[VM_WORDS];
    mock_vm vmc = { vm, VM_WORDS };
    pg106_pbdma_reader reader = { vm_read, &vmc };

    /* ---- 1. Round-trip: the full dispatch launch -------------------
     * pg106_sub_launch() emits SET_OBJECT -> QMD upload -> SEND_PCAS_A
     * -> SIGNALING_PCAS_B -> ENDSEG + one GPFIFO entry; the PBDMA trace
     * must reconstruct exactly those methods. */
    {
        pg106_qmd qmd;
        CHECK(pg106_qmd_build(&sim_qmd, &qmd) == PG106_QMD_OK);

        const uint64_t push_addr = 64ull; /* VM dword 16 */
        uint32_t pb[128];
        uint64_t ring[1];
        pg106_sub_result res;
        pg106_sub_launch_params lp = {
            .chan = &chan, .subch = 1u, .qmd = &qmd,
            .qmd_addr = 0x0000000000A10000ull, .push_addr = push_addr,
        };
        CHECK(pg106_sub_launch(&lp, pb, 128, ring, 1, 0, &res, NULL) ==
              PG106_SUB_OK);
        CHECK(res.pb_words == 74u);

        memcpy(&vm[16], pb, res.pb_words * sizeof(uint32_t));

        pg106_pbdma_entry entries[16];
        static uint32_t pool[512];
        pg106_pbdma_trace tr;
        CHECK(pg106_pbdma_trace_init(&tr, entries, 16, pool, 512) ==
              PG106_PBDMA_OK);
        CHECK(pg106_pbdma_run(ring, 1, &reader, &tr) == PG106_PBDMA_OK);
        CHECK(tr.error == PG106_PBDMA_OK);

        /* Six trace entries: 5 method programs + ENDSEG. */
        CHECK(tr.len == 6u);

        const pg106_pbdma_entry *e = &tr.entries[0];
        CHECK(e->secop == PG106_SEC_OP_INC_METHOD && e->subch == 1u &&
              e->mthd == 0x0000u && e->count_raw == 1u &&
              e->data_len == 1u && e->flags == 0u);
        CHECK(e->pb_offset == 0u && e->gpfifo_index == 0u);
        CHECK(pool[e->data_offset] == 0x0000C1C0u); /* PASCAL_COMPUTE_B */

        e = &tr.entries[1];
        CHECK(e->secop == PG106_SEC_OP_INC_METHOD && e->subch == 1u &&
              e->mthd == 0x0318u && e->count_raw == 2u &&
              e->data_len == 2u);
        CHECK(pool[e->data_offset + 0] == 0x00000000u);
        CHECK(pool[e->data_offset + 1] == 0x0000A100u); /* qmd_addr >> 8 */

        e = &tr.entries[2];
        CHECK(e->secop == PG106_SEC_OP_NON_INC_METHOD && e->subch == 1u &&
              e->mthd == 0x0320u && e->count_raw == 64u &&
              e->data_len == 64u);
        CHECK(memcmp(&pool[e->data_offset], qmd.w,
                     PG106_QMD_WORDS * sizeof(uint32_t)) == 0);

        e = &tr.entries[3];
        CHECK(e->secop == PG106_SEC_OP_INC_METHOD && e->subch == 1u &&
              e->mthd == PG106_NVC1C0_SEND_PCAS_A && e->count_raw == 1u &&
              e->data_len == 1u);
        CHECK(pool[e->data_offset] == 0x0000A100u);

        e = &tr.entries[4];
        CHECK(e->secop == PG106_SEC_OP_IMMD_DATA_METHOD && e->subch == 1u &&
              e->mthd == 0x02BCu && e->data_len == 1u);
        CHECK(pool[e->data_offset] == 0x3u); /* INVALIDATE|SCHEDULE */

        e = &tr.entries[5];
        CHECK(e->secop == PG106_SEC_OP_END_PB_SEGMENT &&
              (e->flags & PG106_PBDMA_FLAG_ENDSEG) != 0u &&
              e->data_len == 0u);

        /* Pool = 1 + 2 + 64 + 1 + 1 = 69 decoded data words. */
        CHECK(tr.pool_len == 69u);

        /* The ENDSEG stops decode: nothing after word 73 was touched. */
        CHECK(tr.entries[5].pb_offset == 73u);
    }

    /* ---- 2. Round-trip: raw pushbuf encoders, multi-segment + wrap -
     * Two segments built with pg106_pb_* (INC + NONINC/IMMD/ENDSEG),
     * published into a wrapped 8-slot ring (GET=6, PUT=1), decoded via
     * the GET/PUT chase. */
    {
        memset(vm, 0, sizeof(vm));
        uint32_t seg1[8], seg2[8];
        pg106_pb pb1, pb2;
        CHECK(pg106_pb_init(&pb1, seg1, 8) == PG106_PB_OK);
        CHECK(pg106_pb_init(&pb2, seg2, 8) == PG106_PB_OK);

        const uint32_t d3[3] = { 0x11111111u, 0x22222222u, 0x33333333u };
        const uint32_t d1[1] = { 0xCAFEBABEu };
        CHECK(pg106_pb_inc(&pb1, 0u, 0x0400u, d3, 3) == PG106_PB_OK);
        CHECK(pg106_pb_endseg(&pb1) == PG106_PB_OK); /* 5 words */
        CHECK(pg106_pb_noninc(&pb2, 2u, 0x0800u, d1, 1) == PG106_PB_OK);
        CHECK(pg106_pb_immd(&pb2, 2u, 0x0810u, 0x3u) == PG106_PB_OK);
        CHECK(pg106_pb_endseg(&pb2) == PG106_PB_OK); /* 4 words */

        /* seg1 at VM dword 100, seg2 at VM dword 200. */
        memcpy(&vm[100], seg1, 5 * sizeof(uint32_t));
        memcpy(&vm[200], seg2, 4 * sizeof(uint32_t));

        uint64_t ring[8];
        memset(ring, 0, sizeof(ring));
        /* LENGTH is the pushbuffer entry (dword) count, per clc06f.h
         * NVC06F_GP_ENTRY1_LENGTH and nouveau's (size >> 2) << 10. */
        ring[6] = pg106_gpfifo_pack(100u * 4u, 5u, PG106_GP_LEVEL_MAIN,
                                    PG106_GP_SYNC_PROCEED,
                                    PG106_GP_FETCH_UNCONDITIONAL);
        ring[7] = pg106_gpfifo_pack(200u * 4u, 4u, PG106_GP_LEVEL_MAIN,
                                    PG106_GP_SYNC_PROCEED,
                                    PG106_GP_FETCH_UNCONDITIONAL);

        pg106_pbdma_entry entries[8];
        uint32_t pool[32];
        pg106_pbdma_trace tr;
        CHECK(pg106_pbdma_trace_init(&tr, entries, 8, pool, 32) ==
              PG106_PBDMA_OK);
        /* GET=6, PUT=(6+2)%8=0: two entries through the wrap. */
        CHECK(pg106_pbdma_run_getput(ring, 8, 6u, 0u, &reader, &tr) ==
              PG106_PBDMA_OK);
        CHECK(tr.error == PG106_PBDMA_OK);
        CHECK(tr.len == 5u); /* 2 + 1(ENDSEG) + 2 + 1(ENDSEG) */

        CHECK(tr.entries[0].gpfifo_index == 6u &&
              tr.entries[0].secop == PG106_SEC_OP_INC_METHOD &&
              tr.entries[0].mthd == 0x0400u &&
              tr.entries[0].subch == 0u && tr.entries[0].data_len == 3u);
        CHECK(tr.entries[1].flags & PG106_PBDMA_FLAG_ENDSEG);
        CHECK(tr.entries[1].gpfifo_index == 6u);
        CHECK(tr.entries[2].gpfifo_index == 7u &&
              tr.entries[2].secop == PG106_SEC_OP_NON_INC_METHOD &&
              tr.entries[2].mthd == 0x0800u &&
              tr.entries[2].subch == 2u && tr.entries[2].data_len == 1u);
        CHECK(pool[tr.entries[2].data_offset] == 0xCAFEBABEu);
        CHECK(tr.entries[3].secop == PG106_SEC_OP_IMMD_DATA_METHOD &&
              tr.entries[3].mthd == 0x0810u &&
              pool[tr.entries[3].data_offset] == 0x3u);
        CHECK((tr.entries[4].flags & PG106_PBDMA_FLAG_ENDSEG) != 0u &&
              tr.entries[4].gpfifo_index == 7u);
        CHECK(tr.pool_len == 5u);

        /* GET == PUT: idle, no entries. */
        pg106_pbdma_trace tr2;
        pg106_pbdma_entry e2[1];
        pg106_pbdma_trace_init(&tr2, e2, 1, pool, 32);
        CHECK(pg106_pbdma_run_getput(ring, 8, 6u, 6u, &reader, &tr2) ==
              PG106_PBDMA_OK);
        CHECK(tr2.len == 0u);
    }

    /* ---- 3. Legacy forms -------------------------------------------
     * (a) The tinygrad typ=1 corrupt word [TG-master ops_nv.py nvm():
     *     (typ<<28)|(count<<16)|(subc<<13)|(mthd>>2) with typ=1]:
     *     1<<28 lands in SEC_OP=0 (GRP0_USE_TERT) and count=1 sets bit16
     *     = TERT_OP 1. Per the dev_ram.ref SEC_OP/TERT table, secondary
     *     000 + tertiary 01 is SET_SUBDEVICE_MASK - the intended INC
     *     header's SUBCH/ADDRESS bits alias into the mask VALUE field
     *     [15:4]. The trace must flag exactly that (a control entry,
     *     NOT a method), matching test_tinygrad_compat.c section 4. */
    {
        memset(vm, 0, sizeof(vm));
        uint32_t word = (1u << 28) | (1u << 16) | (1u << 13) | 0xADu;
        vm[0] = word;
        /* And a genuine legacy-form method word (TERT=0): the same
         * typ=1 base but with count in COUNT_OLD [28:18]. */
        uint32_t legacy = (1u << 28) | (1u << 18) | (1u << 13) | 0xADu;
        vm[1] = legacy;
        vm[2] = 0x00C0FFEEu; /* its single data word */

        uint64_t ring[1];
        ring[0] = pg106_gpfifo_pack(0u, 3u, PG106_GP_LEVEL_MAIN,
                                    PG106_GP_SYNC_PROCEED,
                                    PG106_GP_FETCH_UNCONDITIONAL);

        pg106_pbdma_entry entries[4];
        uint32_t pool[4];
        pg106_pbdma_trace tr;
        CHECK(pg106_pbdma_trace_init(&tr, entries, 4, pool, 4) ==
              PG106_PBDMA_OK);
        CHECK(pg106_pbdma_run(ring, 1, &reader, &tr) == PG106_PBDMA_OK);
        CHECK(tr.error == PG106_PBDMA_OK);
        CHECK(tr.len == 2u);

        const pg106_pbdma_entry *e = &tr.entries[0];
        /* The typ=1 word: tertiary path, TERT_OP aliasing, no method. */
        CHECK(e->secop == PG106_SEC_OP_GRP0_USE_TERT && e->tert_op == 1u);
        CHECK((e->flags & PG106_PBDMA_FLAG_CONTROL) != 0u);
        CHECK(e->mthd == 1u);                       /* SET_SUBDEVICE_MASK */
        CHECK(e->count_raw == ((word >> 4) & 0xFFFu)); /* VALUE 15:4 */
        CHECK(e->count_raw == 0x20Au); /* subch|addr bits aliased in */
        CHECK(e->data_len == 0u);      /* generated no method at all   */
        /* The modern-header view hardware does NOT take (would-be INC):
         * sec_op 0, count field 0x1001 - pinned as the divergence. */
        unsigned sec_op, count_m;
        uint32_t mthd_m;
        pg106_pb_decode(word, &sec_op, &count_m, NULL, &mthd_m);
        CHECK(sec_op == PG106_SEC_OP_GRP0_USE_TERT && count_m == 0x1001u);

        /* The genuine legacy word decodes via COUNT_OLD/ADDRESS_OLD.
         * Note the legacy address field is word bits 12:2, so the 0xAD
         * literal (tinygrad's mthd>>2 form) decodes to method 0xAC. */
        e = &tr.entries[1];
        CHECK((e->flags & PG106_PBDMA_FLAG_LEGACY_TERT) != 0u);
        CHECK(e->secop == PG106_SEC_OP_GRP0_USE_TERT && e->tert_op == 0u);
        CHECK(e->subch == 1u && e->mthd == 0xACu);
        CHECK(e->count_raw == 1u && e->data_len == 1u);
        CHECK(pool[e->data_offset] == 0x00C0FFEEu);
    }

    /* (b) Legacy non-incrementing form: SEC_OP=2, TERT=0
     * (NVC06F_DMA_OPCODE_NONINC_METHOD), COUNT_OLD/ADDRESS_OLD. */
    {
        memset(vm, 0, sizeof(vm));
        /* Field value = mthd >> 2 placed at word bits 12:2. */
        uint32_t word = (2u << 29) | (3u << 18) | (2u << 13) | 0x0100u;
        vm[0] = word;
        vm[1] = 10u; vm[2] = 20u; vm[3] = 30u;
        uint64_t ring[1];
        ring[0] = pg106_gpfifo_pack(0u, 4u, PG106_GP_LEVEL_MAIN,
                                    PG106_GP_SYNC_PROCEED,
                                    PG106_GP_FETCH_UNCONDITIONAL);
        pg106_pbdma_entry entries[2];
        uint32_t pool[4];
        pg106_pbdma_trace tr;
        pg106_pbdma_trace_init(&tr, entries, 2, pool, 4);
        CHECK(pg106_pbdma_run(ring, 1, &reader, &tr) == PG106_PBDMA_OK);
        CHECK(tr.error == PG106_PBDMA_OK && tr.len == 1u);
        CHECK((tr.entries[0].flags & PG106_PBDMA_FLAG_LEGACY_TERT) != 0u);
        CHECK(tr.entries[0].secop == PG106_SEC_OP_GRP2_USE_TERT &&
              tr.entries[0].tert_op == 0u);
        CHECK(tr.entries[0].mthd == 0x0100u &&
              tr.entries[0].count_raw == 3u && tr.entries[0].data_len == 3u);
        CHECK(pool[0] == 10u && pool[1] == 20u && pool[2] == 30u);
    }

    /* (c) Subdevice-mask control entries + conditional skip: SEC_OP=0,
     * TERT=1 (SET_SUBDEVICE_MASK) with mask 0 => INACTIVE, so a
     * following FETCH_CONDITIONAL segment is not decoded. */
    {
        memset(vm, 0, sizeof(vm));
        vm[0] = (0u << 29) | (1u << 16) | (0u << 4); /* SET mask=0 */
        vm[1] = (1u << 29) | (1u << 16) | (0x0300u >> 2); /* never seen */
        uint64_t ring[2];
        ring[0] = pg106_gpfifo_pack(0u * 4u, 1u, PG106_GP_LEVEL_MAIN,
                                    PG106_GP_SYNC_PROCEED,
                                    PG106_GP_FETCH_UNCONDITIONAL);
        ring[1] = pg106_gpfifo_pack(0u * 4u, 2u, PG106_GP_LEVEL_MAIN,
                                    PG106_GP_SYNC_PROCEED,
                                    PG106_GP_FETCH_CONDITIONAL);
        pg106_pbdma_entry entries[4];
        uint32_t pool[4];
        pg106_pbdma_trace tr;
        pg106_pbdma_trace_init(&tr, entries, 4, pool, 4);
        CHECK(pg106_pbdma_run(ring, 2, &reader, &tr) == PG106_PBDMA_OK);
        CHECK(tr.error == PG106_PBDMA_OK && tr.len == 2u);
        CHECK((tr.entries[0].flags & PG106_PBDMA_FLAG_CONTROL) != 0u &&
              tr.entries[0].mthd == 1u); /* SET */
        CHECK((tr.entries[1].flags & PG106_PBDMA_FLAG_SEG_SKIPPED) != 0u);
    }

    /* (d) COUNT=0 => hardware no-op (dev_ram.ref); ONE_INC decodes. */
    {
        memset(vm, 0, sizeof(vm));
        vm[0] = (1u << 29) | (0u << 16) | (1u << 13) | (0x0500u >> 2);
        vm[1] = (5u << 29) | (2u << 16) | (1u << 13) | (0x0510u >> 2);
        vm[2] = 0xAu; vm[3] = 0xBu;
        vm[4] = (7u << 29); /* ENDSEG */
        uint64_t ring[1];
        ring[0] = pg106_gpfifo_pack(0u, 5u, PG106_GP_LEVEL_MAIN,
                                    PG106_GP_SYNC_PROCEED,
                                    PG106_GP_FETCH_UNCONDITIONAL);
        pg106_pbdma_entry entries[4];
        uint32_t pool[4];
        pg106_pbdma_trace tr;
        pg106_pbdma_trace_init(&tr, entries, 4, pool, 4);
        CHECK(pg106_pbdma_run(ring, 1, &reader, &tr) == PG106_PBDMA_OK);
        CHECK(tr.error == PG106_PBDMA_OK && tr.len == 3u);
        CHECK((tr.entries[0].flags & PG106_PBDMA_FLAG_COUNT0_NOP) != 0u &&
              tr.entries[0].data_len == 0u);
        CHECK(tr.entries[1].secop == PG106_SEC_OP_ONE_INC &&
              tr.entries[1].data_len == 2u &&
              pool[tr.entries[1].data_offset] == 0xAu &&
              pool[tr.entries[1].data_offset + 1] == 0xBu);
        CHECK((tr.entries[2].flags & PG106_PBDMA_FLAG_ENDSEG) != 0u);
    }

    /* ---- 4. Error paths -------------------------------------------- */
    {
        /* (a) SEC_OP 6 (NVC06F_DMA_SEC_OP_RESERVED6): PBENTRY, run stops
         * (dev_pbdma.ref: "the PB entry does not decode properly"). */
        memset(vm, 0, sizeof(vm));
        vm[0] = (1u << 29) | (1u << 16) | (0x0600u >> 2);
        vm[1] = 1u;
        vm[2] = (6u << 29) | 0x1234u;          /* reserved opcode */
        vm[3] = 0xFFFFFFFFu;                   /* must never be reached */
        uint64_t ring[1];
        ring[0] = pg106_gpfifo_pack(0u, 4u, PG106_GP_LEVEL_MAIN,
                                    PG106_GP_SYNC_PROCEED,
                                    PG106_GP_FETCH_UNCONDITIONAL);
        pg106_pbdma_entry entries[4];
        uint32_t pool[4];
        pg106_pbdma_trace tr;
        pg106_pbdma_trace_init(&tr, entries, 4, pool, 4);
        CHECK(pg106_pbdma_run(ring, 1, &reader, &tr) ==
              PG106_PBDMA_ERR_PBENTRY);
        CHECK(tr.error == PG106_PBDMA_ERR_PBENTRY);
        CHECK(tr.error_gpfifo == 0u && tr.error_word == ((6u << 29) | 0x1234u));
        CHECK(tr.len == 1u); /* the INC method; decode stopped there */

        /* (b) Method data overrun: header promises 5 words, the only
         * segment holds 2 => PBDMA stalls with nothing left to fetch. */
        memset(vm, 0, sizeof(vm));
        vm[0] = (1u << 29) | (5u << 16) | (0x0700u >> 2);
        vm[1] = 1u; vm[2] = 2u;
        ring[0] = pg106_gpfifo_pack(0u, 3u, PG106_GP_LEVEL_MAIN,
                                    PG106_GP_SYNC_PROCEED,
                                    PG106_GP_FETCH_UNCONDITIONAL);
        pg106_pbdma_trace tr2;
        pg106_pbdma_entry e2[4];
        uint32_t pool2[4];
        pg106_pbdma_trace_init(&tr2, e2, 4, pool2, 4);
        CHECK(pg106_pbdma_run(ring, 1, &reader, &tr2) ==
              PG106_PBDMA_ERR_OVERRUN);
        CHECK(tr2.entries[0].data_len == 2u);
        CHECK((tr2.entries[0].flags & PG106_PBDMA_FLAG_SEG_CROSS) == 0u);

        /* (c) ...but the SAME short segment with a following GP entry is
         * legal: the data continues in the next segment (dev_ram.ref). */
        memset(vm, 0, sizeof(vm));
        vm[0] = (1u << 29) | (5u << 16) | (0x0700u >> 2);
        vm[1] = 1u; vm[2] = 2u;          /* segment 0: hdr + 2 words */
        vm[10] = 3u; vm[11] = 4u; vm[12] = 5u; /* segment 1: the rest */
        uint64_t ring3[2];
        ring3[0] = pg106_gpfifo_pack(0u, 3u, PG106_GP_LEVEL_MAIN,
                                     PG106_GP_SYNC_PROCEED,
                                     PG106_GP_FETCH_UNCONDITIONAL);
        ring3[1] = pg106_gpfifo_pack(10u * 4u, 3u, PG106_GP_LEVEL_MAIN,
                                     PG106_GP_SYNC_PROCEED,
                                     PG106_GP_FETCH_UNCONDITIONAL);
        pg106_pbdma_trace tr3;
        pg106_pbdma_entry e3[4];
        uint32_t pool3[8];
        pg106_pbdma_trace_init(&tr3, e3, 4, pool3, 8);
        CHECK(pg106_pbdma_run(ring3, 2, &reader, &tr3) == PG106_PBDMA_OK);
        CHECK(tr3.error == PG106_PBDMA_OK);
        CHECK(tr3.len == 1u && tr3.entries[0].data_len == 5u);
        CHECK(tr3.entries[0].flags & PG106_PBDMA_FLAG_SEG_CROSS);
        CHECK(pool3[2] == 3u && pool3[4] == 5u);

        /* (d) PBSEG: pending data crossing into a FETCH_CONDITIONAL
         * segment (dev_pbdma.ref INTR_0_PBSEG); run continues. */
        memset(vm, 0, sizeof(vm));
        vm[0] = (1u << 29) | (2u << 16) | (0x0900u >> 2);
        vm[1] = 1u;                       /* segment 0 ends mid-method */
        vm[10] = 2u;                      /* conditional segment: data */
        uint64_t ring4[2];
        ring4[0] = pg106_gpfifo_pack(0u, 2u, PG106_GP_LEVEL_MAIN,
                                     PG106_GP_SYNC_PROCEED,
                                     PG106_GP_FETCH_UNCONDITIONAL);
        ring4[1] = pg106_gpfifo_pack(10u * 4u, 1u, PG106_GP_LEVEL_MAIN,
                                     PG106_GP_SYNC_PROCEED,
                                     PG106_GP_FETCH_CONDITIONAL);
        pg106_pbdma_trace tr4;
        pg106_pbdma_entry e4[4];
        uint32_t pool4[4];
        pg106_pbdma_trace_init(&tr4, e4, 4, pool4, 4);
        CHECK(pg106_pbdma_run(ring4, 2, &reader, &tr4) == PG106_PBDMA_OK);
        CHECK(tr4.error == PG106_PBDMA_ERR_PBSEG);
        CHECK(tr4.entries[0].data_len == 2u &&
              pool4[tr4.entries[0].data_offset + 1] == 2u);

        /* (e) GP control entry with OPCODE_ILLEGAL => GPENTRY recorded,
         * hardware discards the entry and continues with the next. */
        memset(vm, 0, sizeof(vm));
        vm[0] = (1u << 29) | (1u << 16) | (0x0A00u >> 2);
        vm[1] = 0x5A5A5A5Au;
        uint64_t ring5[2];
        ring5[0] = pg106_gpfifo_pack(0u, 0u, PG106_GP_LEVEL_MAIN,
                                     PG106_GP_SYNC_PROCEED,
                                     PG106_GP_FETCH_UNCONDITIONAL);
        /* LENGTH==0 + OPCODE bits 7:0 = 1 (ILLEGAL): ENTRY1 7:0 are
         * bits 39:32 of the packed entry. */
        ring5[0] |= (uint64_t)(GP_OPCODE_ILLEGAL & 0xFFu) << 32;
        ring5[1] = pg106_gpfifo_pack(0u, 2u, PG106_GP_LEVEL_MAIN,
                                     PG106_GP_SYNC_PROCEED,
                                     PG106_GP_FETCH_UNCONDITIONAL);
        pg106_pbdma_trace tr5;
        pg106_pbdma_entry e5[4];
        uint32_t pool5[4];
        pg106_pbdma_trace_init(&tr5, e5, 4, pool5, 4);
        CHECK(pg106_pbdma_run(ring5, 2, &reader, &tr5) == PG106_PBDMA_OK);
        CHECK(tr5.error == PG106_PBDMA_ERR_GPENTRY &&
              tr5.error_gpfifo == 0u);
        CHECK(tr5.len == 2u); /* GP_CONTROL + the real segment */
        CHECK((tr5.entries[0].flags & PG106_PBDMA_FLAG_GP_CONTROL) != 0u);
        CHECK(tr5.entries[1].mthd == 0x0A00u);

        /* (f) Pushbuffer reader fault => FETCH error. */
        mock_vm small = { vm, 2 }; /* faults at dword >= 2 */
        pg106_pbdma_reader rsmall = { vm_read, &small };
        vm[0] = (1u << 29) | (4u << 16) | (0x0B00u >> 2);
        vm[1] = 1u;
        uint64_t ring6[1];
        ring6[0] = pg106_gpfifo_pack(0u, 6u, PG106_GP_LEVEL_MAIN,
                                     PG106_GP_SYNC_PROCEED,
                                     PG106_GP_FETCH_UNCONDITIONAL);
        pg106_pbdma_trace tr6;
        pg106_pbdma_entry e6[4];
        uint32_t pool6[4];
        pg106_pbdma_trace_init(&tr6, e6, 4, pool6, 4);
        CHECK(pg106_pbdma_run(ring6, 1, &rsmall, &tr6) ==
              PG106_PBDMA_ERR_FETCH);
        CHECK(tr6.error == PG106_PBDMA_ERR_FETCH);

        /* (g) Argument errors. */
        pg106_pbdma_trace tr7;
        pg106_pbdma_entry e7[1];
        pg106_pbdma_trace_init(&tr7, e7, 1, NULL, 0);
        CHECK(pg106_pbdma_run(NULL, 0, &reader, &tr7) ==
              PG106_PBDMA_ERR_ARG);
        CHECK(pg106_pbdma_run(ring6, 1, NULL, &tr7) == PG106_PBDMA_ERR_ARG);
        CHECK(pg106_pbdma_run_getput(ring6, 0, 0, 0, &reader, &tr7) ==
              PG106_PBDMA_ERR_ARG);
        CHECK(pg106_pbdma_run_getput(ring6, 8, 8, 0, &reader, &tr7) ==
              PG106_PBDMA_ERR_ARG);
        CHECK(pg106_pbdma_trace_init(NULL, e7, 1, NULL, 0) ==
              PG106_PBDMA_ERR_ARG);
    }

    /* ---- 5. No-ENDSEG segment end is legal --------------------------
     * dev_ram.ref: END_PB_SEGMENT is optional ("it MAY write a single
     * End-PB-Segment control entry"); a segment that simply runs out of
     * length ends normally. NVK never emits ENDSEG; our dispatch.c does
     * - both must decode. */
    {
        memset(vm, 0, sizeof(vm));
        vm[0] = (1u << 29) | (1u << 16) | (0x0C00u >> 2);
        vm[1] = 0x77u;   /* no ENDSEG: segment just ends */
        uint64_t ring[1];
        ring[0] = pg106_gpfifo_pack(0u, 2u, PG106_GP_LEVEL_MAIN,
                                    PG106_GP_SYNC_PROCEED,
                                    PG106_GP_FETCH_UNCONDITIONAL);
        pg106_pbdma_entry entries[2];
        uint32_t pool[2];
        pg106_pbdma_trace tr;
        pg106_pbdma_trace_init(&tr, entries, 2, pool, 2);
        CHECK(pg106_pbdma_run(ring, 1, &reader, &tr) == PG106_PBDMA_OK);
        CHECK(tr.error == PG106_PBDMA_OK && tr.len == 1u);
        CHECK(tr.entries[0].data_len == 1u &&
              pool[tr.entries[0].data_offset] == 0x77u);
    }

    /* ---- 6. METHOD_ADDRESS is 11:0 (clc06f.h:233), not 12:0 --------
     * Header bit 12 is not part of the address: a header carrying
     * 0x1FFF in its low 13 bits addresses method 0x3FFC, never 0x7FFC. */
    {
        memset(vm, 0, sizeof(vm));
        vm[0] = (1u << 29) | (1u << 16) | 0x1FFFu;
        vm[1] = 0x55u;
        uint64_t ring[1];
        ring[0] = pg106_gpfifo_pack(0u, 2u, PG106_GP_LEVEL_MAIN,
                                    PG106_GP_SYNC_PROCEED,
                                    PG106_GP_FETCH_UNCONDITIONAL);
        pg106_pbdma_entry entries[2];
        uint32_t pool[2];
        pg106_pbdma_trace tr;
        pg106_pbdma_trace_init(&tr, entries, 2, pool, 2);
        CHECK(pg106_pbdma_run(ring, 1, &reader, &tr) == PG106_PBDMA_OK);
        CHECK(tr.len == 1u && tr.entries[0].mthd == 0x3FFCu);
        /* An INC of 2 starting at 0x3FFC wraps the 12-bit address:
         * dev_pbdma.ref INTR_0_PBENTRY address-wrap condition. */
        vm[0] = (1u << 29) | (2u << 16) | 0x0FFFu;
        vm[2] = 0x66u;
        ring[0] = pg106_gpfifo_pack(0u, 3u, PG106_GP_LEVEL_MAIN,
                                    PG106_GP_SYNC_PROCEED,
                                    PG106_GP_FETCH_UNCONDITIONAL);
        pg106_pbdma_trace_init(&tr, entries, 2, pool, 2);
        pg106_pbdma_run(ring, 1, &reader, &tr);
        CHECK(tr.error == PG106_PBDMA_ERR_PBENTRY);
    }

    printf("%s %d checks, %d failures\n", failures ? "FAIL" : "PASS",
           checks, failures);
    return failures ? 1 : 0;
}
