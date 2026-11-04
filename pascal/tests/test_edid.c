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
// The only synthetic data is a tiny hand-built 128-byte block used solely
// for rejection-path tests (bad header, bad checksum) — no synthetic bytes
// feed any timing assertion.
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

	// ... and the extension block's own checksum byte 255.
	memcpy(scratch, real, 256);
	scratch[255] ^= 0x01;
	REQUIRE(pg106_edid_parse(scratch, 256, &e) == PG106_EDID_ERR_CHECKSUM);

	// On error, the parser must not have touched the output struct.
	memset(&e, 0xaa, sizeof(e));
	REQUIRE(pg106_edid_parse(scratch, 256, &e) == PG106_EDID_ERR_CHECKSUM);
	REQUIRE(e.manufacturer[0] == (char)0xaa);

	// Truncation on the real blob: base alone is a valid 128-byte EDID
	// (with the extension missing), so the reject boundary is < 128.
	REQUIRE(pg106_edid_parse(real, 128, &e) == PG106_EDID_OK);
	REQUIRE(!e.cea.present);
	REQUIRE(pg106_edid_parse(real, 0, &e) == PG106_EDID_ERR_SIZE);

	ok("EDID rejects: null/size/header/base+ext checksum, out untouched");
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
