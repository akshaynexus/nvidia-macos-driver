// SPDX-License-Identifier: GPL-2.0-only
// pascal/tests/test_top.c — golden vectors for the nvkm_top table reader
// (pg106_top_scan / pg106_top_find) and its wiring into the ACR-boot
// falcon base.
//
// **********************************************************************
// **  SIMULATION, NOT HARDWARE PROOF. The table images below are       **
// **  HAND-BUILT from the row format of nvkm/subdev/top/gk104.c        **
// **  (linux v5.15). Only GR 0x400000 (the BAR0 PGRAPH base, also in   **
// **  helios300-gp106-simtest.py section 2) and SEC2 0x3c0000 (the     **
// **  value PORT-AUDIT.md item 1 says must be confirmed on hardware)   **
// **  are externally corroborated; CE0 0x2e0000 is a format test       **
// **  vector, not a verified GP106 device value.                       **
// **********************************************************************
//
// Golden table layout (each row is one u32 at 0x022700 + i*4, gk104.c:43;
// row semantics gk104.c:45-67, chain bit gk104.c:69-70):
//
//   [0-2]   GR   type 0x00, addr 0x400000, fault 0x26, engine 0,
//           runlist 0, intr 8, reset 3 (O_: fixed inst 0)
//   [3-5]   SEC2 type 0x0d, addr 0x3c0000, engine 2, runlist 2, intr 10,
//           reset 8, no fault (O_: fixed inst 0)
//   [6-8]   CE0  type 0x01, addr 0x2e0000, engine 1, runlist 1, intr 9,
//           reset 4 (O_: fixed inst 0)
//   [9-10]  unknown type 0x11 (not in the gk104.c:75-95 map), DATA inst 5,
//           addr 0x123000 -> entry with type/inst = -1 (upstream
//           "????????", gk104.c:93-94 + base.c:31)
//   [11-12] NOT_VALID rows (gk104.c:46-47): one all-zero, one with
//           garbage upper bits — both must be skipped without closing
//           the pending entry.
//
// Run: cd pascal && make test   (or build/test_top from pascal/)

#include <pascal/acr_boot.h>
#include <pascal/top.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond)) {                                                 \
			printf("FAIL %s:%d: %s\n", __func__, __LINE__, #cond); \
			g_failures++;                                          \
		}                                                              \
	} while (0)

#define CHECK_EQ(a, b)                                                         \
	do {                                                                   \
		unsigned long long va_ = (unsigned long long)(a);              \
		unsigned long long vb_ = (unsigned long long)(b);              \
		if (va_ != vb_) {                                              \
			printf("FAIL %s:%d: %s == %s (0x%llx != 0x%llx)\n",    \
			       __func__, __LINE__, #a, #b, va_, vb_);          \
			g_failures++;                                          \
		}                                                              \
	} while (0)

// --- row builders (bit layout per nvkm/subdev/top/gk104.c:45-67) ------------

#define CHAIN_MORE 0x80000000u // gk104.c:69 (0 = last row of a device)

// gk104.c:64-66: (data & 0x7ffffffc) >> 2 -> word = (type << 2) | 3.
static uint32_t
row_type(uint32_t type, bool more)
{
	return ((type << 2) | 0x3u) | (more ? CHAIN_MORE : 0);
}

// gk104.c:54-63: base 2, each field gated by its presence bit.
static uint32_t
row_enum(int engine, int runlist, int intr, int reset, bool more)
{
	uint32_t w = 0x2u;

	if (engine >= 0)
		w |= 0x20u | ((uint32_t)engine << 26);
	if (runlist >= 0)
		w |= 0x10u | ((uint32_t)runlist << 21);
	if (intr >= 0)
		w |= 0x08u | ((uint32_t)intr << 15);
	if (reset >= 0)
		w |= 0x04u | ((uint32_t)reset << 9);
	return w | (more ? CHAIN_MORE : 0);
}

// gk104.c:48-53: base 1, inst bits[29:26], addr bits[23:12], fault bits[9:3]
// gated by bit 2.
static uint32_t
row_data(uint32_t inst, uint32_t addr, int fault, bool more)
{
	uint32_t w = 0x1u | ((inst & 0xfu) << 26) | (addr & 0x00fff000u);

	if (fault >= 0)
		w |= 0x4u | (((uint32_t)fault << 3) & 0x3f8u);
	return w | (more ? CHAIN_MORE : 0);
}

// --- the golden table image --------------------------------------------------

static const uint32_t g_golden[] = {
	0x80000003, // [0]  GR   ENGINE_TYPE 0x00 (0x00<<2|3), CHAIN
	0x8004063e, // [1]  GR   ENUM engine 0, runlist 0, intr 8, reset 3
	0x00400135, // [2]  GR   DATA inst 0, addr 0x400000, fault 0x26, LAST
	0x80000037, // [3]  SEC2 ENGINE_TYPE 0x0d (0x0d<<2|3), CHAIN
	0x8845103e, // [4]  SEC2 ENUM engine 2, runlist 2, intr 10, reset 8
	0x003c0001, // [5]  SEC2 DATA inst 0, addr 0x3c0000, no fault, LAST
	0x80000007, // [6]  CE0  ENGINE_TYPE 0x01 (0x01<<2|3), CHAIN
	0x8424883e, // [7]  CE0  ENUM engine 1, runlist 1, intr 9, reset 4
	0x002e0001, // [8]  CE0  DATA inst 0, addr 0x2e0000, LAST
	0x80000047, // [9]  unknown ENGINE_TYPE 0x11 (not in the map), CHAIN
	0x14123001, // [10] unknown DATA inst 5, addr 0x123000, LAST
	0x00000000, // [11] NOT_VALID
	0x12345678, // [12] NOT_VALID (garbage upper bits, bits[1:0] = 0)
};

struct top_image {
	const uint32_t *rows;
	size_t n;
};

// Modeled bus: reads inside the 64-row NV_PTOP window, 0 past the end of
// the image (unimplemented PRI space reads as 0 = NOT_VALID, gk104.c:46),
// -EIO outside the window (must never happen — bounds the scan).
static int
img_rd32(void *ud, uint32_t addr, uint32_t *v)
{
	struct top_image *t = ud;
	size_t i;

	if (addr < PG106_TOP_INDEX ||
	    addr >= PG106_TOP_INDEX + PG106_TOP_MAX_ROWS * PG106_TOP_ROW_STRIDE ||
	    (addr - PG106_TOP_INDEX) % PG106_TOP_ROW_STRIDE != 0)
		return -EIO;
	i = (addr - PG106_TOP_INDEX) / PG106_TOP_ROW_STRIDE;
	*v = i < t->n ? t->rows[i] : 0;
	return 0;
}

// Modeled bus that fails the Nth read (1-based) with -EIO.
struct failing_bus {
	struct top_image img;
	size_t fail_on; // 1-based read number to fail at
	size_t reads;
};

static int
failing_rd32(void *ud, uint32_t addr, uint32_t *v)
{
	struct failing_bus *b = ud;

	b->reads++;
	if (b->reads == b->fail_on)
		return -EIO;
	return img_rd32(&b->img, addr, v);
}

// --- 1. golden literals are what the format builders produce -----------------

static void
test_golden_literals(void)
{
	uint32_t built[13];

	built[0] = row_type(0x00, true);
	built[1] = row_enum(0, 0, 8, 3, true);
	built[2] = row_data(0, 0x400000, 0x26, false);
	built[3] = row_type(0x0d, true);
	built[4] = row_enum(2, 2, 10, 8, true);
	built[5] = row_data(0, 0x3c0000, -1, false);
	built[6] = row_type(0x01, true);
	built[7] = row_enum(1, 1, 9, 4, true);
	built[8] = row_data(0, 0x2e0000, -1, false);
	built[9] = row_type(0x11, true);
	built[10] = row_data(5, 0x123000, -1, false);
	built[11] = 0x00000000;
	built[12] = 0x12345678;

	// The committed literals and the format-derived builders must agree
	// byte for byte — this is what makes the literals reviewable.
	CHECK(memcmp(built, g_golden, sizeof(g_golden)) == 0);
}

// --- 2. the golden table scans into the expected devices ----------------------

static void
test_golden_scan(void)
{
	struct top_image img = { g_golden, sizeof(g_golden) / sizeof(g_golden[0]) };
	struct pg106_top_table tbl;
	const struct pg106_top_device *d;
	int ret;

	ret = pg106_top_scan(img_rd32, &img, &tbl);
	CHECK_EQ(ret, 0);
	CHECK_EQ(tbl.ndev, 4); // GR + SEC2 + CE0 + unknown
	CHECK_EQ(tbl.nrows, PG106_TOP_MAX_ROWS); // always reads the full window

	printf("golden table yields (SIM vectors):\n");
	for (size_t i = 0; i < tbl.ndev; i++) {
		d = &tbl.dev[i];
		printf("  [%zu] type %4d inst %2d addr 0x%06x fault %2d "
		       "engine %2d runlist %2d intr %2d reset %2d "
		       "(last_row %d)\n",
		       i, d->type, d->inst, d->addr, d->fault, d->engine,
		       d->runlist, d->intr, d->reset, d->last_row);
	}

	// GR (O_ type: fixed inst 0), full field decode.
	d = pg106_top_find(&tbl, PG106_TOP_TYPE_GR, 0);
	CHECK(d != NULL);
	if (d) {
		CHECK_EQ(d->type, PG106_TOP_TYPE_GR);
		CHECK_EQ(d->inst, 0);
		CHECK_EQ(d->addr, 0x400000);
		CHECK_EQ(d->fault, 0x26);
		CHECK_EQ(d->engine, 0);
		CHECK_EQ(d->runlist, 0);
		CHECK_EQ(d->intr, 8);
		CHECK_EQ(d->reset, 3);
		CHECK_EQ(d->last_row, 2);
		CHECK_EQ(pg106_top_reset_mask(d), 1u << 3);
	}

	// SEC2 — the entry the ACR boot consumes (falcon/base.c:143-147).
	d = pg106_top_find(&tbl, PG106_TOP_TYPE_SEC2, 0);
	CHECK(d != NULL);
	if (d) {
		CHECK_EQ(d->type, PG106_TOP_TYPE_SEC2);
		CHECK_EQ(d->inst, 0);
		CHECK_EQ(d->addr, 0x003c0000);
		CHECK_EQ(d->fault, -1); // no fault field in row [5]
		CHECK_EQ(d->engine, 2);
		CHECK_EQ(d->runlist, 2);
		CHECK_EQ(d->intr, 10);
		CHECK_EQ(d->reset, 8);
		CHECK_EQ(d->last_row, 5);
		CHECK_EQ(pg106_top_reset_mask(d), 1u << 8);
	}

	// CE0.
	d = pg106_top_find(&tbl, PG106_TOP_TYPE_CE0, 0);
	CHECK(d != NULL);
	if (d) {
		CHECK_EQ(d->addr, 0x2e0000);
		CHECK_EQ(d->reset, 4);
		CHECK_EQ(pg106_top_reset_mask(d), 1u << 4);
	}

	// Unknown type row group: kept with type/inst = -1 ("????????"),
	// but its DATA-row addr survives.
	CHECK(tbl.ndev == 4);
	d = &tbl.dev[3];
	CHECK_EQ(d->type, PG106_TOP_TYPE_UNKNOWN);
	CHECK_EQ(d->inst, -1);
	CHECK_EQ(d->addr, 0x123000);

	// Not-present lookups (nvkm_top_addr returns 0; we return NULL).
	CHECK(pg106_top_find(&tbl, PG106_TOP_TYPE_SEC2, 1) == NULL);
	CHECK(pg106_top_find(&tbl, PG106_TOP_TYPE_CE1, 0) == NULL);
	CHECK(pg106_top_find(&tbl, PG106_TOP_TYPE_MSVLD, 0) == NULL);
}

// --- 3. instance addressing: I_ types take the DATA-row instance --------------

static void
test_instance_addressing(void)
{
	// NVDEC (type 0x10) is an I_ row (gk104.c:88): inst comes from the
	// DATA row. CE0 (type 0x01) is O_ (gk104.c:77): inst is fixed 0 even
	// when the DATA row says otherwise (upstream WARN_ON(inst), gk104.c:74).
	const uint32_t rows[] = {
		row_type(0x10, true),		 // NVDEC ENGINE_TYPE
		row_data(3, 0x00340000, -1, false), // inst 3, LAST
		row_type(0x01, true),		 // CE0 ENGINE_TYPE
		row_data(7, 0x002e0000, -1, false), // inst 7 must be ignored
	};
	struct top_image img = { rows, sizeof(rows) / sizeof(rows[0]) };
	struct pg106_top_table tbl;
	const struct pg106_top_device *d;

	CHECK_EQ(pg106_top_scan(img_rd32, &img, &tbl), 0);
	CHECK_EQ(tbl.ndev, 2);

	d = pg106_top_find(&tbl, PG106_TOP_TYPE_NVDEC, 3);
	CHECK(d != NULL);
	if (d)
		CHECK_EQ(d->addr, 0x340000);
	CHECK(pg106_top_find(&tbl, PG106_TOP_TYPE_NVDEC, 0) == NULL);

	d = pg106_top_find(&tbl, PG106_TOP_TYPE_CE0, 0); // O_: inst forced 0
	CHECK(d != NULL);
	if (d)
		CHECK_EQ(d->addr, 0x2e0000);
	CHECK(pg106_top_find(&tbl, PG106_TOP_TYPE_CE0, 7) == NULL);
}

// --- 4. termination + truncation -----------------------------------------------

static void
test_termination(void)
{
	// A device group whose rows never carry a chain-last row is dropped:
	// the scan ends at the fixed 64-row window (gk104.c:35) with the
	// entry still pending (upstream never finalizes `info` either).
	const uint32_t truncated[] = {
		row_type(0x0d, true),		 // SEC2, CHAIN
		row_enum(2, 2, 10, 8, true),	 // CHAIN
		row_data(0, 0x3c0000, -1, true), // CHAIN — never closed
	};
	struct top_image img = { truncated,
				 sizeof(truncated) / sizeof(truncated[0]) };
	struct pg106_top_table tbl;

	CHECK_EQ(pg106_top_scan(img_rd32, &img, &tbl), 0);
	CHECK_EQ(tbl.ndev, 0);
	CHECK(pg106_top_find(&tbl, PG106_TOP_TYPE_SEC2, 0) == NULL);

	// NOT_VALID rows do NOT terminate a device (gk104.c:46-47 keeps the
	// pending entry open): a gap between the ENUM and DATA rows of one
	// device still yields one complete entry.
	{
		const uint32_t gapped[] = {
			row_type(0x0d, true),
			0x00000000, // NOT_VALID in the middle
			row_enum(2, 2, 10, 8, true),
			row_data(0, 0x3c0000, -1, false),
		};
		struct top_image gimg = { gapped,
					  sizeof(gapped) / sizeof(gapped[0]) };

		CHECK_EQ(pg106_top_scan(img_rd32, &gimg, &tbl), 0);
		CHECK_EQ(tbl.ndev, 1);
		CHECK(pg106_top_find(&tbl, PG106_TOP_TYPE_SEC2, 0) != NULL);
	}
}

// --- 5. failure paths ------------------------------------------------------------

static void
test_failure_paths(void)
{
	struct top_image img = { g_golden, sizeof(g_golden) / sizeof(g_golden[0]) };
	struct pg106_top_table tbl;

	// Garbage/bus-fault table: the read error propagates verbatim as a
	// clean error, no partial devices are exposed. (Failing at read 3 =
	// the GR DATA row, before GR's entry closes, so the table stays
	// empty when the scan aborts.)
	{
		struct failing_bus bus = { .img = img, .fail_on = 3 };

		CHECK_EQ(pg106_top_scan(failing_rd32, &bus, &tbl), -EIO);
		CHECK_EQ(bus.reads, 3);
		CHECK_EQ(tbl.ndev, 0);
	}

	// All-zero table (device with an empty PRI window): clean empty scan.
	{
		struct top_image empty = { NULL, 0 };

		CHECK_EQ(pg106_top_scan(img_rd32, &empty, &tbl), 0);
		CHECK_EQ(tbl.ndev, 0);
		CHECK_EQ(tbl.nrows, PG106_TOP_MAX_ROWS);
		CHECK(pg106_top_find(&tbl, PG106_TOP_TYPE_SEC2, 0) == NULL);
	}

	// Argument validation.
	CHECK_EQ(pg106_top_scan(NULL, &img, &tbl), -EINVAL);
	CHECK_EQ(pg106_top_scan(img_rd32, &img, NULL), -EINVAL);
	CHECK(pg106_top_find(NULL, PG106_TOP_TYPE_SEC2, 0) == NULL);
	CHECK_EQ(pg106_top_reset_mask(NULL), 0);
}

// --- 6. ACR-boot integration: falcon base from the table -------------------------

static void
test_acrboot_falcon_base(void)
{
	struct top_image img = { g_golden, sizeof(g_golden) / sizeof(g_golden[0]) };
	struct pg106_top_table tbl;
	uint32_t base;

	CHECK_EQ(pg106_top_scan(img_rd32, &img, &tbl), 0);
	base = pg106_acrboot_falcon_base(&tbl, 0xdead0000u);
	printf("WITH TABLE:    pg106_acrboot_falcon_base -> 0x%06x "
	       "(SEC2 from the golden nvkm_top image)\n",
	       base);
	CHECK_EQ(base, 0x003c0000);

	// Precedence proof: the table wins over the fallback when they
	// disagree — mutate the SEC2 DATA row to a different base.
	{
		uint32_t rows[sizeof(g_golden) / sizeof(g_golden[0])];

		memcpy(rows, g_golden, sizeof(g_golden));
		rows[5] = row_data(0, 0x3d0000, -1, false);
		{
			struct top_image mimg = { rows,
						  sizeof(rows) /
							  sizeof(rows[0]) };

			CHECK_EQ(pg106_top_scan(img_rd32, &mimg, &tbl), 0);
			base = pg106_acrboot_falcon_base(&tbl, 0x003c0000u);
			printf("WITH TABLE (mutated SEC2 row): -> 0x%06x "
			       "(table takes precedence)\n",
			       base);
			CHECK_EQ(base, 0x003d0000);
		}
	}

	// Fallback paths (the old SIM stand-in, loudly not-trusted on
	// hardware — PORT-AUDIT.md item 1).
	base = pg106_acrboot_falcon_base(NULL, 0x003c0000u);
	printf("WITHOUT TABLE: pg106_acrboot_falcon_base -> 0x%06x "
	       "(documented fallback; NOT trustworthy on hardware)\n",
	       base);
	CHECK_EQ(base, 0x003c0000);

	{
		struct top_image empty = { NULL, 0 };

		CHECK_EQ(pg106_top_scan(img_rd32, &empty, &tbl), 0);
		CHECK_EQ(pg106_acrboot_falcon_base(&tbl, 0xdead0000u),
			 0xdead0000u); // empty table -> fallback
	}
}

// --- main -------------------------------------------------------------------------

int
main(void)
{
	printf("**********************************************************************\n");
	printf("**  SIMULATION, NOT HARDWARE PROOF: hand-built nvkm_top table      **\n"
	       "**  images (format: nvkm/subdev/top/gk104.c, linux v5.15).         **\n");
	printf("**********************************************************************\n");

	test_golden_literals();
	test_golden_scan();
	test_instance_addressing();
	test_termination();
	test_failure_paths();
	test_acrboot_falcon_base();

	if (g_failures) {
		printf("== top: %d FAILURE(S)\n", g_failures);
		return 1;
	}
	printf("== top: PASS (simulation only — device bases need a real "
	       "table read)\n");
	return 0;
}
