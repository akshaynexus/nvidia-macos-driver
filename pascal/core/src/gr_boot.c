// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/src/gr_boot.c — GP106 GR (PGRAPH) bring-up SIMULATOR: builds
// the ordered step list nouveau's nvkm GR executes after the ACR layer has
// securely booted FECS/GPCCS, derived from nvkm/engine/gr of linux master
// (see gr_boot.h for the full provenance + falconless verdict).
//
// SIMULATION, NOT HARDWARE PROOF: produces a MODEL replayed against a mock
// bus in pascal/tests/test_gr_boot.c. No hardware is touched or claimed.
//
// The build mirrors the real call order end to end:
//   gf100_gr_oneinit  (gf100.c:1989-2062)  -> DISCOVERY group
//   gf100_gr_init_    (gf100.c:2064-2113)  -> RESET group
//   gf100_gr_init     (gf100.c:2342-2464)  -> GPC_MMU / MMIO_PACK /
//                                             ENGINE_CFG / EXCEPTIONS / ZBC
//   gf100_gr_init_ctxctl_ext (gf100.c:1750-1829) -> FECS_BOOT group
//   gf100_gr_fini     (gf100.c:2115-2126)  -> UNLOAD group
// with the GP102/GP106 func selects (gp102.c:105-124) applied: gm200
// init_gpc_mmu/init_num_active_ltcs, gp100 init_rop_active_fbps/
// init_fecs_exceptions/init_shader_exceptions, gp102 init_swdx_pes_mask,
// gk104 vsc_stream_master/sked_hww_esr/ppc_exceptions, gf117 init_zcull,
// gm107 init_504430.

#include <pascal/gr_boot.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Unit-address helpers (gf100.h:41-45, gr_base-parameterized)
// ---------------------------------------------------------------------------

static uint64_t
gpc_unit(const struct pg106_grboot_config *cfg, uint32_t g, uint32_t off)
{
	return cfg->gr_base + PG106_GRBOOT_GPC_BASE +
	       (uint64_t)g * PG106_GRBOOT_GPC_STRIDE + off;
}

static uint64_t
tpc_unit(const struct pg106_grboot_config *cfg, uint32_t g, uint32_t t,
	 uint32_t off)
{
	return cfg->gr_base + PG106_GRBOOT_TPC_BASE +
	       (uint64_t)g * PG106_GRBOOT_GPC_STRIDE +
	       (uint64_t)t * PG106_GRBOOT_TPC_STRIDE + off;
}

static uint64_t
ppc_unit(const struct pg106_grboot_config *cfg, uint32_t g, uint32_t p,
	 uint32_t off)
{
	return cfg->gr_base + PG106_GRBOOT_PPC_BASE +
	       (uint64_t)g * PG106_GRBOOT_GPC_STRIDE +
	       (uint64_t)p * PG106_GRBOOT_PPC_STRIDE + off;
}

static uint64_t
rop_unit(const struct pg106_grboot_config *cfg, uint32_t r, uint32_t off)
{
	return cfg->gr_base + PG106_GRBOOT_ROP_BASE +
	       (uint64_t)r * PG106_GRBOOT_ROP_STRIDE + off;
}

static uint64_t
gpc_bcast(const struct pg106_grboot_config *cfg, uint32_t off)
{
	return cfg->gr_base + PG106_GRBOOT_GPC_BCAST_BASE + off;
}

// ---------------------------------------------------------------------------
// Step emission
// ---------------------------------------------------------------------------

static int
push_step(struct pg106_grboot_plan *plan, const struct pg106_grboot_step *s)
{
	if (plan->nsteps == 0 || (plan->nsteps & (plan->nsteps - 1)) == 0) {
		size_t cap = plan->nsteps ? plan->nsteps * 2 : 64;
		struct pg106_grboot_step *n =
			realloc(plan->steps, cap * sizeof(*plan->steps));

		if (!n)
			return -1;
		plan->steps = n;
	}
	plan->steps[plan->nsteps++] = *s;
	return 0;
}

static int
push_wr(struct pg106_grboot_plan *plan, uint16_t group, uint64_t addr,
	uint32_t value, uint8_t hw, const char *why)
{
	struct pg106_grboot_step s;

	memset(&s, 0, sizeof(s));
	s.kind = PG106_GRBOOT_WRITE_REG;
	s.space = PG106_GRBOOT_MMIO;
	s.group = group;
	s.hw_value = hw;
	s.addr = addr;
	s.value = value;
	s.why = why;
	return push_step(plan, &s);
}

static int
push_blob(struct pg106_grboot_plan *plan, uint16_t group, uint32_t len,
	  const char *why)
{
	struct pg106_grboot_step s;

	memset(&s, 0, sizeof(s));
	s.kind = PG106_GRBOOT_WRITE_BLOB;
	s.space = PG106_GRBOOT_FW;
	s.group = group;
	s.blob = NULL; // firmware-provided contents, not simulated
	s.blob_len = len;
	s.why = why;
	return push_step(plan, &s);
}

static int
push_poll(struct pg106_grboot_plan *plan, uint16_t group, uint64_t addr,
	  uint32_t mask, uint8_t op, uint32_t value, uint8_t hw,
	  const char *why)
{
	struct pg106_grboot_step s;

	memset(&s, 0, sizeof(s));
	s.kind = PG106_GRBOOT_POLL_REG;
	s.space = PG106_GRBOOT_MMIO;
	s.poll_op = op;
	s.group = group;
	s.hw_value = hw;
	s.addr = addr;
	s.value = value;
	s.mask = mask;
	s.why = why;
	return push_step(plan, &s);
}

static int
push_rd(struct pg106_grboot_plan *plan, uint16_t group, uint64_t addr,
	uint32_t mask, uint32_t expect, uint8_t hw, const char *why)
{
	struct pg106_grboot_step s;

	memset(&s, 0, sizeof(s));
	s.kind = PG106_GRBOOT_READ_REG;
	s.space = PG106_GRBOOT_MMIO;
	s.group = group;
	s.hw_value = hw;
	s.addr = addr;
	s.value = expect;
	s.mask = mask;
	s.why = why;
	return push_step(plan, &s);
}

// ---------------------------------------------------------------------------
// Discovery math: ppc_tpc_nr/min/max + tile mapping + zcull magic, mirroring
// gf100_gr_oneinit (gf100.c:2009-2033) and gf100_gr_oneinit_tiles
// (gf100.c:1914-1987) incl. the gm200 special cases (gm200.c:158-181).
// ---------------------------------------------------------------------------

struct gr_discovery {
	uint32_t tpc_total;
	uint32_t ppc_total;
	uint8_t ppc_tpc_max;
	uint8_t ppc_tpc_min;
	uint32_t rop_nr;
	uint32_t magicgpc918;
	uint8_t screen_tile_row_offset;
	uint8_t tile[256]; // TPC_MAX — gf100.h:38
};

static uint8_t
hweight8(uint8_t m)
{
	uint8_t w = 0;

	while (m) {
		w = (uint8_t)(w + (m & 1));
		m >>= 1;
	}
	return w;
}

static int
compute_tiles(const struct pg106_grboot_config *cfg, struct gr_discovery *d)
{
	static const uint8_t primes[] = { 3, 5, 7, 11, 13, 17, 19, 23, 29, 31,
					  37, 41, 43, 47, 53, 59, 61 };
	// gm200.c:137-149 (gm200_gr_tile_map_{2_8,4_16,6_24})
	static const uint8_t tile_map_2_8[] = { 0, 1, 1, 0, 0, 1, 1, 0 };
	static const uint8_t tile_map_4_16[] = { 0, 1, 2, 3, 2, 3, 0, 1,
						 3, 0, 1, 2, 1, 2, 3, 0 };
	static const uint8_t tile_map_6_24[] = { 0, 1, 2, 3, 4, 5, 3, 4, 5,
						 0, 1, 2, 0, 1, 2, 3, 4, 5,
						 3, 4, 5, 0, 1, 2 };
	uint8_t gpc_map[32], tpc_max = 0;
	uint32_t mul_factor, comm_denom;
	int init_frac[32], init_err[32], run_err[32];
	bool sorted, have_ppc = false;
	int i, j;

	memset(d, 0, sizeof(*d));

	// gf100.c:2011-2033
	for (i = 0; i < (int)cfg->gpc_nr; i++) {
		d->tpc_total += cfg->tpc_nr[i];
		if (cfg->tpc_nr[i] > tpc_max)
			tpc_max = cfg->tpc_nr[i];
		for (j = 0; j < 3; j++) { // func->ppc_nr == 3 (gp102.c:123)
			uint8_t w = hweight8(cfg->ppc_tpc_mask[i][j]);

			if (w == 0)
				continue;
			d->ppc_total++;
			if (d->ppc_tpc_min == 0 || w < d->ppc_tpc_min)
				d->ppc_tpc_min = w;
			if (w > d->ppc_tpc_max)
				d->ppc_tpc_max = w;
			have_ppc = true;
		}
	}
	if (d->tpc_total == 0)
		return -EINVAL;
	if (!have_ppc)
		d->ppc_tpc_max = 0;
	if (tpc_max == 0)
		return -EINVAL;

	// gm200_gr_rops — gm200.c:92-95 (gp102 func .rops, gp102.c:122)
	d->rop_nr = cfg->rop_config;
	// gf117.c:127 (gf117_gr_init_zcull)
	d->magicgpc918 = (0x00800000u + d->tpc_total - 1) / d->tpc_total;

	// gf100.c:1924-1943
	switch (d->tpc_total) {
	case 15: d->screen_tile_row_offset = 0x06; break;
	case 14: d->screen_tile_row_offset = 0x05; break;
	case 13: d->screen_tile_row_offset = 0x02; break;
	case 11: d->screen_tile_row_offset = 0x07; break;
	case 10: d->screen_tile_row_offset = 0x06; break;
	case 7:
	case 5:  d->screen_tile_row_offset = 0x01; break;
	case 3:  d->screen_tile_row_offset = 0x02; break;
	case 2:
	case 1:  d->screen_tile_row_offset = 0x01; break;
	default:
		d->screen_tile_row_offset = 0x03;
		for (i = 0; i < (int)(sizeof(primes) / sizeof(primes[0]));
		     i++) {
			if (d->tpc_total % primes[i]) {
				d->screen_tile_row_offset = primes[i];
				break;
			}
		}
		break;
	}

	// gm200.c:167-180 hardcodes
	if (cfg->gpc_nr == 2 && d->tpc_total == 8) {
		memcpy(d->tile, tile_map_2_8, d->tpc_total);
		d->screen_tile_row_offset = 1;
		return 0;
	}
	if (cfg->gpc_nr == 4 && d->tpc_total == 16) {
		memcpy(d->tile, tile_map_4_16, d->tpc_total);
		d->screen_tile_row_offset = 4;
		return 0;
	}
	if (cfg->gpc_nr == 6 && d->tpc_total == 24) {
		memcpy(d->tile, tile_map_6_24, d->tpc_total);
		d->screen_tile_row_offset = 5;
		return 0;
	}

	// gf100.c:1945-1987
	for (i = 0; i < (int)cfg->gpc_nr; i++)
		gpc_map[i] = (uint8_t)i;
	sorted = false;
	while (!sorted) {
		sorted = true;
		for (i = 0; i + 1 < (int)cfg->gpc_nr; i++) {
			if (cfg->tpc_nr[gpc_map[i + 1]] >
			    cfg->tpc_nr[gpc_map[i]]) {
				uint8_t swap = gpc_map[i];

				gpc_map[i] = gpc_map[i + 1];
				gpc_map[i + 1] = swap;
				sorted = false;
			}
		}
	}

	mul_factor = cfg->gpc_nr * tpc_max;
	if (mul_factor & 1)
		mul_factor = 2;
	else
		mul_factor = 1;
	comm_denom = cfg->gpc_nr * tpc_max * mul_factor;

	for (i = 0; i < (int)cfg->gpc_nr; i++) {
		init_frac[i] = (int)(cfg->tpc_nr[gpc_map[i]] * cfg->gpc_nr *
				     mul_factor);
		init_err[i] = (int)(i * tpc_max * mul_factor - comm_denom / 2);
		run_err[i] = init_frac[i] + init_err[i];
	}

	for (i = 0; (uint32_t)i < d->tpc_total; i++) {
		for (j = 0; j < (int)cfg->gpc_nr; j++) {
			if (run_err[j] * 2 >= (int)comm_denom) {
				d->tile[i] = gpc_map[j];
				run_err[j] += init_frac[j] - (int)comm_denom;
				break;
			}
			run_err[j] += init_frac[j];
		}
	}
	return 0;
}

// ---------------------------------------------------------------------------
// Plan build
// ---------------------------------------------------------------------------

int
pg106_grboot_plan_build(const struct pg106_grboot_config *cfg,
			struct pg106_grboot_plan *plan,
			struct pg106_grboot_info *info)
{
	struct gr_discovery d;
	struct pg106_grboot_plan p;
	size_t i;
	int g, t, pp, rop, ret;

	if (!cfg || !plan || !info)
		return -EINVAL;
	if (cfg->fecs_base == 0 || cfg->gpccs_base == 0 || cfg->gr_base == 0)
		return -EINVAL;
	if (cfg->gpc_nr == 0 || cfg->gpc_nr > PG106_GRBOOT_GPC_MAX)
		return -EINVAL;
	if (cfg->sw_nonctx_size % 4 != 0)
		return -EINVAL;
	if (cfg->reset_war && (cfg->pmc_enable & PG106_GRBOOT_PMC_GR) == 0)
		return -EINVAL;
	for (g = 0; g < (int)cfg->gpc_nr; g++) {
		if (cfg->tpc_nr[g] > PG106_GRBOOT_TPC_MAX_PER_GPC)
			return -EINVAL;
		if (cfg->swdx_pes[g] > 0x0f)
			return -EINVAL;
	}

	ret = compute_tiles(cfg, &d);
	if (ret)
		return ret;

	memset(&p, 0, sizeof(p));

	// ---- G_RESET: engine quiesce (+ optional GP107/GP108 WAR) ----------
	// gf100_gr_init_ (gf100.c:2064-2113): the NvGrResetWar pulse is
	// gated on chipset 0x137/0x138 (GP107/GP108, gf100.c:2070); GP106
	// (0x136) leaves it off. Kept because the layer above may target
	// GP107-class silicon.
	if (cfg->reset_war) {
		if (push_wr(&p, PG106_GRBOOT_G_RESET, PG106_GRBOOT_PMC_ENABLE,
			    cfg->pmc_enable & ~PG106_GRBOOT_PMC_GR, 0,
			    "nvkm/engine/gr/gf100.c:2088-2095 gf100_gr_init_ — "
			    "NvGrResetWar: nvkm_mask(0x200, 0x1000, 0) + "
			    "readback, 50ms stall") != 0)
			goto oom;
		if (push_wr(&p, PG106_GRBOOT_G_RESET, PG106_GRBOOT_PMC_ENABLE,
			    cfg->pmc_enable | PG106_GRBOOT_PMC_GR, 0,
			    "nvkm/engine/gr/gf100.c:2090-2094 gf100_gr_init_ — "
			    "NvGrResetWar: nvkm_mask(0x200, 0x1000, 0x1000) "
			    "after the stall") != 0)
			goto oom;
	}
	if (push_wr(&p, PG106_GRBOOT_G_RESET,
		    cfg->gr_base + PG106_GRBOOT_OFF_400500, 0x00000000, 0,
		    "nvkm/engine/gr/gf100.c:2348 gf100_gr_init — "
		    "nvkm_mask(0x400500, 0x00010001, 0x00000000) (GR intr "
		    "quiesce)") != 0)
		goto oom;

	// ---- G_DISCOVERY: fuse/strap report reads ([HW-VALUE]) -------------
	// gf100_gr_oneinit (gf100.c:1989-2062) reads hardware-provided
	// configuration; every read here is [HW-VALUE].
	if (push_rd(&p, PG106_GRBOOT_G_DISCOVERY,
		    cfg->fecs_base + PG106_GRBOOT_FECSC_OFF_409604,
		    0x0000001fu, cfg->gpc_nr, 1,
		    "nvkm/engine/gr/gf100.c:2010 gf100_gr_oneinit — gpc_nr = "
		    "nvkm_rd32(0x409604) & 0x1f [HW-VALUE: fuse/strap report]") != 0)
		goto oom;
	for (g = 0; g < (int)cfg->gpc_nr; g++) {
		if (push_rd(&p, PG106_GRBOOT_G_DISCOVERY,
			    gpc_unit(cfg, g, PG106_GRBOOT_GPC_OFF_2608),
			    0x000000ffu, cfg->tpc_nr[g], 1,
			    "nvkm/engine/gr/gf100.c:2012 gf100_gr_oneinit — "
			    "tpc_nr[gpc] = nvkm_rd32(GPC_UNIT(g,0x2608)) "
			    "[HW-VALUE: fuse report]") != 0)
			goto oom;
		for (pp = 0; pp < 3; pp++) {
			if (push_rd(&p, PG106_GRBOOT_G_DISCOVERY,
				    gpc_unit(cfg, g,
					     PG106_GRBOOT_GPC_OFF_0C30 +
					     pp * 4u),
				    0x000000ffu, cfg->ppc_tpc_mask[g][pp], 1,
				    "nvkm/engine/gr/gf100.c:2016-2017 "
				    "gf100_gr_oneinit — ppc_tpc_mask = "
				    "nvkm_rd32(GPC_UNIT(g,0x0c30+j*4)) "
				    "[HW-VALUE: fuse report]") != 0)
				goto oom;
		}
		if (push_rd(&p, PG106_GRBOOT_G_DISCOVERY,
			    gpc_unit(cfg, g, PG106_GRBOOT_GPC_OFF_0C50),
			    0x0000000fu, cfg->swdx_pes[g], 1,
			    "nvkm/engine/gr/gp102.c:91-93 "
			    "gp102_gr_init_swdx_pes_mask — nvkm_rd32("
			    "GPC_UNIT(gpc,0x0c50)) & 0xf [HW-VALUE: strap "
			    "report]") != 0)
			goto oom;
	}
	if (push_rd(&p, PG106_GRBOOT_G_DISCOVERY, PG106_GRBOOT_PFB_12006C,
		    0x0000000fu, cfg->fbp_count, 1,
		    "nvkm/engine/gr/gp100.c:94-100 "
		    "gp100_gr_init_rop_active_fbps — fbp_count = "
		    "nvkm_rd32(0x12006c) & 0xf [HW-VALUE: fuse report]") != 0)
		goto oom;
	if (push_rd(&p, PG106_GRBOOT_G_DISCOVERY, PG106_GRBOOT_PFB_12006C,
		    0xffffffffu, cfg->rop_config, 1,
		    "nvkm/engine/gr/gm200.c:92-95 gm200_gr_rops (gp102 func "
		    ".rops, gp102.c:122) — rop_nr = nvkm_rd32(0x12006c) "
		    "[HW-VALUE: fuse report]") != 0)
		goto oom;
	if (push_rd(&p, PG106_GRBOOT_G_DISCOVERY, PG106_GRBOOT_PFB_100800,
		    0xffffffffu, cfg->ltc_active, 1,
		    "nvkm/engine/gr/gm200.c:106-111 "
		    "gm200_gr_init_num_active_ltcs — nvkm_rd32(0x100800) "
		    "[HW-VALUE: fuse report]") != 0)
		goto oom;
	if (push_rd(&p, PG106_GRBOOT_G_DISCOVERY, PG106_GRBOOT_PFB_100804,
		    0xffffffffu, cfg->ltc_magma, 1,
		    "nvkm/engine/gr/gm200.c:106-111 "
		    "gm200_gr_init_num_active_ltcs — nvkm_rd32(0x100804) "
		    "[HW-VALUE: fuse report]") != 0)
		goto oom;
	if (push_rd(&p, PG106_GRBOOT_G_DISCOVERY, PG106_GRBOOT_PFB_100C80,
		    0xf0001fffu, cfg->pri_mmu_rsvd, 1,
		    "nvkm/engine/gr/gm200.c:114-125 gm200_gr_init_gpc_mmu — "
		    "nvkm_rd32(0x100c80) & 0xf0001fff [HW-VALUE: hw state]") != 0)
		goto oom;
	if (push_rd(&p, PG106_GRBOOT_G_DISCOVERY, PG106_GRBOOT_PFB_100CC8,
		    0xffffffffu, cfg->mmu_wr_rd, 1,
		    "nvkm/engine/gr/gm200.c:114-125 gm200_gr_init_gpc_mmu — "
		    "nvkm_rd32(0x100cc8) [HW-VALUE: hw state]") != 0)
		goto oom;
	if (push_rd(&p, PG106_GRBOOT_G_DISCOVERY, PG106_GRBOOT_PFB_100CCC,
		    0xffffffffu, cfg->mmu_rd, 1,
		    "nvkm/engine/gr/gm200.c:114-125 gm200_gr_init_gpc_mmu — "
		    "nvkm_rd32(0x100ccc) [HW-VALUE: hw state]") != 0)
		goto oom;
	if (push_rd(&p, PG106_GRBOOT_G_DISCOVERY, PG106_GRBOOT_PFB_100CC4,
		    0xffffffffu, cfg->mmu_wr, 1,
		    "nvkm/engine/gr/gm200.c:114-125 gm200_gr_init_gpc_mmu — "
		    "nvkm_rd32(0x100cc4) [HW-VALUE: hw state]") != 0)
		goto oom;

	// ---- G_GPC_MMU: gm200_gr_init_gpc_mmu ------------------------------
	if (push_wr(&p, PG106_GRBOOT_G_GPC_MMU,
		    cfg->gr_base + PG106_GRBOOT_OFF_418880,
		    cfg->pri_mmu_rsvd & 0xf0001fffu, 1,
		    "nvkm/engine/gr/gm200.c:114-125 gm200_gr_init_gpc_mmu "
		    "(gp102 func .init_gpc_mmu, gp102.c:107; gf100.c:2350 "
		    "call) — nvkm_wr32(0x418880, rd32(0x100c80) & 0xf0001fff) "
		    "[HW-VALUE: derived from fuse-region read]") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_GPC_MMU,
		    cfg->gr_base + PG106_GRBOOT_OFF_418890, 0x00000000, 0,
		    "nvkm/engine/gr/gm200.c:119 gm200_gr_init_gpc_mmu — "
		    "nvkm_wr32(0x418890, 0)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_GPC_MMU,
		    cfg->gr_base + PG106_GRBOOT_OFF_418894, 0x00000000, 0,
		    "nvkm/engine/gr/gm200.c:120 gm200_gr_init_gpc_mmu — "
		    "nvkm_wr32(0x418894, 0)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_GPC_MMU,
		    cfg->gr_base + PG106_GRBOOT_OFF_4188B4, cfg->mmu_wr_rd, 1,
		    "nvkm/engine/gr/gm200.c:122 gm200_gr_init_gpc_mmu — "
		    "nvkm_wr32(0x4188b4, rd32(0x100cc8)) [HW-VALUE: derived]") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_GPC_MMU,
		    cfg->gr_base + PG106_GRBOOT_OFF_4188B8, cfg->mmu_rd, 1,
		    "nvkm/engine/gr/gm200.c:123 gm200_gr_init_gpc_mmu — "
		    "nvkm_wr32(0x4188b8, rd32(0x100ccc)) [HW-VALUE: derived]") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_GPC_MMU,
		    cfg->gr_base + PG106_GRBOOT_OFF_4188B0, cfg->mmu_wr, 1,
		    "nvkm/engine/gr/gm200.c:124 gm200_gr_init_gpc_mmu — "
		    "nvkm_wr32(0x4188b0, rd32(0x100cc4)) [HW-VALUE: derived]") != 0)
		goto oom;

	// ---- G_MMIO_PACK: sw_nonctx register pack --------------------------
	if (push_blob(&p, PG106_GRBOOT_G_MMIO_PACK, cfg->sw_nonctx_size,
		      "nvkm/engine/gr/gf100.c:2352-2362 gf100_gr_init — with "
		      "signed firmware (gr->firmware, gm200_gr_load "
		      "gm200.c:241) the non-context MMIO pack is gr->sw_nonctx"
		      " (sw_nonctx.bin via gk20a_gr_load_sw) applied by "
		      "gf100_gr_mmio (gf100.c:1079-1093); pack contents are "
		      "firmware data — modeled as an opaque blob") != 0)
		goto oom;

	// ---- G_ENGINE_CFG: wait_idle + per-unit configuration --------------
	if (push_poll(&p, PG106_GRBOOT_G_ENGINE_CFG,
		      cfg->gr_base + PG106_GRBOOT_OFF_40060C, 0x00000001u,
		      PG106_GRBOOT_POLL_EQ, 0x00000000u, 0,
		      "nvkm/engine/gr/gf100.c:1050-1076 gf100_gr_wait_idle "
		      "(gf100.c:2364 call) — poll 0x40060c bit0 (GR busy) "
		      "== 0; also polls PMC 0x200 bit12 and flushes 0x400700 "
		      "(gf100.c:1062)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_ENGINE_CFG, gpc_unit(cfg, 0, 0x3018),
		    0x00000001, 0,
		    "nvkm/engine/gr/gk104.c:429-434 "
		    "gk104_gr_init_vsc_stream_master (gp102 func "
		    ".init_vsc_stream_master, gp102.c:108; gf100.c:2375 call)"
		    " — nvkm_wr32(GPC_UNIT(0,0x3018), 1)") != 0)
		goto oom;

	// init_zcull = gf117_gr_init_zcull (gp102 func .init_zcull,
	// gp102.c:109; gf100.c:2376 call)
	for (i = 0; i < d.tpc_total; i += 8) {
		uint32_t data = 0;
		uint8_t bank[PG106_GRBOOT_GPC_MAX];
		uint32_t j2;

		memset(bank, 0, sizeof(bank));
		for (j2 = 0; j2 < 8 && i + j2 < d.tpc_total; j2++) {
			data |= (uint32_t)bank[d.tile[i + j2]] << (j2 * 4);
			bank[d.tile[i + j2]]++;
		}
		if (push_wr(&p, PG106_GRBOOT_G_ENGINE_CFG,
			    gpc_bcast(cfg, 0x0980 + (uint32_t)(i / 8) * 4),
			    data, 1,
			    "nvkm/engine/gr/gf117.c:134-140 "
			    "gf117_gr_init_zcull — tile->bank packing to "
			    "GPC_BCAST(0x0980+i/8*4); tile[] from oneinit "
			    "[HW-VALUE: derived from fuse TPC counts]") != 0)
			goto oom;
	}
	for (g = 0; g < (int)cfg->gpc_nr; g++) {
		if (push_wr(&p, PG106_GRBOOT_G_ENGINE_CFG,
			    gpc_unit(cfg, g, 0x0914),
			    ((uint32_t)d.screen_tile_row_offset << 8) |
				    cfg->tpc_nr[g],
			    1,
			    "nvkm/engine/gr/gf117.c:142-144 "
			    "gf117_gr_init_zcull — GPC_UNIT(g,0x0914) = "
			    "screen_tile_row_offset<<8 | tpc_nr[g] "
			    "[HW-VALUE: derived from fuse TPC counts]") != 0)
			goto oom;
		if (push_wr(&p, PG106_GRBOOT_G_ENGINE_CFG,
			    gpc_unit(cfg, g, 0x0910),
			    0x00040000u | d.tpc_total, 1,
			    "nvkm/engine/gr/gf117.c:145-146 "
			    "gf117_gr_init_zcull — GPC_UNIT(g,0x0910) = "
			    "0x00040000 | tpc_total [HW-VALUE: derived from "
			    "fuse TPC counts]") != 0)
			goto oom;
		if (push_wr(&p, PG106_GRBOOT_G_ENGINE_CFG,
			    gpc_unit(cfg, g, 0x0918), d.magicgpc918, 1,
			    "nvkm/engine/gr/gf117.c:127,147 "
			    "gf117_gr_init_zcull — GPC_UNIT(g,0x0918) = "
			    "magicgpc918 = DIV_ROUND_UP(0x00800000, "
			    "tpc_total) [HW-VALUE: derived from fuse TPC "
			    "counts]") != 0)
			goto oom;
	}
	if (push_wr(&p, PG106_GRBOOT_G_ENGINE_CFG, gpc_bcast(cfg, 0x3fd4),
		    d.magicgpc918, 1,
		    "nvkm/engine/gr/gf117.c:150 gf117_gr_init_zcull — "
		    "GPC_BCAST(0x3fd4) = magicgpc918 [HW-VALUE: derived from "
		    "fuse TPC counts]") != 0)
		goto oom;

	// init_num_active_ltcs = gm200 (gp102 func, gp102.c:110; gf100.c:2377)
	if (push_wr(&p, PG106_GRBOOT_G_ENGINE_CFG, gpc_bcast(cfg, 0x08ac),
		    cfg->ltc_active, 1,
		    "nvkm/engine/gr/gm200.c:106-111 "
		    "gm200_gr_init_num_active_ltcs (gf100.c:2377 call) — "
		    "nvkm_wr32(GPC_BCAST(0x08ac), rd32(0x100800)) "
		    "[HW-VALUE: derived]") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_ENGINE_CFG, gpc_bcast(cfg, 0x033c),
		    cfg->ltc_magma, 1,
		    "nvkm/engine/gr/gm200.c:106-111 "
		    "gm200_gr_init_num_active_ltcs (gf100.c:2377 call) — "
		    "nvkm_wr32(GPC_BCAST(0x033c), rd32(0x100804)) "
		    "[HW-VALUE: derived]") != 0)
		goto oom;

	// init_rop_active_fbps = gp100 (gp102 func, gp102.c:111; gf100.c:2378)
	if (push_wr(&p, PG106_GRBOOT_G_ENGINE_CFG,
		    cfg->gr_base + PG106_GRBOOT_OFF_408850, cfg->fbp_count, 1,
		    "nvkm/engine/gr/gp100.c:94-100 "
		    "gp100_gr_init_rop_active_fbps (gf100.c:2378 call) — "
		    "nvkm_mask(0x408850, 0xf, fbp_count) zrop [HW-VALUE: "
		    "derived from fuse FBP count]") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_ENGINE_CFG,
		    cfg->gr_base + PG106_GRBOOT_OFF_408958, cfg->fbp_count, 1,
		    "nvkm/engine/gr/gp100.c:94-100 "
		    "gp100_gr_init_rop_active_fbps (gf100.c:2378 call) — "
		    "nvkm_mask(0x408958, 0xf, fbp_count) crop [HW-VALUE: "
		    "derived from fuse FBP count]") != 0)
		goto oom;

	// init_swdx_pes_mask = gp102 (gp102 func, gp102.c:109; gf100.c:2382)
	{
		uint32_t mask = 0;

		for (g = 0; g < (int)cfg->gpc_nr; g++)
			mask |= (uint32_t)cfg->swdx_pes[g] << (g * 4);
		if (push_wr(&p, PG106_GRBOOT_G_ENGINE_CFG,
			    cfg->gr_base + PG106_GRBOOT_OFF_4181D0, mask, 1,
			    "nvkm/engine/gr/gp102.c:86-97 "
			    "gp102_gr_init_swdx_pes_mask (gf100.c:2382 call) "
			    "— nvkm_wr32(0x4181d0, packed GPC_UNIT(g,0x0c50) "
			    "nibbles) [HW-VALUE: derived from strap reads]") != 0)
			goto oom;
	}

	if (push_wr(&p, PG106_GRBOOT_G_ENGINE_CFG,
		    cfg->gr_base + PG106_GRBOOT_OFF_400500, 0x00010001, 0,
		    "nvkm/engine/gr/gf100.c:2387 gf100_gr_init — "
		    "nvkm_wr32(0x400500, 0x00010001) (intr enable)") != 0)
		goto oom;

	// ---- G_EXCEPTIONS: exception register init --------------------------
	if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
		    cfg->gr_base + PG106_GRBOOT_OFF_400100, 0xffffffff, 0,
		    "nvkm/engine/gr/gf100.c:2389 gf100_gr_init — "
		    "nvkm_wr32(0x400100, 0xffffffff) (intr clear-all)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
		    cfg->gr_base + PG106_GRBOOT_OFF_40013C, 0xffffffff, 0,
		    "nvkm/engine/gr/gf100.c:2390 gf100_gr_init — "
		    "nvkm_wr32(0x40013c, 0xffffffff)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
		    cfg->gr_base + PG106_GRBOOT_OFF_400124, 0x00000002, 0,
		    "nvkm/engine/gr/gf100.c:2391 gf100_gr_init — "
		    "nvkm_wr32(0x400124, 0x00000002)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
		    cfg->fecs_base + PG106_GRBOOT_FECSC_OFF_409C24, 0x000e0002,
		    0,
		    "nvkm/engine/gr/gp100.c:88-91 gp100_gr_init_fecs_exceptions"
		    " (gp102 func .init_fecs_exceptions, gp102.c:113; "
		    "gf100.c:2393 call) — nvkm_wr32(0x409c24, 0x000e0002)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
		    cfg->gr_base + PG106_GRBOOT_OFF_405848, 0xc0000000, 0,
		    "nvkm/engine/gr/gm200.c:98-103 gm200_gr_init_ds_hww_esr_2 "
		    "(gp102 func, gp102.c:114; gf100.c:2398 call) — "
		    "nvkm_wr32(0x405848, 0xc0000000)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
		    cfg->gr_base + PG106_GRBOOT_OFF_40584C, 0x00000001, 0,
		    "nvkm/engine/gr/gm200.c:98-103 gm200_gr_init_ds_hww_esr_2 "
		    "— nvkm_mask(0x40584c, 0x1, 0x1)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
		    cfg->gr_base + PG106_GRBOOT_OFF_404000, 0xc0000000, 0,
		    "nvkm/engine/gr/gf100.c:2401 gf100_gr_init — "
		    "nvkm_wr32(0x404000, 0xc0000000) (DISPATCH trap clear)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
		    cfg->gr_base + PG106_GRBOOT_OFF_404600, 0xc0000000, 0,
		    "nvkm/engine/gr/gf100.c:2402 gf100_gr_init — "
		    "nvkm_wr32(0x404600, 0xc0000000) (M2MF trap clear)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
		    cfg->gr_base + PG106_GRBOOT_OFF_408030, 0xc0000000, 0,
		    "nvkm/engine/gr/gf100.c:2403 gf100_gr_init — "
		    "nvkm_wr32(0x408030, 0xc0000000) (CCACHE trap clear)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
		    cfg->gr_base + PG106_GRBOOT_OFF_406018, 0xc0000000, 0,
		    "nvkm/engine/gr/gf100.c:2408 gf100_gr_init — "
		    "nvkm_wr32(0x406018, 0xc0000000) (UNK6 trap clear)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
		    cfg->gr_base + PG106_GRBOOT_OFF_404490, 0xc0000000, 0,
		    "nvkm/engine/gr/gf100.c:2409 gf100_gr_init — "
		    "nvkm_wr32(0x404490, 0xc0000000) (MACRO trap clear)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
		    cfg->gr_base + PG106_GRBOOT_OFF_407020, 0x40000000, 0,
		    "nvkm/engine/gr/gk104.c:390-394 gk104_gr_init_sked_hww_esr"
		    " (gp102 func, gp102.c:115; gf100.c:2411 call) — "
		    "nvkm_wr32(0x407020, 0x40000000)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
		    cfg->gr_base + PG106_GRBOOT_OFF_405840, 0xc0000000, 0,
		    "nvkm/engine/gr/gf100.c:2414 gf100_gr_init — "
		    "nvkm_wr32(0x405840, 0xc0000000) (SHADER trap clear)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
		    cfg->gr_base + PG106_GRBOOT_OFF_405844, 0x00ffffff, 0,
		    "nvkm/engine/gr/gf100.c:2415 gf100_gr_init — "
		    "nvkm_wr32(0x405844, 0x00ffffff) (SHADER hww esr mask)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
		    cfg->gr_base + PG106_GRBOOT_OFF_419CC0, 0x00000008, 0,
		    "nvkm/engine/gr/gf100.c:2237-2243 gf100_gr_init_419cc0 "
		    "(gp102 func .init_419cc0, gp102.c:116; gf100.c:2417 "
		    "call) — nvkm_mask(0x419cc0, 0x8, 0x8) + per-TPC 0x48c "
		    "clears") != 0)
		goto oom;
	// init_ppc_exceptions = gk104 (gp102 func, gp102.c:118; gf100.c:2424)
	for (g = 0; g < (int)cfg->gpc_nr; g++) {
		for (pp = 0; pp < 3; pp++) {
			if (cfg->ppc_tpc_mask[g][pp] == 0)
				continue;
			if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
				    ppc_unit(cfg, g, pp, 0x038), 0xc0000000,
				    0,
				    "nvkm/engine/gr/gk104.c:414-427 "
				    "gk104_gr_init_ppc_exceptions "
				    "(gf100.c:2424 call) — nvkm_wr32("
				    "PPC_UNIT(g,p,0x038), 0xc0000000)") != 0)
				goto oom;
		}
	}
	// per-GPC/TPC exception clears (gf100.c:2427-2444)
	for (g = 0; g < (int)cfg->gpc_nr; g++) {
		if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
			    gpc_unit(cfg, g, 0x0420), 0xc0000000, 0,
			    "nvkm/engine/gr/gf100.c:2428 gf100_gr_init — "
			    "nvkm_wr32(GPC_UNIT(g,0x0420), 0xc0000000) "
			    "(PROP)") != 0)
			goto oom;
		if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
			    gpc_unit(cfg, g, 0x0900), 0xc0000000, 0,
			    "nvkm/engine/gr/gf100.c:2429 gf100_gr_init — "
			    "nvkm_wr32(GPC_UNIT(g,0x0900), 0xc0000000) "
			    "(ZCULL)") != 0)
			goto oom;
		if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
			    gpc_unit(cfg, g, 0x1028), 0xc0000000, 0,
			    "nvkm/engine/gr/gf100.c:2430 gf100_gr_init — "
			    "nvkm_wr32(GPC_UNIT(g,0x1028), 0xc0000000) "
			    "(CCACHE)") != 0)
			goto oom;
		if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
			    gpc_unit(cfg, g, 0x0824), 0xc0000000, 0,
			    "nvkm/engine/gr/gf100.c:2431 gf100_gr_init — "
			    "nvkm_wr32(GPC_UNIT(g,0x0824), 0xc0000000) "
			    "(ESETUP)") != 0)
			goto oom;
		for (t = 0; t < (int)cfg->tpc_nr[g]; t++) {
			if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
				    tpc_unit(cfg, g, t, 0x0508), 0xffffffff,
				    0,
				    "nvkm/engine/gr/gf100.c:2433 gf100_gr_init"
				    " — nvkm_wr32(TPC_UNIT(g,t,0x508), "
				    "0xffffffff)") != 0)
				goto oom;
			if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
				    tpc_unit(cfg, g, t, 0x050c), 0xffffffff,
				    0,
				    "nvkm/engine/gr/gf100.c:2434 gf100_gr_init"
				    " — nvkm_wr32(TPC_UNIT(g,t,0x50c), "
				    "0xffffffff)") != 0)
				goto oom;
			if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
				    tpc_unit(cfg, g, t, 0x0224), 0xc0000000,
				    0,
				    "nvkm/engine/gr/gf100.c:2223-2228 "
				    "gf100_gr_init_tex_hww_esr (gp102 func "
				    ".init_tex_hww_esr, gp102.c:118; "
				    "gf100.c:2435 call) — TPC_UNIT(g,t,0x224) "
				    "= 0xc0000000") != 0)
				goto oom;
			if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
				    tpc_unit(cfg, g, t, 0x0084), 0xc0000000,
				    0,
				    "nvkm/engine/gr/gf100.c:2437 gf100_gr_init"
				    " — nvkm_wr32(TPC_UNIT(g,t,0x084), "
				    "0xc0000000) (POLY)") != 0)
				goto oom;
			if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
				    tpc_unit(cfg, g, t, 0x0430), 0xc0000000,
				    0,
				    "nvkm/engine/gr/gm107.c:301-306 "
				    "gm107_gr_init_504430 (gp102 func "
				    ".init_504430, gp102.c:119; gf100.c:2438 "
				    "call) — nvkm_wr32(TPC_UNIT(g,t,0x430), "
				    "0xc0000000)") != 0)
				goto oom;
			if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
				    tpc_unit(cfg, g, t, 0x0644), 0x00dffffe,
				    0,
				    "nvkm/engine/gr/gp100.c:71-77 "
				    "gp100_gr_init_shader_exceptions (gp102 "
				    "func .init_shader_exceptions, gp102.c:120"
				    "; gf100.c:2440 call) — TPC_UNIT(g,t,0x644)"
				    " = 0x00dffffe") != 0)
				goto oom;
			if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
				    tpc_unit(cfg, g, t, 0x064c), 0x00000105,
				    0,
				    "nvkm/engine/gr/gp100.c:71-77 "
				    "gp100_gr_init_shader_exceptions — "
				    "TPC_UNIT(g,t,0x64c) = 0x00000105") != 0)
				goto oom;
		}
		if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
			    gpc_unit(cfg, g, 0x2c90), 0xffffffff, 0,
			    "nvkm/engine/gr/gf100.c:2442 gf100_gr_init — "
			    "nvkm_wr32(GPC_UNIT(g,0x2c90), 0xffffffff)") != 0)
			goto oom;
		if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
			    gpc_unit(cfg, g, 0x2c94), 0xffffffff, 0,
			    "nvkm/engine/gr/gf100.c:2443 gf100_gr_init — "
			    "nvkm_wr32(GPC_UNIT(g,0x2c94), 0xffffffff)") != 0)
			goto oom;
	}
	// init_rop_exceptions = gf100 (gp102 func, gp102.c:121; gf100.c:2446)
	for (rop = 0; (uint32_t)rop < d.rop_nr; rop++) {
		if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
			    rop_unit(cfg, rop, 0x144), 0x40000000, 0,
			    "nvkm/engine/gr/gf100.c:2201-2213 "
			    "gf100_gr_init_rop_exceptions (gf100.c:2446 call)"
			    " — ROP_UNIT(r,0x144) = 0x40000000") != 0)
			goto oom;
		if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
			    rop_unit(cfg, rop, 0x070), 0x40000000, 0,
			    "nvkm/engine/gr/gf100.c:2201-2213 "
			    "gf100_gr_init_rop_exceptions — ROP_UNIT(r,0x070)"
			    " = 0x40000000") != 0)
			goto oom;
		if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
			    rop_unit(cfg, rop, 0x204), 0xffffffff, 0,
			    "nvkm/engine/gr/gf100.c:2201-2213 "
			    "gf100_gr_init_rop_exceptions — ROP_UNIT(r,0x204)"
			    " = 0xffffffff") != 0)
			goto oom;
		if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
			    rop_unit(cfg, rop, 0x208), 0xffffffff, 0,
			    "nvkm/engine/gr/gf100.c:2201-2213 "
			    "gf100_gr_init_rop_exceptions — ROP_UNIT(r,0x208)"
			    " = 0xffffffff") != 0)
			goto oom;
	}
	if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
		    cfg->gr_base + PG106_GRBOOT_OFF_400108, 0xffffffff, 0,
		    "nvkm/engine/gr/gf100.c:2448 gf100_gr_init — "
		    "nvkm_wr32(0x400108, 0xffffffff) (trap clear-all)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
		    cfg->gr_base + PG106_GRBOOT_OFF_400138, 0xffffffff, 0,
		    "nvkm/engine/gr/gf100.c:2449 gf100_gr_init — "
		    "nvkm_wr32(0x400138, 0xffffffff)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
		    cfg->gr_base + PG106_GRBOOT_OFF_400118, 0xffffffff, 0,
		    "nvkm/engine/gr/gf100.c:2450 gf100_gr_init — "
		    "nvkm_wr32(0x400118, 0xffffffff) (GPC traps)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
		    cfg->gr_base + PG106_GRBOOT_OFF_400130, 0xffffffff, 0,
		    "nvkm/engine/gr/gf100.c:2451 gf100_gr_init — "
		    "nvkm_wr32(0x400130, 0xffffffff)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
		    cfg->gr_base + PG106_GRBOOT_OFF_40011C, 0xffffffff, 0,
		    "nvkm/engine/gr/gf100.c:2192-2199 "
		    "gf100_gr_init_exception2 (gp102 func .init_exception2, "
		    "gp102.c:121; gf100.c:2452 call) — nvkm_wr32(0x40011c, "
		    "0xffffffff)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_EXCEPTIONS,
		    cfg->gr_base + PG106_GRBOOT_OFF_400134, 0xffffffff, 0,
		    "nvkm/engine/gr/gf100.c:2192-2199 "
		    "gf100_gr_init_exception2 — nvkm_wr32(0x400134, "
		    "0xffffffff)") != 0)
		goto oom;

	// ---- G_ZBC: zero-bandwidth-clear defaults (config-gated) ------------
	// gf100_gr_zbc_init runs unconditionally at gf100.c:2458; the gp102
	// func has NO .init_400054 (gf100.c:2455 skips it for GP10x). Only
	// the first zero color/depth entries are emitted here (index 1);
	// the remaining default entries (gf100.c:1008-1030, f32 tables)
	// repeat the same register pattern.
	if (cfg->zbc_init) {
		if (push_wr(&p, PG106_GRBOOT_G_ZBC,
			    cfg->gr_base + PG106_GRBOOT_OFF_418010,
			    0x00000000, 0,
			    "nvkm/engine/gr/gp100.c:38-43 "
			    "gp100_gr_zbc_clear_color (zero entry of "
			    "gf100_gr_zbc_init, gf100.c:1020; gf100.c:2458 "
			    "call) — 0x418010 = ds[0] = 0") != 0)
			goto oom;
		if (push_wr(&p, PG106_GRBOOT_G_ZBC,
			    cfg->gr_base + PG106_GRBOOT_OFF_41804C,
			    0x00000000, 0,
			    "nvkm/engine/gr/gp100.c:38-43 "
			    "gp100_gr_zbc_clear_color — 0x41804c = ds[1] = "
			    "0") != 0)
			goto oom;
		if (push_wr(&p, PG106_GRBOOT_G_ZBC,
			    cfg->gr_base + PG106_GRBOOT_OFF_418088,
			    0x00000000, 0,
			    "nvkm/engine/gr/gp100.c:38-43 "
			    "gp100_gr_zbc_clear_color — 0x418088 = ds[2] = "
			    "0") != 0)
			goto oom;
		if (push_wr(&p, PG106_GRBOOT_G_ZBC,
			    cfg->gr_base + PG106_GRBOOT_OFF_4180C4,
			    0x00000000, 0,
			    "nvkm/engine/gr/gp100.c:38-43 "
			    "gp100_gr_zbc_clear_color — 0x4180c4 = ds[3] = "
			    "0") != 0)
			goto oom;
		if (push_wr(&p, PG106_GRBOOT_G_ZBC,
			    cfg->gr_base + PG106_GRBOOT_OFF_418100,
			    0x00000000, 0,
			    "nvkm/engine/gr/gp100.c:46-48 "
			    "gp100_gr_zbc_clear_color — nvkm_mask(0x418100, "
			    "0x7f, format=0)") != 0)
			goto oom;
		if (push_wr(&p, PG106_GRBOOT_G_ZBC,
			    cfg->gr_base + PG106_GRBOOT_OFF_418110,
			    0x00000000, 0,
			    "nvkm/engine/gr/gp100.c:57-63 "
			    "gp100_gr_zbc_clear_depth (zero entry of "
			    "gf100_gr_zbc_init, gf100.c:1024) — 0x418110 = "
			    "ds = 0") != 0)
			goto oom;
		if (push_wr(&p, PG106_GRBOOT_G_ZBC,
			    cfg->gr_base + PG106_GRBOOT_OFF_41814C,
			    0x00000000, 0,
			    "nvkm/engine/gr/gp100.c:57-63 "
			    "gp100_gr_zbc_clear_depth — nvkm_mask(0x41814c, "
			    "0x7f, format=0)") != 0)
			goto oom;
	}

	// ---- G_FECS_BOOT: falcon start + boot confirm + ctx queries --------
	// gf100_gr_init_ctxctl_ext (gf100.c:1750-1829): FECS/GPCCS are
	// ACR-managed on GP10x — nvkm_acr_bootstrap_falcons (gf100.c:1777-
	// 1781) is the PREVIOUS layer (pg106_acrboot_plan). This group starts
	// at the post-bootstrap handoff.
	if (push_wr(&p, PG106_GRBOOT_G_FECS_BOOT, cfg->fecs_base + 0x800u,
		    0x00000000, 0,
		    "nvkm/engine/gr/gf100.c:1786 gf100_gr_init_ctxctl_ext — "
		    "nvkm_wr32(0x409800, 0) (FECS mailbox ctrl clear)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_FECS_BOOT, cfg->gpccs_base + 0x10cu,
		    0x00000000, 0,
		    "nvkm/engine/gr/gf100.c:1787 gf100_gr_init_ctxctl_ext — "
		    "nvkm_wr32(0x41a10c, 0) (GPCCS)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_FECS_BOOT, cfg->fecs_base + 0x10cu,
		    0x00000000, 0,
		    "nvkm/engine/gr/gf100.c:1788 gf100_gr_init_ctxctl_ext — "
		    "nvkm_wr32(0x40910c, 0) (FECS)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_FECS_BOOT,
		    cfg->gpccs_base + PG106_GRBOOT_FALCON_CPUCTL, 0x00000002,
		    0,
		    "nvkm/engine/gr/gf100.c:1790 gf100_gr_init_ctxctl_ext — "
		    "nvkm_falcon_start(gpccs) -> nvkm_falcon_v1_start "
		    "(nvkm/falcon/v1.c:93-101, wr32 CPUCTL 0x100 = 2)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_FECS_BOOT,
		    cfg->fecs_base + PG106_GRBOOT_FALCON_CPUCTL, 0x00000002, 0,
		    "nvkm/engine/gr/gf100.c:1791 gf100_gr_init_ctxctl_ext — "
		    "nvkm_falcon_start(fecs) -> nvkm_falcon_v1_start "
		    "(nvkm/falcon/v1.c:93-101, wr32 CPUCTL 0x100 = 2)") != 0)
		goto oom;
	if (push_poll(&p, PG106_GRBOOT_G_FECS_BOOT, cfg->fecs_base + 0x800u,
		      0x00000001u, PG106_GRBOOT_POLL_ANY, 0x00000000u, 1,
		      "nvkm/engine/gr/gf100.c:1793-1797 "
		      "gf100_gr_init_ctxctl_ext — FECS falcon boot confirm: "
		      "poll 0x409800 bit0 (mailbox ready) [HW-VALUE: "
		      "firmware state]") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_FECS_BOOT, cfg->fecs_base + 0x800u,
		    0x00000000, 0,
		    "nvkm/engine/gr/gf100.c:972-980 "
		    "gf100_gr_fecs_set_watchdog_timeout (gf100.c:1799 call) — "
		    "nvkm_wr32(0x409800, 0)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_FECS_BOOT,
		    cfg->fecs_base + PG106_GRBOOT_FECSC_OFF_409500, 0x7fffffffu,
		    0,
		    "nvkm/engine/gr/gf100.c:972-980 "
		    "gf100_gr_fecs_set_watchdog_timeout — nvkm_wr32(0x409500, "
		    "0x7fffffff)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_FECS_BOOT,
		    cfg->fecs_base + PG106_GRBOOT_FECSC_OFF_409504,
		    0x00000021u, 0,
		    "nvkm/engine/gr/gf100.c:972-980 "
		    "gf100_gr_fecs_set_watchdog_timeout — nvkm_wr32(0x409504, "
		    "0x21) (mthd SET_WATCHDOG)") != 0)
		goto oom;

	// golden-context / ZCULL / PerfMon image-size queries — the answers
	// come from the booted FECS firmware [HW-VALUE].
	if (push_wr(&p, PG106_GRBOOT_G_FECS_BOOT, cfg->fecs_base + 0x800u,
		    0x00000000, 0,
		    "nvkm/engine/gr/gf100.c:956-963 "
		    "gf100_gr_fecs_discover_image_size (gf100.c:1802 call) — "
		    "nvkm_wr32(0x409800, 0)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_FECS_BOOT,
		    cfg->fecs_base + PG106_GRBOOT_FECSC_OFF_409500,
		    0x00000000, 0,
		    "nvkm/engine/gr/gf100.c:956-963 "
		    "gf100_gr_fecs_discover_image_size — nvkm_wr32(0x409500, "
		    "0)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_FECS_BOOT,
		    cfg->fecs_base + PG106_GRBOOT_FECSC_OFF_409504,
		    0x00000010u, 0,
		    "nvkm/engine/gr/gf100.c:956-963 "
		    "gf100_gr_fecs_discover_image_size — nvkm_wr32(0x409504, "
		    "0x10) (mthd DISCOVER_IMAGE_SIZE)") != 0)
		goto oom;
	if (push_rd(&p, PG106_GRBOOT_G_FECS_BOOT, cfg->fecs_base + 0x800u,
		    0xffffffffu, cfg->ctx_size, 1,
		    "nvkm/engine/gr/gf100.c:964-970,1801-1804 — gr->size read "
		    "back from 0x409800; consumed by gf100_gr_chan_bind "
		    "(gf100.c:327-334) [HW-VALUE: reported by FECS firmware]") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_FECS_BOOT, cfg->fecs_base + 0x800u,
		    0x00000000, 0,
		    "nvkm/engine/gr/gf100.c:940-947 "
		    "gf100_gr_fecs_discover_zcull_image_size (gf100.c:1807 "
		    "call) — nvkm_wr32(0x409800, 0)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_FECS_BOOT,
		    cfg->fecs_base + PG106_GRBOOT_FECSC_OFF_409500,
		    0x00000000, 0,
		    "nvkm/engine/gr/gf100.c:940-947 "
		    "gf100_gr_fecs_discover_zcull_image_size — "
		    "nvkm_wr32(0x409500, 0)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_FECS_BOOT,
		    cfg->fecs_base + PG106_GRBOOT_FECSC_OFF_409504,
		    0x00000016u, 0,
		    "nvkm/engine/gr/gf100.c:940-947 "
		    "gf100_gr_fecs_discover_zcull_image_size — "
		    "nvkm_wr32(0x409504, 0x16) (mthd DISCOVER_ZCULL_SIZE)") != 0)
		goto oom;
	if (push_rd(&p, PG106_GRBOOT_G_FECS_BOOT, cfg->fecs_base + 0x800u,
		    0xffffffffu, cfg->size_zcull, 1,
		    "nvkm/engine/gr/gf100.c:948-954,1806-1809 — "
		    "gr->size_zcull read back from 0x409800 [HW-VALUE: "
		    "reported by FECS firmware]") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_FECS_BOOT, cfg->fecs_base + 0x800u,
		    0x00000000, 0,
		    "nvkm/engine/gr/gf100.c:924-931 "
		    "gf100_gr_fecs_discover_pm_image_size (gf100.c:1812 call)"
		    " — nvkm_wr32(0x409800, 0)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_FECS_BOOT,
		    cfg->fecs_base + PG106_GRBOOT_FECSC_OFF_409500,
		    0x00000000, 0,
		    "nvkm/engine/gr/gf100.c:924-931 "
		    "gf100_gr_fecs_discover_pm_image_size — nvkm_wr32("
		    "0x409500, 0)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_FECS_BOOT,
		    cfg->fecs_base + PG106_GRBOOT_FECSC_OFF_409504,
		    0x00000025u, 0,
		    "nvkm/engine/gr/gf100.c:924-931 "
		    "gf100_gr_fecs_discover_pm_image_size — nvkm_wr32("
		    "0x409504, 0x25) (mthd DISCOVER_PM_SIZE)") != 0)
		goto oom;
	if (push_rd(&p, PG106_GRBOOT_G_FECS_BOOT, cfg->fecs_base + 0x800u,
		    0xffffffffu, cfg->size_pm, 1,
		    "nvkm/engine/gr/gf100.c:932-938,1811-1814 — gr->size_pm "
		    "read back from 0x409800 [HW-VALUE: reported by FECS "
		    "firmware]") != 0)
		goto oom;

	// ---- G_UNLOAD: fini / falcon disable -------------------------------
	// gf100_gr_fini (gf100.c:2115-2126) puts both falcons;
	// nvkm_falcon_put -> gm200_flcn_disable (nvkm/falcon/gm200.c:184-215).
	// The GR falcon func has no .reset_eng (gf100_gr_flcn,
	// gf100.c:2162-2167), so GP10x's 0x3c0 engine reset (falcon/
	// gp102.c:77-79) does NOT apply here.
	if (push_wr(&p, PG106_GRBOOT_G_UNLOAD,
		    cfg->gpccs_base + PG106_GRBOOT_FALCON_RM, 0x00000000, 0,
		    "nvkm/falcon/gm200.c:195 gm200_flcn_disable (gpccs "
		    "falcon_put in gf100_gr_fini, gf100.c:2123) — "
		    "nvkm_falcon_mask(0x048, 0x3, 0)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_UNLOAD,
		    cfg->gpccs_base + PG106_GRBOOT_FALCON_INTR_EN, 0xffffffff,
		    0,
		    "nvkm/falcon/gm200.c:196 gm200_flcn_disable — "
		    "nvkm_falcon_wr32(0x014, 0xffffffff)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_UNLOAD,
		    cfg->fecs_base + PG106_GRBOOT_FALCON_RM, 0x00000000, 0,
		    "nvkm/falcon/gm200.c:195 gm200_flcn_disable (fecs "
		    "falcon_put in gf100_gr_fini, gf100.c:2124) — "
		    "nvkm_falcon_mask(0x048, 0x3, 0)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_UNLOAD,
		    cfg->fecs_base + PG106_GRBOOT_FALCON_INTR_EN, 0xffffffff,
		    0,
		    "nvkm/falcon/gm200.c:196 gm200_flcn_disable — "
		    "nvkm_falcon_wr32(0x014, 0xffffffff)") != 0)
		goto oom;
	if (push_wr(&p, PG106_GRBOOT_G_UNLOAD, PG106_GRBOOT_PMC_ENABLE,
		    cfg->reset_war ? (cfg->pmc_enable & ~PG106_GRBOOT_PMC_GR)
				   : 0u,
		    0,
		    "nvkm/falcon/gm200.c:205 gm200_flcn_disable — "
		    "nvkm_mc_disable(GR): PMC 0x200 GR bit cleared") != 0)
		goto oom;

	// ---- info -----------------------------------------------------------
	memset(info, 0, sizeof(*info));
	info->tpc_total = d.tpc_total;
	info->ppc_total = d.ppc_total;
	info->ppc_tpc_max = d.ppc_tpc_max;
	info->ppc_tpc_min = d.ppc_tpc_min;
	info->rop_nr = d.rop_nr;
	info->magicgpc918 = d.magicgpc918;
	info->pagepool_size = PG106_GRCTX_PAGEPOOL_SIZE;
	info->bundle_size = PG106_GRCTX_BUNDLE_SIZE;
	// gp102_grctx_generate_attrib_cb_size (ctxgp102.c:86-95)
	{
		uint64_t size = (uint64_t)PG106_GRCTX_ALPHA_NR_MAX *
				d.tpc_total;

		for (g = 0; g < (int)cfg->gpc_nr; g++)
			size += (uint64_t)PG106_GRCTX_GFXP_NR * 3u *
				d.ppc_tpc_max;
		info->attrib_cb_size = ((size * 0x20) + 127) & ~(uint64_t)127;
	}
	info->ctx_size = cfg->ctx_size;
	info->compute_class = PG106_GRBOOT_COMPUTE_CLASS;
	info->nsteps = p.nsteps;

	*plan = p;
	return 0;

oom:
	free(p.steps);
	memset(plan, 0, sizeof(*plan));
	return -EINVAL;
}

void
pg106_grboot_plan_free(struct pg106_grboot_plan *plan)
{
	if (!plan)
		return;
	free(plan->steps);
	free(plan->scratch_);
	memset(plan, 0, sizeof(*plan));
}

const char *
pg106_grboot_kind_name(uint8_t kind)
{
	switch (kind) {
	case PG106_GRBOOT_WRITE_REG: return "write";
	case PG106_GRBOOT_WRITE_BLOB: return "blob";
	case PG106_GRBOOT_POLL_REG: return "poll";
	case PG106_GRBOOT_READ_REG: return "read";
	}
	return "?kind";
}

const char *
pg106_grboot_space_name(uint8_t space)
{
	switch (space) {
	case PG106_GRBOOT_MMIO: return "mmio";
	case PG106_GRBOOT_FW: return "fw";
	}
	return "?space";
}

const char *
pg106_grboot_poll_op_name(uint8_t op)
{
	switch (op) {
	case PG106_GRBOOT_POLL_ANY: return "any";
	case PG106_GRBOOT_POLL_EQ: return "eq";
	}
	return "?op";
}

const char *
pg106_grboot_group_name(uint8_t group)
{
	static const char *const names[PG106_GRBOOT_G_COUNT] = {
		"reset", "discovery", "gpc_mmu", "mmio_pack", "engine_cfg",
		"exceptions", "zbc", "fecs_boot", "unload",
	};

	return group < PG106_GRBOOT_G_COUNT ? names[group] : "?group";
}
