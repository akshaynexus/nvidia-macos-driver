// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/src/PG106_mem.cpp — DMA-mapped host memory (pg106_dma.h).

#include "pg106_dma.h"

#include <IOKit/IOLib.h>
#include <sys/errno.h>

#include "pg106_klibc.h"

// The GPU's system-memory PTE/ctxdma address fields carry 40 bits on this
// path (PG106_SUB_PUSH_ADDR_LIMIT, pascal/dispatch.h).
#define PG106_DMA_BITS 40
#define PG106_DMA_MASK 0x000000fffffff000ull

static int
collect(struct pg106_dma *d)
{
	UInt64 off = 0;
	uint32_t cap = 0;

	while (off < d->size) {
		IODMACommand::Segment64 segs[16];
		UInt32 n = 16, i;

		if (d->cmd->gen64IOVMSegments(&off, segs, &n) != kIOReturnSuccess || !n)
			return -EFAULT;
		if (d->nseg + n > cap) {
			uint32_t ncap = cap ? cap * 2 : 32;
			void *a, *l;

			while (ncap < d->nseg + n)
				ncap *= 2;
			a = pg106_krealloc(d->seg_addr, ncap * sizeof(uint64_t));
			if (!a)
				return -ENOMEM;
			d->seg_addr = static_cast<uint64_t *>(a);
			l = pg106_krealloc(d->seg_len, ncap * sizeof(uint64_t));
			if (!l)
				return -ENOMEM;
			d->seg_len = static_cast<uint64_t *>(l);
			cap = ncap;
		}
		for (i = 0; i < n; i++) {
			d->seg_addr[d->nseg] = segs[i].fIOVMAddr;
			d->seg_len[d->nseg] = segs[i].fLength;
			d->nseg++;
		}
	}
	return 0;
}

static int
map(struct pg106_dma *d)
{
	d->cmd = IODMACommand::withSpecification(kIODMACommandOutputHost64,
						 PG106_DMA_BITS, 0,
						 IODMACommand::kMapped, 0, 1);
	if (!d->cmd)
		return -ENOMEM;
	if (d->cmd->setMemoryDescriptor(d->md, true) != kIOReturnSuccess)
		return -EFAULT;
	d->prepared_by_us = true;
	return collect(d);
}

void
pg106_dma_free(struct pg106_dma *d)
{
	if (!d)
		return;
	if (d->cmd) {
		if (d->prepared_by_us)
			d->cmd->clearMemoryDescriptor(true);
		d->cmd->release();
	}
	if (d->md)
		d->md->release();
	if (d->seg_addr)
		pg106_kfree(d->seg_addr);
	if (d->seg_len)
		pg106_kfree(d->seg_len);
	pg106_kfree(d);
}

int
pg106_dma_alloc(uint64_t size, struct pg106_dma **out)
{
	struct pg106_dma *d;
	int ret;

	if (!size || !out)
		return -EINVAL;
	d = static_cast<struct pg106_dma *>(pg106_kcalloc(1, sizeof(*d)));
	if (!d)
		return -ENOMEM;
	size = (size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
	d->bmd = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
		kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared, size,
		PG106_DMA_MASK);
	if (!d->bmd) {
		pg106_kfree(d);
		return -ENOMEM;
	}
	memset(d->bmd->getBytesNoCopy(), 0, size);
	d->md = d->bmd; // the reference inTaskWithPhysicalMask returned
	d->size = size;
	ret = map(d);
	if (ret) {
		pg106_dma_free(d);
		return ret;
	}
	*out = d;
	return 0;
}

int
pg106_dma_wrap(IOMemoryDescriptor *md, struct pg106_dma **out)
{
	struct pg106_dma *d;
	int ret;

	if (!md || !out)
		return -EINVAL;
	d = static_cast<struct pg106_dma *>(pg106_kcalloc(1, sizeof(*d)));
	if (!d)
		return -ENOMEM;
	md->retain();
	d->md = md;
	d->size = md->getLength();
	ret = map(d);
	if (ret) {
		pg106_dma_free(d);
		return ret;
	}
	*out = d;
	return 0;
}

int
pg106_dma_addr(const struct pg106_dma *d, uint64_t off, uint64_t *addr,
	       uint64_t *contig)
{
	uint64_t at = 0;
	uint32_t i;

	if (!d || off >= d->size)
		return -EFAULT;
	for (i = 0; i < d->nseg; i++) {
		if (off < at + d->seg_len[i]) {
			*addr = d->seg_addr[i] + (off - at);
			if (contig)
				*contig = d->seg_len[i] - (off - at);
			return 0;
		}
		at += d->seg_len[i];
	}
	return -EFAULT;
}

void *
pg106_dma_cpu(const struct pg106_dma *d)
{
	return d && d->bmd ? d->bmd->getBytesNoCopy() : nullptr;
}
