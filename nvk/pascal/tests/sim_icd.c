/* SPDX-License-Identifier: MIT */
/*
 * sim_icd.c - turns a Linux build of NVK + the pg106 backend into a Vulkan
 * ICD that finds a simulated GP106: the bridge's recording mock answers the
 * kext protocol and loopback_gpu.c plays GPU for host semaphore/WFI/MEMBAR
 * methods only (nvk/pascal/scripts/linux-sim-icd.sh links it).
 *
 * What this exercises for real: every Vulkan entry point the step 3-4
 * programs call, NVK's pipeline/shader path, and NAK compiling their
 * SPIR-V for SM 6.1 (the code that the macOS dylib carries), plus the
 * pg106 backend's BO/VA/EXEC/sync traffic. What it does not: no 3D/compute/
 * copy method is executed, so data checks are expected to fail.
 *
 * SIMULATION ONLY. Nothing here talks to a GPU or a kext.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "loopback_gpu.h"
#include "nvkmd_pg106.h"

/* nvkmd_nvrm_ctx.c calls this macOS-only helper (nvrm_xnu.c is built only
 * for darwin); the NVRM backend is never used here (NVK_KMD=pg106). */
int64_t nvrm_xnu_wait_event(int fd, uint32_t seen, uint32_t timeout_us);
int64_t
nvrm_xnu_wait_event(int fd, uint32_t seen, uint32_t timeout_us)
{
   (void)fd; (void)seen; (void)timeout_us;
   return -1;
}

static void __attribute__((constructor))
pg106_sim_icd_init(void)
{
   /* Keep NVK off the NVRM backend: on a Linux box with NVIDIA's
    * proprietary driver it would try the real RM. */
   setenv("NVK_KMD", "pg106", 0);
   const char *dev = getenv("PG106_SIM_PCI_DEVICE");
   if (dev != NULL)
      loopback_config.pci_device = (uint16_t)strtoul(dev, NULL, 0);
   nvkmd_pg106_set_transport_hook(loopback_transport_open,
                                  loopback_transport_close);
   fprintf(stderr, "pg106-sim: NVK with the pg106 backend over the bridge mock "
                   "(SIMULATION: no kext, no GPU; only host methods are executed)\n");
}
