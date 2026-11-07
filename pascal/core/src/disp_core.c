// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/src/disp_core.c — GP102-class display engine register model.
// SIMULATION; see disp_core.h for every upstream file:line this follows.

#include <pascal/disp_core.h>

#include <string.h>

#define POLL_LIMIT 2000u // nvkm_msec(device, 2000, ...) as 2000 polls

// --- trace + bus helpers -----------------------------------------------------

static void
rec(struct pg106_dtrace *t, uint8_t kind, uint32_t addr, uint32_t mask,
    uint32_t val, const char *name)
{
	if (!t)
		return;
	if (t->len >= t->cap || !t->rec) {
		t->overflow = true;
		t->len++;
		return;
	}
	struct pg106_dreg *r = &t->rec[t->len++];
	memset(r, 0, sizeof(*r));
	r->kind = kind;
	r->phase = t->phase;
	r->addr = addr;
	r->mask = mask;
	r->val = val;
	r->name = name;
}

static void
phase(struct pg106_dtrace *t, uint8_t ph)
{
	if (t)
		t->phase = ph;
}

static uint32_t
rd(const struct pg106_dbus *b, struct pg106_dtrace *t, uint32_t a)
{
	uint32_t v = b->rd32(b->user, a);
	rec(t, PG106_DREG_RD, a, 0, v, NULL);
	return v;
}

static void
wr(const struct pg106_dbus *b, struct pg106_dtrace *t, uint32_t a, uint32_t v)
{
	b->wr32(b->user, a, v);
	rec(t, PG106_DREG_WR, a, 0, v, NULL);
}

// nvkm_mask: read, merge, write; returns the old value.
static uint32_t
msk(const struct pg106_dbus *b, struct pg106_dtrace *t, uint32_t a,
    uint32_t m, uint32_t v)
{
	uint32_t old = b->rd32(b->user, a);
	uint32_t nv = (old & ~m) | v;
	b->wr32(b->user, a, nv);
	rec(t, PG106_DREG_MASK, a, m, nv, NULL);
	return old;
}

// nvkm_msec wait for (rd(a) & bits) == want. Returns 1 when met.
static int
poll(const struct pg106_dbus *b, struct pg106_dtrace *t, uint32_t a,
     uint32_t bits, uint32_t want)
{
	unsigned i;
	int met = 0;

	for (i = 0; i < POLL_LIMIT; i++) {
		if ((b->rd32(b->user, a) & bits) == want) {
			met = 1;
			break;
		}
	}
	rec(t, PG106_DREG_POLL, a, (uint32_t)met, met ? i + 1 : i, NULL);
	return met;
}

static unsigned
fls_u(unsigned m)
{
	unsigned n = 0;
	while (m) {
		n++;
		m >>= 1;
	}
	return n;
}

// --- engine init (gf119.c:1165-1231, base.c:130-145, nv50.c:202-228) ----------

static void
sor_power(const struct pg106_dbus *b, struct pg106_dtrace *t, int sor,
	  bool normal, bool pu)
{
	const uint32_t soff = (uint32_t)sor * 0x800;
	const uint32_t shift = normal ? 0 : 16;
	const uint32_t state = 0x80000000u | ((pu ? 1u : 0u) << shift);
	const uint32_t field = 0x80000000u | (1u << shift);

	poll(b, t, 0x61c004 + soff, 0x80000000u, 0);	// nv50_sor_power_wait
	msk(b, t, 0x61c004 + soff, field, state);
	poll(b, t, 0x61c004 + soff, 0x80000000u, 0);
	poll(b, t, 0x61c030 + soff, 0x10000000u, 0);	// nv50.c:225-227
}

int
pg106_disp_init(const struct pg106_dbus *b, const struct pg106_disp_init_cfg *c,
		struct pg106_dtrace *t)
{
	unsigned heads = c->heads_mask, sors = c->sors_mask, i;
	uint32_t tmp;

	phase(t, PG106_DPH_INIT);
	if (!heads || !sors) {
		// gf119_head_cnt / gf119_sor_cnt (gf119.c:344-349, 472-477).
		tmp = rd(b, t, 0x612004);
		if (!heads)
			heads = tmp & 0x0000000f;
		if (!sors)
			sors = (tmp & 0x0000ff00) >> 8;
	}

	for (i = 0; i < 4; i++) {			// CRTC caps
		if (!(heads & (1u << i)))
			continue;
		const uint32_t hoff = i * 0x800;
		wr(b, t, 0x6101b4 + hoff, rd(b, t, 0x616104 + hoff));
		wr(b, t, 0x6101b8 + hoff, rd(b, t, 0x616108 + hoff));
		wr(b, t, 0x6101bc + hoff, rd(b, t, 0x61610c + hoff));
	}
	// DAC caps: gp102 has no DACs (gp102.c:173-193 has no .dac).
	for (i = 0; i < fls_u(sors); i++)		// SOR caps (sor.nr)
		wr(b, t, 0x6301c4 + i * 0x800, rd(b, t, 0x61c000 + i * 0x800));

	// "steal display away from vbios" (gf119.c:1200-1209).
	if (rd(b, t, 0x6100ac) & 0x00000100) {
		wr(b, t, 0x6100ac, 0x00000100);
		msk(b, t, 0x6194e8, 0x00000001, 0x00000000);
		if (!poll(b, t, 0x6194e8, 0x00000002, 0))
			return -16; // -EBUSY
	}

	wr(b, t, 0x610010, (c->inst_addr >> 8) | 9);	// hash table/objects
	wr(b, t, 0x610090, 0x00000000);			// supervisor intrs only
	wr(b, t, 0x6100a0, 0x00000000);
	wr(b, t, 0x6100b0, 0x00000307);
	for (i = 0; i < 4; i++)				// underflow reporting off
		if (heads & (1u << i))
			msk(b, t, 0x616308 + i * 0x800, 0x00000111, 0x00000010);

	// nvkm_disp_init: every IOR to "normal: fully enabled" (base.c:139-
	// 142). This is the ONLY place nouveau calls nv50_sor_power for a
	// TMDS SOR — a modeset does not power SORs down/up.
	for (i = 0; i < 8; i++)
		if (sors & (1u << i))
			sor_power(b, t, (int)i, true, true);
	return 0;
}

// --- channels (gp102.c:33-60, 102-127; dispnv50/disp.c:133-156) ------------------

int
pg106_disp_chan_init(const struct pg106_dbus *b, unsigned ctrl, unsigned user,
		     uint32_t push, uint32_t suspend_put, bool core,
		     struct pg106_dtrace *t)
{
	(void)user;
	phase(t, PG106_DPH_CHAN);
	wr(b, t, PG106_DISP_CHAN_PUSH(ctrl), push);
	wr(b, t, PG106_DISP_CHAN_UNK498(ctrl), 0x00010000);
	wr(b, t, PG106_DISP_CHAN_UNK49C(ctrl), 0x00000001);
	msk(b, t, PG106_DISP_CHAN_CTRL(ctrl), 0x00000010, 0x00000010);
	// gp102.c:46/113: PUT reset is indexed by CTRL here.
	wr(b, t, 0x640000 + ctrl * 0x1000, suspend_put);
	wr(b, t, PG106_DISP_CHAN_CTRL(ctrl), core ? 0x01000013 : 0x00000013);
	if (!poll(b, t, PG106_DISP_CHAN_CTRL(ctrl), 0x80000000u, 0))
		return -16; // -EBUSY ("core init: %08x")
	return 0;
}

void
pg106_disp_kick(const struct pg106_dbus *b, unsigned user, uint32_t cur,
		bool vram, struct pg106_dtrace *t)
{
	phase(t, PG106_DPH_KICK);
	if (vram) {	// BAR1 writes are not coherent with the fetch
		wr(b, t, 0x070000, 0x00000001);
		poll(b, t, 0x070000, 0x00000002, 0);
	}
	wr(b, t, PG106_DISP_CHAN_PUT(user), (cur << 2) & 0x00000ffcu);
}

// --- outp / IOR -----------------------------------------------------------------

void
pg106_disp_sor_route(const struct pg106_dbus *b, uint8_t or_mask, int sublink,
		     int sor, int asy_link, struct pg106_dtrace *t)
{
	unsigned m = 0;
	uint32_t moff, sorv, link;

	phase(t, PG106_DPH_ACQUIRE);
	while (m < 8 && !(or_mask & (1u << m)))
		m++;					// __ffs(outp->info.or)
	moff = m * 0x100;
	sorv = sor >= 0 ? (uint32_t)sor + 1 : 0;
	link = sor >= 0 ? (asy_link == 2) : 0;
	if (sublink & 1) {
		msk(b, t, 0x612308 + moff, 0x0000001f, link << 4 | sorv);
		link++;
	}
	if (sublink & 2)
		msk(b, t, 0x612388 + moff, 0x0000001f, link << 4 | sorv);
}

bool
pg106_disp_hdmi_enable(const struct pg106_dbus *b, int head, int sor,
		       uint8_t max_ac_packet, uint8_t rekey, uint32_t khz,
		       bool scdc, bool scrambling, bool low_rates,
		       struct pg106_dtrace *t)
{
	const uint32_t ctrl = 0x40000000u | (uint32_t)max_ac_packet << 16 | rekey;
	const uint32_t hoff = (uint32_t)head * 0x800;
	const uint32_t hdmi = (uint32_t)head * 0x400;
	const uint32_t soff = (uint32_t)sor * 0x800;
	bool high_speed;
	uint32_t sctl = 0;

	phase(t, PG106_DPH_HDMI);
	// gk104_sor_hdmi_ctrl (gk104.c:94-103).
	msk(b, t, 0x6900c0 + hdmi, 0x00000001, 0x00000000);
	wr(b, t, 0x6900cc + hdmi, 0x00000010);
	msk(b, t, 0x6900c0 + hdmi, 0x00000001, 0x00000001);
	wr(b, t, 0x690080 + hdmi, 0x82000000);
	msk(b, t, 0x616798 + hoff, 0x401f007f, ctrl);
	// gm200_sor_hdmi_scdc (gm200.c:62-79).
	high_speed = khz > 340000;
	if (scdc && scrambling) {
		if (high_speed)
			sctl |= 0x00000002;
		if (high_speed || low_rates)
			sctl |= 0x00000001;
	}
	msk(b, t, 0x61c5bc + soff, 0x00000003, sctl);
	return high_speed;
}

void
pg106_disp_hdmi_disable(const struct pg106_dbus *b, int head,
			struct pg106_dtrace *t)
{
	const uint32_t hoff = (uint32_t)head * 0x800;
	const uint32_t hdmi = (uint32_t)head * 0x400;

	phase(t, PG106_DPH_HDMI);
	msk(b, t, 0x616798 + hoff, 0x40000000, 0x00000000);
	msk(b, t, 0x690100 + hdmi, 0x00000001, 0x00000000);
	msk(b, t, 0x6900c0 + hdmi, 0x00000001, 0x00000000);
	msk(b, t, 0x690000 + hdmi, 0x00000001, 0x00000000);
}

void
pg106_disp_sor_clock(const struct pg106_dbus *b, int sor, bool tmds, int link,
		     bool high_speed, struct pg106_dtrace *t)
{
	const uint32_t soff = (uint32_t)sor * 0x800;
	uint32_t div1 = link == 3, div2 = link == 3;

	if (tmds) {
		const uint32_t speed = high_speed ? 0x14 : 0x0a;
		msk(b, t, 0x612300 + soff, 0x007c0000, speed << 18);
		if (high_speed)
			div2 = 1;			// gf119.c:297-298
	}
	msk(b, t, 0x612300 + soff, 0x00000707, (div2 << 8) | div1);
}

// --- interrupt + supervisor ----------------------------------------------------

uint32_t
pg106_disp_intr(const struct pg106_dbus *b, struct pg106_dtrace *t)
{
	uint32_t intr, stat, pending = 0;

	phase(t, PG106_DPH_INTR);
	intr = rd(b, t, PG106_DISP_INTR);
	if (intr & 0x00100000) {			// gf119.c:1128-1141
		stat = rd(b, t, PG106_DISP_SUPER_STAT);
		if (stat & 0x00000007) {
			pending = stat & 0x00000007;
			wr(b, t, PG106_DISP_SUPER_STAT, pending);
			stat &= ~0x00000007u;
		}
		if (stat)
			wr(b, t, PG106_DISP_SUPER_STAT, stat);
	}
	return pending;
}

static void
head_state(const struct pg106_dbus *b, struct pg106_dtrace *t, unsigned head,
	   bool asy, struct pg106_disp_head_state *s)
{
	const uint32_t hoff = (asy ? 0x20000u : 0) + head * 0x300;
	uint32_t d;

	d = rd(b, t, 0x640414 + hoff);			// gf119.c:428-441
	s->vtotal = d >> 16;
	s->htotal = d & 0xffff;
	d = rd(b, t, 0x640418 + hoff);
	s->vsynce = d >> 16;
	s->hsynce = d & 0xffff;
	d = rd(b, t, 0x64041c + hoff);
	s->vblanke = d >> 16;
	s->hblanke = d & 0xffff;
	d = rd(b, t, 0x640420 + hoff);
	s->vblanks = d >> 16;
	s->hblanks = d & 0xffff;
	s->hz = rd(b, t, 0x640450 + hoff);
	d = rd(b, t, 0x640404 + hoff);
	switch ((d & 0x000003c0) >> 6) {
	case 6: s->depth = 30; break;
	case 5: s->depth = 24; break;
	default: s->depth = 18; break;		// 2 and "0: XXX default"
	}
}

static void
sor_state(const struct pg106_dbus *b, struct pg106_dtrace *t, unsigned sor,
	  bool asy, struct pg106_disp_sor_state *s)
{
	const uint32_t coff = (asy ? 0x20000u : 0) + sor * 0x20;
	uint32_t ctrl = rd(b, t, 0x640200 + coff);	// gf119.c:304-336

	s->proto_evo = (ctrl & 0x00000f00) >> 8;
	s->link = 0;
	switch (s->proto_evo) {
	case 0: s->proto = 0; s->link = 1; break;
	case 1: s->proto = 1; s->link = 1; break;
	case 2: s->proto = 1; s->link = 2; break;
	case 5: s->proto = 1; s->link = 3; break;
	case 8: s->proto = 2; s->link = 1; break;
	case 9: s->proto = 2; s->link = 2; break;
	default: s->proto = -1; break;
	}
	s->head = ctrl & 0x0000000f;
}

static int
ior_for(const struct pg106_disp_sor_state *st, unsigned sors, unsigned head)
{
	for (unsigned s = 0; s < 8; s++)
		if ((sors & (1u << s)) && (st[s].head & (1u << head)))
			return (int)s;
	return -1;
}

static void
super_2_2_dp(const struct pg106_dbus *b, struct pg106_dtrace *t, unsigned head,
	     const struct pg106_disp_head_state *h,
	     const struct pg106_disp_sor_sw *sw, struct pg106_disp_super_result *r)
{
	// nv50_disp_super_2_2_dp (nv50.c:1157-1261); gm200_sor_dp has no
	// .activesym, so bestTU = 64 and only audio_sym + watermark land.
	const uint64_t khz = h->hz / 1000;
	const uint64_t linkKBps = (uint64_t)sw->dp_bw * 27000;
	const uint64_t symbol = 100000;
	const uint32_t hoff = head * 0x800;
	uint64_t hh, vv, rate, ratio, unk;

	if (!khz || !sw->dp_nr || !linkKBps)
		return;
	hh = (uint64_t)h->hblanke + h->htotal - h->hblanks - 7;
	hh = hh * linkKBps / khz;
	hh = hh - (3 * (uint64_t)sw->dp_ef) - (12 / sw->dp_nr);
	vv = (uint64_t)h->vblanks - h->vblanke - 25;
	vv = vv * linkKBps / khz;
	vv = vv - ((36 / sw->dp_nr) + 3) - 1;
	msk(b, t, 0x616620 + hoff, 0x0000ffff, (uint32_t)(uint16_t)hh);
	msk(b, t, 0x616624 + hoff, 0x00ffffff, (uint32_t)vv & 0x00ffffff);

	rate = (khz * h->depth / 8) / sw->dp_nr;
	ratio = rate * symbol / linkKBps;
	unk = (symbol - ratio) * 64;
	unk *= ratio;
	unk /= symbol;
	unk /= symbol;
	unk += 6;
	msk(b, t, 0x616610 + hoff, 0x0800003f, 0x08000000 | (uint8_t)unk);
	r->dp_h = (uint16_t)hh;
	r->dp_v = (uint32_t)vv;
	r->dp_watermark = (uint8_t)unk;
}

void
pg106_disp_super(const struct pg106_dbus *b, uint32_t pending, unsigned heads,
		 unsigned sors, const struct pg106_disp_sor_sw *sw,
		 struct pg106_disp_super_result *r, struct pg106_dtrace *t)
{
	unsigned h, s;

	phase(t, pending & 1 ? PG106_DPH_SUPER1 :
		 pending & 2 ? PG106_DPH_SUPER2 : PG106_DPH_SUPER3);
	for (h = 0; h < 4; h++)
		if (heads & (1u << h))
			r->mask[h] = rd(b, t, PG106_DISP_SUPER_HEAD(h));

	if (pending & 0x00000001) {
		// nv50_disp_super_1 (nv50.c:1348-1363): latch ARM + ASY.
		for (h = 0; h < 4; h++) {
			if (!(heads & (1u << h)))
				continue;
			head_state(b, t, h, false, &r->arm[h]);
			head_state(b, t, h, true, &r->asy[h]);
		}
		for (s = 0; s < 8; s++) {
			if (!(sors & (1u << s)))
				continue;
			sor_state(b, t, s, false, &r->sor_arm[s]);
			sor_state(b, t, s, true, &r->sor_asy[s]);
		}
		for (h = 0; h < 4; h++) {		// 1.0: OffInt1 IED
			int ior;
			if (!(heads & (1u << h)) || !(r->mask[h] & 0x00001000))
				continue;
			ior = ior_for(r->sor_arm, sors, h);
			if (ior < 0 || !sw[ior].outp_attached)
				continue;
			if (b->ied)
				b->ied(b->user, 0, 1, (int)h, ior, 0);
			rec(t, PG106_DREG_BLOCK, (uint32_t)ior, 1, h,
			    "super 1.0: IED OffInt1 script (nv50.c:1085-1108)");
		}
	} else if (pending & 0x00000002) {
		for (h = 0; h < 4; h++) {		// 2.0: OffInt2 IED
			int ior;
			if (!(heads & (1u << h)) || !(r->mask[h] & 0x00001000))
				continue;
			ior = ior_for(r->sor_arm, sors, h);
			if (ior < 0 || !sw[ior].outp_attached)
				continue;
			if (b->ied)
				b->ied(b->user, 0, 2, (int)h, ior, 0);
			rec(t, PG106_DREG_BLOCK, (uint32_t)ior, 2, h,
			    "super 2.0: IED OffInt2 script");
		}
		for (h = 0; h < 4; h++) {		// 2.1: VPLL
			uint32_t khz;
			if (!(heads & (1u << h)) || !(r->mask[h] & 0x00010000))
				continue;
			khz = r->asy[h].hz / 1000;
			if (!khz)
				continue;
			rec(t, PG106_DREG_BLOCK, 0x80 + h, 0, khz,
			    "super 2.1: nvkm_devinit_pll_set(PLL_VPLL0+head) "
			    "(nv50.c:1308-1316 -> gf100_devinit_pll_set)");
			if (b->pll_set)
				b->pll_set(b->user, 0x80 + h, khz);
		}
		for (h = 0; h < 4; h++) {		// 2.2
			int ior;
			uint32_t khz;
			if (!(heads & (1u << h)) || !(r->mask[h] & 0x00001000))
				continue;
			ior = ior_for(r->sor_asy, sors, h);
			if (ior < 0)
				continue;
			khz = r->asy[h].hz / 1000;
			if (sw[ior].outp_attached) {
				if (b->ied)
					b->ied(b->user, 1, 0, (int)h, ior, khz);
				rec(t, PG106_DREG_BLOCK, (uint32_t)ior, 0, khz,
				    "super 2.2: IED OnInt2 script for khz "
				    "(nv50.c:1029-1083)");
			}
			// rgclk(head, ior->asy.rgdiv): SOR state never sets
			// rgdiv (only nv50_pior_state does, nv50.c:126), so
			// it is 0 for every SOR.
			msk(b, t, PG106_DISP_HEAD_RGCLK(h), 0x0000000f, 0);
			if (r->sor_asy[ior].proto == 2)
				super_2_2_dp(b, t, h, &r->asy[h], &sw[ior], r);
			pg106_disp_sor_clock(b, ior, r->sor_asy[ior].proto == 1,
					     r->sor_asy[ior].link,
					     sw[ior].high_speed, t);
		}
	} else if (pending & 0x00000004) {
		for (h = 0; h < 4; h++) {		// 3.0: OnInt3 IED
			int ior;
			if (!(heads & (1u << h)) || !(r->mask[h] & 0x00001000))
				continue;
			ior = ior_for(r->sor_asy, sors, h);
			if (ior < 0 || !sw[ior].outp_attached)
				continue;
			if (b->ied)
				b->ied(b->user, 1, 1, (int)h, ior,
				       r->asy[h].hz / 1000);
			rec(t, PG106_DREG_BLOCK, (uint32_t)ior, 1, h,
			    "super 3.0: IED OnInt3 script (gp100 SOR has no war_3)");
		}
	}

	for (h = 0; h < 4; h++)
		if (heads & (1u << h))
			wr(b, t, PG106_DISP_SUPER_HEAD(h), 0x00000000);
	wr(b, t, PG106_DISP_SUPER_ACK, 0x80000000);
}

// --- DP link training model (dp.c) ---------------------------------------------

static int
aux_rd(struct pg106_dp_sink *k, struct pg106_dtrace *t, uint32_t a, uint8_t *d,
       unsigned n)
{
	if (k->fail_aux)
		return -5;
	for (unsigned i = 0; i < n; i++) {
		uint32_t x = a + i;
		uint8_t v = x < sizeof(k->dpcd) ? k->dpcd[x] : 0;
		// The simulated sink's link status (DPCD 0x202-0x204).
		if (x >= 0x202 && x <= 0x203) {
			uint8_t nib = 0;
			if (k->drive_writes >= k->cr_after)
				nib |= 0x1;
			if (k->eq_polls >= k->eq_after)
				nib |= 0x6;
			v = (uint8_t)(nib | nib << 4);
		} else if (x == 0x204) {
			v = k->eq_polls >= k->eq_after ? 0x01 : 0x00;
		}
		d[i] = v;
		rec(t, PG106_DREG_AUX_RD, x, 0, v, NULL);
	}
	return 0;
}

static int
aux_wr(struct pg106_dp_sink *k, struct pg106_dtrace *t, uint32_t a,
       const uint8_t *d, unsigned n)
{
	if (k->fail_aux)
		return -5;
	for (unsigned i = 0; i < n; i++) {
		if (a + i < sizeof(k->dpcd))
			k->dpcd[a + i] = d[i];
		rec(t, PG106_DREG_AUX_WR, a + i, 0, d[i], NULL);
	}
	if (a == 0x103)
		k->drive_writes++;
	return 0;
}

struct lt {
	uint8_t stat[6], conf[4], pc2conf[2], pc2stat;
	bool pc2;
};

// gm200_sor_dp_drive (gm200.c:31-54), lanes map {0,1,2,3}.
static void
dp_drive_hw(const struct pg106_dbus *b, struct pg106_dtrace *t,
	    const struct pg106_dp_link *l, int ln, int pc, int dc, int pe, int pu)
{
	const uint32_t loff = (uint32_t)l->sor * 0x800 + (l->link == 2) * 0x80;
	const uint32_t shift = (uint32_t)ln * 8;
	uint32_t d0, d1, d2, d3;

	pu &= 0x0f;
	d0 = rd(b, t, 0x61c118 + loff) & ~(0x000000ffu << shift);
	d1 = rd(b, t, 0x61c120 + loff) & ~(0x000000ffu << shift);
	d2 = rd(b, t, 0x61c130 + loff);
	if ((d2 & 0x00000f00) < ((uint32_t)pu << 8) || ln == 0)
		d2 = (d2 & ~0x00000f00u) | ((uint32_t)pu << 8);
	wr(b, t, 0x61c118 + loff, d0 | ((uint32_t)dc << shift));
	wr(b, t, 0x61c120 + loff, d1 | ((uint32_t)pe << shift));
	wr(b, t, 0x61c130 + loff, d2);
	d3 = rd(b, t, 0x61c13c + loff) & ~(0x000000ffu << shift);
	wr(b, t, 0x61c13c + loff, d3 | ((uint32_t)pc << shift));
}

// nvkm_dp_train_drive (dp.c:132-201).
static int
dp_drive(const struct pg106_dbus *b, struct pg106_dp_sink *k, struct lt *s,
	 const struct pg106_dp_link *l, bool pc, struct pg106_dtrace *t)
{
	memset(s->pc2conf, 0, sizeof(s->pc2conf));
	for (int i = 0; i < l->nr; i++) {
		uint8_t lane = (s->stat[4 + (i >> 1)] >> ((i & 1) * 4)) & 0xf;
		uint8_t lpc2 = (s->pc2stat >> (i * 2)) & 0x3;
		uint8_t lpre = (lane & 0x0c) >> 2;
		uint8_t lvsw = (lane & 0x03) >> 0;
		uint8_t hivs = 3 - lpre, hipe = 3, hipc = 3;

		if (lpc2 >= hipc)
			lpc2 = hipc | 0x04;	// LC0F MAX_POST_CURSOR2_REACHED
		if (lpre >= hipe) {
			lpre = hipe | 0x04;	// "yes." (MAX_SWING_REACHED)
			lvsw = hivs = 3 - (lpre & 3);
		} else if (lvsw >= hivs) {
			lvsw = hivs | 0x04;
		}
		s->conf[i] = (uint8_t)((lpre << 3) | lvsw);
		s->pc2conf[i >> 1] |= (uint8_t)(lpc2 << ((i & 1) * 4));
		// nvbios_dpcfg_match(...) -> drive(ior, i, pc, dc, pe, pu):
		// MODEL stand-in, the requested levels themselves.
		rec(t, PG106_DREG_BLOCK, (uint32_t)i, lvsw & 3, lpre & 3,
		    "dp drive: nvbios_dpcfg_match not modelled (levels = "
		    "sink request)");
		dp_drive_hw(b, t, l, i, lpc2 & 3, lvsw & 3, lpre & 3, 0);
	}
	if (aux_wr(k, t, 0x103, s->conf, 4))
		return -1;
	if (pc && aux_wr(k, t, 0x10f, s->pc2conf, 2))
		return -1;
	return 0;
}

// nvkm_dp_train_sense (dp.c:92-130); the delay is not simulated.
static int
dp_sense(struct pg106_dp_sink *k, struct lt *s, bool pc,
	 struct pg106_dtrace *t)
{
	if (aux_rd(k, t, 0x202, &s->stat[0], 3))
		return -1;
	if (aux_rd(k, t, 0x206, &s->stat[4], 2))
		return -1;
	if (pc && aux_rd(k, t, 0x20c, &s->pc2stat, 1))
		s->pc2stat = 0;
	return 0;
}

// nvkm_dp_train_pattern (dp.c:203-227) + gm107_sor_dp_pattern.
static void
dp_pattern(const struct pg106_dbus *b, struct pg106_dp_sink *k,
	   const struct pg106_dp_link *l, uint8_t pattern,
	   struct pg106_dtrace *t)
{
	static const uint32_t data[5] = {
		0x10101010, 0x01010101, 0x02020202, 0x03030303, 0x1b1b1b1b };
	const uint32_t soff = (uint32_t)l->sor * 0x800;
	uint8_t tp = 0;

	if (l->link & 1)
		msk(b, t, 0x61c110 + soff, 0x1f1f1f1f, data[pattern]);
	else
		msk(b, t, 0x61c12c + soff, 0x1f1f1f1f, data[pattern]);
	aux_rd(k, t, 0x102, &tp, 1);
	tp &= (uint8_t)~0x0f;
	tp |= (pattern != 4) ? pattern : 7;
	if (pattern != 0)
		tp |= 0x20;
	else
		tp &= (uint8_t)~0x20;
	aux_wr(k, t, 0x102, &tp, 1);
}

int
pg106_dp_train(const struct pg106_dbus *b, struct pg106_dp_sink *k,
	       const struct pg106_dp_link *l, struct pg106_dtrace *t)
{
	const uint32_t soff = (uint32_t)l->sor * 0x800;
	const uint32_t loff = soff + (l->link == 2) * 0x80;
	struct lt s;
	uint8_t sink[2];
	int ret = -1, tries, i;

	phase(t, PG106_DPH_DP);
	memset(&s, 0, sizeof(s));
	// GP106 chipset 0x136: TPS4 is masked (< 0x110), TPS3 kept (>= 0xd0)
	// (dp.c:378-381).
	k->dpcd[0x003] &= (uint8_t)~0x80;
	s.pc2 = (k->dpcd[0x002] & 0x40) != 0;

	// nvkm_dp_train_init (dp.c:445-471).
	rec(t, PG106_DREG_BLOCK, 0, 0, k->dpcd[0x003] & 0x01,
	    k->dpcd[0x003] & 0x01 ? "DP Info script[2] EnableSpread" :
				    "DP Info script[3] DisableSpread");
	rec(t, PG106_DREG_BLOCK, 0, 0, 0,
	    "DP Info script[0] BeforeLinkTraining (nvbios_init, devinit.c)");

	// gf119_sor_dp_links (gf119.c:171-189; gm200_sor_dp.links).
	msk(b, t, 0x612300 + soff, 0x007c0000, (uint32_t)l->bw << 18);
	msk(b, t, 0x61c10c + loff, 0x401f4000,
	    (((1u << l->nr) - 1) << 16) | (l->ef ? 0x00004000u : 0));
	// g94_sor_dp_power (g94.c:102-118).
	msk(b, t, 0x61c130 + loff, 0x0000000f, (1u << l->nr) - 1);
	msk(b, t, 0x61c034 + soff, 0x80000000, 0x80000000);
	poll(b, t, 0x61c034 + soff, 0x80000000, 0);

	// nvkm_dp_train_link (dp.c:313-367), no LTTPRs, no eDP rate table.
	sink[0] = l->bw;
	sink[1] = (uint8_t)(l->nr | (l->ef ? 0x80 : 0));
	if (aux_wr(k, t, 0x100, sink, 2))
		goto fini;

	// nvkm_dp_train_cr (dp.c:276-311).
	{
		bool cr_done = false, abort = false;
		int voltage = s.conf[0] & 0x03;
		tries = 0;
		dp_pattern(b, k, l, 1, t);
		do {
			if (dp_drive(b, k, &s, l, false, t) ||
			    dp_sense(k, &s, false, t))
				break;
			cr_done = true;
			for (i = 0; i < l->nr; i++) {
				uint8_t lane = (s.stat[i >> 1] >> ((i & 1) * 4)) & 0xf;
				if (!(lane & 0x01)) {
					cr_done = false;
					if (s.conf[i] & 0x04)
						abort = true;
					break;
				}
			}
			if ((s.conf[0] & 0x03) != voltage) {
				voltage = s.conf[0] & 0x03;
				tries = 0;
			}
		} while (!cr_done && !abort && ++tries < 5);
		ret = cr_done ? 0 : -1;
	}

	// nvkm_dp_train_eq (dp.c:229-273).
	if (ret == 0) {
		bool eq_done = false, cr_done = true;
		tries = 0;
		dp_pattern(b, k, l,
			   (k->dpcd[0] >= 0x12 && (k->dpcd[2] & 0x40)) ? 3 : 2, t);
		do {
			if ((tries && dp_drive(b, k, &s, l, s.pc2, t)) ||
			    dp_sense(k, &s, s.pc2, t))
				break;
			k->eq_polls++;
			eq_done = (s.stat[2] & 0x01) != 0;
			for (i = 0; i < l->nr && eq_done; i++) {
				uint8_t lane = (s.stat[i >> 1] >> ((i & 1) * 4)) & 0xf;
				if (!(lane & 0x01))
					cr_done = false;
				if (!(lane & 0x02) || !(lane & 0x04))
					eq_done = false;
			}
		} while (!eq_done && cr_done && ++tries <= 5);
		ret = eq_done ? 0 : -1;
	}
	dp_pattern(b, k, l, 0, t);

fini:
	rec(t, PG106_DREG_BLOCK, 0, 0, 0,
	    "DP Info script[1] AfterLinkTraining (nvbios_init)");
	return ret;
}

const char *
pg106_dphase_name(uint8_t p)
{
	static const char *const n[] = {
		"init", "chan", "acquire", "hdmi", "dp", "kick", "intr",
		"super1", "super2", "super3",
	};
	return p < sizeof(n) / sizeof(n[0]) ? n[p] : "?";
}
