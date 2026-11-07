// SPDX-License-Identifier: MIT
// pascal/core/src/edid.c — E-EDID base block + CTA-861 extension parser.
//
// MIT; see edid.h for the specification basis (public VESA E-EDID v1.4 and
// CTA-861-H structure definitions, no GPL code ported).
//
// Layout references used throughout (byte offsets within one 128-byte block
// unless noted):
//   base block   E-EDID §3: header 0..7, manufacturer id 8..9, product 10..11,
//                serial 12..15, version 18..19, input 20, features 24,
//                established timings 35..37, DTD slots 54/72/90/108,
//                extension count 126, checksum 127.
//   DTD          E-EDID §3.10.3: clock 0..1 (10 kHz units, LE), hactive 2 +
//                4[7:4], hblank 3 + 4[3:0], vactive 5 + 7[7:4], vblank 6 +
//                7[3:0], hsync offset 8 + 11[7:6]<<8, hsync width 9 +
//                11[5:4]<<8, vsync offset 10[7:4] + 11[3:2]<<4, vsync width
//                10[3:0] + 11[1:0]<<4, image size 12/13 + 14[7:4]/14[3:0],
//                hborder 15, vborder 16, flags 17.
//   monitor desc E-EDID §3.10.2: bytes 0..2 == 0, byte 3 = range limits (fd),
//                name (fc), serial (ff); distinguished from a real DTD by a
//                zero pixel-clock word (a DTD never has clock 0).
//   CTA-861 ext  byte 0 tag 0x02, 1 revision, 2 DTD start offset, 3 flags +
//                native DTD count (low nibble), 4.. data block collection
//                (tag byte: bits 7..5 tag, 4..0 length), DTDs from offset
//                byte 2, checksum 127.

#include <pascal/edid.h>

#include <string.h>

static const uint8_t header_pattern[8] = {
	0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,
};

// E-EDID §3.2 / §3.7: bytes 8..9 hold the 3-letter PNP id as five 5-bit
// fields (byte 8 holds letters 1-2 in bits [15:10]/[9:5] when read
// big-endian), letter value = bits + 'A' - 1.
static void
decode_manufacturer(const uint8_t *block, char out[4])
{
	uint16_t v = (uint16_t)((block[8] << 8) | block[9]);

	out[0] = (char)('A' + ((v >> 10) & 0x1f) - 1);
	out[1] = (char)('A' + ((v >> 5) & 0x1f) - 1);
	out[2] = (char)('A' + (v & 0x1f) - 1);
	out[3] = '\0';
}

// Decode one 18-byte detailed timing descriptor (offsets in edid.h header).
static void
decode_dtd(const uint8_t *d, struct pg106_edid_timing *t)
{
	uint8_t flags = d[17];

	t->pixel_clock_khz = (uint32_t)(d[0] | (d[1] << 8)) * 10u;

	t->hactive = (uint16_t)(d[2] | ((d[4] >> 4) << 8));
	t->hblank  = (uint16_t)(d[3] | ((d[4] & 0x0f) << 8));
	t->vactive = (uint16_t)(d[5] | ((d[7] >> 4) << 8));
	t->vblank  = (uint16_t)(d[6] | ((d[7] & 0x0f) << 8));

	t->hsync_offset = (uint16_t)(d[8] | ((d[11] >> 6) << 8));
	t->hsync_width  = (uint16_t)(d[9] | (((d[11] >> 4) & 0x3) << 8));
	// Byte 10 carries the low nibble of each Vsync value; byte 11 is
	// split into 2-bit MSB fields: [7:6] H-offset, [5:4] H-width,
	// [3:2] V-offset, [1:0] V-width (E-EDID §3.10.3). Verified against
	// the reference dump, where edid-decode's "Vfront 4 Vsync 5"
	// corresponds to bytes 10..11 = 45 00 (and 35 00 -> 3, 5).
	t->vsync_offset = (uint16_t)((d[10] >> 4) | (((d[11] >> 2) & 0x3) << 4));
	t->vsync_width  = (uint16_t)((d[10] & 0x0f) | ((d[11] & 0x3) << 4));

	t->htotal = (uint16_t)(t->hactive + t->hblank);
	t->vtotal = (uint16_t)(t->vactive + t->vblank);

	t->interlaced = (flags & 0x80) != 0;
	t->analog_bipolar = t->serrations = t->sync_on_rgb = false;
	if ((flags & 0x10) != 0) {
		// Digital: bit 3 selects separate vs composite sync.
		if ((flags & 0x08) != 0) {
			t->sync = PG106_EDID_SYNC_DIGITAL_SEPARATE;
			t->hsync_positive = (flags & 0x02) != 0;
			t->vsync_positive = (flags & 0x04) != 0;
		} else {
			// Digital composite: bit 2 = serrations, bit 1 =
			// H polarity (outside V sync); no V polarity bit.
			t->sync = PG106_EDID_SYNC_DIGITAL_COMPOSITE;
			t->hsync_positive = (flags & 0x02) != 0;
			t->vsync_positive = false;
			t->serrations = (flags & 0x04) != 0;
		}
	} else {
		// Analog (E-EDID 1.4 Table 3.22): bit 3 bipolar composite,
		// bit 2 serrations, bit 1 sync on RGB. Not polarities.
		t->sync = PG106_EDID_SYNC_ANALOG;
		t->hsync_positive = false;
		t->vsync_positive = false;
		t->analog_bipolar = (flags & 0x08) != 0;
		t->serrations = (flags & 0x04) != 0;
		t->sync_on_rgb = (flags & 0x02) != 0;
	}
}

// A descriptor slot is a real DTD iff its pixel clock word is nonzero;
// monitor descriptors (E-EDID §3.10.2) keep bytes 0..2 at 0.
static bool
slot_is_dtd(const uint8_t *slot)
{
	return (slot[0] | slot[1]) != 0;
}

// One CTA-861 extension block (tag 0x02). Byte 2 = d:
//   d == 0      no detailed timing descriptors AND no data block
//               collection (CTA-861 §7.5 — an earlier version wrongly
//               scanned the collection up to the checksum here);
//   d == 4      DTDs start at 4, no data block collection;
//   4 < d <= 127  data block collection in [4, d), DTDs from d;
//   d in 1..3   malformed: nothing parsed from the block.
// DTDs run in 18-byte slots from d while a slot fits before byte 127 and
// its pixel clock is nonzero (zero = padding).
// VIC bytes: bit 7 is the native flag (CTA-861-A..G for VIC 1..64); both
// value and flag are recorded (the corpus test cross-checks).
static void
parse_cta(const uint8_t *block, struct pg106_edid *edid)
{
	unsigned d = block[2];
	unsigned off = 4;

	if (!edid->cea.present) {
		edid->cea.present = true;
		edid->cea.revision = block[1];
		edid->cea.native_dtds = block[3] & 0x0f;
	}
	edid->cea.block_count++;

	if (d == 0 || (d > 0 && d < 4) || d > PG106_EDID_BLOCK_SIZE - 1)
		return;

	while (off < d && edid->cea.vic_count < PG106_EDID_MAX_VICS) {
		unsigned tag = block[off] >> 5;
		unsigned len = block[off] & 0x1f;
		unsigned i;

		if (off + 1 + len > d)
			break;	// header runs past the collection
		if (tag == 0x02) { // Video Data Block
			for (i = 0; i < len &&
				    edid->cea.vic_count < PG106_EDID_MAX_VICS;
			     i++) {
				uint8_t raw = block[off + 1 + i];
				unsigned idx = edid->cea.vic_count;

				edid->cea.vic[idx] = (uint8_t)(raw & 0x7f);
				edid->cea.vic_native[idx] = (raw & 0x80) != 0;
				edid->cea.vic_count++;
			}
		}
		off += 1 + len;
	}

	for (off = d; off + 18 <= PG106_EDID_BLOCK_SIZE - 1 &&
		      edid->cea.dtd_count < PG106_EDID_MAX_CEA_DTDS;
	     off += 18) {
		if (!slot_is_dtd(block + off))
			break;
		decode_dtd(block + off, &edid->cea.dtd[edid->cea.dtd_count++]);
	}
}

enum pg106_edid_status
pg106_edid_parse(const void *data, size_t len, struct pg106_edid *out)
{
	const uint8_t *buf = data;
	unsigned nblocks, i;

	if (data == NULL || out == NULL)
		return PG106_EDID_ERR_NULL;
	if (len < PG106_EDID_BLOCK_SIZE || len > PG106_EDID_MAX_BYTES ||
	    len % PG106_EDID_BLOCK_SIZE != 0)
		return PG106_EDID_ERR_SIZE;

	nblocks = (unsigned)(len / PG106_EDID_BLOCK_SIZE);

	if (memcmp(buf, header_pattern, sizeof(header_pattern)) != 0)
		return PG106_EDID_ERR_HEADER;

	// Every block sums to 0 mod 256 including its checksum byte 127.
	// The base block must; a bad extension is only dropped.
	uint8_t bad = 0;
	for (i = 0; i < nblocks; i++) {
		const uint8_t *block = buf + i * PG106_EDID_BLOCK_SIZE;
		unsigned sum = 0, j;

		for (j = 0; j < PG106_EDID_BLOCK_SIZE; j++)
			sum += block[j];
		if (sum % 256 != 0) {
			if (i == 0)
				return PG106_EDID_ERR_CHECKSUM;
			bad |= (uint8_t)(1u << i);
		}
	}

	memset(out, 0, sizeof(*out));
	out->bad_blocks = bad;

	decode_manufacturer(buf, out->manufacturer);
	out->product_code   = (uint16_t)(buf[10] | (buf[11] << 8));
	out->serial_number  = (uint32_t)buf[12] | ((uint32_t)buf[13] << 8) |
			      ((uint32_t)buf[14] << 16) |
			      ((uint32_t)buf[15] << 24);
	out->version_major  = buf[18];
	out->version_minor  = buf[19];
	out->digital        = (buf[20] & 0x80) != 0;
	out->preferred_timing_is_dtd = (buf[24] & 0x02) != 0;
	out->established_timings[0]  = buf[35];
	out->established_timings[1]  = buf[36];
	out->established_timings[2]  = buf[37];
	out->extension_count = buf[126];
	out->block_count     = (uint8_t)nblocks;

	// Preferred timing: first DTD slot with a nonzero pixel clock, per
	// E-EDID §3.10 ("First detailed timing" must be the preferred mode
	// when feature byte bit 1 is set, which the reference panel sets).
	for (i = 0; i < 4; i++) {
		const uint8_t *slot = buf + 54 + i * 18;

		if (slot_is_dtd(slot)) {
			decode_dtd(slot, &out->preferred_timing);
			out->has_preferred_timing = true;
			break;
		}
	}

	// Every CTA-861 extension, not just block 1 (a block-map or a
	// DisplayID block may sit in front of a CTA block).
	for (i = 1; i < nblocks; i++) {
		const uint8_t *block = buf + i * PG106_EDID_BLOCK_SIZE;
		if (!(bad & (1u << i)) && block[0] == 0x02)
			parse_cta(block, out);
	}

	return PG106_EDID_OK;
}

uint32_t
pg106_edid_refresh_milli_hz(const struct pg106_edid_timing *t)
{
	uint64_t total;

	if (t == NULL || t->htotal == 0 || t->vtotal == 0)
		return 0;
	total = (uint64_t)t->htotal * t->vtotal;
	return (uint32_t)((uint64_t)t->pixel_clock_khz * 1000000u / total);
}

bool
pg106_edid_cea_has_vic(const struct pg106_edid *edid, uint8_t vic)
{
	unsigned i;

	for (i = 0; i < edid->cea.vic_count; i++) {
		if (edid->cea.vic[i] == vic)
			return true;
	}
	return false;
}

// CTA-861-H Table 46, VIC 16: 1920x1080p @ 60 Hz, 148.5 MHz pixel clock,
// htotal 2200 (Hblank 280: front 88, sync 44, back 148), vtotal 1125
// (Vblank 45: front 4, sync 5, back 36), digital separate sync, both
// polarities positive, progressive — i.e. the canonical HDMI 1080p60 mode.
const struct pg106_edid_timing *
pg106_edid_cea_timing_vic16(void)
{
	static const struct pg106_edid_timing vic16 = {
		.pixel_clock_khz = 148500,
		.hactive = 1920,
		.hblank = 280,
		.hsync_offset = 88,
		.hsync_width = 44,
		.vactive = 1080,
		.vblank = 45,
		.vsync_offset = 4,
		.vsync_width = 5,
		.htotal = 2200,
		.vtotal = 1125,
		.sync = PG106_EDID_SYNC_DIGITAL_SEPARATE,
		.hsync_positive = true,
		.vsync_positive = true,
		.interlaced = false,
	};

	return &vic16;
}
