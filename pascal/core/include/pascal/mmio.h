// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/mmio.h — GP106 BAR0 (PRI) engine-block map.
//
// Provenance: every base is an NVIDIA SDK swref macro (third_party/
// open-gpu-kernel-modules, src/common/inc/swref/published/...) or a nouveau
// file literal; the old envytools NV50-era map (PTIMER at 0x020000, a fixed
// "PCOPY" window at 0x060000) was wrong for GF100+ — several ranges MOVED or
// never existed on Pascal. Old-vs-new table with file:line citations:
// research/evidence/MMIO-MAP-CORRECTIONS.md. The same 14-entry dict is
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
//   pascal/tests/test_top.c golden).
// - GR register space is two windows, both from nouveau gr/gf100.h:40-45:
//   PGRAPH 0x400000-0x41ffff holds FE/FECS (0x409000), ROP_BCAST 0x408800,
//   ROP_UNIT 0x410000+u*0x400 and the GPC/TPC broadcast window GPC_BCAST
//   0x418000+r; per-GPC units live at GPC_UNIT 0x500000+gpc*0x8000 (PPC at
//   +0x3000, TPC at +0x4000+tpc*0x800), i.e. PGPC 0x500000-0x5fffff for
//   GPC_MAX = 32 (gf100.h:36). 0x420000-0x4fffff is not used by nouveau.
// - Host channel space outside PFIFO: PBDMA units at 0x040000 + id*0x2000
//   (fifo/gk104.c:311-330, gf100.c:314-317), PRAMIN at 0x700000 (SDK
//   gm107/dev_ram.h:26), PCCSR channel RAM at 0x800000 + chid*8
//   (fifo/gk104.c:44-77).
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
	PG106_MMIO_PBDMA,     // 0x040000: PBDMA units, 0x2000 stride
	PG106_MMIO_PGPC,      // 0x500000: per-GPC/PPC/TPC GR units
	PG106_MMIO_PRAMIN,    // 0x700000: 1 MiB VRAM window (BAR0_WINDOW)
	PG106_MMIO_PCCSR,     // 0x800000: per-channel control (inst bind/enable)
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
	// sample. This is only the hub + broadcast half of GR: it ends where
	// GPC_BCAST (0x418000+r) ends; per-GPC units are PGPC below.
	[PG106_MMIO_PGRAPH]= { "PGRAPH",0x400000u, 0x41ffffu }, // test_top.c golden; gr/gf100.h:40-42
	// Display direct regs inside NV_PDISP 0x006F1FFF:0x00610000: SOR power
	// 0x61c004 (nv50.c nv50_sor_power), head HDMI regs 0x610800+head*0x800.
	[PG106_MMIO_PDISP] = { "PDISP", 0x610000u, 0x61ffffu }, // disp/v04_02/dev_disp.h:26
	// EVO core-channel method window: gf119 core channel latches cl907d
	// methods at 0x640000 + method offset; per-channel windows stride
	// 0x1000 (0x640000 + ctrl*0x1000); head readback 0x640414+hoff.
	[PG106_MMIO_PEVO]  = { "EVO",   0x640000u, 0x64ffffu }, // disp/gf119.c:429,593,962
	// PBDMA i at 0x040000 + i*0x2000 (gk104_runq_intr: 0x040148/0x04014c
	// + runq->id*0x2000). The live count is PFIFO 0x002004[7:0]
	// (gm200_fifo_runq_nr, fifo/gm200.c:31); window sized for 16 units.
	[PG106_MMIO_PBDMA] = { "PBDMA", 0x040000u, 0x05ffffu }, // fifo/gk104.c:311-330
	// GPC_UNIT(t, r) = 0x500000 + t*0x8000 + r, GPC_MAX = 32.
	[PG106_MMIO_PGPC]  = { "PGPC",  0x500000u, 0x5fffffu }, // gr/gf100.h:36,43-45
	// NV_PRAMIN 0x007FFFFF:0x00700000 — the 1 MiB window that
	// NV_PBUS_BAR0_WINDOW (0x1700) points into VRAM; nouveau instmem/nv50.c
	// accesses it as 0x700000 + addr.
	[PG106_MMIO_PRAMIN]= { "PRAMIN",0x700000u, 0x7fffffu }, // gm107/dev_ram.h:26; instmem/nv50.c:71,91
	// NV_PCCSR_CHANNEL_INST(chid) 0x800000 + chid*8 and _CHANNEL 0x800004
	// + chid*8 (gk104_chan_bind_inst / _start / _stop). chid is 12 bits
	// (PBDMA reads it as & 0xfff, gk104.c:313) -> 4096 * 8 bytes.
	[PG106_MMIO_PCCSR] = { "PCCSR", 0x800000u, 0x807fffu }, // fifo/gk104.c:44,60,68,313
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
// or 0x023000 — BAR0 is sparse, not contiguous; 0x004000-0x008fff,
// 0x023000-0x03ffff and 0x060000-0x0fffff are gaps too, engine falcon
// bases among them: GP106 SEC2 at 0x087000 (measured, see
// tests/test_top.c) resolves to NONE by design — its base is
// runtime-discovered via PTOP).
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
