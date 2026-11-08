// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/src/vbios_fetch.c — VBIOS image assembly from a chunked source.
// See vbios_fetch.h; line references are nouveau's nvkm/subdev/bios/ at
// torvalds/linux d48723711bae.

#include <pascal/vbios_fetch.h>
#include <pascal/vbios_bit.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct fetch {
	const struct pg106_vbios_src *src;
	struct pg106_vbios_fetch_result *res;
	uint8_t *data;
	uint32_t size; // valid bytes from offset 0 (bios->size)
	uint32_t cap;
	bool ended;    // the source returned short: nothing more will come
	int err;       // source error (negative errno), sticky
};

// nvbios_extend (base.c): grow the buffer, zero-filled, to at least `len`.
static int
extend(struct fetch *f, uint32_t len)
{
	uint32_t ncap;
	uint8_t *n;

	if (len <= f->cap)
		return 0;
	if (len > PG106_VBIOS_FETCH_MAX)
		return -EFBIG;
	ncap = f->cap ? f->cap : 0x10000u;
	while (ncap < len)
		ncap *= 2;
	if (ncap > PG106_VBIOS_FETCH_MAX)
		ncap = PG106_VBIOS_FETCH_MAX;
	n = realloc(f->data, ncap);
	if (!n)
		return -ENOMEM;
	memset(n + f->cap, 0, ncap - f->cap);
	f->data = n;
	f->cap = ncap;
	return 0;
}

// shadow_fetch (shadow.c:38-49) with acpi_read_slow's reads
// (shadowacpi.c:84-102): read whole chunks from the current end until
// `upto` is covered or a read comes back short. Returns whether size >=
// upto, like shadow_fetch; hard errors are kept in f->err.
static bool
fetch_upto(struct fetch *f, uint32_t upto)
{
	const uint32_t chunk = f->src->chunk;
	uint32_t limit = (upto + 3u) & ~3u; // shadow.c:41

	if (chunk)
		limit = (limit + chunk - 1) / chunk * chunk; // shadowacpi.c:87
	while (f->size < upto && !f->ended && !f->err) {
		uint32_t want = limit - f->size;
		int got;

		if (chunk && want > chunk)
			want = chunk;
		f->err = extend(f, f->size + want);
		if (f->err)
			break;
		got = f->src->read(f->src->user, f->size, want,
				   f->data + f->size);
		f->res->reads++;
		if (got < 0) {
			f->err = got;
			break;
		}
		if ((uint32_t)got > want)
			got = (int)want; // min(length, buffer.length), acpi :50
		f->size += (uint32_t)got;
		f->res->fetched += (uint32_t)got;
		if ((uint32_t)got < want) { // shadowacpi.c:95-96
			f->res->short_read = true;
			f->ended = true;
		}
	}
	return f->size >= upto;
}

// shadow_image (shadow.c:51-107), iterative instead of recursive.
static int
walk(struct fetch *f)
{
	struct pg106_vbios_fetch_result *res = f->res;
	uint32_t offset = 0;
	int idx;

	for (idx = 0;; idx++) {
		struct pg106_vbios_image im;
		int score = 1;

		// shadow.c:64-68: header fetch. A source that ends inside
		// the header still counts if the image parses (the last
		// image of the Helios ROM is 0xa00 bytes).
		fetch_upto(f, offset + PG106_VBIOS_HDR_FETCH);
		if (f->err && f->err != -EFBIG)
			return f->err == -ENOMEM ? -ENOMEM : -EIO;
		if (pg106_vbios_image(f->data, f->size, idx, &im) != 0) {
			// shadow.c:70-73. Image 0 missing = no ROM at all;
			// a later one missing ends the chain with what
			// scored so far (nouveau returns 0 for it).
			if (idx == 0)
				return f->size == 0 && f->err ? -EIO : -ENOENT;
			break;
		}
		res->images++;
		if ((uint64_t)im.base + im.size > PG106_VBIOS_FETCH_MAX)
			return -EFBIG;
		// shadow.c:78-81.
		if (!fetch_upto(f, im.base + im.size)) {
			if (f->err == -ENOMEM)
				return -ENOMEM;
			return -EIO;
		}
		// shadow.c:83-102.
		if (im.type == 0x00) {
			bool ok = pg106_vbios_checksum(&f->data[im.base],
						       im.size) == 0;
			if (idx == 0)
				res->checksum_ok = ok;
			if (!ok) {
				if (f->src->require_checksum)
					return -EBADMSG;
				score += 1; // not rw: shadow.c:90-92
			} else {
				score += 3;
			}
		} else {
			score += 3;
		}
		res->score += score;
		res->size = im.base + im.size;
		offset = im.base + im.size;
		if (im.last) // shadow.c:104-105
			break;
	}
	return 0;
}

int
pg106_vbios_fetch(const struct pg106_vbios_src *src,
		  struct pg106_vbios_blob *blob,
		  struct pg106_vbios_fetch_result *res)
{
	struct pg106_vbios_fetch_result local;
	struct fetch f;
	int ret;

	if (!res)
		res = &local;
	memset(res, 0, sizeof(*res));
	if (blob) {
		blob->data = NULL;
		blob->size = 0;
	}
	if (!src || !src->read || !blob) {
		res->ret = -EINVAL;
		res->error = "bad arguments";
		return res->ret;
	}
	memset(&f, 0, sizeof(f));
	f.src = src;
	f.res = res;

	ret = walk(&f);
	if (ret == 0 && res->size == 0)
		ret = -ENOENT;
	if (ret) {
		free(f.data);
		res->ret = ret;
		res->score = 0;
		switch (ret) {
		case -ENOENT: res->error = "image 0 invalid (no ROM signature/PCIR)"; break;
		case -EIO: res->error = "source failed or stopped before an image ended"; break;
		case -EBADMSG: res->error = "image 0 checksum failed (required by this source)"; break;
		case -EFBIG: res->error = "image chain larger than PG106_VBIOS_FETCH_MAX"; break;
		case -ENOMEM: res->error = "out of memory"; break;
		default: res->error = "fetch failed"; break;
		}
		return ret;
	}
	// The chunk-aligned tail past the last image is not part of the ROM
	// (on the Helios _ROM it is zero fill past RVBS); keep the chain.
	blob->data = f.data;
	blob->size = res->size;
	{
		struct pg106_vbios_rom rom;

		if (pg106_vbios_rom_probe(blob->data, blob->size, &rom) == 0 &&
		    rom.pcir_ok) {
			res->pcir_vendor = rom.vendor_id;
			res->pcir_device = rom.device_id;
		}
	}
	res->ret = 0;
	return 0;
}

int
pg106_vbios_fetch_validate(const struct pg106_vbios_blob *blob,
			   uint16_t pci_vendor, uint16_t pci_device,
			   struct pg106_vbios_fetch_result *res)
{
	struct pg106_vbios_rom rom;
	struct pg106_vbios_bit vb;
	int ret;

	if (!blob || !blob->data || !blob->size)
		ret = -EINVAL;
	else if (pg106_vbios_rom_probe(blob->data, blob->size, &rom) != 0 ||
		 !rom.pcir_ok)
		ret = -ENOENT;
	else if (rom.vendor_id != pci_vendor || rom.device_id != pci_device)
		ret = -ENODEV;
	else
		ret = pg106_vbios_bit_parse(blob->data, blob->size, &vb);

	if (res) {
		if (blob && blob->data && ret != -ENOENT && ret != -EINVAL) {
			res->pcir_vendor = rom.vendor_id;
			res->pcir_device = rom.device_id;
		}
		res->ret = ret;
		switch (ret) {
		case 0: res->error = NULL; break;
		case -EINVAL: res->error = "no ROM"; break;
		case -ENOENT: res->error = "image 0 has no PCIR"; break;
		case -ENODEV: res->error = "PCIR vendor/device is not this GPU"; break;
		default: res->error = "BIT table did not parse"; break;
		}
	}
	return ret;
}

int
pg106_vbios_fetch_first(const struct pg106_vbios_src *const *srcs,
			unsigned nsrc, uint16_t pci_vendor,
			uint16_t pci_device, struct pg106_vbios_blob *blob,
			unsigned *which, struct pg106_vbios_fetch_result *res)
{
	int ret = -ENOENT;
	unsigned i;

	if (!srcs || !blob)
		return -EINVAL;
	blob->data = NULL;
	blob->size = 0;
	for (i = 0; i < nsrc; i++) {
		struct pg106_vbios_fetch_result one;
		struct pg106_vbios_fetch_result *r = res ? &res[i] : &one;
		struct pg106_vbios_blob b;

		if (!srcs[i])
			continue;
		ret = pg106_vbios_fetch(srcs[i], &b, r);
		if (ret == 0)
			ret = pg106_vbios_fetch_validate(&b, pci_vendor,
							 pci_device, r);
		if (ret == 0) {
			*blob = b;
			if (which)
				*which = i;
			return 0;
		}
		pg106_vbios_blob_free(&b);
	}
	return ret;
}

void
pg106_vbios_blob_free(struct pg106_vbios_blob *blob)
{
	if (!blob)
		return;
	free(blob->data);
	blob->data = NULL;
	blob->size = 0;
}
