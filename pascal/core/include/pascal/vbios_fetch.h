// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/vbios_fetch.h — assemble a VBIOS image from a
// chunked source (ACPI _ROM, PCI expansion ROM) and decide whether it is the
// ROM of the device we are driving.
//
// Port of nouveau's shadowing logic (torvalds/linux d48723711bae,
// drivers/gpu/drm/nouveau/):
//   nvkm/subdev/bios/shadow.c:38-49    shadow_fetch: grow the image to
//                                      `upto`, read from the current end
//   nvkm/subdev/bios/shadow.c:51-107   shadow_image: fetch a 0x1000 header,
//                                      parse image idx (nvbios_image), fetch
//                                      to its end, checksum type-0x00 images,
//                                      recurse while !image.last; score
//   nvkm/subdev/bios/shadowacpi.c:25-57 acpi_read_bios: _ROM(offset, length),
//                                      the result is min(length, buffer len)
//   nvkm/subdev/bios/shadowacpi.c:79-102 acpi_read_slow: 0x1000-byte _ROM
//                                      calls, stop at the first short one
//   nvkm/subdev/bios/shadowacpi.c:125-140 ACPI fast requires the checksum,
//                                      ACPI slow does not
//   nvkm/subdev/bios/shadow.c:177-186  source order and per-source skip
//
// The image chain itself (PCIR, NPDE size override, `last` flag) is
// vbios_bit.c's pg106_vbios_image (nvbios_image, image.c:29-83), so the bytes
// we stop at are the bytes nouveau would stop at.
//
// What is added on top of nouveau (the kext needs a yes/no, not a score):
// pg106_vbios_fetch_validate() requires the PCIR vendor/device of image 0 to
// equal the PCI function's ids and the BIT table to parse. nouveau does not
// compare PCIR ids; an ACPI _ROM that serves another GPU's ROM (Optimus
// boards with several dGPU SKUs) would otherwise be accepted.
//
// On the Helios 300 the source is ACPI _ROM on \_SB.PCI0.PEG0.PEGP (SSDT11):
// at most 0x1000 bytes per call, RVBS = 0x29400 bytes in total
// (research/evidence/HELIOS300-HW-PROBE.md). The machine has no ROM chip, so
// the PCI ROM BAR read fails there; the PCI source is kept for other boards.
//
// C11, libc only (calloc/realloc/free). Errors are negative errno.

#ifndef PASCAL_VBIOS_FETCH_H
#define PASCAL_VBIOS_FETCH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// acpi_read_slow's chunk (shadowacpi.c:94). Also the most the Helios _ROM
// returns per call.
#define PG106_VBIOS_ACPI_CHUNK 0x1000u
// Header fetch size per image (shadow.c:64).
#define PG106_VBIOS_HDR_FETCH 0x1000u
// Upper bound on an assembled ROM. nouveau has none (nvbios_extend grows
// without limit); the kext needs one. 1 MiB is 6x the Helios ROM.
#define PG106_VBIOS_FETCH_MAX 0x100000u

// A byte source. read() copies up to `len` bytes starting at `off` into
// `dst` and returns how many it copied (0..len), or a negative errno if the
// source failed outright (ACPI evaluation error, unmapped ROM). A return
// shorter than `len` ends the fetch, like acpi_read_slow's `ret != 0x1000`.
// `chunk` is the largest len the fetcher will ask for (0 = no limit);
// requests are aligned to it.
struct pg106_vbios_src {
	const char *name; // for logs: "ACPI _ROM", "PCI ROM"
	int (*read)(void *user, uint32_t off, uint32_t len, uint8_t *dst);
	void *user;
	uint32_t chunk;
	// Reject an image 0 whose checksum is not zero (nvbios_source
	// .require_checksum). ACPI slow leaves it false (shadowacpi.c:134-140).
	bool require_checksum;
};

// Assembled ROM, owned by the caller after a successful fetch.
struct pg106_vbios_blob {
	uint8_t *data;
	uint32_t size; // exactly the end of the last image of the chain
};

struct pg106_vbios_fetch_result {
	int ret;            // 0 or negative errno, same as the return value
	const char *error;  // static text, NULL on success
	int score;          // shadow_image's score (0 = unusable)
	uint8_t images;     // images walked
	uint32_t size;      // bytes of the chain (== blob size on success)
	uint32_t fetched;   // bytes read from the source (may exceed size:
	                    // reads are chunk-aligned)
	uint32_t reads;     // read() calls
	bool short_read;    // the source returned less than asked at least once
	bool checksum_ok;   // image 0 sums to zero
	uint16_t pcir_vendor, pcir_device; // image 0 PCIR ids
};

// Fetch the image chain from `src`: shadow_image's walk with
// acpi_read_slow's chunking. On success *blob owns a buffer of exactly
// res->size bytes. Returns 0; -EINVAL bad args; -ENOENT no ROM signature /
// image chain at offset 0 ("image 0 invalid"); -EIO the source failed or
// stopped before an image ended ("fetch failed"); -EBADMSG checksum
// required and wrong; -EFBIG the chain runs past PG106_VBIOS_FETCH_MAX;
// -ENOMEM.
int pg106_vbios_fetch(const struct pg106_vbios_src *src,
		      struct pg106_vbios_blob *blob,
		      struct pg106_vbios_fetch_result *res);

// Accept a fetched ROM for the PCI function (pci_vendor, pci_device):
// PCIR vendor/device of image 0 must match (-ENODEV otherwise), and BIT
// must parse (pg106_vbios_bit_parse; its error otherwise). res may be NULL;
// when non-NULL, ret/error/pcir_* are updated.
int pg106_vbios_fetch_validate(const struct pg106_vbios_blob *blob,
			       uint16_t pci_vendor, uint16_t pci_device,
			       struct pg106_vbios_fetch_result *res);

// Try each source in order (the kext passes ACPI _ROM, then PCI ROM) and
// keep the first ROM that fetches and validates. *which gets the index of
// the accepted source. Per-source results go to res[i] (array of nsrc,
// may be NULL). Returns 0 or the error of the last source tried.
int pg106_vbios_fetch_first(const struct pg106_vbios_src *const *srcs,
			    unsigned nsrc, uint16_t pci_vendor,
			    uint16_t pci_device, struct pg106_vbios_blob *blob,
			    unsigned *which,
			    struct pg106_vbios_fetch_result *res);

void pg106_vbios_blob_free(struct pg106_vbios_blob *blob);

#ifdef __cplusplus
}
#endif

#endif // PASCAL_VBIOS_FETCH_H
