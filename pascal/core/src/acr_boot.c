// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/src/acr_boot.c — Phase 2 ACR secure-boot SIMULATOR (see
// acr_boot.h for the design and the SIMULATION, NOT HARDWARE PROOF label).
//
// Builds the ordered falcon-loading step list for the gp102/r352 ACR boot of
// the SEC2 falcon, reusing the real-blob parsers in acr.c — no blob parsing
// is duplicated here; this file only models the *sequence* nouveau executes
// around those blobs. Every emitted step is annotated with its upstream
// origin (nvkm file:line of linux master, /tmp/pascal-ref/upstream/).
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
//   nvkm/subdev/top/{base.c,gk104.c} + nvkm/falcon/base.c:143-147
//                            the nvkm_top table reader feeding the falcon
//                            base (pg106_acrboot_falcon_base; linux v5.15 —
//                            per-line citations in top.h/top.c)

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

// Provenance annotations are STATIC strings: the dynamic values an
// annotation would embed (imem/dmem base, tag, falcon id, WPR offsets) are
// already carried by the step's own addr/value/mask/blob_len fields, and
// the portable core must not pull in <stdio.h> (no printf family in a
// kext; enforced by pascal/kext/scripts/symbol-audit.sh).

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
	 const char *why)
{
	emit(b, &(struct pg106_acrboot_step){
		    .kind = PG106_ACRBOOT_POLL_REG,
		    .space = PG106_ACRBOOT_MMIO,
		    .poll_op = op,
		    .addr = cfg->falcon_base + off,
		    .value = value,
		    .mask = mask,
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

// Group 1: host-side signature patch into the ucode image before the falcon
// sees it (nvkm_falcon_fw_boot -> nvkm_falcon_fw_patch, fw.c:84-90; the
// image write itself is fw.c:58 `fw->fw.img[dst] = sig`). The signature
// bytes come from the blob's signature pool (sig_prod_off), which the
// parsed view locates blob-absolute; the blob base is img - data_offset.
static void
plan_sig_patch(struct builder *b, const struct pg106_acr_hs_ucode *hs)
{
	const uint8_t *blob = hs->img - hs->hdr.data_offset;
	const uint8_t *sig = blob + hs->sig_prod_off;

	obj_blob(b, PG106_ACRBOOT_IMG, hs->sig_base_img, sig, hs->sig_size,
		 "nvkm/falcon/fw.c:58 nvkm_falcon_fw_patch - production "
		 "signature bytes copied from the blob's signature pool "
		 "(blob offset in `blob`; src = blob[sig_prod_off], "
		 "fw.c:258-260) into the image at addr = sig_base_img; site "
		 "from hs.patch_loc/patch_sig indirection (fw.c:243-260)");
}

// Group 2: falcon reset — gm200_flcn_fw_reset -> nvkm_falcon_reset
// (gm200.c:317-321, base.c:242-252) = disable + enable.
static void
plan_reset(struct builder *b, const struct pg106_acrboot_config *cfg)
{
	// --- disable half: gm200_flcn_disable (gm200.c:183-215) ---
	reg_write(b, cfg, PG106_FALCON_RM, 0x00000000u,
		  "nvkm/falcon/gm200.c:195 gm200_flcn_disable - "
		  "nvkm_falcon_mask(0x048, 0x3, 0x0): clear enables");
	reg_write(b, cfg, PG106_FALCON_INTR_EN, 0xffffffffu,
		  "nvkm/falcon/gm200.c:196 gm200_flcn_disable - "
		  "disable all interrupts (0x014 = 0xffffffff)");
	// (reset_prep + PMC disable + reset_eng follow upstream; the PMC
	// enable bits are device-global and outside the falcon-window scope
	// of this model.)
	// --- GP10x engine reset: gp102_flcn_reset_eng ---
	reg_write(b, cfg, PG106_FALCON_GP102_RESET, 0x00000001u,
		  "nvkm/falcon/gp102.c:77 gp102_flcn_reset_eng - "
		  "nvkm_falcon_mask(0x3c0, 0x1, 0x1), then 10us udelay "
		  "(gp102.c:78)");
	reg_write(b, cfg, PG106_FALCON_GP102_RESET, 0x00000000u,
		  "nvkm/falcon/gp102.c:79 gp102_flcn_reset_eng - "
		  "nvkm_falcon_mask(0x3c0, 0x1, 0x0) (deassert)");
	// --- enable half: memory-scrubbing wait ---
	reg_poll(b, cfg, PG106_FALCON_MEM_SCRUB, 0x00000006u, 0,
		 PG106_ACRBOOT_POLL_ANY,
		 "nvkm/falcon/gm200.c:145-148 gm200_flcn_reset_wait_mem_"
		 "scrubbing - poll !(0x10c & 0x6) (IMEM/DMEM scrub done), "
		 "10ms budget");
	reg_write(b, cfg, PG106_FALCON_ENGINE_EN, cfg->pmc_readback,
		  "nvkm/falcon/gm200.c:179 gm200_flcn_enable - "
		  "wr32(0x084, rd32(PMC 0x0)) clock-enable mirror");
}

// Group 3: gp102_acr_load_setup — patch the flcn_acr_desc_v1 living in the
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

// Group 4: gm200_flcn_fw_load (nvkm/falcon/gm200.c:248-315).
static void
plan_load(struct builder *b, const struct pg106_acrboot_config *cfg,
	  const struct pg106_acr_bl_ucode *bl,
	  const struct pg106_acr_hs_ucode *hs)
{
	// --- instance-bound DMA path (fw->inst != NULL, gm200.c:254-283) ---
	reg_write(b, cfg, PG106_FALCON_RM, 0x00000001u,
		  "nvkm/falcon/gm200.c:257 gm200_flcn_fw_load - "
		  "nvkm_falcon_mask(0x048, 0x1, 0x1) before binding inst");
	reg_write(b, cfg, PG106_FALCON_DMAIDX, 0x00000000u,
		  "nvkm/falcon/gm200.c:134 gm200_flcn_bind_inst - "
		  "nvkm_falcon_mask(0x604, 0x7, 0x0) = DMAIDX_VIRT");
	reg_write(b, cfg, PG106_FALCON_BINDINST,
		  (1u << 30) | (cfg->bind_target << 28) |
			  (uint32_t)(cfg->bind_addr >> 12),
		  "nvkm/falcon/gm200.c:135 gm200_flcn_bind_inst - "
		  "wr32(0x054, (1<<30)|(target<<28)|(addr>>12))");
	reg_write(b, cfg, PG106_FALCON_MEMBIND_CTL, 0x00010000u,
		  "nvkm/falcon/gm200.c:136 gm200_flcn_bind_inst - "
		  "nvkm_falcon_mask(0x090, 0x10000, 0x10000)");
	reg_write(b, cfg, PG106_FALCON_MEMBIND_CTL2, 0x00000008u,
		  "nvkm/falcon/gm200.c:137 gm200_flcn_bind_inst - "
		  "nvkm_falcon_mask(0x0a4, 0x8, 0x8)");
	reg_poll(b, cfg, PG106_FALCON_BINDSTAT, 0x00007000u, 0x5000,
		 PG106_ACRBOOT_POLL_EQ,
		 "nvkm/falcon/gm200.c:128,270-274 gm200_flcn_fw_load - "
		 "poll bind_stat == 5 ((0x0dc & 0x7000) >> 12), 10ms");
	reg_write(b, cfg, PG106_FALCON_INTR, 0x00000008u,
		  "nvkm/falcon/gm200.c:276 gm200_flcn_fw_load - "
		  "nvkm_falcon_mask(0x004, 0x8, 0x8) clear bind intr");
	reg_write(b, cfg, PG106_FALCON_BIND_TRIGGER, 0x00000002u,
		  "nvkm/falcon/gm200.c:277 gm200_flcn_fw_load - "
		  "nvkm_falcon_mask(0x058, 0x2, 0x2)");
	reg_poll(b, cfg, PG106_FALCON_BINDSTAT, 0x00007000u, 0x0000,
		 PG106_ACRBOOT_POLL_EQ,
		 "nvkm/falcon/gm200.c:128,279-283 gm200_flcn_fw_load - "
		 "poll bind_stat == 0 (unbind settled), 10ms");

	// --- HS bootloader PIO stream (fw->boot != NULL, gm200.c:289-296):
	// nvkm_falcon_pio_wr(fw->boot, IMEM, code.limit - boot_size,
	// boot_size, tag = boot_addr >> 8, non-secure). This is what
	// actually runs first on the SEC2 falcon: the ROM jumps to BOOTVEC,
	// the BL validates the HS signature and DMA-starts the image via
	// code_dma_base.
	expand_imem_pio(b, cfg, bl->boot_code, bl->boot_size,
			cfg->code_limit - bl->boot_size,
			(uint16_t)(bl->boot_addr >> 8), false,
			"nvkm/falcon/gm200.c:100,290-292 gm200_flcn_fw_load - "
			"BL stream ctl: nvkm_falcon_pio_wr(fw->boot, IMEM, "
			"code.limit - boot_size, boot_size, tag boot_addr>>8, "
			"non-secure) -> gm200_flcn_pio_imem_wr_init IMEMC = "
			"(sec?BIT28:0)|BIT24|imem_base (value carries base; "
			"bl.bin boot code via pg106_acr_ucode_parse_bl)");

	// --- BL DMEM argument block (load_bld, gm200.c:296 ->
	// gm200.c:224-242 gm200_acr_hsfw_load_bld): flcn_bl_dmem_desc_v1
	// PIO-written to DMEM 0, built by pg106_acr_hsfw_bld_build (the
	// acr.c parser exposes it) from the image's mapped VA.
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
				"nvkm/falcon/gm200.c:84,296,241 "
				"gm200_acr_hsfw_load_bld - BL args ctl: "
				"nvkm_falcon_pio_wr(flcn_bl_dmem_desc_v1, "
				"DMEM, 0, 76 bytes) -> "
				"gm200_flcn_pio_dmem_wr_init DMEMC = "
				"BIT24|dmem_base (value carries base 0; "
				"block built by pg106_acr_hsfw_bld_build, "
				"gm200.c:227-237)");
	}

	// --- HS ucode image PIO stream (MODEL: gm200.c:299-313, the same
	// loader's no-boot branch). On real GP106 the ACR "load" fw ships a
	// BL and DMAs the image from code_dma_base itself (gm200.c:290);
	// this PIO stream models the identical loader acting on the parsed
	// ucode_load.bin sections so the trace carries the image bytes.
	// (nmem_base_img = 0 and dmem_base = 0 per fw.c:266,275.)
	expand_imem_pio(b, cfg, hs->img, hs->nmem_size,
			hs->nmem_base, (uint16_t)(hs->nmem_base >> 8), false,
			"nvkm/falcon/gm200.c:100,299-301 gm200_flcn_fw_load "
			"no-boot branch [MODEL of the BL's DMA] - nmem ctl: "
			"nvkm_falcon_pio_wr(img+nmem_base_img, IMEM, "
			"nmem_base, nmem_size, tag nmem_base>>8, non-secure) "
			"-> IMEMC (value carries base; ucode_load.bin "
			"non-secure section)");
	expand_imem_pio(b, cfg, hs->img + hs->imem_base_img, hs->imem_size,
			hs->imem_base, (uint16_t)(hs->imem_base >> 8), true,
			"nvkm/falcon/gm200.c:100,304-306 gm200_flcn_fw_load "
			"no-boot branch [MODEL of the BL's DMA] - secure imem "
			"ctl: nvkm_falcon_pio_wr(img+imem_base_img, IMEM, "
			"imem_base, imem_size, tag imem_base>>8, SECURE "
			"BIT28) -> IMEMC (value carries base+BIT28; "
			"ucode_load.bin apps[0] secure section)");
	expand_dmem_pio(b, cfg, hs->img + hs->dmem_base_img, hs->dmem_size,
			0,
			"nvkm/falcon/gm200.c:84,309-312 gm200_flcn_fw_load "
			"no-boot branch [MODEL of the BL's DMA] - dmem ctl: "
			"nvkm_falcon_pio_wr(img+dmem_base_img, DMEM, "
			"dmem_base 0, dmem_size, non-secure) -> DMEMC (value "
			"carries base; ucode_load.bin data segment incl. the "
			"flcn_acr_desc_v1 the BL reads)");
}

// Group 5: gm200_flcn_fw_boot (nvkm/falcon/gm200.c:217-246), called via
// nvkm_falcon_fw_boot (fw.c:114) from gm200_acr_init ->
// nvkm_acr_hsfw_boot("load") (gm200.c:47-51; acr/base.c:55 passes
// pmbox0/pmbox1 = NULL, mbox0_ok = fwif boot_mbox0, irqsclr = intr_clear).
static void
plan_boot(struct builder *b, const struct pg106_acrboot_config *cfg,
	  const struct pg106_acr_bl_ucode *bl)
{
	reg_write(b, cfg, PG106_FALCON_MBOX0, 0xcafebeefu,
		  "nvkm/falcon/gm200.c:224 gm200_flcn_fw_boot - "
		  "wr32(0x040, pmbox0 ? *pmbox0 : 0xcafebeef); ACR boot "
		  "passes pmbox0=NULL (acr/base.c:55) -> sentinel");
	reg_write(b, cfg, PG106_FALCON_BOOTVEC, (uint32_t)bl->boot_addr,
		  "nvkm/falcon/gm200.c:228 gm200_flcn_fw_boot - "
		  "wr32(0x104, fw->boot_addr) (bl_desc start_tag<<8, "
		  "fw.c:289)");
	reg_write(b, cfg, PG106_FALCON_CPUCTL, 0x00000002u,
		  "nvkm/falcon/gm200.c:229 gm200_flcn_fw_boot - "
		  "wr32(0x100, 0x2) CPUCTL START (cf. nvkm/falcon/v1.c:100 "
		  "nvkm_falcon_v1_start)");
	reg_poll(b, cfg, PG106_FALCON_CPUCTL, 0x00000010u, 0,
		 PG106_ACRBOOT_POLL_ANY,
		 "nvkm/falcon/gm200.c:231-234 gm200_flcn_fw_boot - poll "
		 "0x100 & 0x10 CPUCTL HALTED, 2s budget");
	reg_read(b, cfg, PG106_FALCON_MBOX0, false, 0xffffffffu,
		 cfg->boot_mbox0,
		 "nvkm/falcon/gm200.c:237 gm200_flcn_fw_boot - mbox0 = "
		 "rd32(0x040); must equal fwif boot_mbox0 (gp102.c:239 -> 0 "
		 "for the gp102 ACR load fw; check gm200.c:239)");
	reg_read(b, cfg, PG106_FALCON_MBOX1, false, 0xffffffffu, 0,
		 "nvkm/falcon/gm200.c:238 gm200_flcn_fw_boot - mbox1 = "
		 "rd32(0x044) (reported, not gated on for ACR boot)");
	reg_write(b, cfg, PG106_FALCON_INTR, cfg->intr_clear,
		  "nvkm/falcon/gm200.c:242-243 gm200_flcn_fw_boot - "
		  "nvkm_falcon_mask(0x004, ~0, irqsclr) (intr_clear = 0x10, "
		  "gp102.c:239)");
}

// Group 6a: WPR shadow build (gp102_acr_wpr_build, gp102.c:76-116; caller
// order nvkm_acr_oneinit acr/base.c:283-306: wpr_layout -> wpr_alloc ->
// wpr_build -> wpr_patch).
static int
plan_wpr_build(struct builder *b, const struct pg106_acrboot_config *cfg,
	       struct pg106_acr_lsfw *lsfws, size_t nlsfw,
	       const struct pg106_acr_lsf_signature_v1 *const *sigs)
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
			 "nvkm/subdev/acr/gp102.c:96 gp102_acr_wpr_build - "
			 "nvkm_wobj(wpr, i*24, wpr_header_v1) (addr carries "
			 "the offset; blob carries falcon_id, lsb_offset, "
			 "bootstrap_owner, lazy_bootstrap, bin_version, "
			 "status=COPY; header built by "
			 "pg106_acr_wpr_header_build per gp102.c:86-93)");

		// LSB header: real signature + derived tail
		// (gp102.c:62-74; tail per gm200.c:94-111 via
		// pg106_acr_lsb_header_build).
		if (pg106_acr_lsb_header_build(l, sigs[i], &lsb))
			return -EINVAL;
		buf = ser_lsb_header(b, &lsb);
		if (!buf)
			return -ENOMEM;
		obj_blob(b, PG106_ACRBOOT_WPR, l->offset.lsb, buf, 240,
			 "nvkm/subdev/acr/gp102.c:72 gp102_acr_wpr_build_lsb"
			 " - nvkm_wobj(wpr, offset.lsb, lsb_header_v1) (addr "
			 "carries the offset; blob = the REAL gr/"
			 "fecs_sig.bin-or-gpccs_sig.bin signature per "
			 "gp102.c:69 + gm200_acr_wpr_build_lsb_tail per "
			 "gm200.c:94-111)");

		// LS ucode image copy (gp102.c:104-107). The sim has no LS
		// image bytes (gr/fecs_{desc,image}.bin are not vendored
		// yet) — modeled as zero-fill of the layout extent.
		obj_blob(b, PG106_ACRBOOT_WPR, l->offset.img, NULL,
			 (uint32_t)l->img_size,
			 "nvkm/subdev/acr/gp102.c:105-107 gp102_acr_wpr_build"
			 " - nvkm_wobj(wpr, offset.img, lsfw->img.data, "
			 "img.size) (addr carries the offset, len the image "
			 "size) [SIM: LS image bytes not vendored; zero-fill "
			 "keeps the layout arithmetic]");

		// Bootloader data block (gp102.c:110: lsfw->func->bld_write;
		// content is falcon-specific — zero-fill of bl_data_size).
		obj_blob(b, PG106_ACRBOOT_WPR, l->offset.bld, NULL,
			 l->bl_data_size,
			 "nvkm/subdev/acr/gp102.c:110 gp102_acr_wpr_build - "
			 "lsfw->func->bld_write(wpr, offset.bld) (addr "
			 "carries the offset, len = bl_data_size = "
			 "ALIGN(bld_size,256), gp102.c:155) [SIM: "
			 "falcon-specific bld content not modeled "
			 "byte-for-byte]");
	}

	// Terminator (gp102.c:113-114: nvkm_wo32(wpr, offset,
	// WPR_HEADER_V1_FALCON_ID_INVALID)).
	{
		uint8_t *buf = scratch_new(b, 4);

		if (!buf)
			return -ENOMEM;
		ser32(buf, 0, PG106_ACR_WPR_FALCON_ID_INVALID);
		obj_blob(b, PG106_ACRBOOT_WPR, nlsfw * 24u, buf, 4,
			 "nvkm/subdev/acr/gp102.c:113-114 gp102_acr_wpr_build "
			 "- terminator wpr_header_v1.falcon_id = "
			 "WPR_HEADER_V1_FALCON_ID_INVALID at offset "
			 "nlsfw*24 (addr carries it)");
	}
	return 0;
}

// Group 6b: WPR window check after the HS boot (nvkm_acr_load,
// acr/base.c:105-112 -> gm200_acr_wpr_check, gm200.c:52-62): select window
// 2 (start) / 3 (limit) in PFB 0x100cd4 and require them to match the
// allocation (window values are in 256-byte units, [31:8] of the read).
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

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

// Falcon-base resolution: SEC2/instance-0 out of the scanned nvkm_top table,
// with the old 0x3c0000 stand-in as the documented fallback. See the
// acr_boot.h declaration for the full provenance chain
// (falcon/base.c:143-147 -> top/base.c:44-53 -> gk104.c:26-101) and why the
// fallback is a SIM deviation (upstream: WARN + -ENODEV).
uint32_t
pg106_acrboot_falcon_base(const struct pg106_top_table *top, uint32_t fallback)
{
	const struct pg106_top_device *d;

	if (!top)
		return fallback; // no table image: labeled fallback path
	d = pg106_top_find(top, PG106_TOP_TYPE_SEC2, 0);
	if (!d || d->addr == 0)
		return fallback; // table present but SEC2 base never arrived
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
	uint32_t wpr_size = 0;
	uint64_t alloc_size;

	if (!cfg || !bl || !hs || !plan || !info)
		return -EINVAL;
	if (!lsfws || !sigs)
		return -EINVAL;
	if (nlsfw > PG106_ACR_LSF_MAX)
		return -EINVAL;
	// Sanity: the reset group loads the BL at code_limit - boot_size;
	// refuse configurations where that goes negative (gm200.c:291).
	if (cfg->code_limit < bl->boot_size)
		return -EINVAL;
	memset(plan, 0, sizeof(*plan));
	memset(info, 0, sizeof(*info));

	// WPR geometry first, mirroring nvkm_acr_oneinit's order
	// (acr/base.c:283-297): gp102_acr_wpr_layout (gp102.c:133-160, via
	// pg106_acr_wpr_layout in acr.c) then gp102_acr_wpr_alloc's split
	// of the allocation into shadow + WPR halves (gp102.c:118-131).
	if (pg106_acr_wpr_layout(lsfws, nlsfw, &wpr_size))
		return -EINVAL;
	alloc_size = ((wpr_size + WPR_ALIGN - 1) & ~(WPR_ALIGN - 1)) << 1;
	info->wpr_size = wpr_size;
	info->alloc_size = alloc_size;
	info->shadow_start = cfg->wpr_alloc_base;
	info->wpr_start = cfg->wpr_alloc_base + (alloc_size >> 1);
	info->wpr_end = info->wpr_start + (alloc_size >> 1);

	// 1. WPR shadow build — happens during nvkm_acr_oneinit, i.e.
	// BEFORE the HS boot (acr/base.c:299-306: layout -> alloc -> build).
	{
		int ret = plan_wpr_build(&b, cfg, lsfws, nlsfw, sigs);

		if (ret) {
			pg106_acrboot_plan_free(plan);
			return ret;
		}
	}

	// 2. HS signature patch (host-side image patch, fw.c:84-90).
	plan_sig_patch(&b, hs);

	// 3. Falcon reset (fw.c:94 -> gm200.c:317 -> base.c:242).
	plan_reset(&b, cfg);

	// 4. ACR desc setup in the image DMEM (fw.c:97-101 -> gp102.c:196).
	plan_load_setup(&b, hs, info->shadow_start, info->wpr_start,
			info->wpr_end);

	// 5. Load: bind + BL PIO + BL DMEM args + modeled image stream
	// (fw.c:109 -> gm200.c:248).
	plan_load(&b, cfg, bl, hs);

	// 6. Boot: mailbox handshake + start + halt poll (fw.c:114 ->
	// gm200.c:217).
	plan_boot(&b, cfg, bl);

	// 7. WPR window check after the boot (acr/base.c:101-112 ->
	// gm200.c:52-62).
	plan_wpr_check(&b, cfg, info);

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
