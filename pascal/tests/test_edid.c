// SPDX-License-Identifier: MIT
// pascal/tests/test_edid.c — EDID parser tests against a REAL panel dump.
//
// MIT; see pascal/core/include/pascal/edid.h for the licensing note (VESA
// E-EDID/CTA-861 spec facts, no GPL code).
//
// Positive-path vectors come from the real 256-byte EDID of an Acer
// ED320QR S 1920x1080 monitor, loaded at runtime from
// ../research/reference/edid/acer_ed320qr_s.bin (provenance in that
// directory's README.md; checksum-verified before being committed). The
// expected numbers below were cross-checked against the edid-decode output
// embedded in the original dump.
//
// Synthetic data: a tiny hand-built 128-byte block for rejection-path tests
// (bad header, bad checksum), and hand-built CTA/DisplayID extensions on
// the real base block for the HDMI 2.0 fields (test_hdmi2_blocks). The real
// HDMI 2.0 monitors are in the corpus (test_edid_corpus, test_disp_hw).
//
// This file also prints the parsed timing summary that the Phase 5
// simulated 1080p mode-set sequence consumes (grep for "^TIMING ").

#include <pascal/edid.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define REQUIRE(cond)                                                        \
	do {                                                                 \
		if (!(cond)) {                                               \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                          \
		}                                                            \
	} while (0)

static void
ok(const char *name)
{
	printf("PASS %s\n", name);
}

// Repo layout: the test runs from pascal/ (make) or the repo root; try both.
static bool
load_real_edid(uint8_t buf[256])
{
	static const char *paths[] = {
		"../research/reference/edid/acer_ed320qr_s.bin",
		"research/reference/edid/acer_ed320qr_s.bin",
	};
	FILE *f;
	size_t got;

	for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		f = fopen(paths[i], "rb");
		if (f == NULL)
			continue;
		got = fread(buf, 1, 256, f);
		fclose(f);
		if (got != 256) {
			printf("FAIL edid-load: %s is %zu bytes, want 256\n",
			       paths[i], got);
			return false;
		}
		return true;
	}
	printf("FAIL edid-load: acer_ed320qr_s.bin not found (run from pascal/ "
	       "or repo root)\n");
	return false;
}

// Expected base block facts (cross-checked with the dump's edid-decode run):
// ACR model 0, EDID 1.4, digital input, preferred timing is DTD, 1 extension.
static void
test_real_base(const struct pg106_edid *e)
{
	REQUIRE(strcmp(e->manufacturer, "ACR") == 0);
	REQUIRE(e->product_code == 0);
	REQUIRE(e->serial_number == 0x108020c0u); // bytes c0 20 80 10 LE
	REQUIRE(e->version_major == 1);
	REQUIRE(e->version_minor == 4);
	REQUIRE(e->digital);
	REQUIRE(e->preferred_timing_is_dtd);
	// Bytes 35..37 raw (established timings I & II bitmask; the dump's
	// edid-decode run lists the decoded modes, e.g. 640x480@60, 800x600@60,
	// 1024x768@60, 1280x1024@75).
	REQUIRE(e->established_timings[0] == 0xafu);
	REQUIRE(e->established_timings[1] == 0xcfu);
	REQUIRE(e->established_timings[2] == 0x00u);
	REQUIRE(e->extension_count == 1);
	REQUIRE(e->block_count == 2);
	ok("EDID real: base block ACR/0 serial 0x108020c0 v1.4 digital, 1 ext");
}

// First DTD of the real panel: the canonical 148.5 MHz 1080p60 mode
// (dump's edid-decode: "DTD 1: 1920x1080 60.000000 Hz ... 148.500000 MHz,
// Hfront 88 Hsync 44 Hback 148 Hpol P, Vfront 4 Vsync 5 Vback 36 Vpol P").
static void
test_real_dtd(const struct pg106_edid *e)
{
	const struct pg106_edid_timing *t = &e->preferred_timing;
	uint32_t refresh_mhz;

	REQUIRE(e->has_preferred_timing);
	REQUIRE(t->pixel_clock_khz == 148500);
	REQUIRE(t->hactive == 1920);
	REQUIRE(t->hblank == 280);   // 88 front + 44 sync + 148 back
	REQUIRE(t->htotal == 2200);
	REQUIRE(t->vactive == 1080);
	REQUIRE(t->vblank == 45);    // 4 front + 5 sync + 36 back
	REQUIRE(t->vtotal == 1125);
	REQUIRE(t->hsync_offset == 88);
	REQUIRE(t->hsync_width == 44);
	REQUIRE(t->vsync_offset == 4);
	REQUIRE(t->vsync_width == 5);
	REQUIRE(t->sync == PG106_EDID_SYNC_DIGITAL_SEPARATE);
	REQUIRE(t->hsync_positive);
	REQUIRE(t->vsync_positive);
	REQUIRE(!t->interlaced);

	// Physical plausibility: clock = htotal * vtotal * refresh, and the
	// result must be 60 Hz +/- 2 Hz (gate requirement). In millihertz:
	// 148500e6 / (2200 * 1125) = 60000.
	refresh_mhz = pg106_edid_refresh_milli_hz(t);
	REQUIRE(refresh_mhz >= 58000 && refresh_mhz <= 62000);
	REQUIRE(refresh_mhz == 60000);
	REQUIRE(t->pixel_clock_khz * 1000u ==
		t->htotal * t->vtotal * (refresh_mhz / 1000u));

	ok("EDID real: DTD1 148500 kHz 1920/280 1080/45 digital separate ++p, "
	   "refresh 60.000 Hz");
}

// CTA-861 extension of the real panel: rev 3, VIC 16 (1920x1080@60) present
// and flagged native; VIC 63 (1080p@120) and VIC 31 (1080p@50) also there.
static void
test_real_cea(const struct pg106_edid *e)
{
	REQUIRE(e->cea.present);
	REQUIRE(e->cea.revision == 3);
	REQUIRE(e->cea.vic_count == 12);
	REQUIRE(pg106_edid_cea_has_vic(e, 16));
	REQUIRE(pg106_edid_cea_has_vic(e, 31));
	REQUIRE(pg106_edid_cea_has_vic(e, 63));
	REQUIRE(pg106_edid_cea_has_vic(e, 5));  // 1080i60
	REQUIRE(!pg106_edid_cea_has_vic(e, 32)); // 1080p24 absent
	REQUIRE(!pg106_edid_cea_has_vic(e, 1 + 193)); // no extended VICs

	// The advertised VIC 16 must match the static CTA-861 table timing
	// AND the panel's own preferred DTD (they are the same mode here,
	// which is exactly what the mode-set sequence wants).
	const struct pg106_edid_timing *v16 = pg106_edid_cea_timing_vic16();
	REQUIRE(v16->pixel_clock_khz == 148500);
	REQUIRE(v16->htotal == 2200 && v16->vtotal == 1125);
	REQUIRE(!v16->interlaced);
	REQUIRE(pg106_edid_refresh_milli_hz(v16) == 60000);
	REQUIRE(memcmp(v16, &e->preferred_timing, sizeof(*v16)) == 0);

	// Native flag sits on VIC 16 (index 10 in the short block order).
	REQUIRE(e->cea.vic[10] == 16);
	REQUIRE(e->cea.vic_native[10]);
	REQUIRE(!e->cea.vic_native[0]);

	ok("EDID real: CTA-861 rev3, VIC 16 native, 1080p50/60/120 advertised");
}

// Rejection paths. Synthetic 128-byte block (correct 8-byte header, zeros
// elsewhere — its checksum therefore sums to 0, so any single-byte change
// isolates exactly the rule under test). Real EDID is used for truncation
// and extension-checksum cases.
static void
test_rejections(const uint8_t real[256])
{
	struct pg106_edid e;
	uint8_t syn[128];
	uint8_t scratch[256];

	memset(syn, 0, sizeof(syn));
	memcpy(syn, "\x00\xff\xff\xff\xff\xff\xff\x00", 8);

	REQUIRE(pg106_edid_parse(NULL, 128, &e) == PG106_EDID_ERR_NULL);
	REQUIRE(pg106_edid_parse(syn, 128, NULL) == PG106_EDID_ERR_NULL);

	REQUIRE(pg106_edid_parse(syn, 127, &e) ==
		PG106_EDID_ERR_SIZE); // 127: not a whole block
	REQUIRE(pg106_edid_parse(syn, 100, &e) == PG106_EDID_ERR_SIZE);
	REQUIRE(pg106_edid_parse(real, 129, &e) == PG106_EDID_ERR_SIZE);
	// Above the core's 8-block cap (real-world EDIDs stop far earlier).
	REQUIRE(pg106_edid_parse(syn, 9 * 128, &e) == PG106_EDID_ERR_SIZE);

	// Break the header (byte 1 off the ff run): header must fail even
	// though the (unchanged zero) checksum would still pass.
	syn[1] = 0xfe;
	REQUIRE(pg106_edid_parse(syn, 128, &e) == PG106_EDID_ERR_HEADER);
	syn[1] = 0xff;

	// Now break the checksum: header is valid, so this isolates the sum.
	syn[70] = 1;
	REQUIRE(pg106_edid_parse(syn, 128, &e) == PG106_EDID_ERR_CHECKSUM);
	syn[70] = 0;

	// Same rule on the real blob: corrupt base checksum byte 127 ...
	memcpy(scratch, real, 256);
	scratch[127] ^= 0x01;
	REQUIRE(pg106_edid_parse(scratch, 256, &e) == PG106_EDID_ERR_CHECKSUM);

	// On error, the parser must not have touched the output struct.
	memset(&e, 0xaa, sizeof(e));
	REQUIRE(pg106_edid_parse(scratch, 256, &e) == PG106_EDID_ERR_CHECKSUM);
	REQUIRE(e.manufacturer[0] == (char)0xaa);

	// ... but a bad EXTENSION checksum only drops that block (drm_edid's
	// invalid-block filtering): the base timing survives, CTA is gone.
	memcpy(scratch, real, 256);
	scratch[255] ^= 0x01;
	REQUIRE(pg106_edid_parse(scratch, 256, &e) == PG106_EDID_OK);
	REQUIRE(e.bad_blocks == 0x02 && !e.cea.present &&
		e.has_preferred_timing);

	// Truncation on the real blob: base alone is a valid 128-byte EDID
	// (with the extension missing), so the reject boundary is < 128.
	REQUIRE(pg106_edid_parse(real, 128, &e) == PG106_EDID_OK);
	REQUIRE(!e.cea.present);
	REQUIRE(pg106_edid_parse(real, 0, &e) == PG106_EDID_ERR_SIZE);

	ok("EDID rejects: null/size/header/base checksum (out untouched); bad "
	   "extension dropped, rest kept");
}

// Build a CTA block in place: tag 2, rev 3, byte 2 = d, then fix the
// checksum.
static void
cta_block(uint8_t *b, uint8_t d)
{
	memset(b, 0, 128);
	b[0] = 0x02;
	b[1] = 0x03;
	b[2] = d;
}

static void
fix_sum(uint8_t *b)
{
	unsigned sum = 0;
	for (unsigned i = 0; i < 127; i++)
		sum += b[i];
	b[127] = (uint8_t)(0x100 - (sum & 0xff));
}

static void
test_cta_rules(const uint8_t *real)
{
	static uint8_t blob[4 * 128];
	struct pg106_edid e;
	const uint8_t dtd1080p[18] = { // 148.50 MHz 1920x1080, +h +v
		0x02, 0x3a, 0x80, 0x18, 0x71, 0x38, 0x2d, 0x40, 0x58, 0x2c,
		0x45, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1e };

	// Base block (from the real EDID) declaring 3 extensions: block 1 a
	// non-CTA placeholder (tag 0x70 DisplayID-like), block 2 CTA with a
	// VDB {VIC 16 native, VIC 4} and one DTD, block 3 CTA with d = 0.
	memcpy(blob, real, 128);
	blob[126] = 3;
	fix_sum(blob);
	memset(blob + 128, 0, 128);
	blob[128] = 0x70;
	fix_sum(blob + 128);
	cta_block(blob + 256, 4 + 3);
	blob[256 + 3] = 0x01;              // 1 native DTD
	blob[256 + 4] = 0x40 | 2;          // VDB, 2 bytes
	blob[256 + 5] = 0x90;              // VIC 16 native
	blob[256 + 6] = 0x04;              // VIC 4
	memcpy(blob + 256 + 7, dtd1080p, 18);
	fix_sum(blob + 256);
	cta_block(blob + 384, 0);          // d = 0: nothing to parse ...
	blob[384 + 4] = 0x40 | 1;          // ... even though bytes follow
	blob[384 + 5] = 0x05;
	fix_sum(blob + 384);

	REQUIRE(pg106_edid_parse(blob, sizeof(blob), &e) == PG106_EDID_OK);
	REQUIRE(e.cea.present && e.cea.block_count == 2);
	REQUIRE(e.cea.vic_count == 2 && e.cea.vic[0] == 16 &&
		e.cea.vic_native[0] && e.cea.vic[1] == 4);
	REQUIRE(!pg106_edid_cea_has_vic(&e, 5)); // d = 0 block ignored
	REQUIRE(e.cea.dtd_count == 1 && e.cea.native_dtds == 1);
	REQUIRE(e.cea.dtd[0].pixel_clock_khz == 148500 &&
		e.cea.dtd[0].htotal == 2200 && e.cea.dtd[0].vtotal == 1125);

	// d = 4: DTDs at byte 4, no data block collection.
	cta_block(blob + 384, 4);
	memcpy(blob + 384 + 4, dtd1080p, 18);
	memcpy(blob + 384 + 22, dtd1080p, 18);
	fix_sum(blob + 384);
	REQUIRE(pg106_edid_parse(blob, sizeof(blob), &e) == PG106_EDID_OK);
	REQUIRE(e.cea.dtd_count == 3 && e.cea.vic_count == 2);

	// d = 2 is malformed: block counted, nothing parsed from it.
	cta_block(blob + 384, 2);
	fix_sum(blob + 384);
	REQUIRE(pg106_edid_parse(blob, sizeof(blob), &e) == PG106_EDID_OK);
	REQUIRE(e.cea.block_count == 2 && e.cea.dtd_count == 1);

	// Analog DTD flags: bit 3 bipolar, 2 serrations, 1 sync-on-RGB — no
	// polarity. Digital composite: 2 serrations, 1 H polarity.
	memcpy(blob, real, 128);
	blob[126] = 0;
	blob[54 + 17] = 0x0e;              // analog, bipolar|serr|rgb
	fix_sum(blob);
	REQUIRE(pg106_edid_parse(blob, 128, &e) == PG106_EDID_OK);
	REQUIRE(e.preferred_timing.sync == PG106_EDID_SYNC_ANALOG &&
		e.preferred_timing.analog_bipolar &&
		e.preferred_timing.serrations && e.preferred_timing.sync_on_rgb &&
		!e.preferred_timing.hsync_positive &&
		!e.preferred_timing.vsync_positive);
	blob[54 + 17] = 0x16;              // digital composite, serr, +H
	fix_sum(blob);
	REQUIRE(pg106_edid_parse(blob, 128, &e) == PG106_EDID_OK);
	REQUIRE(e.preferred_timing.sync == PG106_EDID_SYNC_DIGITAL_COMPOSITE &&
		e.preferred_timing.serrations &&
		e.preferred_timing.hsync_positive);
	ok("EDID CTA: every CTA block scanned, CTA DTDs parsed, d=0 means no "
	   "DTDs and no data blocks, d=4 DTD-only; analog sync bits decoded "
	   "as bipolar/serrations/sync-on-RGB");
}

// HDMI 2.0 data blocks, extended VIC bytes and DisplayID timings: the
// fields the display driver's TMDS limit and mode list read. Synthetic
// blocks (checksum-corrected) on top of the real base block; field layouts
// from HDMI 1.4b §8.3.2, HDMI 2.0 §10.3.2, HDMI 2.1 §10.3.6, CTA-861-F
// §7.5.1 and DisplayID 1.3 §4.4.1 / 2.0 §4.1.
static void
test_hdmi2_blocks(const uint8_t *real)
{
	uint8_t blob[384];
	struct pg106_edid e;
	uint8_t *c = blob + 128, *d = blob + 256;
	unsigned o = 4;

	memcpy(blob, real, 128);
	blob[126] = 2;
	fix_sum(blob);
	memset(c, 0, 128);
	c[0] = 0x02; c[1] = 0x03; c[3] = 0x40;
	// VDB: VIC 97, native VIC 16 (0x90), VIC 193 (0xc1), reserved 0x80.
	c[o++] = 0x40 | 4; c[o++] = 97; c[o++] = 0x90; c[o++] = 0xc1; c[o++] = 0x80;
	// HDMI LLC VSDB: PA 1.0.0.0, flags 0xb8 (AI, DC_36, DC_30, Y444), 300 MHz.
	c[o++] = 0x60 | 7; c[o++] = 0x03; c[o++] = 0x0c; c[o++] = 0x00;
	c[o++] = 0x10; c[o++] = 0x00; c[o++] = 0xb8; c[o++] = 60;
	// HF-VSDB: v1, 600 MHz, SCDC + LTE_340 (0x88), DC_30_420 (1).
	c[o++] = 0x60 | 7; c[o++] = 0xd8; c[o++] = 0x5d; c[o++] = 0xc4;
	c[o++] = 1; c[o++] = 120; c[o++] = 0x88; c[o++] = 0x01;
	// YCbCr 4:2:0 VDB: VIC 96 only as 4:2:0; 4:2:0 capability map.
	c[o++] = 0xe0 | 2; c[o++] = 0x0e; c[o++] = 96;
	c[o++] = 0xe0 | 2; c[o++] = 0x0f; c[o++] = 0x01;
	c[o++] = 0xe0 | 2; c[o++] = 0x78; c[o++] = 2;	// HF-EEODB: 2 extensions
	c[2] = (uint8_t)o;
	fix_sum(c);
	// DisplayID 1.3: Type I 2560x1440@144 (583.60 MHz, 10 kHz units),
	// Type VII 1920x1080@240 (571.00 MHz, kHz units, preferred).
	memset(d, 0, 128);
	d[0] = 0x70; d[1] = 0x13; d[2] = 2 * 23; d[3] = 0x03;
	{
		uint8_t *t = d + 5;
		const struct { uint8_t tag; uint32_t clk; uint8_t opt; uint16_t v[8]; } td[2] = {
			{ 0x03, 58360, 0x00, { 2560, 160, 48, 32, 1440, 41, 3, 5 } },
			{ 0x22, 571000, 0x80, { 1920, 160, 48, 32, 1080, 64, 3, 5 } },
		};
		for (int k = 0; k < 2; k++, t += 23) {
			uint32_t clk = td[k].clk - 1;
			t[0] = td[k].tag; t[1] = 0; t[2] = 20;
			t[3] = (uint8_t)clk; t[4] = (uint8_t)(clk >> 8); t[5] = (uint8_t)(clk >> 16);
			t[6] = td[k].opt;
			for (int f = 0; f < 8; f++) {
				uint16_t v = (uint16_t)(td[k].v[f] - 1);
				if (f == 2 || f == 6)
					v |= 0x8000;	// positive polarity
				t[7 + f * 2] = (uint8_t)v;
				t[8 + f * 2] = (uint8_t)(v >> 8);
			}
		}
	}
	fix_sum(d);

	REQUIRE(pg106_edid_parse(blob, sizeof(blob), &e) == PG106_EDID_OK);
	REQUIRE(e.cea.vic_count == 3);
	REQUIRE(e.cea.vic[0] == 97 && !e.cea.vic_native[0]);
	REQUIRE(e.cea.vic[1] == 16 && e.cea.vic_native[1]);
	REQUIRE(e.cea.vic[2] == 193 && !e.cea.vic_native[2]);
	REQUIRE(e.cea.hdmi_vsdb && e.cea.hdmi_flags == 0xb8);
	REQUIRE(e.cea.hdmi_max_tmds_khz == 300000);
	REQUIRE(e.cea.hf_vsdb && e.cea.hf_version == 1);
	REQUIRE(e.cea.max_tmds_khz == 600000);
	REQUIRE(e.cea.scdc_present && !e.cea.rr_capable && e.cea.lte_340_scramble);
	REQUIRE(e.cea.hf_dc420 == 1);
	REQUIRE(e.cea.y420_vic_count == 1 && e.cea.y420_vic[0] == 96 && e.cea.y420_cmdb);
	REQUIRE(e.cea.eeodb_blocks == 2);
	REQUIRE(e.displayid.present && e.displayid.version == 0x13);
	REQUIRE(e.displayid.dtd_count == 2);
	{
		const struct pg106_edid_timing *a = &e.displayid.dtd[0], *b = &e.displayid.dtd[1];
		REQUIRE(a->pixel_clock_khz == 583600 && a->hactive == 2560 && a->htotal == 2720 &&
			a->hsync_offset == 48 && a->hsync_width == 32 && a->vactive == 1440 &&
			a->vtotal == 1481 && a->vsync_offset == 3 && a->vsync_width == 5 &&
			a->hsync_positive && a->vsync_positive && !e.displayid.preferred[0]);
		REQUIRE(pg106_edid_refresh_milli_hz(a) / 1000 == 144);
		REQUIRE(b->pixel_clock_khz == 571000 && b->hactive == 1920 && b->vtotal == 1144 &&
			e.displayid.preferred[1]);
		REQUIRE(pg106_edid_refresh_milli_hz(b) / 1000 == 239);
	}

	// HF-SCDB (extended tag 0x79) instead of the HF-VSDB: same payload.
	memset(c + 4, 0, 123);
	o = 4;
	c[o++] = 0xe0 | 7; c[o++] = 0x79; c[o++] = 0; c[o++] = 0;
	c[o++] = 1; c[o++] = 118; c[o++] = 0xc0; c[o++] = 0;
	c[2] = (uint8_t)o;
	fix_sum(c);
	REQUIRE(pg106_edid_parse(blob, 256, &e) == PG106_EDID_OK);
	REQUIRE(e.cea.hf_vsdb && e.cea.max_tmds_khz == 590000 && e.cea.scdc_present &&
		e.cea.rr_capable && !e.cea.lte_340_scramble && !e.cea.hdmi_vsdb);
	REQUIRE(!e.displayid.present);	// 256 bytes: block 2 not supplied

	// New CTA timings the 4K60 / high-refresh mode list needs.
	REQUIRE(pg106_edid_cea_timing(97)->pixel_clock_khz == 594000 &&
		pg106_edid_cea_timing(97)->htotal == 4400);
	REQUIRE(pg106_edid_cea_timing(96)->htotal == 5280);
	REQUIRE(pg106_edid_cea_timing(102)->hactive == 4096 &&
		pg106_edid_cea_timing(102)->htotal == 4400);
	REQUIRE(pg106_edid_refresh_milli_hz(pg106_edid_cea_timing(63)) == 120000);
	REQUIRE(pg106_edid_refresh_milli_hz(pg106_edid_cea_timing(64)) == 100000);
	REQUIRE(pg106_edid_refresh_milli_hz(pg106_edid_cea_timing(47)) == 120000);
	REQUIRE(pg106_edid_refresh_milli_hz(pg106_edid_cea_timing(60)) == 24000);
	REQUIRE(pg106_edid_refresh_milli_hz(pg106_edid_cea_timing(101)) == 50000);
	ok("edid: HDMI VSDB / HF-VSDB / HF-SCDB / HF-EEODB fields, extended VIC bytes, 4:2:0 blocks, "
	   "DisplayID Type I + VII timings, CTA VICs 41/47/60-64/96/98-102");
}

int
main(void)
{
	uint8_t real[256];
	struct pg106_edid e;

	if (!load_real_edid(real))
		return 1;

	REQUIRE(pg106_edid_parse(real, 256, &e) == PG106_EDID_OK);
	test_real_base(&e);
	test_real_dtd(&e);
	test_real_cea(&e);
	test_rejections(real);
	test_cta_rules(real);
	test_hdmi2_blocks(real);

	// Timing summary for the Phase 5 simulated mode-set sequence. The
	// numbers after "TIMING " are machine-readable; a sibling agent
	// consumes these, not the parser API.
	const struct pg106_edid_timing *t = &e.preferred_timing;
	printf("-- parsed timing summary (Acer ED320QR S, REAL EDID, "
	       "research/reference/edid/) --\n");
	printf("TIMING source=Acer-ED320QR-S-edid.bin base-dtd "
	       "clock_khz=%u hactive=%u hblank=%u htotal=%u "
	       "hsync_offset=%u hsync_width=%u vactive=%u vblank=%u vtotal=%u "
	       "vsync_offset=%u vsync_width=%u hsync_pos=%d vsync_pos=%d "
	       "interlaced=%d sync=%d refresh_mhz=%u\n",
	       t->pixel_clock_khz, t->hactive, t->hblank, t->htotal,
	       t->hsync_offset, t->hsync_width, t->vactive, t->vblank,
	       t->vtotal, t->vsync_offset, t->vsync_width,
	       t->hsync_positive ? 1 : 0, t->vsync_positive ? 1 : 0,
	       t->interlaced ? 1 : 0, (int)t->sync,
	       pg106_edid_refresh_milli_hz(t));
	const struct pg106_edid_timing *v16 = pg106_edid_cea_timing_vic16();
	printf("TIMING source=Acer-ED320QR-S-edid.bin cea-vic16 found=%d "
	       "clock_khz=%u htotal=%u vtotal=%u refresh_mhz=%u "
	       "matches_base_dtd=%d\n",
	       pg106_edid_cea_has_vic(&e, 16) ? 1 : 0,
	       v16->pixel_clock_khz, v16->htotal, v16->vtotal,
	       pg106_edid_refresh_milli_hz(v16),
	       memcmp(v16, t, sizeof(*v16)) == 0 ? 1 : 0);
	printf("-- end summary --\n");

	if (failures != 0) {
		printf("FAIL edid-suite: %d assertion(s) failed\n", failures);
		return 1;
	}
	printf("PASS edid-suite\n");
	return 0;
}
