// SPDX-License-Identifier: GPL-2.0-only
// pascal/tests/test_acr.c — unit tests for the ACR blob parser
// (pascal/core/src/acr.c), run against the REAL gp106/gr firmware blobs and
// the ACR HS ucode blobs committed under research/firmware/gp106/acr/ (see
// that directory's README.md: linux-firmware ships no nvidia/gp106/acr/, so
// the committed GP102-generation ACR blobs stand in for them; the wire
// format is the r352 format parsed here).
//
// Coverage:
//   1. real-blob structural invariants + printed parsed values
//   2. synthetic golden vectors, hand-packed byte-for-byte from the nouveau
//      struct definitions (wpr_header_v1, lsb_header_v1, wpr layout)
//   3. rejection of corrupted/truncated headers
//   4. truncation/mutation fuzz: every prefix of every blob plus seeded
//      random byte flips must return cleanly, never crash
//
// For a memory-checked run (recommended for the fuzz section):
//   cd pascal
//   cc -std=c11 -Wall -Wextra -O1 -g -fsanitize=address,undefined
//     -Icore/include -o build/test_acr_asan tests/test_acr.c core/src/*.c
//   ./build/test_acr_asan

#include <pascal/acr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                        \
	do {                                                               \
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
		if (va_ != vb_) {                                          \
			printf("FAIL %s:%d: %s == %s (0x%llx != 0x%llx)\n",\
			       __func__, __LINE__, #a, #b, va_, vb_);      \
			g_failures++;                                      \
		}                                                          \
	} while (0)

// --- tiny file loader -------------------------------------------------------

static uint8_t *
load_blob(const char *dir, const char *rel, size_t *size_out)
{
	char path[512];
	uint8_t *buf;
	long len;
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, rel);
	f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len < 0) {
		fclose(f);
		return NULL;
	}
	buf = malloc((size_t)len ? (size_t)len : 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
		fclose(f);
		free(buf);
		return NULL;
	}
	fclose(f);
	*size_out = (size_t)len;
	return buf;
}

// The firmware lives in research/firmware/gp106 relative to the repo root;
// the test binary runs from pascal/ under `make test`.
static const char *
fw_dir(void)
{
	static const char *candidates[] = {
		"../research/firmware/gp106",
		"../../research/firmware/gp106",
		"research/firmware/gp106",
	};
	size_t i;

	for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
		char path[512];
		FILE *f;

		snprintf(path, sizeof(path), "%s/README.md", candidates[i]);
		f = fopen(path, "rb");
		if (f) {
			fclose(f);
			return candidates[i];
		}
	}
	return NULL;
}

// --- deterministic PRNG for the mutation fuzz (xorshift32, fixed seed so
// failures are reproducible) -----------------------------------------------

// Test-local little-endian peek (mirrors the byte-wise reads in acr.c).
static uint32_t
rd32le(const uint8_t *b, size_t off)
{
	return (uint32_t)b[off] | ((uint32_t)b[off + 1] << 8) |
	       ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 3] << 24);
}

static uint32_t
rng_next(uint32_t *s)
{
	uint32_t x = *s;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*s = x;
	return x;
}

// --- 1. real blobs ----------------------------------------------------------

// Golden values below were extracted from the committed blobs with a
// one-off python read of the wire structs; the nouveau source mapping is
// cited field-by-field in acr.h. (gp102/acr/ucode_load.bin et al.)
static void
test_real_ucode_load(const char *dir)
{
	const char *name = "acr/ucode_load.bin";
	struct pg106_acr_hs_ucode u;
	struct pg106_acr_desc_v1_summary desc;
	size_t size;
	uint8_t *blob = load_blob(dir, name, &size);
	int ret;

	CHECK(blob);
	if (!blob)
		return;
	printf("ucode_load.bin: %zu bytes\n", size);
	CHECK_EQ(size, 17152); // file size

	ret = pg106_acr_ucode_parse_hs(blob, size, &u);
	CHECK_EQ(ret, 0);
	if (ret) {
		free(blob);
		return;
	}

	// Container header (nvfw_bin_hdr).
	printf("  bin_magic=%#x bin_ver=%u bin_size=%#x hdr=%#x data=%#x/%#x\n",
	       u.hdr.bin_magic, u.hdr.bin_ver, u.hdr.bin_size,
	       u.hdr.header_offset, u.hdr.data_offset, u.hdr.data_size);
	CHECK_EQ(u.hdr.bin_magic, PG106_FLCN_BIN_MAGIC_STD);
	CHECK_EQ(u.hdr.bin_ver, 1);
	CHECK_EQ(u.hdr.bin_size, 0x4300);
	CHECK_EQ(u.hdr.header_offset, 0x100);
	CHECK_EQ(u.hdr.data_offset, 0x200);
	CHECK_EQ(u.hdr.data_size, 0x4100);
	CHECK(u.img_size > 0 && u.img_size == u.hdr.data_size);
	CHECK(u.img >= blob && u.img + u.img_size <= blob + size);

	// HS descriptor (nvfw_hs_header).
	printf("  sig_dbg=%#x/%#x sig_prod=%#x/%#x patch_loc=%#x patch_sig=%#x lhdr=%#x/%#x\n",
	       u.hs.sig_dbg_offset, u.hs.sig_dbg_size, u.hs.sig_prod_offset,
	       u.hs.sig_prod_size, u.hs.patch_loc, u.hs.patch_sig,
	       u.hs.hdr_offset, u.hs.hdr_size);
	CHECK_EQ(u.hs.sig_dbg_offset, 0x120);
	CHECK_EQ(u.hs.sig_dbg_size, 0x10);
	CHECK_EQ(u.hs.sig_prod_offset, 0x130);
	CHECK_EQ(u.hs.sig_prod_size, 0x10);
	CHECK_EQ(u.hs.patch_loc, 0x140);
	CHECK_EQ(u.hs.patch_sig, 0x144);
	CHECK_EQ(u.hs.hdr_offset, 0x148);
	CHECK_EQ(u.hs.hdr_size, 0x24);

	// Indirect patch values read through patch_loc/patch_sig (fw.c:245-246).
	printf("  sig_base_img=%#x sig_size=%#x sig_prod_off=%#x sig_dbg_off=%#x dmem_sign=%#x\n",
	       u.sig_base_img, u.sig_size, u.sig_prod_off, u.sig_dbg_off,
	       u.dmem_sign);
	CHECK_EQ(u.sig_base_img, 0x3820); // *(u32*)(blob+0x140)
	CHECK_EQ(u.sig_size, 0x10);
	CHECK_EQ(u.sig_prod_off, 0x130); // 0x130 + *(u32*)(blob+0x144)=0
	CHECK_EQ(u.sig_dbg_off, 0x120);
	CHECK_EQ(u.dmem_sign, 0xf20); // 0x3820 - 0x2900 (fw.c:277)
	// The signature pool bytes exist and the first production word is
	// stable in the shipped blob.
	CHECK_EQ((unsigned)rd32le(blob, u.sig_prod_off), 0x3d75f53eu);

	// Load header (nvfw_hs_load_header) -> falcon load parameters.
	printf("  nmem=%#x/%#x imem=%#x(->%#x)/%#x dmem=%#x/%#x apps=%u\n",
	       u.nmem_base, u.nmem_size, u.imem_base_img, u.imem_base,
	       u.imem_size, u.dmem_base_img, u.dmem_size, u.load.num_apps);
	CHECK_EQ(u.load.non_sec_code_off, 0x0);
	CHECK_EQ(u.load.non_sec_code_size, 0x100);
	CHECK_EQ(u.load.num_apps, 1);
	CHECK_EQ(u.imem_base_img, 0x100);
	CHECK_EQ(u.imem_base, 0x100); // ALIGN(0x100, 0x100)
	CHECK_EQ(u.imem_size, 0x2800);
	CHECK_EQ(u.dmem_base_img, 0x2900);
	CHECK_EQ(u.dmem_size, 0x1000);
	// Structural invariants: secure code inside image, data segment
	// inside image, patch site inside data segment.
	CHECK(u.imem_base + u.imem_size <= u.img_size);
	CHECK(u.dmem_base_img + u.dmem_size <= u.img_size);
	CHECK(u.sig_base_img >= u.dmem_base_img &&
	      u.sig_base_img < u.dmem_base_img + u.dmem_size);

	// The ACR parameters block at img + dmem_base_img (gp102.c:199) must
	// parse; in the shipped (pre-setup) blob the region fields are zero.
	ret = pg106_acr_desc_v1_parse(u.img + u.dmem_base_img, u.dmem_size,
				      &desc);
	CHECK_EQ(ret, 0);
	if (!ret) {
		printf("  acr_desc: wpr_region_id=%u no_regions=%u blob=%#llx@%#llx\n",
		       desc.wpr_region_id, desc.no_regions,
		       (unsigned long long)desc.ucode_blob_size,
		       (unsigned long long)desc.ucode_blob_base);
		CHECK_EQ(desc.wpr_region_id, 0);
		CHECK_EQ(desc.no_regions, 0);
	}
	free(blob);
}

static void
test_real_ucode_unload(const char *dir)
{
	struct pg106_acr_hs_ucode u;
	size_t size;
	uint8_t *blob = load_blob(dir, "acr/ucode_unload.bin", &size);
	int ret;

	CHECK(blob);
	if (!blob)
		return;
	printf("ucode_unload.bin: %zu bytes\n", size);
	CHECK_EQ(size, 3328);
	ret = pg106_acr_ucode_parse_hs(blob, size, &u);
	CHECK_EQ(ret, 0);
	if (ret) {
		free(blob);
		return;
	}
	printf("  sig_base_img=%#x imem=%#x/%#x dmem=%#x/%#x dmem_sign=%#x\n",
	       u.sig_base_img, u.imem_base, u.imem_size, u.dmem_base_img,
	       u.dmem_size, u.dmem_sign);
	CHECK_EQ(u.hdr.bin_size, 0xd00);
	CHECK_EQ(u.hdr.data_offset, 0x200);
	CHECK_EQ(u.hdr.data_size, 0xb00);
	CHECK_EQ(u.sig_base_img, 0xa00);
	CHECK_EQ(u.imem_base_img, 0x100);
	CHECK_EQ(u.imem_size, 0x900);
	CHECK_EQ(u.dmem_base_img, 0xa00);
	CHECK_EQ(u.dmem_size, 0x100);
	CHECK_EQ(u.dmem_sign, 0x0);
	free(blob);
}

static void
test_real_bl(const char *dir, const char *rel, size_t expect_size,
	     uint32_t expect_start_tag)
{
	struct pg106_acr_bl_ucode b;
	size_t size;
	uint8_t *blob = load_blob(dir, rel, &size);
	int ret;

	CHECK(blob);
	if (!blob)
		return;
	printf("%s: %zu bytes\n", rel, size);
	CHECK_EQ(size, expect_size);
	ret = pg106_acr_ucode_parse_bl(blob, size, &b);
	CHECK_EQ(ret, 0);
	if (ret) {
		free(blob);
		return;
	}
	// Golden values from the committed gp102/acr blobs; see acr.h for the
	// nouveau mapping (nvfw_bl_desc, fw.c:289-292).
	printf("  boot_addr=%#llx boot_size=%#x dmem_load_off=%#x code=%#zx/%#zx\n",
	       (unsigned long long)b.boot_addr, b.boot_size, b.dmem_load_off,
	       (size_t)(b.boot_code - blob), (size_t)b.desc.code_size);
	CHECK_EQ(b.hdr.bin_magic, PG106_FLCN_BIN_MAGIC_STD);
	CHECK_EQ(b.hdr.bin_size, 0x500);
	CHECK_EQ(b.hdr.header_offset, 0x100);
	CHECK_EQ(b.hdr.data_offset, 0x200);
	CHECK_EQ(b.hdr.data_size, 0x300);
	CHECK_EQ(b.desc.start_tag, expect_start_tag);
	CHECK_EQ(b.desc.dmem_load_off, 0x0);
	CHECK_EQ(b.desc.code_off, 0x0);
	CHECK_EQ(b.desc.code_size, 0x200);
	CHECK_EQ(b.desc.data_off, 0x200);
	CHECK_EQ(b.desc.data_size, 0x100);
	CHECK_EQ(b.boot_addr, (uint64_t)expect_start_tag << 8);
	CHECK_EQ(b.boot_size, 0x200);
	CHECK(b.boot_code >= blob &&
	      b.boot_code + b.boot_size <= blob + size);
	free(blob);
}

static void
test_real_ls_sigs(const char *dir)
{
	struct pg106_acr_lsf_signature_v1 sig;
	size_t size, i;
	uint8_t *blob;
	int ret;

	// FECS: nvidia/gp106/gr/fecs_sig.bin — the REAL per-chip signature
	// nouveau loads (lsfw.c nvkm_acr_lsfw_load_sig_image_desc_v1),
	// exactly sizeof(lsf_signature_v1) as gp102.c:66 requires.
	blob = load_blob(dir, "gr/fecs_sig.bin", &size);
	CHECK(blob);
	if (!blob)
		return;
	printf("gr/fecs_sig.bin: %zu bytes\n", size);
	CHECK_EQ(size, 192);
	ret = pg106_acr_lsf_sig_parse(blob, size, &sig);
	CHECK_EQ(ret, 0);
	if (!ret) {
		printf("  falcon_id=%u version=%u prd=%u dbg=%u depmap_count=%u\n",
		       sig.falcon_id, sig.version, sig.b_prd_present,
		       sig.b_dbg_present, sig.depmap_count);
		CHECK_EQ(sig.falcon_id, PG106_ACR_LSF_FECS);
		CHECK_EQ(sig.version, 2);
		CHECK_EQ(sig.b_prd_present, 1);
		CHECK_EQ(sig.b_dbg_present, 1);
		CHECK_EQ(sig.supports_versioning, 1);
		CHECK_EQ(sig.depmap_count, 0);
		for (i = 0; i < sizeof(sig.prd_keys); i++)
			if (sig.prd_keys[0][i] != 0)
				break;
		CHECK(i < sizeof(sig.prd_keys)); // keys are not all-zero
	}
	free(blob);

	// GPCCS.
	blob = load_blob(dir, "gr/gpccs_sig.bin", &size);
	CHECK(blob);
	if (!blob)
		return;
	printf("gr/gpccs_sig.bin: %zu bytes\n", size);
	CHECK_EQ(size, 192);
	ret = pg106_acr_lsf_sig_parse(blob, size, &sig);
	CHECK_EQ(ret, 0);
	if (!ret) {
		printf("  falcon_id=%u version=%u\n", sig.falcon_id,
		       sig.version);
		CHECK_EQ(sig.falcon_id, PG106_ACR_LSF_GPCCS);
		CHECK_EQ(sig.version, 2);
	}
	free(blob);
}

// --- 2. synthetic golden vectors --------------------------------------------

// The synthetic FECS LS firmware description used below. It is hand-built
// (not from a real fecs_desc.bin) but uses the r352 field relationships of
// lsfw.c nvkm_acr_lsfw_from_desc:
//   ucode_size = ALIGN(app_resident_data_offset, 256) + bootloader_size
//              = ALIGN(0x2900, 256) + 0x100 = 0x2a00
//   data_size  = app_size + bootloader_size - ucode_size
//              = 0x2b00 + 0x100 - 0x2a00 = 0x100
// bld_size 0x4c = sizeof(flcn_bl_dmem_desc_v1), the FECS/GPCCS bootloader
// argument block (gm200.c:241 writes it via nvkm_falcon_pio_wr).
static void
make_fecs_lsfw(struct pg106_acr_lsfw *l)
{
	*l = (struct pg106_acr_lsfw){
		.id = PG106_ACR_LSF_FECS,
		.secure_bootloader = false,
		.bootloader_size = 0x100,
		.bootloader_imem_offset = 0x0,
		.app_size = 0x2b00,
		.app_start_offset = 0x100,
		.app_imem_entry = 0x100,
		.app_resident_code_offset = 0x0,
		.app_resident_code_size = 0x2800,
		.app_resident_data_offset = 0x2900,
		.app_resident_data_size = 0x100,
		.app_imem_offset = 0x0,
		.app_dmem_offset = 0x0,
		.ucode_size = 0x2a00,
		.data_size = 0x100,
		.bld_size = 0x4c,
		.flags = 0x0,
		.img_size = 0x2b00,
	};
}

// Exposure of a one-entry array for the zero-entry call in the layout test.
static struct pg106_acr_lsfw *
fecs_array(void)
{
	static struct pg106_acr_lsfw one[1];

	make_fecs_lsfw(&one[0]);
	return one;
}

// gp102_acr_wpr_layout (gp102.c:134-160) worked by hand for one FECS:
//   11 * 24 = 264 -> ALIGN 256 = 0x200; + 0x100 shared = 0x300
//   lsb  = 0x300,  size 240 -> 0x3f0
//   img  = ALIGN(0x3f0, 4096) = 0x1000, + 0x2b00 -> 0x3b00
//   bld  = ALIGN(0x3b00, 256) = 0x3b00, bl_data_size = ALIGN(0x4c,256)=0x100
//   total = 0x3c00
static void
test_wpr_layout_golden(void)
{
	struct pg106_acr_lsfw fecs, gpccs;
	uint32_t wpr = 0;
	int ret;

	make_fecs_lsfw(&fecs);
	ret = pg106_acr_wpr_layout(&fecs, 1, &wpr);
	CHECK_EQ(ret, 0);
	CHECK_EQ(wpr, 0x3c00);
	CHECK_EQ(fecs.offset.lsb, 0x300);
	CHECK_EQ(fecs.offset.img, 0x1000);
	CHECK_EQ(fecs.offset.bld, 0x3b00);
	CHECK_EQ(fecs.bl_data_size, 0x100);

	// Two LSFs (FECS then GPCCS, as nouveau's acr list orders them):
	// GPCCS continues after FECS: bld end 0x3c00 -> lsb 0x3c00 (aligned),
	// img 0x4000 (+0x2b00 = 0x6b00), bld 0x6b00 (+0x100 = 0x6c00).
	make_fecs_lsfw(&gpccs);
	gpccs.id = PG106_ACR_LSF_GPCCS;
	ret = pg106_acr_wpr_layout(&fecs, 1, &wpr); // reset FECS offsets
	CHECK_EQ(ret, 0);
	{
		struct pg106_acr_lsfw two[2] = { fecs, gpccs };
		ret = pg106_acr_wpr_layout(two, 2, &wpr);
		CHECK_EQ(ret, 0);
		CHECK_EQ(wpr, 0x6c00);
		CHECK_EQ(two[0].offset.lsb, 0x300);
		CHECK_EQ(two[1].offset.lsb, 0x3c00);
		CHECK_EQ(two[1].offset.img, 0x4000);
		CHECK_EQ(two[1].offset.bld, 0x6b00);
	}

	// Zero entries is legal: the total is just the 11 WPR header slots
	// (264 -> ALIGN 256 = 0x200) plus the 0x100 shared sub-WPR area.
	ret = pg106_acr_wpr_layout(fecs_array(), 0, &wpr);
	CHECK_EQ(ret, 0);
	CHECK_EQ(wpr, 0x300);

	// Count is capped at MAX_LSF (11 WPR header slots, gp102.c:139).
	make_fecs_lsfw(&fecs);
	{
		struct pg106_acr_lsfw many[PG106_ACR_LSF_MAX + 1];
		size_t i;
		for (i = 0; i < PG106_ACR_LSF_MAX + 1; i++)
			many[i] = fecs;
		CHECK(pg106_acr_wpr_layout(many, PG106_ACR_LSF_MAX + 1,
					   &wpr) < 0);
	}
}

// Golden bytes for the FECS WPR header, packed by hand from struct
// wpr_header_v1 (include/nvfw/acr.h:22-38) and the gp102.c:86-93 builder:
//   falcon_id=2 (FECS), lsb_offset=0x300, bootstrap_owner=7 (SEC2),
//   lazy_bootstrap=1 (rtos is GPCCS=3), bin_version=2 (sig version),
//   status=1 (STATUS_COPY).
static const uint8_t wpr_header_golden[24] = {
	0x02, 0x00, 0x00, 0x00,	// falcon_id       = 2
	0x00, 0x03, 0x00, 0x00,	// lsb_offset      = 0x300
	0x07, 0x00, 0x00, 0x00,	// bootstrap_owner = NVKM_ACR_LSF_SEC2
	0x01, 0x00, 0x00, 0x00,	// lazy_bootstrap  = 1
	0x02, 0x00, 0x00, 0x00,	// bin_version     = 2
	0x01, 0x00, 0x00, 0x00,	// status          = STATUS_COPY
};

static void
test_wpr_header_golden(const char *dir)
{
	struct pg106_acr_lsfw fecs;
	struct pg106_acr_lsf_signature_v1 sig;
	struct pg106_acr_wpr_header_v1 hdr;
	size_t size;
	uint8_t *blob = load_blob(dir, "gr/fecs_sig.bin", &size);
	int ret;

	CHECK(blob);
	if (!blob)
		return;
	CHECK_EQ(pg106_acr_lsf_sig_parse(blob, size, &sig), 0);

	make_fecs_lsfw(&fecs);
	CHECK_EQ(pg106_acr_wpr_layout(&fecs, 1, &(uint32_t){0}), 0);

	// rtos = GPCCS (3): FECS is lazy-bootstrapped (gp102.c:90).
	ret = pg106_acr_wpr_header_build(&fecs, PG106_ACR_LSF_SEC2,
					 PG106_ACR_LSF_GPCCS, &sig, &hdr);
	CHECK_EQ(ret, 0);
	CHECK(memcmp(&hdr, wpr_header_golden, sizeof(hdr)) == 0);
	CHECK_EQ(hdr.falcon_id, PG106_ACR_LSF_FECS);
	CHECK_EQ(hdr.lsb_offset, 0x300);
	CHECK_EQ(hdr.bootstrap_owner, PG106_ACR_LSF_SEC2);
	CHECK_EQ(hdr.lazy_bootstrap, 1);
	CHECK_EQ(hdr.bin_version, 2);
	CHECK_EQ(hdr.status, PG106_ACR_WPR_STATUS_COPY);

	// rtos = FECS itself: not lazy (gp102.c:90 lsfw->id == rtos->id).
	CHECK_EQ(pg106_acr_wpr_header_build(&fecs, PG106_ACR_LSF_SEC2,
					    PG106_ACR_LSF_FECS, &sig, &hdr),
		 0);
	CHECK_EQ(hdr.lazy_bootstrap, 0);
	free(blob);
}

// Golden LSB tail, hand-packed from struct lsb_header_tail
// (include/nvfw/acr.h:80-93) and gm200_acr_wpr_build_lsb_tail (gm200.c:94-111)
// for the synthetic FECS above: ucode_off/img=0x1000, ucode_size=0x2a00,
// data_size=0x100, bl_code_size=0x100, bl_imem_off=0, bl_data_off/bld=0x3b00,
// bl_data_size=0x100, app_code_off=0x100+0=0x100, app_code_size=0x2800,
// app_data_off=0x100+0x2900=0x2a00, app_data_size=0x100, flags=0.
static const uint8_t lsb_tail_golden[48] = {
	0x00, 0x10, 0x00, 0x00,	// ucode_off      = 0x1000
	0x00, 0x2a, 0x00, 0x00,	// ucode_size     = 0x2a00
	0x00, 0x01, 0x00, 0x00,	// data_size      = 0x100
	0x00, 0x01, 0x00, 0x00,	// bl_code_size   = 0x100
	0x00, 0x00, 0x00, 0x00,	// bl_imem_off    = 0
	0x00, 0x3b, 0x00, 0x00,	// bl_data_off    = 0x3b00
	0x00, 0x01, 0x00, 0x00,	// bl_data_size   = 0x100
	0x00, 0x01, 0x00, 0x00,	// app_code_off   = 0x100
	0x00, 0x28, 0x00, 0x00,	// app_code_size  = 0x2800
	0x00, 0x2a, 0x00, 0x00,	// app_data_off   = 0x2a00
	0x00, 0x01, 0x00, 0x00,	// app_data_size  = 0x100
	0x00, 0x00, 0x00, 0x00,	// flags          = 0
};

static void
test_lsb_header_golden(const char *dir)
{
	struct pg106_acr_lsfw fecs;
	struct pg106_acr_lsf_signature_v1 sig;
	struct pg106_acr_lsb_header_v1 lsb;
	size_t size;
	uint8_t *blob = load_blob(dir, "gr/fecs_sig.bin", &size);
	int ret;

	CHECK(blob);
	if (!blob)
		return;
	CHECK_EQ(pg106_acr_lsf_sig_parse(blob, size, &sig), 0);

	make_fecs_lsfw(&fecs);
	CHECK_EQ(pg106_acr_wpr_layout(&fecs, 1, &(uint32_t){0}), 0);

	ret = pg106_acr_lsb_header_build(&fecs, &sig, &lsb);
	CHECK_EQ(ret, 0);
	if (ret) {
		free(blob);
		return;
	}

	// Signature slot: gp102.c:69 memcpy(&hdr.signature, lsfw->sig->data,
	// 192) — must equal the raw fecs_sig.bin bytes exactly.
	CHECK(memcmp(&lsb.signature, blob, PG106_ACR_LSF_SIG_V1_SIZE) == 0);
	// Tail: byte-for-byte against the hand-packed golden above.
	CHECK(memcmp(&lsb.tail, lsb_tail_golden, sizeof(lsb_tail_golden)) == 0);
	// And the field-level view of the same facts (gm200.c:97-110).
	CHECK_EQ(lsb.tail.ucode_off, fecs.offset.img);
	CHECK_EQ(lsb.tail.ucode_size, 0x2a00);
	CHECK_EQ(lsb.tail.data_size, 0x100);
	CHECK_EQ(lsb.tail.bl_code_size, 0x100);
	CHECK_EQ(lsb.tail.bl_imem_off, 0x0);
	CHECK_EQ(lsb.tail.bl_data_off, fecs.offset.bld);
	CHECK_EQ(lsb.tail.bl_data_size, 0x100);
	CHECK_EQ(lsb.tail.app_code_off, 0x100);
	CHECK_EQ(lsb.tail.app_code_size, 0x2800);
	CHECK_EQ(lsb.tail.app_data_off, 0x2a00);
	CHECK_EQ(lsb.tail.app_data_size, 0x100);
	CHECK_EQ(lsb.tail.flags, 0);

	// Mismatched signature is rejected (stricter than nouveau: gp102.c
	// copies whatever sig file was loaded; hardware would fail UI checks).
	{
		struct pg106_acr_lsf_signature_v1 wrong = sig;

		wrong.falcon_id = PG106_ACR_LSF_GPCCS;
		CHECK(pg106_acr_lsb_header_build(&fecs, &wrong, &lsb) < 0);
	}
	// No aliasing surprises: struct size is the wire size (192 + 48).
	CHECK(sizeof(lsb) == 240);
	free(blob);
}

// Build a WPR region image (2 LSF headers + terminator) and parse it back
// through pg106_acr_wpr_header_next — the inverse of gp102_acr_wpr_build /
// gp102_acr_wpr_parse (gp102.c:76-116, 162-176).
static void
test_wpr_parse_roundtrip(void)
{
	struct pg106_acr_wpr_header_v1 hdr;
	uint8_t buf[3 * 24];
	bool term;
	size_t pos;
	int ret;

	memset(buf, 0, sizeof(buf));

	// FECS entry.
	buf[0] = 2;                    // falcon_id
	buf[4] = 0x00; buf[5] = 0x03;  // lsb_offset = 0x300
	buf[8] = 7;                    // bootstrap_owner = SEC2
	buf[12] = 1;                   // lazy_bootstrap
	buf[16] = 2;                   // bin_version
	buf[20] = 1;                   // status = COPY
	// GPCCS entry.
	buf[24] = 3;
	buf[28] = 0x00; buf[29] = 0x39; // lsb_offset = 0x3900
	buf[32] = 7;
	buf[36] = 1;
	buf[40] = 2;
	buf[44] = 1;
	// Terminator: WPR_HEADER_V1_FALCON_ID_INVALID (gp102.c:114).
	buf[48] = 0xff; buf[49] = 0xff; buf[50] = 0xff; buf[51] = 0xff;

	pos = 0;
	ret = pg106_acr_wpr_header_next(buf, sizeof(buf), &pos, &hdr, &term);
	CHECK_EQ(ret, 0);
	CHECK(!term);
	CHECK_EQ(hdr.falcon_id, 2);
	CHECK_EQ(hdr.lsb_offset, 0x300);
	CHECK_EQ(pos, 24);

	ret = pg106_acr_wpr_header_next(buf, sizeof(buf), &pos, &hdr, &term);
	CHECK_EQ(ret, 0);
	CHECK(!term);
	CHECK_EQ(hdr.falcon_id, 3);
	CHECK_EQ(hdr.lsb_offset, 0x3900);
	CHECK_EQ(pos, 48);

	ret = pg106_acr_wpr_header_next(buf, sizeof(buf), &pos, &hdr, &term);
	CHECK_EQ(ret, 0);
	CHECK(term); // caller must stop here (gp102.c:168 loop condition)
	CHECK_EQ(hdr.falcon_id, PG106_ACR_WPR_FALCON_ID_INVALID);
	CHECK_EQ(pos, 72);

	// Truncated buffer: a partial header is rejected, not over-read.
	pos = 48;
	CHECK(pg106_acr_wpr_header_next(buf, 71, &pos, &hdr, &term) < 0);
}

// flcn_bl_dmem_desc_v1 golden, hand-packed from include/nvfw/flcn.h:62-74
// for the parsed gp102 ucode_load.bin mapped at vma 0x1_00000000:
// reserved/signature zero, ctx_dma=1 (FALCON_DMAIDX_VIRT), code_dma_base
// 0x100000000, non_sec 0x0/0x100, sec 0x100/0x2800, entry 0, data_dma_base
// 0x100002900, data_size 0x1000.
static const uint8_t bld_golden[76] = {
	0x00, 0x00, 0x00, 0x00,	0x00, 0x00, 0x00, 0x00,	// reserved[4]
	0x00, 0x00, 0x00, 0x00,	0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,	0x00, 0x00, 0x00, 0x00,	// signature[4]
	0x00, 0x00, 0x00, 0x00,	0x00, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00,			      	// ctx_dma = 1
	0x00, 0x00, 0x00, 0x00,	0x01, 0x00, 0x00, 0x00,	// code_dma_base
	0x00, 0x00, 0x00, 0x00,			      	// non_sec_code_off
	0x00, 0x01, 0x00, 0x00,			      	// non_sec_code_size
	0x00, 0x01, 0x00, 0x00,			      	// sec_code_off
	0x00, 0x28, 0x00, 0x00,			      	// sec_code_size
	0x00, 0x00, 0x00, 0x00,			      	// code_entry_point
	0x00, 0x29, 0x00, 0x00,	0x01, 0x00, 0x00, 0x00,	// data_dma_base
	0x00, 0x10, 0x00, 0x00,			      	// data_size
};

static void
test_bld_build(const char *dir)
{
	struct pg106_acr_hs_ucode u;
	uint8_t bld[PG106_FLCN_BL_DMEM_DESC_V1_SIZE];
	size_t size;
	uint8_t *blob = load_blob(dir, "acr/ucode_load.bin", &size);
	int ret;

	CHECK(blob);
	if (!blob)
		return;
	CHECK_EQ(pg106_acr_ucode_parse_hs(blob, size, &u), 0);

	// gm200_acr_hsfw_load_bld (gm200.c:227-237) with vma->addr = 0x1_00000000.
	ret = pg106_acr_hsfw_bld_build(&u, 0x100000000ull, bld);
	CHECK_EQ(ret, 0);
	CHECK(memcmp(bld, bld_golden, sizeof(bld_golden)) == 0);
	free(blob);
}

// --- 3. rejection of corrupted headers --------------------------------------

static void
test_rejection(void)
{
	struct pg106_acr_bl_ucode bl;
	struct pg106_acr_hs_ucode hs;
	uint8_t buf[64];
	int ret;

	// NULL / tiny inputs.
	CHECK(pg106_acr_ucode_parse_bl(NULL, 0, &bl) < 0);
	CHECK(pg106_acr_ucode_parse_bl(buf, 0, &bl) < 0);
	CHECK(pg106_acr_ucode_parse_bl(buf, 23, &bl) < 0);
	CHECK(pg106_acr_ucode_parse_hs(buf, 23, &hs) < 0);
	CHECK(pg106_acr_lsf_sig_parse(buf, 191, NULL) < 0);

	memset(buf, 0, sizeof(buf));

	// A structurally valid header whose header_offset points past the
	// buffer (bin_size field of a fabricated container).
	memcpy(buf, "\xde\x10\x00\x00\x01\x00\x00\x00\x40\x00\x00\x00"
		    "\x00\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", 24);
	ret = pg106_acr_ucode_parse_bl(buf, 24, &bl);
	CHECK(ret < 0);

	// data_size extending past the buffer.
	memcpy(buf, "\xde\x10\x00\x00\x01\x00\x00\x00\x40\x00\x00\x00"
		    "\x18\x00\x00\x00\xff\xff\x00\x00\x00\x00\x00\x00", 24);
	ret = pg106_acr_ucode_parse_bl(buf, 24, &bl);
	CHECK(ret < 0);

	// Wrong magic.
	memcpy(buf, "\xde\x10\x00\x00\x01\x00\x00\x00\x40\x00\x00\x00"
		    "\x18\x00\x00\x00\x18\x00\x00\x00\x08\x00\x00\x00", 24);
	buf[0] = 0xdf;
	ret = pg106_acr_ucode_parse_bl(buf, 64, &bl);
	CHECK(ret < 0);
	buf[0] = 0xde;
	ret = pg106_acr_ucode_parse_bl(buf, 64, &bl);
	CHECK_EQ(ret, 0); // the same header, uncorrupted, parses

	// desc parse rejects short dmem and NULL out.
	CHECK(pg106_acr_desc_v1_parse(buf, 8, NULL) < 0);
	{
		static uint8_t dmem[PG106_FLCN_ACR_DESC_V1_SIZE];
		struct pg106_acr_desc_v1_summary s;
		memset(dmem, 0, sizeof(dmem));
		CHECK_EQ(pg106_acr_desc_v1_parse(dmem, sizeof(dmem), &s), 0);
		CHECK(pg106_acr_desc_v1_parse(dmem, sizeof(dmem) - 1, &s) < 0);
		dmem[0x200 + 16 + 12] = 3; // regions.no_regions = 3 (> 2)
		CHECK(pg106_acr_desc_v1_parse(dmem, sizeof(dmem), &s) < 0);
	}
}


// --- 3b. flcn_acr_desc_v1 field offsets (include/nvfw/acr.h:208-234) --------

// ucode_blob_size sits at 600 (0x258) right after region_props[1];
// ucode_blob_base is __aligned(8) so it lands at 608 with the 4-byte pad
// at 604. Distinct sentinels in all three slots catch an off-by-pad read.
static void
test_desc_v1_offsets(void)
{
	static uint8_t dmem[PG106_FLCN_ACR_DESC_V1_SIZE];
	struct pg106_acr_desc_v1_summary s;

	memset(dmem, 0, sizeof(dmem));
	dmem[600] = 0x11; dmem[601] = 0x22; dmem[602] = 0x33; dmem[603] = 0x44;
	dmem[604] = 0xee; dmem[605] = 0xee; dmem[606] = 0xee; dmem[607] = 0xee;
	dmem[608] = 0x01; dmem[612] = 0x02; // base = 0x0000000200000001
	dmem[616] = 0x05;                   // vpr_desc.vpr_enabled
	dmem[628] = 0x07;                   // vpr_desc.hdcp_policies
	CHECK_EQ(pg106_acr_desc_v1_parse(dmem, sizeof(dmem), &s), 0);
	CHECK_EQ(s.ucode_blob_size, 0x44332211u);
	CHECK_EQ(s.ucode_blob_base, 0x0000000200000001ull);
	CHECK_EQ(s.vpr.vpr_enabled, 5);
	CHECK_EQ(s.vpr.hdcp_policies, 7);
	printf("desc_v1: ucode_blob_size @600, base @608 (pad @604 ignored)\n");
}

// --- 3c. HS section bounds + debug-signature length --------------------------

static void
put32(uint8_t *b, size_t off, uint32_t v)
{
	b[off] = (uint8_t)v;
	b[off + 1] = (uint8_t)(v >> 8);
	b[off + 2] = (uint8_t)(v >> 16);
	b[off + 3] = (uint8_t)(v >> 24);
}

static void
test_hs_section_bounds(const char *dir)
{
	struct pg106_acr_hs_ucode hs;
	size_t size;
	uint8_t *orig = load_blob(dir, "acr/ucode_load.bin", &size);
	uint8_t *b;
	uint32_t hdr_off, lhdr;

	CHECK(orig);
	if (!orig)
		return;
	b = malloc(size);
	CHECK_EQ(pg106_acr_ucode_parse_hs(orig, size, &hs), 0);
	hdr_off = rd32le(orig, 12); // nvfw_bin_hdr.header_offset
	lhdr = hs.hs.hdr_offset;   // nvfw_hs_load_header

	// imem (apps[0] + apps[num_apps]) past the image end.
	memcpy(b, orig, size);
	put32(b, lhdr + 20 + 4 * hs.load.num_apps, (uint32_t)hs.img_size);
	CHECK(pg106_acr_ucode_parse_hs(b, size, &hs) < 0);
	// imem_base_img (apps[0]) at the image end.
	memcpy(b, orig, size);
	put32(b, lhdr + 20, (uint32_t)hs.img_size);
	CHECK(pg106_acr_ucode_parse_hs(b, size, &hs) < 0);
	// nmem (non_sec_code_size from image offset 0) past the end.
	memcpy(b, orig, size);
	put32(b, lhdr + 4, (uint32_t)hs.img_size + 0x100);
	CHECK(pg106_acr_ucode_parse_hs(b, size, &hs) < 0);
	// Image truncated (bin_hdr.data_size) below imem end but still
	// holding the data segment and patch site is impossible for this
	// layout, so shrink to just before the secure section's end.
	memcpy(b, orig, size);
	put32(b, 20, hs.imem_base_img + hs.imem_size - 4);
	CHECK(pg106_acr_ucode_parse_hs(b, size, &hs) < 0);

	// Debug signature: nouveau copies sig_PROD_size bytes from the
	// debug offset (fw.c:190-192). A debug slot that fits sig_dbg_size
	// but not sig_prod_size must be rejected; one that fits both passes.
	CHECK_EQ(pg106_acr_ucode_parse_hs(orig, size, &hs), 0);
	memcpy(b, orig, size);
	put32(b, hdr_off + 12, 0x20);			  // sig_prod_size
	put32(b, hdr_off + 0, (uint32_t)size - 0x10);	  // sig_dbg_offset
	CHECK(pg106_acr_ucode_parse_hs(b, size, &hs) < 0);
	put32(b, hdr_off + 0, (uint32_t)size - 0x20);
	CHECK_EQ(pg106_acr_ucode_parse_hs(b, size, &hs), 0);
	CHECK_EQ(hs.sig_size, 0x20);
	printf("hs bounds: imem/nmem/data-size truncations and short debug "
	       "signature slot rejected\n");
	free(b);
	free(orig);
}

// --- 4. truncation / mutation fuzz ------------------------------------------

// Every prefix of the blob through both parsers plus the sig parser must
// return cleanly (0 or -EINVAL). Run under -fsanitize=address,undefined to
// make any out-of-bounds access fatal (see file header for the gcc line).
static void
fuzz_truncations(const uint8_t *blob, size_t size, const char *name)
{
	struct pg106_acr_bl_ucode bl;
	struct pg106_acr_hs_ucode hs;
	struct pg106_acr_lsf_signature_v1 sig;
	size_t len;

	for (len = 0; len <= size; len++) {
		(void)pg106_acr_ucode_parse_bl(blob, len, &bl);
		(void)pg106_acr_ucode_parse_hs(blob, len, &hs);
		(void)pg106_acr_lsf_sig_parse(blob, len, &sig);
	}
	(void)name;
}

// Seeded single-byte mutations: every parse must stay in-bounds and return
// 0 or -EINVAL, never crash or hang.
static void
fuzz_mutations(const uint8_t *blob, size_t size, const char *name,
	       unsigned rounds)
{
	struct pg106_acr_bl_ucode bl;
	struct pg106_acr_hs_ucode hs;
	uint8_t *copy = malloc(size);
	uint32_t seed = 0x12345678u;
	unsigned i;

	if (!copy)
		return;
	for (i = 0; i < rounds; i++) {
		size_t off = rng_next(&seed) % size;

		memcpy(copy, blob, size);
		copy[off] ^= (uint8_t)(rng_next(&seed) | 1u);
		(void)pg106_acr_ucode_parse_bl(copy, size, &bl);
		(void)pg106_acr_ucode_parse_hs(copy, size, &hs);
	}
	free(copy);
	(void)name;
}

static void
test_fuzz(const char *dir)
{
	static const char *blobs[] = {
		"acr/bl.bin",
		"acr/ucode_load.bin",
		"acr/ucode_unload.bin",
		"acr/unload_bl.bin",
		"gr/fecs_sig.bin",
		"gr/gpccs_sig.bin",
	};
	size_t i;

	for (i = 0; i < sizeof(blobs) / sizeof(blobs[0]); i++) {
		size_t size;
		uint8_t *blob = load_blob(dir, blobs[i], &size);

		CHECK(blob);
		if (!blob)
			continue;
		fuzz_truncations(blob, size, blobs[i]);
		fuzz_mutations(blob, size, blobs[i], 20000);
		free(blob);
	}
	// LS firmware builders: every truncation of the real inputs returns
	// cleanly (run under ASan to make an over-read fatal).
	{
		size_t ds, is, bs, ns, as, len, n;
		uint8_t *desc = load_blob(dir, "sec2/desc-1.bin", &ds);
		uint8_t *img = load_blob(dir, "sec2/image-1.bin", &is);
		uint8_t *bl = load_blob(dir, "gr/fecs_bl.bin", &bs);
		uint8_t *inst = load_blob(dir, "gr/fecs_inst.bin", &ns);
		uint8_t *data = load_blob(dir, "gr/fecs_data.bin", &as);
		struct pg106_acr_lsfw l;

		CHECK(desc && img && bl && inst && data);
		if (desc && img && bl && inst && data) {
			for (len = 0; len <= ds; len++)
				(void)pg106_acr_lsfw_from_ls_desc_v1(
					desc, len, img, is, PG106_ACR_LSF_SEC2,
					&l);
			for (len = 0; len <= is; len += 0x100)
				(void)pg106_acr_lsfw_from_ls_desc_v1(
					desc, ds, img, len, PG106_ACR_LSF_SEC2,
					&l);
			for (len = 0; len <= bs; len++)
				(void)pg106_acr_lsfw_from_bl_inst_data(
					bl, len, inst, ns, data, as,
					PG106_ACR_LSF_FECS, NULL, 0, &n, &l);
		}
		free(desc);
		free(img);
		free(bl);
		free(inst);
		free(data);
	}
	printf("fuzz: truncations + 20000 mutations per blob done\n");
}

// --- main -------------------------------------------------------------------

int
main(void)
{
	const char *dir = fw_dir();

	if (!dir) {
		printf("FAIL: research/firmware/gp106 not found (run from "
		       "pascal/ or repo root)\n");
		return 1;
	}
	printf("== pg106 ACR parser tests (blobs: %s)\n", dir);

	test_real_ucode_load(dir);
	test_real_ucode_unload(dir);
	test_real_bl(dir, "acr/bl.bin", 1280, 0xfd);
	test_real_bl(dir, "acr/unload_bl.bin", 1280, 0x100);
	test_real_ls_sigs(dir);

	test_wpr_layout_golden();
	test_wpr_header_golden(dir);
	test_lsb_header_golden(dir);
	test_wpr_parse_roundtrip();
	test_bld_build(dir);

	test_rejection();
	test_desc_v1_offsets();
	test_hs_section_bounds(dir);
	test_fuzz(dir);

	if (g_failures) {
		printf("== acr: %d FAILURE(S)\n", g_failures);
		return 1;
	}
	printf("== acr: PASS\n");
	return 0;
}
