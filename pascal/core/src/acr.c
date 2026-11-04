// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/src/acr.c — ACR blob parsing and r352 WPR/LSB header
// arithmetic, derived from nouveau linux master (reference copies under
// /tmp/pascal-ref/):
//   nvkm/falcon/fw.c      nvkm_falcon_fw_ctor_hs (blob walk + sig patch sites)
//   nvkm/subdev/acr/gp102.c  gp102_acr_wpr_{parse,layout,build,build_lsb}
//   nvkm/subdev/acr/gm200.c  gm200_acr_wpr_build_lsb_tail,
//                            gm200_acr_hsfw_load_bld
//   include/nvfw/{fw,hs,acr,flcn}.h  wire structs
//
// Design rules (see acr.h): every read goes through rd*/wr* byte helpers
// after an explicit bounds check, so no struct is ever cast over the input
// and truncated input returns -EINVAL rather than faulting.

#include <pascal/acr.h>

#include <errno.h>
#include <string.h>

// ALIGN from nouveau's os.h (kernel ALIGN macro): round x up to a power of
// two. gp102.c uses ALIGN(...,256)/(...,4096) throughout the WPR layout.
// Takes u64 so intermediate sums cannot wrap before the final overflow check.
static uint64_t
align_u64(uint64_t x, uint32_t a)
{
	return (x + a - 1u) & ~((uint64_t)a - 1u);
}

// Bounds check: [off, off+len) must lie inside a buffer of size `size`.
// Written as subtraction from the tail so it cannot overflow.
static bool
in_range(uint64_t off, uint64_t len, size_t size)
{
	return off <= (uint64_t)size && len <= (uint64_t)size - off;
}

// Little-endian readers/writers. Shifting u8 up through uint32_t avoids
// both signed-shift and aliasing UB regardless of host endianness.
static uint32_t
rd32(const uint8_t *b, size_t off)
{
	return (uint32_t)b[off] |
	       ((uint32_t)b[off + 1] << 8) |
	       ((uint32_t)b[off + 2] << 16) |
	       ((uint32_t)b[off + 3] << 24);
}

static uint64_t
rd64(const uint8_t *b, size_t off)
{
	return (uint64_t)rd32(b, off) | ((uint64_t)rd32(b, off + 4) << 32);
}

static void
wr32(uint8_t *b, size_t off, uint32_t v)
{
	b[off] = (uint8_t)v;
	b[off + 1] = (uint8_t)(v >> 8);
	b[off + 2] = (uint8_t)(v >> 16);
	b[off + 3] = (uint8_t)(v >> 24);
}

static void
wr64(uint8_t *b, size_t off, uint64_t v)
{
	wr32(b, off, (uint32_t)v);
	wr32(b, off + 4, (uint32_t)(v >> 32));
}

// Wire sizes that must not silently drift: these structs are all u32/u8
// aggregates with natural alignment equal to 4, so the C layout equals the
// wire layout on any sane ABI. The parsers never rely on that (they walk
// bytes), but the static asserts document the on-disk sizes cited in acr.h.
_Static_assert(sizeof(struct pg106_flcn_bin_hdr) == 24, "nvfw_bin_hdr");
_Static_assert(sizeof(struct pg106_flcn_bl_desc) == 24, "nvfw_bl_desc");
_Static_assert(sizeof(struct pg106_flcn_hs_header) == 32, "nvfw_hs_header");
_Static_assert(sizeof(struct pg106_acr_wpr_header_v1) == 24,
	       "wpr_header_v1");
_Static_assert(sizeof(struct pg106_acr_lsf_signature_v1) == 192,
	       "lsf_signature_v1");
_Static_assert(sizeof(struct pg106_acr_lsb_header_tail) == 48,
	       "lsb_header_tail");
_Static_assert(sizeof(struct pg106_acr_lsb_header_v1) == 240,
	       "lsb_header_v1");

// Parse the container header every bl/ucode blob starts with
// (fw.c:231 `hdr = nvfw_bin_hdr(subdev, blob->data)`).
static int
bin_hdr_parse(const uint8_t *blob, size_t size, struct pg106_flcn_bin_hdr *out)
{
	if (!in_range(0, sizeof(*out), size))
		return -EINVAL;

	out->bin_magic = rd32(blob, 0);
	out->bin_ver = rd32(blob, 4);
	out->bin_size = rd32(blob, 8);
	out->header_offset = rd32(blob, 12);
	out->data_offset = rd32(blob, 16);
	out->data_size = rd32(blob, 20);

	if (out->bin_magic != PG106_FLCN_BIN_MAGIC_STD &&
	    out->bin_magic != PG106_FLCN_BIN_MAGIC_LE)
		return -EINVAL;
	// The container cannot claim to be larger than the buffer it is in
	// (nouveau trusts bin_size; we cross-check it against the file size).
	if (!in_range(out->bin_size, 0, size))
		return -EINVAL;
	if (!in_range(out->header_offset, 24, size) ||
	    !in_range(out->data_offset, out->data_size, size))
		return -EINVAL;
	return 0;
}

int
pg106_acr_ucode_parse_bl(const void *blob_, size_t size,
			 struct pg106_acr_bl_ucode *out)
{
	const uint8_t *blob = blob_;
	int ret;

	if (!blob || !out)
		return -EINVAL;

	*out = (struct pg106_acr_bl_ucode){0};

	ret = bin_hdr_parse(blob, size, &out->hdr);
	if (ret)
		return ret;

	// fw.c:287 `desc = nvfw_bl_desc(subdev, blob->data + hdr->header_offset)`
	if (!in_range(out->hdr.header_offset, sizeof(out->desc), size))
		return -EINVAL;
	out->desc.start_tag = rd32(blob, out->hdr.header_offset + 0);
	out->desc.dmem_load_off = rd32(blob, out->hdr.header_offset + 4);
	out->desc.code_off = rd32(blob, out->hdr.header_offset + 8);
	out->desc.code_size = rd32(blob, out->hdr.header_offset + 12);
	out->desc.data_off = rd32(blob, out->hdr.header_offset + 16);
	out->desc.data_size = rd32(blob, out->hdr.header_offset + 20);

	// fw.c:291-292 kmemdup(blob->data + data_offset + code_off, code_size)
	if (!in_range(out->hdr.data_offset + (uint64_t)out->desc.code_off,
		      out->desc.code_size, size))
		return -EINVAL;

	out->boot_code = blob + out->hdr.data_offset + out->desc.code_off;
	out->boot_size = out->desc.code_size;
	// fw.c:289 `fw->boot_addr = desc->start_tag << 8` (IMEM tag -> byte addr).
	out->boot_addr = (uint64_t)out->desc.start_tag << 8;
	out->dmem_load_off = out->desc.dmem_load_off;
	return 0;
}

int
pg106_acr_ucode_parse_hs(const void *blob_, size_t size,
			 struct pg106_acr_hs_ucode *out)
{
	const uint8_t *blob = blob_;
	uint64_t loc, sig;
	uint32_t i;
	int ret;

	if (!blob || !out)
		return -EINVAL;

	*out = (struct pg106_acr_hs_ucode){0};

	ret = bin_hdr_parse(blob, size, &out->hdr);
	if (ret)
		return ret;

	// fw.c:232 `hshdr = nvfw_hs_header(subdev, blob->data + header_offset)`
	if (!in_range(out->hdr.header_offset, sizeof(out->hs), size))
		return -EINVAL;
	out->hs.sig_dbg_offset = rd32(blob, out->hdr.header_offset + 0);
	out->hs.sig_dbg_size = rd32(blob, out->hdr.header_offset + 4);
	out->hs.sig_prod_offset = rd32(blob, out->hdr.header_offset + 8);
	out->hs.sig_prod_size = rd32(blob, out->hdr.header_offset + 12);
	out->hs.patch_loc = rd32(blob, out->hdr.header_offset + 16);
	out->hs.patch_sig = rd32(blob, out->hdr.header_offset + 20);
	out->hs.hdr_offset = rd32(blob, out->hdr.header_offset + 24);
	out->hs.hdr_size = rd32(blob, out->hdr.header_offset + 28);

	// fw.c:243-256: with the standard 0x000010de magic patch_loc/patch_sig
	// are blob-absolute offsets of u32s holding the real values; with
	// 0x3b1d14f0 they are the values themselves. Both end up relative to
	// the ucode image (loc) and the signature pool (sig).
	if (out->hdr.bin_magic == PG106_FLCN_BIN_MAGIC_STD) {
		if (!in_range(out->hs.patch_loc, 4, size) ||
		    !in_range(out->hs.patch_sig, 4, size))
			return -EINVAL;
		loc = rd32(blob, out->hs.patch_loc);
		sig = rd32(blob, out->hs.patch_sig);
	} else {
		loc = out->hs.patch_loc;
		sig = out->hs.patch_sig;
	}

	// fw.c:258-260 nvkm_falcon_fw_sign(..., sig_prod_size, blob->data,
	// 1, sig_prod_offset + sig, 1, sig_dbg_offset + sig): the signature
	// pool is the whole blob, the offsets are blob-absolute.
	if (!in_range(out->hs.sig_prod_offset + sig, out->hs.sig_prod_size, size))
		return -EINVAL;
	if (!in_range(out->hs.sig_dbg_offset + sig, out->hs.sig_dbg_size, size))
		return -EINVAL;

	// fw.c:235 nvkm_falcon_fw_ctor(..., blob->data + data_offset,
	// data_size, ...) — the image the falcon will DMA from.
	out->img = blob + out->hdr.data_offset;
	out->img_size = out->hdr.data_size;

	// Stricter than nouveau: the sig patch site must land inside the
	// image (fw.c:58 writes fw->fw.img[dst..dst+sig_size), unchecked
	// upstream — an overflow would corrupt memory past the image).
	if (loc > out->img_size ||
	    !in_range(loc, out->hs.sig_prod_size, out->img_size))
		return -EINVAL;
	out->sig_base_img = (uint32_t)loc;
	out->sig_size = out->hs.sig_prod_size;
	out->sig_prod_off = out->hs.sig_prod_offset + (uint32_t)sig;
	out->sig_dbg_off = out->hs.sig_dbg_offset + (uint32_t)sig;

	// fw.c:264 `lhdr = nvfw_hs_load_header(subdev, blob->data + hshdr->hdr_offset)`
	if (!in_range(out->hs.hdr_offset, 20, size))
		return -EINVAL;
	out->load.non_sec_code_off = rd32(blob, out->hs.hdr_offset + 0);
	out->load.non_sec_code_size = rd32(blob, out->hs.hdr_offset + 4);
	out->load.data_dma_base = rd32(blob, out->hs.hdr_offset + 8);
	out->load.data_size = rd32(blob, out->hs.hdr_offset + 12);
	out->load.num_apps = rd32(blob, out->hs.hdr_offset + 16);
	if (out->load.num_apps == 0 || out->load.num_apps > PG106_ACR_HS_MAX_APPS)
		return -EINVAL;
	if (!in_range(out->hs.hdr_offset + 20,
		      2ull * out->load.num_apps * 4, size))
		return -EINVAL;
	for (i = 0; i < 2u * out->load.num_apps; i++)
		out->load.apps[i] = rd32(blob, out->hs.hdr_offset + 20 + 4 * i);

	// fw.c:266-277: non-secure section, secure section (apps[0] / its
	// size at apps[num_apps]), data segment, signed-area delta.
	out->nmem_base = out->load.non_sec_code_off;
	out->nmem_size = out->load.non_sec_code_size;
	out->imem_base_img = out->load.apps[0];
	out->imem_base = (uint32_t)align_u64(out->load.apps[0], 0x100);
	out->imem_size = out->load.apps[out->load.num_apps];
	out->dmem_base_img = out->load.data_dma_base;
	out->dmem_size = out->load.data_size;
	if (loc < out->dmem_base_img)
		return -EINVAL;
	out->dmem_sign = (uint32_t)(loc - out->dmem_base_img);

	// The DMEM data segment must fit in the image (gp102.c:199 casts
	// &img[dmem_base_img] to flcn_acr_desc_v1, so this is what makes
	// pg106_acr_desc_v1_parse safe on the parsed view).
	if (!in_range(out->dmem_base_img, out->dmem_size, out->img_size))
		return -EINVAL;
	return 0;
}

int
pg106_acr_lsf_sig_parse(const void *data_, size_t size,
			struct pg106_acr_lsf_signature_v1 *out)
{
	const uint8_t *d = data_;

	if (!d || !out || size != PG106_ACR_LSF_SIG_V1_SIZE)
		return -EINVAL;

	memcpy(out->prd_keys, d + 0, sizeof(out->prd_keys));
	memcpy(out->dbg_keys, d + 32, sizeof(out->dbg_keys));
	out->b_prd_present = rd32(d, 64);
	out->b_dbg_present = rd32(d, 68);
	out->falcon_id = rd32(d, 72);
	out->supports_versioning = rd32(d, 76);
	out->version = rd32(d, 80);
	out->depmap_count = rd32(d, 84);
	memcpy(out->depmap, d + 88, sizeof(out->depmap));
	memcpy(out->kdf, d + 176, sizeof(out->kdf));
	return 0;
}

int
pg106_acr_desc_v1_parse(const void *dmem_, size_t dmem_size,
			struct pg106_acr_desc_v1_summary *out)
{
	const uint8_t *d = dmem_;
	size_t off;
	uint32_t i;

	if (!d || !out || !in_range(0, PG106_FLCN_ACR_DESC_V1_SIZE, dmem_size))
		return -EINVAL;

	// include/nvfw/acr.h struct flcn_acr_desc_v1: 0x200 reserved dmem
	// (union ucode_reserved_space), 4 u32 signatures, then the fields
	// below. The signature area is opaque (patched at boot by the falcon
	// ROM); gp102_acr_load_setup only touches what we parse here.
	off = 0x200 + 4 * 4;
	out->wpr_region_id = rd32(d, off + 0);
	out->wpr_offset = rd32(d, off + 4);
	out->mmu_memory_range = rd32(d, off + 8);
	out->no_regions = rd32(d, off + 12);
	if (out->no_regions > 2)
		return -EINVAL;
	off += 16;
	for (i = 0; i < 2; i++) {
		out->region[i].start_addr = rd32(d, off + 0);
		out->region[i].end_addr = rd32(d, off + 4);
		out->region[i].region_id = rd32(d, off + 8);
		out->region[i].read_mask = rd32(d, off + 12);
		out->region[i].write_mask = rd32(d, off + 16);
		out->region[i].client_mask = rd32(d, off + 20);
		out->region[i].shadow_mem_start_addr = rd32(d, off + 24);
		off += 28;
	}
	out->ucode_blob_size = rd32(d, off + 0);
	out->ucode_blob_base = rd64(d, off + 4);
	off += 12;
	out->vpr.vpr_enabled = rd32(d, off + 0);
	out->vpr.vpr_start = rd32(d, off + 4);
	out->vpr.vpr_end = rd32(d, off + 8);
	out->vpr.hdcp_policies = rd32(d, off + 12);
	return 0;
}

int
pg106_acr_wpr_layout(struct pg106_acr_lsfw *lsfws, size_t count,
		     uint32_t *wpr_size)
{
	// Straight port of gp102_acr_wpr_layout (gp102.c:134-160):
	//   wpr += 11 * sizeof(wpr_header_v1); wpr = ALIGN(wpr, 256);
	//   wpr += 0x100;                      // shared sub-WPR headers
	//   per lsfw: ALIGN 256 -> lsb (lsb_header_v1),
	//             ALIGN 4096 -> img (img.size),
	//             ALIGN 256 -> bld (ALIGN(bld_size, 256))
	uint64_t wpr = (uint64_t)PG106_ACR_LSF_MAX * 24;
	size_t i;

	if (!lsfws || !wpr_size)
		return -EINVAL;
	if (count > PG106_ACR_LSF_MAX)
		return -EINVAL;

	wpr = align_u64(wpr, 256);
	wpr += 0x100;

	for (i = 0; i < count; i++) {
		struct pg106_acr_lsfw *l = &lsfws[i];

		wpr = align_u64(wpr, 256);
		l->offset.lsb = (uint32_t)wpr;
		wpr += sizeof(struct pg106_acr_lsb_header_v1);

		wpr = align_u64(wpr, 4096);
		l->offset.img = (uint32_t)wpr;
		wpr += l->img_size;

		wpr = align_u64(wpr, 256);
		l->offset.bld = (uint32_t)wpr;
		l->bl_data_size = align_u64(l->bld_size, 256);
		wpr += l->bl_data_size;
	}

	if (wpr > UINT32_MAX)
		return -EOVERFLOW;
	*wpr_size = (uint32_t)wpr;
	return 0;
}

int
pg106_acr_wpr_header_next(const void *buf_, size_t size, size_t *pos,
			  struct pg106_acr_wpr_header_v1 *hdr,
			  bool *terminator)
{
	const uint8_t *buf = buf_;
	size_t off;

	if (!buf || !pos || !hdr || !terminator)
		return -EINVAL;
	off = *pos;
	if (!in_range(off, sizeof(*hdr), size))
		return -EINVAL;

	hdr->falcon_id = rd32(buf, off + 0);
	hdr->lsb_offset = rd32(buf, off + 4);
	hdr->bootstrap_owner = rd32(buf, off + 8);
	hdr->lazy_bootstrap = rd32(buf, off + 12);
	hdr->bin_version = rd32(buf, off + 16);
	hdr->status = rd32(buf, off + 20);
	*terminator = hdr->falcon_id == PG106_ACR_WPR_FALCON_ID_INVALID;
	*pos = off + sizeof(*hdr);
	return 0;
}

int
pg106_acr_wpr_header_build(const struct pg106_acr_lsfw *lsfw,
			   uint32_t bootstrap_owner, uint32_t rtos_id,
			   const struct pg106_acr_lsf_signature_v1 *sig,
			   struct pg106_acr_wpr_header_v1 *out)
{
	if (!lsfw || !sig || !out)
		return -EINVAL;

	// gp102.c:86-93 (struct wpr_header_v1 hdr = {...} in
	// gp102_acr_wpr_build):
	*out = (struct pg106_acr_wpr_header_v1){
		.falcon_id = lsfw->id,
		.lsb_offset = lsfw->offset.lsb,
		.bootstrap_owner = bootstrap_owner,
		// gp102.c:90: lazy_bootstrap = rtos && lsfw->id != rtos->id;
		// rtos_id == UINT32_MAX encodes "rtos == NULL".
		.lazy_bootstrap = rtos_id != UINT32_MAX &&
				  lsfw->id != rtos_id,
		.bin_version = sig->version,
		.status = PG106_ACR_WPR_STATUS_COPY,
	};
	return 0;
}

int
pg106_acr_lsb_header_build(const struct pg106_acr_lsfw *lsfw,
			   const struct pg106_acr_lsf_signature_v1 *sig,
			   struct pg106_acr_lsb_header_v1 *out)
{
	if (!lsfw || !sig || !out)
		return -EINVAL;

	// gp102.c:66-69 checks the signature file is exactly
	// sizeof(lsb_header_v1.signature) and memcpy's it into the header;
	// our parsed struct is that size by construction, so instead check
	// the signature belongs to this falcon (nouveau does not, but a
	// mismatched sig would fail falcon-UI validation on hardware).
	if (sig->falcon_id != lsfw->id)
		return -EINVAL;

	out->signature = *sig;

	// gm200.c:94-111 gm200_acr_wpr_build_lsb_tail, field for field.
	out->tail.ucode_off = lsfw->offset.img;
	out->tail.ucode_size = lsfw->ucode_size;
	out->tail.data_size = lsfw->data_size;
	out->tail.bl_code_size = lsfw->bootloader_size;
	out->tail.bl_imem_off = lsfw->bootloader_imem_offset;
	out->tail.bl_data_off = lsfw->offset.bld;
	out->tail.bl_data_size = lsfw->bl_data_size;
	out->tail.app_code_off = lsfw->app_start_offset +
				 lsfw->app_resident_code_offset;
	out->tail.app_code_size = lsfw->app_resident_code_size;
	out->tail.app_data_off = lsfw->app_start_offset +
				 lsfw->app_resident_data_offset;
	out->tail.app_data_size = lsfw->app_resident_data_size;
	out->tail.flags = lsfw->flags;
	return 0;
}

int
pg106_acr_hsfw_bld_build(const struct pg106_acr_hs_ucode *fw,
			 uint64_t vma_addr, uint8_t *out)
{
	if (!fw || !out)
		return -EINVAL;

	// gm200.c:227-237 gm200_acr_hsfw_load_bld, serialized little-endian
	// per include/nvfw/flcn.h struct flcn_bl_dmem_desc_v1 (__packed):
	// reserved[4] @0, signature[4] @16, ctx_dma @32, code_dma_base @36,
	// non_sec_code_off @44, non_sec_code_size @48, sec_code_off @52,
	// sec_code_size @56, code_entry_point @60, data_dma_base @64,
	// data_size @72.
	memset(out, 0, PG106_FLCN_BL_DMEM_DESC_V1_SIZE);
	wr32(out, 32, PG106_FALCON_DMAIDX_VIRT); // .ctx_dma
	wr64(out, 36, vma_addr);               // .code_dma_base
	wr32(out, 44, fw->nmem_base);          // .non_sec_code_off
	wr32(out, 48, fw->nmem_size);          // .non_sec_code_size
	wr32(out, 52, fw->imem_base);          // .sec_code_off
	wr32(out, 56, fw->imem_size);          // .sec_code_size
	wr32(out, 60, 0);                      // .code_entry_point
	wr64(out, 64, vma_addr + fw->dmem_base_img); // .data_dma_base
	wr32(out, 72, fw->dmem_size);          // .data_size
	return 0;
}
