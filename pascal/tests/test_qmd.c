// SPDX-License-Identifier: MIT
/*
 * pascal/tests/test_qmd.c - golden vectors for the QMD v01_07 builder
 * (pascal/core).
 *
 * All field values and raw word expectations mirror
 * helios300-gp106-simtest.py section 5; bit positions are the
 * MW(msb:lsb) ranges from research/reference/mesa_clc1c0qmd.h
 * (NVC1C0_QMDV01_07_*). Simulation only, no hardware.
 *
 * Direct build/run:
 *   cc -std=c11 -Wall -Wextra -I ../core/include test_qmd.c ../core/src/qmd.c -o /tmp/opencode/test_qmd
 */
#include <stdio.h>
#include "pascal/qmd.h"

static int failures;

#define CHECK(cond) do { \
    if (cond) { printf("PASS %s\n", #cond); } \
    else { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

/* The exact parameter set from helios300-gp106-simtest.py section 5. */
static pg106_qmd_params simtest_params(void)
{
    pg106_qmd_params p = {0};
    p.program_offset     = 0x00A00000u;  /* shader code offset >> 8 */
    p.raster_width       = 129u;         /* grid X */
    p.raster_height      = 64u;          /* grid Y */
    p.raster_depth       = 1u;           /* grid Z */
    p.thread_dim[0]      = 8u;           /* block X */
    p.thread_dim[1]      = 8u;           /* block Y */
    p.thread_dim[2]      = 1u;           /* block Z */
    p.shared_memory_size = 1024u;        /* bytes per block */
    p.register_count     = 168u;         /* per thread */
    p.barrier_count      = 1u;
    p.local_mem_low_size = 32u;          /* bytes per thread */
    p.release0_addr      = 0x00000000DEAD0000ull;
    p.release0_payload   = 42u;
    p.cb0_addr           = 0x12345678ull;
    p.cb0_size           = 256u;
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
        CHECK(pg106_qmd_extract(scratch, 31, 4) == 0xDEADBEEu);  /* bits 31:4, LSB-aligned */
        CHECK(pg106_qmd_extract(scratch, 3, 0) == 0x5u);
        /* value wider than the field -> ERR_RANGE */
        CHECK(pg106_qmd_insert(scratch, 3, 0, 0x10u) == PG106_QMD_ERR_RANGE);
        /* 32-bit field takes any value */
        CHECK(pg106_qmd_insert(scratch, 575, 544, 0xFFFFFFFFu)
              == PG106_QMD_OK);
        /* bad arguments: NULL, reversed, out of image, crossing words */
        CHECK(pg106_qmd_insert(NULL, 31, 0, 1u) == PG106_QMD_ERR_ARG);
        CHECK(pg106_qmd_insert(scratch, 4, 8, 1u) == PG106_QMD_ERR_ARG);
        CHECK(pg106_qmd_insert(scratch, PG106_QMD_BITS, 2044, 1u)
              == PG106_QMD_ERR_ARG);
        CHECK(pg106_qmd_insert(scratch, 32, 31, 1u) == PG106_QMD_ERR_ARG);
        CHECK(pg106_qmd_extract(NULL, 31, 0) == 0u);
        CHECK(pg106_qmd_extract(scratch, 0, 0) == (0x5u & 1u)); /* 1-bit field 0:0 */
    }

    /* ---- 2. simtest golden: build, extract every field, raw words --- */
    {
        pg106_qmd_params p = simtest_params();
        pg106_qmd q;
        CHECK(pg106_qmd_build(&p, &q) == PG106_QMD_OK);
        CHECK(pg106_qmd_validate(&q) == PG106_QMD_OK);

        /* Field round-trips, each via its NVC1C0_QMDV01_07_* MW range. */
        CHECK(pg106_qmd_extract(q.w, PG106_QMD_QMD_VERSION) == 7u);
        CHECK(pg106_qmd_extract(q.w, PG106_QMD_QMD_MAJOR_VERSION) == 1u);
        CHECK(pg106_qmd_extract(q.w, PG106_QMD_PROGRAM_OFFSET) == 0x00A00000u);
        CHECK(pg106_qmd_extract(q.w, PG106_QMD_CTA_RASTER_WIDTH) == 129u);
        CHECK(pg106_qmd_extract(q.w, PG106_QMD_CTA_RASTER_HEIGHT) == 64u);
        CHECK(pg106_qmd_extract(q.w, PG106_QMD_CTA_RASTER_DEPTH) == 1u);
        CHECK(pg106_qmd_extract(q.w, PG106_QMD_CTA_THREAD_DIMENSION0) == 8u);
        CHECK(pg106_qmd_extract(q.w, PG106_QMD_CTA_THREAD_DIMENSION1) == 8u);
        CHECK(pg106_qmd_extract(q.w, PG106_QMD_CTA_THREAD_DIMENSION2) == 1u);
        CHECK(pg106_qmd_extract(q.w, PG106_QMD_SHARED_MEMORY_SIZE) == 1024u);
        CHECK(pg106_qmd_extract(q.w, PG106_QMD_REGISTER_COUNT) == 168u);
        CHECK(pg106_qmd_extract(q.w, PG106_QMD_BARRIER_COUNT) == 1u);
        CHECK(pg106_qmd_extract(q.w, PG106_QMD_SHADER_LOCAL_MEMORY_LOW_SIZE)
              == 32u);
        CHECK(pg106_qmd_extract(q.w, PG106_QMD_RELEASE0_ADDRESS_LOWER)
              == 0xDEAD0000u);
        CHECK(pg106_qmd_extract(q.w, PG106_QMD_RELEASE0_ADDRESS_UPPER) == 0u);
        CHECK(pg106_qmd_extract(q.w, PG106_QMD_RELEASE0_PAYLOAD) == 42u);
        CHECK(pg106_qmd_extract(q.w,
              PG106_QMD_CONSTANT_BUFFER_ADDR_LOWER(0)) == 0x12345678u);
        CHECK(pg106_qmd_extract(q.w,
              PG106_QMD_CONSTANT_BUFFER_ADDR_UPPER(0)) == 0u);
        CHECK(pg106_qmd_extract(q.w, PG106_QMD_CONSTANT_BUFFER_SIZE(0))
              == 256u);

        /* Raw-word goldens straight from the simtest: word 18 packs
         * CTA_THREAD_DIMENSION0 MW(607:592), QMD_MAJOR_VERSION MW(583:580)
         * and QMD_VERSION MW(579:576); word 12 is CTA_RASTER_WIDTH
         * MW(415:384) alone; word 29 is CONSTANT_BUFFER0_ADDR_LOWER
         * MW(959:928); word 30 packs CB0 size MW(991:975) above the
         * (zero) addr upper MW(967:960); word 45 packs BARRIER_COUNT
         * MW(1471:1467) above SHADER_LOCAL_MEMORY_LOW_SIZE MW(1463:1440);
         * word 46 is REGISTER_COUNT MW(1503:1496) in its top byte. */
        CHECK(q.w[18] == ((8u << 16) | (1u << 4) | 7u));
        CHECK(q.w[12] == 129u);
        CHECK(q.w[29] == 0x12345678u);
        CHECK(q.w[30] == (256u << 15));
        CHECK(q.w[45] == ((1u << 27) | 32u));
        CHECK(q.w[46] == (168u << 24));
        CHECK(q.w[8] == 0x00A00000u);      /* PROGRAM_OFFSET MW(287:256) */
        CHECK(q.w[13] == ((1u << 16) | 64u)); /* DEPTH MW(447:432) | HEIGHT MW(431:416) */
        CHECK(q.w[14] == 0u);              /* MW(479:448) RESUME, unused */
        CHECK(q.w[17] == 1024u);           /* SHARED_MEMORY_SIZE MW(561:544) */
        CHECK(q.w[19] == ((1u << 16) | 8u)); /* DIM2 MW(639:624) | DIM1 MW(623:608) */
        CHECK(q.w[23] == 0xDEAD0000u);     /* RELEASE0_ADDRESS_LOWER MW(767:736) */
        CHECK(q.w[25] == 42u);             /* RELEASE0_PAYLOAD MW(831:800) */

        /* Untouched words stay zero (spot-check the reserved spans). */
        CHECK(q.w[0] == 0u && q.w[31] == 0u && q.w[63] == 0u);
    }

    /* ---- 3. max-width values survive build+validate ------------------ */
    {
        pg106_qmd_params p = simtest_params();
        p.raster_height = 0xFFFFu;       /* MW(431:416) is 16 bits */
        p.shared_memory_size = 0x3FFFFu; /* MW(561:544) is 18 bits */
        p.register_count = 0xFFu;        /* MW(1503:1496) is 8 bits */
        p.barrier_count = 0x1Fu;         /* MW(1471:1467) is 5 bits */
        p.cb0_size = 0x1FFFFu;           /* MW(991:975) is 17 bits */
        pg106_qmd q;
        CHECK(pg106_qmd_build(&p, &q) == PG106_QMD_OK);
        CHECK(pg106_qmd_extract(q.w, PG106_QMD_CTA_RASTER_HEIGHT) == 0xFFFFu);
        CHECK(pg106_qmd_extract(q.w, PG106_QMD_SHARED_MEMORY_SIZE) == 0x3FFFFu);
        CHECK(pg106_qmd_extract(q.w, PG106_QMD_REGISTER_COUNT) == 0xFFu);
        CHECK(pg106_qmd_extract(q.w, PG106_QMD_BARRIER_COUNT) == 0x1Fu);
        CHECK(pg106_qmd_extract(q.w, PG106_QMD_CONSTANT_BUFFER_SIZE(0))
              == 0x1FFFFu);
    }

    /* ---- 4. build range errors --------------------------------------- */
    {
        pg106_qmd_params p = simtest_params();
        pg106_qmd q;
        p.raster_height = 0x10000u;      /* one bit too wide for MW(431:416) */
        CHECK(pg106_qmd_build(&p, &q) == PG106_QMD_ERR_RANGE);
        p = simtest_params();
        p.barrier_count = 0x20u;         /* MW(1471:1467) is 5 bits */
        CHECK(pg106_qmd_build(&p, &q) == PG106_QMD_ERR_RANGE);
        p = simtest_params();
        p.cb0_size = 0x20000u;           /* MW(991:975) is 17 bits */
        CHECK(pg106_qmd_build(&p, &q) == PG106_QMD_ERR_RANGE);
        CHECK(pg106_qmd_build(NULL, &q) == PG106_QMD_ERR_ARG);
        CHECK(pg106_qmd_build(&p, NULL) == PG106_QMD_ERR_ARG);
    }

    /* ---- 5. validate rejects non-v01_07 identities ------------------- */
    {
        pg106_qmd_params p = simtest_params();
        pg106_qmd q;
        pg106_qmd_build(&p, &q);

        uint32_t version = pg106_qmd_extract(q.w, PG106_QMD_QMD_VERSION);
        CHECK(version == PG106_QMD_VERSION_VALUE);   /* Pascal requires 7 */
        uint32_t major = pg106_qmd_extract(q.w, PG106_QMD_QMD_MAJOR_VERSION);
        CHECK(major == PG106_QMD_MAJOR_VERSION_VALUE);

        q.w[18] &= ~0xFu;                /* corrupt QMD_VERSION -> not 7 */
        CHECK(pg106_qmd_validate(&q) == PG106_QMD_ERR_VERSION);
        pg106_qmd_build(&p, &q);
        q.w[18] &= ~(0xFu << 4);         /* corrupt QMD_MAJOR_VERSION */
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
