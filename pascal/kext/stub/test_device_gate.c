// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/stub/test_device_gate.c — Linux-side tests for the kext's pure
// core logic (src/pg106_device_gate.c): the probe-time PCI-id gate and the
// start-time BOOT_0 decision. Style follows pascal/tests/test_pmc.c (PASS lines
// per group, nonzero exit on failure).
//
// No hardware, no mocks beyond constants: ids and names are pci.ids
// (2026.11.06, cited in pg106_device_gate.c), BOOT_0 vectors follow the
// cited decode in pascal/core/include/pascal/pmc.h.

#include "pg106_device_gate.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define REQUIRE(cond)                                                        \
	do {                                                                 \
		if (!(cond)) {                                               \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                          \
		}                                                            \
	} while (0)

static void ok(const char *name) { printf("PASS %s\n", name); }

// Synthetic GP106 BOOT_0: architecture 0x136 in bits [28:20], rev 0xa1 —
// the same construction tests/test_pmc.c uses for the nouveau-cited decode.
#define GP106_BOOT0 ((0x136u << 20) | 0xa1u)
// A GP104-family BOOT_0 (arch 0x134, GP104 — research doc §3.1 table).
#define GP104_BOOT0 ((0x134u << 20) | 0xa1u)

static void
test_gp106_gate(void)
{
	enum pg106_gate_reason why = PG106_GATE_NO_MATCH;
	// Every named GP106M/GP106BM mobile id in pci.ids 2026.11.06.
	static const uint16_t gp106[] = { 0x1C20, 0x1C21, 0x1C22, 0x1C23,
					  0x1C60, 0x1C61, 0x1C62 };

	for (unsigned i = 0; i < sizeof gp106 / sizeof gp106[0]; i++) {
		why = PG106_GATE_NO_MATCH;
		REQUIRE(pg106_device_gate(gp106[i], &why));
		REQUIRE(why == PG106_GATE_OK_GP106);
		REQUIRE(pg106_gate_chip_name(gp106[i]) != NULL);
	}
	REQUIRE(pg106_device_gate(0x1C20, NULL));
	ok("pg106_device_gate: all GP106 mobile ids accepted");

	// Known Pascal siblings: named, but refused at probe because start()
	// could never accept their BOOT_0.
	static const uint16_t siblings[] = { 0x1BA1, 0x1BA2, 0x1BE1,
					     0x1C8C, 0x1C8D, 0x1C92 };
	for (unsigned i = 0; i < sizeof siblings / sizeof siblings[0]; i++) {
		why = PG106_GATE_OK_GP106;
		REQUIRE(!pg106_device_gate(siblings[i], &why));
		REQUIRE(why == PG106_GATE_GP10X_UNSUPPORTED);
		REQUIRE(pg106_gate_chip_name(siblings[i]) != NULL);
	}
	// 0x1C92 is GP107M per pci.ids (was mislabelled GP108M).
	REQUIRE(strncmp(pg106_gate_chip_name(0x1C92), "GP107M", 6) == 0);
	ok("pg106_device_gate: GP104/GP107 siblings named but refused");

	// Turing Helios (2182), other generations and strays.
	REQUIRE(!pg106_device_gate(0x2182, &why));
	REQUIRE(why == PG106_GATE_NO_MATCH);
	REQUIRE(!pg106_device_gate(0x1F08, NULL)); // RTX 2060 (TU106)
	REQUIRE(!pg106_device_gate(0x2484, NULL)); // RTX 3060 (GA104)
	REQUIRE(!pg106_device_gate(0x1C03, NULL)); // desktop GP106: not a target
	REQUIRE(!pg106_device_gate(0x0000, NULL));
	REQUIRE(!pg106_device_gate(0xFFFF, NULL));
	REQUIRE(pg106_gate_chip_name(0x2182) == NULL);
	ok("pg106_device_gate: rejects non-target ids");
}

static void
test_start_decision(void)
{
	// GP106 id + GP106 BOOT_0: accept.
	struct pg106_start_decision d = pg106_start_decision(0x1C20, GP106_BOOT0);
	REQUIRE(d.accept);
	REQUIRE(d.boot0_readable);
	REQUIRE(d.pci_reason == PG106_GATE_OK_GP106);
	REQUIRE(d.boot0_gp106);
	REQUIRE(d.boot0_gp10x);
	REQUIRE(d.boot0_architecture == 0x136);
	REQUIRE(d.boot0_revision == 0xa1);
	d = pg106_start_decision(0x1C62, GP106_BOOT0);
	REQUIRE(d.accept);
	ok("pg106_start_decision: GP106 id + GP106 BOOT_0");

	// Unreadable BAR: all-zero and all-ones reads are refused, never
	// treated as "not read yet".
	d = pg106_start_decision(0x1C20, 0x00000000u);
	REQUIRE(!d.accept);
	REQUIRE(!d.boot0_readable);
	REQUIRE(d.boot0_architecture == 0);
	d = pg106_start_decision(0x1C20, 0xffffffffu);
	REQUIRE(!d.accept);
	REQUIRE(!d.boot0_readable);
	REQUIRE(!d.boot0_gp106);
	ok("pg106_start_decision: 0 and ~0 BOOT_0 rejected");

	// PCI id vs BOOT_0 disagreement: reject (the cross-check's purpose).
	d = pg106_start_decision(0x1C20, GP104_BOOT0);
	REQUIRE(!d.accept);
	REQUIRE(d.boot0_readable && d.boot0_gp10x && !d.boot0_gp106);
	d = pg106_start_decision(0x1BA1, GP106_BOOT0);
	REQUIRE(!d.accept);
	REQUIRE(d.pci_reason == PG106_GATE_GP10X_UNSUPPORTED);
	d = pg106_start_decision(0x2182, GP106_BOOT0);
	REQUIRE(!d.accept);
	REQUIRE(d.pci_reason == PG106_GATE_NO_MATCH);
	ok("pg106_start_decision: id/BOOT_0 mismatch rejected");
}

int
main(void)
{
	test_gp106_gate();
	test_start_decision();
	if (failures) {
		printf("%d failure(s)\n", failures);
		return 1;
	}
	printf("PASS test_device_gate\n");
	return 0;
}
