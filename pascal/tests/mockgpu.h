// SPDX-License-Identifier: GPL-2.0-only
// pascal/tests/mockgpu.h — a register-semantics GP106 model for the tests
// of vram/vmm/fifo/intr/gpu (header-only: every test binary compiles the
// whole core plus one test file, pascal/Makefile).
//
// SIMULATION. What it models, and from where:
//   VRAM       sparse 4 KiB pages; never-written words read back as a
//              poison pattern (real VRAM is not zero after POST), so a
//              page walk through a table that was never uploaded fails.
//   PRAMIN     0x700000 window, base in 0x1700 >> 16 (instmem/nv50.c).
//   MMU        an independent MMU v2 walker written from the hwref
//              format (dev_mmu.h NV_MMU_VER2_*; gp100-mmu-format.pdf):
//              PDB from the instance block 0x200/0x204 (VER2 bit 10
//              required), PD3 VA[48:47], PD2 [46:38], PD1 [37:29], dual
//              PD0 [28:21] (16 B: big then small), PT 64K [20:16] / 4K
//              [20:12]. It shares no code with core/src/mmu.c. A small
//              TLB caches translations per PDB until 0x100cbc invalidates
//              them (PDB from 0x100cb8/0x100cec, or ALL_PDB).
//   BARs       0x1704 / 0x1714 binds (bit 31 + inst >> 12), 0x1710 idle,
//              0x070000 flush; BAR1 accesses translate through the bound
//              instance block.
//   FIFO       0x2270/0x2274 runlist submit (target, addr, id, count),
//              entries parsed from VRAM as gp100 writes them (TSG header
//              gk110_runl_insert_cgrp, channel gp100_runl_insert_chan);
//              0x2284 + id*8 bit 20 pending for two reads; PCCSR
//              0x800000 + chid*8 inst bind, 0x800004 bit 10 enable /
//              bit 11 disable (gk104_chan_start/stop); 0x2254 USERD BAR1
//              window; a BAR1 write to USERD + 0x8c of a channel is the
//              kick (GP_PUT): the PBDMA then walks the GPFIFO from RAMFC
//              0x48/0x4c through the channel's VMM and executes
//              SET_OBJECT, SEMAPHOREA-D RELEASE (clc06f.h), NON_STALL_
//              INTERRUPT (PFIFO 0x2100 bit 31) and PASCAL_DMA_COPY_B
//              copies + semaphore (clc1b5.h). Errors are recorded, not
//              fatal (a channel that is not enabled, not on a submitted
//              runlist, an untranslatable address, a CE class on a
//              runlist that is not a copy engine's).
//   PMC intr   0x100 = PFIFO bit 8 when 0x2100 & 0x2140, plus mg->intr_extra;
//              0x160 allow / 0x180 block (gp100_mc_intr_allow/_block);
//              mg_irq() = the interrupt line.

#ifndef PASCAL_TESTS_MOCKGPU_H
#define PASCAL_TESTS_MOCKGPU_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#define MG_PAGE 4096u
#define MG_HASH (1u << 16)
#define MG_POISON 0xbadc0de5u
#define MG_NCHAN 512u
#define MG_NRUNL 16u
#define MG_TLB 64u

struct mg_page {
	uint64_t pfn; // pfn + 1 (0 = empty)
	uint8_t *data;
};

struct mg_host {
	uint64_t bus;
	uint8_t *p;
	uint64_t len;
};

struct mg_tlb {
	bool valid;
	uint64_t pd, vpn, pa;
	int aper;
};

struct mg_runl {
	uint64_t addr;
	uint32_t target, count, submits, pending;
	bool is_ce; // test config: this runlist serves a copy engine
	uint32_t tsg_entries, chan_entries;
	uint32_t chid[8];
	uint32_t inst_hi[8];
};

struct mg_chan {
	uint32_t pccsr_inst;
	bool enabled;
	uint32_t starts, stops;
	uint32_t subch_class[8];
	// semaphore A-D state
	uint64_t sem_addr;
	uint32_t sem_payload;
	// CE state per channel (one CE subchannel is enough here)
	uint64_t ce_in, ce_out, ce_sem;
	uint32_t ce_pitch_in, ce_pitch_out, ce_len, ce_lines, ce_sem_payload;
};

struct mockgpu {
	// register file
	uint32_t *raddr, *rval;
	size_t nreg, rcap;
	// VRAM
	uint64_t vram_size;
	struct mg_page *pages;
	uint64_t pages_used;
	// host memory reachable by bus address
	struct mg_host host[16];
	int nhost;
	// TLB
	bool tlb_on;
	struct mg_tlb tlb[MG_TLB];
	unsigned tlb_next;
	uint32_t inv_count, inv_lo, inv_hi, inv_last_type;
	bool flush_queued;
	uint32_t bar_flushes;
	// FIFO
	struct mg_runl runl[MG_NRUNL];
	struct mg_chan chan[MG_NCHAN];
	uint32_t rl_lo;
	uint32_t pfifo_intr, pfifo_en;
	uint32_t intr_allow, intr_extra;
	uint32_t preempts;
	// execution counters
	uint32_t kicks, gp_entries, methods, sem_releases, nonstall, ce_copies;
	uint64_t ce_bytes;
	// errors
	char err[16][120];
	unsigned nerr;
	uint64_t writes, reads;
	uint64_t t_us;
	// optional chaining for test-specific registers
	bool (*rd_hook)(struct mockgpu *, uint32_t, uint32_t *);
	bool (*wr_hook)(struct mockgpu *, uint32_t, uint32_t);
	void *hook_user;
};

static void
mg_error(struct mockgpu *m, const char *fmt, uint64_t a, uint64_t b)
{
	if (m->nerr < 16)
		snprintf(m->err[m->nerr], sizeof(m->err[0]), fmt,
			 (unsigned long long)a, (unsigned long long)b);
	m->nerr++;
}

static void
mg_init(struct mockgpu *m, uint64_t vram_size)
{
	memset(m, 0, sizeof(*m));
	m->vram_size = vram_size;
	m->pages = calloc(MG_HASH, sizeof(*m->pages));
	m->tlb_on = true;
}

static void
mg_free(struct mockgpu *m)
{
	unsigned i;

	for (i = 0; i < MG_HASH; i++)
		free(m->pages[i].data);
	free(m->pages);
	free(m->raddr);
	free(m->rval);
	memset(m, 0, sizeof(*m));
}

// ---- register file --------------------------------------------------------------

static uint32_t *
mg_reg(struct mockgpu *m, uint32_t a, bool mk)
{
	size_t i;

	for (i = 0; i < m->nreg; i++)
		if (m->raddr[i] == a)
			return &m->rval[i];
	if (!mk)
		return NULL;
	if (m->nreg == m->rcap) {
		m->rcap = m->rcap ? m->rcap * 2 : 256;
		m->raddr = realloc(m->raddr, m->rcap * sizeof(*m->raddr));
		m->rval = realloc(m->rval, m->rcap * sizeof(*m->rval));
	}
	m->raddr[m->nreg] = a;
	m->rval[m->nreg] = 0;
	return &m->rval[m->nreg++];
}

static uint32_t
mg_get(struct mockgpu *m, uint32_t a)
{
	uint32_t *p = mg_reg(m, a, false);

	return p ? *p : 0;
}

static void
mg_set(struct mockgpu *m, uint32_t a, uint32_t v)
{
	*mg_reg(m, a, true) = v;
}

// ---- VRAM + host memory ----------------------------------------------------------

static uint8_t *
mg_page(struct mockgpu *m, uint64_t pfn, bool mk)
{
	uint32_t h = (uint32_t)(pfn * 0x9e3779b1u) & (MG_HASH - 1);
	unsigned n;

	for (n = 0; n < MG_HASH; n++, h = (h + 1) & (MG_HASH - 1)) {
		struct mg_page *p = &m->pages[h];

		if (p->pfn == pfn + 1)
			return p->data;
		if (!p->pfn) {
			uint32_t i, *w;

			if (!mk)
				return NULL;
			p->pfn = pfn + 1;
			p->data = malloc(MG_PAGE);
			w = (uint32_t *)p->data;
			for (i = 0; i < MG_PAGE / 4; i++)
				w[i] = MG_POISON;
			m->pages_used++;
			return p->data;
		}
	}
	return NULL;
}

static uint32_t
mg_vram_rd32(struct mockgpu *m, uint64_t pa)
{
	uint8_t *p;
	uint32_t v;

	if (pa + 4 > m->vram_size) {
		mg_error(m, "VRAM read beyond end %llx (%llu)", pa, 0);
		return 0xffffffffu;
	}
	p = mg_page(m, pa / MG_PAGE, false);
	if (!p)
		return MG_POISON;
	memcpy(&v, p + (pa & (MG_PAGE - 1) & ~3ull), 4);
	return v;
}

static void
mg_vram_wr32(struct mockgpu *m, uint64_t pa, uint32_t v)
{
	uint8_t *p;

	if (pa + 4 > m->vram_size) {
		mg_error(m, "VRAM write beyond end %llx (%llu)", pa, 0);
		return;
	}
	p = mg_page(m, pa / MG_PAGE, true);
	memcpy(p + (pa & (MG_PAGE - 1) & ~3ull), &v, 4);
}

static uint64_t
mg_vram_rd64(struct mockgpu *m, uint64_t pa)
{
	return mg_vram_rd32(m, pa) | (uint64_t)mg_vram_rd32(m, pa + 4) << 32;
}

static void
mg_add_host(struct mockgpu *m, uint64_t bus, void *p, uint64_t len)
{
	m->host[m->nhost].bus = bus;
	m->host[m->nhost].p = p;
	m->host[m->nhost].len = len;
	m->nhost++;
}

static uint8_t *
mg_host_at(struct mockgpu *m, uint64_t bus, uint32_t len)
{
	int i;

	for (i = 0; i < m->nhost; i++)
		if (bus >= m->host[i].bus &&
		    bus + len <= m->host[i].bus + m->host[i].len)
			return m->host[i].p + (bus - m->host[i].bus);
	return NULL;
}

// aperture: 0 VRAM, 2/3 system memory (PTE encoding)
static uint32_t
mg_mem_rd32(struct mockgpu *m, int aper, uint64_t a)
{
	uint8_t *p;
	uint32_t v;

	if (aper == 0)
		return mg_vram_rd32(m, a);
	p = mg_host_at(m, a, 4);
	if (!p) {
		mg_error(m, "sysmem read of unknown bus address %llx (%llu)", a, 0);
		return 0xffffffffu;
	}
	memcpy(&v, p, 4);
	return v;
}

static void
mg_mem_wr32(struct mockgpu *m, int aper, uint64_t a, uint32_t v)
{
	uint8_t *p;

	if (aper == 0) {
		mg_vram_wr32(m, a, v);
		return;
	}
	p = mg_host_at(m, a, 4);
	if (!p) {
		mg_error(m, "sysmem write to unknown bus address %llx (%llu)", a, 0);
		return;
	}
	memcpy(p, &v, 4);
}

static uint64_t
mg_mem_rd64(struct mockgpu *m, int aper, uint64_t a)
{
	return mg_mem_rd32(m, aper, a) |
	       (uint64_t)mg_mem_rd32(m, aper, a + 4) << 32;
}

// ---- MMU v2 walker (hwref dev_mmu.h) ------------------------------------------------

// PDE: aperture 2:1 (0 invalid, 1 vid, 2 sys coh, 3 sys ncoh), address
// 53:8 (vid: 32:8) holds addr >> 12. Returns the PTE-style aperture
// (0 vid, 2/3 sys) of the next table, -1 if invalid.
static int
mg_pde(uint64_t w, uint64_t *next)
{
	unsigned ap = (unsigned)(w >> 1) & 3u;
	uint64_t f = w >> 8;

	if (!ap)
		return -1;
	f &= ap == 1 ? (1ull << 25) - 1 : (1ull << 46) - 1;
	*next = f << 12;
	return ap == 1 ? 0 : (int)ap;
}

// Translate va through the VMM joined into inst_pa (VRAM). Returns 0 and
// the aperture/pa of the byte, or -1 (unmapped / bad tables).
static int
mg_translate(struct mockgpu *m, uint64_t inst_pa, uint64_t va, int *aper,
	     uint64_t *pa)
{
	uint64_t lo = mg_vram_rd32(m, inst_pa + 0x200);
	uint64_t hi = mg_vram_rd32(m, inst_pa + 0x204);
	uint64_t limit = mg_vram_rd64(m, inst_pa + 0x208);
	uint64_t pd = (lo & ~0xfffull) | hi << 32, next, w, w0, w1;
	int ap, rap = (int)(lo & 3);
	unsigned i;

	if (!(lo & (1u << 10))) {
		mg_error(m, "PDB of inst %llx lacks VER2 (%llx)", inst_pa, lo);
		return -1;
	}
	if (rap == 1)
		return -1;
	if (va > limit)
		return -1;
	if (m->tlb_on)
		for (i = 0; i < MG_TLB; i++)
			if (m->tlb[i].valid && m->tlb[i].pd == pd &&
			    m->tlb[i].vpn == va >> 12) {
				*aper = m->tlb[i].aper;
				*pa = m->tlb[i].pa | (va & 0xfff);
				return 0;
			}
	w = mg_mem_rd64(m, rap, pd + ((va >> 47) & 3) * 8);
	if ((ap = mg_pde(w, &next)) < 0)
		return -1;
	w = mg_mem_rd64(m, ap, next + ((va >> 38) & 0x1ff) * 8);
	if ((ap = mg_pde(w, &next)) < 0)
		return -1;
	w = mg_mem_rd64(m, ap, next + ((va >> 29) & 0x1ff) * 8);
	if ((ap = mg_pde(w, &next)) < 0)
		return -1;
	w0 = mg_mem_rd64(m, ap, next + ((va >> 21) & 0xff) * 16);
	w1 = mg_mem_rd64(m, ap, next + ((va >> 21) & 0xff) * 16 + 8);
	w = 0;
	{
		uint64_t t;
		int tap = mg_pde(w0, &t);

		if (tap >= 0) {
			w = mg_mem_rd64(m, tap, t + ((va >> 16) & 0x1f) * 8);
			if (w & 1) {
				unsigned pap = (unsigned)(w >> 1) & 3u;
				uint64_t f = (w >> 8) &
					     (pap <= 1 ? (1ull << 25) - 1
						       : (1ull << 46) - 1);

				*aper = (int)pap;
				*pa = (f << 12) + (va & 0xffff);
				goto hit;
			}
		}
		tap = mg_pde(w1, &t);
		if (tap < 0)
			return -1;
		w = mg_mem_rd64(m, tap, t + ((va >> 12) & 0x1ff) * 8);
		if (!(w & 1))
			return -1;
		{
			unsigned pap = (unsigned)(w >> 1) & 3u;
			uint64_t f = (w >> 8) & (pap <= 1 ? (1ull << 25) - 1
							  : (1ull << 46) - 1);

			*aper = (int)pap;
			*pa = (f << 12) + (va & 0xfff);
		}
	}
hit:
	if (m->tlb_on) {
		struct mg_tlb *t = &m->tlb[m->tlb_next++ % MG_TLB];

		t->valid = true;
		t->pd = pd;
		t->vpn = va >> 12;
		t->pa = *pa & ~0xfffull;
		t->aper = *aper;
	}
	return 0;
}

static uint32_t
mg_va_rd32(struct mockgpu *m, uint64_t inst, uint64_t va, bool *ok)
{
	int ap;
	uint64_t pa;

	if (mg_translate(m, inst, va, &ap, &pa)) {
		if (ok)
			*ok = false;
		return 0;
	}
	return mg_mem_rd32(m, ap, pa);
}

static bool
mg_va_wr32(struct mockgpu *m, uint64_t inst, uint64_t va, uint32_t v)
{
	int ap;
	uint64_t pa;

	if (mg_translate(m, inst, va, &ap, &pa))
		return false;
	mg_mem_wr32(m, ap, pa, v);
	return true;
}

static void
mg_tlb_invalidate(struct mockgpu *m, uint32_t type)
{
	uint64_t pdbw = (uint64_t)m->inv_hi << 32 | m->inv_lo;
	uint64_t pd = (pdbw >> 4) << 12;
	unsigned i;

	m->inv_count++;
	m->inv_last_type = type;
	for (i = 0; i < MG_TLB; i++)
		if ((type & 2) || m->tlb[i].pd == pd)
			m->tlb[i].valid = false;
	m->flush_queued = true;
}

// ---- BARs ---------------------------------------------------------------------------

static uint64_t
mg_bar_inst(struct mockgpu *m, uint32_t reg)
{
	uint32_t v = mg_get(m, reg);

	return (v & 0x80000000u) ? (uint64_t)(v & 0x0fffffffu) << 12 : UINT64_MAX;
}

static void mg_kick(struct mockgpu *m, uint32_t chid);

static uint32_t
mg_bar1_rd32(struct mockgpu *m, uint64_t off)
{
	uint64_t inst = mg_bar_inst(m, 0x1704);
	bool ok = true;
	uint32_t v;

	if (inst == UINT64_MAX) {
		mg_error(m, "BAR1 read %llx while unbound (%llu)", off, 0);
		return 0xffffffffu;
	}
	v = mg_va_rd32(m, inst, off, &ok);
	if (!ok) {
		mg_error(m, "BAR1 read %llx unmapped (%llu)", off, 0);
		return 0xffffffffu;
	}
	return v;
}

static void
mg_bar1_wr32(struct mockgpu *m, uint64_t off, uint32_t v)
{
	uint64_t inst = mg_bar_inst(m, 0x1704);
	uint32_t ub = mg_get(m, 0x2254);
	uint64_t ubase = (uint64_t)(ub & 0x0fffffffu) << 12;

	if (inst == UINT64_MAX) {
		mg_error(m, "BAR1 write %llx while unbound (%llu)", off, 0);
		return;
	}
	if (!mg_va_wr32(m, inst, off, v)) {
		mg_error(m, "BAR1 write %llx unmapped (%llu)", off, 0);
		return;
	}
	// USERD window (gk104_fifo_init 0x2254): GP_PUT is the kick.
	if ((ub & 0x10000000u) && off >= ubase &&
	    off < ubase + (uint64_t)MG_NCHAN * 0x200 &&
	    ((off - ubase) & 0x1ff) == 0x8c)
		mg_kick(m, (uint32_t)((off - ubase) / 0x200));
}

// ---- FIFO ---------------------------------------------------------------------------

static void
mg_runlist_submit(struct mockgpu *m, uint32_t v)
{
	uint32_t id = (v >> 20) & 0xf, count = v & 0xffff;
	struct mg_runl *r = &m->runl[id];
	uint64_t a;
	uint32_t i;

	r->addr = (uint64_t)(m->rl_lo & 0x0fffffffu) << 12;
	r->target = m->rl_lo >> 28;
	r->count = count;
	r->submits++;
	r->pending = 2;
	r->tsg_entries = r->chan_entries = 0;
	if (r->target != 0)
		mg_error(m, "runlist %llu target %llu (only VRAM modelled)", id,
			 r->target);
	for (i = 0, a = r->addr; i < count; i++, a += 8) {
		uint32_t w0 = mg_vram_rd32(m, a), w1 = mg_vram_rd32(m, a + 4);

		if (w0 & 0x00002000u) { // gk110_runl_insert_cgrp
			if (((w0 >> 14) & 3) != 3 || ((w0 >> 18) & 0xff) != 128 || w1)
				mg_error(m, "bad TSG entry %llx %llx", w0, w1);
			r->tsg_entries++;
		} else if (r->chan_entries < 8) { // gp100_runl_insert_chan
			r->chid[r->chan_entries] = w0 & 0xfff;
			r->inst_hi[r->chan_entries] = w1;
			r->chan_entries++;
		}
	}
}

static bool
mg_scheduled(struct mockgpu *m, uint32_t chid, uint64_t inst, bool *is_ce)
{
	unsigned id, k;

	for (id = 0; id < MG_NRUNL; id++) {
		struct mg_runl *r = &m->runl[id];

		for (k = 0; k < r->chan_entries; k++)
			if (r->chid[k] == chid && r->inst_hi[k] == (uint32_t)(inst >> 12)) {
				*is_ce = r->is_ce;
				return r->tsg_entries > 0;
			}
	}
	return false;
}

static void
mg_method(struct mockgpu *m, struct mg_chan *c, uint64_t inst, bool is_ce,
	  unsigned subch, uint32_t mthd, uint32_t data)
{
	m->methods++;
	if (mthd == 0x0000) { // SET_OBJECT
		c->subch_class[subch] = data & 0xffff;
		if (data == 0xc1b5 && !is_ce)
			mg_error(m, "PASCAL_DMA_COPY_B on a non-CE runlist (%llx %llu)",
				 data, subch);
		return;
	}
	if (mthd < 0x100) { // host methods, clc06f.h
		switch (mthd) {
		case 0x10: c->sem_addr = (c->sem_addr & 0xffffffffull) |
					 (uint64_t)(data & 0xff) << 32; break;
		case 0x14: c->sem_addr = (c->sem_addr & ~0xffffffffull) |
					 (data & ~3u); break;
		case 0x18: c->sem_payload = data; break;
		case 0x1c:
			if ((data & 0x1f) == 2) { // OPERATION_RELEASE
				if (!mg_va_wr32(m, inst, c->sem_addr, c->sem_payload))
					mg_error(m, "semaphore release to unmapped %llx (%llu)",
						 c->sem_addr, 0);
				else if (!(data & (1u << 24))) { // 16-byte: + timestamp
					mg_va_wr32(m, inst, c->sem_addr + 4, 0);
					mg_va_wr32(m, inst, c->sem_addr + 8, (uint32_t)m->t_us);
					mg_va_wr32(m, inst, c->sem_addr + 12, 0);
				}
				m->sem_releases++;
			}
			break;
		case 0x20: // NON_STALL_INTERRUPT
			m->pfifo_intr |= 0x80000000u;
			m->nonstall++;
			break;
		default:
			break;
		}
		return;
	}
	if (c->subch_class[subch] != 0xc1b5) {
		mg_error(m, "method %llx on subch %llu without a modelled class",
			 mthd, subch);
		return;
	}
	switch (mthd) { // clc1b5.h
	case 0x240: c->ce_sem = (c->ce_sem & 0xffffffffull) |
				(uint64_t)(data & 0x1ffff) << 32; break;
	case 0x244: c->ce_sem = (c->ce_sem & ~0xffffffffull) | data; break;
	case 0x248: c->ce_sem_payload = data; break;
	case 0x400: c->ce_in = (c->ce_in & 0xffffffffull) | (uint64_t)(data & 0x1ffff) << 32; break;
	case 0x404: c->ce_in = (c->ce_in & ~0xffffffffull) | data; break;
	case 0x408: c->ce_out = (c->ce_out & 0xffffffffull) | (uint64_t)(data & 0x1ffff) << 32; break;
	case 0x40c: c->ce_out = (c->ce_out & ~0xffffffffull) | data; break;
	case 0x410: c->ce_pitch_in = data; break;
	case 0x414: c->ce_pitch_out = data; break;
	case 0x418: c->ce_len = data; break;
	case 0x41c: c->ce_lines = data; break;
	case 0x300: { // LAUNCH_DMA
		uint32_t l, b;
		uint32_t lines = (data & (1u << 9)) ? c->ce_lines : 1;

		if (data & 3) {
			for (l = 0; l < lines; l++)
				for (b = 0; b + 4 <= c->ce_len; b += 4) {
					bool ok = true;
					uint32_t v = mg_va_rd32(m, inst,
						c->ce_in + (uint64_t)l * c->ce_pitch_in + b, &ok);

					if (!ok || !mg_va_wr32(m, inst,
						c->ce_out + (uint64_t)l * c->ce_pitch_out + b, v)) {
						mg_error(m, "CE copy fault in %llx out %llx",
							 c->ce_in, c->ce_out);
						return;
					}
				}
			m->ce_copies++;
			m->ce_bytes += (uint64_t)lines * c->ce_len;
		}
		if (((data >> 3) & 3) == 1 &&
		    !mg_va_wr32(m, inst, c->ce_sem, c->ce_sem_payload))
			mg_error(m, "CE semaphore to unmapped %llx (%llu)", c->ce_sem, 0);
		break;
	}
	default:
		break;
	}
}

static void
mg_kick(struct mockgpu *m, uint32_t chid)
{
	struct mg_chan *c = &m->chan[chid];
	uint64_t inst = (uint64_t)(c->pccsr_inst & 0x0fffffffu) << 12;
	uint64_t gpfifo, userd;
	uint32_t entries, get, put;
	bool is_ce = false;

	m->kicks++;
	if (!(c->pccsr_inst & 0x80000000u) || !c->enabled) {
		mg_error(m, "kick on channel %llu not bound/enabled (%llx)", chid,
			 c->pccsr_inst);
		return;
	}
	if (!mg_scheduled(m, chid, inst, &is_ce)) {
		mg_error(m, "kick on channel %llu not on a submitted runlist (%llu)",
			 chid, 0);
		return;
	}
	// RAMFC (gk104_chan_ramfc_write).
	gpfifo = mg_vram_rd32(m, inst + 0x48) |
		 (uint64_t)(mg_vram_rd32(m, inst + 0x4c) & 0xff) << 32;
	entries = 1u << ((mg_vram_rd32(m, inst + 0x4c) >> 16) & 0x1f);
	userd = mg_vram_rd32(m, inst + 0x08) |
		(uint64_t)mg_vram_rd32(m, inst + 0x0c) << 32;
	put = mg_vram_rd32(m, userd + 0x8c);
	get = mg_vram_rd32(m, userd + 0x88);
	if (get >= entries)
		get = 0;
	while (get != put % entries) {
		bool ok = true;
		uint64_t e = mg_va_rd32(m, inst, gpfifo + get * 8ull, &ok) |
			     (uint64_t)mg_va_rd32(m, inst, gpfifo + get * 8ull + 4, &ok) << 32;
		uint64_t pb = (e & 0xfffffffcull) | ((e >> 32) & 0xff) << 32;
		uint32_t len = (uint32_t)(e >> 42) & 0x1fffff, i = 0;

		if (!ok) {
			mg_error(m, "GPFIFO entry %llu unmapped (%llx)", get, gpfifo);
			break;
		}
		m->gp_entries++;
		while (i < len) {
			uint32_t h = mg_va_rd32(m, inst, pb + i * 4ull, &ok);
			unsigned op = h >> 29, cnt = (h >> 16) & 0x1fff;
			unsigned sc = (h >> 13) & 7;
			uint32_t mt = (h & 0xfff) << 2, k;

			if (!ok) {
				mg_error(m, "push %llx unmapped (%llu)", pb + i * 4ull, 0);
				break;
			}
			i++;
			if (op == 4) { // IMMD
				mg_method(m, c, inst, is_ce, sc, mt, cnt);
				continue;
			}
			for (k = 0; k < cnt && i < len; k++, i++) {
				uint32_t d = mg_va_rd32(m, inst, pb + i * 4ull, &ok);
				uint32_t at = op == 1 ? mt + 4 * k :
					      op == 5 ? (k ? mt + 4 : mt) : mt;

				mg_method(m, c, inst, is_ce, sc, at, d);
			}
		}
		get = (get + 1) % entries;
	}
	mg_vram_wr32(m, userd + 0x88, get);
}

// ---- BAR0 -----------------------------------------------------------------------------

static uint32_t
mg_pmc_intr(struct mockgpu *m)
{
	uint32_t v = m->intr_extra;

	if (m->pfifo_intr & m->pfifo_en)
		v |= 0x00000100u;
	return v;
}

static bool
mg_irq(struct mockgpu *m)
{
	return (mg_pmc_intr(m) & m->intr_allow) != 0;
}

static uint32_t
mg_rd32(void *u, uint32_t a)
{
	struct mockgpu *m = u;
	uint32_t v;

	m->reads++;
	if (m->rd_hook && m->rd_hook(m, a, &v))
		return v;
	if (a >= 0x700000 && a < 0x800000)
		return mg_vram_rd32(m, ((uint64_t)mg_get(m, 0x1700) << 16) + (a - 0x700000));
	if (a >= 0x800000 && a < 0x800000 + MG_NCHAN * 8) {
		struct mg_chan *c = &m->chan[(a - 0x800000) / 8];

		return (a & 4) ? (c->enabled ? 1u : 0u) : c->pccsr_inst;
	}
	if (a >= 0x2284 && a < 0x2284 + MG_NRUNL * 8 && !((a - 0x2284) & 7)) {
		struct mg_runl *r = &m->runl[(a - 0x2284) / 8];

		if (r->pending) {
			r->pending--;
			return 0x00100000u;
		}
		return 0;
	}
	switch (a) {
	case 0x000100: return mg_pmc_intr(m);
	case 0x100c80: return mg_get(m, a) | 0x00ff0000u |
			      (m->flush_queued ? 0x8000u : 0);
	case 0x070000: return 0;
	case 0x001710: return 0;
	case 0x002100: return m->pfifo_intr;
	case 0x002140: return m->pfifo_en;
	case 0x002634: return mg_get(m, a) & ~0x00100000u;
	case 0x002004: return mg_get(m, a) ? mg_get(m, a) : 4;
	case 0x002008: return MG_NCHAN;
	default: return mg_get(m, a);
	}
}

static void
mg_wr32(void *u, uint32_t a, uint32_t v)
{
	struct mockgpu *m = u;

	m->writes++;
	if (m->wr_hook && m->wr_hook(m, a, v))
		return;
	if (a >= 0x700000 && a < 0x800000) {
		mg_vram_wr32(m, ((uint64_t)mg_get(m, 0x1700) << 16) + (a - 0x700000), v);
		return;
	}
	if (a >= 0x800000 && a < 0x800000 + MG_NCHAN * 8) {
		struct mg_chan *c = &m->chan[(a - 0x800000) / 8];

		if (a & 4) {
			if (v & 0x400) {
				c->enabled = true;
				c->starts++;
			}
			if (v & 0x800) {
				c->enabled = false;
				c->stops++;
			}
		} else {
			c->pccsr_inst = v;
		}
		return;
	}
	switch (a) {
	case 0x100cb8: m->inv_lo = v; return;
	case 0x100cec: m->inv_hi = v; return;
	case 0x100cbc:
		if (v & 0x80000000u)
			mg_tlb_invalidate(m, v & 0xff);
		return;
	case 0x070000: m->bar_flushes++; return;
	case 0x002270: m->rl_lo = v; return;
	case 0x002274: mg_runlist_submit(m, v); return;
	case 0x002100: m->pfifo_intr &= ~v; return;
	case 0x002140: m->pfifo_en = v; return;
	case 0x000160: m->intr_allow |= v; return;
	case 0x000180: m->intr_allow &= ~v; return;
	case 0x002634: m->preempts++; mg_set(m, a, v); return;
	default: mg_set(m, a, v); return;
	}
}

static void
mg_delay(void *u, uint32_t us)
{
	((struct mockgpu *)u)->t_us += us;
}

#endif // PASCAL_TESTS_MOCKGPU_H
