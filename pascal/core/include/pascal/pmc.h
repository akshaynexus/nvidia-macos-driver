// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/pmc.h — PMC_BOOT_0/BOOT_1 decode (chip identification).
//
// Derives from nouveau, file drivers/gpu/drm/nouveau/nvkm/engine/device/base.c
// (functions nvkm_device_ctor: boot0 decode, and nvkm_device_endianness:
// PMC_BOOT_1 pattern), linux master, fetched to /tmp/pascal-ref/nouveau/.
//
// On GP106 the first BAR0 word read after the endianness dance is PMC_BOOT_0
// at offset 0x000000 (base.c: `boot0 = nvkm_rd32(device, 0x000000);`).

#ifndef PASCAL_PMC_H
#define PASCAL_PMC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// PMC register offsets within BAR0.
// 0x000000: PMC_BOOT_0, chip identification (base.c nvkm_device_ctor).
// 0x000004: PMC_BOOT_1, endianness flag (base.c nvkm_device_endianness).
#define PG106_PMC_BOOT_0 0x00000000u
#define PG106_PMC_BOOT_1 0x00000004u
// 0x000200: PMC_ENABLE, one reset/enable bit per engine. nouveau only ever
// read-modify-writes it per engine: nv04_mc_device_disable/enable
// (nvkm/subdev/mc/nv04.c:34-46: nvkm_mask(0x200, mask, 0 / mask), the
// enable followed by a posting read), used by gp100_mc
// (nvkm/subdev/mc/gp100.c:96). Never write it whole.
#define PG106_PMC_ENABLE 0x00000200u

// BOOT_1 values: 0 = GPU little-endian, 0x01000001 = GPU big-endian
// (base.c nvkm_device_endianness: writes 0x01000001 to switch, and treats any
// other nonzero value as "endianness switch not functional, assume LE").
#define PG106_PMC_BOOT_1_LE      0x00000000u
#define PG106_PMC_BOOT_1_BE      0x01000001u

// BOOT_0 modern decode (base.c nvkm_device_ctor):
//   if ((boot0 & 0x1f000000) > 0) {                 // any of bits [28:24] set
//           chipset = (boot0 & 0x1ff00000) >> 20;   // 9-bit architecture, [28:20]
//           chiprev = (boot0 & 0x000000ff);         // revision, [7:0]
#define PG106_BOOT0_MODERN_TEST_MASK 0x1f000000u
#define PG106_BOOT0_CHIPSET_MASK     0x1ff00000u
#define PG106_BOOT0_CHIPSET_SHIFT    20
#define PG106_BOOT0_CHIPREV_MASK     0x000000ffu

// The legacy fallback (base.c, the else-if branch) — pre-NV10 chips whose
// architecture bits [28:20] are zero:
//   else if ((boot0 & 0xff00fff0) == 0x20004000) {
//           chipset = (boot0 & 0x00f00000) ? 0x05 : 0x04;   // NV5 or NV4
// This is the "0xf masking" quirk from older nouveau decode code, which read
// only 4 architecture bits ((boot0 & 0xf00000) >> 20); on current master the
// modern path takes a full 9 bits and the legacy compare only catches NV4/5.
#define PG106_BOOT0_LEGACY_MASK   0xff00fff0u
#define PG106_BOOT0_LEGACY_VALUE  0x20004000u
#define PG106_BOOT0_LEGACY_NV5    0x00f00000u

// Architecture field results of interest (base.c nvkm_device_ctor switch on
// `device->chipset & 0x1f0`, and nvkm_device.h enum card_type):
//   case 0x130: device->card_type = GP100;  // all GP10x: GP100/102/104/106/107/108/B
// GP106 itself is chipset 0x136: base.c `nv136_chipset = { .name = "GP106", ... }`,
// cross-checked in Mesa src/nouveau/drm-shim/nouveau_noop.c (chip_id 0x130
// family, excluding 0x130/0x13b, maps to PASCAL_B / PASCAL_COMPUTE_B, which is
// where GP106's 0xC1C0 compute class comes from).
#define PG106_CARD_TYPE_GP10X_FAMILY 0x130u
#define PG106_ARCH_GP106             0x136u

// Decode status. PG106_PMC_UNKNOWN mirrors base.c's `nvdev_error(device,
// "unknown chipset (%08x)\n", boot0)` for words that match neither decode path.
enum pg106_pmc_status {
	PG106_PMC_OK = 0,
	PG106_PMC_UNKNOWN = 1,
};

struct pg106_pmc_probe {
	// (boot0 & 0x1ff00000) >> 20 — 0x136 for GP106; 0x04/0x05 on the legacy
	// NV4/NV5 path. Undefined when status != PG106_PMC_OK.
	uint32_t architecture;
	// (boot0 >> 16) & 0xf — silicon variant nibble, bits [19:16]. Current
	// nouveau never decodes these bits (base.c uses only [28:20] and [7:0]);
	// real GP106 reports 0 here (nouveau ML report of boot0 136000a1).
	uint32_t implementation;
	// boot0 & 0xff — chiprev in base.c (printed as "%02x"; a1 on GP106).
	uint32_t revision;
	// (architecture & 0x1f0) == 0x130 → nvkm card_type GP100. False on the
	// legacy path.
	bool is_gp10x;
	// architecture == 0x136, i.e. GP106 specifically.
	bool is_gp106;
	// Decoded via the NV_04/05 legacy fallback rather than the modern path.
	bool legacy_nv04;
	enum pg106_pmc_status status;
};

// Decode a PMC_BOOT_0 word. Pure function of boot0; no hardware access.
struct pg106_pmc_probe pg106_pmc_probe(uint32_t boot0);

#ifdef __cplusplus
}
#endif

#endif // PASCAL_PMC_H
