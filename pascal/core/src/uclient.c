// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/src/uclient.c — render-client bookkeeping. See uclient.h.

#include <pascal/uclient.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

// ---- BAR1 user windows ---------------------------------------------------------

void
pg106_uwins_init(struct pg106_uwins *u)
{
	memset(u, 0, sizeof(*u));
}

static void
drop_at(struct pg106_uwins *u, size_t i, const struct pg106_uwin_ops *ops)
{
	void *obj = u->w[i].obj;

	u->w[i] = u->w[--u->n];
	if (ops && ops->put)
		ops->put(ops->user, obj);
}

static bool
is_mapped(const struct pg106_uwin_ops *ops, void *obj)
{
	// No answer = assume mapped: holding a window too long is safe,
	// giving it back early is not.
	return !ops || !ops->mapped || ops->mapped(ops->user, obj);
}

// Give entry i back to whatever owns its range now and remove it.
static void
release_at(struct pg106_uwins *u, size_t i, const struct pg106_uwin_ops *ops)
{
	const struct pg106_uwin *w = &u->w[i];

	if ((w->flags & PG106_UWIN_LIVE) && (w->flags & PG106_UWIN_RESERVED)) {
		if (u->va)
			pg106_range_free(u->va, w->bar1_off, w->size);
		u->given_back++;
	} else if (w->flags & PG106_UWIN_LIVE) {
		if (ops && ops->give_back)
			ops->give_back(ops->user, w);
		u->given_back++;
	} else {
		u->dropped++; // its allocator died with the GPU's memory
	}
	drop_at(u, i, ops);
}

void
pg106_uwins_fini(struct pg106_uwins *u, const struct pg106_uwin_ops *ops)
{
	while (u->n)
		drop_at(u, u->n - 1, ops);
	free(u->w);
	memset(u, 0, sizeof(*u));
}

int
pg106_uwins_add(struct pg106_uwins *u, void *obj, uint64_t bar1_off,
		uint64_t size, uint64_t vram_pa)
{
	if (!obj || !size)
		return -EINVAL;
	if (u->n == u->cap) {
		size_t ncap = u->cap ? u->cap * 2 : 32;
		struct pg106_uwin *n = realloc(u->w, ncap * sizeof(*n));

		if (!n)
			return -ENOMEM;
		u->w = n;
		u->cap = ncap;
	}
	u->w[u->n].obj = obj;
	u->w[u->n].bar1_off = bar1_off;
	u->w[u->n].size = size;
	u->w[u->n].vram_pa = vram_pa;
	u->w[u->n].flags = PG106_UWIN_LIVE;
	u->n++;
	u->added++;
	return 0;
}

static size_t
index_of(const struct pg106_uwins *u, const void *obj)
{
	for (size_t i = 0; i < u->n; i++)
		if (u->w[i].obj == obj)
			return i;
	return SIZE_MAX;
}

const struct pg106_uwin *
pg106_uwins_find(const struct pg106_uwins *u, const void *obj)
{
	const size_t i = index_of(u, obj);

	return i == SIZE_MAX ? NULL : &u->w[i];
}

int
pg106_uwins_free(struct pg106_uwins *u, void *obj, const struct pg106_uwin_ops *ops)
{
	const size_t i = index_of(u, obj);

	if (i == SIZE_MAX)
		return -ENOENT;
	if (!is_mapped(ops, obj)) {
		release_at(u, i, ops);
		return 1;
	}
	u->w[i].flags |= PG106_UWIN_FREED;
	u->held++;
	return 0;
}

size_t
pg106_uwins_reap(struct pg106_uwins *u, const struct pg106_uwin_ops *ops)
{
	size_t gone = 0;

	for (size_t i = u->n; i-- > 0;) {
		if (!(u->w[i].flags & PG106_UWIN_FREED) || is_mapped(ops, u->w[i].obj))
			continue;
		release_at(u, i, ops);
		gone++;
	}
	return gone;
}

void
pg106_uwins_lost(struct pg106_uwins *u, const struct pg106_uwin_ops *ops)
{
	for (size_t i = u->n; i-- > 0;) {
		struct pg106_uwin *w = &u->w[i];

		w->flags &= ~(uint32_t)(PG106_UWIN_LIVE | PG106_UWIN_RESERVED);
		if (ops && ops->block)
			ops->block(ops->user, w->obj);
		if ((w->flags & PG106_UWIN_FREED) && !is_mapped(ops, w->obj))
			release_at(u, i, ops);
	}
	u->va = NULL;
}

int
pg106_uwins_reserve(struct pg106_uwins *u, struct pg106_range *va,
		    const struct pg106_uwin_ops *ops)
{
	int failed = 0;

	u->va = va;
	for (size_t i = u->n; i-- > 0;) {
		struct pg106_uwin *w = &u->w[i];

		if (w->flags & PG106_UWIN_LIVE)
			continue;
		if (!is_mapped(ops, w->obj)) {
			// Nothing can reach the old window any more (a stale
			// connection gets no new mapping): a freed BO goes, a
			// live one stays registered until its free.
			if (w->flags & PG106_UWIN_FREED)
				release_at(u, i, ops);
			continue;
		}
		if (va && pg106_range_reserve(va, w->bar1_off, w->size) == 0) {
			w->flags |= PG106_UWIN_LIVE | PG106_UWIN_RESERVED;
			u->reserved++;
		} else {
			u->reserve_failed++;
			failed++;
		}
	}
	return failed;
}

uint64_t
pg106_uwins_held_bytes(const struct pg106_uwins *u)
{
	uint64_t b = 0;

	for (size_t i = 0; i < u->n; i++)
		if (u->w[i].flags & (PG106_UWIN_FREED | PG106_UWIN_RESERVED))
			b += u->w[i].size;
	return b;
}

// ---- render channel slots ---------------------------------------------------------

void
pg106_rslots_init(struct pg106_rslots *r, uint32_t max)
{
	memset(r, 0, sizeof(*r));
	r->max = max > PG106_RSLOTS_MAX ? PG106_RSLOTS_MAX : max;
}

int
pg106_rslots_find(const struct pg106_rslots *r, const void *owner)
{
	for (uint32_t i = 0; owner && i < PG106_RSLOTS_MAX; i++)
		if (r->owner[i] == owner)
			return (int)i;
	return -1;
}

int
pg106_rslots_pick(const struct pg106_rslots *r, const void *owner)
{
	int s;

	if (!owner)
		return -EINVAL;
	s = pg106_rslots_find(r, owner);
	if (s >= 0)
		return s;
	for (uint32_t i = 0; i < r->max; i++)
		if (!r->owner[i])
			return (int)i;
	return -EBUSY;
}

void
pg106_rslots_take(struct pg106_rslots *r, int slot, const void *owner)
{
	if (slot >= 0 && slot < PG106_RSLOTS_MAX)
		r->owner[slot] = owner;
}

int
pg106_rslots_release(struct pg106_rslots *r, const void *owner)
{
	const int s = pg106_rslots_find(r, owner);

	if (s >= 0)
		r->owner[s] = NULL;
	return s;
}

void
pg106_rslots_drop(struct pg106_rslots *r)
{
	memset(r->owner, 0, sizeof(r->owner));
}
