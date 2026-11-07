// SPDX-License-Identifier: GPL-2.0-only
/*
 * pascal/dispatch.h - GP106 compute-dispatch submission model.
 *
 * Turns one kernel launch into a complete GPFIFO submission: the channel
 * instance block (VMM join + RAMFC), the runlist entry, the pushbuffer
 * method program, the GPFIFO entry and the GP_PUT kick. Everything is
 * modeled as ordered steps over a mock bus recorder; nothing here touches
 * hardware. **SIMULATION ONLY.**
 *
 * Licensing: this module reproduces field layouts (and a few constant
 * values) from nouveau, which is GPL-2.0-only:
 *   - runlist entry layout:  nvkm/engine/fifo/gp100.c gp100_runl_insert_chan()
 *   - TSG runlist entry:     nvkm/engine/fifo/gk110.c gk110_runl_insert_cgrp()
 *   - RAMFC field values:    nvkm/engine/fifo/gk104.c gk104_chan_ramfc_write()
 *     (GP106 wires fifo/gp100.c `.chan = &gm107_chan`, whose `.ramfc` is
 *     gk104_chan_ramfc; the instance block is `gf100_chan_inst`: 0x1000
 *     zeroed bytes, VMM-joined.)
 *   - VMM join:              nvkm/subdev/mmu/vmmgf100.c gf100_vmm_join_()
 *     with gp100_vmm_join()'s base bits (vmmgp100.c:572-579).
 * The method encodings come from MIT-licensed NVIDIA headers
 * (third_party/open-gpu-kernel-modules:src/common/sdk/nvidia/inc/class/
 * clc06f.h, clc1c0.h) via the MIT pascal/pushbuf.h, and the QMD builder
 * (pascal/qmd.h) is MIT. Those remain separable; only the runlist/RAMFC/
 * join sections make this file GPL-2.0-only.
 *
 * Pushbuffer program (subch = the subchannel bound to PASCAL_COMPUTE_B;
 * method offsets from clc1c0.h). It is NVK's, method for method:
 *
 *   one-time compute state, when pg106_sub_launch_params.init != NULL
 *   (nvk_push_dispatch_state_init, nvk_cmd_dispatch.c:32-89, the
 *   cls_compute < VOLTA_COMPUTE_A branches; then nvk_queue.c:224-238):
 *     SET_OBJECT                        0x0000  class 0xC1C0
 *     SET_PROGRAM_REGION_A/B            0x1608  code base hi, lo
 *     SET_SHADER_LOCAL_MEMORY_WINDOW    0x077c  local window (NVK 0xff<<24)
 *     SET_SHADER_SHARED_MEMORY_WINDOW   0x0214  shared window (NVK 0xfe<<24)
 *     and, when slm_addr != 0:
 *     SET_SHADER_LOCAL_MEMORY_A/B       0x0790  SLM base hi, lo
 *     SET_SHADER_LOCAL_MEMORY_NON_THROTTLED_A/B/C 0x02e4  per-TPC hi, lo, 0xff
 *     SET_SHADER_LOCAL_MEMORY_THROTTLED_A/B/C     0x02f0  per-TPC hi, lo, 0xff
 *
 *   per launch (nvk_CmdDispatchBase, nvk_cmd_dispatch.c:340-349):
 *     SEND_PCAS_A                       0x02b4  qmd_addr >> 8
 *     SEND_SIGNALING_PCAS_B (IMMD)      0x02bc  INVALIDATE_TRUE | SCHEDULE_TRUE
 *
 * The QMD itself is not in the pushbuffer: like NVK (nvk_cmd_upload_qmd,
 * nvk_cmd_dispatch.c:159-251, memcpy into an upload buffer) the host
 * writes the 256-byte image to qmd_addr and SEND_PCAS_A points at it.
 * No END_PB_SEGMENT is emitted: the GPFIFO entry's LENGTH bounds the
 * segment, and NVK never emits SEC_OP 7.
 *
 * Completion is the QMD's own RELEASE0 (pascal/qmd.h): the compute engine
 * writes RELEASE0_PAYLOAD to RELEASE0_ADDRESS when the grid completes,
 * provided SEMAPHORE_RELEASE_ENABLE0 is set. No pushbuffer semaphore
 * method is emitted for it.
 *
 * Kick: Pascal has no doorbell register. nouveau's Pascal channel class
 * (chan906f, nouveau_chan.c:445-451) kicks with nvif_chan506f_gpfifo_kick
 * (nvif/chan906f.c:70, nvif/chan506f.c:8-12): one write of the new GPFIFO
 * PUT index to USERD + 0x8c (clc06f.h:62 Nvc06fControl.GPPut). The
 * BAR0/usermode +0x90 "doorbell" is VOLTA_USERMODE_A's work-submit token
 * (nvif/userc361.c:40, used only by chanc36f for Volta+).
 */
#ifndef PASCAL_DISPATCH_H
#define PASCAL_DISPATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pascal/mmu.h"
#include "pascal/pushbuf.h"
#include "pascal/qmd.h"

struct pg106_sub_rec;   /* defined below */

/* Status codes; negative on failure. */
#define PG106_SUB_OK          0
#define PG106_SUB_ERR_ARG     (-3)  /* NULL pointer / nonsensical argument */
#define PG106_SUB_ERR_RANGE   (-2)  /* value does not fit its field */
#define PG106_SUB_ERR_SPACE   (-1)  /* pushbuffer / recorder / ring full */
#define PG106_SUB_ERR_QMD     (-5)  /* QMD not v02_01, or RELEASE0 not armed */

/* Channel instance block: 0x1000 bytes == 1024 u32 words
 * (nvkm chan/gf100.c gf100_chan_inst: .size = 0x1000, .zero = true). */
#define PG106_SUB_INST_WORDS  1024u

/* USERD control area (clc06f.h:48-63 Nvc06fControl_struct): the channel
 * is kicked by writing the new GPFIFO PUT index to GPPut; the PBDMA
 * reports its progress in GPGet (nvif/chan506f.c:11 / :37). */
#define PG106_SUB_USERD_GP_GET 0x88u
#define PG106_SUB_USERD_GP_PUT 0x8cu

/* Instance-block VMM join (gf100_vmm_join_, vmmgf100.c:342-363): the
 * page-directory base and the VA limit, 64-bit words at 0x200 / 0x208. */
#define PG106_SUB_INST_PDB_LO        0x200u
#define PG106_SUB_INST_PDB_HI        0x204u
#define PG106_SUB_INST_VM_LIMIT_LO   0x208u
#define PG106_SUB_INST_VM_LIMIT_HI   0x20cu
#define PG106_SUB_PDB_VER2           (1u << 10) /* gp100_vmm_join "VER2" */
#define PG106_SUB_PDB_BIG_PAGE_64K   (1u << 11) /* gp100_vmm_join "64KiB" */
#define PG106_SUB_PDB_VOL            (1u << 2)  /* gf100_vmm_join_ HOST "VOL." */

/* GPFIFO entry address is GET 31:2 + GET_HI 7:0 (clc06f.h:212,214):
 * a 40-bit pushbuffer address. */
#define PG106_SUB_PUSH_ADDR_LIMIT    (1ull << 40)

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
    /* The channel's VMM, joined into the instance block
     * (nvkm_chan_new_ calls nvkm_vmm_join before ramfc->write,
     * engine/fifo/chan.c:421 vs :475): */
    uint64_t pdb_addr;    /* root page directory, 0x1000-aligned
                             (pg106_mmu_root_addr()) */
    enum pg106_target pdb_target; /* where the page directory lives */
    uint64_t vm_limit;    /* VMM limit, i.e. 1 << va_bits (vmm.c:1153);
                             PG106_MMU_VA_LIMIT for the GP100 layout */
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
 * Channel instance block image, in nouveau's order: zero the 0x1000-byte
 * block (gf100_chan_inst .zero), join the VMM (pg106_sub_vmm_join), then
 * write exactly the fields of gk104_chan_ramfc_write() (offsets are
 * byte addresses / 4 = word index):
 *   0x08/0x0c USERD address         0x30 0xfffff902
 *   0x48     GPFIFO offset (lo32)   0x4c  GPFIFO offset (hi32) | ilog2(len/8)<<16
 *   0x84     0x20400000             0x94  0x30000000 | devm
 *   0x9c     0x00000100             0xac  0x0000001f
 *   0xb8     0xf8000000             0xe4  priv ? 0x20 : 0
 *   0xe8     chid                   0xf8  0x10003080   0xfc  0x10000010
 * `inst` must hold PG106_SUB_INST_WORDS and is zeroed here first.
 * Also validates: inst_addr 0x1000-aligned, gpfifo_len a power of two
 * >= 8, runq < 2, and the join parameters. If `rec` is non-NULL, records
 * one VMM_JOIN / RAMFC step per written offset with its citation.
 */
int pg106_sub_ramfc_build(const pg106_sub_chan *c,
                          uint32_t inst[PG106_SUB_INST_WORDS],
                          struct pg106_sub_rec *rec);

/*
 * VMM join into an instance block (gp100_vmm_join, vmmgp100.c:572-579 ->
 * gf100_vmm_join_, vmmgf100.c:342-363):
 *   0x200/0x204 = pdb_addr | VER2 (bit 10) | 64KiB big pages (bit 11)
 *                 | target (VRAM 0, coherent HOST 2 + VOL bit 2, NCOH 3)
 *   0x208/0x20c = vm_limit - 1
 * FAULT_REPLAY_TEX/GCC (bits 4/5) stay clear: they are set only for a
 * fault-replay VMM (vmm->replay), which nouveau creates for SVM only.
 * Requires pdb_addr nonzero and 0x1000-aligned, 0 < vm_limit <= 2^49.
 */
int pg106_sub_vmm_join(const pg106_sub_chan *c,
                       uint32_t inst[PG106_SUB_INST_WORDS],
                       struct pg106_sub_rec *rec);


/* Ordered steps over a mock bus (the test recorder). */
typedef enum pg106_sub_step_type {
    PG106_SUB_STEP_RAMFC     = 1, /* channel instance block word write  */
    PG106_SUB_STEP_RUNLIST   = 2, /* RAMRL entry half (addr = 0 or 4)   */
    PG106_SUB_STEP_QMD       = 3, /* QMD image upload (host write to VM)*/
    PG106_SUB_STEP_RELEASE0  = 4, /* QMD RELEASE0 armed (sem release)   */
    PG106_SUB_STEP_PB_WORD   = 5, /* pushbuffer word (addr = word index)*/
    PG106_SUB_STEP_GPFIFO    = 6, /* GPFIFO entry write / packed entry  */
    PG106_SUB_STEP_GP_PUT    = 7, /* kick: USERD+0x8c write, value = PUT*/
    PG106_SUB_STEP_VMM_JOIN  = 8, /* instance block PDB / limit word    */
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
 * One-time compute state for a channel (see the program at the top).
 * NVK sizes SLM per TPC aligned to 0x8000 (nvk_device.c:79-85; asserted
 * in nvk_queue.c:221); the windows are the pre-Volta NVK constants.
 */
#define PG106_SUB_NVK_LOCAL_MEMORY_WINDOW  0xff000000u /* nvk_cmd_dispatch.c:82 */
#define PG106_SUB_NVK_SHARED_MEMORY_WINDOW 0xfe000000u /* nvk_cmd_dispatch.c:85 */

typedef struct pg106_sub_compute_state {
    uint64_t program_region;     /* SET_PROGRAM_REGION: base the QMD's
                                    PROGRAM_OFFSET is relative to */
    uint32_t local_memory_window;   /* SET_SHADER_LOCAL_MEMORY_WINDOW */
    uint32_t shared_memory_window;  /* SET_SHADER_SHARED_MEMORY_WINDOW */
    uint64_t slm_addr;           /* SET_SHADER_LOCAL_MEMORY_A/B; 0 = no SLM */
    uint64_t slm_per_tpc;        /* bytes, multiple of 0x8000 */
} pg106_sub_compute_state;

/*
 * Emit the one-time compute state into `pb`. Returns PG106_PB_OK, a
 * pushbuffer error, or PG106_SUB_ERR_RANGE for a misaligned slm_per_tpc
 * or an address beyond the 49-bit VA space.
 */
int pg106_sub_compute_init(pg106_pb *pb, unsigned subch,
                           const pg106_sub_compute_state *st);

/*
 * A kernel launch: everything pg106_sub_launch() needs. The QMD must
 * come from pg106_qmd_build() (v02_01) with RELEASE0 armed. `qmd_addr`
 * is the QMD's address in the channel VM and must be 256-byte aligned
 * (SEND_PCAS_A carries qmd_addr >> 8). `push_addr` is the pushbuffer
 * segment's GPU address (4-byte aligned, < 2^40). `init`, if non-NULL,
 * prepends the one-time compute state (first launch on a channel).
 */
typedef struct pg106_sub_launch_params {
    const pg106_sub_chan *chan;
    unsigned              subch;      /* subchannel bound to PASCAL_COMPUTE_B */
    const pg106_qmd      *qmd;
    uint64_t              qmd_addr;
    uint64_t              push_addr;
    const pg106_sub_compute_state *init;
} pg106_sub_launch_params;

typedef struct pg106_sub_result {
    size_t   pb_words;     /* pushbuffer words written */
    uint64_t gpfifo_entry; /* packed NVC06F_GP_ENTRY0/1 for this launch */
    uint32_t new_put;      /* ring PUT index after appending the entry */
} pg106_sub_result;

/*
 * pg106_sub_launch(): build the complete submission for one kernel
 * launch. Emits the method program into `pb` (capacity `cap` words),
 * appends the packed GPFIFO entry to `gpfifo_ring` at `put`, advances PUT
 * with a power-of-two mask like nvif_chan506f_gpfifo_push
 * (chan506f.c:28 `(cur + 1) & max`), records every step with citations
 * and fills `out`. `ring_entries` must equal chan->gpfifo_len / 8 (the
 * ring RAMFC describes) and `put` must be < ring_entries.
 * Returns PG106_SUB_ERR_QMD for a bad QMD, PG106_SUB_ERR_RANGE for a
 * misaligned / out-of-range address or ring mismatch, PG106_PB_ERR_* from
 * the pushbuffer writer, PG106_SUB_ERR_SPACE when the recorder is full.
 */
int pg106_sub_launch(const pg106_sub_launch_params *p,
                     uint32_t *pb, size_t cap,
                     uint64_t *gpfifo_ring, uint32_t ring_entries,
                     uint32_t put,
                     pg106_sub_result *out,
                     pg106_sub_rec *rec);

#endif /* PASCAL_DISPATCH_H */
