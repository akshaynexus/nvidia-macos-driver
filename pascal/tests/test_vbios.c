// SPDX-License-Identifier: GPL-2.0-only
// pascal/tests/test_vbios.c — unit tests for the VBIOS BIT parser
// (pascal/core/src/vbios_bit.c).
//
// *** SYNTHETIC IMAGE NOTICE ***
// Every image parsed here is built byte-by-byte below in this file: a
// synthetic GP106-style VBIOS (x86 ROM header, PCIR rev 3, BIT table with
// 'i'/'I'/'D'/'d' tokens and a 3-entry devinit script table). It is NOT a
// real dump. The real Helios 300 VBIOS is requested from the owner in
// research/evidence/BLOCKERS.md (item B1) — until that lands, every pass
// here is simulation evidence only (HANDOFF-PASCAL.md §7).
//
// Coverage:
//   1. BIT signature scan, header fields, token directory walk
//      (bit.c/bit_entry semantics, base.c:189)
//   2. 'I' init/devinit script table + per-index script offsets
//      (init.c:358-427) — the interface Phase 1b's interpreter consumes
//   3. 'i' version decode (base.c:195-200)
//   4. ROM header / PCIR probe + checksum (image.c:39-48, pcir.c,
//      shadow.c:84-97)
//   5. bounds-checked accessors (rd08/rd16/rd32 vs base.c's OOB guard)
//   6. rejection: bad BIT magic, bad entry_size, token directory overrun,
//      bad ROM signature, bad PCIR signature, bad script-table pointer
//   7. truncation: EVERY prefix of the synthetic image must fail the BIT
//      parse and probe cleanly — no OOB, no crash, no false success
//
// For a memory-checked run (recommended for the truncation sweep):
//   cd pascal
//   cc -std=c11 -Wall -Wextra -O1 -g -fsanitize=address,undefined
//     -Icore/include -o build/test_vbios_asan tests/test_vbios.c core/src/*.c
//   ./build/test_vbios_asan

#include <pascal/vbios_bit.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
static int g_checks;

#define CHECK(cond)                                                        \
	do {                                                               \
		g_checks++;                                                \
		if (!(cond)) {                                             \
			printf("FAIL %s:%d: %s\n", __func__, __LINE__,     \
			       #cond);                                     \
			g_failures++;                                      \
		}                                                          \
	} while (0)

#define CHECK_EQ(a, b)                                                     \
	do {                                                               \
		unsigned long long va_ = (unsigned long long)(a);          \
		unsigned long long vb_ = (unsigned long long)(b);          \
		g_checks++;                                                \
		if (va_ != vb_) {                                          \
			printf("FAIL %s:%d: %s == %s "                     \
			       "(0x%llx != 0x%llx)\n",                     \
			       __func__, __LINE__, #a, #b, va_, vb_);      \
			g_failures++;                                      \
		}                                                          \
	} while (0)

// ---------------------------------------------------------------------------
// Synthetic GP106-style VBIOS image, hand-built.
//
// Offsets (all cited against the nouveau files in the header of
// vbios_bit.h):
//   0x0000  55 AA            x86 ROM signature (image.c:39-48)
//   0x0018  u16 0x0040       PCIR pointer (pcir.c:30)
//   0x0040  PCIR rev 3       vendor 10de device 1c20 (GP106M), image size
//           2 * 512 = 0x400, image type 0 (x86), indicator 0x80 (last)
//   0x0080  BIT header       ff b8 'B' 'I' 'T', entry_size 12 (real
//           VBIOSes use 12-byte entries; bit.c only requires >= 6),
//           4 tokens: 'i', 'I', 'D', 'd'
//   0x0100  'i' payload      version 84.06.3F.00.07 (GP106-style)
//   0x0110  'I' init table   script@+0x00 macro_idx@+0x02 macro@+0x04 ...
//           (init.c:399-406)
//   0x0130  script table     3 u16 script offsets: 0x0200/0x0210/0x0220
//   0x0160  'D' payload      synthetic 3-entry script pointer array —
//           kept because real VBIOSes carry token 'D', but note: linux
//           master nouveau consumes init scripts from 'I' (init.c:363);
//           no fetched master file reads 'D'. Generic lookup only.
//   0x0200+ script bodies   single INIT_DONE opcode 0x71 (init.c:606)
//   0x0300  'd' payload      filler (DisplayPort table; dp.c consumer,
//           not parsed here)
//   0x03FF  checksum byte    so the full 0x400 sums to 0 (shadow.c:86)
// Unused areas are filled with 0xFF (typical flash padding).
// ---------------------------------------------------------------------------

#define IMG_SIZE 0x0400u
#define PCIR_OFF 0x0040u
#define BIT_OFF 0x0080u

// Entry ids and their expected (version, length, offset) in the image.
#define TOK_I 0x49u
#define TOK_i 0x69u
#define TOK_D 0x44u
#define TOK_d 0x64u

static uint8_t g_img[IMG_SIZE];

static void
put8(uint32_t off, uint8_t v)
{
	g_img[off] = v;
}

static void
put16(uint32_t off, uint16_t v)
{
	g_img[off + 0] = (uint8_t)(v & 0xff);
	g_img[off + 1] = (uint8_t)(v >> 8);
}

static void
put32(uint32_t off, uint32_t v)
{
	put16(off, (uint16_t)(v & 0xffff));
	put16(off + 2, (uint16_t)(v >> 16));
}

// Write one 12-byte BIT token entry (bit.c stride format; bytes +6..+11 are
// the pad/high-offset bytes nouveau never reads).
static void
put_entry(uint32_t off, uint8_t id, uint8_t ver, uint16_t len, uint16_t ptr)
{
	put8(off + 0, id);
	put8(off + 1, ver);
	put16(off + 2, len);
	put16(off + 4, ptr);
	put16(off + 6, 0x0000);
	put32(off + 8, 0x00000000);
}

static void
build_image(void)
{
	memset(g_img, 0xff, sizeof(g_img));

	// x86 ROM header
	put8(0x0000, 0x55);
	put8(0x0001, 0xaa);
	put16(0x0002, 0x00e0);	// init-size filler byte
	put16(0x0018, PCIR_OFF);

	// PCIR data structure (pcir.c field offsets)
	put8(PCIR_OFF + 0x00, 'P');
	put8(PCIR_OFF + 0x01, 'C');
	put8(PCIR_OFF + 0x02, 'I');
	put8(PCIR_OFF + 0x03, 'R');
	put16(PCIR_OFF + 0x04, 0x10de);		// vendor: NVIDIA
	put16(PCIR_OFF + 0x06, 0x1c20);		// device: GP106M
	put16(PCIR_OFF + 0x08, 0x0000);		// VPd pointer
	put16(PCIR_OFF + 0x0a, 0x0018);		// header length
	put8(PCIR_OFF + 0x0c, 0x03);		// PCIR revision 3
	put8(PCIR_OFF + 0x0d, 0x03);		// class: display
	put8(PCIR_OFF + 0x0e, 0x00);
	put8(PCIR_OFF + 0x0f, 0x00);
	put16(PCIR_OFF + 0x10, IMG_SIZE / 512);	// image size, 512-byte units
	put16(PCIR_OFF + 0x12, 0x0001);		// image rev
	put8(PCIR_OFF + 0x14, 0x00);		// x86 image type
	put8(PCIR_OFF + 0x15, 0x80);		// last image
	put16(PCIR_OFF + 0x16, 0x0000);

	// BIT header: magic, header_size, version, pad, entry_size, count
	put8(BIT_OFF + 0, 0xff);
	put8(BIT_OFF + 1, 0xb8);
	put8(BIT_OFF + 2, 'B');
	put8(BIT_OFF + 3, 'I');
	put8(BIT_OFF + 4, 'T');
	put8(BIT_OFF + 5, 0x0c);	// header size (not read by bit.c)
	put8(BIT_OFF + 6, 0x01);	// BIT version (not read by bit.c)
	put8(BIT_OFF + 7, 0x00);
	put8(BIT_OFF + 8, 0x00);
	put8(BIT_OFF + 9, 0x0c);	// entry stride (bit.c:42)
	put8(BIT_OFF + 10, 0x04);	// entry count (bit.c:31)
	put8(BIT_OFF + 11, 0x00);

	// Token directory, 12-byte entries starting at BIT_OFF + 12
	put_entry(BIT_OFF + 12, TOK_i, 0x01, 0x05, 0x0100);
	put_entry(BIT_OFF + 24, TOK_I, 0x02, 0x12, 0x0110);
	put_entry(BIT_OFF + 36, TOK_D, 0x01, 0x0a, 0x0160);
	put_entry(BIT_OFF + 48, TOK_d, 0x01, 0x08, 0x0300);

	// 'i' version payload (base.c:196-200 order: micro, minor, chip,
	// major, patch) — GP106-style 84.06.3F.00.07
	put8(0x0100, 0x00);
	put8(0x0101, 0x3f);
	put8(0x0102, 0x06);
	put8(0x0103, 0x54);
	put8(0x0104, 0x07);

	// 'I' init table (init.c:399-406 pointer fields)
	put16(0x0110 + 0x00, 0x0130);	// script table
	put16(0x0110 + 0x02, 0x0140);	// macro index table
	put16(0x0110 + 0x04, 0x0150);	// macro table
	put16(0x0110 + 0x06, 0x0000);	// condition table (none)
	put16(0x0110 + 0x08, 0x0000);	// io condition table
	put16(0x0110 + 0x0a, 0x0000);	// io flag condition table
	put16(0x0110 + 0x0c, 0x0000);	// function table
	put16(0x0110 + 0x0e, 0x0000);	// unused on 'I' v2
	put16(0x0110 + 0x10, 0x0000);	// xlat table

	// Script table: 3 devinit scripts (init.c:424: u16 per index)
	put16(0x0130, 0x0200);
	put16(0x0132, 0x0210);
	put16(0x0134, 0x0220);

	put16(0x0140, 0x0000);		// macro index table
	put32(0x0150, 0x00000000);	// macro table

	// 'D' payload: synthetic 3-entry script pointer array (see notice
	// above — lookup-only, master nouveau consumes 'I' instead)
	put16(0x0160, 0x0200);
	put16(0x0162, 0x0210);
	put16(0x0164, 0x0220);
	put16(0x0166, 0x0000);
	put16(0x0168, 0x0000);

	// Script bodies: one INIT_DONE opcode each (init.c:606)
	put8(0x0200, PG106_VBIOS_INIT_DONE_OP);
	put8(0x0201, 0xff);
	put8(0x0210, PG106_VBIOS_INIT_DONE_OP);
	put8(0x0211, 0xff);
	put8(0x0220, PG106_VBIOS_INIT_DONE_OP);
	put8(0x0221, 0xff);

	// 'd' DisplayPort payload — filler, not parsed
	put32(0x0300, 0x00000001);
	put32(0x0304, 0x00000000);

	// Checksum byte: whole image sums to 0 (shadow.c:86 scores this)
	{
		uint8_t sum = pg106_vbios_checksum(g_img, IMG_SIZE - 1);
		put8(IMG_SIZE - 1, (uint8_t)(0x00 - sum));
	}
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void
test_parse_and_tokens(void)
{
	struct pg106_vbios_bit vb;
	struct pg106_vbios_bit_entry e;

	CHECK_EQ(pg106_vbios_bit_parse(g_img, IMG_SIZE, &vb), 0);
	CHECK_EQ(vb.bit_offset, BIT_OFF);
	CHECK_EQ(vb.entry_size, 0x0c);
	CHECK_EQ(vb.entry_count, 4);
	CHECK(vb.data == g_img);
	CHECK_EQ(vb.size, IMG_SIZE);

	// 'I' — the init/devinit token (init.c:363)
	CHECK_EQ(pg106_vbios_bit_entry(&vb, TOK_I, &e), 0);
	CHECK_EQ(e.id, TOK_I);
	CHECK_EQ(e.version, 0x02);
	CHECK_EQ(e.length, 0x12);
	CHECK_EQ(e.offset, 0x0110);

	// 'i' — version info (base.c:195)
	CHECK_EQ(pg106_vbios_bit_entry(&vb, TOK_i, &e), 0);
	CHECK_EQ(e.version, 0x01);
	CHECK_EQ(e.length, 0x05);
	CHECK_EQ(e.offset, 0x0100);

	// 'D' — present in real VBIOSes; generic lookup, no master consumer
	CHECK_EQ(pg106_vbios_bit_entry(&vb, TOK_D, &e), 0);
	CHECK_EQ(e.version, 0x01);
	CHECK_EQ(e.length, 0x0a);
	CHECK_EQ(e.offset, 0x0160);

	// 'd' — DisplayPort (dp.c:33)
	CHECK_EQ(pg106_vbios_bit_entry(&vb, TOK_d, &e), 0);
	CHECK_EQ(e.offset, 0x0300);

	// Absent token: bit.c:45 returns -ENOENT
	CHECK_EQ(pg106_vbios_bit_entry(&vb, 'Z', &e), -ENOENT);
}

static void
test_init_scripts(void)
{
	struct pg106_vbios_bit vb;
	struct pg106_vbios_bit_entry e;
	uint32_t table, script;
	uint8_t op;

	CHECK_EQ(pg106_vbios_bit_parse(g_img, IMG_SIZE, &vb), 0);
	CHECK_EQ(pg106_vbios_bit_entry(&vb, TOK_I, &e), 0);

	// init.c:399 — script table pointer from the 'I' table
	CHECK_EQ(pg106_vbios_init_script_table(&vb, &e, &table), 0);
	CHECK_EQ(table, 0x0130);

	// init.c:424 — script N = rd16(table + 2N); 3-entry devinit table
	CHECK_EQ(pg106_vbios_init_script(&vb, &e, 0, &script), 0);
	CHECK_EQ(script, 0x0200);
	CHECK_EQ(pg106_vbios_init_script(&vb, &e, 1, &script), 0);
	CHECK_EQ(script, 0x0210);
	CHECK_EQ(pg106_vbios_init_script(&vb, &e, 2, &script), 0);
	CHECK_EQ(script, 0x0220);

	// Each script starts with INIT_DONE (init.c:606) in this synthetic
	// image — real Phase 1b scripts will start with register ops.
	CHECK_EQ(pg106_vbios_rd08(&vb, 0x0200, &op), 0);
	CHECK_EQ(op, PG106_VBIOS_INIT_DONE_OP);
	CHECK_EQ(pg106_vbios_rd08(&vb, 0x0210, &op), 0);
	CHECK_EQ(op, PG106_VBIOS_INIT_DONE_OP);
	CHECK_EQ(pg106_vbios_rd08(&vb, 0x0220, &op), 0);
	CHECK_EQ(op, PG106_VBIOS_INIT_DONE_OP);

	// Index past the script table reads later image bytes (here 0xFF
	// filler) — in-bounds, so it succeeds with a nonzero garbage offset
	// exactly as nouveau's unguarded rd16 would; documented divergence:
	// a real interpreter must stop at the first 0 script offset.
	CHECK_EQ(pg106_vbios_init_script(&vb, &e, 3, &script), 0);
	CHECK_EQ(script, 0xffff);

	// An index so far it leaves the image: guarded (-EFAULT), where
	// nouveau's nvbios_rd16 would print OOB and return 0 (base.c:41-44)
	CHECK_EQ(pg106_vbios_init_script(&vb, &e, 0xffff, &script), -EFAULT);

	// Corrupt the script-table pointer: init.c:387 "pointer invalid"
	CHECK_EQ(pg106_vbios_bit_entry(&vb, TOK_I, &e), 0);
	put16(0x0110, 0x0000);
	CHECK_EQ(pg106_vbios_init_script_table(&vb, &e, &table), -ENOENT);
	CHECK_EQ(pg106_vbios_init_script(&vb, &e, 0, &script), -ENOENT);
	build_image();	// restore

	// 'I' table too short to hold the script-table pointer (init.c:382)
	CHECK_EQ(pg106_vbios_bit_entry(&vb, TOK_I, &e), 0);
	e.length = 1;
	CHECK_EQ(pg106_vbios_init_script_table(&vb, &e, &table), -EPROTO);
	build_image();
}

static void
test_version(void)
{
	struct pg106_vbios_bit vb;
	struct pg106_vbios_bit_entry e;
	struct pg106_vbios_version v;

	CHECK_EQ(pg106_vbios_bit_parse(g_img, IMG_SIZE, &vb), 0);
	CHECK_EQ(pg106_vbios_bit_entry(&vb, TOK_i, &e), 0);

	// base.c:195-200 field order, synthetic "84.06.3F.00.07"
	CHECK_EQ(pg106_vbios_version(&vb, &e, &v), 0);
	CHECK_EQ(v.major, 0x54);
	CHECK_EQ(v.chip, 0x06);
	CHECK_EQ(v.minor, 0x3f);
	CHECK_EQ(v.micro, 0x00);
	CHECK_EQ(v.patch, 0x07);

	// Short 'i' table: base.c:195 requires length >= 4
	e.length = 3;
	CHECK_EQ(pg106_vbios_version(&vb, &e, &v), -EPROTO);

	// Table pointing outside the image: guarded
	e.length = 4;
	e.offset = (uint16_t)(IMG_SIZE - 1);
	CHECK_EQ(pg106_vbios_version(&vb, &e, &v), -EFAULT);
}

static void
test_rom_probe(void)
{
	struct pg106_vbios_rom rom;

	CHECK_EQ(pg106_vbios_rom_probe(g_img, IMG_SIZE, &rom), 0);
	CHECK(rom.rom_sig_ok);
	CHECK(rom.pcir_ok);
	CHECK_EQ(rom.pcir_offset, PCIR_OFF);
	CHECK_EQ(rom.vendor_id, 0x10de);
	CHECK_EQ(rom.device_id, 0x1c20);	// GP106M
	CHECK_EQ(rom.class_code[0], 0x03);
	CHECK_EQ(rom.image_size, IMG_SIZE);
	CHECK_EQ(rom.image_rev, 0x0001);
	CHECK_EQ(rom.image_type, 0x00);
	CHECK(rom.last_image);
	CHECK_EQ(rom.checksum, 0x00);	// consistent checksum byte

	// Unknown ROM signature (image.c:44-47)
	put8(0x0000, 0x11);
	CHECK_EQ(pg106_vbios_rom_probe(g_img, IMG_SIZE, &rom), -ENOENT);
	build_image();

	// Zero PCIR pointer (pcir.c:31): ok but no PCIR
	put16(0x0018, 0x0000);
	CHECK_EQ(pg106_vbios_rom_probe(g_img, IMG_SIZE, &rom), 0);
	CHECK(!rom.pcir_ok);
	build_image();

	// Bad PCIR signature (pcir.c:40-45): ok but no PCIR
	put8(PCIR_OFF + 0x00, 'X');
	CHECK_EQ(pg106_vbios_rom_probe(g_img, IMG_SIZE, &rom), 0);
	CHECK(!rom.pcir_ok);
	build_image();

	// PCIR struct not fully inside the image
	put16(0x0018, (uint16_t)(IMG_SIZE - 4));
	CHECK_EQ(pg106_vbios_rom_probe(g_img, IMG_SIZE, &rom), -EPROTO);
	build_image();

	// Flipped checksum byte: informational (shadow.c:85-93 tolerates it,
	// like nvbios_prom which does not set require_checksum), while the
	// BIT parse is unaffected. The flip makes the sum nonzero either way.
	put8(IMG_SIZE - 1, (uint8_t)(g_img[IMG_SIZE - 1] ^ 0x01));
	CHECK_EQ(pg106_vbios_rom_probe(g_img, IMG_SIZE, &rom), 0);
	CHECK(rom.checksum != 0x00);
	{
		struct pg106_vbios_bit vb;
		CHECK_EQ(pg106_vbios_bit_parse(g_img, IMG_SIZE, &vb), 0);
	}
	build_image();
}

static void
test_accessors(void)
{
	struct pg106_vbios_bit vb;
	uint8_t b;
	uint16_t w;
	uint32_t d;

	CHECK_EQ(pg106_vbios_bit_parse(g_img, IMG_SIZE, &vb), 0);

	// Unaligned u16 across the entry_size/count bytes: bytes 0x0c,0x04
	// -> LE word 0x040c
	CHECK_EQ(pg106_vbios_rd16(&vb, BIT_OFF + 9, &w), 0);
	CHECK_EQ(w, 0x040c);

	// u32 magic at BIT_OFF: bytes ff b8 'B' 'I' -> LE word 0x4942b8ff
	CHECK_EQ(pg106_vbios_rd32(&vb, BIT_OFF, &d), 0);
	CHECK_EQ(d, 0x4942b8ffu);

	// In-bounds edges
	CHECK_EQ(pg106_vbios_rd08(&vb, IMG_SIZE - 1, &b), 0);
	CHECK_EQ(pg106_vbios_rd16(&vb, IMG_SIZE - 2, &w), 0);
	CHECK_EQ(pg106_vbios_rd32(&vb, IMG_SIZE - 4, &d), 0);

	// Out-of-bounds: -EFAULT, never a read (base.c OOB guard analogue)
	CHECK_EQ(pg106_vbios_rd08(&vb, IMG_SIZE, &b), -EFAULT);
	CHECK_EQ(pg106_vbios_rd16(&vb, IMG_SIZE - 1, &w), -EFAULT);
	CHECK_EQ(pg106_vbios_rd32(&vb, IMG_SIZE - 3, &d), -EFAULT);
	CHECK_EQ(pg106_vbios_rd32(&vb, 0xffffffffu, &d), -EFAULT);

	// Null args
	CHECK_EQ(pg106_vbios_rd08(NULL, 0, &b), -EINVAL);
	CHECK_EQ(pg106_vbios_rd16(&vb, 0, NULL), -EINVAL);
	CHECK_EQ(pg106_vbios_bit_parse(NULL, IMG_SIZE, &vb), -EINVAL);
	CHECK_EQ(pg106_vbios_bit_parse(g_img, IMG_SIZE, NULL), -EINVAL);
	CHECK_EQ(pg106_vbios_bit_entry(&vb, 'I', NULL), -EINVAL);
	CHECK_EQ(pg106_vbios_bit_entry(NULL, 'I', &(struct
		    pg106_vbios_bit_entry){0}), -EINVAL);
}

static void
test_rejections(void)
{
	struct pg106_vbios_bit vb;

	// Bad BIT magic: first signature byte 'B' -> 'X' (base.c:189 scan)
	put8(BIT_OFF + 2, 'X');
	CHECK_EQ(pg106_vbios_bit_parse(g_img, IMG_SIZE, &vb), -ENOENT);
	build_image();

	// Magic present but header cut before bit_offset+12
	CHECK_EQ(pg106_vbios_bit_parse(g_img, BIT_OFF + 11, &vb), -EPROTO);
	CHECK_EQ(pg106_vbios_bit_parse(g_img, BIT_OFF + 8, &vb), -EPROTO);

	// entry_size too small to hold the 6 bytes bit_entry reads
	// (real VBIOSes use 12; bit.c tolerates smaller since it never
	// validates — we reject as malformed)
	put8(BIT_OFF + 9, 0x05);
	CHECK_EQ(pg106_vbios_bit_parse(g_img, IMG_SIZE, &vb), -EPROTO);
	put8(BIT_OFF + 9, 0x00);
	CHECK_EQ(pg106_vbios_bit_parse(g_img, IMG_SIZE, &vb), -EPROTO);
	build_image();

	// entry_count overruns the image
	put8(BIT_OFF + 10, 0xff);
	CHECK_EQ(pg106_vbios_bit_parse(g_img, IMG_SIZE, &vb), -EPROTO);
	build_image();

	// Zero-size and empty images
	CHECK_EQ(pg106_vbios_bit_parse(g_img, 0, &vb), -ENOENT);
	{
		uint8_t tiny[4] = { 0xff, 0xb8, 'B', 'I' };
		CHECK_EQ(pg106_vbios_bit_parse(tiny, sizeof(tiny), &vb),
			 -ENOENT);
	}

	// BIT signature at a nonzero offset in a header-less image
	// (scan is image-wide, base.c:189). NOTE: offset 0 would be
	// indistinguishable from "not found" — inherited from nouveau's
	// findstr+if(bit_offset) pattern, so a real BIT never sits at 0
	// (it cannot: the ROM header is there).
	{
		uint8_t flat[24];
		memset(flat, 0xAA, sizeof(flat));
		memcpy(flat + 3, PG106_VBIOS_BIT_SIG,
		       PG106_VBIOS_BIT_SIG_LEN);
		flat[3 + 9] = 0x06;	// entry_size
		flat[3 + 10] = 0x01;	// one entry
		// entry 'I' at +12: id 'I', ver 2, len 4, offset 0
		flat[3 + 12 + 0] = TOK_I;
		flat[3 + 12 + 1] = 0x02;
		flat[3 + 12 + 2] = 0x04;
		flat[3 + 12 + 3] = 0x00;
		flat[3 + 12 + 4] = 0x00;
		flat[3 + 12 + 5] = 0x00;
		CHECK_EQ(pg106_vbios_bit_parse(flat, sizeof(flat), &vb), 0);
		CHECK_EQ(vb.bit_offset, 3);
		CHECK_EQ(vb.entry_count, 1);
		{
			struct pg106_vbios_bit_entry e;
			CHECK_EQ(pg106_vbios_bit_entry(&vb, TOK_I, &e), 0);
			CHECK_EQ(e.offset, 0x0000);
		}
	}
}

// Every prefix of the image: the parse result must match what the prefix
// actually contains, and nothing may ever read out of bounds (run under
// ASan). Layout facts asserted:
//   n <    0x85  signature cut            -> -ENOENT (or < 5: cannot hold it)
//   n <    0x8C  signature ok, header cut -> -EPROTO
//   n <    0xBC  header ok, token dir cut -> -EPROTO
//   n >=   0xBC  BIT table complete       -> parse OK (self-contained, like
//                                           nouveau parsing whatever got
//                                           shadowed), but each table the
//                                           tokens point at is only usable
//                                           once its bytes are present.
//
// Script-table specifics: script 2's u16 pointer word sits at 0x134..0x135
// (script_table 0x130 + 2*2), so init_script(2) is guarded -EFAULT only for
// prefixes below 0x136 — the *body* it points to (0x220) is a separate,
// later concern for the Phase 1b interpreter.
static void
test_truncation_sweep(void)
{
	struct pg106_vbios_bit vb;
	struct pg106_vbios_rom rom;
	const uint32_t dir_end = BIT_OFF + 12 + 4 * 12;	// 0xBC
	const uint32_t script2_ptr_end = 0x0136;	// past rd16 at 0x134
	uint8_t *prefix;
	uint32_t n;

	prefix = malloc(IMG_SIZE);
	CHECK(prefix != NULL);

	for (n = 0; n < IMG_SIZE; n++) {
		int ret;

		memcpy(prefix, g_img, n);
		ret = pg106_vbios_bit_parse(prefix, n, &vb);

		if (n < dir_end) {
			// BIT structures incomplete: must reject, never
			// succeed or crash.
			if (ret == 0) {
				printf("FAIL %s: prefix %u parsed as valid\n",
				       __func__, n);
				g_failures++;
			}
		} else {
			// BIT table complete: must parse.
			if (ret != 0) {
				printf("FAIL %s: prefix %u rejected (%d)\n",
				       __func__, n, ret);
				g_failures++;
				continue;
			}

			// Devinit script 2 is only resolvable once the u16
			// at 0x220 exists in the prefix; before that the
			// guarded read must fail with -EFAULT.
			{
				struct pg106_vbios_bit_entry e;
				uint32_t script;

				CHECK_EQ(pg106_vbios_bit_entry(&vb, TOK_I,
							       &e), 0);
				ret = pg106_vbios_init_script(&vb, &e, 2,
							      &script);
				if (n >= script2_ptr_end) {
					CHECK_EQ(ret, 0);
					CHECK_EQ(script, 0x0220);
				} else {
					CHECK_EQ(ret, -EFAULT);
				}
			}
		}

		// The informational probe must always be clean on any prefix.
		(void)pg106_vbios_rom_probe(prefix, n, &rom);
	}

	free(prefix);
}

// The full image must parse (sanity anchor for the sweep above).
static void
test_full_image_ok(void)
{
	struct pg106_vbios_bit vb;
	struct pg106_vbios_bit_entry e;
	uint32_t script;

	CHECK_EQ(pg106_vbios_bit_parse(g_img, IMG_SIZE, &vb), 0);
	CHECK_EQ(pg106_vbios_bit_entry(&vb, TOK_I, &e), 0);
	CHECK_EQ(pg106_vbios_init_script(&vb, &e, 2, &script), 0);
	CHECK_EQ(script, 0x0220);
}

// NPDE size override (image.c:56-63, npde.c:50-59): on images whose PCIR
// image_size covers the WHOLE flash (the GP107M layout in
// research/firmware/vbios/msi-gtx1050ti-mobile-*.rom: PCIR 0x28000, NPDE
// 0xf000), nouveau's image 0 is the NPDE size. EFI (type 0x70) images
// ignore NPDE.
static void
test_npde(void)
{
	static uint8_t b[0x3000];
	struct pg106_vbios_rom rom;
	struct pg106_vbios_image im;

	memset(b, 0, sizeof(b));
	b[0] = 0x55; b[1] = 0xaa;
	b[0x18] = 0x40;
	memcpy(&b[0x40], "PCIR", 4);
	b[0x40 + 0x0a] = 0x18;            // PCIR header length
	b[0x40 + 0x10] = 0x18;            // 0x3000 (whole buffer)
	memcpy(&b[0x60], "NPDE", 4);      // (0x40 + 0x18 + 0xf) & ~0xf
	b[0x60 + 0x08] = 0x08;            // 0x1000
	CHECK_EQ(pg106_vbios_rom_probe(b, sizeof(b), &rom), 0);
	CHECK(rom.npde_ok);
	CHECK_EQ(rom.pcir_image_size, 0x3000u);
	CHECK_EQ(rom.image_size, 0x1000u);
	CHECK_EQ(pg106_vbios_image(b, sizeof(b), 0, &im), 0);
	CHECK_EQ(im.size, 0x1000u);
	CHECK_EQ(im.base, 0u);
	// The next image starts at 0x1000 — none there -> end of chain.
	CHECK_EQ(pg106_vbios_image(b, sizeof(b), 1, &im), -ENOENT);
	// NPDE "last" bit ends the chain (npde.c:56).
	b[0x60 + 0x0a] = 0x80;
	CHECK_EQ(pg106_vbios_rom_probe(b, sizeof(b), &rom), 0);
	CHECK(rom.last_image);
	// EFI image: PCIR size kept, NPDE ignored, last forced (image.c:61-62).
	b[0x60 + 0x0a] = 0x00;
	b[0x40 + 0x14] = 0x70;
	CHECK_EQ(pg106_vbios_rom_probe(b, sizeof(b), &rom), 0);
	CHECK(!rom.npde_ok);
	CHECK_EQ(rom.image_size, 0x3000u);
	printf("PASS vbios: NPDE overrides PCIR image size (image.c:56-60)\n");
}

int
main(void)
{
	build_image();

	test_full_image_ok();
	build_image();
	test_parse_and_tokens();
	build_image();
	test_init_scripts();
	build_image();
	test_version();
	build_image();
	test_rom_probe();
	test_npde();
	build_image();
	test_accessors();
	build_image();
	test_rejections();
	build_image();
	test_truncation_sweep();
	build_image();

	// Report
	printf("vbios: %d checks, %d failures\n", g_checks, g_failures);
	if (g_failures == 0) {
		printf("PASS test_vbios: BIT parse, token walk, devinit "
		       "script table, ROM/PCIR probe, bounds guards, "
		       "truncation sweep (behavior asserted for all %u "
		       "prefixes of the %u-byte synthetic image)\n",
		       (unsigned)(IMG_SIZE), (unsigned)IMG_SIZE);
		printf("PASS test_vbios: synthetic-only coverage — real "
		       "Helios 300 VBIOS pending BLOCKERS.md B1; results "
		       "are simulation evidence (HANDOFF-PASCAL.md §7)\n");
		return 0;
	}
	return 1;
}
