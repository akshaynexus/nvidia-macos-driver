// SPDX-License-Identifier: GPL-2.0-only
/*
 * pascal/dispatch.h - GP106 compute-dispatch submission model.
 *
 * Turns one kernel launch into a complete GPFIFO submission: the channel
 * instance (RAMFC) image, the runlist entry, the pushbuffer method program
 * (SET_OBJECT -> QMD upload -> SEND_PCAS_A -> scheduling PCAS_B), and the
 * GPFIFO entry + doorbell. Everything is modeled as ordered steps over a
 * mock bus recorder; nothing here touches hardware. **SIMULATION ONLY.**
 *
 * Licensing: this module reproduces field layouts (and a few constant
 * values) from nouveau, which is GPL-2.0-only:
 *   - runlist entry layout:  nvkm/engine/fifo/gp100.c gp100_runl_insert_chan()
 *   - TSG runlist entry:     nvkm/engine/fifo/gk110.c gk110_runl_insert_cgrp()
 *   - RAMFC field values:    nvkm/engine/fifo/gk104.c gk104_chan_ramfc_write()
 *     (GP106 wires fifo/gp100.c `.chan = &gm107_chan`, whose `.ramfc` is
 *     gk104_chan_ramfc; the instance block is `gf100_chan_inst`: 0x1000
 *     zeroed bytes, VMM-joined.)
 * The method encodings themselves come from MIT-licensed NVIDIA headers
 * (research/reference/clc06f.h, clc1c0.h) via the MIT pascal/pushbuf.h,
 * and the QMD builder (pascal/qmd.h) is MIT (NVK/Mesa). Those remain
 * separable; only the runlist/RAMFC sections make this file GPL-2.0-only.
 *
 * Method sequence emitted per launch (subch = the subchannel bound to
 * PASCAL_COMPUTE_B, all offsets from research/reference/clc1c0.h):
 *
 *   1. SET_OBJECT (0x0000, INC, 1 word = class 0xC1C0)
 *      NVC1C0_SET_OBJECT / NVC1C0_SET_OBJECT_CLASS_ID 15:0.
 *   2. SET_INLINE_QMD_ADDRESS_A/B (0x0318/0x031C, INC, 2 words =
 *      QMD_ADDRESS_SHIFTED8 upper then lower, i.e. qmd_addr >> 8)
 *      NVC1C0_SET_INLINE_QMD_ADDRESS_A_QMD_ADDRESS_SHIFTED8_UPPER 31:0,
 *      NVC1C0_SET_INLINE_QMD_ADDRESS_B_QMD_ADDRESS_SHIFTED8_LOWER 31:0.
 *   3. LOAD_INLINE_QMD_DATA(0) (0x0320, NON-INC, 64 words = the raw
 *      256-byte QMD v01_07 image from pascal/qmd.h)
 *      NVC1C0_LOAD_INLINE_QMD_DATA(i) (0x0320+(i)*4), NVC1C0_LOAD_INLINE_QMD_DATA_V 31:0.
 *   4. SEND_PCAS_A (0x02B4, INC, 1 word = qmd_addr >> 8)
 *      NVC1C0_SEND_PCAS_A_QMD_ADDRESS_SHIFTED8 31:0. This is exactly the
 *      NVK dispatch sequence, src/nouveau/vulkan/nvk_cmd_dispatch.c
 *      "P_MTHD(p, NVA0C0, SEND_PCAS_A); P_NVA0C0_SEND_PCAS_A(p, qmd_addr >> 8)".
 *   5. SEND_SIGNALING_PCAS_B (0x02BC, IMMD, INVALIDATE=TRUE|SCHEDULE=TRUE)
 *      NVC1C0_SEND_SIGNALING_PCAS_B_INVALIDATE 0:0 / _SCHEDULE 1:1; NVK
 *      emits this immediately after SEND_PCAS_A for compute classes
 *      <= TURING_COMPUTE_A (nvk_cmd_dispatch.c, "P_IMMD(p, NVA0C0,
 *      SEND_SIGNALING_PCAS_B, { .invalidate = INVALIDATE_TRUE,
 *      .schedule = SCHEDULE_TRUE })").
 *   6. END_PB_SEGMENT (SEC_OP = 7, clc06f.h NVC06F_DMA_SEC_OP_END_PB_SEGMENT).
 *
 * The QMD's own RELEASE0 fields (pascal/qmd.h
 * PG106_QMD_RELEASE0_*) are the semaphore release: the compute engine
 * writes RELEASE0_PAYLOAD to RELEASE0_ADDRESS when the dispatch
 * completes. The model verifies the fields are armed and records a
 * RELEASE0 step; it does not (and must not) also emit a pushbuffer
 * semaphore method for that release.
 */
#ifndef PASCAL_DISPATCH_H
#define PASCAL_DISPATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pascal/pushbuf.h"
#include "pascal/qmd.h"

struct pg106_sub_rec;   /* defined below */

/* Status codes; negative on failure. */
#define PG106_SUB_OK          0
#define PG106_SUB_ERR_ARG     (-3)  /* NULL pointer / nonsensical argument */
#define PG106_SUB_ERR_RANGE   (-2)  /* value does not fit its field */
#define PG106_SUB_ERR_SPACE   (-1)  /* pushbuffer / recorder / ring full */
#define PG106_SUB_ERR_QMD     (-5)  /* QMD not valid v01_07 or RELEASE0 unarmed */

/* Channel instance block: 0x1000 bytes == 1024 u32 words
 * (nvkm chan/gf100.c gf100_chan_inst: .size = 0x1000, .zero = true). */
#define PG106_SUB_INST_WORDS  1024u

/* GPFIFO doorbell MMIO offset from BAR0: HELIOS300-PASCAL-RESEARCH.md
 * section 6 ("doorbell at BAR0+0x90, tinygrad ops_nv.py reference
 * implementation"). Cross-check against the PBDMA GPFIFO doorbell
 * register before real hardware use. */
#define PG106_SUB_DOORBELL_OFFSET 0x90u

/*
 * One channel of the FIFO, as the submission path needs it. Field
 * semantics follow nvkm_chan (nvkm/engine/fifo/chan.c nvkm_chan_new_()):
 * the kernel assigns chid (nvkm_chid_get), the instance block is a
 * 0x1000-byte VMM-joined allocation, USERD is per-channel
 * (gk104_chan_userd: .bar = NVKM_BAR1_FB, .size = 0x200), and the GPFIFO
 * ring is a caller-allocated VM range passed as offset/length to
 * ramfc->write.
 */
typedef struct pg106_sub_chan {
    uint32_t chid;        /* channel id (chan->id) */
    uint32_t runq;        /* PBDMA index; GP100+ has 2 (gp100_runl .runqs = 2) */
    uint64_t inst_addr;   /* channel instance block GPU address, 0x1000-aligned */
    uint64_t userd_addr;  /* USERD base address (gk104_chan_ramfc_write 0x08/0x0c) */
    uint64_t gpfifo_off;  /* GPFIFO ring byte offset in the channel VM (0x48) */
    uint64_t gpfifo_len;  /* GPFIFO ring length in bytes, power of two >= 8 (0x4c) */
    uint32_t devm;        /* device map bits, mask 0xfff (gk104_chan_ramfc .devm) */
    bool     priv;        /* privileged channel (0xe4 = 0x20 when set) */
} pg106_sub_chan;

/*
 * Runlist (RAMRL) entry formats, 8 bytes each:
 *  - plain channel, GP100+ (nvkm/engine/fifo/gp100.c gp100_runl_insert_chan):
 *      word0 = chid | (runq << 14)
 *      word1 = inst_addr >> 12
 *    Note there is NO engine mask in the GP100+ channel entry: engine
 *    binding is per-subchannel via SET_OBJECT (the older engine-id form
 *    is the TSG entry below).
 *  - channel group / TSG (nvkm/engine/fifo/gk110.c gk110_runl_insert_cgrp):
 *      word0 = (chan_nr << 26) | (128 << 18) | (3 << 14) | 0x00002000 | cgrp_id
 *      word1 = 0
 */
int pg106_sub_runlist_chan(const pg106_sub_chan *c, uint32_t rl[2]);
void pg106_sub_runlist_cgrp(uint32_t chan_nr, uint32_t cgrp_id, uint32_t rl[2]);

/*
 * Channel instance (RAMFC) image. Writes exactly the fields of
 * nvkm/engine/fifo/gk104.c gk104_chan_ramfc_write() into a zeroed
 * 0x1000-byte block (offsets are byte addresses / 4 = word index):
 *   0x08/0x0c USERD address         0x30 0xfffff902
 *   0x48     GPFIFO offset (lo32)   0x4c  GPFIFO offset (hi32) | ilog2(len/8)<<16
 *   0x84     0x20400000             0x94  0x30000000 | devm
 *   0x9c     0x00000100             0xac  0x0000001f
 *   0xb8     0xf8000000             0xe4  priv ? 0x20 : 0
 *   0xe8     chid                   0xf8  0x10003080   0xfc  0x10000010
 * `inst` must hold PG106_SUB_INST_WORDS and is zeroed here first.
 * Also validates: inst_addr 0x1000-aligned, gpfifo_len a power of two
 * >= 8, runq < 2. If `rec` is non-NULL, records one RAMFC step per
 * written offset with its citation.
 */
int pg106_sub_ramfc_build(const pg106_sub_chan *c,
                          uint32_t inst[PG106_SUB_INST_WORDS],
                          struct pg106_sub_rec *rec);


/* Ordered steps over a mock bus (the test recorder). */
typedef enum pg106_sub_step_type {
    PG106_SUB_STEP_RAMFC     = 1, /* channel instance block word write  */
    PG106_SUB_STEP_RUNLIST   = 2, /* RAMRL entry half (addr = 0 or 4)   */
    PG106_SUB_STEP_QMD       = 3, /* QMD image upload (host DMA to VM)  */
    PG106_SUB_STEP_RELEASE0  = 4, /* QMD RELEASE0 armed (sem release)   */
    PG106_SUB_STEP_PB_WORD   = 5, /* pushbuffer word (addr = word index)*/
    PG106_SUB_STEP_GPFIFO    = 6, /* GPFIFO entry write / packed entry  */
    PG106_SUB_STEP_DOORBELL  = 7, /* doorbell ring, value = new PUT     */
} pg106_sub_step_type;

typedef struct pg106_sub_step {
    pg106_sub_step_type type;
    uint64_t    addr;   /* MMIO offset / VM address / array index */
    uint64_t    value;  /* word, entry half, or new PUT index */
    const char *src;    /* citation for this step; never NULL */
} pg106_sub_step;

typedef struct pg106_sub_rec {
    pg106_sub_step *steps;  /* caller-owned storage (may be NULL/cap 0) */
    size_t len;
    size_t cap;
    int    error;           /* first PG106_SUB_ERR_SPACE seen, else OK */
} pg106_sub_rec;

int pg106_sub_rec_init(pg106_sub_rec *rec, pg106_sub_step *buf, size_t cap);
int pg106_sub_rec_push(pg106_sub_rec *rec, pg106_sub_step_type type,
                       uint64_t addr, uint64_t value, const char *src);

/*
 * A kernel launch: everything pg106_sub_launch() needs. The QMD must
 * come from pg106_qmd_build() (v01_07) with RELEASE0 armed when a
 * semaphore release is wanted. `qmd_addr` is the QMD's address in the
 * channel VM and must be 256-byte aligned (SEND_PCAS_A carries
 * qmd_addr >> 8). `push_addr` is the pushbuffer segment's GPU address
 * (4-byte aligned). `put` is the GPFIFO ring PUT index before the
 * launch; `gpfifo_ring`/`ring_entries` describe the ring whose byte
 * offset is chan->gpfifo_off.
 */
typedef struct pg106_sub_launch_params {
    const pg106_sub_chan *chan;
    unsigned              subch;      /* subchannel bound to PASCAL_COMPUTE_B */
    const pg106_qmd      *qmd;
    uint64_t              qmd_addr;
    uint64_t              push_addr;
} pg106_sub_launch_params;

typedef struct pg106_sub_result {
    size_t   pb_words;     /* pushbuffer words written (incl. END_PB_SEGMENT) */
    uint64_t gpfifo_entry; /* packed NVC06F_GP_ENTRY0/1 for this launch */
    uint32_t new_put;      /* ring PUT index after appending the entry */
} pg106_sub_result;

/*
 * pg106_sub_launch(): build the complete submission for one kernel
 * launch. Emits the method program of the module header comment into
 * `pb` (capacity `cap` words), appends the packed GPFIFO entry to
 * `gpfifo_ring` at `put` (ring of `ring_entries` entries), records every
 * step with citations, and fills `out`. Returns the first error
 * (sticky): PG106_SUB_ERR_QMD for a bad QMD, PG106_SUB_ERR_RANGE for an
 * unaligned address, PG106_SUB_ERR_* propagated from the pushbuffer
 * writer, PG106_SUB_ERR_SPACE when pb / recorder / ring are full.
 */
int pg106_sub_launch(const pg106_sub_launch_params *p,
                     uint32_t *pb, size_t cap,
                     uint64_t *gpfifo_ring, uint32_t ring_entries,
                     uint32_t put,
                     pg106_sub_result *out,
                     pg106_sub_rec *rec);

#endif /* PASCAL_DISPATCH_H */
