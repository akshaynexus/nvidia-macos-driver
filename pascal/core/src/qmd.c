// SPDX-License-Identifier: MIT
/*
 * pascal/qmd.c - Queue Meta Data v01_07 builder for PASCAL_COMPUTE_B.
 *
 * Field positions are quoted from research/reference/mesa_clc1c0qmd.h
 * (NVK/Mesa, MIT license) as MW(msb:lsb): bit X of the 2048-bit QMD
 * bitstream is the MSB of a field, word X/32, bit-in-word X%32. Derived
 * only from NVIDIA/Mesa headers; no nouveau (GPL) material is used.
 */
#include "pascal/qmd.h"

int pg106_qmd_insert(uint32_t *qmd, unsigned msb, unsigned lsb,
                     uint32_t value)
{
    if (qmd == NULL || msb < lsb || msb >= PG106_QMD_BITS)
        return PG106_QMD_ERR_ARG;
    /* Every v01_07 field sits inside one u32 word (msb/32 == lsb/32);
     * reject anything else rather than guess a spanning layout. */
    if (msb / 32u != lsb / 32u)
        return PG106_QMD_ERR_ARG;

    unsigned width = msb - lsb + 1u;
    unsigned word = msb / 32u;      /* little-endian word selection */
    unsigned off = lsb % 32u;       /* lsb's position inside that word */
    uint32_t mask = (width == 32u) ? 0xFFFFFFFFu
                                   : ((1u << width) - 1u);
    if (width < 32u && value > mask)
        return PG106_QMD_ERR_RANGE;

    qmd[word] = (qmd[word] & ~(mask << off)) | ((value & mask) << off);
    return PG106_QMD_OK;
}

uint32_t pg106_qmd_extract(const uint32_t *qmd, unsigned msb, unsigned lsb)
{
    if (qmd == NULL || msb < lsb || msb >= PG106_QMD_BITS || msb / 32u != lsb / 32u)
        return 0;
    unsigned width = msb - lsb + 1u;
    unsigned word = msb / 32u;
    unsigned off = lsb % 32u;
    uint32_t mask = (width == 32u) ? 0xFFFFFFFFu
                                   : ((1u << width) - 1u);
    return (qmd[word] >> off) & mask;
}

int pg106_qmd_build(const pg106_qmd_params *p, pg106_qmd *q)
{
    if (p == NULL || q == NULL)
        return PG106_QMD_ERR_ARG;

    for (unsigned i = 0; i < PG106_QMD_WORDS; i++)
        q->w[i] = 0;

    int rc;
    /* Identity first: a QMD without v01_07 is not a Pascal QMD. */
    if ((rc = pg106_qmd_insert(q->w, PG106_QMD_QMD_VERSION,
                               PG106_QMD_VERSION_VALUE)) != PG106_QMD_OK ||
        (rc = pg106_qmd_insert(q->w, PG106_QMD_QMD_MAJOR_VERSION,
                               PG106_QMD_MAJOR_VERSION_VALUE)) != PG106_QMD_OK ||
        (rc = pg106_qmd_insert(q->w, PG106_QMD_PROGRAM_OFFSET,
                               p->program_offset)) != PG106_QMD_OK ||
        (rc = pg106_qmd_insert(q->w, PG106_QMD_CTA_RASTER_WIDTH,
                               p->raster_width)) != PG106_QMD_OK ||
        (rc = pg106_qmd_insert(q->w, PG106_QMD_CTA_RASTER_HEIGHT,
                               p->raster_height)) != PG106_QMD_OK ||
        (rc = pg106_qmd_insert(q->w, PG106_QMD_CTA_RASTER_DEPTH,
                               p->raster_depth)) != PG106_QMD_OK ||
        (rc = pg106_qmd_insert(q->w, PG106_QMD_CTA_THREAD_DIMENSION0,
                               p->thread_dim[0])) != PG106_QMD_OK ||
        (rc = pg106_qmd_insert(q->w, PG106_QMD_CTA_THREAD_DIMENSION1,
                               p->thread_dim[1])) != PG106_QMD_OK ||
        (rc = pg106_qmd_insert(q->w, PG106_QMD_CTA_THREAD_DIMENSION2,
                               p->thread_dim[2])) != PG106_QMD_OK ||
        (rc = pg106_qmd_insert(q->w, PG106_QMD_SHARED_MEMORY_SIZE,
                               p->shared_memory_size)) != PG106_QMD_OK ||
        (rc = pg106_qmd_insert(q->w, PG106_QMD_REGISTER_COUNT,
                               p->register_count)) != PG106_QMD_OK ||
        (rc = pg106_qmd_insert(q->w, PG106_QMD_BARRIER_COUNT,
                               p->barrier_count)) != PG106_QMD_OK ||
        (rc = pg106_qmd_insert(q->w, PG106_QMD_SHADER_LOCAL_MEMORY_LOW_SIZE,
                               p->local_mem_low_size)) != PG106_QMD_OK ||
        (rc = pg106_qmd_insert(q->w, PG106_QMD_RELEASE0_ADDRESS_LOWER,
                               (uint32_t)p->release0_addr)) != PG106_QMD_OK ||
        (rc = pg106_qmd_insert(q->w, PG106_QMD_RELEASE0_ADDRESS_UPPER,
                               (uint32_t)(p->release0_addr >> 32))) != PG106_QMD_OK ||
        (rc = pg106_qmd_insert(q->w, PG106_QMD_RELEASE0_PAYLOAD,
                               p->release0_payload)) != PG106_QMD_OK ||
        (rc = pg106_qmd_insert(q->w,
                               PG106_QMD_CONSTANT_BUFFER_ADDR_LOWER(0),
                               (uint32_t)p->cb0_addr)) != PG106_QMD_OK ||
        (rc = pg106_qmd_insert(q->w,
                               PG106_QMD_CONSTANT_BUFFER_ADDR_UPPER(0),
                               (uint32_t)(p->cb0_addr >> 32))) != PG106_QMD_OK ||
        (rc = pg106_qmd_insert(q->w, PG106_QMD_CONSTANT_BUFFER_SIZE(0),
                               p->cb0_size)) != PG106_QMD_OK)
        return rc;

    return PG106_QMD_OK;
}

int pg106_qmd_validate(const pg106_qmd *q)
{
    if (q == NULL)
        return PG106_QMD_ERR_ARG;
    if (pg106_qmd_extract(q->w, PG106_QMD_QMD_VERSION) !=
            PG106_QMD_VERSION_VALUE ||
        pg106_qmd_extract(q->w, PG106_QMD_QMD_MAJOR_VERSION) !=
            PG106_QMD_MAJOR_VERSION_VALUE)
        return PG106_QMD_ERR_VERSION;
    return PG106_QMD_OK;
}
