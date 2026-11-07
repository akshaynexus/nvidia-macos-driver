/* SPDX-License-Identifier: MIT */
/*
 * offload_vk - the Vulkan half of pg106_offload: render frames with the
 * Pascal NVK into memory the caller owns (an IOSurface's base address on
 * macOS, a page-aligned malloc in the Linux self-test).
 *
 * Two modes, picked at attach time:
 *   zero-copy  the target pages are imported as Vulkan memory
 *              (VK_EXT_external_memory_host -> nvkmd import_host_mem ->
 *              pg106_br_bo_import_host) and the compute shader writes the
 *              pixels into them directly (OFFLOAD-DESIGN.md section 2.1);
 *   copy       the shader writes a host-visible Vulkan buffer and the CPU
 *              copies it row by row into the target after the fence (the
 *              fallback of section 2.1, done on the CPU here).
 */
#ifndef OFFLOAD_VK_H
#define OFFLOAD_VK_H

#include <stddef.h>
#include <stdint.h>

struct ovk;

#define OVK_MAX_TARGETS 4

/* lib: the driver to dlopen; is_icd: call vk_icdGetInstanceProcAddr (no
 * loader); any: accept any device, not only a GP106. Prints the devices.
 * Returns 0 or a negative code with a message on stderr. */
int ovk_open(struct ovk **out, const char *lib, int is_icd, int any);

/* Add a target (up to OVK_MAX_TARGETS): w x h BGRA8 pixels, pitch_B bytes
 * per row, alloc_B bytes at base. force_copy skips the import attempt.
 * *index_out names it in ovk_frame/ovk_mode. */
int ovk_attach(struct ovk *o, void *base, size_t alloc_B, uint32_t w,
               uint32_t h, uint32_t pitch_B, int force_copy, uint32_t *index_out);

/* "zero-copy (host import)" or "copy (staging buffer + memcpy)". */
const char *ovk_mode(const struct ovk *o, uint32_t index);
const char *ovk_device_name(const struct ovk *o);

/* Render frame `frame` into target `index` and wait for it (10 s bound).
 * *gpu_ns: submit -> fence; *copy_ns: CPU copy time (0 for zero-copy). */
int ovk_frame(struct ovk *o, uint32_t index, uint32_t frame, uint64_t *gpu_ns,
              uint64_t *copy_ns);

void ovk_close(struct ovk *o);

/* The pixel offload.comp writes at (x, y) for `frame`. */
static inline uint32_t
ovk_expected(uint32_t x, uint32_t y, uint32_t frame, uint32_t w)
{
   const uint32_t bar = (frame * 8u) % w;
   if (x >= bar && x < bar + 16u)
      return 0xffffffffu;
   return (0xffu << 24) | (((frame * 4u) & 0xffu) << 16) | ((y & 0xffu) << 8) | (x & 0xffu);
}

/* Compare a sparse grid of pixels; returns the number that differ. */
uint32_t ovk_verify(const void *base, uint32_t w, uint32_t h, uint32_t pitch_B,
                    uint32_t frame);

#endif
