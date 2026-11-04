// SPDX-License-Identifier: GPL-2.0-only
/*
 * pg106_mmu - NVIDIA Pascal (GP10x) MMU v2 page-table builder.
 *
 * Builds a Pascal MMU v2 page-table hierarchy in host memory: the root "big"
 * PDE table (level 3) -> mid PDE tables -> 128-bit dual leaf PDEs -> PTE
 * pages, for both 4KiB pages and 64KiB big pages, mirroring what nouveau's
 * nvkm writes into VRAM/sysmem for a GP100-class VMM.
 *
 * Field-layout authority (bit ranges, aperture/kind encodings):
 *   research/reference/hwref_gp100_dev_mmu.h (Mesa hwref dump, NV_MMU_VER2_*).
 * Builder-logic authority (table shapes, shifts, word values):
 *   nouveau nvkm/subdev/mmu/vmmgp100.c, mmu/gp100.c, mmu/gm200.c (kind map),
 *   mmu/vmmgf100.c (gf100_vmm_aper), mmu/vmm.h (descriptor shapes),
 *   from torvalds/linux master; local copy kept at /tmp/pascal-ref/.
 * Format document (secondary, agrees with both above):
 *   open-gpu-doc pascal/gp100-mmu-format.pdf.
 *
 * Single-threaded, libc-only, C11. Error returns are errno-style (0 or
 * negative errno). No Linux kernel dependencies.
 */
#ifndef PASCAL_MMU_H
#define PASCAL_MMU_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Encodings                                                           */
/* ------------------------------------------------------------------ */

/*
 * Backing-store target. This is the caller-facing enumeration; the raw
 * field values differ between PTEs and PDEs and are applied internally:
 *   PTE: NV_MMU_VER2_PTE_APERTURE 2:1 - VIDEO_MEMORY=0, PEER=1,
 *        SYSTEM_COHERENT_MEMORY=2, SYSTEM_NON_COHERENT_MEMORY=3
 *        (hwref_gp100_dev_mmu.h); nouveau maps NVKM_MEM_TARGET_{VRAM,HOST,
 *        NCOH} -> {0,2,3} in gf100_vmm_aper() (vmmgf100.c:324-333).
 *   PDE: NV_MMU_VER2_PDE_APERTURE 2:1 - INVALID=0, VIDEO_MEMORY=1,
 *        SYSTEM_COHERENT_MEMORY=2, SYSTEM_NON_COHERENT_MEMORY=3
 *        (hwref); nouveau writes {1,2,3}<<1 in gp100_vmm_pde()
 *        (vmmgp100.c:239-244).
 */
enum pg106_target {
	PG106_TARGET_VRAM = 0,           /* video memory */
	PG106_TARGET_SYS_COHERENT,       /* cache-coherent system memory */
	PG106_TARGET_SYS_NONCOHERENT,    /* non-coherent system memory */
};

/*
 * Page kinds. 0xff is the invalid kind (NV_MMU_PTE_KIND_INVALID,
 * hwref; nouveau gm200_mmu_kind() sets *invalid = 0xff, gm200.c:68).
 * 0x00 is the plain linear-uncompressed pitch kind
 * (NV_MMU_PTE_KIND_PITCH, hwref); gm200_mmu_kind()'s table maps kind 0
 * to itself (gm200.c:34), i.e. uncompressed, no compression kind swap.
 */
#define PG106_KIND_INVALID	0xffu
#define PG106_KIND_PITCH	0x00u

/* Page sizes handled by this builder (gp100_vmm_desc_12 / _16,
 * vmmgp100.c:402-420; .page table vmmgp100.c:602-610). */
#define PG106_PAGE_4K_SHIFT	12u
#define PG106_PAGE_64K_SHIFT	16u

/*
 * Virtual-address space covered by the hierarchy: 2 + 9 + 9 + 8 + 9 = 37
 * page-index bits above the 4KiB page offset (descriptor "bits" fields,
 * vmmgp100.c:402-420), i.e. VA < 2^49 = 0x2_0000_0000_0000.
 */
#define PG106_MMU_VA_BITS	49u
#define PG106_MMU_VA_LIMIT	(1ULL << PG106_MMU_VA_BITS)

/*
 * Address-field capacity of a PTE/PDE physical address:
 *   VIDEO_MEMORY fields are NV_MMU_VER2_P{DE,TE}_ADDRESS_VID (35-3):8
 *   (hwref) -> mapped address < 2^37;
 *   SYS fields are 53:8 -> address < 2^52.
 * Used to validate pa and page-table addresses against the aperture.
 */
#define PG106_VRAM_ADDR_LIMIT	(1ULL << 37)
#define PG106_SYS_ADDR_LIMIT	(1ULL << 52)

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

struct pg106_mmu;	/* opaque host-memory VMM model */

/* Where the page-table pages themselves live. */
struct pg106_mmu_cfg {
	enum pg106_target table_target;
	/*
	 * GPU address assigned to the root page directory. Subsequent
	 * page-table pages are handed out sequentially, 4KiB-aligned
	 * (nvkm_vmm_desc align field is 0x1000 for every GP100 level;
	 * vmm.h:106, vmmgp100.c:402-420).
	 */
	uint64_t table_base;
};

/* One mapping request. */
struct pg106_map {
	uint64_t va;		/* virtual address, page aligned */
	uint64_t pa;		/* backing address, page aligned */
	uint64_t size;		/* multiple of the page size */
	unsigned page_shift;	/* PG106_PAGE_4K_SHIFT or _64K_SHIFT */
	enum pg106_target target;
	uint8_t kind;		/* PTE kind, bits 63:56 (hwref) */
	bool vol;		/* NV_MMU_VER2_PTE_VOL 3:3 */
	bool ro;		/* NV_MMU_VER2_PTE_READ_ONLY 6:6 */
	bool priv;		/* NV_MMU_VER2_PTE_PRIVILEGE 5:5 */
};

/* Result of pg106_mmu_walk(): the PTE that maps a given VA. */
struct pg106_pte_query {
	bool mapped;
	bool big;			/* mapped with 64KiB pages */
	unsigned page_shift;		/* 12 or 16 */
	uint64_t pte_word;		/* raw 64-bit PTE */
	uint64_t pa;			/* decoded backing address */
	uint8_t aperture;		/* NV_MMU_VER2_PTE_APERTURE 2:1 */
	uint8_t kind;			/* NV_MMU_VER2_PTE_KIND 63:56 */
};

/* PDE levels, named big-endian-first as in gp100-mmu-format.pdf. */
enum pg106_pde_level {
	PG106_PDE_LEVEL3 = 3,	/* root "big" PDE, 4 entries, VA[48:47] */
	PG106_PDE_LEVEL2 = 2,	/* 512 entries, VA[46:38] */
	PG106_PDE_LEVEL1 = 1,	/* 512 entries, VA[37:29] */
	PG106_PDE_LEVEL0_DUAL = 0,	/* 256 x 16B dual PDEs, VA[28:21] */
};

/* Identifies one table in the hierarchy, for address/word inspection. */
enum pg106_table {
	PG106_TABLE_PDE3_ROOT,
	PG106_TABLE_PDE2,
	PG106_TABLE_PDE1,
	PG106_TABLE_DUAL_PDE,
	PG106_TABLE_PTE_4K,	/* 4KiB-page PTE page (512 x 8B) */
	PG106_TABLE_PTE_64K,	/* 64KiB-page PTE table (32 x 8B, 256B) */
};

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* Returns 0 on success, -EINVAL/-ENOMEM on failure. */
int pg106_mmu_init(struct pg106_mmu **out, const struct pg106_mmu_cfg *cfg);
void pg106_mmu_destroy(struct pg106_mmu *mmu);

/* GPU address of the root page directory (what the hardware PDB register
 * 0x100cb8/0x100cec would receive; gp100_vmm_invalidate_pdb,
 * vmmgp100.c:564-570). */
uint64_t pg106_mmu_root_addr(const struct pg106_mmu *mmu);

/* ------------------------------------------------------------------ */
/* Mapping                                                             */
/* ------------------------------------------------------------------ */

/*
 * Map [va, va+size) to [pa, pa+size) with the given page size.
 * Fails with -EEXIST if a page is already mapped (either granularity),
 * -EINVAL on bad alignment/kind/ranges, -ENOMEM on allocation failure,
 * -ERANGE if an address exceeds the aperture's field capacity.
 */
int pg106_mmu_map(struct pg106_mmu *mmu, const struct pg106_map *map);

/*
 * Undo a pg106_mmu_map() of the same page size. Idempotent: pages that
 * were not mapped are skipped. Parent PDEs whose subtrees become empty
 * are unlinked (aperture set back to INVALID), matching
 * gp100_vmm_pd0_unmap() / gf100_vmm_pgt_unmap() semantics
 * (vmmgp100.c:278-283, 188-196).
 */
int pg106_mmu_unmap(struct pg106_mmu *mmu, uint64_t va, uint64_t size,
		    unsigned page_shift);

/* True if kind is usable in a PTE (not the invalid kind). The full
 * 256-entry compression-remap table lives in nouveau gm200_mmu_kind()
 * (gm200.c:33-66) and only matters for compressed kinds, which this
 * builder does not produce (see gp100_vmm_valid()'s non-GSP path
 * reverting compressed kinds, vmmgp100.c:484-487). */
bool pg106_mmu_kind_valid(uint8_t kind);

/* ------------------------------------------------------------------ */
/* Queries                                                             */
/* ------------------------------------------------------------------ */

/*
 * Walk the hierarchy for va and return the PTE that maps it. If both a
 * 4KiB and a 64KiB PTE exist for va (a driver bug in practice), the 4KiB
 * PTE wins. Returns false if any level is unlinked or the PTE is not
 * VALID (NV_MMU_VER2_PTE_VALID 0:0).
 */
bool pg106_mmu_walk(const struct pg106_mmu *mmu, uint64_t va,
		    struct pg106_pte_query *query);

/* Raw PDE word for va's slot at the given level. For the dual level this
 * is the big-page half (word 0); use pg106_mmu_dual_pde_words() for both
 * halves. Slots whose containing table was never allocated return 0
 * (aperture INVALID). Bit 0 (IS_PTE/VALID) is always 0, exactly like
 * nouveau: gp100_vmm_pde() sets only aperture [+VOL] + address
 * (vmmgp100.c:237-251); PDE validity is signalled by APERTURE != INVALID
 * (NV_MMU_VER2_PDE_APERTURE_INVALID = 0, hwref). */
bool pg106_mmu_pde_word(const struct pg106_mmu *mmu, uint64_t va,
			enum pg106_pde_level level, uint64_t *word);

/* Both halves of the 16-byte dual leaf PDE for va (written as one
 * VMM_WO128 at pdei * 0x10 in gp100_vmm_pd0_pde, vmmgp100.c:253-268):
 * word 0 = 64KiB (big) table pointer, word 1 = 4KiB (small) table
 * pointer. Unallocated halves return 0. */
bool pg106_mmu_dual_pde_words(const struct pg106_mmu *mmu, uint64_t va,
			      uint64_t *big_word, uint64_t *small_word);

/* GPU address assigned to the table covering va at the given position.
 * Returns false if that table does not exist (not allocated yet). */
bool pg106_mmu_table_addr(const struct pg106_mmu *mmu, uint64_t va,
			  enum pg106_table table, uint64_t *addr);

#ifdef __cplusplus
}
#endif

#endif /* PASCAL_MMU_H */
