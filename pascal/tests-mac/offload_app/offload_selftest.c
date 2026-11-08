/* SPDX-License-Identifier: MIT */
/*
 * Linux self-test of offload_vk.c: a page-aligned malloc stands in for the
 * IOSurface (same pitch rule, 64-byte-padded rows). Renders 60 frames in
 * zero-copy mode (if the driver has VK_EXT_external_memory_host) and 60 in
 * copy mode, verifying a pixel grid after every frame. This tests the test
 * app's Vulkan path on whatever GPU the box has; it is not Pascal or NVK.
 *
 * Usage: offload_selftest [--driver PATH | --loader PATH] [--any]
 */
#include "offload_vk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __APPLE__
#define DEFAULT_LIB "/Library/GPUBundles/nvmtl/libvulkan_nouveau.dylib"
#define DEFAULT_IS_ICD 1
#else
#define DEFAULT_LIB "libvulkan.so.1"
#define DEFAULT_IS_ICD 0
#endif

/* Two targets on one device, frames alternating between them like the
 * app's double-buffered surfaces. */
static int
run(struct ovk *o, int force_copy, void *base[2], size_t alloc, uint32_t w, uint32_t h,
    uint32_t pitch)
{
   uint32_t idx[2];
   for (int i = 0; i < 2; i++)
      if (ovk_attach(o, base[i], alloc, w, h, pitch, force_copy, &idx[i]) != 0)
         return -1;
   uint64_t gsum = 0, csum = 0;
   uint32_t bad_frames = 0;
   for (uint32_t f = 0; f < 60; f++) {
      uint64_t g, c;
      void *b = base[f % 2];
      memset(b, 0, alloc);
      if (ovk_frame(o, idx[f % 2], f, &g, &c) != 0)
         return -1;
      gsum += g; csum += c;
      bad_frames += ovk_verify(b, w, h, pitch, f) != 0;
   }
   printf("%s %s: 60 frames over 2 targets %ux%u pitch %u, %u bad, gpu %.3f ms/frame, copy %.3f ms/frame\n",
          bad_frames ? "FAIL" : "PASS", ovk_mode(o, idx[0]), w, h, pitch, bad_frames,
          gsum / 60.0 / 1e6, csum / 60.0 / 1e6);
   return bad_frames ? 1 : 0;
}

int
main(int argc, char **argv)
{
   const char *lib = DEFAULT_LIB;
   int is_icd = DEFAULT_IS_ICD, any = 0;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--driver") && i + 1 < argc) { lib = argv[++i]; is_icd = 1; }
      else if (!strcmp(argv[i], "--loader") && i + 1 < argc) { lib = argv[++i]; is_icd = 0; }
      else if (!strcmp(argv[i], "--any")) any = 1;
   }
   const uint32_t w = 512, h = 384, pitch = 512 * 4 + 64;
   const size_t page = (size_t)sysconf(_SC_PAGESIZE);
   const size_t alloc = ((size_t)pitch * h + page - 1) / page * page;
   int rc = 0;
   for (int mode = 0; mode < 2; mode++) {
      void *base[2] = { NULL, NULL };
      if (posix_memalign(&base[0], page, alloc) || posix_memalign(&base[1], page, alloc))
         return 3;
      struct ovk *o = NULL;
      if (ovk_open(&o, lib, is_icd, any) != 0) { ovk_close(o); return 3; }
      int r = run(o, mode, base, alloc, w, h, pitch);
      if (r < 0) { printf("FAIL %s: setup failed\n", mode ? "copy" : "zero-copy"); rc = 1; }
      else rc |= r;
      ovk_close(o);
      free(base[0]);
      free(base[1]);
   }
   printf("offload_selftest: %s\n", rc ? "FAILED" : "all passed");
   return rc;
}
