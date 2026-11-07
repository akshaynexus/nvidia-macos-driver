// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/fifo.h — the GP106 host FIFO: PBDMAs, USERD,
// channel groups + channels on runlists, PCCSR, the GP_PUT kick, PFIFO
// interrupts.
//
// GP106 uses gp100_fifo (engine/fifo/gp100.c:113-132): gk104 runlist and
// PBDMA code, gp100 runlist channel entries, channel groups forced
// (cgrp .force = true: every channel is in a TSG), gm107_chan
// (bind = gk104_chan_bind_inst), PASCAL_CHANNEL_GPFIFO_A.
//
// Init (nvkm_fifo_init, fifo/base.c:139-166):
//   gk104_fifo_init_pbdmas: 0x000204 = PBDMA mask (from 0x002004 & 0xff,
//     gm200_fifo_runq_nr), 0x002a04 |= 0xbfffffff        gk104.c:735-741
//   per PBDMA (gk208_runq_init = gk104 + gf100):           gk208.c:30-35
//     0x04013c + i*0x2000 &= ~0x10000100; 0x040108 = ~0; 0x04010c = 0xfffffeff
//     0x040148 = ~0; 0x04014c = ~0; 0x04012c = 0x000f4240
//   gk104_fifo_init: 0x002254 = 0x10000000 | USERD BAR1 offset >> 12,
//     0x002100 = ~0 (ack), 0x002140 = 0x7fffffff          gk104.c:744-753
// USERD: one VRAM array, chid_nr * 0x200 (gk104_chan_userd .size), mapped
// in BAR1 (fifo/base.c:306-320); the CPU writes GP_PUT through BAR1 at
// USERD + chid * 0x200 + 0x8c and the host snoops it there (the kick).
// chid_nr = 0x002008 (gm200_fifo_chid_nr).
//
// Channel (nvkm_chan_new_ + gk104/gm107 hooks):
//   instance block 0x1000 VRAM, zeroed, VMM joined, RAMFC (gk104_chan_
//   ramfc_write via pascal/dispatch.h pg106_sub_ramfc_build), USERD
//   cleared (gf100_chan_userd_clear, gf100.c:118-128),
//   bind: 0x800000 + chid*8 = 0x80000000 | inst >> 12 (gk104_chan_bind_
//   inst, gk104.c:64-69),
//   runlist: TSG entry (gk110_runl_insert_cgrp) + channel entry
//   (gp100_runl_insert_chan) into a fresh buffer of the double-buffered
//   runlist memory (nv50_runl_update/alloc), commit 0x002270 = target << 28
//   | addr >> 12, 0x002274 = runlist << 20 | count (gk104_runl_commit
//   :430-449), wait 0x002284 + runl*8 bit 20 clear (gk104_runl_pending,
//   nv50_runl_wait),
//   start: 0x800004 + chid*8 |= 0x400 (gk104_chan_start :48-53).
// Teardown: stop (|= 0x800, gk104_chan_stop), preempt the TSG 0x002634 =
// 0x01000000 | tsg (gk110_cgrp_preempt) and wait bit 20 clear
// (gf100_runl_preempt_pending), runlist without it, unbind 0x800000 = 0.
//
// Interrupts (gk104_fifo_intr, gk104.c:652-731): 0x002100 & 0x002140,
// bit 31 = non-stall (NON_STALL_INTERRUPT method; nouveau's fence event),
// acked by writing the bit; enabled with 0x002140 bit 31
// (gf100_fifo_nonstall_allow/block). Other bits are acked and counted;
// a PBDMA (bit 29) or MMU fault (bit 28) error is reported to the caller.
// PMC: PFIFO is PMC_INTR_0 bit 8 (gp100_mc_intrs).
//
// SIMULATION status: tests/test_fifo.c on tests/mockgpu.h. Not yet run on
// a GPU.

#ifndef PASCAL_FIFO_H
#define PASCAL_FIFO_H

#include <pascal/top.h>
#include <pascal/vmm.h>
#include <pascal/vram.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PG106_FIFO_USERD_SIZE	 0x200u
#define PG106_FIFO_USERD_GP_GET	 0x88u
#define PG106_FIFO_USERD_GP_PUT	 0x8cu
#define PG106_FIFO_MAX_CHAN	 64u  // channels this driver tracks
#define PG106_FIFO_MAX_RUNL	 16u
#define PG106_FIFO_RUNL_ENTRIES	 (2u * PG106_FIFO_MAX_CHAN)
#define PG106_FIFO_INTR_NONSTALL 0x80000000u
#define PG106_PMC_INTR_FIFO	 0x00000100u

// CPU access to BAR1 (the caller's mapping of PCI BAR 1).
struct pg106_bar1_io {
	void *user;
	void (*wr32)(void *user, uint64_t off, uint32_t v);
	uint32_t (*rd32)(void *user, uint64_t off);
};

struct pg106_chan;

struct pg106_fifo_runl {
	int id;              // hardware runlist id, -1 = unused slot
	uint64_t mem[2];     // VRAM, PG106_FIFO_RUNL_ENTRIES * 8 each
	unsigned cur;        // next buffer to fill
	uint32_t commits;
	bool is_ce;
	int engine_type;     // pg106_top_type of the (first) engine on it
};

struct pg106_fifo {
	struct pg106_vram *vram;
	struct pg106_bar *bar1;
	struct pg106_bar1_io io;
	uint32_t chid_nr, runq_nr;
	uint64_t userd_pa, userd_size, userd_bar1;
	struct pg106_fifo_runl runl[PG106_FIFO_MAX_RUNL];
	struct pg106_chan *chan[PG106_FIFO_MAX_CHAN];
	uint32_t pmc_enabled;  // PMC_ENABLE bits this init turned on
	// interrupt accounting
	uint32_t intr_nonstall, intr_other, intr_last;
	uint32_t pbdma_errors, mmu_faults;
	bool nonstall_on;
	bool up;
};

struct pg106_chan {
	struct pg106_fifo *fifo;
	uint32_t chid, tsg;
	int runl;             // index into fifo->runl
	uint32_t runq;
	uint64_t inst;        // VRAM instance block
	struct pg106_vmm *vmm;  // NULL for a channel from pg106_chan_create_desc
	uint64_t pdb_addr;    // the VMM joined into the instance block
	enum pg106_target pdb_target;
	uint64_t vm_limit;
	uint64_t ectx;        // inst+0x210 engine context (gk104_ectx_bind), 0 = none
	uint64_t gpfifo_va;   // ring in the channel VMM
	uint32_t entries;     // power of two
	uint32_t put;         // last GP_PUT written
	bool started;
};

// A channel over a VM the caller manages itself (kdisp's client VM):
// the page directory + limit the instance block joins, the GPFIFO ring,
// and the GR engine context pointer for inst+0x210 (gk104_ectx_bind,
// fifo/gk104.c:134-175: ctx VA | 4; 0 = no engine context, a copy-engine
// channel). The pointer is written with the RAMFC, before the channel is
// bound and scheduled, so no preempt is needed (nvkm_chan_cctx_bind does
// it on a live channel and blocks the runlist around it, chan.c:40-70).
struct pg106_chan_desc {
	uint64_t pdb_addr;
	enum pg106_target pdb_target;
	uint64_t vm_limit;
	uint64_t gpfifo_va;
	uint32_t entries;
	uint64_t ectx;
};

// Bring the FIFO up: PMC enables for PFIFO (gk104_mc_reset 0x100) and the
// engines in `top` (their reset lines), PBDMAs, USERD (VRAM + BAR1 map),
// 0x2254, interrupt enables. `top` gives the runlists (gk104_fifo_runl_
// ctor); NULL = none known (pg106_fifo_runl_find fails).
int pg106_fifo_init(struct pg106_fifo *f, struct pg106_vram *vram,
		    struct pg106_bar *bar1, const struct pg106_bar1_io *io,
		    const struct pg106_top_table *top);
// After a power cycle: VRAM and registers are gone; redo the register
// side and rewrite every live channel (inst, USERD, runlists).
int pg106_fifo_restore(struct pg106_fifo *f);
void pg106_fifo_fini(struct pg106_fifo *f);

// Runlist index (into f->runl) serving the first engine of `type`
// (pg106_top_type), -ENOENT when none.
int pg106_fifo_runl_find(const struct pg106_fifo *f, int type);

// Create + schedule a channel in its own TSG on runlist index `runl`,
// GPFIFO ring at gpfifo_va (entries * 8 bytes, already mapped in vmm by
// the caller). The VMM must stay alive until pg106_chan_destroy.
int pg106_chan_create(struct pg106_fifo *f, struct pg106_vmm *vmm, int runl,
		      uint64_t gpfifo_va, uint32_t entries,
		      struct pg106_chan **out);
int pg106_chan_create_desc(struct pg106_fifo *f, int runl,
			   const struct pg106_chan_desc *d,
			   struct pg106_chan **out);
void pg106_chan_destroy(struct pg106_chan *c);
// GP_PUT kick (USERD + 0x8c through BAR1).
void pg106_chan_kick(struct pg106_chan *c, uint32_t put);
// GP_GET as the PBDMA last reported it (USERD + 0x88 through BAR1).
uint32_t pg106_chan_get(struct pg106_chan *c);

// Non-stall interrupt on/off (0x2140 bit 31).
void pg106_fifo_nonstall(struct pg106_fifo *f, bool on);
// PFIFO interrupt handler (gk104_fifo_intr). Returns the bits handled;
// *nonstall true when bit 31 fired (wake the fence waiters).
uint32_t pg106_fifo_intr(struct pg106_fifo *f, bool *nonstall);

#ifdef __cplusplus
}
#endif

#endif // PASCAL_FIFO_H
