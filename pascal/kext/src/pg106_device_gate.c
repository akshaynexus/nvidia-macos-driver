// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/src/pg106_device_gate.c — the device-id table and decision
// logic behind pg106_device_gate.h. Plain C11: stdint/stdbool only, no OS
// calls, no libc — so the same object file serves the XNU kext build and the
// Linux stub harness (stub/, Makefile).
//
// The PCI-id half is compiled into the kext object too, but the PMC half
// comes from the portable core (pascal/core/src/pmc.c), which the kext build
// compiles as well (PG106.xcodeproj sources phase).

#include "pg106_device_gate.h"

#include <stddef.h> // NULL

#include <pascal/pmc.h>

// Supported PCI device ids, from HELIOS300-PASCAL-RESEARCH.md §1 (2017–2018
// Predator Helios 300 line, cross-checked against NVIDIA's 390.12
// supported-chips list: https://download.nvidia.com/XFree86/Linux-x86/390.12/
// README/supportedchips.html). This is the whole list the Phase 3 skeleton
// accepts — anything else must fall through PG106_GATE_NO_MATCH.
struct pg106_gate_entry {
	uint16_t id;
	const char *chip;
	enum pg106_gate_reason reason;
};

static const struct pg106_gate_entry pg106_gate_table[] = {
	// GP106M — GTX 1060 Mobile: the primary target (HANDOFF-PASCAL.md §1).
	{ 0x1C20, "GP106M GTX 1060 Mobile", PG106_GATE_OK_GP106 },
	{ 0x1C23, "GP106M GTX 1060 Mobile", PG106_GATE_OK_GP106 },
	{ 0x1C60, "GP106M GTX 1060 Mobile", PG106_GATE_OK_GP106 },
	// 1050 Ti on the GP106 die — same chipset 0x136, weaker parts bin
	// (research doc §1: "1C21/1C61 (1050 Ti on GP106M)").
	{ 0x1C21, "GP106M GTX 1050 Ti Mobile", PG106_GATE_OK_GP106 },
	{ 0x1C61, "GP106M GTX 1050 Ti Mobile", PG106_GATE_OK_GP106 },
	// GP107M / GP104M siblings from the same Helios generations.
	{ 0x1C8C, "GP107M GTX 1050 Ti Mobile", PG106_GATE_OK_GP10X_OTHER },
	{ 0x1C8D, "GP107M GTX 1050 Mobile",    PG106_GATE_OK_GP10X_OTHER },
	{ 0x1C92, "GP108M GTX 1050 Mobile",    PG106_GATE_OK_GP10X_OTHER },
	{ 0x1BA1, "GP104M GTX 1070 Mobile",    PG106_GATE_OK_GP10X_OTHER },
	{ 0x1BA2, "GP104M GTX 1070 Mobile",    PG106_GATE_OK_GP10X_OTHER },
	{ 0x1BE1, "GP104M GTX 1070 Mobile",    PG106_GATE_OK_GP10X_OTHER },
};

#define PG106_GATE_TABLE_LEN \
	(sizeof pg106_gate_table / sizeof pg106_gate_table[0])

static const struct pg106_gate_entry *
pg106_gate_find(uint16_t device_id)
{
	for (unsigned i = 0; i < PG106_GATE_TABLE_LEN; i++) {
		if (pg106_gate_table[i].id == device_id)
			return &pg106_gate_table[i];
	}
	return NULL;
}

bool
pg106_device_gate(uint16_t device_id, enum pg106_gate_reason *why)
{
	const struct pg106_gate_entry *e = pg106_gate_find(device_id);
	if (why)
		*why = e ? e->reason : PG106_GATE_NO_MATCH;
	return e != NULL;
}

const char *
pg106_gate_chip_name(uint16_t device_id)
{
	const struct pg106_gate_entry *e = pg106_gate_find(device_id);
	return e ? e->chip : NULL;
}

struct pg106_probe_decision
pg106_probe_decision(uint16_t device_id, uint32_t boot0)
{
	struct pg106_probe_decision d;
	struct pg106_pmc_probe pmc;

	d.pci_reason = PG106_GATE_NO_MATCH;
	d.boot0_gp10x = false;
	d.boot0_gp106 = false;
	d.boot0_architecture = 0;
	d.boot0_revision = 0;
	d.accept = false;

	if (!pg106_device_gate(device_id, &d.pci_reason))
		return d;

	pmc = pg106_pmc_probe(boot0);
	d.boot0_gp10x = pmc.is_gp10x;
	d.boot0_gp106 = pmc.is_gp106;
	// Only the modern decode produces a meaningful architecture word; on
	// the legacy NV4/NV5 path (impossible for a PCI id we already gated)
	// keep the fields at 0 rather than echoing 0x04/0x05.
	if (pmc.status == PG106_PMC_OK && !pmc.legacy_nv04) {
		d.boot0_architecture = pmc.architecture;
		d.boot0_revision = pmc.revision;
	}

	// boot0 == 0: BAR0 not readable (probe before start). The PCI gate
	// alone decides; BOOT_0 identity is re-checked at start time.
	if (boot0 == 0) {
		d.accept = true;
		return d;
	}

	// Full check: PCI id says GP106-family AND BOOT_0 says GP106. A card
	// with a mismatched strap/BIOS (GP107 id reporting a GP106 BOOT_0)
	// is exactly the case this cross-check exists for.
	d.accept = d.pci_reason == PG106_GATE_OK_GP106 && d.boot0_gp106;
	return d;
}
