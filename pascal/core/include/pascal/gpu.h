// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/gpu.h — the GP106 as one object: bring-up
// (devboot, with the ACR memory in VRAM), VRAM allocator, BAR2/BAR1 VMMs,
// host FIFO and PMC interrupt routing; plus a self-test that executes GPU
// work (a host semaphore release, then a copy-engine copy).
//
// Used by the kext (pascal/kext/src/PG106.cpp) and the Linux userspace
// harness (pascal/tools/pg106_hwtest.c); tested in tests/test_gpu.c.
//
// pg106_gpu_boot order:
//   1. devboot up to POST (identify, posted, POST if needed): no VRAM use
//   2. VRAM size (pg106_fb_vram_size, the gm200/gp100 FBP walk; nouveau's
//      gf100_ram_ctor) -> allocator over [0, size - 1 MiB)
//   3. devboot again, whole sequence, with cfg->vram: FB init, ACR memory,
//      ACR, SEC2 RTOS, GR, BOOTSTRAP_FALCON, READY (or stop_after)
//   4. BAR2 then BAR1 (gf100_bar_oneinit order), FIFO (runlists from the
//      PTOP the boot read), interrupts: FIFO handler (non-stall)
// Step 4 runs whenever step 3 reached at least PG106_DB_TOP, so the copy
// engine path can be tested even when ACR or GR stop: the CE has no
// context and needs neither.
//
// C11, libc only. SIMULATION status: tests/test_gpu.c; not run on a GPU.

#ifndef PASCAL_GPU_H
#define PASCAL_GPU_H

#include <pascal/devboot.h>
#include <pascal/fifo.h>
#include <pascal/intr.h>
#include <pascal/vmm.h>
#include <pascal/vram.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pg106_gpu_cfg {
	const struct pg106_hw_bus *bus;
	const uint8_t *vbios;
	uint32_t vbios_size;
	const struct pg106_devboot_fw *fw;
	const struct pg106_devboot_hostmem *hostmem; // NULL: HS image in VRAM
	uint64_t acr_vma;          // 0 = nouveau's first-fit VA (0)
	uint64_t sysmem_flush_bus; // 0x100c10 page, 0 = leave
	uint64_t bar1_size;        // PCI BAR1 length (required)
	uint64_t bar2_size;        // PCI BAR3 length, 0 = no BAR2 VMM
	struct pg106_bar1_io bar1_io;
	uint8_t stop_after;        // devboot stop (PG106_DB_READY = all)
	pg106_devboot_event_fn event;
	void *event_user;
	uint32_t sec2_max_polls;
	int (*bootstrap)(void *user, uint64_t falcon_mask); // tests
	void *bootstrap_user;
};

struct pg106_gpu {
	struct pg106_gpu_cfg cfg;
	struct pg106_devboot db;
	bool db_valid;
	uint64_t vram_size;
	struct pg106_vram vram;
	bool vram_up;
	struct pg106_bar bar2, bar1;
	bool bar2_up, bar1_up;
	struct pg106_fifo fifo;
	bool fifo_up;
	struct pg106_intr intr;
	bool intr_up;
	// fence wakeups: bumped by the FIFO non-stall handler
	uint64_t nonstall_events;
	void (*nonstall_cb)(void *ctx);
	void *nonstall_ctx;
	int ret;        // boot result (devboot's, or the memory/FIFO step's)
	uint8_t stage;  // devboot stage reached
};

// Run the bring-up (see the file comment). Returns 0 when devboot reached
// cfg->stop_after and the memory + FIFO layers are up.
int pg106_gpu_boot(struct pg106_gpu *g, const struct pg106_gpu_cfg *cfg);
// Tear down everything (channels must be gone). Safe on a zeroed object.
void pg106_gpu_fini(struct pg106_gpu *g);
bool pg106_gpu_ready(const struct pg106_gpu *g); // READY + FIFO up

// Called by the FIFO interrupt handler when a non-stall interrupt fires.
void pg106_gpu_set_nonstall_cb(struct pg106_gpu *g, void (*cb)(void *),
			       void *ctx);

// ---- self-test: real GPU work -------------------------------------------------

struct pg106_gpu_selftest {
	int ret;
	const char *failed_at;     // step name, NULL on success
	int runlist;               // hardware runlist the copy channel used
	uint32_t chid;
	uint64_t sem_pa;           // VRAM address of the semaphore words
	uint32_t sem_value;        // read back after the host release
	uint64_t sem_wait_us;
	uint32_t ce_sem_value;     // read back after the CE release
	uint64_t ce_wait_us;
	uint32_t ce_bytes, ce_mismatch; // words that differ after the copy
	uint32_t gp_get;           // GPFIFO GET after both submissions
	uint32_t pfifo_intr;       // 0x2100 after the run
	uint64_t nonstall_events;
};

// On the copy engine's runlist: a channel whose VMM maps 1 MiB of VRAM
// with 64 KiB pages; submission 1 = SEMAPHOREA-D release (4 bytes,
// payload 0x5a5a0001) + NON_STALL_INTERRUPT; submission 2 = a 64 KiB
// PASCAL_DMA_COPY_B copy of a pattern + CE semaphore (0x5a5a0002). Each
// is polled through PRAMIN for up to timeout_us. Everything is freed.
int pg106_gpu_selftest(struct pg106_gpu *g, uint32_t timeout_us,
		       struct pg106_gpu_selftest *res);

#ifdef __cplusplus
}
#endif

#endif // PASCAL_GPU_H
