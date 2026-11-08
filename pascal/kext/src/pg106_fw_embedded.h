// SPDX-License-Identifier: GPL-2.0-only
// pg106_fw_embedded.h — the GP106 firmware built into PG106.kext.
//
// The table is generated at build time by scripts/gen-firmware-embed.py
// (src/gen/pg106_fw_embedded.c, not committed) from research/firmware,
// one row per pg106_devboot_fw_names entry (pascal/core/src/devboot.c), in
// that order, each file checked against pascal/kext/firmware.sha256 and
// embedded byte for byte. The kext points struct pg106_devboot_fw at these
// arrays (PG106::embedFirmware) and asks kextd (OSKextRequestResource) only
// for rows the table lacks, which only works for a kext that has a bundle
// on disk (/Library/Extensions), never for an OpenCore-injected one.
#ifndef PG106_FW_EMBEDDED_H
#define PG106_FW_EMBEDDED_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pg106_fw_embedded {
	const char *resource;  // flat resource name, e.g. "gp106-acr-bl.bin"
	const char *path;      // linux-firmware path, e.g. "gp106/acr/bl.bin"
	const uint8_t *data;
	uint32_t size;
	const char *sha256;    // of data, as pinned
};

extern const struct pg106_fw_embedded pg106_fw_embedded[];
extern const uint32_t pg106_fw_embedded_count;

#ifdef __cplusplus
}
#endif

#endif // PG106_FW_EMBEDDED_H
