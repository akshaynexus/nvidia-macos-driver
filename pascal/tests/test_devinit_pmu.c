// SPDX-License-Identifier: GPL-2.0-only
// pascal/tests/test_devinit_pmu.c — GP106 POST model (gm200 VBIOS-PMU
// DEVINIT) against a synthetic image and the committed real ROMs.
//
// SIMULATION ONLY. The "PMU" is a mock falcon that decodes the IMEM/DMEM
// port writes nouveau issues (devinit/gm200.c:32-80) so the test can prove
// the uploaded bytes are exactly the ROM ranges nvbios_pmuRm and BIT 'I'
// +0x14..+0x1a name; its "DEVINIT done" response (0x10a040 bit 13 after
// CPUCTL=2) is a test stand-in for the PMU firmware, not evidence of it.
//
// Upstream oracle used per real ROM (independent of the code under test):
//   'I' version != 1 or length < 0x1c  -> -EINVAL "VBIOS PMU init data not
//                                         found" (gm200.c:134-138)
//   'p' table header outside the image -> -EINVAL "VBIOS PMU/DEVINIT not
//                                         found" (nvbios_rd32 OOB -> 0 ->
//                                         no entries, pmu.c:29-102)
//   else                               -> uploads + exec + poll succeed.
// A ROM that is not present is a PASS-SKIP with the path printed.

#include <pascal/devinit_pmu.h>
#include <pascal/vbios_bit.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define REQUIRE(c)                                                         \
	do {                                                               \
		if (!(c)) {                                                \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
			failures++;                                        \
		}                                                          \
	} while (0)

// --- mock PMU falcon ------------------------------------------------------

#define MEM_BYTES 0x20000u
struct falcon {
	uint8_t imem[MEM_BYTES];
	uint8_t imem_at_devinit[MEM_BYTES]; // snapshot when DEVINIT starts
	unsigned secure_at_devinit;
	uint8_t dmem[MEM_BYTES];
	uint32_t imem_ptr, imem_ctl, dmem_ptr, dmem_ctl;
	uint32_t mailbox, bootvec, cpuctl;
	unsigned resets, imem_secure_words;
	int respond; // set bit 13 on start?
	uint64_t clock_us;
};
static struct falcon F;

static void
put32m(uint8_t *m, uint32_t a, uint32_t v)
{
	if (a + 4 <= MEM_BYTES)
		for (int i = 0; i < 4; i++)
			m[a + i] = (uint8_t)(v >> (8 * i));
}

static uint32_t
get32m(const uint8_t *m, uint32_t a)
{
	if (a + 4 > MEM_BYTES)
		return 0xdeadbeef;
	return (uint32_t)m[a] | (uint32_t)m[a + 1] << 8 |
	       (uint32_t)m[a + 2] << 16 | (uint32_t)m[a + 3] << 24;
}

static uint32_t
f_rd32(void *u, uint32_t addr)
{
	(void)u;
	switch (addr) {
	case PG106_PMU_FALCON_DMEMD0: {
		uint32_t v = get32m(F.dmem, F.dmem_ptr);
		if (F.dmem_ctl & 0x02000000) // AINCR
			F.dmem_ptr += 4;
		return v;
	}
	case PG106_PMU_FALCON_MAILBOX:
		return F.mailbox;
	default:
		return 0;
	}
}

static void
f_wr32(void *u, uint32_t addr, uint32_t val)
{
	(void)u;
	switch (addr) {
	case PG106_PMU_FALCON_IMEMC0:
		F.imem_ctl = val;
		F.imem_ptr = val & 0x00fffffc;
		break;
	case PG106_PMU_FALCON_IMEMT0:
		break; // tag: the block number (ptr >> 8) — ptr already tracks it
	case PG106_PMU_FALCON_IMEMD0:
		put32m(F.imem, F.imem_ptr, val);
		if (F.imem_ctl & 0x10000000)
			F.imem_secure_words++;
		if (F.imem_ctl & 0x01000000)
			F.imem_ptr += 4;
		break;
	case PG106_PMU_FALCON_DMEMC0:
		F.dmem_ctl = val;
		F.dmem_ptr = val & 0x00fffffc;
		break;
	case PG106_PMU_FALCON_DMEMD0:
		put32m(F.dmem, F.dmem_ptr, val);
		if (F.dmem_ctl & 0x01000000)
			F.dmem_ptr += 4;
		break;
	case PG106_PMU_FALCON_MAILBOX:
		F.mailbox = val;
		break;
	case PG106_PMU_FALCON_BOOTVEC:
		F.bootvec = val;
		break;
	case PG106_PMU_FALCON_CPUCTL:
		F.cpuctl = val;
		// Test stand-in for the VBIOS DEVINIT app finishing.
		if (val == 2 && F.mailbox == 0x5000) {
			// DEVINIT start: snapshot IMEM before PRE_OS
			// (gm200.c:174) overwrites it.
			memcpy(F.imem_at_devinit, F.imem, MEM_BYTES);
			F.secure_at_devinit = F.imem_secure_words;
			if (F.respond)
				F.mailbox |= PG106_PMU_DEVINIT_DONE;
		}
		break;
	default:
		break;
	}
}

static int
f_reset(void *u)
{
	(void)u;
	F.resets++;
	return 0;
}

static void
f_time(void *u, uint32_t us)
{
	(void)u;
	F.clock_us += us;
}

static const struct pg106_pmu_bus BUS = { f_rd32, f_wr32, f_reset, f_time, NULL };

#define TRACE_CAP 40000
static struct pg106_pmu_step tr1[TRACE_CAP], tr2[TRACE_CAP];

// --- synthetic image -------------------------------------------------------
//
// image 0 (0x1000, NPDE-sized): 55aa, PCIR @0x40 size 0x10 blocks (0x2000 —
// deliberately LARGER than the NPDE size, the GP107M-style layout), NPDE
// @0x60 size 8 blocks (0x1000). BIT @0x100: 'I' v1 len 0x1c @0x200, 'p'
// v2 len 4 @0x220.
// image 1 @0x1000 (type 0xe0, 0x1000): the PMU table/app, addressed by the
// ROM as 0x1000 + x == image0_size + x -> remap to imaged_addr + x.
// 'p' pointer 0x1100 -> table @0x1100 (remapped 0x1100 too: image0_size
// == imaged_addr here, so to test the remap image 1 is placed at 0x1800
// instead and the ROM pointers stay 0x1xxx).

#define S_SIZE 0x3000u
#define IMG1   0x1800u // imaged_addr; ROM addr A >= 0x1000 -> A - 0x1000 + 0x1800
static uint8_t S[S_SIZE];

static void p8(uint32_t o, uint8_t v) { S[o] = v; }
static void p16(uint32_t o, uint16_t v) { S[o] = (uint8_t)v; S[o + 1] = (uint8_t)(v >> 8); }
static void p32(uint32_t o, uint32_t v) { for (int i = 0; i < 4; i++) S[o + i] = (uint8_t)(v >> (8 * i)); }
static uint32_t R(uint32_t rom) { return rom >= 0x1000 ? rom - 0x1000 + IMG1 : rom; }

static void
build_synth(void)
{
	memset(S, 0, sizeof(S));
	// image 0
	p16(0x0000, 0xaa55);
	p16(0x0018, 0x0040);
	memcpy(&S[0x40], "PCIR", 4);
	p16(0x40 + 0x0a, 0x18);           // PCIR header length
	p16(0x40 + 0x10, 0x10);           // 0x2000 (overridden by NPDE)
	p8(0x40 + 0x14, 0x00);
	memcpy(&S[0x60], "NPDE", 4);      // (0x40 + 0x18 + 0xf) & ~0xf = 0x60
	p16(0x60 + 0x08, 0x08);           // NPDE 0x1000
	p8(0x60 + 0x0a, 0x00);            // not last
	// BIT
	memcpy(&S[0x100], "\xff\xb8" "BIT", 5);
	p8(0x109, 6); p8(0x10a, 2);
	p8(0x10c, 'I'); p8(0x10d, 1); p16(0x10e, 0x1c); p16(0x110, 0x200);
	p8(0x112, 'p'); p8(0x113, 2); p16(0x114, 4); p16(0x116, 0x220);
	// 'I': tables @0x0400 len 0x22, boot scripts @0x0500 len 0x41
	p16(0x200 + 0x14, 0x0400); p16(0x200 + 0x16, 0x22);
	p16(0x200 + 0x18, 0x0500); p16(0x200 + 0x1a, 0x41);
	for (unsigned i = 0; i < 0x24; i++) S[0x400 + i] = (uint8_t)(0xa0 + i);
	for (unsigned i = 0; i < 0x44; i++) S[0x500 + i] = (uint8_t)(0x30 + i);
	// 'p' -> ROM 0x1100 (beyond image0: remapped into image 1)
	p32(0x220, 0x1100);

	// image 1 @IMG1: type 0xe0, last
	p16(IMG1 + 0x00, 0xaa55);
	p16(IMG1 + 0x18, 0x0020);
	memcpy(&S[IMG1 + 0x20], "NPDS", 4);
	p16(IMG1 + 0x20 + 0x0a, 0x18);
	p16(IMG1 + 0x20 + 0x10, 0x08);
	p8(IMG1 + 0x20 + 0x14, 0xe0);
	p8(IMG1 + 0x20 + 0x15, 0x80);     // last
	// The ROM chain walks image0.base + image0.size = 0x1000, which is
	// NOT IMG1 — so put a second copy of the image-1 header at 0x1000
	// with size 0x800 (type 0x00) to reach IMG1 = 0x1800 next.
	p16(0x1000, 0xaa55);
	p16(0x1018, 0x0020);
	memcpy(&S[0x1020], "PCIR", 4);
	p16(0x1020 + 0x0a, 0x18);
	p16(0x1020 + 0x10, 0x04);         // 0x800
	p8(0x1020 + 0x14, 0x03);

	// PMU table at ROM 0x1100: ver 1 hdr 4 len 6 cnt 2
	p8(R(0x1100) + 0, 0x01); p8(R(0x1100) + 1, 4);
	p8(R(0x1100) + 2, 6); p8(R(0x1100) + 3, 2);
	// [0] type 0x04 DEVINIT -> desc ROM 0x1200; [1] PRE_OS -> 0x1600
	p8(R(0x1104), 0x04); p32(R(0x1104) + 2, 0x1200);
	p8(R(0x110a), 0x01); p32(R(0x110a) + 2, 0x1600);
	// DEVINIT descriptor (pmu.c:83-97 offsets)
	uint32_t d = R(0x1200);
	p32(d + 0x08, 0x0000aa00);  // init_addr_pmu
	p32(d + 0x0c, 0x00000100);  // args_addr_pmu (DMEM)
	p32(d + 0x10, 0x00000000);  // boot base
	p32(d + 0x18, 0x00000000);  // boot start
	p32(d + 0x1c, 0x00000040);  // boot end -> boot_size 0x40
	p32(d + 0x20, 0x00000080);  // code_size
	p32(d + 0x24, 0x000000c0);  // data offset (from desc + 0x30)
	p32(d + 0x28, 0x00000000);  // data_addr_pmu
	p32(d + 0x2c, 0x00000220);  // data_size
	for (unsigned i = 0; i < 0x40 + 0x80; i++) S[d + 0x30 + i] = (uint8_t)(i ^ 0x5a);
	// data segment: args at DMEM 0x100: [0x108] = 0x0200 (args base);
	// DMEM[0x200 + 0x08] = 0x1000 -> tables DMEM; [0x200+0x10] = 0x1800
	for (unsigned i = 0; i < 0x220; i++) S[d + 0x30 + 0xc0 + i] = 0;
	p32(d + 0x30 + 0xc0 + 0x108, 0x00000200);
	p32(d + 0x30 + 0xc0 + 0x110, 0x00000200);
	// pmu_args(args+8, 0x08): DMEMC=0x108; rd -> 0x200; DMEMC=0x208;
	// rd DMEM[0x208]. Put the answers there.
	p32(d + 0x30 + 0xc0 + 0x208, 0x00001000);
	p32(d + 0x30 + 0xc0 + 0x210, 0x00001800);
	// PRE_OS descriptor: tiny
	uint32_t q = R(0x1600);
	p32(q + 0x08, 0x00000010); p32(q + 0x1c, 0x00000010);
	p32(q + 0x20, 0x00000010); p32(q + 0x24, 0x20); p32(q + 0x2c, 0x10);
	p32(q + 0x28, 0x00000800);
}

static void
falcon_reset_state(int respond)
{
	memset(&F, 0, sizeof(F));
	F.respond = respond;
}

static void
test_synthetic(void)
{
	struct pg106_vbios_bit vb;
	struct pg106_post_result r1, r2;

	build_synth();
	REQUIRE(pg106_vbios_bit_parse(S, S_SIZE, &vb) == 0);
	// nvkm_bios_new: image0 = NPDE size, imaged = first 0xe0 image.
	REQUIRE(vb.image0_size == 0x1000);
	REQUIRE(vb.imaged_addr == IMG1);
	REQUIRE(pg106_vbios_addr(&vb, 0x1100) == R(0x1100));
	REQUIRE(pg106_vbios_addr(&vb, 0x0fff) == 0x0fff);

	struct pg106_vbios_pmu_app app;
	REQUIRE(pg106_vbios_pmu_app(&vb, 0x04, &app) == 0);
	REQUIRE(app.desc == 0x1200 && app.boot_addr == 0x1230 &&
		app.boot_size == 0x40 && app.code_addr == 0x1270 &&
		app.code_addr_pmu == 0x40 && app.code_size == 0x80 &&
		app.data_addr == 0x1200 + 0x30 + 0xc0 &&
		app.data_size == 0x220 && app.init_addr_pmu == 0xaa00 &&
		app.args_addr_pmu == 0x100);
	REQUIRE(pg106_vbios_pmu_app(&vb, 0x07, &app) == -ENOENT);

	// --- post = true, PMU responds --------------------------------------
	falcon_reset_state(1);
	int rc = pg106_devinit_gm200_post(&vb, true, true, &BUS, tr1,
					  TRACE_CAP, &r1);
	REQUIRE(rc == 0 && r1.error == NULL);
	REQUIRE(F.resets == 2); // DEVINIT + PRE_OS loads
	REQUIRE(r1.exec_addr == 0xaa00 && r1.args_addr == 0x100);
	REQUIRE(r1.tables_dmem == 0x1000 && r1.tables_img == 0x0400 &&
		r1.tables_len == 0x22);
	REQUIRE(r1.scripts_dmem == 0x1800 && r1.scripts_img == 0x0500 &&
		r1.scripts_len == 0x41);
	REQUIRE(r1.have_preos_app);
	// IMEM == ROM boot + code (remapped reads), padded to 256 B.
	for (unsigned i = 0; i < 0xc0; i++)
		REQUIRE(F.imem_at_devinit[i] == S[R(0x1230) + i]);
	REQUIRE(F.secure_at_devinit == 0x80 / 4 * 2); // code + pad to 256 B
	// DMEM: tables/scripts copied word-wise (len rounded up to 4).
	for (unsigned i = 0; i < 0x24; i++)
		REQUIRE(F.dmem[0x1000 + i] == S[0x400 + i]);
	for (unsigned i = 0; i < 0x44; i++)
		REQUIRE(F.dmem[0x1800 + i] == S[0x500 + i]);
	// PRE_OS was exec'd last (pmu_exec with its own init address).
	REQUIRE(F.bootvec == 0x10 && F.cpuctl == 2);
	REQUIRE(r1.polls == 1);

	// Trace landmarks (gm200.c order).
	REQUIRE(tr1[0].kind == PG106_PMU_STEP_RESET);
	REQUIRE(tr1[1].kind == PG106_PMU_STEP_WR &&
		tr1[1].addr == PG106_PMU_FALCON_IMEMC0 &&
		tr1[1].val == 0x01000000);
	REQUIRE(tr1[2].addr == PG106_PMU_FALCON_IMEMT0 && tr1[2].val == 0);
	int saw_5000 = 0;
	for (size_t i = 0; i < r1.steps && i < TRACE_CAP; i++)
		if (tr1[i].addr == PG106_PMU_FALCON_MAILBOX &&
		    tr1[i].kind == PG106_PMU_STEP_WR && tr1[i].val == 0x5000 &&
		    tr1[i].phase == PG106_POST_PH_EXEC)
			saw_5000 = 1;
	REQUIRE(saw_5000);

	// Determinism.
	falcon_reset_state(1);
	REQUIRE(pg106_devinit_gm200_post(&vb, true, true, &BUS, tr2, TRACE_CAP,
					 &r2) == 0);
	REQUIRE(r2.steps == r1.steps &&
		memcmp(tr1, tr2, r1.steps * sizeof(tr1[0])) == 0);
	printf("PASS devinit_pmu: synthetic gm200 POST — remap 0x1100->0x%x, "
	       "IMEM/DMEM bytes == ROM, %zu steps, deterministic\n",
	       R(0x1100), r1.steps);

	// --- post = true, PMU never answers -> -ETIMEDOUT ------------------
	falcon_reset_state(0);
	rc = pg106_devinit_gm200_post(&vb, true, true, &BUS, NULL, 0, &r1);
	REQUIRE(rc == -ETIMEDOUT && r1.polls == PG106_DEVINIT_POST_POLLS);
	REQUIRE(F.clock_us == 1000ull * PG106_DEVINIT_POST_POLLS);
	// --- post = false: only the presence checks, no bus writes --------
	falcon_reset_state(1);
	rc = pg106_devinit_gm200_post(&vb, false, true, &BUS, tr1, TRACE_CAP,
				      &r1);
	REQUIRE(rc == 0 && r1.steps == 0 && F.resets == 0);
	// --- 'I' too short -> -EINVAL "PMU init data not found" -----------
	p16(0x10e, 0x1a);
	REQUIRE(pg106_vbios_bit_parse(S, S_SIZE, &vb) == 0);
	rc = pg106_devinit_gm200_post(&vb, true, true, &BUS, NULL, 0, &r1);
	REQUIRE(rc == -EINVAL && strstr(r1.error, "init data") != NULL);
	p16(0x10e, 0x1c);
	// --- no remap target -> 'p' table reads land in image 1's header
	//     at 0x1100 raw (zeros) -> DEVINIT not found.
	p8(IMG1 + 0x20 + 0x14, 0x03); // image 1 no longer type 0xe0
	REQUIRE(pg106_vbios_bit_parse(S, S_SIZE, &vb) == 0);
	REQUIRE(vb.imaged_addr == 0);
	rc = pg106_devinit_gm200_post(&vb, true, true, &BUS, NULL, 0, &r1);
	REQUIRE(rc == -EINVAL && strstr(r1.error, "DEVINIT not found"));
	printf("PASS devinit_pmu: timeout, post=false, short 'I', missing "
	       "remap image all follow gm200.c\n");
}

static void
test_needs_post(void)
{
	REQUIRE(pg106_devinit_needs_post(0x00000000, false));
	REQUIRE(!pg106_devinit_needs_post(0x00000002, false));
	REQUIRE(pg106_devinit_needs_post(0x00000002, true)); // NvForcePost
	REQUIRE(pg106_devinit_needs_post(0xfffffffdu, false));
	printf("PASS devinit_pmu: posted bit 0x2240c[1] (gf100.c:101) + "
	       "NvForcePost\n");
}

// --- real ROMs ---------------------------------------------------------------

static const char *const ROMS[] = {
	"nvidia-gtx1060-3gb-tpu208872.rom",
	"gigabyte-gtx1060-6gb-tpu186364.rom",
	"asus-gtx1050ti-tpu188459.rom",
	"msi-gtx1050-tpu193426.rom",
	"msi-gtx1050ti-mobile-tpu241139.rom",
};
#define ROM_DIR "../research/firmware/vbios/"
static uint8_t rom[1u << 20];

static uint16_t r16(const uint8_t *d, uint32_t o) { return (uint16_t)(d[o] | d[o + 1] << 8); }
static uint32_t r32(const uint8_t *d, uint32_t o) { return r16(d, o) | (uint32_t)r16(d, o + 2) << 16; }

static void
test_real(void)
{
	unsigned loaded = 0, posted = 0;

	for (unsigned k = 0; k < sizeof(ROMS) / sizeof(ROMS[0]); k++) {
		char path[256];
		snprintf(path, sizeof(path), ROM_DIR "%s", ROMS[k]);
		FILE *f = fopen(path, "rb");
		if (!f) {
			printf("PASS-SKIP %s not present (ROM files are "
			       "fetched separately; see "
			       "research/firmware/vbios/README.md)\n", path);
			continue;
		}
		size_t n = fread(rom, 1, sizeof(rom), f);
		fclose(f);
		loaded++;

		struct pg106_vbios_bit vb;
		struct pg106_post_result res;
		REQUIRE(pg106_vbios_bit_parse(rom, (uint32_t)n, &vb) == 0);

		// Independent oracle from raw bytes (see header).
		struct pg106_vbios_bit_entry I, p;
		const char *want;
		REQUIRE(pg106_vbios_bit_entry(&vb, 'I', &I) == 0);
		if (I.version != 1 || I.length < 0x1c) {
			want = "VBIOS PMU init data not found";
		} else if (pg106_vbios_bit_entry(&vb, 'p', &p) || p.version != 2) {
			want = "VBIOS PMU/DEVINIT not found";
		} else {
			uint32_t t = r32(rom, p.offset);
			uint32_t te = pg106_vbios_addr(&vb, t);
			want = (te + 4 > n) ? "VBIOS PMU/DEVINIT not found" : NULL;
		}

		falcon_reset_state(1);
		int rc = pg106_devinit_gm200_post(&vb, true, true, &BUS, tr1,
						  TRACE_CAP, &res);
		printf("-- %s: %zu bytes, images %u, image0 0x%x, imaged 0x%x, "
		       "'I' v%u len 0x%x\n", ROMS[k], n, vb.image_count,
		       vb.image0_size, vb.imaged_addr, I.version, I.length);
		if (want) {
			REQUIRE(rc == -EINVAL && res.error &&
				strcmp(res.error, want) == 0);
			printf("   gm200_devinit_post -> %d \"%s\" (nouveau "
			       "would fail nvkm_device_preinit on this image)\n",
			       rc, res.error ? res.error : "");
			continue;
		}
		REQUIRE(rc == 0);
		if (rc)
			continue;
		posted++;
		printf("   DEVINIT app desc 0x%x: boot %#x B -> IMEM %#x, code "
		       "%#x B -> IMEM %#x (secure), data %#x B -> DMEM %#x, "
		       "init %#x, args %#x\n", res.devinit.desc,
		       res.devinit.boot_size, res.devinit.boot_addr_pmu,
		       res.devinit.code_size, res.devinit.code_addr_pmu,
		       res.devinit.data_size, res.devinit.data_addr_pmu,
		       res.exec_addr, res.args_addr);
		printf("   tables  ROM 0x%04x len 0x%04x -> DMEM 0x%x\n",
		       res.tables_img, res.tables_len, res.tables_dmem);
		printf("   scripts ROM 0x%04x len 0x%04x -> DMEM 0x%x\n",
		       res.scripts_img, res.scripts_len, res.scripts_dmem);
		printf("   %zu bus steps (%u IMEM + %u DMEM data words), "
		       "PRE_OS app %s\n", res.steps, res.imem_words,
		       res.dmem_words, res.have_preos_app ? "present" : "absent");
		// Bytes uploaded == ROM bytes ('I' pointers are image-0 ROM
		// addresses; remap does not apply below image0_size).
		REQUIRE(res.tables_img == r16(rom, I.offset + 0x14u));
		REQUIRE(res.scripts_len == r16(rom, I.offset + 0x1au));
		if (res.tables_dmem + res.tables_len < MEM_BYTES)
			REQUIRE(memcmp(&F.dmem[res.tables_dmem],
				       &rom[pg106_vbios_addr(&vb, res.tables_img)],
				       res.tables_len) == 0);
		if (res.scripts_dmem + res.scripts_len < MEM_BYTES)
			REQUIRE(memcmp(&F.dmem[res.scripts_dmem],
				       &rom[pg106_vbios_addr(&vb, res.scripts_img)],
				       res.scripts_len) == 0);
		REQUIRE(memcmp(&F.imem_at_devinit[res.devinit.code_addr_pmu],
			       &rom[pg106_vbios_addr(&vb, res.devinit.code_addr)],
			       res.devinit.code_size < 64 ? res.devinit.code_size : 64) == 0);
		REQUIRE(!res.trace_truncated);
	}
	if (!loaded)
		printf("PASS-SKIP real-ROM POST model: no ROMs under " ROM_DIR
		       "\n");
	else
		printf("PASS devinit_pmu: %u ROM(s) through gm200_devinit_post; "
		       "%u complete upload(s), the rest rejected exactly where "
		       "nouveau rejects them\n", loaded, posted);
}

int
main(void)
{
	printf("devinit_pmu tests — SIMULATION ONLY (mock PMU falcon; not "
	       "hardware evidence)\n");
	test_needs_post();
	test_synthetic();
	test_real();
	if (failures) {
		printf("%d check(s) FAILED\n", failures);
		return EXIT_FAILURE;
	}
	printf("all checks passed\n");
	return EXIT_SUCCESS;
}
