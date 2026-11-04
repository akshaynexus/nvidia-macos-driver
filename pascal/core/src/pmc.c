// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/src/pmc.c — PMC_BOOT_0 decode, derived from nouveau
// drivers/gpu/drm/nouveau/nvkm/engine/device/base.c, function
// nvkm_device_ctor (linux master; reference copy at
// /tmp/pascal-ref/nouveau/nvkm/engine/device/base.c).

#include <pascal/pmc.h>

struct pg106_pmc_probe
pg106_pmc_probe(uint32_t boot0)
{
	struct pg106_pmc_probe r = {
		.architecture = 0,
		.implementation = 0,
		.revision = 0,
		.is_gp10x = false,
		.is_gp106 = false,
		.legacy_nv04 = false,
		.status = PG106_PMC_UNKNOWN,
	};

	if ((boot0 & PG106_BOOT0_MODERN_TEST_MASK) > 0) {
		// base.c nvkm_device_ctor:
		//   device->chipset = (boot0 & 0x1ff00000) >> 20;
		//   device->chiprev = (boot0 & 0x000000ff);
		r.architecture = (boot0 & PG106_BOOT0_CHIPSET_MASK) >> PG106_BOOT0_CHIPSET_SHIFT;
		r.revision = boot0 & PG106_BOOT0_CHIPREV_MASK;
		// Bits [19:16] exist in the register but current nouveau ignores
		// them (see header comment); decode them read-only for callers.
		r.implementation = (boot0 >> 16) & 0xfu;
		r.status = PG106_PMC_OK;
	} else if ((boot0 & PG106_BOOT0_LEGACY_MASK) == PG106_BOOT0_LEGACY_VALUE) {
		// base.c, legacy branch:
		//   if (boot0 & 0x00f00000) device->chipset = 0x05;
		//   else                    device->chipset = 0x04;
		//   device->card_type = NV_04;
		r.architecture = (boot0 & PG106_BOOT0_LEGACY_NV5) ? 0x05u : 0x04u;
		r.revision = boot0 & PG106_BOOT0_CHIPREV_MASK;
		r.legacy_nv04 = true;
		r.status = PG106_PMC_OK;
	}
	// Neither path matched: leave status = PG106_PMC_UNKNOWN, matching
	// base.c's "unknown chipset (%08x)" error.

	if (r.status == PG106_PMC_OK && !r.legacy_nv04) {
		// base.c: `case 0x130: device->card_type = GP100; break;` —
		// the switch input is chipset & 0x1f0.
		r.is_gp10x = (r.architecture & 0x1f0u) == PG106_CARD_TYPE_GP10X_FAMILY;
		r.is_gp106 = r.architecture == PG106_ARCH_GP106;
	}

	return r;
}
