// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/src/disp_hw.c — GP106 display driver (one HDMI head) over a
// pg106_disp_host. See disp_hw.h for the nouveau file:line of each step.
//
// HARDWARE CODE PATH, verified only against tests/test_disp_hw.c's mock
// display engine. Not yet run on a GP106.

#include <pascal/disp_hw.h>
#include <pascal/devinit.h>

#include <errno.h>
#include <string.h>

#define HOST(d) ((d)->host)

static void flip_check(struct pg106_dhw *d, bool abort, uint64_t ns);

_Static_assert(sizeof(struct pg106_init_trace_rec) <= 6 * sizeof(uint32_t) &&
	       _Alignof(struct pg106_init_trace_rec) <= _Alignof(uint32_t),
	       "disp_hw.h init_trace storage too small for devinit records");

// ---------------------------------------------------------------------------
// host access
// ---------------------------------------------------------------------------

static uint32_t
rd(struct pg106_dhw *d, uint32_t a)
{
	return HOST(d)->rd32(HOST(d)->user, a);
}

static void
wr(struct pg106_dhw *d, uint32_t a, uint32_t v)
{
	HOST(d)->wr32(HOST(d)->user, a, v);
}

static uint32_t
msk(struct pg106_dhw *d, uint32_t a, uint32_t m, uint32_t v)
{
	uint32_t o = rd(d, a);
	wr(d, a, (o & ~m) | v);
	return o;
}

static void
udelay(struct pg106_dhw *d, uint32_t us)
{
	if (HOST(d)->delay_us && us)
		HOST(d)->delay_us(HOST(d)->user, us);
}

// nvkm_msec(device, ms, cond): poll every 10 us until (rd & m) == v.
static int
wait_eq(struct pg106_dhw *d, uint32_t a, uint32_t m, uint32_t v, uint32_t us)
{
	uint32_t spent = 0;

	for (;;) {
		if ((rd(d, a) & m) == v)
			return 0;
		if (spent >= us)
			return -ETIMEDOUT;
		udelay(d, 10);
		spent += 10;
	}
}

static void
vw32(struct pg106_dhw *d, uint64_t pa, uint32_t v)
{
	HOST(d)->vram_write(HOST(d)->user, pa, &v, 4);
}

static uint32_t
vr32(struct pg106_dhw *d, uint64_t pa)
{
	return HOST(d)->vram_rd32(HOST(d)->user, pa);
}

// pg106_dbus adapters (disp_core.c runs over these).
static uint32_t
bus_rd32(void *u, uint32_t a)
{
	return rd(u, a);
}

static void
bus_wr32(void *u, uint32_t a, uint32_t v)
{
	wr(u, a, v);
}

static void
bus_delay(void *u, uint32_t us)
{
	udelay(u, us);
}

// ---------------------------------------------------------------------------
// devinit glue: VPLL (super 2.1) and IED scripts (super 1.0/2.0/2.2/3.0)
// ---------------------------------------------------------------------------

// The interpreter's hooks get the env; the env is the first member of
// this wrapper, so the driver is one cast away.
struct dhw_ienv {
	struct pg106_init_env e;
	struct pg106_dhw *d;
};

static struct pg106_dhw *
ienv_dhw(struct pg106_init_env *e)
{
	return ((struct dhw_ienv *)(void *)e)->d;
}

static uint32_t
ie_rd32(struct pg106_init_env *e, uint32_t a)
{
	return rd(ienv_dhw(e), a);
}

static void
ie_wr32(struct pg106_init_env *e, uint32_t a, uint32_t v)
{
	wr(ienv_dhw(e), a, v);
}

static uint8_t
ie_rd8(struct pg106_init_env *e, uint16_t port, uint8_t index)
{
	(void)e; (void)port; (void)index;
	return 0; // no VGA IO on this path (scripts that need it warn)
}

static void
ie_wr8(struct pg106_init_env *e, uint16_t port, uint8_t index, uint8_t v)
{
	(void)e; (void)port; (void)index; (void)v;
}

static uint8_t
ie_rdport(struct pg106_init_env *e, uint16_t port)
{
	(void)e; (void)port;
	return 0;
}

static void
ie_wrport(struct pg106_init_env *e, uint16_t port, uint8_t v)
{
	(void)e; (void)port; (void)v;
}

static uint64_t
ie_time(struct pg106_init_env *e, uint32_t us)
{
	struct pg106_dhw *d = ienv_dhw(e);
	udelay(d, us);
	return HOST(d)->time_ns ? HOST(d)->time_ns(HOST(d)->user) / 1000u : 0;
}

static const struct pg106_init_vt ie_vt = {
	.rd32 = ie_rd32,
	.wr32 = ie_wr32,
	.rd8 = ie_rd8,
	.wr8 = ie_wr8,
	.rdport = ie_rdport,
	.wrport = ie_wrport,
	.time = ie_time,
};

// No stdio in the portable core: copy devinit's message by hand.
static void
init_error(struct pg106_dhw *d, const struct pg106_init_env *e, uint32_t script)
{
	size_t i;

	d->st.last_init_script = script;
	for (i = 0; i + 1 < sizeof(d->st.last_init_error) && e->error_msg[i]; i++)
		d->st.last_init_error[i] = e->error_msg[i];
	d->st.last_init_error[i] = 0;
}

static int
init_env(struct pg106_dhw *d, struct dhw_ienv *w)
{
	struct pg106_init_env *e = &w->e;
	uint32_t strap;

	// The interpreter needs a trace buffer (a full one aborts the
	// script); reuse the driver's, from the start, for every run.
	pg106_init_env_init(e, &ie_vt, d->vb.data, d->vb.size,
			    (struct pg106_init_trace_rec *)(void *)d->init_trace,
			    PG106_DHW_INIT_TRACE);
	w->d = d;
	if (pg106_init_env_from_bios(e, &d->vb))
		return -ENOENT;
	// nvkm_device_ctor crystal (device/base.c:3385-3399), GF100+ strap.
	strap = rd(d, 0x101000) & 0x00400040;
	switch (strap) {
	case 0x00000000: e->crystal_khz = 13500; break;
	case 0x00000040: e->crystal_khz = 14318; break;
	case 0x00400000: e->crystal_khz = 27000; break;
	default:         e->crystal_khz = 25000; break;
	}
	e->step_limit = PG106_INIT_STEP_LIMIT_DEFAULT;
	return 0;
}

static int
bus_pll_set(void *u, uint32_t type, uint32_t khz)
{
	struct pg106_dhw *d = u;
	struct dhw_ienv e;
	int ret;

	if (!d->have_vb)
		return -ENOENT;
	if (init_env(d, &e))
		return -ENOENT;
	ret = pg106_init_pll_set(&e.e, type, khz);
	d->diag.pll_type = type;
	d->diag.pll_khz = khz;
	d->diag.pll_ret = ret;
	d->diag.pll_nw = 0;
	for (size_t i = 0; i < e.e.trace_len; i++) {
		const struct pg106_init_trace_rec *t = &e.e.trace[i];
		if (t->kind != PG106_INIT_TRACE_MMIO || d->diag.pll_nw >= PG106_DHW_DIAG_PLLW)
			continue;
		d->diag.pll_w[d->diag.pll_nw][0] = t->address;
		d->diag.pll_w[d->diag.pll_nw][1] = t->value;
		d->diag.pll_nw++;
	}
	if (ret) {
		d->st.pll_fail++;
		init_error(d, &e.e, 0);
	}
	else
		d->st.pll_set++;
	return ret;
}

// nv50_disp_super_ied_on/off: pick the script, run it through the devinit
// interpreter with init.outp/or/link/head set.
static void
bus_ied(void *u, int on, int id, int head, int sor, uint32_t khz)
{
	struct pg106_dhw *d = u;
	struct pg106_dvb_ied_ctx c = {
		.hasht = d->outp.dcb.hasht,
		.hashm_or = d->outp.dcb.or_mask,
		.link = d->outp.dcb.link,
		.head = head,
		.proto_evo = (uint8_t)((d->sor_ctrl >> 8) & 0xf),
		.ior_link = d->outp.dcb.sorconf_link,
		.lvds24 = false,
	};
	struct dhw_ienv w;
	struct pg106_init_env *e = &w.e;
	uint32_t script = 0;
	const char *why = NULL;

	unsigned slot = d->diag.ied_n < PG106_DHW_DIAG_IED ? d->diag.ied_n++ :
			PG106_DHW_DIAG_IED - 1;

	d->diag.ied[slot].on = (uint8_t)on;
	d->diag.ied[slot].id = (uint8_t)id;
	d->diag.ied[slot].khz = khz;
	d->diag.ied[slot].script = 0;
	d->diag.ied[slot].ret = 1;
	d->diag.ied[slot].mmio = 0;
	if (!d->have_vb) {
		d->st.ied_missing++;
		return;
	}
	if (pg106_dvb_ied_script(&d->vb, &c, on != 0, id, khz, &script, &why) ||
	    !script) {
		d->st.ied_missing++;	// nouveau: OUTP_DBG + return
		return;
	}
	d->diag.ied[slot].script = script;
	if (init_env(d, &w)) {
		d->st.ied_fail++;
		return;
	}
	e->outp = &d->outp.dcb;
	e->or = sor;
	e->link = d->outp.dcb.sorconf_link;
	e->head = head;
	d->diag.ied[slot].ret = pg106_init_run(e, script);
	for (size_t i = 0; i < e->trace_len; i++)
		if (e->trace[i].kind == PG106_INIT_TRACE_MMIO)
			d->diag.ied[slot].mmio++;
	if (d->diag.ied[slot].ret != PG106_INIT_OK) {
		d->diag.ied[slot].ret = -d->diag.ied[slot].ret;
		d->st.ied_fail++;
		init_error(d, e, script);
	}
	else
		d->st.ied_run++;
}

// ---------------------------------------------------------------------------
// output discovery
// ---------------------------------------------------------------------------

const struct pg106_dhw_outp pg106_dhw_helios300_outp = {
	.valid = true,
	.dcb = {
		.index = 0,
		.conn_raw = 0x02022f62u,
		.conf_raw = 0x00020010u,
		.type = 2,		// DCB_OUTPUT_TMDS
		.i2c_index = 6,
		.heads = 0xf,
		.connector = 2,
		.location = 0,
		.or_mask = 0x2,
		.link = 1,
		.sorconf_link = 1,
		.extdev = 0,
		.hasht = 0x0002,
		.hashm = 0x0f42,
	},
	.conn_type = 0x61,		// DCB_CONNECTOR_HDMI_0
	.i2c = { .type = PG106_DCB_I2C_PMGR, .drive = 6, .sense = 0xff,
		 .share = 0, .auxch = 0, .ver = 0x41 },
	.hpd_line = 27,
	.hpd_log1 = 2,
};

int
pg106_dhw_outp_from_vbios(const struct pg106_vbios_bit *vb,
			  struct pg106_dhw_outp *o)
{
	struct pg106_dcb_outp cand, first;
	bool have_first = false;
	uint8_t i;

	memset(o, 0, sizeof(*o));
	for (i = 0; i < 32; i++) {
		struct pg106_dcb_conn cn;
		if (pg106_vbios_dcb_outp(vb, i, &cand))
			break;
		if (cand.type != 2)	// DCB_OUTPUT_TMDS
			continue;
		if (!have_first) {
			first = cand;
			have_first = true;
		}
		if (!pg106_dvb_conn(vb, cand.connector, &cn) && cn.type == 0x61) {
			first = cand;
			break;
		}
	}
	if (!have_first)
		return -ENOENT;
	o->dcb = first;
	o->valid = true;
	{
		struct pg106_dcb_conn cn;
		struct pg106_dcb_gpio g;

		o->hpd_line = 0xff;
		if (!pg106_dvb_conn(vb, first.connector, &cn)) {
			o->conn_type = cn.type;
			if (cn.hpd_func != 0xff &&
			    !pg106_dvb_gpio_match(vb, cn.hpd_func, 0xff, &g)) {
				o->hpd_line = g.line;
				o->hpd_log1 = g.log[1];
			}
		}
	}
	if (pg106_dvb_i2c(vb, first.i2c_index, &o->i2c))
		memset(&o->i2c, 0xff, sizeof(o->i2c));
	return 0;
}

int
pg106_dhw_ctor(struct pg106_dhw *d, const struct pg106_disp_host *host,
	       const uint8_t *vbios, uint32_t vbios_len,
	       const struct pg106_dhw_outp *outp)
{
	unsigned s;

	if (!d || !host || !host->rd32 || !host->wr32 || !host->vram_alloc ||
	    !host->vram_write || !host->vram_rd32)
		return -EINVAL;
	memset(d, 0, sizeof(*d));
	d->host = host;
	d->head = -1;
	d->sor = -1;
	for (s = 0; s < 8; s++)
		d->sw[s].sor = -1;
	if (vbios && vbios_len && !pg106_vbios_bit_parse(vbios, vbios_len, &d->vb))
		d->have_vb = true;
	if (outp)
		d->outp = *outp;
	else if (!d->have_vb || pg106_dhw_outp_from_vbios(&d->vb, &d->outp))
		return -ENOENT;
	d->bus.rd32 = bus_rd32;
	d->bus.wr32 = bus_wr32;
	d->bus.pll_set = bus_pll_set;
	d->bus.ied = bus_ied;
	d->bus.delay_us = host->delay_us ? bus_delay : NULL;
	d->bus.user = d;
	return 0;
}

// ---------------------------------------------------------------------------
// RAMHT + ctxdma objects (core/ramht.c, engine/dma/usergf119.c)
// ---------------------------------------------------------------------------

uint32_t
pg106_dhw_ramht_hash(int chid, uint32_t handle)
{
	const unsigned bits = 9;	// order_base_2(0x1000 >> 3)
	uint32_t hash = 0;

	while (handle) {
		hash ^= handle & ((1u << bits) - 1);
		handle >>= bits;
	}
	hash ^= (uint32_t)chid << (bits - 4);
	return hash;
}

// nvkm_object_bind of a gf119 dmaobj into disp->inst + nvkm_ramht_insert
// with gf119_disp_dmac_bind's context.
static int
ctxdma_new(struct pg106_dhw *d, int chid, uint32_t handle, uint64_t start,
	   uint64_t limit)
{
	const unsigned n = PG106_DHW_RAMHT_SIZE >> 3;
	uint32_t off = (d->heap_next + 15u) & ~15u, co, ho;

	if (off + 24 > PG106_DHW_INST_SIZE)
		return -ENOSPC;
	co = ho = pg106_dhw_ramht_hash(chid, handle);
	for (unsigned i = 0; i < n; i++)
		if (d->ramht[i].chid == chid && d->ramht[i].handle == handle)
			return -EEXIST;
	do {
		if (co < n && d->ramht[co].chid < 0)
			break;
		if (++co >= n)
			co = 0;
	} while (co != ho);
	if (co >= n || d->ramht[co].chid >= 0)
		return -ENOSPC;

	d->heap_next = off + 24;
	// gf119_dmaobj_bind: flags0 = kind PITCH << 20 | page SP << 6 | VRAM 9
	// (no gf119_dma_v0 args -> PITCH/SP for a non-VM target).
	vw32(d, d->inst_pa + off + 0x00, (0u << 20) | (1u << 6) | 0x9u);
	vw32(d, d->inst_pa + off + 0x04, (uint32_t)(start >> 8));
	vw32(d, d->inst_pa + off + 0x08, (uint32_t)(limit >> 8));
	vw32(d, d->inst_pa + off + 0x0c, 0);
	vw32(d, d->inst_pa + off + 0x10, 0);
	vw32(d, d->inst_pa + off + 0x14, 0);

	d->ramht[co].chid = chid;
	d->ramht[co].handle = handle;
	vw32(d, d->inst_pa + (co << 3) + 0, handle);
	vw32(d, d->inst_pa + (co << 3) + 4,
	     ((uint32_t)chid << 27 | 0x00000001u) | (off << 9));
	return 0;
}

// ---------------------------------------------------------------------------
// DMA channels (gp102.c, dispnv50/disp.c nv50_dmac_*)
// ---------------------------------------------------------------------------

static int
chan_init(struct pg106_dhw *d, struct pg106_dhw_chan *c, bool core)
{
	c->cur = c->put = 0;
	c->max = PG106_DHW_PUSH_SIZE / 4 - 1 - 12;	// EVO last-12-dword bug
	// nvkm_disp_chan_init: intr(chan, true) then init.
	msk(d, 0x6100a0, 1u << c->user, 1u << c->user);
	return pg106_disp_chan_init(&d->bus, c->ctrl, c->user,
				    (uint32_t)(c->push_pa >> 8) | 0x1u, 0, core,
				    d->trace);
}

static void
chan_fini(struct pg106_dhw *d, struct pg106_dhw_chan *c, bool core)
{
	const uint32_t ctrl = 0x610490 + c->ctrl * 0x10;

	if (core) {			// gf119_disp_core_fini
		msk(d, ctrl, 0x00000010, 0x00000000);
		msk(d, ctrl, 0x00000003, 0x00000000);
	} else {			// gf119_disp_dmac_fini
		msk(d, ctrl, 0x00001010, 0x00001000);
		msk(d, ctrl, 0x00000003, 0x00000000);
	}
	wait_eq(d, ctrl, 0x001e0000, 0, 2000000);
	// chan->func->intr(chan, false) (gf119_disp_chan_intr).
	msk(d, 0x610090, 1u << c->user, 0);
	msk(d, 0x6100a0, 1u << c->user, 0);
}

static uint32_t
chan_get(struct pg106_dhw *d, struct pg106_dhw_chan *c)
{
	return (rd(d, 0x640004 + c->user * 0x1000) & 0x00000ffcu) >> 2;
}

static void
chan_kick(struct pg106_dhw *d, struct pg106_dhw_chan *c)
{
	if (c->put != c->cur) {
		pg106_disp_kick(&d->bus, c->user, c->cur, true, d->trace);
		c->put = c->cur;
	}
}

// nv50_dmac_wait + PUSH_MTHD stream copy: reserve `n` dwords (winding to
// the start with a JUMP when needed), copy, advance.
static int
chan_push(struct pg106_dhw *d, struct pg106_dhw_chan *c, const uint32_t *w,
	  uint32_t n)
{
	uint32_t get, free;

	if (n > c->max)
		return -EINVAL;
	if (c->cur + n >= c->max) {
		// nv50_dmac_wind.
		if (chan_get(d, c) == 0) {
			if (c->put == 0)
				chan_kick(d, c);
			for (uint32_t t = 0; chan_get(d, c) == 0; t += 10) {
				if (t >= 2000000)
					return -ETIMEDOUT;
				udelay(d, 10);
			}
		}
		vw32(d, c->push_pa + (uint64_t)c->cur * 4, 0x20000000u); // JUMP 0
		c->cur = 0;
		chan_kick(d, c);
	}
	for (uint32_t t = 0;; t += 10) {
		get = chan_get(d, c);
		free = get > c->cur ? get - c->cur - 5 : c->max - c->cur;
		if (get > c->cur && get < c->cur + 5)
			free = 0;
		if (free >= n)
			break;
		if (t >= 2000000)
			return -ETIMEDOUT;
		udelay(d, 10);
	}
	HOST(d)->vram_write(HOST(d)->user, c->push_pa + (uint64_t)c->cur * 4, w,
			    n * 4);
	c->cur += n;
	return 0;
}

static int
push_stream(struct pg106_dhw *d, struct pg106_dhw_chan *c,
	    const struct pg106_push *p)
{
	if (p->overflow)
		return -E2BIG;
	return chan_push(d, c, p->buf, (uint32_t)p->len);
}

// ---------------------------------------------------------------------------
// I2C: GM200 pad + gf119 bit-bang bus + nouveau's bit algorithm
// ---------------------------------------------------------------------------

#define T_RISEFALL_US 1u
#define T_HOLD_US     5u
#define T_TIMEOUT_US  2200u

static uint32_t
i2c_addr(const struct pg106_dhw *d)
{
	return 0x00d014 + (uint32_t)d->outp.i2c.drive * 0x20;
}

static void
pad_mode(struct pg106_dhw *d, int i2c_mode)
{
	uint32_t base;

	if (d->outp.i2c.share == 0xff)
		return;		// pad_x: no mode switch
	base = (uint32_t)d->outp.i2c.share * 0x50;
	if (i2c_mode) {
		msk(d, 0x00d970 + base, 0x0000c003, 0x0000c001);
		msk(d, 0x00d97c + base, 0x00000001, 0x00000000);
	} else {
		msk(d, 0x00d97c + base, 0x00000001, 0x00000001);
	}
}

static void
scl(struct pg106_dhw *d, int s)
{
	msk(d, i2c_addr(d), 0x1, s ? 0x1 : 0);
}

static void
sda(struct pg106_dhw *d, int s)
{
	msk(d, i2c_addr(d), 0x2, s ? 0x2 : 0);
}

static int
sense_scl(struct pg106_dhw *d)
{
	return !!(rd(d, i2c_addr(d)) & 0x10);
}

static int
sense_sda(struct pg106_dhw *d)
{
	return !!(rd(d, i2c_addr(d)) & 0x20);
}

static bool
raise_scl(struct pg106_dhw *d)
{
	uint32_t timeout = T_TIMEOUT_US / T_RISEFALL_US;

	scl(d, 1);
	do {
		udelay(d, T_RISEFALL_US);
	} while (!sense_scl(d) && --timeout);
	return timeout != 0;
}

static int
i2c_start(struct pg106_dhw *d)
{
	int ret = 0;

	if (!sense_scl(d) || !sense_sda(d)) {
		scl(d, 0);
		sda(d, 1);
		if (!raise_scl(d))
			ret = -EBUSY;
	}
	sda(d, 0);
	udelay(d, T_HOLD_US);
	scl(d, 0);
	udelay(d, T_HOLD_US);
	return ret;
}

static void
i2c_stop(struct pg106_dhw *d)
{
	scl(d, 0);
	sda(d, 0);
	udelay(d, T_RISEFALL_US);
	scl(d, 1);
	udelay(d, T_HOLD_US);
	sda(d, 1);
	udelay(d, T_HOLD_US);
}

static int
i2c_bitw(struct pg106_dhw *d, int bit)
{
	sda(d, bit);
	udelay(d, T_RISEFALL_US);
	if (!raise_scl(d))
		return -ETIMEDOUT;
	udelay(d, T_HOLD_US);
	scl(d, 0);
	udelay(d, T_HOLD_US);
	return 0;
}

static int
i2c_bitr(struct pg106_dhw *d)
{
	int bit;

	sda(d, 1);
	udelay(d, T_RISEFALL_US);
	if (!raise_scl(d))
		return -ETIMEDOUT;
	udelay(d, T_HOLD_US);
	bit = sense_sda(d);
	scl(d, 0);
	udelay(d, T_HOLD_US);
	return bit;
}

static int
i2c_get_byte(struct pg106_dhw *d, uint8_t *byte, bool last)
{
	int i, bit;

	*byte = 0;
	for (i = 7; i >= 0; i--) {
		bit = i2c_bitr(d);
		if (bit < 0)
			return bit;
		*byte |= (uint8_t)(bit << i);
	}
	return i2c_bitw(d, last ? 1 : 0);
}

static int
i2c_put_byte(struct pg106_dhw *d, uint8_t byte)
{
	int i, ret;

	for (i = 7; i >= 0; i--) {
		ret = i2c_bitw(d, !!(byte & (1u << i)));
		if (ret < 0)
			return ret;
	}
	ret = i2c_bitr(d);
	if (ret == 1)		// nack
		ret = -EIO;
	return ret;
}

struct i2c_msg {
	uint8_t addr;
	bool rd;
	uint8_t *buf;
	uint16_t len;
};

// nvkm_i2c_bit_xfer between nvkm_i2c_bus_acquire/release.
static int
i2c_xfer(struct pg106_dhw *d, struct i2c_msg *m, int num)
{
	int ret = 0, n = num;

	if (d->outp.i2c.drive == 0xff)
		return -ENODEV;
	d->st.i2c_xfers++;
	pad_mode(d, 1);
	while (!ret && n--) {
		uint16_t remaining = m->len;
		uint8_t *p = m->buf;

		ret = i2c_start(d);
		if (ret == 0)
			ret = i2c_put_byte(d, (uint8_t)(m->addr << 1 | (m->rd ? 1 : 0)));
		if (m->rd) {
			while (!ret && remaining--)
				ret = i2c_get_byte(d, p++, !remaining);
		} else {
			while (!ret && remaining--)
				ret = i2c_put_byte(d, *p++);
		}
		m++;
	}
	i2c_stop(d);
	pad_mode(d, 0);		// pad->mode stays OFF -> release turns it off
	if (ret < 0)
		d->st.i2c_errors++;
	return ret < 0 ? ret : num;
}

// drm_do_probe_ddc_edid: [segment 0x30] + offset write 0x50 + read.
static int
ddc_block(struct pg106_dhw *d, unsigned block, uint8_t *out)
{
	uint8_t seg = (uint8_t)(block >> 1), start = (uint8_t)((block & 1) * 128);
	struct i2c_msg m[3];
	int n = 0, ret, retry;

	for (retry = 0; retry < 5; retry++) {	// drm: 5 retries
		n = 0;
		if (seg)
			m[n++] = (struct i2c_msg){ 0x30, false, &seg, 1 };
		m[n++] = (struct i2c_msg){ 0x50, false, &start, 1 };
		m[n++] = (struct i2c_msg){ 0x50, true, out, 128 };
		ret = i2c_xfer(d, m, n);
		if (ret == n)
			return 0;
	}
	return ret < 0 ? ret : -EIO;
}

static bool
block_sum_ok(const uint8_t *b)
{
	uint8_t s = 0;
	for (unsigned i = 0; i < 128; i++)
		s = (uint8_t)(s + b[i]);
	return s == 0;
}

int
pg106_dhw_read_edid(struct pg106_dhw *d)
{
	unsigned ext, b, nblocks;
	int ret;

	d->edid_valid = false;
	d->edid_len = 0;
	ret = ddc_block(d, 0, d->edid_raw);
	if (ret)
		return ret;
	if (!block_sum_ok(d->edid_raw)) {	// one re-read, as drm does
		ret = ddc_block(d, 0, d->edid_raw);
		if (ret)
			return ret;
	}
	ext = d->edid_raw[126];
	nblocks = 1 + ext;
	if (nblocks > sizeof(d->edid_raw) / 128)
		nblocks = sizeof(d->edid_raw) / 128;
	for (b = 1; b < nblocks; b++) {
		if (ddc_block(d, b, d->edid_raw + b * 128))
			break;
	}
	d->edid_len = (size_t)b * 128;
	if (pg106_edid_parse(d->edid_raw, d->edid_len, &d->edid) != PG106_EDID_OK)
		return -EBADMSG;
	d->edid_valid = true;
	return 0;
}

// ---------------------------------------------------------------------------
// HPD (GPIO)
// ---------------------------------------------------------------------------

bool
pg106_dhw_detect(struct pg106_dhw *d)
{
	uint32_t v;

	if (d->outp.hpd_line == 0xff) {	// no HPD line: try DDC
		uint8_t b;
		struct i2c_msg m = { 0x50, true, &b, 1 };
		d->connected = i2c_xfer(d, &m, 1) == 1;
		return d->connected;
	}
	v = rd(d, 0x00d610 + (uint32_t)d->outp.hpd_line * 4);
	// gf119_gpio_sense bit 14; nvkm_gpio_get: present when sense ==
	// (log[1] & 1).
	d->connected = (int)!!(v & 0x00004000) == (d->outp.hpd_log1 & 1);
	return d->connected;
}

// gk104_gpio_intr_mask(gpio, HI|LO, 1 << line, on ? 1 << line : 0).
static void
hpd_intr(struct pg106_dhw *d, bool on)
{
	uint32_t mask, data, inte0, inte1;

	if (d->outp.hpd_line == 0xff)
		return;
	mask = 1u << d->outp.hpd_line;
	data = on ? mask : 0;
	inte0 = rd(d, 0x00dc08);
	inte1 = rd(d, 0x00dc88);
	// HI then LO, as nvkm_gpio_intr_init runs for each type: the
	// trace shows two read-modify-write pairs (0x800, then 0x8000800
	// for line 27).
	inte0 = (inte0 & ~(mask & 0xffff)) | (data & 0xffff);
	inte1 = (inte1 & ~(mask >> 16)) | (data >> 16);
	wr(d, 0x00dc08, inte0);
	wr(d, 0x00dc88, inte1);
	inte0 = rd(d, 0x00dc08);
	inte1 = rd(d, 0x00dc88);
	inte0 = (inte0 & ~((mask << 16) & 0xffffffffu)) | ((data << 16) & 0xffffffffu);
	inte1 = (inte1 & ~((mask >> 16) << 16)) | ((data >> 16) << 16);
	wr(d, 0x00dc08, inte0);
	wr(d, 0x00dc88, inte1);
}

// gk104_gpio_intr_stat; returns true when the HPD line changed.
static bool
hpd_service(struct pg106_dhw *d)
{
	uint32_t intr0 = rd(d, 0x00dc00), intr1 = rd(d, 0x00dc80);
	uint32_t stat0 = rd(d, 0x00dc08) & intr0;
	uint32_t stat1 = rd(d, 0x00dc88) & intr1;
	uint32_t lo = (stat1 & 0xffff0000u) | (stat0 >> 16);
	uint32_t hi = (stat1 << 16) | (stat0 & 0x0000ffffu);

	wr(d, 0x00dc00, intr0);
	wr(d, 0x00dc80, intr1);
	if (d->outp.hpd_line == 0xff)
		return false;
	return ((lo | hi) & (1u << d->outp.hpd_line)) != 0;
}

// ---------------------------------------------------------------------------
// init / fini
// ---------------------------------------------------------------------------

static int
alloc_mem(struct pg106_dhw *d)
{
	const struct pg106_disp_host *h = HOST(d);
	int ret;

	if (d->mem_allocated)
		return 0;
	// nv50_disp_oneinit: nvkm_gpuobj_new(0x10000, align 0x10000).
	if ((ret = h->vram_alloc(h->user, PG106_DHW_INST_SIZE, 0x10000, &d->inst_pa)))
		return ret;
	if ((ret = h->vram_alloc(h->user, PG106_DHW_SYNC_SIZE, 0x1000, &d->sync_pa)))
		goto f0;
	if ((ret = h->vram_alloc(h->user, PG106_DHW_PUSH_SIZE, 0x1000, &d->core.push_pa)))
		goto f1;
	if ((ret = h->vram_alloc(h->user, PG106_DHW_PUSH_SIZE, 0x1000, &d->base.push_pa)))
		goto f2;
	if ((ret = h->vram_alloc(h->user, PG106_DHW_CURS_SIZE, 0x1000, &d->curs_pa)))
		goto f3;
	d->fb_size = (uint64_t)PG106_DHW_FB_MAX_W * 4 * PG106_DHW_FB_MAX_H;
	if ((ret = h->vram_alloc(h->user, d->fb_size, 0x10000, &d->fb_pa)))
		goto f4;
	d->mem_allocated = true;
	return 0;
f4:	h->vram_free(h->user, d->curs_pa, PG106_DHW_CURS_SIZE);
f3:	h->vram_free(h->user, d->base.push_pa, PG106_DHW_PUSH_SIZE);
f2:	h->vram_free(h->user, d->core.push_pa, PG106_DHW_PUSH_SIZE);
f1:	h->vram_free(h->user, d->sync_pa, PG106_DHW_SYNC_SIZE);
f0:	h->vram_free(h->user, d->inst_pa, PG106_DHW_INST_SIZE);
	return ret;
}

static void
free_mem(struct pg106_dhw *d)
{
	const struct pg106_disp_host *h = HOST(d);

	if (!d->mem_allocated || !h->vram_free)
		return;
	h->vram_free(h->user, d->fb_pa, d->fb_size);
	h->vram_free(h->user, d->curs_pa, PG106_DHW_CURS_SIZE);
	h->vram_free(h->user, d->base.push_pa, PG106_DHW_PUSH_SIZE);
	h->vram_free(h->user, d->core.push_pa, PG106_DHW_PUSH_SIZE);
	h->vram_free(h->user, d->sync_pa, PG106_DHW_SYNC_SIZE);
	h->vram_free(h->user, d->inst_pa, PG106_DHW_INST_SIZE);
	d->mem_allocated = false;
}

// core907d_caps_init (core917d's caps_init, dispnv50/core907d.c:33-56):
// GET_CAPABILITIES and wait NV907D_CORE_NOTIFIER_3_CAPABILITIES_4 DONE —
// dword 4, byte 0x10 of the core notifier. (The Helios mmiotrace shows
// nouveau resetting and polling sync+0x10, reading 0xc0000005 back.)
static int
core_caps(struct pg106_dhw *d)
{
	uint32_t w[6];
	struct pg106_push p = { w, 6, 0, false };
	uint32_t v;
	int ret;

	vw32(d, d->sync_pa + 0x10, 0);	// CAPABILITIES_4 DONE FALSE
	v = 0x80000000u;		// NOTIFY ENABLE, MODE WRITE, OFFSET 0
	pg106_push_mthd(&p, PG106_917D_SET_NOTIFIER_CONTROL, &v, 1);
	v = 0;
	pg106_push_mthd(&p, 0x008c, &v, 1);	// GET_CAPABILITIES
	pg106_push_mthd(&p, PG106_917D_SET_NOTIFIER_CONTROL, &v, 1);
	if ((ret = push_stream(d, &d->core, &p)))
		return ret;
	chan_kick(d, &d->core);
	for (uint32_t t = 0; !(vr32(d, d->sync_pa + 0x10) & 1); t += 10) {
		if (t >= 2000000) {
			d->st.ntfy_timeouts++;	// nouveau: NV_ERROR, carry on
			return 0;
		}
		udelay(d, 10);
	}
	return 0;
}

int
pg106_dhw_init(struct pg106_dhw *d)
{
	struct pg106_disp_init_cfg cfg = { 0 };
	uint32_t tmp, hda;
	unsigned i;
	int ret;

	if ((ret = alloc_mem(d)))
		return ret;

	// The VRAM is new after a power cycle: write every object again.
	for (i = 0; i < PG106_DHW_INST_SIZE; i += 4)
		vw32(d, d->inst_pa + i, 0);
	for (i = 0; i < PG106_DHW_SYNC_SIZE; i += 4)
		vw32(d, d->sync_pa + i, 0);
	for (i = 0; i < (PG106_DHW_RAMHT_SIZE >> 3); i++)
		d->ramht[i].chid = -1;
	d->heap_next = PG106_DHW_RAMHT_SIZE;
	if (!d->vram_size)
		d->vram_size = 0x100000000ull;	// limit of the VRAM ctxdmas

	// gf119_head_cnt / gf119_sor_cnt.
	tmp = rd(d, 0x612004);
	d->heads_mask = tmp & 0xf;
	d->sors_mask = (tmp & 0xff00) >> 8;
	// gp100_sor_new: HDA-capable SORs.
	hda = rd(d, 0x08a15c);
	if (!(hda & 0x40000000))
		hda = rd(d, 0x10ebb0) >> 8;
	d->sor_hda_mask = (uint8_t)(hda & d->sors_mask);

	cfg.inst_addr = d->inst_pa;
	cfg.heads_mask = d->heads_mask;
	cfg.sors_mask = d->sors_mask;
	if (d->inst_pa >> 40)
		return -ERANGE;
	if ((ret = pg106_disp_init(&d->bus, &cfg, d->trace)))
		return ret;

	// Core: ctrl/user 0. Base for head 0: ctrl/user 1.
	d->core.ctrl = d->core.user = PG106_DISP_CHID_CORE;
	d->base.ctrl = d->base.user = PG106_DISP_CHID_BASE(0);
	if ((ret = chan_init(d, &d->core, true)))
		goto fail;
	// Core ctxdmas (nv50_dmac_create: sync then vram).
	if ((ret = ctxdma_new(d, 0, PG106_DHW_HANDLE_SYNCBUF, d->sync_pa,
			      d->sync_pa + PG106_DHW_SYNC_SIZE - 1)) ||
	    (ret = ctxdma_new(d, 0, PG106_DHW_HANDLE_VRAM, 0, d->vram_size - 1)))
		goto fail;
	{	// core507d_init
		uint32_t w[2], v = PG106_DHW_HANDLE_SYNCBUF;
		struct pg106_push p = { w, 2, 0, false };
		pg106_push_mthd(&p, PG106_917D_SET_CONTEXT_DMA_NOTIFIER, &v, 1);
		if ((ret = push_stream(d, &d->core, &p)))
			goto fail;
		chan_kick(d, &d->core);
	}
	if ((ret = core_caps(d)))
		goto fail;

	if ((ret = chan_init(d, &d->base, false)))
		goto fail;
	if ((ret = ctxdma_new(d, 1, PG106_DHW_HANDLE_SYNCBUF, d->sync_pa,
			      d->sync_pa + PG106_DHW_SYNC_SIZE - 1)) ||
	    (ret = ctxdma_new(d, 1, PG106_DHW_HANDLE_VRAM, 0, d->vram_size - 1)) ||
	    (ret = ctxdma_new(d, 1, PG106_DHW_HANDLE_FB, 0, d->vram_size - 1)))
		goto fail;

	// Cursor PIO channel (gf119_disp_pioc_init), ctrl 13 user 17.
	msk(d, 0x6100a0, 1u << PG106_DHW_CURS_USER, 1u << PG106_DHW_CURS_USER);
	wr(d, 0x610490 + PG106_DHW_CURS_CTRL * 0x10, 0x00000001);
	if (wait_eq(d, 0x610490 + PG106_DHW_CURS_CTRL * 0x10, 0x00030000,
		    0x00010000, 2000000)) {
		ret = -EBUSY;
		goto fail;
	}
	d->chans_up = true;

	// I2C: aux interrupts off + ack (gk104_aux_mask/stat), pad init (OFF),
	// bus init (0x7).
	wr(d, 0x00dc68, 0);
	wr(d, 0x00dc60, rd(d, 0x00dc60));
	pad_mode(d, 0);
	if (d->outp.i2c.drive != 0xff)
		wr(d, i2c_addr(d), 0x00000007);

	// nvkm_gpio init/fini (gpio/base.c:145-155): every line's interrupt
	// masked (gk104_gpio_intr_mask TOGGLED, 0xffffffff, 0) and the status
	// acked (gk104_gpio_intr_stat) — the Helios mmiotrace shows nouveau
	// doing this before it enables the HPD line.
	wr(d, 0x00dc08, 0);
	wr(d, 0x00dc88, 0);
	hpd_service(d);
	// HPD: interrupt on both edges, then sense.
	hpd_intr(d, true);
	pg106_dhw_detect(d);

	d->inited = true;
	d->head = -1;
	d->sor = -1;
	d->hdmi_on = false;
	d->vblank_on = false;
	d->curs_on = false;
	return 0;
fail:
	pg106_dhw_fini(d, false);
	return ret;
}

void
pg106_dhw_vram_lost(struct pg106_dhw *d)
{
	d->mem_allocated = false;
	d->inst_pa = d->sync_pa = d->curs_pa = d->fb_pa = 0;
	d->core.push_pa = d->base.push_pa = 0;
}

void
pg106_dhw_fini(struct pg106_dhw *d, bool release)
{
	if (d->inited && d->head >= 0)
		pg106_dhw_modeset(d, NULL, 0);
	if (d->vblank_on)
		pg106_dhw_vblank_enable(d, false);
	hpd_intr(d, false);
	if (d->chans_up) {
		// gf119_disp_pioc_fini
		msk(d, 0x610490 + PG106_DHW_CURS_CTRL * 0x10, 0x1, 0);
		wait_eq(d, 0x610490 + PG106_DHW_CURS_CTRL * 0x10, 0x00030000, 0,
			2000000);
		msk(d, 0x6100a0, 1u << PG106_DHW_CURS_USER, 0);
		chan_fini(d, &d->base, false);
		chan_fini(d, &d->core, true);
		d->chans_up = false;
	} else {
		chan_fini(d, &d->base, false);
		chan_fini(d, &d->core, true);
	}
	wr(d, 0x6100b0, 0);	// gf119_disp_fini: all display interrupts off
	d->inited = false;
	if (release)
		free_mem(d);
}

// ---------------------------------------------------------------------------
// interrupts (gf119_disp_intr, gp102_disp_intr_error)
// ---------------------------------------------------------------------------

static void
super_run(struct pg106_dhw *d, uint32_t pending)
{
	// nvkm keeps head->arm/asy and ior->arm/asy from supervisor 1 for 2
	// and 3 (nv50_disp_super_1 latches them); so does d->super_res.
	struct pg106_disp_super_result *r = &d->super_res;

	if (pending & 1)
		memset(r, 0, sizeof(*r));
	for (unsigned s = 0; s < 8; s++) {
		d->sw[s].sor = (int)s;
		d->sw[s].outp_attached = (int)s == d->sor;
		d->sw[s].high_speed = (int)s == d->sor && d->high_speed;
	}
	pg106_disp_super(&d->bus, pending, d->heads_mask, d->sors_mask, d->sw,
			 r, d->trace);
	if (d->diag.nstages < sizeof(d->diag.stages)) {
		d->diag.stages[d->diag.nstages] = pending & 1 ? 1 : pending & 2 ? 2 : 3;
		d->diag.stage_mask[d->diag.nstages] = r->mask[0];
		d->diag.nstages++;
	}
	if (pending & 1) {
		d->diag.asy = r->asy[0];
		d->diag.arm = r->arm[0];
		if (d->sor >= 0) {
			d->diag.sor_asy = r->sor_asy[d->sor];
			d->diag.sor_arm = r->sor_arm[d->sor];
		}
	}
	if ((pending & 2) && d->sor >= 0) {
		d->diag.sor_clock = rd(d, PG106_DISP_SOR_CLOCK(d->sor));
		d->diag.rgclk = rd(d, PG106_DISP_HEAD_RGCLK(0));
	}
	if (pending & 1) d->st.supv[0]++;
	else if (pending & 2) d->st.supv[1]++;
	else if (pending & 4) d->st.supv[2]++;
}

// Display part only; returns true if anything was pending.
static bool
disp_service(struct pg106_dhw *d)
{
	uint32_t intr = rd(d, 0x610088);
	bool any = intr != 0;

	if (intr & 0x00000001) {
		uint32_t stat = rd(d, 0x61008c);
		wr(d, 0x61008c, stat);	// per-bit acks folded into one W1C
		intr &= ~0x00000001u;
	}
	if (intr & 0x00000002) {
		uint32_t stat = rd(d, 0x61009c);
		int chid = -1;
		for (int i = 0; i < 32; i++)
			if (stat & (1u << i)) {
				chid = i;
				break;
			}
		if (chid >= 0) {
			uint32_t mthd = rd(d, 0x6111f0 + chid * 12);
			d->st.chan_errors++;
			d->st.last_error_chid = (uint32_t)chid;
			d->st.last_error_mthd = mthd & 0xffc;
			d->st.last_error_data = rd(d, 0x6111f4 + chid * 12);
			wr(d, 0x61009c, 1u << chid);
			wr(d, 0x6111f0 + chid * 12, 0x90000000);
		}
		intr &= ~0x00000002u;
	}
	if (intr & 0x00100000) {
		uint32_t stat = rd(d, 0x6100ac);
		if (stat & 0x7) {
			uint32_t pending = stat & 0x7;
			wr(d, 0x6100ac, pending);
			super_run(d, pending);
			stat &= ~0x7u;
		}
		if (stat)
			wr(d, 0x6100ac, stat);
	}
	for (unsigned h = 0; h < 4; h++) {
		const uint32_t hoff = h * 0x800;
		if (!(d->heads_mask & (1u << h)) || !(intr & (0x01000000u << h)))
			continue;
		uint32_t stat = rd(d, 0x6100bc + hoff);
		if (stat & 1) {
			uint64_t ns = HOST(d)->time_ns ?
				      HOST(d)->time_ns(HOST(d)->user) : 0;
			d->st.vblank++;
			if ((int)h == d->head)
				flip_check(d, false, ns);
			if (d->vblank)
				d->vblank(d->cb_ctx, (int)h, ns);
		}
		msk(d, 0x6100bc + hoff, 0, 0);
		rd(d, 0x6100c0 + hoff);
	}
	return any;
}

void
pg106_dhw_intr(struct pg106_dhw *d)
{
	bool was;

	d->st.intr++;
	if (d->inited)
		disp_service(d);
	if (hpd_service(d)) {
		was = d->connected;
		pg106_dhw_detect(d);
		d->st.hpd_events++;
		if (d->hotplug && was != d->connected)
			d->hotplug(d->cb_ctx, d->connected);
	}
}

void
pg106_dhw_vblank_enable(struct pg106_dhw *d, bool on)
{
	int h = d->head >= 0 ? d->head : 0;
	msk(d, 0x6100c0 + (uint32_t)h * 0x800, 0x1, on ? 0x1 : 0);
	d->vblank_on = on;
}

// ---------------------------------------------------------------------------
// HDMI side data
// ---------------------------------------------------------------------------

static uint8_t
vic_aspect(uint8_t vic)
{
	switch (vic) {
	case 1: case 2: case 17:
		return 1;		// HDMI_PICTURE_ASPECT_4_3
	case 0:
		return 0;
	default:
		return 2;		// 16:9
	}
}

int
pg106_dhw_avi_infoframe(const struct pg106_disp_mode *m, uint8_t vic,
			uint8_t f[17])
{
	unsigned sum = 0, i;

	(void)m;
	memset(f, 0, 17);
	// hdmi_avi_infoframe_pack (video/hdmi.c): type 0x82, v2, len 13.
	f[0] = 0x82;
	f[1] = 0x02;
	f[2] = 13;
	// PB1: Y = RGB (0), A0 = 1 (active_aspect valid), S = underscan (2).
	f[4] = 0x10 | 0x02;
	// PB2: colorimetry none, picture aspect, active aspect = picture (8).
	f[5] = (uint8_t)(vic_aspect(vic) << 4 | 0x8);
	// PB3: Q = full range (2) — drm_hdmi_avi_infoframe_quant_range with
	// HDMI_QUANTIZATION_RANGE_FULL on a non-HDMI-2.0 sink.
	f[6] = 2u << 2;
	f[7] = vic;
	// PB5: YQ limited (0), content type graphics (0), no repetition.
	for (i = 0; i < 17; i++)
		sum += f[i];
	f[3] = (uint8_t)(256 - (sum & 0xff));
	return 17;
}

size_t
pg106_dhw_eld(const uint8_t *edid, size_t len, uint8_t *eld, size_t max)
{
	struct pg106_edid e;
	size_t mnl, n;

	if (max < 20 + 16 + 30 || pg106_edid_parse(edid, len, &e) != PG106_EDID_OK)
		return 0;
	if (!e.cea.present || !e.cea.hdmi_vsdb || (!e.cea.basic_audio && !e.cea.sad_count))
		return 0;
	memset(eld, 0, max);
	mnl = strlen(e.monitor_name);
	if (mnl > 16)
		mnl = 16;
	// drm_edid_to_eld (drm_edid.c): version 2 header, baseline block.
	eld[0] = 2u << 3;				// ELD_VER
	eld[4] = (uint8_t)(e.cea.revision << 5 | mnl);	// CEA_EDID_VER | MNL
	eld[5] = (uint8_t)(e.cea.sad_count << 4);	// SAD_COUNT, conn HDMI
	eld[7] = e.cea.speaker_alloc;
	eld[16] = edid[8];				// manufacturer
	eld[17] = edid[9];
	eld[18] = edid[10];				// product code
	eld[19] = edid[11];
	memcpy(&eld[20], e.monitor_name, mnl);
	memcpy(&eld[20 + mnl], e.cea.sad, (size_t)e.cea.sad_count * 3);
	n = 20 + mnl + (size_t)e.cea.sad_count * 3;
	eld[2] = (uint8_t)((n - 4 + 3) / 4);		// baseline length, dwords
	return 4 + (size_t)eld[2] * 4;
}

static bool
timing_eq(const struct pg106_edid_timing *t, const struct pg106_disp_mode *m)
{
	uint32_t c = t->pixel_clock_khz, mc = m->pixel_clock_khz;
	// Accept the 1000/1001 variant (drm_match_cea_mode clock tolerance).
	bool clk = (mc + 5 >= c && mc <= c + 5) ||
		   (mc + 5 >= c * 1000 / 1001 && mc <= c * 1000 / 1001 + 5);

	return clk && t->hactive == m->hdisplay && t->vactive == m->vdisplay &&
	       t->htotal == m->htotal && t->vtotal == m->vtotal &&
	       t->hactive + t->hsync_offset == m->hsync_start &&
	       t->hactive + t->hsync_offset + t->hsync_width == m->hsync_end &&
	       !m->interlace;
}

uint8_t
pg106_dhw_mode_vic(const struct pg106_disp_mode *m)
{
	for (unsigned v = 1; v < 128; v++) {
		const struct pg106_edid_timing *t = pg106_edid_cea_timing((uint8_t)v);
		if (t && timing_eq(t, m))
			return (uint8_t)v;
	}
	return 0;
}

static uint8_t
sink_vic(const struct pg106_dhw *d, const struct pg106_disp_mode *m)
{
	if (d->edid_valid)
		for (unsigned i = 0; i < d->edid.cea.vic_count; i++) {
			const struct pg106_edid_timing *t =
				pg106_edid_cea_timing(d->edid.cea.vic[i]);
			if (t && timing_eq(t, m))
				return d->edid.cea.vic[i];
		}
	return pg106_dhw_mode_vic(m);
}

// gk104_sor_hdmi_infoframe_avi (data NULL/size 0 = disable).
static void
write_infoframe(struct pg106_dhw *d, uint32_t base, const uint8_t *f, int size,
		bool avi)
{
	const uint32_t hoff = (uint32_t)d->head * 0x400;
	uint32_t w[5] = { 0 };

	if (avi)
		msk(d, base + hoff, 0x00000001, 0);
	else
		msk(d, base + hoff, 0x00010001, 0);
	if (!size)
		return;
	// pack_hdmi_infoframe
	w[0] = f[0] | f[1] << 8 | (uint32_t)f[2] << 16;
	w[1] = f[3] | f[4] << 8 | (uint32_t)f[5] << 16 | (uint32_t)f[6] << 24;
	w[2] = f[7] | f[8] << 8 | (uint32_t)f[9] << 16;
	w[3] = f[10] | f[11] << 8 | (uint32_t)f[12] << 16 | (uint32_t)f[13] << 24;
	w[4] = f[14] | f[15] << 8 | (uint32_t)f[16] << 16;
	wr(d, base + 0x08 + hoff, w[0]);
	wr(d, base + 0x0c + hoff, w[1]);
	wr(d, base + 0x10 + hoff, w[2]);
	if (avi) {
		wr(d, base + 0x14 + hoff, w[3]);
		wr(d, base + 0x18 + hoff, w[4]);
	}
	msk(d, base + hoff, 0x00000001, 0x00000001);
}

// gf119_sor_hda_hpd / gf119_sor_hda_eld (nvkm_uoutp_mthd_hda_eld order:
// hpd(true) then eld; size 0 -> hpd(false)).
static void
hda_eld(struct pg106_dhw *d, const uint8_t *eld, size_t size)
{
	const uint32_t soff = 0x030 * (uint32_t)d->sor + (uint32_t)d->head * 0x04;
	size_t i;

	if (!(d->sor_hda_mask & (1u << d->sor)))
		return;
	if (!size) {
		msk(d, 0x10ec10 + soff, 0x80000003, 0x80000000);
		return;
	}
	msk(d, 0x616548 + (uint32_t)d->head * 0x800, 0x00000070,
	    (uint32_t)d->head << 4);
	msk(d, 0x10ec10 + soff, 0x80000001, 0x80000001);
	for (i = 0; i < size; i++)
		wr(d, 0x10ec00 + soff, (uint32_t)(i << 8) | eld[i]);
	for (; i < 0x60; i++)
		wr(d, 0x10ec00 + soff, (uint32_t)(i << 8));
	msk(d, 0x10ec10 + soff, 0x80000002, 0x80000002);
}

// ---------------------------------------------------------------------------
// modes
// ---------------------------------------------------------------------------

static void
timing_to_mode(const struct pg106_edid_timing *t, struct pg106_dhw_mode *o)
{
	memset(o, 0, sizeof(*o));
	o->m.pixel_clock_khz = t->pixel_clock_khz;
	o->m.hdisplay = t->hactive;
	o->m.hsync_start = (uint16_t)(t->hactive + t->hsync_offset);
	o->m.hsync_end = (uint16_t)(o->m.hsync_start + t->hsync_width);
	o->m.htotal = t->htotal;
	o->m.vdisplay = t->vactive;
	o->m.vsync_start = (uint16_t)(t->vactive + t->vsync_offset);
	o->m.vsync_end = (uint16_t)(o->m.vsync_start + t->vsync_width);
	o->m.vtotal = t->vtotal;
	o->m.interlace = t->interlaced;
	o->m.hsync_positive = t->hsync_positive;
	o->m.vsync_positive = t->vsync_positive;
	o->m.bpc = 8;
	o->refresh_mhz = pg106_edid_refresh_milli_hz(t);
}

static bool
mode_ok(const struct pg106_dhw *d, const struct pg106_dhw_mode *m)
{
	uint32_t lim = PG106_DHW_TMDS_MAX_KHZ;

	if (d->edid.cea.max_tmds_khz && d->edid.cea.max_tmds_khz < lim)
		lim = d->edid.cea.max_tmds_khz;
	return m->m.pixel_clock_khz && m->m.pixel_clock_khz <= lim &&
	       !m->m.interlace && m->m.hdisplay <= PG106_DHW_FB_MAX_W &&
	       m->m.vdisplay <= PG106_DHW_FB_MAX_H && m->m.htotal > m->m.hdisplay &&
	       m->m.vtotal > m->m.vdisplay &&
	       pg106_hdmi_max_ac_packet(&m->m) >= 0;
}

static size_t
add_mode(const struct pg106_dhw *d, struct pg106_dhw_mode *out, size_t n,
	 size_t max, struct pg106_dhw_mode *m)
{
	if (!mode_ok(d, m) || n >= max)
		return n;
	for (size_t i = 0; i < n; i++)
		if (out[i].m.hdisplay == m->m.hdisplay &&
		    out[i].m.vdisplay == m->m.vdisplay &&
		    (out[i].refresh_mhz + 500) / 1000 == (m->refresh_mhz + 500) / 1000)
			return n;
	if (!m->vic)
		m->vic = sink_vic(d, &m->m);
	out[n] = *m;
	return n + 1;
}

size_t
pg106_dhw_modes(const struct pg106_dhw *d, struct pg106_dhw_mode *out,
		size_t max)
{
	struct pg106_dhw_mode m;
	size_t n = 0;
	unsigned i;

	if (!d->edid_valid)
		return 0;
	for (i = 0; i < d->edid.dtd_count; i++) {
		timing_to_mode(&d->edid.dtd[i], &m);
		m.preferred = i == 0 && n == 0;
		n = add_mode(d, out, n, max, &m);
	}
	for (i = 0; i < d->edid.cea.dtd_count; i++) {
		timing_to_mode(&d->edid.cea.dtd[i], &m);
		m.preferred = n == 0;
		n = add_mode(d, out, n, max, &m);
	}
	for (i = 0; i < d->edid.cea.vic_count; i++) {
		const struct pg106_edid_timing *t = pg106_edid_cea_timing(d->edid.cea.vic[i]);
		if (!t)
			continue;
		timing_to_mode(t, &m);
		m.vic = d->edid.cea.vic[i];
		m.preferred = n == 0;
		n = add_mode(d, out, n, max, &m);
	}
	for (i = 0; i < 24; i++) {
		const struct pg106_edid_timing *t;
		if (!(d->edid.established_timings[i / 8] & (0x80u >> (i % 8))))
			continue;
		// established_timings[b] bit (7 - k) <-> index b*8 + (7-k)
		t = pg106_edid_established_timing((i / 8) * 8 + (7 - i % 8));
		if (!t)
			continue;
		timing_to_mode(t, &m);
		m.preferred = n == 0;
		n = add_mode(d, out, n, max, &m);
	}
	return n;
}

// ---------------------------------------------------------------------------
// modeset
// ---------------------------------------------------------------------------

static int
ntfy_wait(struct pg106_dhw *d, uint64_t pa, uint32_t m, uint32_t v)
{
	for (uint32_t t = 0;; t += 10) {
		// The supervisor handshake needs the interrupt serviced; the
		// gate is held, so run it here (gf119_disp_intr's work).
		disp_service(d);
		if ((vr32(d, pa) & m) == v)
			return 0;
		if (t >= 2000000) {
			d->st.ntfy_timeouts++;
			return -ETIMEDOUT;
		}
		udelay(d, 10);
	}
}

static int
pick_sor(struct pg106_dhw *d, bool hda)
{
	// nvkm_outp_acquire_or -> nvkm_outp_acquire_hda: an unused SOR with
	// the wanted HDA capability, else (no HDA wanted) any unused SOR.
	for (int pass = 0; pass < 2; pass++) {
		bool want = pass == 0 ? hda : !hda;
		if (pass == 1 && hda)
			break;
		for (int s = 0; s < 8; s++) {
			if (!(d->sors_mask & (1u << s)))
				continue;
			if (!!(d->sor_hda_mask & (1u << s)) == want)
				return s;
		}
	}
	return -ENOSPC;
}

static void
diag_readback(struct pg106_dhw *d, int ret)
{
	struct pg106_dhw_diag *g = &d->diag;
	unsigned m = 0;

	g->result = ret;
	g->ntfy = vr32(d, d->sync_pa);
	g->arm_raster = rd(d, 0x640414);
	g->arm_pclk = rd(d, 0x640450);
	g->arm_head_ctrl = rd(d, 0x640408);
	if (d->sor >= 0) {
		const uint32_t soff = (uint32_t)d->sor * 0x800;
		g->arm_sor_ctrl = rd(d, 0x640200 + (uint32_t)d->sor * 0x20);
		g->sor_pwr = rd(d, 0x61c004 + soff);
		g->sor_pwr_stat = rd(d, 0x61c030 + soff);
		g->sor_hdmi_scdc = rd(d, 0x61c5bc + soff);
	}
	while (m < 8 && !(d->outp.dcb.or_mask & (1u << m)))
		m++;
	g->route = rd(d, 0x612308 + m * 0x100);
	g->hdmi_ctrl = rd(d, 0x616798);
}

static int
head_off(struct pg106_dhw *d)
{
	uint32_t w[16];
	struct pg106_push p = { w, 16, 0, false };
	int ret;

	if (d->head < 0)
		return 0;
	flip_check(d, true, 0);
	if (d->curs_on)
		pg106_dhw_cursor_show(d, false);
	if (d->hdmi_on) {
		hda_eld(d, NULL, 0);
		pg106_disp_hdmi_disable(&d->bus, d->head, d->trace);
		d->hdmi_on = false;
	}
	vw32(d, d->sync_pa, 0);			// core507d_ntfy_init
	pg106_disp_push_core_disable(&p, (uint8_t)d->head, (uint8_t)d->sor,
				     d->sor_ctrl);
	if ((ret = push_stream(d, &d->core, &p)))
		return ret;
	chan_kick(d, &d->core);
	ret = ntfy_wait(d, d->sync_pa, 1, 1);
	// release: route off (gm200_sor_route_set with no SOR).
	pg106_disp_sor_route(&d->bus, d->outp.dcb.or_mask, d->outp.dcb.sorconf_link,
			     -1, 0, d->trace);
	d->head = -1;
	d->sor = -1;
	d->sor_ctrl = 0;
	return ret;
}

int
pg106_dhw_modeset(struct pg106_dhw *d, const struct pg106_disp_mode *mode,
		  uint32_t pitch)
{
	struct pg106_disp_commit c;
	struct pg106_dhw_mode chk;
	uint32_t wb[32], wc[128];
	struct pg106_push pb = { wb, 32, 0, false }, pc = { wc, 128, 0, false };
	bool hdmi, audio;
	uint8_t avi[17], eld[128];
	size_t eld_len = 0;
	int ret, sor, ac;

	if (!d->inited)
		return -ENODEV;
	{
		uint32_t seq = d->diag.seq + 1;
		memset(&d->diag, 0, sizeof(d->diag));
		d->diag.seq = seq;
	}
	if (!mode)
		return head_off(d);
	memset(&chk, 0, sizeof(chk));
	chk.m = *mode;
	if (!mode_ok(d, &chk))
		return -EINVAL;
	if (pitch < (uint32_t)mode->hdisplay * 4 || (pitch & 0xff) ||
	    (uint64_t)pitch * mode->vdisplay > d->fb_size)
		return -EINVAL;
	if (d->head >= 0 && (ret = head_off(d)))
		return ret;

	hdmi = !d->edid_valid || d->edid.cea.hdmi_vsdb;
	if (d->edid_valid && d->edid.cea.present && !d->edid.cea.hdmi_vsdb)
		hdmi = false;	// a DVI sink: no HDMI packets
	if (d->outp.conn_type != 0x61)
		hdmi = false;
	eld_len = hdmi && d->edid_valid ?
		  pg106_dhw_eld(d->edid_raw, d->edid_len, eld, sizeof(eld)) : 0;
	audio = eld_len != 0;
	if ((sor = pick_sor(d, audio)) < 0)
		return sor;

	d->head = 0;	// DCB heads 0xf; nouveau's first CRTC is head 0
	d->sor = sor;
	d->mode = *mode;
	d->mode.head = 0;
	d->mode.bpc = 8;
	d->fb_pitch = pitch;
	d->fb_w = mode->hdisplay;
	d->fb_h = mode->vdisplay;
	d->scan_pa = d->fb_pa;
	d->high_speed = mode->pixel_clock_khz > 340000;

	// nv50_sor_atomic_enable: acquire -> route; HDMI; audio.
	pg106_disp_sor_route(&d->bus, d->outp.dcb.or_mask, d->outp.dcb.sorconf_link,
			     sor, d->outp.dcb.sorconf_link, d->trace);
	if (hdmi) {
		ac = pg106_hdmi_max_ac_packet(&d->mode);
		d->high_speed = pg106_disp_hdmi_enable(&d->bus, 0, sor, (uint8_t)ac,
						       PG106_HDMI_REKEY,
						       mode->pixel_clock_khz,
						       false, false, false, d->trace);
		pg106_dhw_avi_infoframe(&d->mode, sink_vic(d, &d->mode), avi);
		write_infoframe(d, 0x690000, avi, 17, true);
		write_infoframe(d, 0x690100, NULL, 0, false);	// no VSI
		d->hdmi_on = true;
		if (audio)
			hda_eld(d, eld, eld_len);
		d->audio = audio;
	}

	memset(&c, 0, sizeof(c));
	c.mode = d->mode;
	c.sor = (uint8_t)sor;
	// SINGLE_TMDS_A/B from the DCB sublink (nv50_sor_atomic_enable).
	c.proto = (d->outp.dcb.sorconf_link & 1) ? PG106_SOR_PROTO_SINGLE_TMDS_A :
						    PG106_SOR_PROTO_SINGLE_TMDS_B;
	c.sor_depth = 0;	// PIXEL_DEPTH_DEFAULT for TMDS
	c.core_vram_handle = PG106_DHW_HANDLE_VRAM;
	c.base.handle = PG106_DHW_HANDLE_FB;
	c.base.offset = d->fb_pa;
	c.base.w = mode->hdisplay;
	c.base.h = mode->vdisplay;
	c.base.pitch = pitch;
	c.base.format = 0xcf;	// A8R8G8B8
	c.base.cpp = 4;
	d->sor_ctrl = ((uint32_t)c.proto << 8) | 1u;

	vw32(d, d->sync_pa, 0);			// core507d_ntfy_init
	pg106_disp_push_base_image(&pb, &c);
	pg106_disp_push_core_modeset(&pc, &c);
	if ((ret = push_stream(d, &d->base, &pb)))
		return ret;
	chan_kick(d, &d->base);			// commit_wndw before commit_core
	if ((ret = push_stream(d, &d->core, &pc)))
		return ret;
	chan_kick(d, &d->core);
	ret = ntfy_wait(d, d->sync_pa, 1, 1);
	diag_readback(d, ret);
	if (ret)
		return ret;
	if (d->curs_on) {
		d->curs_on = false;
		pg106_dhw_cursor_show(d, true);
	}
	return 0;
}

// ---------------------------------------------------------------------------
// flip (base channel image update, no core interlock)
// ---------------------------------------------------------------------------

int
pg106_dhw_flip(struct pg106_dhw *d, uint64_t pa, uint32_t pitch, bool vsync)
{
	uint32_t w[24], v[2];
	struct pg106_push p = { w, 24, 0, false };
	struct pg106_disp_commit c;
	const uint32_t ntfy = 0x40u * (1u + (uint32_t)d->head) + 0x20u;
	int ret;

	if (d->head < 0)
		return -ENODEV;
	if (pa + (uint64_t)pitch * d->fb_h > d->vram_size || (pa >> 40) ||
	    (pa & 0xff) || (pitch & 0xff) || pitch < (uint32_t)d->fb_w * 4)
		return -EINVAL;
	if (d->flip_pending)
		return -EBUSY;
	memset(&c, 0, sizeof(c));
	c.base.handle = PG106_DHW_HANDLE_FB;
	c.base.offset = pa;
	c.base.w = d->fb_w;
	c.base.h = d->fb_h;
	c.base.pitch = pitch;
	c.base.format = 0xcf;
	c.base.cpp = 4;
	// base507c_ntfy_reset + ntfy_set (offset NV50_DISP_BASE_NTFY(head)).
	vw32(d, d->sync_pa + ntfy, 0);
	v[0] = ntfy;				// MODE WRITE, OFFSET = ntfy >> 2
	v[1] = PG106_DHW_HANDLE_SYNCBUF;
	pg106_push_mthd(&p, 0x00a0, v, 2);	// SET_NOTIFIER_CONTROL, _DMA_
	{
		size_t start = p.len;
		pg106_disp_push_base_image(&p, &c);
		// image_set's PRESENT_CONTROL word: interval 1 non-tearing for
		// vsync, interval 0 immediate otherwise (wndw.c:316-323).
		if (!vsync)
			p.buf[start + 1] = 0x1u;	// BEGIN_MODE IMMEDIATE
		// base507c_update with no core interlock.
		p.buf[p.len - 1] = 0;
	}
	if ((ret = push_stream(d, &d->base, &p)))
		return ret;
	chan_kick(d, &d->base);
	d->flip_pending = true;
	d->flip_pa = pa;
	return 0;
}

// Base notifier STATUS != NOT_BEGUN once the flip latched
// (base507c_ntfy_wait_begun).
bool
pg106_dhw_flip_landed(struct pg106_dhw *d)
{
	const uint32_t ntfy = 0x40u * (1u + (uint32_t)(d->head < 0 ? 0 : d->head)) + 0x20u;
	return (vr32(d, d->sync_pa + ntfy) >> 30) != 0;
}

// Report a pending flip (landed, or aborted when the head goes away).
static void
flip_check(struct pg106_dhw *d, bool abort, uint64_t ns)
{
	if (!d->flip_pending)
		return;
	if (!abort && !pg106_dhw_flip_landed(d))
		return;
	d->flip_pending = false;
	if (!abort)
		d->scan_pa = d->flip_pa;
	if (d->flip_done)
		d->flip_done(d->cb_ctx, abort ? -ECANCELED : 0, ns);
}

// ---------------------------------------------------------------------------
// cursor
// ---------------------------------------------------------------------------

int
pg106_dhw_cursor_image(struct pg106_dhw *d, const uint32_t *argb)
{
	if (!d->mem_allocated)
		return -ENODEV;
	return HOST(d)->vram_write(HOST(d)->user, d->curs_pa, argb,
				   PG106_DHW_CURS_SIZE);
}

static int
curs_pio(struct pg106_dhw *d, uint32_t mthd, uint32_t v)
{
	const uint32_t user = 0x640000 + PG106_DHW_CURS_USER * 0x1000;

	// curs507a_space: FREE COUNT (5:0) >= 4 within 100 ms.
	for (uint32_t t = 0; (rd(d, user + 0x0008) & 0x3f) < 4; t += 10) {
		if (t >= 100000)
			return -ETIMEDOUT;
		udelay(d, 10);
	}
	wr(d, user + mthd, v);
	return 0;
}

int
pg106_dhw_cursor_show(struct pg106_dhw *d, bool on)
{
	uint32_t w[8], v[2];
	struct pg106_push p = { w, 8, 0, false };
	int ret;

	if (d->head < 0) {
		d->curs_on = on;	// applied at the next modeset
		return 0;
	}
	if (on) {	// head907d_curs_set
		v[0] = 0x80000000u | 1u << 24 | 1u << 26; // ENABLE, A8R8G8B8, 64x64
		v[1] = (uint32_t)(d->curs_pa >> 8);
		pg106_push_mthd(&p, 0x0480 + (uint32_t)d->head * 0x300, v, 2);
		v[0] = PG106_DHW_HANDLE_VRAM;
		pg106_push_mthd(&p, 0x048c + (uint32_t)d->head * 0x300, v, 1);
	} else {	// head907d_curs_clr
		v[0] = 1u << 24 | 1u << 26;
		pg106_push_mthd(&p, 0x0480 + (uint32_t)d->head * 0x300, v, 1);
		v[0] = 0;
		pg106_push_mthd(&p, 0x048c + (uint32_t)d->head * 0x300, v, 1);
	}
	v[0] = 0;
	pg106_push_mthd(&p, PG106_917D_UPDATE, v, 1);
	if ((ret = push_stream(d, &d->core, &p)))
		return ret;
	chan_kick(d, &d->core);
	d->curs_on = on;
	if (on)
		return pg106_dhw_cursor_move(d, d->curs_x, d->curs_y);
	return 0;
}

int
pg106_dhw_cursor_move(struct pg106_dhw *d, int x, int y)
{
	int ret;

	d->curs_x = (int16_t)x;
	d->curs_y = (int16_t)y;
	if (d->head < 0 || !d->chans_up)
		return 0;
	// curs507a_point + curs507a_update (INTERLOCK_WITH_CORE DISABLE).
	ret = curs_pio(d, 0x0084, ((uint32_t)(uint16_t)y << 16) | (uint16_t)x);
	if (!ret)
		ret = curs_pio(d, 0x0080, 0);
	return ret;
}
