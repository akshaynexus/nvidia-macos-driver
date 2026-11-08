// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/src/pg106_device_gate.c — the device-id table and decision
// logic behind pg106_device_gate.h. Plain C11: stdint/stdbool only, no OS
// calls, no libc, so the same file builds into the XNU kext and the Linux
// stub harness (stub/, Makefile). The BOOT_0 decode is pascal/core's
// pg106_pmc_probe (core/src/pmc.c), which the kext target also compiles.

#include "pg106_device_gate.h"

#include <stddef.h> // NULL

#include <pascal/pmc.h>

// Ids and names are from pci.ids (https://pci-ids.ucw.cz/v2.2/pci.ids,
// version 2026.11.06, vendor 10de). The GP106 rows are every named
// GP106M/GP106BM mobile id; 0x1C2D (unnamed GP106M) is left out until a
// real device is seen. The sibling rows are the GP104M/GP107M parts used
// in the same laptop generations: they are listed so the probe log can
// name what it refused, and are never accepted (start() requires a GP106
// BOOT_0, so accepting them at probe would only produce a failed start).
struct pg106_gate_entry {
	uint16_t id;
	const char *chip;
	enum pg106_gate_reason reason;
};

static const struct pg106_gate_entry pg106_gate_table[] = {
	{ 0x1C20, "GP106M [GeForce GTX 1060 Mobile]",        PG106_GATE_OK_GP106 },
	{ 0x1C21, "GP106M [GeForce GTX 1050 Ti Mobile]",     PG106_GATE_OK_GP106 },
	{ 0x1C22, "GP106M [GeForce GTX 1050 Mobile]",        PG106_GATE_OK_GP106 },
	{ 0x1C23, "GP106M [GeForce GTX 1060 Mobile Rev. 2]", PG106_GATE_OK_GP106 },
	{ 0x1C60, "GP106BM [GeForce GTX 1060 Mobile 6GB]",   PG106_GATE_OK_GP106 },
	{ 0x1C61, "GP106BM [GeForce GTX 1050 Ti Mobile]",    PG106_GATE_OK_GP106 },
	{ 0x1C62, "GP106BM [GeForce GTX 1050 Mobile]",       PG106_GATE_OK_GP106 },

	{ 0x1BA1, "GP104M [GeForce GTX 1070 Mobile]",  PG106_GATE_GP10X_UNSUPPORTED },
	{ 0x1BA2, "GP104M [GeForce GTX 1070 Mobile]",  PG106_GATE_GP10X_UNSUPPORTED },
	{ 0x1BE1, "GP104BM [GeForce GTX 1070 Mobile]", PG106_GATE_GP10X_UNSUPPORTED },
	{ 0x1C8C, "GP107M [GeForce GTX 1050 Ti Mobile]", PG106_GATE_GP10X_UNSUPPORTED },
	{ 0x1C8D, "GP107M [GeForce GTX 1050 Mobile]",  PG106_GATE_GP10X_UNSUPPORTED },
	{ 0x1C92, "GP107M [GeForce GTX 1050 Mobile]",  PG106_GATE_GP10X_UNSUPPORTED },
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
	const enum pg106_gate_reason r = e ? e->reason : PG106_GATE_NO_MATCH;

	if (why)
		*why = r;
	return r == PG106_GATE_OK_GP106;
}

const char *
pg106_gate_chip_name(uint16_t device_id)
{
	const struct pg106_gate_entry *e = pg106_gate_find(device_id);
	return e ? e->chip : NULL;
}

struct pg106_start_decision
pg106_start_decision(uint16_t device_id, uint32_t boot0)
{
	struct pg106_start_decision d;
	struct pg106_pmc_probe pmc;
	const bool id_ok = pg106_device_gate(device_id, &d.pci_reason);

	d.accept = false;
	d.boot0_readable = boot0 != 0x00000000u && boot0 != 0xffffffffu;
	d.boot0_gp10x = false;
	d.boot0_gp106 = false;
	d.boot0_architecture = 0;
	d.boot0_revision = 0;

	if (!d.boot0_readable)
		return d;

	pmc = pg106_pmc_probe(boot0);
	if (pmc.status != PG106_PMC_OK || pmc.legacy_nv04)
		return d;

	d.boot0_gp10x = pmc.is_gp10x;
	d.boot0_gp106 = pmc.is_gp106;
	d.boot0_architecture = pmc.architecture;
	d.boot0_revision = pmc.revision;

	// Both halves must agree. A GP107 id reporting a GP106 BOOT_0 (or the
	// reverse) means a strap/VBIOS mismatch or the wrong device, and is
	// exactly what this cross-check is for.
	d.accept = id_ok && d.boot0_gp106;
	return d;
}
