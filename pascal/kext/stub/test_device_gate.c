// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/stub/test_device_gate.c — Linux-side tests for the kext's pure
// core logic (src/pg106_device_gate.c): the GP10x PCI-id gate and the
// combined probe decision. Style follows pascal/tests/test_pmc.c (PASS lines
// per group, nonzero exit on failure).
//
// No hardware, no mocks beyond constants: every id comes from
// HELIOS300-PASCAL-RESEARCH.md §1, every BOOT_0 vector from the cited decode
// in pascal/core/include/pascal/pmc.h.

#include "pg106_device_gate.h"

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

// Synthetic GP106 BOOT_0: architecture 0x136 in bits [28:20], rev 0xa1 —
// the same construction tests/test_pmc.c uses for the nouveau-cited decode.
#define GP106_BOOT0 ((0x136u << 20) | 0xa1u)
// A GP104-family BOOT_0 (arch 0x134, GP104 — research doc §3.1 table).
#define GP104_BOOT0 ((0x134u << 20) | 0xa1u)

static void
test_gp106_gate(void)
{
	enum pg106_gate_reason why = PG106_GATE_NO_MATCH;

	// The primary target: 1C20/1C23/1C60 GP106M (research doc §1).
	REQUIRE(pg106_device_gate(0x1C20, &why));
	REQUIRE(why == PG106_GATE_OK_GP106);
	REQUIRE(pg106_device_gate(0x1C23, NULL));
	REQUIRE(pg106_device_gate(0x1C60, &why));
	REQUIRE(why == PG106_GATE_OK_GP106);
	// 1050 Ti on the GP106 die — same chipset.
	REQUIRE(pg106_device_gate(0x1C21, &why));
	REQUIRE(why == PG106_GATE_OK_GP106);
	REQUIRE(pg106_device_gate(0x1C61, &why));
	REQUIRE(why == PG106_GATE_OK_GP106);

	// Sibling GP10x parts accepted by the table.
	REQUIRE(pg106_device_gate(0x1C8C, &why));
	REQUIRE(why == PG106_GATE_OK_GP10X_OTHER);
	REQUIRE(pg106_device_gate(0x1BA1, &why));
	REQUIRE(why == PG106_GATE_OK_GP10X_OTHER);
	REQUIRE(pg106_device_gate(0x1C8D, NULL));
	REQUIRE(pg106_device_gate(0x1C92, NULL));
	REQUIRE(pg106_device_gate(0x1BA2, NULL));
	REQUIRE(pg106_device_gate(0x1BE1, NULL));

	// Names come from the same table.
	REQUIRE(pg106_gate_chip_name(0x1C20) != NULL);
	ok("pg106_device_gate: GP106M + GP10x table");

	// Turing Helios (2182, research doc §1) and a few strays must not pass.
	REQUIRE(!pg106_device_gate(0x2182, &why));
	REQUIRE(why == PG106_GATE_NO_MATCH);
	REQUIRE(!pg106_device_gate(0x1F08, NULL)); // RTX 2060 (TU106)
	REQUIRE(!pg106_device_gate(0x2484, NULL)); // RTX 3060 (GA104)
	REQUIRE(!pg106_device_gate(0x0000, NULL));
	REQUIRE(!pg106_device_gate(0xFFFF, NULL));
	REQUIRE(pg106_gate_chip_name(0x2182) == NULL);
	ok("pg106_device_gate: rejects non-Pascal ids");
}

static void
test_probe_decision(void)
{
	// GP106 id + GP106 BOOT_0: accept.
	struct pg106_probe_decision d = pg106_probe_decision(0x1C20, GP106_BOOT0);
	REQUIRE(d.accept);
	REQUIRE(d.pci_reason == PG106_GATE_OK_GP106);
	REQUIRE(d.boot0_gp106);
	REQUIRE(d.boot0_gp10x);
	REQUIRE(d.boot0_architecture == 0x136);
	REQUIRE(d.boot0_revision == 0xa1);
	ok("pg106_probe_decision: GP106 id + GP106 BOOT_0");

	// A GP104 id (accepted GP10x) must NOT be claimed as GP106: the
	// skeleton starts GP106 only.
	d = pg106_probe_decision(0x1BA1, GP104_BOOT0);
	REQUIRE(!d.accept);
	REQUIRE(d.pci_reason == PG106_GATE_OK_GP10X_OTHER);
	REQUIRE(d.boot0_gp10x);
	REQUIRE(!d.boot0_gp106);
	ok("pg106_probe_decision: other GP10x identified but not claimed");

	// PCI id vs BOOT_0 disagreement: reject (the cross-check's purpose).
	d = pg106_probe_decision(0x1C20, GP104_BOOT0);
	REQUIRE(!d.accept);
	d = pg106_probe_decision(0x1BA1, GP106_BOOT0);
	REQUIRE(!d.accept);
	ok("pg106_probe_decision: id/BOOT_0 mismatch rejected");

	// boot0 == 0: BAR0 not readable yet (probe before start) — the PCI id
	// gate alone decides.
	d = pg106_probe_decision(0x1C60, 0);
	REQUIRE(d.accept);
	REQUIRE(!d.boot0_gp106);
	REQUIRE(d.boot0_architecture == 0);
	// Unknown id stays rejected even with a GP106 BOOT_0.
	d = pg106_probe_decision(0x2182, GP106_BOOT0);
	REQUIRE(!d.accept);
	REQUIRE(d.pci_reason == PG106_GATE_NO_MATCH);
	ok("pg106_probe_decision: boot0=0 and unknown-id paths");
}

int
main(void)
{
	test_gp106_gate();
	test_probe_decision();
	if (failures) {
		printf("%d failure(s)\n", failures);
		return 1;
	}
	printf("PASS test_device_gate\n");
	return 0;
}
