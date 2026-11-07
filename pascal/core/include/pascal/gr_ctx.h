// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/gr_ctx.h — the GR (PGRAPH) engine context for
// GP106: the golden context image and one context per channel, so a
// channel on GR's runlist can run PASCAL_A / PASCAL_COMPUTE_B.
//
// Port of nouveau's gf100_gr / grctx code as GP106 uses it (gp104_gr with
// gp104_grctx, nvkm/engine/gr/gp104.c:26-62 + ctxgp104.c:28-52; nvkm at
// Linux d48723711bae, read from a sparse torvalds/linux clone). Every
// register write names its nouveau source.
//
// What nouveau does, and what this module does the same way:
//
//   gf100_gr_oneinit (gf100.c:1990-2062)
//     derives tpc_max, ppc_nr/ppc_mask/ppc_tpc_nr/min/max from the
//     topology the GR plan discovered (pg106_grboot_config), allocates the
//     three global context buffers in VRAM (pagepool 0x20000, bundle
//     0x3000, attrib_cb = gp102_grctx_generate_attrib_cb_size), builds the
//     tile map (gm200_gr_oneinit_tiles -> gf100_gr_oneinit_tiles) and the
//     SM table (gf100_gr_oneinit_sm_id).          -> pg106_gr_init()
//
//   gk20a_gr_load_sw (gk20a.c:294-304)
//     parses gr/sw_ctx.bin (aiv: addr, index, data), gr/sw_bundle_init.bin
//     (av: addr, data) and gr/sw_method_init.bin (av: class | mthd << 16,
//     data) into register packs.                  -> pg106_gr_init()
//
//   gf100_grctx_generate (ctxgf100.c:1436-1557), once, for the first
//     channel: FE power force-on, FECS reset, SCC RAM init; a scratch
//     context buffer (CB_RESERVED 0x80000 + the FECS image size) mapped in
//     a VMM; inst+0x210 = ctx | 4; FECS BIND_POINTER (mthd 3); the main
//     generation (gf100_grctx_generate_main, ctxgf100.c:1342-1433: unk260,
//     sw_ctx mmio, wait idle, the per-channel buffer registers and the
//     attribute/alpha split, the gk104 unkn masks, floorsweeping, the
//     bundle icmds, the class method defaults, unk260(1), r419a3c); FECS
//     WFI_GOLDEN_SAVE (mthd 9); the image is read back and kept.
//     nouveau runs this with the first channel's VMM; here a private VMM
//     and a bare instance block do it, so no FIFO channel is involved.
//                                                  -> pg106_gr_golden()
//
//   gf100_gr_chan_new (gf100.c:377-480) + gf100_gr_chan_bind (:320-353)
//     + gk104_ectx_ctor/_bind (fifo/gk104.c:134-186), per channel:
//     map the global buffers into the channel's VMM (privileged PTEs), an
//     mmio list (0x1000, the HUB ucode applies it on first load) with the
//     buffer registers for this channel's addresses, a copy of the golden
//     image with the header words (0x10 = pairs, 0x14/0x18 = mmio list
//     address, 0x1c = 1) mapped privileged, and the pointer the instance
//     block takes at 0x210: ctx_va | 4.            -> pg106_grctx_chan_create()
//
// Memory for the per-channel objects comes from the VRAM allocator; the
// caller maps it into the channel's VM through pg106_grctx_map (the kext's
// kdisp client VM, a pg106_vmm in the harness and the tests).
//
// SIMULATION status: tests/test_grctx.c runs it against tests/mockgpu.h's
// FECS model. Not yet run on a GPU (the harness stage is staged, not run).
// C11, libc only.

#ifndef PASCAL_GR_CTX_H
#define PASCAL_GR_CTX_H

#include <pascal/gr_boot.h>
#include <pascal/hwexec.h>
#include <pascal/vmm.h>
#include <pascal/vram.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// gp104_gr constants (gp104.c:51-54).
#define PG106_GR_FUNC_GPC_NR 6u
#define PG106_GR_FUNC_TPC_NR 5u
#define PG106_GR_FUNC_PPC_NR 3u
#define PG106_GR_GPC_MAX     32u
#define PG106_GR_TPC_MAX     (PG106_GR_GPC_MAX * PG106_GR_FUNC_TPC_NR) // gf100.h:37
// Scratch in front of the golden image (ctxgf100.c:1434).
#define PG106_GR_CB_RESERVED 0x80000u
// mmio list memory (gf100.c:447).
#define PG106_GR_MMIO_SIZE   0x1000u
// Where the instance block carries the GR context (gk104_ectx_bind).
#define PG106_GR_INST_ECTX   0x210u
#define PG106_GR_ECTX_FLAG   0x4ull // gk104_ectx_bind addr |= 4ULL

// FECS methods (gf100.c:751-980).
#define PG106_FECS_MTHD_BIND_POINTER     0x03u
#define PG106_FECS_MTHD_WFI_GOLDEN_SAVE  0x09u
#define PG106_FECS_MTHD_DISCOVER_IMAGE   0x10u
#define PG106_FECS_MTHD_DISCOVER_ZCULL   0x16u
#define PG106_FECS_MTHD_SET_WATCHDOG     0x21u
#define PG106_FECS_MTHD_DISCOVER_PM      0x25u

// The three firmware register lists gm200_gr_load -> gk20a_gr_load_sw
// reads besides sw_nonctx (gk20a.c:294-304; MODULE_FIRMWARE gp104.c).
struct pg106_gr_fw {
	const uint8_t *sw_ctx;         // aiv entries (12 bytes each)
	size_t sw_ctx_len;
	const uint8_t *sw_bundle_init; // av entries (8 bytes each)
	size_t sw_bundle_init_len;
	const uint8_t *sw_method_init; // av entries (8 bytes each)
	size_t sw_method_init_len;
};

struct pg106_gr_cfg {
	const struct pg106_hw_bus *bus;
	struct pg106_vram *vram;
	// The topology the GR plan discovered (devboot gr_cfg): gpc_nr,
	// tpc_nr[], ppc_tpc_mask[][].
	const struct pg106_grboot_config *topo;
	// FECS DISCOVER_IMAGE_SIZE reply (devboot gr_sizes[0]).
	uint32_t ctx_size;
	// GR's engine index in PTOP (nvkm_engn id: the 0x2640 + id * 8
	// status slot gk104_engn_chsw reads); the Helios table has it at 0.
	unsigned gr_engine;
	const struct pg106_gr_fw *fw;
	// Optional: a golden image from an earlier power cycle (ctx_size
	// bytes, copied). nouveau keeps gr->data across suspend; the kext can
	// hand the previous boot's image back and skip the generation.
	const uint32_t *golden;
};

struct pg106_gr_av {
	uint32_t addr, data;
};

// The engine (nouveau's gf100_gr subset).
struct pg106_gr {
	struct pg106_gr_cfg cfg;
	// Topology (gf100_gr_oneinit).
	uint8_t gpc_nr, tpc_max, tpc_total;
	uint8_t tpc_nr[PG106_GR_GPC_MAX];
	uint8_t ppc_nr[PG106_GR_GPC_MAX], ppc_mask[PG106_GR_GPC_MAX];
	uint8_t ppc_tpc_mask[PG106_GR_GPC_MAX][4], ppc_tpc_nr[PG106_GR_GPC_MAX][4];
	uint8_t ppc_tpc_min, ppc_tpc_max, ppc_total;
	uint8_t screen_tile_row_offset;
	uint8_t tile[PG106_GR_TPC_MAX];
	struct { uint8_t gpc, tpc; } sm[PG106_GR_TPC_MAX];
	uint8_t sm_nr;
	// Global context buffers (VRAM).
	uint64_t pagepool_pa, bundle_pa, attrib_pa;
	uint32_t pagepool_size, bundle_size, attrib_size;
	// Firmware packs.
	struct pg106_gr_av *sw_ctx;     // index dropped (gk20a_gr_aiv_to_init)
	size_t sw_ctx_n;
	struct pg106_gr_av *bundle;
	size_t bundle_n;
	struct pg106_gr_av *method;     // addr = class | mthd << 16, as in the file
	size_t method_n;
	// Golden image (ctx_size bytes), NULL until pg106_gr_golden.
	uint32_t *golden;
	uint32_t size;
	// Diagnostics of the last generation / failure.
	const char *failed_at;
	uint32_t fecs_stat;      // 0x409800 at the failure
	uint64_t mmio_writes, icmds, mthds, idle_waits, idle_wait_us;
	bool up;
};

int pg106_gr_init(struct pg106_gr *gr, const struct pg106_gr_cfg *cfg);
// Generate the golden context image (no-op when gr->golden exists).
int pg106_gr_golden(struct pg106_gr *gr);
void pg106_gr_fini(struct pg106_gr *gr);

// Mapping of VRAM into a channel's VM, supplied by the channel's owner.
// map: [pa, pa+size) at a VA of the owner's choice (4 KiB pages, kind
// PITCH, PTE PRIVILEGE when priv), returned in *va. The owner uploads
// and invalidates its tables itself (after pg106_grctx_chan_create, before
// the channel runs).
struct pg106_grctx_map {
	void *user;
	int (*map)(void *user, uint64_t pa, uint64_t size, bool priv,
		   uint64_t *va);
	void (*unmap)(void *user, uint64_t va, uint64_t size);
};

struct pg106_grctx_chan {
	uint64_t pagepool_va, bundle_va, attrib_va;
	uint64_t mmio_pa, mmio_va;
	uint32_t mmio_nr;   // words written to the mmio list (pairs * 2)
	uint64_t ctx_pa, ctx_va;
	uint32_t ctx_alloc; // VRAM bytes of the context image allocation
	bool up;
};

// gf100_gr_chan_new + gf100_gr_chan_bind + gk104_ectx_ctor. Needs
// gr->golden (call pg106_gr_golden first).
int pg106_grctx_chan_create(struct pg106_gr *gr, const struct pg106_grctx_map *m,
			    struct pg106_grctx_chan *c);
void pg106_grctx_chan_destroy(struct pg106_gr *gr, const struct pg106_grctx_map *m,
			      struct pg106_grctx_chan *c);
// The 64-bit word for inst+0x210 (gk104_ectx_bind): ctx_va | 4.
uint64_t pg106_grctx_chan_ectx(const struct pg106_grctx_chan *c);

// gf100_gr_wait_idle (gf100.c:1050-1076): 0 or -EAGAIN after 2 s.
int pg106_gr_wait_idle(struct pg106_gr *gr);

// gp102_grctx_generate_attrib_cb_size (ctxgp102.c:236-247) from the
// topology alone (what pg106_grboot_info.attrib_cb_size also computes).
uint32_t pg106_gr_attrib_cb_size(const struct pg106_gr *gr);

// ---- GR self-test: real GPU work on a render channel -----------------------------

struct pg106_gpu;

struct pg106_gr_selftest {
	int ret;
	const char *failed_at;  // step name, NULL on success
	int runlist;            // GR's hardware runlist
	uint32_t chid;
	uint32_t golden_size;   // FECS image size
	uint64_t golden_us;     // time spent generating the golden image
	const char *golden_failed_at;
	uint32_t fecs_stat;     // 0x409800 at a golden failure
	uint64_t ctx_va;        // the channel's context image VA
	uint32_t mmio_pairs;
	uint32_t sem_value;     // QMD RELEASE0 of the EXIT kernel, read back
	uint64_t sem_wait_us;
	uint32_t host_sem;      // the host semaphore release after it
	uint32_t store_value;   // what the store kernel wrote (want STORE_MAGIC)
	uint64_t store_wait_us;
	uint32_t gp_get;
	uint32_t gr_intr;       // 0x400100 after the run
	uint32_t fecs_intr;     // 0x409c18
	uint32_t fifo_intr;     // 0x002100
	uint32_t gr_status;     // 0x400700
	uint32_t mmu_faults, pbdma_errors;
	uint64_t nonstall_events;
};

#define PG106_GR_SELFTEST_SEM   0x6a5e0001u
#define PG106_GR_SELFTEST_HOST  0x6a5e0002u
#define PG106_GR_SELFTEST_STORE 0x6a5ec0deu

// On GR's runlist, with the golden context generated through FECS:
// a channel whose VMM maps 1 MiB of VRAM plus the context objects;
// submission 1 = SET_OBJECT(PASCAL_COMPUTE_B) + NVK's compute state +
// a QMD dispatch of a one-thread EXIT kernel whose RELEASE0 writes
// PG106_GR_SELFTEST_SEM, then a host semaphore release of _HOST and a
// NON_STALL_INTERRUPT; submission 2 = a kernel that stores _STORE
// through a constant-buffer pointer (MOV c[0][0x140..], MOV32I, STG.E,
// EXIT). Each is polled through PRAMIN for up to timeout_us. The
// mock GPU cannot run SASS: there submission 2 reports "store kernel".
int pg106_gpu_gr_selftest(struct pg106_gpu *g, const struct pg106_gr_fw *fw,
			  uint32_t timeout_us, struct pg106_gr_selftest *res);

#ifdef __cplusplus
}
#endif

#endif // PASCAL_GR_CTX_H
