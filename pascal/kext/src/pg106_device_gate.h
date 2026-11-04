// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/src/pg106_device_gate.h — PCI device-id gate + probe decision
// for the GP106 kext personality.
//
// Why this exists (HANDOFF-PASCAL.md §6 Phase 3): the kext personality in
// Info.plist matches *any* NVIDIA display/3D device on purpose (the wide-net
// match pattern of kexts/NVRM/Info.plist, mirrored in this tree). The real
// gate is software, in probe(): only the Pascal Helios 300 chips are accepted.
// That decision logic must be plain C11 so it can be built and tested both in
// the kext (macOS) and in the Linux stub harness (stub/).
//
// Device-id facts: HELIOS300-PASCAL-RESEARCH.md §1 (the 2017–2018 Helios 300
// Pascal table, cross-checked against NVIDIA's 390.12 supported-chips list).

#ifndef PG106_DEVICE_GATE_H
#define PG106_DEVICE_GATE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Outcome of the PCI-id gate. Distinguishing GP106 from the other accepted
// GP10x chips matters for later phases (engine wiring differs per chipset;
// HELIOS300-PASCAL-RESEARCH.md §3.1).
enum pg106_gate_reason {
	PG106_GATE_OK_GP106 = 0,   // 1060 Mobile silicon: the primary target
	PG106_GATE_OK_GP10X_OTHER, // another accepted Pascal Helios chip
	PG106_GATE_NO_MATCH,       // not a supported Pascal Helios 300 device
};

// Gate: true iff device_id is in the supported GP10x table. why may be NULL.
// Pure function — no hardware, no OS calls.
bool pg106_device_gate(uint16_t device_id, enum pg106_gate_reason *why);

// Chip name from the table ("GP106M GTX 1060 Mobile", ...) or NULL if the id
// is not in the table.
const char *pg106_gate_chip_name(uint16_t device_id);

// Combined probe decision: PCI-id gate AND PMC_BOOT_0 identity check. The
// PMC half is the pascal/core decoder (pg106_pmc_probe, include/pascal/pmc.h)
// applied to the first BAR0 word; on real silicon boot0 == 0x136000a1 for
// GP106 (nouveau ML report, cited in the pmc.h header).
struct pg106_probe_decision {
	bool accept;                    // gate AND boot0 agree: bring the device up
	enum pg106_gate_reason pci_reason;
	bool boot0_gp10x;               // (boot0 arch & 0x1f0) == 0x130 family
	bool boot0_gp106;               // boot0 architecture == 0x136
	uint32_t boot0_architecture;    // raw decode, 0 when BOOT_0 was unreadable
	uint32_t boot0_revision;        // raw decode, 0 when BOOT_0 was unreadable
};

// Combined decision from a PCI device id and a raw PMC_BOOT_0 word. Pure
// function of its inputs; the IOKit glue (src/PG106.cpp) calls this after
// mapping BAR0. Passing boot0 == 0 means "BAR0 not readable yet": the
// decision then rests on the PCI id alone (probe-time state), and
// boot0_gp10x/boot0_gp106 come out false.
struct pg106_probe_decision pg106_probe_decision(uint16_t device_id,
						 uint32_t boot0);

#ifdef __cplusplus
}
#endif

#endif // PG106_DEVICE_GATE_H
