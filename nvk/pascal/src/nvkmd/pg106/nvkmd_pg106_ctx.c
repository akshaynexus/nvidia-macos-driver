/*
 * Copyright © 2026 the nvidia-macos-driver Pascal port contributors
 * SPDX-License-Identifier: MIT
 *
 * Exec and bind contexts.
 *
 * Exec: NVK's pushes go to EXEC unchanged (nvkmd_nouveau_exec_ctx_exec).
 * Around them the context adds two kinds of control pushes from its own
 * GART ring: SEMAPHORE ACQ_GEQ for GPU waits on other contexts, and one
 * WFI + SYSMEMBAR + SEMAPHORE release of the next sequence number at the
 * end of every flush. The bridge EXEC therefore never carries syncobjs.
 *
 * Bind: the sparse-binding queue. The bridge applies a VM_BIND job before
 * it returns, so waits are resolved on the CPU before the job is sent and
 * signals are CPU signals after it (nvkmd_nouveau_bind_ctx gets the same
 * ordering from the kernel's job scheduler).
 */

#include "nvkmd_pg106.h"

#include "nv_push.h"
#include "util/os_time.h"
#include "util/u_debug.h"
#include "util/u_memory.h"
#include "vk_log.h"

#include "clc06f.h"

#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Control-push encoders                                               */
/* ------------------------------------------------------------------ */

/* Host (channel) methods are accepted on any subchannel; nouveau's fence
 * code uses 0. Incrementing method header: NVC0_FIFO_PKHDR_SQ
 * (nv_push.h), the encoding ENCODING-RESOLUTION.md settles for GP106. */
#define HOST_SUBC 0

uint32_t
nvkmd_pg106_emit_acquire(uint32_t *dw, uint64_t addr, uint32_t value)
{
   /* SEMAPHOREA..D, clc06f.h:75-108. ACQ_GEQ compares the 32-bit word
    * against the payload; ACQUIRE_SWITCH lets the scheduler run other
    * channels while this one waits. */
   dw[0] = NVC0_FIFO_PKHDR_SQ(HOST_SUBC, NVC06F_SEMAPHOREA, 4);
   dw[1] = (uint32_t)(addr >> 32) & 0xff;            /* OFFSET_UPPER 7:0 */
   dw[2] = (uint32_t)addr & ~3u;                     /* OFFSET_LOWER 31:2 */
   dw[3] = value;
   dw[4] = NVC06F_SEMAPHORED_OPERATION_ACQ_GEQ |
           (NVC06F_SEMAPHORED_ACQUIRE_SWITCH_ENABLED << 12);
   return NVKMD_PG106_ACQUIRE_DW;
}

uint32_t
nvkmd_pg106_emit_release(uint32_t *dw, uint64_t addr, uint32_t value)
{
   /* WFI so every engine on the channel has finished the job's work,
    * then a system membar so its writes to system memory are visible
    * before the CPU can observe the semaphore (the ordering behind
    * Linux 2cb66ae604, "Membar before between semaphore writes and the
    * interrupt"), then a 4-byte release with WFI. */
   dw[0] = NVC0_FIFO_PKHDR_SQ(HOST_SUBC, NVC06F_WFI, 1);
   dw[1] = NVC06F_WFI_SCOPE_ALL;
   dw[2] = NVC0_FIFO_PKHDR_SQ(HOST_SUBC, NVC06F_MEM_OP_A, 4);
   dw[3] = 0;                                        /* MEM_OP_A */
   dw[4] = 0;                                        /* MEM_OP_B */
   dw[5] = NVC06F_MEM_OP_C_MEMBAR_TYPE_SYS_MEMBAR;   /* MEM_OP_C */
   dw[6] = NVC06F_MEM_OP_D_OPERATION_MEMBAR << 27;   /* MEM_OP_D 31:27 */
   dw[7] = NVC0_FIFO_PKHDR_SQ(HOST_SUBC, NVC06F_SEMAPHOREA, 4);
   dw[8] = (uint32_t)(addr >> 32) & 0xff;
   dw[9] = (uint32_t)addr & ~3u;
   dw[10] = value;
   dw[11] = NVC06F_SEMAPHORED_OPERATION_RELEASE |
            (NVC06F_SEMAPHORED_RELEASE_WFI_EN << 20) |
            (NVC06F_SEMAPHORED_RELEASE_SIZE_4BYTE << 24);
   return NVKMD_PG106_RELEASE_DW;
}

/* ------------------------------------------------------------------ */
/* Timelines                                                           */
/* ------------------------------------------------------------------ */

uint64_t
nvkmd_pg106_default_timeout_ns(void)
{
   /* Read every time: it is only consulted on slow paths (a sync, a
    * busy ring slot), and tests change it at run time. */
   int64_t ms = debug_get_num_option("NVK_PG106_TIMEOUT_MS", 10000);
   return (uint64_t)(ms > 0 ? ms : 10000) * 1000000ull;
}

struct nvkmd_pg106_timeline *
nvkmd_pg106_timeline_ref(struct nvkmd_pg106_timeline *tl)
{
   p_atomic_inc(&tl->refcnt);
   return tl;
}

void
nvkmd_pg106_timeline_unref(struct nvkmd_pg106_timeline *tl)
{
   if (!p_atomic_dec_zero(&tl->refcnt))
      return;

   nvkmd_mem_unref(tl->mem);
   FREE(tl);
}

VkResult
nvkmd_pg106_timeline_wait(struct nvkmd_pg106_timeline *tl, uint64_t seq,
                          uint64_t abs_timeout_ns)
{
   /* Bounded poll with backoff: spin briefly, then sleep 10 us doubling
    * to 1 ms (the EVENT-WAIT.md polling policy; Pascal has no RM event
    * objects to block on, OFFLOAD-DESIGN.md section 2.3). */
   uint32_t spins = 0;
   uint64_t sleep_us = 10;
   for (;;) {
      if (nvkmd_pg106_timeline_done(tl, seq))
         return VK_SUCCESS;
      if (tl->lost)
         return VK_ERROR_DEVICE_LOST;
      if (abs_timeout_ns != UINT64_MAX && os_time_get_nano() >= abs_timeout_ns)
         return VK_TIMEOUT;

      if (spins < 64) {
         spins++;
         sched_yield();
      } else {
         os_time_sleep(sleep_us);
         if (sleep_us < 1000)
            sleep_us *= 2;
      }
   }
}

static uint64_t
abs_timeout_from_now(uint64_t rel_ns)
{
   const uint64_t now = os_time_get_nano();
   return rel_ns > UINT64_MAX - now ? UINT64_MAX : now + rel_ns;
}

/* ------------------------------------------------------------------ */
/* Exec context                                                        */
/* ------------------------------------------------------------------ */

static VkResult nvkmd_pg106_exec_ctx_flush(struct nvkmd_ctx *_ctx,
                                           struct vk_object_base *log_obj);

static uint32_t *
ctrl_slot_ptr(struct nvkmd_pg106_exec_ctx *ctx, uint32_t slot)
{
   return (uint32_t *)((char *)ctx->ctrl_map + NVKMD_PG106_CTRL_SLOT0 +
                       (size_t)slot * NVKMD_PG106_CTRL_SLOT_SIZE);
}

static uint64_t
ctrl_slot_addr(struct nvkmd_pg106_exec_ctx *ctx, uint32_t slot)
{
   return ctx->ctrl_addr + NVKMD_PG106_CTRL_SLOT0 +
          (uint64_t)slot * NVKMD_PG106_CTRL_SLOT_SIZE;
}

/* Take the next control slot. A slot is free once the flush that last
 * carried it has completed; that flush's sequence is recorded in
 * slot_seq. The new user is the flush that will carry this slot, which
 * is always the next one (sequence submitted + 1). */
static VkResult
ctrl_slot_get(struct nvkmd_pg106_exec_ctx *ctx, struct vk_object_base *log_obj,
              uint32_t *slot_out)
{
   const uint32_t slot = ctx->next_slot;
   const uint64_t prev = ctx->slot_seq[slot];

   if (prev != 0 && !nvkmd_pg106_timeline_done(ctx->tl, prev)) {
      VkResult result = nvkmd_pg106_timeline_wait(ctx->tl, prev,
         abs_timeout_from_now(nvkmd_pg106_default_timeout_ns()));
      if (result != VK_SUCCESS) {
         ctx->tl->lost = true;
         return vk_errorf(log_obj, VK_ERROR_DEVICE_LOST,
                          "pg106: control ring slot %u still busy "
                          "(sequence %" PRIu64 ")", slot, prev);
      }
   }

   ctx->slot_seq[slot] = ctx->tl->submitted + 1;
   ctx->next_slot = (slot + 1) % NVKMD_PG106_CTRL_SLOTS;
   *slot_out = slot;
   return VK_SUCCESS;
}

static VkResult
queue_push(struct nvkmd_pg106_exec_ctx *ctx, struct vk_object_base *log_obj,
           uint64_t addr, uint32_t size_B, bool no_prefetch)
{
   struct nvkmd_pg106_dev *dev = nvkmd_pg106_dev(ctx->base.dev);
   const struct pg106_uapi_exec_push push = {
      .va = addr,
      .va_len = size_B,
      .flags = no_prefetch ? PG106_UAPI_EXEC_PUSH_NO_PREFETCH : 0,
   };

   simple_mtx_lock(&dev->br_mutex);
   int err = pg106_br_ctx_exec(ctx->br, &push);
   simple_mtx_unlock(&dev->br_mutex);
   if (err != 0)
      return nvkmd_pg106_result(log_obj, err, VK_ERROR_UNKNOWN, "EXEC queue");

   ctx->queued_pushes++;
   return VK_SUCCESS;
}

/* Queue the open acquire slot, if any, ahead of whatever comes next. */
static VkResult
close_acquires(struct nvkmd_pg106_exec_ctx *ctx, struct vk_object_base *log_obj)
{
   if (ctx->acq_dw == 0)
      return VK_SUCCESS;

   const uint32_t slot = ctx->acq_slot;
   const uint32_t size_B = ctx->acq_dw * 4;
   ctx->acq_dw = 0;
   return queue_push(ctx, log_obj, ctrl_slot_addr(ctx, slot), size_B, false);
}

static VkResult
nvkmd_pg106_create_exec_ctx(struct nvkmd_pg106_dev *dev,
                            struct vk_object_base *log_obj,
                            enum nvkmd_engines engines,
                            struct nvkmd_ctx **ctx_out)
{
   VkResult result;

   struct nvkmd_pg106_exec_ctx *ctx = CALLOC_STRUCT(nvkmd_pg106_exec_ctx);
   if (ctx == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   ctx->base.ops = &nvkmd_pg106_exec_ctx_ops;
   ctx->base.dev = &dev->base;

   struct nvkmd_pg106_timeline *tl = CALLOC_STRUCT(nvkmd_pg106_timeline);
   if (tl == NULL) {
      result = vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_ctx;
   }
   tl->refcnt = 1;

   result = nvkmd_dev_alloc_mapped_mem(&dev->base, log_obj,
                                       NVKMD_PG106_CTRL_BO_SIZE, 4096,
                                       NVKMD_MEM_GART, NVKMD_MEM_MAP_RDWR,
                                       &tl->mem);
   if (result != VK_SUCCESS)
      goto fail_tl;

   ctx->ctrl_map = tl->mem->map;
   ctx->ctrl_addr = tl->mem->va->addr;
   tl->word = (volatile uint32_t *)ctx->ctrl_map;
   tl->gpu_addr = ctx->ctrl_addr;
   *tl->word = 0;
   ctx->tl = tl;

   result = nvkmd_pg106_dev_open_br_ctx(dev, log_obj, &ctx->br);
   if (result != VK_SUCCESS)
      goto fail_mem;

   *ctx_out = &ctx->base;
   return VK_SUCCESS;

fail_mem:
   nvkmd_mem_unref(tl->mem);
fail_tl:
   FREE(tl);
fail_ctx:
   FREE(ctx);
   return result;
}

static void
nvkmd_pg106_exec_ctx_destroy(struct nvkmd_ctx *_ctx)
{
   struct nvkmd_pg106_exec_ctx *ctx = nvkmd_pg106_exec_ctx(_ctx);

   /* NVK destroys contexts idle, but the control BO must outlive any
    * release still in flight, so make sure. */
   if (ctx->tl->submitted != 0 && !ctx->tl->lost) {
      (void)nvkmd_pg106_timeline_wait(ctx->tl, ctx->tl->submitted,
         abs_timeout_from_now(nvkmd_pg106_default_timeout_ns()));
   }

   pg106_br_ctx_destroy(ctx->br);
   nvkmd_pg106_timeline_unref(ctx->tl);
   FREE(ctx);
}

static VkResult
nvkmd_pg106_exec_ctx_wait(struct nvkmd_ctx *_ctx,
                          struct vk_object_base *log_obj,
                          uint32_t wait_count,
                          const struct vk_sync_wait *waits)
{
   struct nvkmd_pg106_exec_ctx *ctx = nvkmd_pg106_exec_ctx(_ctx);
   const uint32_t slot_dw = NVKMD_PG106_CTRL_SLOT_SIZE / 4;

   if (ctx->tl->lost)
      return VK_ERROR_DEVICE_LOST;

   for (uint32_t i = 0; i < wait_count; i++) {
      struct nvkmd_pg106_timeline *tl = NULL;
      uint64_t seq = 0;
      VkResult result = nvkmd_pg106_sync_gpu_target(waits[i].sync,
                                                    waits[i].wait_value,
                                                    &tl, &seq);
      if (result != VK_SUCCESS)
         return vk_error(log_obj, result);
      if (tl == NULL)
         continue; /* already complete */

      /* Our own earlier flushes are ahead of us on the channel. */
      if (tl == ctx->tl) {
         nvkmd_pg106_timeline_unref(tl);
         continue;
      }

      if (ctx->acq_dw + NVKMD_PG106_ACQUIRE_DW > slot_dw || ctx->acq_dw == 0) {
         result = close_acquires(ctx, log_obj);
         if (result == VK_SUCCESS)
            result = ctrl_slot_get(ctx, log_obj, &ctx->acq_slot);
         if (result != VK_SUCCESS) {
            nvkmd_pg106_timeline_unref(tl);
            return result;
         }
      }

      uint32_t *dw = ctrl_slot_ptr(ctx, ctx->acq_slot) + ctx->acq_dw;
      ctx->acq_dw += nvkmd_pg106_emit_acquire(dw, tl->gpu_addr, (uint32_t)seq);

      /* The waited-on context's control BO stays mapped while that
       * context exists; NVK only destroys contexts once idle. */
      nvkmd_pg106_timeline_unref(tl);
   }

   return VK_SUCCESS;
}

static VkResult
nvkmd_pg106_exec_ctx_exec(struct nvkmd_ctx *_ctx,
                          struct vk_object_base *log_obj,
                          uint32_t exec_count,
                          const struct nvkmd_ctx_exec *execs)
{
   struct nvkmd_pg106_exec_ctx *ctx = nvkmd_pg106_exec_ctx(_ctx);
   struct nvkmd_pg106_dev *dev = nvkmd_pg106_dev(ctx->base.dev);
   VkResult result;

   if (ctx->tl->lost)
      return VK_ERROR_DEVICE_LOST;

   result = close_acquires(ctx, log_obj);
   if (result != VK_SUCCESS)
      return result;

   for (uint32_t i = 0; i < exec_count; i++) {
      /* Same splitting rule as nvkmd_nouveau_exec_ctx_exec: a run of
       * incomplete pushes and the push that completes it go in one EXEC.
       * One slot per EXEC is kept for the end-of-flush release. */
      uint32_t incomplete_count = 0;
      for (uint32_t j = i; j < exec_count && execs[j].incomplete; j++) {
         assert(j < exec_count - 1);
         incomplete_count++;
      }
      assert(incomplete_count + 2 <= dev->exec_push_max);

      if (ctx->queued_pushes + incomplete_count + 2 > dev->exec_push_max) {
         result = nvkmd_pg106_exec_ctx_flush(&ctx->base, log_obj);
         if (result != VK_SUCCESS)
            return result;
      }

      /* Hardware limits (nvkmd_nouveau_ctx.c:202-203): GPFIFO entries
       * hold a dword address and a dword count below 2^23 bytes. */
      assert((execs[i].addr % 4) == 0 && (execs[i].size_B % 4) == 0);
      assert(execs[i].size_B < (1u << 23));

      result = queue_push(ctx, log_obj, execs[i].addr, execs[i].size_B,
                          execs[i].no_prefetch);
      if (result != VK_SUCCESS)
         return result;
      ctx->queued_user++;
   }

   return VK_SUCCESS;
}

static VkResult
nvkmd_pg106_exec_ctx_flush(struct nvkmd_ctx *_ctx,
                           struct vk_object_base *log_obj)
{
   struct nvkmd_pg106_exec_ctx *ctx = nvkmd_pg106_exec_ctx(_ctx);
   struct nvkmd_pg106_dev *dev = nvkmd_pg106_dev(ctx->base.dev);
   VkResult result;

   if (ctx->tl->lost)
      return VK_ERROR_DEVICE_LOST;

   if (ctx->queued_pushes == 0 && ctx->acq_dw == 0 && ctx->signal_count == 0)
      return VK_SUCCESS;

   result = close_acquires(ctx, log_obj);
   if (result != VK_SUCCESS)
      return result;

   uint32_t slot;
   result = ctrl_slot_get(ctx, log_obj, &slot);
   if (result != VK_SUCCESS)
      return result;

   const uint64_t seq = ctx->tl->submitted + 1;
   const uint32_t dw = nvkmd_pg106_emit_release(ctrl_slot_ptr(ctx, slot),
                                                ctx->tl->gpu_addr,
                                                (uint32_t)seq);
   result = queue_push(ctx, log_obj, ctrl_slot_addr(ctx, slot), dw * 4, false);
   if (result != VK_SUCCESS)
      return result;

   simple_mtx_lock(&dev->br_mutex);
   int err = pg106_br_ctx_flush(ctx->br);
   simple_mtx_unlock(&dev->br_mutex);
   if (err != 0) {
      /* The bridge sent nothing or the kext refused the job: the release
       * will never come, so everything waiting on this context fails. */
      ctx->tl->lost = true;
      ctx->signal_count = 0;
      return nvkmd_pg106_result(log_obj, err, VK_ERROR_DEVICE_LOST, "EXEC");
   }

   ctx->tl->submitted = seq;
   ctx->queued_pushes = 0;
   ctx->queued_user = 0;

   for (uint32_t i = 0; i < ctx->signal_count; i++) {
      const struct vk_sync_signal *s = &ctx->signals[i];
      result = nvkmd_pg106_sync_add_point(s->sync, ctx->tl, seq,
                                          s->signal_value);
      if (result != VK_SUCCESS) {
         ctx->signal_count = 0;
         return vk_error(log_obj, result);
      }
   }
   ctx->signal_count = 0;

   return VK_SUCCESS;
}

static VkResult
nvkmd_pg106_exec_ctx_signal(struct nvkmd_ctx *_ctx,
                            struct vk_object_base *log_obj,
                            uint32_t signal_count,
                            const struct vk_sync_signal *signals)
{
   struct nvkmd_pg106_exec_ctx *ctx = nvkmd_pg106_exec_ctx(_ctx);

   for (uint32_t i = 0; i < signal_count; i++) {
      assert(vk_sync_type_is_nvkmd_pg106(signals[i].sync->type));
      if (ctx->signal_count == NVKMD_PG106_MAX_SIGNALS) {
         /* Everything queued so far is in this flush, so signalling at
          * its end is still after the work the signal covers. */
         VkResult result = nvkmd_pg106_exec_ctx_flush(&ctx->base, log_obj);
         if (result != VK_SUCCESS)
            return result;
      }
      ctx->signals[ctx->signal_count++] = signals[i];
   }

   return nvkmd_pg106_exec_ctx_flush(&ctx->base, log_obj);
}

static VkResult
nvkmd_pg106_exec_ctx_sync(struct nvkmd_ctx *_ctx,
                          struct vk_object_base *log_obj)
{
   struct nvkmd_pg106_exec_ctx *ctx = nvkmd_pg106_exec_ctx(_ctx);

   VkResult result = nvkmd_pg106_exec_ctx_flush(&ctx->base, log_obj);
   if (result != VK_SUCCESS)
      return result;

   if (ctx->tl->submitted == 0)
      return VK_SUCCESS;

   result = nvkmd_pg106_timeline_wait(ctx->tl, ctx->tl->submitted,
      abs_timeout_from_now(nvkmd_pg106_default_timeout_ns()));
   if (result == VK_TIMEOUT) {
      ctx->tl->lost = true;
      return vk_errorf(log_obj, VK_ERROR_DEVICE_LOST,
                       "pg106: sequence %" PRIu64 " did not complete within "
                       "NVK_PG106_TIMEOUT_MS", ctx->tl->submitted);
   }
   return result == VK_SUCCESS ? VK_SUCCESS : vk_error(log_obj, result);
}

const struct nvkmd_ctx_ops nvkmd_pg106_exec_ctx_ops = {
   .destroy = nvkmd_pg106_exec_ctx_destroy,
   .wait = nvkmd_pg106_exec_ctx_wait,
   .exec = nvkmd_pg106_exec_ctx_exec,
   .signal = nvkmd_pg106_exec_ctx_signal,
   .flush = nvkmd_pg106_exec_ctx_flush,
   .sync = nvkmd_pg106_exec_ctx_sync,
};

/* ------------------------------------------------------------------ */
/* Bind context                                                        */
/* ------------------------------------------------------------------ */

static VkResult nvkmd_pg106_bind_ctx_flush(struct nvkmd_ctx *_ctx,
                                           struct vk_object_base *log_obj);

static VkResult
nvkmd_pg106_create_bind_ctx(struct nvkmd_pg106_dev *dev,
                            struct vk_object_base *log_obj,
                            struct nvkmd_ctx **ctx_out)
{
   struct nvkmd_pg106_bind_ctx *ctx = CALLOC_STRUCT(nvkmd_pg106_bind_ctx);
   if (ctx == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   ctx->base.ops = &nvkmd_pg106_bind_ctx_ops;
   ctx->base.dev = &dev->base;

   /* VM_BIND has no channel; channel 0 is only the bridge's handle. */
   simple_mtx_lock(&dev->br_mutex);
   int err = pg106_br_ctx_create(dev->br, 0, &ctx->br);
   simple_mtx_unlock(&dev->br_mutex);
   if (err != 0) {
      FREE(ctx);
      return nvkmd_pg106_result(log_obj, err, VK_ERROR_INITIALIZATION_FAILED,
                                "bind context");
   }

   *ctx_out = &ctx->base;
   return VK_SUCCESS;
}

static void
nvkmd_pg106_bind_ctx_destroy(struct nvkmd_ctx *_ctx)
{
   struct nvkmd_pg106_bind_ctx *ctx = nvkmd_pg106_bind_ctx(_ctx);

   pg106_br_ctx_destroy(ctx->br);
   FREE(ctx);
}

static VkResult
nvkmd_pg106_bind_ctx_wait(struct nvkmd_ctx *_ctx,
                          struct vk_object_base *log_obj,
                          uint32_t wait_count,
                          const struct vk_sync_wait *waits)
{
   struct nvkmd_pg106_bind_ctx *ctx = nvkmd_pg106_bind_ctx(_ctx);

   for (uint32_t i = 0; i < wait_count; i++) {
      if (ctx->wait_count == NVKMD_PG106_MAX_SIGNALS) {
         VkResult result = nvkmd_pg106_bind_ctx_flush(&ctx->base, log_obj);
         if (result != VK_SUCCESS)
            return result;
      }
      ctx->waits[ctx->wait_count++] = waits[i];
   }

   return VK_SUCCESS;
}

static VkResult
nvkmd_pg106_bind_ctx_bind(struct nvkmd_ctx *_ctx,
                          struct vk_object_base *log_obj,
                          uint32_t bind_count,
                          const struct nvkmd_ctx_bind *binds)
{
   struct nvkmd_pg106_bind_ctx *ctx = nvkmd_pg106_bind_ctx(_ctx);
   struct nvkmd_pg106_dev *dev = nvkmd_pg106_dev(ctx->base.dev);

   for (uint32_t i = 0; i < bind_count; i++) {
      if (ctx->queued == PG106_BR_MAX_BINDS) {
         VkResult result = nvkmd_pg106_bind_ctx_flush(&ctx->base, log_obj);
         if (result != VK_SUCCESS)
            return result;
      }

      /* nvkmd_nouveau_ctx.c:391-400 */
      struct pg106_uapi_vm_bind_op op = {
         .addr = binds[i].va->addr + binds[i].va_offset_B,
         .range = binds[i].range_B,
      };
      if (binds[i].op == NVKMD_BIND_OP_BIND) {
         op.op = PG106_UAPI_VM_BIND_OP_MAP;
         op.flags = binds[i].va->pte_kind;
         op.handle = nvkmd_pg106_mem(binds[i].mem)->handle;
         op.bo_offset = binds[i].mem_offset_B;
      } else {
         op.op = PG106_UAPI_VM_BIND_OP_UNMAP;
      }

      simple_mtx_lock(&dev->br_mutex);
      int err = pg106_br_ctx_bind(ctx->br, &op);
      simple_mtx_unlock(&dev->br_mutex);
      if (err != 0)
         return nvkmd_pg106_result(log_obj, err, VK_ERROR_UNKNOWN,
                                   "VM_BIND queue");
      ctx->queued++;
   }

   return VK_SUCCESS;
}

static VkResult
nvkmd_pg106_bind_ctx_flush(struct nvkmd_ctx *_ctx,
                           struct vk_object_base *log_obj)
{
   struct nvkmd_pg106_bind_ctx *ctx = nvkmd_pg106_bind_ctx(_ctx);
   struct nvkmd_pg106_dev *dev = nvkmd_pg106_dev(ctx->base.dev);

   /* Waits first: the job must not be applied before them. They are
    * pending by now (the runtime resolves wait-before-signal with
    * WAIT_PENDING before it submits), so they complete. */
   const uint64_t deadline =
      abs_timeout_from_now(nvkmd_pg106_default_timeout_ns());
   for (uint32_t i = 0; i < ctx->wait_count; i++) {
      VkResult result = nvkmd_pg106_sync_wait_value(ctx->waits[i].sync,
                                                    ctx->waits[i].wait_value,
                                                    deadline);
      if (result != VK_SUCCESS) {
         ctx->wait_count = 0;
         return vk_errorf(log_obj, result == VK_TIMEOUT ?
                          VK_ERROR_DEVICE_LOST : result,
                          "pg106: sparse bind wait failed");
      }
   }
   ctx->wait_count = 0;

   if (ctx->queued == 0)
      return VK_SUCCESS;

   simple_mtx_lock(&dev->br_mutex);
   int err = pg106_br_ctx_flush(ctx->br);
   simple_mtx_unlock(&dev->br_mutex);
   ctx->queued = 0;

   return nvkmd_pg106_result(log_obj, err, VK_ERROR_UNKNOWN, "VM_BIND");
}

static VkResult
nvkmd_pg106_bind_ctx_signal(struct nvkmd_ctx *_ctx,
                            struct vk_object_base *log_obj,
                            uint32_t signal_count,
                            const struct vk_sync_signal *signals)
{
   VkResult result = nvkmd_pg106_bind_ctx_flush(_ctx, log_obj);
   if (result != VK_SUCCESS)
      return result;

   /* The job is applied once the flush returns. */
   for (uint32_t i = 0; i < signal_count; i++) {
      result = nvkmd_pg106_sync_signal_value(signals[i].sync,
                                             signals[i].signal_value);
      if (result != VK_SUCCESS)
         return vk_error(log_obj, result);
   }

   return VK_SUCCESS;
}

static VkResult
nvkmd_pg106_bind_ctx_sync(struct nvkmd_ctx *_ctx,
                          struct vk_object_base *log_obj)
{
   return nvkmd_pg106_bind_ctx_flush(_ctx, log_obj);
}

const struct nvkmd_ctx_ops nvkmd_pg106_bind_ctx_ops = {
   .destroy = nvkmd_pg106_bind_ctx_destroy,
   .wait = nvkmd_pg106_bind_ctx_wait,
   .bind = nvkmd_pg106_bind_ctx_bind,
   .signal = nvkmd_pg106_bind_ctx_signal,
   .flush = nvkmd_pg106_bind_ctx_flush,
   .sync = nvkmd_pg106_bind_ctx_sync,
};

VkResult
nvkmd_pg106_create_ctx(struct nvkmd_dev *_dev,
                       struct vk_object_base *log_obj,
                       enum nvkmd_engines engines,
                       struct nvkmd_ctx **ctx_out)
{
   struct nvkmd_pg106_dev *dev = nvkmd_pg106_dev(_dev);

   if (engines == NVKMD_ENGINE_BIND)
      return nvkmd_pg106_create_bind_ctx(dev, log_obj, ctx_out);

   /* No NVDEC on this path (has_video is false). */
   if (engines & (NVKMD_ENGINE_BIND | NVKMD_ENGINE_VDEC))
      return vk_error(log_obj, VK_ERROR_FEATURE_NOT_PRESENT);

   return nvkmd_pg106_create_exec_ctx(dev, log_obj, engines, ctx_out);
}
