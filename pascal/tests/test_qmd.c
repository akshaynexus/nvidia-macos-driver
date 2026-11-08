// SPDX-License-Identifier: MIT
/*
 * pascal/tests/test_qmd.c - golden vectors for the QMD v02_01 builder.
 *
 * Raw-word expectations are derived by hand from the NVC1C0_QMDV02_01_*
 * MW(msb:lsb) positions in
 * third_party/mesa:src/nouveau/headers/nvidia/classes/clc1c0qmd.h
 * (word = bit / 32, shift = bit % 32), the same goldens
 * helios300-gp106-simtest.py section 5 asserts. Which fields a Pascal
 * QMD must carry comes from NAK Qmd2_1 (nak/qmd.rs:510-548) and gallium
 * gp100_compute_setup_launch_desc (nvc0/nve4_compute.c:637-675).
 * Simulation only, no hardware.
 */
#include <stdio.h>
#include "pascal/qmd.h"

static int failures;

#define CHECK(cond) do { \
    if (cond) { printf("PASS %s\n", #cond); } \
    else { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

/* helios300-gp106-simtest.py section 5 launch. */
static pg106_qmd_params simtest_params(void)
{
    pg106_qmd_params p = {0};
    p.program_offset     = 0x00A00000u;
    p.raster_width       = 129u;
    p.raster_height      = 64u;
    p.raster_depth       = 1u;
    p.thread_dim[0]      = 8u;
    p.thread_dim[1]      = 8u;
    p.thread_dim[2]      = 1u;
    p.shared_memory_size = 1024u;
    p.register_count     = 168u;
    p.barrier_count      = 1u;
    p.local_mem_low_size = 32u;
    p.crs_size           = 0x800u;     /* gallium nve4_compute.c:659 */
    p.release0_addr      = 0xDEAD0000ull;
    p.release0_payload   = 42u;
    p.cbuf[0].addr       = 0x12345600ull;
    p.cbuf[0].size       = 256u;
    return p;
}

int main(void)
{
    /* ---- 1. MW(msb,lsb) helper round-trip and range checks ---------- */
    {
        uint32_t scratch[PG106_QMD_WORDS] = {0};
        CHECK(pg106_qmd_insert(scratch, 31, 0, 0xDEADBEEFu) == PG106_QMD_OK);
        CHECK(pg106_qmd_extract(scratch, 31, 0) == 0xDEADBEEFu);
        CHECK(pg106_qmd_insert(scratch, 3, 0, 0x5u) == PG106_QMD_OK);
        CHECK(pg106_qmd_extract(scratch, 31, 4) == 0xDEADBEEu);
        CHECK(pg106_qmd_insert(scratch, 3, 0, 0x10u) == PG106_QMD_ERR_RANGE);
        CHECK(pg106_qmd_insert(NULL, 31, 0, 1u) == PG106_QMD_ERR_ARG);
        CHECK(pg106_qmd_insert(scratch, 4, 8, 1u) == PG106_QMD_ERR_ARG);
        CHECK(pg106_qmd_insert(scratch, PG106_QMD_BITS, 2044, 1u)
              == PG106_QMD_ERR_ARG);
        CHECK(pg106_qmd_insert(scratch, 32, 31, 1u) == PG106_QMD_ERR_ARG);
        CHECK(pg106_qmd_extract(NULL, 31, 0) == 0u);
    }

    /* ---- 2. simtest golden: raw words from clc1c0qmd.h positions ---- */
    {
        pg106_qmd_params p = simtest_params();
        pg106_qmd q;
        CHECK(pg106_qmd_build(&p, &q) == PG106_QMD_OK);
        CHECK(pg106_qmd_validate(&q) == PG106_QMD_OK);

        /* word 4: SEMAPHORE_RELEASE_ENABLE0 MW(138) | SM_GLOBAL_CACHING_ENABLE MW(134) */
        CHECK(q.w[4] == ((1u << 10) | (1u << 6)));
        /* word 8: PROGRAM_OFFSET MW(287:256) */
        CHECK(q.w[8] == 0x00A00000u);
        /* word 11: API_VISIBLE_CALL_LIMIT NO_CHECK MW(378), CWD_MEMBAR_TYPE
         * L1_SYSMEMBAR MW(369:368), RELEASE_MEMBAR_TYPE FE_SYSMEMBAR MW(366),
         * SAMPLER_INDEX INDEPENDENTLY (0) MW(382) */
        CHECK(q.w[11] == ((1u << 26) | (1u << 16) | (1u << 14)));
        CHECK(q.w[12] == 129u);                    /* CTA_RASTER_WIDTH MW(415:384) */
        CHECK(q.w[13] == 64u);                     /* HEIGHT MW(431:416); 447:432 reserved */
        CHECK(q.w[14] == 1u);                      /* CTA_RASTER_DEPTH MW(463:448) */
        CHECK(q.w[17] == 1024u);                   /* SHARED_MEMORY_SIZE MW(561:544) */
        /* word 18: CTA_THREAD_DIMENSION0 MW(607:592) | MAJOR MW(583:580)=2 | VERSION MW(579:576)=1 */
        CHECK(q.w[18] == ((8u << 16) | (2u << 4) | 1u));
        CHECK(q.w[19] == ((1u << 16) | 8u));       /* DIM2 MW(639:624) | DIM1 MW(623:608) */
        CHECK(q.w[20] == 1u);                      /* CONSTANT_BUFFER_VALID(0) MW(640) */
        CHECK(q.w[23] == 0xDEAD0000u);             /* RELEASE0_ADDRESS_LOWER MW(767:736) */
        CHECK(q.w[24] == (1u << 31));              /* STRUCTURE_SIZE ONE_WORD MW(799), UPPER 0 */
        CHECK(q.w[25] == 42u);                     /* RELEASE0_PAYLOAD MW(831:800) */
        /* word 29: BARRIER_COUNT MW(959:955) | SHADER_LOCAL_MEMORY_LOW_SIZE MW(951:928) */
        CHECK(q.w[29] == ((1u << 27) | 32u));
        CHECK(q.w[30] == (168u << 24));            /* REGISTER_COUNT MW(991:984), SLM high 0 */
        CHECK(q.w[31] == 0x800u);                  /* CRS MW(1015:992); SASS_VERSION MW(1023:1016) 0 */
        CHECK(q.w[32] == 0x12345600u);             /* CB0 ADDR_LOWER MW(1055:1024) */
        CHECK(q.w[33] == (16u << 19));             /* CB0 SIZE_SHIFTED4 MW(1087:1075) = 256/16 */

        /* Nothing else is set: words 0-3, 5-7, 9-10, 15-16, 21-22,
         * 26-28 and 34-63 stay zero. */
        static const unsigned set_words[] = { 4, 8, 11, 12, 13, 14, 17, 18,
                                              19, 20, 23, 24, 25, 29, 30,
                                              31, 32, 33 };
        unsigned stray = 0;
        for (unsigned i = 0; i < PG106_QMD_WORDS; i++) {
            int expected = 0;
            for (unsigned k = 0; k < sizeof(set_words) / sizeof(set_words[0]); k++)
                if (set_words[k] == i)
                    expected = 1;
            if (!expected && q.w[i] != 0)
                stray++;
        }
        CHECK(stray == 0u);
    }

    /* ---- 3. no release requested: ENABLE0 and RELEASE0_* stay clear - */
    {
        pg106_qmd_params p = simtest_params();
        pg106_qmd q;
        p.release0_addr = 0;
        CHECK(pg106_qmd_build(&p, &q) == PG106_QMD_OK);
        CHECK(pg106_qmd_extract(q.w, PG106_QMD_SEMAPHORE_RELEASE_ENABLE0) == 0u);
        CHECK(q.w[23] == 0u && q.w[24] == 0u && q.w[25] == 0u);
    }

    /* ---- 4. constant buffer slot 7 and a 49-bit address ------------- */
    {
        pg106_qmd_params p = simtest_params();
        pg106_qmd q;
        p.cbuf[7].addr = 0x1ABCD12345600ull;   /* bit 48 set: needs the 17-bit UPPER */
        p.cbuf[7].size = 0x10000u;             /* 64 KiB -> SHIFTED4 0x1000 */
        CHECK(pg106_qmd_build(&p, &q) == PG106_QMD_OK);
        /* slot i base bit 1024 + i*64: slot 7 is words 46/47.
         * ADDR_LOWER(7) MW(1503:1472) = 0x12345600, ADDR_UPPER(7)
         * MW(1520:1504) = 0x1ABCD, SIZE_SHIFTED4(7) MW(1535:1523) = 0x1000. */
        CHECK(q.w[46] == 0x12345600u);
        CHECK(q.w[47] == ((0x1000u << 19) | 0x1ABCDu));
        CHECK(q.w[20] == ((1u << 7) | 1u));    /* VALID(7) MW(647) + VALID(0) */
    }

    /* ---- 5. range and alignment errors ------------------------------ */
    {
        pg106_qmd_params p;
        pg106_qmd q;
        p = simtest_params(); p.raster_height = 0x10000u;      /* MW(431:416) 16 bits */
        CHECK(pg106_qmd_build(&p, &q) == PG106_QMD_ERR_RANGE);
        p = simtest_params(); p.raster_depth = 0x10000u;       /* MW(463:448) 16 bits */
        CHECK(pg106_qmd_build(&p, &q) == PG106_QMD_ERR_RANGE);
        p = simtest_params(); p.barrier_count = 0x20u;         /* MW(959:955) 5 bits */
        CHECK(pg106_qmd_build(&p, &q) == PG106_QMD_ERR_RANGE);
        p = simtest_params(); p.shared_memory_size = 1000u;    /* not 0x100-aligned */
        CHECK(pg106_qmd_build(&p, &q) == PG106_QMD_ERR_RANGE);
        p = simtest_params(); p.local_mem_low_size = 33u;      /* not 16-aligned */
        CHECK(pg106_qmd_build(&p, &q) == PG106_QMD_ERR_RANGE);
        p = simtest_params(); p.cbuf[0].size = 100u;           /* not 16-aligned */
        CHECK(pg106_qmd_build(&p, &q) == PG106_QMD_ERR_RANGE);
        p = simtest_params(); p.cbuf[0].size = 0x20000u;       /* SHIFTED4 is 13 bits */
        CHECK(pg106_qmd_build(&p, &q) == PG106_QMD_ERR_RANGE);
        p = simtest_params(); p.cbuf[0].addr = 1ull << 49;     /* UPPER is 17 bits */
        CHECK(pg106_qmd_build(&p, &q) == PG106_QMD_ERR_RANGE);
        p = simtest_params(); p.release0_addr = 1ull << 40;    /* UPPER is 8 bits */
        CHECK(pg106_qmd_build(&p, &q) == PG106_QMD_ERR_RANGE);
        CHECK(pg106_qmd_build(NULL, &q) == PG106_QMD_ERR_ARG);
        CHECK(pg106_qmd_build(&p, NULL) == PG106_QMD_ERR_ARG);
    }

    /* ---- 6. validate rejects non-v02_01 identities (incl. v01_07) --- */
    {
        pg106_qmd_params p = simtest_params();
        pg106_qmd q;
        pg106_qmd_build(&p, &q);
        q.w[18] = (q.w[18] & ~0xFFu) | (1u << 4) | 7u;  /* v01_07 identity */
        CHECK(pg106_qmd_validate(&q) == PG106_QMD_ERR_VERSION);
        pg106_qmd_build(&p, &q);
        q.w[18] &= ~(0xFu << 4);                        /* major 0 */
        CHECK(pg106_qmd_validate(&q) == PG106_QMD_ERR_VERSION);
        CHECK(pg106_qmd_validate(NULL) == PG106_QMD_ERR_ARG);
    }

    if (failures == 0) {
        printf("PASS test_qmd: all checks green\n");
        return 0;
    }
    printf("FAIL test_qmd: %d check(s) failed\n", failures);
    return 1;
}
