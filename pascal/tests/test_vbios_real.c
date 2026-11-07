// SPDX-License-Identifier: GPL-2.0-only
// pascal/tests/test_vbios_real.c — BIT parser against REAL VBIOS images.
//
// Phase 1 gate, real-ROM edition (HANDOFF-PASCAL.md §6 Phase 1): loads the
// GP10x ROMs from research/firmware/vbios/ (provenance + sha256 there), and
// for each one:
//   1. pg106_vbios_rom_probe — 0x55aa header, PCIR vendor/device, checksum,
//   2. pg106_vbios_bit_parse — BIT signature + token directory walk,
//   3. prints the FULL token directory (id/version/len/offset) as evidence,
//   4. resolves the init-script table via token 'I' and prints each script
//      index/offset (script 0 = pre-POST; nvbios_post runs scripts from
//      index 0 until the first zero entry, bios-init/init.c:2329-2337),
//   5. decodes the 'i' version string,
//   6. asserts cross-ROM invariants: BIT header + directory in bounds,
//      every token offset+length within the image, script[0] nonzero and
//      in-image, every printed script offset in-image.
//
// Missing ROMs are a PASS-skip (CI-safe: the test binary runs from pascal/
// via the wildcard Makefile; the ROMs are committed, so normally present).
//
// THIS IS SIMULATION, NOT HARDWARE EVIDENCE (HANDOFF-PASCAL.md §7): a
// parsed ROM is host-side evidence about real firmware bytes, not about
// any GPU that is present and running.

#include <pascal/vbios_bit.h>

#include <errno.h>
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

// The committed ROM set (research/firmware/vbios/README.md). Paths are
// relative to pascal/ — the Makefile runs test binaries from there.
static const struct {
	const char *file;
	const char *card;
	uint16_t vendor; // expected PCIR vendor_id
	uint16_t device; // expected PCIR device_id
	const char *version; // expected 'i' decode, "major.chip.minor.micro.patch"
} ROMS[] = {
	{ "nvidia-gtx1060-3gb-tpu208872.rom", "GTX 1060 3GB (GP106)", 0x10de, 0x1c02, "70.26.23.00.01" },
	{ "gigabyte-gtx1060-6gb-tpu186364.rom", "GTX 1060 6GB (GP106)", 0x10de, 0x1c03, "86.06.0e.00.c2" },
	{ "asus-gtx1050ti-tpu188459.rom", "GTX 1050 Ti (GP107)", 0x10de, 0x1c82, "86.07.22.00.6a" },
	{ "msi-gtx1050-tpu193426.rom", "GTX 1050 (GP107)", 0x10de, 0x1c81, "86.07.39.00.85" },
	{ "msi-gtx1050ti-mobile-tpu241139.rom", "GTX 1050 Ti Mobile (GP107M)", 0x10de, 0x1c8c, "86.07.2b.00.00" },
};

#define ROM_DIR "../research/firmware/vbios/"
#define MAX_ROM (1u << 19) // 512 KiB, well above any committed image

// Number of init-script entries printed/asserted per ROM. Real tables are
// short; nvbios_post stops at the first zero entry (bios-init/init.c:2331).
#define N_SCRIPTS 8

static uint8_t rombuf[MAX_ROM];

static size_t
load_rom(const char *name)
{
	char path[256];
	snprintf(path, sizeof(path), ROM_DIR "%s", name);
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	size_t n = fread(rombuf, 1, MAX_ROM, f);
	fclose(f);
	return n;
}

// One full pass over a ROM: probe, BIT walk, token dump, script table.
// Returns the parsed BIT (or skips). `loaded` counts successful loads.
static void
test_rom(const char *file, const char *card, uint16_t want_vendor,
	 uint16_t want_device, const char *want_version, int *loaded)
{
	size_t size = load_rom(file);
	if (size == 0) {
		// PASS-skip, not a failure: CI machines without the ROM
		// checkout still get a green run.
		printf("PASS-SKIP %s: " ROM_DIR "%s not present\n", card, file);
		return;
	}
	REQUIRE(size >= 0x1000 && size <= MAX_ROM);
	printf("-- %s (%s): %zu bytes\n", file, card, size);
	(*loaded)++;

	// --- 1. ROM header / PCIR probe -----------------------------------
	struct pg106_vbios_rom rom;
	REQUIRE(pg106_vbios_rom_probe(rombuf, (uint32_t)size, &rom) == 0);
	REQUIRE(rom.rom_sig_ok); // 0x55aa at offset 0
	REQUIRE(rom.pcir_ok);
	REQUIRE(rom.vendor_id == want_vendor);
	REQUIRE(rom.device_id == want_device);
	REQUIRE(rom.image_size > 0 && rom.image_size <= size);
	// image.c:56-60 + npde.c:55: an NPDE after the PCIR overrides the
	// image size. Independent raw-byte oracle for that rule:
	{
		uint32_t pc = (uint32_t)(rombuf[0x18] | rombuf[0x19] << 8);
		uint32_t hdr = (uint32_t)(rombuf[pc + 0x0a] | rombuf[pc + 0x0b] << 8);
		uint32_t np = (pc + hdr + 0x0f) & ~0x0fu;
		uint32_t want = (uint32_t)(rombuf[pc + 0x10] |
					   rombuf[pc + 0x11] << 8) * 512;
		if (rombuf[pc + 0x14] != 0x70 && np + 0x0b <= size &&
		    !memcmp(&rombuf[np], "NPDE", 4))
			want = (uint32_t)(rombuf[np + 8] | rombuf[np + 9] << 8) * 512;
		REQUIRE(rom.image_size == want);
		// The GP107M dump: PCIR says 0x28000 (whole flash), NPDE 0xf000.
		if (want_device == 0x1c8c)
			REQUIRE(rom.npde_ok && rom.pcir_image_size == 0x28000 &&
				rom.image_size == 0xf000);
	}
	// Expansion-ROM checksum over the declared image: committed ROMs
	// are unmodified, so the sum must be exactly 0 (nvbios_shadow scores
	// this; shadow.c:84-97 tolerates failure but real dumps don't fail).
	REQUIRE(rom.checksum == 0);
	printf("   PCIR @0x%04x: %04x:%04x rev %u image %#x (PCIR %#x, NPDE "
	       "%s)/%#x type %#x last %d checksum %02x\n",
	       rom.pcir_offset, rom.vendor_id, rom.device_id, rom.image_rev,
	       rom.image_size, rom.pcir_image_size, rom.npde_ok ? "yes" : "no",
	       (unsigned)size, rom.image_type, rom.last_image, rom.checksum);
	for (int ix = 0;; ix++) {
		struct pg106_vbios_image im;
		if (pg106_vbios_image(rombuf, (uint32_t)size, ix, &im))
			break;
		printf("   image[%d] base %#x size %#x type %#x%s\n", ix,
		       im.base, im.size, im.type, im.last ? " last" : "");
	}

	// --- 2. BIT locate + directory validation -------------------------
	struct pg106_vbios_bit vb;
	int rc = pg106_vbios_bit_parse(rombuf, (uint32_t)size, &vb);
	REQUIRE(rc == 0);
	if (rc != 0)
		return;
	// Cross-ROM invariant: BIT signature inside the image, header +
	// whole token directory inside the image (parser asserts this too;
	// re-derived here from the parse results as evidence).
	REQUIRE(vb.bit_offset > 0 && vb.bit_offset + PG106_VBIOS_BIT_HDR_LEN +
		     (uint32_t)vb.entry_size * vb.entry_count <= size);
	REQUIRE(vb.entry_size >= PG106_VBIOS_BIT_ENTRY_RD_LEN);
	REQUIRE(vb.entry_count > 0);
	printf("   BIT @0x%04x: %u tokens, stride %u; remap image0 %#x -> "
	       "imaged %#x\n", vb.bit_offset, vb.entry_count, vb.entry_size,
	       vb.image0_size, vb.imaged_addr);
	REQUIRE(vb.image0_size == rom.image_size);

	// --- 3. full token directory as evidence --------------------------
	printf("   token directory:\n");
	for (unsigned i = 0; i < vb.entry_count; i++) {
		const uint8_t *e = rombuf + vb.bit_offset +
				   PG106_VBIOS_BIT_HDR_LEN +
				   (size_t)i * vb.entry_size;
		uint8_t id = e[0], version = e[1];
		uint16_t len = (uint16_t)(e[2] | e[3] << 8);
		uint16_t off = (uint16_t)(e[4] | e[5] << 8);
		// Invariant: every token's table lies inside the image.
		// (u16 offset+length against a >=64 KiB image; both fields
		// are image-absolute per bit_entry/bit.c:34-38.)
		REQUIRE((uint32_t)off + len <= size);
		printf("     '%c' v%u len %5u off 0x%04x\n", id >= 0x20 && id < 0x7f ? id : '?',
		       version, len, off);
	}

	// --- 4. init script table via token 'I' ---------------------------
	struct pg106_vbios_bit_entry bit_i, bit_i_ver;
	REQUIRE(pg106_vbios_bit_entry(&vb, 'I', &bit_i) == 0);
	// Every committed GP10x ROM carries 'I' v1 — the version
	// gm200_devinit_post requires (gm200.c:134). (An older header comment
	// said "'I' is 2 on G80+"; the real ROMs say otherwise.)
	REQUIRE(bit_i.version == 1);
	uint32_t table;
	rc = pg106_vbios_init_script_table(&vb, &bit_i, &table);
	REQUIRE(rc == 0);
	REQUIRE(table != 0 && table < size);
	printf("   'I' v%u len %u off 0x%04x -> script table @0x%04x\n",
	       bit_i.version, bit_i.length, bit_i.offset, table);

	printf("   init scripts:\n");
	for (unsigned i = 0; i < N_SCRIPTS; i++) {
		uint32_t off;
		rc = pg106_vbios_init_script(&vb, &bit_i, (uint16_t)i, &off);
		REQUIRE(rc == 0);
		// nvbios_post runs scripts 0,1,2,... while nonzero
		// (bios-init/init.c:2331): nonzero entries are script
		// offsets and must be in-image. Zeros past the real list
		// are legal; a zero at index 0 is not (no scripts at all).
		if (off != 0)
			REQUIRE(off < size);
		if (i == 0)
			REQUIRE(off != 0);
		printf("     [%u] 0x%04x%s\n", i, off,
		       off ? "" : "  (terminates nvbios_post walk)");
	}

	// --- 5. version token 'i' -----------------------------------------
	REQUIRE(pg106_vbios_bit_entry(&vb, 'i', &bit_i_ver) == 0);
	struct pg106_vbios_version ver;
	REQUIRE(pg106_vbios_version(&vb, &bit_i_ver, &ver) == 0);
	// nvkm order: major.minor is "84.06"-style; committed set is pinned.
	char got[32];
	snprintf(got, sizeof(got), "%02x.%02x.%02x.%02x.%02x", ver.major,
		 ver.chip, ver.minor, ver.micro, ver.patch);
	REQUIRE(strcmp(got, want_version) == 0);
	printf("   version %s (chip %02x)\n", got, ver.chip);

	// --- 6. BIT 'p' PMU application table (pmu.c) -----------------------
	{
		uint32_t t;
		uint8_t pv, ph, pc, pl;
		int pr = pg106_vbios_pmu_table(&vb, &t, &pv, &ph, &pc, &pl);
		struct pg106_vbios_pmu_app app;
		if (pr == 0) {
			printf("   'p' table @0x%x ver %u hdr %u len %u cnt %u\n",
			       t, pv, ph, pl, pc);
			REQUIRE(pg106_vbios_pmu_app(&vb, PG106_VBIOS_PMU_TYPE_DEVINIT,
						    &app) == 0);
			printf("   DEVINIT app @0x%x code %#x B data %#x B\n",
			       app.desc, app.code_size, app.data_size);
		} else {
			printf("   'p' table: %s (rc %d) — the PMU DEVINIT ucode "
			       "is not in this file\n",
			       pr == -EFAULT ? "points past the end of the image" :
			       "absent", pr);
		}
	}

	// --- 7. DCB outputs (dcb.c) — HDMI/DP entries the display uses ------
	{
		struct pg106_dcb_outp o;
		unsigned nd = 0;
		for (uint8_t ix = 0; pg106_vbios_dcb_outp(&vb, ix, &o) == 0; ix++) {
			uint8_t ct = 0xff;
			if (o.type == PG106_DCB_OUTPUT_EOL)
				break;
			if (o.type == PG106_DCB_OUTPUT_UNUSED)
				continue;
			(void)pg106_vbios_conn_type(&vb, o.connector, &ct);
			printf("   dcb[%u] type %u or %#x link %u conn %u (type "
			       "0x%02x) i2c %u heads %#x%s\n", ix, o.type,
			       o.or_mask, o.link, o.connector, ct, o.i2c_index,
			       o.heads, o.type == PG106_DCB_OUTPUT_DP ? " DP" : "");
			nd++;
		}
		REQUIRE(nd > 0);
	}

	ok(card);
}

int
main(void)
{
	printf("vbios REAL-image tests — parser evidence on real NVIDIA ROMs, "
	       "not device evidence (research/firmware/vbios/README.md)\n");

	// Sanity: the synthetic-image knowledge still holds on real bytes —
	// the signature constant is the 5-byte "\xff\xb8BIT".
	REQUIRE(PG106_VBIOS_BIT_SIG_LEN == 5);

	int loaded = 0;
	for (size_t i = 0; i < sizeof(ROMS) / sizeof(ROMS[0]); i++)
		test_rom(ROMS[i].file, ROMS[i].card, ROMS[i].vendor,
			 ROMS[i].device, ROMS[i].version, &loaded);

	// At least one ROM must have parsed for this run to mean anything;
	// with the committed set present all five do.
	if (loaded == 0) {
		printf("PASS-SKIP vbios real: no ROM under " ROM_DIR " (fetch "
		       "them per research/firmware/vbios/README.md); nothing "
		       "real was parsed\n");
		return EXIT_SUCCESS;
	}
	if (loaded == (int)(sizeof(ROMS) / sizeof(ROMS[0]))) {
		char msg[80];
		snprintf(msg, sizeof(msg),
			 "vbios: all %d real GP10x ROMs parsed (probe+BIT+scripts+version)",
			 loaded);
		ok(msg);
	}

	if (failures) {
		printf("%d check(s) FAILED\n", failures);
		return EXIT_FAILURE;
	}
	printf("all checks passed\n");
	return EXIT_SUCCESS;
}
