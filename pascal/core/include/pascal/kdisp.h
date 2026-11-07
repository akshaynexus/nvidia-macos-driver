// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/kdisp.h — kernel side of the bridge wire
// protocol (pascal/bridge/include/pg106_br_wire.h): one client's handles,
// GPU VM, channel ring and fences, driven one frame at a time.
//
// The kext's PG106UserClient is a thin wrapper: its SUBMIT selector hands
// the request bytes to pg106_kd_handle() and copies the response back
// (pascal/kext/src/PG106UserClient.cpp). Everything that can be decided
// without IOKit lives here and is tested on Linux end to end:
// libpg106_bridge -> loopback transport -> this dispatcher -> mock GPU
// (pascal/tests/test_kdisp.c).
//
// Every op is validated before any state changes, with the same rules
// the bridge applies (core/src/uapi.c validators, which encode the
// nouveau kernel's ioctl checks: nouveau_uvmm.c, nouveau_exec.c,
// nouveau_sched.c at Linux d48723711bae) — the kernel side never trusts
// that the caller is our bridge.
//
// What each op does here:
//   HANDSHAKE      version + ring capacity + drm_version + VA end
//   GETPARAM       nouveau_abi16_ioctl_getparam's answers
//                  (nouveau_abi16.c:299-327): PCI ids, GRAPH_UNITS
//                  (gf100_gr_units: gpc | tpc_total << 8 | rop << 32,
//                  gr/gf100.c:1161-1171), PTIMER_TIME, EXEC_PUSH_MAX,
//                  VRAM_BAR_SIZE, VRAM_USED (0: no VRAM BOs yet),
//                  HAS_VMA_TILEMODE (1, as nouveau)
//   VM_INIT        creates the client VM (core mmu) and maps the kernel's
//                  own objects (GPFIFO ring, fence semaphore, fence push
//                  slots) at the start of the kernel-managed region, then
//                  binds the channel (env->chan_bind)
//   BO_ALLOC/FREE  host pages from env->mem_alloc (the kext: an
//                  IOBufferMemoryDescriptor, wired). VRAM placement is
//                  not implemented: PG106_BR_BO_VRAM is served from host
//                  memory (documented stub, kext README)
//   BO_IMPORT_HOST env->mem_import wires the caller's range
//   BO_MAP/UNMAP   env->mem_map returns the cookie userspace maps
//   VM_BIND        drm_gpuvm-style split/merge table (kernel copy), then
//                  core mmu map/unmap of 4K pages with the op's PTE kind;
//                  sparse regions are tracked but no sparse PTEs are
//                  written (mmu.h has none: stub). Binds complete
//                  synchronously; their signal syncobjs get a signaled
//                  fence
//   EXEC           GPFIFO entries for the pushes (LEVEL_SUBROUTINE,
//                  NO_PREFETCH in bit 31: nvif/chan506f.c:22-26 via
//                  nouveau_exec.c:146), then a kernel push doing the
//                  nvc0 fence release (SEMAPHOREA-D RELEASE + WFI, 16-byte,
//                  nvc0_fence.c:35-55, clc06f.h 0x10-0x20), then the
//                  GP_PUT kick (env->kick = USERD + 0x8c). Waits need no
//                  GPU-side acquire: one channel, executed in order, and
//                  every fence a wait can name came from this channel or
//                  from a bind that already completed
//   SYNCOBJ_*      binary + timeline syncobjs over channel seqnos; WAIT
//                  polls the fence semaphore (no interrupts yet: stub)
//
// C11, libc (calloc/realloc/free) only; GPL-2.0-only like the rest of
// the kernel side. The wire header it includes is MIT (pascal/bridge).

#ifndef PASCAL_KDISP_H
#define PASCAL_KDISP_H

#include <pascal/mmu.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pg106_br_wire.h"

// Kernel objects mapped at the start of the kernel-managed VA region.
#define PG106_KD_FENCE_PUSH_WORDS 8u  // one nvc0_fence_emit32 + padding
#define PG106_KD_KERNEL_VA_SPAN   0x00200000ull // ring + fence + pushes fit

// The channel the kext must bind (instance block, RAMFC, runlist) once a
// client VM exists. Addresses are GPU VAs in the client VM except pdb.
struct pg106_kd_chan {
	uint64_t pdb_addr;      // pg106_mmu_root_addr()
	enum pg106_target pdb_target;
	uint64_t vm_limit;      // PG106_MMU_VA_LIMIT
	uint64_t ring_va;       // GPFIFO ring
	uint32_t ring_entries;  // == ib_max (power of two)
	uint64_t fence_va;      // fence semaphore (16-byte release target)
};

// What the dispatcher needs from its host. Required: mem_alloc, mem_free,
// mem_dma, mem_cpu, kick, delay_us, now_ns. Optional hooks may be NULL.
struct pg106_kd_env {
	void *user;
	int (*mem_alloc)(void *user, uint64_t size, uint64_t align,
			 uint32_t flags, void **mem);
	int (*mem_import)(void *user, uint64_t host_addr, uint64_t size,
			  uint32_t flags, void **mem); // NULL = -EOPNOTSUPP
	void (*mem_free)(void *user, void *mem);
	// GPU bus address of byte `off` of mem, and how many bytes from
	// there are contiguous (at least to the end of that 4K page).
	int (*mem_dma)(void *user, void *mem, uint64_t off, uint64_t *addr,
		       uint64_t *contig);
	// Kernel CPU view of mem (kernel-owned objects only).
	void *(*mem_cpu)(void *user, void *mem);
	// Userspace mapping cookie for mem (IOConnectMapMemory64 type).
	int (*mem_map)(void *user, void *mem, uint32_t handle, uint64_t *cookie);
	void (*mem_unmap)(void *user, void *mem, uint64_t cookie);
	// Channel binding; NULL = no channel (EXEC answers -ENODEV).
	int (*chan_bind)(void *user, const struct pg106_kd_chan *ch);
	void (*chan_unbind)(void *user);
	// Page tables changed (upload + TLB invalidate). NULL = model only.
	int (*vm_flush)(void *user, const struct pg106_mmu *mmu);
	void (*kick)(void *user, uint32_t gp_put);
	void (*delay_us)(void *user, uint32_t us);
	uint64_t (*now_ns)(void *user);
	uint64_t (*ptimer_ns)(void *user); // NULL = 0 (GETPARAM PTIMER_TIME)
	// GPU usable (bring-up done, powered). NULL = always.
	bool (*ready)(void *user);

	// Device facts.
	uint16_t pci_vendor, pci_device;
	uint64_t vram_bar_size; // BAR1 length (nouveau: resource_size(BAR1_FB))
	uint64_t vram_size;     // PG106_BR_GETPARAM_VRAM_SIZE
	uint32_t boot0;         // PG106_BR_GETPARAM_BOOT0
	uint64_t graph_units;
	uint32_t ib_max;       // GPFIFO entries, power of two >= 4
	uint32_t drm_version;  // what HANDSHAKE reports
	uint32_t abi_major, abi_minor;
	struct pg106_mmu_cfg mmu_cfg; // where the client VM's tables live
};

struct pg106_kd;

int pg106_kd_create(const struct pg106_kd_env *env, struct pg106_kd **out);
// Client teardown in the order NOUVEAU-UAPI.md section 6.1 rule 5
// requires: wait for in-flight work (bounded), unbind the channel, unmap
// everything, unwire/free every BO, free the kernel objects.
void pg106_kd_destroy(struct pg106_kd *kd);

// One request frame (header + payload) -> one response frame (header +
// payload) in resp (resp_cap >= sizeof header + PG106_BR_WIRE_MAX_RESP).
// Returns 0 when a response was written (its status carries the op
// result), or a negative errno when the request is not a frame at all
// (bad magic/version/length: the transport should fail the call).
int pg106_kd_handle(struct pg106_kd *kd, const void *req, size_t req_len,
		    void *resp, size_t resp_cap, size_t *resp_len);

// Poll the fence and update completion; returns the last completed seqno.
uint32_t pg106_kd_poll(struct pg106_kd *kd);

// Introspection for tests and GET_STATUS.
const struct pg106_mmu *pg106_kd_mmu(const struct pg106_kd *kd);
uint32_t pg106_kd_seqno_emitted(const struct pg106_kd *kd);
size_t pg106_kd_bo_count(const struct pg106_kd *kd);
bool pg106_kd_va_lookup(const struct pg106_kd *kd, uint64_t va,
			uint32_t *handle, uint64_t *bo_offset);

#endif // PASCAL_KDISP_H
