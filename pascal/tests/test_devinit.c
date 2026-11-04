// SPDX-License-Identifier: GPL-2.0-only
// pascal/tests/test_devinit.c — DEVINIT interpreter unit tests.
//
// Phase 1 gate, applied to a SYNTHETIC script substitute (HANDOFF-PASCAL.md
// §6 Phase 1; the real Helios 300 VBIOS dump is still pending —
// research/evidence/BLOCKERS.md B1, not modified here):
//   1. build a synthetic BIOS image exercising every implemented opcode,
//   2. run it and assert the register-write trace against a golden vector
//      embedded below as a literal C array (hand-derived from
//      nvkm/subdev/bios/init.c semantics, opcode by opcode),
//   3. run it a SECOND time with re-applied bus presets and require the two
//      traces diff clean,
//   4. exercise the loud-failure paths (bad opcode, unimplemented opcode,
//      image overrun, trace overflow, runaway loop, runaway sub-recursion,
//      missing/out-of-range tables).
//
// THIS IS SIMULATION, NOT HARDWARE EVIDENCE (HANDOFF-PASCAL.md §7): the
// golden vector proves the interpreter matches nouveau source semantics on
// a fabricated script; it says nothing about any real GPU until the
// interpreter runs a real GP106 VBIOS in a later phase.

#include <pascal/devinit.h>

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

// ---------------------------------------------------------------------------
// Synthetic BIOS image.
//
// Layout (all offsets are BIOS offsets, little-endian throughout):
//   0x0100 condition table        2 x 12B entries (init.c:484)
//   0x0120 io condition table     2 x 5B  entries (init.c:500)
//   0x0130 io flag condition      1 x 9B  entry   (init.c:517)
//   0x0140 macro table            2 x 8B  entries (init.c:1737)
//   0x0150 script table           3 x u16 pointers (INIT_SUB)
//   0x0160 xlat table             1 x u16 pointer  (INIT_XLAT)
//   0x0180 xlat blob              11 22 33 44
//   0x0300 u32 0x00c0ffee         (INIT_ZM_REG_INDIRECT data)
//   0x0330 io-flag data blob      [4] = 0x04
//   0x0700 INIT_SUB[0] target     INIT_ZM_REG + INIT_DONE
//   0x0720 INIT_SUB_DIRECT target INIT_ZM_REG16 + INIT_DONE
//   0x0740.. error-path scripts (bad opcode, unimplemented, self-jump, ...)
//   0x1000 MAIN script: one opcode per 0x20-byte slot, each followed by an
//         INIT_JUMP to the next slot (JUMP itself gets exercised en route).
//   0x1ff8 overrun script: INIT_ZM_REG whose value operand crosses the image
//         end (image size 0x2000).
// ---------------------------------------------------------------------------

#define IMG_SIZE 0x2000u
#define MAIN     0x1000u
#define SLOT(n)  ((uint16_t)(MAIN + (n) * 0x20u))

static uint8_t img[IMG_SIZE];

static void
put08(uint16_t off, uint8_t v)
{
	img[off] = v;
}

static void
put16(uint16_t off, uint16_t v)
{
	put08(off, (uint8_t)v);
	put08((uint16_t)(off + 1), (uint8_t)(v >> 8));
}

static void
put32(uint16_t off, uint32_t v)
{
	put16(off, (uint16_t)v);
	put16((uint16_t)(off + 2), (uint16_t)(v >> 16));
}

static void
build_image(void)
{
	memset(img, 0, sizeof(img));

	// condition table: e0 (R[0x2000] & 0xff00) == 0xab00 -> met
	//                  e1 (R[0x2000] & 0x00ff) == 0xcd   -> unmet
	put32(0x0100, 0x002000); put32(0x0104, 0x0000ff00); put32(0x0108, 0x0000ab00);
	put32(0x010c, 0x002000); put32(0x0110, 0x000000ff); put32(0x0114, 0x000000cd);

	// io condition table: e0 (0x3d4[0x41] & 0x80) == 0x80 -> met
	//                     e1 (0x3d4[0x41] & 0x40) == 0x40 -> unmet
	put16(0x0120, 0x03d4); put08(0x0122, 0x41); put08(0x0123, 0x80); put08(0x0124, 0x80);
	put16(0x0125, 0x03d4); put08(0x0127, 0x41); put08(0x0128, 0x40); put08(0x0129, 0x40);

	// io flag condition: ((0x3c4[0x01] & 0x20) >> 3) indexes blob 0x0330;
	// preset 0x20 -> ioval 4 -> (blob[4] & 0x0f) == 0x04 -> met
	put16(0x0130, 0x03c4); put08(0x0132, 0x01); put08(0x0133, 0x20);
	put08(0x0134, 0x03); put16(0x0135, 0x0330); put08(0x0137, 0x0f);
	put08(0x0138, 0x04);

	// macro table: m0 R[0x00e000] = 0xdead0001, m1 R[0x00e004] = 0xbeef
	put32(0x0140, 0x00e000); put32(0x0144, 0xdead0001);
	put32(0x0148, 0x00e004); put32(0x014c, 0x0000beef);

	// script table: [0]=0x0700 (real), [1]=0x0000 (invalid -> INIT_SUB
	// skips, init.c:1672 `if (addr && init_exec(init))`), [2]=unused
	put16(0x0150, 0x0700);
	put16(0x0152, 0x0000);
	put16(0x0154, 0x0720);

	// xlat table + blob (INIT_XLAT reads blob[index*2 ptr + value])
	put16(0x0160, 0x0180);
	put08(0x0180, 0x11); put08(0x0181, 0x22);
	put08(0x0182, 0x33); put08(0x0183, 0x44);

	// INIT_ZM_REG_INDIRECT data word
	put32(0x0300, 0x00c0ffee);

	// io-flag blob byte
	put08(0x0334, 0x04);

	// 0x0700 INIT_SUB[0] target: R[0x001090] = 0x0badf00d; INIT_DONE
	put08(0x0700, 0x7a); put32(0x0701, 0x001090); put32(0x0705, 0x0badf00d);
	put08(0x0709, 0x71);

	// 0x0720 INIT_SUB_DIRECT target: R[0x001094] = 0x600d; INIT_DONE
	put08(0x0720, 0x77); put32(0x0721, 0x001094); put16(0x0725, 0x600d);
	put08(0x0727, 0x71);

	// error-path scripts
	put08(0x0740, 0xfe);                             // byte absent from nvkm dispatch
	put08(0x0750, 0x4c);                             // INIT_I2C_BYTE: unimplemented here
	put08(0x0760, 0x5c); put16(0x0761, 0x0760);      // INIT_JUMP to self (runaway)
	put08(0x0770, 0x75); put08(0x0771, 0x00);        // INIT_CONDITION, no table
	put08(0x0780, 0x5b); put16(0x0781, 0x0780);      // INIT_SUB_DIRECT to self
	put08(0x0790, 0x6b); put08(0x0791, 0x00);        // INIT_SUB, no script table
	put08(0x07a0, 0x75); put08(0x07a1, 0x05);        // condition index out of range
	put08(0x07b0, 0x6f); put08(0x07b1, 0x05);        // macro index out of range

	// 0x1ff8 INIT_ZM_REG; the value operand read at 0x1ffd needs bytes up
	// to 0x2000 -> past image end
	put08(0x1ff8, 0x7a); put32(0x1ff9, 0x001000);
	put08(0x1ffd, 0xaa); put08(0x1ffe, 0xbb); put08(0x1fff, 0xcc);

	// ---- MAIN script slots ----
	uint16_t a; // scratch

	// n00 INIT_ZM_REG       R[0x001000] = 0x12345678
	a = SLOT(0);
	put08(a, 0x7a); put32(a + 1, 0x001000); put32(a + 5, 0x12345678);
	// n01 INIT_ZM_REG16     R[0x001004] = 0x0000beef
	a = SLOT(1);
	put08(a, 0x77); put32(a + 1, 0x001004); put16(a + 5, 0xbeef);
	// n02 INIT_ANDN_REG     R[0x001008] &= ~0xf0        (preset 0xff -> 0x0f)
	a = SLOT(2);
	put08(a, 0x47); put32(a + 1, 0x001008); put32(a + 5, 0x000000f0);
	// n03 INIT_OR_REG       R[0x001008] |= 0xf000        (0x0f -> 0xf00f)
	a = SLOT(3);
	put08(a, 0x48); put32(a + 1, 0x001008); put32(a + 5, 0x0000f000);
	// n04 INIT_NV_REG       R[0x00100c] = (R & 0xff00) | 0xcd00 (0x1122 -> 0xdd00)
	a = SLOT(4);
	put08(a, 0x6e); put32(a + 1, 0x00100c);
	put32(a + 5, 0x0000ff00); put32(a + 9, 0x0000cd00);
	// n05 INIT_COPY_NV_REG  R[0x001014] = (R & dmask) | (((R[0x1010] << 4) & 0xff0) ^ 0)
	a = SLOT(5);
	put08(a, 0x5f); put32(a + 1, 0x001010); put08(a + 5, 0xfc);
	put32(a + 6, 0x00000ff0); put32(a + 10, 0x00000000);
	put32(a + 14, 0x001014); put32(a + 18, 0xffff0fff);
	// n06 INIT_ZM_REG_SEQUENCE R[0x001020] = 0x11111111; R[0x001024] = 0x22222222
	a = SLOT(6);
	put08(a, 0x58); put32(a + 1, 0x001020); put08(a + 5, 2);
	put32(a + 6, 0x11111111); put32(a + 10, 0x22222222);
	// n07 INIT_ZM_REG_GROUP R[0x001030] = 1, 2, 3 (three writes, same address)
	a = SLOT(7);
	put08(a, 0x91); put32(a + 1, 0x001030); put08(a + 5, 3);
	put32(a + 6, 1); put32(a + 10, 2); put32(a + 14, 3);
	// n08 INIT_RAM_RESTRICT_ZM_GROUP 2 registers x 2 groups, strap = group 1
	//   R[0x001040] = 0xaaaa0001, R[0x001044] = 0xbbbb0001
	a = SLOT(8);
	put08(a, 0x8f); put32(a + 1, 0x001040); put08(a + 5, 4); put08(a + 6, 2);
	put32(a + 7, 0xaaaa0000); put32(a + 11, 0xaaaa0001);
	put32(a + 15, 0xbbbb0000); put32(a + 19, 0xbbbb0001);
	// n09 INIT_COPY_ZM_REG  R[0x001048] = R[0x001000] (0x12345678 from n00)
	a = SLOT(9);
	put08(a, 0x90); put32(a + 1, 0x001000); put32(a + 5, 0x001048);
	// n10 INIT_ZM_MASK_ADD  R[0x00104c] = (R & 0xf000) | ((R + 0x200) & ~0xf0)
	//                       preset 0x1500 -> 0x1700
	a = SLOT(10);
	put08(a, 0x97); put32(a + 1, 0x00104c);
	put32(a + 5, 0x0000f000); put32(a + 9, 0x00000200);
	// n11 INIT_INDEX_ADDRESS_LATCHED via R[0x1050]:R[0x1054], 2 pairs
	//   R[0x1054] = 0xaa;  R[0x1050] = (R & 0xffffff00) | 0x10
	//   R[0x1054] = 0xbb;  R[0x1050] = (R & 0xffffff00) | 0x11
	a = SLOT(11);
	put08(a, 0x49); put32(a + 1, 0x001050); put32(a + 5, 0x001054);
	put32(a + 9, 0xffffff00); put32(a + 13, 0x00000000); put08(a + 17, 2);
	put08(a + 18, 0x10); put08(a + 19, 0xaa);
	put08(a + 20, 0x11); put08(a + 21, 0xbb);
	// n12 INIT_XLAT  R[0x00105c] = (R & ~0xff) | (xlat0[(R[0x1058] >> 0) & 3] << 4)
	//   preset R[0x1058] = 2, blob[2] = 0x33, R[0x105c] = 0xcc00 -> 0xcf30
	a = SLOT(12);
	put08(a, 0x96); put32(a + 1, 0x001058); put08(a + 5, 0x00);
	put08(a + 6, 0x03); put08(a + 7, 0x00); put32(a + 8, 0x00105c);
	put32(a + 12, 0xffffff00); put08(a + 16, 0x04);
	// n13 INIT_IO_RESTRICT_PROG conf = ((0x3d4[0x60] & 0x30) >> 4) = 1
	//   (preset 0x70) -> R[0x001060] = 0x22220000
	a = SLOT(13);
	put08(a, 0x32); put16(a + 1, 0x03d4); put08(a + 3, 0x60);
	put08(a + 4, 0x30); put08(a + 5, 4); put08(a + 6, 2);
	put32(a + 7, 0x001060); put32(a + 11, 0x11110000);
	put32(a + 15, 0x22220000);
	// n14 INIT_ZM_INDEX_IO  0x3c4[0x44] = 0x77
	a = SLOT(14);
	put08(a, 0x62); put16(a + 1, 0x03c4); put08(a + 3, 0x44); put08(a + 4, 0x77);
	// n15 INIT_INDEX_IO     0x3d4[0x61] = (0x3d4[0x61] & 0x0f) | 0x80 (0x35 -> 0x85)
	a = SLOT(15);
	put08(a, 0x78); put16(a + 1, 0x03d4); put08(a + 3, 0x61);
	put08(a + 4, 0x0f); put08(a + 5, 0x80);
	// n16 INIT_ZM_CR        0x3d4[0x75] = 0x42
	a = SLOT(16);
	put08(a, 0x53); put08(a + 1, 0x75); put08(a + 2, 0x42);
	// n17 INIT_CR           0x3d4[0x76] = (0x3d4[0x76] & 0x0f) | 0x30 (0xff -> 0x3f)
	a = SLOT(17);
	put08(a, 0x52); put08(a + 1, 0x76); put08(a + 2, 0x0f); put08(a + 3, 0x30);
	// n18 INIT_ZM_CR_GROUP  0x3d4[0x77] = 0x11; 0x3d4[0x78] = 0x22
	a = SLOT(18);
	put08(a, 0x54); put08(a + 1, 2);
	put08(a + 2, 0x77); put08(a + 3, 0x11);
	put08(a + 4, 0x78); put08(a + 5, 0x22);
	// n19 INIT_CR_INDEX_ADDRESS_LATCHED 0x3d4[0x30]:0x3d4[0x31], base 0x40 x2
	//   (preset 0x3d4[0x30] = 0x00 -> restore writes 0x00 last)
	a = SLOT(19);
	put08(a, 0x51); put08(a + 1, 0x30); put08(a + 2, 0x31);
	put08(a + 3, 0x40); put08(a + 4, 2);
	put08(a + 5, 0xde); put08(a + 6, 0xad);
	// n20 INIT_COPY  0x3d4[0x79] = (0x3d4[0x79] & 0xf3) | ((R[0x1064] >> 2) & 0x0c)
	//   preset 0 / 0xa0 -> 0x08
	a = SLOT(20);
	put08(a, 0x37); put32(a + 1, 0x001064); put08(a + 5, 0x02);
	put08(a + 6, 0x0c); put16(a + 7, 0x03d4); put08(a + 9, 0x79);
	put08(a + 10, 0xf3);
	// n21 INIT_CONDITION 0 (met)
	a = SLOT(21);
	put08(a, 0x75); put08(a + 1, 0x00);
	// n22 INIT_ZM_REG       R[0x001070] = 0x1111 (must execute)
	a = SLOT(22);
	put08(a, 0x7a); put32(a + 1, 0x001070); put32(a + 5, 0x00001111);
	// n23..: condition/NOT window as ONE sequential block — INIT_JUMP is
	//   correctly *not taken* while execution is disabled (init.c:1377),
	//   so the disabled window must flow linearly, not rely on jumps.
	//   [75 01] unmet -> exec off; [7a] skipped; [72] resume; [38] off;
	//   [7a] skipped; [38] on; then JUMP (taken again).
	a = SLOT(23);
	put08(a, 0x75); put08(a + 1, 0x01);
	put08(a + 2, 0x7a); put32(a + 3, 0x001074); put32(a + 7, 0x00002222);
	put08(a + 11, 0x72);
	put08(a + 12, 0x38);
	put08(a + 13, 0x7a); put32(a + 14, 0x001078); put32(a + 18, 0x00000001);
	put08(a + 22, 0x38);
	// n24 INIT_IO_CONDITION 0 (met)
	a = SLOT(24);
	put08(a, 0x76); put08(a + 1, 0x00);
	// n25 INIT_IO_FLAG_CONDITION 0 (met)
	a = SLOT(25);
	put08(a, 0x39); put08(a + 1, 0x00);
	// n26 INIT_STRAP_CONDITION (R[0x101000] & 0xf0) == 0x10 (preset 0x12, met)
	a = SLOT(26);
	put08(a, 0x73); put32(a + 1, 0x000000f0); put32(a + 5, 0x00000010);
	// n27 INIT_RAM_CONDITION (R[0x100000] & 0xff) == 0x36 (preset 0x136, met)
	a = SLOT(27);
	put08(a, 0x6d); put08(a + 1, 0xff); put08(a + 2, 0x36);
	// n28 INIT_ZM_REG       R[0x001080] = 0x1366
	a = SLOT(28);
	put08(a, 0x7a); put32(a + 1, 0x001080); put32(a + 5, 0x00001366);
	// n29 INIT_SUB 0 -> script table[0] = 0x0700
	a = SLOT(29);
	put08(a, 0x6b); put08(a + 1, 0x00);
	// n30 INIT_SUB 1 -> script table[1] = 0 -> skipped (init.c:1672)
	a = SLOT(30);
	put08(a, 0x6b); put08(a + 1, 0x01);
	// n31 INIT_SUB_DIRECT 0x0720
	a = SLOT(31);
	put08(a, 0x5b); put16(a + 1, 0x0720);
	// n32 INIT_MACRO 0 -> R[0x00e000] = 0xdead0001
	a = SLOT(32);
	put08(a, 0x6f); put08(a + 1, 0x00);
	// n33 INIT_MACRO 1 -> R[0x00e004] = 0x0000beef
	a = SLOT(33);
	put08(a, 0x6f); put08(a + 1, 0x01);
	// n34 INIT_REPEAT 2: body INIT_ZM_REG16 R[0x0010a0] = 0xc0ff; END_REPEAT
	//   body opcode sits at SLOT(34)+2 -> both trace records offset 0x1442
	a = SLOT(34);
	put08(a, 0x33); put08(a + 1, 2);
	put08(a + 2, 0x77); put32(a + 3, 0x0010a0); put16(a + 7, 0xc0ff);
	put08(a + 9, 0x36);
	// n35 INIT_TIME 250 us (u < 1000 -> udelay(usec), init.c:1789-1791)
	a = SLOT(35);
	put08(a, 0x74); put16(a + 1, 250);
	// n36 INIT_LTIME 5 ms (mdelay(5) -> 5000 us on the fake clock)
	a = SLOT(36);
	put08(a, 0x57); put16(a + 1, 5);
	// n37 INIT_CONDITION_TIME cond 0 retry 4 (met on first poll -> no delay)
	a = SLOT(37);
	put08(a, 0x56); put08(a + 1, 0x00); put08(a + 2, 0x04);
	// n38 INIT_CONDITION_TIME cond 1 retry 0 + skipped write + INIT_RESUME,
	//   one sequential block: wait=0 -> no poll at all, execution disabled
	//   unconditionally (nvkm u8 wrap, init.c:1242); [7a] skipped; [72] on
	a = SLOT(38);
	put08(a, 0x56); put08(a + 1, 0x01); put08(a + 2, 0x00);
	put08(a + 3, 0x7a); put32(a + 4, 0x0010f0); put32(a + 8, 0x0000aaaa);
	put08(a + 12, 0x72);
	// n39 INIT_IO port 0x03c3 mask 0x00 data 0x01 -> NV_50+ display-restore
	//   special case (init.c:1634-1652) + final wrport
	a = SLOT(39);
	put08(a, 0x69); put16(a + 1, 0x03c3); put08(a + 3, 0x00); put08(a + 4, 0x01);
	// n40 INIT_RESET R[0x001100] = 1, then 2 (forced; 10 us udelay)
	a = SLOT(40);
	put08(a, 0x65); put32(a + 1, 0x001100);
	put32(a + 5, 0x00000001); put32(a + 9, 0x00000002);
	// n41 INIT_RESET_BEGUN
	a = SLOT(41);
	put08(a, 0x8c);
	// n42 INIT_RESET_END
	a = SLOT(42);
	put08(a, 0x8d);
	// n43 INIT_GPIO (stub hook)
	a = SLOT(43);
	put08(a, 0x8e);
	// n44 INIT_RESERVED (1 byte)
	a = SLOT(44);
	put08(a, 0x92);
	// n45 INIT_RESERVED4 (4 bytes)
	a = SLOT(45);
	put08(a, 0xaa); put08(a + 1, 0); put08(a + 2, 0); put08(a + 3, 0);
	// n46 INIT_ZM_REG_INDIRECT "R[0x0010b0]" = VBIOS[0x0300] — nvkm writes
	//   to `addr` (0x300), not `reg`; bug-compatible on purpose
	a = SLOT(46);
	put08(a, 0x5a); put32(a + 1, 0x0010b0); put16(a + 5, 0x0300);
	// n47 INIT_IO_MASK_OR   0x3d4[0x7a] &= ~(1 << or=1) (0xff -> 0xfd)
	a = SLOT(47);
	put08(a, 0x3b); put08(a + 1, 0x7a);
	// n48 INIT_IO_OR        0x3d4[0x7b] |= (1 << or=1) (0x00 -> 0x02)
	a = SLOT(48);
	put08(a, 0x3c); put08(a + 1, 0x7b);
	// n49 INIT_ZM_REG head-mangled: 0x80610040 + head(1)*0x800 -> R[0x610840]
	a = SLOT(49);
	put08(a, 0x7a); put32(a + 1, 0x80610040); put32(a + 5, 0x00000055);
	// n50 INIT_ZM_REG or/link-mangled: 0x606100c0 + or(1)*0x800 + link*0x80
	//   (init_link: link == 2 -> +0x80) -> R[0x610940]
	a = SLOT(50);
	put08(a, 0x7a); put32(a + 1, 0x606100c0); put32(a + 5, 0x00000066);
	// n51 INIT_DONE (terminator, init.c:606-614)
	a = SLOT(51);
	put08(a, 0x71);

	// every non-final slot ends with INIT_JUMP to the next slot; the jump
	// goes right after the opcode's own bytes
	static const uint8_t slot_len[51] = {
		9, 7, 9, 9, 13, 22, 14, 18, 23, 9, 13, 22, 17, 19, 5, 6, 3, 4,
		6, 7, 11, 2, 9, 23, 2, 2, 9, 3, 9, 2, 2, 3, 2, 2, 10, 3, 3, 3,
		13, 5, 13, 1, 1, 1, 1, 4, 7, 2, 2, 9, 9,
	};
	for (unsigned n = 0; n < 51; n++) {
		uint16_t at = (uint16_t)(SLOT(n) + slot_len[n]);
		put08(at, 0x5c);
		put16((uint16_t)(at + 1), SLOT(n + 1));
	}
}


// ---------------------------------------------------------------------------
// Fake bus: sparse MMIO store, indexed-IO ports, direct ports, fake clock.
// ---------------------------------------------------------------------------

#define MAX_REGS 64
struct fake_reg { uint32_t addr; uint32_t val; };
static struct fake_reg mmio[MAX_REGS];
static unsigned mmio_len;

#define MAX_IOPORTS 8
struct fake_io_port { uint16_t port; uint8_t idx[256]; };
static struct fake_io_port ioports[MAX_IOPORTS];
static unsigned ioports_len;

static uint8_t direct_port[65536];
static uint64_t fake_clock;
static unsigned gpio_resets;

static struct fake_io_port *
ioport(uint16_t port)
{
	for (unsigned i = 0; i < ioports_len; i++)
		if (ioports[i].port == port)
			return &ioports[i];
	REQUIRE(ioports_len < MAX_IOPORTS);
	struct fake_io_port *p = &ioports[ioports_len++];
	p->port = port;
	memset(p->idx, 0, sizeof(p->idx));
	return p;
}

static void
mmio_preset(uint32_t addr, uint32_t val)
{
	for (unsigned i = 0; i < mmio_len; i++) {
		if (mmio[i].addr == addr) {
			mmio[i].val = val;
			return;
		}
	}
	REQUIRE(mmio_len < MAX_REGS);
	mmio[mmio_len].addr = addr;
	mmio[mmio_len].val = val;
	mmio_len++;
}

static uint32_t
t_rd32(struct pg106_init_env *env, uint32_t addr)
{
	(void)env;
	for (unsigned i = 0; i < mmio_len; i++)
		if (mmio[i].addr == addr)
			return mmio[i].val;
	return 0;
}

static void
t_wr32(struct pg106_init_env *env, uint32_t addr, uint32_t val)
{
	(void)env;
	mmio_preset(addr, val);
}

static uint8_t
t_rd8(struct pg106_init_env *env, uint16_t port, uint8_t index)
{
	(void)env;
	return ioport(port)->idx[index];
}

static void
t_wr8(struct pg106_init_env *env, uint16_t port, uint8_t index, uint8_t val)
{
	(void)env;
	ioport(port)->idx[index] = val;
}

static uint8_t
t_rdport(struct pg106_init_env *env, uint16_t port)
{
	(void)env;
	return direct_port[port];
}

static void
t_wrport(struct pg106_init_env *env, uint16_t port, uint8_t val)
{
	(void)env;
	direct_port[port] = val;
}

static uint32_t
t_rd32_io(struct pg106_init_env *env, uint16_t port)
{
	(void)env; (void)port;
	return 0; // no master opcode uses direct 32-bit port IO
}

static uint64_t
t_time(struct pg106_init_env *env, uint32_t delay_us)
{
	(void)env;
	fake_clock += delay_us;
	return fake_clock;
}

static void
t_gpio_reset(struct pg106_init_env *env)
{
	(void)env;
	gpio_resets++;
}

static const struct pg106_init_vt test_vt = {
	t_rd32, t_wr32, t_rd8, t_wr8, t_rdport, t_wrport, t_rd32_io,
	t_time, t_gpio_reset,
};

// Bus state the golden run assumes. Re-applied before every run so run 1 and
// run 2 see identical inputs (determinism gate).
static void
apply_presets(void)
{
	mmio_len = 0;
	ioports_len = 0;
	memset(direct_port, 0, sizeof(direct_port));
	fake_clock = 0;
	gpio_resets = 0;

	mmio_preset(0x001000, 0x12345678);
	mmio_preset(0x001008, 0x000000ff);
	mmio_preset(0x00100c, 0x00001122);
	mmio_preset(0x001010, 0x0000a5a5);
	mmio_preset(0x001014, 0x00000077);
	mmio_preset(0x00104c, 0x00001500);
	mmio_preset(0x001050, 0x0000aa00);
	mmio_preset(0x001058, 0x00000002);
	mmio_preset(0x00105c, 0x0000cc00);
	mmio_preset(0x001064, 0x000000a0);
	mmio_preset(0x000200, 0x40000000);
	mmio_preset(0x00e18c, 0x00000000);
	mmio_preset(0x00184c, 0x00000300);
	mmio_preset(0x001850, 0x00000001);
	mmio_preset(0x614100, 0x11100000);
	mmio_preset(0x614900, 0x00000000);
	mmio_preset(0x002000, 0x0000ab00);
	mmio_preset(0x100000, 0x00000136);
	mmio_preset(0x101000, 0x00000012);

	ioport(0x03d4)->idx[0x30] = 0x00;
	ioport(0x03d4)->idx[0x41] = 0x80;
	ioport(0x03d4)->idx[0x60] = 0x50; // (0x50 & 0x30) >> 4 = conf 1
	ioport(0x03d4)->idx[0x61] = 0x35;
	ioport(0x03d4)->idx[0x76] = 0xff;
	ioport(0x03d4)->idx[0x79] = 0x00;
	ioport(0x03d4)->idx[0x7a] = 0xff;
	ioport(0x03d4)->idx[0x7b] = 0x00;
	ioport(0x03c4)->idx[0x01] = 0x20;

	direct_port[0x03c3] = 0x01;
}

static struct pg106_init_env *
setup_env(struct pg106_init_env *env, struct pg106_init_trace_rec *buf,
	  size_t cap)
{
	apply_presets();
	pg106_init_env_init(env, &test_vt, img, sizeof(img), buf, cap);
	env->table_condition = 0x0100;
	env->condition_count = 2;
	env->table_io_condition = 0x0120;
	env->io_condition_count = 2;
	env->table_io_flag_condition = 0x0130;
	env->io_flag_condition_count = 1;
	env->table_macro = 0x0140;
	env->macro_count = 2;
	env->table_script = 0x0150;
	env->table_xlat = 0x0160;
	env->ram_restrict_groups = 2;
	env->ramcfg_index = 1;
	env->head = 1;
	env->or = 1;
	env->link = 2;
	return env;
}

// ---------------------------------------------------------------------------
// Golden vector: the exact trace of MAIN, hand-derived opcode by opcode from
// nvkm/subdev/bios/init.c semantics. Fields {address, value, index,
// script_offset, opcode, kind}. Serves double duty: it pins the interpreter
// to master's behavior AND documents every opcode's side effects.
// ---------------------------------------------------------------------------

static const struct pg106_init_trace_rec GOLDEN[] = {
	// n00 INIT_ZM_REG
	{0x001000, 0x12345678, 0, SLOT(0), PG106_INIT_ZM_REG, PG106_INIT_TRACE_MMIO},
	// n01 INIT_ZM_REG16
	{0x001004, 0x0000beef, 0, SLOT(1), PG106_INIT_ZM_REG16, PG106_INIT_TRACE_MMIO},
	// n02 INIT_ANDN_REG: (0xff & ~0xf0) | 0
	{0x001008, 0x0000000f, 0, SLOT(2), PG106_INIT_ANDN_REG, PG106_INIT_TRACE_MMIO},
	// n03 INIT_OR_REG: (0x0f & ~0) | 0xf000
	{0x001008, 0x0000f00f, 0, SLOT(3), PG106_INIT_OR_REG, PG106_INIT_TRACE_MMIO},
	// n04 INIT_NV_REG: (0x1122 & 0xff00) | 0xcd00
	{0x00100c, 0x0000dd00, 0, SLOT(4), PG106_INIT_NV_REG, PG106_INIT_TRACE_MMIO},
	// n05 INIT_COPY_NV_REG: (0x77 & ~0xf000) | (((0xa5a5 << 4) & 0xff0) ^ 0)
	{0x001014, 0x00000a77, 0, SLOT(5), PG106_INIT_COPY_NV_REG, PG106_INIT_TRACE_MMIO},
	// n06 INIT_ZM_REG_SEQUENCE
	{0x001020, 0x11111111, 0, SLOT(6), PG106_INIT_ZM_REG_SEQUENCE, PG106_INIT_TRACE_MMIO},
	{0x001024, 0x22222222, 0, SLOT(6), PG106_INIT_ZM_REG_SEQUENCE, PG106_INIT_TRACE_MMIO},
	// n07 INIT_ZM_REG_GROUP (three writes to one address)
	{0x001030, 0x00000001, 0, SLOT(7), PG106_INIT_ZM_REG_GROUP, PG106_INIT_TRACE_MMIO},
	{0x001030, 0x00000002, 0, SLOT(7), PG106_INIT_ZM_REG_GROUP, PG106_INIT_TRACE_MMIO},
	{0x001030, 0x00000003, 0, SLOT(7), PG106_INIT_ZM_REG_GROUP, PG106_INIT_TRACE_MMIO},
	// n08 INIT_RAM_RESTRICT_ZM_GROUP: strap group 1 of 2, 2 registers
	{0x001040, 0xaaaa0001, 0, SLOT(8), PG106_INIT_RAM_RESTRICT_ZM_GROUP, PG106_INIT_TRACE_MMIO},
	{0x001044, 0xbbbb0001, 0, SLOT(8), PG106_INIT_RAM_RESTRICT_ZM_GROUP, PG106_INIT_TRACE_MMIO},
	// n09 INIT_COPY_ZM_REG
	{0x001048, 0x12345678, 0, SLOT(9), PG106_INIT_COPY_ZM_REG, PG106_INIT_TRACE_MMIO},
	// n10 INIT_ZM_MASK_ADD: (0x1500 & 0xf000) | (0x1700 & ~0xf000)
	{0x00104c, 0x00001700, 0, SLOT(10), PG106_INIT_ZM_MASK_ADD, PG106_INIT_TRACE_MMIO},
	// n11 INIT_INDEX_ADDRESS_LATCHED (data write, ctrl mask-update, x2)
	{0x001054, 0x000000aa, 0, SLOT(11), PG106_INIT_INDEX_ADDRESS_LATCHED, PG106_INIT_TRACE_MMIO},
	{0x001050, 0x0000aa10, 0, SLOT(11), PG106_INIT_INDEX_ADDRESS_LATCHED, PG106_INIT_TRACE_MMIO},
	{0x001054, 0x000000bb, 0, SLOT(11), PG106_INIT_INDEX_ADDRESS_LATCHED, PG106_INIT_TRACE_MMIO},
	{0x001050, 0x0000aa11, 0, SLOT(11), PG106_INIT_INDEX_ADDRESS_LATCHED, PG106_INIT_TRACE_MMIO},
	// n12 INIT_XLAT: xlat0[2] = 0x33 << 4 = 0x330 into low byte of 0xcc00
	{0x00105c, 0x0000cf30, 0, SLOT(12), PG106_INIT_XLAT, PG106_INIT_TRACE_MMIO},
	// n13 INIT_IO_RESTRICT_PROG: conf 1 -> second entry
	{0x001060, 0x22220000, 0, SLOT(13), PG106_INIT_IO_RESTRICT_PROG, PG106_INIT_TRACE_MMIO},
	// n14 INIT_ZM_INDEX_IO
	{0x03c4, 0x00000077, 0x44, SLOT(14), PG106_INIT_ZM_INDEX_IO, PG106_INIT_TRACE_IO},
	// n15 INIT_INDEX_IO: (0x35 & 0x0f) | 0x80
	{0x03d4, 0x00000085, 0x61, SLOT(15), PG106_INIT_INDEX_IO, PG106_INIT_TRACE_IO},
	// n16 INIT_ZM_CR
	{0x03d4, 0x00000042, 0x75, SLOT(16), PG106_INIT_ZM_CR, PG106_INIT_TRACE_IO},
	// n17 INIT_CR: (0xff & 0x0f) | 0x30
	{0x03d4, 0x0000003f, 0x76, SLOT(17), PG106_INIT_CR, PG106_INIT_TRACE_IO},
	// n18 INIT_ZM_CR_GROUP
	{0x03d4, 0x00000011, 0x77, SLOT(18), PG106_INIT_ZM_CR_GROUP, PG106_INIT_TRACE_IO},
	{0x03d4, 0x00000022, 0x78, SLOT(18), PG106_INIT_ZM_CR_GROUP, PG106_INIT_TRACE_IO},
	// n19 INIT_CR_INDEX_ADDRESS_LATCHED (0x40, 0xde, 0x41, 0xad, restore 0x00)
	{0x03d4, 0x00000040, 0x30, SLOT(19), PG106_INIT_CR_INDEX_ADDRESS_LATCHED, PG106_INIT_TRACE_IO},
	{0x03d4, 0x000000de, 0x31, SLOT(19), PG106_INIT_CR_INDEX_ADDRESS_LATCHED, PG106_INIT_TRACE_IO},
	{0x03d4, 0x00000041, 0x30, SLOT(19), PG106_INIT_CR_INDEX_ADDRESS_LATCHED, PG106_INIT_TRACE_IO},
	{0x03d4, 0x000000ad, 0x31, SLOT(19), PG106_INIT_CR_INDEX_ADDRESS_LATCHED, PG106_INIT_TRACE_IO},
	{0x03d4, 0x00000000, 0x30, SLOT(19), PG106_INIT_CR_INDEX_ADDRESS_LATCHED, PG106_INIT_TRACE_IO},
	// n20 INIT_COPY: (0x00 & 0xf3) | ((0xa0 >> 2) & 0x0c)
	{0x03d4, 0x00000008, 0x79, SLOT(20), PG106_INIT_COPY, PG106_INIT_TRACE_IO},
	// n22 INIT_ZM_REG after met INIT_CONDITION (n21 produces no records;
	//     n23/n24/n27/n44/n45 etc. are the disabled-execution window)
	{0x001070, 0x00001111, 0, SLOT(22), PG106_INIT_ZM_REG, PG106_INIT_TRACE_MMIO},
	// n33 INIT_ZM_REG after met STRAP + RAM conditions
	{0x001080, 0x00001366, 0, SLOT(28), PG106_INIT_ZM_REG, PG106_INIT_TRACE_MMIO},
	// n34 INIT_SUB 0 -> script 0x0700 (record attributed to the sub-offset)
	{0x001090, 0x0badf00d, 0, 0x0700, PG106_INIT_ZM_REG, PG106_INIT_TRACE_MMIO},
	// n36 INIT_SUB_DIRECT 0x0720
	{0x001094, 0x0000600d, 0, 0x0720, PG106_INIT_ZM_REG16, PG106_INIT_TRACE_MMIO},
	// n37/n38 INIT_MACRO
	{0x00e000, 0xdead0001, 0, SLOT(32), PG106_INIT_MACRO, PG106_INIT_TRACE_MMIO},
	{0x00e004, 0x0000beef, 0, SLOT(33), PG106_INIT_MACRO, PG106_INIT_TRACE_MMIO},
	// n39 INIT_REPEAT x2: same body record twice, same script offset
	{0x0010a0, 0x0000c0ff, 0, SLOT(34) + 2, PG106_INIT_ZM_REG16, PG106_INIT_TRACE_MMIO},
	{0x0010a0, 0x0000c0ff, 0, SLOT(34) + 2, PG106_INIT_ZM_REG16, PG106_INIT_TRACE_MMIO},
	// n40 INIT_TIME 250 us; n41 INIT_LTIME 5 ms
	{0x00000000, 0x000000fa, 0, SLOT(35), PG106_INIT_TIME, PG106_INIT_TRACE_TIME},
	{0x00000000, 0x00001388, 0, SLOT(36), PG106_INIT_LTIME, PG106_INIT_TRACE_TIME},
	// n46 INIT_IO 0x03c3/0x01 special case (init.c:1640-1651), then wrport
	// (0x11100000 & 0x0f7fffff) | 0x00800000 — the mask clears bit 28
	{0x614100, 0x01900000, 0, SLOT(39), PG106_INIT_IO, PG106_INIT_TRACE_MMIO},
	{0x00e18c, 0x00020000, 0, SLOT(39), PG106_INIT_IO, PG106_INIT_TRACE_MMIO},
	{0x614900, 0x00800000, 0, SLOT(39), PG106_INIT_IO, PG106_INIT_TRACE_MMIO},
	{0x000200, 0x00000000, 0, SLOT(39), PG106_INIT_IO, PG106_INIT_TRACE_MMIO},
	{0x00000000, 0x00002710, 0, SLOT(39), PG106_INIT_IO, PG106_INIT_TRACE_TIME},
	{0x00e18c, 0x00000000, 0, SLOT(39), PG106_INIT_IO, PG106_INIT_TRACE_MMIO},
	{0x000200, 0x40000000, 0, SLOT(39), PG106_INIT_IO, PG106_INIT_TRACE_MMIO},
	{0x614100, 0x00800018, 0, SLOT(39), PG106_INIT_IO, PG106_INIT_TRACE_MMIO},
	{0x614900, 0x00800018, 0, SLOT(39), PG106_INIT_IO, PG106_INIT_TRACE_MMIO},
	{0x00000000, 0x00002710, 0, SLOT(39), PG106_INIT_IO, PG106_INIT_TRACE_TIME},
	{0x614100, 0x10000018, 0, SLOT(39), PG106_INIT_IO, PG106_INIT_TRACE_MMIO},
	{0x614900, 0x10000018, 0, SLOT(39), PG106_INIT_IO, PG106_INIT_TRACE_MMIO},
	{0x03c3, 0x00000001, 0, SLOT(39), PG106_INIT_IO, PG106_INIT_TRACE_IO},
	// n47 INIT_RESET (forced; save/restore of 0x184c around the two writes)
	{0x00184c, 0x00000000, 0, SLOT(40), PG106_INIT_RESET, PG106_INIT_TRACE_MMIO},
	{0x001100, 0x00000001, 0, SLOT(40), PG106_INIT_RESET, PG106_INIT_TRACE_MMIO},
	{0x00000000, 0x0000000a, 0, SLOT(40), PG106_INIT_RESET, PG106_INIT_TRACE_TIME},
	{0x001100, 0x00000002, 0, SLOT(40), PG106_INIT_RESET, PG106_INIT_TRACE_MMIO},
	{0x00184c, 0x00000300, 0, SLOT(40), PG106_INIT_RESET, PG106_INIT_TRACE_MMIO},
	{0x001850, 0x00000000, 0, SLOT(40), PG106_INIT_RESET, PG106_INIT_TRACE_MMIO},
	// n53 INIT_ZM_REG_INDIRECT — nvkm writes to `addr` (0x300), bug-compatible
	{0x00000300, 0x00c0ffee, 0, SLOT(46), PG106_INIT_ZM_REG_INDIRECT, PG106_INIT_TRACE_MMIO},
	// n54 INIT_IO_MASK_OR (or = 1): 0xff & ~0x02
	{0x03d4, 0x000000fd, 0x7a, SLOT(47), PG106_INIT_IO_MASK_OR, PG106_INIT_TRACE_IO},
	// n55 INIT_IO_OR (or = 1): 0x00 | 0x02
	{0x03d4, 0x00000002, 0x7b, SLOT(48), PG106_INIT_IO_OR, PG106_INIT_TRACE_IO},
	// n56 INIT_ZM_REG head-mangled (+0x800)
	{0x00610840, 0x00000055, 0, SLOT(49), PG106_INIT_ZM_REG, PG106_INIT_TRACE_MMIO},
	// n57 INIT_ZM_REG or/link-mangled (+0x800, +0x80)
	{0x00610940, 0x00000066, 0, SLOT(50), PG106_INIT_ZM_REG, PG106_INIT_TRACE_MMIO},
};

static void
print_rec(const char *tag, unsigned i, const struct pg106_init_trace_rec *r)
{
	printf("  %s[%u] @0x%04x op 0x%02x: addr 0x%08x val 0x%08x idx 0x%02x "
	       "kind %u\n", tag, i, r->script_offset, r->opcode, r->address,
	       r->value, r->index, r->kind);
}

static void
test_golden_trace(void)
{
	static struct pg106_init_env env1, env2;
	static struct pg106_init_trace_rec buf1[256], buf2[256];

	// Run 1.
	setup_env(&env1, buf1, 256);
	int rc = pg106_init_run(&env1, SLOT(0));
	REQUIRE(rc == PG106_INIT_OK);
	if (rc != PG106_INIT_OK)
		printf("  run1 error: %s\n", env1.error_msg);
	REQUIRE(env1.warnings == 0);
	REQUIRE(gpio_resets == 1);
	REQUIRE(fake_clock == 25260); // 250 + 5000 + 2x10000 + 10 us

	// Run 2 from identical inputs: Phase 1 determinism gate on the
	// synthetic substitute.
	setup_env(&env2, buf2, 256);
	rc = pg106_init_run(&env2, SLOT(0));
	REQUIRE(rc == PG106_INIT_OK);

	if (env1.trace_len != env2.trace_len) {
		printf("FAIL determinism: run1 %zu records, run2 %zu\n",
		       env1.trace_len, env2.trace_len);
		failures++;
	}
	size_t n = env1.trace_len < env2.trace_len ? env1.trace_len : env2.trace_len;
	for (size_t i = 0; i < n; i++) {
		if (memcmp(&buf1[i], &buf2[i], sizeof(buf1[i])) != 0) {
			printf("FAIL determinism at record %zu:\n", i);
			print_rec("run1", (unsigned)i, &buf1[i]);
			print_rec("run2", (unsigned)i, &buf2[i]);
			failures++;
			break;
		}
	}

	// Golden comparison.
	if (env1.trace_len != sizeof(GOLDEN) / sizeof(GOLDEN[0])) {
		printf("FAIL golden: expected %zu records, got %zu\n",
		       sizeof(GOLDEN) / sizeof(GOLDEN[0]), env1.trace_len);
		failures++;
	}
	size_t g = sizeof(GOLDEN) / sizeof(GOLDEN[0]);
	if (env1.trace_len < g)
		g = env1.trace_len;
	for (size_t i = 0; i < g; i++) {
		if (memcmp(&buf1[i], &GOLDEN[i], sizeof(GOLDEN[i])) != 0) {
			printf("FAIL golden mismatch at record %zu:\n", i);
			print_rec("want", (unsigned)i, &GOLDEN[i]);
			print_rec("got ", (unsigned)i, &buf1[i]);
			failures++;
		}
	}

	ok("devinit: synthetic script trace matches 67-record golden vector");
	ok("devinit: second run diffs clean against the first (determinism)");
}

// ---------------------------------------------------------------------------
// Error paths: every failure must be loud, coded, and name the opcode.
// ---------------------------------------------------------------------------

static int
expect_error(const char *name, uint16_t script, int want_status,
	     const char *msg_substr, uint8_t want_opcode)
{
	static struct pg106_init_env env;
	static struct pg106_init_trace_rec buf[16];

	setup_env(&env, buf, 16);
	int rc = pg106_init_run(&env, script);
	int good = (rc == want_status);
	if (good && want_opcode && env.error_opcode != want_opcode)
		good = 0;
	if (good && msg_substr && !strstr(env.error_msg, msg_substr))
		good = 0;
	if (!good) {
		printf("FAIL %s: status %d (%s), opcode 0x%02x, msg: %s\n",
		       name, rc, pg106_init_strerr(rc), env.error_opcode,
		       env.error_msg);
		failures++;
	} else {
		printf("  %s -> %s: \"%s\"\n", name, pg106_init_strerr(rc),
		       env.error_msg);
	}
	return good;
}

static void
test_error_paths(void)
{
	int all = 1;

	// Byte 0xfe is not in nvkm's dispatch table (init.c:2309-2312).
	all &= expect_error("bad opcode", 0x0740, PG106_INIT_ERR_OPCODE,
			    "not in nvkm dispatch", 0xfe);
	// 0x4c INIT_I2C_BYTE is in nvkm's table but needs an I2C bus.
	all &= expect_error("unimplemented opcode", 0x0750, PG106_INIT_ERR_OPCODE,
			    "INIT_I2C_BYTE", 0x4c);
	// INIT_ZM_REG value operand crosses the image end.
	all &= expect_error("script overrun", 0x1ff8, PG106_INIT_ERR_OVERRUN,
			    "past image end", 0x7a);
	// INIT_JUMP-to-self with a 64-step budget.
	{
		static struct pg106_init_env env;
		static struct pg106_init_trace_rec buf[8];
		setup_env(&env, buf, 8);
		env.step_limit = 64;
		int rc = pg106_init_run(&env, 0x0760);
		REQUIRE(rc == PG106_INIT_ERR_STEPS);
		if (rc != PG106_INIT_ERR_STEPS) {
			printf("  runaway status %d: %s\n", rc, env.error_msg);
			failures++;
		}
		all &= (rc == PG106_INIT_ERR_STEPS);
	}
	// INIT_SUB_DIRECT pointing at itself hits the nesting cap.
	all &= expect_error("sub-script runaway", 0x0780, PG106_INIT_ERR_DEPTH,
			    "nesting", 0x5b);
	// INIT_CONDITION without a condition table.
	{
		static struct pg106_init_env env;
		static struct pg106_init_trace_rec buf[8];
		apply_presets();
		pg106_init_env_init(&env, &test_vt, img, sizeof(img), buf, 8);
		int rc = pg106_init_run(&env, 0x0770);
		REQUIRE(rc == PG106_INIT_ERR_NO_TABLE);
		all &= (rc == PG106_INIT_ERR_NO_TABLE);
	}
	// INIT_SUB without a script table.
	{
		static struct pg106_init_env env;
		static struct pg106_init_trace_rec buf[8];
		apply_presets();
		pg106_init_env_init(&env, &test_vt, img, sizeof(img), buf, 8);
		int rc = pg106_init_run(&env, 0x0790);
		REQUIRE(rc == PG106_INIT_ERR_NO_TABLE);
		all &= (rc == PG106_INIT_ERR_NO_TABLE);
	}
	// Condition/macro index outside the supplied entry counts.
	all &= expect_error("condition index out of range", 0x07a0,
			    PG106_INIT_ERR_RANGE, "condition", 0x75);
	all &= expect_error("macro index out of range", 0x07b0,
			    PG106_INIT_ERR_RANGE, "macro", 0x6f);
	// Trace buffer too small for the main script (67 records).
	{
		static struct pg106_init_env env;
		static struct pg106_init_trace_rec buf[4];
		setup_env(&env, buf, 4);
		int rc = pg106_init_run(&env, SLOT(0));
		REQUIRE(rc == PG106_INIT_ERR_TRACE_FULL);
		REQUIRE(env.trace_len == 4);
		all &= (rc == PG106_INIT_ERR_TRACE_FULL);
	}
	// NULL vtable rejected.
	{
		static struct pg106_init_env env;
		static struct pg106_init_trace_rec buf[8];
		apply_presets();
		pg106_init_env_init(&env, NULL, img, sizeof(img), buf, 8);
		REQUIRE(pg106_init_run(&env, SLOT(0)) == PG106_INIT_ERR_BADARG);
	}

	if (all)
		ok("devinit: error paths fail loudly with named opcode/status");
}

static void
test_opcode_metadata(void)
{
	REQUIRE(strcmp(pg106_init_opcode_name(0x7a), "INIT_ZM_REG") == 0);
	REQUIRE(strcmp(pg106_init_opcode_name(0x71), "INIT_DONE") == 0);
	REQUIRE(strcmp(pg106_init_opcode_name(0x8f), "INIT_RAM_RESTRICT_ZM_GROUP") == 0);
	REQUIRE(strcmp(pg106_init_opcode_name(0x4c), "INIT_I2C_BYTE") == 0);
	REQUIRE(strcmp(pg106_init_opcode_name(0xfe), "UNKNOWN") == 0);
	REQUIRE(pg106_init_opcode_implemented(0x7a));
	REQUIRE(pg106_init_opcode_implemented(0x71));
	REQUIRE(!pg106_init_opcode_implemented(0x4c));  // I2C
	REQUIRE(!pg106_init_opcode_implemented(0x79));  // PLL
	REQUIRE(!pg106_init_opcode_implemented(0x98));  // AUX
	REQUIRE(!pg106_init_opcode_implemented(0xfe));  // not in nvkm table
	REQUIRE(strcmp(pg106_init_strerr(PG106_INIT_OK), "OK") == 0);
	REQUIRE(strcmp(pg106_init_strerr(PG106_INIT_ERR_OPCODE),
		       "unknown/unimplemented opcode") == 0);
	ok("devinit: opcode name/implementation tables resolve");
}

int
main(void)
{
	// Simulation disclaimer is part of the output, per HANDOFF-PASCAL.md
	// §7 evidence rules.
	printf("devinit tests — SIMULATION ONLY, not hardware evidence "
	       "(synthetic script; real VBIOS pending, BLOCKERS.md B1)\n");

	build_image();
	test_golden_trace();
	test_error_paths();
	test_opcode_metadata();

	if (failures) {
		printf("%d check(s) FAILED\n", failures);
		return EXIT_FAILURE;
	}
	printf("all checks passed\n");
	return EXIT_SUCCESS;
}
