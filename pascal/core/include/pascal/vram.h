// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/vram.h — VRAM allocation and CPU access for
// the GP106, plus the generic range allocator the BAR VMMs reuse for their
// address space.
//
// Allocator: nvkm_mm semantics as nouveau uses them for VRAM
// (nvkm/core/mm.c nvkm_mm_head / nvkm_mm_tail; nvkm_ram_get,
// subdev/fb/ram.c, allocates kernel objects with back = true, i.e. from
// the top). Free space is a sorted list of extents; head allocations take
// the lowest fitting address, tail allocations the highest, both aligned.
// The last PG106_FB_RSVD_TAIL bytes of VRAM are kept out (gf100_ram_ctor
// "vbios etc", ramgf100.c:498: rsvd_tail).
//
// CPU access: the PRAMIN window (BAR0 0x700000, 1 MiB, base in 0x1700 >>
// 16; nv50 instmem slow path, subdev/instmem/nv50.c:61-77 wr32_slow,
// :420 set_bar0_window_addr), through pascal/hwexec.h. It works before
// any VMM exists, which is what the ACR, BAR and FIFO objects need.
//
// FB setup the VMMs depend on, in nvkm_fb_init order (fb/base.c:190-222)
// with GP106's hooks (gp102_fb, fb/gp102.c:69-80):
//   gm200_fb_init       0x100cc8 = mmu_wr >> 8, 0x100ccc = mmu_rd >> 8,
//                       0x100cc4[18:17] = min(size >> 16, 2)
//                       (fb/gm200.c:43-53; buffers 128 KiB each in VRAM,
//                       gf100_fb_oneinit fb/gf100.c:45-65)
//   gp100_fb_init_remapper  0x100c14 bit 18 cleared (fb/gp100.c:40-45)
//   gm200_fb_init_page  page 0 ("per instance": GP10x default_bigpage is
//                       0): 0x100c80 |= 0x1800 (fb/gm200.c:29-41)
// The sysmem flush page (0x100c10, gf100_fb_sysmem_flush_page_init,
// fb/gf100.c:81-87) is written only when the caller has a DMA page for
// it (sysmem_flush_bus != 0). VPR: gp102_fb_vpr_scrub_required
// (fb/gp102.c:36-41) is checked and reported; nouveau runs the NVDEC
// scrubber when it is locked and only warns without the binary
// (fb/base.c:150-160) — so does this, without running anything.
//
// C11, libc only. Errors are negative errno.

#ifndef PASCAL_VRAM_H
#define PASCAL_VRAM_H

#include <pascal/hwexec.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- generic range allocator ------------------------------------------------

struct pg106_range_ext {
	uint64_t start, size;
};

struct pg106_range {
	uint64_t base, size; // managed span
	uint64_t gran;       // every size/address is a multiple of this
	struct pg106_range_ext *free_;
	size_t nfree_, cap_;
	uint64_t free_bytes;
};

int pg106_range_init(struct pg106_range *r, uint64_t base, uint64_t size,
		     uint64_t gran);
void pg106_range_fini(struct pg106_range *r);
// Allocate size bytes (rounded up to gran) aligned to align (power of two,
// at least gran; 0 = gran). tail = from the top (nvkm_mm_tail). -ENOSPC.
int pg106_range_alloc(struct pg106_range *r, uint64_t size, uint64_t align,
		      bool tail, uint64_t *addr);
// Take [addr, addr+size) out of the free space; -EBUSY if any part of it
// is not free, -EINVAL outside the span.
int pg106_range_reserve(struct pg106_range *r, uint64_t addr, uint64_t size);
// Return [addr, addr+size) (as allocated). -EINVAL for a range that is
// outside the span or overlaps free space (double free).
int pg106_range_free(struct pg106_range *r, uint64_t addr, uint64_t size);
// True if [addr, addr+size) is entirely allocated (not free).
bool pg106_range_is_allocated(const struct pg106_range *r, uint64_t addr,
			      uint64_t size);

// ---- VRAM ---------------------------------------------------------------------

#define PG106_VRAM_GRAN 0x1000ull

enum {
	PG106_VRAM_TAIL = 1u << 0, // allocate from the top (kernel objects)
	PG106_VRAM_ZERO = 1u << 1, // zero the allocation through PRAMIN
};

struct pg106_vram {
	const struct pg106_hw_bus *bus;
	uint64_t size;  // VRAM bytes (pg106_fb_vram_size)
	struct pg106_range mm;
	uint64_t window; // PRAMIN base cache (UINT64_MAX = unknown)
	uint64_t pramin_writes, pramin_reads; // MMIO accesses through PRAMIN
};

// Manage [0, size - PG106_FB_RSVD_TAIL). Returns -EINVAL for size 0 or
// smaller than the tail.
int pg106_vram_init(struct pg106_vram *v, const struct pg106_hw_bus *bus,
		    uint64_t size);
void pg106_vram_fini(struct pg106_vram *v);
// After a power cycle the PRAMIN base register is unknown again.
void pg106_vram_forget_window(struct pg106_vram *v);

int pg106_vram_alloc(struct pg106_vram *v, uint64_t size, uint64_t align,
		     uint32_t flags, uint64_t *pa);
void pg106_vram_free(struct pg106_vram *v, uint64_t pa, uint64_t size);

// CPU access through PRAMIN. src NULL = zero fill.
int pg106_vram_write(struct pg106_vram *v, uint64_t pa, const void *src,
		     uint32_t len);
int pg106_vram_read(struct pg106_vram *v, uint64_t pa, void *dst,
		    uint32_t len);
int pg106_vram_wr32(struct pg106_vram *v, uint64_t pa, uint32_t val);
uint32_t pg106_vram_rd32(struct pg106_vram *v, uint64_t pa);
int pg106_vram_wr64(struct pg106_vram *v, uint64_t pa, uint64_t val);

// ---- FB init ------------------------------------------------------------------

#define PG106_FB_MMU_DEBUG_SIZE 0x20000ull // 1 << 17 (gf100_fb_oneinit)

struct pg106_fb {
	uint64_t mmu_rd, mmu_wr; // VRAM debug buffers
	bool vpr_locked;         // gp102_fb_vpr_scrub_required answered true
};

// Allocate the MMU debug buffers (tail, zeroed) and program the FB the way
// nvkm_fb_init does for gp102_fb (see the file comment). sysmem_flush_bus
// is the DMA address of a host page for 0x100c10, 0 = leave it alone.
int pg106_fb_init(struct pg106_fb *fb, struct pg106_vram *v,
		  uint64_t sysmem_flush_bus);

// gp102_fb_vidmem_size (fb/gp102.c:43-55): 0x100ce0 lmag << (lsca + 20),
// 15/16 of it when bit 30 is set. A cross-check for pg106_fb_vram_size.
uint64_t pg106_fb_vidmem_size(const struct pg106_hw_bus *bus);

#ifdef __cplusplus
}
#endif

#endif // PASCAL_VRAM_H
