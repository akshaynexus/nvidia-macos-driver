/*
 * Copyright © 2026 the nvidia-macos-driver Pascal port contributors
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd_pg106.h"

#include "util/cache_ops.h"
#include "util/macros.h"
#include "util/os_misc.h"
#include "util/u_debug.h"
#include "util/u_memory.h"
#include "vk_log.h"

#include "nvtypes.h"
#include "cl902d.h"
#include "cla140.h"
#include "clc06f.h"
#include "clc197.h"
#include "clc1b5.h"
#include "clc1c0.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "pg106_br_iokit.h"

/* ------------------------------------------------------------------ */
/* Transport                                                           */
/* ------------------------------------------------------------------ */

static nvkmd_pg106_transport_open_fn transport_open_hook;
static nvkmd_pg106_transport_close_fn transport_close_hook;

void
nvkmd_pg106_set_transport_hook(nvkmd_pg106_transport_open_fn open_fn,
                               nvkmd_pg106_transport_close_fn close_fn)
{
   transport_open_hook = open_fn;
   transport_close_hook = close_fn;
}

int
nvkmd_pg106_transport_open(struct pg106_br_transport *tr)
{
   memset(tr, 0, sizeof(*tr));
   if (transport_open_hook != NULL)
      return transport_open_hook(tr);

#if defined(__APPLE__)
   /* IOServiceMatching("PG106") + IOServiceOpen (pascal/bridge
    * transport_iokit.c); -ENODEV when the kext is not loaded. */
   return pg106_br_iokit_transport_open(tr);
#else
   return -ENODEV;
#endif
}

void
nvkmd_pg106_transport_close(struct pg106_br_transport *tr)
{
   if (transport_open_hook != NULL) {
      if (transport_close_hook != NULL)
         transport_close_hook(tr);
      return;
   }

#if defined(__APPLE__)
   pg106_br_iokit_transport_close(tr);
#endif
}

VkResult
nvkmd_pg106_result(struct vk_object_base *log_obj, int err,
                   VkResult fallback, const char *what)
{
   if (err == 0)
      return VK_SUCCESS;

   VkResult result = fallback;
   switch (err) {
   case -ENOMEM:
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      break;
   case -ENOSPC:
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      break;
   case -ENODEV:
      /* nouveau_exec.c:373-381: a killed channel is -ENODEV */
      result = VK_ERROR_DEVICE_LOST;
      break;
   default:
      break;
   }

   return vk_errorf(log_obj, result, "pg106: %s failed: %s (%d)",
                    what, strerror(-err), err);
}

/* ------------------------------------------------------------------ */
/* Chip table                                                          */
/* ------------------------------------------------------------------ */

#define GiB (1024ull * 1024ull * 1024ull)

/* Mobile ids: exactly the PG106 kext probe gate
 * (pascal/kext/src/pg106_device_gate.c, from pci.ids). Desktop GP106
 * ids: what the bridge mock reports (pg106_br_mock.h, 10de:1c02); the
 * kext refuses them, so they only ever appear in tests. VRAM sizes are
 * the board defaults from NVIDIA's product pages and stand in until the
 * kext reports the real size (BRIDGE-NVK-REVIEW.md, "Device facts"). */
static const struct nvkmd_pg106_chip pg106_chips[] = {
   { 0x1C20, "NVIDIA GeForce GTX 1060 Mobile",        0x136, 6 * GiB },
   { 0x1C21, "NVIDIA GeForce GTX 1050 Ti Mobile",     0x136, 4 * GiB },
   { 0x1C22, "NVIDIA GeForce GTX 1050 Mobile",        0x136, 4 * GiB },
   { 0x1C23, "NVIDIA GeForce GTX 1060 Mobile Rev. 2", 0x136, 6 * GiB },
   { 0x1C60, "NVIDIA GeForce GTX 1060 Mobile 6GB",    0x136, 6 * GiB },
   { 0x1C61, "NVIDIA GeForce GTX 1050 Ti Mobile",     0x136, 4 * GiB },
   { 0x1C62, "NVIDIA GeForce GTX 1050 Mobile",        0x136, 4 * GiB },
   { 0x1C02, "NVIDIA GeForce GTX 1060 3GB",           0x136, 3 * GiB },
   { 0x1C03, "NVIDIA GeForce GTX 1060 6GB",           0x136, 6 * GiB },
};

const struct nvkmd_pg106_chip *
nvkmd_pg106_chip_lookup(uint16_t device_id)
{
   for (unsigned i = 0; i < ARRAY_SIZE(pg106_chips); i++) {
      if (pg106_chips[i].device_id == device_id)
         return &pg106_chips[i];
   }
   return NULL;
}

/* SM count per board, used only when GETPARAM GRAPH_UNITS answers 0
 * (the skeleton kext and the mock do): 1280/768/640 CUDA cores at 128
 * per Pascal SM. GP106 has two GPCs. */
static uint16_t
pg106_default_tpcs(uint16_t device_id)
{
   switch (device_id) {
   case 0x1C21: case 0x1C61: return 6;
   case 0x1C22: case 0x1C62: return 5;
   case 0x1C02:              return 9;
   default:                  return 10;
   }
}

void
nvkmd_pg106_fill_dev_info(struct nv_device_info *info,
                          const struct nvkmd_pg106_chip *chip,
                          uint64_t graph_units, uint64_t bar_size_B,
                          uint64_t vram_size_B)
{
   memset(info, 0, sizeof(*info));

   info->type = NV_DEVICE_TYPE_DIS;
   info->device_id = chip->device_id;
   info->chipset = chip->chipset;
   snprintf(info->device_name, sizeof(info->device_name), "%s", chip->name);
   snprintf(info->chipset_name, sizeof(info->chipset_name), "GP106");

   /* nouveau_device.c sm_for_chipset: 0x132..0x13a is SM 6.1 */
   info->sm = 61;

   if (graph_units != 0) {
      /* Same packing as NOUVEAU_GETPARAM_GRAPH_UNITS
       * (nouveau_device.c: gpc in [7:0], tpc in [23:8]). */
      info->gpc_count = (graph_units >> 0) & 0xff;
      info->tpc_count = (graph_units >> 8) & 0xffff;
   } else {
      info->gpc_count = 2;
      info->tpc_count = pg106_default_tpcs(chip->device_id);
   }

   /* mp_per_tpc_for_chipset: only GP100 (0x130) has two */
   info->mp_per_tpc = 1;
   /* max_warps_per_mp_for_sm / max_blocks_per_mp_for_sm, SM 61 */
   info->max_warps_per_mp = 64;
   info->max_blocks_per_mp = 32;

   /* init_shared_mem_sizes: SM 61 has 96 KiB per SM, and Kepler..Pascal
    * address at most 48 KiB per workgroup. */
   info->sm_smem_sizes_kB[0] = 96;
   info->sm_smem_size_count = 1;
   info->max_smem_per_wg_kB = 48;

   /* GP10x minus GP100: PASCAL_B, PASCAL_COMPUTE_B (QMD v02_01),
    * PASCAL_DMA_COPY_B, KEPLER_INLINE_TO_MEMORY_B as m2mf, FERMI_TWOD_A,
    * PASCAL_CHANNEL_GPFIFO_A (HELIOS300-PASCAL-RESEARCH.md section 3.1;
    * class headers in src/nouveau/headers/nvidia/classes). */
   info->cls_copy = PASCAL_DMA_COPY_B;
   info->cls_eng2d = FERMI_TWOD_A;
   info->cls_eng3d = PASCAL_B;
   info->cls_m2mf = KEPLER_INLINE_TO_MEMORY_B;
   info->cls_compute = PASCAL_COMPUTE_B;
   info->cls_gpfifo = PASCAL_CHANNEL_GPFIFO_A;
   info->cls_vdec = 0;

   /* nouveau gates transfer queues on Turing+ and video on NVDEC
    * contexts; neither exists here. */
   info->has_transfer_queue = false;
   info->has_video = false;
   info->has_zcull_info = false;

   /* The kext's figure when it gives one (PG106_BR_GETPARAM_VRAM_SIZE),
    * else the board default. */
   info->vram_size_B = vram_size_B ? vram_size_B : chip->vram_size_B;
   info->bar_size_B = bar_size_B;
}

/* ------------------------------------------------------------------ */
/* pdev                                                                */
/* ------------------------------------------------------------------ */

static VkResult
nvkmd_pg106_create_pdev(struct vk_object_base *log_obj,
                        enum nvk_debug debug_flags,
                        struct nvkmd_pdev **pdev_out)
{
   struct nvkmd_pg106_pdev *pdev = CALLOC_STRUCT(nvkmd_pg106_pdev);
   if (pdev == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   simple_mtx_init(&pdev->br_mutex, mtx_plain);

   int err = nvkmd_pg106_transport_open(&pdev->tr);
   if (err == -EACCES) {
      fprintf(stderr, "pg106: the PG106 user client refused this process "
                      "(-EACCES); the GP106 is not offered to it\n");
   }
   if (err != 0) {
      /* No PG106 service: not an error, there is simply no device. */
      simple_mtx_destroy(&pdev->br_mutex);
      FREE(pdev);
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   }

   err = pg106_br_dev_open(&pdev->tr, &pdev->br);
   if (err != 0) {
      nvkmd_pg106_transport_close(&pdev->tr);
      simple_mtx_destroy(&pdev->br_mutex);
      FREE(pdev);
      if (err == -EPROTONOSUPPORT) {
         return vk_errorf(log_obj, VK_ERROR_INCOMPATIBLE_DRIVER,
                          "pg106: kext bridge ABI does not match %u.%u",
                          PG106_BR_ABI_MAJOR, PG106_BR_ABI_MINOR);
      }
      return VK_ERROR_INCOMPATIBLE_DRIVER;
   }

   uint64_t vendor = 0, device = 0, graph_units = 0, bar_size = 0;
   uint64_t push_max = 0, vram_size = 0, boot0 = 0;
   err = pg106_br_getparam(pdev->br, PG106_UAPI_GETPARAM_PCI_VENDOR, &vendor);
   if (err == 0)
      err = pg106_br_getparam(pdev->br, PG106_UAPI_GETPARAM_PCI_DEVICE, &device);
   if (err == 0)
      err = pg106_br_getparam(pdev->br, PG106_UAPI_GETPARAM_EXEC_PUSH_MAX,
                              &push_max);
   /* GRAPH_UNITS, VRAM_BAR_SIZE and the bridge-private VRAM_SIZE/BOOT0
    * may be 0 (skeleton kext, mock defaults); only a transport failure
    * stops us. */
   if (err == 0)
      err = pg106_br_getparam(pdev->br, PG106_UAPI_GETPARAM_GRAPH_UNITS,
                              &graph_units);
   if (err == 0)
      err = pg106_br_getparam(pdev->br, PG106_UAPI_GETPARAM_VRAM_BAR_SIZE,
                              &bar_size);
#ifdef PG106_BR_GETPARAM_VRAM_SIZE
   if (err == 0 &&
       pg106_br_getparam(pdev->br, PG106_BR_GETPARAM_VRAM_SIZE, &vram_size) != 0)
      vram_size = 0;
#endif
#ifdef PG106_BR_GETPARAM_BOOT0
   if (err == 0 &&
       pg106_br_getparam(pdev->br, PG106_BR_GETPARAM_BOOT0, &boot0) != 0)
      boot0 = 0;
#endif

   const struct nvkmd_pg106_chip *chip = NULL;
   VkResult result = VK_ERROR_INCOMPATIBLE_DRIVER;
   if (err != 0) {
      /* A broken kext connection hides the GP106 but must not fail
       * vkEnumeratePhysicalDevices for other GPUs. */
      result = nvkmd_pg106_result(log_obj, err, VK_ERROR_INCOMPATIBLE_DRIVER,
                                  "GETPARAM");
      goto fail;
   }

   /* Is the GPU usable? The kext serves HANDSHAKE and GETPARAM at once
    * but every other op is -ENODEV until its ACR/GR bring-up has
    * finished (or after it failed). */
#ifdef PG106_BR_GETPARAM_READY
   uint64_t ready = 0;
   err = pg106_br_getparam(pdev->br, PG106_BR_GETPARAM_READY, &ready);
   if (err == 0 && ready == 0)
      err = -ENODEV;
#else
   /* No status param: VM_INIT on this probe-only connection is the
    * cheapest op that needs a brought-up GPU. */
   err = pg106_br_vm_init(pdev->br, NVKMD_PG106_KERNEL_START,
                          NVKMD_PG106_KERNEL_END - NVKMD_PG106_KERNEL_START);
#endif
   if (err == -ENODEV) {
      result = vk_errorf(log_obj, VK_ERROR_INCOMPATIBLE_DRIVER,
                         "pg106: the kext has not brought the GPU up");
      goto fail;
   } else if (err != 0) {
      result = nvkmd_pg106_result(log_obj, err, VK_ERROR_INCOMPATIBLE_DRIVER,
                                  "bring-up probe");
      goto fail;
   }

   if (vendor != NVIDIA_VENDOR_ID)
      goto fail;

   chip = nvkmd_pg106_chip_lookup((uint16_t)device);
   if (chip == NULL) {
      result = vk_errorf(log_obj, VK_ERROR_INCOMPATIBLE_DRIVER,
                         "pg106: 10de:%04x is not a GP106 this backend knows",
                         (unsigned)device);
      goto fail;
   }

   /* PMC_BOOT_0 [28:20] is arch|impl (pascal/core pmc.h); a GP106 reads
    * 0x136. 0 means the kext did not say. */
   if (boot0 != 0 && ((boot0 >> 20) & 0x1ff) != chip->chipset) {
      result = vk_errorf(log_obj, VK_ERROR_INCOMPATIBLE_DRIVER,
                         "pg106: BOOT_0 0x%08" PRIx64 " is not chipset 0x%x",
                         boot0, chip->chipset);
      goto fail;
   }

   if (push_max < 3) {
      /* Every flush needs one acquire push, one user push and the
       * release push. */
      result = vk_errorf(log_obj, VK_ERROR_INCOMPATIBLE_DRIVER,
                         "pg106: EXEC_PUSH_MAX %" PRIu64 " is too small",
                         push_max);
      goto fail;
   }

   pdev->base.ops = &nvkmd_pg106_pdev_ops;
   pdev->base.debug_flags = debug_flags;
   nvkmd_pg106_fill_dev_info(&pdev->base.dev_info, chip, graph_units, bar_size,
                             vram_size);
   pdev->base.dev_info.nc_atom_size_B = util_cache_granularity();

   pdev->base.kmd_info = (struct nvkmd_info) {
      .has_dma_buf = false,
      .has_get_vram_used = true,
      .has_alloc_tiled = false,
      .has_map_fixed = false,
      .has_overmap = false,
      .has_compression = false,
#ifdef PG106_BR_HAS_BO_IMPORT_HOST
      .has_host_import = true,
#else
      .has_host_import = false,
#endif
   };

   uint64_t os_page_size;
   os_get_page_size(&os_page_size);
   assert(os_page_size <= UINT32_MAX);
   pdev->base.bind_align_B = os_page_size;

   pdev->sync_type = nvkmd_pg106_sync_get_type();
   pdev->sync_types[0] = &pdev->sync_type;
   pdev->sync_types[1] = NULL;
   pdev->base.sync_types = pdev->sync_types;

   *pdev_out = &pdev->base;
   return VK_SUCCESS;

fail:
   pg106_br_dev_close(pdev->br);
   nvkmd_pg106_transport_close(&pdev->tr);
   simple_mtx_destroy(&pdev->br_mutex);
   FREE(pdev);
   return result;
}

VkResult
nvkmd_pg106_enum_pdev(struct vk_object_base *log_obj,
                      enum nvk_debug debug_flags,
                      nvkmd_enum_pdev_visitor visitor,
                      void *arg)
{
   if (debug_get_bool_option("NVK_PG106_DISABLE", false))
      return VK_SUCCESS;

   /* One PG106 per machine: the kext gate matches one GP106 and the
    * user client has no device index. */
   struct nvkmd_pdev *pdev = NULL;
   VkResult result = nvkmd_pg106_create_pdev(log_obj, debug_flags, &pdev);
   if (result == VK_ERROR_INCOMPATIBLE_DRIVER)
      return VK_SUCCESS;
   if (result != VK_SUCCESS)
      return result;

   result = visitor(pdev, arg);
   if (result == VK_ERROR_INCOMPATIBLE_DRIVER)
      return VK_SUCCESS;

   return result;
}

static void
nvkmd_pg106_pdev_destroy(struct nvkmd_pdev *_pdev)
{
   struct nvkmd_pg106_pdev *pdev = nvkmd_pg106_pdev(_pdev);

   pg106_br_dev_close(pdev->br);
   nvkmd_pg106_transport_close(&pdev->tr);
   simple_mtx_destroy(&pdev->br_mutex);
   FREE(pdev);
}

static uint64_t
nvkmd_pg106_pdev_get_vram_used(struct nvkmd_pdev *_pdev)
{
   struct nvkmd_pg106_pdev *pdev = nvkmd_pg106_pdev(_pdev);
   uint64_t used = 0;

   simple_mtx_lock(&pdev->br_mutex);
   if (pg106_br_getparam(pdev->br, PG106_UAPI_GETPARAM_VRAM_USED, &used) != 0)
      used = 0;
   simple_mtx_unlock(&pdev->br_mutex);

   return used;
}

const struct nvkmd_pdev_ops nvkmd_pg106_pdev_ops = {
   .destroy = nvkmd_pg106_pdev_destroy,
   .get_vram_used = nvkmd_pg106_pdev_get_vram_used,
   .get_drm_primary_fd = NULL,
   .create_dev = nvkmd_pg106_create_dev,
};
