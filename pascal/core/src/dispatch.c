// SPDX-License-Identifier: GPL-2.0-only
/*
 * pascal/dispatch.c - GP106 compute-dispatch submission model.
 *
 * Builds, as ordered steps over a mock bus, everything one kernel launch
 * needs on the host side: the channel instance (RAMFC) image, the runlist
 * entry, the pushbuffer method program, and the GPFIFO entry + doorbell.
 * See dispatch.h for the method program and its citations. SIMULATION
 * ONLY: nothing here touches hardware; the recorder exists so tests can
 * assert the exact sequence.
 *
 * Licensing: GPL-2.0-only. The runlist entry layout and the RAMFC field
 * values are borrowed from nouveau (drivers/gpu/drm/nouveau/nvkm/engine/
 * fifo/{gp100,gk110,gk104,chan/gf100}.c, torvalds/linux master, fetched
 * 2026-11-05 into /tmp/pascal-ref/nvkm-fifo/). The method encodings come
 * from MIT-licensed NVIDIA headers (third_party/open-gpu-kernel-modules:src/common/sdk/nvidia/inc/class/clc06f.h,
 * clc1c0.h) through the MIT pascal/pushbuf.h; the QMD builder is MIT
 * (NVK/Mesa, third_party/mesa:src/nouveau/headers/nvidia/classes/clc1c0qmd.h). Local copies of the
 * nouveau files used:
 *   /tmp/pascal-ref/nvkm-fifo/gp100.c   gp100_runl_insert_chan()
 *   /tmp/pascal-ref/nvkm-fifo/gk110.c   gk110_runl_insert_cgrp()
 *   /tmp/pascal-ref/nvkm-fifo/gk104.c   gk104_chan_ramfc_write()
 *   /tmp/pascal-ref/nvkm-fifo/gf100.c   gf100_chan_inst (block size)
 *   /tmp/pascal-ref/nvkm-fifo/gm107.c   gm107_chan (GP106's chan func)
 */
#include "pascal/dispatch.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* nvkm-derived helpers                                                */
/* ------------------------------------------------------------------ */

/* ilog2 of a nonzero power of two (ramfc GPFIFO limit field:
 * gk104_chan_ramfc_write: `limit2 = ilog2(length / 8)`). */
static int sub_ilog2(uint32_t v)
{
    int l = -1;
    while (v) {
        v >>= 1;
        l++;
    }
    return l;
}

static int sub_chan_check(const pg106_sub_chan *c)
{
    if (c == NULL)
        return PG106_SUB_ERR_ARG;
    if (c->runq >= 2u)                     /* gp100_runl .runqs = 2 */
        return PG106_SUB_ERR_RANGE;
    if (c->inst_addr == 0 || (c->inst_addr & 0xFFFull))
        return PG106_SUB_ERR_RANGE;        /* 0x1000-aligned instance block */
    if (c->gpfifo_len < 8u || (c->gpfifo_len & (c->gpfifo_len - 1u)))
        return PG106_SUB_ERR_RANGE;        /* power of two >= 8 entries*8B */
    return PG106_SUB_OK;
}

/* nvkm/engine/fifo/gp100.c gp100_runl_insert_chan():
 *   nvkm_wo32(memory, offset + 0, chan->id | chan->runq << 14);
 *   nvkm_wo32(memory, offset + 4, chan->inst->addr >> 12); */
int pg106_sub_runlist_chan(const pg106_sub_chan *c, uint32_t rl[2])
{
    int rc = sub_chan_check(c);
    if (rc != PG106_SUB_OK)
        return rc;
    if (rl == NULL)
        return PG106_SUB_ERR_ARG;
    rl[0] = c->chid | (c->runq << 14);
    rl[1] = (uint32_t)(c->inst_addr >> 12);
    return PG106_SUB_OK;
}

/* nvkm/engine/fifo/gk110.c gk110_runl_insert_cgrp():
 *   nvkm_wo32(memory, offset + 0, (cgrp->chan_nr << 26) | (128 << 18) |
 *                                 (3 << 14) | 0x00002000 | cgrp->id);
 *   nvkm_wo32(memory, offset + 4, 0x00000000); */
void pg106_sub_runlist_cgrp(uint32_t chan_nr, uint32_t cgrp_id, uint32_t rl[2])
{
    if (rl == NULL)
        return;
    rl[0] = (chan_nr << 26) | (128u << 18) | (3u << 14) | 0x00002000u |
            cgrp_id;
    rl[1] = 0x00000000u;
}

/* nvkm/engine/fifo/gk104.c gk104_chan_ramfc_write(), into the zeroed
 * 0x1000-byte instance block of gf100_chan_inst. */
int pg106_sub_ramfc_build(const pg106_sub_chan *c,
                          uint32_t inst[PG106_SUB_INST_WORDS],
                          pg106_sub_rec *rec)
{
    int rc = sub_chan_check(c);
    if (rc != PG106_SUB_OK)
        return rc;
    if (inst == NULL)
        return PG106_SUB_ERR_ARG;
    if ((c->devm & ~0xFFFu) != 0)
        return PG106_SUB_ERR_RANGE;        /* gk104_chan_ramfc .devm = 0xfff */

    memset(inst, 0, PG106_SUB_INST_WORDS * sizeof(uint32_t));

    const uint64_t gpfifo = c->gpfifo_off;
    const int      limit2 = sub_ilog2((uint32_t)(c->gpfifo_len / 8u));
    if (limit2 > 0xFFFF)
        return PG106_SUB_ERR_RANGE;

    /* Every offset below is gk104_chan_ramfc_write (nvkm gk104.c:70-91). */
    struct { uint32_t off; uint32_t val; const char *src; } wr[] = {
        { 0x08, (uint32_t)c->userd_addr,                       /* USERD lo */
          "gk104_chan_ramfc_write: wo32(inst,0x08,lower_32_bits(userd))" },
        { 0x0c, (uint32_t)(c->userd_addr >> 32),               /* USERD hi */
          "gk104_chan_ramfc_write: wo32(inst,0x0c,upper_32_bits(userd))" },
        { 0x10, 0x0000faceu,
          "gk104_chan_ramfc_write: wo32(inst,0x10,0x0000face)" },
        { 0x30, 0xfffff902u,
          "gk104_chan_ramfc_write: wo32(inst,0x30,0xfffff902)" },
        { 0x48, (uint32_t)gpfifo,                              /* GPFIFO lo */
          "gk104_chan_ramfc_write: wo32(inst,0x48,lower_32_bits(offset))" },
        { 0x4c, (uint32_t)(gpfifo >> 32) | ((uint32_t)limit2 << 16),
          "gk104_chan_ramfc_write: wo32(inst,0x4c,upper|(limit2<<16)),"
          " limit2=ilog2(length/8)" },
        { 0x84, 0x20400000u,
          "gk104_chan_ramfc_write: wo32(inst,0x84,0x20400000)" },
        { 0x94, 0x30000000u | c->devm,
          "gk104_chan_ramfc_write: wo32(inst,0x94,0x30000000|devm)" },
        { 0x9c, 0x00000100u,
          "gk104_chan_ramfc_write: wo32(inst,0x9c,0x00000100)" },
        { 0xac, 0x0000001fu,
          "gk104_chan_ramfc_write: wo32(inst,0xac,0x0000001f)" },
        { 0xe4, c->priv ? 0x00000020u : 0u,
          "gk104_chan_ramfc_write: wo32(inst,0xe4,priv?0x20:0)" },
        { 0xe8, c->chid,
          "gk104_chan_ramfc_write: wo32(inst,0xe8,chan->id)" },
        { 0xb8, 0xf8000000u,
          "gk104_chan_ramfc_write: wo32(inst,0xb8,0xf8000000)" },
        { 0xf8, 0x10003080u, /* 0x002310 */
          "gk104_chan_ramfc_write: wo32(inst,0xf8,0x10003080)" },
        { 0xfc, 0x10000010u, /* 0x002350 */
          "gk104_chan_ramfc_write: wo32(inst,0xfc,0x10000010)" },
    };

    for (size_t i = 0; i < sizeof(wr) / sizeof(wr[0]); i++) {
        inst[wr[i].off / 4u] = wr[i].val;
        if (rec != NULL) {
            rc = pg106_sub_rec_push(rec, PG106_SUB_STEP_RAMFC,
                                    wr[i].off, wr[i].val, wr[i].src);
            if (rc != PG106_SUB_OK)
                return rc;
        }
    }
    return PG106_SUB_OK;
}

/* ------------------------------------------------------------------ */
/* Mock bus recorder                                                   */
/* ------------------------------------------------------------------ */

int pg106_sub_rec_init(pg106_sub_rec *rec, pg106_sub_step *buf, size_t cap)
{
    if (rec == NULL)
        return PG106_SUB_ERR_ARG;
    rec->steps = buf;
    rec->cap = buf != NULL ? cap : 0;
    rec->len = 0;
    rec->error = PG106_SUB_OK;
    return PG106_SUB_OK;
}

int pg106_sub_rec_push(pg106_sub_rec *rec, pg106_sub_step_type type,
                       uint64_t addr, uint64_t value, const char *src)
{
    if (rec == NULL)
        return PG106_SUB_ERR_ARG;
    if (rec->error != PG106_SUB_OK)
        return rec->error;
    if (rec->len >= rec->cap) {
        rec->error = PG106_SUB_ERR_SPACE;  /* sticky, like the pushbuffer */
        return rec->error;
    }
    rec->steps[rec->len].type = type;
    rec->steps[rec->len].addr = addr;
    rec->steps[rec->len].value = value;
    rec->steps[rec->len].src = src;
    rec->len++;
    return PG106_SUB_OK;
}

/* ------------------------------------------------------------------ */
/* The launch submission model                                         */
/* ------------------------------------------------------------------ */

/* clc1c0.h NVC1C0_SEND_SIGNALING_PCAS_B_{INVALIDATE,SCHEDULE}_TRUE. */
#define PG106_PCAS_B_INVALIDATE_TRUE  0x1u
#define PG106_PCAS_B_SCHEDULE_TRUE    0x2u

/* QMD operand helpers: SEND_PCAS_A and SET_INLINE_QMD_ADDRESS carry the
 * address shifted right by 8 (clc1c0.h *_QMD_ADDRESS_SHIFTED8). */
static uint64_t sub_qmd_shifted8(uint64_t qmd_addr)
{
    return qmd_addr >> 8;
}

int pg106_sub_launch(const pg106_sub_launch_params *p,
                     uint32_t *pb_words, size_t cap,
                     uint64_t *gpfifo_ring, uint32_t ring_entries,
                     uint32_t put,
                     pg106_sub_result *out,
                     pg106_sub_rec *rec)
{
    if (p == NULL || p->chan == NULL || p->qmd == NULL || out == NULL)
        return PG106_SUB_ERR_ARG;
    if (p->subch > PG106_DMA_SUBCHANNEL_MAX)
        return PG106_SUB_ERR_RANGE;

    int rc = sub_chan_check(p->chan);
    if (rc != PG106_SUB_OK)
        return rc;

    /* QMD identity: must be a Pascal v01_07 QMD (pascal/qmd.h). */
    if (pg106_qmd_validate(p->qmd) != PG106_QMD_OK)
        return PG106_SUB_ERR_QMD;

    /* SEND_PCAS_A / SET_INLINE_QMD_ADDRESS operands are addr >> 8, so
     * the QMD must sit on a 256-byte boundary (clc1c0.h
     * NVC1C0_SEND_PCAS_A_QMD_ADDRESS_SHIFTED8 31:0). */
    if ((p->qmd_addr & 0xFFu) != 0)
        return PG106_SUB_ERR_RANGE;
    if ((p->push_addr & 0x3u) != 0)
        return PG106_SUB_ERR_RANGE;
    if (ring_entries == 0u || gpfifo_ring == NULL || put >= ring_entries)
        return PG106_SUB_ERR_RANGE;

    /* The semaphore release is the QMD's own RELEASE0: it must be armed
     * with a nonzero address (pg106_qmd.h RELEASE0_*). No pushbuffer
     * semaphore method is emitted for it - the engine writes it at
     * dispatch completion. */
    const uint32_t rel_lo =
        pg106_qmd_extract(p->qmd->w, PG106_QMD_RELEASE0_ADDRESS_LOWER);
    const uint32_t rel_hi =
        pg106_qmd_extract(p->qmd->w, PG106_QMD_RELEASE0_ADDRESS_UPPER);
    const uint32_t rel_payload =
        pg106_qmd_extract(p->qmd->w, PG106_QMD_RELEASE0_PAYLOAD);
    if ((rel_lo | rel_hi) == 0u)
        return PG106_SUB_ERR_QMD;

    pg106_sub_result r;
    memset(&r, 0, sizeof(r));

    /* Step 0: host DMA of the QMD image into the channel VM at
     * qmd_addr (one recorded step; the image itself is pg106_qmd). */
    if (rec != NULL &&
        pg106_sub_rec_push(rec, PG106_SUB_STEP_QMD, p->qmd_addr, 256u,
                           "host writes the 256-byte v01_07 QMD image at "
                           "qmd_addr (pascal/qmd.h; nvk_cmd_buffer_alloc_qmd "
                           "+ memcpy in nvk_cmd_upload_qmd)") != PG106_SUB_OK)
        return rec->error;

    if (rec != NULL &&
        pg106_sub_rec_push(rec, PG106_SUB_STEP_RELEASE0,
                           ((uint64_t)rel_hi << 32) | rel_lo, rel_payload,
                           "QMD RELEASE0 armed: engine writes RELEASE0_"
                           "PAYLOAD to RELEASE0_ADDRESS at dispatch "
                           "completion (clc1c0qmd.h NVC1C0_QMDV01_07_"
                           "RELEASE0_* via pascal/qmd.h)") != PG106_SUB_OK)
        return rec->error;

    /* The pushbuffer program, through the existing Phase 0 encoders. */
    pg106_pb pb;
    pg106_pb_init(&pb, pb_words, cap);

    /* 1. SET_OBJECT: bind class PASCAL_COMPUTE_B to the subchannel. */
    if (pg106_pb_set_object(&pb, p->subch, PG106_CLASS_PASCAL_COMPUTE_B)
            != PG106_PB_OK)
        return pb.error;

    /* 2+3. Inline QMD upload: address pair, then the 64 QMD words as
     * one non-increasing method (see dispatch.h step list). */
    const uint64_t s8 = sub_qmd_shifted8(p->qmd_addr);
    const uint32_t qmd_addr_pair[2] = {
        (uint32_t)(s8 >> 32),   /* _A: QMD_ADDRESS_SHIFTED8_UPPER */
        (uint32_t)s8,           /* _B: QMD_ADDRESS_SHIFTED8_LOWER */
    };
    if (pg106_pb_inc(&pb, p->subch, 0x0318u /* SET_INLINE_QMD_ADDRESS_A */,
                     qmd_addr_pair, 2u) != PG106_PB_OK)
        return pb.error;
    if (pg106_pb_noninc(&pb, p->subch,
                        0x0320u /* LOAD_INLINE_QMD_DATA(0) */,
                        p->qmd->w, PG106_QMD_WORDS) != PG106_PB_OK)
        return pb.error;

    /* 4. SEND_PCAS_A pointing at the memory-resident QMD. */
    const uint32_t pcas = (uint32_t)s8;
    if (pg106_pb_inc(&pb, p->subch, PG106_NVC1C0_SEND_PCAS_A,
                     &pcas, 1u) != PG106_PB_OK)
        return pb.error;

    /* 5. SEND_SIGNALING_PCAS_B: invalidate + schedule (NVK's post-
     * dispatch step for compute classes <= TURING). */
    if (pg106_pb_immd(&pb, p->subch, 0x02BCu /* SEND_SIGNALING_PCAS_B */,
                      PG106_PCAS_B_INVALIDATE_TRUE |
                      PG106_PCAS_B_SCHEDULE_TRUE) != PG106_PB_OK)
        return pb.error;

    /* 6. Terminate the pushbuffer segment (SEC_OP = 7). */
    if (pg106_pb_endseg(&pb) != PG106_PB_OK)
        return pb.error;
    r.pb_words = pb.len;

    if (rec != NULL) {
        static const char *const src_setobj =
            "NVC1C0_SET_OBJECT 0x0000, class 0xC1C0 (clc1c0.h); INC header "
            "per clc06f.h SEC_OP[31:29]=1, COUNT[28:16], SUBCH[15:13], "
            "ADDR[11:0]";
        static const char *const src_qmd_addr =
            "NVC1C0_SET_INLINE_QMD_ADDRESS_A 0x0318 (SHIFTED8_UPPER 31:0) / "
            "_B 0x031C (SHIFTED8_LOWER 31:0) (clc1c0.h)";
        static const char *const src_qmd_load =
            "NVC1C0_LOAD_INLINE_QMD_DATA(i) (0x0320+(i)*4), V 31:0 "
            "(clc1c0.h): 64 QMD words as a NON-INC method (SEC_OP=3)";
        static const char *const src_pcas_a =
            "NVC1C0_SEND_PCAS_A 0x02B4, QMD_ADDRESS_SHIFTED8 31:0 "
            "(clc1c0.h); NVK: P_MTHD(SEND_PCAS_A); "
            "P_NVA0C0_SEND_PCAS_A(p, qmd_addr >> 8) (nvk_cmd_dispatch.c)";
        static const char *const src_pcas_b =
            "NVC1C0_SEND_SIGNALING_PCAS_B 0x02BC IMMD, INVALIDATE 0:0 / "
            "SCHEDULE 1:1 both TRUE (clc1c0.h); nvk_cmd_dispatch.c emits "
            "this right after SEND_PCAS_A for cls <= TURING_COMPUTE_A";
        static const char *const src_endseg =
            "NVC06F_DMA_SEC_OP_END_PB_SEGMENT = 7 at [31:29] (clc06f.h)";
        static const char *const src_pb_generic =
            "clc06f.h NVC06F_DMA_* via pascal/pushbuf.h";

        /* Header-word indices in the program above: 0 SET_OBJECT,
         * 2 SET_INLINE_QMD_ADDRESS_A, 5 LOAD_INLINE_QMD_DATA(0),
         * 70 SEND_PCAS_A, 72 SEND_SIGNALING_PCAS_B, 73 END_PB_SEGMENT. */
        const size_t i_qmd_load = 5u;
        const size_t i_pcas_a = i_qmd_load + 1u + PG106_QMD_WORDS;
        const size_t i_pcas_b = i_pcas_a + 2u;
        for (size_t i = 0; i < pb.len; i++) {
            const char *s = src_pb_generic;
            if (i == 0)
                s = src_setobj;
            else if (i == 2)
                s = src_qmd_addr;
            else if (i == i_qmd_load)
                s = src_qmd_load;
            else if (i == i_pcas_a)
                s = src_pcas_a;
            else if (i == i_pcas_b)
                s = src_pcas_b;
            else if (i + 1u == pb.len)
                s = src_endseg;
            if (pg106_sub_rec_push(rec, PG106_SUB_STEP_PB_WORD, i,
                                   pb_words[i], s) != PG106_SUB_OK)
                return rec->error;
        }
    }

    /* GPFIFO entry: push the whole segment (clc06f.h NVC06F_GP_ENTRY*,
     * packed by pascal/pushbuf.h): GET = push_addr >> 2, LENGTH in DWORDS
     * (kernel nvif/chan506f.c packs size >> 2), LEVEL = MAIN,
     * SYNC = PROCEED, FETCH = UNCONDITIONAL. */
    const uint32_t seg_len = (uint32_t)pb.len * 4u;
    if (seg_len / 4u != pb.len)
        return PG106_SUB_ERR_RANGE;
    r.gpfifo_entry = pg106_gpfifo_pack(p->push_addr, pb.len,
                                       PG106_GP_LEVEL_MAIN,
                                       PG106_GP_SYNC_PROCEED,
                                       PG106_GP_FETCH_UNCONDITIONAL);

    gpfifo_ring[put] = r.gpfifo_entry;
    r.new_put = (put + 1u) % ring_entries;

    if (rec != NULL) {
        if (pg106_sub_rec_push(rec, PG106_SUB_STEP_GPFIFO, put,
                               r.gpfifo_entry,
                               "NVC06F_GP_ENTRY0/1 (clc06f.h): FETCH 0:0, "
                               "GET 31:2 (dword addr), GET_HI 7:0, PRIV 8, "
                               "LEVEL 9, LENGTH 30:10, SYNC 31 - packed by "
                               "pascal/pushbuf.h pg106_gpfifo_pack") !=
            PG106_SUB_OK)
            return rec->error;
        if (pg106_sub_rec_push(rec, PG106_SUB_STEP_DOORBELL,
                               PG106_SUB_DOORBELL_OFFSET, r.new_put,
                               "GPFIFO doorbell at BAR0+0x90, value = new "
                               "PUT index (HELIOS300-PASCAL-RESEARCH.md "
                               "sec. 6, tinygrad ops_nv.py; cross-check "
                               "against the PBDMA doorbell register "
                               "before hardware use)") != PG106_SUB_OK)
            return rec->error;
    }

    *out = r;
    return PG106_SUB_OK;
}
