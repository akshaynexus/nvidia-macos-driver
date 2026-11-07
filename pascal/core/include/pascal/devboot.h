// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/devboot.h — the GP106 bring-up sequence the
// kext runs at start() and after every D3cold power-up, over a register bus.
//
// Order (each stage cites the nouveau code it follows; torvalds/linux
// d48723711bae, drivers/gpu/drm/nouveau/):
//   IDENTIFY  BOOT_0 must decode as GP106 (nvkm/engine/device/base.c, the
//             boot0 read in nvkm_device_ctor; pascal/pmc.h).
//   POSTED    0x2240c bit 1 (gf100_devinit_preinit, devinit/gf100.c:90-102).
//   POST      only when not posted: gm200_devinit_post with the VBIOS PMU
//             DEVINIT app (devinit/gm200.c:123-176; devinit_pmu.h). Needs the
//             VBIOS (vbios_fetch.h). nvkm_device_preinit treats a failure as
//             fatal (device/base.c:3016), so does this.
//   TOP       PTOP device table -> SEC2 base and reset line (top/gk104.c;
//             falcon/base.c:261-264), with the measured GP106 fallback.
//   ACR       parse the firmware, build the gp102 ACR plan with the chip's
//             own HWCFG / debug / BOOT_0 read-backs, run it
//             (acr_boot.h, hwexec.h); this also starts the SEC2 RTOS.
//   GR        read the GR topology (gf100_gr_oneinit, gr/gf100.c:2009-2033;
//             gm200_gr_rops 0x12006c; gm200.c:107-124 LTC/MMU sources;
//             gp102.c:86-96 SWDX), build the GR plan, run it up to
//             bootstrap_before, ask SEC2 to BOOTSTRAP_FALCON(FECS|GPCCS)
//             (gf100_gr_init_ctxctl_ext, gr/gf100.c:1761-1783), run the rest.
//   READY
// The first failure stops the sequence: no register is written after the
// failing step, the failure is reported through the event hook, and the
// result names the stage, the step and its nouveau citation. Nothing is
// undone (nouveau does not unwind a failed init either); the kext then
// keeps userspace off the GPU and, if configured, powers it off.
//
// SIMULATION status: every stage is exercised against mocks
// (tests/test_devboot.c, test_acr_boot.c, test_gr_boot.c). None of it has
// run on a GP106.

#ifndef PASCAL_DEVBOOT_H
#define PASCAL_DEVBOOT_H

#include <pascal/acr.h>
#include <pascal/acr_boot.h>
#include <pascal/devinit_pmu.h>
#include <pascal/gr_boot.h>
#include <pascal/hwexec.h>
#include <pascal/top.h>
#include <pascal/vmm.h>
#include <pascal/vram.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- firmware ---------------------------------------------------------------

struct pg106_fw_blob {
	const uint8_t *data;
	size_t size;
};

// The linux-firmware files GP106 loads (research/firmware/gp106/README.md:
// WHENCE links acr/ and sec2/ to gp102). Field order = pg106_devboot_fw_names.
struct pg106_devboot_fw {
	struct pg106_fw_blob acr_bl, acr_ucode_load;
	struct pg106_fw_blob fecs_bl, fecs_inst, fecs_data, fecs_sig;
	struct pg106_fw_blob gpccs_bl, gpccs_inst, gpccs_data, gpccs_sig;
	struct pg106_fw_blob sec2_desc, sec2_image, sec2_sig;
	struct pg106_fw_blob sw_nonctx;
};

// Kext bundle resource name <-> linux-firmware path, for every blob above.
// The kext requests `resource` from its bundle (OSKextRequestResource);
// pascal/kext/scripts/build-macos.sh stages `path` (relative to
// research/firmware/) under that name. `required_for` is the stage that
// fails without it.
struct pg106_fw_name {
	const char *resource;
	const char *path;
	size_t offset; // offsetof(struct pg106_devboot_fw, field)
	uint8_t required_for; // enum pg106_devboot_stage
};
extern const struct pg106_fw_name pg106_devboot_fw_names[];
extern const size_t pg106_devboot_fw_count;

// ---- stages + events --------------------------------------------------------

enum pg106_devboot_stage {
	PG106_DB_IDENTIFY = 0,
	PG106_DB_POSTED,
	PG106_DB_POST,
	PG106_DB_TOP,
	PG106_DB_ACR,
	PG106_DB_GR,
	PG106_DB_SEC2_BOOTSTRAP,
	PG106_DB_READY,
	PG106_DB_STAGE_COUNT
};

enum pg106_devboot_ev {
	PG106_DB_EV_BEGIN = 0,
	PG106_DB_EV_OK,
	PG106_DB_EV_SKIP,
	PG106_DB_EV_FAIL,
	PG106_DB_EV_INFO,
};

struct pg106_devboot_event {
	uint8_t stage; // enum pg106_devboot_stage
	uint8_t ev;    // enum pg106_devboot_ev
	int ret;
	const char *detail; // static text or a step's nouveau citation
	uint64_t a, b;      // stage-specific numbers (documented per event)
};

typedef void (*pg106_devboot_event_fn)(void *user,
				       const struct pg106_devboot_event *ev);

// Host pages for the HS image (nouveau keeps it in system memory:
// nvkm_firmware_mem_target = NVKM_MEM_TARGET_HOST, core/firmware.c:169-176).
// alloc returns a CPU pointer and an opaque handle; bus gives the DMA
// address of the 4 KiB page at `off` (IODMACommand on macOS).
struct pg106_devboot_hostmem {
	void *user;
	int (*alloc)(void *user, uint64_t size, void **cpu, void **handle);
	int (*bus)(void *user, void *handle, uint64_t off, uint64_t *addr);
	void (*free)(void *user, void *handle);
	bool coherent; // map SYS_COHERENT (+VOL) rather than NONCOHERENT
};

// Where the ACR memory objects live, chosen by the caller when it does not
// pass a VRAM allocator (tests of the plan alone use fixed values).
struct pg106_devboot_mem {
	uint64_t vma_addr;       // HS image VA in the ACR VMM
	uint64_t wpr_alloc_base; // WPR+shadow allocation (VRAM)
	uint32_t bind_target;    // instance block target (0 = VRAM)
	uint64_t bind_addr;      // instance block address
};

struct pg106_devboot_cfg {
	const struct pg106_hw_bus *bus;
	// VBIOS for POST (NULL: POST fails if the GPU is not posted).
	const uint8_t *vbios;
	uint32_t vbios_size;
	bool force_post;          // NvForcePost (devinit/base.c:87-91)
	const struct pg106_devboot_fw *fw;
	struct pg106_devboot_mem mem;
	// ACR memory the way nouveau builds it (nvkm_acr_oneinit,
	// acr/base.c:283-352; nvkm_falcon_fw_oneinit, falcon/fw.c:127-151):
	// when vram is set, devboot runs FB init (vram.h), allocates the WPR +
	// shadow (gp102_acr_wpr_alloc: tail, align 0x40000), the ACR instance
	// block and VMM in VRAM, puts the HS image in host pages (hostmem) or
	// in VRAM (hostmem NULL), maps it with 4 KiB pages at
	// acr_vma (nvkm_vmm_get first fit: 0 in a fresh VMM), joins the VMM
	// and serves the plan's WPR/IMG writes itself (mem and the bus's
	// mem_write are then ignored).
	struct pg106_vram *vram;
	const struct pg106_devboot_hostmem *hostmem;
	uint64_t acr_vma;
	uint64_t sysmem_flush_bus; // pg106_fb_init; 0 = untouched
	bool fb_ready;             // the caller ran pg106_fb_init already
	// Stop after this stage succeeds (PG106_DB_READY = run everything).
	// The kext's "identify only" safe mode uses PG106_DB_POSTED.
	uint8_t stop_after;
	pg106_devboot_event_fn event;
	void *event_user;
	// SEC2 queue poll budget (pg106_sec2_ctx_init max_polls); 0 = 1000.
	uint32_t sec2_max_polls;
	// Replaces the SEC2 BOOTSTRAP_FALCON exchange when non-NULL (tests
	// that drive the GR mock, which has no SEC2). Returns 0 or -errno.
	int (*bootstrap)(void *user, uint64_t falcon_mask);
	void *bootstrap_user;
};

// Everything the run produced; the kext keeps it for GET_STATUS and for the
// next power-up.
struct pg106_devboot {
	int ret;
	uint8_t stage;      // stage reached (failed stage when ret != 0)
	uint8_t done_mask_; // private: stages completed (bit per stage)
	uint32_t boot0;
	bool posted_before; // 0x2240c bit 1 at POSTED
	bool posted;        // POST ran and succeeded
	struct pg106_post_result post;
	struct pg106_top_table top;
	uint32_t sec2_base, sec2_reset;
	struct pg106_hw_result exec; // last plan run (failing one on error)
	// Parsed firmware (point into the caller's blobs and fecs/gpccs_img).
	struct pg106_acr_bl_ucode bl;
	struct pg106_acr_hs_ucode hs;
	struct pg106_acr_lsf_signature_v1 sig[3];
	struct pg106_acr_lsfw lsfw[3];
	uint8_t *fecs_img, *gpccs_img;
	struct pg106_acrboot_plan acr_plan;
	struct pg106_acrboot_info acr_info;
	struct pg106_grboot_config gr_cfg;
	struct pg106_grboot_plan gr_plan;
	struct pg106_grboot_info gr_info;
	uint32_t gr_sizes[3]; // FECS ctx / zcull / pm replies seen
	unsigned gr_nsizes;
	struct pg106_sec2 sec2;
	// ACR memory (cfg->vram set). Kept until pg106_devboot_fini.
	struct pg106_devboot_acrmem {
		struct pg106_vram *vram;
		struct pg106_fb fb;
		bool fb_done;
		uint64_t wpr_base, wpr_alloc; // shadow at base, WPR in the top half
		uint64_t inst;
		struct pg106_vmm vmm;
		bool vmm_up;
		uint64_t img_size; // mapped bytes (page multiple)
		uint64_t img_pa;   // VRAM placement (0 when in host memory)
		void *img_cpu, *img_handle;
		const struct pg106_devboot_hostmem *host;
		uint64_t va;
		uint32_t bindinst; // the word BINDINST receives
	} acrmem;
};

// Run the sequence. db is zeroed first; call pg106_devboot_fini() after
// (also after a failure). Returns db->ret.
int pg106_devboot_run(struct pg106_devboot *db,
		      const struct pg106_devboot_cfg *cfg);

// Run only the GR stage (with the SEC2 bootstrap inside it) on a db whose
// earlier stages already ran; db->sec2_base must be set. Used after the
// executor-level tests and by callers that re-init GR alone.
int pg106_devboot_gr(struct pg106_devboot *db,
		     const struct pg106_devboot_cfg *cfg);

// Release the plans and composed images. Safe on a zeroed db.
void pg106_devboot_fini(struct pg106_devboot *db);

// Read the GR topology the GR plan is derived from (the discovery reads of
// gf100_gr_oneinit and friends; see the file comment). FECS reply sizes are
// left at plausible placeholders: the plan polls them from FECS itself.
// Returns 0 or -ENODEV when 0x409604 reads 0 GPCs / > 32.
int pg106_devboot_gr_discover(const struct pg106_hw_bus *bus,
			      struct pg106_grboot_config *cfg,
			      size_t sw_nonctx_size);

// VRAM size the way gf100_ram_ctor totals it on GP10x (subdev/fb/
// ramgf100.c:491-520 with gp100_ram's hooks, ramgp100.c:80-86):
// FBPs from 0x022438; per FBP (gm107_ram_probe_fbp, ramgm107.c:27-32 ->
// gm200_ram_probe_fbp_amount, ramgm200.c:26-46) skip it when 0x021d38 has
// its bit; FBPAs per FBP from 0x022458, each not floorswept in 0x021c14
// adds 0x90020c + fbpa * 0x4000 MiB (gp100_ram_probe_fbpa,
// ramgp100.c:75-78). The mixed-memory split is not modelled. Returns 0
// or -ENODEV (0 FBPs / 0 bytes / PRI errors).
int pg106_fb_vram_size(const struct pg106_hw_bus *bus, uint64_t *bytes);

// Top-of-VRAM reservation nouveau keeps out of its allocator ("vbios
// etc", ramgf100.c:498).
#define PG106_FB_RSVD_TAIL 0x100000ull

const char *pg106_devboot_stage_name(uint8_t stage);
const char *pg106_devboot_ev_name(uint8_t ev);

#ifdef __cplusplus
}
#endif

#endif // PASCAL_DEVBOOT_H
