// SPDX-License-Identifier: MIT
/*
 * pascal/tests/test_ce.c - golden vectors for the GP106 copy-engine
 * method encoders (pascal/core/src/ce.c, NVC1B5 / PASCAL_DMA_COPY_B).
 *
 * Golden values cross-checked against:
 *   - third_party/open-gpu-kernel-modules/src/common/sdk/nvidia/inc/class/
 *     clc1b5.h  (PASCAL_DMA_COPY_B 0xC1B5, NVC1B5_* methods/fields)
 *   - third_party/tinygrad/tinygrad/runtime/ops_nv.py
 *     NVCopyQueue.copy (lines 186-195) and .semaphore (lines 197-201),
 *     with NVC6B5 constants from tinygrad/runtime/autogen/nv_570.py
 *     (AMPERE_DMA_COPY_A 0xC6B5 at line 16929; method addresses equal
 *     to NVC1B5's - UAPI-ORACLE.md line 135)
 *   - third_party/mesa/src/nouveau/vulkan/nvk_cmd_copy.c
 *     nvk_cmd_copy_memory_ce (1D copy method order, P_IMMD LAUNCH_DMA)
 *   - third_party/open-gpu-kernel-modules:src/common/sdk/nvidia/inc/class/clc06f.h (pg106_pb header fields, via pushbuf.h)
 *
 * This is a simulation/host-side test only; it touches no hardware.
 * Build/run via the pascal Makefile, or directly:
 *   cc -std=c11 -Wall -Wextra -I ../core/include test_ce.c ../core/src/ce.c \
 *      ../core/src/pushbuf.c -o /tmp/opencode/test_ce
 */
#include <stdio.h>
#include <string.h>
#include "pascal/ce.h"

static int failures;

#define CHECK(cond) do { \
    if (cond) { printf("PASS %s\n", #cond); } \
    else { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

#define CHECK_WORDS(got, exp, n, label) do {                        \
    int ok_ = 1;                                                    \
    for (size_t i_ = 0; i_ < (n); i_++)                             \
        if ((got)[i_] != (exp)[i_]) { ok_ = 0;                      \
            printf("  %s[%zu] got 0x%08X want 0x%08X\n",            \
                   label, i_, (got)[i_], (exp)[i_]); }              \
    CHECK(ok_);                                                     \
} while (0)

/*
 * Decode a whole method stream into (method address -> data word) pairs
 * so tests can compare per-method payloads even when the emission form
 * (increasing vs immediate-data) differs between producers. Handles
 * increasing and immediate-data headers via pg106_pb_decode; IMMD data
 * is recovered from the COUNT field [28:16] per clc06f.h
 * NVC06F_DMA_IMMD_DATA.
 */
struct mthd_map {
    uint32_t mthd[32];
    uint32_t data[32];
    size_t n;
};

static void map_stream(const uint32_t *w, size_t len, struct mthd_map *m)
{
    size_t i = 0;
    memset(m, 0, sizeof(*m));
    while (i < len) {
        unsigned sec_op, count, subch;
        uint32_t mthd;
        pg106_pb_decode(w[i], &sec_op, &count, &subch, &mthd);
        i++;
        if (sec_op == PG106_SEC_OP_IMMD_DATA_METHOD) {
            m->mthd[m->n] = mthd;
            m->data[m->n] = count;      /* clc06f.h NVC06F_DMA_IMMD_DATA */
            m->n++;
        } else {
            for (unsigned k = 0; k < count && i < len; k++, i++) {
                m->mthd[m->n] = mthd + 4u * k;  /* increasing addresses */
                m->data[m->n] = w[i];
                m->n++;
            }
        }
    }
}

static const uint32_t *map_get_nth(const struct mthd_map *m, uint32_t mthd,
                                   size_t nth)
{
    size_t seen = 0;
    for (size_t i = 0; i < m->n; i++)
        if (m->mthd[i] == mthd && seen++ == nth)
            return &m->data[i];
    return NULL;
}

static const uint32_t *map_get(const struct mthd_map *m, uint32_t mthd)
{
    return map_get_nth(m, mthd, 0);
}

int main(void)
{
    /* ================= 1. 1D copy: hand-derived golden words =========
     * Plan: subch 4, src 0x1_0000_0040, dst 0x2_0000_0000, size 0x1000,
     * PIPELINED + flush + multi_line, no semaphore.
     *
     * Every header word is SEC_OP[31:29] | COUNT[28:16] |
     * SUBCHANNEL[15:13] | METHOD>>2 [11:0] (clc06f.h NVC06F_DMA_*),
     * built by pushbuf.c; the constants below cite clc1b5.h:
     *   SET_OBJECT         0x0000, class PASCAL_DMA_COPY_B 0xC1B5
     *   OFFSET_IN_UPPER    0x400 (+_IN_LOWER 0x404, _OUT_UPPER 0x408,
     *                       _OUT_LOWER 0x40C as one 4-word block)
     *   PITCH_IN 0x410 / PITCH_OUT 0x414
     *   LINE_LENGTH_IN 0x418 / LINE_COUNT 0x41C (count 1)
     *   LAUNCH_DMA 0x300: DATA_TRANSFER_TYPE_PIPELINED 1 [1:0] |
     *     FLUSH_ENABLE_TRUE 1 << 2 | SRC_MEMORY_LAYOUT_PITCH 1 << 7 |
     *     DST_MEMORY_LAYOUT_PITCH 1 << 8 | MULTI_LINE_ENABLE_TRUE << 9
     *     = 0x385, sent as IMMD (SEC_OP 4, value in COUNT[28:16]).
     */
    {
        uint32_t buf[32];
        pg106_pb pb;
        static const uint32_t want[] = {
            0x20018000u,    /* SET_OBJECT hdr: inc,1 word,subch 4,mthd 0x0000 */
            0x0000C1B5u,    /* PASCAL_DMA_COPY_B (clc1b5.h line 33) */
            0x20048100u,    /* OFFSET_IN_UPPER hdr: inc,4 words,subch 4,0x400 */
            0x00000001u,    /* OFFSET_IN_UPPER_UPPER = src >> 32 */
            0x00000040u,    /* OFFSET_IN_LOWER_VALUE = src & 0xFFFFFFFF */
            0x00000002u,    /* OFFSET_OUT_UPPER_UPPER = dst >> 32 */
            0x00000000u,    /* OFFSET_OUT_LOWER_VALUE */
            0x20028104u,    /* PITCH_IN hdr: inc,2 words,subch 4,0x410 */
            0x00001000u,    /* PITCH_IN_VALUE  (== chunk size, LINE_COUNT 1) */
            0x00001000u,    /* PITCH_OUT_VALUE */
            0x20028106u,    /* LINE_LENGTH_IN hdr: inc,2 words,subch 4,0x418 */
            0x00001000u,    /* LINE_LENGTH_IN_VALUE (bytes) */
            0x00000001u,    /* LINE_COUNT_VALUE = 1 (single line) */
            0x838580C0u,    /* LAUNCH_DMA IMMD hdr: SEC_OP 4, data 0x385,
                               subch 4, 0x300 >> 2 = 0xC0 */
        };
        pg106_ce_memcpy_plan plan = {
            .src_addr = 0x100000040ull,
            .dst_addr = 0x200000000ull,
            .size = 0x1000,
            .subch = 4,
            .transfer_type = PG106_CE_XFER_PIPELINED,
            .flush_enable = 1,
            .multi_line = 1,
            .sem = NULL,
        };
        CHECK(pg106_pb_init(&pb, buf, 32) == PG106_PB_OK);
        CHECK(pg106_ce_memcpy(&pb, &plan) == PG106_PB_OK);
        CHECK(pb.len == sizeof(want) / sizeof(want[0]));
        CHECK_WORDS(buf, want, pb.len, "golden-1d");
    }

    /* ================= 2. tinygrad NVCopyQueue.copy equivalence ======
     * tinygrad/runtime/ops_nv.py lines 189-195, for src=0x1000,
     * dst=0x2000, sz=0x400 on its COPY:0 queue (subchannel 4, line 614):
     *
     *   nvm(4, NVC6B5_OFFSET_IN_UPPER, hi(src), lo(src), hi(dst), lo(dst))
     *   nvm(4, NVC6B5_LINE_LENGTH_IN,  sz)
     *   nvm(4, NVC6B5_LAUNCH_DMA, flags(non_pipelined, pitch, pitch))
     *
     * nvm() (ops_nv.py line 43) emits header
     *   (typ<<28) | (count<<16) | (subc<<13) | (mthd>>2)  with typ=2,
     * which aliases SEC_OP=1 (INC_METHOD) - see
     * research/evidence/ENCODING-RESOLUTION.md and test_pushbuf.c §3.
     *
     * CLASS-ID DIFFERENCE (documented, asserted below): tinygrad drives
     * the AMPERE copy class NVC6B5 = AMPERE_DMA_COPY_A = 0xC6B5
     * (nv_570.py line 16929, bound at ops_nv.py line 621), we emit
     * PASCAL_DMA_COPY_B = 0xC1B5. The methods used here are
     * generation-stable (nv_570.py: NVC6B5_OFFSET_IN_UPPER 0x400 at
     * line 17035, NVC6B5_LINE_LENGTH_IN 0x418, NVC6B5_LAUNCH_DMA 0x300
     * at line 16969 - identical to clc1b5.h; see also
     * research/evidence/UAPI-ORACLE.md line 135).
     *
     * Other documented divergences (all deliberate):
     *   - tinygrad never writes PITCH_IN/PITCH_OUT or LINE_COUNT: its
     *     copy channel relies on zero-initialized method RAM
     *     (LINE_COUNT 0) - our encoder always programs them.
     *   - tinygrad sends LAUNCH_DMA as a 1-word INC method; we use the
     *     IMMD form (Mesa nvk_cmd_copy_memory_ce style).
     * So the comparison is per-method payload equality, not raw
     * stream equality.
     */
    {
        static const uint32_t tg_stream[] = {
            /* 0x20048100 = (2<<28)|(4<<16)|(4<<13)|(0x400>>2) - note
             * tinygrad's typ=2<<28 form is bit-identical to our
             * SEC_OP=1 INC_METHOD header (ENCODING-RESOLUTION.md). */
            0x20048100u,
            0x00000000u, 0x00001000u,   /* hi/lo src  (ops_nv.py:190) */
            0x00000000u, 0x00002000u,   /* hi/lo dst */
            /* 0x20018106 = (2<<28)|(1<<16)|(4<<13)|(0x418>>2) */
            0x20018106u,
            0x00000400u,                /* LINE_LENGTH_IN (ops_nv.py:191) */
            /* 0x200180C0 = (2<<28)|(1<<16)|(4<<13)|(0x300>>2) */
            0x200180C0u,
            /* nv_flags non_pipelined(2)|src pitch(1<<7)|dst pitch(1<<8)
             * (nv_570.py NVC6B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NON_PIPELINED
             * = 2, *_SRC_MEMORY_LAYOUT_PITCH = 1<<7, *_DST_... = 1<<8) */
            0x00000182u,
        };
        struct mthd_map tg;
        map_stream(tg_stream, sizeof(tg_stream) / sizeof(tg_stream[0]), &tg);

        /* Sanity: tinygrad's typ<<28 headers decode as INC_METHOD with
         * the expected method/count/subchannel (ENCODING-RESOLUTION.md). */
        unsigned sec_op, count, subch;
        uint32_t mthd;
        pg106_pb_decode(0x20048100u, &sec_op, &count, &subch, &mthd);
        CHECK(sec_op == PG106_SEC_OP_INC_METHOD);
        CHECK(count == 4 && subch == 4 && mthd == 0x400u);
        pg106_pb_decode(0x200180C0u, &sec_op, &count, &subch, &mthd);
        CHECK(sec_op == PG106_SEC_OP_INC_METHOD);
        CHECK(count == 1 && subch == 4 && mthd == 0x300u);

        /* Our equivalent stream: same addresses/size/subchannel, launch
         * flags matched to tinygrad's (NON_PIPELINED, no flush, no
         * multi_line). */
        uint32_t buf[32];
        pg106_pb pb;
        pg106_ce_memcpy_plan plan = {
            .src_addr = 0x1000,
            .dst_addr = 0x2000,
            .size = 0x400,
            .subch = 4,
            .transfer_type = PG106_CE_XFER_NON_PIPELINED,
            .flush_enable = 0,
            .multi_line = 0,
            .sem = NULL,
        };
        pg106_pb_init(&pb, buf, 32);
        CHECK(pg106_ce_memcpy(&pb, &plan) == PG106_PB_OK);

        struct mthd_map us;
        map_stream(buf, pb.len, &us);

        const uint32_t *v;
        /* OFFSET block: 4 payload words must match tinygrad's exactly. */
        v = map_get(&us, PG106_NVC1B5_OFFSET_IN_UPPER);
        CHECK(v != NULL);
        if (v != NULL) {
            const uint32_t *t = map_get(&tg, 0x400u);
            CHECK(t != NULL && memcmp(v, t, 4 * sizeof(uint32_t)) == 0);
            CHECK(v[0] == 0 && v[1] == 0x1000u && v[2] == 0 && v[3] == 0x2000u);
        }
        /* LINE_LENGTH_IN payload matches tinygrad's (0x418, ops_nv.py:191). */
        v = map_get(&us, PG106_NVC1B5_LINE_LENGTH_IN);
        CHECK(v != NULL && v[0] == 0x400u);
        {
            const uint32_t *t = map_get(&tg, 0x418u);
            CHECK(t != NULL && t[0] == 0x400u);
        }
        /* LAUNCH_DMA payload matches tinygrad's 0x182 (0x300). */
        v = map_get(&us, PG106_NVC1B5_LAUNCH_DMA);
        CHECK(v != NULL && v[0] == 0x182u);
        {
            const uint32_t *t = map_get(&tg, 0x300u);
            CHECK(t != NULL && t[0] == 0x182u);
        }
        /* The documented extras we always program: */
        v = map_get(&us, PG106_NVC1B5_PITCH_IN);
        CHECK(v != NULL && v[0] == 0x400u && v[1] == 0x400u);
        v = map_get(&us, PG106_NVC1B5_LINE_COUNT);
        CHECK(v != NULL && v[0] == 1u);
        /* And the class id really is Pascal's, not tinygrad's C6B5. */
        CHECK(buf[1] == 0xC1B5u);
        CHECK(buf[1] != 0xC6B5u);
    }

    /* ================= 3. Semaphore release (NVCopyQueue.semaphore) ==
     * ops_nv.py lines 197-201: SET_SEMAPHORE_A (0x240) + hi/lo + payload
     * as one 4-word... 3-word INC block, then LAUNCH_DMA with
     * flush_enable=true and semaphore_type=release_one_word. Our golden:
     * sem at 0x12_3456_7890, payload 0xDEADBEEF, one-word, rest as §1.
     * Launch word = 0x385 | (RELEASE_ONE_WORD 1 << 3) = 0x38D. */
    {
        uint32_t buf[32];
        pg106_pb pb;
        pg106_ce_semaphore sem = {
            .addr = 0x1234567890ull,
            .payload = 0xDEADBEEFu,
            .four_word = 0,
        };
        pg106_ce_memcpy_plan plan = {
            .src_addr = 0x100000040ull,
            .dst_addr = 0x200000000ull,
            .size = 0x1000,
            .subch = 4,
            .transfer_type = PG106_CE_XFER_PIPELINED,
            .flush_enable = 1,
            .multi_line = 1,
            .sem = &sem,
        };
        pg106_pb_init(&pb, buf, 32);
        CHECK(pg106_ce_memcpy(&pb, &plan) == PG106_PB_OK);
        /* Semaphore block lands after the line geometry (word 13):
         * inc,3 words,subch 4,0x240>>2=0x90 -> (1<<29)|(3<<16)|(4<<13)|0x90. */
        CHECK(buf[13] == 0x20038090u);
        CHECK(buf[14] == 0x12u);            /* SET_SEMAPHORE_A_UPPER 16:0 */
        CHECK(buf[15] == 0x34567890u);      /* SET_SEMAPHORE_B_LOWER 31:0 */
        CHECK(buf[16] == 0xDEADBEEFu);      /* SET_SEMAPHORE_PAYLOAD_PAYLOAD */
        /* Final launch IMMD carries the one-word-release bit. */
        CHECK(buf[17] == 0x838D80C0u);
        /* Four-word release flips SEMAPHORE_TYPE 4:3 to 2 (<<3 = 0x10):
         * 0x385 | 0x10 = 0x395. */
        sem.four_word = 1;
        pg106_pb_init(&pb, buf, 32);
        CHECK(pg106_ce_memcpy(&pb, &plan) == PG106_PB_OK);
        CHECK(buf[17] == 0x839580C0u);
    }

    /* ================= 4. 2D pitch copy golden =======================
     * Mesa nvk_cmd_copy.c pitch-surface path: OFFSET_IN_UPPER block,
     * PITCH_IN (row stride), PITCH_OUT, LINE_LENGTH_IN (extent *
     * bytes/px), LINE_COUNT (extent height), then LAUNCH_DMA with
     * multi_line_enable=TRUE. Golden: 3 lines of 0x400 bytes, src pitch
     * 0x800, dst pitch 0x1000, PIPELINED+flush+multi_line. */
    {
        uint32_t buf[32];
        pg106_pb pb;
        static const uint32_t want[] = {
            0x20018000u,    /* SET_OBJECT, subch 4, class 0xC1B5 */
            0x0000C1B5u,
            0x20048100u,    /* OFFSET_IN_UPPER block */
            0x00000000u, 0x00001000u,   /* src 0x1000 */
            0x00000001u, 0x00000000u,   /* dst 0x1_0000_0000 */
            0x20028104u,    /* PITCH_IN */
            0x00000800u,    /* PITCH_IN_VALUE  (src row stride) */
            0x00001000u,    /* PITCH_OUT_VALUE (dst row stride) */
            0x20028106u,    /* LINE_LENGTH_IN */
            0x00000400u,    /* LINE_LENGTH_IN_VALUE (bytes per line) */
            0x00000003u,    /* LINE_COUNT_VALUE = 3 lines */
            0x838580C0u,    /* LAUNCH_DMA IMMD: 0x385 as in §1 */
        };
        pg106_ce_memcpy2d_plan plan = {
            .src_addr = 0x1000,
            .dst_addr = 0x100000000ull,
            .pitch_in = 0x800,
            .pitch_out = 0x1000,
            .line_length = 0x400,
            .line_count = 3,
            .subch = 4,
            .transfer_type = PG106_CE_XFER_PIPELINED,
            .flush_enable = 1,
            .multi_line = 1,
            .sem = NULL,
        };
        pg106_pb_init(&pb, buf, 32);
        CHECK(pg106_ce_memcpy2d(&pb, &plan) == PG106_PB_OK);
        CHECK(pb.len == sizeof(want) / sizeof(want[0]));
        CHECK_WORDS(buf, want, pb.len, "golden-2d");
    }

    /* ================= 5. Chunking above 1 << 31 =====================
     * tinygrad ops_nv.py line 189: `for off in range(0, sz, step:=
     * (1 << 31))`. size = 0x80001000 -> two chunks: 0x80000000 bytes at
     * the base addresses, then 0x1000 bytes at base + 0x80000000. One
     * SET_OBJECT, one launch per chunk, addresses carried per chunk. */
    {
        uint32_t buf[64];
        pg106_pb pb;
        struct mthd_map m;
        pg106_ce_memcpy_plan plan = {
            .src_addr = 0x1000,
            .dst_addr = 0x2000,
            .size = 0x80001000ull,
            .subch = 4,
            .transfer_type = PG106_CE_XFER_NONE,
            .flush_enable = 0,
            .multi_line = 0,
            .sem = NULL,
        };
        pg106_pb_init(&pb, buf, 64);
        CHECK(pg106_ce_memcpy(&pb, &plan) == PG106_PB_OK);
        /* 2 (SET_OBJECT) + 12 per chunk * 2 = 26 words. */
        CHECK(pb.len == 26);
        map_stream(buf, pb.len, &m);
        CHECK(buf[1] == 0xC1B5u);           /* bound once */
        {
            const uint32_t *v = map_get(&m, PG106_NVC1B5_OFFSET_IN_UPPER);
            CHECK(v != NULL && v[0] == 0 && v[1] == 0x1000u &&
                  v[2] == 0 && v[3] == 0x2000u);
        }
        /* First chunk: length 0x80000000, launch with no flags beyond
         * pitch layouts: NONE(0)|pitch(0x180) = 0x180. */
        {
            const uint32_t *v = map_get(&m, PG106_NVC1B5_LINE_LENGTH_IN);
            CHECK(v != NULL && v[0] == 0x80000000u && v[1] == 1u);
            v = map_get(&m, PG106_NVC1B5_LAUNCH_DMA);
            CHECK(v != NULL && v[0] == 0x180u);
        }
        /* Second chunk: addresses advanced by 1 << 31 (occurrence 1 of
         * each repeated method; occurrence 0 was checked above). */
        {
            const uint32_t *v = map_get_nth(&m, PG106_NVC1B5_OFFSET_IN_UPPER, 1);
            CHECK(v != NULL && v[0] == 0 && v[1] == 0x80001000u &&
                  v[2] == 0 && v[3] == 0x80002000u);
            v = map_get_nth(&m, PG106_NVC1B5_LINE_LENGTH_IN, 1);
            CHECK(v != NULL && v[0] == 0x1000u && v[1] == 1u);
        }
        /* Two launches total: IMMD headers carrying NONE|pitch 0x180,
         * subch 4, 0x300>>2. */
        {
            unsigned launches = 0;
            for (size_t i = 0; i < pb.len; i++)
                if (buf[i] == 0x818080C0u)
                    launches++;
            CHECK(launches == 2);
        }

        /* Same two-chunk copy with a release: the semaphore must be
         * programmed and released exactly once, on the last chunk
         * (tinygrad ops_nv.py:187-197 issues the release after all
         * copy launches). 26 + 4 (SET_SEMAPHORE_A header + 3) = 30. */
        pg106_ce_semaphore sem = { .addr = 0x5000, .payload = 7 };
        plan.sem = &sem;
        pg106_pb_init(&pb, buf, 64);
        CHECK(pg106_ce_memcpy(&pb, &plan) == PG106_PB_OK);
        CHECK(pb.len == 30);
        map_stream(buf, pb.len, &m);
        CHECK(map_get_nth(&m, PG106_NVC1B5_SET_SEMAPHORE_A, 0) != NULL);
        CHECK(map_get_nth(&m, PG106_NVC1B5_SET_SEMAPHORE_A, 1) == NULL);
        {
            /* LAUNCH_DMA 0: no SEMAPHORE_TYPE (4:3 == 0); LAUNCH_DMA 1:
             * RELEASE_ONE_WORD_SEMAPHORE (clc1b5.h value 1 at 4:3). */
            const uint32_t *l0 = map_get_nth(&m, PG106_NVC1B5_LAUNCH_DMA, 0);
            const uint32_t *l1 = map_get_nth(&m, PG106_NVC1B5_LAUNCH_DMA, 1);
            CHECK(l0 != NULL && ((l0[0] >> 3) & 3u) == 0u);
            CHECK(l1 != NULL && ((l1[0] >> 3) & 3u) == 1u);
        }
        /* The SET_SEMAPHORE block sits after the second OFFSET block. */
        {
            size_t i_sem = 0, i_off2 = 0, seen_off = 0;
            for (size_t i = 0; i < pb.len; i++) {
                unsigned so, cnt, sc; uint32_t mt;
                pg106_pb_decode(buf[i], &so, &cnt, &sc, &mt);
                if (so != PG106_SEC_OP_INC_METHOD)
                    continue;
                if (mt == PG106_NVC1B5_OFFSET_IN_UPPER && seen_off++ == 1)
                    i_off2 = i;
                if (mt == PG106_NVC1B5_SET_SEMAPHORE_A)
                    i_sem = i;
                i += cnt;
            }
            CHECK(i_off2 != 0 && i_sem > i_off2);
        }
    }

    /* ================= 6. Error paths ================================ */
    {
        pg106_pb pb;
        uint32_t buf[8];
        pg106_ce_memcpy_plan ok_plan = {
            .src_addr = 0x1000, .dst_addr = 0x2000, .size = 0x10,
            .subch = 4, .transfer_type = PG106_CE_XFER_PIPELINED,
        };
        pg106_ce_memcpy_plan bad = ok_plan;
        pg106_ce_memcpy2d_plan bad2d;

        pg106_pb_init(&pb, buf, 8);
        CHECK(pg106_ce_memcpy(NULL, &ok_plan) == PG106_PB_ERR_ARG);
        CHECK(pg106_ce_memcpy(&pb, NULL) == PG106_PB_ERR_ARG);

        bad = ok_plan; bad.size = 0;
        CHECK(pg106_ce_memcpy(&pb, &bad) == PG106_PB_ERR_ARG);

        bad = ok_plan; bad.subch = 8;   /* clc06f.h SUBCHANNEL 15:13 is 3 bits */
        CHECK(pg106_ce_memcpy(&pb, &bad) == PG106_PB_ERR_RANGE);

        bad = ok_plan; bad.src_addr = (uint64_t)1 << 49; /* UPPER 16:0 overflow */
        CHECK(pg106_ce_memcpy(&pb, &bad) == PG106_PB_ERR_RANGE);

        bad = ok_plan; bad.dst_addr = (uint64_t)1 << 49;
        CHECK(pg106_ce_memcpy(&pb, &bad) == PG106_PB_ERR_RANGE);

        bad = ok_plan; bad.transfer_type = 3;   /* DATA_TRANSFER_TYPE 1:0 max 2 */
        CHECK(pg106_ce_memcpy(&pb, &bad) == PG106_PB_ERR_ARG);

        bad = ok_plan; bad.sem = &(pg106_ce_semaphore){ .addr = (uint64_t)1 << 49 };
        CHECK(pg106_ce_memcpy(&pb, &bad) == PG106_PB_ERR_RANGE);

        /* Capacity: the 14-word §1 stream cannot fit in 4 words; the
         * writer stops after SET_OBJECT (2 words, pb_reserve refuses a
         * method whose data does not fit, pushbuf.c line 77) and the
         * error is PG106_PB_ERR_SPACE (sticky). */
        pg106_pb_init(&pb, buf, 4);
        bad = ok_plan;
        CHECK(pg106_ce_memcpy(&pb, &bad) == PG106_PB_ERR_SPACE);
        CHECK(pb.len == 2);

        memset(&bad2d, 0, sizeof(bad2d));
        CHECK(pg106_ce_memcpy2d(NULL, &bad2d) == PG106_PB_ERR_ARG);
        pg106_pb_init(&pb, buf, 32);    /* fresh writer: errors are sticky */
        CHECK(pg106_ce_memcpy2d(&pb, &bad2d) == PG106_PB_ERR_ARG);
        bad2d = (pg106_ce_memcpy2d_plan){
            .src_addr = 0x1000, .dst_addr = 0x2000,
            .pitch_in = 0x800, .pitch_out = 0x1000,
            .line_length = 0x400, .line_count = 3,
            .subch = 4, .transfer_type = PG106_CE_XFER_PIPELINED,
        };
        CHECK(pg106_ce_memcpy2d(&pb, &bad2d) == PG106_PB_OK);
        bad2d.line_count = 0;
        CHECK(pg106_ce_memcpy2d(&pb, &bad2d) == PG106_PB_ERR_ARG);
    }

    if (failures == 0)
        printf("PASS test_ce: all checks green\n");
    else
        printf("FAIL test_ce: %d check(s) failed\n", failures);
    return failures != 0;
}
