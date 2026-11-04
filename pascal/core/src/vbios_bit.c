// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/src/vbios_bit.c — NVIDIA VBIOS BIT-section parser.
//
// See pascal/core/include/pascal/vbios_bit.h for the full provenance
// (nouveau nvkm/subdev/bios/{bit,base,pcir,image,shadow,init}.c, linux
// master, fetched to /tmp/pascal-ref/nouveau/nvkm-subdev-bios/) and the
// offset-chain note: BIT is located by signature scan (base.c:189), not via
// the PCIR chain.

#include <pascal/vbios_bit.h>

#include <errno.h>
#include <string.h>

// --- bounds-checked readers -------------------------------------------------

int
pg106_vbios_rd08(const struct pg106_vbios_bit *vb, uint32_t offset,
		 uint8_t *val)
{
	if (!vb || !vb->data || !val)
		return -EINVAL;
	// nvbios_addr guard (base.c:41-44): offset + size must stay inside
	// the image. We report the failure instead of returning 0.
	if (offset >= vb->size)
		return -EFAULT;
	*val = vb->data[offset];
	return 0;
}

int
pg106_vbios_rd16(const struct pg106_vbios_bit *vb, uint32_t offset,
		 uint16_t *val)
{
	if (!vb || !vb->data || !val)
		return -EINVAL;
	// Order matters: offset + 2 may wrap uint32_t, so test >= size-1
	// (equivalent for size >= 1; size 0 fails the first test).
	if (vb->size < 2 || offset > vb->size - 2)
		return -EFAULT;
	*val = (uint16_t)(vb->data[offset] |
			  (vb->data[offset + 1] << 8));
	return 0;
}

int
pg106_vbios_rd32(const struct pg106_vbios_bit *vb, uint32_t offset,
		 uint32_t *val)
{
	if (!vb || !vb->data || !val)
		return -EINVAL;
	if (vb->size < 4 || offset > vb->size - 4)
		return -EFAULT;
	*val = (uint32_t)vb->data[offset] |
	       ((uint32_t)vb->data[offset + 1] << 8) |
	       ((uint32_t)vb->data[offset + 2] << 16) |
	       ((uint32_t)vb->data[offset + 3] << 24);
	return 0;
}

uint8_t
pg106_vbios_checksum(const uint8_t *data, uint32_t size)
{
	// nouveau nvbios_checksum (base.c:82-88).
	uint8_t sum = 0;
	while (size--)
		sum += *data++;
	return sum;
}

// --- BIT signature scan ------------------------------------------------------

// Mirror of nvbios_findstr (base.c:91-104) for the fixed 5-byte BIT
// signature, with the size/len arithmetic kept signed-safe (nouveau's
// `i <= (int)(size - len)` relies on int promotion; size is uint32_t here).
static uint32_t
find_bit_sig(const uint8_t *data, uint32_t size)
{
	// Explicit bytes (not the string macro) so no NUL terminator is
	// involved.
	static const uint8_t sig[PG106_VBIOS_BIT_SIG_LEN] = {
		0xff, 0xb8, 'B', 'I', 'T',
	};
	uint32_t i, j;

	if (size < PG106_VBIOS_BIT_SIG_LEN)
		return 0;

	for (i = 0; i <= size - PG106_VBIOS_BIT_SIG_LEN; i++) {
		for (j = 0; j < PG106_VBIOS_BIT_SIG_LEN; j++) {
			if (data[i + j] != sig[j])
				break;
		}
		if (j == PG106_VBIOS_BIT_SIG_LEN)
			return i;
	}
	return 0;
}

int
pg106_vbios_bit_parse(const uint8_t *data, uint32_t size,
		      struct pg106_vbios_bit *out)
{
	uint32_t bit_offset, table_end;
	uint8_t entry_size, entry_count;

	if (!data || !out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	if (size == 0)
		return -ENOENT;

	// base.c:189-190: the whole image is scanned for the signature.
	bit_offset = find_bit_sig(data, size);
	if (bit_offset == 0)
		return -ENOENT;

	// bit.c reads bit_offset+9..+12; the header must be fully present.
	if (size - bit_offset < PG106_VBIOS_BIT_HDR_LEN)
		return -EPROTO;

	entry_size = data[bit_offset + 9];	// bit.c:42 walk stride
	entry_count = data[bit_offset + 10];	// bit.c:31 loop count

	// bit_entry reads 6 bytes per entry (bit.c:34-38); a stride smaller
	// than that cannot hold one. nouveau does not check this (it trusts
	// the ROM), but a 0 stride would corrupt any consumer's walk.
	if (entry_size < PG106_VBIOS_BIT_ENTRY_RD_LEN)
		return -EPROTO;

	// The token directory must fit inside the image: every read bit_entry
	// performs (base.c via nvbios_rd08/rd16) goes through the OOB guard,
	// but we validate up front so callers can trust entry_count.
	// entry_size <= 255 and entry_count <= 255, so the product fits u32.
	table_end = bit_offset + PG106_VBIOS_BIT_HDR_LEN +
		    (uint32_t)entry_size * entry_count;
	if (table_end > size)
		return -EPROTO;

	out->data = data;
	out->size = size;
	out->bit_offset = bit_offset;
	out->entry_size = entry_size;
	out->entry_count = entry_count;
	return 0;
}

int
pg106_vbios_bit_entry(const struct pg106_vbios_bit *vb, uint8_t id,
		      struct pg106_vbios_bit_entry *out)
{
	uint32_t entry;
	uint8_t i;

	if (!vb || !out)
		return -EINVAL;

	// bit.c:32-43 — entries start at bit_offset + 12 and advance by the
	// stride byte at bit_offset + 9.
	entry = vb->bit_offset + PG106_VBIOS_BIT_HDR_LEN;
	for (i = 0; i < vb->entry_count; i++, entry += vb->entry_size) {
		// Defensive: pg106_vbios_bit_parse guarantees the directory
		// fits and the stride is >= 6, so this cannot trigger.
		if (entry + PG106_VBIOS_BIT_ENTRY_RD_LEN > vb->size)
			return -EPROTO;

		if (vb->data[entry] != id)	// bit.c:34
			continue;

		// bit.c:35-38: id @+0, version @+1, length u16 @+2,
		// offset u16 @+4 — little-endian on the wire.
		out->id = vb->data[entry + 0];
		out->version = vb->data[entry + 1];
		out->length = (uint16_t)(vb->data[entry + 2] |
					 (vb->data[entry + 3] << 8));
		out->offset = (uint16_t)(vb->data[entry + 4] |
					 (vb->data[entry + 5] << 8));
		return 0;
	}
	return -ENOENT;	// bit.c:45
}

// --- init (devinit) script tables -------------------------------------------

int
pg106_vbios_init_script_table(const struct pg106_vbios_bit *vb,
			      const struct pg106_vbios_bit_entry *bit_I,
			      uint16_t *table_offset)
{
	uint16_t table;
	int ret;

	if (!vb || !bit_I || !table_offset)
		return -EINVAL;
	*table_offset = 0;

	// init_table_ (init.c:382): the pointer field must be within the
	// 'I' table — len >= offset + 2, here offset = 0x00.
	if (bit_I->length < 2)
		return -EPROTO;

	// init_script_table (init.c:399): u16 at bit_I.offset + 0x00.
	ret = pg106_vbios_rd16(vb, bit_I->offset, &table);
	if (ret)
		return ret;
	if (table == 0) {
		// init.c:387 "script table pointer invalid".
		return -ENOENT;
	}

	*table_offset = table;
	return 0;
}

int
pg106_vbios_init_script(const struct pg106_vbios_bit *vb,
			const struct pg106_vbios_bit_entry *bit_I,
			uint16_t index, uint16_t *script_offset)
{
	uint16_t table, script;
	uint32_t entry;
	int ret;

	if (!script_offset)
		return -EINVAL;
	*script_offset = 0;

	ret = pg106_vbios_init_script_table(vb, bit_I, &table);
	if (ret)
		return ret;

	// init_script (init.c:422-424): rd16(script_table + index*2). The
	// pointed-to table has no declared length (nouveau reads it
	// unguarded), so only the image bounds apply.
	entry = (uint32_t)table + (uint32_t)index * 2;
	ret = pg106_vbios_rd16(vb, entry, &script);
	if (ret)
		return -EFAULT;
	// A 0 offset means "no script" to init.c callers — returned as-is.
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

	// base.c:195: the version table is only consulted when length >= 4.
	if (bit_i->length < 4)
		return -EPROTO;

	// base.c:196-200 — byte order: micro, minor, chip, major, patch.
	// Bounds come from pg106_vbios_rd08 (-EFAULT mirrors base.c's OOB
	// guard, which nouveau answers with 0x00 bytes instead).
	out->micro = 0;
	out->minor = 0;
	out->chip = 0;
	out->major = 0;
	out->patch = 0;
	ret = pg106_vbios_rd08(vb, bit_i->offset + 0, &out->micro);
	if (!ret)
		ret = pg106_vbios_rd08(vb, bit_i->offset + 1, &out->minor);
	if (!ret)
		ret = pg106_vbios_rd08(vb, bit_i->offset + 2, &out->chip);
	if (!ret)
		ret = pg106_vbios_rd08(vb, bit_i->offset + 3, &out->major);
	if (!ret && bit_i->length >= 5)
		ret = pg106_vbios_rd08(vb, bit_i->offset + 4, &out->patch);
	return ret;
}

// --- ROM header / PCIR probe -------------------------------------------------

int
pg106_vbios_rom_probe(const uint8_t *data, uint32_t size,
		      struct pg106_vbios_rom *out)
{
	uint32_t image_size;
	uint16_t word;
	uint32_t sig;

	if (!data || !out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	if (size < 2)
		return -ENOENT;

	// image.c:39-48 — 0xaa55 (little-endian first word 0xaa55), 0xbb77,
	// or 'NV' (0x4e56).
	word = (uint16_t)(data[0] | (data[1] << 8));
	switch (word) {
	case 0xaa55:
	case 0xbb77:
	case 0x4e56:
		out->rom_sig_ok = true;
		break;
	default:
		return -ENOENT;
	}

	// pcir.c:30 — u16 at base + 0x18 points at the PCIR struct.
	if (size < 0x1a)
		return 0;	// no room for the pointer: no PCIR
	out->pcir_offset = (uint32_t)data[0x18] | ((uint32_t)data[0x19] << 8);
	if (out->pcir_offset == 0)
		return 0;

	if ((uint64_t)out->pcir_offset + 0x16 > size)
		return -EPROTO;	// pcir.c fields (+0..+0x15) must fit

	// pcir.c:33-39 — signature "PCIR", "RGIS" or "NPDS", else no PCIR.
	sig = (uint32_t)data[out->pcir_offset + 0] |
	      ((uint32_t)data[out->pcir_offset + 1] << 8) |
	      ((uint32_t)data[out->pcir_offset + 2] << 16) |
	      ((uint32_t)data[out->pcir_offset + 3] << 24);
	if (sig != 0x52494350u /* PCIR */ &&
	    sig != 0x53494752u /* RGIS */ &&
	    sig != 0x5344504eu /* NPDS */)
		return 0;

	out->pcir_ok = true;
	out->vendor_id = (uint16_t)(data[out->pcir_offset + 0x04] |
			(data[out->pcir_offset + 0x05] << 8));	// pcir.c:58
	out->device_id = (uint16_t)(data[out->pcir_offset + 0x06] |
			(data[out->pcir_offset + 0x07] << 8));	// pcir.c:59
	out->class_code[0] = data[out->pcir_offset + 0x0d];	// pcir.c:60
	out->class_code[1] = data[out->pcir_offset + 0x0e];	// pcir.c:61
	out->class_code[2] = data[out->pcir_offset + 0x0f];	// pcir.c:62

	// pcir.c:63 — u16 in 512-byte units.
	word = (uint16_t)(data[out->pcir_offset + 0x10] |
			  (data[out->pcir_offset + 0x11] << 8));
	image_size = (uint32_t)word * 512;
	out->image_size = image_size;
	out->image_rev = (uint16_t)(data[out->pcir_offset + 0x12] |
			(data[out->pcir_offset + 0x13] << 8));	// pcir.c:64
	out->image_type = data[out->pcir_offset + 0x14];	// pcir.c:65
	out->last_image = (data[out->pcir_offset + 0x15] & 0x80) != 0;

	// shadow.c:85-97 — nvbios_checksum over the first image; a truncated
	// dump checksums what is present (checksum stays informational: the
	// BIT parser does not require it, matching nvbios_prom flags).
	if (image_size > size)
		image_size = size;
	out->checksum = pg106_vbios_checksum(data, image_size);
	return 0;
}
