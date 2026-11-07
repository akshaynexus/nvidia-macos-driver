// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/src/gr_ctx.c — GR engine context for GP106: the golden
// context image and the per-channel context. See gr_ctx.h for the map to
// nouveau; every register write below cites its line (nvkm at Linux
// d48723711bae).

#include <pascal/dispatch.h>
#include <pascal/gpu.h>
#include <pascal/gr_ctx.h>
#include <pascal/pushbuf.h>
#include <pascal/qmd.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

// gf100.h:41-45 unit addresses (absolute BAR0, as nouveau writes them).
#define GPC_UNIT(t, r)	  (0x500000u + (uint32_t)(t) * 0x8000u + (r))
#define TPC_UNIT(t, m, r) (0x504000u + (uint32_t)(t) * 0x8000u + (uint32_t)(m) * 0x800u + (r))
#define PPC_UNIT(t, m, r) (0x503000u + (uint32_t)(t) * 0x8000u + (uint32_t)(m) * 0x200u + (r))

// gp104_grctx constants (ctxgp104.c:28-52).
#define BUNDLE_SIZE	       0x3000u
#define BUNDLE_MIN_GPM_FIFO    0x180u
#define BUNDLE_TOKEN_LIMIT     0x900u
#define PAGEPOOL_SIZE	       0x20000u
#define ATTRIB_NR_MAX	       0x4b0u
#define ATTRIB_NR	       0x320u
#define ALPHA_NR_MAX	       0xc00u
#define ALPHA_NR	       0x800u
#define GFXP_NR		       0xba8u

#define POLL_2S 2000000u

// ---- bus helpers ----------------------------------------------------------------

static uint32_t
rd(struct pg106_gr *gr, uint32_t a)
{
	return gr->cfg.bus->rd32(gr->cfg.bus->user, a);
}

static void
wr(struct pg106_gr *gr, uint32_t a, uint32_t v)
{
	gr->cfg.bus->wr32(gr->cfg.bus->user, a, v);
	gr->mmio_writes++;
}

// nvkm_mask (include/nvkm/core/device.h:131-136).
static uint32_t
mask(struct pg106_gr *gr, uint32_t a, uint32_t m, uint32_t v)
{
	uint32_t old = rd(gr, a);

	wr(gr, a, (old & ~m) | v);
	return old;
}

static void
delay(struct pg106_gr *gr, uint32_t us)
{
	gr->cfg.bus->delay_us(gr->cfg.bus->user, us);
}

// nvkm_msec(2000, if (!(rd32(a) & bits)) break): wait for bits to clear.
static int
wait_clear(struct pg106_gr *gr, uint32_t a, uint32_t bits)
{
	return pg106_hw_poll(gr->cfg.bus, a, bits, 0, false, POLL_2S, NULL, NULL);
}

// The FECS mailbox poll shape of gf100_gr_fecs_bind_pointer /
// _wfi_golden_save: `err` bit -> -EIO, `ok` bit -> 0, else -ETIMEDOUT.
static int
fecs_wait(struct pg106_gr *gr, uint32_t ok, uint32_t err)
{
	uint32_t t = 0, us = gr->cfg.bus->poll_us ? gr->cfg.bus->poll_us
						   : PG106_HW_POLL_US_DEFAULT;

	for (;;) {
		uint32_t stat = rd(gr, 0x409800);

		gr->fecs_stat = stat;
		if (stat & err)
			return -EIO;
		if (stat & ok)
			return 0;
		if (t >= POLL_2S)
			return -ETIMEDOUT;
		delay(gr, us);
		t += us;
	}
}

// gf100_gr_fecs_bind_pointer (gf100.c:833-850).
static int
fecs_bind_pointer(struct pg106_gr *gr, uint32_t inst)
{
	mask(gr, 0x409800, 0x00000030, 0x00000000);
	wr(gr, 0x409500, inst);
	wr(gr, 0x409504, PG106_FECS_MTHD_BIND_POINTER);
	return fecs_wait(gr, 0x10, 0x20);
}

// gf100_gr_fecs_wfi_golden_save (gf100.c:814-831).
static int
fecs_wfi_golden_save(struct pg106_gr *gr, uint32_t inst)
{
	mask(gr, 0x409800, 0x00000003, 0x00000000);
	wr(gr, 0x409500, inst);
	wr(gr, 0x409504, PG106_FECS_MTHD_WFI_GOLDEN_SAVE);
	return fecs_wait(gr, 0x1, 0x2);
}

// gf100_gr_fecs_reset (gf100.c:2467-2476).
static void
fecs_reset(struct pg106_gr *gr)
{
	wr(gr, 0x409614, 0x00000070);
	delay(gr, 10);
	mask(gr, 0x409614, 0x00000700, 0x00000700);
	delay(gr, 10);
	(void)rd(gr, 0x409614);
}

int
pg106_gr_wait_idle(struct pg106_gr *gr)
{
	uint32_t t = 0, us = gr->cfg.bus->poll_us ? gr->cfg.bus->poll_us
						   : PG106_HW_POLL_US_DEFAULT;

	gr->idle_waits++;
	// gf100_gr_wait_idle (gf100.c:1050-1076), with gk104_engn_chsw
	// (fifo/gk104.c:214-223 via nvkm_fifo_ctxsw_in_progress,
	// fifo/base.c:39-52) for ctxsw_active.
	for (;;) {
		uint32_t stat;
		bool gr_enabled, ctxsw_active, gr_busy;

		(void)rd(gr, 0x400700); // refresh FIFO_ENGINE_STATUS
		gr_enabled = (rd(gr, 0x000200) & 0x1000) != 0;
		stat = rd(gr, 0x002640 + gr->cfg.gr_engine * 8u);
		ctxsw_active = (stat & 0x80000000u) && (stat & 0x00008000u);
		gr_busy = (rd(gr, 0x40060c) & 1) != 0;
		if (!gr_enabled || (!gr_busy && !ctxsw_active))
			return 0;
		if (t >= POLL_2S) {
			gr->failed_at = "gf100_gr_wait_idle timeout";
			return -EAGAIN;
		}
		delay(gr, us);
		t += us;
		gr->idle_wait_us += us;
	}
}

// ---- firmware packs ----------------------------------------------------------------

static uint32_t
le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

// gk20a_gr_av_to_init_ (gk20a.c:36-64): {addr, data} pairs. count = 1,
// pitch = 1 for every entry (gk20a_gr_av_to_init: count 1, pitch 1).
static int
parse_av(const uint8_t *p, size_t len, struct pg106_gr_av **out, size_t *n)
{
	size_t i, nent;
	struct pg106_gr_av *av;

	if (!p || len % 8)
		return -EINVAL;
	nent = len / 8;
	av = calloc(nent ? nent : 1, sizeof(*av));
	if (!av)
		return -ENOMEM;
	for (i = 0; i < nent; i++) {
		av[i].addr = le32(p + i * 8);
		av[i].data = le32(p + i * 8 + 4);
	}
	*out = av;
	*n = nent;
	return 0;
}

// gk20a_gr_aiv_to_init (gk20a.c:72-107): {addr, index, data}; the index is
// not used by gf100_gr_mmio.
static int
parse_aiv(const uint8_t *p, size_t len, struct pg106_gr_av **out, size_t *n)
{
	size_t i, nent;
	struct pg106_gr_av *av;

	if (!p || len % 12)
		return -EINVAL;
	nent = len / 12;
	av = calloc(nent ? nent : 1, sizeof(*av));
	if (!av)
		return -ENOMEM;
	for (i = 0; i < nent; i++) {
		av[i].addr = le32(p + i * 12);
		av[i].data = le32(p + i * 12 + 8);
	}
	*out = av;
	*n = nent;
	return 0;
}

// gf100_gr_mmio (gf100.c:1079-1093) over a parsed av list.
static void
gr_mmio(struct pg106_gr *gr, const struct pg106_gr_av *av, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		wr(gr, av[i].addr, av[i].data);
}

// gf100_gr_icmd (gf100.c:1096-1133) over the bundle list.
static int
gr_icmd(struct pg106_gr *gr, const struct pg106_gr_av *av, size_t n)
{
	uint32_t data = 0;
	size_t i;
	int ret;

	wr(gr, 0x400208, 0x80000000);
	for (i = 0; i < n; i++) {
		if (i == 0 || data != av[i].data) {
			wr(gr, 0x400204, av[i].data);
			data = av[i].data;
		}
		wr(gr, 0x400200, av[i].addr);
		gr->icmds++;
		// Wait for GR to go idle after submitting a GO_IDLE bundle.
		if ((av[i].addr & 0xffff) == 0xe100)
			(void)pg106_gr_wait_idle(gr);
		ret = wait_clear(gr, 0x400700, 0x00000004);
		if (ret) {
			gr->failed_at = "icmd: 0x400700 bit 2 did not clear";
			wr(gr, 0x400208, 0x00000000);
			return ret;
		}
	}
	wr(gr, 0x400208, 0x00000000);
	return 0;
}

// gf100_gr_mthd (gf100.c:1136-1158) over gk20a_gr_av_to_method's packs
// (gk20a.c:109-153): class = addr & 0xffff, method = (addr & 0xffff0000)
// >> 14, written as 0x80000000 | class | method << 14 (= addr & ~0xffff).
// `data` is tracked across the whole list, like the function's local.
static void
gr_mthd(struct pg106_gr *gr, const struct pg106_gr_av *av, size_t n)
{
	uint32_t data = 0;
	size_t i;

	for (i = 0; i < n; i++) {
		const uint32_t cls = av[i].addr & 0xffffu;
		const uint32_t mthd = (av[i].addr & 0xffff0000u) >> 14;

		if (i == 0 || data != av[i].data) {
			wr(gr, 0x40448c, av[i].data);
			data = av[i].data;
		}
		wr(gr, 0x404488, 0x80000000u | cls | (mthd << 14));
		gr->mthds++;
	}
}

// ---- topology (gf100_gr_oneinit, gf100.c:2009-2033) ---------------------------------

static unsigned
hweight8(uint8_t v)
{
	unsigned n = 0;

	while (v) {
		n += v & 1;
		v >>= 1;
	}
	return n;
}

static int
derive_topology(struct pg106_gr *gr)
{
	const struct pg106_grboot_config *t = gr->cfg.topo;
	unsigned i, j;

	gr->gpc_nr = (uint8_t)t->gpc_nr;
	if (!gr->gpc_nr || gr->gpc_nr > PG106_GR_GPC_MAX)
		return -EINVAL;
	for (i = 0; i < gr->gpc_nr; i++) {
		gr->tpc_nr[i] = t->tpc_nr[i];
		if (gr->tpc_nr[i] > PG106_GR_FUNC_TPC_NR + 3) // bogus discovery
			return -EINVAL;
		if (gr->tpc_nr[i] > gr->tpc_max)
			gr->tpc_max = gr->tpc_nr[i];
		gr->tpc_total = (uint8_t)(gr->tpc_total + gr->tpc_nr[i]);
		for (j = 0; j < PG106_GR_FUNC_PPC_NR; j++) {
			gr->ppc_tpc_mask[i][j] = t->ppc_tpc_mask[i][j];
			if (!gr->ppc_tpc_mask[i][j])
				continue;
			gr->ppc_nr[i]++;
			gr->ppc_mask[i] |= (uint8_t)(1u << j);
			gr->ppc_tpc_nr[i][j] = (uint8_t)hweight8(gr->ppc_tpc_mask[i][j]);
			if (gr->ppc_tpc_min == 0 || gr->ppc_tpc_min > gr->ppc_tpc_nr[i][j])
				gr->ppc_tpc_min = gr->ppc_tpc_nr[i][j];
			if (gr->ppc_tpc_max < gr->ppc_tpc_nr[i][j])
				gr->ppc_tpc_max = gr->ppc_tpc_nr[i][j];
		}
		gr->ppc_total = (uint8_t)(gr->ppc_total + gr->ppc_nr[i]);
	}
	if (!gr->tpc_total)
		return -EINVAL;
	return 0;
}

// gf100_gr_oneinit_tiles (gf100.c:1914-1988).
static void
oneinit_tiles_gf100(struct pg106_gr *gr)
{
	static const uint8_t primes[] = { 3, 5, 7, 11, 13, 17, 19, 23, 29,
					  31, 37, 41, 43, 47, 53, 59, 61 };
	int init_frac[PG106_GR_GPC_MAX], init_err[PG106_GR_GPC_MAX];
	int run_err[PG106_GR_GPC_MAX], i, j;
	uint32_t mul_factor, comm_denom;
	uint8_t gpc_map[PG106_GR_GPC_MAX];
	bool sorted;

	switch (gr->tpc_total) {
	case 15: gr->screen_tile_row_offset = 0x06; break;
	case 14: gr->screen_tile_row_offset = 0x05; break;
	case 13: gr->screen_tile_row_offset = 0x02; break;
	case 11: gr->screen_tile_row_offset = 0x07; break;
	case 10: gr->screen_tile_row_offset = 0x06; break;
	case 7:
	case 5: gr->screen_tile_row_offset = 0x01; break;
	case 3: gr->screen_tile_row_offset = 0x02; break;
	case 2:
	case 1: gr->screen_tile_row_offset = 0x01; break;
	default:
		gr->screen_tile_row_offset = 0x03;
		for (i = 0; i < (int)(sizeof(primes) / sizeof(primes[0])); i++) {
			if (gr->tpc_total % primes[i]) {
				gr->screen_tile_row_offset = primes[i];
				break;
			}
		}
		break;
	}

	// Sort GPCs by TPC count, highest-to-lowest.
	for (i = 0; i < gr->gpc_nr; i++)
		gpc_map[i] = (uint8_t)i;
	sorted = false;
	while (!sorted) {
		for (sorted = true, i = 0; i < gr->gpc_nr - 1; i++) {
			if (gr->tpc_nr[gpc_map[i + 1]] > gr->tpc_nr[gpc_map[i + 0]]) {
				uint8_t swap = gpc_map[i];

				gpc_map[i + 0] = gpc_map[i + 1];
				gpc_map[i + 1] = swap;
				sorted = false;
			}
		}
	}

	// Determine tile->GPC mapping.
	mul_factor = (uint32_t)gr->gpc_nr * gr->tpc_max;
	if (mul_factor & 1)
		mul_factor = 2;
	else
		mul_factor = 1;
	comm_denom = (uint32_t)gr->gpc_nr * gr->tpc_max * mul_factor;
	for (i = 0; i < gr->gpc_nr; i++) {
		init_frac[i] = (int)(gr->tpc_nr[gpc_map[i]] * gr->gpc_nr * mul_factor);
		init_err[i] = (int)(i * gr->tpc_max * mul_factor) - (int)(comm_denom / 2);
		run_err[i] = init_frac[i] + init_err[i];
	}
	for (i = 0; i < gr->tpc_total;) {
		for (j = 0; j < gr->gpc_nr; j++) {
			if ((run_err[j] * 2) >= (int)comm_denom) {
				gr->tile[i++] = gpc_map[j];
				run_err[j] += init_frac[j] - (int)comm_denom;
			} else {
				run_err[j] += init_frac[j];
			}
		}
	}
}

// gm200_gr_oneinit_tiles (gm200.c:158-181): three hardcoded maps, else
// the gf100 algorithm.
static void
oneinit_tiles(struct pg106_gr *gr)
{
	static const uint8_t map_6_24[] = { 0, 1, 2, 3, 4, 5, 3, 4, 5, 0, 1, 2,
					    0, 1, 2, 3, 4, 5, 3, 4, 5, 0, 1, 2 };
	static const uint8_t map_4_16[] = { 0, 1, 2, 3, 2, 3, 0, 1, 3, 0, 1, 2,
					    1, 2, 3, 0 };
	static const uint8_t map_2_8[] = { 0, 1, 1, 0, 0, 1, 1, 0 };

	memset(gr->tile, 0xff, sizeof(gr->tile));
	if (gr->gpc_nr == 2 && gr->tpc_total == 8) {
		memcpy(gr->tile, map_2_8, 8);
		gr->screen_tile_row_offset = 1;
	} else if (gr->gpc_nr == 4 && gr->tpc_total == 16) {
		memcpy(gr->tile, map_4_16, 16);
		gr->screen_tile_row_offset = 4;
	} else if (gr->gpc_nr == 6 && gr->tpc_total == 24) {
		memcpy(gr->tile, map_6_24, 24);
		gr->screen_tile_row_offset = 5;
	} else {
		oneinit_tiles_gf100(gr);
	}
}

// gm200_gr_oneinit_sm_id -> gf100_gr_oneinit_sm_id (gf100.c:1896-1912).
static void
oneinit_sm_id(struct pg106_gr *gr)
{
	int tpc, gpc;

	gr->sm_nr = 0;
	for (tpc = 0; tpc < gr->tpc_max; tpc++)
		for (gpc = 0; gpc < gr->gpc_nr; gpc++)
			if (tpc < gr->tpc_nr[gpc]) {
				gr->sm[gr->sm_nr].gpc = (uint8_t)gpc;
				gr->sm[gr->sm_nr].tpc = (uint8_t)tpc;
				gr->sm_nr++;
			}
}

uint32_t
pg106_gr_attrib_cb_size(const struct pg106_gr *gr)
{
	// gp102_grctx_generate_attrib_cb_size (ctxgp102.c:236-247).
	uint32_t size = ALPHA_NR_MAX * gr->tpc_total;
	int gpc;

	for (gpc = 0; gpc < gr->gpc_nr; gpc++)
		size += GFXP_NR * PG106_GR_FUNC_PPC_NR * gr->ppc_tpc_max;
	return ((size * 0x20) + 127) & ~127u;
}

// ---- patch writes (gf100_grctx_patch_wr32, ctxgf100.c:995-1004) ----------------------

// With no mmio list (golden generation) the write goes to the register;
// with one (per-channel) it is appended as an {addr, data} pair.
struct patch {
	struct pg106_gr *gr;
	uint32_t *list;  // NULL = registers
	uint32_t nr, cap;
	bool overflow;
};

static void
patch_wr32(struct patch *p, uint32_t addr, uint32_t data)
{
	if (!p->list) {
		wr(p->gr, addr, data);
		return;
	}
	if (p->nr + 2 > p->cap) {
		p->overflow = true;
		return;
	}
	p->list[p->nr++] = addr;
	p->list[p->nr++] = data;
}

// gp100_grctx_generate_pagepool (ctxgp100.c:32-39).
static void
gen_pagepool(struct patch *p, uint64_t addr)
{
	patch_wr32(p, 0x40800c, (uint32_t)(addr >> 8));
	patch_wr32(p, 0x408010, 0x8007d800);
	patch_wr32(p, 0x419004, (uint32_t)(addr >> 8));
	patch_wr32(p, 0x419008, 0x00000000);
}

// gm107_grctx_generate_bundle (ctxgm107.c:879-891).
static void
gen_bundle(struct patch *p, uint64_t addr, uint32_t size)
{
	const uint32_t state_limit = BUNDLE_MIN_GPM_FIFO < size / 0x20
					     ? BUNDLE_MIN_GPM_FIFO
					     : size / 0x20;
	const uint32_t token_limit = BUNDLE_TOKEN_LIMIT;

	patch_wr32(p, 0x408004, (uint32_t)(addr >> 8));
	patch_wr32(p, 0x408008, 0x80000000 | (size >> 8));
	patch_wr32(p, 0x418e24, (uint32_t)(addr >> 8));
	patch_wr32(p, 0x418e28, 0x80000000 | (size >> 8));
	patch_wr32(p, 0x4064c8, (state_limit << 16) | token_limit);
}

// gp100_grctx_generate_attrib_cb (ctxgp100.c:83-90) -> gm107 (ctxgm107.c:
// 936-941) -> gf100 (ctxgf100.c:1053-1058).
static void
gen_attrib_cb(struct patch *p, uint64_t addr, uint32_t size)
{
	patch_wr32(p, 0x418810, 0x80000000 | (uint32_t)(addr >> 12));
	patch_wr32(p, 0x419848, 0x10000000 | (uint32_t)(addr >> 12));
	patch_wr32(p, 0x419c2c, 0x10000000 | (uint32_t)(addr >> 12));
	patch_wr32(p, 0x419b00, 0x00000000 | (uint32_t)(addr >> 12));
	patch_wr32(p, 0x419b04, 0x80000000 | (size >> 7));
}

// gp102_grctx_generate_attrib (ctxgp102.c:190-234).
static void
gen_attrib(struct patch *p)
{
	struct pg106_gr *gr = p->gr;
	const uint32_t alpha = ALPHA_NR, attrib = ATTRIB_NR, gfxp = GFXP_NR;
	const uint32_t max_batches = 0xffff;
	uint32_t size = ALPHA_NR_MAX * gr->tpc_total;
	uint32_t ao = 0, bo = ao + size;
	int gpc, ppc, n = 0;

	patch_wr32(p, 0x405830, attrib);
	patch_wr32(p, 0x40585c, alpha);
	patch_wr32(p, 0x4064c4, ((alpha / 4) << 16) | max_batches);
	for (gpc = 0; gpc < gr->gpc_nr; gpc++) {
		for (ppc = 0; ppc < (int)PG106_GR_FUNC_PPC_NR; ppc++, n++) {
			const uint32_t as = alpha * gr->ppc_tpc_nr[gpc][ppc];
			const uint32_t bs = attrib * gr->ppc_tpc_max;
			const uint32_t gs = gfxp * gr->ppc_tpc_max;
			const uint32_t u = 0x418ea0 + (uint32_t)n * 0x04;
			const uint32_t o = PPC_UNIT(gpc, ppc, 0);
			const uint32_t pr = GPC_UNIT(gpc, 0xc44 + (uint32_t)ppc * 4);

			if (!(gr->ppc_mask[gpc] & (1 << ppc)))
				continue;
			patch_wr32(p, o + 0xc0, gs);
			patch_wr32(p, pr, bs);
			patch_wr32(p, o + 0xf4, bo);
			patch_wr32(p, o + 0xf0, bs);
			bo += gs;
			patch_wr32(p, o + 0xe4, as);
			patch_wr32(p, o + 0xf8, ao);
			ao += ALPHA_NR_MAX * gr->ppc_tpc_nr[gpc][ppc];
			patch_wr32(p, u, bs);
		}
	}
	patch_wr32(p, 0x4181e4, 0x00000100);
	patch_wr32(p, 0x41befc, 0x00000100);
}

// The per-channel buffer registers in nouveau's order (the tail of
// gf100_gr_chan_new, gf100.c:470-479, and the same calls inside
// gf100_grctx_generate_main, ctxgf100.c:1368-1371).
static void
gen_buffers(struct patch *p, uint64_t pagepool, uint64_t bundle,
	    uint64_t attrib)
{
	gen_pagepool(p, pagepool);
	gen_bundle(p, bundle, BUNDLE_SIZE);
	gen_attrib_cb(p, attrib, pg106_gr_attrib_cb_size(p->gr));
	gen_attrib(p);
}

// ---- floorsweeping (gf100_grctx_generate_floorsweep, ctxgf100.c:1303-1340) ------------

// gf100_gr_init_num_tpc_per_gpc (gf100.c:2170-2184).
static void
init_num_tpc_per_gpc(struct pg106_gr *gr, bool pd, bool ds)
{
	int gpc, i, j;
	uint32_t data;

	for (gpc = 0, i = 0; i < 4; i++) {
		for (data = 0, j = 0; j < 8 && gpc < gr->gpc_nr; j++, gpc++)
			data |= (uint32_t)gr->tpc_nr[gpc] << (j * 4);
		if (pd)
			wr(gr, 0x406028 + (uint32_t)i * 4, data);
		if (ds)
			wr(gr, 0x405870 + (uint32_t)i * 4, data);
	}
}

// gf117_grctx_generate_rop_mapping (ctxgf117.c:198-240).
static void
rop_mapping(struct pg106_gr *gr)
{
	uint32_t data[6] = { 0 }, data2[2] = { 0 };
	uint8_t shift, ntpcv;
	int i;

	// Pack tile map into register format.
	for (i = 0; i < 32; i++)
		data[i / 6] |= (uint32_t)(gr->tile[i] & 0x07) << ((i % 6) * 5);
	// Magic.
	shift = 0;
	ntpcv = gr->tpc_total;
	while (!(ntpcv & (1 << 4))) {
		ntpcv <<= 1;
		shift++;
	}
	data2[0] = (uint32_t)ntpcv << 16;
	data2[0] |= (uint32_t)shift << 21;
	data2[0] |= (((1u << (0 + 5)) % ntpcv) << 24);
	for (i = 1; i < 7; i++)
		data2[1] |= ((1u << (i + 5)) % ntpcv) << ((i - 1) * 5);
	// GPC_BROADCAST
	wr(gr, 0x418bb8, ((uint32_t)gr->tpc_total << 8) | gr->screen_tile_row_offset);
	for (i = 0; i < 6; i++)
		wr(gr, 0x418b08 + (uint32_t)i * 4, data[i]);
	// GPC_BROADCAST.TP_BROADCAST
	wr(gr, 0x41bfd0, ((uint32_t)gr->tpc_total << 8) | gr->screen_tile_row_offset |
				 data2[0]);
	wr(gr, 0x41bfe4, data2[1]);
	for (i = 0; i < 6; i++)
		wr(gr, 0x41bf00 + (uint32_t)i * 4, data[i]);
	// UNK78xx
	wr(gr, 0x4078bc, ((uint32_t)gr->tpc_total << 8) | gr->screen_tile_row_offset);
	for (i = 0; i < 6; i++)
		wr(gr, 0x40780c + (uint32_t)i * 4, data[i]);
}

// gm200_grctx_generate_dist_skip_table (ctxgm200.c:408-428).
static void
dist_skip_table(struct pg106_gr *gr)
{
	uint8_t data[32] = { 0 };
	int gpc, ppc, i;

	for (gpc = 0; gpc < gr->gpc_nr; gpc++) {
		for (ppc = 0; ppc < (int)PG106_GR_FUNC_PPC_NR; ppc++) {
			uint8_t ppc_tpcs = gr->ppc_tpc_nr[gpc][ppc];
			uint8_t ppc_tpcm = gr->ppc_tpc_mask[gpc][ppc];

			while (ppc_tpcs-- > gr->ppc_tpc_min)
				ppc_tpcm &= (uint8_t)(ppc_tpcm - 1);
			ppc_tpcm ^= gr->ppc_tpc_mask[gpc][ppc];
			data[gpc] |= ppc_tpcm;
		}
	}
	for (i = 0; i < 8; i++)
		wr(gr, 0x4064d0 + (uint32_t)i * 4,
		   (uint32_t)data[i * 4] | (uint32_t)data[i * 4 + 1] << 8 |
			   (uint32_t)data[i * 4 + 2] << 16 |
			   (uint32_t)data[i * 4 + 3] << 24);
}

// gp100_grctx_generate_smid_config (ctxgp100.c:105-124).
static void
smid_config(struct pg106_gr *gr)
{
	const uint32_t dist_nr = ((uint32_t)gr->tpc_total + 3) / 4;
	uint32_t dist[PG106_GR_TPC_MAX / 4] = { 0 }, gpcs[16] = { 0 };
	unsigned sm, i;

	for (sm = 0; sm < gr->sm_nr; sm++) {
		const uint8_t gpc = gr->sm[sm].gpc;
		const uint8_t tpc = gr->sm[sm].tpc;

		dist[sm / 4] |= (((uint32_t)gpc << 4) | tpc) << ((sm % 4) * 8);
		gpcs[gpc + (PG106_GR_FUNC_GPC_NR * (tpc / 4))] |= sm << ((tpc % 4) * 8);
	}
	for (i = 0; i < dist_nr; i++)
		wr(gr, 0x405b60 + i * 4, dist[i]);
	for (i = 0; i < 16; i++)
		wr(gr, 0x405ba0 + i * 4, gpcs[i]);
}

static void
floorsweep(struct pg106_gr *gr)
{
	uint32_t tmp;
	unsigned sm, i;

	for (sm = 0; sm < gr->sm_nr; sm++) {
		const int gpc = gr->sm[sm].gpc, tpc = gr->sm[sm].tpc;

		// gm107_grctx_generate_sm_id (ctxgm107.c:950-957); gp104_grctx
		// has no .tpc_nr hook.
		wr(gr, TPC_UNIT(gpc, tpc, 0x698), sm);
		wr(gr, GPC_UNIT(gpc, 0x0c10 + (uint32_t)tpc * 4), sm);
		wr(gr, TPC_UNIT(gpc, tpc, 0x088), sm);
	}
	init_num_tpc_per_gpc(gr, false, true);
	init_num_tpc_per_gpc(gr, true, false); // !skip_pd_num_tpc_per_gpc
	// .r4060a8 NULL
	rop_mapping(gr);               // .rop_mapping = gf117
	// .alpha_beta_tables, .max_ways_evict NULL
	dist_skip_table(gr);           // .dist_skip_table = gm200
	wr(gr, 0x406500, 0x00000000);  // gm200_grctx_generate_r406500 (ctxgm200.c:402-406)
	// gk104_grctx_generate_gpc_tpc_nr (ctxgk104.c:915-919).
	wr(gr, 0x405b00, ((uint32_t)gr->tpc_total << 8) | gr->gpc_nr);
	// .r419f78 NULL
	// gm200_grctx_generate_tpc_mask (ctxgm200.c:393-400), func->tpc_nr = 5.
	for (tmp = 0, i = 0; i < gr->gpc_nr; i++)
		tmp |= ((1u << gr->tpc_nr[i]) - 1) << (i * PG106_GR_FUNC_TPC_NR);
	wr(gr, 0x4041c4, tmp);
	smid_config(gr);               // .smid_config = gp100
}

// ---- gf100_grctx_generate_main (ctxgf100.c:1342-1433) for gp104_grctx -------------------

static int
generate_main(struct pg106_gr *gr, uint64_t pagepool_va, uint64_t bundle_va,
	      uint64_t attrib_va)
{
	struct patch p = { gr, NULL, 0, 0, false };
	uint32_t idle_timeout;
	int ret;

	// nvkm_mc_unk260(device, 0): a no-op on GP10x (gp100_mc has no
	// .unk260, mc/gp100.c; only gf100_mc writes 0x260, mc/gf100.c:61-75).
	gr_mmio(gr, gr->sw_ctx, gr->sw_ctx_n); // gr->sw_ctx (gf100_gr_mmio)
	// gp104: no init_419bd8, no r419ea8.
	if ((ret = pg106_gr_wait_idle(gr)))
		return ret;
	idle_timeout = mask(gr, 0x404154, 0xffffffff, 0x00000000);
	gen_buffers(&p, pagepool_va, bundle_va, attrib_va);
	// .patch_ltc NULL, .unknown_size 0.
	// gk104_grctx_generate_unkn (ctxgk104.c:894-903).
	mask(gr, 0x418c6c, 0x00000001, 0x00000001);
	mask(gr, 0x41980c, 0x00000010, 0x00000010);
	mask(gr, 0x41be08, 0x00000004, 0x00000004);
	mask(gr, 0x4064c0, 0x80000000, 0x80000000);
	mask(gr, 0x405800, 0x08000000, 0x08000000);
	mask(gr, 0x419c00, 0x00000008, 0x00000008);
	floorsweep(gr);
	if ((ret = pg106_gr_wait_idle(gr)))
		return ret;
	// .r400088 NULL. gr->bundle (sw_bundle_init.bin) replaces
	// grctx->icmd; gr->bundle_veid / bundle64 are NULL on GP10x and
	// gp104_grctx has no sw_veid_bundle_init / sw_bundle64_init.
	if ((ret = gr_icmd(gr, gr->bundle, gr->bundle_n)))
		return ret;
	// gf100_gr_icmd(gr, grctx->sw_veid_bundle_init) with a NULL list still
	// brackets nothing with 0x400208 (ctxgf100.c:1393-1396; the owner's
	// trace shows the pair).
	if ((ret = gr_icmd(gr, NULL, 0)))
		return ret;
	wr(gr, 0x404154, idle_timeout);
	gr_mthd(gr, gr->method, gr->method_n); // gr->method (sw_method_init.bin)
	// nvkm_mc_unk260(device, 1): no-op on GP10x (see above).
	// .r419cb8 .r418800 .r419eb0 .r419e00 .r418e94 NULL;
	// gm200_grctx_generate_r419a3c (ctxgm200.c:356-361); gp104_grctx has
	// no .r408840 (only gp102_grctx) and no .r419c0c.
	mask(gr, 0x419a3c, 0x00000014, 0x00000000);
	return pg106_gr_wait_idle(gr);
}

// ---- engine init ----------------------------------------------------------------------

int
pg106_gr_init(struct pg106_gr *gr, const struct pg106_gr_cfg *cfg)
{
	int ret;

	if (!gr || !cfg || !cfg->bus || !cfg->vram || !cfg->topo || !cfg->fw ||
	    !cfg->ctx_size || cfg->ctx_size > (64u << 20) || (cfg->ctx_size & 3))
		return -EINVAL;
	memset(gr, 0, sizeof(*gr));
	gr->cfg = *cfg;
	gr->size = cfg->ctx_size;
	ret = derive_topology(gr);
	if (ret)
		return ret;
	oneinit_tiles(gr);
	oneinit_sm_id(gr);
	// gk20a_gr_load_sw.
	if ((ret = parse_aiv(cfg->fw->sw_ctx, cfg->fw->sw_ctx_len, &gr->sw_ctx,
			     &gr->sw_ctx_n)) ||
	    (ret = parse_av(cfg->fw->sw_bundle_init, cfg->fw->sw_bundle_init_len,
			    &gr->bundle, &gr->bundle_n)) ||
	    (ret = parse_av(cfg->fw->sw_method_init, cfg->fw->sw_method_init_len,
			    &gr->method, &gr->method_n)))
		goto fail;
	if (!gr->sw_ctx_n || !gr->bundle_n || !gr->method_n) {
		ret = -ENOENT;
		goto fail;
	}
	// Global context buffers (gf100_gr_oneinit, gf100.c:2036-2049):
	// NVKM_MEM_TARGET_INST_SR_LOST = VRAM, re-created after a power cycle.
	gr->pagepool_size = PAGEPOOL_SIZE;
	gr->bundle_size = BUNDLE_SIZE;
	gr->attrib_size = pg106_gr_attrib_cb_size(gr);
	if ((ret = pg106_vram_alloc(cfg->vram, gr->pagepool_size, 0x1000,
				    PG106_VRAM_TAIL, &gr->pagepool_pa)) ||
	    (ret = pg106_vram_alloc(cfg->vram, gr->bundle_size, 0x1000,
				    PG106_VRAM_TAIL, &gr->bundle_pa)) ||
	    (ret = pg106_vram_alloc(cfg->vram, gr->attrib_size, 0x1000,
				    PG106_VRAM_TAIL, &gr->attrib_pa)))
		goto fail;
	if (cfg->golden) {
		gr->golden = calloc(1, gr->size); // the kext klibc has calloc, not malloc
		if (!gr->golden) {
			ret = -ENOMEM;
			goto fail;
		}
		memcpy(gr->golden, cfg->golden, gr->size);
	}
	gr->up = true;
	return 0;
fail:
	pg106_gr_fini(gr);
	return ret;
}

void
pg106_gr_fini(struct pg106_gr *gr)
{
	if (!gr || !gr->cfg.vram)
		return;
	if (gr->pagepool_pa)
		pg106_vram_free(gr->cfg.vram, gr->pagepool_pa, gr->pagepool_size);
	if (gr->bundle_pa)
		pg106_vram_free(gr->cfg.vram, gr->bundle_pa, gr->bundle_size);
	if (gr->attrib_pa)
		pg106_vram_free(gr->cfg.vram, gr->attrib_pa, gr->attrib_size);
	free(gr->sw_ctx);
	free(gr->bundle);
	free(gr->method);
	free(gr->golden);
	memset(gr, 0, sizeof(*gr));
}

// ---- golden context (gf100_grctx_generate, ctxgf100.c:1436-1557) ----------------------

// The private VMM the generation runs in: nouveau uses the first channel's
// VMM and instance block; here it is a VMM of our own joined into a bare
// instance block, which is all FECS reads (BIND_POINTER takes the block
// address, the context pointer is at 0x210, the PDB at 0x200).
struct gen_vm {
	struct pg106_vmm vmm;
	uint64_t next_va;
	bool up;
};

static int
gen_map(void *user, uint64_t pa, uint64_t size, bool priv, uint64_t *va)
{
	struct gen_vm *g = user;
	struct pg106_map m;
	int ret;

	memset(&m, 0, sizeof(m));
	m.va = g->next_va;
	m.pa = pa;
	m.size = (size + 0xfff) & ~0xfffull;
	m.page_shift = PG106_PAGE_4K_SHIFT;
	m.target = PG106_TARGET_VRAM;
	m.kind = PG106_KIND_PITCH;
	m.priv = priv;
	ret = pg106_vmm_map(&g->vmm, &m);
	if (ret)
		return ret;
	*va = m.va;
	g->next_va += (m.size + 0xffff) & ~0xffffull;
	return 0;
}

int
pg106_gr_golden(struct pg106_gr *gr)
{
	struct gen_vm g;
	uint64_t inst = 0, data = 0, pagepool_va, bundle_va, attrib_va, ctx_va;
	const uint32_t data_size = PG106_GR_CB_RESERVED + gr->size;
	int ret;

	if (!gr || !gr->up)
		return -EINVAL;
	if (gr->golden)
		return 0;
	gr->failed_at = NULL;
	memset(&g, 0, sizeof(g));
	g.next_va = 0x10000; // VA 0 stays unmapped

	// NV_PGRAPH_FE_PWR_MODE_FORCE_ON.
	wr(gr, 0x404170, 0x00000012);
	(void)wait_clear(gr, 0x404170, 0x00000010);
	// .unkn88c NULL. Reset FECS.
	fecs_reset(gr);
	// NV_PGRAPH_FE_PWR_MODE_AUTO.
	wr(gr, 0x404170, 0x00000010);
	(void)wait_clear(gr, 0x404170, 0x00000010);
	// Init SCC RAM.
	wr(gr, 0x40802c, 0x00000001);

	// A VMM with the global buffers (gf100_gr_chan_new maps them
	// privileged, gf100.c:384-421) and the scratch context.
	ret = pg106_vmm_init(&g.vmm, gr->cfg.vram, PG106_MMU_VA_LIMIT, 0, false);
	if (ret) {
		gr->failed_at = "golden: VMM";
		return ret;
	}
	g.up = true;
	ret = pg106_vram_alloc(gr->cfg.vram, PG106_INST_SIZE, 0x1000,
			       PG106_VRAM_TAIL | PG106_VRAM_ZERO, &inst);
	if (!ret)
		ret = pg106_vmm_join(&g.vmm, inst);
	if (!ret)
		ret = gen_map(&g, gr->pagepool_pa, gr->pagepool_size, true, &pagepool_va);
	if (!ret)
		ret = gen_map(&g, gr->bundle_pa, gr->bundle_size, true, &bundle_va);
	if (!ret)
		ret = gen_map(&g, gr->attrib_pa, gr->attrib_size, true, &attrib_va);
	// Allocate memory to store context, and dummy global context
	// buffers (nvkm_memory_new INST, zeroed).
	if (!ret)
		ret = pg106_vram_alloc(gr->cfg.vram, data_size, 0x1000,
				       PG106_VRAM_TAIL | PG106_VRAM_ZERO, &data);
	if (!ret)
		ret = gen_map(&g, data, data_size, false, &ctx_va);
	if (!ret)
		ret = pg106_vmm_commit(&g.vmm);
	if (ret) {
		gr->failed_at = "golden: memory";
		goto out;
	}
	// Setup context pointer.
	pg106_vram_wr32(gr->cfg.vram, inst + PG106_GR_INST_ECTX,
			(uint32_t)(ctx_va + PG106_GR_CB_RESERVED) | 4u);
	pg106_vram_wr32(gr->cfg.vram, inst + PG106_GR_INST_ECTX + 4,
			(uint32_t)((ctx_va + PG106_GR_CB_RESERVED) >> 32));
	pg106_bar_flush(gr->cfg.bus);
	// Make channel current (gr->firmware path).
	ret = fecs_bind_pointer(gr, 0x80000000u | (uint32_t)(inst >> 12));
	if (ret) {
		gr->failed_at = "golden: FECS BIND_POINTER";
		goto out_inst;
	}
	pg106_vram_wr32(gr->cfg.vram, data + 0x1c, 1);
	pg106_vram_wr32(gr->cfg.vram, data + 0x20, 0);
	pg106_vram_wr32(gr->cfg.vram, data + 0x28, 0);
	pg106_vram_wr32(gr->cfg.vram, data + 0x2c, 0);

	ret = generate_main(gr, pagepool_va, bundle_va, attrib_va);
	if (ret) {
		if (!gr->failed_at)
			gr->failed_at = "golden: generate_main";
		goto out_inst;
	}
	ret = fecs_wfi_golden_save(gr, 0x80000000u | (uint32_t)(inst >> 12));
	if (ret) {
		gr->failed_at = "golden: FECS WFI_GOLDEN_SAVE";
		goto out_inst;
	}
	mask(gr, 0x409b00, 0x80000000, 0x00000000);

	gr->golden = calloc(1, gr->size); // the kext klibc has calloc, not malloc
	if (!gr->golden) {
		ret = -ENOMEM;
		goto out_inst;
	}
	ret = pg106_vram_read(gr->cfg.vram, data + PG106_GR_CB_RESERVED, gr->golden,
			      gr->size);
	if (ret) {
		free(gr->golden);
		gr->golden = NULL;
		gr->failed_at = "golden: readback";
	}
out_inst:
	pg106_vram_wr32(gr->cfg.vram, inst + PG106_GR_INST_ECTX, 0);
	pg106_vram_wr32(gr->cfg.vram, inst + PG106_GR_INST_ECTX + 4, 0);
out:
	if (data)
		pg106_vram_free(gr->cfg.vram, data, data_size);
	if (inst)
		pg106_vram_free(gr->cfg.vram, inst, PG106_INST_SIZE);
	if (g.up)
		pg106_vmm_fini(&g.vmm);
	return ret;
}

// ---- per-channel context ----------------------------------------------------------------

int
pg106_grctx_chan_create(struct pg106_gr *gr, const struct pg106_grctx_map *m,
			struct pg106_grctx_chan *c)
{
	uint32_t list[PG106_GR_MMIO_SIZE / 4];
	struct patch p = { gr, list, 0, PG106_GR_MMIO_SIZE / 4, false };
	uint32_t hdr[0x30 / 4];
	int ret;

	if (!gr || !gr->up || !gr->golden || !m || !m->map || !m->unmap || !c)
		return -EINVAL;
	memset(c, 0, sizeof(*c));
	// Map pagepool, bundle and attribute circular buffers (privileged:
	// gf100_vmm_map_v0 .priv = 1, gf100.c:380,384-421).
	if ((ret = m->map(m->user, gr->pagepool_pa, gr->pagepool_size, true,
			  &c->pagepool_va)) ||
	    (ret = m->map(m->user, gr->bundle_pa, gr->bundle_size, true,
			  &c->bundle_va)) ||
	    (ret = m->map(m->user, gr->attrib_pa, gr->attrib_size, true,
			  &c->attrib_va)))
		goto fail;
	// The mmio list (gf100.c:446-467): 0x1000 of INST memory, mapped
	// privileged; filled with the buffer registers for this channel.
	ret = pg106_vram_alloc(gr->cfg.vram, PG106_GR_MMIO_SIZE, 0x1000,
			       PG106_VRAM_TAIL | PG106_VRAM_ZERO, &c->mmio_pa);
	if (ret)
		goto fail;
	if ((ret = m->map(m->user, c->mmio_pa, PG106_GR_MMIO_SIZE, true, &c->mmio_va)))
		goto fail;
	gen_buffers(&p, c->pagepool_va, c->bundle_va, c->attrib_va);
	if (p.overflow) {
		ret = -ENOSPC;
		goto fail;
	}
	c->mmio_nr = p.nr;
	ret = pg106_vram_write(gr->cfg.vram, c->mmio_pa, list, p.nr * 4);
	if (ret)
		goto fail;
	// gf100_gr_chan_bind (gf100.c:320-353): the golden image, then the
	// firmware header words.
	c->ctx_alloc = (gr->size + 0xfff) & ~0xfffu;
	ret = pg106_vram_alloc(gr->cfg.vram, c->ctx_alloc, 0x1000, PG106_VRAM_TAIL,
			       &c->ctx_pa);
	if (ret)
		goto fail;
	ret = pg106_vram_write(gr->cfg.vram, c->ctx_pa, gr->golden, gr->size);
	if (ret)
		goto fail;
	memcpy(hdr, gr->golden, sizeof(hdr));
	hdr[0x10 / 4] = c->mmio_nr / 2;
	hdr[0x14 / 4] = (uint32_t)c->mmio_va;
	hdr[0x18 / 4] = (uint32_t)(c->mmio_va >> 32);
	hdr[0x1c / 4] = 1;
	hdr[0x20 / 4] = 0;
	hdr[0x28 / 4] = 0;
	hdr[0x2c / 4] = 0;
	ret = pg106_vram_write(gr->cfg.vram, c->ctx_pa, hdr, sizeof(hdr));
	if (!ret)
		ret = pg106_vram_wr32(gr->cfg.vram, c->ctx_pa + 0xf4, 0);
	if (!ret)
		ret = pg106_vram_wr32(gr->cfg.vram, c->ctx_pa + 0xf8, 0);
	if (ret)
		goto fail;
	// gk104_ectx_ctor (fifo/gk104.c:178-186): the image mapped privileged.
	if ((ret = m->map(m->user, c->ctx_pa, c->ctx_alloc, true, &c->ctx_va)))
		goto fail;
	c->up = true;
	return 0;
fail:
	pg106_grctx_chan_destroy(gr, m, c);
	return ret;
}

void
pg106_grctx_chan_destroy(struct pg106_gr *gr, const struct pg106_grctx_map *m,
			 struct pg106_grctx_chan *c)
{
	if (!gr || !m || !c)
		return;
	if (c->ctx_va)
		m->unmap(m->user, c->ctx_va, c->ctx_alloc);
	if (c->mmio_va)
		m->unmap(m->user, c->mmio_va, PG106_GR_MMIO_SIZE);
	if (c->attrib_va)
		m->unmap(m->user, c->attrib_va, gr->attrib_size);
	if (c->bundle_va)
		m->unmap(m->user, c->bundle_va, gr->bundle_size);
	if (c->pagepool_va)
		m->unmap(m->user, c->pagepool_va, gr->pagepool_size);
	if (c->ctx_pa)
		pg106_vram_free(gr->cfg.vram, c->ctx_pa, c->ctx_alloc);
	if (c->mmio_pa)
		pg106_vram_free(gr->cfg.vram, c->mmio_pa, PG106_GR_MMIO_SIZE);
	memset(c, 0, sizeof(*c));
}

uint64_t
pg106_grctx_chan_ectx(const struct pg106_grctx_chan *c)
{
	return c && c->up ? (c->ctx_va | PG106_GR_ECTX_FLAG) : 0;
}

// ---- self-test ----------------------------------------------------------------------

#define ST_VA	  0x300000000ull
#define ST_SIZE	  0x100000ull
#define ST_RING	  0x00000u // 512 entries
#define ST_PUSH	  0x10000u
#define ST_SEM	  0x20000u // +0: QMD release, +0x10: host, +0x20: store sem
#define ST_QMD	  0x30000u // k0 at +0, k1 at +0x100
#define ST_CBUF	  0x31000u
#define ST_CODE	  0x40000u // SET_PROGRAM_REGION; k0 at +0x1000, k1 at +0x2000
#define ST_STORE  0x50000u
#define ST_SUBCH  1u
#define ST_ENTRIES 512u

// SM 6.x (sm_61) kernels, word for word what NAK (Mesa's compiler, the
// one the NVK dylib carries) encodes for them — a NAK test built in the
// nvk/pascal NAK-on-nouveau tree ran ShaderModelInfo::new(61, 64)'s
// encoder on the same IR (nvdisasm in CUDA 13 no longer accepts SM61).
// Pascal packs three 64-bit instructions behind one control word
// (stall / yield / barrier fields per slot); NAK's scheduler sets them.
const uint64_t pg106_gr_sm61_exit[PG106_GR_SM61_EXIT_WORDS] = { // exit
	0x001f8000fc4007edull, // ctrl: exit stall 13, nop stall 2
	0xe30000000007000full, // EXIT
	0x50b0000000070f00ull, // NOP
	0x50b0000000070f00ull, // NOP
};
const uint64_t pg106_gr_sm61_store[PG106_GR_SM61_STORE_WORDS] = { // *(u32 *)c[0][0x140..0x147] = STORE_MAGIC; exit
	0x001f9800fc2007e1ull, // ctrl
	0x4c98078005070000ull, // MOV R0, c[0x0][0x140]
	0x4c98078005170001ull, // MOV R1, c[0x0][0x144]
	0x0106a5ec0de7f002ull, // MOV32I R2, 0x6a5ec0de (PG106_GR_SELFTEST_STORE)
	0x001f8801fda000e2ull, // ctrl: st rd barrier 0, exit waits on it
	0xeedce00000070002ull, // STG.E.STRONG.SYS [R0], R2
	0xe30000000007000full, // EXIT
	0x50b0000000070f00ull, // NOP
};
// NAK's gather_info reports num_gprs 24 for both (the QMD REGISTER_COUNT).
#define ST_REGISTER_COUNT PG106_GR_SM61_REGS
#define st_k0 pg106_gr_sm61_exit
#define st_k1 pg106_gr_sm61_store

struct st_vm {
	struct pg106_vmm *vmm;
	uint64_t next_va;
};

static int
st_map(void *user, uint64_t pa, uint64_t size, bool priv, uint64_t *va)
{
	struct st_vm *v = user;
	struct pg106_map m;
	int ret;

	memset(&m, 0, sizeof(m));
	m.va = v->next_va;
	m.pa = pa;
	m.size = (size + 0xfff) & ~0xfffull;
	m.page_shift = PG106_PAGE_4K_SHIFT;
	m.target = PG106_TARGET_VRAM;
	m.kind = PG106_KIND_PITCH;
	m.priv = priv;
	ret = pg106_vmm_map(v->vmm, &m);
	if (ret)
		return ret;
	*va = m.va;
	v->next_va += (m.size + 0xffff) & ~0xffffull;
	return 0;
}

static void
st_unmap(void *user, uint64_t va, uint64_t size)
{
	struct st_vm *v = user;

	pg106_vmm_unmap(v->vmm, va, (size + 0xfff) & ~0xfffull, PG106_PAGE_4K_SHIFT);
}

static int
st_poll(struct pg106_gpu *g, uint64_t pa, uint32_t want, uint32_t budget,
	uint32_t *got, uint64_t *waited)
{
	const struct pg106_hw_bus *b = g->cfg.bus;
	uint64_t t = 0;

	for (;;) {
		*got = pg106_vram_rd32(&g->vram, pa);
		if (*got == want) {
			*waited = t;
			return 0;
		}
		if (t >= budget) {
			*waited = t;
			return -ETIMEDOUT;
		}
		b->delay_us(b->user, 10);
		t += 10;
	}
}

static uint32_t
st_submit(struct pg106_gpu *g, struct pg106_chan *c, uint64_t buf, uint32_t put,
	  uint32_t push_off, const uint32_t *w, size_t n)
{
	uint64_t e = pg106_gpfifo_pack(ST_VA + push_off, (uint32_t)n,
				       PG106_GP_LEVEL_MAIN, 0, 0);

	pg106_vram_write(&g->vram, buf + push_off, w, (uint32_t)(n * 4));
	pg106_vram_wr64(&g->vram, buf + ST_RING + put * 8ull, e);
	pg106_bar_flush(g->cfg.bus);
	put = (put + 1) & (ST_ENTRIES - 1);
	pg106_chan_kick(c, put);
	return put;
}

// INVALIDATE_SKED_CACHES (clc1c0.h:348, 0x0298) after the compute state:
// NVK's nvk_cmd_buffer_begin_compute (nvk_cmd_dispatch.c:106, cls >=
// MAXWELL_COMPUTE_B) and NAK's hw_runner PushBuilder::new both send it
// before their first PCAS on this GPU.
static void
st_sked_invalidate(pg106_pb *pb)
{
	uint32_t z = 0;

	pg106_pb_inc(pb, ST_SUBCH, 0x0298, &z, 1);
}

// One dispatch: QMD at qmd_off (program at code_off, RELEASE0 of sem_val
// to ST_SEM + sem_off), preceded on the first call by the compute state.
static int
st_dispatch(pg106_pb *pb, uint64_t buf, struct pg106_gpu *g, bool first,
	    uint32_t qmd_off, uint32_t code_off, uint32_t sem_off, uint32_t sem_val,
	    bool cbuf)
{
	pg106_qmd_params qp;
	pg106_qmd qmd;
	uint32_t a, b = 3;

	memset(&qp, 0, sizeof(qp));
	qp.program_offset = code_off;
	qp.raster_width = qp.raster_height = qp.raster_depth = 1;
	qp.thread_dim[0] = qp.thread_dim[1] = qp.thread_dim[2] = 1;
	qp.register_count = ST_REGISTER_COUNT;
	// No local memory at all: SLM 0 and CRS 0, what NAK's crs_size()
	// gives these kernels (sm50.rs:82, max_crs_depth <= 16) and what
	// NAK's hw_runner dispatched on this GPU (nak-hw.log, run3) with no
	// SET_SHADER_LOCAL_MEMORY_* on the channel. grctx-run5 asked for
	// gallium's 0x800 (nve4_compute.c:661, backed there by the screen's
	// TLS area, which the selftest never sets up): SKED rejected the
	// launch with 0x407020 = 0x1000 TOTAL_TEMP_SIZE (gk104_sked_error).
	qp.crs_size = 0;
	qp.release0_addr = ST_VA + ST_SEM + sem_off;
	qp.release0_payload = sem_val;
	if (cbuf) {
		qp.cbuf[0].addr = ST_VA + ST_CBUF;
		qp.cbuf[0].size = 0x200;
	}
	if (pg106_qmd_build(&qp, &qmd))
		return -EINVAL;
	if (pg106_vram_write(&g->vram, buf + ST_QMD + qmd_off, qmd.w, sizeof(qmd.w)))
		return -EIO;
	if (first) {
		pg106_sub_compute_state st;

		memset(&st, 0, sizeof(st));
		st.program_region = ST_VA + ST_CODE;
		st.local_memory_window = PG106_SUB_NVK_LOCAL_MEMORY_WINDOW;
		st.shared_memory_window = PG106_SUB_NVK_SHARED_MEMORY_WINDOW;
		if (pg106_sub_compute_init(pb, ST_SUBCH, &st))
			return -EINVAL;
		st_sked_invalidate(pb);
	}
	a = (uint32_t)((ST_VA + ST_QMD + qmd_off) >> 8);
	pg106_pb_inc(pb, ST_SUBCH, PG106_NVC1C0_SEND_PCAS_A, &a, 1);
	pg106_pb_inc(pb, ST_SUBCH, 0x02bc, &b, 1); // SEND_SIGNALING_PCAS_B
	return pb->error ? -EINVAL : 0;
}

// What gf100_gr_intr + gf100_gr_trap_intr would read (gr/gf100.c:1607-1700,
// 1456-1560; gf100_gr_trap_gpc/_tpc/_mp 1372-1455). Nothing is acknowledged.
void
pg106_gr_read_trap(struct pg106_gpu *g, const struct pg106_gr *gr, struct pg106_gr_trap *t)
{
	const struct pg106_hw_bus *b = g->cfg.bus;
	unsigned gp, tp;

#define R(a) b->rd32(b->user, (a))
	memset(t, 0, sizeof(*t));
	t->intr = R(0x400100);
	t->trap = R(0x400108);
	t->mthd = R(0x400704);
	t->data = R(0x400708);
	t->code = R(0x400110);
	if (((t->mthd >> 16) & 7) < 4)
		t->class0 = R(0x404200 + ((t->mthd >> 16) & 7) * 4);
	t->dispatch = R(0x404000);
	t->m2mf = R(0x404600);
	t->ccache = R(0x408030);
	t->shader = R(0x405840);
	t->unk6 = R(0x40601c);
	t->macro = R(0x404490);
	t->macro_pc = R(0x404494);
	t->macro_op = R(0x40449c);
	t->sked = R(0x407020);
	t->gpc_mask = R(0x400118);
	t->gpc_nr = gr->gpc_nr < PG106_GR_DIAG_GPCS ? gr->gpc_nr : PG106_GR_DIAG_GPCS;
	for (gp = 0; gp < t->gpc_nr; gp++) {
		t->tpc_nr[gp] = gr->tpc_nr[gp] < PG106_GR_DIAG_TPCS ? gr->tpc_nr[gp]
								     : PG106_GR_DIAG_TPCS;
		t->gpc[gp] = R(GPC_UNIT(gp, 0x2c90));
		for (tp = 0; tp < t->tpc_nr[gp]; tp++) {
			t->tpc[gp][tp] = R(TPC_UNIT(gp, tp, 0x508));
			t->mp_warp[gp][tp] = R(TPC_UNIT(gp, tp, 0x648));
			t->mp_global[gp][tp] = R(TPC_UNIT(gp, tp, 0x650));
		}
	}
	t->ctxsw_cur = R(0x409b00);
	t->ctxsw_next = R(0x409b04);
	t->fecs_mbox[0] = R(0x409800);
	t->fecs_mbox[1] = R(0x409804);
	t->fecs_mbox[2] = R(0x409810);
	t->fecs_mbox[3] = R(0x40981c);
	t->fe_status = R(0x400700);
	t->fifo_access = R(0x400500);
	t->engine_status = R(0x002640 + gr->cfg.gr_engine * 8u);
#undef R
}

// A host semaphore release (4 bytes, WFI) + NON_STALL_INTERRUPT at the
// end of a submission (clc06f.h SEMAPHOREA-D, 0x10-0x20).
static void
st_host_sem(pg106_pb *pb, uint64_t va, uint32_t payload)
{
	uint32_t hd[5] = { (uint32_t)(va >> 32) & 0xffu, (uint32_t)va, payload,
			   0x01000002u, 0 };

	pg106_pb_inc(pb, 0, 0x0010, hd, 5);
}

static int
st_wait_sem(struct pg106_gpu *g, uint64_t pa, uint32_t want, uint32_t budget,
	    uint32_t *got, uint64_t *waited)
{
	return st_poll(g, pa, want, budget, got, waited);
}

int
pg106_gpu_gr_selftest(struct pg106_gpu *g, const struct pg106_gr_fw *fw,
		      uint32_t timeout_us, struct pg106_gr_selftest *res)
{
	struct pg106_gr *gr = NULL;
	struct pg106_gr_cfg gc;
	struct pg106_vmm vmm;
	struct st_vm sv = { &vmm, ST_VA + 0x10000000ull };
	const struct pg106_grctx_map map = { &sv, st_map, st_unmap };
	struct pg106_grctx_chan gch;
	struct pg106_chan_desc d;
	struct pg106_chan *c = NULL;
	struct pg106_map m;
	uint64_t buf = 0, t0, ns0 = g ? g->nonstall_events : 0;
	uint32_t put = 0, w[256], i;
	pg106_pb pb;
	int k, ret;
	bool vmm_up = false, gch_up = false, rel0_missing = false;
	size_t n;

	if (!g || !res || !fw)
		return -EINVAL;
	memset(res, 0, sizeof(*res));
	memset(&gch, 0, sizeof(gch));
	res->runlist = -1;
	if (!pg106_gpu_ready(g)) {
		res->failed_at = "GPU not READY";
		return res->ret = -ENODEV;
	}
	k = pg106_fifo_runl_find(&g->fifo, PG106_TOP_TYPE_GR);
	if (k < 0) {
		res->failed_at = "no GR runlist in PTOP";
		return res->ret = -ENOENT;
	}
	res->runlist = g->fifo.runl[k].id;
	gr = calloc(1, sizeof(*gr));
	if (!gr) {
		res->failed_at = "memory";
		return res->ret = -ENOMEM;
	}
	// 1. The engine and the golden context.
	memset(&gc, 0, sizeof(gc));
	gc.bus = g->cfg.bus;
	gc.vram = &g->vram;
	gc.topo = &g->db.gr_cfg;
	gc.ctx_size = g->db.gr_nsizes ? g->db.gr_sizes[0] : 0;
	gc.fw = fw;
	for (n = 0; n < g->db.top.ndev; n++)
		if (g->db.top.dev[n].type == PG106_TOP_TYPE_GR && g->db.top.dev[n].engine >= 0)
			gc.gr_engine = (unsigned)g->db.top.dev[n].engine;
	ret = pg106_gr_init(gr, &gc);
	if (ret) {
		res->failed_at = "GR engine init (topology / firmware lists / buffers)";
		goto out;
	}
	res->golden_size = gr->size;
	t0 = g->cfg.bus->rd32 ? 0 : 0;
	(void)t0;
	ret = pg106_gr_golden(gr);
	res->golden_us = gr->idle_wait_us;
	res->golden_failed_at = gr->failed_at;
	res->fecs_stat = gr->fecs_stat;
	if (ret) {
		res->failed_at = "golden context";
		goto out;
	}
	// 2. The channel: 1 MiB of VRAM at ST_VA, the context objects above.
	ret = pg106_vram_alloc(&g->vram, ST_SIZE, 0x10000, PG106_VRAM_ZERO, &buf);
	if (ret) {
		res->failed_at = "VRAM for the buffers";
		goto out;
	}
	ret = pg106_vmm_init(&vmm, &g->vram, PG106_MMU_VA_LIMIT, 0x200000, false);
	if (ret) {
		res->failed_at = "channel VMM";
		goto out;
	}
	vmm_up = true;
	memset(&m, 0, sizeof(m));
	m.va = ST_VA;
	m.pa = buf;
	m.size = ST_SIZE;
	m.page_shift = PG106_PAGE_64K_SHIFT;
	m.target = PG106_TARGET_VRAM;
	m.kind = PG106_KIND_PITCH;
	ret = pg106_vmm_map(&vmm, &m);
	if (!ret)
		ret = pg106_grctx_chan_create(gr, &map, &gch);
	if (ret) {
		res->failed_at = "channel VMM map / GR channel context";
		goto out;
	}
	gch_up = true;
	res->ctx_va = gch.ctx_va;
	res->mmio_pairs = gch.mmio_nr / 2;
	ret = pg106_vmm_commit(&vmm);
	if (ret) {
		res->failed_at = "channel VMM upload / flush";
		goto out;
	}
	memset(&d, 0, sizeof(d));
	d.pdb_addr = pg106_mmu_root_addr(vmm.mmu);
	d.pdb_target = PG106_TARGET_VRAM;
	d.vm_limit = vmm.limit;
	d.gpfifo_va = ST_VA + ST_RING;
	d.entries = ST_ENTRIES;
	d.ectx = pg106_grctx_chan_ectx(&gch);
	ret = pg106_chan_create_desc(&g->fifo, k, &d, &c);
	if (ret) {
		res->failed_at = "channel create (runlist commit / bind)";
		goto out;
	}
	res->chid = c->chid;
	// Code + constant buffer.
	pg106_vram_write(&g->vram, buf + ST_CODE + 0x1000, st_k0, sizeof(st_k0));
	pg106_vram_write(&g->vram, buf + ST_CODE + 0x2000, st_k1, sizeof(st_k1));
	pg106_vram_wr64(&g->vram, buf + ST_CBUF + 0x140, ST_VA + ST_STORE);
	pg106_bar_flush(g->cfg.bus);

	// 3. Submission 1: SET_OBJECT + NVK's compute state + a host
	// semaphore. The context loads on this kick; no SM work yet.
	{
		pg106_sub_compute_state st;
		uint32_t got;
		uint64_t waited;

		res->step = 1;
		pg106_pb_init(&pb, w, sizeof(w) / 4);
		pg106_pb_set_object(&pb, ST_SUBCH, PG106_CLASS_PASCAL_COMPUTE_B);
		memset(&st, 0, sizeof(st));
		st.program_region = ST_VA + ST_CODE;
		st.local_memory_window = PG106_SUB_NVK_LOCAL_MEMORY_WINDOW;
		st.shared_memory_window = PG106_SUB_NVK_SHARED_MEMORY_WINDOW;
		if (pg106_sub_compute_init(&pb, ST_SUBCH, &st) || pb.error) {
			ret = -EINVAL;
			res->failed_at = "push encode (class bind)";
			goto out;
		}
		st_sked_invalidate(&pb);
		st_host_sem(&pb, ST_VA + ST_SEM + 0x30, PG106_GR_SELFTEST_HOST2);
		put = st_submit(g, c, buf, put, ST_PUSH, w, pb.len);
		ret = st_wait_sem(g, buf + ST_SEM + 0x30, PG106_GR_SELFTEST_HOST2, timeout_us,
				  &got, &waited);
		if (ret) {
			res->failed_at = "class bind: the context load / SET_OBJECT + compute "
					 "state never completed (host semaphore after them)";
			goto out;
		}
	}

	// 4. Submission 2: the EXIT kernel with RELEASE0, then a host
	// semaphore + NON_STALL_INTERRUPT.
	res->step = 2;
	pg106_pb_init(&pb, w, sizeof(w) / 4);
	ret = st_dispatch(&pb, buf, g, false, 0x000, 0x1000, 0x0, PG106_GR_SELFTEST_SEM,
			  false);
	if (!ret)
		st_host_sem(&pb, ST_VA + ST_SEM + 0x10, PG106_GR_SELFTEST_HOST);
	if (ret || pb.error) {
		ret = ret ? ret : -EINVAL;
		res->failed_at = "push encode";
		goto out;
	}
	put = st_submit(g, c, buf, put, ST_PUSH + 0x1000, w, pb.len);
	ret = st_poll(g, buf + ST_SEM, PG106_GR_SELFTEST_SEM, timeout_us, &res->sem_value,
		      &res->sem_wait_us);
	res->host_sem = pg106_vram_rd32(&g->vram, buf + ST_SEM + 0x10);
	for (i = 0; i < 100 && res->host_sem != PG106_GR_SELFTEST_HOST; i++) {
		g->cfg.bus->delay_us(g->cfg.bus->user, 100);
		res->host_sem = pg106_vram_rd32(&g->vram, buf + ST_SEM + 0x10);
	}
	if (ret && res->host_sem != PG106_GR_SELFTEST_HOST) {
		res->failed_at = "QMD RELEASE0 never landed (dispatch did not run)";
		goto out;
	}
	if (res->host_sem != PG106_GR_SELFTEST_HOST) {
		ret = -ETIMEDOUT;
		res->failed_at = "host semaphore after the dispatch never landed";
		goto out;
	}
	// The host semaphore releases with WFI (RELEASE_WFI_EN), so the grid
	// ran to completion; a missing RELEASE0 is then a QMD release
	// problem, not a dispatch one: run the store kernel anyway and
	// report both.
	rel0_missing = ret != 0;
	ret = 0;

	// 5. Submission 3: the store kernel.
	res->step = 3;
	pg106_pb_init(&pb, w, sizeof(w) / 4);
	ret = st_dispatch(&pb, buf, g, false, 0x100, 0x2000, 0x20, PG106_GR_SELFTEST_SEM + 1,
			  true);
	if (ret || pb.error) {
		ret = ret ? ret : -EINVAL;
		res->failed_at = "push encode (store kernel)";
		goto out;
	}
	put = st_submit(g, c, buf, put, ST_PUSH + 0x2000, w, pb.len);
	{
		uint32_t got;

		ret = st_poll(g, buf + ST_SEM + 0x20, PG106_GR_SELFTEST_SEM + 1, timeout_us,
			      &got, &res->store_wait_us);
	}
	res->store_value = pg106_vram_rd32(&g->vram, buf + ST_STORE);
	if (ret) {
		res->failed_at = "store kernel's RELEASE0 never landed";
		goto out;
	}
	if (res->store_value != PG106_GR_SELFTEST_STORE) {
		ret = -EIO;
		res->failed_at = "store kernel result (STG.E through c[0][0x140]) differs";
	}
	if (!ret && rel0_missing) {
		ret = -ETIMEDOUT;
		res->step = 2;
		res->failed_at = "QMD RELEASE0 of the EXIT kernel never landed (the grid "
				 "completed: host semaphore with WFI, store kernel OK)";
	}
	if (!ret)
		res->step = 0;
out:
	if (c)
		res->gp_get = pg106_chan_get(c);
	if (ret == -ETIMEDOUT && gr && gr->up)
		pg106_gr_read_trap(g, gr, &res->trap);
	res->gr_intr = g->cfg.bus->rd32(g->cfg.bus->user, 0x400100);
	res->fecs_intr = g->cfg.bus->rd32(g->cfg.bus->user, 0x409c18);
	res->fifo_intr = g->cfg.bus->rd32(g->cfg.bus->user, 0x002100);
	res->gr_status = g->cfg.bus->rd32(g->cfg.bus->user, 0x400700);
	if (g->intr_up && pg106_intr_pending(g->intr))
		pg106_intr_service(g->intr);
	res->fifo_intr_after = g->cfg.bus->rd32(g->cfg.bus->user, 0x002100);
	res->mmu_faults = g->fifo.mmu_faults;
	res->pbdma_errors = g->fifo.pbdma_errors;
	res->nonstall_events = g->nonstall_events;
	// Submission 2's NON_STALL_INTERRUPT raises PFIFO_INTR bit 31 (the
	// engine non-stall line, gk104_fifo_intr 0x80000000); the service
	// acknowledges it and counts one event (hardware: grctx-run6 read
	// 0x80000000 before, events 1 -> 2).
	if (!ret && (res->fifo_intr_after || res->nonstall_events != ns0 + 1)) {
		ret = -EIO;
		res->step = 2;
		res->failed_at = res->fifo_intr_after
			? "PFIFO_INTR still pending after the service"
			: "NON_STALL_INTERRUPT did not raise exactly one non-stall event";
	}
	if (c)
		pg106_chan_destroy(c);
	if (gch_up)
		pg106_grctx_chan_destroy(gr, &map, &gch);
	if (vmm_up)
		pg106_vmm_fini(&vmm);
	if (buf)
		pg106_vram_free(&g->vram, buf, ST_SIZE);
	if (gr) {
		pg106_gr_fini(gr);
		free(gr);
	}
	return res->ret = ret;
}
