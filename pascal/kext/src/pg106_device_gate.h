// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/src/pg106_device_gate.h — PCI device-id gate (probe time) and
// BOOT_0 identity check (start time) for the GP106 kext personality.
//
// Why this exists (HANDOFF-PASCAL.md §6 Phase 3): the kext personality in
// Info.plist matches *any* NVIDIA display-class device on purpose (the
// wide-net pattern of kexts/NVRM/Info.plist). The real gate is software:
// probe() accepts only GP106-die ids, and start() accepts only a readable
// BOOT_0 that decodes as GP106. The logic is plain C11 so it is built and
// tested both in the kext and in the Linux stub harness (stub/).
//
// Two separate functions because the inputs exist at different times: at
// probe() BAR0 is not mapped, so only the PCI id is known; at start() the
// BOOT_0 word has been read and must be valid. An earlier version used one
// function with boot0 == 0 meaning "not read yet", which also accepted an
// all-zero read from a dead BAR at start time.

#ifndef PG106_DEVICE_GATE_H
#define PG106_DEVICE_GATE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum pg106_gate_reason {
	PG106_GATE_OK_GP106 = 0,    // GP106M/GP106BM mobile id: accepted
	PG106_GATE_GP10X_UNSUPPORTED, // known Pascal sibling (GP104/GP107):
	                              // named in logs, never accepted
	PG106_GATE_NO_MATCH,        // not in the table at all
};

// Probe-time gate: true iff device_id is a GP106-die mobile part. Only ids
// that start() can actually accept pass, so IOKit never starts the driver on
// a sibling only to have bring-up refuse it. why may be NULL.
bool pg106_device_gate(uint16_t device_id, enum pg106_gate_reason *why);

// Chip name from the table ("GP106M [GeForce GTX 1060 Mobile]", ...) for
// any known id, accepted or not; NULL if the id is not in the table.
const char *pg106_gate_chip_name(uint16_t device_id);

// Start-time decision from the PCI id and the raw PMC_BOOT_0 word read
// from BAR0.
struct pg106_start_decision {
	bool accept;                 // id gate passed AND BOOT_0 is a GP106
	enum pg106_gate_reason pci_reason;
	bool boot0_readable;         // false for 0x00000000 / 0xffffffff
	bool boot0_gp10x;            // decoded architecture is GP10x
	bool boot0_gp106;            // decoded architecture == 0x136
	uint32_t boot0_architecture; // decode; 0 unless readable + modern path
	uint32_t boot0_revision;     // decode; 0 unless readable + modern path
};

// 0x00000000 and 0xffffffff are what a BAR that is not decoding (memory
// space disabled, device in D3cold, link down) reads back as; neither is a
// valid BOOT_0 for any NVIDIA GPU, so both are rejected rather than decoded.
struct pg106_start_decision pg106_start_decision(uint16_t device_id,
						 uint32_t boot0);

#ifdef __cplusplus
}
#endif

#endif // PG106_DEVICE_GATE_H
