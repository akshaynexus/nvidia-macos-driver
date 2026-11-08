// SPDX-License-Identifier: MIT
/*
 * pascal/ce.h - GP106 copy-engine (CE) DMA method encoders.
 *
 * Part of the portable Pascal (GP106M) core library. Pure C11, libc only,
 * no hardware access: these functions only pack method streams through the
 * pascal/pushbuf.h pg106_pb_* writer.
 *
 * Class + method encodings (golden reference, MIT licensed, in-tree):
 *   third_party/open-gpu-kernel-modules/src/common/sdk/nvidia/inc/class/
 *   clc1b5.h
 *     - PASCAL_DMA_COPY_B                       0xC1B5
 *     - NVC1B5_SET_SEMAPHORE_A/B/PAYLOAD        0x240/0x244/0x248
 *     - NVC1B5_LAUNCH_DMA                       0x300 (fields 1:0 .. 24:24)
 *     - NVC1B5_OFFSET_IN_UPPER/LOWER            0x400/0x404
 *     - NVC1B5_OFFSET_OUT_UPPER/LOWER           0x408/0x40C
 *     - NVC1B5_PITCH_IN / PITCH_OUT             0x410/0x414
 *     - NVC1B5_LINE_LENGTH_IN / LINE_COUNT      0x418/0x41C
 *   Mesa mirrors the same class/numbers in
 *   third_party/mesa/src/nouveau/headers/nvidia/classes/clc1b5.h and drives
 *   them from src/nouveau/vulkan/nvk_cmd_copy.c (nvk_cmd_copy_memory_ce
 *   emits OFFSET_IN(_UPPER)/OFFSET_OUT(_UPPER), LINE_LENGTH_IN, LINE_COUNT,
 *   P_IMMD LAUNCH_DMA for the 1D buffer-copy path).
 *
 * Method addresses are generation-stable: tinygrad drives the AMPERE class
 * NVC6B5 (0xC6B5, AMPERE_DMA_COPY_A) with identical method addresses for
 * its copy queue (tinygrad/runtime/ops_nv.py NVCopyQueue.copy, lines
 * 186-195; nv_570.py NVC6B5_OFFSET_IN_UPPER = 0x400, NVC6B5_LAUNCH_DMA =
 * 0x300), and research/evidence/UAPI-ORACLE.md line 135 records the same
 * Maxwell->Blackwell stability for 0x240/0x300/0x400/0x418. Only the class
 * id differs (C6B5 vs our C1B5), never the method addresses/fields used
 * here.
 *
 * GP106 has 4 copy-engine instances (research/prior-art/pascal-egpu/
 * RESEARCH.md: ".ce = gp102_ce_new // Copy Engines (4 instances,
 * mask 0x0f)"); any instance is programmed through the same NVC1B5 class,
 * bound to a channel subchannel. The nouveau nvkm engine/ce sources are
 * NOT vendored in this repo, so instance/runlist details live in the
 * research notes, not in this encoder: which subchannel number a CE
 * channel occupies is a channel-layout choice made by the caller.
 */
#ifndef PASCAL_CE_H
#define PASCAL_CE_H

#include <stddef.h>
#include <stdint.h>

#include "pascal/pushbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Class id, from clc1b5.h PASCAL_DMA_COPY_B. */
#define PG106_CLASS_PASCAL_DMA_COPY_B   0xC1B5u

/* NVC1B5 method addresses (clc1b5.h, same numbers in NVC6B5/NV90B5). */
#define PG106_NVC1B5_SET_SEMAPHORE_A        0x0240u  /* NVC1B5_SET_SEMAPHORE_A */
#define PG106_NVC1B5_SET_SEMAPHORE_B        0x0244u  /* NVC1B5_SET_SEMAPHORE_B */
#define PG106_NVC1B5_SET_SEMAPHORE_PAYLOAD  0x0248u  /* NVC1B5_SET_SEMAPHORE_PAYLOAD */
#define PG106_NVC1B5_LAUNCH_DMA             0x0300u  /* NVC1B5_LAUNCH_DMA */
#define PG106_NVC1B5_OFFSET_IN_UPPER        0x0400u  /* NVC1B5_OFFSET_IN_UPPER */
#define PG106_NVC1B5_OFFSET_IN_LOWER        0x0404u  /* NVC1B5_OFFSET_IN_LOWER */
#define PG106_NVC1B5_OFFSET_OUT_UPPER       0x0408u  /* NVC1B5_OFFSET_OUT_UPPER */
#define PG106_NVC1B5_OFFSET_OUT_LOWER       0x040Cu  /* NVC1B5_OFFSET_OUT_LOWER */
#define PG106_NVC1B5_PITCH_IN               0x0410u  /* NVC1B5_PITCH_IN */
#define PG106_NVC1B5_PITCH_OUT              0x0414u  /* NVC1B5_PITCH_OUT */
#define PG106_NVC1B5_LINE_LENGTH_IN         0x0418u  /* NVC1B5_LINE_LENGTH_IN */
#define PG106_NVC1B5_LINE_COUNT             0x041Cu  /* NVC1B5_LINE_COUNT */

/*
 * LAUNCH_DMA field values (clc1b5.h *_VALUE macros). The launch word is
 *   transfer_type [1:0] | flush_enable [2:2] << 2
 *   | semaphore_type [4:3] << 3 | SRC_MEMORY_LAYOUT_PITCH [7:7]
 *   | DST_MEMORY_LAYOUT_PITCH [8:8] | MULTI_LINE_ENABLE [9:9].
 * Both memory layouts are always PITCH here (linear); blocklinear
 * (SET_{SRC,DST}_BLOCK_SIZE etc.) is not supported by this encoder.
 */
#define PG106_NVC1B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NONE           0x0u
#define PG106_NVC1B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_PIPELINED      0x1u
#define PG106_NVC1B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NON_PIPELINED  0x2u
#define PG106_NVC1B5_LAUNCH_DMA_FLUSH_ENABLE_TRUE                 0x1u
#define PG106_NVC1B5_LAUNCH_DMA_SEMAPHORE_TYPE_NONE               0x0u
#define PG106_NVC1B5_LAUNCH_DMA_SEMAPHORE_TYPE_RELEASE_ONE_WORD   0x1u
#define PG106_NVC1B5_LAUNCH_DMA_SEMAPHORE_TYPE_RELEASE_FOUR_WORD  0x2u
#define PG106_NVC1B5_LAUNCH_DMA_SRC_MEMORY_LAYOUT_PITCH           (1u << 7)
#define PG106_NVC1B5_LAUNCH_DMA_DST_MEMORY_LAYOUT_PITCH           (1u << 8)
#define PG106_NVC1B5_LAUNCH_DMA_MULTI_LINE_ENABLE_TRUE            (1u << 9)

/* transfer_type field of the plans below (NVC1B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_*). */
#define PG106_CE_XFER_NONE           0u
#define PG106_CE_XFER_PIPELINED      1u
#define PG106_CE_XFER_NON_PIPELINED  2u

/*
 * Largest single-copy byte count: tinygrad's copy queue splits CE copies
 * at 1 << 31 (ops_nv.py NVCopyQueue.copy: `for off in range(0, sz,
 * step:=(1 << 31))`); LINE_LENGTH_IN is a 32-bit byte count so larger
 * transfers are emitted as several offset/length/launch chunks.
 */
#define PG106_CE_MAX_CHUNK ((uint64_t)1 << 31)

/*
 * Optional post-copy semaphore release: SET_SEMAPHORE_A/B/PAYLOAD name
 * the location and payload, and the LAUNCH_DMA semaphore_type becomes
 * RELEASE_ONE_WORD (or RELEASE_FOUR_WORD with a timestamp in the three
 * words after `payload`; tinygrad NVCopyQueue.semaphore, ops_nv.py
 * lines 197-201). The release fires when this chunk's copy completes.
 */
typedef struct pg106_ce_semaphore {
    uint64_t addr;      /* GPU virtual address of the semaphore */
    uint32_t payload;   /* SET_SEMAPHORE_PAYLOAD_PAYLOAD */
    unsigned four_word; /* 0: one word, nonzero: four-word + timestamp */
} pg106_ce_semaphore;

/* 1D (single linear region) copy plan. */
typedef struct pg106_ce_memcpy_plan {
    uint64_t src_addr;          /* GPU virtual address of the source */
    uint64_t dst_addr;          /* GPU virtual address of the destination */
    uint64_t size;              /* bytes to copy, > 0 */
    unsigned subch;             /* CE subchannel 0..7 (clc06f.h field) */
    unsigned transfer_type;     /* PG106_CE_XFER_* */
    unsigned flush_enable;      /* NVC1B5_LAUNCH_DMA_FLUSH_ENABLE_TRUE or 0 */
    unsigned multi_line;        /* NVC1B5_LAUNCH_DMA_MULTI_LINE_ENABLE_TRUE or 0 */
    const pg106_ce_semaphore *sem; /* NULL: no semaphore release */
} pg106_ce_memcpy_plan;

/* Simple 2D pitch-to-pitch copy plan (lines of `line_length` bytes). */
typedef struct pg106_ce_memcpy2d_plan {
    uint64_t src_addr;
    uint64_t dst_addr;
    uint32_t pitch_in;          /* source line stride in bytes, > 0 */
    uint32_t pitch_out;         /* destination line stride in bytes, > 0 */
    uint32_t line_length;       /* bytes per line, 1..0xFFFFFFFF */
    uint32_t line_count;        /* number of lines, > 0 */
    unsigned subch;
    unsigned transfer_type;
    unsigned flush_enable;
    unsigned multi_line;
    const pg106_ce_semaphore *sem;
} pg106_ce_memcpy2d_plan;

/*
 * Emit a 1D linear copy over `pb`. Sizes above PG106_CE_MAX_CHUNK are
 * split into consecutive chunks exactly like tinygrad's copy queue
 * (address + chunk size, next iteration), with one SET_OBJECT at the
 * front binding PASCAL_DMA_COPY_B (0xC1B5) to `subch`. Per chunk the
 * stream is OFFSET_IN_UPPER (increasing, 4 words: in upper/lower, out
 * upper/lower), PITCH_IN (2 words), LINE_LENGTH_IN (2 words: length,
 * LINE_COUNT=1), then LAUNCH_DMA as an immediate-data method with the
 * launch flags in the COUNT field (Mesa nvk_cmd_copy.c P_IMMD form).
 * `sem`, if given, is emitted once (SET_SEMAPHORE_A/B/PAYLOAD) before
 * the final chunk's LAUNCH_DMA only - earlier chunks never release; a four-word release adds the timestamp words
 * count to the payload method only via the launch semaphore_type.
 * Returns the first pg106_pb error seen, or PG106_PB_ERR_ARG for NULL /
 * size 0 / transfer_type > NON_PIPELINED, PG106_PB_ERR_RANGE for a
 * subchannel above 7 or an address that does not fit
 * NVC1B5_OFFSET_IN_UPPER_UPPER 16:0 (49-bit GPU address space).
 */
int pg106_ce_memcpy(pg106_pb *pb, const pg106_ce_memcpy_plan *plan);

/*
 * Emit a 2D pitch-to-pitch copy (one chunk; no size splitting: a caller
 * with a bigger surface issues several plans). LINE_COUNT carries
 * `line_count` instead of 1. Blocklinear surfaces are not supported by
 * design (see the layout defines above) - there is nothing to detect,
 * the plan simply has no tile parameters.
 */
int pg106_ce_memcpy2d(pg106_pb *pb, const pg106_ce_memcpy2d_plan *plan);

#ifdef __cplusplus
}
#endif

#endif /* PASCAL_CE_H */
