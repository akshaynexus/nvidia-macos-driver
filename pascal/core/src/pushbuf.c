// SPDX-License-Identifier: MIT
/*
 * pascal/pushbuf.c - GP106 pushbuffer method + GPFIFO entry encoders.
 *
 * Every field position below is quoted from the golden reference
 * research/reference/clc06f.h (NVIDIA class header for
 * PASCAL_CHANNEL_GPFIFO_A, MIT license); method and class constants come
 * from research/reference/clc1c0.h. No nouveau (GPL) material is used.
 *
 * Layout summary (all names are clc06f.h macros):
 *   NVC06F_DMA_SEC_OP             31:29
 *   NVC06F_DMA_METHOD_COUNT       28:16   (immediate data for IMMD)
 *   NVC06F_DMA_METHOD_SUBCHANNEL  15:13
 *   NVC06F_DMA_METHOD_ADDRESS     11:0    (method >> 2)
 *   NVC06F_GP_ENTRY0_FETCH         0:0
 *   NVC06F_GP_ENTRY0_GET          31:2
 *   NVC06F_GP_ENTRY1_GET_HI        7:0    (address bits 39:32)
 *   NVC06F_GP_ENTRY1_PRIV          8:8
 *   NVC06F_GP_ENTRY1_LEVEL         9:9
 *   NVC06F_GP_ENTRY1_LENGTH      30:10
 *   NVC06F_GP_ENTRY1_SYNC        31:31
 */
#include "pascal/pushbuf.h"

int pg106_pb_init(pg106_pb *pb, uint32_t *words, size_t cap)
{
    if (pb == NULL)
        return PG106_PB_ERR_ARG;
    pb->words = words;
    pb->cap = cap;
    pb->len = 0;
    pb->error = PG106_PB_OK;
    return PG106_PB_OK;
}

uint32_t pg106_pb_mthd_word(unsigned sec_op, unsigned subch,
                            uint32_t mthd, unsigned count)
{
    return ((sec_op & 0x7u) << 29)                /* NVC06F_DMA_SEC_OP 31:29 */
         | ((count & PG106_DMA_METHOD_COUNT_MAX) << 16)   /* 28:16 */
         | ((subch & PG106_DMA_SUBCHANNEL_MAX) << 13)     /* 15:13 */
         | ((mthd >> 2) & PG106_DMA_METHOD_ADDRESS_MAX);  /* 11:0 */
}

void pg106_pb_decode(uint32_t word, unsigned *sec_op, unsigned *count,
                     unsigned *subch, uint32_t *mthd)
{
    if (sec_op != NULL)
        *sec_op = (word >> 29) & 0x7u;
    if (count != NULL)
        *count = (word >> 16) & PG106_DMA_METHOD_COUNT_MAX;
    if (subch != NULL)
        *subch = (word >> 13) & PG106_DMA_SUBCHANNEL_MAX;
    if (mthd != NULL)
        *mthd = (word & PG106_DMA_METHOD_ADDRESS_MAX) << 2;
}

/* Common validation for inc/noninc/immd headers. */
static int pb_check_header(pg106_pb *pb, unsigned subch, uint32_t mthd,
                           unsigned count)
{
    if (pb == NULL)
        return PG106_PB_ERR_ARG;
    if (pb->error != PG106_PB_OK)
        return pb->error;
    if (subch > PG106_DMA_SUBCHANNEL_MAX)
        return (pb->error = PG106_PB_ERR_RANGE);
    /* method >> 2 must fit NVC06F_DMA_METHOD_ADDRESS 11:0 */
    if ((mthd >> 2) > PG106_DMA_METHOD_ADDRESS_MAX)
        return (pb->error = PG106_PB_ERR_RANGE);
    if (count > PG106_DMA_METHOD_COUNT_MAX)
        return (pb->error = PG106_PB_ERR_RANGE);
    return PG106_PB_OK;
}

/* Reserve n words, returning the write cursor or NULL on exhaustion. */
static uint32_t *pb_reserve(pg106_pb *pb, size_t n)
{
    if (n > pb->cap - pb->len) {
        pb->error = PG106_PB_ERR_SPACE;
        return NULL;
    }
    return pb->words + pb->len;
}

int pg106_pb_inc(pg106_pb *pb, unsigned subch, uint32_t mthd,
                 const uint32_t *data, unsigned count)
{
    int rc = pb_check_header(pb, subch, mthd, count);
    if (rc != PG106_PB_OK)
        return rc;
    /* An increasing method with no data has no meaning on the wire. */
    if (count < 1u || data == NULL)
        return (pb->error = PG106_PB_ERR_ARG);

    uint32_t *dst = pb_reserve(pb, 1u + (size_t)count);
    if (dst == NULL)
        return pb->error;

    dst[0] = pg106_pb_mthd_word(PG106_SEC_OP_INC_METHOD, subch, mthd, count);
    for (unsigned i = 0; i < count; i++)
        dst[1u + i] = data[i];
    pb->len += 1u + (size_t)count;
    return PG106_PB_OK;
}

int pg106_pb_noninc(pg106_pb *pb, unsigned subch, uint32_t mthd,
                    const uint32_t *data, unsigned count)
{
    int rc = pb_check_header(pb, subch, mthd, count);
    if (rc != PG106_PB_OK)
        return rc;
    if (count > 0u && data == NULL)
        return (pb->error = PG106_PB_ERR_ARG);

    uint32_t *dst = pb_reserve(pb, 1u + (size_t)count);
    if (dst == NULL)
        return pb->error;

    dst[0] = pg106_pb_mthd_word(PG106_SEC_OP_NON_INC_METHOD, subch, mthd,
                                count);
    for (unsigned i = 0; i < count; i++)
        dst[1u + i] = data[i];
    pb->len += 1u + (size_t)count;
    return PG106_PB_OK;
}

int pg106_pb_immd(pg106_pb *pb, unsigned subch, uint32_t mthd, uint32_t data)
{
    /* The value shares NVC06F_DMA_IMMD_DATA 28:16 with COUNT. */
    int rc = pb_check_header(pb, subch, mthd, data);
    if (rc != PG106_PB_OK)
        return rc;

    uint32_t *dst = pb_reserve(pb, 1u);
    if (dst == NULL)
        return pb->error;

    dst[0] = pg106_pb_mthd_word(PG106_SEC_OP_IMMD_DATA_METHOD, subch, mthd,
                                data);
    pb->len += 1u;
    return PG106_PB_OK;
}

int pg106_pb_set_object(pg106_pb *pb, unsigned subch, uint32_t class_id)
{
    /* NVC1C0_SET_OBJECT: one increasing method whose data word is the
     * class id (research/reference/clc1c0.h). */
    return pg106_pb_inc(pb, subch, PG106_NVC1C0_SET_OBJECT, &class_id, 1u);
}

int pg106_pb_endseg(pg106_pb *pb)
{
    if (pb == NULL)
        return PG106_PB_ERR_ARG;
    if (pb->error != PG106_PB_OK)
        return pb->error;

    uint32_t *dst = pb_reserve(pb, 1u);
    if (dst == NULL)
        return pb->error;

    *dst = pg106_pb_mthd_word(PG106_SEC_OP_END_PB_SEGMENT, 0u, 0u, 0u);
    pb->len += 1u;
    return PG106_PB_OK;
}

uint64_t pg106_gpfifo_pack(uint64_t addr, uint32_t len,
                           unsigned level, unsigned sync, unsigned fetch)
{
    /* ENTRY0: GET 31:2 = dword address (addr >> 2), FETCH 0:0. */
    uint64_t lo = ((addr >> 2) & 0x3FFFFFFFull) << 2
                | (uint64_t)(fetch & 1u);

    /* ENTRY1: GET_HI 7:0 carries address bits 39:32, i.e. addr >> 32;
     * then PRIV 8, LEVEL 9, LENGTH 30:10, SYNC 31. */
    uint64_t hi = ((addr >> 32) & 0xFFull)              /* GET_HI 7:0 */
                | 0ull << 8                             /* PRIV 8:8 = user */
                | ((uint64_t)(level & 1u)) << 9         /* LEVEL 9:9 */
                | ((uint64_t)(len & PG106_GP_ENTRY1_LENGTH_MAX)) << 10
                | ((uint64_t)(sync & 1u)) << 31;         /* SYNC 31:31 */
    return lo | (hi << 32);
}

void pg106_gpfifo_unpack(uint64_t entry, uint64_t *addr, uint32_t *len,
                         unsigned *level, unsigned *sync, unsigned *fetch,
                         unsigned *priv)
{
    if (addr != NULL)
        *addr = (((entry >> 32) & 0xFFull) << 32)        /* GET_HI -> 39:32 */
              | (entry & (0x3FFFFFFFull << 2));          /* GET 31:2 */
    if (len != NULL)
        *len = (uint32_t)((entry >> 42) & PG106_GP_ENTRY1_LENGTH_MAX);
    if (level != NULL)
        *level = (unsigned)((entry >> 41) & 1u);
    if (sync != NULL)
        *sync = (unsigned)((entry >> 63) & 1u);
    if (fetch != NULL)
        *fetch = (unsigned)(entry & 1u);
    if (priv != NULL)
        *priv = (unsigned)((entry >> 40) & 1u);
}
