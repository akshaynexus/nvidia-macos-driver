// SPDX-License-Identifier: GPL-2.0-only
// pascal/tests/test_acr_boot.c — ACR secure-boot + SEC2 RTOS SIMULATION test.
//
// **********************************************************************
// **  SIMULATION, NOT HARDWARE PROOF.                                   **
// **  pg106_acrboot_plan_build()'s step list and the pg106_sec2_*        **
// **  queue code run against a MOCK falcon. Nothing here runs on,       **
// **  reads, or proves anything about a real GP106.                     **
// **********************************************************************
//
// What IS real: every firmware file, parsed by pascal/core/src/acr.c —
// acr/{bl,ucode_load}.bin (linux-firmware nvidia/gp102/acr, which WHENCE
// links as nvidia/gp106/acr), gr/{fecs,gpccs}_{bl,inst,data,sig}.bin and
// sec2/{desc,image,sig}-1.bin (see research/firmware/gp106/README.md).
//
// The mock models REGISTER SEMANTICS taken from nouveau, not the answers
// the plan expects:
//   * 0x10c scrub BUSY bits read 6 for a while after an engine reset or a
//     PMC enable, then clear (gm200.c:145-147 waits for them to CLEAR);
//     IMEM/DMEM PIO while scrubbing is counted as a violation.
//   * falcon registers read 0 / drop writes while the SEC2 PMC_ENABLE bit
//     is clear; nvkm_mask RMWs therefore see real old values.
//   * the bind handshake raises 0x008 bit 3 and walks 0x0dc 1 -> 5 -> 0
//     (gm200.c:122-138, 270-283).
//   * STARTCPU (CPUCTL bit 1) is write-only (dev_falcon_v4.h
//     NV_PFALCON_FALCON_CPUCTL_STARTCPU "-WXVF"): reads never return it.
//   * a toy "ACR HS firmware" runs on START: it consumes the BL argument
//     block from DMEM 0, the patched signature and flcn_acr_desc_v1 from
//     the mapped image, the WPR shadow, and programs the 0x100cd4 windows
//     from the desc. It loads the WPR entries with lazy_bootstrap == 0
//     (only those) and rejects bootloader blocks whose DMA bases were not
//     wpr_patch'ed into the WPR.
//   * a toy "SEC2 RTOS" posts the init message on START (only if the ACR
//     loaded it), executes BOOTSTRAP_FALCON commands from the cmdq ring
//     and answers in the msgq ring, raising SWGEN0 (0x008 bit 6).
// Every SIM value (queue offsets in EMEM, IRQDEST, scrub latency, error
// codes) is a stand-in; real values come from hardware/firmware.
//
// Run: cd pascal && make test

#include <pascal/acr.h>
#include <pascal/acr_boot.h>
#include <pascal/devboot.h>
#include <pascal/devinit_pmu.h>
#include <pascal/hwexec.h>
#include <pascal/top.h>

#include "mockgpu.h"

#include <errno.h>
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
			printf("FAIL %s:%d: %s == %s "                     \
			       "(0x%llx != 0x%llx)\n", __func__, __LINE__, \
			       #a, #b, va_, vb_);                          \
			g_failures++;                                      \
		}                                                          \
	} while (0)

// --- file loading ---------------------------------------------------------------

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

static uint32_t
rd32le(const uint8_t *b, size_t off)
{
	return (uint32_t)b[off] | ((uint32_t)b[off + 1] << 8) |
	       ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 3] << 24);
}

static uint64_t
rd64le(const uint8_t *b, size_t off)
{
	return (uint64_t)rd32le(b, off) | ((uint64_t)rd32le(b, off + 4) << 32);
}

static void
wr32le(uint8_t *b, size_t off, uint32_t v)
{
	b[off] = (uint8_t)v;
	b[off + 1] = (uint8_t)(v >> 8);
	b[off + 2] = (uint8_t)(v >> 16);
	b[off + 3] = (uint8_t)(v >> 24);
}

// --- fixture --------------------------------------------------------------------

#define SEC2_BASE PG106_ACRBOOT_SEC2_BASE_GP106
#define ACR_VMA 0x100000000ull
#define WPR_ALLOC_BASE 0x0f0000000ull
// SIM stand-in HWCFG for SEC2: decodes to 64 KB IMEM / 64 KB DMEM — the
// PMU word captured on GP106 (FINDINGS.md:66); SEC2's own word was not
// captured, only "IMEM 64KB visible" (FINDINGS.md:160).
#define SEC2_HWCFG_SIM 0x400e0100u
#define BOOT0_GP106_A1 0x136000a1u // FINDINGS.md:50

struct fixture {
	uint8_t *bl_blob, *hs_blob, *fecs_sig_blob, *gpccs_sig_blob;
	uint8_t *fecs_bl, *fecs_inst, *fecs_data, *gpccs_bl, *gpccs_inst,
		*gpccs_data, *sec2_desc, *sec2_img, *sec2_sig_blob;
	size_t bl_size, hs_size, fecs_sig_size, gpccs_sig_size, fecs_bl_size,
		fecs_inst_size, fecs_data_size, gpccs_bl_size, gpccs_inst_size,
		gpccs_data_size, sec2_desc_size, sec2_img_size, sec2_sig_size;
	uint8_t *fecs_img, *gpccs_img; // composed WPR images
	struct pg106_acr_bl_ucode bl;
	struct pg106_acr_hs_ucode hs;
	struct pg106_acr_lsf_signature_v1 fecs_sig, gpccs_sig, sec2_sig;
	struct pg106_acr_lsfw lsfw[3]; // FECS, GPCCS, SEC2 (acr->lsfw order)
	const struct pg106_acr_lsf_signature_v1 *sigs[3];
	struct pg106_acrboot_config cfg;
};

static int
load_all(struct fixture *fx, const char *dir)
{
	struct {
		const char *rel;
		uint8_t **p;
		size_t *n;
	} files[] = {
		{ "acr/bl.bin", &fx->bl_blob, &fx->bl_size },
		{ "acr/ucode_load.bin", &fx->hs_blob, &fx->hs_size },
		{ "gr/fecs_sig.bin", &fx->fecs_sig_blob, &fx->fecs_sig_size },
		{ "gr/gpccs_sig.bin", &fx->gpccs_sig_blob,
		  &fx->gpccs_sig_size },
		{ "gr/fecs_bl.bin", &fx->fecs_bl, &fx->fecs_bl_size },
		{ "gr/fecs_inst.bin", &fx->fecs_inst, &fx->fecs_inst_size },
		{ "gr/fecs_data.bin", &fx->fecs_data, &fx->fecs_data_size },
		{ "gr/gpccs_bl.bin", &fx->gpccs_bl, &fx->gpccs_bl_size },
		{ "gr/gpccs_inst.bin", &fx->gpccs_inst, &fx->gpccs_inst_size },
		{ "gr/gpccs_data.bin", &fx->gpccs_data, &fx->gpccs_data_size },
		{ "sec2/desc-1.bin", &fx->sec2_desc, &fx->sec2_desc_size },
		{ "sec2/image-1.bin", &fx->sec2_img, &fx->sec2_img_size },
		{ "sec2/sig-1.bin", &fx->sec2_sig_blob, &fx->sec2_sig_size },
	};
	size_t i;

	for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		*files[i].p = load_blob(dir, files[i].rel, files[i].n);
		if (!*files[i].p) {
			printf("FAIL: missing %s/%s\n", dir, files[i].rel);
			g_failures++;
			return -1;
		}
	}
	return 0;
}

static int
fixture_setup(struct fixture *fx, const char *dir)
{
	size_t n = 0;
	int ret = 0;

	memset(fx, 0, sizeof(*fx));
	if (load_all(fx, dir))
		return -1;

	ret |= pg106_acr_ucode_parse_bl(fx->bl_blob, fx->bl_size, &fx->bl);
	ret |= pg106_acr_ucode_parse_hs(fx->hs_blob, fx->hs_size, &fx->hs);
	ret |= pg106_acr_lsf_sig_parse(fx->fecs_sig_blob, fx->fecs_sig_size,
				       &fx->fecs_sig);
	ret |= pg106_acr_lsf_sig_parse(fx->gpccs_sig_blob, fx->gpccs_sig_size,
				       &fx->gpccs_sig);
	ret |= pg106_acr_lsf_sig_parse(fx->sec2_sig_blob, fx->sec2_sig_size,
				       &fx->sec2_sig);
	CHECK_EQ(ret, 0);
	if (ret)
		return -1;
	CHECK_EQ(fx->fecs_sig.falcon_id, PG106_ACR_LSF_FECS);
	CHECK_EQ(fx->gpccs_sig.falcon_id, PG106_ACR_LSF_GPCCS);
	CHECK_EQ(fx->sec2_sig.falcon_id, PG106_ACR_LSF_SEC2);

	// FECS / GPCCS images: nvkm_acr_lsfw_load_bl_inst_data_sig.
	CHECK_EQ(pg106_acr_lsfw_from_bl_inst_data(
			 fx->fecs_bl, fx->fecs_bl_size, fx->fecs_inst,
			 fx->fecs_inst_size, fx->fecs_data, fx->fecs_data_size,
			 PG106_ACR_LSF_FECS, NULL, 0, &n, &fx->lsfw[0]),
		 0);
	fx->fecs_img = malloc(n);
	CHECK_EQ(pg106_acr_lsfw_from_bl_inst_data(
			 fx->fecs_bl, fx->fecs_bl_size, fx->fecs_inst,
			 fx->fecs_inst_size, fx->fecs_data, fx->fecs_data_size,
			 PG106_ACR_LSF_FECS, fx->fecs_img, n, &n, &fx->lsfw[0]),
		 0);
	CHECK_EQ(pg106_acr_lsfw_from_bl_inst_data(
			 fx->gpccs_bl, fx->gpccs_bl_size, fx->gpccs_inst,
			 fx->gpccs_inst_size, fx->gpccs_data,
			 fx->gpccs_data_size, PG106_ACR_LSF_GPCCS, NULL, 0, &n,
			 &fx->lsfw[1]),
		 0);
	fx->gpccs_img = malloc(n);
	CHECK_EQ(pg106_acr_lsfw_from_bl_inst_data(
			 fx->gpccs_bl, fx->gpccs_bl_size, fx->gpccs_inst,
			 fx->gpccs_inst_size, fx->gpccs_data,
			 fx->gpccs_data_size, PG106_ACR_LSF_GPCCS, fx->gpccs_img,
			 n, &n, &fx->lsfw[1]),
		 0);
	// SEC2 RTOS image: nvkm_acr_lsfw_load_sig_image_desc_v1.
	CHECK_EQ(pg106_acr_lsfw_from_ls_desc_v1(fx->sec2_desc,
						fx->sec2_desc_size,
						fx->sec2_img, fx->sec2_img_size,
						PG106_ACR_LSF_SEC2,
						&fx->lsfw[2]),
		 0);
	fx->sigs[0] = &fx->fecs_sig;
	fx->sigs[1] = &fx->gpccs_sig;
	fx->sigs[2] = &fx->sec2_sig;

	printf("fixture: ACR bl/ucode_load + FECS/GPCCS bl+inst+data+sig + "
	       "SEC2 desc-1/image-1/sig-1 parsed via acr.c\n");
	printf("  FECS  img 0x%zx (bl 0x%x @imem 0x%x, code 0x%x, data 0x%x)\n",
	       fx->lsfw[0].img_size, fx->lsfw[0].bootloader_size,
	       fx->lsfw[0].bootloader_imem_offset,
	       fx->lsfw[0].app_resident_code_size,
	       fx->lsfw[0].app_resident_data_size);
	printf("  GPCCS img 0x%zx flags 0x%x\n", fx->lsfw[1].img_size,
	       fx->lsfw[1].flags);
	printf("  SEC2  img 0x%zx (bl 0x%x @imem 0x%x, entry 0x%x)\n",
	       fx->lsfw[2].img_size, fx->lsfw[2].bootloader_size,
	       fx->lsfw[2].bootloader_imem_offset, fx->lsfw[2].app_imem_entry);

	fx->cfg = (struct pg106_acrboot_config){
		.falcon_base = SEC2_BASE,
		.hwcfg = SEC2_HWCFG_SIM,
		.debug_reg = 0, // production part (SIM)
		.vma_addr = ACR_VMA,
		.wpr_alloc_base = WPR_ALLOC_BASE,
		.bootstrap_owner = PG106_ACR_LSF_SEC2, // gp102.c:89
		.rtos_id = PG106_ACR_LSF_SEC2,	       // acr/base.c:59-72
		.boot_mbox0 = 0,		       // acr/gp102.c:239
		.intr_clear = 0x10,		       // acr/gp102.c:239
		.pmc_boot0 = BOOT0_GP106_A1,
		.pmc_mask = 1u << 8, // overwritten from the top table below
		.bind_target = 0,
		.bind_addr = 0x10000000ull,
		.cpuctl_alias_en = false,
	};
	return 0;
}

static void
fixture_free(struct fixture *fx)
{
	free(fx->bl_blob);
	free(fx->hs_blob);
	free(fx->fecs_sig_blob);
	free(fx->gpccs_sig_blob);
	free(fx->fecs_bl);
	free(fx->fecs_inst);
	free(fx->fecs_data);
	free(fx->gpccs_bl);
	free(fx->gpccs_inst);
	free(fx->gpccs_data);
	free(fx->sec2_desc);
	free(fx->sec2_img);
	free(fx->sec2_sig_blob);
	free(fx->fecs_img);
	free(fx->gpccs_img);
}

// --- mock falcon + toy firmware -------------------------------------------------

#define IMEM_SIZE 0x10000u
#define DMEM_SIZE 0x10000u
#define EMEM_SIZE 0x2000u
#define SCRUB_READS 3 // SIM: busy for 3 reads of 0x10c after a reset
#define HALT_READS 2  // SIM: ACR halts after 2 CPUCTL reads
#define NREG 64
// SIM queue layout the toy RTOS reports (in EMEM, emem_addr-based).
#define Q_CMDQ_OFF (PG106_SEC2_EMEM_ADDR + 0x000u)
#define Q_CMDQ_SIZE 0x80u
#define Q_MSGQ_OFF (PG106_SEC2_EMEM_ADDR + 0x100u)
#define Q_MSGQ_SIZE 0x80u

struct mock {
	// configuration
	const struct fixture *fx;
	uint32_t pmc_mask;
	bool alias_en;
	uint32_t fail_falcon; // RTOS answers error_code 1 for this id
	bool rtos_wrong_unit; // RTOS answers with unit 0x07 (msg to nobody)

	// plain registers
	uint32_t raddr[NREG], rval[NREG];
	size_t nreg;
	uint32_t pmc_enable;
	uint32_t wpr_sel;
	uint64_t wpr_win_start, wpr_win_end;

	// falcon state
	int scrub_left;
	uint32_t intr_stat;
	int bind_phase; // 0 idle, 1 requested, 2 bound (state 5), 3 trigger
	int bind_reads;
	uint32_t imemc, dmemc, ememc;
	uint8_t imem[IMEM_SIZE];
	uint16_t imem_tag[IMEM_SIZE / 0x100];
	uint8_t dmem[DMEM_SIZE];
	uint8_t emem[EMEM_SIZE];
	uint32_t bootvec;
	uint32_t mbox0, mbox1;
	int run_state; // 0 idle, 1 HS running, 2 HS halted, 3 RTOS running
	int halt_reads;
	uint32_t irqdest;

	// memory objects the plan writes
	uint8_t *img;  // HS image as mapped at ACR_VMA
	size_t img_size;
	uint8_t *wpr;  // shadow buffer (object-relative)
	size_t wpr_size;

	// toy firmware outcomes
	bool acr_ok;
	uint32_t acr_err;
	bool rtos_loaded;
	uint32_t rtos_cmdq_tail, rtos_msgq_head;
	uint64_t booted_mask;
	uint32_t nboot_cmds, nrewinds;

	// violations / counters
	uint32_t pio_during_scrub;
	uint32_t writes_while_gated;
	uint32_t imem_bytes, dmem_bytes;
	size_t n_imemc, n_dmemc;
	uint32_t imemc_vals[8], dmemc_vals[8];
	uint64_t time_us;

	// Optional GPU memory model (tests/mockgpu.h): every non-SEC2,
	// non-PMC register goes to it (PRAMIN, MMU invalidate, FB), and the
	// toy ACR fetches the HS image by falcon DMA through the instance
	// block in BINDINST, and the WPR shadow from VRAM.
	struct mockgpu *gpu;
	uint32_t dma_words, dma_faults;
	uint64_t dma_inst;
};

static uint32_t *
reg_slot(struct mock *m, uint32_t addr, bool create)
{
	size_t i;

	for (i = 0; i < m->nreg; i++)
		if (m->raddr[i] == addr)
			return &m->rval[i];
	if (!create || m->nreg == NREG)
		return NULL;
	m->raddr[m->nreg] = addr;
	m->rval[m->nreg] = 0;
	return &m->rval[m->nreg++];
}

static uint32_t
reg_get(struct mock *m, uint32_t addr)
{
	uint32_t *p = reg_slot(m, addr, false);

	return p ? *p : 0;
}

static void
reg_set(struct mock *m, uint32_t addr, uint32_t v)
{
	uint32_t *p = reg_slot(m, addr, true);

	if (p)
		*p = v;
}

static bool
sec2_powered(const struct mock *m)
{
	return !m->pmc_mask || (m->pmc_enable & m->pmc_mask) == m->pmc_mask;
}

static void
falcon_reset_state(struct mock *m)
{
	m->scrub_left = SCRUB_READS;
	m->intr_stat = 0;
	m->bind_phase = 0;
	m->run_state = 0;
	m->mbox0 = m->mbox1 = 0;
	m->bootvec = 0;
	memset(m->imem, 0, sizeof(m->imem));
	memset(m->dmem, 0, sizeof(m->dmem));
	memset(m->imem_tag, 0xff, sizeof(m->imem_tag));
}

static uint8_t *
mem_at(struct mock *m, uint32_t ctl_off, uint32_t addr)
{
	if (ctl_off == PG106_FALCON_EMEMC)
		return addr + 4 <= EMEM_SIZE ? &m->emem[addr] : NULL;
	return addr + 4 <= DMEM_SIZE ? &m->dmem[addr] : NULL;
}

// toy RTOS: post one message into the msgq ring (wrapping like the
// firmware must for nvkm_falcon_msgq_pop's `head < position` rewind,
// msgq.c:57-60).
static void
rtos_post(struct mock *m, const uint8_t *msg, uint32_t size)
{
	uint32_t at = m->rtos_msgq_head;
	uint8_t *p;

	if (at + size > Q_MSGQ_OFF + Q_MSGQ_SIZE)
		at = Q_MSGQ_OFF;
	p = &m->emem[at - PG106_SEC2_EMEM_ADDR];
	memcpy(p, msg, size);
	m->rtos_msgq_head = at + ((size + 3u) & ~3u);
	reg_set(m, SEC2_BASE + PG106_SEC2_MSGQ_HEAD, m->rtos_msgq_head);
	m->intr_stat |= 0x40; // SWGEN0
}

// toy ACR HS firmware: consume what the host set up, validate it like the
// real ACR would have to, program the WPR windows, load the non-lazy LSFs.
static void
acr_run(struct mock *m)
{
	const struct fixture *fx = m->fx;
	const uint8_t *d = m->dmem;
	uint8_t expect[PG106_FLCN_BL_DMEM_DESC_V1_SIZE];
	const uint8_t *sig;
	uint64_t wpr_start, wpr_end;
	size_t pos = 0, blk;
	int found_bl = 0;

	m->acr_ok = false;
	m->acr_err = 0;
	if (m->gpu) {
		// Falcon DMA (DMAIDX_VIRT): BINDINST names the instance block
		// (gm200_flcn_bind_inst: bit 30 valid, 29:28 target, addr >> 12);
		// the BL fetches code_dma_base.. through that VMM.
		uint32_t bi = reg_get(m, SEC2_BASE + PG106_FALCON_BINDINST);
		uint64_t inst = (uint64_t)(bi & 0x0fffffffu) << 12, shadow;
		size_t i;

		m->dma_inst = inst;
		if (!(bi & 0x40000000u) || ((bi >> 28) & 3u) != 0) {
			m->acr_err = 0xb3;
			goto halt;
		}
		memset(m->img, 0, m->img_size);
		for (i = 0; i + 4 <= m->img_size; i += 4) {
			bool okr = true;
			uint32_t v = mg_va_rd32(m->gpu, inst, ACR_VMA + i, &okr);

			if (!okr) {
				m->dma_faults++;
				m->acr_err = 0xb4;
				goto halt;
			}
			wr32le(m->img, i, v);
			m->dma_words++;
		}
		// The ACR reads the shadow WPR from VRAM (desc 0x238 >> 8).
		shadow = (uint64_t)rd32le(m->img, fx->hs.dmem_base_img + 0x238) << 8;
		for (i = 0; i + 4 <= m->wpr_size; i += 4)
			wr32le(m->wpr, i, mg_vram_rd32(m->gpu, shadow + i));
	}
	// The boot ROM fetches BOOTVEC through the IMEM tags; the block
	// tagged BOOTVEC>>8 must hold the BL (gm200.c:228,290-292).
	for (blk = 0; blk < IMEM_SIZE / 0x100; blk++)
		if (m->imem_tag[blk] == (m->bootvec >> 8) &&
		    memcmp(&m->imem[blk * 0x100], fx->bl.boot_code, 0x100) == 0)
			found_bl = 1;
	if (!found_bl) {
		m->acr_err = 0xb1;
		goto halt;
	}
	// The BL reads flcn_bl_dmem_desc_v1 from DMEM 0
	// (gm200_acr_hsfw_load_bld, acr/gm200.c:224-242).
	pg106_acr_hsfw_bld_build(&fx->hs, ACR_VMA, expect);
	if (memcmp(d, expect, sizeof(expect)) != 0) {
		m->acr_err = 0xb2;
		goto halt;
	}
	// Signature patched into the image (prod unless debug-fused).
	sig = fx->hs_blob + ((reg_get(m, SEC2_BASE + PG106_FALCON_SEC2_DEBUG) &
			      PG106_FALCON_SEC2_DEBUG_DBG)
				     ? fx->hs.sig_dbg_off
				     : fx->hs.sig_prod_off);
	if (memcmp(m->img + fx->hs.sig_base_img, sig, fx->hs.sig_size) != 0) {
		m->acr_err = 0x5a;
		goto halt;
	}
	// flcn_acr_desc_v1 region 0 from the image (gp102_acr_load_setup).
	wpr_start = (uint64_t)rd32le(m->img, fx->hs.dmem_base_img + 0x220) << 8;
	wpr_end = (uint64_t)rd32le(m->img, fx->hs.dmem_base_img + 0x224) << 8;
	if (wpr_end <= wpr_start) {
		m->acr_err = 0xde;
		goto halt;
	}
	m->wpr_win_start = wpr_start;
	m->wpr_win_end = wpr_end;
	// Walk the WPR headers; validate every bld points into the WPR.
	for (;;) {
		struct pg106_acr_wpr_header_v1 h;
		bool term = false;
		uint32_t tail_bld;
		uint64_t bld_code; // 64-bit DMA base: a WPR above 4 GiB is real

		if (pg106_acr_wpr_header_next(m->wpr, m->wpr_size, &pos, &h,
					      &term) || term)
			break;
		if (h.status != PG106_ACR_WPR_STATUS_COPY ||
		    h.bootstrap_owner != PG106_ACR_LSF_SEC2) {
			m->acr_err = 0x51;
			goto halt;
		}
		tail_bld = rd32le(m->wpr, h.lsb_offset + 192 + 20);
		bld_code = rd64le(m->wpr, tail_bld + 36);
		if (bld_code < wpr_start || bld_code >= wpr_end) {
			m->acr_err = 0xd3; // unpatched DMA base
			goto halt;
		}
		if (!h.lazy_bootstrap && h.falcon_id == PG106_ACR_LSF_SEC2)
			m->rtos_loaded = true;
	}
	m->acr_ok = true;
halt:
	m->mbox0 = m->acr_ok ? 0 : m->acr_err;
}

static void
rtos_start(struct mock *m)
{
	uint8_t im[PG106_SEC2_INIT_MSG_SIZE] = { 0 };

	if (!m->rtos_loaded)
		return; // nothing in IMEM: the CPU halts again immediately
	m->run_state = 3;
	m->irqdest = 0x40; // SIM: SWGEN0 routed to the host
	m->rtos_cmdq_tail = Q_CMDQ_OFF;
	reg_set(m, SEC2_BASE + PG106_SEC2_CMDQ_HEAD, Q_CMDQ_OFF);
	reg_set(m, SEC2_BASE + PG106_SEC2_CMDQ_TAIL, Q_CMDQ_OFF);
	// Init message sits at the msgq tail the host reads first
	// (msgq.c:161-166).
	m->rtos_msgq_head = Q_MSGQ_OFF;
	reg_set(m, SEC2_BASE + PG106_SEC2_MSGQ_TAIL, Q_MSGQ_OFF);
	reg_set(m, SEC2_BASE + PG106_SEC2_MSGQ_HEAD, Q_MSGQ_OFF);
	im[0] = PG106_SEC2_UNIT_INIT;
	im[1] = PG106_SEC2_INIT_MSG_SIZE;
	im[4] = PG106_SEC2_INIT_MSG_INIT;
	im[5] = 2;
	wr32le(im, 8, Q_CMDQ_OFF);
	im[12] = Q_CMDQ_SIZE;
	im[14] = 0;
	im[15] = PG106_SEC2_QUEUE_ID_CMDQ;
	// the msgq entry starts AFTER the init message itself
	wr32le(im, 16, Q_MSGQ_OFF);
	im[20] = Q_MSGQ_SIZE;
	im[22] = 0;
	im[23] = PG106_SEC2_QUEUE_ID_MSGQ;
	rtos_post(m, im, sizeof(im));
}

// toy RTOS: execute commands between its tail and the new head.
static void
rtos_cmds(struct mock *m, uint32_t head)
{
	while (m->rtos_cmdq_tail != head) {
		const uint8_t *c =
			&m->emem[m->rtos_cmdq_tail - PG106_SEC2_EMEM_ADDR];
		uint8_t unit = c[0], size = c[1];

		if (unit == PG106_FLCN_UNIT_ID_REWIND) {
			m->nrewinds++;
			m->rtos_cmdq_tail = Q_CMDQ_OFF;
			continue;
		}
		if (unit == PG106_SEC2_UNIT_ACR && size == 16 &&
		    c[4] == PG106_SEC2_ACR_CMD_BOOTSTRAP_FALCON) {
			uint8_t r[PG106_SEC2_ACR_BOOTSTRAP_MSG_SIZE] = { 0 };
			uint32_t id = rd32le(c, 12);
			uint32_t err = id == m->fail_falcon ? 1u : 0u;

			m->nboot_cmds++;
			if (!err && rd32le(c, 8) ==
					    PG106_SEC2_ACR_BOOTSTRAP_FALCON_FLAGS_RESET_YES)
				m->booted_mask |= 1ull << id;
			r[0] = m->rtos_wrong_unit ? 0x07 : PG106_SEC2_UNIT_ACR;
			r[1] = sizeof(r);
			r[3] = m->rtos_wrong_unit ? 0x0f : c[3]; // seq_id
			wr32le(r, 8, err);
			wr32le(r, 12, id);
			rtos_post(m, r, sizeof(r));
		}
		m->rtos_cmdq_tail += (size + 3u) & ~3u;
	}
	reg_set(m, SEC2_BASE + PG106_SEC2_CMDQ_TAIL, m->rtos_cmdq_tail);
}

static uint32_t
mock_rd32(void *ctx, uint32_t addr)
{
	struct mock *m = ctx;
	uint32_t off = addr - SEC2_BASE, v;
	uint8_t *p;

	if (addr == (uint32_t)PG106_PMC_ENABLE)
		return m->pmc_enable;
	if (addr == (uint32_t)PG106_PMC_BOOT_0)
		return BOOT0_GP106_A1;
	if (addr == (uint32_t)PG106_PFB_WPR_CHECK) {
		// gm200_acr_wpr_check decode (acr/gm200.c:57-61): window
		// register = addr >> 8 in [31:8]; limit is window + 0x20000.
		if (m->wpr_sel == 2)
			return (uint32_t)(m->wpr_win_start >> 8) & 0xffffff00u;
		if (m->wpr_sel == 3)
			return (uint32_t)((m->wpr_win_end - 0x20000ull) >> 8) &
			       0xffffff00u;
		return 0;
	}
	if (addr < SEC2_BASE || addr >= SEC2_BASE + 0x1000)
		return m->gpu ? mg_rd32(m->gpu, addr) : reg_get(m, addr);
	if (!sec2_powered(m))
		return 0;

	switch (off) {
	case PG106_FALCON_MEM_SCRUB:
		if (m->scrub_left > 0) {
			m->scrub_left--;
			return 0x6;
		}
		return 0;
	case PG106_FALCON_INTR_STAT:
		if (m->bind_phase == 1 && ++m->bind_reads >= 2)
			m->intr_stat |= 0x8; // bind intr after a short while
		return m->intr_stat;
	case PG106_FALCON_BINDSTAT:
		if (m->bind_phase == 1)
			return (m->intr_stat & 0x8) ? 0x5000 : 0x1000;
		if (m->bind_phase == 2)
			return 0x5000;
		return 0;
	case PG106_FALCON_CPUCTL:
		// SIM: ALIAS_EN (bit 6) shows up once the HS ucode has run.
		v = (m->alias_en && m->run_state >= 2) ? 0x40u : 0u;
		if (m->run_state == 1 && ++m->halt_reads >= HALT_READS) {
			m->run_state = 2;
			m->intr_stat |= 0x10;
		}
		if (m->run_state == 2 || m->run_state == 0)
			v |= 0x10; // HALTED (STARTCPU never reads back)
		return v;
	case PG106_FALCON_MBOX0:
		return m->mbox0;
	case PG106_FALCON_MBOX1:
		return m->mbox1;
	case PG106_FALCON_IRQDEST:
		return m->irqdest;
	case PG106_FALCON_MEM_CAPS:
		return SEC2_HWCFG_SIM;
	case PG106_FALCON_DMEMD:
	case PG106_FALCON_EMEMD: {
		uint32_t ctl = off == PG106_FALCON_DMEMD ? PG106_FALCON_DMEMC
						      : PG106_FALCON_EMEMC;
		uint32_t *c = ctl == PG106_FALCON_DMEMC ? &m->dmemc : &m->ememc;

		p = mem_at(m, ctl, *c & 0xffffu);
		v = p ? rd32le(p, 0) : 0;
		if (*c & 0x02000000u)
			*c = (*c & 0xffff0000u) | ((*c + 4) & 0xffffu);
		return v;
	}
	}
	return reg_get(m, addr);
}

static void
mock_wr32(void *ctx, uint32_t addr, uint32_t v)
{
	struct mock *m = ctx;
	uint32_t off = addr - SEC2_BASE, old;
	uint8_t *p;

	if (addr == (uint32_t)PG106_PMC_ENABLE) {
		bool was = sec2_powered(m);

		m->pmc_enable = v;
		if (!was && sec2_powered(m))
			falcon_reset_state(m); // engine comes out of reset
		return;
	}
	if (addr == (uint32_t)PG106_PFB_WPR_CHECK) {
		m->wpr_sel = v;
		return;
	}
	if (addr < SEC2_BASE || addr >= SEC2_BASE + 0x1000) {
		if (m->gpu)
			mg_wr32(m->gpu, addr, v);
		else
			reg_set(m, addr, v);
		return;
	}
	if (!sec2_powered(m)) {
		m->writes_while_gated++;
		return;
	}
	old = reg_get(m, addr);
	switch (off) {
	case PG106_FALCON_GP102_RESET:
		if ((old & 1) && !(v & 1))
			falcon_reset_state(m);
		break;
	case PG106_FALCON_INTR:
		m->intr_stat &= ~v;
		if (v & 0x8) // bind intr acked: state stays 5 until trigger
			m->bind_phase = m->bind_phase == 1 ? 2 : m->bind_phase;
		break;
	case PG106_FALCON_MEMBIND_CTL2:
		if ((v & 0x8) && (reg_get(m, SEC2_BASE +
					     PG106_FALCON_MEMBIND_CTL) &
				  0x10000)) {
			m->bind_phase = 1;
			m->bind_reads = 0;
		}
		break;
	case PG106_FALCON_BIND_TRIGGER:
		if ((v & 0x2) && m->bind_phase == 2)
			m->bind_phase = 0; // bind_stat -> 0
		break;
	case PG106_FALCON_IMEMC:
		m->imemc = v;
		if (m->n_imemc < 8)
			m->imemc_vals[m->n_imemc] = v;
		m->n_imemc++;
		break;
	case PG106_FALCON_IMEMT:
		if ((m->imemc & 0xffffu) < IMEM_SIZE)
			m->imem_tag[(m->imemc & 0xffffu) >> 8] = (uint16_t)v;
		break;
	case PG106_FALCON_IMEMD:
		if (m->scrub_left > 0)
			m->pio_during_scrub++;
		if ((m->imemc & 0xffffu) + 4 <= IMEM_SIZE)
			wr32le(m->imem, m->imemc & 0xffffu, v);
		m->imemc = (m->imemc & 0xffff0000u) |
			   ((m->imemc + 4) & 0xffffu);
		m->imem_bytes += 4;
		break;
	case PG106_FALCON_DMEMC:
		m->dmemc = v;
		if (m->n_dmemc < 8)
			m->dmemc_vals[m->n_dmemc] = v;
		m->n_dmemc++;
		break;
	case PG106_FALCON_EMEMC:
		m->ememc = v;
		break;
	case PG106_FALCON_DMEMD:
	case PG106_FALCON_EMEMD: {
		uint32_t ctl = off == PG106_FALCON_DMEMD ? PG106_FALCON_DMEMC
						      : PG106_FALCON_EMEMC;
		uint32_t *c = ctl == PG106_FALCON_DMEMC ? &m->dmemc : &m->ememc;

		if (m->scrub_left > 0)
			m->pio_during_scrub++;
		p = mem_at(m, ctl, *c & 0xffffu);
		if (p)
			wr32le(p, 0, v);
		if (*c & 0x01000000u)
			*c = (*c & 0xffff0000u) | ((*c + 4) & 0xffffu);
		if (ctl == PG106_FALCON_DMEMC)
			m->dmem_bytes += 4;
		break;
	}
	case PG106_FALCON_MBOX0:
		m->mbox0 = v;
		break;
	case PG106_FALCON_MBOX1:
		m->mbox1 = v;
		break;
	case PG106_FALCON_BOOTVEC:
		m->bootvec = v;
		break;
	case PG106_FALCON_CPUCTL:
	case PG106_FALCON_CPUCTL_ALIAS:
		// gm200_flcn_fw_boot starts the HS ucode through 0x100
		// (gm200.c:229); afterwards, with ALIAS_EN set, only the alias
		// restarts the CPU (v1.c:95-100).
		if (!(v & 0x2))
			break;
		if (m->run_state == 0 && off != PG106_FALCON_CPUCTL)
			break;
		if (m->run_state == 2 &&
		    m->alias_en != (off == PG106_FALCON_CPUCTL_ALIAS))
			break;
		if (m->run_state == 0) {
			m->run_state = 1;
			m->halt_reads = 0;
			acr_run(m);
		} else if (m->run_state == 2) {
			rtos_start(m);
		}
		return; // STARTCPU is write-only
	case PG106_SEC2_CMDQ_HEAD:
		reg_set(m, addr, v);
		if (m->run_state == 3)
			rtos_cmds(m, v);
		return;
	}
	reg_set(m, addr, v);
}

static void
mock_init(struct mock *m, const struct fixture *fx,
	  const struct pg106_acrboot_info *info, uint32_t pmc_mask)
{
	memset(m, 0, sizeof(*m));
	m->fx = fx;
	m->pmc_mask = pmc_mask;
	m->pmc_enable = 0x40003120u | pmc_mask; // FINDINGS.md:51 initial
	m->fail_falcon = UINT32_MAX;
	falcon_reset_state(m);
	m->scrub_left = 0;
	m->img_size = fx->hs.img_size;
	m->img = malloc(m->img_size);
	memcpy(m->img, fx->hs.img, m->img_size);
	m->wpr_size = info->alloc_size / 2;
	m->wpr = calloc(1, m->wpr_size);
}

static void
mock_free(struct mock *m)
{
	free(m->img);
	free(m->wpr);
}

// --- plan replayer --------------------------------------------------------------

struct replay {
	size_t poll_timeouts, read_mismatches, oob;
};

static void
obj_store(struct mock *m, struct replay *r, uint8_t space, uint64_t off,
	  const uint8_t *src, uint32_t len)
{
	uint8_t *dst = space == PG106_ACRBOOT_WPR ? m->wpr : m->img;
	size_t cap = space == PG106_ACRBOOT_WPR ? m->wpr_size : m->img_size;

	if (off + len > cap) {
		r->oob++;
		return;
	}
	if (src)
		memcpy(dst + off, src, len);
	else
		memset(dst + off, 0, len);
}

static struct replay
replay_plan(struct mock *m, const struct pg106_acrboot_plan *plan)
{
	struct replay r = { 0 };
	size_t i;

	for (i = 0; i < plan->nsteps; i++) {
		const struct pg106_acrboot_step *s = &plan->steps[i];
		uint32_t v, n, tries;
		uint8_t w[4];

		switch (s->kind) {
		case PG106_ACRBOOT_WRITE_REG:
			if (s->space == PG106_ACRBOOT_MMIO) {
				mock_wr32(m, (uint32_t)s->addr, s->value);
			} else {
				wr32le(w, 0, s->value);
				obj_store(m, &r, s->space, s->addr, w, 4);
			}
			break;
		case PG106_ACRBOOT_WRITE_BLOB:
			obj_store(m, &r, s->space, s->addr, s->blob,
				  s->blob_len);
			break;
		case PG106_ACRBOOT_MASK_REG:
			v = mock_rd32(m, (uint32_t)s->addr);
			mock_wr32(m, (uint32_t)s->addr,
				  (v & ~s->mask) | s->value);
			break;
		case PG106_ACRBOOT_DELAY_US:
			m->time_us += s->value;
			break;
		case PG106_ACRBOOT_POLL_REG:
			// one read per 10 us of budget (SIM clock)
			tries = s->timeout_us / 10 ? s->timeout_us / 10 : 1;
			for (n = 0; n < tries; n++) {
				bool ok;

				v = mock_rd32(m, (uint32_t)s->addr) & s->mask;
				ok = s->poll_op == PG106_ACRBOOT_POLL_EQ
					     ? v == s->value
					     : v != 0;
				m->time_us += 10;
				if (ok)
					break;
			}
			if (n == tries)
				r.poll_timeouts++;
			break;
		case PG106_ACRBOOT_READ_REG:
			v = mock_rd32(m, (uint32_t)s->addr);
			if ((v & s->mask) != (s->value & s->mask))
				r.read_mismatches++;
			break;
		}
	}
	return r;
}

// --- shared plan ------------------------------------------------------------------

static struct pg106_acrboot_plan g_plan;
static struct pg106_acrboot_info g_info;

static int
build(struct fixture *fx, const struct pg106_acrboot_config *cfg,
      struct pg106_acrboot_plan *plan, struct pg106_acrboot_info *info)
{
	return pg106_acrboot_plan_build(cfg, &fx->bl, &fx->hs, fx->lsfw, 3,
					fx->sigs, plan, info);
}

static void
dump_plan(const struct pg106_acrboot_plan *p)
{
	size_t i;

	for (i = 0; i < p->nsteps; i++) {
		const struct pg106_acrboot_step *s = &p->steps[i];

		// IMEMD/DMEMD data words are summarised, not dumped one by one.
		if (s->kind == PG106_ACRBOOT_WRITE_REG &&
		    s->space == PG106_ACRBOOT_MMIO &&
		    (s->addr == SEC2_BASE + PG106_FALCON_IMEMD ||
		     s->addr == SEC2_BASE + PG106_FALCON_DMEMD))
			continue;
		printf("[%4zu] %-10s %-4s 0x%09llx", i,
		       pg106_acrboot_kind_name(s->kind),
		       pg106_acrboot_space_name(s->space),
		       (unsigned long long)s->addr);
		if (s->kind == PG106_ACRBOOT_WRITE_REG)
			printf(" = 0x%08x", s->value);
		if (s->kind == PG106_ACRBOOT_MASK_REG)
			printf(" &~0x%08x |0x%08x", s->mask, s->value);
		if (s->kind == PG106_ACRBOOT_DELAY_US)
			printf(" %u us", s->value);
		if (s->kind == PG106_ACRBOOT_WRITE_BLOB)
			printf(" len=%u%s", s->blob_len,
			       s->blob ? "" : " (zero-fill)");
		if (s->kind == PG106_ACRBOOT_POLL_REG)
			printf(" mask=0x%08x %s 0x%08x (%u us)", s->mask,
			       pg106_acrboot_poll_op_name(s->poll_op),
			       s->value, s->timeout_us);
		if (s->kind == PG106_ACRBOOT_READ_REG)
			printf(" expect (v&0x%08x)==0x%08x", s->mask,
			       s->value);
		printf(" | %s\n", s->why);
	}
}

// --- full boot: plan replay + SEC2 init message + BOOTSTRAP_FALCON ---------------

static struct pg106_flcn_bus
mock_bus(struct mock *m)
{
	return (struct pg106_flcn_bus){ .ctx = m, .rd32 = mock_rd32,
					.wr32 = mock_wr32 };
}

static void
test_full_boot(struct fixture *fx)
{
	struct mock *m = malloc(sizeof(*m));
	struct pg106_flcn_bus bus;
	struct pg106_sec2 sec2;
	struct replay r;
	int ret;

	printf("\n== SIMULATION, NOT HARDWARE PROOF: ACR HS boot -> SEC2 RTOS "
	       "-> BOOTSTRAP_FALCON(FECS, GPCCS) on the mock ==\n");
	ret = build(fx, &fx->cfg, &g_plan, &g_info);
	CHECK_EQ(ret, 0);
	if (ret) {
		free(m);
		return;
	}
	printf("plan: %zu steps, wpr_size 0x%x, alloc 0x%llx (shadow 0x%llx, "
	       "wpr 0x%llx-0x%llx), code.limit 0x%x data.limit 0x%x\n",
	       g_plan.nsteps, g_info.wpr_size,
	       (unsigned long long)g_info.alloc_size,
	       (unsigned long long)g_info.shadow_start,
	       (unsigned long long)g_info.wpr_start,
	       (unsigned long long)g_info.wpr_end, g_info.code_limit,
	       g_info.data_limit);
	dump_plan(&g_plan);
	CHECK_EQ(g_info.code_limit, 0x10000); // 64 KB SEC2 IMEM
	CHECK_EQ(g_info.data_limit, 0x10000);

	mock_init(m, fx, &g_info, fx->cfg.pmc_mask);
	r = replay_plan(m, &g_plan);
	CHECK_EQ(r.poll_timeouts, 0);
	CHECK_EQ(r.read_mismatches, 0);
	CHECK_EQ(r.oob, 0);
	CHECK_EQ(m->pio_during_scrub, 0);
	// gm200_flcn_disable runs reset_eng (0x3c0, 0x040, 0x10c) AFTER
	// nvkm_mc_disable (gm200.c:205-212), so nouveau itself touches the
	// falcon while its PMC bit is clear. The mock drops those writes;
	// what silicon does with them is a hardware-only unknown.
	printf("falcon writes while PMC-gated (upstream order): %u\n",
	       m->writes_while_gated);
	CHECK(m->acr_ok);
	CHECK_EQ(m->acr_err, 0);
	CHECK(m->rtos_loaded);
	CHECK_EQ(m->run_state, 3); // SEC2 RTOS started by the plan's last step

	// Load group = BL + 76-byte args, nothing else (gm200.c:289-297).
	CHECK_EQ(m->n_imemc, 1);
	CHECK_EQ(m->imemc_vals[0], 0x01000000u | (0x10000u - fx->bl.boot_size));
	CHECK_EQ(m->imem_bytes, fx->bl.boot_size);
	CHECK_EQ(m->n_dmemc, 1);
	CHECK_EQ(m->dmemc_vals[0], 0x01000000u);
	CHECK_EQ(m->dmem_bytes, PG106_FLCN_BL_DMEM_DESC_V1_SIZE);
	printf("load: IMEM %u B (BL at 0x%x, tag 0x%x), DMEM %u B (BL args "
	       "at 0) — no HS image PIO\n",
	       m->imem_bytes, 0x10000u - fx->bl.boot_size,
	       (unsigned)(fx->bl.boot_addr >> 8), m->dmem_bytes);

	// SEC2 RTOS: init message, then nvkm_acr_bootstrap_falcons(FECS|GPCCS)
	// as gf100_gr_init_ctxctl_ext asks (gr/gf100.c:1761-1781).
	bus = mock_bus(m);
	pg106_sec2_ctx_init(&sec2, &bus, SEC2_BASE, 64);
	ret = pg106_acr_bootstrap_falcons(&sec2,
					  (1ull << PG106_ACR_LSF_FECS) |
						  (1ull << PG106_ACR_LSF_GPCCS));
	CHECK_EQ(ret, 0);
	CHECK_EQ(sec2.initmsg, 1);
	CHECK_EQ(sec2.cmdq.offset, Q_CMDQ_OFF);
	CHECK_EQ(sec2.cmdq.head_reg, PG106_SEC2_CMDQ_HEAD);
	CHECK_EQ(sec2.msgq.offset, Q_MSGQ_OFF);
	CHECK_EQ(m->nboot_cmds, 2);
	CHECK_EQ(m->booted_mask, (1ull << PG106_ACR_LSF_FECS) |
					 (1ull << PG106_ACR_LSF_GPCCS));
	CHECK_EQ(sec2.unhandled_intr, 0);
	printf("SEC2: init msg parsed (cmdq 0x%08x/%u, msgq 0x%08x), "
	       "BOOTSTRAP_FALCON x%u -> booted mask 0x%llx\n",
	       sec2.cmdq.offset, sec2.cmdq.size, sec2.msgq.offset,
	       m->nboot_cmds, (unsigned long long)m->booted_mask);

	// Many more commands wrap both rings: cmdq REWIND (cmdq.c:58-68) and
	// the msgq `head < position` restart (msgq.c:57-60).
	{
		int k, bad = 0;

		for (k = 0; k < 12; k++)
			bad |= pg106_sec2_bootstrap_falcon(&sec2,
							   PG106_ACR_LSF_FECS);
		CHECK_EQ(bad, 0);
		CHECK(m->nrewinds >= 1);
		printf("ring wrap: 12 more commands, %u cmdq rewind(s)\n",
		       m->nrewinds);
	}

	// The RTOS reporting an error_code fails the call (sec2/gp102.c:47-52).
	m->fail_falcon = PG106_ACR_LSF_GPCCS;
	CHECK_EQ(pg106_sec2_bootstrap_falcon(&sec2, PG106_ACR_LSF_GPCCS),
		 -EINVAL);
	m->fail_falcon = UINT32_MAX;
	// Only FECS|GPCCS|SEC2 can be bootstrapped (acr/base.c:156-157).
	CHECK_EQ(pg106_acr_bootstrap_falcons(&sec2,
					     1ull << PG106_ACR_LSF_PMU),
		 -ENOSYS);
	// A reply for an unknown sequence is dropped -> the caller times out.
	m->rtos_wrong_unit = true;
	CHECK_EQ(pg106_sec2_bootstrap_falcon(&sec2, PG106_ACR_LSF_FECS),
		 -ETIMEDOUT);
	m->rtos_wrong_unit = false;

	mock_free(m);
	free(m);
}


// --- the shared executor (core/src/hwexec.c) on the same mock --------------------
//
// The kext runs plans through pg106_hwexec_acr, not through replay_plan
// above. Same mock, same expectations, plus the failure contract: a step
// that times out stops the run and nothing after it reaches the bus.

struct xbus {
	struct mock *m;
	uint32_t stuck_addr; // reads of this address always return stuck_val
	uint32_t stuck_val;
	size_t writes, writes_after_stuck;
	bool stuck_seen;
	size_t oob;
};

static uint32_t
x_rd32(void *u, uint32_t a)
{
	struct xbus *x = u;

	if (x->stuck_addr && a == x->stuck_addr) {
		x->stuck_seen = true;
		return x->stuck_val;
	}
	return mock_rd32(x->m, a);
}

static void
x_wr32(void *u, uint32_t a, uint32_t v)
{
	struct xbus *x = u;

	x->writes++;
	if (x->stuck_seen)
		x->writes_after_stuck++;
	mock_wr32(x->m, a, v);
}

static void
x_delay(void *u, uint32_t us)
{
	((struct xbus *)u)->m->time_us += us;
}

static void
x_delay_m(void *u, uint32_t us)
{
	((struct mock *)u)->time_us += us;
}

static int
x_mem(void *u, uint8_t space, uint64_t off, const uint8_t *src, uint32_t len)
{
	struct xbus *x = u;
	struct replay r = { 0 };

	obj_store(x->m, &r, space, off, src, len);
	x->oob += r.oob;
	return r.oob ? -EFAULT : 0;
}

static void
test_hwexec(struct fixture *fx)
{
	struct mock *m = malloc(sizeof(*m));
	struct xbus x = { 0 };
	struct pg106_hw_bus bus = { &x, x_rd32, x_wr32, x_delay, x_mem, 0 };
	struct pg106_hw_result res;
	struct pg106_flcn_bus fb;
	struct pg106_sec2 sec2;

	printf("\n== SIMULATION: ACR plan through pg106_hwexec_acr (the kext's "
	       "executor) on the mock ==\n");
	if (!g_plan.nsteps) {
		CHECK(0);
		free(m);
		return;
	}
	mock_init(m, fx, &g_info, fx->cfg.pmc_mask);
	x.m = m;
	CHECK_EQ(pg106_hwexec_acr(&bus, &g_plan, &res), 0);
	CHECK_EQ(res.step, g_plan.nsteps);
	CHECK_EQ(x.oob, 0);
	CHECK(m->acr_ok);
	CHECK_EQ(m->acr_err, 0);
	CHECK(m->rtos_loaded);
	CHECK_EQ(m->run_state, 3);
	CHECK(res.polls > 0);
	fb = pg106_hw_flcn_bus(&bus);
	pg106_sec2_ctx_init(&sec2, &fb, SEC2_BASE, 64);
	CHECK_EQ(pg106_acr_bootstrap_falcons(&sec2,
					     (1ull << PG106_ACR_LSF_FECS) |
						     (1ull << PG106_ACR_LSF_GPCCS)),
		 0);
	CHECK_EQ(m->booted_mask, (1ull << PG106_ACR_LSF_FECS) |
					 (1ull << PG106_ACR_LSF_GPCCS));
	printf("executor: %zu steps, %zu poll reads, %llu us waited; SEC2 "
	       "BOOTSTRAP_FALCON(FECS|GPCCS) over the adapter bus OK\n",
	       res.step, res.polls, (unsigned long long)res.waited_us);
	mock_free(m);

	// Scrub never finishes: the first 0x10c poll times out after its
	// budget (gm200.c:145-147, 10 ms), and the run stops there.
	mock_init(m, fx, &g_info, fx->cfg.pmc_mask);
	memset(&x, 0, sizeof(x));
	x.m = m;
	x.stuck_addr = SEC2_BASE + PG106_FALCON_MEM_SCRUB;
	x.stuck_val = 0x6;
	CHECK_EQ(pg106_hwexec_acr(&bus, &g_plan, &res), -ETIMEDOUT);
	CHECK_EQ(res.kind, PG106_ACRBOOT_POLL_REG);
	CHECK_EQ(res.addr, SEC2_BASE + PG106_FALCON_MEM_SCRUB);
	CHECK_EQ(res.got, 0x6);
	CHECK(res.why && res.why[0]);
	CHECK_EQ(x.writes_after_stuck, 0);
	CHECK(res.waited_us >= g_plan.steps[res.step].timeout_us);
	CHECK(!m->acr_ok);
	printf("timeout: step %zu (%s) -ETIMEDOUT after %llu us, 0 writes after\n",
	       res.step, res.why, (unsigned long long)res.waited_us);
	mock_free(m);

	// A wrong HWCFG read-back (config does not match the chip) stops the
	// run at the oneinit READ_REG, before any write.
	mock_init(m, fx, &g_info, fx->cfg.pmc_mask);
	memset(&x, 0, sizeof(x));
	x.m = m;
	x.stuck_addr = SEC2_BASE + PG106_FALCON_MEM_CAPS;
	x.stuck_val = 0x20202060u; // FECS-sized falcon
	CHECK_EQ(pg106_hwexec_acr(&bus, &g_plan, &res), -EIO);
	CHECK_EQ(res.kind, PG106_ACRBOOT_READ_REG);
	CHECK_EQ(x.writes, 0);
	mock_free(m);

	// No memory-object writer: refused at the first WPR step.
	mock_init(m, fx, &g_info, fx->cfg.pmc_mask);
	memset(&x, 0, sizeof(x));
	x.m = m;
	bus.mem_write = NULL;
	CHECK_EQ(pg106_hwexec_acr(&bus, &g_plan, &res), -ENOSYS);
	CHECK(g_plan.steps[res.step].space != PG106_ACRBOOT_MMIO);
	bus.mem_write = x_mem;
	CHECK_EQ(pg106_hwexec_acr(NULL, &g_plan, &res), -EINVAL);
	mock_free(m);

	// The kext's sequence (devboot) up to the ACR stage on the same mock:
	// identify, posted, PTOP (empty here -> measured SEC2 fallback, no
	// reset line), then the plan built from the chip's own read-backs.
	{
		struct pg106_devboot_fw fw;
		struct pg106_devboot_cfg c;
		struct pg106_devboot db;

		memset(&fw, 0, sizeof(fw));
		fw.acr_bl = (struct pg106_fw_blob){ fx->bl_blob, fx->bl_size };
		fw.acr_ucode_load = (struct pg106_fw_blob){ fx->hs_blob, fx->hs_size };
		fw.fecs_bl = (struct pg106_fw_blob){ fx->fecs_bl, fx->fecs_bl_size };
		fw.fecs_inst = (struct pg106_fw_blob){ fx->fecs_inst, fx->fecs_inst_size };
		fw.fecs_data = (struct pg106_fw_blob){ fx->fecs_data, fx->fecs_data_size };
		fw.fecs_sig = (struct pg106_fw_blob){ fx->fecs_sig_blob, fx->fecs_sig_size };
		fw.gpccs_bl = (struct pg106_fw_blob){ fx->gpccs_bl, fx->gpccs_bl_size };
		fw.gpccs_inst = (struct pg106_fw_blob){ fx->gpccs_inst, fx->gpccs_inst_size };
		fw.gpccs_data = (struct pg106_fw_blob){ fx->gpccs_data, fx->gpccs_data_size };
		fw.gpccs_sig = (struct pg106_fw_blob){ fx->gpccs_sig_blob, fx->gpccs_sig_size };
		fw.sec2_desc = (struct pg106_fw_blob){ fx->sec2_desc, fx->sec2_desc_size };
		fw.sec2_image = (struct pg106_fw_blob){ fx->sec2_img, fx->sec2_img_size };
		fw.sec2_sig = (struct pg106_fw_blob){ fx->sec2_sig_blob, fx->sec2_sig_size };

		mock_init(m, fx, &g_info, 0);
		reg_set(m, PG106_DEVINIT_POSTED_REG, PG106_DEVINIT_POSTED_BIT);
		memset(&x, 0, sizeof(x));
		x.m = m;
		memset(&c, 0, sizeof(c));
		c.bus = &bus;
		c.fw = &fw;
		c.mem = (struct pg106_devboot_mem){ ACR_VMA, WPR_ALLOC_BASE, 0,
						    fx->cfg.bind_addr };
		c.stop_after = PG106_DB_ACR;
		CHECK_EQ(pg106_devboot_run(&db, &c), 0);
		CHECK_EQ(db.stage, PG106_DB_ACR);
		CHECK_EQ(db.sec2_base, SEC2_BASE);
		CHECK(m->acr_ok);
		CHECK(m->rtos_loaded);
		CHECK_EQ(m->run_state, 3);
		CHECK_EQ(x.oob, 0);
		CHECK_EQ(pg106_acr_bootstrap_falcons(&sec2,
						     (1ull << PG106_ACR_LSF_FECS) |
							     (1ull << PG106_ACR_LSF_GPCCS)),
			 0);
		printf("devboot identify -> posted -> top -> ACR on the mock: "
		       "%zu plan steps, RTOS running, BOOTSTRAP_FALCON OK\n",
		       db.acr_plan.nsteps);
		pg106_devboot_fini(&db);
		mock_free(m);
	}
	free(m);
	printf("PASS executor failure contract: timeout, read-back mismatch, "
	       "missing mem_write, bad args; devboot ACR stage end to end\n");
}

// --- mutation tests: each fixed bug is caught by the mock -------------------------

static void
copy_plan(struct pg106_acrboot_plan *dst, const struct pg106_acrboot_plan *src,
	  size_t extra)
{
	dst->steps = malloc((src->nsteps + extra) * sizeof(*dst->steps));
	memcpy(dst->steps, src->steps, src->nsteps * sizeof(*dst->steps));
	dst->nsteps = src->nsteps;
	dst->scratch_ = NULL;
	dst->nscratch_ = 0;
}

static struct replay
run_on_fresh_mock(struct fixture *fx, const struct pg106_acrboot_plan *p,
		  struct mock *m)
{
	mock_init(m, fx, &g_info, fx->cfg.pmc_mask);
	return replay_plan(m, p);
}

static void
test_mutations(struct fixture *fx)
{
	struct mock *m = malloc(sizeof(*m));
	struct pg106_acrboot_plan mp;
	struct replay r;
	size_t i, n;

	printf("\n== mutation tests (each reverts one fixed bug; the mock must "
	       "notice) ==\n");

	// (2) scrub poll inverted: POLL_ANY on 0x10c stops while BUSY.
	copy_plan(&mp, &g_plan, 0);
	for (i = n = 0; i < mp.nsteps; i++)
		if (mp.steps[i].kind == PG106_ACRBOOT_POLL_REG &&
		    mp.steps[i].addr == SEC2_BASE + PG106_FALCON_MEM_SCRUB) {
			mp.steps[i].poll_op = PG106_ACRBOOT_POLL_ANY;
			n++;
		}
	CHECK(n >= 4);
	r = run_on_fresh_mock(fx, &mp, m);
	printf("scrub POLL_ANY mutant: pio_during_scrub=%u\n",
	       m->pio_during_scrub);
	CHECK(m->pio_during_scrub > 0);
	(void)r;
	mock_free(m);
	free(mp.steps);

	// (3) load group doing the no-boot branch too: append the HS data
	// segment DMEM PIO at 0 right after the BL args (the old model) —
	// it overwrites the BL descriptor and the toy ACR refuses to run.
	{
		size_t last_dmemd = 0;
		uint32_t w;

		for (i = 0; i < g_plan.nsteps; i++)
			if (g_plan.steps[i].addr ==
			    SEC2_BASE + PG106_FALCON_DMEMD)
				last_dmemd = i;
		copy_plan(&mp, &g_plan, 1 + fx->hs.dmem_size / 4);
		memmove(&mp.steps[last_dmemd + 2 + fx->hs.dmem_size / 4],
			&mp.steps[last_dmemd + 1],
			(mp.nsteps - last_dmemd - 1) * sizeof(*mp.steps));
		mp.steps[last_dmemd + 1] = mp.steps[last_dmemd];
		mp.steps[last_dmemd + 1].addr = SEC2_BASE + PG106_FALCON_DMEMC;
		mp.steps[last_dmemd + 1].value = 0x01000000u;
		for (w = 0; w < fx->hs.dmem_size / 4; w++) {
			mp.steps[last_dmemd + 2 + w] = mp.steps[last_dmemd];
			mp.steps[last_dmemd + 2 + w].value =
				rd32le(fx->hs.img + fx->hs.dmem_base_img, w * 4);
		}
		mp.nsteps += 1 + fx->hs.dmem_size / 4;
		r = run_on_fresh_mock(fx, &mp, m);
		printf("image-DMEM-PIO mutant: acr_ok=%d err=0x%x "
		       "read_mismatches=%zu\n",
		       m->acr_ok, m->acr_err, r.read_mismatches);
		CHECK(!m->acr_ok);
		CHECK_EQ(m->acr_err, 0xb2);
		CHECK(r.read_mismatches > 0); // MBOX0 != boot_mbox0
		mock_free(m);
		free(mp.steps);
	}

	// (8) RMW as plain writes: 0x048 carries a pre-set bit 2 (SIM) that
	// nvkm_falcon_mask(0x048, 0x3, 0) preserves and a plain write loses.
	copy_plan(&mp, &g_plan, 0);
	mock_init(m, fx, &g_info, fx->cfg.pmc_mask);
	reg_set(m, SEC2_BASE + PG106_FALCON_RM, 0x4);
	r = replay_plan(m, &g_plan);
	CHECK_EQ(reg_get(m, SEC2_BASE + PG106_FALCON_RM) & 0x4, 0x4);
	mock_free(m);
	for (i = 0; i < mp.nsteps; i++)
		if (mp.steps[i].kind == PG106_ACRBOOT_MASK_REG)
			mp.steps[i].kind = PG106_ACRBOOT_WRITE_REG;
	mock_init(m, fx, &g_info, fx->cfg.pmc_mask);
	reg_set(m, SEC2_BASE + PG106_FALCON_RM, 0x4);
	r = replay_plan(m, &mp);
	printf("plain-write mutant: 0x048 = 0x%x (bit 2 lost), PMC_ENABLE "
	       "= 0x%08x\n",
	       reg_get(m, SEC2_BASE + PG106_FALCON_RM), m->pmc_enable);
	CHECK_EQ(reg_get(m, SEC2_BASE + PG106_FALCON_RM) & 0x4, 0);
	// ...and PMC_ENABLE would be clobbered to just the SEC2 bit.
	CHECK_EQ(m->pmc_enable, fx->cfg.pmc_mask);
	mock_free(m);
	free(mp.steps);

	// (9) no PMC reset at all: the falcon never leaves reset scrubbing
	// state on the mc_enable edge, so dropping the mc steps is visible
	// as a missing 0x200 toggle.
	{
		size_t clr = (size_t)-1, set = (size_t)-1;

		for (i = 0; i < g_plan.nsteps; i++) {
			const struct pg106_acrboot_step *s = &g_plan.steps[i];

			if (s->kind != PG106_ACRBOOT_MASK_REG ||
			    s->addr != PG106_PMC_ENABLE)
				continue;
			if (s->value == 0 && clr == (size_t)-1)
				clr = i;
			if (s->value == fx->cfg.pmc_mask && clr != (size_t)-1 &&
			    set == (size_t)-1)
				set = i;
		}
		CHECK(clr != (size_t)-1 && set != (size_t)-1 && clr < set);
	}

	// (1) rtos = GPCCS (the old test value): SEC2 gets lazy_bootstrap = 1,
	// the toy ACR does not load it, no init message, bootstrap times out.
	{
		struct pg106_acrboot_config cfg = fx->cfg;
		struct pg106_acrboot_plan p2;
		struct pg106_acrboot_info i2;
		struct pg106_flcn_bus bus;
		struct pg106_sec2 sec2;

		cfg.rtos_id = PG106_ACR_LSF_GPCCS;
		CHECK_EQ(build(fx, &cfg, &p2, &i2), 0);
		mock_init(m, fx, &i2, cfg.pmc_mask);
		r = replay_plan(m, &p2);
		bus = mock_bus(m);
		pg106_sec2_ctx_init(&sec2, &bus, SEC2_BASE, 16);
		CHECK(!m->rtos_loaded);
		CHECK_EQ(pg106_acr_bootstrap_falcons(
				 &sec2, (1ull << PG106_ACR_LSF_FECS) |
						(1ull << PG106_ACR_LSF_GPCCS)),
			 -ETIMEDOUT);
		printf("rtos=GPCCS mutant: rtos_loaded=%d, bootstrap -> "
		       "-ETIMEDOUT\n",
		       m->rtos_loaded);
		mock_free(m);
		pg106_acrboot_plan_free(&p2);
		// restore the layout offsets of the shared lsfws
		CHECK_EQ(build(fx, &fx->cfg, &p2, &i2), 0);
		pg106_acrboot_plan_free(&p2);
	}

	// wpr_patch missing: drop every patched-bld blob (the second write at
	// each offset.bld) -> the toy ACR sees WPR-relative DMA bases.
	{
		copy_plan(&mp, &g_plan, 0);
		for (i = n = 0; i < mp.nsteps; i++)
			if (mp.steps[i].space == PG106_ACRBOOT_WPR &&
			    strstr(mp.steps[i].why, "gp102_acr_wpr_patch")) {
				mp.steps[i].kind = PG106_ACRBOOT_DELAY_US;
				mp.steps[i].value = 0;
				n++;
			}
		CHECK_EQ(n, 3);
		r = run_on_fresh_mock(fx, &mp, m);
		CHECK(!m->acr_ok);
		CHECK_EQ(m->acr_err, 0xd3);
		mock_free(m);
		free(mp.steps);
	}

	// (9) debug-fused part: 0x408 bit 20 -> the debug signature is the
	// one patched (gm200.c:335-337) and the toy ACR accepts it.
	{
		struct pg106_acrboot_config cfg = fx->cfg;
		struct pg106_acrboot_plan p2;
		struct pg106_acrboot_info i2;

		cfg.debug_reg = PG106_FALCON_SEC2_DEBUG_DBG;
		CHECK_EQ(build(fx, &cfg, &p2, &i2), 0);
		CHECK(i2.debug_sig);
		mock_init(m, fx, &i2, cfg.pmc_mask);
		reg_set(m, SEC2_BASE + PG106_FALCON_SEC2_DEBUG,
			PG106_FALCON_SEC2_DEBUG_DBG);
		r = replay_plan(m, &p2);
		CHECK(m->acr_ok);
		CHECK_EQ(r.read_mismatches, 0);
		CHECK(memcmp(m->img + fx->hs.sig_base_img,
			     fx->hs_blob + fx->hs.sig_dbg_off,
			     fx->hs.sig_size) == 0);
		// the production plan on a debug part is refused
		mock_free(m);
		mock_init(m, fx, &g_info, fx->cfg.pmc_mask);
		reg_set(m, SEC2_BASE + PG106_FALCON_SEC2_DEBUG,
			PG106_FALCON_SEC2_DEBUG_DBG);
		r = replay_plan(m, &g_plan);
		CHECK(!m->acr_ok);
		CHECK(r.read_mismatches > 0);
		mock_free(m);
		pg106_acrboot_plan_free(&p2);
	}

	// v1_start with ALIAS_EN: the RTOS only starts through 0x130.
	{
		struct pg106_acrboot_config cfg = fx->cfg;
		struct pg106_acrboot_plan p2;
		struct pg106_acrboot_info i2;

		cfg.cpuctl_alias_en = true;
		CHECK_EQ(build(fx, &cfg, &p2, &i2), 0);
		mock_init(m, fx, &i2, cfg.pmc_mask);
		m->alias_en = true;
		r = replay_plan(m, &p2);
		CHECK_EQ(r.read_mismatches, 0);
		CHECK_EQ(r.poll_timeouts, 0);
		CHECK_EQ(m->run_state, 3);
		mock_free(m);
		pg106_acrboot_plan_free(&p2);
	}
	free(m);
}

// --- step-list structure ----------------------------------------------------------

static size_t
find(const struct pg106_acrboot_plan *p, size_t from, uint8_t kind,
     uint64_t addr)
{
	size_t i;

	for (i = from; i < p->nsteps; i++)
		if (p->steps[i].kind == kind && p->steps[i].addr == addr &&
		    p->steps[i].space == PG106_ACRBOOT_MMIO)
			return i;
	return (size_t)-1;
}

static void
test_structure(struct fixture *fx)
{
	const struct pg106_acrboot_plan *p = &g_plan;
	size_t hwcfg, wpr0 = (size_t)-1, dbg_rd, disable048, en084, imemc,
		bootvec, start, halt, mbox, check, sec2start, i;

	hwcfg = find(p, 0, PG106_ACRBOOT_READ_REG,
		     SEC2_BASE + PG106_FALCON_MEM_CAPS);
	for (i = 0; i < p->nsteps; i++)
		if (p->steps[i].space == PG106_ACRBOOT_WPR) {
			wpr0 = i;
			break;
		}
	dbg_rd = find(p, hwcfg + 1, PG106_ACRBOOT_READ_REG,
		      SEC2_BASE + PG106_FALCON_SEC2_DEBUG);
	dbg_rd = find(p, dbg_rd + 1, PG106_ACRBOOT_READ_REG,
		      SEC2_BASE + PG106_FALCON_SEC2_DEBUG);
	disable048 = find(p, 0, PG106_ACRBOOT_MASK_REG,
			  SEC2_BASE + PG106_FALCON_RM);
	en084 = find(p, disable048, PG106_ACRBOOT_WRITE_REG,
		     SEC2_BASE + PG106_FALCON_ENGINE_EN);
	imemc = find(p, 0, PG106_ACRBOOT_WRITE_REG,
		     SEC2_BASE + PG106_FALCON_IMEMC);
	bootvec = find(p, 0, PG106_ACRBOOT_WRITE_REG,
		       SEC2_BASE + PG106_FALCON_BOOTVEC);
	start = find(p, bootvec, PG106_ACRBOOT_WRITE_REG,
		     SEC2_BASE + PG106_FALCON_CPUCTL);
	halt = find(p, start, PG106_ACRBOOT_POLL_REG,
		    SEC2_BASE + PG106_FALCON_CPUCTL);
	mbox = find(p, halt, PG106_ACRBOOT_READ_REG,
		    SEC2_BASE + PG106_FALCON_MBOX0);
	check = find(p, 0, PG106_ACRBOOT_READ_REG, PG106_PFB_WPR_CHECK);
	sec2start = find(p, halt + 1, PG106_ACRBOOT_WRITE_REG,
			 SEC2_BASE + PG106_FALCON_CPUCTL);
	printf("\nstructure: hwcfg=%zu wpr0=%zu sigdbg=%zu disable=%zu "
	       "en084=%zu imemc=%zu bootvec=%zu start=%zu halt=%zu mbox=%zu "
	       "wprcheck=%zu sec2start=%zu\n",
	       hwcfg, wpr0, dbg_rd, disable048, en084, imemc, bootvec, start,
	       halt, mbox, check, sec2start);
	CHECK(hwcfg < wpr0 && wpr0 < dbg_rd && dbg_rd < disable048 &&
	      disable048 < en084 && en084 < imemc && imemc < bootvec &&
	      bootvec < start && start < halt && halt < mbox && mbox < check &&
	      check < sec2start);
	CHECK(sec2start == p->nsteps - 1);

	// Every falcon RMW upstream does is a MASK step (finding 8): 0x048,
	// 0x090, 0x0a4, 0x058, 0x604, 0x3c0, 0x004.
	{
		static const uint32_t rmw[] = {
			PG106_FALCON_RM, PG106_FALCON_MEMBIND_CTL,
			PG106_FALCON_MEMBIND_CTL2, PG106_FALCON_BIND_TRIGGER,
			PG106_FALCON_DMAIDX, PG106_FALCON_GP102_RESET,
			PG106_FALCON_INTR,
		};
		size_t k;

		for (k = 0; k < sizeof(rmw) / sizeof(rmw[0]); k++) {
			CHECK(find(p, 0, PG106_ACRBOOT_MASK_REG,
				   SEC2_BASE + rmw[k]) != (size_t)-1);
			CHECK(find(p, 0, PG106_ACRBOOT_WRITE_REG,
				   SEC2_BASE + rmw[k]) == (size_t)-1);
		}
	}

	// Scrub waits are EQ-0 polls with the 10 ms budget (finding 2).
	for (i = 0; i < p->nsteps; i++) {
		const struct pg106_acrboot_step *s = &p->steps[i];

		if (s->kind == PG106_ACRBOOT_POLL_REG &&
		    s->addr == SEC2_BASE + PG106_FALCON_MEM_SCRUB) {
			CHECK_EQ(s->poll_op, PG106_ACRBOOT_POLL_EQ);
			CHECK_EQ(s->value, 0);
			CHECK_EQ(s->mask, 6);
			CHECK_EQ(s->timeout_us, 10000);
		}
	}

	// WPR shadow: real images, rtos SEC2 not lazy, FECS/GPCCS lazy.
	{
		size_t nh = 0, nimg = 0;

		for (i = 0; i < p->nsteps; i++) {
			const struct pg106_acrboot_step *s = &p->steps[i];

			if (s->space != PG106_ACRBOOT_WPR ||
			    s->kind != PG106_ACRBOOT_WRITE_BLOB)
				continue;
			if (s->blob_len == 24) {
				uint32_t id = rd32le(s->blob, 0);

				CHECK_EQ(id, fx->lsfw[nh].id);
				CHECK_EQ(rd32le(s->blob, 8), PG106_ACR_LSF_SEC2);
				CHECK_EQ(rd32le(s->blob, 12),
					 id == PG106_ACR_LSF_SEC2 ? 0u : 1u);
				CHECK_EQ(rd32le(s->blob, 16),
					 fx->sigs[nh]->version);
				nh++;
			} else if (s->addr == fx->lsfw[0].offset.img ||
				   s->addr == fx->lsfw[1].offset.img ||
				   s->addr == fx->lsfw[2].offset.img) {
				CHECK(s->blob != NULL);
				nimg++;
			}
		}
		CHECK_EQ(nh, 3);
		CHECK_EQ(nimg, 3);
	}

	// LS firmware derivations (lsfw.c) on the real files.
	CHECK_EQ(fx->lsfw[0].bootloader_imem_offset, 0x5c00); // start_tag 0x5c
	CHECK_EQ(fx->lsfw[0].bootloader_size, 0x100);
	CHECK_EQ(fx->lsfw[0].app_resident_code_size, 0x5200); // ALIGN(20927)
	CHECK_EQ(fx->lsfw[0].app_resident_data_size, 0x900);  // ALIGN(2256)
	CHECK_EQ(fx->lsfw[0].img_size, 0x5c00);
	CHECK_EQ(fx->lsfw[0].flags, 0);
	CHECK_EQ(fx->lsfw[1].flags, PG106_ACR_LSF_FORCE_PRIV_LOAD);
	CHECK(memcmp(fx->fecs_img + 0x100, fx->fecs_inst,
		     fx->fecs_inst_size) == 0);
	CHECK_EQ(fx->lsfw[2].bootloader_imem_offset, 0xfd00);
	CHECK_EQ(fx->lsfw[2].bootloader_size, 0x200);
	CHECK_EQ(fx->lsfw[2].ucode_size, 0x5500);
	CHECK_EQ(fx->lsfw[2].data_size, 0x15700);
	CHECK_EQ(fx->lsfw[2].bld_size, PG106_FLCN_BL_DMEM_DESC_V2_SIZE);
	// The SEC2 BL lands in the top 0x300 of a 64 KB IMEM — consistent
	// with the measured 64 KB (FINDINGS.md:160).
	CHECK(fx->lsfw[2].bootloader_imem_offset + fx->lsfw[2].bootloader_size
	      <= 0x10000);

	// Bootloader blocks (bld_write + bld_patch).
	{
		uint8_t b[PG106_FLCN_BL_DMEM_DESC_V2_SIZE];
		size_t len;
		const struct pg106_acr_lsfw *s2 = &fx->lsfw[2];

		CHECK_EQ(pg106_acr_lsfw_bld_build(s2, (int64_t)g_info.wpr_start,
						  b, sizeof(b), &len),
			 0);
		CHECK_EQ(len, 84);
		CHECK_EQ(rd32le(b, 32), PG106_FALCON_SEC2_DMAIDX_UCODE);
		CHECK_EQ(rd64le(b, 36), g_info.wpr_start + s2->offset.img +
						s2->app_start_offset);
		CHECK_EQ(rd32le(b, 60), 0x500); // app_imem_entry
		CHECK_EQ(rd64le(b, 64), g_info.wpr_start + s2->offset.img +
						s2->app_start_offset +
						s2->app_resident_data_offset);
		CHECK_EQ(rd32le(b, 76), 1);
		CHECK_EQ(rd32le(b, 80), PG106_SEC2_EMEM_ADDR);
		CHECK_EQ(pg106_acr_lsfw_bld_build(&fx->lsfw[0], 0, b, sizeof(b),
						  &len),
			 0);
		CHECK_EQ(len, 76);
		CHECK_EQ(rd32le(b, 32), PG106_FALCON_DMAIDX_UCODE);
		CHECK_EQ(rd64le(b, 36), (uint64_t)fx->lsfw[0].offset.img + 0x100);
		CHECK_EQ(rd32le(b, 48), 0x5200);
		CHECK_EQ(rd64le(b, 64),
			 (uint64_t)fx->lsfw[0].offset.img + 0x100 + 0x5200);
	}

	// Annotations: every step names its nouveau origin.
	for (i = 0; i < p->nsteps; i++)
		CHECK(p->steps[i].why && strstr(p->steps[i].why, "nvkm/"));
}

// --- HWCFG decode + falcon base -------------------------------------------------

static const uint32_t g_sec2_top_rows[] = {
	0x80000037, // ENGINE_TYPE 0x0d SEC2 ((0x0d<<2)|3), CHAIN
	0x8845103e, // ENUM engine 2, runlist 2, intr 10, reset 8, CHAIN
	0x00087001, // DATA inst 0, addr 0x087000, last
};

struct top_bus {
	const uint32_t *rows;
	size_t n;
};

static int
top_rd32(void *ud, uint32_t addr, uint32_t *v)
{
	struct top_bus *b = ud;
	size_t i;

	if (addr < PG106_TOP_INDEX ||
	    addr >= PG106_TOP_INDEX + PG106_TOP_MAX_ROWS * PG106_TOP_ROW_STRIDE)
		return -EIO;
	i = (addr - PG106_TOP_INDEX) / PG106_TOP_ROW_STRIDE;
	*v = i < b->n ? b->rows[i] : 0;
	return 0;
}

static void
test_base_and_hwcfg(struct fixture *fx)
{
	struct top_bus bus = { g_sec2_top_rows, 3 };
	struct pg106_top_table tbl;
	const struct pg106_top_device *d;

	CHECK_EQ(pg106_top_scan(top_rd32, &bus, &tbl), 0);
	fx->cfg.falcon_base =
		pg106_acrboot_falcon_base(&tbl, PG106_ACRBOOT_SEC2_BASE_GP106);
	CHECK_EQ(fx->cfg.falcon_base, 0x00087000);
	d = pg106_top_find(&tbl, PG106_TOP_TYPE_SEC2, 0);
	CHECK(d && d->reset >= 0);
	if (d && d->reset >= 0)
		fx->cfg.pmc_mask = 1u << d->reset; // nvkm_top_reset
	CHECK_EQ(pg106_acrboot_falcon_base(NULL, PG106_ACRBOOT_SEC2_BASE_GP106),
		 0x00087000);
	printf("falcon base: table 0x%06x, fallback 0x%06x (measured, "
	       "FINDINGS.md:160); SEC2 PMC mask 0x%08x (top reset line)\n",
	       fx->cfg.falcon_base,
	       pg106_acrboot_falcon_base(NULL, PG106_ACRBOOT_SEC2_BASE_GP106),
	       fx->cfg.pmc_mask);

	// HWCFG decode vs the captured GP106 words (FINDINGS.md:60,66).
	CHECK_EQ(PG106_FALCON_HWCFG_CODE_LIMIT(0x20202060u), 0x6000);  // 24 KB
	CHECK_EQ(PG106_FALCON_HWCFG_DATA_LIMIT(0x20202060u), 0x1000);  // 4 KB
	CHECK_EQ(PG106_FALCON_HWCFG_CODE_LIMIT(0x400e0100u), 0x10000); // 64 KB
	CHECK_EQ(PG106_FALCON_HWCFG_DATA_LIMIT(0x400e0100u), 0x10000); // 64 KB
}

// --- argument validation + parser gates ---------------------------------------------

static void
test_arg_validation(struct fixture *fx)
{
	struct pg106_acrboot_plan plan = { 0 };
	struct pg106_acrboot_info info;
	struct pg106_acrboot_config cfg = fx->cfg;
	struct pg106_acr_lsfw many[PG106_ACR_LSF_MAX + 1];
	const struct pg106_acr_lsf_signature_v1 *sigs[PG106_ACR_LSF_MAX + 1];
	size_t i;

	for (i = 0; i < PG106_ACR_LSF_MAX + 1; i++) {
		many[i] = fx->lsfw[0];
		sigs[i] = &fx->fecs_sig;
	}
	CHECK_EQ(pg106_acrboot_plan_build(&cfg, &fx->bl, &fx->hs, many,
					  PG106_ACR_LSF_MAX + 1, sigs, &plan,
					  &info),
		 -EINVAL);
	// HWCFG with too little IMEM for the BL (gm200.c:291).
	cfg.hwcfg = 0x00000001u; // code.limit 0x100 < boot_size 0x200
	CHECK_EQ(build(fx, &cfg, &plan, &info), -EINVAL);
	cfg = fx->cfg;
	CHECK_EQ(pg106_acrboot_plan_build(NULL, &fx->bl, &fx->hs, fx->lsfw, 3,
					  fx->sigs, &plan, &info),
		 -EINVAL);
	{
		const struct pg106_acr_lsf_signature_v1 *wrong[3] = {
			&fx->gpccs_sig, &fx->gpccs_sig, &fx->sec2_sig
		};

		CHECK_EQ(pg106_acrboot_plan_build(&cfg, &fx->bl, &fx->hs,
						  fx->lsfw, 3, wrong, &plan,
						  &info),
			 -EINVAL);
	}
	pg106_acrboot_plan_free(&plan);

	// LS builders reject truncated inputs.
	{
		struct pg106_acr_lsfw l;
		size_t n;

		CHECK_EQ(pg106_acr_lsfw_from_ls_desc_v1(fx->sec2_desc, 100,
							fx->sec2_img,
							fx->sec2_img_size,
							PG106_ACR_LSF_SEC2, &l),
			 -EINVAL);
		CHECK_EQ(pg106_acr_lsfw_from_ls_desc_v1(fx->sec2_desc,
							fx->sec2_desc_size,
							fx->sec2_img, 0x4000,
							PG106_ACR_LSF_SEC2, &l),
			 -EINVAL);
		CHECK_EQ(pg106_acr_lsfw_from_bl_inst_data(
				 fx->fecs_bl, 0x100, fx->fecs_inst,
				 fx->fecs_inst_size, fx->fecs_data,
				 fx->fecs_data_size, PG106_ACR_LSF_FECS, NULL,
				 0, &n, &l),
			 -EINVAL);
		CHECK_EQ(pg106_acr_lsfw_from_bl_inst_data(
				 fx->fecs_bl, fx->fecs_bl_size, fx->fecs_inst,
				 fx->fecs_inst_size, fx->fecs_data,
				 fx->fecs_data_size, PG106_ACR_LSF_SEC2, NULL,
				 0, &n, &l),
			 -EINVAL);
		l = fx->lsfw[0];
		l.bld_kind = PG106_ACR_BLD_NONE;
		CHECK_EQ(pg106_acr_lsfw_bld_build(&l, 0, (uint8_t[84]){ 0 }, 84,
						  &n),
			 -EINVAL);
	}
}

// --- ACR memory built by devboot (cfg->vram): falcon DMA through the VMM ----

struct hm_ctx {
	struct mockgpu *gpu;
	void *buf;
	uint64_t bus;
	unsigned allocs, frees;
};

static int
hm_alloc(void *u, uint64_t size, void **cpu, void **handle)
{
	struct hm_ctx *h = u;

	h->buf = malloc(size);
	h->bus = 0x3000000000ull; // SIM bus address of the host pages
	mg_add_host(h->gpu, h->bus, h->buf, size);
	*cpu = h->buf;
	*handle = h;
	h->allocs++;
	return 0;
}

static int
hm_bus(void *u, void *handle, uint64_t off, uint64_t *addr)
{
	struct hm_ctx *h = u;

	(void)handle;
	*addr = h->bus + off;
	return 0;
}

static void
hm_free(void *u, void *handle)
{
	struct hm_ctx *h = u;

	(void)handle;
	h->gpu->nhost = 0;
	free(h->buf);
	h->buf = NULL;
	h->frees++;
}

static void
fill_fw(const struct fixture *fx, struct pg106_devboot_fw *fw)
{
	memset(fw, 0, sizeof(*fw));
	fw->acr_bl = (struct pg106_fw_blob){ fx->bl_blob, fx->bl_size };
	fw->acr_ucode_load = (struct pg106_fw_blob){ fx->hs_blob, fx->hs_size };
	fw->fecs_bl = (struct pg106_fw_blob){ fx->fecs_bl, fx->fecs_bl_size };
	fw->fecs_inst = (struct pg106_fw_blob){ fx->fecs_inst, fx->fecs_inst_size };
	fw->fecs_data = (struct pg106_fw_blob){ fx->fecs_data, fx->fecs_data_size };
	fw->fecs_sig = (struct pg106_fw_blob){ fx->fecs_sig_blob, fx->fecs_sig_size };
	fw->gpccs_bl = (struct pg106_fw_blob){ fx->gpccs_bl, fx->gpccs_bl_size };
	fw->gpccs_inst = (struct pg106_fw_blob){ fx->gpccs_inst, fx->gpccs_inst_size };
	fw->gpccs_data = (struct pg106_fw_blob){ fx->gpccs_data, fx->gpccs_data_size };
	fw->gpccs_sig = (struct pg106_fw_blob){ fx->gpccs_sig_blob, fx->gpccs_sig_size };
	fw->sec2_desc = (struct pg106_fw_blob){ fx->sec2_desc, fx->sec2_desc_size };
	fw->sec2_image = (struct pg106_fw_blob){ fx->sec2_img, fx->sec2_img_size };
	fw->sec2_sig = (struct pg106_fw_blob){ fx->sec2_sig_blob, fx->sec2_sig_size };
}

static void
test_devboot_acrmem(struct fixture *fx)
{
	static const uint64_t vram_6g = 0x180000000ull;
	struct mock *m = malloc(sizeof(*m));
	struct mockgpu *gpu = malloc(sizeof(*gpu));
	struct pg106_devboot_fw fw;
	unsigned pass;

	printf("\n== SIMULATION: devboot builds the ACR memory (WPR, inst, VMM in "
	       "VRAM); the toy ACR fetches the HS image by DMA through BINDINST ==\n");
	fill_fw(fx, &fw);
	for (pass = 0; pass < 2; pass++) { // 0: image in VRAM, 1: host pages
		struct pg106_hw_bus bus = { m, mock_rd32, mock_wr32, x_delay_m, NULL, 0 };
		struct pg106_devboot_hostmem hmem;
		struct hm_ctx hc = { 0 };
		struct pg106_vram vram;
		struct pg106_devboot_cfg c;
		struct pg106_devboot db;
		struct pg106_flcn_bus fb;
		struct pg106_sec2 sec2;

		mg_init(gpu, vram_6g);
		mock_init(m, fx, &g_info, 0);
		m->gpu = gpu;
		mg_set(gpu, PG106_DEVINIT_POSTED_REG, PG106_DEVINIT_POSTED_BIT);
		hc.gpu = gpu;
		hmem = (struct pg106_devboot_hostmem){ &hc, hm_alloc, hm_bus, hm_free, true };
		CHECK_EQ(pg106_vram_init(&vram, &bus, vram_6g), 0);
		memset(&c, 0, sizeof(c));
		c.bus = &bus;
		c.fw = &fw;
		c.vram = &vram;
		c.hostmem = pass ? &hmem : NULL;
		c.acr_vma = ACR_VMA;
		c.stop_after = PG106_DB_ACR;
		CHECK_EQ(pg106_devboot_run(&db, &c), 0);
		CHECK_EQ(db.stage, PG106_DB_ACR);
		CHECK(m->acr_ok);
		CHECK_EQ(m->acr_err, 0);
		CHECK(m->rtos_loaded);
		CHECK_EQ(m->run_state, 3);
		CHECK_EQ(m->dma_faults, 0);
		CHECK_EQ(m->dma_words, m->img_size / 4);
		CHECK_EQ(m->dma_inst, db.acrmem.inst);
		CHECK_EQ(reg_get(m, SEC2_BASE + PG106_FALCON_BINDINST), db.acrmem.bindinst);
		CHECK_EQ(db.acrmem.bindinst, 0x40000000u | (uint32_t)(db.acrmem.inst >> 12));
		// Hardware reference (owner's mmiotrace of this GPU): the WPR
		// nouveau allocated was shadow 0x17fe00000, WPR 0x17fe40000 -
		// 0x17fe80000, i.e. alloc_size 0x80000 from these firmware files.
		CHECK_EQ(db.acr_info.alloc_size, 0x80000);
		CHECK_EQ(db.acr_info.wpr_end - db.acr_info.wpr_start, 0x40000);
		CHECK_EQ(db.acr_info.shadow_start, db.acrmem.wpr_base);
		CHECK_EQ(db.acrmem.wpr_base & 0x3ffff, 0);
		CHECK(db.acrmem.wpr_base + db.acrmem.wpr_alloc <= vram_6g - 0x100000);
		// Joined, VER2, 64K big pages, VRAM.
		CHECK_EQ(mg_vram_rd64(gpu, db.acrmem.inst + 0x200),
			 pg106_mmu_root_addr(db.acrmem.vmm.mmu) | 0xc00);
		CHECK(gpu->inv_count >= 1);
		CHECK_EQ(gpu->nerr, 0);
		CHECK_EQ(mg_get(gpu, 0x100c80) & 0x1800, 0x1800); // FB init ran
		if (pass) {
			int ap;
			uint64_t pa;

			CHECK_EQ(hc.allocs, 1);
			CHECK_EQ(mg_translate(gpu, db.acrmem.inst, ACR_VMA, &ap, &pa), 0);
			CHECK_EQ(ap, 2);
			CHECK_EQ(pa, 0x3000000000ull);
		} else {
			CHECK(db.acrmem.img_pa != 0);
		}
		fb = pg106_hw_flcn_bus(&bus);
		pg106_sec2_ctx_init(&sec2, &fb, SEC2_BASE, 64);
		CHECK_EQ(pg106_acr_bootstrap_falcons(&sec2, (1ull << PG106_ACR_LSF_FECS) |
							   (1ull << PG106_ACR_LSF_GPCCS)), 0);
		printf("%s: inst 0x%llx (BINDINST 0x%08x), pd 0x%llx, WPR 0x%llx-0x%llx, "
		       "%u image words fetched by DMA, BOOTSTRAP_FALCON OK\n",
		       pass ? "HS image in host pages" : "HS image in VRAM",
		       (unsigned long long)db.acrmem.inst, db.acrmem.bindinst,
		       (unsigned long long)pg106_mmu_root_addr(db.acrmem.vmm.mmu),
		       (unsigned long long)db.acr_info.wpr_start,
		       (unsigned long long)db.acr_info.wpr_end, m->dma_words);
		pg106_devboot_fini(&db);
		CHECK_EQ(vram.mm.free_bytes, vram_6g - 0x100000); // all returned
		if (pass)
			CHECK_EQ(hc.frees, 1);
		pg106_vram_fini(&vram);
		mock_free(m);
		mg_free(gpu);
	}
	// Mutation: the image never mapped (VMM tables not uploaded) -> the
	// DMA faults and the ACR reports an error instead of booting.
	{
		struct pg106_hw_bus bus = { m, mock_rd32, mock_wr32, x_delay_m, NULL, 0 };
		struct pg106_vram vram;
		struct pg106_devboot_cfg c;
		struct pg106_devboot db;

		mg_init(gpu, vram_6g);
		mock_init(m, fx, &g_info, 0);
		m->gpu = gpu;
		mg_set(gpu, PG106_DEVINIT_POSTED_REG, PG106_DEVINIT_POSTED_BIT);
		CHECK_EQ(pg106_vram_init(&vram, &bus, vram_6g), 0);
		memset(&c, 0, sizeof(c));
		c.bus = &bus;
		c.fw = &fw;
		c.vram = &vram;
		c.acr_vma = ACR_VMA + 0x10000000ull; // BL desc names ACR_VMA: unmapped
		c.stop_after = PG106_DB_ACR;
		CHECK(pg106_devboot_run(&db, &c) != 0);
		CHECK(!m->acr_ok);
		pg106_devboot_fini(&db);
		pg106_vram_fini(&vram);
		mock_free(m);
		mg_free(gpu);
	}
	free(m);
	free(gpu);
	printf("PASS devboot ACR memory: WPR alloc 0x80000 (= trace), BINDINST -> "
	       "inst -> VMM -> HS image (VRAM and host), unmapped image fails\n");
}

// --- main -------------------------------------------------------------------------

int
main(void)
{
	const char *dir = fw_dir();
	struct fixture fx;

	if (!dir) {
		printf("FAIL: research/firmware/gp106 not found\n");
		return 1;
	}
	printf("**********************************************************************\n");
	printf("**  SIMULATION, NOT HARDWARE PROOF.                                 **\n");
	printf("**  Modeled ACR/SEC2 boot vs a mock falcon; real firmware only.     **\n");
	printf("**********************************************************************\n");
	printf("== pg106 ACR boot + SEC2 RTOS tests (firmware: %s)\n", dir);

	if (fixture_setup(&fx, dir) != 0) {
		printf("== acr_boot: %d FAILURE(S)\n", g_failures);
		return 1;
	}
	test_base_and_hwcfg(&fx);
	test_full_boot(&fx);
	test_hwexec(&fx);
	test_devboot_acrmem(&fx);
	if (g_plan.nsteps) {
		test_structure(&fx);
		test_mutations(&fx);
	}
	test_arg_validation(&fx);

	pg106_acrboot_plan_free(&g_plan);
	fixture_free(&fx);
	if (g_failures) {
		printf("== acr_boot: %d FAILURE(S)\n", g_failures);
		return 1;
	}
	printf("== acr_boot: PASS (simulation only — no hardware claims)\n");
	return 0;
}
