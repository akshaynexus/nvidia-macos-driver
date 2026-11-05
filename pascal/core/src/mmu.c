// SPDX-License-Identifier: GPL-2.0-only
/*
 * Pascal (GP10x) MMU v2 page-table builder - portable host-memory model.
 *
 * Derived from nouveau's nvkm GP100 VMM:
 *   - nvkm/subdev/mmu/vmmgp100.c  (PTE/PDE word values, table geometry)
 *   - nvkm/subdev/mmu/gp100.c     (GP100 uses gm200_mmu_kind)
 *   - nvkm/subdev/mmu/gm200.c     (kind table, invalid kind 0xff)
 *   - nvkm/subdev/mmu/vmmgf100.c  (gf100_vmm_aper target -> aperture)
 *   - nvkm/subdev/mmu/vmm.h       (nvkm_vmm_desc {type,bits,size,align})
 * Field bit ranges from Mesa hwref third_party/mesa:src/nouveau/headers/nvidia/hwref/pascal/gp100/dev_mmu.h
 * (NV_MMU_VER2_*). All reused constants cite their source at the point of
 * use below.
 */

#include <pascal/mmu.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Geometry (vmmgp100.c gp100_vmm_desc_12/_16, lines 402-420; each     */
/* descriptor is { type, bits, bytes-per-entry, align }, vmm.h:97-108) */
/* ------------------------------------------------------------------ */

/* Root big PDE: { PGD, 2, 8, 0x1000 } - covers VA bits [48:47]. */
#define PG106_ROOT_BITS		2u
#define PG106_ROOT_ENTRIES	(1u << PG106_ROOT_BITS)

/* Level 2 PDE: { PGD, 9, 8, 0x1000 } - covers VA bits [46:38]. */
#define PG106_PDE_BITS		9u
#define PG106_PDE_ENTRIES	(1u << PG106_PDE_BITS)

/* Level 1 PDE: { PGD, 9, 8, 0x1000 } - covers VA bits [37:29]. */
#define PG106_PDE1_BITS		9u
#define PG106_PDE1_ENTRIES	(1u << PG106_PDE1_BITS)

/* Dual leaf PDE: { PGD, 8, 16, 0x1000 } - covers VA bits [28:21]; each
 * entry is 16 bytes (VMM_WO128 at pdei * 0x10, gp100_vmm_pd0_pde). */
#define PG106_DUAL_BITS		8u
#define PG106_DUAL_ENTRIES	(1u << PG106_DUAL_BITS)

/* 4KiB-page PTE page: { SPT, 9, 8, 0x1000 } - covers VA bits [20:12]. */
#define PG106_SPT_BITS		9u
#define PG106_SPT_ENTRIES	(1u << PG106_SPT_BITS)

/* 64KiB-page PTE table: { LPT, 5, 8, 0x0100 } - covers VA bits [20:16]. */
#define PG106_LPT_BITS		5u
#define PG106_LPT_ENTRIES	(1u << PG106_LPT_BITS)

/* Slot-selection shifts derived from the descriptor bits fields above. */
#define PG106_ROOT_SHIFT	47u	/* 12 + 9 + 9 + 8 + 9 */
#define PG106_PDE_SHIFT		38u
#define PG106_PDE1_SHIFT	29u
#define PG106_DUAL_SHIFT	21u
#define PG106_SPT_SHIFT		12u
#define PG106_LPT_SHIFT		16u

/* All page-table pages are handed out 4KiB-aligned (nvkm_vmm_desc.align
 * is 0x1000 for every level; vmm.h:106). The 256-byte 64K PTE table
 * (align 0x100) would in the real driver be sub-allocated; this host
 * model gives it a full slot to keep address assignment trivial. */
#define PG106_TABLE_ALIGN	0x1000u

/* ------------------------------------------------------------------ */
/* Data structures                                                     */
/* ------------------------------------------------------------------ */

/* One PTE page. The first PG106_LPT_ENTRIES slots are used when the page
 * holds 64KiB-page PTEs (the LPT table is only 32 x 8B). */
struct pg106_pte_page {
	uint64_t addr;			/* GPU address of this page */
	uint64_t pte[PG106_SPT_ENTRIES];	/* raw PTE words */
};

/* Dual leaf PDE table: 256 entries x 16B (two 64-bit words each). */
struct pg106_dual_table {
	uint64_t addr;
	uint64_t word_big[PG106_DUAL_ENTRIES];   /* entry word 0: 64K half */
	uint64_t word_small[PG106_DUAL_ENTRIES]; /* entry word 1: 4K half */
	struct pg106_pte_page *big[PG106_DUAL_ENTRIES];
	struct pg106_pte_page *small[PG106_DUAL_ENTRIES];
	uint32_t big_used[PG106_DUAL_ENTRIES];
	uint32_t small_used[PG106_DUAL_ENTRIES];
};

/* Level 1 PDE table: children are dual tables. */
struct pg106_pd1_low {
	uint64_t addr;
	uint64_t pde[PG106_PDE1_ENTRIES];
	struct pg106_dual_table *child[PG106_PDE1_ENTRIES];
	uint32_t used;		/* non-empty pde[] slots */
};

/* Level 2 PDE table: children are level-1 tables. */
struct pg106_pd1_mid {
	uint64_t addr;
	uint64_t pde[PG106_PDE_ENTRIES];
	struct pg106_pd1_low *child[PG106_PDE_ENTRIES];
	uint32_t used;
};

struct pg106_mmu {
	struct pg106_mmu_cfg cfg;
	uint64_t next_addr;	/* next page-table GPU address to hand out */
	uint64_t root_addr;
	uint64_t root[PG106_ROOT_ENTRIES];
	struct pg106_pd1_mid *l2[PG106_ROOT_ENTRIES];
};

/* ------------------------------------------------------------------ */
/* Field encoding                                                      */
/* ------------------------------------------------------------------ */

/* PTE aperture value for a target: NV_MMU_VER2_PTE_APERTURE 2:1
 * (hwref), mapped from NVKM memory targets by gf100_vmm_aper()
 * (vmmgf100.c:324-333): VRAM->0, HOST(coherent)->2, NCOH->3. */
static uint8_t pte_aperture(enum pg106_target t)
{
	switch (t) {
	case PG106_TARGET_VRAM:
		return 0x0;	/* NV_MMU_VER2_PTE_APERTURE_VIDEO_MEMORY */
	case PG106_TARGET_SYS_COHERENT:
		return 0x2;	/* ..._SYSTEM_COHERENT_MEMORY */
	case PG106_TARGET_SYS_NONCOHERENT:
		return 0x3;	/* ..._SYSTEM_NON_COHERENT_MEMORY */
	}
	return 0xffu;		/* not representable; rejected by validation */
}

/* PDE aperture value for a target: NV_MMU_VER2_PDE_APERTURE 2:1
 * (hwref), values written by gp100_vmm_pde() (vmmgp100.c:239-244):
 * VRAM -> 1<<1, HOST -> 2<<1 (+VOL), NCOH -> 3<<1. */
static uint8_t pde_aperture(enum pg106_target t)
{
	switch (t) {
	case PG106_TARGET_VRAM:
		return 0x1;	/* NV_MMU_VER2_PDE_APERTURE_VIDEO_MEMORY */
	case PG106_TARGET_SYS_COHERENT:
		return 0x2;	/* ..._SYSTEM_COHERENT_MEMORY */
	case PG106_TARGET_SYS_NONCOHERENT:
		return 0x3;	/* ..._SYSTEM_NON_COHERENT_MEMORY */
	}
	return 0x0;		/* NV_MMU_VER2_PDE_APERTURE_INVALID */
}

/*
 * Encode one 64-bit PDE half (root/mid/low PDEs, and both halves of a
 * dual PDE - the VER2 and VER2_DUAL descriptors share the per-word
 * layout: bit 0 IS_PTE/VALID, 2:1 APERTURE, 3 VOL, 53:8 ADDRESS,
 * hwref_gp100_dev_mmu.h). Mirrors gp100_vmm_pde() (vmmgp100.c:237-251):
 * aperture in bits 2:1, VOL only for coherent system memory, address at
 * bit 8 as addr >> 4 (4KiB-aligned address fields are 53:8).
 *
 * Bit 0 stays 0 exactly like nouveau: gp100_vmm_pde() never sets it; a
 * PDE is linked iff its aperture differs from
 * NV_MMU_VER2_PDE_APERTURE_INVALID (0).
 */
static uint64_t pde_encode(enum pg106_target target, uint64_t table_addr)
{
	uint64_t data = (uint64_t)pde_aperture(target) << 1;

	if (target == PG106_TARGET_SYS_COHERENT)
		data |= 1ULL << 3;	/* NV_MMU_VER2_PDE_VOL_TRUE */
	data |= table_addr >> 4;	/* ADDRESS 53:8 */
	return data;
}

/* True if a PDE half links to a table (aperture != INVALID). */
static bool pde_linked(uint64_t word)
{
	return ((word >> 1) & 3u) != 0x0u;
}

/*
 * Encode one 64-bit PTE. Mirrors gp100_vmm_valid()'s map->type packing
 * (vmmgp100.c:490-495) plus the address from gp100_vmm_pgt_pte()
 * (vmmgp100.c:135, data = (addr >> 4) | map->type):
 *   bit 0  VALID            (NV_MMU_VER2_PTE_VALID_TRUE)
 *   bits 2:1 APERTURE       (NV_MMU_VER2_PTE_APERTURE)
 *   bit 3  VOL              (NV_MMU_VER2_PTE_VOL_TRUE)
 *   bit 5  PRIVILEGE        (NV_MMU_VER2_PTE_PRIVILEGE_TRUE)
 *   bit 6  READ_ONLY        (NV_MMU_VER2_PTE_READ_ONLY_TRUE)
 *   bits 63:56 KIND         (NV_MMU_VER2_PTE_KIND)
 *   bits 53:8 ADDRESS       (NV_MMU_VER2_PTE_ADDRESS_SYS, pa >> 4)
 * Compressed kinds and COMPTAGLINE (36:53) are not produced; see
 * gp100_vmm_valid() vmmgp100.c:468-488.
 */
static uint64_t pte_encode(const struct pg106_map *map, uint64_t pa)
{
	uint64_t data = pa >> 4;

	data |= 1ULL << 0;				/* VALID */
	data |= (uint64_t)pte_aperture(map->target) << 1;
	if (map->vol)
		data |= 1ULL << 3;
	if (map->priv)
		data |= 1ULL << 5;
	if (map->ro)
		data |= 1ULL << 6;
	data |= (uint64_t)map->kind << 56;
	return data;
}

/* ------------------------------------------------------------------ */
/* Allocation helpers                                                  */
/* ------------------------------------------------------------------ */

/* Hand out the next page-table GPU address; -ERANGE once the address
 * would exceed the aperture's address field (VID fields are 32:8 ->
 * 2^37, SYS fields 53:8 -> 2^52; hwref ADDRESS_VID/ADDRESS_SYS). */
static int take_addr(struct pg106_mmu *m, uint64_t *out)
{
	uint64_t limit = (m->cfg.table_target == PG106_TARGET_VRAM)
				? PG106_VRAM_ADDR_LIMIT : PG106_SYS_ADDR_LIMIT;

	if (m->next_addr + PG106_TABLE_ALIGN > limit)
		return -ERANGE;
	*out = m->next_addr;
	m->next_addr += PG106_TABLE_ALIGN;
	return 0;
}

static void *alloc_zero(size_t size)
{
	return calloc(1, size);
}

/* ------------------------------------------------------------------ */
/* Tree construction                                                   */
/* ------------------------------------------------------------------ */

static struct pg106_pd1_mid *ensure_l2(struct pg106_mmu *m, unsigned ri)
{
	struct pg106_pd1_mid *t = m->l2[ri];

	if (!t) {
		uint64_t addr;
		int ret;

		t = alloc_zero(sizeof(*t));
		if (!t)
			return NULL;
		ret = take_addr(m, &addr);
		if (ret) {
			free(t);
			return NULL;
		}
		t->addr = addr;
		m->l2[ri] = t;
	}
	/* Idempotent rewrite: also relinks after an unmap zeroed the word
	 * while the (cached) table survived. */
	m->root[ri] = pde_encode(m->cfg.table_target, t->addr);
	return t;
}

static struct pg106_pd1_low *ensure_l1(struct pg106_mmu *m,
				       struct pg106_pd1_mid *l2, unsigned i2)
{
	struct pg106_pd1_low *t = l2->child[i2];

	if (!t) {
		uint64_t addr;
		int ret;

		t = alloc_zero(sizeof(*t));
		if (!t)
			return NULL;
		ret = take_addr(m, &addr);
		if (ret) {
			free(t);
			return NULL;
		}
		t->addr = addr;
		l2->child[i2] = t;
		l2->used++;
	}
	l2->pde[i2] = pde_encode(m->cfg.table_target, t->addr);
	return t;
}

static struct pg106_dual_table *ensure_dual(struct pg106_mmu *m,
					    struct pg106_pd1_low *l1,
					    unsigned i1)
{
	struct pg106_dual_table *t = l1->child[i1];

	if (!t) {
		uint64_t addr;
		int ret;

		t = alloc_zero(sizeof(*t));
		if (!t)
			return NULL;
		ret = take_addr(m, &addr);
		if (ret) {
			free(t);
			return NULL;
		}
		t->addr = addr;
		l1->child[i1] = t;
		l1->used++;
	}
	l1->pde[i1] = pde_encode(m->cfg.table_target, t->addr);
	return t;
}

static struct pg106_pte_page *ensure_small_pte(struct pg106_mmu *m,
					       struct pg106_dual_table *dual,
					       unsigned di)
{
	struct pg106_pte_page *p = dual->small[di];

	if (!p) {
		uint64_t addr;
		int ret;

		p = alloc_zero(sizeof(*p));
		if (!p)
			return NULL;
		ret = take_addr(m, &addr);
		if (ret) {
			free(p);
			return NULL;
		}
		p->addr = addr;
		dual->small[di] = p;
	}
	/* Word 1 of the dual PDE: small (4K) table pointer. */
	dual->word_small[di] = pde_encode(m->cfg.table_target, p->addr);
	return p;
}

static struct pg106_pte_page *ensure_big_pte(struct pg106_mmu *m,
					     struct pg106_dual_table *dual,
					     unsigned di)
{
	struct pg106_pte_page *p = dual->big[di];

	if (!p) {
		uint64_t addr;
		int ret;

		p = alloc_zero(sizeof(*p));
		if (!p)
			return NULL;
		ret = take_addr(m, &addr);
		if (ret) {
			free(p);
			return NULL;
		}
		p->addr = addr;
		dual->big[di] = p;
	}
	/* Word 0 of the dual PDE: big (64K) table pointer. */
	dual->word_big[di] = pde_encode(m->cfg.table_target, p->addr);
	return p;
}

/* Find the dual table covering va without allocating. */
static struct pg106_dual_table *find_dual(const struct pg106_mmu *m,
					  uint64_t va)
{
	unsigned ri = (va >> PG106_ROOT_SHIFT) & (PG106_ROOT_ENTRIES - 1u);
	unsigned i2 = (va >> PG106_PDE_SHIFT) & (PG106_PDE_ENTRIES - 1u);
	unsigned i1 = (va >> PG106_PDE1_SHIFT) & (PG106_PDE1_ENTRIES - 1u);
	struct pg106_pd1_mid *l2 = m->l2[ri];
	struct pg106_pd1_low *l1;

	if (!l2)
		return NULL;
	l1 = l2->child[i2];
	if (!l1)
		return NULL;
	return l1->child[i1];
}

/* Walk down to (and allocate) the dual table covering va. */
static struct pg106_dual_table *walk_to_dual(struct pg106_mmu *m, uint64_t va)
{
	unsigned ri = (va >> PG106_ROOT_SHIFT) & (PG106_ROOT_ENTRIES - 1u);
	unsigned i2 = (va >> PG106_PDE_SHIFT) & (PG106_PDE_ENTRIES - 1u);
	unsigned i1 = (va >> PG106_PDE1_SHIFT) & (PG106_PDE1_ENTRIES - 1u);
	struct pg106_pd1_mid *l2 = ensure_l2(m, ri);
	struct pg106_pd1_low *l1;

	if (!l2)
		return NULL;
	l1 = ensure_l1(m, l2, i2);
	if (!l1)
		return NULL;
	return ensure_dual(m, l1, i1);
}

/* ------------------------------------------------------------------ */
/* Validation                                                          */
/* ------------------------------------------------------------------ */

static int check_map_args(const struct pg106_mmu *m, const struct pg106_map *map)
{
	uint64_t page, pa_limit;
	unsigned shift = map->page_shift;

	if (!m || !map || map->size == 0)
		return -EINVAL;
	if (shift != PG106_PAGE_4K_SHIFT && shift != PG106_PAGE_64K_SHIFT)
		return -EINVAL;
	if (map->target != PG106_TARGET_VRAM &&
	    map->target != PG106_TARGET_SYS_COHERENT &&
	    map->target != PG106_TARGET_SYS_NONCOHERENT)
		return -EINVAL;
	if (!pg106_mmu_kind_valid(map->kind))
		return -EINVAL;	/* gp100_vmm_valid(): kindm[kind] == kind_inv
				 * rejects the request, vmmgp100.c:463-466 */

	page = 1ULL << shift;
	if ((map->va | map->pa | map->size) & (page - 1))
		return -EINVAL;
	if (map->va > PG106_MMU_VA_LIMIT - map->size)
		return -EINVAL;

	/* Backing address must fit the aperture's PTE address field. */
	pa_limit = (map->target == PG106_TARGET_VRAM)
				? PG106_VRAM_ADDR_LIMIT : PG106_SYS_ADDR_LIMIT;
	if (map->pa > pa_limit - map->size)
		return -ERANGE;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Public API: lifecycle                                               */
/* ------------------------------------------------------------------ */

int pg106_mmu_init(struct pg106_mmu **out, const struct pg106_mmu_cfg *cfg)
{
	struct pg106_mmu *m;
	uint64_t limit;

	if (!out || !cfg)
		return -EINVAL;
	if (cfg->table_target != PG106_TARGET_VRAM &&
	    cfg->table_target != PG106_TARGET_SYS_COHERENT &&
	    cfg->table_target != PG106_TARGET_SYS_NONCOHERENT)
		return -EINVAL;
	if (cfg->table_base & (PG106_TABLE_ALIGN - 1u))
		return -EINVAL;

	limit = (cfg->table_target == PG106_TARGET_VRAM)
				? PG106_VRAM_ADDR_LIMIT : PG106_SYS_ADDR_LIMIT;
	if (cfg->table_base > limit - PG106_TABLE_ALIGN)
		return -ERANGE;

	m = alloc_zero(sizeof(*m));
	if (!m)
		return -ENOMEM;
	m->cfg = *cfg;
	m->root_addr = cfg->table_base;
	m->next_addr = cfg->table_base + PG106_TABLE_ALIGN;
	*out = m;
	return 0;
}

void pg106_mmu_destroy(struct pg106_mmu *m)
{
	unsigned ri, i2, i1;

	if (!m)
		return;
	for (ri = 0; ri < PG106_ROOT_ENTRIES; ri++) {
		struct pg106_pd1_mid *l2 = m->l2[ri];

		if (!l2)
			continue;
		for (i2 = 0; i2 < PG106_PDE_ENTRIES; i2++) {
			struct pg106_pd1_low *l1 = l2->child[i2];
			unsigned di;

			if (!l1)
				continue;
			for (i1 = 0; i1 < PG106_PDE1_ENTRIES; i1++) {
				struct pg106_dual_table *dual = l1->child[i1];

				if (!dual)
					continue;
				for (di = 0; di < PG106_DUAL_ENTRIES; di++) {
					free(dual->big[di]);
					free(dual->small[di]);
				}
				free(dual);
			}
			free(l1);
		}
		free(l2);
	}
	free(m);
}

uint64_t pg106_mmu_root_addr(const struct pg106_mmu *m)
{
	return m ? m->root_addr : 0;
}

bool pg106_mmu_kind_valid(uint8_t kind)
{
	/* gm200_mmu_kind() (used by GP100, mmu/gp100.c:35) returns a
	 * 256-entry table with *invalid = 0xff (gm200.c:68); kind 0x00
	 * maps to itself (uncompressed PITCH). Entries remapped by the
	 * table concern compression only, which this builder does not
	 * produce (vmmgp100.c:468-488). */
	return kind != PG106_KIND_INVALID;
}

/* ------------------------------------------------------------------ */
/* Public API: mapping                                                 */
/* ------------------------------------------------------------------ */

int pg106_mmu_map(struct pg106_mmu *m, const struct pg106_map *map)
{
	uint64_t page, off;
	int ret;

	ret = check_map_args(m, map);
	if (ret)
		return ret;

	page = 1ULL << map->page_shift;
	for (off = 0; off < map->size; off += page) {
		uint64_t va = map->va + off;
		uint64_t pa = map->pa + off;
		unsigned di = (va >> PG106_DUAL_SHIFT) & (PG106_DUAL_ENTRIES - 1u);
		struct pg106_dual_table *dual = walk_to_dual(m, va);

		if (!dual)
			return -ENOMEM;

		if (map->page_shift == PG106_PAGE_4K_SHIFT) {
			unsigned pi = (va >> PG106_SPT_SHIFT) &
				      (PG106_SPT_ENTRIES - 1u);
			struct pg106_pte_page *pt = ensure_small_pte(m, dual, di);

			if (!pt)
				return -ENOMEM;
			if (pt->pte[pi] & 1ULL)	/* VALID already set */
				return -EEXIST;
			pt->pte[pi] = pte_encode(map, pa);
			dual->small_used[di]++;
		} else {
			unsigned pi = (va >> PG106_LPT_SHIFT) &
				      (PG106_LPT_ENTRIES - 1u);
			struct pg106_pte_page *pt = ensure_big_pte(m, dual, di);

			if (!pt)
				return -ENOMEM;
			if (pt->pte[pi] & 1ULL)
				return -EEXIST;
			pt->pte[pi] = pte_encode(map, pa);
			dual->big_used[di]++;
		}
	}
	return 0;
}

int pg106_mmu_unmap(struct pg106_mmu *m, uint64_t va, uint64_t size,
		    unsigned page_shift)
{
	uint64_t page, off;

	if (!m || size == 0)
		return -EINVAL;
	if (page_shift != PG106_PAGE_4K_SHIFT && page_shift != PG106_PAGE_64K_SHIFT)
		return -EINVAL;
	if (va & ((1ULL << page_shift) - 1))
		return -EINVAL;
	if (size & ((1ULL << page_shift) - 1))
		return -EINVAL;
	if (va > PG106_MMU_VA_LIMIT - size)
		return -EINVAL;

	page = 1ULL << page_shift;
	for (off = 0; off < size; off += page) {
		uint64_t cur = va + off;
		unsigned ri = (cur >> PG106_ROOT_SHIFT) & (PG106_ROOT_ENTRIES - 1u);
		unsigned i2 = (cur >> PG106_PDE_SHIFT) & (PG106_PDE_ENTRIES - 1u);
		unsigned i1 = (cur >> PG106_PDE1_SHIFT) & (PG106_PDE1_ENTRIES - 1u);
		unsigned di = (cur >> PG106_DUAL_SHIFT) & (PG106_DUAL_ENTRIES - 1u);
		struct pg106_pd1_mid *l2 = m->l2[ri];
		struct pg106_pd1_low *l1;
		struct pg106_dual_table *dual;
		struct pg106_pte_page *pt;
		unsigned pi;
		uint64_t *word;

		if (!l2)
			continue;
		l1 = l2->child[i2];
		if (!l1)
			continue;
		dual = l1->child[i1];
		if (!dual)
			continue;

		if (page_shift == PG106_PAGE_4K_SHIFT) {
			pi = (cur >> PG106_SPT_SHIFT) & (PG106_SPT_ENTRIES - 1u);
			pt = dual->small[di];
			word = &dual->word_small[di];
			if (!pt || !(pt->pte[pi] & 1ULL))
				continue;
			pt->pte[pi] = 0;	/* VALID_FALSE unmaps */
			if (--dual->small_used[di] == 0)
				*word = 0;
		} else {
			pi = (cur >> PG106_LPT_SHIFT) & (PG106_LPT_ENTRIES - 1u);
			pt = dual->big[di];
			word = &dual->word_big[di];
			if (!pt || !(pt->pte[pi] & 1ULL))
				continue;
			pt->pte[pi] = 0;
			if (--dual->big_used[di] == 0)
				*word = 0;
		}

		/* Unlink the dual PDE once neither half is live
		 * (cf. gp100_vmm_pd0_unmap(): both words -> 0). */
		if (!dual->small_used[di] && !dual->big_used[di]) {
			l1->pde[i1] = 0;
			if (--l1->used == 0) {
				l2->pde[i2] = 0;
				if (--l2->used == 0)
					m->root[ri] = 0;
			}
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Public API: queries                                                 */
/* ------------------------------------------------------------------ */

bool pg106_mmu_walk(const struct pg106_mmu *m, uint64_t va,
		    struct pg106_pte_query *q)
{
	unsigned ri, i2, i1, di;
	struct pg106_pd1_mid *l2;
	struct pg106_pd1_low *l1;
	struct pg106_dual_table *dual;

	if (!m || !q)
		return false;
	if (va >= PG106_MMU_VA_LIMIT)
		return false;
	memset(q, 0, sizeof(*q));

	ri = (va >> PG106_ROOT_SHIFT) & (PG106_ROOT_ENTRIES - 1u);
	i2 = (va >> PG106_PDE_SHIFT) & (PG106_PDE_ENTRIES - 1u);
	i1 = (va >> PG106_PDE1_SHIFT) & (PG106_PDE1_ENTRIES - 1u);
	di = (va >> PG106_DUAL_SHIFT) & (PG106_DUAL_ENTRIES - 1u);

	if (!pde_linked(m->root[ri]))
		return false;
	l2 = m->l2[ri];
	if (!l2 || !pde_linked(l2->pde[i2]))
		return false;
	l1 = l2->child[i2];
	if (!l1 || !pde_linked(l1->pde[i1]))
		return false;
	dual = l1->child[i1];
	if (!dual)
		return false;

	/* 4KiB PTE first; the 64KiB half is the fallback. */
	if (pde_linked(dual->word_small[di])) {
		struct pg106_pte_page *pt = dual->small[di];
		unsigned pi = (va >> PG106_SPT_SHIFT) & (PG106_SPT_ENTRIES - 1u);

		if (pt && (pt->pte[pi] & 1ULL)) {
			q->mapped = true;
			q->big = false;
			q->page_shift = PG106_PAGE_4K_SHIFT;
			q->pte_word = pt->pte[pi];
			q->pa = (q->pte_word >> 8) << PG106_SPT_SHIFT;
			q->aperture = (uint8_t)((q->pte_word >> 1) & 3u);
			q->kind = (uint8_t)(q->pte_word >> 56);
			return true;
		}
	}
	if (pde_linked(dual->word_big[di])) {
		struct pg106_pte_page *pt = dual->big[di];
		unsigned pi = (va >> PG106_LPT_SHIFT) & (PG106_LPT_ENTRIES - 1u);

		if (pt && (pt->pte[pi] & 1ULL)) {
			q->mapped = true;
			q->big = true;
			q->page_shift = PG106_PAGE_64K_SHIFT;
			q->pte_word = pt->pte[pi];
			q->pa = (q->pte_word >> 8) << PG106_SPT_SHIFT;
			q->aperture = (uint8_t)((q->pte_word >> 1) & 3u);
			q->kind = (uint8_t)(q->pte_word >> 56);
			return true;
		}
	}
	return false;
}

bool pg106_mmu_pde_word(const struct pg106_mmu *m, uint64_t va,
			enum pg106_pde_level level, uint64_t *word)
{
	unsigned ri, i2, i1;
	struct pg106_pd1_mid *l2;
	struct pg106_pd1_low *l1;

	if (!m || !word || va >= PG106_MMU_VA_LIMIT)
		return false;

	ri = (va >> PG106_ROOT_SHIFT) & (PG106_ROOT_ENTRIES - 1u);
	i2 = (va >> PG106_PDE_SHIFT) & (PG106_PDE_ENTRIES - 1u);
	i1 = (va >> PG106_PDE1_SHIFT) & (PG106_PDE1_ENTRIES - 1u);

	switch (level) {
	case PG106_PDE_LEVEL3:
		*word = m->root[ri];
		return true;
	case PG106_PDE_LEVEL2:
		l2 = m->l2[ri];
		*word = l2 ? l2->pde[i2] : 0;
		return true;
	case PG106_PDE_LEVEL1:
		l2 = m->l2[ri];
		l1 = l2 ? l2->child[i2] : NULL;
		*word = l1 ? l1->pde[i1] : 0;
		return true;
	case PG106_PDE_LEVEL0_DUAL:
		return pg106_mmu_dual_pde_words(m, va, word, NULL);
	}
	return false;
}

bool pg106_mmu_dual_pde_words(const struct pg106_mmu *m, uint64_t va,
			      uint64_t *big_word, uint64_t *small_word)
{
	unsigned di;
	struct pg106_dual_table *dual;

	if (!m || (!big_word && !small_word) || va >= PG106_MMU_VA_LIMIT)
		return false;

	di = (va >> PG106_DUAL_SHIFT) & (PG106_DUAL_ENTRIES - 1u);
	dual = find_dual(m, va);
	if (big_word)
		*big_word = dual ? dual->word_big[di] : 0;
	if (small_word)
		*small_word = dual ? dual->word_small[di] : 0;
	return true;
}

bool pg106_mmu_table_addr(const struct pg106_mmu *m, uint64_t va,
			  enum pg106_table table, uint64_t *addr)
{
	unsigned ri, i2, i1, di;
	struct pg106_pd1_mid *l2;
	struct pg106_pd1_low *l1;
	struct pg106_dual_table *dual;

	if (!m || !addr || va >= PG106_MMU_VA_LIMIT)
		return false;

	ri = (va >> PG106_ROOT_SHIFT) & (PG106_ROOT_ENTRIES - 1u);
	i2 = (va >> PG106_PDE_SHIFT) & (PG106_PDE_ENTRIES - 1u);
	i1 = (va >> PG106_PDE1_SHIFT) & (PG106_PDE1_ENTRIES - 1u);
	di = (va >> PG106_DUAL_SHIFT) & (PG106_DUAL_ENTRIES - 1u);

	switch (table) {
	case PG106_TABLE_PDE3_ROOT:
		*addr = m->root_addr;
		return true;
	case PG106_TABLE_PDE2:
		l2 = m->l2[ri];
		if (!l2)
			return false;
		*addr = l2->addr;
		return true;
	case PG106_TABLE_PDE1:
		l2 = m->l2[ri];
		l1 = l2 ? l2->child[i2] : NULL;
		if (!l1)
			return false;
		*addr = l1->addr;
		return true;
	case PG106_TABLE_DUAL_PDE:
		l2 = m->l2[ri];
		l1 = l2 ? l2->child[i2] : NULL;
		dual = l1 ? l1->child[i1] : NULL;
		if (!dual)
			return false;
		*addr = dual->addr;
		return true;
	case PG106_TABLE_PTE_4K:
		l2 = m->l2[ri];
		l1 = l2 ? l2->child[i2] : NULL;
		dual = l1 ? l1->child[i1] : NULL;
		if (!dual || !dual->small[di])
			return false;
		*addr = dual->small[di]->addr;
		return true;
	case PG106_TABLE_PTE_64K:
		l2 = m->l2[ri];
		l1 = l2 ? l2->child[i2] : NULL;
		dual = l1 ? l1->child[i1] : NULL;
		if (!dual || !dual->big[di])
			return false;
		*addr = dual->big[di]->addr;
		return true;
	}
	return false;
}
