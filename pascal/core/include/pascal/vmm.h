// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/vmm.h — GP100-layout VMMs whose page tables
// live in VRAM: the ACR VMM (SEC2's falcon DMA), BAR1, BAR2, and the
// per-client channel VMMs.
//
// A pg106_vmm is core mmu (pascal/mmu.h, the page-table builder) plus:
//   - a VRAM region for its tables, [table_base, table_limit), allocated
//     from pascal/vram.h (tail, like every nvkm_mmu_ptc page);
//   - upload: pg106_mmu_sync writes the changed table words through PRAMIN
//     (leaves before the PDEs that link them);
//   - the TLB invalidate of gf100_vmm_invalidate with gp100's PDB
//     register pair (mmu/vmmgf100.c:187-225, vmmgp100.c:564-579):
//       wait 0x100c80 & 0x00ff0000 (flush slots)       2 s
//       0x100cb8 = lo32(pdb), 0x100cec = hi32(pdb)     unless ALL_PDB
//         pdb = (pd addr >> 12) << 4 | aperture (VRAM 0, HOST 2, NCOH 3)
//       0x100cbc = 0x80000000 | type                   type: PAGE_ALL 1,
//                                                      HUB_ONLY 4 for BAR
//                                                      VMMs (gp100_vmm_flush)
//       wait 0x100c80 & 0x00008000 (flush queued)      2 s
//   - instance-block join (gp100_vmm_join -> gf100_vmm_join_,
//     vmmgp100.c:583-591, vmmgf100.c:342-363): inst+0x200 = pd addr |
//     VER2 (bit 10) | 64KiB big pages (bit 11) | target, inst+0x208 =
//     limit - 1 (both 64-bit).
//
// BAR1/BAR2 (subdev/bar/gf100.c, gm107.c): a 0x1000-byte instance block
// in VRAM per BAR, a VMM sized to the BAR (BAR2 is BAR index 3 on the PCI
// function: nvkm_device_pci resource NVKM_BAR2_INST), joined; bound with
//   BAR2: 0x001714 = 0x80000000 | inst >> 12  (gf100_bar_bar2_init :72-81)
//   BAR1: 0x001704 = 0x80000000 | inst >> 12  (gf100_bar_bar1_init :51-58)
// and waited on through 0x001710 (gm107_bar_bar2_wait bits 3:2,
// gm107_bar_bar1_wait bits 1:0, gm107.c:26-44). g84_bar_flush
// (bar/g84.c:28-40: 0x070000 = 1, wait bit 1 clear) after CPU writes
// through a BAR. On the Helios nouveau's binds read 0x8017febf (BAR2) and
// 0x8017feaa (BAR1) in the owner's mmiotrace: instance blocks at VRAM
// 0x17febf000 / 0x17feaa000, near the top of 6 GiB (pascal/README.md
// "Hardware reference values").
//
// C11, libc only. SIMULATION status: exercised against the register-
// semantics GPU mock (tests/mockgpu.h, test_vmm.c); not yet run on a GPU.

#ifndef PASCAL_VMM_H
#define PASCAL_VMM_H

#include <pascal/mmu.h>
#include <pascal/vram.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// TLB invalidate registers (vmmgf100.c, vmmgp100.c).
#define PG106_MMU_INVALIDATE_PDB_LO 0x100cb8u
#define PG106_MMU_INVALIDATE_PDB_HI 0x100cecu
#define PG106_MMU_INVALIDATE	    0x100cbcu
#define PG106_MMU_CTRL		    0x100c80u
#define PG106_MMU_INV_PAGE_ALL	    0x00000001u
#define PG106_MMU_INV_ALL_PDB	    0x00000002u
#define PG106_MMU_INV_HUB_ONLY	    0x00000004u

// Instance block (gf100_vmm_join_).
#define PG106_INST_PDB	    0x200u
#define PG106_INST_LIMIT    0x208u
#define PG106_INST_SIZE	    0x1000u
#define PG106_PDB_VER2	    (1ull << 10)
#define PG106_PDB_BIG_64K   (1ull << 11)
#define PG106_PDB_VOL	    (1ull << 2)

// BAR binding registers (bar/gf100.c, gm107.c, g84.c).
#define PG106_BAR1_BLOCK    0x001704u
#define PG106_BAR2_BLOCK    0x001714u
#define PG106_BAR_STATUS    0x001710u
#define PG106_BAR_FLUSH	    0x070000u
#define PG106_BAR_BIND_VALID 0x80000000u

// Default page-table region per VMM (1 MiB = 256 table pages: maps at
// least 255 * 2 MiB of 4K pages, more with 64K pages).
#define PG106_VMM_TABLES_DEFAULT 0x100000ull

struct pg106_vmm {
	struct pg106_vram *vram;
	struct pg106_mmu *mmu;
	uint64_t limit;          // VA end (exclusive), nvkm vmm->limit
	uint64_t table_base, table_size;
	bool hub_only;           // BAR VMM: flush with HUB_ONLY (gp100_vmm_flush)
	uint64_t flushes, syncs;
	struct pg106_mmu_sync_stats last_sync;
};

// Create a VMM with limit `limit` (1 << 49 = whole GP100 space) and
// `table_bytes` of VRAM for its tables (0 = PG106_VMM_TABLES_DEFAULT).
int pg106_vmm_init(struct pg106_vmm *vmm, struct pg106_vram *vram,
		   uint64_t limit, uint64_t table_bytes, bool hub_only);
void pg106_vmm_fini(struct pg106_vmm *vmm);

// Map / unmap (core mmu semantics, VA must lie below limit). No upload:
// call pg106_vmm_commit (sync + flush) afterwards.
int pg106_vmm_map(struct pg106_vmm *vmm, const struct pg106_map *m);
int pg106_vmm_unmap(struct pg106_vmm *vmm, uint64_t va, uint64_t size,
		    unsigned page_shift);
// Upload changed tables to VRAM.
int pg106_vmm_sync(struct pg106_vmm *vmm);
// TLB invalidate for this VMM's PDB (gp100_vmm_flush). -ETIMEDOUT when
// either 0x100c80 wait runs out.
int pg106_vmm_flush(struct pg106_vmm *vmm);
// sync, then flush if anything was written.
int pg106_vmm_commit(struct pg106_vmm *vmm);
// The invalidate for any VRAM-resident page directory at pd_addr.
int pg106_vmm_flush_pdb(const struct pg106_hw_bus *bus, uint64_t pd_addr,
			bool hub_only);
// For a core mmu whose tables were placed (cfg.table_base/limit) in a VRAM
// range the caller owns (kdisp's client VMs): upload the changed words,
// then (flush) invalidate its PDB when anything was written.
int pg106_mmu_upload(struct pg106_vram *vram, struct pg106_mmu *mmu,
		     uint64_t table_base, uint64_t table_size, bool flush);

// PDB word the hardware sees (join and invalidate encodings).
uint64_t pg106_vmm_pdb_join(const struct pg106_vmm *vmm);
uint32_t pg106_vmm_pdb_invalidate(const struct pg106_vmm *vmm);

// Write inst+0x200 / +0x208 in VRAM (gp100_vmm_join).
int pg106_vmm_join(struct pg106_vmm *vmm, uint64_t inst_pa);
// gf100_vmm_part: inst+0x200 = 0 (both words).
int pg106_vmm_part(struct pg106_vmm *vmm, uint64_t inst_pa);

// ---- BARs --------------------------------------------------------------------

enum pg106_bar_id { PG106_BAR1 = 1, PG106_BAR2 = 2 };

struct pg106_bar {
	uint8_t id;
	uint64_t inst;   // VRAM instance block
	uint64_t size;   // aperture bytes = VMM limit
	struct pg106_vmm vmm;
	struct pg106_range va; // BAR offsets handed out by pg106_bar_map
	bool bound;
};

// gf100_bar_oneinit_bar: instance block (tail, not zeroed in nouveau;
// zeroed here), VMM over [0, size), join. Then bind (init) and wait.
int pg106_bar_init(struct pg106_bar *bar, struct pg106_vram *vram,
		   uint8_t id, uint64_t size);
// Re-bind after a power cycle (VRAM contents restored by the caller's
// re-upload: pg106_bar_restore re-joins and rewrites every table).
int pg106_bar_bind(struct pg106_bar *bar);
int pg106_bar_restore(struct pg106_bar *bar);
// gf100_bar_bar{1,2}_fini: clear the valid bit.
void pg106_bar_unbind(struct pg106_bar *bar);
void pg106_bar_fini(struct pg106_bar *bar);
// Map [pa, pa+size) of VRAM (or system memory, target != VRAM) at a free
// BAR offset; 4 KiB pages, kind PITCH. *off = byte offset inside the BAR.
int pg106_bar_map(struct pg106_bar *bar, uint64_t pa, uint64_t size,
		  enum pg106_target target, uint64_t *off);
int pg106_bar_unmap(struct pg106_bar *bar, uint64_t off, uint64_t size);
// g84_bar_flush: 0x070000 = 1, wait for bit 1 to clear (2 s).
int pg106_bar_flush(const struct pg106_hw_bus *bus);

#ifdef __cplusplus
}
#endif

#endif // PASCAL_VMM_H
