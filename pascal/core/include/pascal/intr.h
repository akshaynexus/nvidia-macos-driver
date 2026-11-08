// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/intr.h — PMC interrupt routing for the GP106
// (gp100_mc, nvkm/subdev/mc/gp100.c:25-110).
//
// Leaf 0 is PMC_INTR_0: pending 0x000100 (nv04_mc_intr_pending), allow
// 0x000160 (gp100_mc_intr_allow), block 0x000180 (gp100_mc_intr_block);
// leaf 1 (0x104/0x164/0x184) is the non-stall tree gp100 also has
// (intr_nonstall = true), blocked here and never allowed: the FIFO's
// non-stall event arrives as PFIFO 0x2100 bit 31 under PMC bit 8 (leaf 0,
// gk104_fifo_intr). Unit bits (gp100_mc_intrs): DISP 0x04000000, FIFO
// 0x100, FAULT 0x200, PRIVRING 0x40000000, BUS 0x10000000, FB 0x08002000,
// LTC 0x02000000, PMU 0x01000000, GPIO/I2C 0x200000, TIMER 0x100000,
// THERM 0x40000, TOP engines 0x9000 (GR 0x1000 = PTOP intr 12).
//
// Use from an OS interrupt (the kext's IOFilterInterruptEventSource):
//   primary (filter):  if (pg106_intr_pending(i)) { pg106_intr_unarm(i);
//                      schedule the secondary }   (gp100_mc_intr_unarm)
//   secondary (work loop, gate held): pg106_intr_service(i);
//                      pg106_intr_rearm(i)        (gp100_mc_intr_rearm)
// A pending bit no handler claims is blocked (written to 0x180) so it
// cannot storm, and counted.
//
// SIMULATION status: tests/test_intr.c on tests/mockgpu.h.

#ifndef PASCAL_INTR_H
#define PASCAL_INTR_H

#include <pascal/hwexec.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PG106_PMC_INTR_0	0x000100u
#define PG106_PMC_INTR_ALLOW_0	0x000160u
#define PG106_PMC_INTR_BLOCK_0	0x000180u
#define PG106_PMC_INTR_BLOCK_1	0x000184u
#define PG106_INTR_MAX		8u

typedef void (*pg106_intr_fn)(void *ctx, uint32_t pending);

struct pg106_intr {
	const struct pg106_hw_bus *bus;
	struct {
		uint32_t mask;
		pg106_intr_fn fn;
		void *ctx;
	} h[PG106_INTR_MAX];
	uint32_t allowed;   // leaf-0 bits armed by rearm
	uint32_t blocked;   // unhandled bits blocked for good
	uint64_t services, unhandled, spurious;
	uint32_t last;      // pending word of the last service
};

// Block everything (both leaves); no handlers.
void pg106_intr_init(struct pg106_intr *i, const struct pg106_hw_bus *bus);
// Route `mask` to fn(ctx) (fn NULL removes the handler for exactly that
// mask) and allow / block the bits accordingly. -ENOSPC, -EBUSY when an
// existing handler already owns part of mask.
int pg106_intr_set(struct pg106_intr *i, uint32_t mask, pg106_intr_fn fn,
		   void *ctx);
// Primary-context helpers: no allocation, two MMIO accesses at most.
bool pg106_intr_pending(const struct pg106_intr *i);
void pg106_intr_unarm(const struct pg106_intr *i);
void pg106_intr_rearm(const struct pg106_intr *i);
// Secondary context: read PMC_INTR_0, call the handlers. Returns the
// pending word (masked by what is allowed).
uint32_t pg106_intr_service(struct pg106_intr *i);
// After a power cycle the enables are gone: block all, then rearm.
void pg106_intr_restore(struct pg106_intr *i);

#ifdef __cplusplus
}
#endif

#endif // PASCAL_INTR_H
