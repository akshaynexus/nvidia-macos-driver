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

/*
 * Dirty range of one table: entries [lo, hi] changed since the last
 * pg106_mmu_sync(). A table that was just allocated is dirty as a whole,
 * so its memory is written once in full (zeroes included) before any PDE
 * links it: VRAM or host pages handed out for page tables are not known
 * to be zero (nouveau zeroes new page-table memory too: nvkm_mmu_ptc_get
 * zero = true for every level, vmm.c nvkm_vmm_ref_hwpt :418-424).
 */
struct pg106_dirty {
	uint32_t lo, hi;
	bool any;
};

static void dirty_mark(struct pg106_dirty *d, uint32_t i)
{
	if (!d->any) {
		d->lo = d->hi = i;
		d->any = true;
		return;
	}
	if (i < d->lo)
		d->lo = i;
	if (i > d->hi)
		d->hi = i;
}

static void dirty_all(struct pg106_dirty *d, uint32_t n)
{
	d->any = true;
	d->lo = 0;
	d->hi = n - 1;
}

/* One PTE page. The first PG106_LPT_ENTRIES slots are used when the page
 * holds 64KiB-page PTEs (the LPT table is only 32 x 8B). */
struct pg106_pte_page {
	uint64_t addr;			/* GPU address of this page */
	uint64_t pte[PG106_SPT_ENTRIES];	/* raw PTE words */
	struct pg106_dirty d;
};

/*
 * Reference counting follows nvkm_vmm_pt.refs[] (vmm.h:15, vmm.c):
 *   - a PTE table counts its valid PTEs: big_used[di] / small_used[di]
 *     are pgt->refs[type], += on map (vmm.c:371), -= on unmap
 *     (vmm.c:277);
 *   - a directory counts its LIVE child tables, one per linked PDE (each
 *     half of a dual PDE separately): `used` is pgd->refs[0], ++ when a
 *     child becomes live (nvkm_vmm_ref_hwpt, vmm.c:426), -- when a child
 *     empties (nvkm_vmm_unref_pdes, vmm.c:150), recursing upward at 0.
 * A PDE word is written when its child goes 0 -> 1 references and
 * zeroed when it goes back to 0, so `used` can never drift from the
 * number of linked PDEs. Table memory is kept cached after it empties
 * (nouveau frees it; only the PDE words are observable here).
 */

/* Dual leaf PDE table: 256 entries x 16B (two 64-bit words each). */
struct pg106_dual_table {
	uint64_t addr;
	uint64_t word_big[PG106_DUAL_ENTRIES];   /* entry word 0: 64K half */
	uint64_t word_small[PG106_DUAL_ENTRIES]; /* entry word 1: 4K half */
	struct pg106_pte_page *big[PG106_DUAL_ENTRIES];
	struct pg106_pte_page *small[PG106_DUAL_ENTRIES];
	uint32_t big_used[PG106_DUAL_ENTRIES];   /* valid 64K PTEs */
	uint32_t small_used[PG106_DUAL_ENTRIES]; /* valid 4K PTEs */
	uint32_t used;		/* linked PDE halves (big + small) */
	struct pg106_dirty d;	/* in 16-byte dual entries */
};

/* Level 1 PDE table: children are dual tables. */
struct pg106_pd1_low {
	uint64_t addr;
	uint64_t pde[PG106_PDE1_ENTRIES];
	struct pg106_dual_table *child[PG106_PDE1_ENTRIES];
	uint32_t used;		/* linked pde[] slots (live children) */
	struct pg106_dirty d;
};

/* Level 2 PDE table: children are level-1 tables. */
struct pg106_pd1_mid {
	uint64_t addr;
	uint64_t pde[PG106_PDE_ENTRIES];
	struct pg106_pd1_low *child[PG106_PDE_ENTRIES];
	uint32_t used;		/* linked pde[] slots (live children) */
	struct pg106_dirty d;
};

struct pg106_mmu {
	struct pg106_mmu_cfg cfg;
	uint64_t next_addr;	/* next page-table GPU address to hand out */
	uint64_t root_addr;
	uint64_t root[PG106_ROOT_ENTRIES];
	struct pg106_pd1_mid *l2[PG106_ROOT_ENTRIES];
	struct pg106_dirty root_d;
	uint32_t ntables;	/* page-table pages handed out (root included) */
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

/*
 * Backing address of a PTE (inverse of pte_encode). The address field
 * depends on the aperture (hwref dev_mmu.h:610-616):
 *   VIDEO/PEER: NV_MMU_VER2_PTE_ADDRESS_VID (35-3):8, i.e. 25 bits;
 *               bits 35:33 are ADDRESS_VID_PEER (peer id) and 53:36
 *               COMPTAGLINE - neither is part of the address;
 *   SYSTEM:     NV_MMU_VER2_PTE_ADDRESS_SYS 53:8, 46 bits.
 * Bits 63:56 (KIND) are never address bits. The field holds pa >> 12.
 */
static uint64_t pte_decode_pa(uint64_t word)
{
	unsigned aper = (unsigned)(word >> 1) & 3u;
	uint64_t field = word >> 8;

	if (aper <= 1u)		/* VIDEO_MEMORY (0) / PEER_MEMORY (1) */
		field &= (1ULL << 25) - 1;
	else
		field &= (1ULL << 46) - 1;
	return field << PG106_SPT_SHIFT;
}

/* ------------------------------------------------------------------ */
/* Allocation helpers                                                  */
/* ------------------------------------------------------------------ */

/* Hand out the next page-table GPU address; -ERANGE once the address
 * would exceed the aperture's address field (VID fields are 32:8 ->
 * 2^37, SYS fields 53:8 -> 2^58; hwref ADDRESS_VID/ADDRESS_SYS). */
static int take_addr(struct pg106_mmu *m, uint64_t *out)
{
	uint64_t limit = (m->cfg.table_target == PG106_TARGET_VRAM)
				? PG106_VRAM_ADDR_LIMIT : PG106_SYS_ADDR_LIMIT;

	if (m->next_addr + PG106_TABLE_ALIGN > limit)
		return -ERANGE;
	/* The caller's page-table region is full (cfg.table_limit). */
	if (m->cfg.table_limit &&
	    m->next_addr + PG106_TABLE_ALIGN > m->cfg.table_limit)
		return -ENOSPC;
	*out = m->next_addr;
	m->next_addr += PG106_TABLE_ALIGN;
	m->ntables++;
	return 0;
}

static void *alloc_zero(size_t size)
{
	return calloc(1, size);
}

/* ------------------------------------------------------------------ */
/* Tree construction                                                   */
/* ------------------------------------------------------------------ */

/* Allocate one zeroed table node plus its GPU address. The PDE that
 * points at it is NOT written here: ref_pte() links it once the first
 * PTE below it becomes valid. Returns 0, -ENOMEM, or take_addr()'s
 * -ERANGE (address space for page tables exhausted). */
static int alloc_table(struct pg106_mmu *m, size_t size, void **out,
		       uint64_t *addr)
{
	void *t = alloc_zero(size);
	int ret;

	if (!t)
		return -ENOMEM;
	ret = take_addr(m, addr);
	if (ret) {
		free(t);
		return ret;
	}
	*out = t;
	return 0;
}

#define ENSURE_CHILD(m, slot, nent)					\
	do {								\
		if (!(slot)) {						\
			void *n_;					\
			uint64_t a_;					\
			int r_ = alloc_table((m), sizeof(*(slot)), &n_, &a_); \
			if (r_)						\
				return r_;				\
			(slot) = n_;					\
			(slot)->addr = a_;				\
			dirty_all(&(slot)->d, (nent));			\
		}							\
	} while (0)

/* Make sure every table on va's path exists, down to the PTE page of
 * the requested size. Nothing is linked yet. */
static int ensure_path(struct pg106_mmu *m, uint64_t va, bool big,
		       struct pg106_dual_table **dual_out)
{
	unsigned ri = (va >> PG106_ROOT_SHIFT) & (PG106_ROOT_ENTRIES - 1u);
	unsigned i2 = (va >> PG106_PDE_SHIFT) & (PG106_PDE_ENTRIES - 1u);
	unsigned i1 = (va >> PG106_PDE1_SHIFT) & (PG106_PDE1_ENTRIES - 1u);
	unsigned di = (va >> PG106_DUAL_SHIFT) & (PG106_DUAL_ENTRIES - 1u);
	struct pg106_pd1_mid *l2;
	struct pg106_pd1_low *l1;
	struct pg106_dual_table *dual;

	ENSURE_CHILD(m, m->l2[ri], PG106_PDE_ENTRIES);
	l2 = m->l2[ri];
	ENSURE_CHILD(m, l2->child[i2], PG106_PDE1_ENTRIES);
	l1 = l2->child[i2];
	ENSURE_CHILD(m, l1->child[i1], PG106_DUAL_ENTRIES);
	dual = l1->child[i1];
	/* A PTE page is a full 4KiB slot either way (see PG106_TABLE_ALIGN):
	 * the first upload zeroes all of it. */
	if (big)
		ENSURE_CHILD(m, dual->big[di], PG106_SPT_ENTRIES);
	else
		ENSURE_CHILD(m, dual->small[di], PG106_SPT_ENTRIES);
	*dual_out = dual;
	return 0;
}

/* One PTE at va became valid: take a reference on its PTE table and,
 * for every table that thereby goes from 0 to 1 references, link the
 * PDE pointing at it (nvkm_vmm_ref_ptes vmm.c:371 + ref_hwpt :426). */
static void ref_pte(struct pg106_mmu *m, uint64_t va, bool big)
{
	enum pg106_target tt = m->cfg.table_target;
	unsigned ri = (va >> PG106_ROOT_SHIFT) & (PG106_ROOT_ENTRIES - 1u);
	unsigned i2 = (va >> PG106_PDE_SHIFT) & (PG106_PDE_ENTRIES - 1u);
	unsigned i1 = (va >> PG106_PDE1_SHIFT) & (PG106_PDE1_ENTRIES - 1u);
	unsigned di = (va >> PG106_DUAL_SHIFT) & (PG106_DUAL_ENTRIES - 1u);
	struct pg106_pd1_mid *l2 = m->l2[ri];
	struct pg106_pd1_low *l1 = l2->child[i2];
	struct pg106_dual_table *dual = l1->child[i1];

	if (big) {
		if (dual->big_used[di]++)
			return;
		/* Word 0 of the dual PDE: big (64K) table pointer. */
		dual->word_big[di] = pde_encode(tt, dual->big[di]->addr);
	} else {
		if (dual->small_used[di]++)
			return;
		/* Word 1 of the dual PDE: small (4K) table pointer. */
		dual->word_small[di] = pde_encode(tt, dual->small[di]->addr);
	}
	dirty_mark(&dual->d, di);
	if (dual->used++)
		return;
	l1->pde[i1] = pde_encode(tt, dual->addr);
	dirty_mark(&l1->d, i1);
	if (l1->used++)
		return;
	l2->pde[i2] = pde_encode(tt, l1->addr);
	dirty_mark(&l2->d, i2);
	if (l2->used++)
		return;
	m->root[ri] = pde_encode(tt, l2->addr);
	dirty_mark(&m->root_d, ri);
}

/* Inverse of ref_pte(): drop the PTE table's reference and unlink every
 * PDE whose child reaches 0 (nvkm_vmm_unref_ptes vmm.c:277-289,
 * nvkm_vmm_unref_pdes :150; gp100_vmm_pd0_unmap zeroes the dual PDE). */
static void unref_pte(struct pg106_mmu *m, uint64_t va, bool big)
{
	unsigned ri = (va >> PG106_ROOT_SHIFT) & (PG106_ROOT_ENTRIES - 1u);
	unsigned i2 = (va >> PG106_PDE_SHIFT) & (PG106_PDE_ENTRIES - 1u);
	unsigned i1 = (va >> PG106_PDE1_SHIFT) & (PG106_PDE1_ENTRIES - 1u);
	unsigned di = (va >> PG106_DUAL_SHIFT) & (PG106_DUAL_ENTRIES - 1u);
	struct pg106_pd1_mid *l2 = m->l2[ri];
	struct pg106_pd1_low *l1 = l2->child[i2];
	struct pg106_dual_table *dual = l1->child[i1];

	if (big) {
		if (--dual->big_used[di])
			return;
		dual->word_big[di] = 0;
	} else {
		if (--dual->small_used[di])
			return;
		dual->word_small[di] = 0;
	}
	dirty_mark(&dual->d, di);
	if (--dual->used)
		return;
	l1->pde[i1] = 0;
	dirty_mark(&l1->d, i1);
	if (--l1->used)
		return;
	l2->pde[i2] = 0;
	dirty_mark(&l2->d, i2);
	if (--l2->used)
		return;
	m->root[ri] = 0;
	dirty_mark(&m->root_d, ri);
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
	if (cfg->table_limit &&
	    (cfg->table_limit & (PG106_TABLE_ALIGN - 1u) ||
	     cfg->table_limit < cfg->table_base + PG106_TABLE_ALIGN)) {
		free(m);
		return -EINVAL;
	}
	m->cfg = *cfg;
	m->root_addr = cfg->table_base;
	m->next_addr = cfg->table_base + PG106_TABLE_ALIGN;
	m->ntables = 1;
	dirty_all(&m->root_d, PG106_ROOT_ENTRIES);
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

/*
 * True if a PTE of the OTHER page size already maps any part of the page
 * at va. Both halves of a dual PDE may be live at once (vmm.c:440-446
 * handles an LPT over existing SPTs), but this builder promises -EEXIST
 * for any double mapping, so overlap is checked at both granularities:
 * a 4K page is covered by the 64K PTE (va >> 16) & 31; a 64K page covers
 * the sixteen 4K PTEs starting at (va >> 12) & 511.
 */
static bool other_size_mapped(const struct pg106_mmu *m, uint64_t va, bool big)
{
	unsigned di = (va >> PG106_DUAL_SHIFT) & (PG106_DUAL_ENTRIES - 1u);
	const struct pg106_dual_table *dual = find_dual(m, va);

	if (!dual)
		return false;
	if (!big) {
		const struct pg106_pte_page *pt = dual->big[di];
		unsigned pi = (va >> PG106_LPT_SHIFT) & (PG106_LPT_ENTRIES - 1u);

		return pt && (pt->pte[pi] & 1ULL);
	} else {
		const struct pg106_pte_page *pt = dual->small[di];
		unsigned pi = (va >> PG106_SPT_SHIFT) & (PG106_SPT_ENTRIES - 1u);
		unsigned n = 1u << (PG106_PAGE_64K_SHIFT - PG106_PAGE_4K_SHIFT);

		if (!pt)
			return false;
		for (unsigned i = 0; i < n; i++)
			if (pt->pte[pi + i] & 1ULL)
				return true;
		return false;
	}
}

int pg106_mmu_map(struct pg106_mmu *m, const struct pg106_map *map)
{
	uint64_t page, off;
	bool big;
	int ret;

	ret = check_map_args(m, map);
	if (ret)
		return ret;

	big = map->page_shift == PG106_PAGE_64K_SHIFT;
	page = 1ULL << map->page_shift;
	for (off = 0; off < map->size; off += page) {
		uint64_t va = map->va + off;
		unsigned di = (va >> PG106_DUAL_SHIFT) & (PG106_DUAL_ENTRIES - 1u);
		struct pg106_dual_table *dual;
		struct pg106_pte_page *pt;
		unsigned pi;

		ret = ensure_path(m, va, big, &dual);
		if (ret)
			break;
		if (big) {
			pt = dual->big[di];
			pi = (va >> PG106_LPT_SHIFT) & (PG106_LPT_ENTRIES - 1u);
		} else {
			pt = dual->small[di];
			pi = (va >> PG106_SPT_SHIFT) & (PG106_SPT_ENTRIES - 1u);
		}
		if ((pt->pte[pi] & 1ULL) || other_size_mapped(m, va, big)) {
			ret = -EEXIST;
			break;
		}
		pt->pte[pi] = pte_encode(map, map->pa + off);
		dirty_mark(&pt->d, pi);
		ref_pte(m, va, big);
	}
	if (ret) {
		/* All or nothing, like nvkm_vmm_ptes_get_map()'s fail path
		 * (vmm.c nvkm_vmm_iter "goto fail" unrefs what it took):
		 * drop the pages this call already mapped. */
		if (off)
			pg106_mmu_unmap(m, map->va, off, map->page_shift);
		return ret;
	}
	return 0;
}

int pg106_mmu_unmap(struct pg106_mmu *m, uint64_t va, uint64_t size,
		    unsigned page_shift)
{
	uint64_t page, off;
	bool big;

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

	big = page_shift == PG106_PAGE_64K_SHIFT;
	page = 1ULL << page_shift;
	for (off = 0; off < size; off += page) {
		uint64_t cur = va + off;
		unsigned di = (cur >> PG106_DUAL_SHIFT) & (PG106_DUAL_ENTRIES - 1u);
		struct pg106_dual_table *dual = find_dual(m, cur);
		struct pg106_pte_page *pt;
		unsigned pi;

		if (!dual)
			continue;
		if (big) {
			pt = dual->big[di];
			pi = (cur >> PG106_LPT_SHIFT) & (PG106_LPT_ENTRIES - 1u);
		} else {
			pt = dual->small[di];
			pi = (cur >> PG106_SPT_SHIFT) & (PG106_SPT_ENTRIES - 1u);
		}
		if (!pt || !(pt->pte[pi] & 1ULL))
			continue;	/* idempotent: not mapped */
		pt->pte[pi] = 0;	/* VALID_FALSE unmaps */
		dirty_mark(&pt->d, pi);
		unref_pte(m, cur, big);
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

	/*
	 * A valid big-page PTE wins; the small table is consulted only when
	 * the big entry does not map va. That is the hardware rule nouveau
	 * works around in nvkm_vmm_ref_hwpt (vmm.c:440-446): LPTEs over
	 * valid SPTEs must be written "unmapped" rather than invalid/sparse,
	 * otherwise "the MMU [would not look] at the SPTEs". (The map path
	 * refuses to create both, so this only matters for hand-built
	 * states.)
	 */
	if (pde_linked(dual->word_big[di])) {
		struct pg106_pte_page *pt = dual->big[di];
		unsigned pi = (va >> PG106_LPT_SHIFT) & (PG106_LPT_ENTRIES - 1u);

		if (pt && (pt->pte[pi] & 1ULL)) {
			q->mapped = true;
			q->big = true;
			q->page_shift = PG106_PAGE_64K_SHIFT;
			q->pte_word = pt->pte[pi];
			q->pa = pte_decode_pa(q->pte_word);
			q->aperture = (uint8_t)((q->pte_word >> 1) & 3u);
			q->kind = (uint8_t)(q->pte_word >> 56);
			return true;
		}
	}
	if (pde_linked(dual->word_small[di])) {
		struct pg106_pte_page *pt = dual->small[di];
		unsigned pi = (va >> PG106_SPT_SHIFT) & (PG106_SPT_ENTRIES - 1u);

		if (pt && (pt->pte[pi] & 1ULL)) {
			q->mapped = true;
			q->big = false;
			q->page_shift = PG106_PAGE_4K_SHIFT;
			q->pte_word = pt->pte[pi];
			q->pa = pte_decode_pa(q->pte_word);
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

/* ------------------------------------------------------------------ */
/* Upload: write changed table words to their GPU addresses            */
/* ------------------------------------------------------------------ */

/* Emit entries [lo, hi] of a table of 64-bit words (`stride` words per
 * entry: 1, or 2 for the dual PDE whose halves live in two arrays) as
 * little-endian bytes, in chunks. */
struct sync_ctx {
	pg106_mmu_write_fn wr;
	void *user;
	uint32_t writes;
	uint64_t bytes;
};

#define SYNC_CHUNK_WORDS 32u

static int sync_words(struct sync_ctx *c, uint64_t base, const uint64_t *w0,
		      const uint64_t *w1, struct pg106_dirty *d)
{
	uint8_t buf[SYNC_CHUNK_WORDS * 8];
	unsigned stride = w1 ? 2u : 1u;
	uint32_t i, n = 0;
	uint64_t at;
	int ret;

	if (!d->any)
		return 0;
	at = base + (uint64_t)d->lo * stride * 8u;
	for (i = d->lo; i <= d->hi; i++) {
		unsigned h;

		for (h = 0; h < stride; h++) {
			uint64_t v = h ? w1[i] : w0[i];
			unsigned b;

			for (b = 0; b < 8; b++)
				buf[n * 8 + b] = (uint8_t)(v >> (8 * b));
			if (++n == SYNC_CHUNK_WORDS) {
				ret = c->wr(c->user, at, buf, n * 8);
				if (ret)
					return ret;
				c->writes++;
				c->bytes += n * 8;
				at += n * 8;
				n = 0;
			}
		}
	}
	if (n) {
		ret = c->wr(c->user, at, buf, n * 8);
		if (ret)
			return ret;
		c->writes++;
		c->bytes += n * 8;
	}
	d->any = false;
	return 0;
}

int pg106_mmu_sync(struct pg106_mmu *m, pg106_mmu_write_fn wr, void *user,
		   struct pg106_mmu_sync_stats *stats)
{
	struct sync_ctx c = { wr, user, 0, 0 };
	unsigned ri, i2, i1, di;
	int ret = 0;

	if (!m || !wr)
		return -EINVAL;
	/* Leaves first, the root last: a PDE never points at a table whose
	 * contents have not been written yet (the GPU may walk the tree
	 * while this runs; nouveau orders its writes the same way, PTEs in
	 * nvkm_vmm_ptes_get_map before the PDE in nvkm_vmm_ref_hwpt). */
	for (ri = 0; ri < PG106_ROOT_ENTRIES && !ret; ri++) {
		struct pg106_pd1_mid *l2 = m->l2[ri];

		if (!l2)
			continue;
		for (i2 = 0; i2 < PG106_PDE_ENTRIES && !ret; i2++) {
			struct pg106_pd1_low *l1 = l2->child[i2];

			if (!l1)
				continue;
			for (i1 = 0; i1 < PG106_PDE1_ENTRIES && !ret; i1++) {
				struct pg106_dual_table *dual = l1->child[i1];

				if (!dual)
					continue;
				for (di = 0; di < PG106_DUAL_ENTRIES && !ret; di++) {
					struct pg106_pte_page *pt;

					pt = dual->big[di];
					if (pt)
						ret = sync_words(&c, pt->addr, pt->pte,
								 NULL, &pt->d);
					pt = dual->small[di];
					if (pt && !ret)
						ret = sync_words(&c, pt->addr, pt->pte,
								 NULL, &pt->d);
				}
				if (!ret)
					ret = sync_words(&c, dual->addr,
							 dual->word_big,
							 dual->word_small, &dual->d);
			}
			if (!ret)
				ret = sync_words(&c, l1->addr, l1->pde, NULL, &l1->d);
		}
		if (!ret)
			ret = sync_words(&c, l2->addr, l2->pde, NULL, &l2->d);
	}
	if (!ret)
		ret = sync_words(&c, m->root_addr, m->root, NULL, &m->root_d);
	if (stats) {
		stats->writes = c.writes;
		stats->bytes = c.bytes;
	}
	return ret;
}

static void mark_tree(struct pg106_mmu *m)
{
	unsigned ri, i2, i1, di;

	dirty_all(&m->root_d, PG106_ROOT_ENTRIES);
	for (ri = 0; ri < PG106_ROOT_ENTRIES; ri++) {
		struct pg106_pd1_mid *l2 = m->l2[ri];

		if (!l2)
			continue;
		dirty_all(&l2->d, PG106_PDE_ENTRIES);
		for (i2 = 0; i2 < PG106_PDE_ENTRIES; i2++) {
			struct pg106_pd1_low *l1 = l2->child[i2];

			if (!l1)
				continue;
			dirty_all(&l1->d, PG106_PDE1_ENTRIES);
			for (i1 = 0; i1 < PG106_PDE1_ENTRIES; i1++) {
				struct pg106_dual_table *dual = l1->child[i1];

				if (!dual)
					continue;
				dirty_all(&dual->d, PG106_DUAL_ENTRIES);
				for (di = 0; di < PG106_DUAL_ENTRIES; di++) {
					if (dual->big[di])
						dirty_all(&dual->big[di]->d,
							  PG106_SPT_ENTRIES);
					if (dual->small[di])
						dirty_all(&dual->small[di]->d,
							  PG106_SPT_ENTRIES);
				}
			}
		}
	}
}

void pg106_mmu_mark_all_dirty(struct pg106_mmu *m)
{
	if (m)
		mark_tree(m);
}

bool pg106_mmu_dirty(const struct pg106_mmu *m)
{
	unsigned ri, i2, i1, di;

	if (!m)
		return false;
	if (m->root_d.any)
		return true;
	for (ri = 0; ri < PG106_ROOT_ENTRIES; ri++) {
		const struct pg106_pd1_mid *l2 = m->l2[ri];

		if (!l2)
			continue;
		if (l2->d.any)
			return true;
		for (i2 = 0; i2 < PG106_PDE_ENTRIES; i2++) {
			const struct pg106_pd1_low *l1 = l2->child[i2];

			if (!l1)
				continue;
			if (l1->d.any)
				return true;
			for (i1 = 0; i1 < PG106_PDE1_ENTRIES; i1++) {
				const struct pg106_dual_table *dual = l1->child[i1];

				if (!dual)
					continue;
				if (dual->d.any)
					return true;
				for (di = 0; di < PG106_DUAL_ENTRIES; di++)
					if ((dual->big[di] && dual->big[di]->d.any) ||
					    (dual->small[di] && dual->small[di]->d.any))
						return true;
			}
		}
	}
	return false;
}

uint32_t pg106_mmu_table_count(const struct pg106_mmu *m)
{
	return m ? m->ntables : 0;
}

uint64_t pg106_mmu_table_end(const struct pg106_mmu *m)
{
	return m ? m->next_addr : 0;
}
