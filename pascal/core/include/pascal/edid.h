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

#ifdef __cplusplus
extern "C" {
#endif

// Every EDID block is 128 bytes; the blob is a whole number of blocks
// (E-EDID: base block + 0..254 extension blocks, but real-world blobs stop
// well before; the portable core caps at 8 blocks / 1 KiB).
#define PG106_EDID_BLOCK_SIZE    128u
#define PG106_EDID_MAX_BLOCKS    8u
#define PG106_EDID_MAX_BYTES     (PG106_EDID_BLOCK_SIZE * PG106_EDID_MAX_BLOCKS)
#define PG106_EDID_MAX_VICS      32u
#define PG106_EDID_MAX_CEA_DTDS  12u // 6 per CTA block x 2 blocks

// Parse outcome. Errors are ordered so a caller learns the most specific
// problem: bounds first, then the fixed header pattern, then block checksums.
enum pg106_edid_status {
	PG106_EDID_OK = 0,
	PG106_EDID_ERR_NULL,     // data or out pointer missing
	PG106_EDID_ERR_SIZE,     // len < 128, not a multiple of 128, or > max
	PG106_EDID_ERR_HEADER,   // bytes 0..7 not 00 ff ff ff ff ff ff 00
	PG106_EDID_ERR_CHECKSUM, // the BASE block does not sum to 0 mod 256
	// (an extension with a bad checksum is dropped, not fatal — see
	// pg106_edid.bad_blocks)
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
// in 10 kHz units expanded to kHz. Hsync offset/width are 8-bit + two MSBs
// from byte 11[7:6]/[5:4]; vsync offset/width are nibble-packed in byte 10
// plus 2 MSBs from byte 11[3:2]/[1:0]. Sync
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
	// Analog sync (flag bit 4 = 0, E-EDID 1.4 Table 3.22): bit 3 =
	// bipolar analog composite, bit 2 = serrations, bit 1 = sync on all
	// three RGB lines (0 = green only). Analog sync has NO H/V polarity
	// bits; hsync_positive/vsync_positive stay false. Digital composite
	// uses bit 2 for serrations and bit 1 for the H polarity.
	bool analog_bipolar;
	bool serrations;
	bool sync_on_rgb;
};

// Parsed EDID. Base block identity/timings plus the CTA-861 extension of
// block 1 when present (tag byte 0x02). established_timings holds raw bytes
// 35..37 (E-EDID 1.4 Table 3.18; corrected 2026-11-08 — the previous
// comment had the bit positions wrong): byte 35 bit 5 = 640x480@60,
// byte 35 bit 0 = 800x600@60, byte 36 bit 3 = 1024x768@60.
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
	// Bit n set: extension block n failed its checksum and was dropped
	// (base block failures are PG106_EDID_ERR_CHECKSUM instead).
	uint8_t bad_blocks;

	struct pg106_edid_timing preferred_timing; // first DTD (bytes 54..71)
	bool has_preferred_timing;                 // no non-monitor descriptor found

	// CTA-861 extension blocks (tag 0x02) — ALL of them, in block order
	// (VICs and DTDs accumulate across blocks; revision is the first's).
	struct {
		bool present;
		uint8_t block_count;                     // CTA blocks parsed
		uint8_t revision;                        // ext byte 1
		// Short-video VICs in order (CTA-861-F §7.5.1): byte values
		// 1..127 are VICs 1..127, 129..192 are VICs 1..64 with the
		// native flag, 193..253 are VICs 193..253 (no flag); 0, 128,
		// 254 and 255 are reserved and skipped.
		uint8_t vic[PG106_EDID_MAX_VICS];
		uint8_t vic_count;
		bool vic_native[PG106_EDID_MAX_VICS];    // per-VIC native flag bit
		// CTA-861 detailed timings (from byte d = ext byte 2 until a
		// zero-clock slot or the 18-byte window passes byte 126).
		struct pg106_edid_timing dtd[PG106_EDID_MAX_CEA_DTDS];
		uint8_t dtd_count;
		uint8_t native_dtds; // byte 3 [3:0] of the first CTA block
		// Audio / HDMI facts (CTA-861 data blocks), for the display
		// driver's ELD and mode filter.
		bool basic_audio;    // byte 3 bit 6 of the first CTA block
		uint8_t sad[30];     // Short Audio Descriptors, 3 bytes each
		uint8_t sad_count;
		uint8_t speaker_alloc; // Speaker Allocation block byte 1
		bool hdmi_vsdb;      // HDMI LLC VSDB (OUI 00-0c-03): an HDMI sink
		// HDMI LLC VSDB (HDMI 1.4b §8.3.2; byte n = n-th byte of the
		// block counting the tag byte as 0): byte 6 flags, byte 7
		// Max_TMDS_Clock * 5 MHz (0 / absent = not stated).
		uint8_t hdmi_flags;  // byte 6: AI 7, DC_48 6, DC_36 5, DC_30 4, Y444 3, DVI_Dual 0
		uint32_t hdmi_max_tmds_khz;
		// HDMI Forum VSDB (OUI c4-5d-d8, HDMI 2.0 §10.3.2) or the
		// HDMI Forum SCDB (extended tag 0x79, HDMI 2.1 §10.3.6: the
		// same payload after two zero bytes). byte 4 version, byte 5
		// Max_TMDS_Character_Rate * 5 MHz, byte 6 SCDC_Present 7,
		// RR_Capable 6, LTE_340Mcsc_scramble 3, byte 7 DC_*_420 [2:0].
		bool hf_vsdb;
		uint8_t hf_version;
		bool scdc_present;   // byte 6 bit 7
		bool rr_capable;     // byte 6 bit 6 (SCDC read requests)
		bool lte_340_scramble; // byte 6 bit 3: scrambles <= 340 MHz too
		uint8_t hf_dc420;    // byte 7 [2:0]
		uint32_t max_tmds_khz; // HF byte 5 * 5000 (0 = absent)
		// YCbCr 4:2:0 Video Data Block (extended tag 0x0e): VICs the
		// sink accepts ONLY as 4:2:0 (not offered: PG106 sends RGB).
		uint8_t y420_vic[8];
		uint8_t y420_vic_count;
		bool y420_cmdb;      // 4:2:0 Capability Map present (ext tag 0x0f)
		// HDMI Forum EDID Extension Override Data Block (ext tag 0x78,
		// HDMI 2.1 §10.3.7): the real extension count when the base
		// block's byte 126 says 1 (0 = absent). The DDC reader uses it.
		uint8_t eeodb_blocks;
	} cea;

	// DisplayID extension blocks (tag 0x70; VESA DisplayID 1.3 §4.4.1
	// Type I and DisplayID 2.0 §4.1 Type VII detailed timings, 20 bytes
	// each). Many high-refresh monitors put their >60 Hz timings only
	// here.
	struct {
		bool present;
		uint8_t version;     // byte 1 (0x12/0x13 = 1.x, 0x20 = 2.0)
		struct pg106_edid_timing dtd[8];
		bool preferred[8];   // timing options byte 3 bit 7
		uint8_t dtd_count;
	} displayid;

	// Every DTD of the base block (descriptor slots 0..3 with a nonzero
	// pixel clock), in slot order; dtd[0] equals preferred_timing.
	struct pg106_edid_timing dtd[4];
	uint8_t dtd_count;
	// Display Product Name descriptor (tag 0xfc), trimmed at 0x0a; "" if
	// absent.
	char monitor_name[14];
};

// Parse a full EDID blob (base block plus any extension blocks, all 128-byte
// multiples). The header and the BASE checksum must be valid; an extension
// block whose checksum fails is dropped (recorded in bad_blocks) and the
// rest of the blob is still used — the same policy as drm_edid's
// invalid-block filtering. On anything but PG106_EDID_OK, *out is left
// untouched.
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

// CTA-861-H short-descriptor timing for `vic` (progressive formats the
// display driver can offer: 1-4, 16-19, 31-34, 41, 47, 60-64, 93-102),
// or NULL.
const struct pg106_edid_timing *pg106_edid_cea_timing(uint8_t vic);

// VESA DMT timing for established-timing index `bit` = (byte - 35) * 8 +
// bit position (E-EDID 1.4 Table 3.18: 5 = 640x480@60, 0 = 800x600@60,
// 11 = 1024x768@60 — the three tabulated; others return NULL).
const struct pg106_edid_timing *pg106_edid_established_timing(unsigned bit);

#ifdef __cplusplus
}
#endif

#endif /* PASCAL_EDID_H */
