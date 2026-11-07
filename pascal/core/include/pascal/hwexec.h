// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/hwexec.h — execute the acr_boot / gr_boot step
// lists against a register bus, with real poll timeouts.
//
// The plans (acr_boot.h, gr_boot.h) are ordered lists of register writes,
// read-modify-writes, polls, delays and memory-object writes, each with the
// nouveau line it comes from. Until now only test replayers ran them. This
// is the one executor both the tests and the kext use: the kext supplies
// rd32/wr32/delay_us over its BAR0 mapping (pascal/kext/src/PG106.cpp) and
// a memory-object writer; tests supply the existing mocks.
//
// Semantics, per step kind (acr_boot.h / gr_boot.h step comments):
//   WRITE_REG   MMIO: wr32. WPR/IMG (ACR): mem_write of the LE word.
//   WRITE_BLOB  ACR: mem_write(space, addr, blob or zero fill, len).
//               GR: the sw_nonctx register pack — {u32 addr, u32 data}
//               pairs written in order (gm200_gr_load -> gk20a_gr_av_to_init,
//               engine/gr/gk20a.c:30-65; applied by gf100_gr_mmio,
//               gf100.c:1079-1093, from gf100_gr_init, gf100.c:2358-2359).
//               Without the pack the step fails with -ENOENT: the plan only
//               knows its size.
//   MASK_REG    v = rd32; wr32((v & ~mask) | value) (nvkm_mask).
//   POLL_REG    re-read until (rd32 & mask) meets the op, waiting
//               poll_us between reads; -ETIMEDOUT once timeout_us has been
//               spent (nvkm_msec semantics: the budget is time, not reads).
//   READ_REG    rd32; (v & mask) must equal (value & mask), else -EIO.
//               GR steps flagged hw_value carry stand-ins from the config,
//               so a mismatch there is counted, reported through observe(),
//               and not fatal.
//   DELAY_US    delay_us(value).
// Any failure stops at that step; nothing after it is issued.
//
// PRAMIN: pg106_hw_pramin_write() writes VRAM through the BAR0 window at
// 0x700000 whose 1 MiB-aligned base is set in 0x001700 (base >> 16), the
// nv50 instmem slow path GP106 uses (subdev/instmem/nv50.c:61-77 wr32_slow,
// :420 set_bar0_window_addr). The kext's mem_write for the ACR WPR space
// can be built on it; see pascal/kext/README.md for what is still missing
// before the ACR plan can run on hardware (instance block + VMM for IMG).
//
// C11, libc only. Errors are negative errno.

#ifndef PASCAL_HWEXEC_H
#define PASCAL_HWEXEC_H

#include <pascal/acr_boot.h>
#include <pascal/devinit_pmu.h>
#include <pascal/gr_boot.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Default poll interval (microseconds between reads of a POLL_REG step).
#define PG106_HW_POLL_US_DEFAULT 10u

// PRAMIN window (subdev/instmem/nv50.c).
#define PG106_PRAMIN_WINDOW	 0x00700000u
#define PG106_PRAMIN_WINDOW_SIZE 0x00100000u
#define PG106_PRAMIN_BASE_REG	 0x00001700u

struct pg106_hw_bus {
	void *user;
	uint32_t (*rd32)(void *user, uint32_t addr);         // required
	void (*wr32)(void *user, uint32_t addr, uint32_t v); // required
	void (*delay_us)(void *user, uint32_t us);           // required
	// Memory objects of the ACR plan (PG106_ACRBOOT_WPR / _IMG); src NULL
	// = zero fill. NULL hook: an ACR plan with WPR/IMG steps fails with
	// -ENOSYS at the first one.
	int (*mem_write)(void *user, uint8_t space, uint64_t off,
			 const uint8_t *src, uint32_t len);
	uint32_t poll_us; // 0 = PG106_HW_POLL_US_DEFAULT
};

struct pg106_hw_result {
	int ret;              // 0 or negative errno
	size_t step;          // failing step (or steps run on success)
	uint8_t kind;         // failing step kind
	uint64_t addr;        // failing step address
	uint32_t expect;      // failing READ/POLL: expected (value & mask)
	uint32_t got;         // failing READ/POLL: last value read (masked)
	const char *why;      // failing step's nouveau citation
	uint64_t waited_us;   // total time spent in polls + delays
	size_t polls;         // reads issued by POLL_REG steps
	size_t hw_mismatches; // GR hw_value READ_REG mismatches (not fatal)
};

// Called for GR READ_REG / POLL_REG steps flagged hw_value, with the value
// the bus returned: the discovery reads and FECS size replies a caller
// needs (ctx_size etc.). May be NULL.
typedef void (*pg106_hw_observe_fn)(void *user, size_t step,
				    const struct pg106_grboot_step *s,
				    uint32_t value);

// Run every step of an ACR plan. res may be NULL.
int pg106_hwexec_acr(const struct pg106_hw_bus *bus,
		     const struct pg106_acrboot_plan *plan,
		     struct pg106_hw_result *res);
// Steps [from, to) only (to clamped to nsteps).
int pg106_hwexec_acr_range(const struct pg106_hw_bus *bus,
			   const struct pg106_acrboot_plan *plan, size_t from,
			   size_t to, struct pg106_hw_result *res);

// Run GR plan steps [from, to) (to clamped to nsteps). The SEC2
// BOOTSTRAP_FALCON exchange belongs at info.bootstrap_before, so a caller
// runs [0, bootstrap_before), bootstraps, then [bootstrap_before, nsteps).
// sw_nonctx/sw_nonctx_len: the gr/sw_nonctx.bin pack (may be NULL; the
// WRITE_BLOB step then fails with -ENOENT; a length that is not a multiple
// of 8 is -EINVAL).
int pg106_hwexec_gr(const struct pg106_hw_bus *bus,
		    const struct pg106_grboot_plan *plan, size_t from,
		    size_t to, const uint8_t *sw_nonctx, size_t sw_nonctx_len,
		    pg106_hw_observe_fn observe, void *observe_user,
		    struct pg106_hw_result *res);

// One poll with the executor's timing rules, for callers outside a plan.
// Returns 0 or -ETIMEDOUT; *got = last (value & mask).
int pg106_hw_poll(const struct pg106_hw_bus *bus, uint32_t addr,
		  uint32_t mask, uint32_t value, bool any, uint32_t timeout_us,
		  uint32_t *got, struct pg106_hw_result *acct);

// Write len bytes (src NULL = zeros) to VRAM physical address `pa` through
// the PRAMIN window. *window caches the programmed 0x1700 base (pass a
// variable initialised to UINT64_MAX). Unaligned head/tail bytes are
// read-modify-written. Returns 0 or -EINVAL (pa beyond 2^40).
int pg106_hw_pramin_write(const struct pg106_hw_bus *bus, uint64_t pa,
			  const uint8_t *src, uint32_t len, uint64_t *window);
uint32_t pg106_hw_pramin_rd32(const struct pg106_hw_bus *bus, uint64_t pa,
			      uint64_t *window);

// Adapters for the modules that take their own bus types: SEC2 queues
// (acr_boot.h pg106_flcn_bus) and the POST model (devinit_pmu.h
// pg106_pmu_bus; its time hook maps to delay_us, falcon_reset stays NULL
// = nouveau's nvkm_falcon_reset is not modelled on this path).
struct pg106_flcn_bus pg106_hw_flcn_bus(const struct pg106_hw_bus *bus);
struct pg106_pmu_bus pg106_hw_pmu_bus(const struct pg106_hw_bus *bus);

#endif // PASCAL_HWEXEC_H
