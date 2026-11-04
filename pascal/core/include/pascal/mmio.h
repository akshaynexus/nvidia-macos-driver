// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/mmio.h — GP106 BAR0 (PRI) engine-block map.
//
// Provenance: the base/end addresses are hardware facts from the envytools
// MMIO map (https://envytools.readthedocs.io/en/latest/hw/mmio.html), not
// derived from any single nouveau file; they are cross-recorded in this repo
// in helios300-gp106-simtest.py section 2 (the 9-entry MMIO dict, already
// validated there) and HELIOS300-PASCAL-RESEARCH.md section 6. Every range
// below is byte-identical to that simtest record; tests/test_pmc.c
// round-trips each value against the simtest's literals.
//
// Layout is the GF100+ (Kepler..Pascal) BAR0 map. PMC sits at offset 0 so
// PMC_BOOT_0 (pascal/pmc.h PG106_PMC_BOOT_0) is BAR0 word 0 — base.c does
// `boot0 = nvkm_rd32(device, 0x000000);` right after mapping the PRI.

#ifndef PASCAL_MMIO_H
#define PASCAL_MMIO_H

#include <stdbool.h>
#include <stdint.h>

// Block order matches helios300-gp106-simtest.py MMIO dict order.
enum pg106_mmio_block {
	PG106_MMIO_NONE = -1,
	PG106_MMIO_PMC = 0,   // 0x000000: host interface, BOOT_0/BOOT_1
	PG106_MMIO_PBUS,      // 0x001000: bus / PCI config shadow
	PG106_MMIO_PFIFO,     // 0x002000: FIFO / PBDMA (GPFIFO rings live here)
	PG106_MMIO_PTIMER,    // 0x020000: time / alarm
	PG106_MMIO_PCOPY,     // 0x060000: CE (copy engine) dispatch — PDI
	PG106_MMIO_PFB,       // 0x100000: framebuffer / MMU
	PG106_MMIO_PPMU,      // 0x10A000: PMU falcon (nouveau runs it nofw)
	PG106_MMIO_PGRAPH,    // 0x400000: 3D/compute engine
	PG106_MMIO_PDISP,     // 0x610000: display (CPU-driven on Pascal, gf119 base)
	PG106_MMIO_BLOCK_COUNT,
};

struct pg106_mmio_range {
	const char *name;
	uint32_t base; // first byte of the block, inclusive
	uint32_t end;  // last byte of the block, inclusive
};

// base/end literals: helios300-gp106-simtest.py, section 2 MMIO dict
// (origin: envytools MMIO map). Keep byte-identical to the simtest.
static const struct pg106_mmio_range pg106_mmio_map[PG106_MMIO_BLOCK_COUNT] = {
	[PG106_MMIO_PMC]   = { "PMC",   0x000000u, 0x000fffu },
	[PG106_MMIO_PBUS]  = { "PBUS",  0x001000u, 0x001fffu },
	[PG106_MMIO_PFIFO] = { "PFIFO", 0x002000u, 0x003fffu },
	[PG106_MMIO_PTIMER]= { "PTIMER",0x020000u, 0x020fffu },
	[PG106_MMIO_PCOPY] = { "PCOPY", 0x060000u, 0x06ffffu },
	[PG106_MMIO_PFB]   = { "PFB",   0x100000u, 0x1fffffu },
	[PG106_MMIO_PPMU]  = { "PPMU",  0x10a000u, 0x10afffu },
	[PG106_MMIO_PGRAPH]= { "PGRAPH",0x400000u, 0x41ffffu },
	[PG106_MMIO_PDISP] = { "PDISP", 0x610000u, 0x61ffffu },
};

// True when addr falls inside the block's inclusive [base, end] range.
static inline bool
pg106_mmio_contains(enum pg106_mmio_block block, uint32_t addr)
{
	if ((unsigned)block >= PG106_MMIO_BLOCK_COUNT)
		return false;
	return addr >= pg106_mmio_map[block].base && addr <= pg106_mmio_map[block].end;
}

// Block containing addr, or PG106_MMIO_NONE for unmapped gaps (e.g. 0x021000,
// between PTIMER's end and PCOPY's base — BAR0 is sparse, not contiguous).
//
// The spans are not disjoint: PPMU (0x10a000-0x10afff) nests inside PFB's
// decoded span (0x100000-0x1fffff), exactly as in the simtest record. When
// several blocks match, the most specific (smallest) span wins, so PPMU
// addresses resolve to PPMU regardless of table order.
static inline enum pg106_mmio_block
pg106_mmio_block_at(uint32_t addr)
{
	enum pg106_mmio_block best = PG106_MMIO_NONE;
	uint32_t best_size = 0;

	for (unsigned i = 0; i < PG106_MMIO_BLOCK_COUNT; i++) {
		if (!pg106_mmio_contains((enum pg106_mmio_block)i, addr))
			continue;
		uint32_t size = pg106_mmio_map[i].end - pg106_mmio_map[i].base;
		if (best == PG106_MMIO_NONE || size < best_size) {
			best = (enum pg106_mmio_block)i;
			best_size = size;
		}
	}
	return best;
}

// Name of the block containing addr, or NULL when unmapped.
static inline const char *
pg106_mmio_block_name(uint32_t addr)
{
	enum pg106_mmio_block b = pg106_mmio_block_at(addr);
	return b == PG106_MMIO_NONE ? (const char *)0 : pg106_mmio_map[b].name;
}

#endif // PASCAL_MMIO_H
