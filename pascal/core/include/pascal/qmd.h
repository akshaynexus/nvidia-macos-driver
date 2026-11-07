// SPDX-License-Identifier: MIT
/*
 * pascal/qmd.h - Queue Meta Data v02_01 builder for PASCAL_COMPUTE_B.
 *
 * Pascal (GP106) compute launches describe the kernel in a 256-byte QMD
 * referenced by SEND_PCAS_A (see dispatch.h). Every Pascal userspace
 * uses QMD version 02_01:
 *   - NAK picks Qmd2_1 for cls_compute >= PASCAL_COMPUTE_A
 *     (third_party/mesa:src/nouveau/compiler/nak/qmd.rs:942-943 nak_fill_qmd,
 *     Qmd2_1 at :510-548);
 *   - gallium nvc0 fills NVC0C0_QMDV02_01 in
 *     gp100_compute_setup_launch_desc (gallium/drivers/nouveau/nvc0/
 *     nve4_compute.c:637-675).
 * v01_07 (what this file used to build) is the Kepler/Maxwell layout;
 * several fields sit elsewhere in v02_01 (CTA_RASTER_DEPTH, the constant
 * buffer block, REGISTER_COUNT, BARRIER_COUNT, SHADER_LOCAL_MEMORY_*).
 *
 * Bit positions are NVC1C0_QMDV02_01_* from
 * third_party/mesa:src/nouveau/headers/nvidia/classes/clc1c0qmd.h
 * (identical, field for field, to the NVC0C0_QMDV02_01_* layout in
 * clc0c0qmd.h that NAK and gallium compile against; test_mesa_diff.c
 * checks both). MW(msb:lsb) over a 2048-bit little-endian bitstream:
 * bit X is in u32 word X/32 at bit X%32. Every field used here lies in
 * one u32 word, which the insert helper requires.
 */
#ifndef PASCAL_QMD_H
#define PASCAL_QMD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PG106_QMD_WORDS 64   /* 256 bytes == 2048 bits */
#define PG106_QMD_BITS  2048

/* Field bit ranges (msb, lsb); expand as
 * pg106_qmd_insert(q->w, PG106_QMD_<NAME>, value). Names are the
 * NVC1C0_QMDV02_01_<NAME> fields of clc1c0qmd.h. */
#define PG106_QMD_SM_GLOBAL_CACHING_ENABLE     134, 134
#define PG106_QMD_SEMAPHORE_RELEASE_ENABLE0    138, 138
#define PG106_QMD_PROGRAM_OFFSET               287, 256
#define PG106_QMD_RELEASE_MEMBAR_TYPE          366, 366
#define PG106_QMD_CWD_MEMBAR_TYPE              369, 368
#define PG106_QMD_API_VISIBLE_CALL_LIMIT       378, 378
#define PG106_QMD_SAMPLER_INDEX                382, 382
#define PG106_QMD_CTA_RASTER_WIDTH             415, 384
#define PG106_QMD_CTA_RASTER_HEIGHT            431, 416
#define PG106_QMD_CTA_RASTER_DEPTH             463, 448
#define PG106_QMD_SHARED_MEMORY_SIZE           561, 544
#define PG106_QMD_QMD_VERSION                  579, 576
#define PG106_QMD_QMD_MAJOR_VERSION            583, 580
#define PG106_QMD_CTA_THREAD_DIMENSION0        607, 592
#define PG106_QMD_CTA_THREAD_DIMENSION1        623, 608
#define PG106_QMD_CTA_THREAD_DIMENSION2        639, 624
#define PG106_QMD_CONSTANT_BUFFER_VALID(i)     (640 + (i)), (640 + (i))
#define PG106_QMD_RELEASE0_ADDRESS_LOWER       767, 736
#define PG106_QMD_RELEASE0_ADDRESS_UPPER       775, 768
#define PG106_QMD_RELEASE0_STRUCTURE_SIZE      799, 799
#define PG106_QMD_RELEASE0_PAYLOAD             831, 800
#define PG106_QMD_SHADER_LOCAL_MEMORY_LOW_SIZE  951, 928
#define PG106_QMD_BARRIER_COUNT                959, 955
#define PG106_QMD_SHADER_LOCAL_MEMORY_HIGH_SIZE 983, 960
#define PG106_QMD_REGISTER_COUNT               991, 984
#define PG106_QMD_SHADER_LOCAL_MEMORY_CRS_SIZE 1015, 992
#define PG106_QMD_SASS_VERSION                 1023, 1016
#define PG106_QMD_CONSTANT_BUFFER_ADDR_LOWER(i) (1055 + (i) * 64), (1024 + (i) * 64)
#define PG106_QMD_CONSTANT_BUFFER_ADDR_UPPER(i) (1072 + (i) * 64), (1056 + (i) * 64)
#define PG106_QMD_CONSTANT_BUFFER_SIZE_SHIFTED4(i) (1087 + (i) * 64), (1075 + (i) * 64)

/* Enum values (clc1c0qmd.h NVC1C0_QMDV02_01_*). */
#define PG106_QMD_VERSION_VALUE                 1u /* QMD_VERSION for 02_01 */
#define PG106_QMD_MAJOR_VERSION_VALUE           2u /* QMD_MAJOR_VERSION */
#define PG106_QMD_RELEASE_MEMBAR_FE_SYSMEMBAR   1u /* RELEASE_MEMBAR_TYPE_FE_SYSMEMBAR */
#define PG106_QMD_CWD_MEMBAR_L1_SYSMEMBAR       1u /* CWD_MEMBAR_TYPE_L1_SYSMEMBAR */
#define PG106_QMD_API_VISIBLE_CALL_LIMIT_NO_CHECK 1u
#define PG106_QMD_SAMPLER_INDEX_INDEPENDENTLY   0u
#define PG106_QMD_RELEASE0_STRUCTURE_ONE_WORD   1u /* RELEASE0_STRUCTURE_SIZE_ONE_WORD */

/* Eight constant-buffer slots: CONSTANT_BUFFER_VALID(i) covers bits
 * 647:640 (QMD_RESERVED_I starts at 648); gallium asserts index < 8
 * (nve4_compute.c:524). */
#define PG106_QMD_MAX_CBUFS 8u

/* Status codes; negative on failure. */
#define PG106_QMD_OK           0
#define PG106_QMD_ERR_RANGE   (-2)  /* value does not fit / misaligned */
#define PG106_QMD_ERR_ARG     (-3)  /* NULL pointer or unsupported bit range */
#define PG106_QMD_ERR_VERSION (-4)  /* validate(): not a v02_01 QMD */

/* One Queue Meta Data image, raw u32 words, ready to DMA. */
typedef struct pg106_qmd {
    uint32_t w[PG106_QMD_WORDS];
} pg106_qmd;

/*
 * Shared MW(msb,lsb) insert helper (bit X of the QMD = MSB). Overwrites
 * exactly the field's bits. `msb` and `lsb` must select the same u32
 * word and `value` must fit the field's width.
 */
int pg106_qmd_insert(uint32_t *qmd, unsigned msb, unsigned lsb,
                     uint32_t value);

/* Shared MW(msb,lsb) extract helper; 0 on a bad range. */
uint32_t pg106_qmd_extract(const uint32_t *qmd, unsigned msb, unsigned lsb);

/* One constant buffer binding; size 0 = slot unbound (NAK fill_qmd
 * skips cb.size == 0, qmd.rs:876-879). */
typedef struct pg106_qmd_cbuf {
    uint64_t addr;   /* GPU VA, < 2^49 (ADDR_UPPER is 17 bits) */
    uint32_t size;   /* bytes, multiple of 16, <= 0x1FFF0 (SIZE_SHIFTED4 13 bits) */
} pg106_qmd_cbuf;

/* Launch parameters. Units follow the v02_01 field definitions. */
typedef struct pg106_qmd_params {
    uint32_t program_offset;         /* PROGRAM_OFFSET: byte offset of the
                                        kernel from SET_PROGRAM_REGION
                                        (gallium cp->code_base) */
    uint32_t raster_width;           /* CTA_RASTER_WIDTH (grid X) */
    uint32_t raster_height;          /* CTA_RASTER_HEIGHT, 16 bits */
    uint32_t raster_depth;           /* CTA_RASTER_DEPTH, 16 bits */
    uint32_t thread_dim[3];          /* CTA_THREAD_DIMENSION0..2 */
    uint32_t shared_memory_size;     /* bytes, multiple of 0x100 (gallium
                                        align(.., 0x100), NAK next_multiple_of) */
    uint32_t register_count;         /* REGISTER_COUNT, 8 bits */
    uint32_t barrier_count;          /* BARRIER_COUNT, 5 bits */
    uint32_t local_mem_low_size;     /* SHADER_LOCAL_MEMORY_LOW_SIZE, bytes per
                                        thread, multiple of 16 (gallium hdr[1]
                                        & 0xfffff0, NAK next_multiple_of(0x10)) */
    uint32_t crs_size;               /* SHADER_LOCAL_MEMORY_CRS_SIZE (gallium
                                        0x800, NAK info.crs_size) */
    uint64_t release0_addr;          /* 0 = no release; else < 2^40 */
    uint32_t release0_payload;       /* RELEASE0_PAYLOAD (one-word release) */
    pg106_qmd_cbuf cbuf[PG106_QMD_MAX_CBUFS];
} pg106_qmd_params;

/*
 * Zero the QMD and fill it the way Pascal userspace does:
 *   identity     QMD_MAJOR_VERSION 2 / QMD_VERSION 1 (NAK qmd_init!)
 *   always set   API_VISIBLE_CALL_LIMIT = NO_CHECK, SAMPLER_INDEX =
 *                INDEPENDENTLY (NAK qmd_init!, gallium :647),
 *                SM_GLOBAL_CACHING_ENABLE = 1 (NAK :519, gallium :644),
 *                RELEASE_MEMBAR_TYPE = FE_SYSMEMBAR, CWD_MEMBAR_TYPE =
 *                L1_SYSMEMBAR (gallium :645-646),
 *                SHADER_LOCAL_MEMORY_HIGH_SIZE = 0 (gallium :658, NAK
 *                qmd_impl_set_slm_size)
 *   per launch   grid/block, PROGRAM_OFFSET, SHARED_MEMORY_SIZE,
 *                SHADER_LOCAL_MEMORY_LOW/CRS_SIZE, REGISTER_COUNT,
 *                BARRIER_COUNT
 *   per cbuf     ADDR_LOWER/UPPER, SIZE_SHIFTED4 = size/16 and
 *                CONSTANT_BUFFER_VALID(i) = TRUE (gallium
 *                gp100_cp_launch_desc_set_cb :519-532, NAK set_cbuf)
 *   release      when release0_addr != 0: SEMAPHORE_RELEASE_ENABLE0 = 1,
 *                RELEASE0_ADDRESS, RELEASE0_PAYLOAD and
 *                RELEASE0_STRUCTURE_SIZE = ONE_WORD (the release
 *                fields are ignored unless ENABLE0 is set; tinygrad
 *                QMD.set_release arms release{i}_enable the same way,
 *                ops_nv.py:81-87).
 * SASS_VERSION is left 0: neither NAK's Qmd2_1 nor gallium's v02_01
 * path sets it (they set 0x30 only for v00_06 / v01_07).
 * Returns ERR_ARG for NULL, ERR_RANGE for a value that does not fit or
 * is misaligned; the QMD is then only partially filled.
 */
int pg106_qmd_build(const pg106_qmd_params *p, pg106_qmd *q);

/* 0 if the QMD carries the v02_01 identity, else ERR_VERSION. */
int pg106_qmd_validate(const pg106_qmd *q);

#ifdef __cplusplus
}
#endif

#endif /* PASCAL_QMD_H */
