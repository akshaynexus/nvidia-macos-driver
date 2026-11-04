// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/mmio.h — GP106 BAR0 (PRI) engine-block map.
//
// Provenance: every base is an NVIDIA SDK swref macro (third_party/
// open-gpu-kernel-modules, src/common/inc/swref/published/...) or a nouveau
// file literal; the old envytools NV50-era map (PTIMER at 0x020000, a fixed
// "PCOPY" window at 0x060000) was wrong for GF100+ — several ranges MOVED or
// never existed on Pascal. Old-vs-new table with file:line citations:
// research/evidence/MMIO-MAP-CORRECTIONS.md. The same 10-entry dict is
// cross-recorded in helios300-gp106-simtest.py section 2; tests/test_pmc.c
// round-trips each value against the simtest's literals.
//
// Layout notes (GF100+/GP106):
// - PMC sits at offset 0 so PMC_BOOT_0 (pascal/pmc.h PG106_PMC_BOOT_0) is
//   BAR0 word 0 — base.c does `boot0 = nvkm_rd32(device, 0x000000);` right
//   after mapping the PRI.
// - GR/SEC2/CE engine blocks are NOT fixed ranges: on GF100+ they are
//   discovered at runtime through the PTOP device-info table (BAR0
//   0x022700+i*4, pascal/top.h PG106_TOP_INDEX); PGRAPH below is kept as a
//   convenience range because its top-table address is invariant (0x400000,
//   pascal/tests/test_top.c golden; nouveau gr/gf100.c puts the GR falcons at
//   0x418000/0x41a000 inside it).
// - The spans are not the whole story: the SDK defines the display device as
//   NV_PDISP 0x006F1FFF:0x00610000. We split that envelope into the
//   direct-register window (PDISP: PMC-enable-adjacent regs, SOR power at
//   0x61c004, ...) and the EVO core-channel method window (EVO: 0x640000+;
//   gf119.c reads latched head state back at 0x640414 + head*0x300). The
//   middle of the envelope (0x620000-0x63ffff) and the tail (0x650000+)
//   remain unenumerated gaps in this map.

#ifndef PASCAL_MMIO_H
#define PASCAL_MMIO_H

#include <stdbool.h>
#include <stdint.h>

// Block order matches helios300-gp106-simtest.py MMIO dict order.
enum pg106_mmio_block {
	PG106_MMIO_NONE = -1,
	PG106_MMIO_PMC = 0,   // 0x000000: host interface, BOOT_0/BOOT_1
	PG106_MMIO_PBUS,      // 0x001000: bus / PCI config shadow
	PG106_MMIO_PFIFO,     // 0x002000: FIFO / runlist / PBDMA control
	PG106_MMIO_PTIMER,    // 0x009000: time / alarm (MOVED from 0x020000)
	PG106_MMIO_PTOP,      // 0x022000: device-info/top table + devinit latch (NEW)
	PG106_MMIO_PFB,       // 0x100000: framebuffer / MMU
	PG106_MMIO_PPMU,      // 0x10a000: PMU falcon (nouveau runs it nofw)
	PG106_MMIO_PGRAPH,    // 0x400000: 3D/compute engine (top-table GR base)
	PG106_MMIO_PDISP,     // 0x610000: display direct registers (gf119 layout)
	PG106_MMIO_PEVO,      // 0x640000: EVO core-channel method window (NEW)
	PG106_MMIO_BLOCK_COUNT,
};

struct pg106_mmio_range {
	const char *name;
	uint32_t base; // first byte of the block, inclusive
	uint32_t end;  // last byte of the block, inclusive
};

// base/end literals: helios300-gp106-simtest.py, section 2 MMIO dict
// (origin: SDK swref headers + nouveau, cited per line). Keep byte-identical
// to the simtest.
static const struct pg106_mmio_range pg106_mmio_map[PG106_MMIO_BLOCK_COUNT] = {
	// NV_PMC_BOOT_0 = 0x00000000; NV_PMC_ENABLE = 0x00000200.
	[PG106_MMIO_PMC]   = { "PMC",   0x000000u, 0x000fffu }, // gm107/dev_pmc.h:27,38
	// PBUS stayed in the 0x001000 page on GF100+: NV_PBUS_INTR_0 0x1100,
	// NV_PBUS_BAR0_WINDOW 0x1700, NV_PBUS_BAR2_BLOCK 0x1714.
	[PG106_MMIO_PBUS]  = { "PBUS",  0x001000u, 0x001fffu }, // gm107/dev_bus.h:26,43,63
	// PFIFO stayed at 0x002000: NV_PFIFO_FB_IFACE 0x26f0; nouveau
	// fifo/gk104.c runlist 0x2700/0x2274, intr 0x2100, runlist kick 0x2a00.
	[PG106_MMIO_PFIFO] = { "PFIFO", 0x002000u, 0x003fffu }, // gm107/dev_fifo.h:26; fifo/gk104.c:446,749
	// MOVED: NV50's PTIMER 0x020000 is NV04/NV50-era; on GF100+ the block
	// is at 0x9400 (TIME_0) / 0x9410 (TIME_1), 0x9100-0x9480 overall.
	[PG106_MMIO_PTIMER]= { "PTIMER",0x009000u, 0x009fffu }, // gm107/dev_timer.h:26-40; kepler/gk104/dev_timer.h:35
	// NEW: the 0x022000 page is the PTOP/device-info block: top table
	// 0x022700+i*4, devinit disable flags 0x022500, devinit-ran latch
	// 0x02240c. SDK names it NV_PTOP on GA100 (0x224fc/0x22800).
	[PG106_MMIO_PTOP]  = { "PTOP",  0x022000u, 0x022fffu }, // ga100/dev_top.h:28-29; top/gk104.c:43; devinit/gf100.c:70,101
	// PFB: NV_PFB_NISO_FLUSH_SYSMEM_ADDR 0x100c10, PRI_MMU_INVALIDATE_PDB
	// 0x100cb8, PRI_MMU_LOCAL_MEMORY_RANGE 0x100ce0; WPR check 0x100cd4.
	[PG106_MMIO_PFB]   = { "PFB",   0x100000u, 0x1fffffu }, // gp102/dev_fb.h:26; gm107/dev_fb.h:26,29; acr/gm200.c:57
	// PMU falcon: nouveau hardcodes the base for gm107+ —
	// nvkm_falcon_ctor(..., 0x10a000, &pmu->falcon).
	[PG106_MMIO_PPMU]  = { "PPMU",  0x10a000u, 0x10afffu }, // pmu/base.c:145
	// GR base from the top table (type 0 -> GR), 0x400000 on every GF100+
	// sample; GPCCS falcon 0x41a000 caps the window at 0x41ffff.
	[PG106_MMIO_PGRAPH]= { "PGRAPH",0x400000u, 0x41ffffu }, // test_top.c golden; gr/gf100.c:2649
	// Display direct regs inside NV_PDISP 0x006F1FFF:0x00610000: SOR power
	// 0x61c004 (nv50.c nv50_sor_power), head HDMI regs 0x610800+head*0x800.
	[PG106_MMIO_PDISP] = { "PDISP", 0x610000u, 0x61ffffu }, // disp/v04_02/dev_disp.h:26
	// EVO core-channel method window: gf119 core channel latches cl907d
	// methods at 0x640000 + method offset; per-channel windows stride
	// 0x1000 (0x640000 + ctrl*0x1000); head readback 0x640414+hoff.
	[PG106_MMIO_PEVO]  = { "EVO",   0x640000u, 0x64ffffu }, // disp/gf119.c:429,593,962
};

// True when addr falls inside the block's inclusive [base, end] range.
static inline bool
pg106_mmio_contains(enum pg106_mmio_block block, uint32_t addr)
{
	if ((unsigned)block >= PG106_MMIO_BLOCK_COUNT)
		return false;
	return addr >= pg106_mmio_map[block].base && addr <= pg106_mmio_map[block].end;
}

// Block containing addr, or PG106_MMIO_NONE for unmapped gaps (e.g. 0x021000
// or 0x023000 — BAR0 is sparse, not contiguous; the whole 0x004000-0x008fff
// and 0x023000-0x0fffff spans are gaps too, engine/CE falcon bases among
// them: on GP106 SEC2's top-table address 0x3c0000 resolves to NONE by
// design, its real base is runtime-discovered via PTOP).
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
