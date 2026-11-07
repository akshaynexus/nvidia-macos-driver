/* SPDX-License-Identifier: MIT */
/*
 * Unit tests for the pg106 nvkmd backend, linked against the real Mesa
 * nvkmd core (nvkmd.c from 17ca6174 + nvk-macos.patch + nvk-pascal.patch),
 * Mesa's util library, the real pg106 bridge and its mock "kext", with
 * loopback_gpu.c executing the host semaphore methods.
 *
 * SIMULATION ONLY: no GPU, no kext, no macOS.
 */
#include "nvkmd/pg106/nvkmd_pg106.h"
#include "loopback_gpu.h"

#include "util/os_time.h"

#include "nvtypes.h"
#include "cl902d.h"
#include "cla140.h"
#include "clc06f.h"
#include "clc197.h"
#include "clc1b5.h"
#include "clc1c0.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond) do {                                                   \
   checks++;                                                               \
   if (!(cond)) {                                                          \
      failures++;                                                          \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
   }                                                                       \
} while (0)

#define CHECK_VK(expr, want) do {                                          \
   VkResult _r = (expr);                                                   \
   checks++;                                                               \
   if (_r != (want)) {                                                     \
      failures++;                                                          \
      fprintf(stderr, "FAIL %s:%d: %s = %d, want %d\n", __FILE__, __LINE__,\
              #expr, (int)_r, (int)(want));                                \
   }                                                                       \
} while (0)

/* ---- helpers -------------------------------------------------------- */

static VkResult
keep_pdev(struct nvkmd_pdev *pdev, void *arg)
{
   *(struct nvkmd_pdev **)arg = pdev;
   return VK_SUCCESS;
}

static struct nvkmd_pdev *
open_pdev(void)
{
   struct nvkmd_pdev *pdev = NULL;
   VkResult r = nvkmd_enum_pdev(NULL, 0, keep_pdev, &pdev);
   CHECK(r == VK_SUCCESS);
   return pdev;
}

static size_t
count_frames(struct pg106_br_mock *mock, uint8_t op)
{
   size_t n = 0;
   for (size_t i = 0; i < pg106_br_mock_frame_count(mock); i++)
      n += pg106_br_mock_frame(mock, i)->op == op;
   return n;
}

/* Index of the n-th frame with `op` (n from 0), or SIZE_MAX. */
static size_t
find_frame(struct pg106_br_mock *mock, uint8_t op, size_t nth)
{
   for (size_t i = 0; i < pg106_br_mock_frame_count(mock); i++) {
      if (pg106_br_mock_frame(mock, i)->op == op && nth-- == 0)
         return i;
   }
   return SIZE_MAX;
}

static const void *
frame_payload(struct pg106_br_mock *mock, size_t index)
{
   return pg106_br_mock_frame(mock, index)->bytes + sizeof(struct pg106_br_frame);
}

static struct pg106_uapi_vm_bind_op
bind_op(struct pg106_br_mock *mock, size_t frame, uint32_t i)
{
   struct pg106_uapi_vm_bind_op op;
   memcpy(&op, (const uint8_t *)frame_payload(mock, frame) +
               sizeof(struct pg106_uapi_vm_bind) + i * sizeof(op), sizeof(op));
   return op;
}

static struct pg106_uapi_exec
exec_hdr(struct pg106_br_mock *mock, size_t frame)
{
   struct pg106_uapi_exec e;
   memcpy(&e, frame_payload(mock, frame), sizeof(e));
   return e;
}

static struct pg106_uapi_exec_push
exec_push(struct pg106_br_mock *mock, size_t frame, uint32_t i)
{
   struct pg106_uapi_exec_push p;
   memcpy(&p, (const uint8_t *)frame_payload(mock, frame) +
              sizeof(struct pg106_uapi_exec) + i * sizeof(p), sizeof(p));
   return p;
}

static struct vk_sync *
new_sync(const struct vk_sync_type *type, bool timeline, uint64_t initial)
{
   struct vk_sync *s = calloc(1, type->size);
   s->type = type;
   s->flags = timeline ? VK_SYNC_IS_TIMELINE : 0;
   CHECK(type->init(NULL, s, initial) == VK_SUCCESS);
   return s;
}

static void
free_sync(struct vk_sync *s)
{
   s->type->finish(NULL, s);
   free(s);
}

/* A stand-in for an NVK push: one host SEMAPHORE release of `value` to
 * `addr` (same encoder as the backend's own release). */
static uint32_t
user_release_push(uint32_t *dw, uint64_t addr, uint32_t value)
{
   return nvkmd_pg106_emit_release(dw, addr, value);
}

/* ---- tests ---------------------------------------------------------- */

static void
test_no_transport(void)
{
   /* Linux, no hook: no PG106 service, enumeration finds nothing. */
   nvkmd_pg106_set_transport_hook(NULL, NULL);
   struct nvkmd_pdev *pdev = NULL;
   CHECK_VK(nvkmd_enum_pdev(NULL, 0, keep_pdev, &pdev), VK_SUCCESS);
   CHECK(pdev == NULL);
   nvkmd_pg106_set_transport_hook(loopback_transport_open,
                                  loopback_transport_close);
}

static void
test_enum_and_dev_info(void)
{
   struct nvkmd_pdev *pdev = open_pdev();
   CHECK(pdev != NULL);
   if (pdev == NULL)
      return;

   const struct nv_device_info *i = &pdev->dev_info;
   CHECK(i->chipset == 0x136);
   CHECK(i->sm == 61);
   CHECK(i->device_id == 0x1C02);        /* the mock's GP106 */
   CHECK(i->type == NV_DEVICE_TYPE_DIS);
   CHECK(i->cls_eng3d == PASCAL_B);
   CHECK(i->cls_compute == PASCAL_COMPUTE_B);
   CHECK(i->cls_copy == PASCAL_DMA_COPY_B);
   CHECK(i->cls_m2mf == KEPLER_INLINE_TO_MEMORY_B);
   CHECK(i->cls_eng2d == FERMI_TWOD_A);
   CHECK(i->cls_gpfifo == PASCAL_CHANNEL_GPFIFO_A);
   CHECK(i->gpc_count == 2 && i->tpc_count == 9 && i->mp_per_tpc == 1);
   CHECK(i->max_warps_per_mp == 64 && i->max_blocks_per_mp == 32);
   CHECK(i->sm_smem_size_count == 1 && i->sm_smem_sizes_kB[0] == 96);
   CHECK(i->max_smem_per_wg_kB == 48);
   CHECK(!i->has_transfer_queue && !i->has_video);
   CHECK(!pdev->kmd_info.has_dma_buf && !pdev->kmd_info.has_alloc_tiled);
   CHECK(pdev->kmd_info.has_get_vram_used);
   CHECK(pdev->bind_align_B == 4096);

   const struct vk_sync_type *t = pdev->sync_types[0];
   CHECK(t != NULL && pdev->sync_types[1] == NULL);
   CHECK(t->features & VK_SYNC_FEATURE_TIMELINE);
   CHECK(t->features & VK_SYNC_FEATURE_BINARY);
   CHECK(t->features & VK_SYNC_FEATURE_WAIT_PENDING);
   CHECK(t->features & VK_SYNC_FEATURE_CPU_RESET);
   CHECK(vk_sync_type_is_nvkmd_pg106(t));

   CHECK(nvkmd_pdev_get_vram_used(pdev) == 0); /* mock answers 0 */
   nvkmd_pdev_destroy(pdev);

   /* NVK_KMD=nvrm hides the Pascal backend. */
   setenv("NVK_KMD", "nvrm", 1);
   pdev = NULL;
   CHECK_VK(nvkmd_enum_pdev(NULL, 0, keep_pdev, &pdev), VK_SUCCESS);
   CHECK(pdev == NULL);
   unsetenv("NVK_KMD");
}

static void
test_enum_rejects(void)
{
   struct nvkmd_pdev *pdev;

   /* ABI major mismatch: no device, no error. */
   loopback_config.kext_major = 7;
   pdev = NULL;
   CHECK_VK(nvkmd_enum_pdev(NULL, 0, keep_pdev, &pdev), VK_SUCCESS);
   CHECK(pdev == NULL);
   loopback_config.kext_major = 0;

   /* A non-GP106 id (GP104M 1BA1, which the kext gate also refuses). */
   loopback_config.pci_device = 0x1BA1;
   pdev = NULL;
   CHECK_VK(nvkmd_enum_pdev(NULL, 0, keep_pdev, &pdev), VK_SUCCESS);
   CHECK(pdev == NULL);

   /* The Helios 300's own id. */
   loopback_config.pci_device = 0x1C20;
   pdev = open_pdev();
   CHECK(pdev != NULL);
   if (pdev) {
      CHECK(pdev->dev_info.device_id == 0x1C20);
      CHECK(pdev->dev_info.tpc_count == 10);
      CHECK(pdev->dev_info.vram_size_B == 6ull << 30);
      CHECK(strstr(pdev->dev_info.device_name, "1060 Mobile") != NULL);
      nvkmd_pdev_destroy(pdev);
   }
   loopback_config.pci_device = 0;

   /* Bring-up not finished: the kext answers -ENODEV. */
   loopback_config.fail_getparam = -ENODEV;
   pdev = NULL;
   CHECK_VK(nvkmd_enum_pdev(NULL, 0, keep_pdev, &pdev), VK_SUCCESS);
   CHECK(pdev == NULL);
   loopback_config.fail_getparam = 0;

   /* A ring too small for acquire + push + release. */
   loopback_config.ib_max = 6; /* EXEC_PUSH_MAX 2 */
   pdev = NULL;
   CHECK_VK(nvkmd_enum_pdev(NULL, 0, keep_pdev, &pdev), VK_SUCCESS);
   CHECK(pdev == NULL);
   loopback_config.ib_max = 0;
}

static void
test_dev_and_mem(void)
{
   loopback_reset_all();
   struct nvkmd_pdev *pdev = open_pdev();
   if (pdev == NULL)
      return;
   struct nvkmd_dev *dev = NULL;
   CHECK_VK(nvkmd_pdev_create_dev(pdev, NULL, &dev), VK_SUCCESS);
   if (dev == NULL)
      return;
   CHECK(loopback_count() == 2); /* probe connection + this device */
   struct loopback_gpu *gpu = loopback_get(1);
   struct pg106_br_mock *mock = loopback_mock(gpu);

   /* VM_INIT reserves [2^39, 2^40) like nouveau_device.c:508-515. */
   size_t vi = find_frame(mock, PG106_BR_OP_VM_INIT, 0);
   CHECK(vi != SIZE_MAX);
   if (vi != SIZE_MAX) {
      struct pg106_uapi_vm_init init;
      memcpy(&init, frame_payload(mock, vi), sizeof(init));
      CHECK(init.kernel_managed_addr == 1ull << 39);
      CHECK(init.kernel_managed_size == 1ull << 39);
   }
   CHECK(dev->va_end == 1ull << 39);

   uint64_t ts = 1;
   CHECK_VK(nvkmd_dev_get_gpu_timestamp_checked(dev, &ts), VK_SUCCESS);

   /* alloc: BO_ALLOC (flags 1:1 + COHERENT), then a MAP of the whole BO */
   struct nvkmd_mem *mem = NULL;
   CHECK_VK(nvkmd_dev_alloc_mem(dev, NULL, 10000, 0,
                                NVKMD_MEM_GART | NVKMD_MEM_CAN_MAP, &mem),
            VK_SUCCESS);
   if (mem == NULL)
      return;
   CHECK(mem->size_B == 12288);
   CHECK(mem->va != NULL && mem->va->addr >= NVKMD_PG106_HEAP_START &&
         mem->va->addr + mem->size_B <= NVKMD_PG106_HEAP_END);
   size_t ba = find_frame(mock, PG106_BR_OP_BO_ALLOC, 0);
   CHECK(ba != SIZE_MAX);
   if (ba != SIZE_MAX) {
      struct pg106_br_bo_alloc req;
      memcpy(&req, frame_payload(mock, ba), sizeof(req));
      CHECK(req.size_B == 12288 && req.align_B == 4096);
      CHECK(req.flags == (PG106_BR_BO_GART | PG106_BR_BO_CAN_MAP |
                          PG106_BR_BO_COHERENT));
   }
   const uint32_t handle = nvkmd_pg106_mem(mem)->handle;
   size_t vb = find_frame(mock, PG106_BR_OP_VM_BIND, 0);
   CHECK(vb != SIZE_MAX && vb > ba);
   if (vb != SIZE_MAX) {
      struct pg106_uapi_vm_bind_op op = bind_op(mock, vb, 0);
      CHECK(op.op == PG106_UAPI_VM_BIND_OP_MAP && op.handle == handle);
      CHECK(op.addr == mem->va->addr && op.range == 12288 &&
            op.bo_offset == 0 && op.flags == 0);
   }

   /* the macOS patch's NVKMD_MEM_BAR bit is not sent to the kext */
   CHECK(nvkmd_pg106_bo_flags(NVKMD_MEM_VRAM | NVKMD_MEM_CAN_MAP | (1u << 7)) ==
         (PG106_BR_BO_VRAM | PG106_BR_BO_CAN_MAP));

   /* maps: internal + client share one bridge mapping */
   void *map = NULL;
   CHECK_VK(nvkmd_mem_map(mem, NULL, NVKMD_MEM_MAP_RDWR, NULL, &map), VK_SUCCESS);
   CHECK(map != NULL && map == pg106_br_mock_bo_backing(mock, handle));
   CHECK_VK(nvkmd_mem_map(mem, NULL, NVKMD_MEM_MAP_RDWR | NVKMD_MEM_MAP_CLIENT,
                          NULL, &map), VK_SUCCESS);
   CHECK(mem->client_map == mem->map);
   CHECK(count_frames(mock, PG106_BR_OP_BO_MAP) == 1);
   nvkmd_mem_unmap(mem, NVKMD_MEM_MAP_CLIENT);
   CHECK(count_frames(mock, PG106_BR_OP_BO_UNMAP) == 0);
   nvkmd_mem_unmap(mem, 0);
   CHECK(count_frames(mock, PG106_BR_OP_BO_UNMAP) == 1);

   /* lookup by VA goes through the core's list */
   uint64_t off = 0;
   struct nvkmd_mem *found = nvkmd_dev_lookup_mem_by_va(dev, mem->va->addr + 8, &off);
   CHECK(found == mem && off == 8);
   if (found)
      nvkmd_mem_unref(found);

   /* free: UNMAP reaches the kext before BO_FREE (unbind before release) */
   nvkmd_mem_unref(mem);
   size_t um = find_frame(mock, PG106_BR_OP_VM_BIND, 1);
   size_t bf = find_frame(mock, PG106_BR_OP_BO_FREE, 0);
   CHECK(um != SIZE_MAX && bf != SIZE_MAX && um < bf);
   if (um != SIZE_MAX)
      CHECK(bind_op(mock, um, 0).op == PG106_UAPI_VM_BIND_OP_UNMAP);

   /* tiled / dma-buf / host import are refused as advertised */
   CHECK_VK(nvkmd_dev_alloc_tiled_mem(dev, NULL, 65536, 65536, 0xfe, 0,
                                      NVKMD_MEM_VRAM, &mem),
            VK_ERROR_FEATURE_NOT_PRESENT);
   CHECK_VK(nvkmd_dev_import_dma_buf(dev, NULL, 3, &mem),
            VK_ERROR_INVALID_EXTERNAL_HANDLE);
#ifndef PG106_BR_HAS_BO_IMPORT_HOST
   static char page[4096] __attribute__((aligned(4096)));
   CHECK_VK(nvkmd_dev_import_host_mem(dev, NULL, page, 4096, NVKMD_MEM_GART, &mem),
            VK_ERROR_INVALID_EXTERNAL_HANDLE);
#endif

   nvkmd_dev_destroy(dev);
   nvkmd_pdev_destroy(pdev);
}

static void
test_sparse_va(void)
{
   loopback_reset_all();
   struct nvkmd_pdev *pdev = open_pdev();
   struct nvkmd_dev *dev = NULL;
   if (pdev == NULL || nvkmd_pdev_create_dev(pdev, NULL, &dev) != VK_SUCCESS)
      return;
   struct pg106_br_mock *mock = loopback_mock(loopback_get(1));

   struct nvkmd_mem *mem = NULL;
   CHECK_VK(nvkmd_dev_alloc_mem(dev, NULL, 65536, 65536, NVKMD_MEM_VRAM, &mem),
            VK_SUCCESS);
   struct nvkmd_va *va = NULL;
   CHECK_VK(nvkmd_dev_alloc_va(dev, NULL, NVKMD_VA_SPARSE, 0xfe /* kind */,
                               1 << 20, 65536, 0, &va), VK_SUCCESS);
   if (mem == NULL || va == NULL)
      return;
   const size_t base = pg106_br_mock_frame_count(mock);
   (void)base;

   size_t f = find_frame(mock, PG106_BR_OP_VM_BIND, 1);
   struct pg106_uapi_vm_bind_op op = bind_op(mock, f, 0);
   CHECK(op.op == PG106_UAPI_VM_BIND_OP_MAP &&
         op.flags == PG106_UAPI_VM_BIND_SPARSE && op.handle == 0 &&
         op.addr == va->addr && op.range == 1 << 20);

   CHECK_VK(nvkmd_va_bind_mem(va, NULL, 65536, mem, 0, 65536), VK_SUCCESS);
   op = bind_op(mock, find_frame(mock, PG106_BR_OP_VM_BIND, 2), 0);
   CHECK(op.op == PG106_UAPI_VM_BIND_OP_MAP && op.flags == 0xfe &&
         op.handle == nvkmd_pg106_mem(mem)->handle &&
         op.addr == va->addr + 65536 && op.range == 65536);

   /* unbind is an UNMAP, not nouveau's handle-0 MAP (-ENOENT) */
   CHECK_VK(nvkmd_va_unbind(va, NULL, 65536, 65536), VK_SUCCESS);
   op = bind_op(mock, find_frame(mock, PG106_BR_OP_VM_BIND, 3), 0);
   CHECK(op.op == PG106_UAPI_VM_BIND_OP_UNMAP && op.flags == 0 &&
         op.addr == va->addr + 65536);

   /* the BO is no longer bound through va, so this free succeeds */
   nvkmd_va_free(va);
   op = bind_op(mock, find_frame(mock, PG106_BR_OP_VM_BIND, 4), 0);
   CHECK(op.op == PG106_UAPI_VM_BIND_OP_UNMAP && op.flags == 0);
   op = bind_op(mock, find_frame(mock, PG106_BR_OP_VM_BIND, 5), 0);
   CHECK(op.op == PG106_UAPI_VM_BIND_OP_UNMAP &&
         op.flags == PG106_UAPI_VM_BIND_SPARSE);

   nvkmd_mem_unref(mem);
   CHECK(count_frames(mock, PG106_BR_OP_BO_FREE) == 1);
   nvkmd_dev_destroy(dev);
   nvkmd_pdev_destroy(pdev);
}

static void
test_encoders(void)
{
   uint32_t dw[16];
   const uint64_t addr = 0x12345678f0ull;

   CHECK(nvkmd_pg106_emit_acquire(dw, addr, 7) == NVKMD_PG106_ACQUIRE_DW);
   /* SEC_OP 1 (INC), COUNT 4, SUBCH 0, ADDRESS 0x10>>2: clc06f.h / the
    * ENCODING-RESOLUTION.md form, typ in bits [31:29]. */
   CHECK(dw[0] == 0x20040004u);
   CHECK(dw[1] == 0x12 && dw[2] == 0x345678f0u && dw[3] == 7);
   CHECK(dw[4] == (NVC06F_SEMAPHORED_OPERATION_ACQ_GEQ | (1u << 12)));

   CHECK(nvkmd_pg106_emit_release(dw, addr, 9) == NVKMD_PG106_RELEASE_DW);
   CHECK(dw[0] == 0x2001001eu && dw[1] == 1);         /* WFI 0x78, SCOPE_ALL */
   CHECK(dw[2] == 0x2004000au);                        /* MEM_OP_A..D at 0x28 */
   CHECK(dw[5] == 0 && dw[6] == (5u << 27));           /* SYS_MEMBAR, MEMBAR */
   CHECK(dw[7] == 0x20040004u && dw[10] == 9);
   CHECK(dw[11] == (NVC06F_SEMAPHORED_OPERATION_RELEASE | (1u << 24)));
}

static void
test_exec_and_sync(void)
{
   loopback_reset_all();
   struct nvkmd_pdev *pdev = open_pdev();
   struct nvkmd_dev *dev = NULL;
   if (pdev == NULL || nvkmd_pdev_create_dev(pdev, NULL, &dev) != VK_SUCCESS)
      return;
   struct loopback_gpu *gpu = loopback_get(1);
   struct pg106_br_mock *mock = loopback_mock(gpu);
   const struct vk_sync_type *type = pdev->sync_types[0];

   struct nvkmd_ctx *a = NULL, *b = NULL;
   CHECK_VK(nvkmd_dev_create_ctx(dev, NULL, NVKMD_ENGINE_3D |
                                 NVKMD_ENGINE_COMPUTE | NVKMD_ENGINE_COPY, &a),
            VK_SUCCESS);
   CHECK_VK(nvkmd_dev_create_ctx(dev, NULL, NVKMD_ENGINE_COPY, &b), VK_SUCCESS);
   if (a == NULL || b == NULL)
      return;
   struct nvkmd_pg106_exec_ctx *ea = nvkmd_pg106_exec_ctx(a);
   struct nvkmd_pg106_exec_ctx *eb = nvkmd_pg106_exec_ctx(b);

   /* A user pushbuffer and a result BO, both GART + mapped. */
   struct nvkmd_mem *pb = NULL, *res = NULL;
   CHECK_VK(nvkmd_dev_alloc_mapped_mem(dev, NULL, 4096, 4096, NVKMD_MEM_GART,
                                       NVKMD_MEM_MAP_RDWR, &pb), VK_SUCCESS);
   CHECK_VK(nvkmd_dev_alloc_mapped_mem(dev, NULL, 4096, 4096, NVKMD_MEM_GART,
                                       NVKMD_MEM_MAP_RDWR, &res), VK_SUCCESS);
   if (pb == NULL || res == NULL)
      return;
   uint32_t *pbw = pb->map;
   const uint32_t n = user_release_push(pbw, res->va->addr, 0xC0FFEE);

   /* ctx A: wait nothing, exec the user push, signal timeline S = 5 */
   struct vk_sync *s = new_sync(type, true, 0);
   const struct nvkmd_ctx_exec ex = { .addr = pb->va->addr, .size_B = n * 4 };
   CHECK_VK(nvkmd_ctx_exec(a, NULL, 1, &ex), VK_SUCCESS);
   const struct vk_sync_signal sig = { .sync = s, .signal_value = 5 };
   CHECK_VK(nvkmd_ctx_signal(a, NULL, 1, &sig), VK_SUCCESS);

   size_t e0 = find_frame(mock, PG106_BR_OP_EXEC, 0);
   CHECK(e0 != SIZE_MAX);
   if (e0 != SIZE_MAX) {
      struct pg106_uapi_exec h = exec_hdr(mock, e0);
      CHECK(h.push_count == 2 && h.wait_count == 0 && h.sig_count == 0);
      CHECK(exec_push(mock, e0, 0).va == pb->va->addr);
      CHECK(exec_push(mock, e0, 1).va_len == NVKMD_PG106_RELEASE_DW * 4);
      CHECK(exec_push(mock, e0, 1).va >= ea->ctrl_addr &&
            exec_push(mock, e0, 1).va < ea->ctrl_addr + NVKMD_PG106_CTRL_BO_SIZE);
   }
   CHECK(*(uint32_t *)res->map == 0xC0FFEE);    /* the user push ran */
   CHECK(*ea->tl->word == 1 && ea->tl->submitted == 1);

   uint64_t v = 0;
   CHECK_VK(type->get_value(NULL, s, &v), VK_SUCCESS);
   CHECK(v == 5);
   const struct vk_sync_wait w5 = { .sync = s, .wait_value = 5 };
   CHECK_VK(type->wait_many(NULL, 1, &w5, 0, os_time_get_absolute_timeout(1000000000)),
            VK_SUCCESS);

   /* ctx B waits on S = 5 on the GPU: point already done -> no acquire */
   CHECK_VK(nvkmd_ctx_wait(b, NULL, 1, &w5), VK_SUCCESS);
   CHECK(eb->acq_dw == 0);

   /* Pending (not yet done) point: make A's release "hang", signal 6 */
   loopback_set_drop_releases(gpu, 1);
   CHECK_VK(nvkmd_ctx_exec(a, NULL, 1, &ex), VK_SUCCESS);
   const struct vk_sync_signal sig6 = { .sync = s, .signal_value = 6 };
   CHECK_VK(nvkmd_ctx_signal(a, NULL, 1, &sig6), VK_SUCCESS);
   CHECK(*ea->tl->word == 1 && ea->tl->submitted == 2);
   loopback_set_drop_releases(gpu, 0);

   /* WAIT_PENDING sees the point; a complete wait times out */
   const struct vk_sync_wait w6 = { .sync = s, .wait_value = 6 };
   CHECK_VK(type->wait_many(NULL, 1, &w6, VK_SYNC_WAIT_PENDING, 0), VK_SUCCESS);
   CHECK_VK(type->wait_many(NULL, 1, &w6, 0, os_time_get_absolute_timeout(2000000)),
            VK_TIMEOUT);
   const struct vk_sync_wait w7 = { .sync = s, .wait_value = 7 };
   CHECK_VK(type->wait_many(NULL, 1, &w7, VK_SYNC_WAIT_PENDING, 0), VK_TIMEOUT);

   /* B waits for 6 on the GPU: ACQ_GEQ on A's word, payload 2 */
   CHECK_VK(nvkmd_ctx_wait(b, NULL, 1, &w6), VK_SUCCESS);
   CHECK(eb->acq_dw == NVKMD_PG106_ACQUIRE_DW);
   CHECK_VK(nvkmd_ctx_flush(b, NULL), VK_SUCCESS);
   size_t eb0 = find_frame(mock, PG106_BR_OP_EXEC, 2);
   CHECK(eb0 != SIZE_MAX);
   if (eb0 != SIZE_MAX) {
      struct pg106_uapi_exec h = exec_hdr(mock, eb0);
      CHECK(h.push_count == 2);  /* acquire slot, release */
      const uint32_t *acq = loopback_va_ptr(gpu, exec_push(mock, eb0, 0).va, 20);
      CHECK(acq != NULL && exec_push(mock, eb0, 0).va_len == 20);
      if (acq) {
         CHECK(acq[2] == (uint32_t)ea->tl->gpu_addr && acq[3] == 2);
         CHECK((acq[4] & 0x1f) == NVC06F_SEMAPHORED_OPERATION_ACQ_GEQ);
      }
   }
   /* the hung release left B's acquire unsatisfied (it would block) */
   CHECK(loopback_stats(gpu)->acquires == 1);
   CHECK(loopback_stats(gpu)->acquires_unsatisfied == 1);

   /* "the GPU catches up": write A's word, everything completes */
   *ea->tl->word = 2;
   CHECK_VK(type->wait_many(NULL, 1, &w6, 0, os_time_get_absolute_timeout(1000000000)),
            VK_SUCCESS);
   CHECK_VK(nvkmd_ctx_sync(a, NULL), VK_SUCCESS);
   CHECK_VK(nvkmd_ctx_sync(b, NULL), VK_SUCCESS);

   /* CPU signal / binary reset */
   struct vk_sync *bin = new_sync(type, false, 0);
   const struct vk_sync_wait wb = { .sync = bin, .wait_value = 0 };
   CHECK_VK(type->wait_many(NULL, 1, &wb, 0, 0), VK_TIMEOUT);
   CHECK_VK(type->signal(NULL, bin, 0), VK_SUCCESS);
   CHECK_VK(type->wait_many(NULL, 1, &wb, 0, 0), VK_SUCCESS);
   CHECK_VK(type->reset(NULL, bin), VK_SUCCESS);
   CHECK_VK(type->wait_many(NULL, 1, &wb, 0, 0), VK_TIMEOUT);
   /* WAIT_ANY over {bin (unsignalled), s (=6)} */
   const struct vk_sync_wait any[2] = { wb, w6 };
   CHECK_VK(type->wait_many(NULL, 2, any, VK_SYNC_WAIT_ANY, 0), VK_SUCCESS);
   CHECK_VK(type->wait_many(NULL, 2, any, 0, 0), VK_TIMEOUT);

   /* no stray pushes anywhere */
   CHECK(loopback_stats(gpu)->bad_push_va == 0);
   CHECK(loopback_stats(gpu)->bad_sem_va == 0);
   CHECK(loopback_stats(gpu)->membars == loopback_stats(gpu)->exec_frames + 2);

   free_sync(bin);
   free_sync(s);
   nvkmd_mem_unref(pb);
   nvkmd_mem_unref(res);
   nvkmd_ctx_destroy(b);
   nvkmd_ctx_destroy(a);
   nvkmd_dev_destroy(dev);
   nvkmd_pdev_destroy(pdev);
}

static void
test_push_splitting(void)
{
   loopback_reset_all();
   loopback_config.ib_max = 10;           /* EXEC_PUSH_MAX = 4 */
   struct nvkmd_pdev *pdev = open_pdev();
   struct nvkmd_dev *dev = NULL;
   if (pdev == NULL || nvkmd_pdev_create_dev(pdev, NULL, &dev) != VK_SUCCESS)
      return;
   struct loopback_gpu *gpu = loopback_get(1);
   struct pg106_br_mock *mock = loopback_mock(gpu);
   struct nvkmd_ctx *ctx = NULL;
   CHECK_VK(nvkmd_dev_create_ctx(dev, NULL, NVKMD_ENGINE_3D, &ctx), VK_SUCCESS);
   struct nvkmd_mem *pb = NULL;
   CHECK_VK(nvkmd_dev_alloc_mapped_mem(dev, NULL, 4096, 4096, NVKMD_MEM_GART,
                                       NVKMD_MEM_MAP_RDWR, &pb), VK_SUCCESS);
   if (ctx == NULL || pb == NULL)
      return;

   /* 7 single-NOP pushes; #2 and #3 are an incomplete run ending at #4 */
   struct nvkmd_ctx_exec ex[7];
   for (int i = 0; i < 7; i++) {
      ex[i] = (struct nvkmd_ctx_exec) {
         .addr = pb->va->addr + 64 * i, .size_B = 8,
         .incomplete = (i == 2 || i == 3),
      };
      uint32_t *p = (uint32_t *)((char *)pb->map + 64 * i);
      p[0] = 0x20010002u;  /* NVC06F_NOP */
      p[1] = 0;
   }
   CHECK_VK(nvkmd_ctx_exec(ctx, NULL, 7, ex), VK_SUCCESS);
   CHECK_VK(nvkmd_ctx_sync(ctx, NULL), VK_SUCCESS);

   /* every EXEC within 4 pushes, each ends in the release, the run 2..4
    * stays in one EXEC, all 7 user pushes went out exactly once */
   size_t frames = count_frames(mock, PG106_BR_OP_EXEC);
   uint32_t user = 0;
   bool run_split = true;
   for (size_t k = 0; k < frames; k++) {
      size_t f = find_frame(mock, PG106_BR_OP_EXEC, k);
      struct pg106_uapi_exec h = exec_hdr(mock, f);
      CHECK(h.push_count >= 2 && h.push_count <= 4);
      CHECK(exec_push(mock, f, h.push_count - 1).va_len ==
            NVKMD_PG106_RELEASE_DW * 4);
      for (uint32_t p = 0; p + 1 < h.push_count; p++) {
         if (exec_push(mock, f, p).va == ex[2].addr)
            run_split = !(p + 2 < h.push_count &&
                          exec_push(mock, f, p + 2).va == ex[4].addr);
         user++;
      }
   }
   CHECK(user == 7);
   CHECK(!run_split);
   CHECK(nvkmd_pg106_exec_ctx(ctx)->tl->submitted == frames);
   CHECK(*nvkmd_pg106_exec_ctx(ctx)->tl->word == frames);

   nvkmd_mem_unref(pb);
   nvkmd_ctx_destroy(ctx);
   nvkmd_dev_destroy(dev);
   nvkmd_pdev_destroy(pdev);
}

static void
test_device_lost(void)
{
   loopback_reset_all();
   setenv("NVK_PG106_TIMEOUT_MS", "50", 1);
   struct nvkmd_pdev *pdev = open_pdev();
   struct nvkmd_dev *dev = NULL;
   if (pdev == NULL || nvkmd_pdev_create_dev(pdev, NULL, &dev) != VK_SUCCESS)
      return;
   struct loopback_gpu *gpu = loopback_get(1);
   const struct vk_sync_type *type = pdev->sync_types[0];
   struct nvkmd_ctx *ctx = NULL;
   CHECK_VK(nvkmd_dev_create_ctx(dev, NULL, NVKMD_ENGINE_3D, &ctx), VK_SUCCESS);
   if (ctx == NULL)
      return;
   struct nvkmd_pg106_exec_ctx *ec = nvkmd_pg106_exec_ctx(ctx);

   /* hung GPU: sync times out after NVK_PG106_TIMEOUT_MS -> DEVICE_LOST */
   struct vk_sync *s = new_sync(type, true, 0);
   loopback_set_drop_releases(gpu, 1);
   const struct vk_sync_signal sig = { .sync = s, .signal_value = 1 };
   CHECK_VK(nvkmd_ctx_signal(ctx, NULL, 1, &sig), VK_SUCCESS);
   const uint64_t t0 = os_time_get_nano();
   CHECK_VK(nvkmd_ctx_sync(ctx, NULL), VK_ERROR_DEVICE_LOST);
   CHECK(os_time_get_nano() - t0 < 5000000000ull);
   CHECK(ec->tl->lost);
   uint64_t v;
   CHECK_VK(type->get_value(NULL, s, &v), VK_ERROR_DEVICE_LOST);
   CHECK_VK(nvkmd_ctx_flush(ctx, NULL), VK_ERROR_DEVICE_LOST);
   free_sync(s);
   nvkmd_ctx_destroy(ctx);

   /* the kext refuses an EXEC (-ENODEV: channel killed) */
   ctx = NULL;
   CHECK_VK(nvkmd_dev_create_ctx(dev, NULL, NVKMD_ENGINE_3D, &ctx), VK_SUCCESS);
   loopback_set_drop_releases(gpu, 0);
   pg106_br_mock_fail_next(loopback_mock(gpu), PG106_BR_OP_EXEC, -ENODEV);
   s = new_sync(type, true, 0);
   const struct vk_sync_signal sig2 = { .sync = s, .signal_value = 1 };
   CHECK_VK(nvkmd_ctx_signal(ctx, NULL, 1, &sig2), VK_ERROR_DEVICE_LOST);
   CHECK(nvkmd_pg106_exec_ctx(ctx)->signal_count == 0);
   CHECK_VK(nvkmd_ctx_sync(ctx, NULL), VK_ERROR_DEVICE_LOST);
   free_sync(s);
   nvkmd_ctx_destroy(ctx);

   unsetenv("NVK_PG106_TIMEOUT_MS");
   nvkmd_dev_destroy(dev);
   nvkmd_pdev_destroy(pdev);
}

static void
test_bind_ctx(void)
{
   loopback_reset_all();
   struct nvkmd_pdev *pdev = open_pdev();
   struct nvkmd_dev *dev = NULL;
   if (pdev == NULL || nvkmd_pdev_create_dev(pdev, NULL, &dev) != VK_SUCCESS)
      return;
   struct pg106_br_mock *mock = loopback_mock(loopback_get(1));
   const struct vk_sync_type *type = pdev->sync_types[0];

   struct nvkmd_ctx *bctx = NULL;
   CHECK_VK(nvkmd_dev_create_ctx(dev, NULL, NVKMD_ENGINE_BIND, &bctx), VK_SUCCESS);
   struct nvkmd_mem *mem = NULL;
   CHECK_VK(nvkmd_dev_alloc_mem(dev, NULL, 65536, 65536, NVKMD_MEM_VRAM, &mem),
            VK_SUCCESS);
   struct nvkmd_va *va = NULL;
   CHECK_VK(nvkmd_dev_alloc_va(dev, NULL, NVKMD_VA_SPARSE, 0, 1 << 20, 65536,
                               0, &va), VK_SUCCESS);
   if (bctx == NULL || mem == NULL || va == NULL)
      return;

   struct vk_sync *in = new_sync(type, true, 3);   /* already at 3 */
   struct vk_sync *out = new_sync(type, true, 0);
   const struct vk_sync_wait w = { .sync = in, .wait_value = 3 };
   const struct nvkmd_ctx_bind binds[2] = {
      { .op = NVKMD_BIND_OP_BIND, .va = va, .va_offset_B = 0, .mem = mem,
        .mem_offset_B = 0, .range_B = 65536 },
      { .op = NVKMD_BIND_OP_UNBIND, .va = va, .va_offset_B = 0,
        .range_B = 65536 },
   };
   const size_t before = count_frames(mock, PG106_BR_OP_VM_BIND);
   CHECK_VK(nvkmd_ctx_wait(bctx, NULL, 1, &w), VK_SUCCESS);
   CHECK_VK(nvkmd_ctx_bind(bctx, NULL, 2, binds), VK_SUCCESS);
   const struct vk_sync_signal sg = { .sync = out, .signal_value = 9 };
   CHECK_VK(nvkmd_ctx_signal(bctx, NULL, 1, &sg), VK_SUCCESS);

   CHECK(count_frames(mock, PG106_BR_OP_VM_BIND) == before + 1); /* one job */
   size_t f = find_frame(mock, PG106_BR_OP_VM_BIND, before);
   struct pg106_uapi_vm_bind hdr;
   memcpy(&hdr, frame_payload(mock, f), sizeof(hdr));
   CHECK(hdr.op_count == 2 && hdr.wait_count == 0 && hdr.sig_count == 0);
   CHECK(bind_op(mock, f, 0).op == PG106_UAPI_VM_BIND_OP_MAP);
   CHECK(bind_op(mock, f, 1).op == PG106_UAPI_VM_BIND_OP_UNMAP);
   uint64_t v = 0;
   CHECK_VK(type->get_value(NULL, out, &v), VK_SUCCESS);
   CHECK(v == 9);

   free_sync(in);
   free_sync(out);
   nvkmd_va_free(va);
   nvkmd_mem_unref(mem);
   nvkmd_ctx_destroy(bctx);
   nvkmd_dev_destroy(dev);
   nvkmd_pdev_destroy(pdev);
}

int
main(void)
{
   nvkmd_pg106_set_transport_hook(loopback_transport_open,
                                  loopback_transport_close);
   test_no_transport();
   test_enum_and_dev_info();
   test_enum_rejects();
   test_dev_and_mem();
   test_sparse_va();
   test_encoders();
   test_exec_and_sync();
   test_push_splitting();
   test_device_lost();
   test_bind_ctx();
   loopback_reset_all();

   printf("test_nvkmd_pg106: %d checks, %d failure(s)\n", checks, failures);
   return failures != 0;
}
