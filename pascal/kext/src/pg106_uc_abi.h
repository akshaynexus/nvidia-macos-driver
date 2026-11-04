// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/src/pg106_uc_abi.h — the userspace↔kext ABI of PG106UserClient.
//
// Shaped after NVRMUserClient (kexts/NVRM/NVRM.cpp:298-348, externalMethod
// dispatch table at NVRM.cpp:1921-1939): plain IOExternalMethodDispatch
// selectors with fixed structure outputs. This header is the contract userspace
// (the future NVK bridge, Phase 4) compiles against, so it is deliberately
// plain C — the Phase 0 constants in <pascal/pmc.h> are portable too.
//
// Selectors, in dispatch-table order (PG106UserClient::externalMethod):
//   0: GET_VERSION  — version handshake
//   1: GET_PMC      — raw PMC_BOOT_0 + the pascal/core decode of it
//
// Licensing note: this tree is GPL-2.0-only (see README.md). Userspace
// talking to this kext is a separate program and may use any license; this
// header is GPL because it lives in the kext tree.

#ifndef PG106_UC_ABI_H
#define PG106_UC_ABI_H

#include <stdint.h>

#include <pascal/pmc.h> // struct pg106_pmc_probe (portable core decoder)

#define PG106_UC_ABI_MAJOR 0u
#define PG106_UC_ABI_MINOR 1u

enum pg106_uc_selector {
	PG106_UC_SEL_GET_VERSION = 0,
	PG106_UC_SEL_GET_PMC = 1,
};

// Selector 0 output. Handshake: the caller sends the major/minor it was
// built against (later, as scalar inputs) and the kext answers with its own.
// Phase 3 keeps it one-directional; the fields below are stable from 0.1.
struct pg106_uc_version {
	uint32_t major;
	uint32_t minor;
	uint32_t flags; // reserved, must be 0
};

// Selector 1 output: the raw first BAR0 word and the pascal/core decode of
// it (pg106_pmc_probe). The GPU must have been identified (kext start()
// succeeded) or probe.status is PG106_PMC_UNKNOWN with boot0 == 0.
struct pg106_uc_pmc {
	uint32_t boot0;                // raw PMC_BOOT_0, BAR0 + 0x000000
	struct pg106_pmc_probe probe;  // decoded by pascal/core
};

#endif // PG106_UC_ABI_H
