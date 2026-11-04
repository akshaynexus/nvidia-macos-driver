// SPDX-License-Identifier: GPL-2.0-only
// pascal/tests/test_acr_boot.c — Phase 2 ACR secure-boot SIMULATION test.
//
// **********************************************************************
// **  SIMULATION, NOT HARDWARE PROOF.                                   **
// **  This test drives pg106_acrboot_plan_build()'s model of the        **
// **  nouveau falcon-loading sequence against a MOCK BUS that merely    **
// **  records writes. Nothing here runs on, reads, or proves anything   **
// **  about a real GP106. (HANDOFF-PASCAL.md Phase 2 gate wording.)     **
// **********************************************************************
//
// What IS real: the firmware blobs. Every blob (gp102/acr/{bl,ucode_load,
// ucode_unload,unload_bl}.bin standing in for the never-shipped gp106/acr/,
// and the REAL gp106/gr/{fecs,gpccs}_sig.bin) is validated through the
// pascal/core/src/acr.c parsers (structural signatures/headers — the
// Phase 2 gate) BEFORE the plan is built, and the plan's byte streams are
// checked against those parsed views.
//
// Checks:
//   (a) step-ordering invariants (sig patch -> reset -> setup -> load ->
//       boot -> WPR check; reset before DMAPIO; BOOTVEC before CPUCTL
//       start; mailbox read after the halt poll; ...)
//   (b) IMEM/DMEM write-stream byte counts vs the parsed blob sections
//   (c) every step carries a non-empty `why` provenance annotation
//   (d) all blob headers/signatures validated via acr.c first
//   (e) the full step list is dumped to stdout as the committed-evidence
//       trace
//
// Run: cd pascal && make test   (or build/test_acr_boot from pascal/)

#include <pascal/acr.h>
#include <pascal/acr_boot.h>

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

// --- tiny file loader (same shape as test_acr.c) ----------------------------

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

// Firmware lives in research/firmware/gp106 relative to the repo root; the
// test binary runs from pascal/ under `make test`.
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

// --- mock bus ---------------------------------------------------------------
//
// Records every MMIO write; polls/reads return canned "firmware answered"
// values. The mock's only cleverness: IMEMD/DMEMD writes are recognized so
// the byte streams can be counted, and the PFB WPR-check readback responds
// with the selector-dependent window value the plan expects.

#define OFF_INTR      0x004u
#define OFF_MBOX0     0x040u
#define OFF_MBOX1     0x044u
#define OFF_BINDSTAT  0x0dcu
#define OFF_CPUCTL    0x100u
#define OFF_MEM_SCRUB 0x10cu
#define OFF_IMEMC     0x180u
#define OFF_IMEMD     0x184u
#define OFF_IMEMT     0x188u
#define OFF_DMEMC     0x1c0u
#define OFF_DMEMD     0x1c4u

struct mock_bus {
	uint32_t falcon_base;
	// WPR-check state: last selector written to PFB 0x100cd4 + expected
	// window values (mock returns whatever makes the model pass — that
	// is the point of a mock).
	uint32_t wpr_check_sel;
	uint64_t wpr_start, wpr_end;

	// recorded traffic
	size_t n_writes;
	uint32_t imemc_vals[16];
	size_t n_imemc;
	uint32_t dmemc_vals[16];
	size_t n_dmemc;
	uint32_t imemt_tags[64];
	size_t n_imemt;
	uint32_t imem_bytes; // IMEMD writes * 4
	uint32_t dmem_bytes; // DMEMD writes * 4
	size_t n_bindstat_polls;
	size_t n_halt_polls;
	size_t n_mbox_reads;
};

static uint32_t
mock_off(const struct mock_bus *bus, uint64_t addr)
{
	return (uint32_t)(addr - bus->falcon_base);
}

static void
mock_write(void *ctx, const struct pg106_acrboot_step *s)
{
	struct mock_bus *bus = ctx;

	CHECK(s->space == PG106_ACRBOOT_MMIO);
	bus->n_writes++;

	if (s->addr == PG106_PFB_WPR_CHECK) {
		bus->wpr_check_sel = s->value; // window selector
		return;
	}

	switch (mock_off(bus, s->addr)) {
	case OFF_IMEMC:
		CHECK(bus->n_imemc <
		      sizeof(bus->imemc_vals) / sizeof(bus->imemc_vals[0]));
		bus->imemc_vals[bus->n_imemc++] = s->value;
		break;
	case OFF_IMEMT:
		CHECK(bus->n_imemt <
		      sizeof(bus->imemt_tags) / sizeof(bus->imemt_tags[0]));
		bus->imemt_tags[bus->n_imemt++] = s->value;
		break;
	case OFF_IMEMD:
		bus->imem_bytes += 4;
		break;
	case OFF_DMEMC:
		CHECK(bus->n_dmemc <
		      sizeof(bus->dmemc_vals) / sizeof(bus->dmemc_vals[0]));
		bus->dmemc_vals[bus->n_dmemc++] = s->value;
		break;
	case OFF_DMEMD:
		bus->dmem_bytes += 4;
		break;
	}
}

static void
mock_blob(void *ctx, const struct pg106_acrboot_step *s)
{
	(void)ctx;
	CHECK(s->kind == PG106_ACRBOOT_WRITE_BLOB);
	// Blob writes are validated via the step list itself in the
	// assertions below (real sig bytes, WPR header fields, zero-fills).
}

// Returns the canned value a poll/read would see; the caller (run_plan)
// then applies the step's own pass condition to it.
static uint32_t
mock_read(void *ctx, const struct pg106_acrboot_step *s)
{
	struct mock_bus *bus = ctx;

	if (s->addr == PG106_PFB_WPR_CHECK) {
		// gm200_acr_wpr_check (gm200.c:57-61): selector 2 -> start
		// window, 3 -> limit - 0x20000, values in 256-byte units.
		if (bus->wpr_check_sel == 2)
			return (uint32_t)((bus->wpr_start >> 8) & 0xffffff00u);
		return (uint32_t)(((bus->wpr_end - 0x20000ull) >> 8) &
				  0xffffff00u);
	}

	switch (mock_off(bus, s->addr)) {
	case OFF_MEM_SCRUB:
		return 0x00000006u; // scrub done
	case OFF_BINDSTAT:
		// First bind poll reports state 5 (bind done), later 0.
		return ++bus->n_bindstat_polls == 1 ? 0x00005000u : 0x0u;
	case OFF_CPUCTL:
		bus->n_halt_polls++;
		return 0x00000012u; // bit 4 HALTED (+ START echo)
	case OFF_MBOX0:
		bus->n_mbox_reads++;
		return 0x00000000u; // boot_mbox0_ok for the gp102 load fw
	case OFF_MBOX1:
		return 0x00000000u;
	}
	return 0;
}

// Replay the plan. Returns the number of failed steps (should be 0).
static size_t
run_plan(struct mock_bus *bus, const struct pg106_acrboot_plan *plan)
{
	size_t i, fails = 0;

	for (i = 0; i < plan->nsteps; i++) {
		const struct pg106_acrboot_step *s = &plan->steps[i];
		uint32_t v;

		switch (s->kind) {
		case PG106_ACRBOOT_WRITE_REG:
			// IMG/WPR u32 writes are validated via the step list
			// itself; only falcon MMIO goes through the bus.
			if (s->space == PG106_ACRBOOT_MMIO)
				mock_write(bus, s);
			break;
		case PG106_ACRBOOT_WRITE_BLOB:
			mock_blob(bus, s);
			break;
		case PG106_ACRBOOT_POLL_REG:
			v = mock_read(bus, s);
			if (s->poll_op == PG106_ACRBOOT_POLL_EQ) {
				if ((v & s->mask) != s->value)
					fails++;
			} else {
				if (!(v & s->mask))
					fails++;
			}
			break;
		case PG106_ACRBOOT_READ_REG:
			v = mock_read(bus, s);
			if ((v & s->mask) != (s->value & s->mask))
				fails++;
			break;
		}
	}
	return fails;
}

// --- synthetic LS falcon descriptions (same shapes as test_acr.c; the
// relationships are the r352 ones from lsfw.c) --------------------------------

static void
make_fecs_lsfw(struct pg106_acr_lsfw *l)
{
	*l = (struct pg106_acr_lsfw){
		.id = PG106_ACR_LSF_FECS,
		.bootloader_size = 0x100,
		.bootloader_imem_offset = 0x0,
		.app_size = 0x2b00,
		.app_start_offset = 0x100,
		.app_imem_entry = 0x100,
		.app_resident_code_offset = 0x0,
		.app_resident_code_size = 0x2800,
		.app_resident_data_offset = 0x2900,
		.app_resident_data_size = 0x100,
		.ucode_size = 0x2a00,
		.data_size = 0x100,
		.bld_size = 0x4c,
		.img_size = 0x2b00,
	};
}

static void
make_gpccs_lsfw(struct pg106_acr_lsfw *l)
{
	make_fecs_lsfw(l);
	l->id = PG106_ACR_LSF_GPCCS;
}

// --- shared fixture -----------------------------------------------------------

static struct pg106_acrboot_plan g_plan;
static struct pg106_acrboot_info g_info;

struct fixture {
	const char *fwdir;
	uint8_t *bl_blob, *hs_blob, *unl_bl_blob, *unl_hs_blob, *fecs_sig_blob,
		*gpccs_sig_blob;
	size_t bl_size, hs_size, unl_bl_size, unl_hs_size, fecs_sig_size,
		gpccs_sig_size;
	struct pg106_acr_bl_ucode bl;
	struct pg106_acr_hs_ucode hs;
	struct pg106_acr_lsf_signature_v1 fecs_sig, gpccs_sig;
	struct pg106_acrboot_config cfg;
	uint64_t desc_base; // dmem_base_img + 0x210: ACR desc field base
};

// (d) ALL blob headers/signatures validated via acr.c first. Returns 0 on
// success (fixture fully populated), nonzero if any blob is missing or
// structurally invalid — the Phase 2 gate.
static int
fixture_setup(struct fixture *fx, const char *dir)
{
	struct pg106_acr_bl_ucode ubl;
	struct pg106_acr_hs_ucode uhs;
	int ret = 0;

	memset(fx, 0, sizeof(*fx));
	fx->fwdir = dir;

	// gp102 ACR blobs (stand in for the never-shipped nvidia/gp106/acr/ —
	// see research/firmware/gp106/README.md) + REAL gp106 GR sigs.
	fx->bl_blob = load_blob(dir, "acr/bl.bin", &fx->bl_size);
	fx->hs_blob = load_blob(dir, "acr/ucode_load.bin", &fx->hs_size);
	fx->unl_bl_blob = load_blob(dir, "acr/unload_bl.bin",
				    &fx->unl_bl_size);
	fx->unl_hs_blob = load_blob(dir, "acr/ucode_unload.bin",
				    &fx->unl_hs_size);
	fx->fecs_sig_blob = load_blob(dir, "gr/fecs_sig.bin",
				      &fx->fecs_sig_size);
	fx->gpccs_sig_blob = load_blob(dir, "gr/gpccs_sig.bin",
				       &fx->gpccs_sig_size);
	CHECK(fx->bl_blob && fx->hs_blob && fx->unl_bl_blob &&
	      fx->unl_hs_blob && fx->fecs_sig_blob && fx->gpccs_sig_blob);
	if (!fx->bl_blob || !fx->hs_blob || !fx->unl_bl_blob ||
	    !fx->unl_hs_blob || !fx->fecs_sig_blob || !fx->gpccs_sig_blob)
		return -1;

	ret |= pg106_acr_ucode_parse_bl(fx->bl_blob, fx->bl_size, &fx->bl);
	ret |= pg106_acr_ucode_parse_hs(fx->hs_blob, fx->hs_size, &fx->hs);
	ret |= pg106_acr_ucode_parse_bl(fx->unl_bl_blob, fx->unl_bl_size,
					&ubl);
	ret |= pg106_acr_ucode_parse_hs(fx->unl_hs_blob, fx->unl_hs_size,
					&uhs);
	ret |= pg106_acr_lsf_sig_parse(fx->fecs_sig_blob, fx->fecs_sig_size,
				       &fx->fecs_sig);
	ret |= pg106_acr_lsf_sig_parse(fx->gpccs_sig_blob, fx->gpccs_sig_size,
				       &fx->gpccs_sig);
	CHECK_EQ(ret, 0);
	if (ret)
		return -1;
	CHECK_EQ(ubl.hdr.bin_magic, PG106_FLCN_BIN_MAGIC_STD);
	CHECK_EQ(uhs.hdr.bin_magic, PG106_FLCN_BIN_MAGIC_STD);
	CHECK_EQ(fx->fecs_sig.falcon_id, PG106_ACR_LSF_FECS);
	CHECK_EQ(fx->gpccs_sig.falcon_id, PG106_ACR_LSF_GPCCS);

	printf("fixture: all 6 blobs parsed via acr.c "
	       "(bl 0x%zx B, ucode_load 0x%zx B, unload_bl 0x%zx B, "
	       "ucode_unload 0x%zx B, fecs_sig 0x%zx B, gpccs_sig 0x%zx B)\n",
	       fx->bl_size, fx->hs_size, fx->unl_bl_size, fx->unl_hs_size,
	       fx->fecs_sig_size, fx->gpccs_sig_size);

	// Simulated hardware/firmware geometry (see acr_boot.h: the falcon
	// base and IMEM limit are runtime-discovered on real hardware).
	fx->cfg = (struct pg106_acrboot_config){
		.falcon_base = 0x003c0000, // SIM stand-in (nvkm_top at runtime)
		.code_limit = 0x8000,      // SIM stand-in (FALCON_MEM_CAPS)
		.vma_addr = 0x100000000ull,
		.wpr_alloc_base = 0x0f0000000ull,
		.bootstrap_owner = PG106_ACR_LSF_SEC2, // gp102.c:89
		.rtos_id = PG106_ACR_LSF_GPCCS,
		.boot_mbox0 = 0,       // gp102_acr_load_fwif (gp102.c:239)
		.intr_clear = 0x10,    // gp102_acr_load_fwif (gp102.c:239)
		.pmc_readback = 0x13f, // SIM stand-in for PMC readback
		.bind_target = 0,      // NVKM_MEM_TARGET_VRAM
		.bind_addr = 0x10000000ull,
	};
	// Image-relative base of the flcn_acr_desc_v1 fields the setup
	// patches (0x210 = 0x200 reserved + 4 sig slots).
	fx->desc_base = fx->hs.dmem_base_img + 0x210;
	return 0;
}

struct mock_bus
make_bus(const struct fixture *fx)
{
	struct mock_bus bus = { .falcon_base = fx->cfg.falcon_base };

	// The mock's WPR-check readback echoes the plan's own geometry
	// (gm200_acr_wpr_check windows).
	bus.wpr_start = g_info.wpr_start;
	bus.wpr_end = g_info.wpr_end;
	return bus;
}

// --- 1. plan build + trace dump + replay -------------------------------------

static void
test_plan_build_and_replay(struct fixture *fx)
{
	struct pg106_acr_lsfw lsfws[2];
	const struct pg106_acr_lsf_signature_v1 *sigs[2];
	size_t i;
	int ret;

	printf("\n== SIMULATION, NOT HARDWARE PROOF: replaying the modeled "
	       "ACR boot against a mock bus ==\n");

	make_fecs_lsfw(&lsfws[0]);
	make_gpccs_lsfw(&lsfws[1]);
	sigs[0] = &fx->fecs_sig;
	sigs[1] = &fx->gpccs_sig;

	ret = pg106_acrboot_plan_build(&fx->cfg, &fx->bl, &fx->hs, lsfws, 2,
				       sigs, &g_plan, &g_info);
	CHECK_EQ(ret, 0);
	if (ret)
		return;
	printf("plan: %zu steps, wpr_size 0x%x, alloc 0x%llx "
	       "(shadow 0x%llx, wpr 0x%llx-0x%llx)\n",
	       g_plan.nsteps, g_info.wpr_size,
	       (unsigned long long)g_info.alloc_size,
	       (unsigned long long)g_info.shadow_start,
	       (unsigned long long)g_info.wpr_start,
	       (unsigned long long)g_info.wpr_end);
	CHECK_EQ(g_info.nsteps, g_plan.nsteps);

	// (e) full trace dump — the committed-evidence trace.
	for (i = 0; i < g_plan.nsteps; i++) {
		const struct pg106_acrboot_step *s = &g_plan.steps[i];

		printf("[%4zu] %-10s %-3s 0x%09llx", i,
		       pg106_acrboot_kind_name(s->kind),
		       pg106_acrboot_space_name(s->space),
		       (unsigned long long)s->addr);
		if (s->kind == PG106_ACRBOOT_WRITE_REG)
			printf(" = 0x%08x", s->value);
		if (s->kind == PG106_ACRBOOT_WRITE_BLOB)
			printf(" len=%u%s", s->blob_len,
			       s->blob ? "" : " (zero-fill)");
		if (s->kind == PG106_ACRBOOT_POLL_REG)
			printf(" mask=0x%08x %s 0x%08x", s->mask,
			       pg106_acrboot_poll_op_name(s->poll_op),
			       s->value);
		if (s->kind == PG106_ACRBOOT_READ_REG)
			printf(" expect (v&0x%08x)==0x%08x", s->mask,
			       s->value);
		printf(" | %s\n", s->why);
	}

	// Replay: the mock must satisfy every poll/read condition.
	{
		struct mock_bus bus = make_bus(fx);

		CHECK_EQ(run_plan(&bus, &g_plan), 0);
		CHECK_EQ(bus.n_halt_polls, 1);
		CHECK_EQ(bus.n_mbox_reads, 1); // MBOX0 (the MBOX1 read is separate)
		CHECK_EQ(bus.n_bindstat_polls, 2);
	}
}

// --- 2. ordering invariants ----------------------------------------------------

static size_t
find_first(const struct pg106_acrboot_plan *p, bool (*pred)(const struct pg106_acrboot_step *))
{
	size_t i;

	for (i = 0; i < p->nsteps; i++)
		if (pred(&p->steps[i]))
			return i;
	return (size_t)-1;
}

static bool
is_sig_patch(const struct pg106_acrboot_step *s)
{
	return s->space == PG106_ACRBOOT_IMG &&
	       s->kind == PG106_ACRBOOT_WRITE_BLOB && s->blob != NULL;
}

static bool
is_reset_set(const struct pg106_acrboot_step *s)
{
	return s->space == PG106_ACRBOOT_MMIO &&
	       s->addr == 0x003c0000ull + PG106_FALCON_GP102_RESET &&
	       s->value == 1;
}

static bool
is_reset_clear(const struct pg106_acrboot_step *s)
{
	return s->space == PG106_ACRBOOT_MMIO &&
	       s->addr == 0x003c0000ull + PG106_FALCON_GP102_RESET &&
	       s->value == 0;
}

static bool
is_desc_setup(const struct pg106_acrboot_step *s)
{
	// The 9 gp102_acr_load_setup writes land at dmem_base_img + 0x210
	// and above (flcn_acr_desc_v1 field base); everything else in the
	// IMG space is the sig patch (a blob) or nothing.
	return s->space == PG106_ACRBOOT_IMG &&
	       s->kind == PG106_ACRBOOT_WRITE_REG && s->addr >= 0x210;
}

static bool
is_imemc(const struct pg106_acrboot_step *s)
{
	return s->space == PG106_ACRBOOT_MMIO &&
	       s->addr == 0x003c0000ull + PG106_FALCON_IMEMC;
}

static bool
is_dmemc(const struct pg106_acrboot_step *s)
{
	return s->space == PG106_ACRBOOT_MMIO &&
	       s->addr == 0x003c0000ull + PG106_FALCON_DMEMC;
}

static bool
is_bootvec(const struct pg106_acrboot_step *s)
{
	return s->space == PG106_ACRBOOT_MMIO &&
	       s->addr == 0x003c0000ull + PG106_FALCON_BOOTVEC;
}

static bool
is_cpuctl_start(const struct pg106_acrboot_step *s)
{
	return s->space == PG106_ACRBOOT_MMIO &&
	       s->addr == 0x003c0000ull + PG106_FALCON_CPUCTL &&
	       s->kind == PG106_ACRBOOT_WRITE_REG && s->value == 0x2;
}

static bool
is_halt_poll(const struct pg106_acrboot_step *s)
{
	return s->space == PG106_ACRBOOT_MMIO &&
	       s->addr == 0x003c0000ull + PG106_FALCON_CPUCTL &&
	       s->kind == PG106_ACRBOOT_POLL_REG;
}

static bool
is_mbox0_read(const struct pg106_acrboot_step *s)
{
	return s->space == PG106_ACRBOOT_MMIO &&
	       s->addr == 0x003c0000ull + PG106_FALCON_MBOX0 &&
	       s->kind == PG106_ACRBOOT_READ_REG;
}

static bool
is_irqsclr(const struct pg106_acrboot_step *s)
{
	return s->space == PG106_ACRBOOT_MMIO &&
	       s->addr == 0x003c0000ull + PG106_FALCON_INTR &&
	       s->kind == PG106_ACRBOOT_WRITE_REG && s->value == 0x10;
}

static bool
is_wpr_any(const struct pg106_acrboot_step *s)
{
	return s->space == PG106_ACRBOOT_WPR;
}

static bool
is_wpr_terminator(const struct pg106_acrboot_step *s)
{
	return s->space == PG106_ACRBOOT_WPR &&
	       s->kind == PG106_ACRBOOT_WRITE_BLOB && s->blob_len == 4 &&
	       rd32le(s->blob, 0) == PG106_ACR_WPR_FALCON_ID_INVALID;
}

static bool
is_wpr_check_read(const struct pg106_acrboot_step *s)
{
	return s->space == PG106_ACRBOOT_MMIO &&
	       s->addr == PG106_PFB_WPR_CHECK &&
	       s->kind == PG106_ACRBOOT_READ_REG;
}

static void
test_ordering(void)
{
	const struct pg106_acrboot_plan *p = &g_plan;
	size_t sig, rst_set, rst_clr, setup, imemc0, bootvec, start, halt,
		mbox0, irqsclr, wpr0, term, wpr_check, dmemc0;

	// nouveau's own order (each stage cited in the step `why`s):
	// WPR build (oneinit) -> sig patch -> reset -> setup -> load -> boot
	// -> WPR check.
	sig = find_first(p, is_sig_patch);
	rst_set = find_first(p, is_reset_set);
	rst_clr = find_first(p, is_reset_clear);
	setup = find_first(p, is_desc_setup);
	imemc0 = find_first(p, is_imemc);
	dmemc0 = find_first(p, is_dmemc);
	bootvec = find_first(p, is_bootvec);
	start = find_first(p, is_cpuctl_start);
	halt = find_first(p, is_halt_poll);
	mbox0 = find_first(p, is_mbox0_read);
	irqsclr = find_first(p, is_irqsclr);
	wpr0 = find_first(p, is_wpr_any);
	term = find_first(p, is_wpr_terminator);
	wpr_check = find_first(p, is_wpr_check_read);

	printf("\nordering: wpr0=%zu sig=%zu rst_set=%zu rst_clr=%zu "
	       "setup=%zu imemc0=%zu dmemc0=%zu bootvec=%zu start=%zu "
	       "halt=%zu mbox0=%zu irqsclr=%zu term=%zu wpr_check=%zu\n",
	       wpr0, sig, rst_set, rst_clr, setup, imemc0, dmemc0, bootvec,
	       start, halt, mbox0, irqsclr, term, wpr_check);

	CHECK(sig != (size_t)-1);
	CHECK(rst_set != (size_t)-1);
	CHECK(rst_clr != (size_t)-1);
	CHECK(setup != (size_t)-1);
	CHECK(imemc0 != (size_t)-1);
	CHECK(dmemc0 != (size_t)-1);
	CHECK(bootvec != (size_t)-1);
	CHECK(start != (size_t)-1);
	CHECK(halt != (size_t)-1);
	CHECK(mbox0 != (size_t)-1);
	CHECK(irqsclr != (size_t)-1);
	CHECK(wpr0 != (size_t)-1);
	CHECK(term != (size_t)-1);
	CHECK(wpr_check != (size_t)-1);
	if ((sig | rst_set | rst_clr | setup | imemc0 | dmemc0 | bootvec |
	     start | halt | mbox0 | irqsclr | wpr0 | term | wpr_check) ==
	    (size_t)-1)
		return;

	// (a) the invariants.
	CHECK(wpr0 < sig);                    // oneinit WPR build first
	CHECK(sig < rst_set);                 // fw_boot: patch before reset
	CHECK(rst_set < rst_clr);             // eng reset assert, deassert
	CHECK(rst_clr < setup);               // reset before setup
	CHECK(rst_set < imemc0);              // reset before any IMEM PIO
	CHECK(setup < imemc0);                // setup before load
	CHECK(imemc0 < dmemc0);               // BL imem stream precedes bld
	CHECK(bootvec < start);               // BOOTVEC before CPUCTL START
	CHECK(start < halt);                  // START before halt poll
	CHECK(halt < mbox0);                  // poll before mailbox readback
	CHECK(mbox0 < irqsclr);               // mailbox before IRQSCLR
	CHECK(term > wpr0);                   // WPR terminator after headers
	CHECK(wpr_check > start);             // WPR check after HS boot
	// The plan ends with the second WPR-window readback (limit).
	CHECK(p->nsteps >= 1 &&
	      is_wpr_check_read(&p->steps[p->nsteps - 1]));
}

// --- 3. byte counts + PIO stream shapes ----------------------------------------

static void
test_byte_counts(struct fixture *fx)
{
	const struct pg106_acr_hs_ucode *hs = &fx->hs;
	struct mock_bus bus = make_bus(fx);
	uint32_t expect_imem, expect_dmem;
	size_t i;

	// Replay to populate the mock's counters.
	CHECK_EQ(run_plan(&bus, &g_plan), 0);

	// (b) IMEM stream = BL (0x200) + HS nmem (0x100) + HS imem (0x2800).
	expect_imem = fx->bl.boot_size + hs->nmem_size + hs->imem_size;
	CHECK_EQ(fx->bl.boot_size, 0x200);
	CHECK_EQ(hs->nmem_size, 0x100);
	CHECK_EQ(hs->imem_size, 0x2800);
	CHECK_EQ(bus.imem_bytes, expect_imem);
	// DMEM stream = BL arg block (76) + HS data segment (0x1000).
	expect_dmem = PG106_FLCN_BL_DMEM_DESC_V1_SIZE + hs->dmem_size;
	CHECK_EQ(hs->dmem_size, 0x1000);
	CHECK_EQ(bus.dmem_bytes, expect_dmem);
	printf("byte counts: IMEM %u bytes (BL 0x%x + nmem 0x%x + imem "
	       "0x%x), DMEM %u bytes (bld 0x%x + dseg 0x%x)\n",
	       bus.imem_bytes, fx->bl.boot_size, hs->nmem_size,
	       hs->imem_size, bus.dmem_bytes,
	       (unsigned)PG106_FLCN_BL_DMEM_DESC_V1_SIZE, hs->dmem_size);

	// Three IMEM streams: BL (non-sec), nmem (non-sec), imem (SECURE).
	CHECK_EQ(bus.n_imemc, 3);
	CHECK_EQ(bus.imemc_vals[0],
		 0x01000000u | (fx->cfg.code_limit - fx->bl.boot_size));
	CHECK_EQ(bus.imemc_vals[1], 0x01000000u | hs->nmem_base);
	CHECK_EQ(bus.imemc_vals[2],
		 0x11000000u | hs->imem_base); // BIT(28) secure
	// Two DMEM streams: BL arg block at 0, HS data segment at dmem_base
	// (fw.c:275: dmem_base = 0 for the HS constructor — the segment is
	// written at DMEM offset dmem_base_img inside the image-mapped view,
	// but the PIO target is dmem_base = 0).
	CHECK_EQ(bus.n_dmemc, 2);
	CHECK_EQ(bus.dmemc_vals[0], 0x01000000u | 0x0);
	CHECK_EQ(bus.dmemc_vals[1], 0x01000000u | 0x0);

	// BL tags: boot_addr >> 8 = bl_desc start_tag, incrementing per
	// 0x100-byte chunk (gm200.c:290-292 + base.c:190-206 tag++).
	CHECK_EQ(bus.n_imemt,
		 (fx->bl.boot_size + hs->nmem_size + hs->imem_size) /
			 PG106_FALCON_PIO_CHUNK);
	CHECK_EQ(bus.imemt_tags[0], fx->bl.boot_addr >> 8);
	CHECK_EQ(bus.imemt_tags[1], (fx->bl.boot_addr >> 8) + 1);
	printf("PIO tags: BL starts at 0x%x (start_tag), 3 streams, %zu "
	       "0x100-byte chunks total\n",
	       bus.imemt_tags[0], bus.n_imemt);

	// The BL data words in the IMEMD stream must be the real bl.bin boot
	// code: the first 64 words of the first stream (right after the
	// first IMEMT) replay bl->boot_code. Walk the step list directly.
	{
		size_t first_imemt = (size_t)-1;
		size_t w = 0;

		for (i = 0; i < g_plan.nsteps; i++) {
			const struct pg106_acrboot_step *s = &g_plan.steps[i];

			if (s->space == PG106_ACRBOOT_MMIO &&
			    s->addr == fx->cfg.falcon_base + PG106_FALCON_IMEMT) {
				first_imemt = i;
				break;
			}
		}
		CHECK(first_imemt != (size_t)-1);
		for (i = first_imemt + 1;
		     i < g_plan.nsteps && w < fx->bl.boot_size / 4; i++) {
			const struct pg106_acrboot_step *s = &g_plan.steps[i];

			if (s->space == PG106_ACRBOOT_MMIO &&
			    s->addr == fx->cfg.falcon_base + PG106_FALCON_IMEMD) {
				CHECK_EQ(s->value,
					 rd32le(fx->bl.boot_code, w * 4));
				w++;
			}
		}
		CHECK_EQ(w, fx->bl.boot_size / 4);
	}

	// The BL DMEM arg block (first DMEMD stream) must equal
	// pg106_acr_hsfw_bld_build()'s output — the acr.c builder fed with
	// the same vma (gm200.c:227-241).
	{
		uint8_t bld[PG106_FLCN_BL_DMEM_DESC_V1_SIZE];
		size_t first_dmemd = (size_t)-1;
		size_t w = 0;

		CHECK_EQ(pg106_acr_hsfw_bld_build(&fx->hs, fx->cfg.vma_addr,
						  bld), 0);
		for (i = 0; i < g_plan.nsteps; i++) {
			const struct pg106_acrboot_step *s = &g_plan.steps[i];

			if (s->space == PG106_ACRBOOT_MMIO &&
			    s->addr == fx->cfg.falcon_base + PG106_FALCON_DMEMD) {
				first_dmemd = i;
				break;
			}
		}
		CHECK(first_dmemd != (size_t)-1);
		for (i = first_dmemd;
		     i < g_plan.nsteps &&
		     w < PG106_FLCN_BL_DMEM_DESC_V1_SIZE / 4;
		     i++) {
			const struct pg106_acrboot_step *s = &g_plan.steps[i];

			if (s->space == PG106_ACRBOOT_MMIO &&
			    s->addr == fx->cfg.falcon_base + PG106_FALCON_DMEMD) {
				CHECK_EQ(s->value, rd32le(bld, w * 4));
				w++;
			}
		}
		CHECK_EQ(w, PG106_FLCN_BL_DMEM_DESC_V1_SIZE / 4);
		// Spot-check the args block itself (gm200.c:227-237).
		CHECK_EQ(rd32le(bld, 32), PG106_FALCON_DMAIDX_VIRT);
		CHECK_EQ(rd32le(bld, 36), (uint32_t)fx->cfg.vma_addr);
		CHECK_EQ(rd32le(bld, 64),
			 (uint32_t)(fx->cfg.vma_addr + hs->dmem_base_img));
	}
}

// --- 4. image-space steps: sig patch + ACR desc setup ----------------------------

static void
test_img_steps(struct fixture *fx)
{
	const struct pg106_acr_hs_ucode *hs = &fx->hs;
	size_t i;
	size_t n_desc_writes = 0;

	// Sig patch: one blob step at sig_base_img carrying the REAL sig
	// bytes from blob[sig_prod_off] (fw.c:58 + fw.c:258-260).
	for (i = 0; i < g_plan.nsteps; i++) {
		const struct pg106_acrboot_step *s = &g_plan.steps[i];

		if (s->space == PG106_ACRBOOT_IMG &&
		    s->kind == PG106_ACRBOOT_WRITE_BLOB && s->blob) {
			CHECK_EQ(s->addr, hs->sig_base_img);
			CHECK_EQ(s->blob_len, hs->sig_size);
			CHECK(s->blob >= fx->hs_blob &&
			      s->blob + s->blob_len <=
				      fx->hs_blob + fx->hs_size);
			CHECK_EQ((size_t)(s->blob - fx->hs_blob),
				 hs->sig_prod_off);
			CHECK(memcmp(s->blob, fx->hs_blob + hs->sig_prod_off,
				     hs->sig_size) == 0);
		}
	}

	// ACR desc setup (gp102_acr_load_setup): 9 u32 writes at
	// dmem_base_img + {0x210, 0x21c, 0x220..0x238}.
	{
		static const struct {
			uint64_t off;
			uint32_t val;
		} want[] = {
			{ 0x210, 1 }, // wpr_region_id
			{ 0x21c, 2 }, // no_regions
			{ 0x220, 0 }, // start = wpr_start>>8 (below)
			{ 0x224, 0 }, // end   = wpr_end>>8   (below)
			{ 0x228, 1 }, // region_id
			{ 0x22c, 0xf }, // read_mask
			{ 0x230, 0xc }, // write_mask
			{ 0x234, 0x2 }, // client_mask
			{ 0x238, 0 }, // shadow_mem_start (below)
		};
		uint32_t got[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };
		size_t k;

		for (i = 0; i < g_plan.nsteps; i++) {
			const struct pg106_acrboot_step *s = &g_plan.steps[i];

			if (s->space != PG106_ACRBOOT_IMG ||
			    s->kind != PG106_ACRBOOT_WRITE_REG)
				continue;
			for (k = 0; k < 9; k++)
				if (s->addr == hs->dmem_base_img + want[k].off)
					got[k] = s->value;
		}
		CHECK_EQ(got[0], 1);
		CHECK_EQ(got[1], 2);
		CHECK_EQ(got[2], (uint32_t)(g_info.wpr_start >> 8));
		CHECK_EQ(got[3], (uint32_t)(g_info.wpr_end >> 8));
		CHECK_EQ(got[4], 1);
		CHECK_EQ(got[5], 0xf);
		CHECK_EQ(got[6], 0xc);
		CHECK_EQ(got[7], 0x2);
		CHECK_EQ(got[8], (uint32_t)(g_info.shadow_start >> 8));
		n_desc_writes = 9;
		(void)want;
	}
	CHECK_EQ(n_desc_writes, 9);
}

// --- 5. WPR shadow steps ---------------------------------------------------------

static void
test_wpr_steps(struct fixture *fx)
{
	const struct pg106_acrboot_plan *p = &g_plan;
	struct pg106_acr_lsfw two[2];
	size_t i, n_wpr_headers = 0, n_wpr_lsbs = 0, n_wpr_imgs = 0;

	// Layout arithmetic must match the acr.c parser's gp102 port exactly.
	// Recompute the SAME two-LSF layout the plan used for the goldens.
	make_fecs_lsfw(&two[0]);
	make_gpccs_lsfw(&two[1]);
	CHECK_EQ(pg106_acr_wpr_layout(two, 2, &(uint32_t){ 0 }), 0);

	// WPR geometry: gp102_acr_wpr_alloc (gp102.c:118-131).
	CHECK_EQ(g_info.wpr_size, 0x6c00); // two 0x2b00-image LSFs (golden)
	CHECK_EQ(g_info.alloc_size, 0x80000); // ALIGN(0x6c00,0x40000)<<1
	CHECK_EQ(g_info.shadow_start, fx->cfg.wpr_alloc_base);
	CHECK_EQ(g_info.wpr_start, fx->cfg.wpr_alloc_base + 0x40000);
	CHECK_EQ(g_info.wpr_end, fx->cfg.wpr_alloc_base + 0x80000);

	for (i = 0; i < p->nsteps; i++) {
		const struct pg106_acrboot_step *s = &p->steps[i];

		if (s->space != PG106_ACRBOOT_WPR)
			continue;
		if (s->kind != PG106_ACRBOOT_WRITE_BLOB)
			continue;
		if (s->blob_len == 24) {
			// wpr_header_v1 (gp102.c:86-93, 96).
			CHECK_EQ(s->addr, n_wpr_headers * 24u);
			CHECK_EQ(rd32le(s->blob, 0),
				 n_wpr_headers == 0 ? PG106_ACR_LSF_FECS
						    : PG106_ACR_LSF_GPCCS);
			CHECK_EQ(rd32le(s->blob, 8), PG106_ACR_LSF_SEC2);
			CHECK_EQ(rd32le(s->blob, 20),
				 PG106_ACR_WPR_STATUS_COPY);
			CHECK_EQ(rd32le(s->blob, 16), 2); // bin_version
			n_wpr_headers++;
		} else if (s->blob_len == 240) {
			// lsb_header_v1: signature must be the REAL sig file
			// bytes (gp102.c:69), tail carries the layout offsets
			// (gm200.c:94-111).
			const struct pg106_acr_lsfw *l = n_wpr_lsbs == 0
							     ? &two[0]
							     : &two[1];
			const struct pg106_acr_lsf_signature_v1 *sg =
				n_wpr_lsbs == 0 ? &fx->fecs_sig
						: &fx->gpccs_sig;

			CHECK(memcmp(s->blob, sg, 192) == 0);
			CHECK_EQ(rd32le(s->blob, 192 + 0), l->offset.img);
			CHECK_EQ(rd32le(s->blob, 192 + 20), l->offset.bld);
			CHECK_EQ(rd32le(s->blob, 192 + 44), l->flags);
			n_wpr_lsbs++;
		} else if (s->blob_len == 4) {
			CHECK(is_wpr_terminator(s));
		} else {
			// image / bld zero-fills (sim has no LS image bytes)
			CHECK(s->blob == NULL);
			CHECK(s->blob_len > 0);
			n_wpr_imgs++;
		}
	}
	CHECK_EQ(n_wpr_headers, 2);
	CHECK_EQ(n_wpr_lsbs, 2);
	CHECK_EQ(n_wpr_imgs, 4); // 2 img + 2 bld zero-fills
	printf("WPR shadow: 2 wpr_headers + 2 lsb_headers (real sig bytes) "
	       "+ 2 image + 2 bld blocks + terminator\n");
}

// --- 6. provenance annotation coverage --------------------------------------------

static void
test_annotations(void)
{
	size_t i, n_no_why = 0, n_nouveau_refs = 0;

	for (i = 0; i < g_plan.nsteps; i++) {
		const struct pg106_acrboot_step *s = &g_plan.steps[i];

		if (!s->why || !s->why[0])
			n_no_why++;
		else if (strstr(s->why, "nvkm/") || strstr(s->why, "nvkm_"))
			n_nouveau_refs++;
	}
	// (c) every step annotated.
	CHECK_EQ(n_no_why, 0);
	// And every annotation names its nouveau origin.
	CHECK_EQ(n_nouveau_refs, g_plan.nsteps);
	printf("annotations: %zu steps, all carry nvkm file:line provenance\n",
	       g_plan.nsteps);
}

// --- 7. the structural gate is real: corrupt blob -> acr.c rejects ---------------

static void
test_gate_rejects_corrupt(const char *dir)
{
	struct pg106_acr_hs_ucode hs;
	uint8_t *blob;
	size_t size;

	// Flip the container magic of a copy of ucode_load.bin: acr.c must
	// reject it, i.e. the plan could never be built from a bad blob.
	blob = load_blob(dir, "acr/ucode_load.bin", &size);
	CHECK(blob);
	if (!blob)
		return;
	blob[0] ^= 0xff;
	CHECK(pg106_acr_ucode_parse_hs(blob, size, &hs) < 0);
	free(blob);

	// Truncation is likewise rejected before any plan could exist.
	blob = load_blob(dir, "acr/bl.bin", &size);
	CHECK(blob);
	if (!blob)
		return;
	{
		struct pg106_acr_bl_ucode bl;

		CHECK(pg106_acr_ucode_parse_bl(blob, size - 1, &bl) < 0);
	}
	free(blob);
}

// --- 8. argument validation --------------------------------------------------------

static void
test_arg_validation(struct fixture *fx)
{
	struct pg106_acr_lsfw lsfws[PG106_ACR_LSF_MAX + 1];
	const struct pg106_acr_lsf_signature_v1 *sigs[PG106_ACR_LSF_MAX + 1];
	struct pg106_acrboot_plan plan = { 0 };
	struct pg106_acrboot_info info;
	struct pg106_acrboot_config cfg = fx->cfg;
	size_t i;
	int ret;

	make_fecs_lsfw(&lsfws[0]);
	for (i = 0; i < PG106_ACR_LSF_MAX + 1; i++) {
		lsfws[i] = lsfws[0];
		sigs[i] = &fx->fecs_sig;
	}

	// Too many LSFs (gp102.c:139 caps at 11 WPR header slots).
	ret = pg106_acrboot_plan_build(&cfg, &fx->bl, &fx->hs, lsfws,
				       PG106_ACR_LSF_MAX + 1, sigs, &plan,
				       &info);
	CHECK_EQ(ret, -EINVAL);

	// code_limit must hold the BL (gm200.c:291 loads it at
	// code_limit - boot_size).
	cfg = fx->cfg;
	cfg.code_limit = 0x100;
	ret = pg106_acrboot_plan_build(&cfg, &fx->bl, &fx->hs, lsfws, 1,
				       sigs, &plan, &info);
	CHECK_EQ(ret, -EINVAL);

	// NULL args.
	CHECK_EQ(pg106_acrboot_plan_build(NULL, &fx->bl, &fx->hs, lsfws, 1,
					  sigs, &plan, &info), -EINVAL);
	CHECK_EQ(pg106_acrboot_plan_build(&cfg, NULL, &fx->hs, lsfws, 1,
					  sigs, &plan, &info), -EINVAL);
	CHECK_EQ(pg106_acrboot_plan_build(&cfg, &fx->bl, &fx->hs, NULL, 1,
					  sigs, &plan, &info), -EINVAL);

	// Signature/falcon mismatch is rejected by the LSB builder inside
	// the plan (sig->falcon_id != lsfw->id, acr.c's stricter check).
	{
		const struct pg106_acr_lsf_signature_v1 *wrong[1] = {
			&fx->gpccs_sig,
		};

		ret = pg106_acrboot_plan_build(&cfg, &fx->bl, &fx->hs, lsfws,
					       1, wrong, &plan, &info);
		CHECK_EQ(ret, -EINVAL);
	}

	pg106_acrboot_plan_free(&plan); // free of a never-built plan is safe
}

// --- main ---------------------------------------------------------------------------

int
main(void)
{
	const char *dir = fw_dir();
	struct fixture fx;

	if (!dir) {
		printf("FAIL: research/firmware/gp106 not found (run from "
		       "pascal/ or repo root)\n");
		return 1;
	}

	printf("**********************************************************************\n");
	printf("**  SIMULATION, NOT HARDWARE PROOF (Phase 2 gate wording).         **\n");
	printf("**  Modeled falcon boot sequence vs a mock bus; real blobs only.   **\n");
	printf("**********************************************************************\n");
	printf("== pg106 ACR boot-sequence simulator tests (blobs: %s)\n",
	       dir);

	// (d) blob validation gate.
	if (fixture_setup(&fx, dir) != 0) {
		printf("== acr_boot: FAILURE(S): %d\n", g_failures);
		return 1;
	}

	test_plan_build_and_replay(&fx);
	if (g_plan.nsteps) {
		test_ordering();
		test_byte_counts(&fx);
		test_img_steps(&fx);
		test_wpr_steps(&fx);
	}
	test_annotations();
	test_gate_rejects_corrupt(dir);
	test_arg_validation(&fx);

	pg106_acrboot_plan_free(&g_plan);
	free(fx.bl_blob);
	free(fx.hs_blob);
	free(fx.unl_bl_blob);
	free(fx.unl_hs_blob);
	free(fx.fecs_sig_blob);
	free(fx.gpccs_sig_blob);

	if (g_failures) {
		printf("== acr_boot: %d FAILURE(S)\n", g_failures);
		return 1;
	}
	printf("== acr_boot: PASS (simulation only — no hardware claims)\n");
	return 0;
}
