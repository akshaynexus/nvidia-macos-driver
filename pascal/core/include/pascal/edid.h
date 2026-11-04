// SPDX-License-Identifier: MIT
// pascal/core/include/pascal/edid.h — E-EDID base block + CTA-861 extension parser.
//
// Licensing: MIT. Written against the public VESA E-EDID v1.4 structure
// definitions (E-EDID / VESA ENHANCED EDID STANDARD, release A, rev 2) and
// CTA-861-H extension layout as documented in VESA E-EDID §3.10 (detailed
// timing descriptor) and the CTA-861 extension chapter. No code is ported
// from GPL sources (in particular NOT from linux drm_edid.c); field offsets
// and bit positions come from the published specifications and were
// cross-checked against the real dump in research/reference/edid/.
//
// Purpose (HANDOFF-PASCAL.md Phase 5, part A): turn the real EDID of a
// 1080p panel into the timing numbers the simulated mode-set sequence
// programs into the gf119/gp102 display hardware: pixel clock, active and
// blanking sizes, sync offsets/widths and polarities, plus the CEA-861
// VIC 16 (1920x1080 @ 60 Hz) entry when the panel advertises it.

#ifndef PASCAL_EDID_H
#define PASCAL_EDID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Every EDID block is 128 bytes; the blob is a whole number of blocks
// (E-EDID: base block + 0..254 extension blocks, but real-world blobs stop
// well before; the portable core caps at 8 blocks / 1 KiB).
#define PG106_EDID_BLOCK_SIZE    128u
#define PG106_EDID_MAX_BLOCKS    8u
#define PG106_EDID_MAX_BYTES     (PG106_EDID_BLOCK_SIZE * PG106_EDID_MAX_BLOCKS)
#define PG106_EDID_MAX_VICS      32u

// Parse outcome. Errors are ordered so a caller learns the most specific
// problem: bounds first, then the fixed header pattern, then block checksums.
enum pg106_edid_status {
	PG106_EDID_OK = 0,
	PG106_EDID_ERR_NULL,     // data or out pointer missing
	PG106_EDID_ERR_SIZE,     // len < 128, not a multiple of 128, or > max
	PG106_EDID_ERR_HEADER,   // bytes 0..7 not 00 ff ff ff ff ff ff 00
	PG106_EDID_ERR_CHECKSUM, // some 128-byte block does not sum to 0 mod 256
};

// Sync scheme from DTD flag byte (E-EDID §3.10.3.1): bit 4 selects analog
// vs digital; with digital, bit 3 separates "digital separate" (bits 1/2
// carry H/V polarity) from "digital composite" (bit 1 carries one polarity).
enum pg106_edid_sync {
	PG106_EDID_SYNC_ANALOG = 0,
	PG106_EDID_SYNC_DIGITAL_COMPOSITE,
	PG106_EDID_SYNC_DIGITAL_SEPARATE,
};

// One Detailed Timing Descriptor (E-EDID §3.10.3, 18 bytes). htotal/vtotal
// are derived (active + blank); pixel clock is the 16-bit little-endian word
// in 10 kHz units expanded to kHz. Hsync offset/width are 8-bit; vsync
// offset/width are nibble-packed across bytes 10/11 (8 bits each). Sync
// polarities are only meaningful for PG106_EDID_SYNC_DIGITAL_SEPARATE
// (H: flag bit 1, V: flag bit 2).
struct pg106_edid_timing {
	uint32_t pixel_clock_khz;
	uint16_t hactive;
	uint16_t hblank;
	uint16_t hsync_offset;
	uint16_t hsync_width;
	uint16_t vactive;
	uint16_t vblank;
	uint16_t vsync_offset;
	uint16_t vsync_width;
	uint16_t htotal;         // hactive + hblank
	uint16_t vtotal;         // vactive + vblank
	uint8_t sync;            // enum pg106_edid_sync
	bool hsync_positive;     // separate sync: H polarity
	bool vsync_positive;     // separate sync: V polarity
	bool interlaced;         // flag byte bit 7
};

// Parsed EDID. Base block identity/timings plus the CTA-861 extension of
// block 1 when present (tag byte 0x02). established_timings holds raw bytes
// 35..37 (E-EDID §3.8): bit n of byte 0/1/2 = established timing n/8/16,
// e.g. byte 0 bit 3 = 640x480@60, byte 1 bit 1 = 800x600@60,
// byte 2 bit 3 = 1024x768@60 (values per E-EDID tables §3.8.1/.2).
struct pg106_edid {
	char manufacturer[4];    // 3-letter PNP id, NUL-terminated ("ACR")
	uint16_t product_code;   // bytes 10..11, little-endian
	uint32_t serial_number;  // bytes 12..15, little-endian
	uint8_t version_major;   // byte 18 (e.g. 1)
	uint8_t version_minor;   // byte 19 (e.g. 3 or 4)
	bool digital;            // video input definition byte 20, bit 7
	bool preferred_timing_is_dtd; // feature byte 24, bit 1

	uint8_t established_timings[3];
	uint8_t extension_count; // declared in base block byte 126
	uint8_t block_count;     // blocks actually supplied to the parser

	struct pg106_edid_timing preferred_timing; // first DTD (bytes 54..71)
	bool has_preferred_timing;                 // no non-monitor descriptor found

	// CTA-861 extension (block 1) when present.
	struct {
		bool present;
		uint8_t revision;                        // ext byte 1
		// Short-video VICs in order (raw & 0x7f). The VIC byte's bit
		// 7 is the native-format flag (CTA-861-A/B) or, under the
		// CTA-861-H reading, marks extended VICs (193 + low 7 bits);
		// both interpretations share the low 7 bits, which is all
		// VIC <= 63 consumers (i.e. Phase 5) need.
		uint8_t vic[PG106_EDID_MAX_VICS];
		uint8_t vic_count;
		bool vic_native[PG106_EDID_MAX_VICS];    // per-VIC native flag bit
	} cea;
};

// Parse a full EDID blob (base block plus any extension blocks, all 128-byte
// multiples). Validates the fixed header and every block's checksum before
// decoding; on anything but PG106_EDID_OK, *out is left untouched.
enum pg106_edid_status pg106_edid_parse(const void *data, size_t len,
					struct pg106_edid *out);

// Refresh rate in millihertz = pixel_clock_khz * 1e6 / (htotal * vtotal),
// 0 if htotal or vtotal is 0 (avoids the divide).
uint32_t pg106_edid_refresh_milli_hz(const struct pg106_edid_timing *t);

// True if the CTA-861 extension advertises this VIC in its video data block.
bool pg106_edid_cea_has_vic(const struct pg106_edid *edid, uint8_t vic);

// CTA-861 VIC 16 (1920x1080p @ 60 Hz) as a timing struct, per the CTA-861-H
// short-descriptor table: 148.500 MHz, 2200x1125 total (Hfront 88 / Hsync 44
// / Hback 148, Vfront 4 / Vsync 5 / Vback 36), digital separate sync, both
// polarities positive, progressive. This is the exact mode the panel above
// lists as its native CEA format; a mode-set sequence can program it
// directly or instead use edid->preferred_timing (byte-identical here).
const struct pg106_edid_timing *pg106_edid_cea_timing_vic16(void);

#endif /* PASCAL_EDID_H */
