// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/src/intr.c — PMC interrupt routing (gp100_mc). See intr.h.

#include <pascal/intr.h>

#include <errno.h>
#include <string.h>

static void
wr(const struct pg106_intr *i, uint32_t a, uint32_t v)
{
	i->bus->wr32(i->bus->user, a, v);
}

void
pg106_intr_init(struct pg106_intr *i, const struct pg106_hw_bus *bus)
{
	memset(i, 0, sizeof(*i));
	i->bus = bus;
	// gp100_mc_intr_unarm: block every bit of both leaves.
	wr(i, PG106_PMC_INTR_BLOCK_0, 0xffffffffu);
	wr(i, PG106_PMC_INTR_BLOCK_1, 0xffffffffu);
}

int
pg106_intr_set(struct pg106_intr *i, uint32_t mask, pg106_intr_fn fn,
	       void *ctx)
{
	unsigned k, freek = PG106_INTR_MAX;

	if (!i || !mask)
		return -EINVAL;
	for (k = 0; k < PG106_INTR_MAX; k++) {
		if (!i->h[k].mask) {
			if (freek == PG106_INTR_MAX)
				freek = k;
			continue;
		}
		if (i->h[k].mask == mask) {
			if (!fn) {
				memset(&i->h[k], 0, sizeof(i->h[k]));
				i->allowed &= ~mask;
				wr(i, PG106_PMC_INTR_BLOCK_0, mask);
				return 0;
			}
			i->h[k].fn = fn;
			i->h[k].ctx = ctx;
			return 0;
		}
		if (i->h[k].mask & mask)
			return -EBUSY;
	}
	if (!fn)
		return -ENOENT;
	if (freek == PG106_INTR_MAX)
		return -ENOSPC;
	i->h[freek].mask = mask;
	i->h[freek].fn = fn;
	i->h[freek].ctx = ctx;
	i->allowed |= mask;
	i->blocked &= ~mask;
	wr(i, PG106_PMC_INTR_ALLOW_0, mask); // gp100_mc_intr_allow
	return 0;
}

bool
pg106_intr_pending(const struct pg106_intr *i)
{
	uint32_t v = i->bus->rd32(i->bus->user, PG106_PMC_INTR_0);

	// 0xffffffff = the device is gone (D3cold / surprise removal).
	return v != 0xffffffffu && (v & i->allowed) != 0;
}

void
pg106_intr_unarm(const struct pg106_intr *i)
{
	wr(i, PG106_PMC_INTR_BLOCK_0, 0xffffffffu);
}

void
pg106_intr_rearm(const struct pg106_intr *i)
{
	if (i->allowed)
		wr(i, PG106_PMC_INTR_ALLOW_0, i->allowed);
}

uint32_t
pg106_intr_service(struct pg106_intr *i)
{
	uint32_t pend, left;
	unsigned k;

	pend = i->bus->rd32(i->bus->user, PG106_PMC_INTR_0);
	if (pend == 0xffffffffu)
		return 0;
	pend &= i->allowed;
	i->services++;
	i->last = pend;
	if (!pend) {
		i->spurious++;
		return 0;
	}
	left = pend;
	for (k = 0; k < PG106_INTR_MAX; k++)
		if (i->h[k].mask & pend) {
			i->h[k].fn(i->h[k].ctx, pend & i->h[k].mask);
			left &= ~i->h[k].mask;
		}
	if (left) {
		// Nobody owns these: keep them blocked from now on.
		i->unhandled++;
		i->blocked |= left;
		i->allowed &= ~left;
		wr(i, PG106_PMC_INTR_BLOCK_0, left);
	}
	return pend;
}

void
pg106_intr_restore(struct pg106_intr *i)
{
	wr(i, PG106_PMC_INTR_BLOCK_0, 0xffffffffu);
	wr(i, PG106_PMC_INTR_BLOCK_1, 0xffffffffu);
	pg106_intr_rearm(i);
}
