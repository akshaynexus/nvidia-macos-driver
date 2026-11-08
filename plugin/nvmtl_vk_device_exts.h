/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
// The device extensions nvmtl_vk_init asks the GPU's Vulkan driver for, each enabled only when the driver lists it.
// The positions are fixed: nvmtl_vk.c reads have[k] by index (have[0] VK_EXT_extended_dynamic_state3 ... have[16]
// VK_EXT_pipeline_robustness). Shared with plugin/tests/nvmtl_vk_dryrun.c, which replays the plugin's device creation
// on Linux against NVK's Pascal backend, so the two cannot drift apart.
#ifndef NVMTL_VK_DEVICE_EXTS_H
#define NVMTL_VK_DEVICE_EXTS_H
#define NVMTL_VK_WANT_COUNT 17
#define NVMTL_VK_WANT_EXTS                                                                                          \
    "VK_EXT_extended_dynamic_state3", "VK_EXT_depth_clip_enable", "VK_EXT_memory_budget",                          \
    "VK_KHR_acceleration_structure", "VK_KHR_ray_query", "VK_KHR_deferred_host_operations",                        \
    "VK_EXT_external_memory_host",                                                                                  \
    "VK_EXT_image_drm_format_modifier",                                                                             \
    "VK_KHR_external_memory_fd",                                                                                    \
    "VK_EXT_mutable_descriptor_type", "VK_KHR_cooperative_matrix", "VK_EXT_calibrated_timestamps",                 \
    "VK_EXT_sample_locations", "VK_KHR_pipeline_executable_properties",                                             \
    "VK_EXT_provoking_vertex", "VK_KHR_vertex_attribute_divisor", "VK_EXT_pipeline_robustness"
#endif
