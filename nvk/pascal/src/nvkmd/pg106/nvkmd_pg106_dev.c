/*
 * Copyright © 2026 the nvidia-macos-driver Pascal port contributors
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd_pg106.h"

#include "util/u_memory.h"
#include "vk_log.h"

#include <errno.h>
#include <stdio.h>

VkResult
nvkmd_pg106_create_dev(struct nvkmd_pdev *_pdev,
                       struct vk_object_base *log_obj,
                       struct nvkmd_dev **dev_out)
{
   struct nvkmd_pg106_dev *dev = CALLOC_STRUCT(nvkmd_pg106_dev);
   if (dev == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   dev->base.ops = &nvkmd_pg106_dev_ops;
   dev->base.pdev = _pdev;
   dev->base.va_start = 0;
   dev->base.va_end = NVKMD_PG106_KERNEL_START;

   list_inithead(&dev->base.mems);
   simple_mtx_init(&dev->base.mems_mutex, mtx_plain);
   simple_mtx_init(&dev->br_mutex, mtx_plain);
   simple_mtx_init(&dev->heap_mutex, mtx_plain);

   VkResult result;
   int err = nvkmd_pg106_transport_open(&dev->tr);
   if (err != 0) {
      result = nvkmd_pg106_result(log_obj, err, VK_ERROR_INITIALIZATION_FAILED,
                                  "transport open");
      goto fail_mutex;
   }

   err = pg106_br_dev_open(&dev->tr, &dev->br);
   if (err != 0) {
      result = nvkmd_pg106_result(log_obj, err, VK_ERROR_INITIALIZATION_FAILED,
                                  "bridge open");
      goto fail_transport;
   }

   /* The same reservation the nouveau winsys makes
    * (nouveau_device.c:508-515): the kernel manages [2^39, 2^40). */
   err = pg106_br_vm_init(dev->br, NVKMD_PG106_KERNEL_START,
                          NVKMD_PG106_KERNEL_END - NVKMD_PG106_KERNEL_START);
   if (err == -EBUSY) {
      /* The v0 kext has one hardware channel; the connection that did
       * VM_INIT first holds it until it closes. */
      result = vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                         "pg106: the GP106 is in use by another process "
                         "(one render client at a time)");
      goto fail_br;
   } else if (err != 0) {
      result = nvkmd_pg106_result(log_obj, err, VK_ERROR_INITIALIZATION_FAILED,
                                  "VM_INIT");
      goto fail_br;
   }

   uint64_t push_max = 0;
   err = pg106_br_getparam(dev->br, PG106_UAPI_GETPARAM_EXEC_PUSH_MAX,
                           &push_max);
   if (err != 0 || push_max < 3) {
      result = nvkmd_pg106_result(log_obj, err ? err : -EINVAL,
                                  VK_ERROR_INITIALIZATION_FAILED,
                                  "GETPARAM EXEC_PUSH_MAX");
      goto fail_br;
   }
   dev->exec_push_max = (uint32_t)MIN2(push_max, UINT32_MAX);

   /* VM_BIND carries no channel; the VA context sits on channel 0 and
    * does not use up a channel number. */
   err = pg106_br_ctx_create(dev->br, 0, &dev->va_br);
   if (err != 0) {
      result = nvkmd_pg106_result(log_obj, err, VK_ERROR_INITIALIZATION_FAILED,
                                  "VA context");
      goto fail_br;
   }

   util_vma_heap_init(&dev->heap, NVKMD_PG106_HEAP_START,
                      NVKMD_PG106_HEAP_END - NVKMD_PG106_HEAP_START);
   util_vma_heap_init(&dev->replay_heap, NVKMD_PG106_REPLAY_HEAP_START,
                      NVKMD_PG106_REPLAY_HEAP_END -
                      NVKMD_PG106_REPLAY_HEAP_START);

   *dev_out = &dev->base;
   return VK_SUCCESS;

fail_br:
   pg106_br_dev_close(dev->br);
fail_transport:
   nvkmd_pg106_transport_close(&dev->tr);
fail_mutex:
   simple_mtx_destroy(&dev->heap_mutex);
   simple_mtx_destroy(&dev->br_mutex);
   simple_mtx_destroy(&dev->base.mems_mutex);
   FREE(dev);
   return result;
}

VkResult
nvkmd_pg106_dev_open_br_ctx(struct nvkmd_pg106_dev *dev,
                            struct vk_object_base *log_obj,
                            struct pg106_br_ctx **ctx_out)
{
   simple_mtx_lock(&dev->br_mutex);

   int err = pg106_br_ctx_create(dev->br, dev->next_channel, ctx_out);
   if (err == 0) {
      dev->next_channel++;
   } else if (err == -ENOENT && dev->next_channel != 0) {
      /* The kext has no channel of that number (the skeleton only has
       * channel 0, CHANNEL_ALLOC is not in the bridge yet): share 0. */
      err = pg106_br_ctx_create(dev->br, 0, ctx_out);
      if (err == 0 && !dev->shared_channel_warned) {
         dev->shared_channel_warned = true;
         fprintf(stderr, "pg106: the kext offers one channel; NVK's "
                         "contexts share it (submission order is global)\n");
      }
   }

   simple_mtx_unlock(&dev->br_mutex);

   return nvkmd_pg106_result(log_obj, err, VK_ERROR_INITIALIZATION_FAILED,
                             "channel open");
}

static void
nvkmd_pg106_dev_destroy(struct nvkmd_dev *_dev)
{
   struct nvkmd_pg106_dev *dev = nvkmd_pg106_dev(_dev);

   util_vma_heap_finish(&dev->heap);
   util_vma_heap_finish(&dev->replay_heap);

   pg106_br_ctx_destroy(dev->va_br);
   /* Closing the connection releases every kext object it still owns
    * (pg106_br_dev_close tears CPU maps down first). */
   pg106_br_dev_close(dev->br);
   nvkmd_pg106_transport_close(&dev->tr);

   simple_mtx_destroy(&dev->heap_mutex);
   simple_mtx_destroy(&dev->br_mutex);
   simple_mtx_destroy(&dev->base.mems_mutex);
   FREE(dev);
}

static VkResult
nvkmd_pg106_dev_get_gpu_timestamp_checked(struct nvkmd_dev *_dev,
                                          uint64_t *timestamp)
{
   struct nvkmd_pg106_dev *dev = nvkmd_pg106_dev(_dev);

   simple_mtx_lock(&dev->br_mutex);
   int err = pg106_br_getparam(dev->br, PG106_UAPI_GETPARAM_PTIMER_TIME,
                               timestamp);
   simple_mtx_unlock(&dev->br_mutex);

   return err == 0 ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
}

static uint64_t
nvkmd_pg106_dev_get_gpu_timestamp(struct nvkmd_dev *_dev)
{
   uint64_t ts = 0;
   if (nvkmd_pg106_dev_get_gpu_timestamp_checked(_dev, &ts) != VK_SUCCESS)
      return 0;
   return ts;
}

const struct nvkmd_dev_ops nvkmd_pg106_dev_ops = {
   .destroy = nvkmd_pg106_dev_destroy,
   .get_gpu_timestamp = nvkmd_pg106_dev_get_gpu_timestamp,
   .get_gpu_timestamp_checked = nvkmd_pg106_dev_get_gpu_timestamp_checked,
   .get_drm_fd = NULL,
   .alloc_mem = nvkmd_pg106_alloc_mem,
   .alloc_tiled_mem = nvkmd_pg106_alloc_tiled_mem,
   .import_dma_buf = nvkmd_pg106_import_dma_buf,
   .import_host_mem = nvkmd_pg106_import_host_mem,
   .alloc_va = nvkmd_pg106_alloc_va,
   .create_ctx = nvkmd_pg106_create_ctx,
};
