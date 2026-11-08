// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/src/vram.c — range allocator, VRAM through PRAMIN, FB init.
// See vram.h for the nouveau sources.

#include <pascal/devboot.h>
#include <pascal/vram.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

// ---- range allocator ----------------------------------------------------------

static bool
pow2(uint64_t v)
{
	return v && !(v & (v - 1));
}

static uint64_t
round_up(uint64_t v, uint64_t a)
{
	return (v + a - 1) & ~(a - 1);
}

static int
ext_insert(struct pg106_range *r, size_t at, uint64_t start, uint64_t size)
{
	if (r->nfree_ == r->cap_) {
		size_t ncap = r->cap_ ? r->cap_ * 2 : 16;
		struct pg106_range_ext *n =
			realloc(r->free_, ncap * sizeof(*n));

		if (!n)
			return -ENOMEM;
		r->free_ = n;
		r->cap_ = ncap;
	}
	memmove(&r->free_[at + 1], &r->free_[at],
		(r->nfree_ - at) * sizeof(*r->free_));
	r->free_[at].start = start;
	r->free_[at].size = size;
	r->nfree_++;
	return 0;
}

static void
ext_remove(struct pg106_range *r, size_t at)
{
	memmove(&r->free_[at], &r->free_[at + 1],
		(r->nfree_ - at - 1) * sizeof(*r->free_));
	r->nfree_--;
}

int
pg106_range_init(struct pg106_range *r, uint64_t base, uint64_t size,
		 uint64_t gran)
{
	if (!r || !pow2(gran) || !size || (base | size) & (gran - 1) ||
	    base + size < base)
		return -EINVAL;
	memset(r, 0, sizeof(*r));
	r->base = base;
	r->size = size;
	r->gran = gran;
	r->free_bytes = size;
	return ext_insert(r, 0, base, size);
}

void
pg106_range_fini(struct pg106_range *r)
{
	if (!r)
		return;
	free(r->free_);
	memset(r, 0, sizeof(*r));
}

// Carve [a, a+size) out of extent i (which contains it).
static int
carve(struct pg106_range *r, size_t i, uint64_t a, uint64_t size)
{
	struct pg106_range_ext *e = &r->free_[i];
	const uint64_t es = e->start, ee = e->start + e->size;
	int ret = 0;

	if (a == es && a + size == ee) {
		ext_remove(r, i);
	} else if (a == es) {
		e->start += size;
		e->size -= size;
	} else if (a + size == ee) {
		e->size -= size;
	} else {
		e->size = a - es;
		ret = ext_insert(r, i + 1, a + size, ee - (a + size));
		if (ret) {
			e->size = ee - es; // undo
			return ret;
		}
	}
	r->free_bytes -= size;
	return 0;
}

int
pg106_range_alloc(struct pg106_range *r, uint64_t size, uint64_t align,
		  bool tail, uint64_t *addr)
{
	size_t k;

	if (!r || !addr || !size)
		return -EINVAL;
	if (!align)
		align = r->gran;
	if (!pow2(align) || align < r->gran)
		return -EINVAL;
	size = round_up(size, r->gran);
	for (k = 0; k < r->nfree_; k++) {
		// nvkm_mm_head walks low to high, nvkm_mm_tail high to low.
		size_t i = tail ? r->nfree_ - 1 - k : k;
		const struct pg106_range_ext *e = &r->free_[i];
		uint64_t a;

		if (e->size < size)
			continue;
		if (tail) {
			a = (e->start + e->size - size) & ~(align - 1);
			if (a < e->start)
				continue;
		} else {
			a = round_up(e->start, align);
			if (a < e->start || a + size > e->start + e->size)
				continue;
		}
		if (carve(r, i, a, size))
			return -ENOMEM;
		*addr = a;
		return 0;
	}
	return -ENOSPC;
}

int
pg106_range_reserve(struct pg106_range *r, uint64_t addr, uint64_t size)
{
	size_t i;

	if (!r || !size || addr < r->base || addr + size > r->base + r->size ||
	    addr + size < addr)
		return -EINVAL;
	size = round_up(size + (addr & (r->gran - 1)), r->gran);
	addr &= ~(r->gran - 1);
	for (i = 0; i < r->nfree_; i++) {
		const struct pg106_range_ext *e = &r->free_[i];

		if (addr >= e->start && addr + size <= e->start + e->size)
			return carve(r, i, addr, size) ? -ENOMEM : 0;
	}
	return -EBUSY;
}

int
pg106_range_free(struct pg106_range *r, uint64_t addr, uint64_t size)
{
	size_t i;
	uint64_t end;

	if (!r || !size || addr & (r->gran - 1))
		return -EINVAL;
	size = round_up(size, r->gran);
	end = addr + size;
	if (addr < r->base || end > r->base + r->size || end < addr)
		return -EINVAL;
	// First extent starting after addr.
	for (i = 0; i < r->nfree_ && r->free_[i].start < addr; i++)
		;
	if (i > 0 && r->free_[i - 1].start + r->free_[i - 1].size > addr)
		return -EINVAL; // overlaps the previous free extent
	if (i < r->nfree_ && r->free_[i].start < end)
		return -EINVAL; // overlaps the next one
	r->free_bytes += size;
	if (i > 0 && r->free_[i - 1].start + r->free_[i - 1].size == addr) {
		r->free_[i - 1].size += size;
		if (i < r->nfree_ && r->free_[i].start == end) {
			r->free_[i - 1].size += r->free_[i].size;
			ext_remove(r, i);
		}
		return 0;
	}
	if (i < r->nfree_ && r->free_[i].start == end) {
		r->free_[i].start = addr;
		r->free_[i].size += size;
		return 0;
	}
	if (ext_insert(r, i, addr, size)) {
		r->free_bytes -= size;
		return -ENOMEM;
	}
	return 0;
}

bool
pg106_range_is_allocated(const struct pg106_range *r, uint64_t addr,
			 uint64_t size)
{
	size_t i;

	if (!r || !size || addr < r->base || addr + size > r->base + r->size)
		return false;
	for (i = 0; i < r->nfree_; i++) {
		const struct pg106_range_ext *e = &r->free_[i];

		if (e->start < addr + size && addr < e->start + e->size)
			return false;
	}
	return true;
}

// ---- VRAM -----------------------------------------------------------------------

int
pg106_vram_init(struct pg106_vram *v, const struct pg106_hw_bus *bus,
		uint64_t size)
{
	if (!v || !bus || !bus->rd32 || !bus->wr32 ||
	    size <= PG106_FB_RSVD_TAIL || size & (PG106_VRAM_GRAN - 1))
		return -EINVAL;
	memset(v, 0, sizeof(*v));
	v->bus = bus;
	v->size = size;
	v->window = UINT64_MAX;
	return pg106_range_init(&v->mm, 0, size - PG106_FB_RSVD_TAIL,
				PG106_VRAM_GRAN);
}

void
pg106_vram_fini(struct pg106_vram *v)
{
	if (!v)
		return;
	pg106_range_fini(&v->mm);
}

void
pg106_vram_forget_window(struct pg106_vram *v)
{
	if (v)
		v->window = UINT64_MAX;
}

int
pg106_vram_alloc(struct pg106_vram *v, uint64_t size, uint64_t align,
		 uint32_t flags, uint64_t *pa)
{
	int ret;

	if (!v || !pa)
		return -EINVAL;
	ret = pg106_range_alloc(&v->mm, size, align < PG106_VRAM_GRAN ?
					PG106_VRAM_GRAN : align,
				(flags & PG106_VRAM_TAIL) != 0, pa);
	if (ret)
		return ret;
	if (flags & PG106_VRAM_ZERO) {
		uint64_t n = (size + PG106_VRAM_GRAN - 1) & ~(PG106_VRAM_GRAN - 1);
		uint64_t off;

		for (off = 0; off < n; off += 0x10000) {
			uint64_t chunk = n - off < 0x10000 ? n - off : 0x10000;

			ret = pg106_vram_write(v, *pa + off, NULL, (uint32_t)chunk);
			if (ret) {
				pg106_range_free(&v->mm, *pa, size);
				return ret;
			}
		}
	}
	return 0;
}

void
pg106_vram_free(struct pg106_vram *v, uint64_t pa, uint64_t size)
{
	if (v && size)
		pg106_range_free(&v->mm, pa, size);
}

int
pg106_vram_write(struct pg106_vram *v, uint64_t pa, const void *src,
		 uint32_t len)
{
	if (!v || pa + len > v->size || pa + len < pa)
		return -EINVAL;
	v->pramin_writes += (len + 3) / 4;
	return pg106_hw_pramin_write(v->bus, pa, src, len, &v->window);
}

int
pg106_vram_read(struct pg106_vram *v, uint64_t pa, void *dst, uint32_t len)
{
	uint8_t *d = dst;
	uint32_t done = 0;

	if (!v || !dst || pa + len > v->size || pa + len < pa)
		return -EINVAL;
	while (done < len) {
		const uint64_t at = pa + done;
		const uint32_t lane = (uint32_t)(at & 3u);
		uint32_t w = pg106_hw_pramin_rd32(v->bus, at, &v->window), k;
		uint32_t n = 4u - lane;

		v->pramin_reads++;
		if (n > len - done)
			n = len - done;
		for (k = 0; k < n; k++)
			d[done + k] = (uint8_t)(w >> (8u * (lane + k)));
		done += n;
	}
	return 0;
}

int
pg106_vram_wr32(struct pg106_vram *v, uint64_t pa, uint32_t val)
{
	uint8_t b[4] = { (uint8_t)val, (uint8_t)(val >> 8),
			 (uint8_t)(val >> 16), (uint8_t)(val >> 24) };

	return pg106_vram_write(v, pa, b, 4);
}

int
pg106_vram_wr64(struct pg106_vram *v, uint64_t pa, uint64_t val)
{
	int ret = pg106_vram_wr32(v, pa, (uint32_t)val);

	return ret ? ret : pg106_vram_wr32(v, pa + 4, (uint32_t)(val >> 32));
}

uint32_t
pg106_vram_rd32(struct pg106_vram *v, uint64_t pa)
{
	if (!v || pa + 4 > v->size)
		return 0xffffffffu;
	v->pramin_reads++;
	return pg106_hw_pramin_rd32(v->bus, pa, &v->window);
}

// ---- FB init ----------------------------------------------------------------------

static void
mask32(const struct pg106_hw_bus *b, uint32_t a, uint32_t m, uint32_t val)
{
	uint32_t old = b->rd32(b->user, a);

	b->wr32(b->user, a, (old & ~m) | val);
}

int
pg106_fb_init(struct pg106_fb *fb, struct pg106_vram *v,
	      uint64_t sysmem_flush_bus)
{
	const struct pg106_hw_bus *b;
	int ret;

	if (!fb || !v)
		return -EINVAL;
	b = v->bus;
	if (!fb->mmu_rd) {
		// gf100_fb_oneinit (fb/gf100.c:54-62): INST, align 0x1000,
		// zero = true; rd first, then wr.
		ret = pg106_vram_alloc(v, PG106_FB_MMU_DEBUG_SIZE, 0x1000,
				       PG106_VRAM_TAIL | PG106_VRAM_ZERO,
				       &fb->mmu_rd);
		if (ret)
			return ret;
		ret = pg106_vram_alloc(v, PG106_FB_MMU_DEBUG_SIZE, 0x1000,
				       PG106_VRAM_TAIL | PG106_VRAM_ZERO,
				       &fb->mmu_wr);
		if (ret) {
			pg106_vram_free(v, fb->mmu_rd, PG106_FB_MMU_DEBUG_SIZE);
			fb->mmu_rd = 0;
			return ret;
		}
	}
	// nvkm_fb_init (fb/base.c:204): sysmem flush page first.
	if (sysmem_flush_bus)
		b->wr32(b->user, 0x100c10, (uint32_t)(sysmem_flush_bus >> 8));
	// gm200_fb_init (fb/gm200.c:49-52).
	b->wr32(b->user, 0x100cc8, (uint32_t)(fb->mmu_wr >> 8));
	b->wr32(b->user, 0x100ccc, (uint32_t)(fb->mmu_rd >> 8));
	mask32(b, 0x100cc4, 0x00060000,
	       (uint32_t)((PG106_FB_MMU_DEBUG_SIZE >> 16) < 2
				  ? (PG106_FB_MMU_DEBUG_SIZE >> 16) : 2) << 17);
	// gp100_fb_init_remapper (fb/gp100.c:44).
	mask32(b, 0x100c14, 0x00040000, 0x00000000);
	// gm200_fb_init_page, page 0 (fb/gm200.c:36).
	mask32(b, 0x100c80, 0x00001800, 0x00001800);
	// gp102_fb_vpr_scrub_required (fb/gp102.c:39-40).
	b->wr32(b->user, 0x100cd0, 0x2);
	fb->vpr_locked = (b->rd32(b->user, 0x100cd0) & 0x10) != 0;
	return 0;
}

uint64_t
pg106_fb_vidmem_size(const struct pg106_hw_bus *bus)
{
	const uint32_t data = bus->rd32(bus->user, 0x100ce0);
	const uint32_t lmag = (data & 0x000003f0) >> 4;
	const uint32_t lsca = data & 0x0000000f;
	const uint64_t size = (uint64_t)lmag << (lsca + 20);

	return (data & 0x40000000) ? size / 16 * 15 : size;
}
