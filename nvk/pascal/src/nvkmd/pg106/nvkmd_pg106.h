/*
 * Copyright © 2026 the nvidia-macos-driver Pascal port contributors
 * SPDX-License-Identifier: MIT
 *
 * nvkmd backend for Pascal GP106 (GTX 1060 Mobile and its GP106 mobile
 * siblings) on top of the pg106 userspace bridge (pascal/bridge, MIT).
 *
 * The bridge carries the nouveau CS uapi subset (GETPARAM, VM_INIT,
 * VM_BIND, EXEC) plus BO lifecycle to the PG106 kext. There is no GSP on
 * Pascal, so nothing in here talks to RM objects, escapes, usermode
 * doorbells or work-submit tokens; that is the whole difference from the
 * nvkmd/nvrm backend in nvk-macos.patch.
 *
 * Completion tracking does not use kernel syncobjs. Every exec context
 * owns one small GART BO holding a 32-bit progress word and a ring of
 * control pushes. Each flush ends with a host SEMAPHORE release
 * (NVC06F_SEMAPHOREA..D, clc06f.h) of the context's next sequence
 * number; CPU waits poll the word, GPU waits are SEMAPHORE ACQ_GEQ on
 * another context's word. This is the "mapped GART word + poll" scheme
 * of research/evidence/OFFLOAD-DESIGN.md section 2.3.
 */
#ifndef NVKMD_PG106_H
#define NVKMD_PG106_H 1

#include "nvkmd/nvkmd.h"
#include "util/simple_mtx.h"
#include "util/vma.h"
#include "vk_sync.h"

#include "pg106_br_bridge.h"

#include <stdbool.h>
#include <stdint.h>

/* VA layout, identical to nvkmd/nouveau (nvkmd_nouveau.h,
 * nouveau_device.h:18) so NVK sees the same address space it sees on
 * Linux: [4 KiB, 2^38) general heap, [2^38, 2^39) capture/replay heap,
 * [2^39, 2^40) reserved for the kernel through VM_INIT. */
#define NVKMD_PG106_HEAP_START        ((uint64_t)4096)
#define NVKMD_PG106_HEAP_END          ((uint64_t)1 << 38)
#define NVKMD_PG106_REPLAY_HEAP_START NVKMD_PG106_HEAP_END
#define NVKMD_PG106_REPLAY_HEAP_END   ((uint64_t)1 << 39)
#define NVKMD_PG106_KERNEL_START      ((uint64_t)1 << 39)
#define NVKMD_PG106_KERNEL_END        ((uint64_t)1 << 40)

/* Control BO of an exec context: the progress word lives at offset 0,
 * the control-push ring follows. */
#define NVKMD_PG106_CTRL_BO_SIZE   (64u * 1024u)
#define NVKMD_PG106_CTRL_SLOT_SIZE 256u
#define NVKMD_PG106_CTRL_SLOT0     NVKMD_PG106_CTRL_SLOT_SIZE
#define NVKMD_PG106_CTRL_SLOTS \
   ((NVKMD_PG106_CTRL_BO_SIZE - NVKMD_PG106_CTRL_SLOT0) / NVKMD_PG106_CTRL_SLOT_SIZE)

/* Dwords one GPU wait costs in a control push (header + 4 data) and the
 * dwords of the end-of-flush release (WFI, SYSMEMBAR, release). */
#define NVKMD_PG106_ACQUIRE_DW 5u
#define NVKMD_PG106_RELEASE_DW 12u

/* ------------------------------------------------------------------ */
/* Transport selection                                                 */
/* ------------------------------------------------------------------ */

/* Opens one connection to the PG106 kext. On macOS this is the IOKit
 * user-client transport from pascal/bridge (pg106_br_iokit.h); anywhere
 * else only a test hook can provide one. Returns 0 or a negative errno
 * (-ENODEV when there is no PG106 device). */
int nvkmd_pg106_transport_open(struct pg106_br_transport *tr);
void nvkmd_pg106_transport_close(struct pg106_br_transport *tr);

/* Test seam: replaces the platform transport (Linux unit tests use a
 * loopback transport). Pass NULLs to restore the default. */
typedef int (*nvkmd_pg106_transport_open_fn)(struct pg106_br_transport *tr);
typedef void (*nvkmd_pg106_transport_close_fn)(struct pg106_br_transport *tr);
void nvkmd_pg106_set_transport_hook(nvkmd_pg106_transport_open_fn open_fn,
                                    nvkmd_pg106_transport_close_fn close_fn);

/* errno from the bridge -> VkResult, logged against log_obj. */
VkResult nvkmd_pg106_result(struct vk_object_base *log_obj, int err,
                            VkResult fallback, const char *what);

/* ------------------------------------------------------------------ */
/* Device identity                                                     */
/* ------------------------------------------------------------------ */

struct nvkmd_pg106_chip {
   uint16_t device_id;
   const char *name;
   uint16_t chipset;     /* PMC BOOT_0 arch|impl: 0x136 for GP106 */
   uint64_t vram_size_B; /* default; the board may differ */
};

/* The GP106 ids the PG106 kext gate accepts (pascal/kext/src/
 * pg106_device_gate.c) plus the GP106 desktop ids the bridge mock
 * reports. NULL for anything else. */
const struct nvkmd_pg106_chip *nvkmd_pg106_chip_lookup(uint16_t device_id);

/* Fill an nv_device_info for a GP106 the way the nouveau winsys does
 * (nouveau_device.c sm_for_chipset, max_*_for_sm, init_shared_mem_sizes)
 * from the chip entry and the bridge GETPARAMs. */
void nvkmd_pg106_fill_dev_info(struct nv_device_info *info,
                               const struct nvkmd_pg106_chip *chip,
                               uint64_t graph_units, uint64_t bar_size_B);

/* ------------------------------------------------------------------ */
/* pdev / dev                                                          */
/* ------------------------------------------------------------------ */

struct nvkmd_pg106_pdev {
   struct nvkmd_pdev base;

   /* The probe connection, kept for get_vram_used. */
   struct pg106_br_transport tr;
   struct pg106_br_dev *br;
   simple_mtx_t br_mutex;

   struct vk_sync_type sync_type;
   const struct vk_sync_type *sync_types[2];
};

NVKMD_DECL_SUBCLASS(pdev, pg106);

VkResult nvkmd_pg106_enum_pdev(struct vk_object_base *log_obj,
                               enum nvk_debug debug_flags,
                               nvkmd_enum_pdev_visitor visitor,
                               void *arg);

struct nvkmd_pg106_dev {
   struct nvkmd_dev base;

   /* One kext connection per nvkmd_dev, like one DRM fd per
    * nouveau_ws_device: VM_INIT is per connection. */
   struct pg106_br_transport tr;
   struct pg106_br_dev *br;

   /* Serializes every bridge call: the bridge keeps per-device tables
    * and is not thread-safe. */
   simple_mtx_t br_mutex;

   /* Bridge context used for the synchronous VA binds of
    * nvkmd_va_ops (nvkmd_nouveau_va.c issues one VM_BIND per call). */
   struct pg106_br_ctx *va_br;

   uint32_t exec_push_max;
   uint32_t next_channel;
   bool shared_channel_warned;

   simple_mtx_t heap_mutex;
   struct util_vma_heap heap;
   struct util_vma_heap replay_heap;
};

NVKMD_DECL_SUBCLASS(dev, pg106);

VkResult nvkmd_pg106_create_dev(struct nvkmd_pdev *pdev,
                                struct vk_object_base *log_obj,
                                struct nvkmd_dev **dev_out);

/* Open a bridge context, preferring a channel of its own. The skeleton
 * kext provisions only channel 0; further contexts then share it, which
 * is correct for NVK's upload queue (copy-only) next to one 3D queue but
 * not for two 3D queues (BRIDGE-NVK-REVIEW.md, "Channels"). */
VkResult nvkmd_pg106_dev_open_br_ctx(struct nvkmd_pg106_dev *dev,
                                     struct vk_object_base *log_obj,
                                     struct pg106_br_ctx **ctx_out);

/* ------------------------------------------------------------------ */
/* mem / va                                                            */
/* ------------------------------------------------------------------ */

struct nvkmd_pg106_mem {
   struct nvkmd_mem base;

   uint32_t handle;
   bool imported;   /* host-pointer import: the CPU pointer is host_ptr */
   void *host_ptr;

   /* The bridge allows one CPU mapping per BO; nvkmd can hold an
    * internal and a client map at once, so both share it. */
   simple_mtx_t cpu_mutex;
   void *cpu_map;
   uint32_t cpu_users;
};

NVKMD_DECL_SUBCLASS(mem, pg106);

VkResult nvkmd_pg106_alloc_mem(struct nvkmd_dev *dev,
                               struct vk_object_base *log_obj,
                               uint64_t size_B, uint64_t align_B,
                               enum nvkmd_mem_flags flags,
                               struct nvkmd_mem **mem_out);

VkResult nvkmd_pg106_alloc_tiled_mem(struct nvkmd_dev *dev,
                                     struct vk_object_base *log_obj,
                                     uint64_t size_B, uint64_t align_B,
                                     uint8_t pte_kind, uint16_t tile_mode,
                                     enum nvkmd_mem_flags flags,
                                     struct nvkmd_mem **mem_out);

VkResult nvkmd_pg106_import_dma_buf(struct nvkmd_dev *dev,
                                    struct vk_object_base *log_obj,
                                    int fd, struct nvkmd_mem **mem_out);

VkResult nvkmd_pg106_import_host_mem(struct nvkmd_dev *dev,
                                     struct vk_object_base *log_obj,
                                     void *host_ptr, uint64_t size_B,
                                     enum nvkmd_mem_flags flags,
                                     struct nvkmd_mem **mem_out);

/* Bridge placement flags for nvkmd flags (values are 1:1 for the six
 * upstream bits; nvk-macos.patch's NVKMD_MEM_BAR is dropped because the
 * Pascal kext has no separate BAR1 placement). */
uint32_t nvkmd_pg106_bo_flags(enum nvkmd_mem_flags flags);

struct nvkmd_pg106_va {
   struct nvkmd_va base;
};

NVKMD_DECL_SUBCLASS(va, pg106);

VkResult nvkmd_pg106_alloc_va(struct nvkmd_dev *dev,
                              struct vk_object_base *log_obj,
                              enum nvkmd_va_flags flags, uint8_t pte_kind,
                              uint64_t size_B, uint64_t align_B,
                              uint64_t fixed_addr, struct nvkmd_va **va_out);

/* ------------------------------------------------------------------ */
/* Timelines (per exec context progress word)                          */
/* ------------------------------------------------------------------ */

struct nvkmd_pg106_timeline {
   uint32_t refcnt;
   struct nvkmd_mem *mem;      /* the context's control BO */
   uint64_t gpu_addr;          /* GPU VA of the progress word */
   volatile uint32_t *word;    /* CPU view of the progress word */
   uint64_t submitted;         /* last sequence handed to the GPU */
   bool lost;                  /* a flush failed: waits report DEVICE_LOST */
};

struct nvkmd_pg106_timeline *
nvkmd_pg106_timeline_ref(struct nvkmd_pg106_timeline *tl);
void nvkmd_pg106_timeline_unref(struct nvkmd_pg106_timeline *tl);

/* True once the GPU has released `seq` (wrap-aware 32-bit compare; at
 * most 2^31 sequences may be outstanding, far above the ring size). */
static inline bool
nvkmd_pg106_timeline_done(const struct nvkmd_pg106_timeline *tl, uint64_t seq)
{
   const uint32_t word = *tl->word;
   return (int32_t)(word - (uint32_t)seq) >= 0;
}

/* CPU wait for `seq`, bounded by abs_timeout_ns (os_time_get_nano
 * clock). VK_TIMEOUT on expiry, VK_ERROR_DEVICE_LOST if lost. */
VkResult nvkmd_pg106_timeline_wait(struct nvkmd_pg106_timeline *tl,
                                   uint64_t seq, uint64_t abs_timeout_ns);

/* Default bound for nvkmd_ctx_sync and ring-slot reuse waits:
 * NVK_PG106_TIMEOUT_MS, else 10 s. A wait past it is device loss
 * (OFFLOAD-DESIGN.md section 2.5, "Fence wait exceeds bound"). */
uint64_t nvkmd_pg106_default_timeout_ns(void);

/* Encoders for the control pushes; exposed for the unit tests, which
 * replay them in the loopback GPU. Return the number of dwords. */
uint32_t nvkmd_pg106_emit_acquire(uint32_t *dw, uint64_t addr, uint32_t value);
uint32_t nvkmd_pg106_emit_release(uint32_t *dw, uint64_t addr, uint32_t value);

/* ------------------------------------------------------------------ */
/* Contexts                                                            */
/* ------------------------------------------------------------------ */

#define NVKMD_PG106_MAX_SIGNALS 64u

struct nvkmd_pg106_exec_ctx {
   struct nvkmd_ctx base;

   struct pg106_br_ctx *br;
   struct nvkmd_pg106_timeline *tl;
   uint32_t *ctrl_map;
   uint64_t ctrl_addr;

   uint32_t next_slot;
   uint64_t slot_seq[NVKMD_PG106_CTRL_SLOTS];

   /* Control slot the pending GPU waits are written into (0 = none). */
   uint32_t acq_slot;
   uint32_t acq_dw;

   uint32_t queued_pushes;  /* in the bridge ctx, incl. acquire slots */
   uint32_t queued_user;    /* NVK's own pushes */

   uint32_t signal_count;
   struct vk_sync_signal signals[NVKMD_PG106_MAX_SIGNALS];
};

NVKMD_DECL_SUBCLASS(ctx, pg106_exec);

struct nvkmd_pg106_bind_ctx {
   struct nvkmd_ctx base;

   struct pg106_br_ctx *br;
   uint32_t queued;

   uint32_t wait_count;
   struct vk_sync_wait waits[NVKMD_PG106_MAX_SIGNALS];
};

NVKMD_DECL_SUBCLASS(ctx, pg106_bind);

VkResult nvkmd_pg106_create_ctx(struct nvkmd_dev *dev,
                                struct vk_object_base *log_obj,
                                enum nvkmd_engines engines,
                                struct nvkmd_ctx **ctx_out);

/* ------------------------------------------------------------------ */
/* vk_sync                                                             */
/* ------------------------------------------------------------------ */

#define NVKMD_PG106_SYNC_MAX_PENDING 16u

struct nvkmd_pg106_sync {
   struct vk_sync base;

   simple_mtx_t mutex;
   uint64_t value;            /* highest value known complete */

   uint32_t pending_count;
   struct nvkmd_pg106_sync_point {
      struct nvkmd_pg106_timeline *tl;
      uint64_t seq;
      uint64_t value;
   } pending[NVKMD_PG106_SYNC_MAX_PENDING];
};

struct vk_sync_type nvkmd_pg106_sync_get_type(void);

void nvkmd_pg106_sync_finish(struct vk_device *device, struct vk_sync *sync);

static inline bool
vk_sync_type_is_nvkmd_pg106(const struct vk_sync_type *type)
{
   return type->finish == nvkmd_pg106_sync_finish;
}

/* Record that `tl` reaching `seq` makes `sync` reach `value` (binary
 * syncs: value 1). Blocks on the oldest point if all are in use. */
VkResult nvkmd_pg106_sync_add_point(struct vk_sync *sync,
                                    struct nvkmd_pg106_timeline *tl,
                                    uint64_t seq, uint64_t value);

/* What a GPU wait for `value` has to acquire: *tl_out NULL when the
 * value is already complete. VK_ERROR_UNKNOWN when nothing pending can
 * reach it (the runtime only submits such waits with WAIT_PENDING, which
 * this type does not advertise). The returned timeline is referenced. */
VkResult nvkmd_pg106_sync_gpu_target(struct vk_sync *sync, uint64_t value,
                                     struct nvkmd_pg106_timeline **tl_out,
                                     uint64_t *seq_out);

/* CPU-side wait used by the bind context and the vk_sync type. */
VkResult nvkmd_pg106_sync_wait_value(struct vk_sync *sync, uint64_t value,
                                     uint64_t abs_timeout_ns);
VkResult nvkmd_pg106_sync_signal_value(struct vk_sync *sync, uint64_t value);

#endif /* NVKMD_PG106_H */
