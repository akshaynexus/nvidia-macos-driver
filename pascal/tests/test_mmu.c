// SPDX-License-Identifier: GPL-2.0-only
/*
 * Golden vectors for the Pascal (GP10x) MMU v2 page-table builder.
 *
 * Expected words are recomputed here independently of pascal/core/src/mmu.c,
 * by packing bit ranges taken verbatim from the Mesa hwref header
 * third_party/mesa:src/nouveau/headers/nvidia/hwref/pascal/gp100/dev_mmu.h (NV_MMU_VER2_*), and cross-checked
 * against nouveau's builder functions:
 *   - nvkm/subdev/mmu/vmmgp100.c: gp100_vmm_pgt_pte() (PTE = (addr >> 4) |
 *     map->type), gp100_vmm_valid() (map->type = VALID|aperture<<1|vol<<3|
 *     priv<<5|ro<<6|kind<<56, lines 490-495), gp100_vmm_pde() (PDE aperture
 *     1/2/3 << 1, VOL for coherent sysmem, address >> 4, lines 237-251),
 *     gp100_vmm_pd0_pde() (128-bit dual PDE, lines 253-268), descriptor
 *     tables gp100_vmm_desc_12/_16 (lines 402-420).
 *   - nvkm/subdev/mmu/vmmgf100.c: gf100_vmm_aper() VRAM->0 HOST->2 NCOH->3
 *     (lines 324-333).
 *   - nvkm/subdev/mmu/gm200.c: gm200_mmu_kind() kind table, *invalid = 0xff
 *     (lines 30-69) - used by GP100 (mmu/gp100.c:35).
 * Local copies of the nouveau files: /tmp/pascal-ref/.
 */

#include <pascal/mmu.h>

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

static int checks;
static int failures;

#define CHECK(cond, name)                                                   \
	do {                                                                \
		checks++;                                                   \
		if (!(cond)) {                                              \
			failures++;                                         \
			printf("FAIL: %s (test_mmu.c:%d)\n", name, __LINE__); \
		}                                                           \
	} while (0)

#define CHECK_EQ_U64(got, want, name)                                       \
	do {                                                                \
		uint64_t g_ = (uint64_t)(got), w_ = (uint64_t)(want);       \
		checks++;                                                   \
		if (g_ != w_) {                                             \
			failures++;                                         \
			printf("FAIL: %s (test_mmu.c:%d): got 0x%016" PRIx64 \
			       ", want 0x%016" PRIx64 "\n",                 \
			       name, __LINE__, g_, w_);                     \
		}                                                           \
	} while (0)

/* ------------------------------------------------------------------ */
/* MW-style field packing. The (lo,hi) ranges in the wrappers below are */
/* copied verbatim from third_party/mesa:src/nouveau/headers/nvidia/hwref/pascal/gp100/dev_mmu.h.       */
/* ------------------------------------------------------------------ */

#define PG_FLD(lo, hi, v) ((((uint64_t)(v)) << (lo)) & \
			   ((((uint64_t)1 << ((hi) - (lo) + 1)) - 1) << (lo)))

/* NV_MMU_VER2_PTE_* (hwref_gp100_dev_mmu.h:586-624) */
#define pg_pte_valid(v)		PG_FLD(0, 0, v)			/* VALID 0:0 */
#define pg_pte_aperture(v)	PG_FLD(1, 2, v)			/* APERTURE 2:1 */
#define pg_pte_vol(v)		PG_FLD(3, 3, v)			/* VOL 3:3 */
#define pg_pte_privilege(v)	PG_FLD(5, 5, v)			/* PRIVILEGE 5:5 */
#define pg_pte_read_only(v)	PG_FLD(6, 6, v)			/* READ_ONLY 6:6 */
#define pg_pte_address(v)	PG_FLD(8, 53, v)		/* ADDRESS_SYS 53:8 */
#define pg_pte_kind(v)		PG_FLD(56, 63, v)		/* KIND 63:56 */

/* NV_MMU_VER2_PDE_* / NV_MMU_VER2_DUAL_PDE_* per-word layout
 * (hwref_gp100_dev_mmu.h:525-585; the DUAL_PDE word 0 (big) and word 1
 * (small) halves are bit-identical within their 64-bit word). */
#define pg_pde_aperture(v)	PG_FLD(1, 2, v)			/* APERTURE 2:1 */
#define pg_pde_vol(v)		PG_FLD(3, 3, v)			/* VOL 3:3 */
#define pg_pde_address(v)	PG_FLD(8, 53, v)		/* ADDRESS_SYS 53:8 */

/* Field values (hwref macro names). */
#define PTE_APER_VIDEO		0x0	/* NV_MMU_VER2_PTE_APERTURE_VIDEO_MEMORY */
#define PTE_APER_SYS_COH	0x2	/* ..._SYSTEM_COHERENT_MEMORY */
#define PTE_APER_SYS_NCOH	0x3	/* ..._SYSTEM_NON_COHERENT_MEMORY */
#define PDE_APER_INVALID	0x0	/* NV_MMU_VER2_PDE_APERTURE_INVALID */
#define PDE_APER_VIDEO		0x1	/* NV_MMU_VER2_PDE_APERTURE_VIDEO_MEMORY */
#define PDE_APER_SYS_COH	0x2	/* ..._SYSTEM_COHERENT_MEMORY */

/* Independent PTE expectation (see gp100_vmm_valid packing, vmmgp100.c). */
static uint64_t exp_pte(uint64_t pa, uint64_t aper, bool vol, bool priv,
			bool ro, uint64_t kind)
{
	return pg_pte_valid(1) |
	       pg_pte_aperture(aper) |
	       pg_pte_vol(vol) |
	       pg_pte_privilege(priv) |
	       pg_pte_read_only(ro) |
	       pg_pte_address(pa >> PG106_PAGE_4K_SHIFT) |
	       pg_pte_kind(kind);
}

/*
 * Independent PDE expectation. Bit 0 (IS_PTE/VALID) deliberately not set:
 * gp100_vmm_pde() (vmmgp100.c:237-251) writes only aperture [+VOL] and the
 * address; a PDE is linked iff aperture != NV_MMU_VER2_PDE_APERTURE_INVALID.
 */
static uint64_t exp_pde(uint64_t table_addr, uint64_t aper, bool vol)
{
	return pg_pde_aperture(aper) |
	       pg_pde_vol(vol) |
	       pg_pde_address(table_addr >> PG106_PAGE_4K_SHIFT);
}

static struct pg106_mmu *mk_vmm(enum pg106_target target, uint64_t base)
{
	struct pg106_mmu_cfg cfg = { .table_target = target,
				     .table_base = base };
	struct pg106_mmu *mmu = NULL;

	if (pg106_mmu_init(&mmu, &cfg) != 0) {
		printf("FAIL: pg106_mmu_init\n");
		failures++;
		return NULL;
	}
	return mmu;
}

static void map1(struct pg106_mmu *mmu, uint64_t va, uint64_t pa,
		 unsigned shift, enum pg106_target target, uint8_t kind)
{
	struct pg106_map m = {
		.va = va, .pa = pa, .size = 1ULL << shift,
		.page_shift = shift, .target = target, .kind = kind,
	};
	CHECK(pg106_mmu_map(mmu, &m) == 0, "pg106_mmu_map");
}

/* ------------------------------------------------------------------ */
/* 1. 4KiB page at VA 0, video RAM, kind PITCH.                        */
/* ------------------------------------------------------------------ */
static void test_4k_va0_vram(void)
{
	struct pg106_mmu *mmu = mk_vmm(PG106_TARGET_VRAM, 0x10000000ULL);
	const uint64_t va = 0, pa = 0x20000000ULL; /* 512MiB VRAM */
	uint64_t root, l2addr, l1addr, dualaddr, pteaddr, word, big, small;
	struct pg106_pte_query q;

	CHECK(mmu != NULL, "vmm alloc");
	if (!mmu)
		return;
	map1(mmu, va, pa, PG106_PAGE_4K_SHIFT, PG106_TARGET_VRAM,
	     PG106_KIND_PITCH);

	/* PTE: cross-check the whole word against the literal nouveau
	 * produces: gp100_vmm_pgt_pte() writes (addr >> 4) | map->type
	 * (vmmgp100.c:135) where gp100_vmm_valid() sets type = BIT(0)
	 * [VALID] | aper << 1 with VRAM aper 0 (gf100_vmm_aper,
	 * vmmgf100.c:327) | kind 0x00 << 56 (vmmgp100.c:490-495).
	 * (0x20000000 >> 4) | 1 = 0x0000000002000001. */
	CHECK(pg106_mmu_walk(mmu, va, &q), "walk va 0");
	CHECK_EQ_U64(q.pte_word, 0x0000000002000001ULL, "PTE literal");
	CHECK_EQ_U64(q.pte_word, exp_pte(pa, PTE_APER_VIDEO, false, false,
					 false, PG106_KIND_PITCH),
		     "PTE from field packing");
	CHECK_EQ_U64(q.pa, pa, "walk decodes pa");
	CHECK(!q.big && q.page_shift == PG106_PAGE_4K_SHIFT, "4K page size");
	CHECK_EQ_U64(q.aperture, PTE_APER_VIDEO, "PTE aperture field");
	CHECK_EQ_U64(q.kind, PG106_KIND_PITCH, "PTE kind field");

	/* Next 4K page: same PTE page, different slot -> unmapped. */
	CHECK(!pg106_mmu_walk(mmu, va + 0x1000ULL, &q), "va+4K unmapped");

	/* Dual leaf PDE (word 1 = small pointer, gp100_vmm_pd0_pde). */
	CHECK(pg106_mmu_table_addr(mmu, va, PG106_TABLE_PTE_4K, &pteaddr),
	      "PTE page addr");
	CHECK(pg106_mmu_dual_pde_words(mmu, va, &big, &small), "dual words");
	CHECK_EQ_U64(big, 0, "no 64K half");
	CHECK_EQ_U64(small, exp_pde(pteaddr, PDE_APER_VIDEO, false),
		     "dual small word");
	CHECK_EQ_U64(small, PG_FLD(1, 2, 1) |
			    PG_FLD(8, 53, pteaddr >> 12),
		     "small word = VIDEO_MEMORY aperture + address");

	/* Mid PDEs and root: aperture VIDEO_MEMORY = 1 - this is nouveau's
	 * `*data |= 1ULL << 1` for VRAM tables (gp100_vmm_pde,
	 * vmmgp100.c:240); note it differs from the PTE encoding where
	 * VIDEO_MEMORY = 0. */
	CHECK(pg106_mmu_table_addr(mmu, va, PG106_TABLE_PDE2, &l2addr),
	      "l2 addr");
	CHECK(pg106_mmu_table_addr(mmu, va, PG106_TABLE_PDE1, &l1addr),
	      "l1 addr");
	CHECK(pg106_mmu_table_addr(mmu, va, PG106_TABLE_DUAL_PDE, &dualaddr),
	      "dual addr");
	CHECK(pg106_mmu_pde_word(mmu, va, PG106_PDE_LEVEL3, &root), "root w");
	CHECK(pg106_mmu_pde_word(mmu, va, PG106_PDE_LEVEL2, &word), "l2 w");
	CHECK_EQ_U64(word, exp_pde(l1addr, PDE_APER_VIDEO, false), "l2 word");
	CHECK(pg106_mmu_pde_word(mmu, va, PG106_PDE_LEVEL1, &word), "l1 w");
	CHECK_EQ_U64(word, exp_pde(dualaddr, PDE_APER_VIDEO, false),
		     "l1 word");
	CHECK_EQ_U64(root, exp_pde(l2addr, PDE_APER_VIDEO, false), "root w");
	CHECK_EQ_U64((root >> 1) & 3, 1, "root aperture == nouveau VRAM 1<<1");

	/* Every table address is 4KiB-aligned and inside the VRAM field
	 * (NV_MMU_VER2_PDE_ADDRESS_VID 32:8 -> 2^37). */
	CHECK(pteaddr % 0x1000 == 0 && pteaddr < PG106_VRAM_ADDR_LIMIT,
	      "PTE page addr in VID field");
	CHECK(pg106_mmu_root_addr(mmu) == 0x10000000ULL, "root addr");

	pg106_mmu_destroy(mmu);
	printf("PASS: 4K page at VA 0 (VRAM, kind PITCH): PTE/dual/mid/root words\n");
}

/* ------------------------------------------------------------------ */
/* 2. 64KiB big page at a VA crossing all PDE levels.                  */
/* ------------------------------------------------------------------ */
static void test_64k_big_vram(void)
{
	struct pg106_mmu *mmu = mk_vmm(PG106_TARGET_VRAM, 0x14000000ULL);
	/* 2^47 + ... : root slot 1, L2 slot 0x10, L1 slot 0x108. */
	const uint64_t va = 0x0000842100000000ULL;
	const uint64_t pa = 0x30000000ULL;
	uint64_t pteaddr, big, small, word;
	struct pg106_pte_query q;

	CHECK(mmu != NULL, "vmm alloc");
	if (!mmu)
		return;
	map1(mmu, va, pa, PG106_PAGE_64K_SHIFT, PG106_TARGET_VRAM,
	     PG106_KIND_PITCH);

	/* gp100_vmm_desc_16[]: { LPT, 5, 8, 0x0100 } - 32 PTEs per 256B
	 * table covering VA[20:16] (vmmgp100.c:404). PTE = (pa >> 4) | 1,
	 * advancing by map->next = (1 << 16) >> 4 per entry
	 * (vmmgp100.c:436, 135-142). */
	CHECK(((va >> 16) & 31) == 0, "LPT slot 0");
	CHECK(pg106_mmu_walk(mmu, va, &q), "walk big va");
	CHECK_EQ_U64(q.pte_word, 0x0000000003000001ULL, "big PTE literal");
	CHECK_EQ_U64(q.pte_word, exp_pte(pa, PTE_APER_VIDEO, false, false,
					 false, PG106_KIND_PITCH),
		     "big PTE from fields");
	CHECK(q.big && q.page_shift == PG106_PAGE_64K_SHIFT, "big flag/shift");

	/* Every 64K sub-offset inside the page resolves to the same PTE. */
	CHECK(pg106_mmu_walk(mmu, va + 0xc000ULL, &q), "walk va+48K");
	CHECK_EQ_U64(q.pte_word, 0x0000000003000001ULL, "same big PTE");

	/* Dual PDE: big half linked, small half INVALID (0). */
	CHECK(pg106_mmu_table_addr(mmu, va, PG106_TABLE_PTE_64K, &pteaddr),
	      "big table addr");
	CHECK(pg106_mmu_dual_pde_words(mmu, va, &big, &small), "dual words");
	CHECK_EQ_U64(big, exp_pde(pteaddr, PDE_APER_VIDEO, false),
		     "dual big word");
	CHECK_EQ_U64(small, 0, "dual small half empty");

	/* Next 64K page in the same dual slot: PTE slot 1 not written. */
	CHECK(!pg106_mmu_walk(mmu, va + 0x10000ULL, &q), "va+64K unmapped");

	/* Deep path: root slot 1, L2 slot 0x10, L1 slot 0x108 (shifts from
	 * gp100_vmm_desc_16 bits fields, vmmgp100.c:402-410). */
	CHECK(((va >> 47) & 3) == 1 && ((va >> 38) & 511) == 0x10 &&
	      ((va >> 29) & 511) == 0x108, "slot indices nonzero");
	CHECK(pg106_mmu_pde_word(mmu, va, PG106_PDE_LEVEL3, &word) && word,
	      "root linked");
	CHECK(pg106_mmu_pde_word(mmu, va, PG106_PDE_LEVEL2, &word) && word,
	      "l2 linked");
	CHECK(pg106_mmu_pde_word(mmu, va, PG106_PDE_LEVEL1, &word) && word,
	      "l1 linked");

	pg106_mmu_destroy(mmu);
	printf("PASS: 64K big page (VRAM): LPT PTE, dual PDE big half, deep walk\n");
}

/* ------------------------------------------------------------------ */
/* 3. System-memory apertures: coherent (with VOL) and non-coherent.   */
/* ------------------------------------------------------------------ */
static void test_sysmem_coherent(void)
{
	struct pg106_mmu *mmu = mk_vmm(PG106_TARGET_SYS_COHERENT,
				       0x18000000ULL);
	const uint64_t va = 0x10000000ULL, pa = 0x0c000000ULL;
	const uint64_t va_nc = va + 0x1000ULL, pa_nc = pa + 0x1000ULL;
	uint64_t pteaddr, l2addr, word, big, small;
	struct pg106_map m;
	struct pg106_pte_query q;

	CHECK(mmu != NULL, "vmm alloc");
	if (!mmu)
		return;

	/* Coherent sysmem: aper 2. VOL mirrors nouveau's unversioned map
	 * default vol = (target == HOST) (vmmgp100.c:446) -> PTE bit 3
	 * (vmmgp100.c:492). PTE literal: (0x0c000000 >> 4) | BIT(0)
	 * | (2 << 1) | (1 << 3) = 0x0000000000c0000d. */
	m = (struct pg106_map){ .va = va, .pa = pa, .size = 0x1000,
				.page_shift = PG106_PAGE_4K_SHIFT,
				.target = PG106_TARGET_SYS_COHERENT,
				.kind = PG106_KIND_PITCH, .vol = true };
	CHECK(pg106_mmu_map(mmu, &m) == 0, "map syscoh");
	CHECK(pg106_mmu_walk(mmu, va, &q), "walk syscoh");
	CHECK_EQ_U64(q.pte_word, 0x0000000000c0000dULL, "syscoh PTE literal");
	CHECK_EQ_U64(q.pte_word, exp_pte(pa, PTE_APER_SYS_COH, true, false,
					 false, PG106_KIND_PITCH),
		     "syscoh PTE from fields");
	CHECK_EQ_U64(q.aperture, PTE_APER_SYS_COH,
		     "NV_MMU_VER2_PTE_APERTURE_SYSTEM_COHERENT_MEMORY");

	/* Non-coherent sysmem: aper 3 (gf100_vmm_aper NCOH -> 3,
	 * vmmgf100.c:329), no VOL requested -> bit 3 clear. */
	m = (struct pg106_map){ .va = va_nc, .pa = pa_nc, .size = 0x1000,
				.page_shift = PG106_PAGE_4K_SHIFT,
				.target = PG106_TARGET_SYS_NONCOHERENT,
				.kind = PG106_KIND_PITCH };
	CHECK(pg106_mmu_map(mmu, &m) == 0, "map sysnc");
	CHECK(pg106_mmu_walk(mmu, va_nc, &q), "walk sysnc");
	CHECK_EQ_U64(q.pte_word, exp_pte(pa_nc, PTE_APER_SYS_NCOH, false,
					 false, false, PG106_KIND_PITCH),
		     "sysnc PTE from fields");
	CHECK_EQ_U64(q.aperture, PTE_APER_SYS_NCOH, "noncoherent aperture");

	/* The page-table pages themselves live in coherent sysmem: PDEs
	 * get aperture 2 AND VOL - gp100_vmm_pde() writes
	 * `2ULL << 1` plus `BIT_ULL(3)` for NVKM_MEM_TARGET_HOST
	 * (vmmgp100.c:241-243). The root PDE's address field points at
	 * the level-2 table. */
	CHECK(pg106_mmu_table_addr(mmu, va, PG106_TABLE_PTE_4K, &pteaddr),
	      "pte addr");
	CHECK(pg106_mmu_table_addr(mmu, va, PG106_TABLE_PDE2, &l2addr),
	      "l2 addr");
	CHECK(pg106_mmu_dual_pde_words(mmu, va, &big, &small), "dual words");
	CHECK_EQ_U64(small, exp_pde(pteaddr, PDE_APER_SYS_COH, true),
		     "dual small word (HOST: aper 2 + VOL)");
	CHECK_EQ_U64(small, PG_FLD(1, 2, 2) | PG_FLD(3, 3, 1) |
			    PG_FLD(8, 53, pteaddr >> 12),
		     "small word literal form");
	CHECK(pg106_mmu_pde_word(mmu, va, PG106_PDE_LEVEL3, &word), "root w");
	CHECK_EQ_U64(word, exp_pde(l2addr, PDE_APER_SYS_COH, true),
		     "root word (table sysmem HOST)");

	pg106_mmu_destroy(mmu);
	printf("PASS: sysmem coherent (aper 2 + VOL) / non-coherent (aper 3) PTEs and PDEs\n");
}

/* ------------------------------------------------------------------ */
/* 4. Unmapped VAs and unmap teardown.                                 */
/* ------------------------------------------------------------------ */
static void test_unmapped_and_unmap(void)
{
	struct pg106_mmu *mmu = mk_vmm(PG106_TARGET_VRAM, 0x1c000000ULL);
	const uint64_t va = 0x000040000000ULL; /* L2 slot 1, else zero */
	uint64_t root;
	struct pg106_pte_query q;

	CHECK(mmu != NULL, "vmm alloc");
	if (!mmu)
		return;

	CHECK(!pg106_mmu_walk(mmu, va, &q), "nothing mapped");
	CHECK(!pg106_mmu_walk(mmu, PG106_MMU_VA_LIMIT, &q), "va out of range");

	map1(mmu, va, 0x0e000000ULL, PG106_PAGE_4K_SHIFT, PG106_TARGET_VRAM,
	     PG106_KIND_PITCH);
	CHECK(pg106_mmu_walk(mmu, va, &q), "mapped after map");

	CHECK(pg106_mmu_unmap(mmu, va, 0x1000, PG106_PAGE_4K_SHIFT) == 0,
	      "unmap");
	CHECK(!pg106_mmu_walk(mmu, va, &q), "unmapped after unmap");

	/* Empty subtree is unlinked: root word back to INVALID
	 * (aperture 0), like gp100_vmm_pd0_unmap()/gf100_vmm_pgt_unmap(). */
	CHECK(pg106_mmu_pde_word(mmu, va, PG106_PDE_LEVEL3, &root), "root w");
	CHECK_EQ_U64((root >> 1) & 3, PDE_APER_INVALID, "root unlinked");

	/* Idempotent. */
	CHECK(pg106_mmu_unmap(mmu, va, 0x1000, PG106_PAGE_4K_SHIFT) == 0,
	      "second unmap");

	pg106_mmu_destroy(mmu);
	printf("PASS: unmapped VA invalid; unmap tears down PDE chain\n");
}

/* ------------------------------------------------------------------ */
/* 5. Walk through all PDE levels: 4K page at a high, fully-distributed */
/*    VA (root slot 1, L2 slot 0x46, L1 slot 0x10, dual slot 0x01).    */
/* ------------------------------------------------------------------ */
static void test_deep_walk(void)
{
	struct pg106_mmu *mmu = mk_vmm(PG106_TARGET_VRAM, 0x1c000000ULL);
	const uint64_t va = 0x0000918200200000ULL;
	const uint64_t pa = 0x04000000ULL;
	uint64_t big, small, word, l2addr, l1addr, dualaddr;
	struct pg106_pte_query q;

	CHECK(mmu != NULL, "vmm alloc");
	if (!mmu)
		return;

	map1(mmu, va, pa, PG106_PAGE_4K_SHIFT, PG106_TARGET_VRAM,
	     PG106_KIND_PITCH);

	CHECK(((va >> 47) & 3) == 1, "root slot 1");
	CHECK(((va >> 38) & 511) == 0x46, "l2 slot 0x46");
	CHECK(((va >> 29) & 511) == 0x10, "l1 slot 0x10");
	CHECK(((va >> 21) & 255) == 0x01, "dual slot 0x01");

	/* All three single-word PDE levels linked, with independently
	 * recomputed words. */
	CHECK(pg106_mmu_table_addr(mmu, va, PG106_TABLE_PDE2, &l2addr),
	      "l2 addr");
	CHECK(pg106_mmu_table_addr(mmu, va, PG106_TABLE_PDE1, &l1addr),
	      "l1 addr");
	CHECK(pg106_mmu_table_addr(mmu, va, PG106_TABLE_DUAL_PDE, &dualaddr),
	      "dual addr");
	CHECK(pg106_mmu_pde_word(mmu, va, PG106_PDE_LEVEL3, &word), "root");
	CHECK_EQ_U64(word, exp_pde(l2addr, PDE_APER_VIDEO, false),
		     "root word");
	CHECK(pg106_mmu_pde_word(mmu, va, PG106_PDE_LEVEL2, &word), "l2");
	CHECK_EQ_U64(word, exp_pde(l1addr, PDE_APER_VIDEO, false), "l2 word");
	CHECK(pg106_mmu_pde_word(mmu, va, PG106_PDE_LEVEL1, &word), "l1");
	CHECK_EQ_U64(word, exp_pde(dualaddr, PDE_APER_VIDEO, false),
		     "l1 word");

	/* Dual PDE: 16-byte entry, big half empty, small half linked
	 * (VMM_WO128 at pdei * 0x10, gp100_vmm_pd0_pde). */
	CHECK(pg106_mmu_dual_pde_words(mmu, va, &big, &small), "dual");
	CHECK_EQ_U64(big, 0, "no big half");
	CHECK(pg106_mmu_pde_word(mmu, va, PG106_PDE_LEVEL0_DUAL, &word),
	      "dual via pde_word");
	CHECK_EQ_U64(word, big, "pde_word(dual) returns big half");

	/* The walk itself descends root -> l2 -> l1 -> dual -> PTE page. */
	CHECK(pg106_mmu_walk(mmu, va, &q), "deep walk");
	CHECK_EQ_U64(q.pte_word, 0x0000000000400001ULL, "deep PTE literal");
	CHECK_EQ_U64(q.pa, pa, "deep walk pa");
	CHECK(!q.big, "4K resolution");

	pg106_mmu_destroy(mmu);
	printf("PASS: 2-level-deep+ walk (root slot 1, L2 0x46, L1 0x10, dual 0x01)\n");
}

/* ------------------------------------------------------------------ */
/* 6. Kind handling: KIND_INVALID rejected, PITCH accepted.            */
/* ------------------------------------------------------------------ */
static void test_kind_invalid(void)
{
	struct pg106_mmu *mmu = mk_vmm(PG106_TARGET_VRAM, 0x1c000000ULL);
	struct pg106_map m = {
		.va = 0x200000ULL, .pa = 0x08000000ULL, .size = 0x1000,
		.page_shift = PG106_PAGE_4K_SHIFT,
		.target = PG106_TARGET_VRAM,
		.kind = PG106_KIND_INVALID,	/* 0xff */
	};
	struct pg106_pte_query q;

	CHECK(mmu != NULL, "vmm alloc");
	if (!mmu)
		return;

	/* gm200_mmu_kind() (GP100's kind callback, mmu/gp100.c:35) marks
	 * 0xff invalid (gm200.c:68); gp100_vmm_valid() rejects such kinds
	 * with -EINVAL (vmmgp100.c:462-466). */
	CHECK(!pg106_mmu_kind_valid(PG106_KIND_INVALID), "kind 0xff invalid");
	CHECK(pg106_mmu_kind_valid(PG106_KIND_PITCH), "kind 0x00 valid");
	CHECK(pg106_mmu_map(mmu, &m) == -EINVAL, "KIND_INVALID rejected");
	CHECK(!pg106_mmu_walk(mmu, m.va, &q), "nothing mapped by rejected map");

	m.kind = PG106_KIND_PITCH;
	CHECK(pg106_mmu_map(mmu, &m) == 0, "PITCH accepted");
	CHECK(pg106_mmu_walk(mmu, m.va, &q), "walk PITCH");
	CHECK_EQ_U64(q.kind, PG106_KIND_PITCH, "PTE kind field");

	pg106_mmu_destroy(mmu);
	printf("PASS: kind handling (KIND_INVALID rejected, PITCH mapped)\n");
}

/* ------------------------------------------------------------------ */
/* 7. Refcounts are per linked PDE (nvkm_vmm_pt.refs[], vmm.h:15).     */
/* ------------------------------------------------------------------ */
static void test_refcount_shared_pd(void)
{
	struct pg106_mmu *mmu = mk_vmm(PG106_TARGET_VRAM, 0x10000000ULL);
	const uint64_t A = 0x000000, B = 0x200000; /* same dual table, di 0/1 */
	struct pg106_pte_query q;
	uint64_t w;

	if (!mmu)
		return;
	map1(mmu, A, 0x1000000, 12, PG106_TARGET_VRAM, PG106_KIND_PITCH);
	map1(mmu, B, 0x2000000, 12, PG106_TARGET_VRAM, PG106_KIND_PITCH);
	CHECK(pg106_mmu_unmap(mmu, A, 0x1000, 12) == 0, "unmap A");
	/* B's PTE table is still referenced, so every PDE above it must stay
	 * linked: vmm.c:150 only unlinks a PD when --pgd->refs[0] hits 0. */
	CHECK(pg106_mmu_walk(mmu, B, &q) && q.pa == 0x2000000, "B survives unmap A");
	CHECK(!pg106_mmu_walk(mmu, A, &q), "A unmapped");
	map1(mmu, A, 0x3000000, 12, PG106_TARGET_VRAM, PG106_KIND_PITCH);
	CHECK(pg106_mmu_unmap(mmu, B, 0x1000, 12) == 0, "unmap B");
	CHECK(pg106_mmu_walk(mmu, A, &q) && q.pa == 0x3000000, "remapped A survives unmap B");
	/* Repeated unmaps are idempotent and must not underflow a count:
	 * afterwards a fresh map still links the full path. */
	CHECK(pg106_mmu_unmap(mmu, B, 0x1000, 12) == 0, "unmap B again");
	CHECK(pg106_mmu_unmap(mmu, A, 0x1000, 12) == 0, "unmap A");
	CHECK(pg106_mmu_unmap(mmu, A, 0x1000, 12) == 0, "unmap A again");
	CHECK(pg106_mmu_pde_word(mmu, A, PG106_PDE_LEVEL3, &w) && w == 0,
	      "root PDE unlinked once everything is gone");
	map1(mmu, B, 0x4000000, 12, PG106_TARGET_VRAM, PG106_KIND_PITCH);
	CHECK(pg106_mmu_walk(mmu, B, &q) && q.pa == 0x4000000, "relink after empty");
	map1(mmu, A, 0x5000000, 12, PG106_TARGET_VRAM, PG106_KIND_PITCH);
	CHECK(pg106_mmu_unmap(mmu, A, 0x1000, 12) == 0, "unmap A (3)");
	CHECK(pg106_mmu_walk(mmu, B, &q), "B still live (no wrapped count)");
	pg106_mmu_destroy(mmu);
	printf("PASS: per-PDE refcounts (shared PD, unmap/remap)\n");
}

/* ------------------------------------------------------------------ */
/* 8. Overlap at both granularities, big-page precedence in walk.      */
/* ------------------------------------------------------------------ */
static void test_overlap_both_sizes(void)
{
	struct pg106_mmu *mmu = mk_vmm(PG106_TARGET_VRAM, 0x10000000ULL);
	struct pg106_map m = { .size = 0x1000, .page_shift = 12,
			       .target = PG106_TARGET_VRAM,
			       .kind = PG106_KIND_PITCH };
	struct pg106_pte_query q;

	if (!mmu)
		return;
	map1(mmu, 0x10000, 0x800000, 16, PG106_TARGET_VRAM, PG106_KIND_PITCH);
	m.va = 0x12000; m.pa = 0x900000;	/* inside the 64K page */
	CHECK(pg106_mmu_map(mmu, &m) == -EEXIST, "4K under 64K -> EEXIST");
	m.va = 0x40000;
	CHECK(pg106_mmu_map(mmu, &m) == 0, "4K elsewhere in the same 2MiB");
	m.va = 0x40000; m.size = 0x10000; m.page_shift = 16; m.pa = 0xA00000;
	CHECK(pg106_mmu_map(mmu, &m) == -EEXIST, "64K over 4K -> EEXIST");
	CHECK(pg106_mmu_walk(mmu, 0x12000, &q) && q.big &&
	      q.pa == 0x800000, "64K page still owns its range");
	CHECK(pg106_mmu_walk(mmu, 0x40000, &q) && !q.big &&
	      q.pa == 0x900000, "4K page unaffected by failed 64K map");
	/* A failed multi-page map leaves no partial mapping behind. */
	m.va = 0x20000; m.size = 0x30000; m.pa = 0xB00000; /* 0x20000..0x4ffff */
	CHECK(pg106_mmu_map(mmu, &m) == -EEXIST, "64K run hits the 4K page");
	CHECK(!pg106_mmu_walk(mmu, 0x20000, &q), "partial 64K map rolled back");
	CHECK(!pg106_mmu_walk(mmu, 0x30000, &q), "partial 64K map rolled back (2)");
	pg106_mmu_destroy(mmu);
	printf("PASS: -EEXIST at both granularities, all-or-nothing map\n");
}

/* ------------------------------------------------------------------ */
/* 9. PA decode masks KIND 63:56 / per-aperture address width.         */
/* ------------------------------------------------------------------ */
static void test_pa_decode_limits(void)
{
	struct pg106_mmu *mmu = mk_vmm(PG106_TARGET_VRAM, 0x10000000ULL);
	struct pg106_map m = { .size = 0x1000, .page_shift = 12,
			       .kind = 0xFE };
	struct pg106_pte_query q;

	if (!mmu)
		return;
	/* Highest VRAM page: ADDRESS_VID (35-3):8 is 25 bits of pa >> 12. */
	m.va = 0x1000; m.pa = (1ULL << 37) - 0x1000; m.target = PG106_TARGET_VRAM;
	CHECK(pg106_mmu_map(mmu, &m) == 0, "map top VRAM page, kind 0xFE");
	CHECK(pg106_mmu_walk(mmu, m.va, &q), "walk top VRAM page");
	CHECK_EQ_U64(q.pa, (1ULL << 37) - 0x1000, "VID pa excludes KIND bits");
	CHECK_EQ_U64(q.kind, 0xFE, "kind decoded");
	/* SYS: ADDRESS_SYS 53:8 is 46 bits of pa >> 12 -> pa < 2^58. */
	m.va = 0x2000; m.pa = (1ULL << 58) - 0x1000;
	m.target = PG106_TARGET_SYS_NONCOHERENT;
	CHECK(pg106_mmu_map(mmu, &m) == 0, "map top SYS page (2^58 - 4K)");
	CHECK(pg106_mmu_walk(mmu, m.va, &q), "walk top SYS page");
	CHECK_EQ_U64(q.pa, (1ULL << 58) - 0x1000, "SYS pa excludes KIND bits");
	m.va = 0x3000; m.pa = 1ULL << 58;
	CHECK(pg106_mmu_map(mmu, &m) == -ERANGE, "SYS pa 2^58 -> ERANGE");
	m.target = PG106_TARGET_VRAM; m.pa = 1ULL << 37;
	CHECK(pg106_mmu_map(mmu, &m) == -ERANGE, "VID pa 2^37 -> ERANGE");
	pg106_mmu_destroy(mmu);
	printf("PASS: PTE address decode per aperture, 2^37 / 2^58 limits\n");
}

/* ------------------------------------------------------------------ */
/* 10. Page-table space exhaustion is -ERANGE and rolls back.          */
/* ------------------------------------------------------------------ */
static void test_table_space_exhausted(void)
{
	/* Root at limit - 6 pages leaves exactly five table slots: L2, L1,
	 * dual, and two 64K PTE tables. A 64K run touching three dual
	 * entries (di 0..2) needs a sixth. */
	struct pg106_mmu *mmu = mk_vmm(PG106_TARGET_VRAM,
				       (1ULL << 37) - 0x6000);
	struct pg106_map m = { .va = 0, .pa = 0, .size = 0x410000,
			       .page_shift = 16, .target = PG106_TARGET_VRAM,
			       .kind = PG106_KIND_PITCH };
	struct pg106_pte_query q;
	uint64_t w;

	if (!mmu)
		return;
	CHECK(pg106_mmu_map(mmu, &m) == -ERANGE, "table space exhausted -> ERANGE");
	CHECK(!pg106_mmu_walk(mmu, 0, &q), "no partial map after ERANGE");
	CHECK(pg106_mmu_pde_word(mmu, 0, PG106_PDE_LEVEL3, &w) && w == 0,
	      "root PDE unlinked after rollback");
	m.size = 0x400000;	/* di 0..1: fits the five cached tables */
	CHECK(pg106_mmu_map(mmu, &m) == 0, "smaller map reuses cached tables");
	CHECK(pg106_mmu_walk(mmu, 0x3F0000, &q) && q.pa == 0x3F0000, "walk last page");
	pg106_mmu_destroy(mmu);
	printf("PASS: page-table exhaustion -> -ERANGE, rolled back\n");
}

int main(void)
{
	test_4k_va0_vram();
	test_64k_big_vram();
	test_sysmem_coherent();
	test_unmapped_and_unmap();
	test_deep_walk();
	test_kind_invalid();
	test_refcount_shared_pd();
	test_overlap_both_sizes();
	test_pa_decode_limits();
	test_table_space_exhausted();

	printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "PASSED",
	       checks, failures);
	return failures ? 1 : 0;
}
