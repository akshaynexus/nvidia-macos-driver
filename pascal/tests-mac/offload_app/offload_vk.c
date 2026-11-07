/* SPDX-License-Identifier: MIT */
#include "offload_vk.h"

#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "offload_comp_spv.h"

static const uint32_t gp106_ids[] = { 0x1C20, 0x1C21, 0x1C22, 0x1C23, 0x1C60, 0x1C61, 0x1C62 };

#define FNS(X) \
   X(vkGetDeviceQueue) X(vkDestroyDevice) X(vkCreateBuffer) X(vkDestroyBuffer) \
   X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) \
   X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkCreateCommandPool) \
   X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandBuffer) \
   X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) \
   X(vkQueueSubmit) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) \
   X(vkResetFences) X(vkCreateShaderModule) X(vkDestroyShaderModule) \
   X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) \
   X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateComputePipelines) \
   X(vkDestroyPipeline) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) \
   X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCmdBindPipeline) \
   X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDispatch) \
   X(vkDeviceWaitIdle) X(vkGetMemoryHostPointerPropertiesEXT)

struct ovk {
   void *lib;
   PFN_vkGetInstanceProcAddr gipa;
   VkInstance inst;
   VkPhysicalDevice pdev;
   VkDevice dev;
   VkQueue queue;
   uint32_t qfam;
   VkPhysicalDeviceMemoryProperties memp;
   int has_host_import;
   VkDeviceSize host_align;
   char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];

#define DECL(f) PFN_##f f;
   FNS(DECL)
#undef DECL
   PFN_vkDestroyInstance vkDestroyInstance;

   VkCommandPool pool;
   VkCommandBuffer cb;
   VkFence fence;
   VkShaderModule sm;
   VkDescriptorSetLayout dsl;
   VkPipelineLayout pl;
   VkPipeline pipe;
   VkDescriptorPool dp;

   uint32_t n_targets;
   struct ovk_target {
      void *base;
      uint32_t w, h, pitch_B;
      int zero_copy;
      VkBuffer buf;
      VkDeviceMemory mem;
      void *staging;   /* copy mode: the mapped Vulkan buffer */
      VkDescriptorSet ds;
   } t[OVK_MAX_TARGETS];
};

static uint64_t
now_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#define CK(call) do {                                                       \
   VkResult _r = (call);                                                    \
   if (_r != VK_SUCCESS) {                                                  \
      fprintf(stderr, "offload_vk: %s = %d\n", #call, (int)_r);             \
      return -1;                                                            \
   }                                                                        \
} while (0)

static int
is_gp106(const VkPhysicalDeviceProperties *p)
{
   if (p->vendorID != 0x10de)
      return 0;
   for (size_t i = 0; i < sizeof gp106_ids / sizeof gp106_ids[0]; i++)
      if (p->deviceID == gp106_ids[i])
         return 1;
   return 0;
}

static int
mem_type(const struct ovk *o, uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < o->memp.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (o->memp.memoryTypes[i].propertyFlags & want) == want)
         return (int)i;
   return -1;
}

int
ovk_open(struct ovk **out, const char *lib, int is_icd, int any)
{
   struct ovk *o = calloc(1, sizeof(*o));
   if (!o)
      return -1;
   *out = o;
   o->lib = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
   if (!o->lib) {
      fprintf(stderr, "offload_vk: dlopen(%s): %s\n", lib, dlerror());
      return -1;
   }
   o->gipa = (PFN_vkGetInstanceProcAddr)dlsym(o->lib, is_icd ? "vk_icdGetInstanceProcAddr"
                                                             : "vkGetInstanceProcAddr");
   if (!o->gipa)
      o->gipa = (PFN_vkGetInstanceProcAddr)dlsym(o->lib, "vkGetInstanceProcAddr");
   if (!o->gipa) {
      fprintf(stderr, "offload_vk: %s has no GetInstanceProcAddr\n", lib);
      return -1;
   }

   PFN_vkCreateInstance ci = (PFN_vkCreateInstance)o->gipa(NULL, "vkCreateInstance");
   VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "pg106_offload", 1, NULL, 0,
                             VK_API_VERSION_1_2 };
   VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL };
   CK(ci(&ici, NULL, &o->inst));
   o->vkDestroyInstance = (PFN_vkDestroyInstance)o->gipa(o->inst, "vkDestroyInstance");

   PFN_vkEnumeratePhysicalDevices epd =
      (PFN_vkEnumeratePhysicalDevices)o->gipa(o->inst, "vkEnumeratePhysicalDevices");
   PFN_vkGetPhysicalDeviceProperties gpp =
      (PFN_vkGetPhysicalDeviceProperties)o->gipa(o->inst, "vkGetPhysicalDeviceProperties");
   PFN_vkGetPhysicalDeviceProperties2 gpp2 =
      (PFN_vkGetPhysicalDeviceProperties2)o->gipa(o->inst, "vkGetPhysicalDeviceProperties2");
   PFN_vkEnumerateDeviceExtensionProperties edx =
      (PFN_vkEnumerateDeviceExtensionProperties)o->gipa(o->inst, "vkEnumerateDeviceExtensionProperties");
   PFN_vkGetPhysicalDeviceQueueFamilyProperties gqf =
      (PFN_vkGetPhysicalDeviceQueueFamilyProperties)o->gipa(o->inst, "vkGetPhysicalDeviceQueueFamilyProperties");
   PFN_vkGetPhysicalDeviceMemoryProperties gmp =
      (PFN_vkGetPhysicalDeviceMemoryProperties)o->gipa(o->inst, "vkGetPhysicalDeviceMemoryProperties");
   PFN_vkCreateDevice cd = (PFN_vkCreateDevice)o->gipa(o->inst, "vkCreateDevice");
   PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)o->gipa(o->inst, "vkGetDeviceProcAddr");

   uint32_t n = 0;
   CK(epd(o->inst, &n, NULL));
   VkPhysicalDevice devs[16];
   if (n > 16) n = 16;
   CK(epd(o->inst, &n, devs));
   for (uint32_t i = 0; i < n; i++) {
      VkPhysicalDeviceProperties p;
      gpp(devs[i], &p);
      printf("vulkan device [%u] %04x:%04x %s%s\n", i, p.vendorID, p.deviceID, p.deviceName,
             is_gp106(&p) ? "  <- GP106" : "");
      if (!o->pdev && (any || is_gp106(&p))) {
         o->pdev = devs[i];
         snprintf(o->name, sizeof o->name, "%s", p.deviceName);
      }
   }
   if (!o->pdev) {
      fprintf(stderr, "offload_vk: no GP106 Vulkan device (OWNER-RUNBOOK.md step 3 first)\n");
      return -2;
   }

   uint32_t ne = 0;
   CK(edx(o->pdev, NULL, &ne, NULL));
   VkExtensionProperties *ext = calloc(ne ? ne : 1, sizeof(*ext));
   CK(edx(o->pdev, NULL, &ne, ext));
   for (uint32_t i = 0; i < ne; i++)
      if (!strcmp(ext[i].extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME))
         o->has_host_import = 1;
   free(ext);
   if (o->has_host_import) {
      VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
      VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                         .pNext = &hp };
      gpp2(o->pdev, &p2);
      o->host_align = hp.minImportedHostPointerAlignment;
   }

   gmp(o->pdev, &o->memp);
   uint32_t qn = 0;
   gqf(o->pdev, &qn, NULL);
   VkQueueFamilyProperties qf[16];
   if (qn > 16) qn = 16;
   gqf(o->pdev, &qn, qf);
   o->qfam = UINT32_MAX;
   for (uint32_t i = 0; i < qn && o->qfam == UINT32_MAX; i++)
      if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
         o->qfam = i;
   if (o->qfam == UINT32_MAX)
      return -1;

   const float prio = 1.0f;
   const char *exts[] = { VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME };
   VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, o->qfam, 1, &prio };
   VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL,
                              o->has_host_import ? 1u : 0u, exts, NULL };
   CK(cd(o->pdev, &dci, NULL, &o->dev));
#define LOAD(f) o->f = (PFN_##f)gdpa(o->dev, #f);
   FNS(LOAD)
#undef LOAD
   if (!o->vkGetMemoryHostPointerPropertiesEXT)
      o->has_host_import = 0;
   o->vkGetDeviceQueue(o->dev, o->qfam, 0, &o->queue);

   VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                    VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, o->qfam };
   CK(o->vkCreateCommandPool(o->dev, &cpci, NULL, &o->pool));
   VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, o->pool,
                                       VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
   CK(o->vkAllocateCommandBuffers(o->dev, &cai, &o->cb));
   VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0 };
   CK(o->vkCreateFence(o->dev, &fci, NULL, &o->fence));

   VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0,
                                     sizeof offload_comp_spv, offload_comp_spv };
   CK(o->vkCreateShaderModule(o->dev, &smci, NULL, &o->sm));
   VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                      VK_SHADER_STAGE_COMPUTE_BIT, NULL };
   VkDescriptorSetLayoutCreateInfo dslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                             NULL, 0, 1, &b };
   CK(o->vkCreateDescriptorSetLayout(o->dev, &dslci, NULL, &o->dsl));
   VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 16 };
   VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1,
                                       &o->dsl, 1, &pcr };
   CK(o->vkCreatePipelineLayout(o->dev, &plci, NULL, &o->pl));
   VkComputePipelineCreateInfo cpi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
      { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT,
        o->sm, "main", NULL }, o->pl, VK_NULL_HANDLE, 0 };
   CK(o->vkCreateComputePipelines(o->dev, VK_NULL_HANDLE, 1, &cpi, NULL, &o->pipe));
   VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, OVK_MAX_TARGETS };
   VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0,
                                       OVK_MAX_TARGETS, 1, &ps };
   CK(o->vkCreateDescriptorPool(o->dev, &dpci, NULL, &o->dp));
   return 0;
}

static int
attach_import(struct ovk *o, struct ovk_target *t, void *base, size_t alloc_B)
{
   if (!o->has_host_import || !o->host_align ||
       ((uintptr_t)base % o->host_align) || (alloc_B % o->host_align)) {
      fprintf(stderr, "offload_vk: host import unavailable (extension %s, alignment %llu, "
              "base %p, size %zu)\n", o->has_host_import ? "present" : "absent",
              (unsigned long long)o->host_align, base, alloc_B);
      return -1;
   }
   VkMemoryHostPointerPropertiesEXT hpp = { .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
   if (o->vkGetMemoryHostPointerPropertiesEXT(o->dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                              base, &hpp) != VK_SUCCESS || !hpp.memoryTypeBits)
      return -1;
   VkExternalMemoryBufferCreateInfo emb = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL,
                                            VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT };
   VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &emb, 0, alloc_B,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
   if (o->vkCreateBuffer(o->dev, &bci, NULL, &t->buf) != VK_SUCCESS)
      return -1;
   VkMemoryRequirements mr;
   o->vkGetBufferMemoryRequirements(o->dev, t->buf, &mr);
   int mt = mem_type(o, mr.memoryTypeBits & hpp.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
   if (mt < 0) {
      o->vkDestroyBuffer(o->dev, t->buf, NULL);
      t->buf = VK_NULL_HANDLE;
      return -1;
   }
   VkImportMemoryHostPointerInfoEXT imp = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, NULL,
                                            VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, base };
   VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, alloc_B, (uint32_t)mt };
   if (o->vkAllocateMemory(o->dev, &mai, NULL, &t->mem) != VK_SUCCESS) {
      o->vkDestroyBuffer(o->dev, t->buf, NULL);
      t->buf = VK_NULL_HANDLE;
      return -1;
   }
   CK(o->vkBindBufferMemory(o->dev, t->buf, t->mem, 0));
   t->zero_copy = 1;
   return 0;
}

int
ovk_attach(struct ovk *o, void *base, size_t alloc_B, uint32_t w, uint32_t h,
           uint32_t pitch_B, int force_copy, uint32_t *index_out)
{
   if (o->n_targets == OVK_MAX_TARGETS || pitch_B % 4 || (size_t)pitch_B * h > alloc_B)
      return -1;
   struct ovk_target *t = &o->t[o->n_targets];
   memset(t, 0, sizeof(*t));
   t->base = base;
   t->w = w;
   t->h = h;
   t->pitch_B = pitch_B;

   if (force_copy || attach_import(o, t, base, alloc_B) != 0) {
      t->zero_copy = 0;
      VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0,
                                 (VkDeviceSize)pitch_B * h, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                 VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
      CK(o->vkCreateBuffer(o->dev, &bci, NULL, &t->buf));
      VkMemoryRequirements mr;
      o->vkGetBufferMemoryRequirements(o->dev, t->buf, &mr);
      int mt = mem_type(o, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      if (mt < 0)
         return -1;
      VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, (uint32_t)mt };
      CK(o->vkAllocateMemory(o->dev, &mai, NULL, &t->mem));
      CK(o->vkBindBufferMemory(o->dev, t->buf, t->mem, 0));
      CK(o->vkMapMemory(o->dev, t->mem, 0, VK_WHOLE_SIZE, 0, &t->staging));
   }

   VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, o->dp, 1, &o->dsl };
   CK(o->vkAllocateDescriptorSets(o->dev, &dsai, &t->ds));
   VkDescriptorBufferInfo dbi = { t->buf, 0, VK_WHOLE_SIZE };
   VkWriteDescriptorSet wds = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, t->ds, 0, 0, 1,
                                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dbi, NULL };
   o->vkUpdateDescriptorSets(o->dev, 1, &wds, 0, NULL);
   *index_out = o->n_targets++;
   return 0;
}

const char *
ovk_mode(const struct ovk *o, uint32_t index)
{
   return o->t[index].zero_copy ? "zero-copy (host import)" : "copy (staging buffer + memcpy)";
}

const char *
ovk_device_name(const struct ovk *o)
{
   return o->name;
}

int
ovk_frame(struct ovk *o, uint32_t index, uint32_t frame, uint64_t *gpu_ns, uint64_t *copy_ns)
{
   const struct ovk_target *t = &o->t[index];
   CK(o->vkResetCommandBuffer(o->cb, 0));
   VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                   VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
   CK(o->vkBeginCommandBuffer(o->cb, &bi));
   o->vkCmdBindPipeline(o->cb, VK_PIPELINE_BIND_POINT_COMPUTE, o->pipe);
   o->vkCmdBindDescriptorSets(o->cb, VK_PIPELINE_BIND_POINT_COMPUTE, o->pl, 0, 1, &t->ds, 0, NULL);
   const uint32_t pc[4] = { t->w, t->h, t->pitch_B / 4, frame };
   o->vkCmdPushConstants(o->cb, o->pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
   o->vkCmdDispatch(o->cb, (t->w + 15) / 16, (t->h + 15) / 16, 1);
   VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT,
                          VK_ACCESS_HOST_READ_BIT };
   o->vkCmdPipelineBarrier(o->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                           0, 1, &mb, 0, NULL, 0, NULL);
   CK(o->vkEndCommandBuffer(o->cb));
   VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &o->cb, 0, NULL };
   const uint64_t t0 = now_ns();
   CK(o->vkQueueSubmit(o->queue, 1, &si, o->fence));
   /* Completion is observed before the surface is published (OFFLOAD-DESIGN
    * section 2.3: no fence travels with the IOSurface). */
   CK(o->vkWaitForFences(o->dev, 1, &o->fence, VK_TRUE, 10ull * 1000000000ull));
   const uint64_t t1 = now_ns();
   CK(o->vkResetFences(o->dev, 1, &o->fence));
   *gpu_ns = t1 - t0;
   *copy_ns = 0;
   if (!t->zero_copy) {
      for (uint32_t y = 0; y < t->h; y++)
         memcpy((char *)t->base + (size_t)y * t->pitch_B,
                (const char *)t->staging + (size_t)y * t->pitch_B, (size_t)t->w * 4);
      *copy_ns = now_ns() - t1;
   }
   return 0;
}

uint32_t
ovk_verify(const void *base, uint32_t w, uint32_t h, uint32_t pitch_B, uint32_t frame)
{
   uint32_t bad = 0;
   for (uint32_t y = 0; y < h; y += 7) {
      const uint32_t *row = (const uint32_t *)((const char *)base + (size_t)y * pitch_B);
      for (uint32_t x = 0; x < w; x += 5)
         bad += row[x] != ovk_expected(x, y, frame, w);
   }
   return bad;
}

void
ovk_close(struct ovk *o)
{
   if (!o)
      return;
   if (o->dev) {
      o->vkDeviceWaitIdle(o->dev);
      for (uint32_t i = 0; i < o->n_targets; i++) {
         struct ovk_target *t = &o->t[i];
         if (t->staging) o->vkUnmapMemory(o->dev, t->mem);
         if (t->buf) o->vkDestroyBuffer(o->dev, t->buf, NULL);
         if (t->mem) o->vkFreeMemory(o->dev, t->mem, NULL);
      }
      o->vkDestroyDescriptorPool(o->dev, o->dp, NULL);
      o->vkDestroyPipeline(o->dev, o->pipe, NULL);
      o->vkDestroyPipelineLayout(o->dev, o->pl, NULL);
      o->vkDestroyDescriptorSetLayout(o->dev, o->dsl, NULL);
      o->vkDestroyShaderModule(o->dev, o->sm, NULL);
      o->vkDestroyFence(o->dev, o->fence, NULL);
      o->vkDestroyCommandPool(o->dev, o->pool, NULL);
      o->vkDestroyDevice(o->dev, NULL);
   }
   if (o->inst && o->vkDestroyInstance)
      o->vkDestroyInstance(o->inst, NULL);
   free(o);
}
