// SPDX-License-Identifier: MIT
/*
 * pascal/qmd.c - Queue Meta Data v02_01 builder for PASCAL_COMPUTE_B.
 *
 * Field positions: NVC1C0_QMDV02_01_* in
 * third_party/mesa:src/nouveau/headers/nvidia/classes/clc1c0qmd.h, as
 * MW(msb:lsb) (bit X of the 2048-bit QMD is word X/32, bit X%32). Fill
 * order and defaults follow NAK Qmd2_1 (nak/qmd.rs:510-548, fill_qmd
 * :845-882) and gallium gp100_compute_setup_launch_desc
 * (nvc0/nve4_compute.c:637-675). MIT sources only; no nouveau material.
 */
#include "pascal/qmd.h"

int pg106_qmd_insert(uint32_t *qmd, unsigned msb, unsigned lsb,
                     uint32_t value)
{
    if (qmd == NULL || msb < lsb || msb >= PG106_QMD_BITS)
        return PG106_QMD_ERR_ARG;
    /* Every field used here sits inside one u32 word (msb/32 ==
     * lsb/32); reject anything else rather than guess a spanning layout. */
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

/* Insert, remembering the first failure. */
#define QMD_SET(q, rc, field, v)                                        \
    do {                                                                \
        if ((rc) == PG106_QMD_OK)                                       \
            (rc) = pg106_qmd_insert((q)->w, field, (v));                \
    } while (0)

int pg106_qmd_build(const pg106_qmd_params *p, pg106_qmd *q)
{
    if (p == NULL || q == NULL)
        return PG106_QMD_ERR_ARG;

    for (unsigned i = 0; i < PG106_QMD_WORDS; i++)
        q->w[i] = 0;

    /* Alignment the producers guarantee before packing: shared memory
     * is rounded to 0x100 (gallium nve4_compute.c:656, NAK set_smem_size
     * next_multiple_of(0x100)), SLM per thread to 0x10 (gallium :657
     * hdr[1] & 0xfffff0, NAK set_slm_size). Reject rather than round so
     * a caller bug is visible. */
    if ((p->shared_memory_size & 0xFFu) != 0 ||
        (p->local_mem_low_size & 0xFu) != 0)
        return PG106_QMD_ERR_RANGE;
    /* RELEASE0_ADDRESS_UPPER is 8 bits (775:768): 40-bit address. */
    if (p->release0_addr >> 40)
        return PG106_QMD_ERR_RANGE;

    int rc = PG106_QMD_OK;

    /* NAK qmd_init!(QMDV02_01, 2, 1) (qmd.rs:78-86, :518) + :519. */
    QMD_SET(q, rc, PG106_QMD_QMD_MAJOR_VERSION, PG106_QMD_MAJOR_VERSION_VALUE);
    QMD_SET(q, rc, PG106_QMD_QMD_VERSION, PG106_QMD_VERSION_VALUE);
    QMD_SET(q, rc, PG106_QMD_API_VISIBLE_CALL_LIMIT,
            PG106_QMD_API_VISIBLE_CALL_LIMIT_NO_CHECK);
    QMD_SET(q, rc, PG106_QMD_SAMPLER_INDEX,
            PG106_QMD_SAMPLER_INDEX_INDEPENDENTLY);
    QMD_SET(q, rc, PG106_QMD_SM_GLOBAL_CACHING_ENABLE, 1u);
    /* gallium nve4_compute.c:645-646. */
    QMD_SET(q, rc, PG106_QMD_RELEASE_MEMBAR_TYPE,
            PG106_QMD_RELEASE_MEMBAR_FE_SYSMEMBAR);
    QMD_SET(q, rc, PG106_QMD_CWD_MEMBAR_TYPE,
            PG106_QMD_CWD_MEMBAR_L1_SYSMEMBAR);

    /* Per-launch fields (gallium :649-662, NAK fill_qmd). */
    QMD_SET(q, rc, PG106_QMD_PROGRAM_OFFSET, p->program_offset);
    QMD_SET(q, rc, PG106_QMD_CTA_RASTER_WIDTH, p->raster_width);
    QMD_SET(q, rc, PG106_QMD_CTA_RASTER_HEIGHT, p->raster_height);
    QMD_SET(q, rc, PG106_QMD_CTA_RASTER_DEPTH, p->raster_depth);
    QMD_SET(q, rc, PG106_QMD_CTA_THREAD_DIMENSION0, p->thread_dim[0]);
    QMD_SET(q, rc, PG106_QMD_CTA_THREAD_DIMENSION1, p->thread_dim[1]);
    QMD_SET(q, rc, PG106_QMD_CTA_THREAD_DIMENSION2, p->thread_dim[2]);
    QMD_SET(q, rc, PG106_QMD_SHARED_MEMORY_SIZE, p->shared_memory_size);
    QMD_SET(q, rc, PG106_QMD_SHADER_LOCAL_MEMORY_LOW_SIZE,
            p->local_mem_low_size);
    QMD_SET(q, rc, PG106_QMD_SHADER_LOCAL_MEMORY_HIGH_SIZE, 0u);
    QMD_SET(q, rc, PG106_QMD_SHADER_LOCAL_MEMORY_CRS_SIZE, p->crs_size);
    QMD_SET(q, rc, PG106_QMD_REGISTER_COUNT, p->register_count);
    QMD_SET(q, rc, PG106_QMD_BARRIER_COUNT, p->barrier_count);

    /* Constant buffers: gallium gp100_cp_launch_desc_set_cb
     * (nve4_compute.c:519-532) - ADDR_LOWER/UPPER, SIZE_SHIFTED4 =
     * size / 16, VALID(i) = TRUE. NAK asserts size is a multiple of 16
     * (qmd_impl_set_cbuf); so do we. */
    for (unsigned i = 0; i < PG106_QMD_MAX_CBUFS && rc == PG106_QMD_OK; i++) {
        const pg106_qmd_cbuf *cb = &p->cbuf[i];
        if (cb->size == 0)
            continue;
        if ((cb->size & 0xFu) != 0 || (cb->addr >> 49) != 0)
            return PG106_QMD_ERR_RANGE;
        QMD_SET(q, rc, PG106_QMD_CONSTANT_BUFFER_ADDR_LOWER(i),
                (uint32_t)cb->addr);
        QMD_SET(q, rc, PG106_QMD_CONSTANT_BUFFER_ADDR_UPPER(i),
                (uint32_t)(cb->addr >> 32));
        QMD_SET(q, rc, PG106_QMD_CONSTANT_BUFFER_SIZE_SHIFTED4(i),
                cb->size >> 4);
        QMD_SET(q, rc, PG106_QMD_CONSTANT_BUFFER_VALID(i), 1u);
    }

    /* Completion release. RELEASE0_* are only acted on with
     * SEMAPHORE_RELEASE_ENABLE0 set (clc1c0qmd.h:484); ONE_WORD writes
     * just the 32-bit payload (RELEASE0_STRUCTURE_SIZE_ONE_WORD), the
     * default FOUR_WORDS would also write a timestamp. */
    if (p->release0_addr != 0) {
        QMD_SET(q, rc, PG106_QMD_SEMAPHORE_RELEASE_ENABLE0, 1u);
        QMD_SET(q, rc, PG106_QMD_RELEASE0_ADDRESS_LOWER,
                (uint32_t)p->release0_addr);
        QMD_SET(q, rc, PG106_QMD_RELEASE0_ADDRESS_UPPER,
                (uint32_t)(p->release0_addr >> 32));
        QMD_SET(q, rc, PG106_QMD_RELEASE0_STRUCTURE_SIZE,
                PG106_QMD_RELEASE0_STRUCTURE_ONE_WORD);
        QMD_SET(q, rc, PG106_QMD_RELEASE0_PAYLOAD, p->release0_payload);
    }
    return rc;
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
