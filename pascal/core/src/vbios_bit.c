// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/src/vbios_bit.c — NVIDIA VBIOS parser (image chain, BIT,
// remap, 'I'/'p' tables, DCB). See vbios_bit.h for provenance; all line
// references are linux master nvkm/subdev/bios/*.c.

#include <pascal/vbios_bit.h>

#include <errno.h>
#include <string.h>

// --- raw (no remap) little-endian readers -----------------------------------

static bool
raw_ok(uint32_t size, uint32_t off, uint32_t n)
{
	return n <= size && off <= size - n;
}

static uint16_t
raw16(const uint8_t *d, uint32_t off)
{
	return (uint16_t)(d[off] | (d[off + 1] << 8));
}

static uint32_t
raw32(const uint8_t *d, uint32_t off)
{
	return (uint32_t)d[off] | ((uint32_t)d[off + 1] << 8) |
	       ((uint32_t)d[off + 2] << 16) | ((uint32_t)d[off + 3] << 24);
}

// --- remap + bounds-checked readers (base.c:31-79) --------------------------

uint32_t
pg106_vbios_addr(const struct pg106_vbios_bit *vb, uint32_t addr)
{
	// base.c:36-39: addresses past image 0 are redirected into the
	// first type-0xe0 image ("some tables have weird pointers",
	// base.c:167-168).
	if (vb && addr >= vb->image0_size && vb->imaged_addr) {
		addr -= vb->image0_size;
		addr += vb->imaged_addr;
	}
	return addr;
}

static int
vb_ptr(const struct pg106_vbios_bit *vb, uint32_t offset, uint32_t n,
       uint32_t *eff)
{
	if (!vb || !vb->data)
		return -EINVAL;
	*eff = pg106_vbios_addr(vb, offset);
	// base.c:41-44 OOB guard (addr + size > bios->size).
	if (!raw_ok(vb->size, *eff, n))
		return -EFAULT;
	return 0;
}

int
pg106_vbios_rd08(const struct pg106_vbios_bit *vb, uint32_t offset,
		 uint8_t *val)
{
	uint32_t e;
	int ret;

	if (!val)
		return -EINVAL;
	if ((ret = vb_ptr(vb, offset, 1, &e)))
		return ret;
	*val = vb->data[e];
	return 0;
}

int
pg106_vbios_rd16(const struct pg106_vbios_bit *vb, uint32_t offset,
		 uint16_t *val)
{
	uint32_t e;
	int ret;

	if (!val)
		return -EINVAL;
	if ((ret = vb_ptr(vb, offset, 2, &e)))
		return ret;
	*val = raw16(vb->data, e);
	return 0;
}

int
pg106_vbios_rd32(const struct pg106_vbios_bit *vb, uint32_t offset,
		 uint32_t *val)
{
	uint32_t e;
	int ret;

	if (!val)
		return -EINVAL;
	if ((ret = vb_ptr(vb, offset, 4, &e)))
		return ret;
	*val = raw32(vb->data, e);
	return 0;
}

uint8_t
pg106_vbios_nv_rd08(const struct pg106_vbios_bit *vb, uint32_t a)
{
	uint8_t v = 0;
	return pg106_vbios_rd08(vb, a, &v) ? 0x00 : v;	// base.c:62
}

uint16_t
pg106_vbios_nv_rd16(const struct pg106_vbios_bit *vb, uint32_t a)
{
	uint16_t v = 0;
	return pg106_vbios_rd16(vb, a, &v) ? 0x0000 : v;	// base.c:70
}

uint32_t
pg106_vbios_nv_rd32(const struct pg106_vbios_bit *vb, uint32_t a)
{
	uint32_t v = 0;
	return pg106_vbios_rd32(vb, a, &v) ? 0 : v;	// base.c:78
}

uint8_t
pg106_vbios_checksum(const uint8_t *data, uint32_t size)
{
	uint8_t sum = 0;	// base.c:82-88
	while (size--)
		sum += *data++;
	return sum;
}

// --- image chain (image.c, pcir.c, npde.c) ---------------------------------
//
// nvbios_image clears imaged_addr while it walks (image.c:71-81), so the
// chain is read RAW here: no remap, OOB reads behave as nouveau's 0.

static uint16_t
img_rd16(const uint8_t *d, uint32_t size, uint32_t off)
{
	return raw_ok(size, off, 2) ? raw16(d, off) : 0;
}

static uint32_t
img_rd32(const uint8_t *d, uint32_t size, uint32_t off)
{
	return raw_ok(size, off, 4) ? raw32(d, off) : 0;
}

static uint8_t
img_rd08(const uint8_t *d, uint32_t size, uint32_t off)
{
	return off < size ? d[off] : 0;
}

// nvbios_imagen (image.c:29-66). Returns false where nouveau does.
static bool
imagen(const uint8_t *d, uint32_t size, struct pg106_vbios_image *im)
{
	uint32_t pcir, npde;
	uint16_t hdr;

	switch (img_rd16(d, size, im->base + 0x00)) {	// image.c:39-47
	case 0xaa55:
	case 0xbb77:
	case 0x4e56:
		break;
	default:
		return false;
	}

	// nvbios_pcirTe (pcir.c:27-50): pointer is image-relative.
	pcir = img_rd16(d, size, im->base + 0x18);
	if (!pcir)
		return false;
	pcir += im->base;
	switch (img_rd32(d, size, pcir)) {
	case 0x52494350u: /* PCIR */
	case 0x53494752u: /* RGIS */
	case 0x5344504eu: /* NPDS */
		break;
	default:
		return false;
	}
	hdr = img_rd16(d, size, pcir + 0x0a);
	im->pcir = pcir;
	im->pcir_size = (uint32_t)img_rd16(d, size, pcir + 0x10) * 512;
	im->size = im->pcir_size;			// image.c:52
	im->type = img_rd08(d, size, pcir + 0x14);	// image.c:53
	im->last = (img_rd08(d, size, pcir + 0x15) & 0x80) != 0;
	im->npde = false;

	if (im->type != PG106_VBIOS_IMAGE_TYPE_EFI) {	// image.c:56-60
		// nvbios_npdeTe (npde.c:28-47): 16-aligned after the PCIR.
		npde = (pcir + hdr + 0x0f) & ~0x0fu;
		if (npde && img_rd32(d, size, npde) == 0x4544504eu /* NPDE */) {
			im->size = (uint32_t)img_rd16(d, size, npde + 0x08) *
				   512;				// npde.c:55
			im->last = (img_rd08(d, size, npde + 0x0a) & 0x80) != 0;
			im->npde = true;
		}
	} else {
		im->last = true;				// image.c:62
	}
	return true;
}

int
pg106_vbios_image(const uint8_t *data, uint32_t size, int idx,
		  struct pg106_vbios_image *out)
{
	if (!data || !out || idx < 0)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	// image.c:74-80: base += size until idx images are skipped.
	// SIM DEVIATION: a size-0 image (PCIR/NPDE length 0) would make
	// nouveau's walk revisit the same image forever (nvkm_bios_new's
	// `while (nvbios_image(bios, idx++, &image))`); it ends the chain
	// here, and the chain is capped at 32 images.
	if (idx >= 32)
		return -ENOENT;
	do {
		out->base += out->size;
		if (out->last || out->base >= size || !imagen(data, size, out) ||
		    out->size == 0) {
			if (out->size == 0 && idx == 0 && out->base < size &&
			    out->pcir)
				return 0; // the image itself exists
			return -ENOENT;
		}
	} while (idx--);
	return 0;
}

// --- BIT ---------------------------------------------------------------------

static uint32_t
find_bit_sig(const uint8_t *data, uint32_t size)
{
	static const uint8_t sig[PG106_VBIOS_BIT_SIG_LEN] = {
		0xff, 0xb8, 'B', 'I', 'T',
	};
	uint32_t i;

	if (size < PG106_VBIOS_BIT_SIG_LEN)
		return 0;
	for (i = 0; i <= size - PG106_VBIOS_BIT_SIG_LEN; i++)
		if (!memcmp(&data[i], sig, sizeof(sig)))
			return i;
	return 0;
}

int
pg106_vbios_bit_parse(const uint8_t *data, uint32_t size,
		      struct pg106_vbios_bit *out)
{
	struct pg106_vbios_image im;
	uint32_t bit_offset, table_end;
	int idx = 0;

	if (!data || !out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	if (size == 0)
		return -ENOENT;

	// nvkm_bios_new image walk (base.c:170-178), before the BIT scan.
	if (pg106_vbios_image(data, size, idx++, &im) == 0) {
		out->image0_size = im.size;
		out->image_count = 1;
		while (pg106_vbios_image(data, size, idx++, &im) == 0) {
			out->image_count++;
			if (im.type == PG106_VBIOS_IMAGE_TYPE_IMAGED) {
				out->imaged_addr = im.base;
				break;
			}
		}
	}

	bit_offset = find_bit_sig(data, size);		// base.c:189
	if (bit_offset == 0)
		return -ENOENT;
	if (size - bit_offset < PG106_VBIOS_BIT_HDR_LEN)
		return -EPROTO;
	out->entry_size = data[bit_offset + 9];		// bit.c:42
	out->entry_count = data[bit_offset + 10];	// bit.c:31
	if (out->entry_size < PG106_VBIOS_BIT_ENTRY_RD_LEN)
		return -EPROTO;
	table_end = bit_offset + PG106_VBIOS_BIT_HDR_LEN +
		    (uint32_t)out->entry_size * out->entry_count;
	if (table_end > size)
		return -EPROTO;

	out->data = data;
	out->size = size;
	out->bit_offset = bit_offset;
	return 0;
}

int
pg106_vbios_bit_entry(const struct pg106_vbios_bit *vb, uint8_t id,
		      struct pg106_vbios_bit_entry *out)
{
	uint32_t entry;
	uint8_t i;

	if (!vb || !out || !vb->data)
		return -EINVAL;
	// bit.c:31-43. The directory itself lives in image 0, below any
	// remap boundary, so raw reads are exact.
	entry = vb->bit_offset + PG106_VBIOS_BIT_HDR_LEN;
	for (i = 0; i < vb->entry_count; i++, entry += vb->entry_size) {
		if (!raw_ok(vb->size, entry, PG106_VBIOS_BIT_ENTRY_RD_LEN))
			return -EPROTO;
		if (vb->data[entry] != id)
			continue;
		out->id = vb->data[entry + 0];
		out->version = vb->data[entry + 1];
		out->length = raw16(vb->data, entry + 2);
		out->offset = raw16(vb->data, entry + 4);
		return 0;
	}
	return -ENOENT;
}

// --- 'I' init tables (init.c:358-427) --------------------------------------

int
pg106_vbios_init_table(const struct pg106_vbios_bit *vb,
		       const struct pg106_vbios_bit_entry *bit_I,
		       uint16_t field, uint32_t *ptr)
{
	uint16_t v;
	int ret;

	if (!vb || !bit_I || !ptr)
		return -EINVAL;
	*ptr = 0;
	if (bit_I->length < (uint32_t)field + 2)	// init.c:382
		return -EPROTO;
	if ((ret = pg106_vbios_rd16(vb, (uint32_t)bit_I->offset + field, &v)))
		return ret;
	if (!v)						// init.c:386-388
		return -ENOENT;
	*ptr = v;
	return 0;
}

int
pg106_vbios_init_script_table(const struct pg106_vbios_bit *vb,
			      const struct pg106_vbios_bit_entry *bit_I,
			      uint32_t *table_offset)
{
	return pg106_vbios_init_table(vb, bit_I, 0x00, table_offset);
}

int
pg106_vbios_init_script(const struct pg106_vbios_bit *vb,
			const struct pg106_vbios_bit_entry *bit_I,
			uint16_t index, uint32_t *script_offset)
{
	uint32_t table;
	uint16_t script;
	int ret;

	if (!script_offset)
		return -EINVAL;
	*script_offset = 0;
	if ((ret = pg106_vbios_init_script_table(vb, bit_I, &table)))
		return ret;
	// init.c:422-424 (u16 script offsets).
	if (pg106_vbios_rd16(vb, table + (uint32_t)index * 2, &script))
		return -EFAULT;
	*script_offset = script;
	return 0;
}

int
pg106_vbios_version(const struct pg106_vbios_bit *vb,
		    const struct pg106_vbios_bit_entry *bit_i,
		    struct pg106_vbios_version *out)
{
	int ret;

	if (!vb || !bit_i || !out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	if (bit_i->length < 4)				// base.c:195
		return -EPROTO;
	ret = pg106_vbios_rd08(vb, bit_i->offset + 0u, &out->micro);
	if (!ret)
		ret = pg106_vbios_rd08(vb, bit_i->offset + 1u, &out->minor);
	if (!ret)
		ret = pg106_vbios_rd08(vb, bit_i->offset + 2u, &out->chip);
	if (!ret)
		ret = pg106_vbios_rd08(vb, bit_i->offset + 3u, &out->major);
	// base.c:200 reads +4 unconditionally; a 4-byte table leaves 0.
	if (!ret && bit_i->length >= 5)
		ret = pg106_vbios_rd08(vb, bit_i->offset + 4u, &out->patch);
	return ret;
}

// --- ROM probe ---------------------------------------------------------------

int
pg106_vbios_rom_probe(const uint8_t *data, uint32_t size,
		      struct pg106_vbios_rom *out)
{
	struct pg106_vbios_image im;
	uint32_t p, sig;
	uint16_t word;

	if (!data || !out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	if (size < 2)
		return -ENOENT;

	word = raw16(data, 0);				// image.c:39-43
	if (word != 0xaa55 && word != 0xbb77 && word != 0x4e56)
		return -ENOENT;
	out->rom_sig_ok = true;

	if (size < 0x1a)
		return 0;
	p = raw16(data, 0x18);				// pcir.c:30
	out->pcir_offset = p;
	if (!p)
		return 0;
	if ((uint64_t)p + 0x16 > size)
		return -EPROTO;
	sig = raw32(data, p);
	if (sig != 0x52494350u && sig != 0x53494752u && sig != 0x5344504eu)
		return 0;

	out->pcir_ok = true;
	out->vendor_id = raw16(data, p + 0x04);		// pcir.c:58-64
	out->device_id = raw16(data, p + 0x06);
	out->class_code[0] = data[p + 0x0d];
	out->class_code[1] = data[p + 0x0e];
	out->class_code[2] = data[p + 0x0f];
	out->pcir_image_size = (uint32_t)raw16(data, p + 0x10) * 512;
	out->image_rev = raw16(data, p + 0x12);
	out->image_type = data[p + 0x14];
	out->last_image = (data[p + 0x15] & 0x80) != 0;
	out->image_size = out->pcir_image_size;

	// image.c:56-60: an NPDE overrides size/last for non-EFI images.
	if (pg106_vbios_image(data, size, 0, &im) == 0 && im.npde) {
		out->npde_ok = true;
		out->image_size = im.size;
		out->last_image = im.last;
	}

	// shadow.c:86 checksums image.size (the NPDE-corrected size).
	out->checksum = pg106_vbios_checksum(data, out->image_size > size ?
					     size : out->image_size);
	return 0;
}

// --- BIT 'p' PMU table (pmu.c:29-102) ---------------------------------------

int
pg106_vbios_pmu_table(const struct pg106_vbios_bit *vb, uint32_t *table,
		      uint8_t *ver, uint8_t *hdr, uint8_t *cnt, uint8_t *len)
{
	struct pg106_vbios_bit_entry bit_p;
	uint32_t data = 0;
	int ret;

	if (!vb || !table || !ver || !hdr || !cnt || !len)
		return -EINVAL;
	*table = 0;
	if (pg106_vbios_bit_entry(vb, 'p', &bit_p))
		return -ENOENT;
	if (bit_p.version != 2 || bit_p.length < 4)	// pmu.c:36
		return -ENOENT;
	if ((ret = pg106_vbios_rd32(vb, bit_p.offset, &data)))
		return ret;
	if (!data)
		return -ENOENT;
	*table = data;
	ret = pg106_vbios_rd08(vb, data + 0, ver);	// pmu.c:39-42
	if (!ret)
		ret = pg106_vbios_rd08(vb, data + 1, hdr);
	if (!ret)
		ret = pg106_vbios_rd08(vb, data + 2, len);
	if (!ret)
		ret = pg106_vbios_rd08(vb, data + 3, cnt);
	return ret;
}

int
pg106_vbios_pmu_app(const struct pg106_vbios_bit *vb, uint8_t type,
		    struct pg106_vbios_pmu_app *app)
{
	uint32_t table, e, d, w[12];
	uint8_t ver, hdr, cnt, len, idx, etype;
	int ret;

	if (!app)
		return -EINVAL;
	memset(app, 0, sizeof(*app));
	if ((ret = pg106_vbios_pmu_table(vb, &table, &ver, &hdr, &cnt, &len)))
		return ret;

	for (idx = 0; idx < cnt; idx++) {		// pmu.c:81 (pmuEe idx<cnt)
		e = table + hdr + (uint32_t)idx * len;	// pmu.c:55
		if ((ret = pg106_vbios_rd08(vb, e + 0x00, &etype)))
			return ret;
		if ((ret = pg106_vbios_rd32(vb, e + 0x02, &d)))
			return ret;
		if (etype != type || !d)		// pmu.c:82
			continue;
		for (unsigned i = 0; i < 12; i++)
			if ((ret = pg106_vbios_rd32(vb, d + 4 * i, &w[i])))
				return ret;
		app->type = etype;
		app->desc = d;
		// pmu.c:83-97, field for field.
		app->init_addr_pmu = w[0x08 / 4];
		app->args_addr_pmu = w[0x0c / 4];
		app->boot_addr = d + 0x30;
		app->boot_addr_pmu = w[0x10 / 4] + w[0x18 / 4];
		app->boot_size = w[0x1c / 4] - w[0x18 / 4];
		app->code_addr = app->boot_addr + app->boot_size;
		app->code_addr_pmu = app->boot_addr_pmu + app->boot_size;
		app->code_size = w[0x20 / 4];
		app->data_addr = d + 0x30 + w[0x24 / 4];
		app->data_addr_pmu = w[0x28 / 4];
		app->data_size = w[0x2c / 4];
		return 0;
	}
	return -ENOENT;
}

// --- DCB (dcb.c, conn.c, gpio.c) --------------------------------------------

int
pg106_vbios_dcb_table(const struct pg106_vbios_bit *vb, uint32_t *dcb,
		      uint8_t *ver, uint8_t *hdr, uint8_t *cnt, uint8_t *len)
{
	uint16_t p;
	uint32_t sig;

	if (!vb || !dcb || !ver || !hdr || !cnt || !len)
		return -EINVAL;
	*dcb = 0;
	if (pg106_vbios_rd16(vb, 0x36, &p) || !p)	// dcb.c:34
		return -ENOENT;
	if (pg106_vbios_rd08(vb, p, ver))
		return -EFAULT;
	if (*ver >= 0x42 || *ver < 0x30)		// dcb.c:43-46 (+ pre-3.0
		return -ENOENT;				// layouts: not GP10x)
	if (pg106_vbios_rd32(vb, p + 6u, &sig) || sig != 0x4edcbdcbu)
		return -ENOENT;				// dcb.c:48
	if (pg106_vbios_rd08(vb, p + 1u, hdr) ||
	    pg106_vbios_rd08(vb, p + 2u, cnt) ||
	    pg106_vbios_rd08(vb, p + 3u, len))
		return -EFAULT;
	*dcb = p;
	return 0;
}

int
pg106_vbios_dcb_outp(const struct pg106_vbios_bit *vb, uint8_t idx,
		     struct pg106_dcb_outp *o)
{
	uint32_t dcb, e;
	uint8_t ver, hdr, cnt, len;
	int ret;

	if (!o)
		return -EINVAL;
	memset(o, 0, sizeof(*o));
	if ((ret = pg106_vbios_dcb_table(vb, &dcb, &ver, &hdr, &cnt, &len)))
		return ret;
	if (idx >= cnt)					// dcb.c:105
		return -ENOENT;
	e = dcb + hdr + (uint32_t)idx * len;
	if (pg106_vbios_rd32(vb, e, &o->conn_raw) ||
	    pg106_vbios_rd32(vb, e + 4, &o->conf_raw))
		return -EFAULT;
	o->index = idx;
	o->entry = e;
	// dcb.c:128-136.
	o->or_mask   = (o->conn_raw & 0x0f000000) >> 24;
	o->location  = (o->conn_raw & 0x00300000) >> 20;
	o->bus       = (o->conn_raw & 0x000f0000) >> 16;
	o->connector = (o->conn_raw & 0x0000f000) >> 12;
	o->heads     = (o->conn_raw & 0x00000f00) >> 8;
	o->i2c_index = (o->conn_raw & 0x000000f0) >> 4;
	o->type      = (o->conn_raw & 0x0000000f);
	o->link      = 0;
	if (ver >= 0x40) {				// dcb.c:141-184
		uint32_t conf = o->conf_raw;
		switch (o->type) {
		case PG106_DCB_OUTPUT_DP:
			switch (conf & 0x00e00000) {
			case 0x00000000: o->dp_link_bw = 0x06; break;
			case 0x00200000: o->dp_link_bw = 0x0a; break;
			case 0x00400000: o->dp_link_bw = 0x14; break;
			default:         o->dp_link_bw = 0x1e; break;
			}
			switch ((conf & 0x0f000000) >> 24) {
			case 0xf: case 0x4: o->dp_link_nr = 4; break;
			case 0x3: case 0x2: o->dp_link_nr = 2; break;
			default:            o->dp_link_nr = 1; break;
			}
			/* fallthrough */
		case PG106_DCB_OUTPUT_TMDS:
		case PG106_DCB_OUTPUT_LVDS:
			o->link = (conf & 0x00000030) >> 4;
			o->sorconf_link = o->link;
			o->extdev = 0x00;
			if (o->location != 0)
				o->extdev = (conf & 0x0000ff00) >> 8;
			break;
		default:
			break;
		}
	}
	// dcb.c:110-120 hash helpers.
	o->hasht = (uint16_t)((o->extdev << 8) | (o->location << 4) | o->type);
	o->hashm = (uint16_t)((o->heads << 8) | (o->link << 6) | o->or_mask);
	return 0;
}

int
pg106_vbios_conn_type(const struct pg106_vbios_bit *vb, uint8_t idx,
		      uint8_t *type)
{
	uint32_t dcb;
	uint16_t t;
	uint8_t ver, hdr, cnt, len;

	if (!type)
		return -EINVAL;
	*type = 0xff;
	if (pg106_vbios_dcb_table(vb, &dcb, &ver, &hdr, &cnt, &len))
		return -ENOENT;
	if (ver < 0x30 || hdr < 0x16)			// conn.c:32
		return -ENOENT;
	if (pg106_vbios_rd16(vb, dcb + 0x14, &t) || !t)
		return -ENOENT;
	if (pg106_vbios_rd08(vb, t + 0u, &ver) || pg106_vbios_rd08(vb, t + 1u, &hdr) ||
	    pg106_vbios_rd08(vb, t + 2u, &cnt) || pg106_vbios_rd08(vb, t + 3u, &len))
		return -EFAULT;
	if (ver != 0x30 && ver != 0x40)			// conn.c:78-80
		return -ENOENT;
	if (idx >= cnt)					// conn.c:62
		return -ENOENT;
	return pg106_vbios_rd08(vb, t + hdr + (uint32_t)idx * len, type) ?
		-EFAULT : 0;
}

int
pg106_vbios_gpio_entry(const struct pg106_vbios_bit *vb, uint8_t ent,
		       uint8_t *func, uint8_t *line)
{
	uint32_t dcb, e, info;
	uint16_t g;
	uint8_t ver, hdr, cnt, len;

	if (!func || !line)
		return -EINVAL;
	if (pg106_vbios_dcb_table(vb, &dcb, &ver, &hdr, &cnt, &len))
		return -ENOENT;
	if (!(ver >= 0x30 && hdr >= 0x0c))		// gpio.c:35
		return -ENOENT;
	if (pg106_vbios_rd16(vb, dcb + 0x0a, &g) || !g)
		return -ENOENT;
	if (pg106_vbios_rd08(vb, g, &ver))
		return -EFAULT;
	if (ver < 0x30 || ver > 0x41)			// gpio.c:42-53
		return -ENOENT;
	if (pg106_vbios_rd08(vb, g + 1u, &hdr) || pg106_vbios_rd08(vb, g + 2u, &cnt) ||
	    pg106_vbios_rd08(vb, g + 3u, &len))
		return -EFAULT;
	if (ent >= cnt)					// gpio.c:69
		return -ENOENT;
	e = g + hdr + (uint32_t)ent * len;
	if (ver < 0x40) {				// gpio.c:81-89
		uint16_t i16;
		if (pg106_vbios_rd16(vb, e, &i16))
			return -EFAULT;
		*line = i16 & 0x001f;
		*func = (uint8_t)((i16 & 0x07e0) >> 5);
	} else {					// gpio.c:90-108
		if (pg106_vbios_rd32(vb, e, &info))
			return -EFAULT;
		*line = (uint8_t)(info & (ver < 0x41 ? 0x1f : 0x3f));
		*func = (uint8_t)((info & 0x0000ff00) >> 8);
	}
	return 0;
}
