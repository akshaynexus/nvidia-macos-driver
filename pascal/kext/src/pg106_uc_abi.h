// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/src/pg106_uc_abi.h — the userspace↔kext ABI of PG106UserClient.
//
// Shaped after NVRMUserClient (kexts/NVRM/NVRM.cpp:298-348, externalMethod
// dispatch table at NVRM.cpp:1921-1939): plain IOExternalMethodDispatch
// selectors. Plain C so userspace (tools/pg106_probe.c) compiles against it.
//
// Client types (IOServiceOpen `type`, checked in initWithTask):
//   DEVICE (0)  administrator only: every selector, including the raw
//               identity (GET_PMC). pg106_probe uses it.
//   RENDER (1)  the bridge (pascal/bridge, transport_iokit.c): GET_VERSION,
//               SUBMIT, GET_STATUS. One dispatcher (VM, BOs, syncobjs,
//               handles) per connection; a connection can name only its own
//               handles. Administrator only unless the owner enables
//               unprivileged access (boot-arg pg106_render_users=1 or
//               Info.plist PG106RenderUsers; pascal/kext/README.md "Who may
//               open the render client").
//
// Selectors, in dispatch-table order (PG106UserClient::externalMethod):
//   0: GET_VERSION  — ABI version                         (both types)
//   1: GET_PMC      — raw PMC_BOOT_0 + its decode          (DEVICE only)
//   2: SUBMIT       — one bridge wire frame in, one out    (both types)
//                     (pg106_br_wire.h; PG106_BR_IOKIT_SEL_SUBMIT)
//   3: GET_STATUS   — bring-up / power state               (both types)
//
// Every output struct is made of uint32_t fields only: no bool, no enum
// and no padding, so the layout is the same for every compiler on either
// side and no padding byte can carry kernel stack contents out.
//
// ABI history: 0.1 copied struct pg106_pmc_probe out; 0.2 pg106_uc_pmc;
// 0.3 adds SUBMIT, GET_STATUS and the RENDER type.
//
// Licensing note: this tree is GPL-2.0-only (README.md). Userspace talking
// to this kext is a separate program and may use any license; the bridge
// does not include this header (it uses the MIT pg106_br_wire.h numbers,
// which PG106UserClient.cpp static-asserts against these).

#ifndef PG106_UC_ABI_H
#define PG106_UC_ABI_H

#include <stdint.h>

#define PG106_UC_ABI_MAJOR 0u
#define PG106_UC_ABI_MINOR 3u

#define PG106_UC_TYPE_DEVICE 0u
#define PG106_UC_TYPE_RENDER 1u

enum pg106_uc_selector {
	PG106_UC_SEL_GET_VERSION = 0,
	PG106_UC_SEL_GET_PMC = 1,
	PG106_UC_SEL_SUBMIT = 2,
	PG106_UC_SEL_GET_STATUS = 3,
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
// pascal/core decode of it (struct pg106_pmc_probe, flattened).
struct pg106_uc_pmc {
	uint32_t boot0;          // raw PMC_BOOT_0, BAR0 + 0x000000
	uint32_t architecture;   // pg106_pmc_probe.architecture
	uint32_t implementation; // pg106_pmc_probe.implementation
	uint32_t revision;       // pg106_pmc_probe.revision
	uint32_t status;         // enum pg106_pmc_status value (0 = OK)
	uint32_t flags;          // PG106_UC_PMC_*
};

// pg106_uc_status.flags bits.
#define PG106_UC_ST_READY        (1u << 0) // bring-up done, sessions served
#define PG106_UC_ST_D3COLD       (1u << 1) // idle power-off enabled by policy
#define PG106_UC_ST_D3COLD_OFF   (1u << 2) // disabled after a failed attempt
#define PG106_UC_ST_CHANNEL_HELD (1u << 3) // a render client owns the channel
#define PG106_UC_ST_ACPI         (1u << 4) // ACPI companion found
#define PG106_UC_ST_RENDER_USERS (1u << 5) // non-root render clients allowed

// Selector 3 output. Signed values (errno, -1) travel as their two's
// complement bit pattern.
struct pg106_uc_status {
	uint32_t pm_state;       // enum pg106_pm_state (pascal/pm.h)
	uint32_t boot_stage;     // enum pg106_devboot_stage reached / failed
	uint32_t boot_ret;       // 0 or negative errno of the last boot
	uint32_t boot_step;      // failing plan step (when boot_ret != 0)
	uint32_t generation;     // successful boots since load
	uint32_t power_cycles;   // D3cold entries since load
	uint32_t sessions;       // open render sessions
	uint32_t vbios_size;     // bytes of the ROM in use (0 = none)
	uint32_t vbios_source;   // 0 none, 1 ACPI _ROM, 2 PCI ROM BAR
	uint32_t fw_present;     // bit i = pg106_devboot_fw_names[i] loaded
	uint32_t fw_missing;     // bit i = request failed
	uint32_t vram_mib;       // pg106_fb_vram_size, MiB
	uint32_t bar1_mib;       // BAR1 aperture, MiB
	uint32_t flags;          // PG106_UC_ST_*
};

#ifdef __cplusplus
static_assert(sizeof(struct pg106_uc_version) == 12, "pg106_uc_version layout");
static_assert(sizeof(struct pg106_uc_pmc) == 24, "pg106_uc_pmc layout");
static_assert(sizeof(struct pg106_uc_status) == 56, "pg106_uc_status layout");
#else
_Static_assert(sizeof(struct pg106_uc_version) == 12, "pg106_uc_version layout");
_Static_assert(sizeof(struct pg106_uc_pmc) == 24, "pg106_uc_pmc layout");
_Static_assert(sizeof(struct pg106_uc_status) == 56, "pg106_uc_status layout");
#endif

#endif // PG106_UC_ABI_H
