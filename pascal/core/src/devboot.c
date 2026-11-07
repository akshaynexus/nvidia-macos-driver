// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/src/devboot.c — GP106 bring-up sequence over a register bus.
// See devboot.h for the stage order and sources.

#include <pascal/devboot.h>
#include <pascal/pmc.h>
#include <pascal/vbios_bit.h>

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define FW(field) offsetof(struct pg106_devboot_fw, field)

// Resource names are flat (Xcode copies bundle resources into
// Contents/Resources without their directories).
const struct pg106_fw_name pg106_devboot_fw_names[] = {
	{ "gp106-acr-bl.bin", "gp106/acr/bl.bin", FW(acr_bl), PG106_DB_ACR },
	{ "gp106-acr-ucode_load.bin", "gp106/acr/ucode_load.bin", FW(acr_ucode_load), PG106_DB_ACR },
	{ "gp106-gr-fecs_bl.bin", "gp106/gr/fecs_bl.bin", FW(fecs_bl), PG106_DB_ACR },
	{ "gp106-gr-fecs_inst.bin", "gp106/gr/fecs_inst.bin", FW(fecs_inst), PG106_DB_ACR },
	{ "gp106-gr-fecs_data.bin", "gp106/gr/fecs_data.bin", FW(fecs_data), PG106_DB_ACR },
	{ "gp106-gr-fecs_sig.bin", "gp106/gr/fecs_sig.bin", FW(fecs_sig), PG106_DB_ACR },
	{ "gp106-gr-gpccs_bl.bin", "gp106/gr/gpccs_bl.bin", FW(gpccs_bl), PG106_DB_ACR },
	{ "gp106-gr-gpccs_inst.bin", "gp106/gr/gpccs_inst.bin", FW(gpccs_inst), PG106_DB_ACR },
	{ "gp106-gr-gpccs_data.bin", "gp106/gr/gpccs_data.bin", FW(gpccs_data), PG106_DB_ACR },
	{ "gp106-gr-gpccs_sig.bin", "gp106/gr/gpccs_sig.bin", FW(gpccs_sig), PG106_DB_ACR },
	{ "gp106-sec2-desc-1.bin", "gp106/sec2/desc-1.bin", FW(sec2_desc), PG106_DB_ACR },
	{ "gp106-sec2-image-1.bin", "gp106/sec2/image-1.bin", FW(sec2_image), PG106_DB_ACR },
	{ "gp106-sec2-sig-1.bin", "gp106/sec2/sig-1.bin", FW(sec2_sig), PG106_DB_ACR },
	// gm200_gr_load reads gr/sw_nonctx.bin (gr/gm200.c:222-250;
	// MODULE_FIRMWARE gp102.c:146). Not vendored in research/firmware.
	{ "gp106-gr-sw_nonctx.bin", "gp106/gr/sw_nonctx.bin", FW(sw_nonctx), PG106_DB_GR },
};
const size_t pg106_devboot_fw_count =
	sizeof(pg106_devboot_fw_names) / sizeof(pg106_devboot_fw_names[0]);

struct run {
	struct pg106_devboot *db;
	const struct pg106_devboot_cfg *cfg;
	const struct pg106_hw_bus *bus;
};

static void
emit(struct run *r, uint8_t stage, uint8_t ev, int ret, const char *detail,
     uint64_t a, uint64_t b)
{
	struct pg106_devboot_event e;

	if (!r->cfg->event)
		return;
	e.stage = stage;
	e.ev = ev;
	e.ret = ret;
	e.detail = detail;
	e.a = a;
	e.b = b;
	r->cfg->event(r->cfg->event_user, &e);
}

static int
fail(struct run *r, uint8_t stage, int ret, const char *detail, uint64_t a,
     uint64_t b)
{
	r->db->ret = ret;
	r->db->stage = stage;
	emit(r, stage, PG106_DB_EV_FAIL, ret, detail, a, b);
	return ret;
}

static void
ok(struct run *r, uint8_t stage, const char *detail, uint64_t a, uint64_t b)
{
	r->db->done_mask_ |= (uint8_t)(1u << stage);
	r->db->stage = stage;
	emit(r, stage, PG106_DB_EV_OK, 0, detail, a, b);
}

// A plan step failed: report the step and its citation (a = step index,
// b = address).
static int
fail_exec(struct run *r, uint8_t stage)
{
	const struct pg106_hw_result *x = &r->db->exec;

	return fail(r, stage, x->ret, x->why ? x->why : "executor refused the plan",
		    x->step, x->addr);
}

static bool
pri_error(uint32_t v)
{
	// 0xbadf.... is the PRI ring's "no response" signature
	// (pascal/diag.h, pascal-egpu FINDINGS).
	return (v & 0xfff00000u) == 0xbad00000u || v == 0xffffffffu;
}

// ---- IDENTIFY / POSTED / POST -------------------------------------------------

static int
st_identify(struct run *r)
{
	struct pg106_pmc_probe p;

	emit(r, PG106_DB_IDENTIFY, PG106_DB_EV_BEGIN, 0, "read PMC_BOOT_0", 0, 0);
	r->db->boot0 = r->bus->rd32(r->bus->user, PG106_PMC_BOOT_0);
	p = pg106_pmc_probe(r->db->boot0);
	if (r->db->boot0 == 0 || r->db->boot0 == 0xffffffffu)
		return fail(r, PG106_DB_IDENTIFY, -ENODEV,
			    "BOOT_0 unreadable (BAR0 not decoding / GPU powered off)",
			    r->db->boot0, 0);
	if (p.status != PG106_PMC_OK || !p.is_gp106)
		return fail(r, PG106_DB_IDENTIFY, -ENODEV,
			    "BOOT_0 is not a GP106", r->db->boot0, p.architecture);
	ok(r, PG106_DB_IDENTIFY, "GP106", r->db->boot0, p.revision);
	return 0;
}

static int
st_posted(struct run *r, bool *need_post)
{
	uint32_t v = r->bus->rd32(r->bus->user, PG106_DEVINIT_POSTED_REG);

	if (pri_error(v))
		return fail(r, PG106_DB_POSTED, -EIO,
			    "0x2240c unreadable (PRI error)", v, 0);
	r->db->posted_before = (v & PG106_DEVINIT_POSTED_BIT) != 0;
	*need_post = pg106_devinit_needs_post(v, r->cfg->force_post);
	ok(r, PG106_DB_POSTED,
	   *need_post ? "not posted: POST required" : "posted", v, *need_post);
	return 0;
}

static int
st_post(struct run *r)
{
	struct pg106_vbios_bit vb;
	struct pg106_pmu_bus pb;
	int ret;

	emit(r, PG106_DB_POST, PG106_DB_EV_BEGIN, 0,
	     "gm200_devinit_post (VBIOS PMU DEVINIT app)", r->cfg->vbios_size, 0);
	if (!r->cfg->vbios || !r->cfg->vbios_size)
		return fail(r, PG106_DB_POST, -ENOENT,
			    "GPU not posted and no VBIOS to POST with", 0, 0);
	ret = pg106_vbios_bit_parse(r->cfg->vbios, r->cfg->vbios_size, &vb);
	if (ret)
		return fail(r, PG106_DB_POST, ret, "VBIOS BIT did not parse", 0, 0);
	pb = pg106_hw_pmu_bus(r->bus);
	ret = pg106_devinit_gm200_post(&vb, true, true, &pb, NULL, 0,
				       &r->db->post);
	if (ret)
		return fail(r, PG106_DB_POST, ret,
			    r->db->post.error ? r->db->post.error : "POST failed",
			    r->db->post.polls, r->db->post.steps);
	r->db->posted = true;
	ok(r, PG106_DB_POST, "DEVINIT done (0x10a040 bit 13)", r->db->post.polls,
	   r->db->post.steps);
	return 0;
}

// ---- TOP ------------------------------------------------------------------------

static int
top_rd(void *u, uint32_t addr, uint32_t *v)
{
	const struct pg106_hw_bus *bus = u;

	*v = bus->rd32(bus->user, addr);
	return 0;
}

static int
st_top(struct run *r)
{
	const struct pg106_top_device *d;
	int ret;

	ret = pg106_top_scan(top_rd, (void *)r->bus, &r->db->top);
	if (ret)
		return fail(r, PG106_DB_TOP, ret, "PTOP scan failed", 0, 0);
	r->db->sec2_base = pg106_acrboot_falcon_base(&r->db->top,
						     PG106_ACRBOOT_SEC2_BASE_GP106);
	d = pg106_top_find(&r->db->top, PG106_TOP_TYPE_SEC2, 0);
	r->db->sec2_reset = d ? pg106_top_reset_mask(d) : 0;
	ok(r, PG106_DB_TOP,
	   d ? "SEC2 from PTOP" : "SEC2 not in PTOP: measured fallback 0x087000",
	   r->db->sec2_base, r->db->sec2_reset);
	return 0;
}

// ---- ACR --------------------------------------------------------------------------

static bool
have(const struct pg106_fw_blob *b)
{
	return b->data && b->size;
}

static int
parse_fw(struct run *r)
{
	const struct pg106_devboot_fw *fw = r->cfg->fw;
	struct pg106_devboot *db = r->db;
	size_t n = 0;
	int ret = 0;

	if (!fw || !have(&fw->acr_bl) || !have(&fw->acr_ucode_load) ||
	    !have(&fw->fecs_bl) || !have(&fw->fecs_inst) ||
	    !have(&fw->fecs_data) || !have(&fw->fecs_sig) ||
	    !have(&fw->gpccs_bl) || !have(&fw->gpccs_inst) ||
	    !have(&fw->gpccs_data) || !have(&fw->gpccs_sig) ||
	    !have(&fw->sec2_desc) || !have(&fw->sec2_image) ||
	    !have(&fw->sec2_sig))
		return fail(r, PG106_DB_ACR, -ENOENT,
			    "ACR/GR/SEC2 firmware incomplete", 0, 0);
	ret |= pg106_acr_ucode_parse_bl(fw->acr_bl.data, fw->acr_bl.size, &db->bl);
	ret |= pg106_acr_ucode_parse_hs(fw->acr_ucode_load.data,
					fw->acr_ucode_load.size, &db->hs);
	ret |= pg106_acr_lsf_sig_parse(fw->fecs_sig.data, fw->fecs_sig.size,
				       &db->sig[0]);
	ret |= pg106_acr_lsf_sig_parse(fw->gpccs_sig.data, fw->gpccs_sig.size,
				       &db->sig[1]);
	ret |= pg106_acr_lsf_sig_parse(fw->sec2_sig.data, fw->sec2_sig.size,
				       &db->sig[2]);
	if (ret)
		return fail(r, PG106_DB_ACR, -EINVAL,
			    "ACR ucode or LS signature did not parse", 0, 0);
	if (db->sig[0].falcon_id != PG106_ACR_LSF_FECS ||
	    db->sig[1].falcon_id != PG106_ACR_LSF_GPCCS ||
	    db->sig[2].falcon_id != PG106_ACR_LSF_SEC2)
		return fail(r, PG106_DB_ACR, -EINVAL,
			    "LS signature falcon ids are not FECS/GPCCS/SEC2", 0, 0);

	// FECS, GPCCS images (nvkm_acr_lsfw_load_bl_inst_data_sig), then SEC2
	// (nvkm_acr_lsfw_load_sig_image_desc_v1): acr->lsfw order.
	ret = pg106_acr_lsfw_from_bl_inst_data(
		fw->fecs_bl.data, fw->fecs_bl.size, fw->fecs_inst.data,
		fw->fecs_inst.size, fw->fecs_data.data, fw->fecs_data.size,
		PG106_ACR_LSF_FECS, NULL, 0, &n, &db->lsfw[0]);
	if (!ret && !(db->fecs_img = malloc(n)))
		ret = -ENOMEM;
	if (!ret)
		ret = pg106_acr_lsfw_from_bl_inst_data(
			fw->fecs_bl.data, fw->fecs_bl.size, fw->fecs_inst.data,
			fw->fecs_inst.size, fw->fecs_data.data,
			fw->fecs_data.size, PG106_ACR_LSF_FECS, db->fecs_img, n,
			&n, &db->lsfw[0]);
	if (!ret)
		ret = pg106_acr_lsfw_from_bl_inst_data(
			fw->gpccs_bl.data, fw->gpccs_bl.size,
			fw->gpccs_inst.data, fw->gpccs_inst.size,
			fw->gpccs_data.data, fw->gpccs_data.size,
			PG106_ACR_LSF_GPCCS, NULL, 0, &n, &db->lsfw[1]);
	if (!ret && !(db->gpccs_img = malloc(n)))
		ret = -ENOMEM;
	if (!ret)
		ret = pg106_acr_lsfw_from_bl_inst_data(
			fw->gpccs_bl.data, fw->gpccs_bl.size,
			fw->gpccs_inst.data, fw->gpccs_inst.size,
			fw->gpccs_data.data, fw->gpccs_data.size,
			PG106_ACR_LSF_GPCCS, db->gpccs_img, n, &n, &db->lsfw[1]);
	if (!ret)
		ret = pg106_acr_lsfw_from_ls_desc_v1(
			fw->sec2_desc.data, fw->sec2_desc.size,
			fw->sec2_image.data, fw->sec2_image.size,
			PG106_ACR_LSF_SEC2, &db->lsfw[2]);
	if (ret)
		return fail(r, PG106_DB_ACR, ret, "LS falcon images did not compose",
			    0, 0);
	return 0;
}

static int
st_acr(struct run *r)
{
	struct pg106_devboot *db = r->db;
	const struct pg106_acr_lsf_signature_v1 *sigs[3];
	struct pg106_acrboot_config c;
	const uint32_t base = db->sec2_base;
	int ret;

	emit(r, PG106_DB_ACR, PG106_DB_EV_BEGIN, 0,
	     "gp102 ACR HS boot + SEC2 RTOS start", base, 0);
	ret = parse_fw(r);
	if (ret)
		return ret;

	memset(&c, 0, sizeof(c));
	c.falcon_base = base;
	// The [READBACK] fields come from the chip itself, so the plan's
	// READ_REG steps check that nothing changed between here and there.
	c.hwcfg = r->bus->rd32(r->bus->user, base + PG106_FALCON_MEM_CAPS);
	c.debug_reg = r->bus->rd32(r->bus->user, base + PG106_FALCON_SEC2_DEBUG);
	if (pri_error(c.hwcfg) || PG106_FALCON_HWCFG_CODE_LIMIT(c.hwcfg) == 0)
		return fail(r, PG106_DB_ACR, -ENODEV,
			    "SEC2 HWCFG unreadable (PRI error / falcon gated)",
			    c.hwcfg, base);
	// The plan's last step starts the SEC2 RTOS through 0x100 or 0x130
	// depending on CPUCTL bit 6 as read AFTER the HS ucode ran
	// (nvkm_falcon_v1_start, falcon/v1.c:93-101). The plan cannot branch,
	// so it is built for 0x100 and that last write is re-issued below
	// from a fresh read.
	c.cpuctl_alias_en = false;
	c.vma_addr = r->cfg->mem.vma_addr;
	c.wpr_alloc_base = r->cfg->mem.wpr_alloc_base;
	c.bootstrap_owner = PG106_ACR_LSF_SEC2; // acr/gp102.c:89
	c.rtos_id = PG106_ACR_LSF_SEC2;         // acr/base.c:59-72
	c.boot_mbox0 = 0;                       // acr/gp102.c:239
	c.intr_clear = 0x10;                    // acr/gp102.c:239
	c.pmc_boot0 = db->boot0;
	c.pmc_mask = db->sec2_reset;
	c.bind_target = r->cfg->mem.bind_target;
	c.bind_addr = r->cfg->mem.bind_addr;

	sigs[0] = &db->sig[0];
	sigs[1] = &db->sig[1];
	sigs[2] = &db->sig[2];
	ret = pg106_acrboot_plan_build(&c, &db->bl, &db->hs, db->lsfw, 3, sigs,
				       &db->acr_plan, &db->acr_info);
	if (ret)
		return fail(r, PG106_DB_ACR, ret, "ACR plan did not build", 0, 0);
	emit(r, PG106_DB_ACR, PG106_DB_EV_INFO, 0, "ACR plan built (steps, wpr size)",
	     db->acr_plan.nsteps, db->acr_info.wpr_size);
	if (db->acr_plan.nsteps == 0)
		return fail(r, PG106_DB_ACR, -EINVAL, "empty ACR plan", 0, 0);
	{
		const size_t last = db->acr_plan.nsteps - 1;
		const struct pg106_acrboot_step *s = &db->acr_plan.steps[last];
		uint32_t cpuctl;

		if (s->kind != PG106_ACRBOOT_WRITE_REG ||
		    s->addr != base + PG106_FALCON_CPUCTL)
			return fail(r, PG106_DB_ACR, -EINVAL,
				    "ACR plan does not end with the SEC2 RTOS start",
				    db->acr_plan.nsteps, 0);
		ret = pg106_hwexec_acr_range(r->bus, &db->acr_plan, 0, last,
					     &db->exec);
		if (ret)
			return fail_exec(r, PG106_DB_ACR);
		cpuctl = r->bus->rd32(r->bus->user, base + PG106_FALCON_CPUCTL);
		r->bus->wr32(r->bus->user,
			     base + ((cpuctl & 0x40u) ? PG106_FALCON_CPUCTL_ALIAS
						      : PG106_FALCON_CPUCTL),
			     s->value);
		emit(r, PG106_DB_ACR, PG106_DB_EV_INFO, 0,
		     "SEC2 RTOS start (falcon/v1.c:93-101; a = CPUCTL, b = alias)",
		     cpuctl, (cpuctl & 0x40u) != 0);
	}
	ok(r, PG106_DB_ACR, "HS ACR done, SEC2 RTOS started", db->exec.step,
	   db->exec.waited_us);
	return 0;
}

// ---- GR ----------------------------------------------------------------------------

#define GR_BASE 0x00400000ull	  // gf100.c:2641-2650
#define FECS_BASE 0x00409000u
#define GPCCS_BASE 0x0041a000u
#define GPC_UNIT(g, r) (0x500000u + (uint32_t)(g) * 0x8000u + (r)) // gf100.h:43

int
pg106_devboot_gr_discover(const struct pg106_hw_bus *bus,
			  struct pg106_grboot_config *c, size_t sw_nonctx_size)
{
	uint32_t g, p, v;

	memset(c, 0, sizeof(*c));
	c->gr_base = GR_BASE;
	c->fecs_base = FECS_BASE;
	c->gpccs_base = GPCCS_BASE;
	c->reset_war = false; // GP107/GP108 only (gf100.c:2070)
	c->pmc_enable = bus->rd32(bus->user, PG106_GRBOOT_PMC_ENABLE);
	// gf100_gr_oneinit (gf100.c:2009-2033), ppc_nr = 3 for gp102/gp104
	// (gp102.c:125, gp104.c:53).
	v = bus->rd32(bus->user, FECS_BASE + 0x604u);
	c->gpc_nr = v & 0x1fu;
	if (pri_error(v) || c->gpc_nr == 0 || c->gpc_nr > PG106_GRBOOT_GPC_MAX)
		return -ENODEV;
	for (g = 0; g < c->gpc_nr; g++) {
		v = bus->rd32(bus->user, GPC_UNIT(g, 0x2608));
		c->tpc_nr[g] = (uint8_t)(v > 0xff ? 0xff : v); // plan_build bounds it
		for (p = 0; p < 3; p++) {
			v = bus->rd32(bus->user, GPC_UNIT(g, 0x0c30 + p * 4));
			c->ppc_tpc_mask[g][p] = (uint8_t)(v > 0xff ? 0xff : v);
		}
		c->swdx_pes[g] = (uint8_t)(bus->rd32(bus->user,
						     GPC_UNIT(g, 0x0c50)) & 0xf);
	}
	c->rop_config = bus->rd32(bus->user, PG106_GRBOOT_PFB_12006C); // gm200.c:91-95
	c->fbp_count = c->rop_config & 0xfu;                          // gp100.c:98
	c->ltc_active = bus->rd32(bus->user, PG106_GRBOOT_PFB_100800);
	c->ltc_magma = bus->rd32(bus->user, PG106_GRBOOT_PFB_100804);
	c->pri_mmu_rsvd = bus->rd32(bus->user, PG106_GRBOOT_PFB_100C80);
	c->mmu_wr = bus->rd32(bus->user, PG106_GRBOOT_PFB_100CC4);
	c->mmu_wr_rd = bus->rd32(bus->user, PG106_GRBOOT_PFB_100CC8);
	c->mmu_rd = bus->rd32(bus->user, PG106_GRBOOT_PFB_100CCC);
	// Placeholders: FECS answers these during the plan (observed).
	c->ctx_size = 1;
	c->size_zcull = 1;
	c->size_pm = 1;
	c->sw_nonctx_size = (uint32_t)sw_nonctx_size;
	c->zbc_init = true;
	return 0;
}

static void
gr_observe(void *u, size_t i, const struct pg106_grboot_step *s, uint32_t v)
{
	struct pg106_devboot *db = u;

	(void)i;
	// The three FECS size queries are the full-word ANY polls
	// (gf100.c:924-970).
	if (s->kind == PG106_GRBOOT_POLL_REG && s->mask == 0xffffffffu &&
	    db->gr_nsizes < 3)
		db->gr_sizes[db->gr_nsizes++] = v;
}

static int
st_gr(struct run *r)
{
	struct pg106_devboot *db = r->db;
	const struct pg106_fw_blob *pack = &r->cfg->fw->sw_nonctx;
	struct pg106_flcn_bus fb;
	int ret;

	emit(r, PG106_DB_GR, PG106_DB_EV_BEGIN, 0, "GR init (gf100_gr_init)", 0, 0);
	if (!have(pack))
		return fail(r, PG106_DB_GR, -ENOENT,
			    "gr/sw_nonctx.bin missing (gm200_gr_load needs it)", 0, 0);
	ret = pg106_devboot_gr_discover(r->bus, &db->gr_cfg, pack->size);
	if (ret)
		return fail(r, PG106_DB_GR, ret, "0x409604 reports no GPCs",
			    db->gr_cfg.gpc_nr, 0);
	ret = pg106_grboot_plan_build(&db->gr_cfg, &db->gr_plan, &db->gr_info);
	if (ret)
		return fail(r, PG106_DB_GR, ret,
			    "GR plan did not build from the discovered topology",
			    db->gr_cfg.gpc_nr, db->gr_cfg.tpc_nr[0]);
	emit(r, PG106_DB_GR, PG106_DB_EV_INFO, 0, "GR topology (gpcs, tpcs)",
	     db->gr_cfg.gpc_nr, db->gr_info.tpc_total);
	ret = pg106_hwexec_gr(r->bus, &db->gr_plan, 0, db->gr_info.bootstrap_before,
			      pack->data, pack->size, gr_observe, db, &db->exec);
	if (ret)
		return fail_exec(r, PG106_DB_GR);

	// gf100_gr_init_ctxctl_ext: nvkm_acr_bootstrap_falcons(FECS|GPCCS)
	// sits between mc_unk260(0) and mc_unk260(1) (gf100.c:1777-1783).
	emit(r, PG106_DB_SEC2_BOOTSTRAP, PG106_DB_EV_BEGIN, 0,
	     "SEC2 BOOTSTRAP_FALCON(FECS, GPCCS)", db->sec2_base, 0);
	if (r->cfg->bootstrap) {
		ret = r->cfg->bootstrap(r->cfg->bootstrap_user,
					(1ull << PG106_ACR_LSF_FECS) |
						(1ull << PG106_ACR_LSF_GPCCS));
	} else {
		fb = pg106_hw_flcn_bus(r->bus);
		pg106_sec2_ctx_init(&db->sec2, &fb, db->sec2_base,
				    r->cfg->sec2_max_polls ? r->cfg->sec2_max_polls
							   : 1000);
		ret = pg106_acr_bootstrap_falcons(
			&db->sec2, (1ull << PG106_ACR_LSF_FECS) |
					   (1ull << PG106_ACR_LSF_GPCCS));
	}
	if (ret)
		return fail(r, PG106_DB_SEC2_BOOTSTRAP, ret,
			    ret == -ETIMEDOUT ? "SEC2 RTOS did not answer"
					      : "SEC2 refused BOOTSTRAP_FALCON",
			    db->sec2.initmsg, db->sec2.unhandled_intr);
	ok(r, PG106_DB_SEC2_BOOTSTRAP, "FECS and GPCCS bootstrapped",
	   db->sec2.cmdq.offset, db->sec2.msgq.offset);

	ret = pg106_hwexec_gr(r->bus, &db->gr_plan, db->gr_info.bootstrap_before,
			      db->gr_plan.nsteps, pack->data, pack->size,
			      gr_observe, db, &db->exec);
	if (ret)
		return fail_exec(r, PG106_DB_GR);
	ok(r, PG106_DB_GR, "GR up (FECS ctx size, compute class)",
	   db->gr_nsizes ? db->gr_sizes[0] : 0, db->gr_info.compute_class);
	return 0;
}

// ---- driver -----------------------------------------------------------------------

int
pg106_devboot_run(struct pg106_devboot *db, const struct pg106_devboot_cfg *cfg)
{
	struct run r;
	bool need_post = false;
	int ret;

	if (!db)
		return -EINVAL;
	memset(db, 0, sizeof(*db));
	if (!cfg || !cfg->bus || !cfg->bus->rd32 || !cfg->bus->wr32 ||
	    !cfg->bus->delay_us) {
		db->ret = -EINVAL;
		return db->ret;
	}
	r.db = db;
	r.cfg = cfg;
	r.bus = cfg->bus;

	if ((ret = st_identify(&r)) || cfg->stop_after == PG106_DB_IDENTIFY)
		return ret;
	if ((ret = st_posted(&r, &need_post)) || cfg->stop_after == PG106_DB_POSTED)
		return ret;
	if (need_post) {
		if ((ret = st_post(&r)))
			return ret;
	} else {
		emit(&r, PG106_DB_POST, PG106_DB_EV_SKIP, 0,
		     "already posted (0x2240c bit 1)", 0, 0);
	}
	if (cfg->stop_after == PG106_DB_POST)
		return 0;
	if ((ret = st_top(&r)) || cfg->stop_after == PG106_DB_TOP)
		return ret;
	if ((ret = st_acr(&r)) || cfg->stop_after == PG106_DB_ACR)
		return ret;
	if ((ret = st_gr(&r)))
		return ret;
	ok(&r, PG106_DB_READY, "bring-up complete", db->gr_info.tpc_total,
	   db->gr_nsizes ? db->gr_sizes[0] : 0);
	return 0;
}

int
pg106_devboot_gr(struct pg106_devboot *db, const struct pg106_devboot_cfg *cfg)
{
	struct run r;
	int ret;

	if (!db || !cfg || !cfg->bus || !cfg->fw)
		return -EINVAL;
	r.db = db;
	r.cfg = cfg;
	r.bus = cfg->bus;
	db->ret = 0;
	ret = st_gr(&r);
	if (!ret)
		ok(&r, PG106_DB_READY, "bring-up complete", db->gr_info.tpc_total,
		   db->gr_nsizes ? db->gr_sizes[0] : 0);
	return ret;
}

void
pg106_devboot_fini(struct pg106_devboot *db)
{
	if (!db)
		return;
	pg106_acrboot_plan_free(&db->acr_plan);
	pg106_grboot_plan_free(&db->gr_plan);
	free(db->fecs_img);
	free(db->gpccs_img);
	db->fecs_img = NULL;
	db->gpccs_img = NULL;
}

const char *
pg106_devboot_stage_name(uint8_t s)
{
	static const char *const n[PG106_DB_STAGE_COUNT] = {
		"identify", "posted", "post", "top", "acr", "gr",
		"sec2-bootstrap", "ready",
	};
	return s < PG106_DB_STAGE_COUNT ? n[s] : "?";
}

const char *
pg106_devboot_ev_name(uint8_t ev)
{
	switch (ev) {
	case PG106_DB_EV_BEGIN: return "begin";
	case PG106_DB_EV_OK:    return "ok";
	case PG106_DB_EV_SKIP:  return "skip";
	case PG106_DB_EV_FAIL:  return "FAIL";
	case PG106_DB_EV_INFO:  return "info";
	default:                return "?";
	}
}
