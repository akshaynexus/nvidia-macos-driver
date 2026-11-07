/* SPDX-License-Identifier: MIT */
/*
 * pg106_vk_smoke - OWNER-RUNBOOK.md step 3: does the Pascal NVK ICD work at
 * all on the Helios 300's GP106?
 *
 * No window, no WSI, no Metal. It loads the Vulkan driver directly (as the
 * Metal plugin does, plugin/nvmtl_vk.c) or through a loader, picks the GP106,
 * and runs four checks, each with a CPU readback:
 *
 *   1. fill:    vkCmdFillBuffer into device-local memory, copy to a host
 *               buffer, compare every word
 *   2. compute: dispatch smoke.comp (out[i] = 3i + 7 + bias), 4096 values
 *   3. clear:   vkCmdClearColorImage on a 256x256 RGBA8 optimal image,
 *               copied to a buffer, every pixel compared
 *   4. repeat:  the compute check 200 times with a new bias each time,
 *               reporting submit->fence time (no target, a baseline)
 *
 * Prints PASS/FAIL per check and exits 0 only if all pass.
 *
 * Usage: pg106_vk_smoke [--driver PATH | --loader PATH] [--any] [--list]
 *   default driver: /Library/GPUBundles/nvmtl/libvulkan_nouveau.dylib on
 *   macOS, the system loader (libvulkan.so.1) elsewhere.
 *   --any   use the first device, not only GP106 (to test the test on a
 *           machine without one)
 */
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "smoke_comp_spv.h"

#ifdef __APPLE__
#define DEFAULT_LIB "/Library/GPUBundles/nvmtl/libvulkan_nouveau.dylib"
#define DEFAULT_IS_ICD 1
#else
#define DEFAULT_LIB "libvulkan.so.1"
#define DEFAULT_IS_ICD 0
#endif

/* The GP106 ids the PG106 kext accepts (pascal/kext/src/pg106_device_gate.c). */
static const uint32_t gp106_ids[] = {
   0x1C20, 0x1C21, 0x1C22, 0x1C23, 0x1C60, 0x1C61, 0x1C62,
};

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkPhysicalDevice pdev;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam = UINT32_MAX;
static VkPhysicalDeviceMemoryProperties memp;
static VkCommandPool pool;
static int failed, passed;

#define I(f) static PFN_##f p##f
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties);
I(vkGetPhysicalDeviceProperties2); I(vkGetPhysicalDeviceQueueFamilyProperties);
I(vkGetPhysicalDeviceMemoryProperties); I(vkCreateDevice); I(vkGetDeviceProcAddr);
I(vkDestroyInstance);
I(vkGetDeviceQueue); I(vkDestroyDevice); I(vkCreateBuffer); I(vkDestroyBuffer);
I(vkGetBufferMemoryRequirements); I(vkAllocateMemory); I(vkFreeMemory);
I(vkBindBufferMemory); I(vkMapMemory); I(vkUnmapMemory); I(vkCreateCommandPool);
I(vkDestroyCommandPool); I(vkAllocateCommandBuffers); I(vkFreeCommandBuffers);
I(vkBeginCommandBuffer); I(vkEndCommandBuffer); I(vkCmdFillBuffer); I(vkCmdCopyBuffer);
I(vkCmdPipelineBarrier); I(vkQueueSubmit); I(vkCreateFence); I(vkDestroyFence);
I(vkWaitForFences); I(vkResetFences); I(vkCreateShaderModule); I(vkDestroyShaderModule);
I(vkCreateDescriptorSetLayout); I(vkDestroyDescriptorSetLayout); I(vkCreatePipelineLayout);
I(vkDestroyPipelineLayout); I(vkCreateComputePipelines); I(vkDestroyPipeline);
I(vkCreateDescriptorPool); I(vkDestroyDescriptorPool); I(vkAllocateDescriptorSets);
I(vkUpdateDescriptorSets); I(vkCmdBindPipeline); I(vkCmdBindDescriptorSets);
I(vkCmdPushConstants); I(vkCmdDispatch); I(vkCreateImage); I(vkDestroyImage);
I(vkGetImageMemoryRequirements); I(vkBindImageMemory); I(vkCmdClearColorImage);
I(vkCmdCopyImageToBuffer); I(vkInvalidateMappedMemoryRanges); I(vkDeviceWaitIdle);
#undef I

#define VK(call) do {                                                        \
   VkResult _r = (call);                                                     \
   if (_r != VK_SUCCESS) {                                                   \
      fprintf(stderr, "error: %s = %d (%s:%d)\n", #call, (int)_r, __FILE__,  \
              __LINE__);                                                     \
      exit(3);                                                               \
   }                                                                         \
} while (0)

static void
result(const char *name, bool ok, const char *detail)
{
   printf("%s %-8s %s\n", ok ? "PASS" : "FAIL", name, detail);
   if (ok) passed++; else failed++;
}

static uint64_t
now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void
load_instance_fns(void)
{
#define L(f) do { p##f = (PFN_##f)gipa(inst, #f); if (!p##f) { fprintf(stderr, "missing %s\n", #f); exit(3); } } while (0)
   L(vkEnumeratePhysicalDevices); L(vkGetPhysicalDeviceProperties);
   L(vkGetPhysicalDeviceProperties2); L(vkGetPhysicalDeviceQueueFamilyProperties);
   L(vkGetPhysicalDeviceMemoryProperties); L(vkCreateDevice); L(vkGetDeviceProcAddr);
   L(vkDestroyInstance);
#undef L
}

static void
load_device_fns(void)
{
#define D(f) do { p##f = (PFN_##f)pvkGetDeviceProcAddr(dev, #f); if (!p##f) { fprintf(stderr, "missing %s\n", #f); exit(3); } } while (0)
   D(vkGetDeviceQueue); D(vkDestroyDevice); D(vkCreateBuffer); D(vkDestroyBuffer);
   D(vkGetBufferMemoryRequirements); D(vkAllocateMemory); D(vkFreeMemory);
   D(vkBindBufferMemory); D(vkMapMemory); D(vkUnmapMemory); D(vkCreateCommandPool);
   D(vkDestroyCommandPool); D(vkAllocateCommandBuffers); D(vkFreeCommandBuffers);
   D(vkBeginCommandBuffer); D(vkEndCommandBuffer); D(vkCmdFillBuffer); D(vkCmdCopyBuffer);
   D(vkCmdPipelineBarrier); D(vkQueueSubmit); D(vkCreateFence); D(vkDestroyFence);
   D(vkWaitForFences); D(vkResetFences); D(vkCreateShaderModule); D(vkDestroyShaderModule);
   D(vkCreateDescriptorSetLayout); D(vkDestroyDescriptorSetLayout); D(vkCreatePipelineLayout);
   D(vkDestroyPipelineLayout); D(vkCreateComputePipelines); D(vkDestroyPipeline);
   D(vkCreateDescriptorPool); D(vkDestroyDescriptorPool); D(vkAllocateDescriptorSets);
   D(vkUpdateDescriptorSets); D(vkCmdBindPipeline); D(vkCmdBindDescriptorSets);
   D(vkCmdPushConstants); D(vkCmdDispatch); D(vkCreateImage); D(vkDestroyImage);
   D(vkGetImageMemoryRequirements); D(vkBindImageMemory); D(vkCmdClearColorImage);
   D(vkCmdCopyImageToBuffer); D(vkInvalidateMappedMemoryRanges); D(vkDeviceWaitIdle);
#undef D
}

static uint32_t
mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < memp.memoryTypeCount; i++) {
      if ((bits & (1u << i)) && (memp.memoryTypes[i].propertyFlags & want) == want)
         return i;
   }
   fprintf(stderr, "error: no memory type with flags 0x%x\n", want);
   exit(3);
}

struct buf { VkBuffer b; VkDeviceMemory m; void *map; VkDeviceSize size; };

static struct buf
buf_new(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props)
{
   struct buf r = { .size = size };
   VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, usage,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
   VK(pvkCreateBuffer(dev, &bci, NULL, &r.b));
   VkMemoryRequirements mr;
   pvkGetBufferMemoryRequirements(dev, r.b, &mr);
   VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                                mem_type(mr.memoryTypeBits, props) };
   VK(pvkAllocateMemory(dev, &mai, NULL, &r.m));
   VK(pvkBindBufferMemory(dev, r.b, r.m, 0));
   if (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
      VK(pvkMapMemory(dev, r.m, 0, VK_WHOLE_SIZE, 0, &r.map));
   return r;
}

static void
buf_free(struct buf *b)
{
   if (b->map) pvkUnmapMemory(dev, b->m);
   pvkDestroyBuffer(dev, b->b, NULL);
   pvkFreeMemory(dev, b->m, NULL);
}

static void
host_read_barrier(VkCommandBuffer cb)
{
   VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL,
                          VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                          VK_ACCESS_HOST_READ_BIT };
   pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

static VkCommandBuffer
cb_begin(void)
{
   VkCommandBuffer cb;
   VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool,
                                      VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
   VK(pvkAllocateCommandBuffers(dev, &ai, &cb));
   VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                   VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
   VK(pvkBeginCommandBuffer(cb, &bi));
   return cb;
}

/* Submit, wait (10 s bound), return elapsed ns or 0 on timeout. */
static uint64_t
cb_submit_wait(VkCommandBuffer cb)
{
   VK(pvkEndCommandBuffer(cb));
   VkFence f;
   VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0 };
   VK(pvkCreateFence(dev, &fci, NULL, &f));
   VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL };
   const uint64_t t0 = now_ns();
   VK(pvkQueueSubmit(queue, 1, &si, f));
   VkResult r = pvkWaitForFences(dev, 1, &f, VK_TRUE, 10ull * 1000000000ull);
   const uint64_t dt = now_ns() - t0;
   pvkDestroyFence(dev, f, NULL);
   pvkFreeCommandBuffers(dev, pool, 1, &cb);
   if (r != VK_SUCCESS) {
      fprintf(stderr, "error: fence wait = %d (10 s bound)\n", (int)r);
      return 0;
   }
   return dt ? dt : 1;
}

static void
check_fill(void)
{
   const VkDeviceSize size = 1 << 20;
   struct buf src = buf_new(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   struct buf dst = buf_new(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   memset(dst.map, 0, size);
   VkCommandBuffer cb = cb_begin();
   pvkCmdFillBuffer(cb, src.b, 0, size, 0xA5C3E10Fu);
   VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_ACCESS_TRANSFER_READ_BIT };
   pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         1, &mb, 0, NULL, 0, NULL);
   VkBufferCopy bc = { 0, 0, size };
   pvkCmdCopyBuffer(cb, src.b, dst.b, 1, &bc);
   host_read_barrier(cb);
   const uint64_t ns = cb_submit_wait(cb);

   size_t bad = 0;
   const uint32_t *w = dst.map;
   for (size_t i = 0; i < size / 4; i++)
      bad += w[i] != 0xA5C3E10Fu;
   char d[160];
   snprintf(d, sizeof d, "1 MiB fill in VRAM + copy to host: %zu bad words, first 0x%08x, %.3f ms",
            bad, w[0], ns / 1e6);
   result("fill", ns && bad == 0, d);
   buf_free(&src);
   buf_free(&dst);
}

struct compute {
   VkShaderModule sm; VkDescriptorSetLayout dsl; VkPipelineLayout pl; VkPipeline p;
   VkDescriptorPool dp; VkDescriptorSet ds; struct buf out;
};

static struct compute
compute_new(uint32_t n)
{
   struct compute c;
   VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0,
                                     sizeof smoke_comp_spv, smoke_comp_spv };
   VK(pvkCreateShaderModule(dev, &smci, NULL, &c.sm));
   VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                      VK_SHADER_STAGE_COMPUTE_BIT, NULL };
   VkDescriptorSetLayoutCreateInfo dslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                             NULL, 0, 1, &b };
   VK(pvkCreateDescriptorSetLayout(dev, &dslci, NULL, &c.dsl));
   VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 8 };
   VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0,
                                       1, &c.dsl, 1, &pcr };
   VK(pvkCreatePipelineLayout(dev, &plci, NULL, &c.pl));
   VkComputePipelineCreateInfo cpci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
      { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT,
        c.sm, "main", NULL }, c.pl, VK_NULL_HANDLE, 0 };
   VK(pvkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &c.p));
   VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
   VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0,
                                       1, 1, &ps };
   VK(pvkCreateDescriptorPool(dev, &dpci, NULL, &c.dp));
   VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL,
                                        c.dp, 1, &c.dsl };
   VK(pvkAllocateDescriptorSets(dev, &dsai, &c.ds));
   c.out = buf_new(n * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   VkDescriptorBufferInfo dbi = { c.out.b, 0, VK_WHOLE_SIZE };
   VkWriteDescriptorSet wds = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, c.ds, 0, 0, 1,
                                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dbi, NULL };
   pvkUpdateDescriptorSets(dev, 1, &wds, 0, NULL);
   return c;
}

static uint64_t
compute_run(struct compute *c, uint32_t n, uint32_t bias, size_t *bad_out)
{
   memset(c->out.map, 0xff, n * 4);
   VkCommandBuffer cb = cb_begin();
   pvkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c->p);
   pvkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c->pl, 0, 1, &c->ds, 0, NULL);
   const uint32_t pc[2] = { n, bias };
   pvkCmdPushConstants(cb, c->pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
   pvkCmdDispatch(cb, (n + 63) / 64, 1, 1);
   host_read_barrier(cb);
   const uint64_t ns = cb_submit_wait(cb);
   size_t bad = 0;
   const uint32_t *v = c->out.map;
   for (uint32_t i = 0; i < n; i++)
      bad += v[i] != i * 3u + 7u + bias;
   *bad_out = bad;
   return ns;
}

static void
compute_free(struct compute *c)
{
   buf_free(&c->out);
   pvkDestroyDescriptorPool(dev, c->dp, NULL);
   pvkDestroyPipeline(dev, c->p, NULL);
   pvkDestroyPipelineLayout(dev, c->pl, NULL);
   pvkDestroyDescriptorSetLayout(dev, c->dsl, NULL);
   pvkDestroyShaderModule(dev, c->sm, NULL);
}

static void
check_compute(void)
{
   const uint32_t n = 4096;
   struct compute c = compute_new(n);
   size_t bad = 0;
   const uint64_t ns = compute_run(&c, n, 0, &bad);
   const uint32_t *v = c.out.map;
   char d[160];
   snprintf(d, sizeof d, "4096 x (3i+7): %zu bad, v[0]=%u v[4095]=%u, %.3f ms",
            bad, v[0], v[n - 1], ns / 1e6);
   result("compute", ns && bad == 0, d);

   /* 4: the same dispatch 200 times, new bias each time */
   uint64_t total = 0, worst = 0;
   size_t bad_runs = 0;
   int timeouts = 0;
   for (uint32_t k = 1; k <= 200; k++) {
      size_t b = 0;
      const uint64_t t = compute_run(&c, n, k, &b);
      if (!t) { timeouts++; break; }
      total += t;
      if (t > worst) worst = t;
      bad_runs += b != 0;
   }
   snprintf(d, sizeof d, "200 dispatches: %zu with wrong data, %d timeouts, mean %.3f ms, worst %.3f ms",
            bad_runs, timeouts, total / 200.0 / 1e6, worst / 1e6);
   result("repeat", timeouts == 0 && bad_runs == 0, d);
   compute_free(&c);
}

static void
check_clear(void)
{
   const uint32_t w = 256, h = 256;
   VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D,
      VK_FORMAT_R8G8B8A8_UNORM, { w, h, 1 }, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE,
      0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
   VkImage img;
   VK(pvkCreateImage(dev, &ici, NULL, &img));
   VkMemoryRequirements mr;
   pvkGetImageMemoryRequirements(dev, img, &mr);
   VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                                mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
   VkDeviceMemory im;
   VK(pvkAllocateMemory(dev, &mai, NULL, &im));
   VK(pvkBindImageMemory(dev, img, im, 0));
   struct buf out = buf_new(w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   memset(out.map, 0, w * h * 4);

   VkCommandBuffer cb = cb_begin();
   VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
   VkImageMemoryBarrier ib = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0,
      VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, rng };
   pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 1, &ib);
   /* (0.2, 0.4, 0.6, 1.0) -> UNORM8 51, 102, 153, 255 */
   VkClearColorValue cc = { .float32 = { 0.2f, 0.4f, 0.6f, 1.0f } };
   pvkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &rng);
   ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
   ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 1, &ib);
   VkBufferImageCopy bic = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 },
                             { w, h, 1 } };
   pvkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out.b, 1, &bic);
   host_read_barrier(cb);
   const uint64_t ns = cb_submit_wait(cb);

   size_t bad = 0;
   const uint8_t *p = out.map;
   for (size_t i = 0; i < (size_t)w * h; i++) {
      /* allow +-1 for the float->UNORM rounding mode */
      const int dr = p[4 * i] - 51, dg = p[4 * i + 1] - 102, db = p[4 * i + 2] - 153;
      bad += dr < -1 || dr > 1 || dg < -1 || dg > 1 || db < -1 || db > 1 || p[4 * i + 3] != 255;
   }
   char d[160];
   snprintf(d, sizeof d, "256x256 RGBA8 clear + copy: %zu bad pixels, first %u,%u,%u,%u, %.3f ms",
            bad, p[0], p[1], p[2], p[3], ns / 1e6);
   result("clear", ns && bad == 0, d);
   buf_free(&out);
   pvkDestroyImage(dev, img, NULL);
   pvkFreeMemory(dev, im, NULL);
}

static bool
is_gp106(const VkPhysicalDeviceProperties *p)
{
   if (p->vendorID != 0x10de)
      return false;
   for (size_t i = 0; i < sizeof gp106_ids / sizeof gp106_ids[0]; i++)
      if (p->deviceID == gp106_ids[i])
         return true;
   return false;
}

int
main(int argc, char **argv)
{
   const char *lib = DEFAULT_LIB;
   int is_icd = DEFAULT_IS_ICD, any = 0, list = 0;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--driver") && i + 1 < argc) { lib = argv[++i]; is_icd = 1; }
      else if (!strcmp(argv[i], "--loader") && i + 1 < argc) { lib = argv[++i]; is_icd = 0; }
      else if (!strcmp(argv[i], "--any")) any = 1;
      else if (!strcmp(argv[i], "--list")) list = 1;
      else { fprintf(stderr, "usage: %s [--driver PATH | --loader PATH] [--any] [--list]\n", argv[0]); return 2; }
   }

   void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
   if (!h) { fprintf(stderr, "error: dlopen(%s): %s\n", lib, dlerror()); return 3; }
   gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, is_icd ? "vk_icdGetInstanceProcAddr" : "vkGetInstanceProcAddr");
   if (!gipa) gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
   if (!gipa) { fprintf(stderr, "error: %s exports no GetInstanceProcAddr\n", lib); return 3; }
   printf("driver: %s (%s)\n", lib, is_icd ? "ICD, no loader" : "loader");

   pvkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "pg106_vk_smoke", 1, NULL, 0,
                             VK_API_VERSION_1_2 };
   VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL };
   VK(pvkCreateInstance(&ici, NULL, &inst));
   load_instance_fns();

   uint32_t n = 0;
   VK(pvkEnumeratePhysicalDevices(inst, &n, NULL));
   VkPhysicalDevice devs[16];
   if (n > 16) n = 16;
   VK(pvkEnumeratePhysicalDevices(inst, &n, devs));
   printf("devices: %u\n", n);
   for (uint32_t i = 0; i < n; i++) {
      VkPhysicalDeviceDriverProperties drv = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
      VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv };
      pvkGetPhysicalDeviceProperties2(devs[i], &p2);
      const VkPhysicalDeviceProperties *p = &p2.properties;
      printf("  [%u] %04x:%04x %s, Vulkan %u.%u.%u, driver %s (%s)%s\n", i, p->vendorID, p->deviceID,
             p->deviceName, VK_API_VERSION_MAJOR(p->apiVersion), VK_API_VERSION_MINOR(p->apiVersion),
             VK_API_VERSION_PATCH(p->apiVersion), drv.driverName, drv.driverInfo,
             is_gp106(p) ? "  <- GP106" : "");
      if (pdev == VK_NULL_HANDLE && (any || is_gp106(p)))
         pdev = devs[i];
   }
   if (list) return 0;
   if (pdev == VK_NULL_HANDLE) {
      printf("FAIL no GP106 Vulkan device (is PG106 loaded and brought up? see OWNER-RUNBOOK.md step 3)\n");
      return 1;
   }

   pvkGetPhysicalDeviceMemoryProperties(pdev, &memp);
   for (uint32_t i = 0; i < memp.memoryHeapCount; i++)
      printf("  heap %u: %" PRIu64 " MiB%s\n", i, (uint64_t)(memp.memoryHeaps[i].size >> 20),
             (memp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " device-local" : "");
   uint32_t qn = 0;
   pvkGetPhysicalDeviceQueueFamilyProperties(pdev, &qn, NULL);
   VkQueueFamilyProperties qf[16];
   if (qn > 16) qn = 16;
   pvkGetPhysicalDeviceQueueFamilyProperties(pdev, &qn, qf);
   for (uint32_t i = 0; i < qn; i++) {
      const VkQueueFlags need = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
      if ((qf[i].queueFlags & need) == need) { qfam = i; break; }
   }
   if (qfam == UINT32_MAX) { printf("FAIL no graphics+compute queue family\n"); return 1; }

   const float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio };
   VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, NULL };
   VK(pvkCreateDevice(pdev, &dci, NULL, &dev));
   load_device_fns();
   pvkGetDeviceQueue(dev, qfam, 0, &queue);
   VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                    VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfam };
   VK(pvkCreateCommandPool(dev, &cpci, NULL, &pool));

   check_fill();
   check_compute();
   check_clear();

   VK(pvkDeviceWaitIdle(dev));
   pvkDestroyCommandPool(dev, pool, NULL);
   pvkDestroyDevice(dev, NULL);
   pvkDestroyInstance(inst, NULL);
   printf("pg106_vk_smoke: %d/%d passed\n", passed, passed + failed);
   return failed ? 1 : 0;
}
