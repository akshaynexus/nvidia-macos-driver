// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/src/hwexec.c — step-list executor over a register bus. See
// hwexec.h for the semantics and sources.

#include <pascal/hwexec.h>

#include <errno.h>
#include <string.h>

static uint32_t
poll_interval(const struct pg106_hw_bus *bus, uint32_t timeout_us)
{
	uint32_t us = bus->poll_us ? bus->poll_us : PG106_HW_POLL_US_DEFAULT;

	if (timeout_us && us > timeout_us)
		us = timeout_us;
	return us ? us : 1;
}

static bool
bus_ok(const struct pg106_hw_bus *bus)
{
	return bus && bus->rd32 && bus->wr32 && bus->delay_us;
}

int
pg106_hw_poll(const struct pg106_hw_bus *bus, uint32_t addr, uint32_t mask,
	      uint32_t value, bool any, uint32_t timeout_us, uint32_t *got,
	      struct pg106_hw_result *acct)
{
	const uint32_t step = poll_interval(bus, timeout_us);
	uint64_t waited = 0;
	uint32_t v;

	for (;;) {
		v = bus->rd32(bus->user, addr) & mask;
		if (acct)
			acct->polls++;
		if (any ? v != 0 : v == value)
			break;
		// nvkm_msec: the condition is re-checked until the time
		// budget is gone, then the caller sees -ETIMEDOUT.
		if (waited >= timeout_us) {
			if (got)
				*got = v;
			if (acct)
				acct->waited_us += waited;
			return -ETIMEDOUT;
		}
		bus->delay_us(bus->user, step);
		waited += step;
	}
	if (got)
		*got = v;
	if (acct)
		acct->waited_us += waited;
	return 0;
}

static void
fail(struct pg106_hw_result *r, int ret, size_t i, uint8_t kind,
     uint64_t addr, uint32_t expect, uint32_t got, const char *why)
{
	r->ret = ret;
	r->step = i;
	r->kind = kind;
	r->addr = addr;
	r->expect = expect;
	r->got = got;
	r->why = why;
}

int
pg106_hwexec_acr(const struct pg106_hw_bus *bus,
		 const struct pg106_acrboot_plan *plan,
		 struct pg106_hw_result *res)
{
	return pg106_hwexec_acr_range(bus, plan, 0, plan ? plan->nsteps : 0,
				      res);
}

int
pg106_hwexec_acr_range(const struct pg106_hw_bus *bus,
		       const struct pg106_acrboot_plan *plan, size_t from,
		       size_t to, struct pg106_hw_result *res)
{
	struct pg106_hw_result local;
	struct pg106_hw_result *r = res ? res : &local;
	size_t i;

	memset(r, 0, sizeof(*r));
	if (!bus_ok(bus) || !plan || (!plan->steps && plan->nsteps) ||
	    from > to) {
		r->ret = -EINVAL;
		return r->ret;
	}
	if (to > plan->nsteps)
		to = plan->nsteps;
	for (i = from; i < to; i++) {
		const struct pg106_acrboot_step *s = &plan->steps[i];
		const uint32_t a = (uint32_t)s->addr;
		uint32_t v;
		int ret;

		if (s->space != PG106_ACRBOOT_MMIO &&
		    (s->kind == PG106_ACRBOOT_WRITE_REG ||
		     s->kind == PG106_ACRBOOT_WRITE_BLOB)) {
			uint8_t w[4];
			const uint8_t *src = s->blob;
			uint32_t len = s->blob_len;

			if (!bus->mem_write) {
				fail(r, -ENOSYS, i, s->kind, s->addr, 0, 0,
				     s->why);
				return r->ret;
			}
			if (s->kind == PG106_ACRBOOT_WRITE_REG) {
				w[0] = (uint8_t)s->value;
				w[1] = (uint8_t)(s->value >> 8);
				w[2] = (uint8_t)(s->value >> 16);
				w[3] = (uint8_t)(s->value >> 24);
				src = w;
				len = 4;
			}
			ret = bus->mem_write(bus->user, s->space, s->addr, src,
					     len);
			if (ret) {
				fail(r, ret, i, s->kind, s->addr, 0, 0, s->why);
				return r->ret;
			}
			continue;
		}
		if (s->addr > UINT32_MAX) {
			fail(r, -EINVAL, i, s->kind, s->addr, 0, 0, s->why);
			return r->ret;
		}
		switch (s->kind) {
		case PG106_ACRBOOT_WRITE_REG:
			bus->wr32(bus->user, a, s->value);
			break;
		case PG106_ACRBOOT_WRITE_BLOB:
			// An MMIO blob is not produced by acr_boot; refuse
			// rather than guess a port.
			fail(r, -EINVAL, i, s->kind, s->addr, 0, 0, s->why);
			return r->ret;
		case PG106_ACRBOOT_MASK_REG:
			v = bus->rd32(bus->user, a);
			bus->wr32(bus->user, a, (v & ~s->mask) | s->value);
			break;
		case PG106_ACRBOOT_POLL_REG:
			ret = pg106_hw_poll(bus, a, s->mask, s->value,
					    s->poll_op == PG106_ACRBOOT_POLL_ANY,
					    s->timeout_us, &v, r);
			if (ret) {
				fail(r, ret, i, s->kind, s->addr,
				     s->value & s->mask, v, s->why);
				return r->ret;
			}
			break;
		case PG106_ACRBOOT_READ_REG:
			v = bus->rd32(bus->user, a);
			if ((v & s->mask) != (s->value & s->mask)) {
				fail(r, -EIO, i, s->kind, s->addr,
				     s->value & s->mask, v & s->mask, s->why);
				return r->ret;
			}
			break;
		case PG106_ACRBOOT_DELAY_US:
			bus->delay_us(bus->user, s->value);
			r->waited_us += s->value;
			break;
		case PG106_ACRBOOT_FALCON_START:
			// nvkm_falcon_v1_start (falcon/v1.c:93-101).
			v = bus->rd32(bus->user, a);
			bus->wr32(bus->user, (v & 0x40u) ? a + 0x30u : a, s->value);
			r->starts++;
			r->start_cpuctl = v;
			break;
		default:
			fail(r, -EINVAL, i, s->kind, s->addr, 0, 0, s->why);
			return r->ret;
		}
	}
	r->step = i;
	return 0;
}

// gk20a_fw_av pairs (gr/gk20a.c:30-34), little-endian u32 each.
static uint32_t
le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

int
pg106_hwexec_gr(const struct pg106_hw_bus *bus,
		const struct pg106_grboot_plan *plan, size_t from, size_t to,
		const uint8_t *sw_nonctx, size_t sw_nonctx_len,
		pg106_hw_observe_fn observe, void *observe_user,
		struct pg106_hw_result *res)
{
	struct pg106_hw_result local;
	struct pg106_hw_result *r = res ? res : &local;
	size_t i;

	memset(r, 0, sizeof(*r));
	if (!bus_ok(bus) || !plan || (!plan->steps && plan->nsteps) ||
	    from > to || (sw_nonctx && sw_nonctx_len % 8)) {
		r->ret = -EINVAL;
		return r->ret;
	}
	if (to > plan->nsteps)
		to = plan->nsteps;
	for (i = from; i < to; i++) {
		const struct pg106_grboot_step *s = &plan->steps[i];
		const uint32_t a = (uint32_t)s->addr;
		uint32_t v;
		size_t k;
		int ret;

		if (s->addr > UINT32_MAX) {
			fail(r, -EINVAL, i, s->kind, s->addr, 0, 0, s->why);
			return r->ret;
		}
		switch (s->kind) {
		case PG106_GRBOOT_WRITE_REG:
			bus->wr32(bus->user, a, s->value);
			break;
		case PG106_GRBOOT_WRITE_BLOB:
			if (!sw_nonctx) {
				fail(r, -ENOENT, i, s->kind, s->addr, 0, 0,
				     s->why);
				return r->ret;
			}
			for (k = 0; k + 8 <= sw_nonctx_len; k += 8)
				bus->wr32(bus->user, le32(sw_nonctx + k),
					  le32(sw_nonctx + k + 4));
			break;
		case PG106_GRBOOT_MASK_REG:
			v = bus->rd32(bus->user, a);
			bus->wr32(bus->user, a, (v & ~s->mask) | s->value);
			break;
		case PG106_GRBOOT_POLL_REG:
			ret = pg106_hw_poll(bus, a, s->mask, s->value,
					    s->poll_op == PG106_GRBOOT_POLL_ANY,
					    s->timeout_us, &v, r);
			if (ret) {
				fail(r, ret, i, s->kind, s->addr,
				     s->value & s->mask, v, s->why);
				return r->ret;
			}
			if (s->hw_value && observe)
				observe(observe_user, i, s, v);
			break;
		case PG106_GRBOOT_READ_REG:
			v = bus->rd32(bus->user, a);
			if (s->hw_value) {
				if ((v & s->mask) != (s->value & s->mask))
					r->hw_mismatches++;
				if (observe)
					observe(observe_user, i, s, v);
			} else if ((v & s->mask) != (s->value & s->mask)) {
				fail(r, -EIO, i, s->kind, s->addr,
				     s->value & s->mask, v & s->mask, s->why);
				return r->ret;
			}
			break;
		case PG106_GRBOOT_FALCON_START:
			// nvkm_falcon_v1_start (falcon/v1.c:93-101).
			v = bus->rd32(bus->user, a);
			bus->wr32(bus->user, (v & 0x40u) ? a + 0x30u : a, s->value);
			r->starts++;
			r->start_cpuctl = v;
			if (observe)
				observe(observe_user, i, s, v);
			break;
		default:
			fail(r, -EINVAL, i, s->kind, s->addr, 0, 0, s->why);
			return r->ret;
		}
	}
	r->step = i;
	return 0;
}

// nv50_instobj_wr32_slow (instmem/nv50.c:61-77): window base = pa &
// 0xffffff00000, offset = pa & 0xfffff, 0x001700 = base >> 16 (:420).
static uint32_t
pramin_at(const struct pg106_hw_bus *bus, uint64_t pa, uint64_t *window)
{
	const uint64_t base = pa & 0xffffff00000ull;

	if (*window != base) {
		bus->wr32(bus->user, PG106_PRAMIN_BASE_REG, (uint32_t)(base >> 16));
		*window = base;
	}
	return PG106_PRAMIN_WINDOW + (uint32_t)(pa & 0xfffffull);
}

uint32_t
pg106_hw_pramin_rd32(const struct pg106_hw_bus *bus, uint64_t pa,
		     uint64_t *window)
{
	return bus->rd32(bus->user, pramin_at(bus, pa & ~3ull, window));
}

int
pg106_hw_pramin_write(const struct pg106_hw_bus *bus, uint64_t pa,
		      const uint8_t *src, uint32_t len, uint64_t *window)
{
	uint32_t done = 0;

	if (!bus_ok(bus) || !window || pa + len > (1ull << 40) || pa + len < pa)
		return -EINVAL;
	while (done < len) {
		const uint64_t at = pa + done;
		const uint32_t lane = (uint32_t)(at & 3u);
		uint32_t n = 4u - lane, w = 0, k;

		if (n > len - done)
			n = len - done;
		if (lane || n < 4) // partial word: read-modify-write
			w = pg106_hw_pramin_rd32(bus, at, window);
		for (k = 0; k < n; k++) {
			const uint32_t sh = 8u * (lane + k);
			const uint8_t b = src ? src[done + k] : 0;

			w = (w & ~(0xffu << sh)) | ((uint32_t)b << sh);
		}
		bus->wr32(bus->user, pramin_at(bus, at & ~3ull, window), w);
		done += n;
	}
	return 0;
}

struct pg106_flcn_bus
pg106_hw_flcn_bus(const struct pg106_hw_bus *bus)
{
	struct pg106_flcn_bus f;

	f.ctx = bus->user;
	f.rd32 = bus->rd32;
	f.wr32 = bus->wr32;
	return f;
}

struct pg106_pmu_bus
pg106_hw_pmu_bus(const struct pg106_hw_bus *bus)
{
	struct pg106_pmu_bus p;

	p.rd32 = bus->rd32;
	p.wr32 = bus->wr32;
	p.falcon_reset = NULL;
	p.time = bus->delay_us;
	p.user = bus->user;
	return p;
}
