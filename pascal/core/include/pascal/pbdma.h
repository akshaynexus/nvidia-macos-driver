// SPDX-License-Identifier: MIT
/*
 * pascal/pbdma.h - software model of the GP106 PBDMA (pushbuffer DMA),
 * the hardware unit that walks the GPFIFO ring + pushbuffer and produces
 * the method/data stream consumed by the engines.
 *
 * Part of the portable Pascal (GP106M) core library. Pure C11, libc only,
 * no hardware access. This is the CONSUMPTION-side counterpart of
 * pascal/pushbuf.h (encoders) and pascal/dispatch.h (full launches): the
 * decoder validates that everything our encoders emit is what real Host
 * hardware would actually read.
 *
 * Semantics derived from (all read-only references, no code copied):
 *   - NVIDIA open-gpu-doc "Host Pushbuffer Format (FIFO_DMA)" chapter in
 *     manuals/volta/gv100/dev_ram.ref.txt (Pascal has no published
 *     dev_pbdma/dev_ram ref; the FIFO_DMA format is byte-identical to
 *     clc06f.h, which IS the Pascal class header):
 *       * SEC_OP/TERT_OP instruction table (secondary 000+tertiary 01/10/11
 *         = subdevice-mask control entries; 001 INC; 011 NON-INC;
 *         100 IMMD; 101 ONE_INC; 111 END_PB_SEGMENT),
 *       * COUNT=0 => "the other fields are ignored, and the PB method
 *         effectively becomes a no-op" (INC, NON-INC and ONE-INC sections),
 *       * END_PB_SEGMENT => "No further PB entries from that PB segment
 *         will be decoded or processed",
 *       * "the PB method data entries may be located in a PB segment
 *         separate from their corresponding method header" (data may cross
 *         a GP-entry boundary; a segment with no END_PB_SEGMENT is legal).
 *   - NVIDIA open-gpu-doc manuals/volta/gv100/dev_pbdma.ref.txt:
 *       * GP entry processing: LENGTH==0 control entries with OPCODE
 *         NOP/GP_CRC/PB_CRC, OPCODE_ILLEGAL/invalid => NV_PPBDMA_INTR_0_
 *         GPENTRY, "the invalid GP entry is discarded... continue with the
 *         next GP entry",
 *       * NV_PPBDMA_INTR_0_PBENTRY conditions (a PB entry that "does not
 *         decode properly into a PB instruction" incl. SEC_OP 6 = RESERVED;
 *         an INC/ONE_INC header whose COUNT would wrap the method address),
 *       * NV_PPBDMA_INTR_0_PBSEG: a method sequence "that begins in a
 *         non-conditionally fetched PB segment and ends in a
 *         conditionally-fetched PB segment",
 *       * NV_PPBDMA_SUBDEVICE filtering (SET/STORE/USE_SUBDEVICE_MASK).
 *   - clc06f.h field names: NVC06F_DMA_SEC_OP 31:29, TERT_OP 17:16,
 *     COUNT 28:16, legacy COUNT_OLD 28:18 / ADDRESS_OLD 12:2 /
 *     OPCODE3 17:16, NVC06F_GP_ENTRY0/1 (third_party/open-gpu-kernel-modules:src/common/sdk/nvidia/inc/class/clc06f.h).
 *
 * Error model mirrors the hardware interrupt conditions listed above:
 * PBENTRY and fetch faults stop the run (hardware stalls); GPENTRY and
 * PBSEG are recorded (sticky) but the run continues, exactly as the ref
 * describes the hardware continuing after those interrupts.
 */
#ifndef PASCAL_PBDMA_H
#define PASCAL_PBDMA_H

#include <stddef.h>
#include <stdint.h>

#include "pascal/pushbuf.h"

/* Status codes. Negative on failure. */
#define PG106_PBDMA_OK           0
#define PG106_PBDMA_ERR_ARG    (-1) /* NULL pointer / bad argument        */
#define PG106_PBDMA_ERR_FETCH  (-2) /* pushbuffer reader fault (stall)    */
#define PG106_PBDMA_ERR_PBENTRY (-3)/* invalid PB instruction, SEC_OP 6,  */
                                    /* or method-address wrap (INTR_0_18) */
#define PG106_PBDMA_ERR_GPENTRY (-4)/* invalid GP control entry (INTR_0_  */
                                    /* GPENTRY; hardware skips + continues)*/
#define PG106_PBDMA_ERR_PBSEG  (-5) /* method seq crossed into a FETCH_   */
                                    /* CONDITIONAL segment (INTR_0_30)    */
#define PG106_PBDMA_ERR_OVERRUN (-6)/* method data past the last GPFIFO   */
                                    /* segment (PBDMA would stall on fetch)*/

/* Trace entry flags. */
#define PG106_PBDMA_FLAG_LEGACY_TERT (1u << 0) /* decoded via SEC_OP 0/2  */
                                /* tertiary path (TERT_OP [17:16]); this   */
                                /* is where a tinygrad `typ=1` corrupt     */
                                /* word (1<<28) lands                      */
#define PG106_PBDMA_FLAG_COUNT0_NOP  (1u << 1) /* COUNT=0: hardware no-op, */
                                /* "the other fields are ignored" (ref)    */
#define PG106_PBDMA_FLAG_CONTROL     (1u << 2) /* SET/STORE/USE_SUBDEVICE_ */
                                /* MASK PB control entry                   */
#define PG106_PBDMA_FLAG_ENDSEG      (1u << 3) /* END_PB_SEGMENT control   */
#define PG106_PBDMA_FLAG_GP_CONTROL  (1u << 4) /* LENGTH==0 GP control entry*/
#define PG106_PBDMA_FLAG_SEG_CROSS   (1u << 5) /* this method's data       */
                                /* continued into following segment(s)     */
#define PG106_PBDMA_FLAG_SEG_SKIPPED (1u << 6) /* conditional segment while */
                                /* subdevice-filtered INACTIVE: not decoded*/

/*
 * Pushbuffer fetch callback. `dword_addr` is the pushbuffer address in
 * dword units (GP_ENTRY GET 33:2 << ... i.e. byte_addr >> 2). Returns 0
 * and writes *out on success; any nonzero value models a fetch fault
 * (PBDMA MEMREQ error => the run stops with PG106_PBDMA_ERR_FETCH).
 */
typedef int (*pg106_pbdma_reader_fn)(void *ctx, uint64_t dword_addr,
                                     uint32_t *out);

typedef struct pg106_pbdma_reader {
    pg106_pbdma_reader_fn read; /* never NULL */
    void *ctx;                  /* passed back to read() */
} pg106_pbdma_reader;

/* One decoded PB instruction (method header or control entry). */
typedef struct pg106_pbdma_entry {
    uint32_t gpfifo_index; /* ring slot this segment came from              */
    uint32_t pb_offset;    /* dword offset of the instruction in its segment*/
    uint8_t  secop;        /* raw NVC06F_DMA_SEC_OP 31:29                   */
    uint8_t  tert_op;      /* NVC06F_DMA_TERT_OP 17:16 (legacy forms)       */
    uint8_t  subch;        /* NVC06F_DMA_METHOD_SUBCHANNEL 15:13            */
    uint8_t  count;        /* low 8 bits of COUNT 28:16 (16 for QCQ checks) */
    uint32_t count_raw;    /* full COUNT field: modern 28:16, legacy 28:18  */
    uint32_t mthd;         /* method byte address (COUNT field == IMMD data) */
    uint32_t flags;        /* PG106_PBDMA_FLAG_*                            */
    uint32_t data_offset;  /* offset of this method's data in the pool      */
    uint32_t data_len;     /* data words consumed/generated (see notes)     */
} pg106_pbdma_entry;

/* data_len notes: INC/NON-INC/ONE_INC = COUNT data words (COUNT=0 => 0);
 * IMMD = 1 (the immediate value rides in the header's [28:16] and is
 * copied into the pool so reconstruction is uniform); control entries,
 * ENDSEG and GP control entries = 0. */

/*
 * The decoded trace. Caller owns all storage. `data_pool` receives every
 * method's data words back-to-back (entries reference it by offset/len),
 * so a trace is self-contained for memcmp-style reconstruction checks.
 * `error` is the FIRST error seen (sticky); GPENTRY/PBSEG errors are
 * recorded there but do not stop the run, mirroring the hardware.
 */
typedef struct pg106_pbdma_trace {
    pg106_pbdma_entry *entries; /* caller-owned storage (may be NULL/cap 0) */
    size_t len;
    size_t cap;
    uint32_t *data_pool;        /* caller-owned storage (may be NULL/cap 0) */
    size_t pool_len;
    size_t pool_cap;
    int    error;               /* first PG106_PBDMA_ERR_*, else OK         */
    uint32_t error_gpfifo;      /* ring slot index where it happened        */
    uint32_t error_word;        /* HDR_SHADOW analog: raw word/GP entry lo  */
} pg106_pbdma_trace;

/* Attach a trace to caller-owned storage (either buffer may be NULL/0). */
int pg106_pbdma_trace_init(pg106_pbdma_trace *t,
                           pg106_pbdma_entry *entries, size_t cap,
                           uint32_t *data_pool, size_t pool_cap);

/*
 * Decode a full GPFIFO ring: entries 0..gpfifo_count-1, each unpacked with
 * pg106_gpfifo_unpack() and its pushbuffer segment decoded in order.
 * Returns PG106_PBDMA_OK, PG106_PBDMA_ERR_ARG, or the first fatal error
 * (FETCH/PBENTRY/OVERRUN; GPENTRY/PBSEG only appear in trace->error).
 */
int pg106_pbdma_run(const uint64_t *gpfifo_ring, uint32_t gpfifo_count,
                    const pg106_pbdma_reader *reader,
                    pg106_pbdma_trace *out_trace);

/*
 * GET/PUT form with ring wrap: decodes the (put - get) mod ring_entries
 * entries starting at slot `get` (PUT == GET means empty, as in hardware:
 * NV_PPBDMA_GP_PUT == NV_PPBDMA_GP_GET is the idle condition). This is
 * how a real channel runs: userspace publishes entries at PUT and the
 * PBDMA chases GET up to PUT, wrapping the ring.
 */
int pg106_pbdma_run_getput(const uint64_t *gpfifo_ring, uint32_t ring_entries,
                           uint32_t get, uint32_t put,
                           const pg106_pbdma_reader *reader,
                           pg106_pbdma_trace *out_trace);

#endif /* PASCAL_PBDMA_H */
