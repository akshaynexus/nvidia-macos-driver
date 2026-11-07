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
//   GR / FECS  (mg->gr, when a runlist has is_gr) the FECS mailbox of
//              gf100_gr_fecs_* (gr/gf100.c:751-980): 0x409504 methods with
//              0x409500 data, replies in 0x409800 — the size queries
//              (0x10/0x16/0x25, answered after a few reads), SET_WATCHDOG
//              0x21, BIND_POINTER 3 (bit 4 ok / bit 5 error: the instance
//              block must carry a VER2 PDB) and WFI_GOLDEN_SAVE 9 (bit 0
//              ok / bit 1 error: the bound block's 0x210 pointer must
//              translate; the "ucode" then writes a golden image there:
//              magic, the icmd/method/mmio counts it saw since the bind,
//              and a trailer at the image end), 0x409b00 = current
//              context. The falcons boot (0x409800 bit 0) only after the
//              bootstrap flag and both CPUCTL starts (0x100 or the 0x130
//              alias). 0x404170 FE power mode, 0x40802c SCC init, 0x400200
//              icmds (0x400700 bit 2 busy for one read), 0x404488 methods
//              are recorded. A kick on a GR-runlist channel loads its
//              context first: inst+0x210 must point (bit 2 set) at a
//              readable copy of the golden image whose header says
//              0x1c = 1, and the mmio list at 0x14/0x18 (0x10 pairs) is
//              applied to the register file; errors otherwise. Then
//              PASCAL_COMPUTE_B (SET_OBJECT 0xc1c0, GR runlists only):
//              SEND_PCAS_A keeps the QMD address, SEND_SIGNALING_PCAS_B
//              reads the QMD (v02_01 major version checked) and performs
//              its RELEASE0 semaphore (clc1c0qmd.h fields).

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
	bool is_gr; // test config: this runlist serves GR (FECS context)
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
	// GR state per channel
	bool ctx_loaded;
	uint64_t qmd_addr;
	uint32_t compute_methods, dispatches;
};

#define MG_GR_MAGIC 0x47525458u // 'GRTX': first word of the mock golden image

struct mg_gr {
	// configuration (the test sets these)
	uint32_t ctx_size, zcull_size, pm_size; // FECS size replies
	bool bootstrapped;                      // the SEC2 bootstrap "ran"
	// falcon state
	bool fecs_started, gpccs_started, mbox_ready;
	uint32_t pending_mthd;
	int answer_in;
	uint32_t bound_inst;  // BIND_POINTER data (bit 31 | inst >> 12)
	bool bound;
	uint32_t fecs_resets, fe_mode;
	bool scc_init;
	uint32_t busy_reads;  // 0x400700 bit 2 set for this many reads
	// what the "ucode" saw since the bind
	uint32_t icmds, mthds, mmio_writes;
	// golden image
	bool golden_saved;
	uint64_t golden_pa;
	uint32_t golden_icmds, golden_mthds, golden_mmio;
	// context loads
	uint32_t loads, mmio_applied, bind_errors, save_errors, load_errors;
	uint32_t last_loaded_chid;
	uint64_t last_pagepool, last_bundle, last_attrib; // from the mmio list
	// compute
	uint32_t dispatches, releases;
	uint64_t last_qmd;
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
	// GR / FECS model
	struct mg_gr gr;
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

static struct mg_runl *
mg_runl_of(struct mockgpu *m, uint32_t chid, uint64_t inst)
{
	unsigned id, k;

	for (id = 0; id < MG_NRUNL; id++) {
		struct mg_runl *r = &m->runl[id];

		for (k = 0; k < r->chan_entries; k++)
			if (r->chid[k] == chid && r->inst_hi[k] == (uint32_t)(inst >> 12))
				return r;
	}
	return NULL;
}

static bool
mg_scheduled(struct mockgpu *m, uint32_t chid, uint64_t inst, bool *is_ce)
{
	struct mg_runl *r = mg_runl_of(m, chid, inst);

	if (!r)
		return false;
	*is_ce = r->is_ce;
	return r->tsg_entries > 0;
}

// ---- GR / FECS -------------------------------------------------------------------------

// QMD bit field (clc1c0qmd.h style msb:lsb over the 2048-bit image).
static uint32_t
mg_qmd_bits(const uint32_t *q, unsigned msb, unsigned lsb)
{
	uint32_t v = 0;
	unsigned i;

	for (i = lsb; i <= msb; i++)
		v |= ((q[i / 32] >> (i % 32)) & 1u) << (i - lsb);
	return v;
}

// gf100_gr_fecs_* method semantics (gr/gf100.c:751-980).
static void
mg_fecs_method(struct mockgpu *m, uint32_t mthd)
{
	struct mg_gr *g = &m->gr;
	uint32_t data = mg_get(m, 0x409500);
	uint32_t stat = mg_get(m, 0x409800);

	if (!g->mbox_ready) {
		mg_error(m, "FECS method %llx before the falcon booted (%llu)", mthd, 0);
		return;
	}
	switch (mthd) {
	case 0x10: case 0x16: case 0x25: // DISCOVER_*_SIZE
		g->pending_mthd = mthd;
		g->answer_in = 3;
		return;
	case 0x21: // SET_WATCHDOG_TIMEOUT: no reply
		return;
	case 0x03: { // BIND_POINTER: make the instance block current
		uint64_t inst = (uint64_t)(data & 0x0fffffffu) << 12;
		uint32_t lo = mg_vram_rd32(m, inst + 0x200);

		if (!(data & 0x80000000u) || !(lo & (1u << 10))) {
			mg_error(m, "BIND_POINTER %llx: no VER2 PDB at inst+0x200 (%llx)",
				 data, lo);
			g->bind_errors++;
			mg_set(m, 0x409800, stat | 0x20);
			return;
		}
		g->bound_inst = data;
		g->bound = true;
		g->icmds = g->mthds = g->mmio_writes = 0;
		mg_set(m, 0x409800, stat | 0x10);
		return;
	}
	case 0x09: { // WFI_GOLDEN_SAVE: write the image at the bound 0x210 pointer
		uint64_t inst = (uint64_t)(data & 0x0fffffffu) << 12;
		uint64_t ptr, pa;
		uint32_t i;
		int ap;

		if (!g->bound || data != g->bound_inst) {
			mg_error(m, "WFI_GOLDEN_SAVE on %llx but bound to %llx", data,
				 g->bound_inst);
			goto save_fail;
		}
		if (!g->scc_init || (g->fe_mode & 0xf) != 0x0) {
			mg_error(m, "WFI_GOLDEN_SAVE without SCC init / FE auto (%llx %llu)",
				 g->fe_mode, g->scc_init);
			goto save_fail;
		}
		ptr = mg_vram_rd64(m, inst + 0x210);
		if (!(ptr & 4)) {
			mg_error(m, "WFI_GOLDEN_SAVE: inst+0x210 %llx lacks bit 2 (%llu)",
				 ptr, 0);
			goto save_fail;
		}
		ptr &= ~7ull;
		if (mg_translate(m, inst, ptr, &ap, &pa) || ap != 0 ||
		    mg_translate(m, inst, ptr + g->ctx_size - 4, &ap, &pa)) {
			mg_error(m, "WFI_GOLDEN_SAVE: context %llx not mapped in VRAM (%llu)",
				 ptr, g->ctx_size);
			goto save_fail;
		}
		mg_translate(m, inst, ptr, &ap, &pa);
		g->golden_pa = pa;
		mg_va_wr32(m, inst, ptr + 0, MG_GR_MAGIC);
		mg_va_wr32(m, inst, ptr + 4, g->icmds);
		mg_va_wr32(m, inst, ptr + 8, g->mthds);
		mg_va_wr32(m, inst, ptr + 12, g->mmio_writes);
		for (i = 0x30; i < g->ctx_size - 4; i += 0x1000)
			mg_va_wr32(m, inst, ptr + i, 0xc7c70000u | (i >> 12));
		mg_va_wr32(m, inst, ptr + g->ctx_size - 4, MG_GR_MAGIC ^ g->ctx_size);
		g->golden_icmds = g->icmds;
		g->golden_mthds = g->mthds;
		g->golden_mmio = g->mmio_writes;
		g->golden_saved = true;
		mg_set(m, 0x409b00, 0x80000000u | (data & 0x0fffffffu));
		mg_set(m, 0x409800, stat | 0x1);
		return;
save_fail:
		g->save_errors++;
		mg_set(m, 0x409800, stat | 0x2);
		return;
	}
	default:
		mg_error(m, "unknown FECS method %llx (data %llx)", mthd, data);
		return;
	}
}

// Context load on the first kick of a GR channel (what the HUB ucode does
// with inst+0x210 and the mmio list, gf100_gr_chan_bind's header words).
static bool
mg_gr_load(struct mockgpu *m, uint32_t chid, struct mg_chan *c, uint64_t inst)
{
	struct mg_gr *g = &m->gr;
	uint64_t ptr = mg_vram_rd64(m, inst + 0x210), mmio;
	uint32_t n, i;
	bool ok = true;

	if (c->ctx_loaded)
		return true;
	if (!(ptr & 4) || !(ptr & ~7ull)) {
		mg_error(m, "GR channel %llu kicked without a context (0x210 = %llx)",
			 chid, ptr);
		g->load_errors++;
		return false;
	}
	ptr &= ~7ull;
	if (!g->golden_saved) {
		mg_error(m, "GR channel %llu loaded before any golden save (%llu)", chid, 0);
		g->load_errors++;
		return false;
	}
	if (mg_va_rd32(m, inst, ptr, &ok) != MG_GR_MAGIC || !ok ||
	    mg_va_rd32(m, inst, ptr + g->ctx_size - 4, &ok) !=
		    (MG_GR_MAGIC ^ g->ctx_size) || !ok) {
		mg_error(m, "GR channel %llu: context at %llx is not the golden image",
			 chid, ptr);
		g->load_errors++;
		return false;
	}
	if (mg_va_rd32(m, inst, ptr + 0x1c, &ok) != 1 || !ok) {
		mg_error(m, "GR channel %llu: context header 0x1c != 1 (%llu)", chid, 0);
		g->load_errors++;
		return false;
	}
	n = mg_va_rd32(m, inst, ptr + 0x10, &ok);
	mmio = mg_va_rd32(m, inst, ptr + 0x14, &ok) |
	       (uint64_t)mg_va_rd32(m, inst, ptr + 0x18, &ok) << 32;
	if (!ok || !n || n > 0x1000 / 8) {
		mg_error(m, "GR channel %llu: bad mmio list header (%llu pairs)", chid, n);
		g->load_errors++;
		return false;
	}
	for (i = 0; i < n; i++) {
		uint32_t a = mg_va_rd32(m, inst, mmio + i * 8ull, &ok);
		uint32_t d = mg_va_rd32(m, inst, mmio + i * 8ull + 4, &ok);

		if (!ok) {
			mg_error(m, "GR channel %llu: mmio list entry %llu unmapped", chid, i);
			g->load_errors++;
			return false;
		}
		mg_set(m, a, d);
		g->mmio_applied++;
		if (a == 0x40800c)
			g->last_pagepool = (uint64_t)d << 8;
		if (a == 0x408004)
			g->last_bundle = (uint64_t)d << 8;
		if (a == 0x418810)
			g->last_attrib = (uint64_t)(d & 0x0fffffffu) << 12;
	}
	c->ctx_loaded = true;
	g->loads++;
	g->last_loaded_chid = chid;
	mg_set(m, 0x409b00, 0x80000000u | (uint32_t)(inst >> 12));
	return true;
}

// PASCAL_COMPUTE_B methods (clc1c0.h) as the mock runs them.
static void
mg_compute_method(struct mockgpu *m, struct mg_chan *c, uint64_t inst,
		  uint32_t mthd, uint32_t data)
{
	struct mg_gr *g = &m->gr;

	c->compute_methods++;
	switch (mthd) {
	case 0x02b4: // SEND_PCAS_A: QMD address >> 8
		c->qmd_addr = (uint64_t)data << 8;
		return;
	case 0x02bc: { // SEND_SIGNALING_PCAS_B: run the QMD
		uint32_t q[64], i;
		bool ok = true;

		if (!c->ctx_loaded) {
			mg_error(m, "dispatch on a channel without a GR context (%llx %llu)",
				 c->qmd_addr, 0);
			return;
		}
		for (i = 0; i < 64; i++)
			q[i] = mg_va_rd32(m, inst, c->qmd_addr + i * 4, &ok);
		if (!ok) {
			mg_error(m, "QMD at %llx unmapped (%llu)", c->qmd_addr, 0);
			return;
		}
		if (mg_qmd_bits(q, 583, 580) != 2) {
			mg_error(m, "QMD at %llx: major version %llu is not 2", c->qmd_addr,
				 mg_qmd_bits(q, 583, 580));
			return;
		}
		g->dispatches++;
		c->dispatches++;
		g->last_qmd = c->qmd_addr;
		if (mg_qmd_bits(q, 138, 138)) {
			uint64_t a = mg_qmd_bits(q, 767, 736) |
				     (uint64_t)mg_qmd_bits(q, 775, 768) << 32;

			if (!mg_va_wr32(m, inst, a, mg_qmd_bits(q, 831, 800)))
				mg_error(m, "QMD RELEASE0 to unmapped %llx (%llu)", a, 0);
			else
				g->releases++;
		}
		return;
	}
	default:
		return; // state methods: accepted
	}
}

static void
mg_method(struct mockgpu *m, struct mg_chan *c, uint64_t inst, bool is_ce,
	  bool is_gr, unsigned subch, uint32_t mthd, uint32_t data)
{
	m->methods++;
	if (mthd == 0x0000) { // SET_OBJECT
		c->subch_class[subch] = data & 0xffff;
		if (data == 0xc1b5 && !is_ce)
			mg_error(m, "PASCAL_DMA_COPY_B on a non-CE runlist (%llx %llu)",
				 data, subch);
		if ((data & 0xffff) == 0xc1c0 && !is_gr)
			mg_error(m, "PASCAL_COMPUTE_B on a non-GR runlist (%llx %llu)",
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
	if (c->subch_class[subch] == 0xc1c0) {
		mg_compute_method(m, c, inst, mthd, data);
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
	bool is_ce = false, is_gr = false;
	struct mg_runl *r;

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
	r = mg_runl_of(m, chid, inst);
	is_gr = r && r->is_gr;
	if (is_gr && !mg_gr_load(m, chid, c, inst))
		return;
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
				mg_method(m, c, inst, is_ce, is_gr, sc, mt, cnt);
				continue;
			}
			for (k = 0; k < cnt && i < len; k++, i++) {
				uint32_t d = mg_va_rd32(m, inst, pb + i * 4ull, &ok);
				uint32_t at = op == 1 ? mt + 4 * k :
					      op == 5 ? (k ? mt + 4 : mt) : mt;

				mg_method(m, c, inst, is_ce, is_gr, sc, at, d);
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
	case 0x409800: { // FECS mailbox: size replies land after a few reads
		struct mg_gr *g = &m->gr;

		if (g->pending_mthd && --g->answer_in <= 0) {
			uint32_t v = g->pending_mthd == 0x10 ? g->ctx_size :
				     g->pending_mthd == 0x16 ? g->zcull_size : g->pm_size;

			g->pending_mthd = 0;
			mg_set(m, a, v);
		}
		return mg_get(m, a);
	}
	case 0x400700: // bit 2: icmd in flight, for one read
		if (m->gr.busy_reads) {
			m->gr.busy_reads--;
			return 0x4;
		}
		return 0;
	case 0x404170: return m->gr.fe_mode & ~0x10u; // FE power mode settles
	case 0x40060c: return 0;                       // GR idle
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
			c->ctx_loaded = false; // a new instance block: load again
			c->qmd_addr = 0;
		}
		return;
	}
	if (a >= 0x400000 && a < 0x600000 && m->gr.bound) {
		m->gr.mmio_writes++; // GR state the ucode would save (counted)
		if (a == 0x400200) {
			m->gr.icmds++;
			m->gr.busy_reads = 1;
		} else if (a == 0x404488) {
			m->gr.mthds++;
		}
	}
	switch (a) {
	case 0x409504: mg_set(m, a, v); mg_fecs_method(m, v); return;
	case 0x409614: // gf100_gr_fecs_reset: 0x70, then the 0x700 RMW
		if (v == 0x70)
			m->gr.fecs_resets++;
		mg_set(m, a, v);
		return;
	case 0x404170: m->gr.fe_mode = v; return;
	case 0x40802c: m->gr.scc_init = (v & 1) != 0; return;
	case 0x409100: case 0x409130: // FECS CPUCTL (0x130 = ALIAS_EN alias)
		if (v & 2)
			m->gr.fecs_started = true;
		goto falcon_start;
	case 0x41a100: case 0x41a130: // GPCCS
		if (v & 2)
			m->gr.gpccs_started = true;
falcon_start:
		if (m->gr.fecs_started && m->gr.gpccs_started && m->gr.bootstrapped) {
			m->gr.mbox_ready = true;
			mg_set(m, 0x409800, 1); // ucode up: mailbox ready
		}
		return;
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
