// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/src/pg106_uc_abi.h — the userspace↔kext ABI of PG106UserClient.
//
// Shaped after NVRMUserClient (kexts/NVRM/NVRM.cpp:298-348, externalMethod
// dispatch table at NVRM.cpp:1921-1939): plain IOExternalMethodDispatch
// selectors with fixed-size structure outputs and no inputs. Plain C so
// userspace (tools/pg106_probe.c today, the bridge transport later) compiles
// against it.
//
// Selectors, in dispatch-table order (PG106UserClient::externalMethod):
//   0: GET_VERSION  — ABI version
//   1: GET_PMC      — raw PMC_BOOT_0 + the pascal/core decode of it
//
// Every output struct is made of uint32_t fields only: no bool, no enum
// (both have implementation-defined size) and no padding, so the layout is
// the same for every compiler on either side and there are no padding bytes
// that could carry kernel stack contents out. The _Static_asserts pin it.
//
// ABI history: 0.1 copied struct pg106_pmc_probe (bool/enum members, one
// padding byte) straight out; 0.2 replaced it with pg106_uc_pmc below.
//
// Licensing note: this tree is GPL-2.0-only (see README.md). Userspace
// talking to this kext is a separate program and may use any license; this
// header is GPL because it lives in the kext tree.

#ifndef PG106_UC_ABI_H
#define PG106_UC_ABI_H

#include <stdint.h>

#define PG106_UC_ABI_MAJOR 0u
#define PG106_UC_ABI_MINOR 2u

// IOServiceOpen() type argument; PG106UserClient::initWithTask refuses others.
#define PG106_UC_TYPE_DEVICE 0u

enum pg106_uc_selector {
	PG106_UC_SEL_GET_VERSION = 0,
	PG106_UC_SEL_GET_PMC = 1,
	PG106_UC_SEL_COUNT
};

// Selector 0 output.
struct pg106_uc_version {
	uint32_t major;
	uint32_t minor;
	uint32_t flags; // reserved, always 0
};

// pg106_uc_pmc.flags bits.
#define PG106_UC_PMC_IS_GP10X   (1u << 0)
#define PG106_UC_PMC_IS_GP106   (1u << 1)
#define PG106_UC_PMC_LEGACY_NV4 (1u << 2)

// Selector 1 output: the first BAR0 word as read by start() and the
// pascal/core decode of it (struct pg106_pmc_probe, flattened). Only reachable
// after start() succeeded, so boot0 is always a word that start() accepted.
struct pg106_uc_pmc {
	uint32_t boot0;          // raw PMC_BOOT_0, BAR0 + 0x000000
	uint32_t architecture;   // pg106_pmc_probe.architecture
	uint32_t implementation; // pg106_pmc_probe.implementation
	uint32_t revision;       // pg106_pmc_probe.revision
	uint32_t status;         // enum pg106_pmc_status value (0 = OK)
	uint32_t flags;          // PG106_UC_PMC_*
};

#ifdef __cplusplus
static_assert(sizeof(struct pg106_uc_version) == 12, "pg106_uc_version layout");
static_assert(sizeof(struct pg106_uc_pmc) == 24, "pg106_uc_pmc layout");
#else
_Static_assert(sizeof(struct pg106_uc_version) == 12, "pg106_uc_version layout");
_Static_assert(sizeof(struct pg106_uc_pmc) == 24, "pg106_uc_pmc layout");
#endif

#endif // PG106_UC_ABI_H
