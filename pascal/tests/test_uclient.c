// SPDX-License-Identifier: GPL-2.0-only
// pascal/tests/test_uclient.c — render-client bookkeeping (core/src/uclient.c)
// as the kext uses it:
//
//   BAR1 user windows: two render clients share one BAR1 allocator and one
//   VRAM allocator (pg106_range, the allocator pg106_bar_map_ex and
//   pg106_vram_alloc use). Client A frees a VRAM BO while its BAR1 window is
//   still mapped; client B allocates next and must not get A's window, nor
//   its VRAM. Across a system sleep (the allocators re-created by the wake's
//   boot) a window still mapped by a DEVICE_LOST connection is blocked and
//   reserved in the new allocator, so the next client cannot alias it.
//   test_suspend.c runs the same through pg106_gpu_boot on the mock GPU.
//
//   Render channel slots: a DEVICE_LOST connection holds no slot after the
//   GPU's channels were dropped (grDrop), so a new client is not refused
//   with -EBUSY, and the stale connection's release frees nobody else's.

#include <pascal/uclient.h>
#include <pascal/vram.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_checks;

#define CHECK(c)                                                         \
	do {                                                             \
		g_checks++;                                              \
		if (!(c)) {                                              \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			g_fail++;                                        \
		}                                                        \
	} while (0)
#define CHECK_EQ(a, b)                                                   \
	do {                                                             \
		long long a_ = (long long)(a), b_ = (long long)(b);      \
		g_checks++;                                              \
		if (a_ != b_) {                                          \
			printf("FAIL %s:%d: %s == %s (%lld vs %lld)\n",  \
			       __FILE__, __LINE__, #a, #b, a_, b_);      \
			g_fail++;                                        \
		}                                                        \
	} while (0)

#define BAR1_SIZE 0x1000000ull // 16 MiB aperture
#define VRAM_SIZE 0x4000000ull
#define WIN 0x100000ull        // every BO: 1 MiB

// One mappable object: what the kext's IOMemoryDescriptor stands for.
struct obj {
	int maps;      // live user mappings
	bool blocked;  // ops->block ran (IOKit redirect)
	int puts;      // ops->put ran
	uint64_t off, pa;
};

struct sys {
	struct pg106_range bar1, vram; // the "current" allocators
	unsigned gen;                  // bumps at every re-creation
	int give_backs;
};

static bool
o_mapped(void *u, void *o)
{
	(void)u;
	return ((struct obj *)o)->maps > 0;
}

static void
o_give_back(void *u, const struct pg106_uwin *w)
{
	struct sys *s = u;

	CHECK_EQ(pg106_range_free(&s->bar1, w->bar1_off, w->size), 0);
	CHECK_EQ(pg106_range_free(&s->vram, w->vram_pa, w->size), 0);
	s->give_backs++;
}

static void
o_block(void *u, void *o)
{
	(void)u;
	((struct obj *)o)->blocked = true;
}

static void
o_put(void *u, void *o)
{
	(void)u;
	((struct obj *)o)->puts++;
}

static void
sys_up(struct sys *s, struct pg106_uwins *uw, const struct pg106_uwin_ops *ops)
{
	CHECK_EQ(pg106_range_init(&s->bar1, 0, BAR1_SIZE, 0x1000), 0);
	CHECK_EQ(pg106_range_init(&s->vram, 0, VRAM_SIZE, 0x1000), 0);
	s->gen++;
	// pg106_gpu_boot: right after BAR1 init, before any BAR1 mapping.
	if (uw)
		CHECK_EQ(pg106_uwins_reserve(uw, &s->bar1, ops), 0);
}

static void
sys_down(struct sys *s, struct pg106_uwins *uw, const struct pg106_uwin_ops *ops)
{
	pg106_uwins_lost(uw, ops); // the kext's memLost (fMemEpoch++)
	pg106_range_fini(&s->bar1);
	pg106_range_fini(&s->vram);
}

// PG106UserClient::memAlloc for a VRAM | CAN_MAP BO.
static int
bo_alloc(struct sys *s, struct pg106_uwins *uw, struct obj *o)
{
	int rc;

	memset(o, 0, sizeof(*o));
	rc = pg106_range_alloc(&s->vram, WIN, 0x1000, false, &o->pa);
	if (rc)
		return rc;
	rc = pg106_range_alloc(&s->bar1, WIN, 0x1000, false, &o->off);
	if (rc)
		return rc;
	return pg106_uwins_add(uw, o, o->off, WIN, o->pa);
}

static bool
overlap(uint64_t a, uint64_t b, uint64_t n)
{
	return a < b + n && b < a + n;
}

static void
test_free_while_mapped(void)
{
	struct sys s = { 0 };
	struct pg106_uwins uw;
	const struct pg106_uwin_ops ops = { &s, o_mapped, o_give_back, o_block, o_put };
	struct obj a, b, c, d;

	pg106_uwins_init(&uw);
	sys_up(&s, NULL, NULL);
	// Client A: a mapped VRAM BO, freed while the mapping lives (the
	// process keeps the pointer; IOKit keeps the IOMemoryMap).
	CHECK_EQ(bo_alloc(&s, &uw, &a), 0);
	a.maps = 1;
	CHECK_EQ(pg106_uwins_free(&uw, &a, &ops), 0); // held
	CHECK(pg106_range_is_allocated(&s.bar1, a.off, WIN));
	CHECK(pg106_range_is_allocated(&s.vram, a.pa, WIN));
	CHECK_EQ(s.give_backs, 0);
	CHECK_EQ(a.puts, 0);
	CHECK_EQ(pg106_uwins_held_bytes(&uw), WIN);
	// Client B allocates next: never A's BAR1 window, never A's VRAM.
	CHECK_EQ(bo_alloc(&s, &uw, &b), 0);
	CHECK(b.off != a.off);
	CHECK(!overlap(b.off, a.off, WIN));
	CHECK(!overlap(b.pa, a.pa, WIN));
	// A reap while A is still mapped gives nothing back.
	CHECK_EQ(pg106_uwins_reap(&uw, &ops), 0);
	CHECK(pg106_range_is_allocated(&s.bar1, a.off, WIN));
	// A unmaps: the next reap returns A's window and VRAM, and only now
	// may a third client get them.
	a.maps = 0;
	CHECK_EQ(pg106_uwins_reap(&uw, &ops), 1);
	CHECK_EQ(a.puts, 1);
	CHECK_EQ(s.give_backs, 1);
	CHECK(!pg106_range_is_allocated(&s.bar1, a.off, WIN));
	CHECK(pg106_uwins_find(&uw, &a) == NULL);
	CHECK_EQ(bo_alloc(&s, &uw, &c), 0);
	CHECK_EQ(c.off, a.off); // first fit: the window A held, now free
	// An unmapped BO goes back at once.
	CHECK_EQ(pg106_uwins_free(&uw, &c, &ops), 1);
	CHECK_EQ(c.puts, 1);
	CHECK(!pg106_range_is_allocated(&s.bar1, c.off, WIN));
	// Two mappings: one unmap is not enough.
	CHECK_EQ(bo_alloc(&s, &uw, &d), 0);
	d.maps = 2;
	CHECK_EQ(pg106_uwins_free(&uw, &d, &ops), 0);
	d.maps = 1;
	CHECK_EQ(pg106_uwins_reap(&uw, &ops), 0);
	d.maps = 0;
	CHECK_EQ(pg106_uwins_reap(&uw, &ops), 1);
	// Unknown object: the caller frees it itself.
	CHECK_EQ(pg106_uwins_free(&uw, &a, &ops), -ENOENT);
	CHECK_EQ(pg106_uwins_free(&uw, &b, &ops), 1);
	CHECK_EQ(uw.n, 0);
	CHECK_EQ(b.puts, 1);
	CHECK(!a.blocked && !b.blocked); // no sleep: nothing redirected
	pg106_uwins_fini(&uw, &ops);
	pg106_range_fini(&s.bar1);
	pg106_range_fini(&s.vram);
	printf("PASS BAR1 windows: client A frees a BO while mapped -> window + VRAM "
	       "held; client B gets neither; given back on A's last unmap, then reused\n");
}

static void
test_sleep(void)
{
	struct sys s = { 0 };
	struct pg106_uwins uw;
	const struct pg106_uwin_ops ops = { &s, o_mapped, o_give_back, o_block, o_put };
	struct obj a, b, h, n1, n2;
	int give0;

	pg106_uwins_init(&uw);
	sys_up(&s, &uw, &ops);
	// Before the sleep: A mapped and live, B live but unmapped, H freed
	// while mapped (held).
	CHECK_EQ(bo_alloc(&s, &uw, &a), 0);
	a.maps = 1;
	CHECK_EQ(bo_alloc(&s, &uw, &b), 0);
	CHECK_EQ(bo_alloc(&s, &uw, &h), 0);
	h.maps = 1;
	CHECK_EQ(pg106_uwins_free(&uw, &h, &ops), 0);
	// System sleep: the memory is gone, every window blocked.
	sys_down(&s, &uw, &ops);
	CHECK(a.blocked && b.blocked && h.blocked);
	CHECK(!(pg106_uwins_find(&uw, &a)->flags & PG106_UWIN_LIVE));
	// Wake: the new BAR1 allocator reserves what is still mapped (A, H)
	// before anything else allocates; B (unmapped: a stale connection
	// gets no new mapping) is not reserved.
	sys_up(&s, &uw, &ops);
	CHECK(pg106_range_is_allocated(&s.bar1, a.off, WIN));
	CHECK(pg106_range_is_allocated(&s.bar1, h.off, WIN));
	CHECK(!pg106_range_is_allocated(&s.bar1, b.off, WIN));
	CHECK_EQ(pg106_uwins_find(&uw, &a)->flags, PG106_UWIN_LIVE | PG106_UWIN_RESERVED);
	CHECK_EQ(uw.reserved, 2);
	// A new client after the wake: none of the stale mapped windows.
	CHECK_EQ(bo_alloc(&s, &uw, &n1), 0);
	CHECK_EQ(bo_alloc(&s, &uw, &n2), 0);
	CHECK(!overlap(n1.off, a.off, WIN) && !overlap(n1.off, h.off, WIN));
	CHECK(!overlap(n2.off, a.off, WIN) && !overlap(n2.off, h.off, WIN));
	// The stale connection closes: its BOs are freed into a registry
	// that knows they belong to no allocator (nothing given back into
	// the new VRAM allocator: its own VRAM died with the old one).
	give0 = s.give_backs;
	CHECK_EQ(pg106_uwins_free(&uw, &b, &ops), 1); // unmapped: dropped
	CHECK_EQ(pg106_uwins_free(&uw, &a, &ops), 0); // mapped: held
	CHECK_EQ(s.give_backs, give0);
	// The stale task unmaps: the reserved ranges return to the new BAR1
	// allocator; the new client's windows are untouched.
	a.maps = 0;
	h.maps = 0;
	CHECK_EQ(pg106_uwins_reap(&uw, &ops), 2);
	CHECK(!pg106_range_is_allocated(&s.bar1, a.off, WIN) || overlap(a.off, n1.off, WIN) ||
	      overlap(a.off, n2.off, WIN));
	CHECK(pg106_range_is_allocated(&s.bar1, n1.off, WIN));
	CHECK(pg106_range_is_allocated(&s.bar1, n2.off, WIN));
	CHECK_EQ(s.give_backs, give0);
	CHECK_EQ(a.puts + b.puts + h.puts, 3);
	// A second sleep with a held window still mapped from two sleeps ago.
	CHECK_EQ(pg106_uwins_free(&uw, &n1, &ops), 1);
	n2.maps = 1;
	sys_down(&s, &uw, &ops);
	sys_up(&s, &uw, &ops);
	sys_down(&s, &uw, &ops);
	sys_up(&s, &uw, &ops);
	CHECK(pg106_range_is_allocated(&s.bar1, n2.off, WIN));
	CHECK_EQ(pg106_uwins_free(&uw, &n2, &ops), 0);
	n2.maps = 0;
	CHECK_EQ(pg106_uwins_reap(&uw, &ops), 1);
	CHECK(!pg106_range_is_allocated(&s.bar1, n2.off, WIN));
	CHECK_EQ(uw.n, 0);
	// A window that cannot be reserved (outside the new aperture) is
	// reported and stays blocked.
	{
		struct obj x;

		CHECK_EQ(bo_alloc(&s, &uw, &x), 0);
		x.maps = 1;
		sys_down(&s, &uw, &ops);
		CHECK_EQ(pg106_range_init(&s.bar1, 0, 0x1000, 0x1000), 0); // a 4 KiB BAR1
		CHECK_EQ(pg106_range_init(&s.vram, 0, VRAM_SIZE, 0x1000), 0);
		CHECK_EQ(pg106_uwins_reserve(&uw, &s.bar1, &ops), 1);
		CHECK(x.blocked);
		CHECK(!(pg106_uwins_find(&uw, &x)->flags & PG106_UWIN_LIVE));
		pg106_uwins_fini(&uw, &ops); // kext stop: references dropped
		CHECK_EQ(x.puts, 1);
		pg106_range_fini(&s.bar1);
		pg106_range_fini(&s.vram);
	}
	printf("PASS BAR1 windows across sleep: stale mapped windows blocked, reserved "
	       "in the new BAR1 allocator before its first use, never handed to the "
	       "next client; released on unmap; an unreservable one reported\n");
}

static void
test_slots(void)
{
	struct pg106_rslots r;
	int a = 1, b = 2, c = 3, d = 4, e = 5, s;

	pg106_rslots_init(&r, 2);
	CHECK_EQ(pg106_rslots_pick(&r, NULL), -EINVAL);
	s = pg106_rslots_pick(&r, &a);
	CHECK_EQ(s, 0);
	pg106_rslots_take(&r, s, &a);
	CHECK_EQ(pg106_rslots_pick(&r, &a), 0); // its own slot again
	s = pg106_rslots_pick(&r, &b);
	CHECK_EQ(s, 1);
	pg106_rslots_take(&r, s, &b);
	CHECK_EQ(pg106_rslots_pick(&r, &c), -EBUSY); // both in use
	// System sleep: grDrop frees every slot. A and B are DEVICE_LOST
	// connections that stay open.
	pg106_rslots_drop(&r);
	CHECK_EQ(pg106_rslots_find(&r, &a), -1);
	// After the wake new clients get channels, not -EBUSY.
	s = pg106_rslots_pick(&r, &c);
	CHECK_EQ(s, 0);
	pg106_rslots_take(&r, s, &c);
	s = pg106_rslots_pick(&r, &d);
	CHECK_EQ(s, 1);
	pg106_rslots_take(&r, s, &d);
	// The stale connections close: their release finds no slot and
	// frees nobody else's.
	CHECK_EQ(pg106_rslots_release(&r, &a), -1);
	CHECK_EQ(pg106_rslots_release(&r, &b), -1);
	CHECK_EQ(pg106_rslots_find(&r, &c), 0);
	CHECK_EQ(pg106_rslots_find(&r, &d), 1);
	CHECK_EQ(pg106_rslots_pick(&r, &e), -EBUSY);
	CHECK_EQ(pg106_rslots_release(&r, &c), 0);
	CHECK_EQ(pg106_rslots_pick(&r, &e), 0);
	pg106_rslots_init(&r, 99);
	CHECK_EQ(r.max, PG106_RSLOTS_MAX);
	printf("PASS render slots: after grDrop DEVICE_LOST connections hold none; "
	       "new clients get slots (no -EBUSY); stale releases free nobody's\n");
}

int
main(void)
{
	printf("== uclient: BAR1 user windows + render slots (pure; the kext's bookkeeping) ==\n");
	test_free_while_mapped();
	test_sleep();
	test_slots();
	printf("%s: %d checks, %d failures\n", g_fail ? "FAILED" : "PASSED", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
