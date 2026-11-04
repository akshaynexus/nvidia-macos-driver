// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/stub/test_core_shim.c — proves the pascal/core sources compile
// and run through the KEXT LIBC SHIM (src/pg106_klibc_shim.h +
// src/klibc/*.h) instead of a normal libc include set. This is the Linux-side
// stand-in for the kext build's header environment: -DPG106_KLIBC_KERNEL_SIM
// turns the shim on, -include puts it ahead of all sources, -Isrc/klibc
// shadows <stdlib.h>/<string.h>/<errno.h> exactly like the xcodeproj does.
//
// What that proves (HANDOFF-PASCAL.md §6 Phase 3 gate): the portable core's
// entire libc surface is covered by the shim — mmu.c's calloc/free
// (mmu.c:222-227) map onto the shim allocator, string/errno fall through the
// klibc headers — so the kext build needs nothing else.
//
// Compile flags come from the Makefile; this file itself stays plain.

#include <pascal/mmu.h>
#include <pascal/pmc.h>

#include <stdio.h>

static int failures = 0;

#define REQUIRE(cond)                                                        \
	do {                                                                 \
		if (!(cond)) {                                               \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                          \
		}                                                            \
	} while (0)

static void ok(const char *name) { printf("PASS %s\n", name); }

int
main(void)
{
	// PMC decode through the shim build (the same object the kext links).
	struct pg106_pmc_probe p = pg106_pmc_probe((0x136u << 20) | 0xa1u);
	REQUIRE(p.status == PG106_PMC_OK);
	REQUIRE(p.is_gp106);
	REQUIRE(p.architecture == 0x136);
	REQUIRE(p.revision == 0xa1);
	ok("core under klibc shim: pmc decode");

	// MMU build under the shim — this is the path that exercises
	// calloc/free inside core/src/mmu.c (alloc_zero, mmu.c:222-227).
	struct pg106_mmu *mmu = NULL;
	const struct pg106_mmu_cfg cfg = {
		.table_target = PG106_TARGET_SYS_COHERENT,
		.table_base = 0x1000,
	};
	REQUIRE(pg106_mmu_init(&mmu, &cfg) == 0);
	REQUIRE(mmu != NULL);

	struct pg106_map m = {
		.va = 0x1000000,
		.pa = 0x2000000,
		.size = 0x40000,
		.page_shift = PG106_PAGE_4K_SHIFT,
		.target = PG106_TARGET_SYS_COHERENT,
		.kind = PG106_KIND_PITCH,
		.vol = false,
		.ro = false,
		.priv = false,
	};
	REQUIRE(pg106_mmu_map(mmu, &m) == 0);

	struct pg106_pte_query q;
	REQUIRE(pg106_mmu_walk(mmu, 0x1000000, &q));
	REQUIRE(q.mapped && !q.big);
	REQUIRE(q.pa == 0x2000000);

	REQUIRE(pg106_mmu_unmap(mmu, 0x1000000, 0x40000,
				PG106_PAGE_4K_SHIFT) == 0);
	REQUIRE(pg106_mmu_walk(mmu, 0x1000000, &q) == false);
	REQUIRE(pg106_mmu_walk(mmu, 0x1000000, &q) == false);

	// Destroy path frees through the shim's pg106_kfree.
	pg106_mmu_destroy(mmu);
	ok("core under klibc shim: mmu map/walk/unmap/destroy (calloc+free)");

	if (failures) {
		printf("%d failure(s)\n", failures);
		return 1;
	}
	printf("PASS test_core_shim\n");
	return 0;
}
