// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/vbios_bit.h — NVIDIA VBIOS parser: image chain
// (PCIR/NPDE), BIT token directory, image0/imaged address remap, BIT 'I'
// init tables, BIT 'p' PMU application table, and the DCB output /
// connector / GPIO tables the display + init-script code consumes.
//
// Derived from nouveau, linux master (sparse clone used for every line
// reference below: /tmp/review-ref/linux/drivers/gpu/drm/nouveau,
// torvalds/linux d48723711bae, 2026-11-06):
//   nvkm/subdev/bios/base.c   nvbios_addr (l.31-47): image0/imaged remap +
//                             OOB guard; nvbios_rd08/16/32 (l.57-79);
//                             nvkm_bios_new (l.150-213): image0_size /
//                             imaged_addr (l.170-178), BIT scan (l.189),
//                             'i' version (l.195-200)
//   nvkm/subdev/bios/image.c  nvbios_imagen (l.29-66): ROM signature, PCIR,
//                             and the NPDE size override (l.56-63);
//                             nvbios_image (l.68-83)
//   nvkm/subdev/bios/npde.c   nvbios_npdeTe/Tp (l.28-59)
//   nvkm/subdev/bios/pcir.c   nvbios_pcirTe/Tp (l.27-70)
//   nvkm/subdev/bios/bit.c    bit_entry (l.27-49)
//   nvkm/subdev/bios/init.c   init_table/init_table_ (l.358-406),
//                             init_script (l.408-427)
//   nvkm/subdev/bios/pmu.c    nvbios_pmuTe/Ee/Ep/Rm (l.29-102)
//   nvkm/subdev/bios/dcb.c    dcb_table (l.27-98), dcb_outp_parse
//                             (l.122-190)
//   nvkm/subdev/bios/conn.c   nvbios_connTe/Ee/Ep (l.28-90)
//   nvkm/subdev/bios/gpio.c   dcb_gpio_table/entry/parse (l.29-119)
//
// BIT is located by signature scan (base.c:189), never via the PCIR chain.
//
// GP106 POST — what this module does and does NOT feed (corrected; an
// earlier header claimed "no dedicated nouveau handling needed" and that
// the BIT 'I' scripts are what nvkm_devinit_post would execute — both
// wrong for GP10x):
//   nv136 (GP106) uses gm200_devinit_new (nvkm/engine/device/base.c, the
//   nv136_chipset entry). gm200_devinit_post (devinit/gm200.c:123-176)
//   does NOT interpret the 'I' boot scripts on the host: it requires
//   bit_I.version == 1 && bit_I.length >= 0x1c (gm200.c:134-138), uploads
//   the VBIOS PMU DEVINIT application (BIT 'p', type 0x04, via
//   nvbios_pmuRm — pg106_vbios_pmu_app below) to the PMU falcon, copies
//   the init tables ('I'+0x14/+0x16) and the boot scripts ('I'+0x18/+0x1a)
//   into PMU DMEM and lets the PMU run them. The host interpreter
//   (devinit.c) is only used on GP10x for the DCB encoder scripts after a
//   POST (devinit/nv50.c:118-140), supervisor IED scripts
//   (engine/disp/nv50.c:1029-1108) and DP link scripts (engine/disp/dp.c
//   :434-471). The POST model lives in devinit_pmu.{c,h}.
//
// Wire format is little-endian. Every read is byte-wise with bounds checks
// against the caller's buffer; truncated or hostile input returns a
// negative errno instead of ever reading out of bounds. C11, libc-only.
//
// Evidence status: exercised on a synthetic image (tests/test_vbios.c) and
// on the committed TechPowerUp ROMs (tests/test_vbios_real.c). That is
// host-side parser evidence on real firmware bytes, not device evidence.

#ifndef PASCAL_VBIOS_BIT_H
#define PASCAL_VBIOS_BIT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// BIT signature scanned for by nvkm_bios_new (base.c:189-190):
// 0xff, 0xb8, 'B' 'I' 'T' — two literals because 'B' is a hex digit.
#define PG106_VBIOS_BIT_SIG "\xff\xb8" "BIT"
#define PG106_VBIOS_BIT_SIG_LEN 5u

// BIT header is 12 bytes: bit_entry() reads entry_count at +10 and the
// stride at +9 and starts the walk at bit_offset + 12 (bit.c:31-42).
#define PG106_VBIOS_BIT_HDR_LEN 12u

// Per-token bytes read by bit_entry(): id u8 @+0, version u8 @+1,
// length u16 @+2, offset u16 @+4 (bit.c:35-38).
#define PG106_VBIOS_BIT_ENTRY_RD_LEN 6u

// INIT_DONE opcode terminating init scripts (init.c init_done, [0x71]).
#define PG106_VBIOS_INIT_DONE_OP 0x71u

// PCIR image type of the "imaged" image whose base the remap redirects
// image0-overflowing addresses into (base.c:173).
#define PG106_VBIOS_IMAGE_TYPE_EFI 0x70u
#define PG106_VBIOS_IMAGE_TYPE_IMAGED 0xe0u

// BIT 'p' PMU application types used by gm200 devinit (gm200.c:120, :141).
#define PG106_VBIOS_PMU_TYPE_PRE_OS  0x01u
#define PG106_VBIOS_PMU_TYPE_DEVINIT 0x04u

// DCB output types (include/nvkm/subdev/bios/dcb.h enum dcb_output_type).
#define PG106_DCB_OUTPUT_ANALOG 0x0u
#define PG106_DCB_OUTPUT_TMDS   0x2u
#define PG106_DCB_OUTPUT_LVDS   0x3u
#define PG106_DCB_OUTPUT_DP     0x6u
#define PG106_DCB_OUTPUT_EOL    0xeu
#define PG106_DCB_OUTPUT_UNUSED 0xfu

// Connector types used by init_generic_condition (include/nvkm/subdev/bios/
// conn.h enum dcb_connector_type).
#define PG106_DCB_CONNECTOR_eDP 0x47u
#define PG106_DCB_CONNECTOR_DP  0x46u
#define PG106_DCB_CONNECTOR_HDMI_0 0x60u

// DCB_GPIO_UNUSED (include/nvkm/subdev/bios/gpio.h).
#define PG106_DCB_GPIO_UNUSED 0xffu

// ---------------------------------------------------------------------------
// Structures (host-endian mirrors; every field copied byte-wise).
// ---------------------------------------------------------------------------

// One BIT token directory entry (include/nvkm/subdev/bios/bit.h): length
// and offset are u16 upstream (bit.h:7-8) — kept u16 here on purpose.
// Everything DERIVED from them (table pointers, script offsets, 'C'/'p'
// u32 pointers) is u32, as in nvbios_rd*(u32 addr).
struct pg106_vbios_bit_entry {
	uint8_t id;
	uint8_t version;
	uint16_t length;
	uint16_t offset;
};

// Parsed image + BIT header. Fill by pg106_vbios_bit_parse().
struct pg106_vbios_bit {
	const uint8_t *data;   // borrowed, not owned, never modified
	uint32_t size;
	uint32_t bit_offset;   // base.c:189 (0 if none)
	uint8_t entry_size;    // bit.c:42 stride
	uint8_t entry_count;   // bit.c:31
	// nvbios_addr remap inputs (base.c:170-178): image 0's size (NPDE
	// size when present — image.c:56-60) and the base of the first later
	// image of type 0xe0. 0 = no remap.
	uint32_t image0_size;
	uint32_t imaged_addr;
	uint8_t image_count;   // images walked by nvbios_image
};

// One image of the ROM chain, as nvbios_imagen describes it.
struct pg106_vbios_image {
	uint32_t base;
	uint32_t size;        // NPDE size if an NPDE follows a non-EFI PCIR
	uint32_t pcir_size;   // PCIR image_size (pcir.c:63), before override
	uint32_t pcir;        // absolute PCIR offset
	uint8_t type;         // PCIR image_type
	bool last;
	bool npde;            // NPDE override applied (image.c:57-60)
};

// ROM header + PCIR + NPDE probe of image 0 (image.c, pcir.c, npde.c).
struct pg106_vbios_rom {
	bool rom_sig_ok;       // 0xaa55 / 0xbb77 / 'NV' (image.c:39-43)
	uint32_t pcir_offset;  // u16 at 0x18 (pcir.c:30), 0 if none
	bool pcir_ok;          // PCIR/RGIS/NPDS signature (pcir.c:33-36)
	uint16_t vendor_id;
	uint16_t device_id;
	uint8_t class_code[3];
	// Image size nouveau uses for image 0: the NPDE size when an NPDE
	// structure follows the PCIR of a non-EFI image (image.c:56-60,
	// npde.c:55), else the PCIR size. This is image0_size for the remap
	// and the size nvbios_shadow checksums (shadow.c:86).
	uint32_t image_size;
	uint32_t pcir_image_size; // pcir + 0x10 * 512 (pcir.c:63)
	bool npde_ok;             // NPDE present and used
	uint16_t image_rev;
	uint8_t image_type;
	bool last_image;
	uint8_t checksum;         // sum over image_size: 0 = consistent
};

// VBIOS version bytes as decoded by nvkm_bios_new (base.c:195-200).
struct pg106_vbios_version {
	uint8_t micro, minor, chip, major, patch;
};

// BIT 'p' PMU application (struct nvbios_pmuR, include/nvkm/subdev/bios/
// pmu.h) plus where it came from. *_addr are ROM addresses (pre-remap,
// exactly what nvbios_rd32 is handed); *_addr_pmu are falcon IMEM/DMEM
// addresses.
struct pg106_vbios_pmu_app {
	uint8_t type;
	uint32_t desc;          // pmuE.data: the application descriptor
	uint32_t boot_addr_pmu;
	uint32_t boot_addr;
	uint32_t boot_size;
	uint32_t code_addr_pmu;
	uint32_t code_addr;
	uint32_t code_size;
	uint32_t init_addr_pmu;
	uint32_t data_addr_pmu;
	uint32_t data_addr;
	uint32_t data_size;
	uint32_t args_addr_pmu;
};

// DCB output entry (struct dcb_output subset, dcb.c:122-190).
struct pg106_dcb_outp {
	uint8_t index;       // raw DCB index
	uint32_t entry;      // image offset of the entry
	uint32_t conn_raw, conf_raw;
	uint8_t type;        // enum dcb_output_type
	uint8_t i2c_index;
	uint8_t heads;
	uint8_t connector;   // index into the connector table
	uint8_t bus;
	uint8_t location;
	uint8_t or_mask;     // dcb_output.or: OR / macro-link MASK (bits 27:24)
	uint8_t link;        // sublink mask (conf bits 5:4)
	int sorconf_link;
	uint8_t extdev;
	uint8_t dp_link_bw;  // 0x06/0x0a/0x14/0x1e
	uint8_t dp_link_nr;
	uint16_t hasht, hashm;
	bool i2c_upper_default; // set by nvkm outp code, not DCB; false here
};

// ---------------------------------------------------------------------------
// API (errno-style returns: 0 or negative errno)
// ---------------------------------------------------------------------------

// nvbios_image (image.c:68-83) on a raw buffer: image `idx` of the chain.
// Returns 0, -ENOENT past the last image / unknown signature, -EINVAL on
// null args.
int pg106_vbios_image(const uint8_t *data, uint32_t size, int idx,
		      struct pg106_vbios_image *out);

// Locate BIT and compute the remap inputs (nvkm_bios_new order: images,
// then BIT scan). -EINVAL null args; -ENOENT no signature; -EPROTO header
// or directory overruns the image / stride < 6.
int pg106_vbios_bit_parse(const uint8_t *data, uint32_t size,
			  struct pg106_vbios_bit *out);

// bit_entry (bit.c:27-49): 0 + *out, -ENOENT absent, -EINVAL null args.
int pg106_vbios_bit_entry(const struct pg106_vbios_bit *vb, uint8_t id,
			  struct pg106_vbios_bit_entry *out);

// nvbios_addr (base.c:31-47): apply the image0/imaged remap to `addr`.
// Returns the effective offset. Pure arithmetic; no bounds check.
uint32_t pg106_vbios_addr(const struct pg106_vbios_bit *vb, uint32_t addr);

// nvbios_rd08/16/32 (base.c:57-79), WITH the remap, returning -EFAULT on
// OOB instead of nouveau's silent 0 (nouveau logs "OOB"). vb->size bounds.
int pg106_vbios_rd08(const struct pg106_vbios_bit *vb, uint32_t offset,
		     uint8_t *val);
int pg106_vbios_rd16(const struct pg106_vbios_bit *vb, uint32_t offset,
		     uint16_t *val);
int pg106_vbios_rd32(const struct pg106_vbios_bit *vb, uint32_t offset,
		     uint32_t *val);

// nouveau-semantics readers: remapped, 0 on OOB (base.c:62/70/78). For
// model code that must reproduce upstream's degradation exactly.
uint8_t pg106_vbios_nv_rd08(const struct pg106_vbios_bit *vb, uint32_t a);
uint16_t pg106_vbios_nv_rd16(const struct pg106_vbios_bit *vb, uint32_t a);
uint32_t pg106_vbios_nv_rd32(const struct pg106_vbios_bit *vb, uint32_t a);

// init_table_ (init.c:376-406) for the 'I' sub-table pointer at
// `field` (0x00 script, 0x02 macro index, 0x04 macro, 0x06 condition,
// 0x08 io condition, 0x0a io flag condition, 0x0c function, 0x10 xlat).
// 0 + *ptr; -ENOENT pointer 0 ("pointer invalid"); -EPROTO 'I' too short
// ("init data too short"); -EFAULT OOB.
int pg106_vbios_init_table(const struct pg106_vbios_bit *vb,
			   const struct pg106_vbios_bit_entry *bit_I,
			   uint16_t field, uint32_t *ptr);

// Script-table pointer ('I'+0x00, init.c:399).
int pg106_vbios_init_script_table(const struct pg106_vbios_bit *vb,
				  const struct pg106_vbios_bit_entry *bit_I,
				  uint32_t *table_offset);

// init_script (init.c:408-427), BIT path: rd16(script_table + 2*index).
// 0 offset = "no script". -ENOENT no table; -EFAULT OOB.
int pg106_vbios_init_script(const struct pg106_vbios_bit *vb,
			    const struct pg106_vbios_bit_entry *bit_I,
			    uint16_t index, uint32_t *script_offset);

// 'i' version (base.c:195-200). -EPROTO length < 4, -EFAULT OOB.
int pg106_vbios_version(const struct pg106_vbios_bit *vb,
			const struct pg106_vbios_bit_entry *bit_i,
			struct pg106_vbios_version *out);

// Image-0 probe (image.c nvbios_imagen + pcir.c + npde.c, base 0).
// -ENOENT unknown ROM signature; -EPROTO PCIR fields overrun.
int pg106_vbios_rom_probe(const uint8_t *data, uint32_t size,
			  struct pg106_vbios_rom *out);

// nvbios_checksum (base.c:82-88).
uint8_t pg106_vbios_checksum(const uint8_t *data, uint32_t size);

// --- BIT 'p' PMU table (pmu.c) ---------------------------------------------

// nvbios_pmuTe (pmu.c:29-47): bit 'p' version 2, length >= 4, rd32 pointer.
// 0 + *table/hdr fields; -ENOENT no 'p' / wrong version / pointer 0;
// -EFAULT the table header lies outside the image (nouveau then reads
// zeros and finds no entries).
int pg106_vbios_pmu_table(const struct pg106_vbios_bit *vb, uint32_t *table,
			  uint8_t *ver, uint8_t *hdr, uint8_t *cnt,
			  uint8_t *len);

// nvbios_pmuRm (pmu.c:74-102): first entry of `type` with nonzero data.
// 0 + *app; -ENOENT none (upstream returns false); -EFAULT table/
// descriptor OOB (upstream: false via zero reads).
int pg106_vbios_pmu_app(const struct pg106_vbios_bit *vb, uint8_t type,
			struct pg106_vbios_pmu_app *app);

// --- DCB (dcb.c, conn.c, gpio.c) --------------------------------------------

// dcb_table (dcb.c:27-98), DCB 3.x/4.x path only (GP10x ROMs carry 4.x).
int pg106_vbios_dcb_table(const struct pg106_vbios_bit *vb, uint32_t *dcb,
			  uint8_t *ver, uint8_t *hdr, uint8_t *cnt,
			  uint8_t *len);

// dcb_outp_parse (dcb.c:122-190): 0 + *out; -ENOENT idx >= cnt.
int pg106_vbios_dcb_outp(const struct pg106_vbios_bit *vb, uint8_t idx,
			 struct pg106_dcb_outp *out);

// nvbios_connEp (conn.c:72-90) type byte for connector `idx`; -ENOENT when
// absent (upstream init_conn then reports "script needs connector type").
int pg106_vbios_conn_type(const struct pg106_vbios_bit *vb, uint8_t idx,
			  uint8_t *type);

// dcb_gpio_parse (gpio.c:76-119), idx 0 (the main GPIO table): func and
// line of entry `ent`. -ENOENT past the table.
int pg106_vbios_gpio_entry(const struct pg106_vbios_bit *vb, uint8_t ent,
			   uint8_t *func, uint8_t *line);

#ifdef __cplusplus
}
#endif

#endif // PASCAL_VBIOS_BIT_H
