// SPDX-License-Identifier: MIT
/*
 * pascal/pushbuf.h - GP106 pushbuffer method + GPFIFO entry encoders.
 *
 * Part of the portable Pascal (GP106M) core library. Pure C11, libc only,
 * no hardware access: these functions only pack/unpack words.
 *
 * Encoding source (golden reference, MIT licensed, saved in this repo):
 *   third_party/open-gpu-kernel-modules:src/common/sdk/nvidia/inc/class/clc06f.h
 *     - NVC06F_DMA_SEC_OP            31:29  (3 bits)
 *         1 = INC_METHOD, 3 = NON_INC_METHOD, 4 = IMMD_DATA_METHOD,
 *         7 = END_PB_SEGMENT   (NVC06F_DMA_SEC_OP_*)
 *     - NVC06F_DMA_METHOD_ADDRESS    11:0   (method >> 2 goes here)
 *     - NVC06F_DMA_METHOD_SUBCHANNEL 15:13
 *     - NVC06F_DMA_METHOD_COUNT      28:16  (also NVC06F_DMA_IMMD_DATA 28:16)
 *     - NVC06F_GP_ENTRY0_FETCH 0:0, NVC06F_GP_ENTRY0_GET 31:2
 *     - NVC06F_GP_ENTRY1_GET_HI 7:0, _PRIV 8:8, _LEVEL 9:9,
 *       _LENGTH 30:10, _SYNC 31:31
 *   third_party/open-gpu-kernel-modules:src/common/sdk/nvidia/inc/class/clc1c0.h
 *     - PASCAL_COMPUTE_B 0xC1C0, NVC1C0_SET_OBJECT 0x0000,
 *       NVC1C0_SEND_PCAS_A 0x02B4, NVC1C0_SEND_PCAS_B 0x02B8
 *
 * The tinygrad `typ << 28` form is intentionally NOT emitted; see
 * research/evidence/ENCODING-RESOLUTION.md for why SEC_OP is 3 bits at
 * [31:29] and why the shorthand happens to alias INC_METHOD.
 */
#ifndef PASCAL_PUSHBUF_H
#define PASCAL_PUSHBUF_H

#include <stddef.h>
#include <stdint.h>

/* SEC_OP values, verbatim from clc06f.h NVC06F_DMA_SEC_OP_* (31:29). */
#define PG106_SEC_OP_GRP0_USE_TERT      0u
#define PG106_SEC_OP_INC_METHOD         1u
#define PG106_SEC_OP_GRP2_USE_TERT      2u
#define PG106_SEC_OP_NON_INC_METHOD     3u
#define PG106_SEC_OP_IMMD_DATA_METHOD   4u
#define PG106_SEC_OP_ONE_INC            5u
#define PG106_SEC_OP_RESERVED6          6u
#define PG106_SEC_OP_END_PB_SEGMENT     7u

/* Field widths derived from clc06f.h bit ranges. */
#define PG106_DMA_METHOD_COUNT_MAX      0x1FFFu  /* NVC06F_DMA_METHOD_COUNT 28:16 */
#define PG106_DMA_SUBCHANNEL_MAX        0x7u     /* NVC06F_DMA_METHOD_SUBCHANNEL 15:13 */
/* NVC06F_DMA_METHOD_ADDRESS 11:0 (clc06f.h:233) holds mthd >> 2, so it is
 * 12 bits wide: dword index <= 0xFFF, byte method < 0x4000. Bit 12 of the
 * header is unused (SUBCHANNEL starts at 13). */
#define PG106_DMA_METHOD_ADDRESS_MAX    0x0FFFu
#define PG106_DMA_METHOD_LIMIT          0x4000u  /* first unencodable method */
#define PG106_GP_ENTRY1_LENGTH_MAX      0x1FFFFFu /* NVC06F_GP_ENTRY1_LENGTH 30:10 */

/* GPFIFO entry level / sync / fetch enums, from clc06f.h *_VALUE macros. */
#define PG106_GP_LEVEL_MAIN             0u  /* NVC06F_GP_ENTRY1_LEVEL_MAIN */
#define PG106_GP_LEVEL_SUBROUTINE       1u  /* NVC06F_GP_ENTRY1_LEVEL_SUBROUTINE */
#define PG106_GP_SYNC_PROCEED           0u  /* NVC06F_GP_ENTRY1_SYNC_PROCEED */
#define PG106_GP_SYNC_WAIT              1u  /* NVC06F_GP_ENTRY1_SYNC_WAIT */
#define PG106_GP_FETCH_UNCONDITIONAL    0u  /* NVC06F_GP_ENTRY0_FETCH_UNCONDITIONAL */
#define PG106_GP_FETCH_CONDITIONAL      1u  /* NVC06F_GP_ENTRY0_FETCH_CONDITIONAL */

/* Class IDs, from third_party/open-gpu-kernel-modules:src/common/sdk/nvidia/inc/class/clc1c0.h. */
#define PG106_CLASS_PASCAL_CHANNEL_GPFIFO_A 0xC06Fu
#define PG106_CLASS_PASCAL_COMPUTE_B        0xC1C0u

/* PASCAL_COMPUTE_B methods used by the submission path (clc1c0.h). */
#define PG106_NVC1C0_SET_OBJECT         0x0000u  /* NVC1C0_SET_OBJECT */
#define PG106_NVC1C0_SEND_PCAS_A        0x02B4u  /* NVC1C0_SEND_PCAS_A */
#define PG106_NVC1C0_SEND_PCAS_B        0x02B8u  /* NVC1C0_SEND_PCAS_B */

/* Status codes. Negative on failure, sticky in pg106_pb.error. */
#define PG106_PB_OK          0
#define PG106_PB_ERR_SPACE  (-1)   /* pushbuffer capacity exceeded */
#define PG106_PB_ERR_RANGE  (-2)   /* value does not fit its clc06f.h field */
#define PG106_PB_ERR_ARG    (-3)   /* NULL pointer or nonsensical argument */

/*
 * A pushbuffer writer over a caller-owned u32 array. Writes are
 * bounds-checked against `cap`; the first error is remembered in `error`
 * and every later write is a no-op returning that error, so callers can
 * batch a command stream and check once at the end.
 */
typedef struct pg106_pb {
    uint32_t *words;  /* caller-owned storage */
    size_t    cap;    /* capacity in u32 words */
    size_t    len;    /* words written so far */
    int       error;  /* PG106_PB_OK or the first error seen */
} pg106_pb;

/* Attach a writer to `words` (may be NULL/cap 0, e.g. to measure size). */
int pg106_pb_init(pg106_pb *pb, uint32_t *words, size_t cap);

/*
 * Build one method header word, masking each field into its clc06f.h
 * range: SEC_OP [31:29], COUNT [28:16], SUBCHANNEL [15:13], ADDRESS [11:0].
 * The API entry points below range-check before calling this; this raw
 * builder exists for tests and decoders and never fails.
 */
uint32_t pg106_pb_mthd_word(unsigned sec_op, unsigned subch,
                            uint32_t mthd, unsigned count);

/* Decode a method header word into its clc06f.h fields (any may be NULL). */
void pg106_pb_decode(uint32_t word, unsigned *sec_op, unsigned *count,
                     unsigned *subch, uint32_t *mthd);

/*
 * Increasing method: header + `count` data words (count 1..0x1FFF,
 * clc06f.h NVC06F_DMA_INCR_*). `data` must hold `count` words.
 */
int pg106_pb_inc(pg106_pb *pb, unsigned subch, uint32_t mthd,
                 const uint32_t *data, unsigned count);

/*
 * Non-increasing method: same header with SEC_OP = 3 (NVC06F_DMA_NONINCR_*).
 * `count` may be 0 (then `data` may be NULL): e.g. SEND_PCAS_A, where the
 * QMD address is delivered by a separate entry (see the simtest section 3).
 */
int pg106_pb_noninc(pg106_pb *pb, unsigned subch, uint32_t mthd,
                    const uint32_t *data, unsigned count);

/*
 * Immediate-data method (NVC06F_DMA_IMMD_*): the value rides in the
 * COUNT/DATA field [28:16] and no data words follow.
 */
int pg106_pb_immd(pg106_pb *pb, unsigned subch, uint32_t mthd, uint32_t data);

/*
 * SET_OBJECT: increasing method NVC1C0_SET_OBJECT (0x0000) whose single
 * data word is the class id, e.g. PG106_CLASS_PASCAL_COMPUTE_B (clc1c0.h).
 */
int pg106_pb_set_object(pg106_pb *pb, unsigned subch, uint32_t class_id);

/* Terminate a pushbuffer segment: one word, SEC_OP = 7. */
int pg106_pb_endseg(pg106_pb *pb);

/*
 * Pack one 64-bit GPFIFO entry (clc06f.h NVC06F_GP_ENTRY0/1):
 *   GET 33:2 (dword address of the pushbuffer segment, so `addr` must be
 *            4-byte aligned; bits [39:32] land in ENTRY1_GET_HI 7:0),
 *   FETCH 0:0, PRIV 8:8 (always 0/user from pack; see unpack),
 *   LEVEL 9:9, LENGTH 30:10 (DWORD COUNT of the pushbuffer segment, per
 *            clc06f.h and kernel nvif/chan506f.c gpfifo_push packing
 *            `size >> 2` — NOT bytes), SYNC 31:31.
 * Out-of-range fields are masked to their hardware widths, as the PBDMA
 * would see them.
 */
uint64_t pg106_gpfifo_pack(uint64_t addr, uint32_t len,
                           unsigned level, unsigned sync, unsigned fetch);

/* Inverse of pg106_gpfifo_pack; any output pointer may be NULL. */
void pg106_gpfifo_unpack(uint64_t entry, uint64_t *addr, uint32_t *len,
                         unsigned *level, unsigned *sync, unsigned *fetch,
                         unsigned *priv);

#endif /* PASCAL_PUSHBUF_H */
