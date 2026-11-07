/*
 * Copyright © 2026 the nvidia-macos-driver Pascal port contributors
 * SPDX-License-Identifier: MIT
 *
 * BOs: one bridge BO handle (the GEM-handle analogue) plus a VA bound
 * over the whole BO, exactly the shape of create_mem_or_close_bo in
 * nvkmd_nouveau_mem.c.
 */

#include "nvkmd_pg106.h"

#include "util/bitscan.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "vk_log.h"

#include <errno.h>

/* The bridge flags are defined to the nvkmd_mem_flags values
 * (pg106_br_wire.h); keep it that way at compile time. */
static_assert(PG106_BR_BO_LOCAL    == NVKMD_MEM_LOCAL,    "flag value");
static_assert(PG106_BR_BO_GART     == NVKMD_MEM_GART,     "flag value");
static_assert(PG106_BR_BO_VRAM     == NVKMD_MEM_VRAM,     "flag value");
static_assert(PG106_BR_BO_CAN_MAP  == NVKMD_MEM_CAN_MAP,  "flag value");
static_assert(PG106_BR_BO_SHARED   == NVKMD_MEM_SHARED,   "flag value");
static_assert(PG106_BR_BO_COHERENT == NVKMD_MEM_COHERENT, "flag value");

#define PG106_BR_BO_ALL (PG106_BR_BO_LOCAL | PG106_BR_BO_GART | \
                         PG106_BR_BO_VRAM | PG106_BR_BO_CAN_MAP | \
                         PG106_BR_BO_SHARED | PG106_BR_BO_COHERENT)

uint32_t
nvkmd_pg106_bo_flags(enum nvkmd_mem_flags flags)
{
   /* Anything beyond the six upstream bits is a macOS-patch extension
    * (NVKMD_MEM_BAR, nvk-macos.patch nvkmd.h) the Pascal kext has no
    * notion of: VRAM is always CPU-reachable through BAR1 when CAN_MAP
    * is set. */
   return (uint32_t)flags & PG106_BR_BO_ALL;
}

static VkResult
create_mem(struct nvkmd_pg106_dev *dev, struct vk_object_base *log_obj,
           enum nvkmd_mem_flags flags, uint32_t handle, bool imported,
           void *host_ptr, uint64_t size_B, uint64_t align_B,
           enum nvkmd_va_flags va_flags, struct nvkmd_mem **mem_out)
{
   VkResult result;

   struct nvkmd_pg106_mem *mem = CALLOC_STRUCT(nvkmd_pg106_mem);
   if (mem == NULL) {
      result = vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_bo;
   }

   align_B = MAX2(dev->base.pdev->bind_align_B, align_B);
   nvkmd_mem_init(&dev->base, &mem->base, &nvkmd_pg106_mem_ops,
                  flags, size_B, align_B);
   mem->handle = handle;
   mem->imported = imported;
   mem->host_ptr = host_ptr;
   simple_mtx_init(&mem->cpu_mutex, mtx_plain);

   result = nvkmd_dev_alloc_va(&dev->base, log_obj, va_flags,
                               0 /* pte_kind */, size_B, align_B,
                               0 /* fixed_addr */, &mem->base.va);
   if (result != VK_SUCCESS)
      goto fail_mem;

   result = nvkmd_va_bind_mem(mem->base.va, log_obj, 0, &mem->base, 0, size_B);
   if (result != VK_SUCCESS)
      goto fail_va;

   *mem_out = &mem->base;
   return VK_SUCCESS;

fail_va:
   nvkmd_va_free(mem->base.va);
fail_mem:
   simple_mtx_destroy(&mem->cpu_mutex);
   FREE(mem);
fail_bo:
   simple_mtx_lock(&dev->br_mutex);
   pg106_br_bo_free(dev->br, handle);
   simple_mtx_unlock(&dev->br_mutex);
   return result;
}

VkResult
nvkmd_pg106_alloc_mem(struct nvkmd_dev *_dev,
                      struct vk_object_base *log_obj,
                      uint64_t size_B, uint64_t align_B,
                      enum nvkmd_mem_flags flags,
                      struct nvkmd_mem **mem_out)
{
   struct nvkmd_pg106_dev *dev = nvkmd_pg106_dev(_dev);

   /* Only one placement flag may be specified (as nvkmd/nouveau). */
   assert(util_bitcount(flags & NVKMD_MEM_PLACEMENT_FLAGS) == 1);

   assert(util_is_power_of_two_or_zero64(align_B));
   align_B = MAX2(align_B, _dev->pdev->bind_align_B);
   size_B = align64(size_B, align_B);

   /* Discrete GPU: CPU maps are system memory or WB BAR1 VRAM, both
    * coherent on x86 (the nouveau rule, nvkmd_nouveau_mem.c). */
   flags |= NVKMD_MEM_COHERENT;

   uint32_t handle = 0;
   simple_mtx_lock(&dev->br_mutex);
   int err = pg106_br_bo_alloc(dev->br, size_B, align_B,
                               nvkmd_pg106_bo_flags(flags), &handle);
   simple_mtx_unlock(&dev->br_mutex);
   if (err != 0) {
      return nvkmd_pg106_result(log_obj, err, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                                "BO alloc");
   }

   enum nvkmd_va_flags va_flags = 0;
   if (flags & NVKMD_MEM_GART)
      va_flags |= NVKMD_VA_GART;

   return create_mem(dev, log_obj, flags, handle, false, NULL,
                     size_B, align_B, va_flags, mem_out);
}

VkResult
nvkmd_pg106_alloc_tiled_mem(struct nvkmd_dev *dev,
                            struct vk_object_base *log_obj,
                            uint64_t size_B, uint64_t align_B,
                            uint8_t pte_kind, uint16_t tile_mode,
                            enum nvkmd_mem_flags flags,
                            struct nvkmd_mem **mem_out)
{
   /* has_alloc_tiled is false: NVK puts the PTE kind on the VA bind,
    * never on the BO. nvk-macos.patch still tries a compressed tiled
    * BO for some images and falls back to a plain one on failure
    * (nvk_device_memory.c), which is what this answer selects. */
   if (pte_kind == 0 && tile_mode == 0)
      return nvkmd_pg106_alloc_mem(dev, log_obj, size_B, align_B,
                                   flags, mem_out);
   return VK_ERROR_FEATURE_NOT_PRESENT;
}

VkResult
nvkmd_pg106_import_dma_buf(struct nvkmd_dev *dev,
                           struct vk_object_base *log_obj,
                           int fd, struct nvkmd_mem **mem_out)
{
   /* has_dma_buf is false; macOS has no dma-buf. */
   return vk_error(log_obj, VK_ERROR_INVALID_EXTERNAL_HANDLE);
}

VkResult
nvkmd_pg106_import_host_mem(struct nvkmd_dev *_dev,
                            struct vk_object_base *log_obj,
                            void *host_ptr, uint64_t size_B,
                            enum nvkmd_mem_flags flags,
                            struct nvkmd_mem **mem_out)
{
#ifdef PG106_BR_HAS_BO_IMPORT_HOST
   struct nvkmd_pg106_dev *dev = nvkmd_pg106_dev(_dev);
   const uint64_t page = _dev->pdev->bind_align_B;

   /* VK_EXT_external_memory_host: the app's pointer and size must be
    * aligned to minImportedHostPointerAlignment (the page size here);
    * the kext refuses anything else with -EINVAL. */
   if (((uintptr_t)host_ptr & (page - 1)) || (size_B & (page - 1)) ||
       size_B == 0)
      return vk_error(log_obj, VK_ERROR_INVALID_EXTERNAL_HANDLE);

   flags = (flags & ~NVKMD_MEM_PLACEMENT_FLAGS) | NVKMD_MEM_GART |
           NVKMD_MEM_CAN_MAP | NVKMD_MEM_COHERENT;

   uint32_t handle = 0;
   simple_mtx_lock(&dev->br_mutex);
   int err = pg106_br_bo_import_host(dev->br, host_ptr, size_B,
                                     nvkmd_pg106_bo_flags(flags), &handle);
   simple_mtx_unlock(&dev->br_mutex);
   if (err == -EINVAL || err == -EFAULT)
      return vk_error(log_obj, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   if (err != 0) {
      return nvkmd_pg106_result(log_obj, err, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                                "host import");
   }

   /* The kext wires the range for the BO lifetime (NOUVEAU-UAPI.md
    * section 6.1.4) and the VA bind below maps those pages. */
   return create_mem(dev, log_obj, flags, handle, true, host_ptr,
                     size_B, page, NVKMD_VA_GART, mem_out);
#else
   return vk_error(log_obj, VK_ERROR_INVALID_EXTERNAL_HANDLE);
#endif
}

static void
nvkmd_pg106_mem_free(struct nvkmd_mem *_mem)
{
   struct nvkmd_pg106_mem *mem = nvkmd_pg106_mem(_mem);
   struct nvkmd_pg106_dev *dev = nvkmd_pg106_dev(_mem->dev);

   /* Unbind first: the bridge refuses to free a BO that is still bound
    * (-EBUSY, the unbind-before-release rule, NOUVEAU-UAPI.md 6.1.3). */
   nvkmd_va_free(mem->base.va);

   simple_mtx_lock(&dev->br_mutex);
   int err = pg106_br_bo_free(dev->br, mem->handle);
   simple_mtx_unlock(&dev->br_mutex);
   if (err != 0)
      nvkmd_pg106_result(NULL, err, VK_ERROR_UNKNOWN, "BO free");

   simple_mtx_destroy(&mem->cpu_mutex);
   FREE(mem);
}

static VkResult
nvkmd_pg106_mem_map(struct nvkmd_mem *_mem,
                    struct vk_object_base *log_obj,
                    enum nvkmd_mem_map_flags map_flags,
                    void *fixed_addr,
                    void **map_out)
{
   struct nvkmd_pg106_mem *mem = nvkmd_pg106_mem(_mem);
   struct nvkmd_pg106_dev *dev = nvkmd_pg106_dev(_mem->dev);

   if (map_flags & NVKMD_MEM_MAP_FIXED)
      return vk_error(log_obj, VK_ERROR_MEMORY_MAP_FAILED);

   if (mem->imported) {
      *map_out = mem->host_ptr;
      return VK_SUCCESS;
   }

   VkResult result = VK_SUCCESS;
   simple_mtx_lock(&mem->cpu_mutex);
   if (mem->cpu_users == 0) {
      void *ptr = NULL;
      simple_mtx_lock(&dev->br_mutex);
      int err = pg106_br_bo_map(dev->br, mem->handle,
                                nvkmd_pg106_bo_flags(mem->base.flags), &ptr);
      simple_mtx_unlock(&dev->br_mutex);
      if (err != 0) {
         result = nvkmd_pg106_result(log_obj, err, VK_ERROR_MEMORY_MAP_FAILED,
                                     "BO map");
      } else {
         mem->cpu_map = ptr;
      }
   }
   if (result == VK_SUCCESS) {
      mem->cpu_users++;
      *map_out = mem->cpu_map;
   }
   simple_mtx_unlock(&mem->cpu_mutex);

   return result;
}

static void
nvkmd_pg106_mem_unmap(struct nvkmd_mem *_mem,
                      enum nvkmd_mem_map_flags flags,
                      void *map)
{
   struct nvkmd_pg106_mem *mem = nvkmd_pg106_mem(_mem);
   struct nvkmd_pg106_dev *dev = nvkmd_pg106_dev(_mem->dev);

   if (mem->imported)
      return;

   simple_mtx_lock(&mem->cpu_mutex);
   assert(mem->cpu_users > 0 && map == mem->cpu_map);
   if (--mem->cpu_users == 0) {
      simple_mtx_lock(&dev->br_mutex);
      pg106_br_bo_unmap(dev->br, mem->handle);
      simple_mtx_unlock(&dev->br_mutex);
      mem->cpu_map = NULL;
   }
   simple_mtx_unlock(&mem->cpu_mutex);
}

static uint32_t
nvkmd_pg106_mem_log_handle(struct nvkmd_mem *_mem)
{
   return nvkmd_pg106_mem(_mem)->handle;
}

const struct nvkmd_mem_ops nvkmd_pg106_mem_ops = {
   .free = nvkmd_pg106_mem_free,
   .map = nvkmd_pg106_mem_map,
   .unmap = nvkmd_pg106_mem_unmap,
   .overmap = NULL,          /* has_overmap = false */
   .export_dma_buf = NULL,   /* has_dma_buf = false */
   .log_handle = nvkmd_pg106_mem_log_handle,
};
