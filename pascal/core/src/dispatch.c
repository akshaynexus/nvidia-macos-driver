// SPDX-License-Identifier: GPL-2.0-only
/*
 * pascal/dispatch.c - GP106 compute-dispatch submission model.
 *
 * Builds, as ordered steps over a mock bus, everything one kernel launch
 * needs on the host side: the channel instance block (VMM join + RAMFC),
 * the runlist entry, the pushbuffer method program, and the GPFIFO entry
 * + USERD GP_PUT kick.
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
 * (NVK/Mesa, third_party/mesa:src/nouveau/headers/nvidia/classes/clc1c0qmd.h).
 * nouveau functions used (drivers/gpu/drm/nouveau/, Linux 6.18):
 *   nvkm/engine/fifo/gp100.c    gp100_runl_insert_chan()
 *   nvkm/engine/fifo/gk110.c    gk110_runl_insert_cgrp()
 *   nvkm/engine/fifo/gk104.c    gk104_chan_ramfc_write()
 *   nvkm/engine/fifo/chan.c     nvkm_chan_new_() (zero, join, ramfc order)
 *   nvkm/subdev/mmu/vmmgf100.c  gf100_vmm_join_()
 *   nvkm/subdev/mmu/vmmgp100.c  gp100_vmm_join()
 *   nvif/chan506f.c             gpfifo_push / gpfifo_kick (USERD+0x8c)
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

    /* nvkm_chan_new_ order: zeroed gf100_chan_inst (chan.c:409), VMM
     * join (chan.c:421), then ramfc->write (chan.c:475). */
    memset(inst, 0, PG106_SUB_INST_WORDS * sizeof(uint32_t));
    rc = pg106_sub_vmm_join(c, inst, rec);
    if (rc != PG106_SUB_OK)
        return rc;

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

/* gp100_vmm_join (vmmgp100.c:572-579) -> gf100_vmm_join_
 * (vmmgf100.c:342-363). */
int pg106_sub_vmm_join(const pg106_sub_chan *c,
                       uint32_t inst[PG106_SUB_INST_WORDS],
                       pg106_sub_rec *rec)
{
    if (c == NULL || inst == NULL)
        return PG106_SUB_ERR_ARG;
    if (c->pdb_addr == 0 || (c->pdb_addr & 0xFFFull) != 0)
        return PG106_SUB_ERR_RANGE;        /* page directories are 4K-aligned */
    if (c->vm_limit == 0 || c->vm_limit > PG106_MMU_VA_LIMIT)
        return PG106_SUB_ERR_RANGE;

    /* u64 base = BIT_ULL(10) VER2 | BIT_ULL(11) 64KiB (gp100_vmm_join) */
    uint64_t base = PG106_SUB_PDB_VER2 | PG106_SUB_PDB_BIG_PAGE_64K;
    switch (c->pdb_target) {             /* gf100_vmm_join_ :346-355 */
    case PG106_TARGET_VRAM:
        base |= 0u;
        break;
    case PG106_TARGET_SYS_COHERENT:
        base |= 2u | PG106_SUB_PDB_VOL;
        break;
    case PG106_TARGET_SYS_NONCOHERENT:
        base |= 3u;
        break;
    default:
        return PG106_SUB_ERR_RANGE;        /* WARN_ON(1); -EINVAL */
    }
    base |= c->pdb_addr;                  /* base |= pd->addr */
    const uint64_t limit = c->vm_limit - 1u;

    struct { uint32_t off; uint32_t val; const char *src; } wr[] = {
        { PG106_SUB_INST_PDB_LO, (uint32_t)base,
          "gf100_vmm_join_: wo64(inst,0x200,base) lo; base = pd addr | "
          "target | VER2 BIT(10) | 64KiB BIT(11) (gp100_vmm_join)" },
        { PG106_SUB_INST_PDB_HI, (uint32_t)(base >> 32),
          "gf100_vmm_join_: wo64(inst,0x200,base) hi" },
        { PG106_SUB_INST_VM_LIMIT_LO, (uint32_t)limit,
          "gf100_vmm_join_: wo64(inst,0x208,vmm->limit - 1) lo" },
        { PG106_SUB_INST_VM_LIMIT_HI, (uint32_t)(limit >> 32),
          "gf100_vmm_join_: wo64(inst,0x208,vmm->limit - 1) hi" },
    };
    for (size_t i = 0; i < sizeof(wr) / sizeof(wr[0]); i++) {
        inst[wr[i].off / 4u] = wr[i].val;
        if (rec != NULL &&
            pg106_sub_rec_push(rec, PG106_SUB_STEP_VMM_JOIN, wr[i].off,
                               wr[i].val, wr[i].src) != PG106_SUB_OK)
            return rec->error;
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

/* PASCAL_COMPUTE_B methods (clc1c0.h line numbers). */
#define NVC1C0_SET_SHADER_SHARED_MEMORY_WINDOW         0x0214u /* :213 */
#define NVC1C0_SEND_SIGNALING_PCAS_B                   0x02bcu /* :381 */
#define NVC1C0_SET_SHADER_LOCAL_MEMORY_NON_THROTTLED_A 0x02e4u /* :389 */
#define NVC1C0_SET_SHADER_LOCAL_MEMORY_THROTTLED_A     0x02f0u /* :398 */
#define NVC1C0_SET_SHADER_LOCAL_MEMORY_WINDOW          0x077cu /* :516 */
#define NVC1C0_SET_SHADER_LOCAL_MEMORY_A               0x0790u /* :519 */
#define NVC1C0_SET_PROGRAM_REGION_A                    0x1608u /* :665 */
/* SEND_SIGNALING_PCAS_B INVALIDATE 0:0 / SCHEDULE 1:1, both TRUE (:382,385). */
#define PCAS_B_INVALIDATE_SCHEDULE                     0x3u

/* 49-bit GPU VA: SET_PROGRAM_REGION_A / SET_SHADER_LOCAL_MEMORY_A
 * ADDRESS_UPPER are 16:0 (clc1c0.h:666, :520). */
#define SUB_VA_LIMIT (1ull << 49)

/*
 * Citation table for the recorder: every method header word we emit gets
 * the source line it reproduces; data words get the generic citation.
 */
#define SUB_MAX_MTHDS 16
typedef struct sub_emit {
    pg106_pb   *pb;
    size_t      hdr[SUB_MAX_MTHDS];
    const char *src[SUB_MAX_MTHDS];
    size_t      n;
} sub_emit;

static int sub_inc(sub_emit *e, unsigned subch, uint32_t mthd,
                   const uint32_t *data, unsigned count, const char *src)
{
    size_t at = e->pb->len;
    int rc = pg106_pb_inc(e->pb, subch, mthd, data, count);
    if (rc == PG106_PB_OK && e->n < SUB_MAX_MTHDS) {
        e->hdr[e->n] = at;
        e->src[e->n++] = src;
    }
    return rc;
}

static int sub_immd(sub_emit *e, unsigned subch, uint32_t mthd, uint32_t v,
                    const char *src)
{
    size_t at = e->pb->len;
    int rc = pg106_pb_immd(e->pb, subch, mthd, v);
    if (rc == PG106_PB_OK && e->n < SUB_MAX_MTHDS) {
        e->hdr[e->n] = at;
        e->src[e->n++] = src;
    }
    return rc;
}

static int sub_compute_init(sub_emit *e, unsigned subch,
                            const pg106_sub_compute_state *st)
{
    int rc;

    if (st->program_region >= SUB_VA_LIMIT || st->slm_addr >= SUB_VA_LIMIT)
        return PG106_SUB_ERR_RANGE;
    /* nvk_queue.c:221 assert(!(slm_per_tpc & 0x7fff)); NON_THROTTLED_A
     * SIZE_UPPER is 7:0 (clc1c0.h:390) -> < 2^40. */
    if (st->slm_addr != 0 &&
        ((st->slm_per_tpc & 0x7FFFull) != 0 || st->slm_per_tpc == 0 ||
         (st->slm_per_tpc >> 40) != 0))
        return PG106_SUB_ERR_RANGE;

    const uint32_t cls = PG106_CLASS_PASCAL_COMPUTE_B;
    if ((rc = sub_inc(e, subch, PG106_NVC1C0_SET_OBJECT, &cls, 1,
            "nvk_push_dispatch_state_init (nvk_cmd_dispatch.c:36): "
            "P_MTHD(NV90C0, SET_OBJECT) class_id = cls_compute 0xC1C0")))
        return rc;

    const uint32_t region[2] = { (uint32_t)(st->program_region >> 32),
                                 (uint32_t)st->program_region };
    if ((rc = sub_inc(e, subch, NVC1C0_SET_PROGRAM_REGION_A, region, 2,
            "nvk_cmd_dispatch.c:50-52 (cls < VOLTA): SET_PROGRAM_REGION_A/B "
            "= shader heap base hi/lo (clc1c0.h:665-669)")))
        return rc;
    if ((rc = sub_inc(e, subch, NVC1C0_SET_SHADER_LOCAL_MEMORY_WINDOW,
            &st->local_memory_window, 1,
            "nvk_cmd_dispatch.c:81-82: SET_SHADER_LOCAL_MEMORY_WINDOW "
            "(NVK 0xff << 24; clc1c0.h:516)")))
        return rc;
    if ((rc = sub_inc(e, subch, NVC1C0_SET_SHADER_SHARED_MEMORY_WINDOW,
            &st->shared_memory_window, 1,
            "nvk_cmd_dispatch.c:84-85: SET_SHADER_SHARED_MEMORY_WINDOW "
            "(NVK 0xfe << 24; clc1c0.h:213)")))
        return rc;

    if (st->slm_addr == 0)
        return PG106_PB_OK;

    const uint32_t slm[2] = { (uint32_t)(st->slm_addr >> 32),
                              (uint32_t)st->slm_addr };
    const uint32_t per_tpc[3] = { (uint32_t)(st->slm_per_tpc >> 32),
                                  (uint32_t)st->slm_per_tpc, 0xffu };
    if ((rc = sub_inc(e, subch, NVC1C0_SET_SHADER_LOCAL_MEMORY_A, slm, 2,
            "nvk_queue.c:225-227: SET_SHADER_LOCAL_MEMORY_A/B = SLM "
            "base hi/lo (clc1c0.h:519-522)")))
        return rc;
    if ((rc = sub_inc(e, subch, NVC1C0_SET_SHADER_LOCAL_MEMORY_NON_THROTTLED_A,
            per_tpc, 3,
            "nvk_queue.c:229-232: SET_SHADER_LOCAL_MEMORY_NON_THROTTLED_A/"
            "B/C = slm_per_tpc hi/lo, 0xff (clc1c0.h:389-396)")))
        return rc;
    return sub_inc(e, subch, NVC1C0_SET_SHADER_LOCAL_MEMORY_THROTTLED_A,
            per_tpc, 3,
            "nvk_queue.c:234-238 (cls < VOLTA): SET_SHADER_LOCAL_MEMORY_"
            "THROTTLED_A/B/C = slm_per_tpc hi/lo, 0xff (clc1c0.h:398-404)");
}

int pg106_sub_compute_init(pg106_pb *pb, unsigned subch,
                           const pg106_sub_compute_state *st)
{
    if (pb == NULL || st == NULL)
        return PG106_SUB_ERR_ARG;
    sub_emit e = { .pb = pb };
    return sub_compute_init(&e, subch, st);
}

int pg106_sub_launch(const pg106_sub_launch_params *p,
                     uint32_t *pb_words, size_t cap,
                     uint64_t *gpfifo_ring, uint32_t ring_entries,
                     uint32_t put,
                     pg106_sub_result *out,
                     pg106_sub_rec *rec)
{
    if (p == NULL || p->chan == NULL || p->qmd == NULL || out == NULL ||
        gpfifo_ring == NULL)
        return PG106_SUB_ERR_ARG;
    if (p->subch > PG106_DMA_SUBCHANNEL_MAX)
        return PG106_SUB_ERR_RANGE;

    int rc = sub_chan_check(p->chan);
    if (rc != PG106_SUB_OK)
        return rc;

    /* QMD identity: must be a Pascal v02_01 QMD (pascal/qmd.h). */
    if (pg106_qmd_validate(p->qmd) != PG106_QMD_OK)
        return PG106_SUB_ERR_QMD;

    /* SEND_PCAS_A carries addr >> 8 (clc1c0.h:374-375
     * QMD_ADDRESS_SHIFTED8 31:0): 256-byte aligned, < 2^40. */
    if ((p->qmd_addr & 0xFFu) != 0 || (p->qmd_addr >> 40) != 0)
        return PG106_SUB_ERR_RANGE;
    /* GPFIFO GET 31:2 + GET_HI 7:0: dword aligned, 40-bit. */
    if ((p->push_addr & 0x3u) != 0 ||
        p->push_addr >= PG106_SUB_PUSH_ADDR_LIMIT)
        return PG106_SUB_ERR_RANGE;
    /* The ring must be the one RAMFC 0x48/0x4c describes (gpfifo_len
     * bytes of 8-byte entries); nouveau wraps PUT with a power-of-two
     * mask, nvif_chan506f_gpfifo_push: cur = (cur + 1) & max
     * (chan506f.c:28). sub_chan_check already made gpfifo_len a power
     * of two. */
    if ((uint64_t)ring_entries * 8u != p->chan->gpfifo_len ||
        put >= ring_entries)
        return PG106_SUB_ERR_RANGE;

    /* Completion release: the QMD's RELEASE0, which the engine honours
     * only with SEMAPHORE_RELEASE_ENABLE0 set (clc1c0qmd.h:484). */
    const uint32_t rel_en =
        pg106_qmd_extract(p->qmd->w, PG106_QMD_SEMAPHORE_RELEASE_ENABLE0);
    const uint32_t rel_lo =
        pg106_qmd_extract(p->qmd->w, PG106_QMD_RELEASE0_ADDRESS_LOWER);
    const uint32_t rel_hi =
        pg106_qmd_extract(p->qmd->w, PG106_QMD_RELEASE0_ADDRESS_UPPER);
    const uint32_t rel_payload =
        pg106_qmd_extract(p->qmd->w, PG106_QMD_RELEASE0_PAYLOAD);
    if (!rel_en || (rel_lo | rel_hi) == 0u)
        return PG106_SUB_ERR_QMD;

    pg106_sub_result r;
    memset(&r, 0, sizeof(r));

    if (rec != NULL &&
        pg106_sub_rec_push(rec, PG106_SUB_STEP_QMD, p->qmd_addr, 256u,
                           "host writes the 256-byte v02_01 QMD image at "
                           "qmd_addr (nvk_cmd_upload_qmd, "
                           "nvk_cmd_dispatch.c:159-251: memcpy into the "
                           "upload buffer)") != PG106_SUB_OK)
        return rec->error;
    if (rec != NULL &&
        pg106_sub_rec_push(rec, PG106_SUB_STEP_RELEASE0,
                           ((uint64_t)rel_hi << 32) | rel_lo, rel_payload,
                           "QMD SEMAPHORE_RELEASE_ENABLE0 + RELEASE0_*: "
                           "engine writes RELEASE0_PAYLOAD (ONE_WORD) to "
                           "RELEASE0_ADDRESS at grid completion "
                           "(clc1c0qmd.h:484, 580-602)") != PG106_SUB_OK)
        return rec->error;

    pg106_pb pb;
    pg106_pb_init(&pb, pb_words, cap);
    sub_emit e = { .pb = &pb };

    if (p->init != NULL) {
        rc = sub_compute_init(&e, p->subch, p->init);
        if (rc != PG106_PB_OK)
            return rc;
    }

    /* nvk_CmdDispatchBase (nvk_cmd_dispatch.c:340-349). */
    const uint32_t pcas = (uint32_t)(p->qmd_addr >> 8);
    if ((rc = sub_inc(&e, p->subch, PG106_NVC1C0_SEND_PCAS_A, &pcas, 1,
            "nvk_cmd_dispatch.c:341-342: P_MTHD(NVA0C0, SEND_PCAS_A); "
            "P_NVA0C0_SEND_PCAS_A(p, qmd_addr >> 8) (clc1c0.h:374)")))
        return rc;
    if ((rc = sub_immd(&e, p->subch, NVC1C0_SEND_SIGNALING_PCAS_B,
            PCAS_B_INVALIDATE_SCHEDULE,
            "nvk_cmd_dispatch.c:344-348 (cls <= TURING): P_IMMD(NVA0C0, "
            "SEND_SIGNALING_PCAS_B, INVALIDATE_TRUE | SCHEDULE_TRUE) "
            "(clc1c0.h:381-385)")))
        return rc;
    r.pb_words = pb.len;
    if (pb.len > PG106_GP_ENTRY1_LENGTH_MAX)
        return PG106_SUB_ERR_RANGE;

    if (rec != NULL) {
        size_t m = 0;
        for (size_t i = 0; i < pb.len; i++) {
            const char *s = "method data word (clc06f.h NVC06F_DMA_* via "
                            "pascal/pushbuf.h)";
            if (m < e.n && e.hdr[m] == i)
                s = e.src[m++];
            if (pg106_sub_rec_push(rec, PG106_SUB_STEP_PB_WORD, i,
                                   pb_words[i], s) != PG106_SUB_OK)
                return rec->error;
        }
    }

    /* GPFIFO entry: GET = push_addr, LENGTH in dwords (nvif/chan506f.c
     * gpfifo_push packs size >> 2 at bit 10), LEVEL = MAIN, SYNC =
     * PROCEED, FETCH = UNCONDITIONAL. */
    r.gpfifo_entry = pg106_gpfifo_pack(p->push_addr, (uint32_t)pb.len,
                                       PG106_GP_LEVEL_MAIN,
                                       PG106_GP_SYNC_PROCEED,
                                       PG106_GP_FETCH_UNCONDITIONAL);
    gpfifo_ring[put] = r.gpfifo_entry;
    r.new_put = (put + 1u) & (ring_entries - 1u);

    if (rec != NULL) {
        if (pg106_sub_rec_push(rec, PG106_SUB_STEP_GPFIFO, put,
                               r.gpfifo_entry,
                               "NVC06F_GP_ENTRY0/1 (clc06f.h): FETCH 0:0, "
                               "GET 31:2, GET_HI 7:0, PRIV 8, LEVEL 9, "
                               "LENGTH 30:10, SYNC 31; nvif_chan506f_gpfifo_"
                               "push (chan506f.c:15-32)") != PG106_SUB_OK)
            return rec->error;
        if (pg106_sub_rec_push(rec, PG106_SUB_STEP_GP_PUT,
                               p->chan->userd_addr + PG106_SUB_USERD_GP_PUT,
                               r.new_put,
                               "kick: USERD+0x8c = new GP_PUT "
                               "(nvif_chan506f_gpfifo_kick, chan506f.c:11; "
                               "clc06f.h:62 GPPut)") != PG106_SUB_OK)
            return rec->error;
    }

    *out = r;
    return PG106_SUB_OK;
}
