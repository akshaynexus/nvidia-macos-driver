// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/uclient.h — render-client bookkeeping the kext's
// IOKit glue (PG106UserClient.cpp, PG106.cpp) keeps pure and host-tested:
//
// 1. BAR1 user windows. A VRAM BO with CAN_MAP gets a BAR1 window, and the
//    client maps that window's physical range (IOConnectMapMemory64). The
//    mapping outlives the BO: IOKit keeps it until the task unmaps it, exits
//    or the connection is freed, and it is not tied to the BO. If the window
//    and its VRAM went back to the allocators while it is mapped, the next
//    allocation (another process, another user) gets the same window and
//    the stale mapping reads and writes it. So a window is never given back
//    while any mapping of it exists:
//      - pg106_uwins_free: the BO is gone; an unmapped window is given back
//        now, a mapped one is held (FREED) until pg106_uwins_reap finds its
//        last mapping gone.
//      - pg106_uwins_lost: the GPU's memory is gone (system sleep, D3cold, a
//        re-boot). Every window is cut off from the CPU (ops->block: IOKit
//        redirect) and no longer belongs to any allocator (!LIVE).
//      - pg106_uwins_reserve: a new BAR1 allocator (pg106_gpu_boot, right
//        after BAR1 init, before anything maps through it): every window
//        that is still mapped is taken out of its free space (RESERVED), so
//        the new allocator never hands it to anyone while the stale mapping
//        exists. Reaped later like any held window.
//      - pg106_uwins_scratch: at the end of a boot, the PTEs of every
//        RESERVED window point at one VRAM page nobody owns (SCRATCH).
//      - pg106_uwins_settle: a blocked window must not stay blocked: a CPU
//        access sleeps in IOKit uninterruptibly until the redirect is lifted,
//        so the thread could never be killed and its connection never close.
//        With the GPU up, a SCRATCH window is unblocked: a stale access reads
//        or writes the scratch page, never another client's VRAM. Every
//        other blocked window (the GPU failed or is off, its window could
//        not be reserved or backed) has its mappings cut (ops->cut) and is
//        then unblocked: the sleeper wakes, finds no mapping and faults in
//        its task like any bad access. A sleep in progress settles nothing
//        (the wake that follows does).
//    Whether a mapping exists is the caller's question (ops->mapped; the
//    kext: the descriptor's retain count beyond the registry's own). That
//    count also covers a thread asleep in the fault, so a window is reaped
//    after it was unblocked, never before.
//
// 2. Render channel slots: one per connection, up to `max` at once. When the
//    GPU's channels go away (grDrop: sleep, D3cold) every slot is freed, so
//    a connection that is DEVICE_LOST holds none, and its later release
//    finds no slot and touches nothing.
//
// C11, no libc beyond the klibc subset (stdlib realloc/free, string.h).

#ifndef PASCAL_UCLIENT_H
#define PASCAL_UCLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <pascal/vram.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- BAR1 user windows ---------------------------------------------------------

enum {
	PG106_UWIN_LIVE     = 1u << 0, // allocated in the current BAR1 allocator
	PG106_UWIN_RESERVED = 1u << 1, // LIVE by pg106_uwins_reserve: VA range
				       // only (no PTEs, no VRAM of its own)
	PG106_UWIN_FREED    = 1u << 2, // the BO is gone; waits for its mappings
	PG106_UWIN_BLOCKED  = 1u << 3, // ops->block ran, ops->unblock has not
	PG106_UWIN_SCRATCH  = 1u << 4, // RESERVED, every PTE -> the scratch page
};

struct pg106_uwin {
	void *obj;         // the caller's mappable object (kext: the descriptor)
	void *client;      // the connection whose map set holds the mappings of
			   // obj (kext: the PG106UserClient), or NULL once gone
	uint64_t bar1_off; // BAR1 window
	uint64_t size;     // bytes, page multiple
	uint64_t vram_pa;  // VRAM behind it (LIVE and not RESERVED)
	uint32_t flags;
};

struct pg106_uwin_ops {
	void *user;
	// A user mapping of obj exists (or is being created).
	bool (*mapped)(void *user, void *obj);
	// LIVE, not RESERVED: unmap the BAR1 window and free its VRAM.
	void (*give_back)(void *user, const struct pg106_uwin *w);
	// The memory is gone: stop CPU access through every mapping of obj.
	void (*block)(void *user, void *obj);
	// Drop the registry's reference to obj (the entry is gone).
	void (*put)(void *user, void *obj);
	// Lift ops->block: threads asleep in a fault of obj run again.
	void (*unblock)(void *user, void *obj);
	// Remove every CPU mapping of obj that connection `client` holds (NULL
	// when it is gone). No wait on the mapping's threads.
	void (*cut)(void *user, void *client, void *obj);
	// A RESERVED | SCRATCH window goes: unmap its PTEs and free its VA.
	void (*scratch_unmap)(void *user, const struct pg106_uwin *w);
};

struct pg106_uwins {
	struct pg106_uwin *w;
	size_t n, cap;
	struct pg106_range *va; // the BAR1 allocator the RESERVED ranges are in
	// counters (registry)
	uint32_t added, held, given_back, dropped, reserved, reserve_failed;
	uint32_t scratched, scratch_failed, unblocked, cut;
};

void pg106_uwins_init(struct pg106_uwins *u);
// Every entry's obj is put; nothing is given back (the GPU goes away). A
// window still blocked is cut and unblocked first: nothing lifts it later.
void pg106_uwins_fini(struct pg106_uwins *u, const struct pg106_uwin_ops *ops);
// A new LIVE window. The registry owns one reference to obj from here on
// (ops->put when the entry goes). -ENOMEM, -EINVAL (size 0, obj NULL).
int pg106_uwins_add(struct pg106_uwins *u, void *obj, void *client,
		    uint64_t bar1_off, uint64_t size, uint64_t vram_pa);
// The connection is being freed (its map set goes with it): no entry names it
// any more.
void pg106_uwins_disown(struct pg106_uwins *u, const void *client);
// obj's BO is freed. 1 = given back / dropped now, 0 = held (still mapped),
// -ENOENT = not registered.
int pg106_uwins_free(struct pg106_uwins *u, void *obj,
		     const struct pg106_uwin_ops *ops);
// Every FREED entry whose last mapping is gone: given back (LIVE), its
// RESERVED range returned, or dropped (!LIVE). Returns how many went.
size_t pg106_uwins_reap(struct pg106_uwins *u, const struct pg106_uwin_ops *ops);
// The BAR1 allocator and VRAM are gone: every entry !LIVE and blocked.
// FREED entries with no mapping left go now.
void pg106_uwins_lost(struct pg106_uwins *u, const struct pg106_uwin_ops *ops);
// A new BAR1 allocator `va`, before its first allocation: every !LIVE entry
// that is still mapped is reserved in it (LIVE | RESERVED); FREED unmapped
// ones go. Returns the number of windows that could not be reserved (they
// stay !LIVE and blocked; nothing hands them out knowingly).
int pg106_uwins_reserve(struct pg106_uwins *u, struct pg106_range *va,
			const struct pg106_uwin_ops *ops);
// Every RESERVED window without scratch PTEs: map(ctx, off, size) points them
// at the scratch page (SCRATCH). Returns how many could not be (they stay
// blocked until pg106_uwins_settle cuts them).
int pg106_uwins_scratch(struct pg106_uwins *u,
			int (*map)(void *ctx, uint64_t off, uint64_t size), void *ctx);
// up: the new BAR1 allocator is serving (SCRATCH windows unblock, anything
// else blocked is cut and unblocked). !up: the GPU is off or failed for good
// (every blocked window is cut and unblocked); never call it for a sleep
// that will end in a boot. Returns how many windows were unblocked.
size_t pg106_uwins_settle(struct pg106_uwins *u, bool up,
			  const struct pg106_uwin_ops *ops);
// The entry of obj, or NULL.
const struct pg106_uwin *pg106_uwins_find(const struct pg106_uwins *u,
					  const void *obj);
// Bytes of BAR1 held by FREED or RESERVED entries.
uint64_t pg106_uwins_held_bytes(const struct pg106_uwins *u);

// ---- render channel slots ---------------------------------------------------------

#define PG106_RSLOTS_MAX 8

struct pg106_rslots {
	const void *owner[PG106_RSLOTS_MAX]; // NULL = free
	uint32_t max;                        // slots in use at most (<= MAX)
};

void pg106_rslots_init(struct pg106_rslots *r, uint32_t max);
// Slot index of owner, or -1.
int pg106_rslots_find(const struct pg106_rslots *r, const void *owner);
// owner's slot, or the first free one below max (not yet taken: the caller
// sets it with pg106_rslots_take once its channel exists). -EBUSY: none
// free; -EINVAL: owner NULL.
int pg106_rslots_pick(const struct pg106_rslots *r, const void *owner);
void pg106_rslots_take(struct pg106_rslots *r, int slot, const void *owner);
// owner's slot freed; returns its index, or -1 (no slot: dropped already).
int pg106_rslots_release(struct pg106_rslots *r, const void *owner);
// Every slot freed (the channels are gone with the GPU).
void pg106_rslots_drop(struct pg106_rslots *r);

#ifdef __cplusplus
}
#endif

#endif // PASCAL_UCLIENT_H
