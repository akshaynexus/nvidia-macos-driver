// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/src/disp_vbios.c — DCB I2C / connector / GPIO / IED tables.
// See disp_vbios.h for the nouveau file:line each function follows.

#include <pascal/disp_vbios.h>

#include <errno.h>
#include <string.h>

static int
dcb(const struct pg106_vbios_bit *vb, uint32_t *d, uint8_t *ver, uint8_t *hdr,
    uint8_t *cnt, uint8_t *len)
{
	return pg106_vbios_dcb_table(vb, d, ver, hdr, cnt, len) ? -ENOENT : 0;
}

// --- I2C (bios/i2c.c:28-133) ------------------------------------------------

int
pg106_dvb_i2c(const struct pg106_vbios_bit *vb, uint8_t idx,
	      struct pg106_dcb_i2c *o)
{
	uint32_t d, ent, v32;
	uint16_t t = 0, v16;
	uint8_t ver, hdr, cnt, len, b0, b1;

	if (!o)
		return -EINVAL;
	memset(o, 0xff, sizeof(*o));
	if (dcb(vb, &d, &ver, &hdr, &cnt, &len))
		return -ENOENT;
	if (ver >= 0x15 && pg106_vbios_rd16(vb, d + 2, &t))
		return -EFAULT;
	if (ver >= 0x30 && pg106_vbios_rd16(vb, d + 4, &t))
		return -EFAULT;
	if (!t)
		return -ENOENT;
	if (ver >= 0x42)
		return -ENOTSUP;		// "ccb %02x not supported"
	if (ver >= 0x30) {
		if (pg106_vbios_rd08(vb, t + 0, &ver) ||
		    pg106_vbios_rd08(vb, t + 1, &hdr) ||
		    pg106_vbios_rd08(vb, t + 2, &cnt) ||
		    pg106_vbios_rd08(vb, t + 3, &len))
			return -EFAULT;
	} else {
		hdr = 0;
		cnt = 16;
		len = 4;
	}
	if (idx >= cnt)
		return -ENOENT;
	ent = t + hdr + (uint32_t)idx * len;
	o->ver = ver;
	o->drive = o->sense = o->share = o->auxch = PG106_DCB_I2C_UNUSED;
	if (ver >= 0x41) {
		if (pg106_vbios_rd32(vb, ent, &v32))
			return -EFAULT;
		if ((v32 & 0x1f) == 0x1f && ((v32 >> 5) & 0x1f) == 0x1f)
			o->type = PG106_DCB_I2C_UNUSED;
		else
			o->type = PG106_DCB_I2C_PMGR;
	} else if (ver >= 0x30) {
		if (pg106_vbios_rd08(vb, ent + 3, &o->type))
			return -EFAULT;
	} else {
		if (pg106_vbios_rd08(vb, ent + 3, &o->type))
			return -EFAULT;
		o->type &= 0x07;
		if (o->type == 0x07)
			o->type = PG106_DCB_I2C_UNUSED;
	}
	if (pg106_vbios_rd08(vb, ent + 0, &b0) || pg106_vbios_rd08(vb, ent + 1, &b1) ||
	    pg106_vbios_rd16(vb, ent + 0, &v16))
		return -EFAULT;
	switch (o->type) {
	case PG106_DCB_I2C_NV04_BIT:
		o->drive = b0;
		o->sense = b1;
		break;
	case PG106_DCB_I2C_NV4E_BIT:
		o->drive = b1;
		break;
	case PG106_DCB_I2C_NVIO_BIT:
		o->drive = b0 & 0x0f;
		if (b1 & 0x01)
			o->share = b1 >> 1;
		break;
	case PG106_DCB_I2C_NVIO_AUX:
		o->auxch = b0 & 0x0f;
		if (b1 & 0x01)
			o->share = o->auxch;
		break;
	case PG106_DCB_I2C_PMGR:
		o->drive = v16 & 0x1f;
		if (o->drive == 0x1f)
			o->drive = PG106_DCB_I2C_UNUSED;
		o->auxch = (v16 & 0x3e0) >> 5;
		if (o->auxch == 0x1f)
			o->auxch = PG106_DCB_I2C_UNUSED;
		o->share = o->auxch;
		break;
	case PG106_DCB_I2C_UNUSED:
		break;
	default:			// "unknown i2c type"
		o->type = PG106_DCB_I2C_UNUSED;
		break;
	}
	return 0;
}

// --- connector (bios/conn.c:28-98, engine/disp/conn.c:44-75) ---------------

int
pg106_dvb_conn(const struct pg106_vbios_bit *vb, uint8_t idx,
	       struct pg106_dcb_conn *o)
{
	static const uint8_t hpd_map[] = { 0x07, 0x08, 0x51, 0x52, 0x5e, 0x5f, 0x60 };
	uint32_t d, e;
	uint16_t t;
	uint8_t ver, hdr, cnt, len, b[4] = { 0 };
	unsigned i, bit;

	if (!o)
		return -EINVAL;
	memset(o, 0, sizeof(*o));
	o->hpd_func = 0xff;
	if (dcb(vb, &d, &ver, &hdr, &cnt, &len))
		return -ENOENT;
	if (ver < 0x30 || hdr < 0x16)
		return -ENOENT;
	if (pg106_vbios_rd16(vb, d + 0x14, &t) || !t)
		return -ENOENT;
	if (pg106_vbios_rd08(vb, t + 0, &ver) || pg106_vbios_rd08(vb, t + 1, &hdr) ||
	    pg106_vbios_rd08(vb, t + 2, &cnt) || pg106_vbios_rd08(vb, t + 3, &len))
		return -EFAULT;
	if ((ver != 0x30 && ver != 0x40) || idx >= cnt)
		return -ENOENT;
	e = t + hdr + (uint32_t)idx * len;
	for (i = 0; i < 4 && i < len; i++)
		if (pg106_vbios_rd08(vb, e + i, &b[i]))
			return -EFAULT;
	o->type = b[0];
	o->location = b[1] & 0x0f;
	o->hpd = (b[1] & 0x30) >> 4;
	o->dp = (b[1] & 0xc0) >> 6;
	if (len >= 4) {
		o->hpd |= (b[2] & 0x03) << 2;
		o->dp |= b[2] & 0x0c;
		o->di = (b[2] & 0xf0) >> 4;
		o->hpd |= (b[3] & 0x07) << 4;
		o->sr = (b[3] & 0x08) >> 3;
		o->lcdid = (b[3] & 0x70) >> 4;
	}
	// conn.c:60-66: ffs(hpd) - 1 indexes the function table.
	for (bit = 0; bit < 8; bit++)
		if (o->hpd & (1u << bit))
			break;
	if (bit < 8 && bit < sizeof(hpd_map))
		o->hpd_func = hpd_map[bit];
	return 0;
}

// --- GPIO (bios/gpio.c:28-150) ------------------------------------------------

int
pg106_dvb_gpio_match(const struct pg106_vbios_bit *vb, uint8_t func,
		     uint8_t line, struct pg106_dcb_gpio *o)
{
	uint32_t d, e, info;
	uint16_t g, i16;
	uint8_t ver, hdr, cnt, len, info1, i;

	if (!o || (func == 0xff && line == 0xff))
		return -EINVAL;
	if (dcb(vb, &d, &ver, &hdr, &cnt, &len))
		return -ENOENT;
	if (!(ver >= 0x30 && hdr >= 0x0c))
		return -ENOENT;		// (DCB 2.2 paths not needed on GP10x)
	if (pg106_vbios_rd16(vb, d + 0x0a, &g) || !g)
		return -ENOENT;
	if (pg106_vbios_rd08(vb, g, &ver))
		return -EFAULT;
	if (ver < 0x30 || ver > 0x41)
		return -ENOENT;
	if (pg106_vbios_rd08(vb, g + 1, &hdr) || pg106_vbios_rd08(vb, g + 2, &cnt) ||
	    pg106_vbios_rd08(vb, g + 3, &len))
		return -EFAULT;
	for (i = 0; i < cnt; i++) {
		memset(o, 0, sizeof(*o));
		e = g + hdr + (uint32_t)i * len;
		o->entry = e;
		if (ver < 0x40) {
			if (pg106_vbios_rd16(vb, e, &i16))
				return -EFAULT;
			o->line = i16 & 0x1f;
			o->func = (uint8_t)((i16 & 0x07e0) >> 5);
			o->log[0] = (uint8_t)((i16 & 0x1800) >> 11);
			o->log[1] = (uint8_t)((i16 & 0x6000) >> 13);
			o->param = !!(i16 & 0x8000);
		} else if (ver < 0x41) {
			if (pg106_vbios_rd32(vb, e, &info))
				return -EFAULT;
			o->line = info & 0x1f;
			o->func = (uint8_t)((info & 0xff00) >> 8);
			o->log[0] = (uint8_t)((info & 0x18000000) >> 27);
			o->log[1] = (uint8_t)((info & 0x60000000) >> 29);
			o->param = !!(info & 0x80000000u);
		} else {
			if (pg106_vbios_rd32(vb, e, &info) ||
			    pg106_vbios_rd08(vb, e + 4, &info1))
				return -EFAULT;
			o->line = info & 0x3f;
			o->func = (uint8_t)((info & 0xff00) >> 8);
			o->log[0] = (info1 & 0x30) >> 4;
			o->log[1] = (info1 & 0xc0) >> 6;
			o->param = !!(info & 0x80000000u);
		}
		if ((line == 0xff || line == o->line) &&
		    (func == 0xff || func == o->func))
			return 0;
	}
	memset(o, 0, sizeof(*o));
	return -ENOENT;
}

// --- display table / IED (bios/disp.c, engine/disp/nv50.c) -------------------

static uint32_t
disp_table(const struct pg106_vbios_bit *vb, uint8_t *ver, uint8_t *hdr,
	   uint8_t *cnt, uint8_t *len, uint8_t *sub)
{
	struct pg106_vbios_bit_entry U;
	uint16_t data;

	if (pg106_vbios_bit_entry(vb, 'U', &U) || U.version != 1)
		return 0;
	if (pg106_vbios_rd16(vb, U.offset, &data) || !data)
		return 0;
	if (pg106_vbios_rd08(vb, data, ver))
		return 0;
	if (*ver < 0x20 || *ver > 0x22)
		return 0;
	if (pg106_vbios_rd08(vb, data + 1u, hdr) || pg106_vbios_rd08(vb, data + 2u, len) ||
	    pg106_vbios_rd08(vb, data + 3u, cnt) || pg106_vbios_rd08(vb, data + 4u, sub))
		return 0;
	return data;
}

// nvbios_outp_parse: entry idx of the display table -> outp header.
// Returns the outp data offset; *ver stays nonzero while idx < cnt (the
// match loop keeps going over entries that fail to parse).
static uint32_t
outp_parse(const struct pg106_vbios_bit *vb, uint8_t idx, uint8_t *ver,
	   uint8_t *hdr, uint8_t *cnt, uint8_t *len, uint16_t *type,
	   uint32_t *mask, uint16_t script[3])
{
	uint8_t tcnt, tlen, sub;
	uint16_t info;
	uint32_t t = disp_table(vb, ver, hdr, &tcnt, &tlen, &sub);

	if (!t || idx >= tcnt) {
		*ver = 0;
		return 0;
	}
	// nvbios_disp_parse: entry = t + hdr + idx*len, info.data = rd16.
	if (tlen < 2 || pg106_vbios_rd16(vb, t + *hdr + (uint32_t)idx * tlen, &info))
		return 0;
	// nvbios_outp_entry: hdr <- sub, cnt <- rd08(info + 5), len 6.
	*hdr = sub;
	if (pg106_vbios_rd08(vb, info + 5u, cnt))
		return 0;
	*len = 0x06;
	if (*hdr < 0x0a)
		return 0;
	if (pg106_vbios_rd16(vb, info + 0u, type) || pg106_vbios_rd32(vb, info + 2u, mask) ||
	    pg106_vbios_rd16(vb, info + 6u, &script[0]) ||
	    pg106_vbios_rd16(vb, info + 8u, &script[1]))
		return 0;
	if (*ver <= 0x20)
		*mask |= 0x00c0;	// match any link
	script[2] = 0;
	if (*hdr >= 0x0c && pg106_vbios_rd16(vb, info + 0x0au, &script[2]))
		return 0;
	return info;
}

uint32_t
pg106_dvb_iedt(const struct pg106_vbios_bit *vb,
	       const struct pg106_dvb_ied_ctx *c, uint8_t *hdr, uint8_t *cnt,
	       uint8_t *len, uint16_t script[3])
{
	unsigned l = 0;
	uint16_t m, type;
	uint32_t mask, data;
	uint8_t ver, idx = 0;

	while (l < 8 && !(c->link & (1u << l)))
		l++;
	l = l < 8 ? l + 1 : 0;				// ffs(outp->info.link)
	m = (uint16_t)((0x0100u << c->head) | (l << 6) | c->hashm_or);
	for (;;) {
		data = outp_parse(vb, idx++, &ver, hdr, cnt, len, &type, &mask,
				  script);
		if (!data && !ver)
			return 0;
		// nvbios_outp_match compares the u16 info->mask.
		if (data && type == c->hasht && ((uint16_t)mask & m) == m)
			return data;
		if (idx == 0)
			return 0;
	}
}

static uint32_t
ocfg_match(const struct pg106_vbios_bit *vb, uint32_t outp, uint8_t hdr,
	   uint8_t cnt, uint8_t len, uint8_t proto, uint8_t flags,
	   uint16_t clkcmp[2])
{
	for (uint8_t i = 0; i < cnt; i++) {
		uint32_t d = outp + hdr + (uint32_t)i * len;
		uint8_t p;
		uint16_t f;

		if (pg106_vbios_rd08(vb, d, &p) || pg106_vbios_rd16(vb, d + 1, &f) ||
		    pg106_vbios_rd16(vb, d + 2, &clkcmp[0]) ||
		    pg106_vbios_rd16(vb, d + 4, &clkcmp[1]))
			return 0;
		// info->flags is a u8 holding rd16(data + 1) (disp.c:139).
		if ((p == proto || p == 0xff) && (uint8_t)f == flags)
			return d;
	}
	return 0;
}

static uint16_t
oclk_match(const struct pg106_vbios_bit *vb, uint16_t cmp, uint32_t khz)
{
	unsigned guard = 0;

	while (cmp && guard++ < 256) {
		uint16_t f, s;
		if (pg106_vbios_rd16(vb, cmp, &f) || pg106_vbios_rd16(vb, cmp + 2u, &s))
			return 0;
		if (khz / 10 >= f)
			return s;
		cmp = (uint16_t)(cmp + 4);
	}
	return 0;
}

int
pg106_dvb_ied_script(const struct pg106_vbios_bit *vb,
		     const struct pg106_dvb_ied_ctx *c, bool on, int id,
		     uint32_t khz, uint32_t *script, const char **why)
{
	uint8_t hdr, cnt, len, flags = 0;
	uint16_t s[3], clkcmp[2];
	uint32_t data;
	const char *dummy;

	if (!why)
		why = &dummy;
	*script = 0;
	*why = NULL;
	data = pg106_dvb_iedt(vb, c, &hdr, &cnt, &len, s);
	if (!data) {
		*why = "missing IEDT";
		return 0;
	}
	if (!on) {
		if (id < 0 || id > 2)
			return -EINVAL;
		*script = s[id];
		if (!*script)
			*why = "IEDT has no OffInt script";
		return 0;
	}
	if (c->lvds24)
		flags |= 0x02;
	if (c->ior_link == 3)
		flags |= 0x01;
	data = ocfg_match(vb, data, hdr, cnt, len, c->proto_evo, flags, clkcmp);
	if (!data) {
		*why = "missing IEDT RS";
		return 0;
	}
	if (id < 0 || id > 1)
		return -EINVAL;
	*script = oclk_match(vb, clkcmp[id], khz);
	if (!*script)
		*why = "missing IEDT RSS";
	return 0;
}

uint16_t
pg106_dvb_outp_hasht(const struct pg106_dcb_outp *o)
{
	return (uint16_t)((o->extdev << 8) | (o->location << 4) | o->type);
}

uint16_t
pg106_dvb_outp_hashm(const struct pg106_dcb_outp *o)
{
	return (uint16_t)((o->heads << 8) | (o->link << 6) | o->or_mask);
}

// --- PLL limits (bios/pll.c:81-98, 184-205, 352-381) ------------------------

int
pg106_dvb_pll(const struct pg106_vbios_bit *vb, uint8_t type,
	      struct pg106_dvb_pll *o)
{
	struct pg106_vbios_bit_entry c;
	uint32_t data = 0, e, p;
	uint16_t v16;
	uint8_t ver, hdr, len, cnt, t, b;

	memset(o, 0, sizeof(*o));
	if (pg106_vbios_bit_entry(vb, 'C', &c))
		return -ENOENT;
	if (c.version == 1 && c.length >= 10) {
		if (pg106_vbios_rd16(vb, c.offset + 8u, &v16))
			return -ENOENT;
		data = v16;
	} else if (c.version == 2 && c.length >= 4) {
		if (pg106_vbios_rd32(vb, c.offset, &data))
			return -ENOENT;
	}
	if (!data || pg106_vbios_rd08(vb, data + 0, &ver) ||
	    pg106_vbios_rd08(vb, data + 1, &hdr) || pg106_vbios_rd08(vb, data + 2, &len) ||
	    pg106_vbios_rd08(vb, data + 3, &cnt))
		return -ENOENT;
	if (ver != 0x40 && ver != 0x50)
		return -ENOTSUP;
	for (e = data + hdr; cnt--; e += len) {	// pll_map_type
		if (pg106_vbios_rd08(vb, e, &t))
			return -ENOENT;
		if (t == type)
			break;
	}
	if (t != type)
		return -ENOENT;
	o->ver = ver;
	if (ver == 0x40) {
		if (pg106_vbios_rd16(vb, e + 9, &v16))
			return -ENOENT;
		o->refclk_khz = v16 * 1000u;
		if (pg106_vbios_rd16(vb, e + 1, &v16))
			return -ENOENT;
		p = v16;
		if (pg106_vbios_rd16(vb, p + 0, &v16))
			return -ENOENT;
		o->vco_min_khz = v16 * 1000u;
		if (pg106_vbios_rd16(vb, p + 2, &v16))
			return -ENOENT;
		o->vco_max_khz = v16 * 1000u;
		if (pg106_vbios_rd08(vb, p + 12, &b))
			return -ENOENT;
		o->min_p = b;
		if (pg106_vbios_rd08(vb, p + 13, &b))
			return -ENOENT;
		o->max_p = b;
	} else {
		if (pg106_vbios_rd16(vb, e + 1, &v16))
			return -ENOENT;
		o->refclk_khz = v16 * 1000u;
		if (pg106_vbios_rd16(vb, e + 5, &v16))
			return -ENOENT;
		o->vco_min_khz = v16 * 1000u;
		if (pg106_vbios_rd16(vb, e + 7, &v16))
			return -ENOENT;
		o->vco_max_khz = v16 * 1000u;
		if (pg106_vbios_rd08(vb, e + 17, &b))
			return -ENOENT;
		o->min_p = b;
		if (pg106_vbios_rd08(vb, e + 18, &b))
			return -ENOENT;
		o->max_p = b;
	}
	return 0;
}
