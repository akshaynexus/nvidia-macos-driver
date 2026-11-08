// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/src/suspend.c — system sleep (quiesce) and the wake probe.
// See suspend.h for the nouveau sources each step mirrors.

#include <pascal/devinit_pmu.h>
#include <pascal/intr.h>
#include <pascal/pmc.h>
#include <pascal/suspend.h>

#include <errno.h>
#include <string.h>

static uint32_t
rd(const struct pg106_hw_bus *b, uint32_t a)
{
	return b->rd32(b->user, a);
}

static void
wr(const struct pg106_hw_bus *b, uint32_t a, uint32_t v)
{
	b->wr32(b->user, a, v);
}

static uint32_t
left(uint32_t budget, uint64_t used)
{
	return used >= budget ? 0 : (uint32_t)(budget - used);
}

static void
fail_at(struct pg106_suspend_res *r, int ret, const char *at)
{
	if (!r->ret) {
		r->ret = ret;
		r->failed_at = at;
	}
}

int
pg106_gpu_suspend(struct pg106_gpu *g, uint32_t budget_us,
		  struct pg106_suspend_res *res)
{
	struct pg106_suspend_res tmp;
	struct pg106_suspend_res *r = res ? res : &tmp;
	const struct pg106_hw_bus *bus;
	struct pg106_hw_result acct;
	unsigned i;

	memset(r, 0, sizeof(*r));
	if (!g || !g->db_valid || !g->cfg.bus)
		return 0; // never booted: nothing on the GPU to stop
	bus = g->cfg.bus;
	memset(&acct, 0, sizeof(acct));
	if (!budget_us)
		budget_us = PG106_SUSPEND_BUDGET_US;

	// 1. nvkm_inth_block / nvkm_mc_intr_unarm: nothing may interrupt a
	// GPU that is going down.
	if (g->intr && g->intr->bus)
		pg106_intr_unarm(g->intr);
	else
		wr(bus, PG106_PMC_INTR_BLOCK_0, 0xffffffffu);

	if (g->fifo_up) {
		struct pg106_fifo *f = &g->fifo;

		// 2. Every channel object's fini: gk104_chan_stop +
		// gk110_cgrp_preempt, gf100_runl_preempt_pending.
		for (i = 0; i < PG106_FIFO_MAX_CHAN; i++) {
			struct pg106_chan *c = f->chan[i];

			if (!c)
				continue;
			wr(bus, 0x800004 + c->chid * 8u,
			   (rd(bus, 0x800004 + c->chid * 8u) & ~0x800u) | 0x800u);
			wr(bus, PG106_FIFO_PREEMPT, 0x01000000u | c->tsg);
			if (pg106_hw_poll(bus, PG106_FIFO_PREEMPT, 0x00100000u, 0,
					  false, left(budget_us, acct.waited_us),
					  NULL, &acct)) {
				r->preempt_timeouts++;
				fail_at(r, -ETIMEDOUT, "channel preempt");
			}
			c->started = false;
			r->chans_preempted++;
		}
		// 3. nvkm_runl_fini -> gk104_runl_fini: scheduling off on
		// every runlist this driver set up.
		for (i = 0; i < PG106_FIFO_MAX_RUNL; i++)
			if (f->runl[i].id >= 0 && f->runl[i].id < 32)
				r->runl_mask |= 1u << f->runl[i].id;
		if (r->runl_mask)
			wr(bus, PG106_FIFO_SCHED_DISABLE,
			   rd(bus, PG106_FIFO_SCHED_DISABLE) | r->runl_mask);
	}

	// 4. GR idle: gf100_gr_wait_idle. Only when PMC_ENABLE has GR (bit
	// 12): a disabled GR is idle by definition there too.
	if (rd(bus, PG106_PMC_ENABLE) & 0x1000u) {
		bool idle = false;

		for (;;) {
			r->gr_ctxsw = rd(bus, PG106_FIFO_ENGINE_STATUS(0));
			r->gr_status = rd(bus, PG106_PGRAPH_STATUS);
			if (!(r->gr_ctxsw & 0x8000u) && !(r->gr_status & 1u)) {
				idle = true;
				break;
			}
			if (acct.waited_us >= budget_us)
				break;
			bus->delay_us(bus->user, 10);
			acct.waited_us += 10;
		}
		if (!idle)
			fail_at(r, -ETIMEDOUT, "GR idle (0x40060c bit 0 / 0x2640 bit 15)");
	}

	// 5. Every PTOP engine idle (gk104_engn_status busy = bit 31): CE,
	// and GR again from the FIFO's side.
	if (g->db.top.ndev) {
		for (i = 0; i < g->db.top.ndev; i++) {
			int e = g->db.top.dev[i].engine;

			if (e < 0 || e >= 32)
				continue;
			if (pg106_hw_poll(bus, PG106_FIFO_ENGINE_STATUS(e),
					  0x80000000u, 0, false,
					  left(budget_us, acct.waited_us), NULL,
					  &acct))
				r->engines_busy |= 1u << e;
		}
		if (r->engines_busy)
			fail_at(r, -ETIMEDOUT, "engine idle (0x2640 + engine * 8 bit 31)");
	}

	// 6. FIFO interrupts off (nvkm_inth_block for the FIFO's own tree);
	// the channels are forgotten by pg106_gpu_fini without a second
	// preempt.
	if (g->fifo_up) {
		wr(bus, 0x002140, 0);
		g->fifo.nonstall_on = false;
		g->fifo.suspended = true;
	}
	r->waited_us = acct.waited_us;
	r->pmc_intr = rd(bus, PG106_PMC_INTR_0);
	return r->ret;
}

// ---- wake ----------------------------------------------------------------------

static bool
bad_read(uint32_t v)
{
	// all ones (no device), or a PRI error (0xbadfxxxx: unit not clocked)
	return v == 0xffffffffu || (v & 0xfff00000u) == 0xbad00000u;
}

void
pg106_wpr_query(const struct pg106_hw_bus *bus, uint64_t *start, uint64_t *end,
		bool *valid)
{
	uint32_t s, l;

	wr(bus, PG106_PFB_WPR_INFO, 2);
	s = rd(bus, PG106_PFB_WPR_INFO);
	wr(bus, PG106_PFB_WPR_INFO, 3);
	l = rd(bus, PG106_PFB_WPR_INFO);
	*valid = !bad_read(s) && !bad_read(l);
	*start = (uint64_t)(s & 0xffffff00u) << 8;
	*end = ((uint64_t)(l & 0xffffff00u) << 8) + 0x20000u;
}

void
pg106_resume_probe(const struct pg106_hw_bus *bus, uint32_t boot0_expected,
		   bool acpi_power, unsigned attempt,
		   struct pg106_resume_probe *p)
{
	memset(p, 0, sizeof(*p));
	p->boot0_expected = boot0_expected;
	p->acpi_power = acpi_power;
	p->attempt = attempt;
	p->boot0 = rd(bus, PG106_PMC_BOOT_0);
	p->boot0_readable = p->boot0 != 0xffffffffu;
	if (!p->boot0_readable || p->boot0 != boot0_expected)
		return;
	p->posted = (rd(bus, PG106_DEVINIT_POSTED_REG) & PG106_DEVINIT_POSTED_BIT) != 0;
	// Whatever the posted bit says (DEVINIT need not set it: devboot.h
	// posted_this_power_cycle), a WPR left by the last ACR boot means the
	// GPU kept its power.
	pg106_wpr_query(bus, &p->wpr_start, &p->wpr_end, &p->wpr_valid);
	p->wpr_locked = p->wpr_valid && p->wpr_start != 0 && p->wpr_start < p->wpr_end;
}

uint8_t
pg106_resume_decide(const struct pg106_resume_probe *p)
{
	if (!p->boot0_readable)
		return p->acpi_power && p->attempt < 2 ? PG106_RESUME_POWER_ON
						       : PG106_RESUME_FAIL_NO_POWER;
	if (p->boot0 != p->boot0_expected)
		return PG106_RESUME_FAIL_IDENTITY;
	if (p->wpr_locked)
		return p->acpi_power && p->attempt < 2 ? PG106_RESUME_POWER_CYCLE
						       : PG106_RESUME_FAIL_WARM;
	return PG106_RESUME_BOOT;
}

const char *
pg106_resume_plan_name(uint8_t plan)
{
	switch (plan) {
	case PG106_RESUME_BOOT:          return "boot";
	case PG106_RESUME_POWER_ON:      return "power-on";
	case PG106_RESUME_POWER_CYCLE:   return "power-cycle";
	case PG106_RESUME_FAIL_NO_POWER: return "no power (BOOT_0 all ones)";
	case PG106_RESUME_FAIL_WARM:     return "power not lost: WPR still locked, no ACR unload";
	case PG106_RESUME_FAIL_IDENTITY: return "BOOT_0 changed";
	default:                         return "?";
	}
}

void
pg106_resume_warm_fixup(const struct pg106_hw_bus *bus)
{
	wr(bus, PG106_FIFO_SCHED_DISABLE, 0);
}
