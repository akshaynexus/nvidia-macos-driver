// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/acr.h — ACR secure-boot blob parsers:
// falcon HS ucode images (bl.bin / ucode_load.bin / ucode_unload.bin) and the
// WPR/LSB header layout the r352 ACR builds in shadow memory for the
// load-state falcons (FECS/GPCCS on GP106).
//
// Derived from nouveau (linux master, reference copies under /tmp/pascal-ref/):
//   include/nvfw/fw.h                 nvfw_bin_hdr, nvfw_bl_desc
//   include/nvfw/hs.h                 nvfw_hs_header, nvfw_hs_load_header
//   include/nvfw/acr.h                wpr_header_v1, lsf_signature_v1,
//                                     lsb_header_tail, lsb_header_v1,
//                                     flcn_acr_desc_v1
//   include/nvfw/flcn.h               flcn_bl_dmem_desc_v1
//   include/nvkm/subdev/acr.h         nvkm_acr_lsfw, nvkm_acr_lsf_id,
//                                     nvkm_acr_lsf_func flags
//   nvkm/subdev/acr/gp102.c           GP10x (r352) WPR parse/layout/build
//   nvkm/subdev/acr/gm200.c           gm200_acr_wpr_build_lsb_tail,
//                                     gm200_acr_hsfw_load_bld
//   nvkm/falcon/fw.c                  nvkm_falcon_fw_ctor_hs: how the raw
//                                     ucode blob is walked
// NOTE: on current master the historical acr_r352.c has been merged into
// gm200.c/gp102.c; the r352 WPR/LSB layout lives there now (HANDOFF-PASCAL.md
// §5 names acr_r352.c, which is the pre-merge location of the same code).
//
// GP106 wiring: nvkm/engine/device/base.c wires acr = gp102_acr_new for the
// whole GP10x family, so the gp102 ACR (r352) header format is THE format for
// this GPU. The HS ucode blobs nouveau loads for it are
// nvidia/<chip>/acr/{bl,ucode_load,unload_bl,ucode_unload}.bin (gp102.c
// MODULE_FIRMWARE lines 178-188, 225-235).
//
// Wire format is little-endian on disk. Every parse reads byte-wise (no
// type-punning), validates all offsets against the buffer size before use,
// and never dereferences beyond the caller's buffer, so truncated or hostile
// input returns -EINVAL instead of faulting.
//
// Single-threaded, libc-only, C11. Error returns are errno-style (0 or
// negative errno). No Linux kernel dependencies.

#ifndef PASCAL_ACR_H
#define PASCAL_ACR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// Falcon IDs (include/nvkm/subdev/acr.h, enum nvkm_acr_lsf_id, lines 9-16).
#define PG106_ACR_LSF_PMU      0u
#define PG106_ACR_LSF_GSPLITE  1u
#define PG106_ACR_LSF_FECS     2u
#define PG106_ACR_LSF_GPCCS    3u
#define PG106_ACR_LSF_NVDEC    4u
#define PG106_ACR_LSF_SEC2     7u
#define PG106_ACR_LSF_MINION   10u

// nvkm_acr_wpr_layout reserves 11 WPR header slots: gp102.c:139
// `wpr += 11 /* MAX_LSF */ * sizeof(struct wpr_header_v1);`
#define PG106_ACR_LSF_MAX      11u

// wpr_header_v1 terminator (include/nvfw/acr.h, WPR_HEADER_V1_FALCON_ID_INVALID).
#define PG106_ACR_WPR_FALCON_ID_INVALID 0xffffffffu

// wpr_header_v1.status values (include/nvfw/acr.h, WPR_HEADER_V1_STATUS_*
// lines 29-36). The ACR writes STATUS_COPY when building the shadow copy
// (gp102.c:92 `.status = WPR_HEADER_V1_STATUS_COPY`).
#define PG106_ACR_WPR_STATUS_NONE                    0u
#define PG106_ACR_WPR_STATUS_COPY                    1u
#define PG106_ACR_WPR_STATUS_VALIDATION_CODE_FAILED  2u
#define PG106_ACR_WPR_STATUS_VALIDATION_DATA_FAILED  3u
#define PG106_ACR_WPR_STATUS_VALIDATION_DONE         4u
#define PG106_ACR_WPR_STATUS_VALIDATION_SKIPPED      5u
#define PG106_ACR_WPR_STATUS_BOOTSTRAP_READY         6u
#define PG106_ACR_WPR_STATUS_REVOCATION_CHECK_FAILED 7u

// LSB header tail flags (include/nvkm/subdev/acr.h lines 117-119; comment
// there: "currently map directly to LSB header flags").
#define PG106_ACR_LSF_LOAD_CODE_AT_0  0x00000001u
#define PG106_ACR_LSF_DMACTL_REQ_CTX  0x00000004u
#define PG106_ACR_LSF_FORCE_PRIV_LOAD 0x00000008u

// sizeof(struct lsf_signature_v1): 2*16 prd + 2*16 dbg keys + 6 u32 + 88
// depmap + 16 kdf = 192. gp102.c:66 requires the .sig firmware file to be
// exactly this size (`WARN_ON(lsfw->sig->size != sizeof(hdr.signature))`).
#define PG106_ACR_LSF_SIG_V1_SIZE 192u

// Loader DMA index used for the HS bootloader descriptor
// (include/nvkm/engine/falcon.h, enum: FALCON_DMAIDX_VIRT = 1; used by
// gm200.c:228 `.ctx_dma = FALCON_DMAIDX_VIRT`).
#define PG106_FALCON_DMAIDX_VIRT 1u
// LS bootloader DMA indices (same enum, include/nvkm/engine/falcon.h:8-15):
// FALCON_DMAIDX_UCODE = 0 is what gm200_gr_acr_bld_write puts in the
// FECS/GPCCS block (nvkm/engine/gr/gm200.c:64), FALCON_SEC2_DMAIDX_UCODE = 6
// what gp102_sec2_acr_bld_write_1 puts in the SEC2 block
// (nvkm/engine/sec2/gp102.c:256).
#define PG106_FALCON_DMAIDX_UCODE 0u
#define PG106_FALCON_SEC2_DMAIDX_UCODE 6u
// gp102_sec2_flcn.emem_addr (nvkm/engine/sec2/gp102.c:211): DMEM addresses
// at or above this go to the EMEM PIO port (nvkm/falcon/base.c:127-133);
// also the SEC2 LS bootloader's argv (sec2/gp102.c:265).
#define PG106_SEC2_EMEM_ADDR 0x01000000u

// Falcon ucode bin-container magics (nvkm/falcon/fw.c:243-256, the
// `switch (hdr->bin_magic)` in nvkm_falcon_fw_ctor_hs): 0x000010de is
// NVIDIA's standard container where patch_loc/patch_sig are absolute blob
// offsets pointing at u32s holding the real values; 0x3b1d14f0 is the newer
// format with the values stored directly.
#define PG106_FLCN_BIN_MAGIC_STD 0x000010deu
#define PG106_FLCN_BIN_MAGIC_LE  0x3b1d14f0u

// Sanity cap on load-header app entries (nvfw_hs_load_header.apps is a
// flexible array; the real gp102 ACR ucode carries exactly 1 app). Parsed
// blobs declaring more are rejected.
#define PG106_ACR_HS_MAX_APPS 4u

// sizeof(struct flcn_bl_dmem_desc_v1) __packed (include/nvfw/flcn.h:62-74):
// 4*4 reserved + 4*4 signature + ctx_dma + u64 code_dma_base + 4*4 + u64
// data_dma_base + data_size = 16+16+4+8+16+8+4 + 4 = 76.
#define PG106_FLCN_BL_DMEM_DESC_V1_SIZE 76u
// sizeof(struct flcn_bl_dmem_desc_v2) __packed (include/nvfw/flcn.h:79-93):
// the v1 layout + argc + argv = 76 + 8 = 84. SEC2 ver-1 LS bld_size
// (nvkm/engine/sec2/gp102.c:273).
#define PG106_FLCN_BL_DMEM_DESC_V2_SIZE 84u
// sizeof(struct nvfw_ls_desc_v1) (include/nvfw/ls.h:7-49): head (4 u32 +
// char date[64] + 13 u32 = 132) + nb_imem/nb_dmem overlays + 64 load_ovl
// pairs + compressed = 132 + 8 + 512 + 4 = 656 (= sec2/desc-1.bin size).
#define PG106_ACR_LS_DESC_V1_SIZE 656u
// sizeof(struct nvfw_ls_desc_head) — the only part
// nvkm_acr_lsfw_from_desc consumes (nvkm/subdev/acr/lsfw.c:120-138).
#define PG106_ACR_LS_DESC_HEAD_SIZE 132u

// sizeof(struct flcn_acr_desc_v1) (include/nvfw/acr.h:208-234):
// 0x200 reserved + 4 u32 signatures + wpr_region_id/wpr_offset/mmu_memory_range
// + regions{no_regions + 2 * 7 u32} + ucode_blob_size + u64 blob base
// (u64-aligned) + vpr 4 u32 = 512+16+12+60+4+8+16 = 632 bytes.
#define PG106_FLCN_ACR_DESC_V1_SIZE 632u

// ---------------------------------------------------------------------------
// Wire-format structs (host-endian mirrors; every field copied byte-wise
// during parse, so no struct is ever cast over the blob).
// ---------------------------------------------------------------------------

// Container header at offset 0 of every bl.bin/ucode_*.bin
// (include/nvfw/fw.h:7-14, struct nvfw_bin_hdr).
struct pg106_flcn_bin_hdr {
	// nvfw_bin_hdr.bin_magic — PG106_FLCN_BIN_MAGIC_* (0x000010de in the
	// gp102 ACR blobs).
	uint32_t bin_magic;
	// nvfw_bin_hdr.bin_ver — container version (1 in all gp102 ACR blobs).
	uint32_t bin_ver;
	// nvfw_bin_hdr.bin_size — logical size of the whole container
	// (0x500 for bl.bin, 0x4300 for gp102 ucode_load.bin).
	uint32_t bin_size;
	// nvfw_bin_hdr.header_offset — offset of the bl_desc / hs_header
	// (0x100 in the gp102 ACR blobs).
	uint32_t header_offset;
	// nvfw_bin_hdr.data_offset — offset of the ucode image payload (0x200).
	uint32_t data_offset;
	// nvfw_bin_hdr.data_size — size of the ucode image payload.
	uint32_t data_size;
};

// Bootloader descriptor, at bin_hdr.header_offset in bl.bin
// (include/nvfw/fw.h:18-25, struct nvfw_bl_desc; consumed by
// nvkm/falcon/fw.c:286-294).
struct pg106_flcn_bl_desc {
	// nvfw_bl_desc.start_tag — IMEM tag the boot ROM loads the BL at;
	// fw.c:289 `fw->boot_addr = desc->start_tag << 8`.
	uint32_t start_tag;
	// nvfw_bl_desc.dmem_load_off — DMEM offset the BL's data segment is
	// copied to (0 in gp102 bl.bin/unload_bl.bin).
	uint32_t dmem_load_off;
	// nvfw_bl_desc.code_off — BL code offset, relative to bin_hdr.data_offset.
	uint32_t code_off;
	// nvfw_bl_desc.code_size — BL code size in bytes (0x200 in gp102 blobs).
	uint32_t code_size;
	// nvfw_bl_desc.data_off — BL data offset, relative to data_offset.
	uint32_t data_off;
	// nvfw_bl_desc.data_size — BL data size (0x100 in gp102 blobs).
	uint32_t data_size;
};

// HS ucode descriptor, at bin_hdr.header_offset in ucode_load/unload.bin
// (include/nvfw/hs.h:7-16, struct nvfw_hs_header; consumed by
// nvkm/falcon/fw.c:231-232, 245-260).
struct pg106_flcn_hs_header {
	// nvfw_hs_header.sig_dbg_offset — blob-absolute offset of the debug
	// signature (0x120 in gp102 ucode_load.bin).
	uint32_t sig_dbg_offset;
	// nvfw_hs_header.sig_dbg_size — debug signature size (0x10).
	uint32_t sig_dbg_size;
	// nvfw_hs_header.sig_prod_offset — blob-absolute offset of the
	// production signature (0x130).
	uint32_t sig_prod_offset;
	// nvfw_hs_header.sig_prod_size — production signature size (0x10).
	uint32_t sig_prod_size;
	// nvfw_hs_header.patch_loc — with bin_magic 0x000010de: blob-absolute
	// offset of a u32 holding the image-relative offset where the selected
	// signature is patched in (0x140; pointed-to value 0x3820 in
	// ucode_load.bin). With magic 0x3b1d14f0: the value itself (fw.c:243-256).
	uint32_t patch_loc;
	// nvfw_hs_header.patch_sig — same indirection, giving the u32 added to
	// sig_prod_offset/sig_dbg_offset to select the signature slot (0x144;
	// pointed-to value 0).
	uint32_t patch_sig;
	// nvfw_hs_header.hdr_offset — blob-absolute offset of the HS load
	// header (0x148).
	uint32_t hdr_offset;
	// nvfw_hs_header.hdr_size — its size (0x24 = 20 head bytes + 1 app pair).
	uint32_t hdr_size;
};

// HS load header, at hs_header.hdr_offset (include/nvfw/hs.h:34-41, struct
// nvfw_hs_load_header; consumed by nvkm/falcon/fw.c:264-277). apps[] is a
// flexible array of num_apps (offset,size) u32 pairs; nouveau reads
// apps[0] as the secure entry offset and apps[num_apps] as its size
// (fw.c:270-272). We cap it at PG106_ACR_HS_MAX_APPS pairs.
struct pg106_flcn_hs_load_header {
	// nvfw_hs_load_header.non_sec_code_off — IMEM offset of the
	// non-secure code section (0 in gp102 ACR blobs).
	uint32_t non_sec_code_off;
	// nvfw_hs_load_header.non_sec_code_size — its size (0x100).
	uint32_t non_sec_code_size;
	// nvfw_hs_load_header.data_dma_base — image-relative DMEM base of the
	// data segment (0x2900 in ucode_load.bin; fw.c:274 fw->dmem_base_img).
	// gp102.c:199 locates the flcn_acr_desc_v1 at img[dmem_base_img].
	uint32_t data_dma_base;
	// nvfw_hs_load_header.data_size — data segment size (0x1000; fw.c:276).
	uint32_t data_size;
	// nvfw_hs_load_header.num_apps — number of app entries (1 in the
	// gp102 ACR blobs).
	uint32_t num_apps;
	// nvfw_hs_load_header.apps — packed (offset,size) pairs: apps[0] is the
	// secure code offset (0x100; fw.c:270 imem_base_img) and apps[1] its
	// size (0x2800; fw.c:272 imem_size).
	uint32_t apps[2u * PG106_ACR_HS_MAX_APPS];
};

// WPR header, one per load-state falcon at the start of the shadow-buffer
// WPR region, terminated by falcon_id == INVALID
// (include/nvfw/acr.h:22-38, struct wpr_header_v1; built by gp102.c:86-97).
struct pg106_acr_wpr_header_v1 {
	// wpr_header_v1.falcon_id — enum nvkm_acr_lsf_id of the falcon this
	// entry describes (2=FECS, 3=GPCCS; gp102.c:87 `.falcon_id = lsfw->id`).
	uint32_t falcon_id;
	// wpr_header_v1.lsb_offset — WPR-relative offset of the matching LSB
	// header (gp102.c:88 `.lsb_offset = lsfw->offset.lsb`).
	uint32_t lsb_offset;
	// wpr_header_v1.bootstrap_owner — falcon allowed to bootstrap this LSF;
	// on GP10x always SEC2 (gp102.c:89 `.bootstrap_owner = NVKM_ACR_LSF_SEC2`).
	uint32_t bootstrap_owner;
	// wpr_header_v1.lazy_bootstrap — 1 unless this entry is the runtime
	// falcon being bootstrapped now (gp102.c:90
	// `.lazy_bootstrap = rtos && lsfw->id != rtos->id`).
	uint32_t lazy_bootstrap;
	// wpr_header_v1.bin_version — LS ucode version, taken from the
	// signature's version field (gp102.c:91 `.bin_version = sig->version`).
	uint32_t bin_version;
	// wpr_header_v1.status — PG106_ACR_WPR_STATUS_* (gp102.c:92).
	uint32_t status;
};

// LS falcon signature, byte-for-byte the content of nvidia/gp106/gr/fecs_sig
// .bin (include/nvfw/acr.h:67-78, struct lsf_signature_v1; loaded by
// lsfw.c nvkm_acr_lsfw_load_sig_image_desc_v1 and copied into the LSB header
// by gp102.c:69).
struct pg106_acr_lsf_signature_v1 {
	// lsf_signature_v1.prd_keys — production RSA key hashes, 2 slots x 16
	// bytes (offsets 0x00-0x1f of fecs_sig.bin).
	uint8_t prd_keys[2][16];
	// lsf_signature_v1.dbg_keys — debug RSA key hashes (offsets 0x20-0x3f).
	uint8_t dbg_keys[2][16];
	// lsf_signature_v1.b_prd_present — production keys valid flag
	// (1 in the real gp106 sigs, offset 0x40).
	uint32_t b_prd_present;
	// lsf_signature_v1.b_dbg_present — debug keys valid flag (1, offset 0x44).
	uint32_t b_dbg_present;
	// lsf_signature_v1.falcon_id — the falcon this signature signs
	// (2=FECS in fecs_sig.bin, 3=GPCCS in gpccs_sig.bin; offset 0x48).
	uint32_t falcon_id;
	// lsf_signature_v1.supports_versioning — 1 in the gp106 sigs (0x4c).
	uint32_t supports_versioning;
	// lsf_signature_v1.version — LS ucode version, copied into the WPR
	// header's bin_version (2 in the gp106 sigs; offset 0x50).
	uint32_t version;
	// lsf_signature_v1.depmap_count — dependency-map entry count
	// (0 in the gp106 sigs; offset 0x54).
	uint32_t depmap_count;
	// lsf_signature_v1.depmap — LSF_LSB_DEPMAP_SIZE(11)*2*4 bytes of
	// dependency map (zeroed in the gp106 sigs; offsets 0x58-0xbf).
	uint8_t depmap[11 * 2 * 4];
	// lsf_signature_v1.kdf — key-derivation data (offsets 0xc0-0xcf).
	uint8_t kdf[16];
};

// LSB header tail: the plaintext loader parameters following the signature
// (include/nvfw/acr.h:80-93, struct lsb_header_tail; filled by
// gm200.c:94-111 gm200_acr_wpr_build_lsb_tail). All offsets are WPR-relative
// unless noted; they direct the ACR's LS load of the falcon.
struct pg106_acr_lsb_header_tail {
	// lsb_header_tail.ucode_off — WPR offset of the LS ucode image
	// (gm200.c:97 `.ucode_off = lsfw->offset.img`).
	uint32_t ucode_off;
	// lsb_header_tail.ucode_size — image size (gm200.c:98 lsfw->ucode_size).
	uint32_t ucode_size;
	// lsb_header_tail.data_size — DMEM data size (gm200.c:99 lsfw->data_size).
	uint32_t data_size;
	// lsb_header_tail.bl_code_size — bootloader code size
	// (gm200.c:100 lsfw->bootloader_size).
	uint32_t bl_code_size;
	// lsb_header_tail.bl_imem_off — bootloader IMEM offset
	// (gm200.c:101 lsfw->bootloader_imem_offset).
	uint32_t bl_imem_off;
	// lsb_header_tail.bl_data_off — WPR offset of the bootloader's DMEM
	// argument block (gm200.c:102 `.bl_data_off = lsfw->offset.bld`;
	// gp102.c:51 wpr_patch re-patches addresses through this field).
	uint32_t bl_data_off;
	// lsb_header_tail.bl_data_size — its size (gm200.c:103 lsfw->bl_data_size).
	uint32_t bl_data_size;
	// lsb_header_tail.app_code_off — resident code offset inside the image:
	// app_start_offset + app_resident_code_offset (gm200.c:104-105).
	uint32_t app_code_off;
	// lsb_header_tail.app_code_size — resident code size (gm200.c:106).
	uint32_t app_code_size;
	// lsb_header_tail.app_data_off — resident data offset inside the image:
	// app_start_offset + app_resident_data_offset (gm200.c:107-108).
	uint32_t app_data_off;
	// lsb_header_tail.app_data_size — resident data size (gm200.c:109).
	uint32_t app_data_size;
	// lsb_header_tail.flags — nvkm_acr_lsf_func.flags
	// (gm200.c:110 `.flags = lsfw->func->flags`).
	uint32_t flags;
};

// Full LSB header = signature + tail (include/nvfw/acr.h:102-105, struct
// lsb_header_v1; written into the WPR at lsfw->offset.lsb by gp102.c:72).
struct pg106_acr_lsb_header_v1 {
	struct pg106_acr_lsf_signature_v1 signature; // lsb_header_v1.signature
	struct pg106_acr_lsb_header_tail tail;       // lsb_header_v1.tail
};

// Bootloader DMEM argument block the ACR HS falcon expects at DMEM offset 0
// (include/nvfw/flcn.h:62-74, struct flcn_bl_dmem_desc_v1 __packed; filled by
// gm200.c:227-237 gm200_acr_hsfw_load_bld and PIO-written to DMEM). u64
// fields are little-endian on the wire; build serializes byte-wise.
struct pg106_flcn_bl_dmem_desc_v1 {
	// flcn_bl_dmem_desc_v1.reserved — zeros (NVIDIA ABI padding).
	uint32_t reserved[4];
	// flcn_bl_dmem_desc_v1.signature — zeros; patched by the falcon boot
	// ROM with the HS signature check result.
	uint32_t signature[4];
	// flcn_bl_dmem_desc_v1.ctx_dma — DMA context index
	// (gm200.c:228 FALCON_DMAIDX_VIRT).
	uint32_t ctx_dma;
	// flcn_bl_dmem_desc_v1.code_dma_base — VA of the HS ucode image
	// (gm200.c:229 fw->vma->addr).
	uint64_t code_dma_base;
	// flcn_bl_dmem_desc_v1.non_sec_code_off — IMEM offset of non-secure
	// code (gm200.c:230 fw->nmem_base = load_header non_sec_code_off).
	uint32_t non_sec_code_off;
	// flcn_bl_dmem_desc_v1.non_sec_code_size — its size (gm200.c:231).
	uint32_t non_sec_code_size;
	// flcn_bl_dmem_desc_v1.sec_code_off — IMEM offset of secure code,
	// 256-aligned apps[0] (gm200.c:232-233 fw->imem_base).
	uint32_t sec_code_off;
	// flcn_bl_dmem_desc_v1.sec_code_size — its size (gm200.c:233).
	uint32_t sec_code_size;
	// flcn_bl_dmem_desc_v1.code_entry_point — entry PC, 0 (gm200.c:234).
	uint32_t code_entry_point;
	// flcn_bl_dmem_desc_v1.data_dma_base — VA of the data segment
	// (gm200.c:235 fw->vma->addr + fw->dmem_base_img).
	uint64_t data_dma_base;
	// flcn_bl_dmem_desc_v1.data_size — its size (gm200.c:236 fw->dmem_size).
	uint32_t data_size;
};

// ACR parameters block living in the HS ucode's DMEM at dmem_base_img;
// gp102_acr_load_setup patches the WPR region into it before boot
// (include/nvfw/acr.h:208-234, struct flcn_acr_desc_v1; gp102.c:197-213).
// This summary carries the fields the driver actually reads/writes.
struct pg106_acr_desc_v1_summary {
	// flcn_acr_desc_v1.wpr_region_id — MMU region ID holding the WPR
	// (gp102.c:202 writes 1).
	uint32_t wpr_region_id;
	// flcn_acr_desc_v1.wpr_offset — legacy WPR offset (unused on GP10x; 0).
	uint32_t wpr_offset;
	// flcn_acr_desc_v1.mmu_memory_range — MMU memory range index (legacy).
	uint32_t mmu_memory_range;
	// flcn_acr_desc_v1.regions.no_regions — number of valid region_props
	// (gp102.c:203 writes 2).
	uint32_t no_regions;
	// flcn_acr_desc_v1.regions.region_props[2] — WPR region descriptor:
	// start/end addresses in 256-byte units (gp102.c:204-205 write
	// wpr_start>>8 / wpr_end>>8), region_id (1), read_mask (0xf),
	// write_mask (0xc), client_mask (0x2), shadow_mem_start_addr
	// (shadow_start>>8, the v1 field gm200's lacks; gp102.c:206-210).
	struct {
		uint32_t start_addr, end_addr, region_id;
		uint32_t read_mask, write_mask, client_mask;
		uint32_t shadow_mem_start_addr;
	} region[2];
	// flcn_acr_desc_v1.ucode_blob_size — size of the LS ucode blob region.
	uint32_t ucode_blob_size;
	// flcn_acr_desc_v1.ucode_blob_base — its base address.
	uint64_t ucode_blob_base;
	// flcn_acr_desc_v1.vpr_desc — VPR carveout descriptor (4 u32).
	struct {
		uint32_t vpr_enabled, vpr_start, vpr_end, hdcp_policies;
	} vpr;
};

// ---------------------------------------------------------------------------
// Parsed ucode views
// ---------------------------------------------------------------------------

// Parsed bl.bin (bootloader container). Pointers reference the caller's
// blob and stay valid while it does.
struct pg106_acr_bl_ucode {
	struct pg106_flcn_bin_hdr hdr;  // container header (offset 0)
	struct pg106_flcn_bl_desc desc; // BL descriptor (at hdr.header_offset)
	// BL code image: blob + hdr.data_offset + desc.code_off, desc.code_size
	// bytes (nvkm/falcon/fw.c:291 kmemdup source).
	const uint8_t *boot_code;
	uint32_t boot_size;       // desc.code_size (fw.c:290 fw->boot_size)
	uint64_t boot_addr;       // (uint64_t)desc.start_tag << 8 (fw.c:289)
	uint32_t dmem_load_off;   // desc.dmem_load_off
};

// Parsed HS ucode container (ucode_load.bin / ucode_unload.bin). Field
// names follow nvkm_falcon_fw (nvkm/falcon/priv.h) as filled by
// nvkm_falcon_fw_ctor_hs (nvkm/falcon/fw.c:214-305).
struct pg106_acr_hs_ucode {
	struct pg106_flcn_bin_hdr hdr;      // container header (offset 0)
	struct pg106_flcn_hs_header hs;     // HS descriptor (hdr.header_offset)
	struct pg106_flcn_hs_load_header load; // load header (hs.hdr_offset)
	// Ucode image: blob + hdr.data_offset, hdr.data_size bytes
	// (fw.c:235 passes it to nvkm_falcon_fw_ctor as the fw image).
	const uint8_t *img;
	size_t img_size;          // hdr.data_size
	// Signature patch site: image-relative offset where the selected
	// production signature is written at boot (fw.c:245-246 `loc`;
	// fw.c:58 fw->fw.img[dst] = sig). sig_size bytes long.
	uint32_t sig_base_img;
	uint32_t sig_size;        // hs.sig_prod_size (fw.c:258)
	// Blob-absolute offsets of the production and debug signatures after
	// patch_sig indirection (fw.c:259-260: hs.sig_prod_offset + sig and
	// hs.sig_dbg_offset + sig). Use to copy sig_size bytes out of the blob.
	// The debug signature is sig_size (= sig_prod_size) bytes too:
	// nvkm_falcon_fw_sign copies `sig_nr_dbg * fw->sig_size` bytes from
	// the dbg offset (fw.c:190-192), never sig_dbg_size.
	uint32_t sig_prod_off;
	uint32_t sig_dbg_off;
	// Non-secure (ROM-booted) code section: IMEM offsets
	// (fw.c:267-268 fw->nmem_base/fw->nmem_size).
	uint32_t nmem_base;
	uint32_t nmem_size;
	// Secure code section: imem_base_img = load.apps[0],
	// imem_base = ALIGN(apps[0], 0x100), imem_size = apps[1]
	// (fw.c:270-272). The BL copies the secure image to the aligned base.
	uint32_t imem_base_img;
	uint32_t imem_base;
	uint32_t imem_size;
	// Data segment: image-relative DMEM base and size
	// (fw.c:274-276 fw->dmem_base_img/fw->dmem_size).
	uint32_t dmem_base_img;
	uint32_t dmem_size;
	// Signed-area offset inside the data segment: loc - data_dma_base
	// (fw.c:277 fw->dmem_sign).
	uint32_t dmem_sign;
};

// Host-side description of one load-state falcon, the portable subset of
// struct nvkm_acr_lsfw (include/nvkm/subdev/acr.h:72-113) needed to run the
// WPR layout/build arithmetic. Callers fill it from their LS firmware
// loader (nouveau's lsfw.c fills it from gr/fecs_{desc,image,sig}.bin).
struct pg106_acr_lsfw {
	// nvkm_acr_lsfw.id — enum nvkm_acr_lsf_id (FECS=2, GPCCS=3, SEC2=7).
	uint32_t id;
	// nvkm_acr_lsfw.secure_bootloader — from the LS desc v2 (false for
	// GP10x FECS/GPCCS, which use the v0/v1 desc path).
	bool secure_bootloader;
	// nvkm_acr_lsfw.bootloader_size — BL code size, 256-aligned
	// (lsfw.c nvkm_acr_lsfw_from_desc: ALIGN(desc->bootloader_size, 256)).
	uint32_t bootloader_size;
	// nvkm_acr_lsfw.bootloader_imem_offset — BL IMEM offset.
	uint32_t bootloader_imem_offset;
	// nvkm_acr_lsfw.app_size — full app size, 256-aligned (lsfw.c).
	uint32_t app_size;
	// nvkm_acr_lsfw.app_start_offset — app base inside the image (lsfw.c).
	uint32_t app_start_offset;
	// nvkm_acr_lsfw.app_imem_entry — IMEM entry point (lsfw.c).
	uint32_t app_imem_entry;
	// nvkm_acr_lsfw.app_resident_code_offset/size — resident code slice
	// (lsfw.c; consumed by gm200.c:104-106 into tail.app_code_*).
	uint32_t app_resident_code_offset;
	uint32_t app_resident_code_size;
	// nvkm_acr_lsfw.app_resident_data_offset/size — resident data slice
	// (lsfw.c; gm200.c:107-109 into tail.app_data_*).
	uint32_t app_resident_data_offset;
	uint32_t app_resident_data_size;
	// nvkm_acr_lsfw.app_imem_offset/app_dmem_offset — app load targets
	// (lsfw.c; unused by the r352 tail build but part of the LSF description).
	uint32_t app_imem_offset;
	uint32_t app_dmem_offset;
	// nvkm_acr_lsfw.ucode_size/data_size — derived in lsfw.c:
	// ucode_size = ALIGN(app_resident_data_offset,256) + bootloader_size;
	// data_size = app_size + bootloader_size - ucode_size.
	uint32_t ucode_size;
	uint32_t data_size;
	// nvkm_acr_lsf_func.bld_size (include/nvkm/subdev/acr.h:122) — size of
	// the falcon-specific bootloader DMEM argument block (for FECS/GPCCS
	// the flcn_bl_dmem_desc-shaped block); bl_data_size = ALIGN(bld_size,256).
	uint32_t bld_size;
	// nvkm_acr_lsf_func.flags (acr.h:120) — LSB header tail flags.
	uint32_t flags;
	// nvkm_acr_lsfw.img.size — the LS ucode image size in bytes
	// (consumed by the layout: wpr += img.size after 4096-align).
	size_t img_size;
	// nvkm_acr_lsfw.offset.{lsb,img,bld} — filled by pg106_acr_wpr_layout
	// (gp102.c:144-157), consumed by the WPR builders.
	struct {
		uint32_t lsb;
		uint32_t img;
		uint32_t bld;
	} offset;
	// nvkm_acr_lsfw.bl_data_size — set by pg106_acr_wpr_layout:
	// ALIGN(bld_size, 256) (gp102.c:155).
	uint32_t bl_data_size;
	// nvkm_acr_lsfw.img.data — the LS ucode image bytes written into the
	// WPR at offset.img (gp102.c:104-107), img_size bytes. NULL = not
	// available (synthetic descriptions; the WPR model zero-fills).
	const uint8_t *img;
	// Which nvkm_acr_lsf_func.bld_write shapes this falcon's bootloader
	// data block (PG106_ACR_BLD_*). PG106_ACR_BLD_NONE = not modelled
	// (zero-fill of bl_data_size).
	uint32_t bld_kind;
};

// Bootloader-data block shapes (nvkm_acr_lsf_func.bld_write/.bld_patch):
//   GR       gm200_gr_acr_bld_write/_patch (nvkm/engine/gr/gm200.c:46-74):
//            flcn_bl_dmem_desc_v1, ctx_dma UCODE, FECS and GPCCS on gp10x
//            (gp104_gr_fwif, nvkm/engine/gr/gp104.c:93).
//   SEC2_V1  gp102_sec2_acr_bld_write_1/_patch_1 (sec2/gp102.c:241-269):
//            flcn_bl_dmem_desc_v2, ctx_dma SEC2_UCODE, argc 1, argv EMEM;
//            the variant nouveau picks first (gp102_sec2_fwif version 1,
//            sec2/gp102.c:307; nvkm_firmware_load tries the list in order,
//            include/nvkm/core/firmware.h:67-71).
#define PG106_ACR_BLD_NONE    0u
#define PG106_ACR_BLD_GR      1u
#define PG106_ACR_BLD_SEC2_V1 2u

// ---------------------------------------------------------------------------
// API — all functions return 0 on success or a negative errno (-EINVAL) on
// malformed input, and never read or write outside the caller's buffers.
// ---------------------------------------------------------------------------

// Parse a bootloader container (nvidia/<chip>/acr/bl.bin or unload_bl.bin).
// Mirrors the bl branch of nvkm_falcon_fw_ctor_hs (fw.c:279-297):
// bin header, then nvfw_bl_desc at header_offset, boot code at
// data_offset + code_off.
int pg106_acr_ucode_parse_bl(const void *blob, size_t size,
			     struct pg106_acr_bl_ucode *out);

// Parse an HS ucode container (ucode_load.bin / ucode_unload.bin). Mirrors
// the img branch of nvkm_falcon_fw_ctor_hs (fw.c:227-277), including the
// patch_loc/patch_sig indirection selected by bin_magic. Stricter than
// nouveau: also requires the signature patch site to fit inside the image
// (nouveau would overwrite whatever followed).
int pg106_acr_ucode_parse_hs(const void *blob, size_t size,
			     struct pg106_acr_hs_ucode *out);

// Parse a raw LS signature (nvidia/gp106/gr/fecs_sig.bin /
// gpccs_sig.bin). Requires size == PG106_ACR_LSF_SIG_V1_SIZE, matching
// gp102.c:66's WARN_ON.
int pg106_acr_lsf_sig_parse(const void *data, size_t size,
			    struct pg106_acr_lsf_signature_v1 *out);

// Parse the ACR parameters block out of an HS image's data segment
// (img + dmem_base_img, gp102.c:199). dmem points at that location,
// dmem_size is the remaining data-segment size.
int pg106_acr_desc_v1_parse(const void *dmem, size_t dmem_size,
			    struct pg106_acr_desc_v1_summary *out);

// GP10x (r352) WPR layout: mirror of gp102_acr_wpr_layout (gp102.c:134-160).
// Fills each entry's offset.lsb/img/bld and bl_data_size, and returns the
// total WPR size in *wpr_size. Requires count <= PG106_ACR_LSF_MAX and that
// the arithmetic does not overflow u32.
int pg106_acr_wpr_layout(struct pg106_acr_lsfw *lsfws, size_t count,
			 uint32_t *wpr_size);

// Read one WPR header at *pos from a shadow-buffer image. Advances *pos by
// sizeof(wpr_header_v1). Sets *terminator when falcon_id == INVALID (the
// end marker; gp102.c:168 loop condition) — callers must stop then.
int pg106_acr_wpr_header_next(const void *buf, size_t size, size_t *pos,
			      struct pg106_acr_wpr_header_v1 *hdr,
			      bool *terminator);

// Build the WPR header for one LSF. Mirrors the header initialization in
// gp102_acr_wpr_build (gp102.c:86-93): bin_version comes from the parsed
// signature, lazy_bootstrap is set unless falcon_id == rtos_id; pass
// rtos_id = UINT32_MAX for "no runtime falcon" (rtos == NULL in nouveau,
// which makes lazy_bootstrap 0).
int pg106_acr_wpr_header_build(const struct pg106_acr_lsfw *lsfw,
			       uint32_t bootstrap_owner, uint32_t rtos_id,
			       const struct pg106_acr_lsf_signature_v1 *sig,
			       struct pg106_acr_wpr_header_v1 *out);

// Build the LSB header for one LSF. Mirrors gp102_acr_wpr_build_lsb +
// gm200_acr_wpr_build_lsb_tail (gp102.c:62-74, gm200.c:94-111): copies the
// signature and derives all tail fields from the lsfw. Stricter than
// nouveau: rejects sig->falcon_id != lsfw->id.
int pg106_acr_lsb_header_build(const struct pg106_acr_lsfw *lsfw,
			       const struct pg106_acr_lsf_signature_v1 *sig,
			       struct pg106_acr_lsb_header_v1 *out);

// Build the HS bootloader DMEM argument block (gm200_acr_hsfw_load_bld,
// gm200.c:227-241) serialized little-endian into out, which must be
// PG106_FLCN_BL_DMEM_DESC_V1_SIZE bytes. vma_addr is the VA the HS image
// was mapped at (nouveau: fw->vma->addr).
int pg106_acr_hsfw_bld_build(const struct pg106_acr_hs_ucode *fw,
			     uint64_t vma_addr, uint8_t *out);

// nvkm_acr_lsfw_load_sig_image_desc_v1 + nvkm_acr_lsfw_from_desc
// (nvkm/subdev/acr/lsfw.c:119-178): fill an lsfw from an nvfw_ls_desc_v1
// (sec2/desc-1.bin) and its image (sec2/image-1.bin; img.size = file size,
// lsfw.c:105). Sets bld_kind/bld_size for the SEC2 ver-1 func
// (sec2/gp102.c:271-280) and flags = 0 (that func sets none). Stricter
// than nouveau: the resident code/data slices and the bootloader must lie
// inside the image.
int pg106_acr_lsfw_from_ls_desc_v1(const void *desc, size_t desc_size,
				   const uint8_t *img, size_t img_size,
				   uint32_t falcon_id,
				   struct pg106_acr_lsfw *out);

// nvkm_acr_lsfw_load_bl_inst_data_sig (nvkm/subdev/acr/lsfw.c:248-322):
// fill an lsfw for FECS/GPCCS from gr/<falcon>_{bl,inst,data}.bin and
// compose its WPR image (BL code, then inst, then data, each 256-aligned).
// img_out must hold *img_size_out bytes; pass img_out = NULL first to get
// the size (lsfw fields are filled either way). Sets bld_kind GR,
// bld_size = sizeof(flcn_bl_dmem_desc_v1), and flags =
// PG106_ACR_LSF_FORCE_PRIV_LOAD for GPCCS only (gm200_gr_gpccs_acr vs
// gm200_gr_fecs_acr, nvkm/engine/gr/gm200.c:76-89). Stricter than nouveau
// (which copies ALIGN(code_size,256) bytes unchecked, lsfw.c:304): the
// copied BL span must lie inside the bl blob. On success out->img points at
// img_out.
int pg106_acr_lsfw_from_bl_inst_data(const void *bl, size_t bl_size,
				     const void *inst, size_t inst_size,
				     const void *data, size_t data_size,
				     uint32_t falcon_id, uint8_t *img_out,
				     size_t img_cap, size_t *img_size_out,
				     struct pg106_acr_lsfw *out);

// Serialize the falcon's bootloader data block (lsfw->func->bld_write,
// gp102.c:110) after wpr_patch's address adjust (gp102_acr_wpr_patch,
// gp102.c:33-59 -> bld_patch adds `adjust` to code_dma_base/data_dma_base:
// gm200.c:46-54 for GR, sec2/gp102.c:241-249 for SEC2 v1). adjust = 0
// gives the bld_write bytes; nvkm_acr_oneinit passes
// wpr_start - wpr_prev (acr/base.c:306; wpr_prev defaults to 0,
// base.c:415). Writes bld_size bytes into out (cap >= bld_size) and
// returns them in *len. -EINVAL for PG106_ACR_BLD_NONE.
int pg106_acr_lsfw_bld_build(const struct pg106_acr_lsfw *lsfw,
			     int64_t adjust, uint8_t *out, size_t cap,
			     size_t *len);

#endif // PASCAL_ACR_H
