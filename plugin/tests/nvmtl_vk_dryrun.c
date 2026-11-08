/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
// nvmtl_vk_dryrun.c - the plugin's Vulkan side, replayed on Linux against NVK's Pascal backend.
//
//   nvmtl_vk_dryrun --driver <ICD .so> [--spv <file.spv>]... [--dispatch <file.spv>] [--blit] [--no-gate]
//
// The plugin (nvmtl_vk.c) cannot build off macOS (Mach-O, IOKit, CoreFoundation). This program repeats what it does
// on the way to the first Metal compute dispatch, with the same values:
//   1. vkCreateInstance, vkEnumeratePhysicalDevices, the 10de device;
//   2. nvmtl_vk_init's device creation: the 17 wanted extensions (nvmtl_vk_device_exts.h, the plugin's own list),
//      the feature chain it builds from what the driver reports (bufferDeviceAddress always, extended dynamic state 3
//      polygon mode + depth clip, mutable descriptors, pipeline robustness, vertex divisor, provoking vertex, Float16 /
//      Int8 / 16-bit storage / memory model / int64 atomics / descriptor indexing as reported), shaderInt64 required;
//   3. ensure_descriptors_locked's set layout: NVMTL_NBIND bindings (32 storage buffers, 128 sampled images,
//      32 samplers, the address-table buffer at 640, 128 storage images from 480), mutable bands when the extension
//      is there; the bindless heap (set 2: 5 x update-after-bind arrays of 131072 / 2048, falling back to 2 and 1
//      bindings as the plugin does); the compute pipeline layout { set, set, heap } + 48 push-constant bytes
//      (nvmtl_vk_compute_pipeline);
//   4. per --spv: the plugin's capability gate (nvmtl_spirv_ok: Float16 needs shaderFloat16, CooperativeMatrixKHR the
//      extension, ...; --no-gate skips it), then vkCreateShaderModule + vkCreateComputePipelines "main" (NVK runs NAK
//      for SM 6.1 here);
//   5. --dispatch: one dispatch of that pipeline with a 64 KiB storage buffer bound at every buffer binding the
//      module declares, submitted with a fence (the plugin's per-command-buffer path);
//   6. --blit: vkCmdCopyBuffer 1 MiB between two buffers (a Metal blit encoder's copyFromBuffer).
// One "ok"/"FAIL" line per step, exit 0 when every step passed.
//
// With the sim ICD (nvk/pascal/tests/sim_icd.c) the device is NVK + NAK over a simulated GP106: everything up to
// the submission is real, and only host methods (semaphores) execute, so no data is checked. With PG106_KDSIM=1 the
// sim ICD routes the bridge into the kext's own dispatcher (pascal/tools/pg106_kdsim.c) instead of the recording mock.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

#include "../nvmtl_vk_device_exts.h"

// nvmtl_vk.c constants (keep in step; nvmtl_vk_dryrun.py checks them against the source)
#define NVMTL_NBUF 32
#define NVMTL_NTEX 128
#define NVMTL_NSAMP 32
#define NVMTL_ADDRESS_BINDING (NVMTL_NBUF + NVMTL_NTEX + NVMTL_NSAMP)
#define NVMTL_ADDRESS_BINDING_NUMBER 640
#define NVMTL_NSTEX 128
#define NVMTL_STEX_BASE (NVMTL_ADDRESS_BINDING + 1)
#define NVMTL_STEX_BINDING_NUMBER 480
#define NVMTL_NBIND (NVMTL_STEX_BASE + NVMTL_NSTEX)
#define NVMTL_DISPATCH_PC_BYTES 48
#define NVMTL_BINDLESS_SLOTS 131072
#define NVMTL_BINDLESS_SAMPLER_SLOTS 2048
#define NVMTL_BINDING_NUMBER(i) ((i) == NVMTL_ADDRESS_BINDING ? NVMTL_ADDRESS_BINDING_NUMBER : \
                                 (i) >= NVMTL_STEX_BASE ? NVMTL_STEX_BINDING_NUMBER + ((i) - NVMTL_STEX_BASE) : (i))

static PFN_vkGetInstanceProcAddr gipa;
static int passed, failed;
#define STEP(ok, ...) do { if (ok) { passed++; printf("ok   "); } else { failed++; printf("FAIL "); } printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

#define IFN(n) static PFN_##n p##n
IFN(vkCreateInstance); IFN(vkEnumeratePhysicalDevices); IFN(vkGetPhysicalDeviceProperties); IFN(vkGetPhysicalDeviceProperties2);
IFN(vkGetPhysicalDeviceFeatures); IFN(vkGetPhysicalDeviceFeatures2); IFN(vkEnumerateDeviceExtensionProperties);
IFN(vkGetPhysicalDeviceQueueFamilyProperties); IFN(vkGetPhysicalDeviceMemoryProperties); IFN(vkCreateDevice);
IFN(vkGetDeviceProcAddr); IFN(vkDestroyInstance);
IFN(vkGetDeviceQueue); IFN(vkCreateDescriptorSetLayout); IFN(vkCreatePipelineLayout); IFN(vkCreateShaderModule);
IFN(vkCreateComputePipelines); IFN(vkCreateBuffer); IFN(vkGetBufferMemoryRequirements); IFN(vkAllocateMemory);
IFN(vkBindBufferMemory); IFN(vkCreateDescriptorPool); IFN(vkAllocateDescriptorSets); IFN(vkUpdateDescriptorSets);
IFN(vkCreateCommandPool); IFN(vkAllocateCommandBuffers); IFN(vkBeginCommandBuffer); IFN(vkEndCommandBuffer);
IFN(vkCmdBindPipeline); IFN(vkCmdBindDescriptorSets); IFN(vkCmdPushConstants); IFN(vkCmdDispatch); IFN(vkCmdCopyBuffer);
IFN(vkCreateFence); IFN(vkQueueSubmit); IFN(vkWaitForFences); IFN(vkDeviceWaitIdle); IFN(vkDestroyDevice);
IFN(vkMapMemory); IFN(vkDestroyPipeline); IFN(vkDestroyShaderModule);
static VkPipeline made[512]; static VkShaderModule mods[512]; static int nmade, nmods;

static VkInstance inst;
static VkPhysicalDevice pdev;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam = UINT32_MAX;
static VkPhysicalDeviceMemoryProperties memp;
static VkDescriptorSetLayout setLayout, heapLayout;
static int g_mutable, g_bindless;
// what nvmtl_spirv_ok gates on (filled from the same feature query the plugin makes)
static int g_float16, g_storage16, g_uniform_storage16, g_vmm, g_vmm_device, g_coop_matrix, g_atomic64;
static int gated, banded;

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
	for (uint32_t i = 0; i < memp.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (memp.memoryTypes[i].propertyFlags & want) == want) return i;
	return UINT32_MAX;
}

// The plugin's device: nvmtl_vk_init from "static const char *kWant" to vkCreateDevice (dev-only arms left out:
// NVMTL_PSTAT, NVMTL_HAZARD, NVMTL_NO_ROBUST, NVK sparse).
static VkResult create_plugin_device(void) {
	static const char *kWant[NVMTL_VK_WANT_COUNT] = { NVMTL_VK_WANT_EXTS };
	const char *exts[NVMTL_VK_WANT_COUNT]; uint32_t next = 0; int have[NVMTL_VK_WANT_COUNT] = { 0 };
	uint32_t na = 0;
	if (pvkEnumerateDeviceExtensionProperties(pdev, NULL, &na, NULL) == VK_SUCCESS && na) {
		VkExtensionProperties *props = calloc(na, sizeof *props);
		if (props && pvkEnumerateDeviceExtensionProperties(pdev, NULL, &na, props) == VK_SUCCESS)
			for (uint32_t i = 0; i < na; i++)
				for (int w = 0; w < NVMTL_VK_WANT_COUNT; w++)
					if (!have[w] && !strcmp(props[i].extensionName, kWant[w])) { have[w] = 1; exts[next++] = kWant[w]; }
		free(props);
	}
	for (int w = 0; w < NVMTL_VK_WANT_COUNT; w++) printf("     ext %-40s %s\n", kWant[w], have[w] ? "present" : "ABSENT");
	const int dyn3_on = have[0], clip_on = have[0] && have[1], rt = have[3] && have[4] && have[5];
	g_mutable = have[9] && !getenv("NVMTL_NO_TEXBUF");
	const int compute_unchecked = have[16] && !getenv("NVMTL_COMPUTE_ROBUST");
	VkPhysicalDeviceRayQueryFeaturesKHR rqf = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, NULL, VK_TRUE };
	VkPhysicalDeviceAccelerationStructureFeaturesKHR asf = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &rqf,
	                                                        VK_TRUE, VK_FALSE, VK_FALSE, VK_FALSE, VK_TRUE };
	VkPhysicalDeviceVulkan12Features v12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, rt ? (void *)&asf : NULL };
	v12.bufferDeviceAddress = VK_TRUE;
	VkPhysicalDeviceVulkan11Features v11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &v12 };
	static VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mdf;
	if (g_mutable) { mdf = (VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT){ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT, v12.pNext, VK_TRUE }; v12.pNext = &mdf; }
	static VkPhysicalDevicePipelineRobustnessFeaturesEXT prf;
	if (compute_unchecked) { prf = (VkPhysicalDevicePipelineRobustnessFeaturesEXT){ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES_EXT, v12.pNext, VK_TRUE }; v12.pNext = &prf; }
	VkPhysicalDeviceDepthClipEnableFeaturesEXT clipf = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT, NULL, VK_TRUE };
	if (clip_on) clipf.pNext = &v11;
	VkPhysicalDeviceExtendedDynamicState3FeaturesEXT dyn3 = {
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT, clip_on ? (void *)&clipf : (void *)&v11 };
	dyn3.extendedDynamicState3PolygonMode = VK_TRUE;
	static VkPhysicalDeviceProvokingVertexFeaturesEXT pvf;
	if (dyn3_on && have[14]) {
		VkPhysicalDeviceProvokingVertexFeaturesEXT qpv = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT };
		VkPhysicalDeviceExtendedDynamicState3FeaturesEXT q3 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT, &qpv };
		VkPhysicalDeviceFeatures2 qf2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &q3 };
		pvkGetPhysicalDeviceFeatures2(pdev, &qf2);
		if (qpv.provokingVertexLast && q3.extendedDynamicState3ProvokingVertexMode) {
			dyn3.extendedDynamicState3ProvokingVertexMode = VK_TRUE;
			pvf = (VkPhysicalDeviceProvokingVertexFeaturesEXT){ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT, dyn3.pNext, VK_TRUE, VK_FALSE };
			dyn3.pNext = &pvf;
		}
	}
	if (clip_on) dyn3.extendedDynamicState3DepthClipEnable = VK_TRUE;
	static VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR divf;
	if (have[15]) {
		VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR qd = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR };
		VkPhysicalDeviceFeatures2 qf2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &qd };
		VkPhysicalDeviceVertexAttributeDivisorPropertiesKHR dp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_KHR };
		VkPhysicalDeviceProperties2 pp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &dp };
		pvkGetPhysicalDeviceFeatures2(pdev, &qf2); pvkGetPhysicalDeviceProperties2(pdev, &pp);
		if (qd.vertexAttributeInstanceRateDivisor && dp.supportsNonZeroFirstInstance) {
			divf = (VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR){ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR, v12.pNext, VK_TRUE, VK_FALSE };
			v12.pNext = &divf;
		}
	}
	{
		VkPhysicalDeviceVulkan12Features q12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
		VkPhysicalDeviceVulkan13Features q13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
		VkPhysicalDeviceVulkan11Features q11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &q13 };
		q12.pNext = &q11;
		VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &q12 };
		pvkGetPhysicalDeviceFeatures2(pdev, &f2);
		printf("     bufferDeviceAddress %u (the plugin always enables it), shaderFloat16 %u, shaderInt8 %u, "
		       "storage16 %u/%u, memory model %u/%u, int64 atomics %u/%u, descriptorIndexing %u\n",
		       q12.bufferDeviceAddress, q12.shaderFloat16, q12.shaderInt8, q11.storageBuffer16BitAccess,
		       q11.uniformAndStorageBuffer16BitAccess, q12.vulkanMemoryModel, q12.vulkanMemoryModelDeviceScope,
		       q12.shaderBufferInt64Atomics, q12.shaderSharedInt64Atomics, q12.descriptorIndexing);
		v12.shaderFloat16 = q12.shaderFloat16; v12.shaderInt8 = q12.shaderInt8;
		g_float16 = q12.shaderFloat16; g_storage16 = q11.storageBuffer16BitAccess;
		g_uniform_storage16 = q11.uniformAndStorageBuffer16BitAccess; g_vmm = q12.vulkanMemoryModel;
		g_vmm_device = q12.vulkanMemoryModelDeviceScope;
		g_atomic64 = q12.shaderBufferInt64Atomics && q12.shaderSharedInt64Atomics;
		g_coop_matrix = 0; // the plugin also needs VK_KHR_cooperative_matrix with 16x8 f16 tiles; absent on Pascal
		v12.vulkanMemoryModel = q12.vulkanMemoryModel; v12.vulkanMemoryModelDeviceScope = q12.vulkanMemoryModelDeviceScope;
		v11.storageBuffer16BitAccess = q11.storageBuffer16BitAccess;
		v11.uniformAndStorageBuffer16BitAccess = q11.uniformAndStorageBuffer16BitAccess;
		v11.shaderDrawParameters = q11.shaderDrawParameters;
		v12.shaderBufferInt64Atomics = q12.shaderBufferInt64Atomics; v12.shaderSharedInt64Atomics = q12.shaderSharedInt64Atomics;
		v12.hostQueryReset = q12.hostQueryReset; v12.samplerMirrorClampToEdge = q12.samplerMirrorClampToEdge;
		v12.scalarBlockLayout = q12.scalarBlockLayout;
		v12.shaderOutputLayer = q12.shaderOutputLayer; v12.shaderOutputViewportIndex = q12.shaderOutputViewportIndex;
		const int bindless = !getenv("NVMTL_NO_BINDLESS") && q12.descriptorIndexing && q12.runtimeDescriptorArray &&
			q12.shaderSampledImageArrayNonUniformIndexing && q12.descriptorBindingPartiallyBound &&
			q12.descriptorBindingSampledImageUpdateAfterBind && q12.descriptorBindingUpdateUnusedWhilePending &&
			q12.shaderStorageImageArrayNonUniformIndexing && q12.shaderUniformTexelBufferArrayNonUniformIndexing &&
			q12.shaderStorageTexelBufferArrayNonUniformIndexing && q12.descriptorBindingStorageImageUpdateAfterBind &&
			q12.descriptorBindingUniformTexelBufferUpdateAfterBind && q12.descriptorBindingStorageTexelBufferUpdateAfterBind;
		if (bindless) {
			v12.descriptorIndexing = v12.runtimeDescriptorArray = v12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
			v12.descriptorBindingPartiallyBound = v12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
			v12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
			v12.shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
			v12.shaderUniformTexelBufferArrayNonUniformIndexing = VK_TRUE;
			v12.shaderStorageTexelBufferArrayNonUniformIndexing = VK_TRUE;
			v12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
			v12.descriptorBindingUniformTexelBufferUpdateAfterBind = VK_TRUE;
			v12.descriptorBindingStorageTexelBufferUpdateAfterBind = VK_TRUE;
		}
		printf("     bindless %s\n", bindless ? "ON" : "OFF");
		g_bindless = bindless;
	}
	VkPhysicalDeviceFeatures supported; pvkGetPhysicalDeviceFeatures(pdev, &supported);
	if (!supported.shaderInt64) { printf("     shaderInt64 ABSENT: the plugin refuses the device (vk: 64-bit shader addresses unavailable)\n"); return VK_ERROR_FEATURE_NOT_PRESENT; }
	VkPhysicalDeviceFeatures enabled = { 0 }; enabled.shaderInt64 = VK_TRUE;
	enabled.sampleRateShading = supported.sampleRateShading; enabled.occlusionQueryPrecise = supported.occlusionQueryPrecise;
	enabled.samplerAnisotropy = supported.samplerAnisotropy; enabled.imageCubeArray = supported.imageCubeArray;
	enabled.robustBufferAccess = supported.robustBufferAccess; enabled.tessellationShader = supported.tessellationShader;
	enabled.vertexPipelineStoresAndAtomics = supported.vertexPipelineStoresAndAtomics;
	enabled.independentBlend = supported.independentBlend; enabled.alphaToOne = supported.alphaToOne;
	enabled.dualSrcBlend = supported.dualSrcBlend; enabled.wideLines = supported.wideLines;
	const float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio };
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, dyn3_on ? (const void *)&dyn3 : (const void *)&v11, 0,
	                           1, &qci, 0, NULL, next, next ? exts : NULL, &enabled };
	return pvkCreateDevice(pdev, &dci, NULL, &dev);
}

static VkResult create_set_layout(void) {
	static VkDescriptorSetLayoutBinding b[NVMTL_NBIND];
	for (uint32_t i = 0; i < NVMTL_NBIND; i++) {
		VkDescriptorType t = i < NVMTL_NBUF || i == NVMTL_ADDRESS_BINDING ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
		                   : i >= NVMTL_STEX_BASE ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
		                   : i < NVMTL_NBUF + NVMTL_NTEX ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
		b[i] = (VkDescriptorSetLayoutBinding){ NVMTL_BINDING_NUMBER(i), t, 1, VK_SHADER_STAGE_ALL, NULL };
	}
	VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, NVMTL_NBIND, b };
	static const VkDescriptorType texTypes[2] = { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER };
	static const VkDescriptorType stexTypes[2] = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER };
	static VkMutableDescriptorTypeListEXT mlists[NVMTL_NBIND];
	VkMutableDescriptorTypeCreateInfoEXT mci = { VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT, NULL, NVMTL_NBIND, mlists };
	if (g_mutable) {
		for (uint32_t i = 0; i < NVMTL_NBIND; i++) {
			mlists[i] = (VkMutableDescriptorTypeListEXT){ 0, NULL };
			if (b[i].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) { b[i].descriptorType = VK_DESCRIPTOR_TYPE_MUTABLE_EXT; mlists[i] = (VkMutableDescriptorTypeListEXT){ 2, texTypes }; }
			else if (b[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) { b[i].descriptorType = VK_DESCRIPTOR_TYPE_MUTABLE_EXT; mlists[i] = (VkMutableDescriptorTypeListEXT){ 2, stexTypes }; }
		}
		lci.pNext = &mci;
	}
	return pvkCreateDescriptorSetLayout(dev, &lci, NULL, &setLayout);
}

// ensure_descriptors_locked's bindless heap (set 2): five bindings, then two, then one, as the plugin falls back.
static uint32_t create_heap(void) {
	IFN(vkCreateDescriptorPool); IFN(vkAllocateDescriptorSets);
	pvkCreateDescriptorPool = (PFN_vkCreateDescriptorPool)pvkGetDeviceProcAddr(dev, "vkCreateDescriptorPool");
	pvkAllocateDescriptorSets = (PFN_vkAllocateDescriptorSets)pvkGetDeviceProcAddr(dev, "vkAllocateDescriptorSets");
	const VkDescriptorBindingFlags hf = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
	                                    VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
	const VkDescriptorBindingFlags hfs[5] = { hf, hf, hf, hf, hf };
	const VkDescriptorSetLayoutBinding hb[5] = { { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, NVMTL_BINDLESS_SLOTS, VK_SHADER_STAGE_ALL, NULL },
	                                             { 1, VK_DESCRIPTOR_TYPE_SAMPLER, NVMTL_BINDLESS_SAMPLER_SLOTS, VK_SHADER_STAGE_ALL, NULL },
	                                             { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, NVMTL_BINDLESS_SLOTS, VK_SHADER_STAGE_ALL, NULL },
	                                             { 3, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, NVMTL_BINDLESS_SLOTS, VK_SHADER_STAGE_ALL, NULL },
	                                             { 4, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, NVMTL_BINDLESS_SLOTS, VK_SHADER_STAGE_ALL, NULL } };
	const VkDescriptorPoolSize hs[5] = { { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, NVMTL_BINDLESS_SLOTS }, { VK_DESCRIPTOR_TYPE_SAMPLER, NVMTL_BINDLESS_SAMPLER_SLOTS },
	                                     { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, NVMTL_BINDLESS_SLOTS }, { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, NVMTL_BINDLESS_SLOTS },
	                                     { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, NVMTL_BINDLESS_SLOTS } };
	for (uint32_t hnb = 5;; hnb = hnb == 5 ? 2 : 1) {
		VkDescriptorSetLayoutBindingFlagsCreateInfo hfi = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, NULL, hnb, hfs };
		VkDescriptorSetLayoutCreateInfo hli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &hfi,
		                                        VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, hnb, hb };
		VkDescriptorPoolCreateInfo hpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL,
		                                   VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT, 1, hnb, hs };
		VkDescriptorPool hp = VK_NULL_HANDLE; VkDescriptorSet hset = VK_NULL_HANDLE;
		VkResult hr = pvkCreateDescriptorSetLayout(dev, &hli, NULL, &heapLayout);
		if (!hr) hr = pvkCreateDescriptorPool(dev, &hpi, NULL, &hp);
		if (!hr) { VkDescriptorSetAllocateInfo hai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, hp, 1, &heapLayout };
		           hr = pvkAllocateDescriptorSets(dev, &hai, &hset); }
		printf("     bindless heap with %u binding(s) -> %d%s\n", hnb, hr, hr ? " (plugin: 'vk: BINDLESS heap with N bindings refused')" : "");
		if (!hr) return hnb;
		heapLayout = VK_NULL_HANDLE;
		if (hnb == 1) return 0;
	}
}

static uint32_t *read_spv(const char *path, size_t *n) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
	uint32_t *w = len > 20 && len % 4 == 0 ? malloc((size_t)len) : NULL;
	if (w && fread(w, 1, (size_t)len, f) != (size_t)len) { free(w); w = NULL; }
	fclose(f);
	*n = (size_t)len;
	return w && w[0] == 0x07230203u ? w : (free(w), NULL);
}

// (set 0) bindings the module decorates; enough to bind a buffer at each storage-buffer binding below 32.
static uint32_t spv_buffer_bindings(const uint32_t *w, size_t n, uint32_t *out, uint32_t cap) {
	uint32_t got = 0;
	for (size_t i = 5; i < n / 4;) {
		uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
		if (!wc) break;
		if (op == 71 && wc == 4 && w[i + 2] == 33 && w[i + 3] < NVMTL_NBUF && got < cap) out[got++] = w[i + 3]; // Binding
		i += wc;
	}
	return got;
}

// nvmtl_vk.c nvmtl_spirv_ok: a module declaring a capability the device does not have is refused before NVK sees
// it ("vk: compute module requires unavailable enabled capability N"); 0 = refused, with the capability in *cap.
static int plugin_capability_gate(const uint32_t *w, size_t n, uint32_t *cap) {
	for (size_t i = 5; i < n / 4;) {
		uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
		if (!wc || wc > n / 4 - i) return 0;
		if (op == 17 && wc == 2) {
			uint32_t c = w[i + 1];
			int ok = c == 12 ? g_atomic64 : c == 9 ? g_float16 : c == 4433 ? g_storage16 : c == 4434 ? g_uniform_storage16 :
			         c == 5345 ? g_vmm : c == 5346 ? g_vmm_device : c == 6022 ? g_coop_matrix : 1;
			if (!ok) { *cap = c; return 0; }
		}
		i += wc;
	}
	return 1;
}

// nvmtl_spirv_bands_ok: a binding outside every band set 0 builds (192-199 framebuffer fetch, 200-479 imageblock / tile
// memory) is only logged by the plugin, and NVK's descriptor lowering then refuses the pipeline. Returns that binding.
static uint32_t plugin_missing_band(const uint32_t *w, size_t n) {
	for (size_t i = 5; i < n / 4;) {
		uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
		if (!wc) break;
		if (op == 71 && wc == 4 && w[i + 2] == 33) {
			uint32_t b = w[i + 3];
			if (b >= 192 && b < 480) return b;
		}
		i += wc;
	}
	return 0;
}

static VkResult make_buffer(VkDeviceSize size, VkBufferUsageFlags use, VkBuffer *b, VkDeviceMemory *m) {
	VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, use, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
	VkResult r = pvkCreateBuffer(dev, &bci, NULL, b);
	if (r) return r;
	VkMemoryRequirements req; pvkGetBufferMemoryRequirements(dev, *b, &req);
	uint32_t t = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (t == UINT32_MAX) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, t };
	if ((r = pvkAllocateMemory(dev, &mai, NULL, m))) return r;
	return pvkBindBufferMemory(dev, *b, *m, 0);
}

static VkResult submit_and_wait(VkCommandBuffer cb, const char *what) {
	VkFence fence; VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	VkResult r = pvkCreateFence(dev, &fci, NULL, &fence);
	if (r) return r;
	VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL };
	if ((r = pvkQueueSubmit(queue, 1, &si, fence))) return r;
	r = pvkWaitForFences(dev, 1, &fence, VK_TRUE, 5ull * 1000 * 1000 * 1000);
	(void)what;
	return r;
}

int main(int argc, char **argv) {
	const char *lib = NULL, *dispatch = NULL; int blit = 0, no_gate = 0;
	const char *spv[256]; int nspv = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--driver") && i + 1 < argc) lib = argv[++i];
		else if (!strcmp(argv[i], "--spv") && i + 1 < argc && nspv < 256) spv[nspv++] = argv[++i];
		else if (!strcmp(argv[i], "--dispatch") && i + 1 < argc) dispatch = argv[++i];
		else if (!strcmp(argv[i], "--blit")) blit = 1;
		else if (!strcmp(argv[i], "--no-gate")) no_gate = 1; // hand every module to NVK, as if the plugin had no gate
		else { fprintf(stderr, "usage: %s --driver ICD.so [--spv F.spv]... [--dispatch F.spv] [--blit] [--no-gate]\n", argv[0]); return 2; }
	}
	if (!lib) { fprintf(stderr, "--driver is required\n"); return 2; }
	void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
	if (!h) { fprintf(stderr, "dlopen(%s): %s\n", lib, dlerror()); return 3; }
	gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
	if (!gipa) gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
	if (!gipa) { fprintf(stderr, "%s exports no GetInstanceProcAddr\n", lib); return 3; }
#define LI(n) p##n = (PFN_##n)gipa(inst, #n)
	LI(vkCreateInstance);
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "nvmtl_vk_dryrun", 1, "NVMTL", 1, VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL };
	VkResult r = pvkCreateInstance(&ici, NULL, &inst);
	STEP(r == VK_SUCCESS, "vkCreateInstance (Vulkan 1.3) -> %d", r);
	if (r) return 1;
	LI(vkEnumeratePhysicalDevices); LI(vkGetPhysicalDeviceProperties); LI(vkGetPhysicalDeviceProperties2);
	LI(vkGetPhysicalDeviceFeatures); LI(vkGetPhysicalDeviceFeatures2); LI(vkEnumerateDeviceExtensionProperties);
	LI(vkGetPhysicalDeviceQueueFamilyProperties); LI(vkGetPhysicalDeviceMemoryProperties); LI(vkCreateDevice);
	LI(vkGetDeviceProcAddr); LI(vkDestroyInstance);
	uint32_t n = 0; r = pvkEnumeratePhysicalDevices(inst, &n, NULL);
	VkPhysicalDevice pds[8]; if (n > 8) n = 8;
	if (!r && n) r = pvkEnumeratePhysicalDevices(inst, &n, pds);
	for (uint32_t i = 0; !r && i < n; i++) {
		VkPhysicalDeviceProperties pp; pvkGetPhysicalDeviceProperties(pds[i], &pp);
		if (pp.vendorID == 0x10de && !pdev) {
			pdev = pds[i];
			printf("     device %04x:%04x %s, Vulkan %u.%u, maxPushConstantsSize %u, maxComputeSharedMemorySize %u\n",
			       pp.vendorID, pp.deviceID, pp.deviceName, VK_API_VERSION_MAJOR(pp.apiVersion),
			       VK_API_VERSION_MINOR(pp.apiVersion), pp.limits.maxPushConstantsSize, pp.limits.maxComputeSharedMemorySize);
		}
	}
	STEP(pdev != VK_NULL_HANDLE, "vkEnumeratePhysicalDevices: %u device(s), NVIDIA one %s (plugin: 'vk: pdev ...')", n, pdev ? "found" : "MISSING");
	if (!pdev) return 1;
	pvkGetPhysicalDeviceMemoryProperties(pdev, &memp);
	uint32_t qn = 0; pvkGetPhysicalDeviceQueueFamilyProperties(pdev, &qn, NULL);
	VkQueueFamilyProperties qf[16]; if (qn > 16) qn = 16; pvkGetPhysicalDeviceQueueFamilyProperties(pdev, &qn, qf);
	for (uint32_t i = 0; i < qn; i++)
		if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) { qfam = i; break; }
	STEP(qfam != UINT32_MAX, "graphics+compute queue family %d", (int)qfam);
	if (qfam == UINT32_MAX) return 1;
	r = create_plugin_device();
	STEP(r == VK_SUCCESS, "vkCreateDevice with the plugin's extensions and feature chain -> %d (plugin: 'vk: vkCreateDevice' error line)", r);
	if (r) return 1;
#define LD(n) p##n = (PFN_##n)pvkGetDeviceProcAddr(dev, #n)
	LD(vkGetDeviceQueue); LD(vkCreateDescriptorSetLayout); LD(vkCreatePipelineLayout); LD(vkCreateShaderModule);
	LD(vkCreateComputePipelines); LD(vkCreateBuffer); LD(vkGetBufferMemoryRequirements); LD(vkAllocateMemory);
	LD(vkBindBufferMemory); LD(vkCreateDescriptorPool); LD(vkAllocateDescriptorSets); LD(vkUpdateDescriptorSets);
	LD(vkCreateCommandPool); LD(vkAllocateCommandBuffers); LD(vkBeginCommandBuffer); LD(vkEndCommandBuffer);
	LD(vkCmdBindPipeline); LD(vkCmdBindDescriptorSets); LD(vkCmdPushConstants); LD(vkCmdDispatch); LD(vkCmdCopyBuffer);
	LD(vkCreateFence); LD(vkQueueSubmit); LD(vkWaitForFences); LD(vkDeviceWaitIdle); LD(vkDestroyDevice); LD(vkMapMemory);
	LD(vkDestroyPipeline); LD(vkDestroyShaderModule);
	pvkGetDeviceQueue(dev, qfam, 0, &queue);
	r = create_set_layout();
	STEP(r == VK_SUCCESS, "descriptor set layout: %u bindings%s -> %d", NVMTL_NBIND, g_mutable ? ", mutable texture bands" : "", r);
	if (r) return 1;
	uint32_t heap = g_bindless ? create_heap() : 0;
	STEP(!g_bindless || heap == 5, "bindless heap (set 2): %u of 5 bindings (sampled, samplers, storage images, texel buffers)", heap);
	VkDescriptorSetLayout sl[3] = { setLayout, setLayout, heapLayout };
	VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, NVMTL_DISPATCH_PC_BYTES };
	VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, heap ? 3 : 2, sl, 1, &pcr };
	VkPipelineLayout layout;
	r = pvkCreatePipelineLayout(dev, &plci, NULL, &layout);
	STEP(r == VK_SUCCESS, "compute pipeline layout {set, set%s} + %u push bytes -> %d", heap ? ", heap" : "", NVMTL_DISPATCH_PC_BYTES, r);
	if (r) return 1;
	VkPipeline dpipe = VK_NULL_HANDLE; uint32_t dbind[NVMTL_NBUF]; uint32_t ndbind = 0;
	if (dispatch) spv[nspv++] = dispatch;
	for (int i = 0; i < nspv; i++) {
		size_t len = 0; uint32_t *w = read_spv(spv[i], &len);
		if (!w) { STEP(0, "read %s", spv[i]); continue; }
		const char *base = strrchr(spv[i], '/') ? strrchr(spv[i], '/') + 1 : spv[i];
		uint32_t cap = 0;
		if (!no_gate && !plugin_capability_gate(w, len, &cap)) {
			gated++;
			printf("gate %s: the plugin refuses it, capability %u unavailable (%s)\n", base, cap,
			       cap == 9 ? "Float16: shaderFloat16" : cap == 6022 ? "CooperativeMatrixKHR" : cap == 12 ? "Int64Atomics" : "see nvmtl_spirv_ok");
			free(w);
			continue;
		}
		VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, len, w };
		VkShaderModule mod; r = pvkCreateShaderModule(dev, &smi, NULL, &mod);
		VkPipeline pipe = VK_NULL_HANDLE;
		if (!r) {
			VkComputePipelineCreateInfo cpi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
				{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", NULL },
				layout, VK_NULL_HANDLE, -1 };
			r = pvkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe);
			if (nmods < 512) mods[nmods++] = mod;
			if (!r && pipe && nmade < 512) made[nmade++] = pipe;
		}
		uint32_t band = plugin_missing_band(w, len);
		if (r != VK_SUCCESS && band) {
			banded++;
			printf("band %s: binding %u (%s) has no band in the plugin's set layout; NVK refused it (%d), as on the Mac\n",
			       base, band, band < 200 ? "framebuffer fetch" : "imageblock / tile memory", r);
			free(w);
			continue;
		}
		STEP(r == VK_SUCCESS, "compute pipeline %s (%zu B SPIR-V, NAK SM 6.1) -> %d", base, len, r);
		if (!r && dispatch && spv[i] == dispatch) { dpipe = pipe; ndbind = spv_buffer_bindings(w, len, dbind, NVMTL_NBUF); }
		free(w);
	}
	VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qfam };
	VkCommandPool pool; r = pvkCreateCommandPool(dev, &cpci, NULL, &pool);
	if (dpipe) {
		VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NVMTL_NBUF + 1 };
		VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 2, 1, &ps };
		VkDescriptorPool dp; VkDescriptorSet set = VK_NULL_HANDLE;
		VkBuffer buf; VkDeviceMemory mem;
		r = make_buffer(65536, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &buf, &mem);
		if (!r) r = pvkCreateDescriptorPool(dev, &dpci, NULL, &dp);
		if (!r) { VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &setLayout };
		          r = pvkAllocateDescriptorSets(dev, &dai, &set); }
		if (!r) {
			VkDescriptorBufferInfo bi = { buf, 0, VK_WHOLE_SIZE };
			VkWriteDescriptorSet wr[NVMTL_NBUF];
			for (uint32_t k = 0; k < ndbind; k++)
				wr[k] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, dbind[k], 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi, NULL };
			pvkUpdateDescriptorSets(dev, ndbind, wr, 0, NULL);
			VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
			VkCommandBuffer cb; r = pvkAllocateCommandBuffers(dev, &cai, &cb);
			VkCommandBufferBeginInfo bgi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
			if (!r) r = pvkBeginCommandBuffer(cb, &bgi);
			if (!r) {
				uint32_t pc[NVMTL_DISPATCH_PC_BYTES / 4] = { 0 };
				pvkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, dpipe);
				pvkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
				pvkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
				pvkCmdDispatch(cb, 4, 1, 1);
				r = pvkEndCommandBuffer(cb);
			}
			if (!r) r = submit_and_wait(cb, "dispatch");
		}
		STEP(r == VK_SUCCESS, "dispatch 4x1x1 of %s with %u buffer binding(s), fence -> %d", strrchr(dispatch, '/') ? strrchr(dispatch, '/') + 1 : dispatch, ndbind, r);
	}
	if (blit) {
		VkBuffer a, b; VkDeviceMemory ma, mb;
		r = make_buffer(1 << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &a, &ma);
		if (!r) r = make_buffer(1 << 20, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &b, &mb);
		VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
		VkCommandBuffer cb = VK_NULL_HANDLE;
		if (!r) r = pvkAllocateCommandBuffers(dev, &cai, &cb);
		VkCommandBufferBeginInfo bgi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
		if (!r) r = pvkBeginCommandBuffer(cb, &bgi);
		if (!r) { VkBufferCopy c = { 0, 0, 1 << 20 }; pvkCmdCopyBuffer(cb, a, b, 1, &c); r = pvkEndCommandBuffer(cb); }
		if (!r) r = submit_and_wait(cb, "blit");
		STEP(r == VK_SUCCESS, "blit: vkCmdCopyBuffer 1 MiB, fence -> %d", r);
	}
	pvkDeviceWaitIdle(dev);
	for (int i = 0; i < nmade; i++) pvkDestroyPipeline(dev, made[i], NULL);
	for (int i = 0; i < nmods; i++) pvkDestroyShaderModule(dev, mods[i], NULL);
	pvkDestroyDevice(dev, NULL);
	pvkDestroyInstance(inst, NULL);
	printf("nvmtl_vk_dryrun: %d/%d steps passed, %d module(s) refused by the plugin's capability gate, %d needing an "
	       "imageblock / framebuffer-fetch band\n", passed, passed + failed, gated, banded);
	return failed ? 1 : 0;
}
