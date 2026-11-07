/*
 * Copyright © 2026 the nvidia-macos-driver Pascal port contributors
 * SPDX-License-Identifier: MIT
 *
 * vk_sync for the pg106 backend: a value known on the CPU plus a short
 * list of (context timeline, sequence) -> value points that the GPU will
 * complete. Binary and timeline in one type, like vk_drm_syncobj. It
 * supports WAIT_PENDING, which the runtime's ASSISTED timeline mode needs
 * of every GPU-waitable type (vk_device.c get_timeline_mode).
 */

#include "nvkmd_pg106.h"

#include "util/os_time.h"
#include "util/u_math.h"

#include <sched.h>

static struct nvkmd_pg106_sync *
to_pg106_sync(struct vk_sync *sync)
{
   assert(vk_sync_type_is_nvkmd_pg106(sync->type));
   return container_of(sync, struct nvkmd_pg106_sync, base);
}

static bool
is_timeline(const struct nvkmd_pg106_sync *sync)
{
   return sync->base.flags & VK_SYNC_IS_TIMELINE;
}

/* Binary syncs count 0/1: any wait is a wait for 1. */
static uint64_t
target_value(const struct nvkmd_pg106_sync *sync, uint64_t value)
{
   return is_timeline(sync) ? value : 1;
}

static void
drop_point_locked(struct nvkmd_pg106_sync *sync, uint32_t i)
{
   nvkmd_pg106_timeline_unref(sync->pending[i].tl);
   sync->pending[i] = sync->pending[--sync->pending_count];
}

/* Fold completed points into value. Returns true if a point's context
 * was lost (that point is dropped and will never complete). */
static bool
reap_locked(struct nvkmd_pg106_sync *sync)
{
   bool lost = false;
   for (uint32_t i = 0; i < sync->pending_count;) {
      struct nvkmd_pg106_sync_point *p = &sync->pending[i];
      if (nvkmd_pg106_timeline_done(p->tl, p->seq)) {
         sync->value = MAX2(sync->value, p->value);
         drop_point_locked(sync, i);
      } else if (p->tl->lost) {
         lost = true;
         drop_point_locked(sync, i);
      } else {
         i++;
      }
   }
   return lost;
}

static bool
reachable_locked(const struct nvkmd_pg106_sync *sync, uint64_t target)
{
   if (sync->value >= target)
      return true;
   for (uint32_t i = 0; i < sync->pending_count; i++) {
      if (sync->pending[i].value >= target)
         return true;
   }
   return false;
}

static VkResult
nvkmd_pg106_sync_init(struct vk_device *device, struct vk_sync *_sync,
                      uint64_t initial_value)
{
   struct nvkmd_pg106_sync *sync = to_pg106_sync(_sync);

   simple_mtx_init(&sync->mutex, mtx_plain);
   sync->value = is_timeline(sync) ? initial_value : (initial_value != 0);
   sync->pending_count = 0;
   return VK_SUCCESS;
}

void
nvkmd_pg106_sync_finish(struct vk_device *device, struct vk_sync *_sync)
{
   struct nvkmd_pg106_sync *sync = to_pg106_sync(_sync);

   while (sync->pending_count > 0)
      drop_point_locked(sync, sync->pending_count - 1);
   simple_mtx_destroy(&sync->mutex);
}

VkResult
nvkmd_pg106_sync_signal_value(struct vk_sync *_sync, uint64_t value)
{
   struct nvkmd_pg106_sync *sync = to_pg106_sync(_sync);

   simple_mtx_lock(&sync->mutex);
   if (is_timeline(sync))
      sync->value = MAX2(sync->value, value);
   else
      sync->value = 1;
   simple_mtx_unlock(&sync->mutex);
   return VK_SUCCESS;
}

static VkResult
nvkmd_pg106_sync_signal(struct vk_device *device, struct vk_sync *sync,
                        uint64_t value)
{
   return nvkmd_pg106_sync_signal_value(sync, value);
}

static VkResult
nvkmd_pg106_sync_get_value(struct vk_device *device, struct vk_sync *_sync,
                           uint64_t *value)
{
   struct nvkmd_pg106_sync *sync = to_pg106_sync(_sync);

   simple_mtx_lock(&sync->mutex);
   const bool lost = reap_locked(sync);
   *value = sync->value;
   simple_mtx_unlock(&sync->mutex);
   return lost ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}

static VkResult
nvkmd_pg106_sync_reset(struct vk_device *device, struct vk_sync *_sync)
{
   struct nvkmd_pg106_sync *sync = to_pg106_sync(_sync);

   simple_mtx_lock(&sync->mutex);
   while (sync->pending_count > 0)
      drop_point_locked(sync, sync->pending_count - 1);
   sync->value = 0;
   simple_mtx_unlock(&sync->mutex);
   return VK_SUCCESS;
}

VkResult
nvkmd_pg106_sync_add_point(struct vk_sync *_sync,
                           struct nvkmd_pg106_timeline *tl,
                           uint64_t seq, uint64_t value)
{
   struct nvkmd_pg106_sync *sync = to_pg106_sync(_sync);
   VkResult result = VK_SUCCESS;

   simple_mtx_lock(&sync->mutex);
   reap_locked(sync);
   if (sync->pending_count == NVKMD_PG106_SYNC_MAX_PENDING) {
      /* Wait for the oldest point instead of growing without bound. */
      struct nvkmd_pg106_sync_point *p = &sync->pending[0];
      result = nvkmd_pg106_timeline_wait(p->tl, p->seq,
         os_time_get_absolute_timeout(nvkmd_pg106_default_timeout_ns()));
      reap_locked(sync);
      if (result != VK_SUCCESS && sync->pending_count ==
                                  NVKMD_PG106_SYNC_MAX_PENDING) {
         simple_mtx_unlock(&sync->mutex);
         return VK_ERROR_DEVICE_LOST;
      }
   }

   sync->pending[sync->pending_count++] = (struct nvkmd_pg106_sync_point) {
      .tl = nvkmd_pg106_timeline_ref(tl),
      .seq = seq,
      .value = is_timeline(sync) ? value : 1,
   };
   simple_mtx_unlock(&sync->mutex);
   return result == VK_TIMEOUT ? VK_SUCCESS : result;
}

VkResult
nvkmd_pg106_sync_gpu_target(struct vk_sync *_sync, uint64_t value,
                            struct nvkmd_pg106_timeline **tl_out,
                            uint64_t *seq_out)
{
   struct nvkmd_pg106_sync *sync = to_pg106_sync(_sync);
   const uint64_t target = target_value(sync, value);

   *tl_out = NULL;
   *seq_out = 0;

   simple_mtx_lock(&sync->mutex);
   const bool lost = reap_locked(sync);
   if (sync->value >= target) {
      simple_mtx_unlock(&sync->mutex);
      return VK_SUCCESS;
   }

   /* The earliest point that gets there. For a binary sync every point
    * is 1, so this is the oldest outstanding signal, which is the one a
    * wait consumes. */
   const struct nvkmd_pg106_sync_point *best = NULL;
   for (uint32_t i = 0; i < sync->pending_count; i++) {
      const struct nvkmd_pg106_sync_point *p = &sync->pending[i];
      if (p->value >= target && (best == NULL || p->value < best->value))
         best = p;
   }

   VkResult result = VK_SUCCESS;
   if (best != NULL) {
      *tl_out = nvkmd_pg106_timeline_ref(best->tl);
      *seq_out = best->seq;
   } else {
      result = lost ? VK_ERROR_DEVICE_LOST : VK_ERROR_UNKNOWN;
   }
   simple_mtx_unlock(&sync->mutex);
   return result;
}

/* One poll: 1 = satisfied, 0 = not yet, <0 = lost. */
static int
poll_one(struct vk_sync *_sync, uint64_t value, bool pending_only)
{
   struct nvkmd_pg106_sync *sync = to_pg106_sync(_sync);
   const uint64_t target = target_value(sync, value);

   simple_mtx_lock(&sync->mutex);
   const bool lost = reap_locked(sync);
   const bool ok = pending_only ? reachable_locked(sync, target)
                                : sync->value >= target;
   simple_mtx_unlock(&sync->mutex);

   if (ok)
      return 1;
   return lost ? -1 : 0;
}

static VkResult
wait_many(uint32_t wait_count, const struct vk_sync_wait *waits,
          enum vk_sync_wait_flags wait_flags, uint64_t abs_timeout_ns)
{
   const bool pending_only = wait_flags & VK_SYNC_WAIT_PENDING;
   const bool any = wait_flags & VK_SYNC_WAIT_ANY;
   uint32_t spins = 0;
   uint64_t sleep_us = 10;

   for (;;) {
      uint32_t done = 0;
      for (uint32_t i = 0; i < wait_count; i++) {
         const int r = poll_one(waits[i].sync, waits[i].wait_value,
                                pending_only);
         if (r < 0)
            return VK_ERROR_DEVICE_LOST;
         if (r > 0) {
            if (any)
               return VK_SUCCESS;
            done++;
         }
      }
      if (done == wait_count)
         return VK_SUCCESS;

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

VkResult
nvkmd_pg106_sync_wait_value(struct vk_sync *sync, uint64_t value,
                            uint64_t abs_timeout_ns)
{
   const struct vk_sync_wait w = { .sync = sync, .wait_value = value };
   return wait_many(1, &w, VK_SYNC_WAIT_COMPLETE, abs_timeout_ns);
}

static VkResult
nvkmd_pg106_sync_wait_many(struct vk_device *device, uint32_t wait_count,
                           const struct vk_sync_wait *waits,
                           enum vk_sync_wait_flags wait_flags,
                           uint64_t abs_timeout_ns)
{
   return wait_many(wait_count, waits, wait_flags, abs_timeout_ns);
}

struct vk_sync_type
nvkmd_pg106_sync_get_type(void)
{
   return (struct vk_sync_type) {
      .size = sizeof(struct nvkmd_pg106_sync),
      .features = VK_SYNC_FEATURE_BINARY |
                  VK_SYNC_FEATURE_TIMELINE |
                  VK_SYNC_FEATURE_GPU_WAIT |
                  VK_SYNC_FEATURE_GPU_MULTI_WAIT |
                  VK_SYNC_FEATURE_CPU_WAIT |
                  VK_SYNC_FEATURE_CPU_RESET |
                  VK_SYNC_FEATURE_CPU_SIGNAL |
                  VK_SYNC_FEATURE_WAIT_ANY |
                  VK_SYNC_FEATURE_WAIT_PENDING,
      .init = nvkmd_pg106_sync_init,
      .finish = nvkmd_pg106_sync_finish,
      .signal = nvkmd_pg106_sync_signal,
      .get_value = nvkmd_pg106_sync_get_value,
      .reset = nvkmd_pg106_sync_reset,
      .wait_many = nvkmd_pg106_sync_wait_many,
   };
}
