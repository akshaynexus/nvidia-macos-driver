// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/src/acr_boot.c — Phase 2 ACR secure-boot SIMULATOR (see
// acr_boot.h for the design and the SIMULATION, NOT HARDWARE PROOF label).
//
// Builds the ordered falcon-loading step list for the gp102/r352 ACR boot of
// the SEC2 falcon, reusing the real-blob parsers in acr.c — no blob parsing
// is duplicated here; this file only models the *sequence* nouveau executes
// around those blobs. Every emitted step is annotated with its upstream
// origin (nvkm file:line of torvalds/linux master d48723711bae, 2026-11-06,
// relative to drivers/gpu/drm/nouveau/).
//
// Derived from (linux master reference copies):
//   nvkm/falcon/fw.c       nvkm_falcon_fw_boot order: patch -> reset ->
//                          setup -> load -> boot (fw.c:73-124)
//   nvkm/falcon/gm200.c    PIO IMEM/DMEM register sequences, fw_load, fw_boot
//   nvkm/falcon/gp102.c    gp102_flcn_reset_eng
//   nvkm/falcon/v1.c       nvkm_falcon_v1_start (CPUCTL START)
//   nvkm/subdev/acr/gp102.c  gp102_acr_load_setup, gp102_acr_wpr_*,
//                            gp102_acr_load_fwif (boot_mbox0/intr_clear)
//   nvkm/subdev/acr/gm200.c  gm200_acr_hsfw_load_bld, gm200_acr_wpr_check,
//                            gm200_acr_wpr_build (per-LSF shadow writes)
//   nvkm/subdev/acr/base.c   nvkm_acr_oneinit layout->alloc->build order,
//                            nvkm_acr_load init->wpr_check order
//   nvkm/subdev/top/{base.c,gk104.c} + nvkm/falcon/base.c:261-264
//                            the nvkm_top table reader feeding the falcon
//                            base (pg106_acrboot_falcon_base)
//   nvkm/falcon/{cmdq.c,msgq.c,qmgr.c}, nvkm/engine/sec2/{base.c,gp102.c}
//                            the SEC2 queue executor (pg106_sec2_*)

#include <pascal/acr_boot.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

// Provenance annotations are STATIC strings: the dynamic values an
// annotation would embed (imem/dmem base, tag, falcon id, WPR offsets) are
// already carried by the step's own addr/value/mask/blob_len fields, and
// the portable core must not pull in <stdio.h> (no printf family in a
// kext; enforced by pascal/kext/scripts/symbol-audit.sh).

// Allocation granularity of the WPR + shadow allocation
// (nvkm/subdev/acr/gp102.c:122: ALIGN(wpr_size, 0x40000) << 1).
#define WPR_ALIGN 0x40000ull

// Byte-serializers for the headers the plan writes into the WPR shadow
// buffer. Little-endian on the wire (same discipline as acr.c's rd32/wr32).
static void
ser32(uint8_t *b, size_t off, uint32_t v)
{
	b[off + 0] = (uint8_t)v;
	b[off + 1] = (uint8_t)(v >> 8);
	b[off + 2] = (uint8_t)(v >> 16);
	b[off + 3] = (uint8_t)(v >> 24);
}

static uint32_t
peek32(const uint8_t *b, size_t off)
{
	return (uint32_t)b[off] | ((uint32_t)b[off + 1] << 8) |
	       ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 3] << 24);
}

// ---------------------------------------------------------------------------
// Plan construction
// ---------------------------------------------------------------------------

struct builder {
	struct pg106_acrboot_plan *plan;
	size_t cap; // allocated step capacity
	int failed;
};

// Append one step. All steps funnel through here so the invariant "why is
// always non-empty" is enforced at the source.
static void
emit(struct builder *b, const struct pg106_acrboot_step *s)
{
	if (b->failed)
		return;
	if (!s->why || !s->why[0]) {
		b->failed = 1; // every step must carry its provenance
		return;
	}
	if (b->plan->nsteps == b->cap) {
		size_t newcap = b->cap ? b->cap * 2 : 64;
		struct pg106_acrboot_step *p =
			realloc(b->plan->steps, newcap * sizeof(*p));

		if (!p) {
			b->failed = 1;
			return;
		}
		b->plan->steps = p;
		b->cap = newcap;
	}
	b->plan->steps[b->plan->nsteps++] = *s;
}

// Scratch buffer, owned by the plan (freed by pg106_acrboot_plan_free).
// Returns NULL on OOM (builder latches failure).
static uint8_t *
scratch_new(struct builder *b, size_t len)
{
	uint8_t *p = calloc(1, len ? len : 1);
	void **list;

	if (!p) {
		b->failed = 1;
		return NULL;
	}
	list = realloc(b->plan->scratch_,
		       (b->plan->nscratch_ + 1) * sizeof(*list));
	if (!list) {
		free(p);
		b->failed = 1;
		return NULL;
	}
	b->plan->scratch_ = list;
	list[b->plan->nscratch_++] = p;
	return p;
}

// MMIO register write at falcon_base + off.
static void
reg_write(struct builder *b, const struct pg106_acrboot_config *cfg,
	  uint32_t off, uint32_t value, const char *why)
{
	emit(b, &(struct pg106_acrboot_step){
		    .kind = PG106_ACRBOOT_WRITE_REG,
		    .space = PG106_ACRBOOT_MMIO,
		    .addr = cfg->falcon_base + off,
		    .value = value,
		    .why = why,
		});
}

// BAR0 register write at a full address (PFB WPR-check window).
static void
reg_write_abs(struct builder *b, uint64_t addr, uint32_t value,
	      const char *why)
{
	emit(b, &(struct pg106_acrboot_step){
		    .kind = PG106_ACRBOOT_WRITE_REG,
		    .space = PG106_ACRBOOT_MMIO,
		    .addr = addr,
		    .value = value,
		    .why = why,
		});
}

static void
reg_poll(struct builder *b, const struct pg106_acrboot_config *cfg,
	 uint32_t off, uint32_t mask, uint32_t value, uint8_t op,
	 uint32_t timeout_us, const char *why)
{
	emit(b, &(struct pg106_acrboot_step){
		    .kind = PG106_ACRBOOT_POLL_REG,
		    .space = PG106_ACRBOOT_MMIO,
		    .poll_op = op,
		    .addr = cfg->falcon_base + off,
		    .value = value,
		    .mask = mask,
		    .timeout_us = timeout_us,
		    .why = why,
		});
}

// nvkm_falcon_mask / nvkm_mask read-modify-write at a full BAR0 address.
static void
mask_abs(struct builder *b, uint64_t addr, uint32_t mask, uint32_t value,
	 const char *why)
{
	emit(b, &(struct pg106_acrboot_step){
		    .kind = PG106_ACRBOOT_MASK_REG,
		    .space = PG106_ACRBOOT_MMIO,
		    .addr = addr,
		    .value = value,
		    .mask = mask,
		    .why = why,
		});
}

static void
reg_mask(struct builder *b, const struct pg106_acrboot_config *cfg,
	 uint32_t off, uint32_t mask, uint32_t value, const char *why)
{
	mask_abs(b, cfg->falcon_base + off, mask, value, why);
}

static void
delay_us(struct builder *b, uint32_t us, const char *why)
{
	emit(b, &(struct pg106_acrboot_step){
		    .kind = PG106_ACRBOOT_DELAY_US,
		    .space = PG106_ACRBOOT_MMIO,
		    .value = us,
		    .why = why,
		});
}

// Register read-back at falcon_base + off (or a full address when full_addr
// is true — the PFB WPR-check window lives outside the falcon window).
static void
reg_read(struct builder *b, const struct pg106_acrboot_config *cfg,
	 uint64_t off, bool full_addr, uint32_t mask, uint32_t expect,
	 const char *why)
{
	emit(b, &(struct pg106_acrboot_step){
		    .kind = PG106_ACRBOOT_READ_REG,
		    .space = PG106_ACRBOOT_MMIO,
		    .addr = full_addr ? off : cfg->falcon_base + (uint32_t)off,
		    .value = expect,
		    .mask = mask,
		    .why = why,
		});
}

// Object-relative write into the WPR shadow buffer / HS image buffer.
static void
obj_write(struct builder *b, uint8_t space, uint64_t off, uint32_t value,
	  const char *why)
{
	emit(b, &(struct pg106_acrboot_step){
		    .kind = PG106_ACRBOOT_WRITE_REG,
		    .space = space,
		    .addr = off,
		    .value = value,
		    .why = why,
		});
}

static void
obj_blob(struct builder *b, uint8_t space, uint64_t off, const uint8_t *blob,
	 uint32_t len, const char *why)
{
	emit(b, &(struct pg106_acrboot_step){
		    .kind = PG106_ACRBOOT_WRITE_BLOB,
		    .space = space,
		    .addr = off,
		    .blob = blob,
		    .blob_len = len,
		    .why = why,
		});
}

// Expand one nvkm_falcon_pio_wr() IMEM transfer into register steps:
// gm200_flcn_pio_imem_wr_init (gm200.c:98-101) writes IMEMC =
// (sec?BIT(28):0)|BIT(24)|imem_base once; each <=0x100-byte chunk writes
// IMEMT = tag then IMEMD u32 words (gm200.c:104-112); the chunk loop is
// nvkm_falcon_pio_wr (base.c:175-209) with imem_pio.min/.max = 0x100
// (gm200.c:114-120). `why_ctl` names the stream this transfer belongs to.
static void
expand_imem_pio(struct builder *b, const struct pg106_acrboot_config *cfg,
		const uint8_t *data, uint32_t len, uint32_t imem_base,
		uint16_t tag, bool sec, const char *why_ctl)
{
	reg_write(b, cfg, PG106_FALCON_IMEMC,
		  (sec ? 0x10000000u : 0u) | 0x01000000u | imem_base, why_ctl);

	{
		uint32_t off = 0;
		uint16_t t = tag;

		while (off < len) {
			uint32_t chunk = len - off < PG106_FALCON_PIO_CHUNK
					 ? len - off
					 : PG106_FALCON_PIO_CHUNK;
			uint32_t i;

			reg_write(b, cfg, PG106_FALCON_IMEMT, t,
				  "nvkm/falcon/gm200.c:106 "
				  "gm200_flcn_pio_imem_wr - IMEMT=tag "
				  "(value carries it; tag++ per 0x100-byte "
				  "chunk, nvkm/falcon/base.c:190-206)");

			for (i = 0; i < chunk / 4; i++)
				reg_write(b, cfg, PG106_FALCON_IMEMD,
					  peek32(data, off + i * 4),
					  "nvkm/falcon/gm200.c:108 "
					  "gm200_flcn_pio_imem_wr - IMEMD "
					  "u32 data word (value carries it)");
			off += chunk;
			t++;
		}
	}
}

// Expand one nvkm_falcon_pio_wr() DMEM transfer into register steps:
// gm200_flcn_pio_dmem_wr_init (gm200.c:82-85) writes DMEMC = BIT(24)|base
// once, then DMEMD u32 words (gm200.c:70-79). dmem_pio.max = 0x100
// (gm200.c:89-90). `why_ctl` names the stream this transfer belongs to.
static void
expand_dmem_pio(struct builder *b, const struct pg106_acrboot_config *cfg,
		const uint8_t *data, uint32_t len, uint32_t dmem_base,
		const char *why_ctl)
{
	reg_write(b, cfg, PG106_FALCON_DMEMC, 0x01000000u | dmem_base,
		  why_ctl);

	{
		uint32_t off = 0;

		while (off < len) {
			uint32_t chunk = len - off < PG106_FALCON_PIO_CHUNK
					 ? len - off
					 : PG106_FALCON_PIO_CHUNK;
			uint32_t i;

			for (i = 0; i < chunk / 4; i++)
				reg_write(b, cfg, PG106_FALCON_DMEMD,
					  peek32(data, off + i * 4),
					  "nvkm/falcon/gm200.c:73 "
					  "gm200_flcn_pio_dmem_wr - DMEMD "
					  "u32 data word (value carries it)");
			off += chunk;
		}
	}
}

// Serialize struct wpr_header_v1 into a scratch buffer (fields per
// include/nvfw/acr.h:22-38; values per gp102_acr_wpr_build, gp102.c:86-93,
// built by pg106_acr_wpr_header_build in acr.c).
static uint8_t *
ser_wpr_header(struct builder *b, const struct pg106_acr_wpr_header_v1 *h)
{
	uint8_t *buf = scratch_new(b, 24);

	if (!buf)
		return NULL;
	ser32(buf, 0, h->falcon_id);
	ser32(buf, 4, h->lsb_offset);
	ser32(buf, 8, h->bootstrap_owner);
	ser32(buf, 12, h->lazy_bootstrap);
	ser32(buf, 16, h->bin_version);
	ser32(buf, 20, h->status);
	return buf;
}

// Serialize struct lsb_header_v1 (240 bytes = 192-byte signature + 48-byte
// tail) into a scratch buffer, mirroring the nvkm_wobj of the built header
// (gp102_acr_wpr_build_lsb, gp102.c:72). Layout per include/nvfw/acr.h
// (:67-78 signature, :80-93 tail).
static uint8_t *
ser_lsb_header(struct builder *b, const struct pg106_acr_lsb_header_v1 *h)
{
	uint8_t *buf = scratch_new(b, 240);

	if (!buf)
		return NULL;
	memcpy(buf + 0, h->signature.prd_keys, 32);   // prd_keys[2][16] @0
	memcpy(buf + 32, h->signature.dbg_keys, 32);  // dbg_keys[2][16] @32
	ser32(buf, 64, h->signature.b_prd_present);
	ser32(buf, 68, h->signature.b_dbg_present);
	ser32(buf, 72, h->signature.falcon_id);
	ser32(buf, 76, h->signature.supports_versioning);
	ser32(buf, 80, h->signature.version);
	ser32(buf, 84, h->signature.depmap_count);
	memcpy(buf + 88, h->signature.depmap, 88);    // depmap @88
	memcpy(buf + 176, h->signature.kdf, 16);      // kdf @176
	ser32(buf, 192 + 0, h->tail.ucode_off);	      // lsb_header_tail @192
	ser32(buf, 192 + 4, h->tail.ucode_size);
	ser32(buf, 192 + 8, h->tail.data_size);
	ser32(buf, 192 + 12, h->tail.bl_code_size);
	ser32(buf, 192 + 16, h->tail.bl_imem_off);
	ser32(buf, 192 + 20, h->tail.bl_data_off);
	ser32(buf, 192 + 24, h->tail.bl_data_size);
	ser32(buf, 192 + 28, h->tail.app_code_off);
	ser32(buf, 192 + 32, h->tail.app_code_size);
	ser32(buf, 192 + 36, h->tail.app_data_off);
	ser32(buf, 192 + 40, h->tail.app_data_size);
	ser32(buf, 192 + 44, h->tail.flags);
	return buf;
}

// ---------------------------------------------------------------------------
// Plan groups
// ---------------------------------------------------------------------------

// Group 1: nvkm_falcon_oneinit (nvkm/falcon/base.c:254-283), run by the
// first nvkm_falcon_get on the SEC2 falcon — nvkm_acr_oneinit's lsfw loop
// (acr/base.c:248) for the SEC2 LS firmware.
static void
plan_oneinit(struct builder *b, const struct pg106_acrboot_config *cfg)
{
	reg_read(b, cfg, PG106_FALCON_VERSION, false, 0, 0,
		 "nvkm/falcon/base.c:267-271 nvkm_falcon_oneinit - "
		 "rd32(0x12c): version/secret/ports (recorded, not gated)");
	reg_read(b, cfg, PG106_FALCON_MEM_CAPS, false, 0xffffffffu, cfg->hwcfg,
		 "nvkm/falcon/base.c:273-275 nvkm_falcon_oneinit - "
		 "rd32(0x108) HWCFG: code.limit = (r & 0x1ff) << 8, "
		 "data.limit = (r & 0x3fe00) >> 1");
	reg_read(b, cfg, PG106_FALCON_SEC2_DEBUG, false,
		 PG106_FALCON_SEC2_DEBUG_DBG,
		 cfg->debug_reg & PG106_FALCON_SEC2_DEBUG_DBG,
		 "nvkm/falcon/base.c:277-280 nvkm_falcon_oneinit - "
		 "falcon->debug = (rd32(func->debug = 0x408) >> 20) & 1 "
		 "(sec2/gp102.c:205)");
}

// gm200_flcn_reset_wait_mem_scrubbing (nvkm/falcon/gm200.c:140-152).
static void
plan_scrub_wait(struct builder *b, const struct pg106_acrboot_config *cfg)
{
	reg_mask(b, cfg, PG106_FALCON_MBOX0, 0, 0,
		 "nvkm/falcon/gm200.c:143 gm200_flcn_reset_wait_mem_"
		 "scrubbing - nvkm_falcon_mask(0x040, 0, 0) (read + write "
		 "back MBOX0)");
	reg_poll(b, cfg, PG106_FALCON_MEM_SCRUB, 0x00000006u, 0,
		 PG106_ACRBOOT_POLL_EQ, 10000,
		 "nvkm/falcon/gm200.c:145-149 gm200_flcn_reset_wait_mem_"
		 "scrubbing - wait until !(rd32(0x10c) & 0x6): IMEM/DMEM "
		 "scrub BUSY bits clear, 10 ms (-ETIMEDOUT otherwise)");
}

// gp102_flcn_reset_eng (nvkm/falcon/gp102.c:66-82); gp102_sec2_flcn has no
// .reset_prep.
static void
plan_reset_eng(struct builder *b, const struct pg106_acrboot_config *cfg)
{
	reg_mask(b, cfg, PG106_FALCON_GP102_RESET, 0x1u, 0x1u,
		 "nvkm/falcon/gp102.c:77 gp102_flcn_reset_eng - "
		 "nvkm_falcon_mask(0x3c0, 0x1, 0x1) assert engine reset");
	delay_us(b, 10, "nvkm/falcon/gp102.c:78 gp102_flcn_reset_eng - "
			"udelay(10)");
	reg_mask(b, cfg, PG106_FALCON_GP102_RESET, 0x1u, 0x0u,
		 "nvkm/falcon/gp102.c:79 gp102_flcn_reset_eng - "
		 "nvkm_falcon_mask(0x3c0, 0x1, 0x0) deassert");
	plan_scrub_wait(b, cfg);
}

// gm200_flcn_enable (nvkm/falcon/gm200.c:154-181) with gp102_sec2_flcn's
// hooks (reset_eng = gp102_flcn_reset_eng, reset_pmc = true, no select;
// sec2/gp102.c:198-204).
static void
plan_enable(struct builder *b, const struct pg106_acrboot_config *cfg)
{
	plan_reset_eng(b, cfg); // gm200.c:160-164
	if (cfg->pmc_mask) {
		// gm200.c:172-173 nvkm_mc_enable -> nv04_mc_device_enable
		// (nvkm/subdev/mc/nv04.c:40-46).
		mask_abs(b, PG106_PMC_ENABLE, cfg->pmc_mask, cfg->pmc_mask,
			 "nvkm/subdev/mc/nv04.c:44 nv04_mc_device_enable (via "
			 "nvkm_mc_enable, nvkm/falcon/gm200.c:173) - "
			 "nvkm_mask(0x200, sec2_bit, sec2_bit)");
		reg_read(b, cfg, PG106_PMC_ENABLE, true, 0, 0,
			 "nvkm/subdev/mc/nv04.c:45 nv04_mc_device_enable - "
			 "nvkm_rd32(0x200) posting read");
	}
	plan_scrub_wait(b, cfg); // gm200.c:175-177
	reg_read(b, cfg, PG106_PMC_BOOT_0, true, 0xffffffffu, cfg->pmc_boot0,
		 "nvkm/falcon/gm200.c:179 gm200_flcn_enable - "
		 "nvkm_rd32(device, 0x000000) (PMC_BOOT_0)");
	reg_write(b, cfg, PG106_FALCON_ENGINE_EN, cfg->pmc_boot0,
		  "nvkm/falcon/gm200.c:179 gm200_flcn_enable - "
		  "wr32(0x084, PMC_BOOT_0 value just read)");
}

// gm200_flcn_disable (nvkm/falcon/gm200.c:183-215).
static void
plan_disable(struct builder *b, const struct pg106_acrboot_config *cfg)
{
	reg_mask(b, cfg, PG106_FALCON_RM, 0x3u, 0x0u,
		 "nvkm/falcon/gm200.c:195 gm200_flcn_disable - "
		 "nvkm_falcon_mask(0x048, 0x3, 0x0)");
	reg_write(b, cfg, PG106_FALCON_INTR_EN, 0xffffffffu,
		  "nvkm/falcon/gm200.c:196 gm200_flcn_disable - "
		  "wr32(0x014, 0xffffffff) disable all interrupts");
	if (cfg->pmc_mask)
		// gm200.c:198-206 (reset_pmc, no reset_prep) nvkm_mc_disable
		// -> nv04_mc_device_disable (mc/nv04.c:34-37), no read-back.
		mask_abs(b, PG106_PMC_ENABLE, cfg->pmc_mask, 0,
			 "nvkm/subdev/mc/nv04.c:36 nv04_mc_device_disable "
			 "(via nvkm_mc_disable, nvkm/falcon/gm200.c:205) - "
			 "nvkm_mask(0x200, sec2_bit, 0)");
	plan_reset_eng(b, cfg); // gm200.c:208-212
}

// Group 3: nvkm_falcon_fw_patch (nvkm/falcon/fw.c:30-64) with
// gm200_flcn_fw_signature (gm200.c:323-342): enable the falcon, read the
// debug register, pick signature slot 1 (debug) when bit 20 is set. The
// patched bytes are fw->sigs[idx * sig_size], which nvkm_falcon_fw_sign
// filled from blob[sig_prod_off] (slot 0) and blob[sig_dbg_off] (slot 1),
// both sig_prod_size long (fw.c:186-192, fw.c:258-260).
static void
plan_sig_patch(struct builder *b, const struct pg106_acrboot_config *cfg,
	       const struct pg106_acr_hs_ucode *hs, bool dbg)
{
	const uint8_t *blob = hs->img - hs->hdr.data_offset;

	plan_enable(b, cfg); // gm200.c:331
	reg_read(b, cfg, PG106_FALCON_SEC2_DEBUG, false,
		 PG106_FALCON_SEC2_DEBUG_DBG,
		 cfg->debug_reg & PG106_FALCON_SEC2_DEBUG_DBG,
		 "nvkm/falcon/gm200.c:335-338 gm200_flcn_fw_signature - "
		 "rd32(0x408) & 0x00100000 -> debug signature (idx 1)");
	if (dbg)
		obj_blob(b, PG106_ACRBOOT_IMG, hs->sig_base_img,
			 blob + hs->sig_dbg_off, hs->sig_size,
			 "nvkm/falcon/fw.c:45-61 nvkm_falcon_fw_patch - idx 1: "
			 "DEBUG signature (sig_prod_size bytes copied from "
			 "blob[sig_dbg_offset + sig], fw.c:192,260) into "
			 "img[sig_base_img]");
	else
		obj_blob(b, PG106_ACRBOOT_IMG, hs->sig_base_img,
			 blob + hs->sig_prod_off, hs->sig_size,
			 "nvkm/falcon/fw.c:45-61 nvkm_falcon_fw_patch - idx 0: "
			 "PRODUCTION signature (blob[sig_prod_offset + sig], "
			 "fw.c:190,259) into img[sig_base_img]");
}

// Group 4: gm200_flcn_fw_reset -> nvkm_falcon_reset (gm200.c:317-321,
// base.c:242-252) = disable + nvkm_falcon_enable.
static void
plan_reset(struct builder *b, const struct pg106_acrboot_config *cfg)
{
	plan_disable(b, cfg);
	plan_enable(b, cfg);
}

// Group 5: gp102_acr_load_setup — patch the flcn_acr_desc_v1 living in the
// HS image's DMEM segment (gp102.c:196-213; desc at img[dmem_base_img],
// field offsets per include/nvfw/acr.h:208-234: 0x200 reserved + 4 u32
// signature slots = 0x210 base, region_props[0] at 0x220).
static void
plan_load_setup(struct builder *b, const struct pg106_acr_hs_ucode *hs,
		uint64_t shadow_start, uint64_t wpr_start, uint64_t wpr_end)
{
	const uint64_t d = hs->dmem_base_img;

	obj_write(b, PG106_ACRBOOT_IMG, d + 0x210, 1,
		  "nvkm/subdev/acr/gp102.c:202 gp102_acr_load_setup - "
		  "desc->wpr_region_id = 1");
	obj_write(b, PG106_ACRBOOT_IMG, d + 0x21c, 2,
		  "nvkm/subdev/acr/gp102.c:203 gp102_acr_load_setup - "
		  "desc->regions.no_regions = 2");
	obj_write(b, PG106_ACRBOOT_IMG, d + 0x220,
		  (uint32_t)(wpr_start >> 8),
		  "nvkm/subdev/acr/gp102.c:204 gp102_acr_load_setup - "
		  "region_props[0].start_addr = wpr_start >> 8");
	obj_write(b, PG106_ACRBOOT_IMG, d + 0x224,
		  (uint32_t)(wpr_end >> 8),
		  "nvkm/subdev/acr/gp102.c:205 gp102_acr_load_setup - "
		  "region_props[0].end_addr = wpr_end >> 8");
	obj_write(b, PG106_ACRBOOT_IMG, d + 0x228, 1,
		  "nvkm/subdev/acr/gp102.c:206 gp102_acr_load_setup - "
		  "region_props[0].region_id = 1");
	obj_write(b, PG106_ACRBOOT_IMG, d + 0x22c, 0xf,
		  "nvkm/subdev/acr/gp102.c:207 gp102_acr_load_setup - "
		  "region_props[0].read_mask = 0xf");
	obj_write(b, PG106_ACRBOOT_IMG, d + 0x230, 0xc,
		  "nvkm/subdev/acr/gp102.c:208 gp102_acr_load_setup - "
		  "region_props[0].write_mask = 0xc");
	obj_write(b, PG106_ACRBOOT_IMG, d + 0x234, 0x2,
		  "nvkm/subdev/acr/gp102.c:209 gp102_acr_load_setup - "
		  "region_props[0].client_mask = 0x2");
	obj_write(b, PG106_ACRBOOT_IMG, d + 0x238,
		  (uint32_t)(shadow_start >> 8),
		  "nvkm/subdev/acr/gp102.c:210 gp102_acr_load_setup - "
		  "region_props[0].shadow_mem_start_addr = shadow_start >> 8 "
		  "(v1 field gm200's desc lacks)");
}

// Group 6: gm200_flcn_fw_load (nvkm/falcon/gm200.c:248-315). The ACR load
// firmware has fw->boot (bl.bin), so after the instance bind only the BL is
// PIO'd and the BL argument block written; the function returns there
// (gm200.c:289-297). The HS image itself is DMA'd by the BL from the mapped
// image (code_dma_base in the argument block) — never PIO'd by the host.
static void
plan_load(struct builder *b, const struct pg106_acrboot_config *cfg,
	  const struct pg106_acr_bl_ucode *bl,
	  const struct pg106_acr_hs_ucode *hs, uint32_t code_limit)
{
	// --- instance-bound DMA path (fw->inst != NULL, gm200.c:254-283) ---
	reg_mask(b, cfg, PG106_FALCON_RM, 0x1u, 0x1u,
		 "nvkm/falcon/gm200.c:257 gm200_flcn_fw_load - "
		 "nvkm_falcon_mask(0x048, 0x1, 0x1) before binding inst");
	reg_mask(b, cfg, PG106_FALCON_DMAIDX, 0x7u, 0x0u,
		 "nvkm/falcon/gm200.c:134 gm200_flcn_bind_inst - "
		 "nvkm_falcon_mask(0x604, 0x7, 0x0) = DMAIDX_VIRT");
	reg_write(b, cfg, PG106_FALCON_BINDINST,
		  (1u << 30) | (cfg->bind_target << 28) |
			  (uint32_t)(cfg->bind_addr >> 12),
		  "nvkm/falcon/gm200.c:135 gm200_flcn_bind_inst - "
		  "wr32(0x054, (1<<30)|(target<<28)|(addr>>12))");
	reg_mask(b, cfg, PG106_FALCON_MEMBIND_CTL, 0x00010000u, 0x00010000u,
		 "nvkm/falcon/gm200.c:136 gm200_flcn_bind_inst - "
		 "nvkm_falcon_mask(0x090, 0x10000, 0x10000)");
	reg_mask(b, cfg, PG106_FALCON_MEMBIND_CTL2, 0x8u, 0x8u,
		 "nvkm/falcon/gm200.c:137 gm200_flcn_bind_inst - "
		 "nvkm_falcon_mask(0x0a4, 0x8, 0x8)");
	// gm200_flcn_bind_stat(falcon, bind_intr = true) returns -1 until
	// 0x008 bit 3 is set (gm200.c:125-126), then the 0x0dc state. The
	// bit stays latched until the 0x004 write below clears it, so two
	// sequential polls are equivalent to the single combined loop.
	reg_poll(b, cfg, PG106_FALCON_INTR_STAT, 0x00000008u, 0,
		 PG106_ACRBOOT_POLL_ANY, 10000,
		 "nvkm/falcon/gm200.c:125-126,270-274 gm200_flcn_bind_stat "
		 "(bind_intr = true, sec2/gp102.c:208) - wait for 0x008 bit 3 "
		 "(bind interrupt) before trusting 0x0dc, 10 ms");
	reg_poll(b, cfg, PG106_FALCON_BINDSTAT, 0x00007000u, 0x5000,
		 PG106_ACRBOOT_POLL_EQ, 10000,
		 "nvkm/falcon/gm200.c:128,270-274 gm200_flcn_fw_load - "
		 "poll bind_stat == 5 ((0x0dc & 0x7000) >> 12), 10 ms");
	reg_mask(b, cfg, PG106_FALCON_INTR, 0x8u, 0x8u,
		 "nvkm/falcon/gm200.c:276 gm200_flcn_fw_load - "
		 "nvkm_falcon_mask(0x004, 0x8, 0x8) clear bind intr");
	reg_mask(b, cfg, PG106_FALCON_BIND_TRIGGER, 0x2u, 0x2u,
		 "nvkm/falcon/gm200.c:277 gm200_flcn_fw_load - "
		 "nvkm_falcon_mask(0x058, 0x2, 0x2)");
	reg_poll(b, cfg, PG106_FALCON_BINDSTAT, 0x00007000u, 0x0000,
		 PG106_ACRBOOT_POLL_EQ, 10000,
		 "nvkm/falcon/gm200.c:128,279-283 gm200_flcn_fw_load - "
		 "poll bind_stat(intr = false) == 0, 10 ms");

	// --- HS bootloader PIO (fw->boot != NULL, gm200.c:289-294):
	// nvkm_falcon_pio_wr(fw->boot, IMEM, code.limit - boot_size,
	// boot_size, tag boot_addr >> 8, non-secure).
	expand_imem_pio(b, cfg, bl->boot_code, bl->boot_size,
			code_limit - bl->boot_size,
			(uint16_t)(bl->boot_addr >> 8), false,
			"nvkm/falcon/gm200.c:100,290-292 gm200_flcn_fw_load - "
			"BL to IMEM top: nvkm_falcon_pio_wr(fw->boot, IMEM, "
			"code.limit - boot_size, boot_size, tag "
			"boot_addr>>8, non-secure) -> IMEMC = BIT24|base");

	// --- BL DMEM argument block, then RETURN (gm200.c:296 ->
	// gm200_acr_hsfw_load_bld, nvkm/subdev/acr/gm200.c:224-242).
	{
		uint8_t *bld = scratch_new(b, PG106_FLCN_BL_DMEM_DESC_V1_SIZE);

		if (!bld)
			return;
		if (pg106_acr_hsfw_bld_build(hs, cfg->vma_addr, bld)) {
			b->failed = 1;
			return;
		}
		expand_dmem_pio(b, cfg, bld, PG106_FLCN_BL_DMEM_DESC_V1_SIZE,
				0,
				"nvkm/subdev/acr/gm200.c:241 gm200_acr_hsfw_"
				"load_bld (nvkm/falcon/gm200.c:296) - "
				"nvkm_falcon_pio_wr(flcn_bl_dmem_desc_v1, "
				"DMEM, 0, 76 bytes) -> DMEMC = BIT24|0; "
				"gm200_flcn_fw_load returns here");
	}
}

// Group 7: gm200_flcn_fw_boot (nvkm/falcon/gm200.c:217-246), called via
// nvkm_falcon_fw_boot (fw.c:114) from gm200_acr_init ->
// nvkm_acr_hsfw_boot("load") (acr/gm200.c:46-50; acr/base.c:55 passes
// pmbox0/pmbox1 = NULL, mbox0_ok = fwif boot_mbox0, irqsclr = intr_clear).
static void
plan_boot(struct builder *b, const struct pg106_acrboot_config *cfg,
	  const struct pg106_acr_bl_ucode *bl)
{
	reg_write(b, cfg, PG106_FALCON_MBOX0, 0xcafebeefu,
		  "nvkm/falcon/gm200.c:224 gm200_flcn_fw_boot - "
		  "wr32(0x040, pmbox0 ? *pmbox0 : 0xcafebeef); ACR boot "
		  "passes pmbox0=NULL (acr/base.c:55)");
	reg_write(b, cfg, PG106_FALCON_BOOTVEC, (uint32_t)bl->boot_addr,
		  "nvkm/falcon/gm200.c:228 gm200_flcn_fw_boot - "
		  "wr32(0x104, fw->boot_addr) (bl_desc start_tag<<8, "
		  "fw.c:289)");
	reg_write(b, cfg, PG106_FALCON_CPUCTL, 0x00000002u,
		  "nvkm/falcon/gm200.c:229 gm200_flcn_fw_boot - "
		  "wr32(0x100, 0x2) CPUCTL STARTCPU");
	reg_poll(b, cfg, PG106_FALCON_CPUCTL, 0x00000010u, 0,
		 PG106_ACRBOOT_POLL_ANY, 2000000,
		 "nvkm/falcon/gm200.c:231-235 gm200_flcn_fw_boot - poll "
		 "0x100 & 0x10 CPUCTL HALTED, 2 s");
	reg_read(b, cfg, PG106_FALCON_MBOX0, false, 0xffffffffu,
		 cfg->boot_mbox0,
		 "nvkm/falcon/gm200.c:237,239 gm200_flcn_fw_boot - mbox0 = "
		 "rd32(0x040) must equal mbox0_ok (boot_mbox0 = 0, "
		 "acr/gp102.c:239)");
	reg_read(b, cfg, PG106_FALCON_MBOX1, false, 0, 0,
		 "nvkm/falcon/gm200.c:238 gm200_flcn_fw_boot - mbox1 = "
		 "rd32(0x044) (logged only)");
	reg_mask(b, cfg, PG106_FALCON_INTR, 0xffffffffu, cfg->intr_clear,
		 "nvkm/falcon/gm200.c:242-243 gm200_flcn_fw_boot - "
		 "nvkm_falcon_mask(0x004, 0xffffffff, irqsclr = 0x10, "
		 "acr/gp102.c:239)");
}

// Group 2: WPR shadow build (gp102_acr_wpr_build, gp102.c:76-116) and
// wpr_patch (gp102.c:32-59), in nvkm_acr_oneinit order (acr/base.c:283-306:
// wpr_layout -> wpr_alloc -> wpr_build -> wpr_patch(wpr_start - wpr_prev)).
static int
plan_wpr_build(struct builder *b, const struct pg106_acrboot_config *cfg,
	       struct pg106_acr_lsfw *lsfws, size_t nlsfw,
	       const struct pg106_acr_lsf_signature_v1 *const *sigs,
	       uint64_t wpr_start)
{
	size_t i;

	for (i = 0; i < nlsfw; i++) {
		struct pg106_acr_lsfw *l = &lsfws[i];
		struct pg106_acr_wpr_header_v1 wh;
		struct pg106_acr_lsb_header_v1 lsb;
		uint8_t *buf;

		// WPR header (gp102.c:86-97, via pg106_acr_wpr_header_build).
		if (pg106_acr_wpr_header_build(l, cfg->bootstrap_owner,
					       cfg->rtos_id, sigs[i], &wh))
			return -EINVAL;
		buf = ser_wpr_header(b, &wh);
		if (!buf)
			return -ENOMEM;
		obj_blob(b, PG106_ACRBOOT_WPR, i * 24u, buf, 24,
			 "nvkm/subdev/acr/gp102.c:86-97 gp102_acr_wpr_build - "
			 "nvkm_wobj(wpr, i*24, wpr_header_v1 {falcon_id, "
			 "lsb_offset, bootstrap_owner = SEC2, lazy_bootstrap = "
			 "rtos && id != rtos->id, bin_version = sig->version, "
			 "status = COPY})");

		// LSB header: real signature + derived tail (gp102.c:61-74;
		// tail per gm200.c:93-111).
		if (pg106_acr_lsb_header_build(l, sigs[i], &lsb))
			return -EINVAL;
		buf = ser_lsb_header(b, &lsb);
		if (!buf)
			return -ENOMEM;
		obj_blob(b, PG106_ACRBOOT_WPR, l->offset.lsb, buf, 240,
			 "nvkm/subdev/acr/gp102.c:69-72 gp102_acr_wpr_build_lsb"
			 " - nvkm_wobj(wpr, offset.lsb, lsb_header_v1 = sig file "
			 "+ gm200_acr_wpr_build_lsb_tail, gm200.c:93-111)");

		// LS ucode image (gp102.c:104-107).
		if (l->img)
			obj_blob(b, PG106_ACRBOOT_WPR, l->offset.img, l->img,
				 (uint32_t)l->img_size,
				 "nvkm/subdev/acr/gp102.c:104-107 gp102_acr_"
				 "wpr_build - nvkm_wobj(wpr, offset.img, "
				 "lsfw->img.data, img.size)");
		else
			obj_blob(b, PG106_ACRBOOT_WPR, l->offset.img, NULL,
				 (uint32_t)l->img_size,
				 "nvkm/subdev/acr/gp102.c:104-107 gp102_acr_"
				 "wpr_build - nvkm_wobj(wpr, offset.img, ...) "
				 "[SIM: caller gave no image bytes; zero-fill "
				 "keeps the layout arithmetic]");

		// Bootloader data block (gp102.c:110 lsfw->func->bld_write),
		// then wpr_patch's bld_patch adding wpr_start (gp102.c:51).
		if (l->bld_kind != PG106_ACR_BLD_NONE) {
			uint8_t *w = scratch_new(b, PG106_FLCN_BL_DMEM_DESC_V2_SIZE);
			uint8_t *pt = scratch_new(b, PG106_FLCN_BL_DMEM_DESC_V2_SIZE);
			size_t n = 0, n2 = 0;

			if (!w || !pt)
				return -ENOMEM;
			if (pg106_acr_lsfw_bld_build(l, 0, w,
						     PG106_FLCN_BL_DMEM_DESC_V2_SIZE,
						     &n) ||
			    pg106_acr_lsfw_bld_build(l, (int64_t)wpr_start, pt,
						     PG106_FLCN_BL_DMEM_DESC_V2_SIZE,
						     &n2))
				return -EINVAL;
			obj_blob(b, PG106_ACRBOOT_WPR, l->offset.bld, w,
				 (uint32_t)n,
				 l->bld_kind == PG106_ACR_BLD_GR
				 ? "nvkm/engine/gr/gm200.c:56-74 gm200_gr_acr_"
				   "bld_write (gp102.c:110) - "
				   "flcn_bl_dmem_desc_v1 {ctx_dma UCODE, "
				   "code/data_dma_base WPR-relative}"
				 : "nvkm/engine/sec2/gp102.c:251-269 gp102_sec2_"
				   "acr_bld_write_1 (gp102.c:110) - "
				   "flcn_bl_dmem_desc_v2 {ctx_dma SEC2_UCODE, "
				   "argc 1, argv EMEM 0x01000000}");
			obj_blob(b, PG106_ACRBOOT_WPR, l->offset.bld, pt,
				 (uint32_t)n2,
				 l->bld_kind == PG106_ACR_BLD_GR
				 ? "nvkm/subdev/acr/gp102.c:51 gp102_acr_wpr_"
				   "patch -> gm200_gr_acr_bld_patch "
				   "(nvkm/engine/gr/gm200.c:46-54) - "
				   "code/data_dma_base += wpr_start - wpr_prev "
				   "(acr/base.c:306; wpr_prev 0)"
				 : "nvkm/subdev/acr/gp102.c:51 gp102_acr_wpr_"
				   "patch -> gp102_sec2_acr_bld_patch_1 "
				   "(sec2/gp102.c:240-249) - code/data_dma_base "
				   "+= wpr_start - wpr_prev (acr/base.c:306)");
		} else {
			obj_blob(b, PG106_ACRBOOT_WPR, l->offset.bld, NULL,
				 l->bl_data_size,
				 "nvkm/subdev/acr/gp102.c:110 gp102_acr_wpr_build "
				 "- lsfw->func->bld_write(wpr, offset.bld) [SIM: "
				 "bld_kind NONE, zero-fill of bl_data_size]");
		}
	}

	// Terminator (gp102.c:113-114).
	{
		uint8_t *buf = scratch_new(b, 4);

		if (!buf)
			return -ENOMEM;
		ser32(buf, 0, PG106_ACR_WPR_FALCON_ID_INVALID);
		obj_blob(b, PG106_ACRBOOT_WPR, nlsfw * 24u, buf, 4,
			 "nvkm/subdev/acr/gp102.c:113-114 gp102_acr_wpr_build "
			 "- nvkm_wo32(wpr, nlsfw*24, "
			 "WPR_HEADER_V1_FALCON_ID_INVALID)");
	}
	return 0;
}

// Group 8: WPR window check after the HS boot (nvkm_acr_load,
// acr/base.c:105-112 -> gm200_acr_wpr_check, gm200.c:52-62).
static void
plan_wpr_check(struct builder *b, const struct pg106_acrboot_config *cfg,
	       const struct pg106_acrboot_info *info)
{
	reg_write_abs(b, PG106_PFB_WPR_CHECK, 2,
		      "nvkm/subdev/acr/gm200.c:57 gm200_acr_wpr_check - "
		      "wr32(0x100cd4, 2): select WPR start window");
	reg_read(b, cfg, PG106_PFB_WPR_CHECK, true, 0xffffff00u,
		 (uint32_t)((info->wpr_start >> 8) & 0xffffff00ull),
		 "nvkm/subdev/acr/gm200.c:58 gm200_acr_wpr_check + "
		 "acr/base.c:107 - start = (rd32(0x100cd4) & 0xffffff00) "
		 "<< 8 must equal wpr_start");
	reg_write_abs(b, PG106_PFB_WPR_CHECK, 3,
		      "nvkm/subdev/acr/gm200.c:59 gm200_acr_wpr_check - "
		      "wr32(0x100cd4, 3): select WPR limit window");
	reg_read(b, cfg, PG106_PFB_WPR_CHECK, true, 0xffffff00u,
		 (uint32_t)(((info->wpr_end - 0x20000ull) >> 8) &
			    0xffffff00ull),
		 "nvkm/subdev/acr/gm200.c:60-61 gm200_acr_wpr_check + "
		 "acr/base.c:107-112 - limit = ((rd32 & 0xffffff00) << 8) + "
		 "0x20000 must equal wpr_end");
}

// Group 9: nvkm_acr_load takes a reference on the rtos owner (acr/base.c:
// 116-121), which runs nvkm_sec2_init (sec2/base.c:74-93): falcon_get
// (already initialised, no registers), 0x014 = ~0, then nvkm_falcon_start
// -> nvkm_falcon_v1_start (nvkm/falcon/v1.c:93-101).
static void
plan_sec2_start(struct builder *b, const struct pg106_acrboot_config *cfg)
{
	reg_write(b, cfg, PG106_FALCON_INTR_EN, 0xffffffffu,
		  "nvkm/engine/sec2/base.c:86 nvkm_sec2_init - "
		  "nvkm_falcon_wr32(0x014, 0xffffffff)");
	reg_read(b, cfg, PG106_FALCON_CPUCTL, false, 0x00000040u,
		 cfg->cpuctl_alias_en ? 0x40u : 0u,
		 "nvkm/falcon/v1.c:95-97 nvkm_falcon_v1_start - "
		 "rd32(0x100) & BIT(6) (ALIAS_EN) picks the start register");
	if (cfg->cpuctl_alias_en)
		reg_write(b, cfg, PG106_FALCON_CPUCTL_ALIAS, 0x2u,
			  "nvkm/falcon/v1.c:98 nvkm_falcon_v1_start - "
			  "wr32(0x130, 0x2) (ALIAS_EN set)");
	else
		reg_write(b, cfg, PG106_FALCON_CPUCTL, 0x2u,
			  "nvkm/falcon/v1.c:100 nvkm_falcon_v1_start - "
			  "wr32(0x100, 0x2) STARTCPU (sec2/base.c:91)");
}

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

uint32_t
pg106_acrboot_falcon_base(const struct pg106_top_table *top, uint32_t fallback)
{
	const struct pg106_top_device *d;

	if (!top)
		return fallback;
	d = pg106_top_find(top, PG106_TOP_TYPE_SEC2, 0);
	if (!d || d->addr == 0)
		return fallback;
	return d->addr;
}

int
pg106_acrboot_plan_build(const struct pg106_acrboot_config *cfg,
			 const struct pg106_acr_bl_ucode *bl,
			 const struct pg106_acr_hs_ucode *hs,
			 struct pg106_acr_lsfw *lsfws, size_t nlsfw,
			 const struct pg106_acr_lsf_signature_v1 *const *sigs,
			 struct pg106_acrboot_plan *plan,
			 struct pg106_acrboot_info *info)
{
	struct builder b = { .plan = plan };
	uint32_t wpr_size = 0, code_limit;
	uint64_t alloc_size;
	bool dbg;
	int ret;

	if (!cfg || !bl || !hs || !plan || !info)
		return -EINVAL;
	if (!lsfws || !sigs)
		return -EINVAL;
	if (nlsfw > PG106_ACR_LSF_MAX)
		return -EINVAL;
	code_limit = PG106_FALCON_HWCFG_CODE_LIMIT(cfg->hwcfg);
	// gm200.c:291 loads the BL at code.limit - boot_size; the IMEM PIO
	// path WARNs and fails on a length that is not a multiple of
	// imem_pio.min = 0x100 (base.c:186-187, gm200.c:116).
	if (code_limit < bl->boot_size || bl->boot_size == 0 ||
	    (bl->boot_size & (PG106_FALCON_PIO_CHUNK - 1)) != 0)
		return -EINVAL;
	memset(plan, 0, sizeof(*plan));
	memset(info, 0, sizeof(*info));
	info->code_limit = code_limit;
	info->data_limit = PG106_FALCON_HWCFG_DATA_LIMIT(cfg->hwcfg);
	dbg = (cfg->debug_reg & PG106_FALCON_SEC2_DEBUG_DBG) != 0;
	info->debug_sig = dbg;

	// WPR geometry (acr/base.c:283-297): gp102_acr_wpr_layout then
	// gp102_acr_wpr_alloc's shadow + WPR halves (gp102.c:118-131).
	if (pg106_acr_wpr_layout(lsfws, nlsfw, &wpr_size))
		return -EINVAL;
	alloc_size = ((wpr_size + WPR_ALIGN - 1) & ~(WPR_ALIGN - 1)) << 1;
	info->wpr_size = wpr_size;
	info->alloc_size = alloc_size;
	info->shadow_start = cfg->wpr_alloc_base;
	info->wpr_start = cfg->wpr_alloc_base + (alloc_size >> 1);
	info->wpr_end = info->wpr_start + (alloc_size >> 1);

	plan_oneinit(&b, cfg);				   // 1
	ret = plan_wpr_build(&b, cfg, lsfws, nlsfw, sigs, // 2
			     info->wpr_start);
	if (ret) {
		pg106_acrboot_plan_free(plan);
		return ret;
	}
	plan_sig_patch(&b, cfg, hs, dbg);		   // 3 (fw.c:84-90)
	plan_reset(&b, cfg);				   // 4 (fw.c:94)
	plan_load_setup(&b, hs, info->shadow_start,	   // 5 (fw.c:97-101)
			info->wpr_start, info->wpr_end);
	plan_load(&b, cfg, bl, hs, code_limit);		   // 6 (fw.c:109)
	plan_boot(&b, cfg, bl);				   // 7 (fw.c:114)
	plan_wpr_check(&b, cfg, info);			   // 8
	if (cfg->rtos_id != UINT32_MAX)
		plan_sec2_start(&b, cfg);		   // 9

	if (b.failed) {
		pg106_acrboot_plan_free(plan);
		return -ENOMEM;
	}
	info->nsteps = plan->nsteps;
	return 0;
}

void
pg106_acrboot_plan_free(struct pg106_acrboot_plan *plan)
{
	size_t i;

	if (!plan)
		return;
	free(plan->steps);
	plan->steps = NULL;
	plan->nsteps = 0;
	for (i = 0; i < plan->nscratch_; i++)
		free(plan->scratch_[i]);
	free(plan->scratch_);
	plan->scratch_ = NULL;
	plan->nscratch_ = 0;
}

const char *
pg106_acrboot_kind_name(uint8_t kind)
{
	switch (kind) {
	case PG106_ACRBOOT_WRITE_REG:
		return "WRITE_REG";
	case PG106_ACRBOOT_WRITE_BLOB:
		return "WRITE_BLOB";
	case PG106_ACRBOOT_POLL_REG:
		return "POLL_REG";
	case PG106_ACRBOOT_READ_REG:
		return "READ_REG";
	case PG106_ACRBOOT_MASK_REG:
		return "MASK_REG";
	case PG106_ACRBOOT_DELAY_US:
		return "DELAY_US";
	}
	return "?kind";
}

const char *
pg106_acrboot_space_name(uint8_t space)
{
	switch (space) {
	case PG106_ACRBOOT_MMIO:
		return "MMIO";
	case PG106_ACRBOOT_WPR:
		return "WPR";
	case PG106_ACRBOOT_IMG:
		return "IMG";
	}
	return "?space";
}

const char *
pg106_acrboot_poll_op_name(uint8_t op)
{
	switch (op) {
	case PG106_ACRBOOT_POLL_ANY:
		return "ANY";
	case PG106_ACRBOOT_POLL_EQ:
		return "EQ";
	}
	return "?op";
}

// ---------------------------------------------------------------------------
// SEC2 queue executor
// ---------------------------------------------------------------------------

static uint32_t
frd(struct pg106_sec2 *s, uint32_t off)
{
	return s->bus.rd32(s->bus.ctx, s->base + off);
}

static void
fwr(struct pg106_sec2 *s, uint32_t off, uint32_t v)
{
	s->bus.wr32(s->bus.ctx, s->base + off, v);
}

static uint32_t
align4(uint32_t v)
{
	return (v + PG106_FLCN_QUEUE_ALIGNMENT - 1u) &
	       ~(PG106_FLCN_QUEUE_ALIGNMENT - 1u);
}

// nvkm_falcon_pio routing (nvkm/falcon/base.c:120-137): a DMEM address at
// or above emem_addr goes to the EMEM port with the base rebased.
static void
pio_route(uint32_t *mem_base, uint32_t *ctl, uint32_t *dat)
{
	if (*mem_base >= PG106_SEC2_EMEM_ADDR) {
		*mem_base -= PG106_SEC2_EMEM_ADDR;
		*ctl = PG106_FALCON_EMEMC; // gp102.c:37,53
		*dat = PG106_FALCON_EMEMD; // gp102.c:28,44
	} else {
		*ctl = PG106_FALCON_DMEMC; // gm200.c:66,84
		*dat = PG106_FALCON_DMEMD; // gm200.c:47,73
	}
}

// nvkm_falcon_pio_wr(..., DMEM, mem_base, len, ...) (base.c:174-209) with
// gm200_flcn_dmem_pio / gp102_flcn_emem_pio: one wr_init (BIT(24)|base),
// then u32 words; a trailing partial word is dropped exactly as the
// `while (len >= 4)` loops do (gm200.c:72-78, gp102.c:43-47). EMEM's
// pio->min = 4 makes nouveau reject len % 4 (base.c:186); callers here
// only pass 4-aligned lengths.
static void
pio_wr(struct pg106_sec2 *s, uint32_t mem_base, const uint8_t *src,
       uint32_t len)
{
	uint32_t ctl, dat, i;

	pio_route(&mem_base, &ctl, &dat);
	fwr(s, ctl, 0x01000000u | mem_base);
	for (i = 0; i + 4 <= len; i += 4)
		fwr(s, dat, peek32(src, i));
}

// nvkm_falcon_pio_rd (base.c:139-172): BIT(25)|base, then u32 reads; the
// DMEM reader keeps a trailing partial word (gm200.c:52-60).
static void
pio_rd(struct pg106_sec2 *s, uint32_t mem_base, uint8_t *dst, uint32_t len)
{
	uint32_t ctl, dat, i, w;

	pio_route(&mem_base, &ctl, &dat);
	fwr(s, ctl, 0x02000000u | mem_base);
	for (i = 0; i + 4 <= len; i += 4)
		ser32(dst, i, frd(s, dat));
	if (i < len) {
		w = frd(s, dat);
		for (; i < len; i++, w >>= 8)
			dst[i] = (uint8_t)w;
	}
}

void
pg106_sec2_ctx_init(struct pg106_sec2 *s, const struct pg106_flcn_bus *bus,
		    uint32_t base, uint32_t max_polls)
{
	memset(s, 0, sizeof(*s));
	s->bus = *bus;
	s->base = base;
	s->max_polls = max_polls ? max_polls : 1;
}

// --- msgq (nvkm/falcon/msgq.c) ----------------------------------------------

static bool
msgq_empty(struct pg106_sec2 *s)
{
	// msgq.c:43-49
	return frd(s, s->msgq.head_reg) == frd(s, s->msgq.tail_reg);
}

static int
msgq_pop(struct pg106_sec2 *s, uint8_t *data, uint32_t size)
{
	uint32_t head, tail, available;

	// msgq.c:51-74
	head = frd(s, s->msgq.head_reg);
	if (head < s->msgq.position)
		s->msgq.position = s->msgq.offset;
	tail = s->msgq.position;
	available = head - tail;
	if (size > available)
		return -EINVAL;
	pio_rd(s, tail, data, size);
	s->msgq.position += align4(size);
	return 0;
}

// msgq.c:76-112. Returns 1 with a message in buf, 0 when empty, <0 error.
static int
msgq_read(struct pg106_sec2 *s, uint8_t *buf)
{
	int ret = 0;

	s->msgq.position = frd(s, s->msgq.tail_reg); // open, msgq.c:29
	if (msgq_empty(s))
		goto close;
	ret = msgq_pop(s, buf, PG106_FLCN_HDR_SIZE);
	if (ret)
		goto close;
	if (buf[1] > PG106_FLCN_MSG_BUF_SIZE) {
		ret = -ENOSPC;
		goto close;
	}
	if (buf[1] > PG106_FLCN_HDR_SIZE) {
		ret = msgq_pop(s, buf + PG106_FLCN_HDR_SIZE,
			       buf[1] - PG106_FLCN_HDR_SIZE);
		if (ret)
			goto close;
	}
	ret = 1;
close:
	if (ret >= 0) // msgq.c:33-41 close(commit = ret >= 0)
		fwr(s, s->msgq.tail_reg, s->msgq.position);
	return ret;
}

// gp102_sec2_acr_bootstrap_falcon_callback (sec2/gp102.c:39-56).
static int
bootstrap_callback(struct pg106_sec2_seq *seq, const uint8_t *msg)
{
	seq->msg_error_code = peek32(msg, 8);
	seq->msg_falcon_id = peek32(msg, 12);
	return seq->msg_error_code ? -EINVAL : 0;
}

// msgq.c:114-137 nvkm_falcon_msgq_exec.
static int
msgq_exec(struct pg106_sec2 *s, const uint8_t *msg)
{
	struct pg106_sec2_seq *seq;
	uint8_t id = msg[3];

	if (id >= PG106_FLCN_QMGR_SEQ_NUM)
		return -EINVAL; // seq.id[] would be indexed out of range
	seq = &s->seq[id];
	if (seq->state != PG106_SEC2_SEQ_USED &&
	    seq->state != PG106_SEC2_SEQ_CANCELLED)
		return -EINVAL;
	if (seq->state == PG106_SEC2_SEQ_USED)
		seq->result = bootstrap_callback(seq, msg);
	seq->done = true; // complete_all(&seq->done); never async here
	return 0;
}

// msgq.c:139-151.
static void
msgq_recv(struct pg106_sec2 *s)
{
	uint8_t buf[PG106_FLCN_MSG_BUF_SIZE + PG106_FLCN_HDR_SIZE];

	while (msgq_read(s, buf) > 0)
		(void)msgq_exec(s, buf);
}

int
pg106_sec2_initmsg(struct pg106_sec2 *s)
{
	uint8_t m[PG106_SEC2_INIT_MSG_SIZE];
	struct pg106_sec2_init_msg *im = &s->init;
	int ret, i;

	// nvkm_falcon_msgq_recv_initmsg (msgq.c:153-174): queue 0 regs,
	// offset = current tail, one pop of sizeof(msg), header size check.
	// (queue registers are kept falcon-relative here)
	s->msgq.head_reg = PG106_SEC2_MSGQ_HEAD;
	s->msgq.tail_reg = PG106_SEC2_MSGQ_TAIL;
	s->msgq.offset = frd(s, PG106_SEC2_MSGQ_TAIL);
	s->msgq.position = frd(s, s->msgq.tail_reg);
	ret = msgq_pop(s, m, sizeof(m));
	if (ret == 0 && m[1] != sizeof(m))
		ret = -EINVAL;
	if (ret == 0)
		fwr(s, s->msgq.tail_reg, s->msgq.position);
	if (ret)
		return ret;

	im->unit_id = m[0];
	im->size = m[1];
	im->ctrl_flags = m[2];
	im->seq_id = m[3];
	im->msg_type = m[4];
	im->num_queues = m[5];
	im->os_debug_entry_point = (uint16_t)(m[6] | (m[7] << 8));
	for (i = 0; i < 2; i++) {
		const uint8_t *q = m + 8 + 8 * i;

		im->queue_info[i].offset = peek32(q, 0);
		im->queue_info[i].size = (uint16_t)(q[4] | (q[5] << 8));
		im->queue_info[i].index = q[6];
		im->queue_info[i].id = q[7];
	}
	im->sw_managed_area_offset = peek32(m, 24);
	im->sw_managed_area_size = (uint16_t)(m[28] | (m[29] << 8));

	// gp102_sec2_initmsg (sec2/gp102.c:131-147).
	if (im->unit_id != PG106_SEC2_UNIT_INIT ||
	    im->msg_type != PG106_SEC2_INIT_MSG_INIT)
		return -EINVAL;
	for (i = 0; i < 2; i++) {
		uint32_t idx = im->queue_info[i].index;

		if (im->queue_info[i].id == PG106_SEC2_QUEUE_ID_MSGQ) {
			// nvkm_falcon_msgq_init (msgq.c:176-188)
			s->msgq.head_reg = PG106_SEC2_MSGQ_HEAD +
					   idx * PG106_SEC2_Q_STRIDE;
			s->msgq.tail_reg = PG106_SEC2_MSGQ_TAIL +
					   idx * PG106_SEC2_Q_STRIDE;
			s->msgq.offset = im->queue_info[i].offset;
			s->msgq.size = im->queue_info[i].size;
		} else {
			// nvkm_falcon_cmdq_init (cmdq.c:174-188)
			s->cmdq.head_reg = PG106_SEC2_CMDQ_HEAD +
					   idx * PG106_SEC2_Q_STRIDE;
			s->cmdq.tail_reg = PG106_SEC2_CMDQ_TAIL +
					   idx * PG106_SEC2_Q_STRIDE;
			s->cmdq.offset = im->queue_info[i].offset;
			s->cmdq.size = im->queue_info[i].size;
			s->cmdq.ready = true; // complete_all(&cmdq->ready)
		}
	}
	return 0;
}

uint32_t
pg106_sec2_intr(struct pg106_sec2 *s)
{
	// gp102_sec2_intr (sec2/gp102.c:152-196).
	uint32_t disp = frd(s, PG106_FALCON_IRQDEST);
	uint32_t intr = frd(s, PG106_FALCON_INTR_STAT) & disp & ~(disp >> 16);
	uint32_t handled = 0;

	if (intr & 0x00000040u) {
		if (s->initmsg == 0) {
			int ret = pg106_sec2_initmsg(s);

			s->initmsg = ret ? ret : 1;
		}
		if (s->initmsg > 0 && !msgq_empty(s))
			msgq_recv(s);
		fwr(s, PG106_FALCON_INTR, 0x00000040u);
		intr &= ~0x00000040u;
		handled |= 0x40u;
	}
	if (intr & 0x00000010u) {
		// "halted" while running: nouveau logs + TRACEPC dump.
		fwr(s, PG106_FALCON_INTR, 0x00000010u);
		intr &= ~0x00000010u;
		handled |= 0x10u;
	}
	if (intr) {
		s->unhandled_intr |= intr;
		fwr(s, PG106_FALCON_INTR, intr);
	}
	return handled;
}

// --- cmdq (nvkm/falcon/cmdq.c) ---------------------------------------------

// cmdq.c:25-48
static bool
cmdq_has_room(struct pg106_sec2 *s, uint32_t size, bool *rewind)
{
	uint32_t head = frd(s, s->cmdq.head_reg);
	uint32_t tail = frd(s, s->cmdq.tail_reg);
	uint32_t free = 0;

	size = align4(size);
	if (head >= tail) {
		free = s->cmdq.offset + s->cmdq.size - head;
		free -= PG106_FLCN_HDR_SIZE;
		if (size > free) {
			*rewind = true;
			head = s->cmdq.offset;
		}
	}
	if (head < tail)
		free = tail - head - 1;
	return size <= free;
}

// cmdq.c:50-56
static void
cmdq_push(struct pg106_sec2 *s, const uint8_t *data, uint32_t size)
{
	pio_wr(s, s->cmdq.position, data, size);
	s->cmdq.position += align4(size);
}

int
pg106_sec2_bootstrap_falcon(struct pg106_sec2 *s, uint32_t falcon_id)
{
	uint8_t cmd[PG106_SEC2_ACR_BOOTSTRAP_CMD_SIZE] = { 0 };
	struct pg106_sec2_seq *seq = NULL;
	uint32_t i, polls;
	bool rewind = false, room = false;
	int ret;

	if (!s)
		return -EINVAL;
	// cmdq.c:131-135: wait for cmdq->ready (set by the init message).
	for (polls = 0; !s->cmdq.ready && polls < s->max_polls; polls++)
		pg106_sec2_intr(s);
	if (!s->cmdq.ready)
		return -ETIMEDOUT;

	// qmgr.c:25-46 nvkm_falcon_qmgr_seq_acquire: first free id.
	for (i = 0; i < PG106_FLCN_QMGR_SEQ_NUM; i++) {
		if (s->seq[i].state == PG106_SEC2_SEQ_FREE) {
			seq = &s->seq[i];
			break;
		}
	}
	if (!seq)
		return -EAGAIN;
	memset(seq, 0, sizeof(*seq));

	// gp102_sec2_acr_bootstrap_falcon (sec2/gp102.c:63-69) +
	// cmdq.c:141-142 seq_id / ctrl_flags.
	cmd[0] = PG106_SEC2_UNIT_ACR;			// hdr.unit_id
	cmd[1] = PG106_SEC2_ACR_BOOTSTRAP_CMD_SIZE;	// hdr.size
	cmd[2] = PG106_FLCN_CMD_FLAGS_STATUS | PG106_FLCN_CMD_FLAGS_INTR;
	cmd[3] = (uint8_t)i;				// hdr.seq_id
	cmd[4] = PG106_SEC2_ACR_CMD_BOOTSTRAP_FALCON;	// cmd_type
	ser32(cmd, 8, PG106_SEC2_ACR_BOOTSTRAP_FALCON_FLAGS_RESET_YES);
	ser32(cmd, 12, falcon_id);
	seq->state = PG106_SEC2_SEQ_USED; // cmdq.c:144

	// nvkm_falcon_cmdq_write (cmdq.c:99-116): open (retry -EAGAIN until
	// the 2 s budget), push, close.
	for (polls = 0; polls < s->max_polls; polls++) {
		rewind = false;
		if (cmdq_has_room(s, sizeof(cmd), &rewind)) {
			room = true;
			break;
		}
		pg106_sec2_intr(s);
	}
	if (!room) {
		seq->state = PG106_SEC2_SEQ_FREE; // cmdq.c:150-153
		return -EAGAIN;
	}
	s->cmdq.position = frd(s, s->cmdq.head_reg); // cmdq.c:84
	if (rewind) {
		// nvkm_falcon_cmdq_rewind (cmdq.c:58-68)
		uint8_t rw[PG106_FLCN_HDR_SIZE] = { PG106_FLCN_UNIT_ID_REWIND,
						    PG106_FLCN_HDR_SIZE, 0, 0 };

		cmdq_push(s, rw, sizeof(rw));
		s->cmdq.position = s->cmdq.offset;
	}
	cmdq_push(s, cmd, sizeof(cmd));
	fwr(s, s->cmdq.head_reg, s->cmdq.position); // close, cmdq.c:95

	// cmdq.c:156-163: wait for the reply (1000 ms), take seq->result.
	for (polls = 0; !seq->done && polls < s->max_polls; polls++)
		pg106_sec2_intr(s);
	if (!seq->done)
		return -ETIMEDOUT; // seq stays USED, as upstream leaks it
	ret = seq->result;
	seq->state = PG106_SEC2_SEQ_FREE; // qmgr.c:48-57 seq_release
	seq->done = false;
	return ret;
}

int
pg106_acr_bootstrap_falcons(struct pg106_sec2 *s, uint64_t mask)
{
	uint32_t id;

	if (!s)
		return -EINVAL;
	// acr/base.c:156-157
	if ((mask & PG106_SEC2_BOOTSTRAP_FALCONS) != mask)
		return -ENOSYS;
	// acr/base.c:162-166 for_each_set_bit (ascending)
	for (id = 0; id < PG106_ACR_LSF_MAX; id++) {
		int ret;

		if (!(mask & (1ull << id)))
			continue;
		ret = pg106_sec2_bootstrap_falcon(s, id);
		if (ret)
			return ret;
	}
	return 0;
}
