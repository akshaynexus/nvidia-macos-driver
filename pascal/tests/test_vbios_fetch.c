// SPDX-License-Identifier: GPL-2.0-only
// pascal/tests/test_vbios_fetch.c — VBIOS assembly from a chunked source
// (core/src/vbios_fetch.c), with the Helios 300's own ROM served through a
// fake ACPI _ROM that behaves like the one the probe found
// (research/evidence/HELIOS300-HW-PROBE.md): at most 0x1000 bytes per call,
// RVBS = 0x29400 bytes in total.
//
// SIMULATION: the fake _ROM is a model of SSDT11's method, not the method
// itself. What it returns past RVBS on the real machine is not known, so
// both plausible behaviours (zero fill, short buffer) are tested. The ROM
// bytes are the real image (research/firmware/vbios/helios300-gp106.rom,
// gitignored); without it the real-ROM checks PASS-SKIP and the synthetic
// ones still run.
//
// Run from pascal/: make test (or $BUILD_DIR/test_vbios_fetch).

#include <pascal/vbios_bit.h>
#include <pascal/vbios_fetch.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

#define CHECK(c)                                                         \
	do {                                                             \
		if (!(c)) {                                              \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			g_fail++;                                        \
		}                                                        \
	} while (0)
#define CHECK_EQ(a, b)                                                   \
	do {                                                             \
		long long a_ = (long long)(a), b_ = (long long)(b);      \
		if (a_ != b_) {                                          \
			printf("FAIL %s:%d: %s == %s (0x%llx vs 0x%llx)\n", \
			       __FILE__, __LINE__, #a, #b, a_, b_);      \
			g_fail++;                                        \
		}                                                        \
	} while (0)

#define ROM_PATH "../research/firmware/vbios/helios300-gp106.rom"
#define HELIOS_RVBS 0x29400u

// ---- fake _ROM ------------------------------------------------------------

enum past_end { ZERO_FILL, SHORT_BUF };

struct fake_rom {
	const uint8_t *rom;
	uint32_t rvbs;      // bytes the method serves
	uint32_t cap;       // per-call cap (0x1000 on the Helios)
	enum past_end tail; // what a read past RVBS returns
	int fail_at_call;   // 1-based call that fails with -EIO (0 = never)
	unsigned calls;
	uint32_t max_len_seen;
	uint32_t misaligned; // calls whose offset is not cap-aligned
};

static int
fake_rom_read(void *user, uint32_t off, uint32_t len, uint8_t *dst)
{
	struct fake_rom *f = user;
	uint32_t n;

	f->calls++;
	if (len > f->max_len_seen)
		f->max_len_seen = len;
	if (f->cap && off % f->cap)
		f->misaligned++;
	if (f->fail_at_call && (int)f->calls == f->fail_at_call)
		return -EIO;
	if (f->cap && len > f->cap)
		len = f->cap; // the method clamps Arg1
	if (off >= f->rvbs) {
		if (f->tail == SHORT_BUF)
			return 0;
		memset(dst, 0, len);
		return (int)len;
	}
	n = f->rvbs - off < len ? f->rvbs - off : len;
	memcpy(dst, f->rom + off, n);
	if (n < len) {
		if (f->tail == SHORT_BUF)
			return (int)n;
		memset(dst + n, 0, len - n);
		return (int)len;
	}
	return (int)n;
}

static struct pg106_vbios_src
acpi_src(struct fake_rom *f)
{
	return (struct pg106_vbios_src){
		.name = "ACPI _ROM",
		.read = fake_rom_read,
		.user = f,
		.chunk = PG106_VBIOS_ACPI_CHUNK,
		.require_checksum = false,
	};
}

// ---- synthetic ROM ----------------------------------------------------------

static void
put16(uint8_t *p, uint32_t o, uint16_t v)
{
	p[o] = (uint8_t)v;
	p[o + 1] = (uint8_t)(v >> 8);
}

// One legacy image of `blocks` 512-byte blocks with a PCIR at 0x40 and
// (in image 0) a BIT header with no tokens at 0x100. type/last per image.
static void
synth_image(uint8_t *img, uint32_t blocks, uint16_t dev, uint8_t type,
	    bool last, bool bit)
{
	uint32_t size = blocks * 512u, i;
	uint8_t sum = 0;

	memset(img, 0, size);
	img[0] = 0x55;
	img[1] = 0xaa;
	put16(img, 0x18, 0x40);
	memcpy(img + 0x40, "PCIR", 4);
	put16(img, 0x44, 0x10de);
	put16(img, 0x46, dev);
	img[0x4d] = 0x00;
	img[0x4e] = 0x00;
	img[0x4f] = 0x03; // display controller
	put16(img, 0x50, (uint16_t)blocks);
	img[0x54] = type;
	img[0x55] = last ? 0x80 : 0x00;
	if (bit) {
		memcpy(img + 0x100, "\xff\xb8" "BIT", 5);
		img[0x100 + 9] = 6;  // entry stride
		img[0x100 + 10] = 0; // no tokens
	}
	if (type == 0x00) {
		for (i = 0; i < size - 1; i++)
			sum = (uint8_t)(sum + img[i]);
		img[size - 1] = (uint8_t)(0x100 - sum); // checksum to 0
	}
}

// image 0 (0x1200 bytes, type 0) + image 1 (0x600 bytes, type 0xe0, last).
#define SYN_SIZE 0x1800u
static void
synth_rom(uint8_t *rom, uint16_t dev)
{
	synth_image(rom, 9, dev, 0x00, false, true);
	synth_image(rom + 0x1200, 3, dev, 0xe0, true, false);
}

static uint8_t *
load(const char *path, size_t *n)
{
	FILE *f = fopen(path, "rb");
	uint8_t *b;
	long sz;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc(sz > 0 ? (size_t)sz : 1);
	if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) {
		free(b);
		b = NULL;
	}
	fclose(f);
	*n = (size_t)sz;
	return b;
}

// ---- tests ----------------------------------------------------------------

static void
test_synthetic(void)
{
	static uint8_t rom[SYN_SIZE];
	struct fake_rom f = { rom, SYN_SIZE, PG106_VBIOS_ACPI_CHUNK, ZERO_FILL, 0, 0, 0, 0 };
	struct pg106_vbios_src src = acpi_src(&f);
	struct pg106_vbios_fetch_result r;
	struct pg106_vbios_blob b;

	synth_rom(rom, 0x1c20);
	CHECK_EQ(pg106_vbios_fetch(&src, &b, &r), 0);
	CHECK_EQ(b.size, SYN_SIZE);
	CHECK(b.data && memcmp(b.data, rom, SYN_SIZE) == 0);
	CHECK_EQ(r.images, 2);
	CHECK(r.checksum_ok);
	CHECK_EQ(r.score, 4 + 4); // shadow_image: 1+3 per good image
	CHECK_EQ(r.pcir_device, 0x1c20);
	CHECK_EQ(f.max_len_seen, PG106_VBIOS_ACPI_CHUNK);
	CHECK_EQ(f.misaligned, 0);
	CHECK_EQ(pg106_vbios_fetch_validate(&b, 0x10de, 0x1c20, &r), 0);
	// Another GP106 SKU's ROM is refused for this PCI function.
	CHECK_EQ(pg106_vbios_fetch_validate(&b, 0x10de, 0x1c60, &r), -ENODEV);
	CHECK(r.error != NULL);
	CHECK_EQ(pg106_vbios_fetch_validate(&b, 0x8086, 0x1c20, NULL), -ENODEV);
	pg106_vbios_blob_free(&b);
	CHECK(b.data == NULL && b.size == 0);
	printf("PASS synthetic 2-image ROM via 0x1000-byte reads (score %d)\n",
	       8);

	// Short buffer past the end gives the same ROM.
	f = (struct fake_rom){ rom, SYN_SIZE, PG106_VBIOS_ACPI_CHUNK, SHORT_BUF, 0, 0, 0, 0 };
	CHECK_EQ(pg106_vbios_fetch(&src, &b, &r), 0);
	CHECK_EQ(b.size, SYN_SIZE);
	pg106_vbios_blob_free(&b);

	// A bad image-0 checksum is accepted with a lower score unless the
	// source requires it (shadow.c:85-94).
	rom[0x200] ^= 0x01;
	f = (struct fake_rom){ rom, SYN_SIZE, PG106_VBIOS_ACPI_CHUNK, ZERO_FILL, 0, 0, 0, 0 };
	CHECK_EQ(pg106_vbios_fetch(&src, &b, &r), 0);
	CHECK(!r.checksum_ok);
	CHECK_EQ(r.score, 2 + 4);
	pg106_vbios_blob_free(&b);
	src.require_checksum = true;
	CHECK_EQ(pg106_vbios_fetch(&src, &b, &r), -EBADMSG);
	CHECK(b.data == NULL);
	src.require_checksum = false;
	rom[0x200] ^= 0x01;
	printf("PASS checksum: scored without, refused with require_checksum\n");

	// The source stops inside image 1: "fetch failed".
	f = (struct fake_rom){ rom, 0x1400, PG106_VBIOS_ACPI_CHUNK, SHORT_BUF, 0, 0, 0, 0 };
	CHECK_EQ(pg106_vbios_fetch(&src, &b, &r), -EIO);
	CHECK(b.data == NULL);
	// An ACPI evaluation error on the second call.
	f = (struct fake_rom){ rom, SYN_SIZE, PG106_VBIOS_ACPI_CHUNK, ZERO_FILL, 2, 0, 0, 0 };
	CHECK_EQ(pg106_vbios_fetch(&src, &b, &r), -EIO);
	// Error on the very first call: nothing fetched.
	f = (struct fake_rom){ rom, SYN_SIZE, PG106_VBIOS_ACPI_CHUNK, ZERO_FILL, 1, 0, 0, 0 };
	CHECK_EQ(pg106_vbios_fetch(&src, &b, &r), -EIO);
	// No ROM signature at all (an all-zero _ROM, as a disabled method
	// returns).
	{
		static uint8_t zero[0x2000];

		f = (struct fake_rom){ zero, sizeof zero, PG106_VBIOS_ACPI_CHUNK, ZERO_FILL, 0, 0, 0, 0 };
		CHECK_EQ(pg106_vbios_fetch(&src, &b, &r), -ENOENT);
		CHECK(r.error != NULL);
	}
	CHECK_EQ(pg106_vbios_fetch(NULL, &b, &r), -EINVAL);
	CHECK_EQ(pg106_vbios_fetch(&src, NULL, &r), -EINVAL);
	printf("PASS failures: truncated source, ACPI error, no signature, bad args\n");
}

static void
test_fallback(void)
{
	static uint8_t good[SYN_SIZE], other[SYN_SIZE], zero[0x1000];
	struct fake_rom fa = { zero, sizeof zero, PG106_VBIOS_ACPI_CHUNK, SHORT_BUF, 0, 0, 0, 0 };
	struct fake_rom fp = { good, SYN_SIZE, 0, SHORT_BUF, 0, 0, 0, 0 };
	struct pg106_vbios_src a = acpi_src(&fa);
	struct pg106_vbios_src p = {
		.name = "PCI ROM", .read = fake_rom_read, .user = &fp,
		.chunk = 0, .require_checksum = true,
	};
	const struct pg106_vbios_src *srcs[2] = { &a, &p };
	struct pg106_vbios_fetch_result res[2];
	struct pg106_vbios_blob b;
	unsigned which = 99;

	synth_rom(good, 0x1c20);
	synth_rom(other, 0x1c02);
	// ACPI has nothing; the PCI ROM (one unchunked read per fetch) wins.
	CHECK_EQ(pg106_vbios_fetch_first(srcs, 2, 0x10de, 0x1c20, &b, &which,
					 res),
		 0);
	CHECK_EQ(which, 1);
	CHECK_EQ(res[0].ret, -ENOENT);
	CHECK_EQ(res[1].ret, 0);
	CHECK_EQ(b.size, SYN_SIZE);
	pg106_vbios_blob_free(&b);
	// ACPI serves another GPU's ROM: refused, PCI used instead.
	fa = (struct fake_rom){ other, SYN_SIZE, PG106_VBIOS_ACPI_CHUNK, ZERO_FILL, 0, 0, 0, 0 };
	fp.calls = 0;
	CHECK_EQ(pg106_vbios_fetch_first(srcs, 2, 0x10de, 0x1c20, &b, &which,
					 res),
		 0);
	CHECK_EQ(which, 1);
	CHECK_EQ(res[0].ret, -ENODEV);
	pg106_vbios_blob_free(&b);
	// Neither matches: the last error comes back and nothing is kept.
	fp.rom = other;
	CHECK_EQ(pg106_vbios_fetch_first(srcs, 2, 0x10de, 0x1c20, &b, &which,
					 res),
		 -ENODEV);
	CHECK(b.data == NULL);
	printf("PASS source order: ACPI _ROM first, PCI ROM fallback, PCIR id gate\n");
}

static void
test_real(void)
{
	size_t n = 0;
	uint8_t *rom = load(ROM_PATH, &n);
	struct pg106_vbios_fetch_result r;
	struct pg106_vbios_blob b;
	struct pg106_vbios_bit vb;
	struct pg106_vbios_pmu_app app;
	int tail;

	if (!rom) {
		printf("PASS-SKIP real ROM: " ROM_PATH " not present "
		       "(research/firmware/vbios/README.md)\n");
		return;
	}
	CHECK_EQ(n, HELIOS_RVBS);
	for (tail = ZERO_FILL; tail <= SHORT_BUF; tail++) {
		struct fake_rom f = { rom, HELIOS_RVBS, PG106_VBIOS_ACPI_CHUNK,
				      (enum past_end)tail, 0, 0, 0, 0 };
		struct pg106_vbios_src src = acpi_src(&f);

		CHECK_EQ(pg106_vbios_fetch(&src, &b, &r), 0);
		CHECK_EQ(b.size, HELIOS_RVBS);
		CHECK(b.data && memcmp(b.data, rom, HELIOS_RVBS) == 0);
		CHECK_EQ(r.images, 4); // 0x00 f200, 0xe0 b200, 0xe0 e600, 0x70 a00
		CHECK(r.checksum_ok);
		CHECK_EQ(r.score, 16);
		CHECK_EQ(f.max_len_seen, PG106_VBIOS_ACPI_CHUNK);
		CHECK_EQ(f.misaligned, 0);
		// 0x29400 rounds up to 42 chunks of 0x1000; never more.
		CHECK_EQ(f.calls, 42);
		CHECK_EQ(r.pcir_vendor, 0x10de);
		CHECK_EQ(r.pcir_device, 0x1c20);
		CHECK_EQ(pg106_vbios_fetch_validate(&b, 0x10de, 0x1c20, &r), 0);
		// The fetched ROM drives the POST model's inputs.
		CHECK_EQ(pg106_vbios_bit_parse(b.data, b.size, &vb), 0);
		CHECK_EQ(pg106_vbios_pmu_app(&vb, PG106_VBIOS_PMU_TYPE_DEVINIT,
					     &app),
			 0);
		CHECK_EQ(pg106_vbios_fetch_validate(&b, 0x10de, 0x1c60, NULL),
			 -ENODEV);
		pg106_vbios_blob_free(&b);
		printf("PASS real Helios ROM via fake _ROM (%s past RVBS): "
		       "0x%x bytes, %u reads, 4 images, PCIR 10de:1c20, "
		       "DEVINIT app found\n",
		       tail == ZERO_FILL ? "zero fill" : "short buffer",
		       HELIOS_RVBS, f.calls);
	}

	// If the method stops serving at 0xf000 (the first-image cut the
	// desktop dumps had), the ROM is refused, not half-used.
	{
		struct fake_rom f = { rom, 0xf000, PG106_VBIOS_ACPI_CHUNK, SHORT_BUF, 0, 0, 0, 0 };
		struct pg106_vbios_src src = acpi_src(&f);

		CHECK_EQ(pg106_vbios_fetch(&src, &b, &r), -EIO);
		printf("PASS real ROM cut at 0xf000: refused (-EIO)\n");
	}
	free(rom);
}

int
main(void)
{
	printf("== SIMULATION: VBIOS fetch through a fake ACPI _ROM ==\n");
	test_synthetic();
	test_fallback();
	test_real();
	if (g_fail) {
		printf("%d failure(s)\n", g_fail);
		return 1;
	}
	printf("PASS test_vbios_fetch\n");
	return 0;
}
