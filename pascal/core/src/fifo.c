// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/src/fifo.c — gp100 host FIFO. See fifo.h for the nouveau
// sources of every register write.

#include <pascal/dispatch.h>
#include <pascal/fifo.h>
#include <pascal/pmc.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define PMC_ENABLE_FIFO 0x00000100u // gk104_mc_reset (mc/gk104.c:27-31)

static const struct pg106_hw_bus *
bus_of(const struct pg106_fifo *f)
{
	return f->vram->bus;
}

static uint32_t
rd(const struct pg106_fifo *f, uint32_t a)
{
	return bus_of(f)->rd32(bus_of(f)->user, a);
}

static void
wr(const struct pg106_fifo *f, uint32_t a, uint32_t v)
{
	bus_of(f)->wr32(bus_of(f)->user, a, v);
}

static void
mask(const struct pg106_fifo *f, uint32_t a, uint32_t m, uint32_t v)
{
	wr(f, a, (rd(f, a) & ~m) | v);
}

// nvkm_mc_enable for one PMC bit, if it is not already on.
static void
pmc_on(struct pg106_fifo *f, uint32_t bit)
{
	uint32_t v = rd(f, PG106_PMC_ENABLE);

	if (!bit || (v & bit) == bit)
		return;
	wr(f, PG106_PMC_ENABLE, v | bit);
	(void)rd(f, PG106_PMC_ENABLE); // nv04_mc_device_enable posting read
	f->pmc_enabled |= bit;
}

static void
init_regs(struct pg106_fifo *f)
{
	uint32_t i, pbdma_mask = 0;

	pmc_on(f, PMC_ENABLE_FIFO);
	for (i = 0; i < f->runq_nr && i < 32; i++)
		pbdma_mask |= 1u << i;
	// gk104_fifo_init_pbdmas (gk104.c:735-741).
	wr(f, 0x000204, pbdma_mask);
	mask(f, 0x002a04, 0xbfffffffu, 0xbfffffffu);
	for (i = 0; i < f->runq_nr; i++) {
		const uint32_t o = i * 0x2000u;

		// gf100_runq_init (gf100.c:353-360).
		mask(f, 0x04013c + o, 0x10000100u, 0x00000000u);
		wr(f, 0x040108 + o, 0xffffffffu);
		wr(f, 0x04010c + o, 0xfffffeffu);
		// gk104_runq_init (gk104.c:379-387).
		wr(f, 0x040148 + o, 0xffffffffu);
		wr(f, 0x04014c + o, 0xffffffffu);
		// gk208_runq_init (gk208.c:34).
		wr(f, 0x04012c + o, 0x000f4240u);
	}
	// gk104_fifo_init (gk104.c:748-752).
	wr(f, 0x002254, 0x10000000u | (uint32_t)(f->userd_bar1 >> 12));
	wr(f, 0x002100, 0xffffffffu);
	wr(f, 0x002140, 0x7fffffffu | (f->nonstall_on ? 0x80000000u : 0));
}

static void
runl_setup(struct pg106_fifo *f, const struct pg106_top_table *top)
{
	size_t i;
	unsigned k;

	for (k = 0; k < PG106_FIFO_MAX_RUNL; k++)
		f->runl[k].id = -1;
	if (!top)
		return;
	// gk104_fifo_runl_ctor (gk104.c:756-800): one runlist per id named
	// in the device table.
	for (i = 0; i < top->ndev; i++) {
		const struct pg106_top_device *d = &top->dev[i];
		bool ce = d->type == PG106_TOP_TYPE_CE0 ||
			  d->type == PG106_TOP_TYPE_CE1 ||
			  d->type == PG106_TOP_TYPE_CE2 ||
			  d->type == PG106_TOP_TYPE_CE_N;

		if (d->runlist < 0 || d->runlist >= (int)PG106_FIFO_MAX_RUNL)
			continue;
		for (k = 0; k < PG106_FIFO_MAX_RUNL; k++)
			if (f->runl[k].id == d->runlist || f->runl[k].id < 0)
				break;
		if (k == PG106_FIFO_MAX_RUNL)
			continue;
		if (f->runl[k].id < 0) {
			f->runl[k].id = d->runlist;
			f->runl[k].engine_type = d->type;
			f->runl[k].is_ce = ce;
		}
		if (d->engine >= 0)
			pmc_on(f, pg106_top_reset_mask(d)); // engine out of reset
	}
}

int
pg106_fifo_runl_find(const struct pg106_fifo *f, int type)
{
	unsigned k;

	for (k = 0; k < PG106_FIFO_MAX_RUNL; k++)
		if (f->runl[k].id >= 0 && f->runl[k].engine_type == type)
			return (int)k;
	return -ENOENT;
}

int
pg106_fifo_init(struct pg106_fifo *f, struct pg106_vram *vram,
		struct pg106_bar *bar1, const struct pg106_bar1_io *io,
		const struct pg106_top_table *top)
{
	int ret;

	if (!f || !vram || !bar1 || !io || !io->wr32 || !io->rd32)
		return -EINVAL;
	memset(f, 0, sizeof(*f));
	f->vram = vram;
	f->bar1 = bar1;
	f->io = *io;
	f->runq_nr = rd(f, 0x002004) & 0xffu; // gm200_fifo_runq_nr
	f->chid_nr = rd(f, 0x002008);         // gm200_fifo_chid_nr
	if (!f->runq_nr || f->runq_nr > 16 || !f->chid_nr || f->chid_nr > 4096)
		return -ENODEV;
	if (f->chid_nr > PG106_FIFO_MAX_CHAN)
		f->chid_nr = PG106_FIFO_MAX_CHAN; // USERD only for what we track
	runl_setup(f, top);
	// fifo/base.c:306-320: USERD in VRAM (INST, zeroed) mapped in BAR1.
	f->userd_size = (uint64_t)f->chid_nr * PG106_FIFO_USERD_SIZE;
	f->userd_size = (f->userd_size + 0xfff) & ~0xfffull;
	ret = pg106_vram_alloc(vram, f->userd_size, 0x1000,
			       PG106_VRAM_TAIL | PG106_VRAM_ZERO, &f->userd_pa);
	if (ret)
		return ret;
	ret = pg106_bar_map(bar1, f->userd_pa, f->userd_size, PG106_TARGET_VRAM,
			    &f->userd_bar1);
	if (ret) {
		pg106_vram_free(vram, f->userd_pa, f->userd_size);
		return ret;
	}
	init_regs(f);
	f->up = true;
	return 0;
}

// ---- runlists ----------------------------------------------------------------------

static int
runl_commit(struct pg106_fifo *f, int k)
{
	struct pg106_fifo_runl *r = &f->runl[k];
	uint32_t n = 0, i;
	uint64_t mem;
	int ret;

	if (!r->mem[0]) {
		// nv50_runl_alloc: two buffers (INST memory = VRAM).
		for (i = 0; i < 2; i++) {
			ret = pg106_vram_alloc(f->vram, PG106_FIFO_RUNL_ENTRIES * 8,
					       0x1000, PG106_VRAM_TAIL, &r->mem[i]);
			if (ret)
				return ret;
		}
	}
	mem = r->mem[r->cur];
	r->cur ^= 1;
	// nv50_runl_update: per TSG its header, then its channels.
	for (i = 0; i < PG106_FIFO_MAX_CHAN; i++) {
		struct pg106_chan *c = f->chan[i];
		uint32_t rl[2];
		pg106_sub_chan sc;

		if (!c || c->runl != k || !c->started)
			continue;
		pg106_sub_runlist_cgrp(1, c->tsg, rl);
		pg106_vram_wr32(f->vram, mem + n * 8, rl[0]);
		pg106_vram_wr32(f->vram, mem + n * 8 + 4, rl[1]);
		n++;
		memset(&sc, 0, sizeof(sc));
		sc.chid = c->chid;
		sc.runq = c->runq;
		sc.inst_addr = c->inst;
		sc.gpfifo_len = (uint64_t)c->entries * 8u;
		if (pg106_sub_runlist_chan(&sc, rl))
			return -EINVAL;
		pg106_vram_wr32(f->vram, mem + n * 8, rl[0]);
		pg106_vram_wr32(f->vram, mem + n * 8 + 4, rl[1]);
		n++;
	}
	// gk104_runl_commit (gk104.c:446-447), target VRAM = 0.
	wr(f, 0x002270, (0u << 28) | (uint32_t)(mem >> 12));
	wr(f, 0x002274, ((uint32_t)r->id << 20) | n);
	r->commits++;
	// nv50_runl_wait -> gk104_runl_pending (gk104.c:422-427).
	return pg106_hw_poll(bus_of(f), 0x002284 + (uint32_t)r->id * 8u,
			     0x00100000u, 0, false, 2000000, NULL, NULL);
}

// ---- channels -------------------------------------------------------------------------

static int
chan_write(struct pg106_chan *c)
{
	struct pg106_fifo *f = c->fifo;
	uint32_t *img = calloc(PG106_SUB_INST_WORDS, sizeof(uint32_t));
	pg106_sub_chan sc;
	const uint64_t userd = f->userd_pa + (uint64_t)c->chid * PG106_FIFO_USERD_SIZE;
	static const uint32_t clear[] = { 0x40, 0x44, 0x48, 0x4c, 0x50, 0x58,
					  0x5c, 0x60, 0x88, 0x8c };
	unsigned i;
	int ret;

	if (!img)
		return -ENOMEM;
	memset(&sc, 0, sizeof(sc));
	sc.chid = c->chid;
	sc.runq = c->runq;
	sc.inst_addr = c->inst;
	sc.userd_addr = userd;
	sc.gpfifo_off = c->gpfifo_va;
	sc.gpfifo_len = (uint64_t)c->entries * 8u;
	sc.devm = 0x1; // nouveau_chan.c:309 args->devm = BIT(0)
	sc.priv = false;
	sc.pdb_addr = c->pdb_addr;
	sc.pdb_target = c->pdb_target;
	sc.vm_limit = c->vm_limit;
	// gf100_chan_inst (zeroed) + gp100_vmm_join + gk104_chan_ramfc_write.
	ret = pg106_sub_ramfc_build(&sc, img, NULL) ? -EINVAL : 0;
	if (!ret && c->ectx) {
		// gk104_ectx_bind (fifo/gk104.c:134-175): GR's pointer at 0x210.
		img[0x210 / 4] = (uint32_t)c->ectx;
		img[0x214 / 4] = (uint32_t)(c->ectx >> 32);
	}
	if (!ret)
		ret = pg106_vram_write(f->vram, c->inst, img, PG106_INST_SIZE);
	free(img);
	// gf100_chan_userd_clear (gf100.c:118-128).
	for (i = 0; !ret && i < sizeof(clear) / sizeof(clear[0]); i++)
		ret = pg106_vram_wr32(f->vram, userd + clear[i], 0);
	return ret;
}

static void
chan_bind(struct pg106_chan *c)
{
	// gk104_chan_bind_inst (gk104.c:64-69).
	wr(c->fifo, 0x800000 + c->chid * 8u,
	   0x80000000u | (uint32_t)(c->inst >> 12));
}

int
pg106_chan_create(struct pg106_fifo *f, struct pg106_vmm *vmm, int runl,
		  uint64_t gpfifo_va, uint32_t entries, struct pg106_chan **out)
{
	struct pg106_chan_desc d;
	int ret;

	if (!vmm || !vmm->mmu)
		return -EINVAL;
	memset(&d, 0, sizeof(d));
	d.pdb_addr = pg106_mmu_root_addr(vmm->mmu);
	d.pdb_target = PG106_TARGET_VRAM;
	d.vm_limit = vmm->limit;
	d.gpfifo_va = gpfifo_va;
	d.entries = entries;
	ret = pg106_chan_create_desc(f, runl, &d, out);
	if (!ret)
		(*out)->vmm = vmm;
	return ret;
}

int
pg106_chan_create_desc(struct pg106_fifo *f, int runl,
		       const struct pg106_chan_desc *d, struct pg106_chan **out)
{
	struct pg106_chan *c;
	uint32_t chid;
	uint32_t entries;
	uint64_t gpfifo_va;
	int ret;

	if (!f || !f->up || !d || !out || runl < 0 ||
	    runl >= (int)PG106_FIFO_MAX_RUNL || f->runl[runl].id < 0)
		return -EINVAL;
	entries = d->entries;
	gpfifo_va = d->gpfifo_va;
	if (entries < 2 || (entries & (entries - 1)) || gpfifo_va & 7 ||
	    !d->pdb_addr || (d->pdb_addr & 0xfff) || !d->vm_limit)
		return -EINVAL;
	for (chid = 0; chid < f->chid_nr && f->chan[chid]; chid++)
		;
	if (chid == f->chid_nr)
		return -ENOSPC;
	c = calloc(1, sizeof(*c));
	if (!c)
		return -ENOMEM;
	c->fifo = f;
	c->chid = chid;
	c->tsg = chid; // one TSG per channel (gk110_fifo_chid_ctor ids)
	c->runl = runl;
	c->runq = 0;
	c->pdb_addr = d->pdb_addr;
	c->pdb_target = d->pdb_target;
	c->vm_limit = d->vm_limit;
	c->ectx = d->ectx;
	c->gpfifo_va = gpfifo_va;
	c->entries = entries;
	ret = pg106_vram_alloc(f->vram, PG106_INST_SIZE, 0x1000, PG106_VRAM_TAIL,
			       &c->inst);
	if (ret) {
		free(c);
		return ret;
	}
	ret = chan_write(c);
	if (ret)
		goto fail;
	f->chan[chid] = c;
	chan_bind(c);
	c->started = true;
	ret = runl_commit(f, runl);
	if (ret)
		goto fail_unbind;
	// gk104_chan_start (gk104.c:48-53).
	mask(f, 0x800004 + chid * 8u, 0x00000400u, 0x00000400u);
	*out = c;
	return 0;
fail_unbind:
	c->started = false;
	wr(f, 0x800000 + chid * 8u, 0);
	f->chan[chid] = NULL;
fail:
	pg106_vram_free(f->vram, c->inst, PG106_INST_SIZE);
	free(c);
	return ret;
}

void
pg106_chan_destroy(struct pg106_chan *c)
{
	struct pg106_fifo *f;

	if (!c)
		return;
	f = c->fifo;
	// gk104_chan_stop, gk110_cgrp_preempt + gf100_runl_preempt_pending.
	mask(f, 0x800004 + c->chid * 8u, 0x00000800u, 0x00000800u);
	wr(f, 0x002634, 0x01000000u | c->tsg);
	pg106_hw_poll(bus_of(f), 0x002634, 0x00100000u, 0, false, 2000000,
		      NULL, NULL);
	c->started = false;
	runl_commit(f, c->runl);
	wr(f, 0x800000 + c->chid * 8u, 0); // gk104_chan_unbind
	f->chan[c->chid] = NULL;
	pg106_vram_free(f->vram, c->inst, PG106_INST_SIZE);
	free(c);
}

void
pg106_chan_kick(struct pg106_chan *c, uint32_t put)
{
	struct pg106_fifo *f = c->fifo;

	c->put = put & (c->entries - 1);
	f->io.wr32(f->io.user,
		   f->userd_bar1 + (uint64_t)c->chid * PG106_FIFO_USERD_SIZE +
			   PG106_FIFO_USERD_GP_PUT,
		   c->put);
}

uint32_t
pg106_chan_get(struct pg106_chan *c)
{
	struct pg106_fifo *f = c->fifo;

	return f->io.rd32(f->io.user,
			  f->userd_bar1 + (uint64_t)c->chid * PG106_FIFO_USERD_SIZE +
				  PG106_FIFO_USERD_GP_GET);
}

int
pg106_fifo_restore(struct pg106_fifo *f)
{
	unsigned i, k;
	int ret = 0;

	if (!f || !f->up)
		return -EINVAL;
	f->pmc_enabled = 0;
	pmc_on(f, PMC_ENABLE_FIFO);
	pg106_vram_write(f->vram, f->userd_pa, NULL, (uint32_t)f->userd_size);
	init_regs(f);
	for (i = 0; i < PG106_FIFO_MAX_CHAN && !ret; i++) {
		struct pg106_chan *c = f->chan[i];

		if (!c)
			continue;
		ret = chan_write(c);
		if (!ret)
			chan_bind(c);
	}
	for (k = 0; k < PG106_FIFO_MAX_RUNL && !ret; k++) {
		bool any = false;

		for (i = 0; i < PG106_FIFO_MAX_CHAN; i++)
			any |= f->chan[i] && f->chan[i]->runl == (int)k;
		if (any)
			ret = runl_commit(f, (int)k);
	}
	for (i = 0; i < PG106_FIFO_MAX_CHAN && !ret; i++)
		if (f->chan[i])
			mask(f, 0x800004 + i * 8u, 0x400u, 0x400u);
	return ret;
}

void
pg106_fifo_fini(struct pg106_fifo *f)
{
	unsigned i, k;

	if (!f || !f->vram)
		return;
	for (i = 0; i < PG106_FIFO_MAX_CHAN; i++)
		pg106_chan_destroy(f->chan[i]);
	for (k = 0; k < PG106_FIFO_MAX_RUNL; k++)
		for (i = 0; i < 2; i++)
			if (f->runl[k].mem[i])
				pg106_vram_free(f->vram, f->runl[k].mem[i],
						PG106_FIFO_RUNL_ENTRIES * 8);
	if (f->up) {
		wr(f, 0x002140, 0); // no more PFIFO interrupts
		pg106_bar_unmap(f->bar1, f->userd_bar1, f->userd_size);
		pg106_vram_free(f->vram, f->userd_pa, f->userd_size);
	}
	memset(f, 0, sizeof(*f));
}

// ---- interrupts ---------------------------------------------------------------------------

void
pg106_fifo_nonstall(struct pg106_fifo *f, bool on)
{
	if (!f || !f->up)
		return;
	f->nonstall_on = on;
	// gf100_fifo_nonstall_allow / _block (gf100.c:440-459).
	mask(f, 0x002140, 0x80000000u, on ? 0x80000000u : 0);
}

uint32_t
pg106_fifo_intr(struct pg106_fifo *f, bool *nonstall)
{
	uint32_t en, stat;

	if (nonstall)
		*nonstall = false;
	if (!f || !f->up)
		return 0;
	en = rd(f, 0x002140);
	stat = rd(f, 0x002100) & en;
	f->intr_last = stat;
	if (!stat)
		return 0;
	if (stat & PG106_FIFO_INTR_NONSTALL) {
		// gk104.c:715-719.
		wr(f, 0x002100, PG106_FIFO_INTR_NONSTALL);
		f->intr_nonstall++;
		if (nonstall)
			*nonstall = true;
	}
	if (stat & 0x20000000u) {
		// PBDMA: ack every PBDMA's INTR (gf100_fifo_intr_pbdma ->
		// gk104_runq_intr). The channel is left as it is; the caller
		// reports the error.
		uint32_t i;

		for (i = 0; i < f->runq_nr; i++) {
			const uint32_t o = i * 0x2000u;

			wr(f, 0x040108 + o, rd(f, 0x040108 + o));
			wr(f, 0x040148 + o, rd(f, 0x040148 + o));
		}
		f->pbdma_errors++;
	}
	if (stat & 0x10000000u) {
		// An MMU fault stays pending until its unit is cleared
		// (gf100_fifo_intr_mmu_fault); not ported: mask it like
		// nouveau masks any bit it cannot handle (gk104.c:723-726) so
		// it cannot storm, and report it.
		f->mmu_faults++;
		mask(f, 0x002140, 0x10000000u, 0);
	}
	if (stat & ~PG106_FIFO_INTR_NONSTALL) {
		f->intr_other++;
		// gk104.c:721-727 for the rest: ack.
		wr(f, 0x002100, stat & ~PG106_FIFO_INTR_NONSTALL);
	}
	return stat;
}
