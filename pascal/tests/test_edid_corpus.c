// SPDX-License-Identifier: MIT
// pascal/tests/test_edid_corpus.c — differential EDID test against edid-decode.
//
// MIT; see pascal/core/include/pascal/edid.h for the licensing note (VESA
// E-EDID/CTA-861 spec facts, no GPL code).
//
// For every dump in research/reference/edid/corpus/ (see that directory's
// MANIFEST.md for provenance: real-world EDIDs from the linuxhw/EDID repo)
// this binary runs BOTH our parser (pg106_edid_*) and /usr/bin/edid-decode
// and compares every field both expose. A field-by-field diff table is
// printed for each file with at least one disagreement; the exit status is
// 0 iff every compared field agrees. scripts/edid_corpus_diff.sh is a thin
// standalone wrapper around this binary; `make test` runs it corpus-wide.
//
// COMPARISON SUBSET (the overlap contract)
// ----------------------------------------
// Our core parser intentionally supports a subset of E-EDID/CTA-861; only
// the overlap with edid-decode is compared. Everything below is documented
// here and SKIPPED (not counted as a mismatch):
//
//   - Established/standard timings: our parser keeps bytes 35..37 raw and
//     never decodes DMT/GTF modes; not compared.
//   - Sync SCHEME (analog / digital composite / digital separate) is not
//     printed by edid-decode in a comparable textual form; instead we
//     compare the polarities: Hpol is always compared, Vpol only when our
//     parser reports anything but digital-composite sync (composite
//     carries no V polarity bit).
//   - Stereo DTDs (flags byte bits 6:5; zero occurrences in the corpus)
//     overload the sync bits; polarity fields are skipped.
//   - Interlaced first DTDs: E-EDID stores per-field vertical values while
//     edid-decode prints full-frame actives/totals, so vactive, vtotal and
//     the derived refresh are skipped; hactive, hblank, vblank and the
//     sync fields are per-field in both and compared.
//   - CTA extended VICs (VIC >= 193, i.e. VIC bytes with bit 7 set under
//     the CTA-861-H reading): our data model records the low 7 bits plus
//     the flag bit, ambiguous between "native VIC" and "extended VIC";
//     files containing them skip the VIC-set comparison. No corpus file
//     exercises this.
//   - Our CTA VIC list caps at PG106_EDID_MAX_VICS (32); if edid-decode
//     lists more, only the first 32 are compared (the cap is noted).
//   - Blobs outside the core's 8-block/1 KiB envelope: skipped whole.
//   - edid-decode-only structure (DisplayID/VTB extensions, range limits,
//     color curves, audio blocks, ...): not modeled by our parser.
//
// Any disagreement WITHIN that overlap is a parser bug and fails the run.
//
// Deterministic synthetic regression vectors (hand-built checksum-corrected
// blocks, same convention as test_edid.c) lock in two parser fixes the
// corpus work uncovered:
//   - DTD byte 11 holds four 2-bit sync MSB extensions ([7:6] H-off,
//     [5:4] H-wid, [3:2] V-off, [1:0] V-wid); a DTD with H offset 300 and
//     V offset 32 must decode exactly, no nibble bleeding between fields.
//   - CTA-861 with DTD offset 0: the data block collection must still be
//     walked to the checksum byte, so a Video Data Block without CEA DTDs
//     yields its VICs.

#define _POSIX_C_SOURCE 200809L // popen/pclose

#include <pascal/edid.h>

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REF_MAX_VICS 64

// Stats across the whole run.
struct stats {
	long fields;      // fields actually compared (overlap only)
	long matched;
	long mismatched;
	long skipped;     // documented-subset fields not compared
	int files;
	int files_failed;
};

// edid-decode's view of the base block's first DTD.
struct ref_dtd {
	bool present;
	bool interlaced;
	unsigned long hactive, vactive, hblank, vblank, htotal, vtotal;
	unsigned long hoff, hwid, voff, vwid;
	long clock_khz;
	long refresh_milli_hz;
	char hpol, vpol;
};

// edid-decode's view of block 1 (CTA presence + Video Data Block VICs).
struct ref_cea {
	bool block1_is_cta;
	bool has_revision;
	long revision;
	bool has_vdb;
	unsigned vic[REF_MAX_VICS];
	bool native[REF_MAX_VICS];
	unsigned vic_count;
	bool vic_extended; // a VIC >= 193: outside the comparable subset
};

// edid-decode's view of base-block identity fields.
struct ref_base {
	char manufacturer[8];
	bool has_manufacturer;
	long model;
	bool has_model;
	long serial;
	bool has_serial;
	long ver_major, ver_minor;
	bool has_version;
	long extension_blocks;
	bool has_extension_blocks;
	bool digital;
	bool has_digital;
};

static struct stats g_stats;

static void *
xmalloc(size_t n)
{
	void *p = malloc(n);

	if (p == NULL) {
		fprintf(stderr, "out of memory\n");
		exit(2);
	}
	return p;
}

// ----------------------------------------------------- edid-decode text --
// Scraping helpers. Everything is line-oriented prefix matching on the
// format printed by the installed edid-decode; unknown shapes simply leave
// the corresponding ref_* field marked absent and the field is skipped.

static const char *
line_end(const char *p, const char *end)
{
	const char *nl = memchr(p, '\n', (size_t)(end - p));

	return nl == NULL ? end : nl;
}

static const char *
section_start(const char *txt, const char *header)
{
	return strstr(txt, header);
}

// Section = text from `start` up to the next block separator line.
static const char *
section_end(const char *start)
{
	const char *sep = strstr(start, "\n----------------");

	return sep == NULL ? start + strlen(start) : sep;
}

static bool
grab_long_between(const char *sec, const char *end, const char *label,
		  long *out)
{
	const char *p = strstr(sec, label);

	if (p == NULL)
		return false;
	p += strlen(label);
	while (p < end && (*p == ' ' || *p == '\t'))
		p++;
	if (p >= end)
		return false;
	if (p[0] == '-' || !isdigit((unsigned char)p[0]))
		return false;
	*out = strtol(p, NULL, 10);
	return true;
}

// The refresh is the "<number> Hz" token of the DTD line.
static bool
grab_hz_milli(const char *line, const char *end, long *out_milli_hz)
{
	const char *p = line;

	while ((p = strstr(p, " Hz")) != NULL && p < end) {
		const char *q = p;

		while (q > line && (isdigit((unsigned char)q[-1]) ||
				    q[-1] == '.'))
			q--;
		if (q < p) {
			double hz = strtod(q, NULL);

			*out_milli_hz = (long)(hz * 1000.0 + 0.5);
			return true;
		}
		p += 3;
	}
	return false;
}

// The pixel clock is the last "<number> MHz" token of the DTD line.
static bool
grab_mhz_khz(const char *line, const char *end, long *out_khz)
{
	const char *p = line;

	while ((p = strstr(p, " MHz")) != NULL && p < end) {
		const char *q = p;

		while (q > line && (isdigit((unsigned char)q[-1]) ||
				    q[-1] == '.'))
			q--;
		if (q < p) {
			double mhz = strtod(q, NULL);

			*out_khz = (long)(mhz * 1000.0 + 0.5);
			return true;
		}
		p += 4;
	}
	return false;
}

static void
parse_ref_dtd(const char *txt, struct ref_dtd *r)
{
	const char *sec, *end, *p, *q;
	char vline[512];

	memset(r, 0, sizeof(*r));

	sec = section_start(txt, "Block 0, Base EDID");
	if (sec == NULL)
		return;
	end = section_end(sec);

	// "DTD 1:" or "DTD  1:" (spacing varies with the numbering width);
	// base section only, so CEA DTDs never leak in.
	for (p = sec; (p = strstr(p, "DTD")) != NULL && p < end; p += 3) {
		q = p + 3;
		while (q < end && *q == ' ')
			q++;
		if (q + 1 < end && q[0] == '1' && q[1] == ':') {
			q += 2;
			break;
		}
	}
	if (p == NULL || p >= end)
		return;

	// q now sits just past "1:", inside the DTD line itself.
	if (sscanf(q, " %lu x %lu", &r->hactive, &r->vactive) != 2)
		return;
	if (!grab_hz_milli(q, end, &r->refresh_milli_hz))
		return;
	if (!grab_mhz_khz(q, end, &r->clock_khz))
		return;

	p = strstr(q, "Hfront");
	if (p == NULL || p >= end)
		return;
	if (sscanf(p, "Hfront %lu Hsync %lu Hback %lu Hpol %c",
		   &r->hoff, &r->hwid, &r->hblank, &r->hpol) != 4)
		return;
	p = strstr(p, "Vfront");
	if (p == NULL || p >= end)
		return;
	{
		const char *le = line_end(p, end);
		size_t n = (size_t)(le - p);

		if (sscanf(p, "Vfront %lu Vsync %lu Vback %lu Vpol %c",
			   &r->voff, &r->vwid, &r->vblank, &r->vpol) != 4)
			return;
		if (n >= sizeof(vline))
			n = sizeof(vline) - 1;
		memcpy(vline, p, n);
		vline[n] = '\0';
		// Interlaced DTDs carry the "+0.5 Odd/Even Field" markers on
		// the Vfront line (edid-decode prints full-frame actives
		// there; see the subset contract in the header comment).
		if (strstr(vline, "Odd Field") != NULL ||
		    strstr(vline, "Even Field") != NULL)
			r->interlaced = true;
	}

	r->hblank += r->hoff + r->hwid;
	r->vblank += r->voff + r->vwid;
	r->htotal = r->hactive + r->hblank;
	r->vtotal = r->vactive + r->vblank;
	r->present = true;
}

static void
parse_ref_cea(const char *txt, struct ref_cea *r)
{
	const char *b1, *end, *p;

	memset(r, 0, sizeof(*r));
	r->block1_is_cta =
		strstr(txt, "Block 1, CTA-861 Extension Block") != NULL;
	if (!r->block1_is_cta)
		return;

	b1 = section_start(txt, "Block 1, CTA-861 Extension Block");
	end = section_end(b1);

	r->has_revision = grab_long_between(b1, end, "Revision:",
					    &r->revision);

	// Exactly "    Video Data Block:" — a "YCbCr 4:2:0 Video Data
	// Block:" line starts with "YCbCr", so it does not match.
	p = strstr(b1, "\n    Video Data Block:");
	if (p == NULL || p >= end)
		return; // CTA block without a video data block
	r->has_vdb = true;

	for (p = line_end(p + 1, end) + 1; p < end;
	     p = line_end(p, end) + 1) {
		const char *le = line_end(p, end);
		char buf[256];
		size_t n = (size_t)(le - p);
		unsigned long vic;
		char *vicp;

		if (n >= sizeof(buf))
			n = sizeof(buf) - 1;
		memcpy(buf, p, n);
		buf[n] = '\0';

		// Lines look like "    VIC  16: 1920x1080 ... (native)".
		vicp = strstr(buf, "VIC");
		if (vicp == NULL)
			break; // next data block: end of the VIC list
		if (sscanf(vicp, "VIC %lu", &vic) != 1)
			break;
		if (r->vic_count < REF_MAX_VICS) {
			r->vic[r->vic_count] = (unsigned)vic;
			r->native[r->vic_count] =
				strstr(buf, "(native)") != NULL;
			r->vic_count++;
		}
		if (vic >= 193)
			r->vic_extended = true;
	}
}

static void
parse_ref_base(const char *txt, struct ref_base *r)
{
	const char *b0 = section_start(txt, "Block 0, Base EDID");
	const char *end, *p;

	memset(r, 0, sizeof(*r));
	if (b0 == NULL)
		return;
	end = section_end(b0);

	p = strstr(b0, "Manufacturer:");
	if (p != NULL && p < end) {
		char man[8] = { 0 };

		if (sscanf(p, "Manufacturer: %7s", man) == 1) {
			memcpy(r->manufacturer, man, sizeof(r->manufacturer));
			r->has_manufacturer = true;
		}
	}
	r->has_model = grab_long_between(b0, end, "Model:", &r->model);
	// First "Serial Number:" is the base-block field (bytes 12..15);
	// descriptor-based serials print later as "Display Product ...".
	r->has_serial = grab_long_between(b0, end, "Serial Number:",
					  &r->serial);
	p = strstr(b0, "EDID Structure Version & Revision:");
	if (p != NULL && p < end &&
	    sscanf(p, "EDID Structure Version & Revision: %ld.%ld",
		   &r->ver_major, &r->ver_minor) == 2)
		r->has_version = true;
	r->has_extension_blocks =
		grab_long_between(b0, end, "Extension blocks:",
				  &r->extension_blocks);
	p = strstr(b0, "Digital display");
	if (p != NULL && p < end) {
		r->digital = true;
		r->has_digital = true;
	}
}

// Capture a whole command's stdout (edid-decode output is at most a few
// dozen KiB for real-world EDIDs).
static char *
capture_cmd(const char *cmd)
{
	FILE *f = popen(cmd, "r");
	size_t cap = 1 << 16, len = 0;
	char *buf;

	if (f == NULL)
		return NULL;
	buf = xmalloc(cap);
	for (;;) {
		size_t n;

		if (len + 4096 + 1 > cap) {
			char *nb = realloc(buf, cap * 2);

			if (nb == NULL) {
				free(buf);
				pclose(f);
				return NULL;
			}
			buf = nb;
			cap *= 2;
		}
		n = fread(buf + len, 1, 4096, f);
		if (n == 0)
			break;
		len += n;
	}
	pclose(f);
	buf[len] = '\0';
	return buf;
}

// ------------------------------------------------------------ compare ---

static bool g_diff_header_shown; // per-file diff table latch

static void
diff_row(const char *field, long ours, long ref)
{
	if (!g_diff_header_shown) {
		printf("   | %-24s | %10s | %12s |\n", "field", "ours",
		       "edid-decode");
		printf("   |--------------------------|------------|---------"
		       "-----|\n");
		g_diff_header_shown = true;
	}
	printf("   | %-24s | %10ld | %12ld |\n", field, ours, ref);
}

static void
note_skip(const char *field, const char *why)
{
	g_stats.skipped++;
	printf("   [skip] %s: %s\n", field, why);
}

static void
cmp_long(const char *field, long ours, long ref)
{
	g_stats.fields++;
	if (ours == ref) {
		g_stats.matched++;
		return;
	}
	g_stats.mismatched++;
	diff_row(field, ours, ref);
}

static void
cmp_bool(const char *field, bool ours, long ref)
{
	cmp_long(field, ours ? 1 : 0, ref);
}

// Compare one file. Returns true when every compared field agrees.
static bool
check_file(const char *path)
{
	struct stats before = g_stats;
	struct pg106_edid e;
	struct ref_dtd rd;
	struct ref_cea rc;
	struct ref_base rb;
	uint8_t *blob = NULL;
	char *cmd, *txt;
	FILE *f;
	long len;
	char name[160];
	const char *slash = strrchr(path, '/');
	const char *corpus = strstr(path, "corpus/");
	bool clean;

	if (corpus != NULL)
		snprintf(name, sizeof(name), "%s", corpus + strlen("corpus/"));
	else
		snprintf(name, sizeof(name), "%s",
			 slash != NULL ? slash + 1 : path);
	g_diff_header_shown = false;

	printf("== %s\n", name);
	g_stats.files++;

	// Explicit args may be absolute paths; keep fopen lenient.
	f = fopen(path, "rb");
	if (f == NULL) {
		g_stats.mismatched++;
		printf("   FAIL: cannot open\n");
		return false;
	}
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len > 0 && (size_t)len <= PG106_EDID_MAX_BYTES) {
		blob = xmalloc((size_t)len);
		if (fread(blob, 1, (size_t)len, f) != (size_t)len) {
			free(blob);
			blob = NULL;
		}
	}
	fclose(f);

	cmd = xmalloc(strlen(path) + 32);
	sprintf(cmd, "edid-decode '%s' 2>&1", path);
	txt = capture_cmd(cmd);
	free(cmd);

	// Files outside the core's 8-block envelope are skipped whole (see
	// the subset contract in the header comment).
	if (blob == NULL) {
		note_skip("file", "unreadable or outside the core's "
			  "128-byte-multiple 1 KiB envelope");
		free(txt);
		free(blob);
		clean = true;
		goto out;
	}

	if (strstr(txt, "Block 0, Base EDID") == NULL) {
		// edid-decode rejected it too: our parser must also reject.
		enum pg106_edid_status st = pg106_edid_parse(blob,
			(size_t)len, &e);

		if (st != PG106_EDID_OK)
			printf("   PASS both parsers reject the blob\n");
		else {
			cmp_long("parse_status", (long)st, -1);
			printf("   FAIL: our parser accepts what edid-decode "
			       "rejects\n");
		}
		free(blob);
		free(txt);
		clean = g_stats.mismatched == before.mismatched;
		goto out;
	}

	if (pg106_edid_parse(blob, (size_t)len, &e) != PG106_EDID_OK) {
		g_stats.mismatched++;
		printf("   FAIL: edid-decode parses this blob, our parser "
		       "rejects it\n");
		free(blob);
		free(txt);
		return false;
	}

	parse_ref_dtd(txt, &rd);
	parse_ref_cea(txt, &rc);
	parse_ref_base(txt, &rb);

	// --- base block identity ---
	if (rb.has_manufacturer)
		cmp_long("manufacturer",
			 strcmp(e.manufacturer, rb.manufacturer) == 0 ?
			 1 : 0, 1);
	else
		note_skip("manufacturer", "not printed by edid-decode");
	if (rb.has_model)
		cmp_long("product_code", e.product_code, rb.model);
	else
		note_skip("product_code", "not printed by edid-decode");
	if (rb.has_serial)
		cmp_long("serial_number", (long)e.serial_number, rb.serial);
	else
		note_skip("serial_number", "not printed by edid-decode");
	if (rb.has_version) {
		cmp_long("version_major", e.version_major, rb.ver_major);
		cmp_long("version_minor", e.version_minor, rb.ver_minor);
	} else
		note_skip("version", "not printed by edid-decode");
	if (rb.has_digital)
		cmp_bool("digital", e.digital, rb.digital ? 1 : 0);
	else
		note_skip("digital", "not printed by edid-decode");
	if (rb.has_extension_blocks)
		cmp_long("extension_count", e.extension_count,
			 rb.extension_blocks);
	else
		note_skip("extension_count", "not printed by edid-decode");

	// --- first DTD ---
	if (rd.present != e.has_preferred_timing) {
		cmp_long("has_preferred_timing",
			 e.has_preferred_timing ? 1 : 0, rd.present ? 1 : 0);
	} else if (rd.present) {
		const struct pg106_edid_timing *t = &e.preferred_timing;
		bool stereo, skip_vertical;
		size_t i;
		uint8_t flags = 0;

		// Raw flags byte of the first DTD slot (bytes 54..71), for
		// the stereo-subset decision only.
		for (i = 0; i < 4; i++) {
			const uint8_t *slot = blob + 54 + i * 18;

			if ((slot[0] | slot[1]) != 0) {
				flags = slot[17];
				break;
			}
		}
		stereo = (flags & 0x60) != 0;
		skip_vertical = rd.interlaced || t->interlaced;

		cmp_bool("dtd1.interlaced", t->interlaced,
			 rd.interlaced ? 1 : 0);
		cmp_long("dtd1.pixel_clock_khz", (long)t->pixel_clock_khz,
			 rd.clock_khz);
		cmp_long("dtd1.hactive", t->hactive, (long)rd.hactive);
		cmp_long("dtd1.hblank", t->hblank, (long)rd.hblank);
		cmp_long("dtd1.htotal", t->htotal, (long)rd.htotal);
		cmp_long("dtd1.hsync_offset", t->hsync_offset,
			 (long)rd.hoff);
		cmp_long("dtd1.hsync_width", t->hsync_width,
			 (long)rd.hwid);
		if (skip_vertical) {
			note_skip("dtd1.vactive", "interlaced: edid-decode "
				  "prints full-frame vs our per-field values");
			note_skip("dtd1.vtotal",
				  "interlaced: full-frame vs per-field");
			note_skip("dtd1.refresh",
				  "interlaced: full-frame vs per-field rate");
			cmp_long("dtd1.vblank", t->vblank, (long)rd.vblank);
		} else {
			long ours_mhz = (long)pg106_edid_refresh_milli_hz(t);
			long delta = ours_mhz - rd.refresh_milli_hz;

			cmp_long("dtd1.vactive", t->vactive,
				 (long)rd.vactive);
			cmp_long("dtd1.vblank", t->vblank, (long)rd.vblank);
			cmp_long("dtd1.vtotal", t->vtotal, (long)rd.vtotal);
			// Both sides compute clock / (htotal*vtotal); the
			// only difference is rounding of the 6-decimals
			// print, so allow 2 millihertz.
			g_stats.fields++;
			if (delta < 0)
				delta = -delta;
			if (delta <= 2)
				g_stats.matched++;
			else {
				g_stats.mismatched++;
				diff_row("dtd1.refresh_milli_hz", ours_mhz,
					 rd.refresh_milli_hz);
			}
		}
		cmp_long("dtd1.vsync_offset", t->vsync_offset,
			 (long)rd.voff);
		cmp_long("dtd1.vsync_width", t->vsync_width,
			 (long)rd.vwid);
		if (stereo) {
			note_skip("dtd1.sync", "stereo DTD: sync bits carry "
				  "stereo semantics");
			note_skip("dtd1.hsync_positive", "stereo DTD");
			note_skip("dtd1.vsync_positive", "stereo DTD");
		} else {
			cmp_long("dtd1.hsync_positive",
				 t->hsync_positive ? 1 : 0,
				 rd.hpol == 'P' ? 1 : 0);
			if (t->sync != PG106_EDID_SYNC_DIGITAL_COMPOSITE)
				cmp_long("dtd1.vsync_positive",
					 t->vsync_positive ? 1 : 0,
					 rd.vpol == 'P' ? 1 : 0);
			else
				note_skip("dtd1.vsync_positive",
					  "digital composite has no V "
					  "polarity bit");
		}
	}

	// --- CTA-861 ---
	cmp_bool("block1_is_cta", e.cea.present, rc.block1_is_cta ? 1 : 0);
	if (e.cea.present && rc.block1_is_cta) {
		if (rc.has_revision)
			cmp_long("cea.revision", e.cea.revision, rc.revision);
		else
			note_skip("cea.revision", "not printed by "
				  "edid-decode");
	}
	if (e.cea.present && rc.block1_is_cta && rc.has_vdb &&
	    !rc.vic_extended) {
		unsigned long capped = rc.vic_count;

		if (capped > PG106_EDID_MAX_VICS) {
			printf("   [note] edid-decode lists %lu VIC bytes; "
			       "comparing the first %u (our cap)\n", capped,
			       PG106_EDID_MAX_VICS);
			capped = PG106_EDID_MAX_VICS;
		}
		cmp_long("cea.vic_count", e.cea.vic_count, (long)capped);
		for (unsigned long i = 0; i < capped && i < rc.vic_count;
		     i++) {
			char field[32];

			snprintf(field, sizeof(field), "cea.has_vic[%u]",
				 rc.vic[i]);
			cmp_bool(field,
				 pg106_edid_cea_has_vic(&e,
				 (uint8_t)rc.vic[i]), 1);
			if (rc.native[i]) {
				// Both parsers keep data-block order, so the
				// native flag is comparable by index.
				snprintf(field, sizeof(field),
					 "cea.native[%u]", rc.vic[i]);
				if (i < e.cea.vic_count)
					cmp_bool(field, e.cea.vic_native[i],
						 1);
				else
					note_skip(field,
						  "VIC index beyond our "
						  "count");
			}
		}
	} else if (e.cea.present && rc.vic_extended) {
		note_skip("cea.vic_set", "extended VICs (>=193) present: "
			  "outside the comparable subset");
	}

	free(blob);
	free(txt);
	clean = g_stats.mismatched == before.mismatched;

out:
	if (clean) {
		long compared = g_stats.fields - before.fields;

		printf("   PASS (%ld field%s compared%s)\n", compared,
		       compared == 1 ? "" : "s",
		       g_stats.skipped - before.skipped > 0 ?
		       ", some skipped" : "");
	} else {
		g_stats.files_failed++;
		printf("   FAIL %s\n", name);
	}
	return clean;
}

// ------------------------------------------------------- corpus walking --

static int
cmp_names(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static char **
corpus_files(const char *dir, size_t *count)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	char **names = NULL;
	size_t n = 0, cap = 0;

	*count = 0;
	if (d == NULL)
		return NULL;
	while ((de = readdir(d)) != NULL) {
		size_t len = strlen(de->d_name);

		if (len < 4 || strcmp(de->d_name + len - 4, ".bin") != 0)
			continue;
		if (n == cap) {
			char **nb = realloc(names, (cap ? cap * 2 : 64) *
					    sizeof(*nb));

			if (nb == NULL)
				break;
			names = nb;
			cap = cap ? cap * 2 : 64;
		}
		names[n] = xmalloc(len + 1);
		memcpy(names[n], de->d_name, len + 1);
		n++;
	}
	closedir(d);
	qsort(names, n, sizeof(*names), cmp_names);
	*count = n;
	return names;
}

// ------------------------------------------------------------ regressions
// Hand-built checksum-corrected blocks (same convention as test_edid.c:
// synthetic bytes only for targeted regression paths, never for timing
// assertions the corpus differential already covers).

static void
fix_checksum(uint8_t *block)
{
	unsigned sum = 0;

	for (unsigned i = 0; i < 127; i++)
		sum += block[i];
	block[127] = (uint8_t)(256u - (sum & 0xffu));
}

// Regression 1 (the corpus differential flagged this shape on
// Apple_APPA033, whose first DTD has Vfront 32): byte 11 of a DTD is four
// 2-bit MSB fields, [7:6] H-off, [5:4] H-wid, [3:2] V-off, [1:0] V-wid. A
// DTD with H sync offset 300 (needs the [9:8] bits) and V sync offset 32
// must decode exactly, with no nibble bleeding between fields.
static void
test_regression_sync_msb(void)
{
	uint8_t block[128];
	struct pg106_edid e;
	const struct pg106_edid_timing *t;

	memset(block, 0, sizeof(block));
	memcpy(block, "\x00\xff\xff\xff\xff\xff\xff\x00", 8);
	block[18] = 1;    // EDID 1.4
	block[20] = 0x80; // digital input
	block[24] = 0x02; // preferred timing is the first DTD
	block[126] = 0;

	// DTD slot 0 (bytes 54..): 148.5 MHz, 1920x1080, hblank 280,
	// vblank 45 (canonical 1080p blanking), H offset 300 / H width 44,
	// V offset 32 / V width 8, digital separate +H +V, progressive.
	block[54] = 0x02; block[55] = 0x3a; // clock word 0x3a02 = 14850 *10 kHz
	block[56] = 0x80; block[57] = 0x18; // hact lo 0x80 / hbl lo 0x18
	block[58] = 0x71;                   // hact hi 7 / hbl hi 1
	block[59] = 0x38; block[60] = 0x2d; // vact lo 0x38 / vbl lo 45
	block[61] = 0x40;                   // vact hi 4 / vbl hi 0
	block[62] = 0x2c;                   // H sync offset low 8 of 300
	block[63] = 44;                     // H sync width
	block[64] = 0x08;                   // V off lo-nib 0 / V wid lo 8
	block[65] = 0x48; // H-off MSB 01 [7:6], V-off MSB 10 [3:2]
	block[71] = 0x1e; // digital separate, +H +V, progressive
	fix_checksum(block);

	if (pg106_edid_parse(block, sizeof(block), &e) != PG106_EDID_OK) {
		printf("FAIL corpus-regression sync-msb: parse rejected\n");
		g_stats.mismatched++;
		return;
	}
	t = &e.preferred_timing;
	if (!e.has_preferred_timing || t->hactive != 1920 ||
	    t->hblank != 280 || t->vactive != 1080 || t->vblank != 45 ||
	    t->htotal != 2200 || t->vtotal != 1125) {
		printf("FAIL corpus-regression sync-msb: base DTD fields "
		       "(hact %u hbl %u vact %u vbl %u)\n",
		       t->hactive, t->hblank, t->vactive, t->vblank);
		g_stats.mismatched++;
		return;
	}
	if (t->hsync_offset != 300 || t->hsync_width != 44 ||
	    t->vsync_offset != 32 || t->vsync_width != 8) {
		printf("FAIL corpus-regression sync-msb: hoff=%u hwid=%u "
		       "voff=%u vwid=%u, want 300/44/32/8\n",
		       t->hsync_offset, t->hsync_width, t->vsync_offset,
		       t->vsync_width);
		g_stats.mismatched++;
		return;
	}
	printf("PASS corpus-regression: DTD byte-11 sync MSBs "
	       "(hoff 300 hwid 44 voff 32 vwid 8)\n");
}

// Regression 2: a CTA-861 extension with DTD offset 0 (no CEA DTDs) must
// still walk the data block collection, so a Video Data Block yields its
// VICs. Before the fix, parse_cta() returned early and reported zero VICs.
static void
test_regression_cta_no_dtd(void)
{
	uint8_t blob[256];
	struct pg106_edid e;

	memset(blob, 0, sizeof(blob));
	memcpy(blob, "\x00\xff\xff\xff\xff\xff\xff\x00", 8);
	blob[18] = 1;
	blob[126] = 1; // one extension follows
	fix_checksum(blob);

	blob[128 + 0] = 0x02; // CTA-861 tag
	blob[128 + 1] = 3;    // revision 3
	blob[128 + 2] = 0;    // DTD offset 0: no CEA DTDs (the bug trigger)
	blob[128 + 3] = 0;
	blob[128 + 4] = (2u << 5) | 2u; // Video Data Block, 2 bytes
	blob[128 + 5] = 16;             // VIC 16
	blob[128 + 6] = 4;              // VIC 4
	fix_checksum(blob + 128);

	if (pg106_edid_parse(blob, sizeof(blob), &e) != PG106_EDID_OK) {
		printf("FAIL corpus-regression cta-no-dtd: parse rejected\n");
		g_stats.mismatched++;
		return;
	}
	if (!e.cea.present || e.cea.revision != 3 || e.cea.vic_count != 2 ||
	    !pg106_edid_cea_has_vic(&e, 16) || !pg106_edid_cea_has_vic(&e, 4)) {
		printf("FAIL corpus-regression cta-no-dtd: present=%d rev=%u "
		       "vic_count=%u vic16=%d vic4=%d\n", e.cea.present,
		       e.cea.revision, e.cea.vic_count,
		       pg106_edid_cea_has_vic(&e, 16),
		       pg106_edid_cea_has_vic(&e, 4));
		g_stats.mismatched++;
		return;
	}
	printf("PASS corpus-regression: CTA DTD-offset-0 still yields "
	       "Video Data Block VICs\n");
}

// ----------------------------------------------------------------- main --

int
main(int argc, char **argv)
{
	static const char *dirs[] = {
		"../research/reference/edid/corpus",
		"research/reference/edid/corpus",
	};
	char **names = NULL;
	size_t n = 0;
	const char *dir = NULL;
	bool all_clean = true;

	// Explicit file arguments (the scripts/edid_corpus_diff.sh path).
	if (argc > 1) {
		for (int i = 1; i < argc; i++)
			if (!check_file(argv[i]))
				all_clean = false;
	} else {
		for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
			names = corpus_files(dirs[i], &n);
			if (names != NULL && n > 0) {
				dir = dirs[i];
				break;
			}
			free(names);
			names = NULL;
		}
		if (names == NULL || n == 0) {
			printf("SKIP edid-corpus: no corpus found under "
			       "research/reference/edid/corpus\n");
			return 0;
		}
		for (size_t i = 0; i < n; i++) {
			char path[512];

			snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
			if (!check_file(path))
				all_clean = false;
		}
	}

	test_regression_sync_msb();
	test_regression_cta_no_dtd();

	printf("\nSUMMARY: %d file(s), %ld field(s) compared, %ld matched, "
	       "%ld mismatched, %ld skipped (documented subset)\n",
	       g_stats.files, g_stats.fields, g_stats.matched,
	       g_stats.mismatched, g_stats.skipped);

	for (size_t i = 0; i < n; i++)
		free(names[i]);
	free(names);

	if (!all_clean || g_stats.mismatched != 0) {
		printf("FAIL edid-corpus: %ld divergence(s) in %d file(s)\n",
		       g_stats.mismatched, g_stats.files_failed);
		return 1;
	}
	printf("PASS edid-corpus\n");
	return 0;
}
