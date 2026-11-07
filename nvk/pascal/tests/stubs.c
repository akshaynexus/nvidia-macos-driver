/* SPDX-License-Identifier: MIT */
/*
 * Link stubs for the unit test: the NVRM backend (Turing+, needs the
 * open-gpu-kernel-modules headers and macOS) is not built on Linux, and
 * vk_push_print lives in a generated-header TU only NVK_DEBUG_PUSH_DUMP
 * reaches.
 */
#include "nvkmd/nvkmd.h"
#include "nv_push.h"

VkResult nvkmd_nvrm_enum_pdev(struct vk_object_base *log_obj,
                              enum nvk_debug debug_flags,
                              nvkmd_enum_pdev_visitor visitor, void *arg);

VkResult
nvkmd_nvrm_enum_pdev(struct vk_object_base *log_obj,
                     enum nvk_debug debug_flags,
                     nvkmd_enum_pdev_visitor visitor, void *arg)
{
   (void)log_obj; (void)debug_flags; (void)visitor; (void)arg;
   return VK_SUCCESS; /* no NVRM device on this machine */
}

void
vk_push_print(FILE *fp, const struct nv_push *push,
              const struct nv_device_info *devinfo)
{
   (void)push; (void)devinfo;
   fprintf(fp, "<push dump not built into the unit test>\n");
}
