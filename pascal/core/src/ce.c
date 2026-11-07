// SPDX-License-Identifier: MIT
/*
 * pascal/ce.c - GP106 copy-engine (CE) DMA method encoders.
 *
 * Everything here is derived from MIT-licensed sources only:
 *   - third_party/open-gpu-kernel-modules/src/common/sdk/nvidia/inc/class/
 *     clc1b5.h (PASCAL_DMA_COPY_B 0xC1B5 and the NVC1B5_* method/field
 *     macros; every constant emitted below is cited to its macro name),
 *   - tinygrad/runtime/ops_nv.py NVCopyQueue (chunking at 1 << 31 and the
 *     OFFSET_IN_UPPER / LINE_LENGTH_IN / LAUNCH_DMA sequence, MIT),
 *   - mesa src/nouveau/vulkan/nvk_cmd_copy.c nvk_cmd_copy_memory_ce
 *     (the 1D copy method order and the P_IMMD LAUNCH_DMA form, MIT).
 * No nouveau (GPL) logic is used, so this file is MIT.
 *
 * SIMULATION/HOST-SIDE ONLY: packs words through pascal/pushbuf.h, never
 * touches hardware.
 */
#include "pascal/ce.h"

#include <stddef.h>

/*
 * NVC1B5_OFFSET_IN_UPPER_UPPER is 16:0 (clc1b5.h line 131), so GPU
 * virtual addresses are 49-bit.
 */
#define PG106_CE_ADDR_MAX (((uint64_t)1 << 49) - 1)

/* Highest field value of NVC1B5_LAUNCH_DMA_DATA_TRANSFER_TYPE 1:0. */
#define PG106_CE_XFER_MAX PG106_NVC1B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NON_PIPELINED

static int ce_check_addr(uint64_t addr)
{
    return addr <= PG106_CE_ADDR_MAX ? PG106_PB_OK : PG106_PB_ERR_RANGE;
}

/*
 * One copy chunk: offsets, pitches, line geometry, optional semaphore
 * setup, LAUNCH_DMA. Shared by the 1D and 2D paths; `line_count` is 1
 * for 1D chunks (NVC1B5_LINE_COUNT_VALUE) and the caller's line count
 * for 2D. `sem_type` is the NVC1B5_LAUNCH_DMA_SEMAPHORE_TYPE_* value to
 * OR into the launch word (NONE when no semaphore was emitted).
 */
static int ce_emit_chunk(pg106_pb *pb, unsigned subch,
                         uint64_t src, uint64_t dst,
                         uint32_t pitch_in, uint32_t pitch_out,
                         uint32_t line_length, uint32_t line_count,
                         unsigned transfer_type, unsigned flush_enable,
                         unsigned multi_line,
                         const pg106_ce_semaphore *sem)
{
    uint32_t geo[2];
    uint32_t launch;
    int rc;

    /* OFFSET_IN_UPPER, 4 increasing words: NVC1B5_OFFSET_IN_UPPER
     * (0x400), _IN_LOWER (0x404), _OUT_UPPER (0x408), _OUT_LOWER
     * (0x40C) - one contiguous block, emitted as a single increasing
     * method like tinygrad ops_nv.py line 190 and Mesa nvk_cmd_copy.c
     * P_MTHD(p, NV90B5, OFFSET_IN_UPPER) block. */
    {
        uint32_t off[4] = {
            (uint32_t)(src >> 32),              /* NVC1B5_OFFSET_IN_UPPER_UPPER  16:0 */
            (uint32_t)src,                      /* NVC1B5_OFFSET_IN_LOWER_VALUE  31:0 */
            (uint32_t)(dst >> 32),              /* NVC1B5_OFFSET_OUT_UPPER_UPPER 16:0 */
            (uint32_t)dst,                      /* NVC1B5_OFFSET_OUT_LOWER_VALUE 31:0 */
        };
        rc = pg106_pb_inc(pb, subch, PG106_NVC1B5_OFFSET_IN_UPPER, off, 4);
        if (rc != PG106_PB_OK)
            return rc;
    }

    /* PITCH_IN/PITCH_OUT (NVC1B5_PITCH_IN_VALUE 31:0). */
    {
        uint32_t pitch[2] = { pitch_in, pitch_out };
        rc = pg106_pb_inc(pb, subch, PG106_NVC1B5_PITCH_IN, pitch, 2);
        if (rc != PG106_PB_OK)
            return rc;
    }

    /* LINE_LENGTH_IN (bytes) + LINE_COUNT (NVC1B5_LINE_LENGTH_IN_VALUE,
     * NVC1B5_LINE_COUNT_VALUE). Mesa's nvk_cmd_copy_memory_ce emits the
     * same pair; LINE_COUNT = 1 for a 1D chunk. */
    geo[0] = line_length;
    geo[1] = line_count;
    rc = pg106_pb_inc(pb, subch, PG106_NVC1B5_LINE_LENGTH_IN, geo, 2);
    if (rc != PG106_PB_OK)
        return rc;

    /* Optional semaphore release: SET_SEMAPHORE_A (UPPER 16:0),
     * SET_SEMAPHORE_B (LOWER 31:0), SET_SEMAPHORE_PAYLOAD - one
     * increasing block starting at 0x240, the exact order of tinygrad
     * NVCopyQueue.semaphore (ops_nv.py lines 197-201). */
    if (sem != NULL) {
        uint32_t s[3] = {
            (uint32_t)(sem->addr >> 32),        /* NVC1B5_SET_SEMAPHORE_A_UPPER 16:0 */
            (uint32_t)sem->addr,                /* NVC1B5_SET_SEMAPHORE_B_LOWER 31:0 */
            sem->payload,                       /* NVC1B5_SET_SEMAPHORE_PAYLOAD_PAYLOAD */
        };
        rc = pg106_pb_inc(pb, subch, PG106_NVC1B5_SET_SEMAPHORE_A, s, 3);
        if (rc != PG106_PB_OK)
            return rc;
    }

    /*
     * LAUNCH_DMA. Bits per clc1b5.h: DATA_TRANSFER_TYPE 1:0,
     * FLUSH_ENABLE 2:2, SEMAPHORE_TYPE 4:3, SRC_MEMORY_LAYOUT 7:7 and
     * DST_MEMORY_LAYOUT 8:8 always PITCH (linear copies only),
     * MULTI_LINE_ENABLE 9:9. REMAP_ENABLE 10:10 stays FALSE (no
     * SET_REMAP_COMPONENTS is programmed). Emitted as an immediate-data
     * method (Mesa: P_IMMD(p, NV90B5, LAUNCH_DMA, {...}) in
     * nvk_cmd_copy_memory_ce); tinygrad instead sends one data word with
     * its typ<<28 INC form - same method address 0x300, same payload
     * bits (see tests/test_ce.c for the cross-check).
     */
    launch = transfer_type;
    if (flush_enable)
        launch |= PG106_NVC1B5_LAUNCH_DMA_FLUSH_ENABLE_TRUE << 2;
    if (sem != NULL) {
        launch |= (sem->four_word
                       ? PG106_NVC1B5_LAUNCH_DMA_SEMAPHORE_TYPE_RELEASE_FOUR_WORD
                       : PG106_NVC1B5_LAUNCH_DMA_SEMAPHORE_TYPE_RELEASE_ONE_WORD)
                  << 3;
    }
    launch |= PG106_NVC1B5_LAUNCH_DMA_SRC_MEMORY_LAYOUT_PITCH;
    launch |= PG106_NVC1B5_LAUNCH_DMA_DST_MEMORY_LAYOUT_PITCH;
    if (multi_line)
        launch |= PG106_NVC1B5_LAUNCH_DMA_MULTI_LINE_ENABLE_TRUE;

    return pg106_pb_immd(pb, subch, PG106_NVC1B5_LAUNCH_DMA, launch);
}

int pg106_ce_memcpy(pg106_pb *pb, const pg106_ce_memcpy_plan *plan)
{
    int rc;

    if (pb == NULL || plan == NULL)
        return PG106_PB_ERR_ARG;
    if (plan->size == 0)
        return PG106_PB_ERR_ARG;
    if (plan->subch > PG106_DMA_SUBCHANNEL_MAX)
        return PG106_PB_ERR_RANGE;
    if (plan->transfer_type > PG106_CE_XFER_MAX)
        return PG106_PB_ERR_ARG;
    rc = ce_check_addr(plan->src_addr);
    if (rc != PG106_PB_OK)
        return rc;
    rc = ce_check_addr(plan->dst_addr);
    if (rc != PG106_PB_OK)
        return rc;
    if (plan->sem != NULL) {
        rc = ce_check_addr(plan->sem->addr);
        if (rc != PG106_PB_OK)
            return rc;
    }

    /* Bind PASCAL_DMA_COPY_B (NVC1B5_SET_OBJECT is method 0x0000, the
     * same SET_OBJECT slot every Fermi+ class uses) once per stream. */
    rc = pg106_pb_set_object(pb, plan->subch, PG106_CLASS_PASCAL_DMA_COPY_B);
    if (rc != PG106_PB_OK)
        return rc;

    /* Chunk at 1 << 31 like tinygrad NVCopyQueue.copy (ops_nv.py lines
     * 189-195). Pitch defaults to the chunk size: LINE_COUNT is 1, so
     * the pitch values are don't-cares on hardware, but Mesa's image
     * path always programs them and a defined stream is easier to
     * golden-test than a stale-method-RAM one. */
    for (uint64_t off = 0; off < plan->size; ) {
        uint64_t chunk = plan->size - off;
        if (chunk > PG106_CE_MAX_CHUNK)
            chunk = PG106_CE_MAX_CHUNK;
        /* The release signals completion of the whole copy, so only the
         * final LAUNCH_DMA carries it. tinygrad keeps the two apart:
         * NVCopyQueue.copy (ops_nv.py:187-193) launches the chunks with
         * no semaphore, NVCopyQueue.semaphore (ops_nv.py:195-197) is one
         * SET_SEMAPHORE_A/B/PAYLOAD + releasing LAUNCH_DMA afterwards.
         * Releasing per chunk would wake waiters before later chunks
         * have landed. */
        const int last = (off + chunk == plan->size);

        rc = ce_emit_chunk(pb, plan->subch,
                           plan->src_addr + off, plan->dst_addr + off,
                           (uint32_t)chunk, (uint32_t)chunk,
                           (uint32_t)chunk, 1,
                           plan->transfer_type, plan->flush_enable,
                           plan->multi_line, last ? plan->sem : NULL);
        if (rc != PG106_PB_OK)
            return rc;
        off += chunk;
    }
    return pb->error;
}

int pg106_ce_memcpy2d(pg106_pb *pb, const pg106_ce_memcpy2d_plan *plan)
{
    int rc;

    if (pb == NULL || plan == NULL)
        return PG106_PB_ERR_ARG;
    if (plan->pitch_in == 0 || plan->pitch_out == 0 ||
        plan->line_length == 0 || plan->line_count == 0)
        return PG106_PB_ERR_ARG;
    if (plan->subch > PG106_DMA_SUBCHANNEL_MAX)
        return PG106_PB_ERR_RANGE;
    if (plan->transfer_type > PG106_CE_XFER_MAX)
        return PG106_PB_ERR_ARG;
    rc = ce_check_addr(plan->src_addr);
    if (rc != PG106_PB_OK)
        return rc;
    rc = ce_check_addr(plan->dst_addr);
    if (rc != PG106_PB_OK)
        return rc;
    if (plan->sem != NULL) {
        rc = ce_check_addr(plan->sem->addr);
        if (rc != PG106_PB_OK)
            return rc;
    }

    rc = pg106_pb_set_object(pb, plan->subch, PG106_CLASS_PASCAL_DMA_COPY_B);
    if (rc != PG106_PB_OK)
        return rc;

    /* Mesa's NVK image copy path (nvk_cmd_copy.c, `struct nv_push *p =
     * nvk_cmd_buffer_push(cmd, 31)` block) programs the same
     * OFFSET/PITCH/LINE_LENGTH/LINE_COUNT set for pitch surfaces. */
    return ce_emit_chunk(pb, plan->subch,
                         plan->src_addr, plan->dst_addr,
                         plan->pitch_in, plan->pitch_out,
                         plan->line_length, plan->line_count,
                         plan->transfer_type, plan->flush_enable,
                         plan->multi_line, plan->sem);
}
