// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/stub/test_core_shim.c — proves the pascal/core sources compile
// and run through the kext's libc layer instead of a normal libc: src/klibc/
// first on the include path (so core's <stdlib.h>/<errno.h> are the kext's),
// -DPG106_KLIBC_KERNEL_SIM, and src/pg106_klibc.c linked in on the host
// allocator. The mmu path exercises calloc/free from core/src/mmu.c; the
// direct tests below exercise the allocator's size header, overflow and
// realloc rules that the kext build relies on (IOFree needs the size).
//
// Compile flags come from the Makefile; this file itself stays plain.

#include <pascal/mmu.h>
#include <pascal/pmc.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pg106_klibc.h"

static int failures = 0;

#define REQUIRE(cond)                                                        \
	do {                                                                 \
		if (!(cond)) {                                               \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                          \
		}                                                            \
	} while (0)

static void ok(const char *name) { printf("PASS %s\n", name); }

static void
test_allocator(void)
{
	// Core's calloc/free resolve to the kext allocator symbols.
	unsigned char *p = calloc(3, 5);
	REQUIRE(p != NULL);
	for (int i = 0; i < 15; i++)
		REQUIRE(p[i] == 0);
	memset(p, 0xab, 15);

	// realloc grows and keeps the old contents; shrink keeps the prefix.
	p = realloc(p, 4096);
	REQUIRE(p != NULL);
	for (int i = 0; i < 15; i++)
		REQUIRE(p[i] == 0xab);
	p = realloc(p, 4);
	REQUIRE(p != NULL && p[0] == 0xab && p[3] == 0xab);
	free(p);

	// realloc(NULL, n) allocates; free(NULL) is a no-op.
	p = realloc(NULL, 32);
	REQUIRE(p != NULL);
	free(p);
	free(NULL);

	// n * size overflow and header overflow are refused, not wrapped.
	REQUIRE(calloc(SIZE_MAX / 2, 4) == NULL);
	REQUIRE(pg106_kcalloc(1, SIZE_MAX - 4) == NULL);
	REQUIRE(pg106_krealloc(NULL, SIZE_MAX) == NULL);

	// Returned pointers keep 16-byte alignment of the backend.
	void *q = calloc(1, 1);
	REQUIRE(q != NULL && ((uintptr_t)q & 15u) == 0);
	free(q);
	ok("klibc allocator: calloc/realloc/free, overflow, alignment");
}

int
main(void)
{
	test_allocator();

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
