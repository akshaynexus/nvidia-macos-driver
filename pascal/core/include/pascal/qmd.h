// SPDX-License-Identifier: MIT
/*
 * pascal/qmd.h - Queue Meta Data v01_07 builder for PASCAL_COMPUTE_B.
 *
 * Pascal (GP106) compute launches describe the kernel in a 256-byte QMD,
 * version 01_07, delivered to the compute pipeline with the
 * NVC1C0_SEND_PCAS_A method (see pushbuf.h). The bit layout below is
 * taken verbatim from the golden reference
 * research/reference/mesa_clc1c0qmd.h (NVK/Mesa, MIT license), which
 * expresses each field as MW(msb:lsb) over a 2048-bit little-endian
 * bitstream: bit X is the MSB of the field, X/32 selects the u32 word,
 * X%32 is the bit position inside that word. Every v01_07 field used
 * here lies inside a single u32 word, which this helper requires.
 *
 * The 15 fields exercised by helios300-gp106-simtest.py section 5 are
 * covered; each macro cites its NVC1C0_QMDV01_07_* source name.
 */
#ifndef PASCAL_QMD_H
#define PASCAL_QMD_H

#include <stddef.h>
#include <stdint.h>

#define PG106_QMD_WORDS 64   /* 256 bytes == 2048 bits */
#define PG106_QMD_BITS  2048

/* Field bit ranges: (msb, lsb), bit 0 = first byte LSB. Source names in
 * comments; expand as pg106_qmd_insert(q->w, PG106_QMD_<NAME>, value). */
#define PG106_QMD_PROGRAM_OFFSET              287, 256  /* NVC1C0_QMDV01_07_PROGRAM_OFFSET: shader code offset >> 8 */
#define PG106_QMD_CTA_RASTER_WIDTH            415, 384  /* NVC1C0_QMDV01_07_CTA_RASTER_WIDTH (grid X) */
#define PG106_QMD_CTA_RASTER_HEIGHT           431, 416  /* NVC1C0_QMDV01_07_CTA_RASTER_HEIGHT (grid Y) */
#define PG106_QMD_CTA_RASTER_DEPTH            447, 432  /* NVC1C0_QMDV01_07_CTA_RASTER_DEPTH (grid Z) */
#define PG106_QMD_SHARED_MEMORY_SIZE          561, 544  /* NVC1C0_QMDV01_07_SHARED_MEMORY_SIZE (bytes per block) */
#define PG106_QMD_QMD_VERSION                 579, 576  /* NVC1C0_QMDV01_07_QMD_VERSION (Pascal: 7) */
#define PG106_QMD_QMD_MAJOR_VERSION           583, 580  /* NVC1C0_QMDV01_07_QMD_MAJOR_VERSION (Pascal: 1) */
#define PG106_QMD_CTA_THREAD_DIMENSION0       607, 592  /* NVC1C0_QMDV01_07_CTA_THREAD_DIMENSION0 (block X) */
#define PG106_QMD_CTA_THREAD_DIMENSION1       623, 608  /* NVC1C0_QMDV01_07_CTA_THREAD_DIMENSION1 (block Y) */
#define PG106_QMD_CTA_THREAD_DIMENSION2       639, 624  /* NVC1C0_QMDV01_07_CTA_THREAD_DIMENSION2 (block Z) */
#define PG106_QMD_RELEASE0_ADDRESS_LOWER      767, 736  /* NVC1C0_QMDV01_07_RELEASE0_ADDRESS_LOWER (semaphore address, 32 LSBs) */
#define PG106_QMD_RELEASE0_ADDRESS_UPPER      775, 768  /* NVC1C0_QMDV01_07_RELEASE0_ADDRESS_UPPER */
#define PG106_QMD_RELEASE0_PAYLOAD            831, 800  /* NVC1C0_QMDV01_07_RELEASE0_PAYLOAD */
/* Constant buffer slot i (i = 0 for the simtest's CB0), same header: */
#define PG106_QMD_CONSTANT_BUFFER_ADDR_LOWER(i) ((959 + (i) * 64)), ((928 + (i) * 64))  /* ..._CONSTANT_BUFFER_ADDR_LOWER(i) */
#define PG106_QMD_CONSTANT_BUFFER_ADDR_UPPER(i) ((967 + (i) * 64)), ((960 + (i) * 64))  /* ..._CONSTANT_BUFFER_ADDR_UPPER(i) */
#define PG106_QMD_CONSTANT_BUFFER_SIZE(i)       ((991 + (i) * 64)), ((975 + (i) * 64))  /* ..._CONSTANT_BUFFER_SIZE(i) */
#define PG106_QMD_SHADER_LOCAL_MEMORY_LOW_SIZE 1463, 1440 /* NVC1C0_QMDV01_07_SHADER_LOCAL_MEMORY_LOW_SIZE (bytes per thread) */
#define PG106_QMD_BARRIER_COUNT               1471, 1467 /* NVC1C0_QMDV01_07_BARRIER_COUNT */
#define PG106_QMD_REGISTER_COUNT              1503, 1496 /* NVC1C0_QMDV01_07_REGISTER_COUNT (per thread) */

/* Required identity of a Pascal QMD (the hardware rejects others). */
#define PG106_QMD_VERSION_VALUE       7u  /* clc1c0qmd.h NVC1C0_QMDV01_07 */
#define PG106_QMD_MAJOR_VERSION_VALUE 1u

/* Status codes; negative on failure. */
#define PG106_QMD_OK           0
#define PG106_QMD_ERR_RANGE   (-2)  /* value does not fit its MW(msb:lsb) field */
#define PG106_QMD_ERR_ARG     (-3)  /* NULL pointer or unsupported bit range */
#define PG106_QMD_ERR_VERSION (-4)  /* validate(): version/major not 01_07 */

/* One Queue Meta Data image, raw u32 words, ready to DMA. */
typedef struct pg106_qmd {
    uint32_t w[PG106_QMD_WORDS];
} pg106_qmd;

/*
 * Shared MW(msb,lsb) insert helper (bit X of the QMD = MSB). Overwrites
 * exactly the field's bits. `msb` and `lsb` must select the same u32
 * word (true for every v01_07 field above) and `value` must fit the
 * field's width.
 */
int pg106_qmd_insert(uint32_t *qmd, unsigned msb, unsigned lsb,
                     uint32_t value);

/* Shared MW(msb,lsb) extract helper; 0 on a bad range. */
uint32_t pg106_qmd_extract(const uint32_t *qmd, unsigned msb, unsigned lsb);

/* The fields the simtest exercises; sizes are in the unit the header
 * names (bytes for SHARED_MEMORY_SIZE / LOCAL_MEMORY, raw counts for the
 * rest). `program_offset` is the shader code offset already >> 8. */
typedef struct pg106_qmd_params {
    uint32_t program_offset;         /* PG106_QMD_PROGRAM_OFFSET */
    uint32_t raster_width;           /* CTA_RASTER_WIDTH */
    uint32_t raster_height;          /* CTA_RASTER_HEIGHT */
    uint32_t raster_depth;           /* CTA_RASTER_DEPTH */
    uint32_t thread_dim[3];          /* CTA_THREAD_DIMENSION0..2 */
    uint32_t shared_memory_size;     /* SHARED_MEMORY_SIZE */
    uint32_t register_count;         /* REGISTER_COUNT */
    uint32_t barrier_count;          /* BARRIER_COUNT */
    uint32_t local_mem_low_size;     /* SHADER_LOCAL_MEMORY_LOW_SIZE */
    uint64_t release0_addr;          /* RELEASE0_ADDRESS_{LOWER,UPPER} */
    uint32_t release0_payload;       /* RELEASE0_PAYLOAD */
    uint64_t cb0_addr;               /* CONSTANT_BUFFER0 addr lower/upper */
    uint32_t cb0_size;               /* CONSTANT_BUFFER0 size */
} pg106_qmd_params;

/*
 * Zero the QMD, stamp QMD_VERSION = 7 / QMD_MAJOR_VERSION = 1, and
 * insert every field in `p`. Each value is range-checked against its
 * MW field; on the first failure the corresponding error is returned
 * and the QMD is left with the fields inserted so far.
 */
int pg106_qmd_build(const pg106_qmd_params *p, pg106_qmd *q);

/* 0 if the QMD carries the Pascal v01_07 identity, else ERR_VERSION. */
int pg106_qmd_validate(const pg106_qmd *q);

#endif /* PASCAL_QMD_H */
