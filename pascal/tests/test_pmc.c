// SPDX-License-Identifier: GPL-2.0-only
// pascal/tests/test_pmc.c — PMC_BOOT_0 decode + BAR0 map unit tests.
//
// Pure CPU: no hardware, no hardware mocks — every vector is either computed
// from nouveau source (cited inline) or round-tripped against the constants
// already validated in the repo's helios300-gp106-simtest.py section 2.
// Prints one "PASS <name>" line per group (simtest style), exits nonzero on
// any failed assertion.

#include <pascal/mmio.h>
#include <pascal/pmc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define REQUIRE(cond)                                                        \
	do {                                                                 \
		if (!(cond)) {                                               \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                          \
		}                                                            \
	} while (0)

static void
ok(const char *name)
{
	printf("PASS %s\n", name);
}

static int
name_is(uint32_t addr, const char *expected)
{
	const char *got = pg106_mmio_block_name(addr);
	return got != NULL && strcmp(got, expected) == 0;
}

// Synthetic GP106 BOOT_0, computed the same way base.c computes a chipset
// override (nvkm_device_ctor: `override_boot0 = ((chipset & 0x1ff) << 20) |
// 0x000000a1` with chipset = 0x136, GP106). Matches real silicon: the nouveau
// mailing list documents a GTX 1060 reporting boot0 = 0x136000a1 (lore,
// "unknown chipset (136000a1) for GTX 1060", Dec 2016 — reference copy at
// /tmp/pascal-ref/nouveau-gtx1060-boot0-136000a1.txt).
#define GP106_BOOT0 0x136000a1u

static void
test_gp106_decode(void)
{
	struct pg106_pmc_probe p = pg106_pmc_probe(GP106_BOOT0);

	REQUIRE(p.status == PG106_PMC_OK);
	REQUIRE(p.architecture == PG106_ARCH_GP106);      // (0x136000a1 & 0x1ff00000) >> 20 = 0x136
	REQUIRE(p.revision == 0xa1u);                     // base.c: chiprev = boot0 & 0x000000ff
	REQUIRE(p.implementation == 0x0u);                // bits [19:16], zero on real GP106
	REQUIRE(p.is_gp10x);                              // 0x136 & 0x1f0 == 0x130 → card_type GP100
	REQUIRE(p.is_gp106);                              // chipset == 0x136 → nv136_chipset "GP106"
	REQUIRE(!p.legacy_nv04);
	ok("PMC probe: synthetic GP106 boot0 0x136000a1 -> arch 0x136 rev a1 GP10x");
}

static void
test_gp10x_family(void)
{
	// Whole GP10x family shares card_type GP100 (base.c `case 0x130:
	// device->card_type = GP100`), while each variant has its own chipset
	// entry: 0x130 GP100, 0x132 GP102, 0x134 GP104, 0x136 GP106, 0x137
	// GP107, 0x138 GP108, 0x13b GP10B (base.c chipset switch). Revision
	// byte 0xa1 per the base.c override formula.
	static const uint32_t boot0s[] = {
		0x130000a1u, 0x132000a1u, 0x134000a1u,
		0x137000a1u, 0x138000a1u, 0x13b000a1u,
	};
	for (unsigned i = 0; i < sizeof(boot0s) / sizeof(boot0s[0]); i++) {
		struct pg106_pmc_probe p = pg106_pmc_probe(boot0s[i]);
		REQUIRE(p.status == PG106_PMC_OK);
		REQUIRE(p.is_gp10x);
		REQUIRE(!p.is_gp106);
		REQUIRE((p.architecture & 0x1f0u) == PG106_CARD_TYPE_GP10X_FAMILY);
	}

	// Non-GP10x modern chips decode fine but must not claim GP10x:
	// 0x120 = GM10x family (base.c case 0x110/0x120 → GM100), 0x160 =
	// TU100 family, 0x0c0 = NV_C0 (Kepler).
	static const uint32_t others[] = { 0x120000a1u, 0x160000a1u, 0x0c0000a1u };
	for (unsigned i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
		struct pg106_pmc_probe p = pg106_pmc_probe(others[i]);
		REQUIRE(p.status == PG106_PMC_OK);
		REQUIRE(!p.is_gp10x);
		REQUIRE(!p.is_gp106);
	}
	ok("PMC probe: GP10x family members detected, non-GP10x rejected");
}

static void
test_rejects_and_legacy(void)
{
	// Zero BOOT_0 matches neither decode path (modern test needs bits
	// [28:24] set; legacy compare needs the 0x20004000 pattern) — this is
	// base.c's "unknown chipset (%08x)" case.
	struct pg106_pmc_probe zero = pg106_pmc_probe(0x00000000u);
	REQUIRE(zero.status == PG106_PMC_UNKNOWN);
	REQUIRE(!zero.is_gp10x && !zero.is_gp106 && !zero.legacy_nv04);

	// Bits [28:20] zero, legacy pattern absent: 0x00ff0000 & 0x1f000000
	// == 0 and & 0xff00fff0 == 0x00f00000 != 0x20004000.
	struct pg106_pmc_probe junk = pg106_pmc_probe(0x00ff0000u);
	REQUIRE(junk.status == PG106_PMC_UNKNOWN);

	// Legacy NV4/NV5 path (base.c: (boot0 & 0xff00fff0) == 0x20004000;
	// chipset 5 iff boot0 & 0x00f00000, bits [23:20]). The two vectors are
	// base.c's own chipset-override constants: 0x20004000 for chipset 0x04
	// and 0x20104000 for any other pre-NV10 chipset — 0x20104000 has bit 20
	// set, so it decodes as NV5; 0x20004000 has bits [23:20] clear → NV4.
	struct pg106_pmc_probe nv5 = pg106_pmc_probe(0x20104000u);
	REQUIRE(nv5.status == PG106_PMC_OK);
	REQUIRE(nv5.legacy_nv04);
	REQUIRE(nv5.architecture == 0x05u);
	REQUIRE(!nv5.is_gp10x && !nv5.is_gp106);
	struct pg106_pmc_probe nv4 = pg106_pmc_probe(0x20004000u);
	REQUIRE(nv4.status == PG106_PMC_OK);
	REQUIRE(nv4.legacy_nv04);
	REQUIRE(nv4.architecture == 0x04u);
	ok("PMC probe: unknown boot0s rejected, NV4/NV5 legacy path decoded");
}

static void
test_register_constants(void)
{
	// PMC registers sit at the start of the PMC block (base.c:
	// nvkm_rd32(device, 0x000000) for BOOT_0, 0x000004 for BOOT_1 in
	// nvkm_device_endianness).
	REQUIRE(PG106_PMC_BOOT_0 == pg106_mmio_map[PG106_MMIO_PMC].base);
	REQUIRE(PG106_PMC_BOOT_1 == 0x00000004u);
	// Endianness pattern (base.c nvkm_device_endianness).
	REQUIRE(PG106_PMC_BOOT_1_LE == 0x00000000u);
	REQUIRE(PG106_PMC_BOOT_1_BE == 0x01000001u);
	// Decode masks, straight from nvkm_device_ctor.
	REQUIRE(PG106_BOOT0_CHIPSET_MASK == 0x1ff00000u);
	REQUIRE(PG106_BOOT0_CHIPSET_SHIFT == 20);
	REQUIRE(PG106_BOOT0_MODERN_TEST_MASK == 0x1f000000u);
	REQUIRE(PG106_BOOT0_CHIPREV_MASK == 0x000000ffu);
	REQUIRE(PG106_BOOT0_LEGACY_MASK == 0xff00fff0u);
	REQUIRE(PG106_BOOT0_LEGACY_VALUE == 0x20004000u);
	REQUIRE(PG106_BOOT0_LEGACY_NV5 == 0x00f00000u);
	REQUIRE(PG106_CARD_TYPE_GP10X_FAMILY == 0x130u);
	REQUIRE(PG106_ARCH_GP106 == 0x136u);
	ok("PMC constants round-trip against nouveau base.c values");
}

static void
test_mmio_ranges(void)
{
	// Round-trip every range against the literals in
	// helios300-gp106-simtest.py section 2 (SDK swref headers + nouveau;
	// see research/evidence/MMIO-MAP-CORRECTIONS.md). If either side
	// drifts, these comparisons break.
	static const struct pg106_mmio_range simtest[PG106_MMIO_BLOCK_COUNT] = {
		[PG106_MMIO_PMC]   = { "PMC",   0x000000u, 0x000fffu },
		[PG106_MMIO_PBUS]  = { "PBUS",  0x001000u, 0x001fffu },
		[PG106_MMIO_PFIFO] = { "PFIFO", 0x002000u, 0x003fffu },
		[PG106_MMIO_PTIMER]= { "PTIMER",0x009000u, 0x009fffu },
		[PG106_MMIO_PTOP]  = { "PTOP",  0x022000u, 0x022fffu },
		[PG106_MMIO_PFB]   = { "PFB",   0x100000u, 0x1fffffu },
		[PG106_MMIO_PPMU]  = { "PPMU",  0x10a000u, 0x10afffu },
		[PG106_MMIO_PGRAPH]= { "PGRAPH",0x400000u, 0x41ffffu },
		[PG106_MMIO_PDISP] = { "PDISP", 0x610000u, 0x61ffffu },
		[PG106_MMIO_PEVO]  = { "EVO",   0x640000u, 0x64ffffu },
	};

	for (unsigned i = 0; i < PG106_MMIO_BLOCK_COUNT; i++) {
		const struct pg106_mmio_range *m = &pg106_mmio_map[i];
		const struct pg106_mmio_range *s = &simtest[i];
		REQUIRE(m->base == s->base);
		REQUIRE(m->end == s->end);
		REQUIRE(pg106_mmio_contains((enum pg106_mmio_block)i, m->base));
		REQUIRE(pg106_mmio_contains((enum pg106_mmio_block)i, m->end));
		REQUIRE(!pg106_mmio_contains((enum pg106_mmio_block)i, m->base - 1));
		REQUIRE(!pg106_mmio_contains((enum pg106_mmio_block)i, m->end + 1));
		REQUIRE(pg106_mmio_block_at(m->base) == (enum pg106_mmio_block)i);
		REQUIRE(pg106_mmio_block_at(m->end) == (enum pg106_mmio_block)i);
	}
	// simtest's own sanity anchors.
	REQUIRE(pg106_mmio_map[PG106_MMIO_PMC].base == 0x0u);       // BOOT_0 at +0x0
	REQUIRE(pg106_mmio_map[PG106_MMIO_PTIMER].base == 0x9000u); // moved off 0x020000
	REQUIRE(pg106_mmio_map[PG106_MMIO_PTOP].base == 0x022000u); // 0x02240c latch page
	REQUIRE(pg106_mmio_map[PG106_MMIO_PEVO].base == 0x640000u); // EVO window (F1)
	REQUIRE(pg106_mmio_map[PG106_MMIO_PDISP].base == 0x610000u);
	REQUIRE(pg106_mmio_map[PG106_MMIO_PGRAPH].end > pg106_mmio_map[PG106_MMIO_PPMU].end);
	ok("MMIO map: 10 blocks match simtest base/end literals, bounds exact");
}

static void
test_mmio_names(void)
{
	REQUIRE(name_is(0x000000u, "PMC"));
	REQUIRE(name_is(0x002000u, "PFIFO"));
	REQUIRE(name_is(0x009400u, "PTIMER"));  // TIME_0 (gm107/dev_timer.h:26)
	REQUIRE(name_is(0x02240cu, "PTOP"));    // devinit-ran latch
	REQUIRE(name_is(0x10a000u, "PPMU"));
	REQUIRE(name_is(0x400000u, "PGRAPH"));
	REQUIRE(name_is(0x610000u, "PDISP"));
	REQUIRE(name_is(0x640414u, "EVO"));     // gf119.c:429 head readback
	// Inclusive upper bounds land in the same block...
	REQUIRE(name_is(0x000fffu, "PMC"));
	REQUIRE(name_is(0x41ffffu, "PGRAPH"));
	// ...one past them lands in the next block or a gap.
	REQUIRE(name_is(0x001000u, "PBUS"));
	REQUIRE(pg106_mmio_block_name(0x021000u) == NULL);  // PFIFO..PTOP gap
	REQUIRE(pg106_mmio_block_name(0x023000u) == NULL);  // past PTOP
	REQUIRE(pg106_mmio_block_name(0x062000u) == NULL);  // old PCOPY page: unmapped
	REQUIRE(pg106_mmio_block_name(0x420000u) == NULL);  // past PGRAPH
	REQUIRE(pg106_mmio_block_name(0x650000u) == NULL);  // past EVO (NV_PDISP tail)
	REQUIRE(pg106_mmio_block_name(0xffffffffu) == NULL);
	ok("MMIO lookup: block names and boundary addresses resolve correctly");
}

int
main(void)
{
	test_gp106_decode();
	test_gp10x_family();
	test_rejects_and_legacy();
	test_register_constants();
	test_mmio_ranges();
	test_mmio_names();

	if (failures) {
		printf("%d check(s) FAILED\n", failures);
		return EXIT_FAILURE;
	}
	printf("all checks passed\n");
	return EXIT_SUCCESS;
}
