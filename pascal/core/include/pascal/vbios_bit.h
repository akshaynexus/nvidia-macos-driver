// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/vbios_bit.h — NVIDIA VBIOS BIT-section parser:
// locate the BIT table, walk its token directory, and expose the init
// (devinit) script lookup that the Phase 1b script interpreter consumes.
//
// Derived from nouveau, linux master, reference copies under
// /tmp/pascal-ref/nouveau/nvkm-subdev-bios/:
//   nvkm/subdev/bios/bit.c    bit_entry(): token walk (this file's core)
//   nvkm/subdev/bios/base.c   nvkm_bios_new(): BIT signature scan (l.189),
//                             version 'i' decode (l.195-200), rd08/rd16/rd32
//                             bounds guard nvbios_addr (l.31-47)
//   nvkm/subdev/bios/pcir.c   nvbios_pcirTp(): PCIR struct fields
//   nvkm/subdev/bios/image.c  nvbios_imagen(): ROM signature check (l.39-48)
//   nvkm/subdev/bios/shadow.c nvbios_shadow(): image checksum scoring
//                             (l.84-97) — tolerated unless require_checksum
//   nvkm/subdev/bios/init.c   init_table()/init_script(): token 'I' init
//                             script tables (l.358-427); INIT_DONE 0x71
//                             (l.606)
//
// OFFSET-CHAIN NOTE (mission assumption verified against source): nouveau
// does NOT derive the BIT offset from the PCI data structure. The ROM header
// word at 0x00 is checked against 0xaa55/0xbb77/'NV' (image.c:39-43); the
// u16 at 0x18 points at the PCIR struct (pcir.c:30), whose only use here is
// the image size/type — and the BIT table is found by scanning the whole
// shadowed image for the 5-byte signature ff b8 'B' 'I' 'T'
// (base.c:189-190, nvbios_findstr). There is no "BIT offset in the PCIR
// struct at 0x30" on linux master; this parser follows the source.
//
// Token set evidenced on linux master (consumer file:line):
//   'I' (0x49) init script tables      init.c:363
//   'i' (0x69) version info            base.c:195
//   'P' (0x50) perf/thermal entries    perf.c:36, therm.c:34
//   'd' (0x64) DisplayPort table       dp.c:33
// Real VBIOSes carry more tokens (e.g. 'D', 'T', 'L' — community docs /
// envytools, not consumed by the master nouveau files fetched); the lookup
// below is by arbitrary token id, so those parse without special support.
//
// GP106 (Helios 300 GTX 1060 Mobile): no dedicated nouveau handling needed —
// bit.c/bit_entry is chip-agnostic; the devinit scripts it exposes are what
// nvkm_devinit_post()/nvbios_init would execute (Phase 1b interpreter).
//
// Wire format is little-endian. Every read is byte-wise with bounds checks
// against the caller's buffer; truncated or hostile input returns a negative
// errno instead of ever reading out of bounds. Single-threaded, libc-only,
// C11. No Linux kernel dependencies.
//
// NOTE: this parser has only ever been exercised on a synthetic image
// (pascal/tests/test_vbios.c). The real Helios 300 VBIOS dump is blocked on
// the owner (research/evidence/BLOCKERS.md B1); until then every result here
// is simulation evidence, not device evidence (HANDOFF-PASCAL.md §7).

#ifndef PASCAL_VBIOS_BIT_H
#define PASCAL_VBIOS_BIT_H

#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// BIT signature scanned for by nvkm_bios_new (base.c:189-190):
// 0xff, 0xb8, 'B' 'I' 'T' — written as two string literals because 'B'
// is a hex digit and "\xb8BIT" would parse as one oversized \x escape
// (same concatenation nouveau's base.c:189 uses).
#define PG106_VBIOS_BIT_SIG "\xff\xb8" "BIT"
#define PG106_VBIOS_BIT_SIG_LEN 5u

// BIT header is 12 bytes: bit_entry() reads entry_count at +10 and entry_size
// at +9 (bit.c:31-32) and starts the walk at bit_offset + 12 (bit.c:32).
#define PG106_VBIOS_BIT_HDR_LEN 12u

// Per-token bytes read by bit_entry(): id u8 @+0, version u8 @+1,
// length u16 @+2, offset u16 @+4 (bit.c:34-38). The walk stride itself is
// the byte at +9 and may be larger (real VBIOSes use 12-byte entries with
// pad + a u32 high offset nouveau never reads).
#define PG106_VBIOS_BIT_ENTRY_RD_LEN 6u

// INIT_DONE opcode terminating init scripts (init.c:606 "[0x71] = init_done").
#define PG106_VBIOS_INIT_DONE_OP 0x71u

// ---------------------------------------------------------------------------
// Wire-format structs (host-endian mirrors; every field copied byte-wise
// during parse, so no struct is ever cast over the image).
// ---------------------------------------------------------------------------

// One BIT token directory entry (include/nvkm/subdev/bios/bit.h,
// struct bit_entry — MIT-licensed header; fields as read by bit.c:34-38).
struct pg106_vbios_bit_entry {
	// bit_entry.id — token character, e.g. 'I' (init scripts), 'i'
	// (version), 'P' (perf), 'd' (DisplayPort).
	uint8_t id;
	// bit_entry.version — per-token table version ('I' is 2 on G80+).
	uint8_t version;
	// bit_entry.length — byte length of the pointed-to table.
	uint16_t length;
	// bit_entry.offset — image-absolute offset of the table.
	uint16_t offset;
};

// Parsed BIT header. Fill by pg106_vbios_bit_parse(); read-only afterwards.
struct pg106_vbios_bit {
	// Borrowed pointer to the caller's image; not owned, not modified.
	const uint8_t *data;
	uint32_t size;
	// Image offset of the 5-byte BIT signature (base.c:189).
	uint32_t bit_offset;
	// Byte at bit_offset + 9 (bit.c:42) — token directory stride.
	uint8_t entry_size;
	// Byte at bit_offset + 10 (bit.c:31) — number of token entries.
	uint8_t entry_count;
};

// ROM header + PCIR data structure probe (image.c:39-48, pcir.c:27-68).
// The PCIR chain is NOT used to find BIT (see header comment); it is
// reported here because nvkm uses it for the image size/type and the
// checksum nvbios_shadow scores (shadow.c:84-97).
struct pg106_vbios_rom {
	// True iff the first word is 0xaa55, 0xbb77 or 'NV' (image.c:39-43).
	bool rom_sig_ok;
	// PCIR struct offset (u16 at 0x18, pcir.c:30), 0 if none/zero.
	uint32_t pcir_offset;
	// True iff PCIR pointed to and its signature is PCIR/RGIS/NPDS
	// (pcir.c:34-36).
	bool pcir_ok;
	// pcir + 0x04 / + 0x06 (pcir.c:58-59). 10de:1c20 = GP106M.
	uint16_t vendor_id;
	uint16_t device_id;
	// pcir + 0x0d..0x0f (pcir.c:60-62).
	uint8_t class_code[3];
	// pcir + 0x10 (u16, in 512-byte units) * 512 (pcir.c:63).
	uint32_t image_size;
	// pcir + 0x12 (pcir.c:64).
	uint16_t image_rev;
	// pcir + 0x14 — 0x00 x86 image, 0x70 EFI (pcir.c:65; image.c:56).
	uint8_t image_type;
	// pcir + 0x15 bit 7 (pcir.c:66).
	bool last_image;
	// nvbios_checksum(data, image_size) (base.c:82-88) over the first
	// image: 0 means the ROM checksum byte is consistent (shadow.c:85-97
	// scores checksum-failed images lower but tolerates them unless
	// require_checksum).
	uint8_t checksum;
};

// VBIOS version bytes as decoded by nvkm_bios_new (base.c:195-200) from the
// 'i' token table.
struct pg106_vbios_version {
	uint8_t micro; // bit_i.offset + 0
	uint8_t minor; // + 1
	uint8_t chip;  // + 2 (0x06 for GP106)
	uint8_t major; // + 3 (0x54 = "84." on real GP106 VBIOSes)
	uint8_t patch; // + 4
};

// ---------------------------------------------------------------------------
// API (errno-style returns: 0 or negative errno)
// ---------------------------------------------------------------------------

// Locate the BIT table in a raw VBIOS image and validate its token
// directory. Mirrors nvkm_bios_new's signature scan (base.c:189) plus the
// implied header layout of bit_entry (bit.c:31-42). Returns 0 on success;
// -EINVAL on null args; -ENOENT when the signature is absent; -EPROTO when
// the header or token directory overruns the image or entry_size cannot
// hold the 6 bytes bit_entry reads.
int pg106_vbios_bit_parse(const uint8_t *data, uint32_t size,
			  struct pg106_vbios_bit *out);

// Find a token by id — nouveau bit_entry (bit.c:28-49). Returns 0 and fills
// *out when found; -ENOENT when absent; -EINVAL on null args. The entry's
// offset/length are not dereferenced here; read the table through the
// bounds-checked accessors below (nouveau relies on nvbios_rd16's OOB guard
// instead — base.c:41-44).
int pg106_vbios_bit_entry(const struct pg106_vbios_bit *vb, uint8_t id,
			  struct pg106_vbios_bit_entry *out);

// Bounds-checked readers mirroring nvbios_rd08/rd16/rd32 (base.c:57-79),
// but returning -EFAULT instead of a silent 0 on out-of-bounds (divergence
// noted: nouveau's nvkm_error "OOB" path). All reads are little-endian,
// unaligned-safe, byte-wise.
int pg106_vbios_rd08(const struct pg106_vbios_bit *vb, uint32_t offset,
		     uint8_t *val);
int pg106_vbios_rd16(const struct pg106_vbios_bit *vb, uint32_t offset,
		     uint16_t *val);
int pg106_vbios_rd32(const struct pg106_vbios_bit *vb, uint32_t offset,
		     uint32_t *val);

// Init-script table lookup for the Phase 1b devinit interpreter.
//
// On G80+ (incl. GP106) the devinit scripts live under token 'I'
// (init.c:363): bit_I.offset is the init table, whose u16 at +0x00 points
// at the script table — a flat u16 array of image-absolute script offsets
// indexed by script number (init.c:399 init_script_table, init.c:422-424
// init_script). Returns 0 and the script-table offset; -ENOENT when the
// pointer is 0 (init.c:387 "script table pointer invalid" warning) and
// -EPROTO when the 'I' entry is shorter than the script-table pointer.
int pg106_vbios_init_script_table(const struct pg106_vbios_bit *vb,
				  const struct pg106_vbios_bit_entry *bit_I,
				  uint16_t *table_offset);

// Resolve init script `index` to its image offset: rd16(script_table +
// 2*index), init.c:424. Returns 0 and the offset — which may be 0, the
// "no such script" value init.c callers test for (e.g. init.c:44-47;
// script 0 = pre-POST init, 1 = post-POST per nvkm_devinit_post callers).
// -ENOENT when there is no script table; -EFAULT when the index reads
// outside the image.
int pg106_vbios_init_script(const struct pg106_vbios_bit *vb,
			    const struct pg106_vbios_bit_entry *bit_I,
			    uint16_t index, uint16_t *script_offset);

// Decode the 'i' version table exactly as nvkm_bios_new does
// (base.c:195-200). `bit_i` must be the entry for token 'i' with length
// >= 4 (base.c:195's check). Returns 0, -EPROTO on short length, -EFAULT
// if the table is not fully inside the image.
int pg106_vbios_version(const struct pg106_vbios_bit *vb,
			const struct pg106_vbios_bit_entry *bit_i,
			struct pg106_vbios_version *out);

// Probe the ROM header and PCIR data structure of a raw image (image.c
// nvbios_imagen + pcir.c nvbios_pcirTp, base = 0). Returns 0 with the
// fields above filled; -ENOENT when the ROM signature is unknown;
// -EPROTO when PCIR is pointed at but its fields are not fully inside the
// image. A zero u16 at 0x18 or a non-PCIR/RGIS/NPDS signature yields 0
// with pcir_ok = false (pcir.c:31-46 treats it as "no PCIR").
int pg106_vbios_rom_probe(const uint8_t *data, uint32_t size,
			  struct pg106_vbios_rom *out);

// u8 sum over `size` bytes — nouveau nvbios_checksum (base.c:82-88). A
// valid x86 expansion ROM sums to 0 over its declared image size.
uint8_t pg106_vbios_checksum(const uint8_t *data, uint32_t size);

#endif // PASCAL_VBIOS_BIT_H
