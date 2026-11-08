/*
 * Copyright © 2026 the nvidia-macos-driver Pascal port contributors
 * SPDX-License-Identifier: MIT
 *
 * GPU VA: userspace-managed like nvkmd/nouveau (util_vma_heap, same
 * heaps), every bind one synchronous VM_BIND job through the device's
 * VA bridge context (nvkmd_nouveau_va.c issues one ioctl per call).
 */

#include "nvkmd_pg106.h"

#include "util/u_math.h"
#include "util/u_memory.h"
#include "vk_log.h"

#include <inttypes.h>

static VkResult
alloc_heap_addr_locked(struct nvkmd_pg106_dev *dev,
                       struct vk_object_base *log_obj,
                       enum nvkmd_va_flags flags,
                       uint64_t size_B, uint64_t align_B,
                       uint64_t fixed_addr, uint64_t *addr_out)
{
   if (flags & NVKMD_VA_ALLOC_FIXED) {
      assert(flags & NVKMD_VA_REPLAY);
      if (fixed_addr < NVKMD_PG106_REPLAY_HEAP_START ||
          fixed_addr >= NVKMD_PG106_REPLAY_HEAP_END) {
         return vk_errorf(log_obj, VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS,
                          "Capture address 0x%" PRIx64 " not in the replay "
                          "heap", fixed_addr);
      }
      if (fixed_addr & (align_B - 1)) {
         return vk_errorf(log_obj, VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS,
                          "Unaligned capture address: 0x%" PRIx64, fixed_addr);
      }
      if (!util_vma_heap_alloc_addr(&dev->replay_heap, fixed_addr, size_B)) {
         return vk_errorf(log_obj, VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS,
                          "Replay address collision: 0x%" PRIx64, fixed_addr);
      }
      *addr_out = fixed_addr;
      return VK_SUCCESS;
   }

   struct util_vma_heap *heap =
      (flags & NVKMD_VA_REPLAY) ? &dev->replay_heap : &dev->heap;
   *addr_out = util_vma_heap_alloc(heap, size_B, align_B);
   if (*addr_out == 0) {
      return vk_errorf(log_obj, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "Failed to allocate virtual address range");
   }
   return VK_SUCCESS;
}

static void
free_heap_addr(struct nvkmd_pg106_dev *dev, enum nvkmd_va_flags flags,
               uint64_t addr, uint64_t size_B)
{
   simple_mtx_lock(&dev->heap_mutex);
   if (flags & NVKMD_VA_REPLAY)
      util_vma_heap_free(&dev->replay_heap, addr, size_B);
   else
      util_vma_heap_free(&dev->heap, addr, size_B);
   simple_mtx_unlock(&dev->heap_mutex);
}

/* One VM_BIND job with a single op, applied before return. */
static VkResult
vm_bind(struct nvkmd_pg106_dev *dev, struct vk_object_base *log_obj,
        const struct pg106_uapi_vm_bind_op *op)
{
   simple_mtx_lock(&dev->br_mutex);
   int err = pg106_br_ctx_bind(dev->va_br, op);
   if (err == 0)
      err = pg106_br_ctx_flush(dev->va_br);
   simple_mtx_unlock(&dev->br_mutex);

   return nvkmd_pg106_result(log_obj, err, VK_ERROR_UNKNOWN, "VM_BIND");
}

VkResult
nvkmd_pg106_alloc_va(struct nvkmd_dev *_dev,
                     struct vk_object_base *log_obj,
                     enum nvkmd_va_flags flags, uint8_t pte_kind,
                     uint64_t size_B, uint64_t align_B,
                     uint64_t fixed_addr, struct nvkmd_va **va_out)
{
   struct nvkmd_pg106_dev *dev = nvkmd_pg106_dev(_dev);
   VkResult result;

   struct nvkmd_pg106_va *va = CALLOC_STRUCT(nvkmd_pg106_va);
   if (va == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   assert(util_is_power_of_two_or_zero64(align_B));
   align_B = MAX2(align_B, _dev->pdev->bind_align_B);
   size_B = align64(size_B, align_B);
   assert((fixed_addr == 0) == !(flags & NVKMD_VA_ALLOC_FIXED));

   simple_mtx_lock(&dev->heap_mutex);
   result = alloc_heap_addr_locked(dev, log_obj, flags, size_B, align_B,
                                   fixed_addr, &va->base.addr);
   simple_mtx_unlock(&dev->heap_mutex);
   if (result != VK_SUCCESS)
      goto fail_alloc;

   if (flags & NVKMD_VA_SPARSE) {
      /* nvkmd_nouveau_va.c:144-151: a sparse region is a handle-0
       * MAP|SPARSE; the bridge turns it into a region entry. */
      const struct pg106_uapi_vm_bind_op op = {
         .op = PG106_UAPI_VM_BIND_OP_MAP,
         .flags = PG106_UAPI_VM_BIND_SPARSE,
         .addr = va->base.addr,
         .range = size_B,
      };
      result = vm_bind(dev, log_obj, &op);
      if (result != VK_SUCCESS)
         goto fail_addr;
   }

   va->base.ops = &nvkmd_pg106_va_ops;
   va->base.dev = &dev->base;
   va->base.flags = flags;
   va->base.pte_kind = pte_kind;
   va->base.size_B = size_B;

   *va_out = &va->base;
   return VK_SUCCESS;

fail_addr:
   free_heap_addr(dev, flags, va->base.addr, size_B);
fail_alloc:
   FREE(va);
   return result;
}

static void
nvkmd_pg106_va_free(struct nvkmd_va *_va)
{
   struct nvkmd_pg106_dev *dev = nvkmd_pg106_dev(_va->dev);
   struct nvkmd_pg106_va *va = nvkmd_pg106_va(_va);
   VkResult result;

   /* nvkmd_nouveau_va.c:183-200: UNMAP the whole range, then drop the
    * sparse region. UNMAP|SPARSE alone would also release what is
    * mapped inside it, but keeping both ops keeps the uapi traffic the
    * same as on Linux. */
   const struct pg106_uapi_vm_bind_op unmap = {
      .op = PG106_UAPI_VM_BIND_OP_UNMAP,
      .addr = va->base.addr,
      .range = va->base.size_B,
   };
   result = vm_bind(dev, NULL, &unmap);

   if (result == VK_SUCCESS && (va->base.flags & NVKMD_VA_SPARSE)) {
      const struct pg106_uapi_vm_bind_op unmap_sparse = {
         .op = PG106_UAPI_VM_BIND_OP_UNMAP,
         .flags = PG106_UAPI_VM_BIND_SPARSE,
         .addr = va->base.addr,
         .range = va->base.size_B,
      };
      result = vm_bind(dev, NULL, &unmap_sparse);
   }

   /* As on nouveau, a failed unbind leaks the VA range rather than
    * handing out addresses the GPU may still translate. */
   if (result == VK_SUCCESS)
      free_heap_addr(dev, va->base.flags, va->base.addr, va->base.size_B);

   FREE(va);
}

static VkResult
nvkmd_pg106_va_bind_mem(struct nvkmd_va *_va,
                        struct vk_object_base *log_obj,
                        uint64_t va_offset_B,
                        struct nvkmd_mem *_mem,
                        uint64_t mem_offset_B,
                        uint64_t range_B)
{
   struct nvkmd_pg106_dev *dev = nvkmd_pg106_dev(_va->dev);
   struct nvkmd_pg106_mem *mem = nvkmd_pg106_mem(_mem);

   assert(_mem->dev == _va->dev);

   /* PTE kind rides in flags[7:0] (nvkmd_nouveau_va.c:229). */
   const struct pg106_uapi_vm_bind_op op = {
      .op = PG106_UAPI_VM_BIND_OP_MAP,
      .flags = _va->pte_kind,
      .handle = mem->handle,
      .addr = _va->addr + va_offset_B,
      .bo_offset = mem_offset_B,
      .range = range_B,
   };
   return vm_bind(dev, log_obj, &op);
}

static VkResult
nvkmd_pg106_va_unbind(struct nvkmd_va *_va,
                      struct vk_object_base *log_obj,
                      uint64_t va_offset_B,
                      uint64_t range_B)
{
   struct nvkmd_pg106_dev *dev = nvkmd_pg106_dev(_va->dev);

   /* Deliberate difference from nvkmd_nouveau_va_unbind, which sends a
    * handle-0 non-sparse MAP that the kernel rejects with -ENOENT
    * (nouveau_uvmm.c:1278-1283; NOUVEAU-UAPI.md section 5.2). UNMAP is
    * what the call means, and inside a sparse region it leaves the
    * region sparse, which is what NVK expects of an unbind. */
   const struct pg106_uapi_vm_bind_op op = {
      .op = PG106_UAPI_VM_BIND_OP_UNMAP,
      .addr = _va->addr + va_offset_B,
      .range = range_B,
   };
   return vm_bind(dev, log_obj, &op);
}

const struct nvkmd_va_ops nvkmd_pg106_va_ops = {
   .free = nvkmd_pg106_va_free,
   .bind_mem = nvkmd_pg106_va_bind_mem,
   .unbind = nvkmd_pg106_va_unbind,
};
