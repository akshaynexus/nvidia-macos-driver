// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/src/pg106_dma.h — host memory the GPU reaches by DMA, with
// bus addresses from IODMACommand (the system mapper's IOVAs when VT-d is
// on, physical addresses when it is off): IOKit/IODMACommand.h
// withSpecification(kIODMACommandOutputHost64, 40 address bits, kMapped),
// setMemoryDescriptor(autoPrepare) and gen64IOVMSegments.
//
// Used for the HS ACR image (nouveau keeps it in host memory), the sysmem
// flush page, render-client BOs and imports, and PG106Accel's SYSMEM maps.

#ifndef PG106_DMA_H
#define PG106_DMA_H

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOMemoryDescriptor.h>

#include <stdint.h>

struct pg106_dma {
	IOMemoryDescriptor *md;        // what is mapped (retained)
	IOBufferMemoryDescriptor *bmd; // set when pg106_dma_alloc made it
	IODMACommand *cmd;
	uint64_t size;
	uint32_t nseg;
	uint64_t *seg_addr, *seg_len;  // IOVA segments in order
	bool prepared_by_us;
};

// Wired kernel pages (zeroed), 4 KiB aligned, DMA-mapped. -ENOMEM.
int pg106_dma_alloc(uint64_t size, struct pg106_dma **out);
// Map an existing descriptor (its pages get wired for the mapping's
// lifetime: setMemoryDescriptor autoPrepare). Takes a reference.
int pg106_dma_wrap(IOMemoryDescriptor *md, struct pg106_dma **out);
// Bus address of byte off, and how many bytes from there are contiguous.
int pg106_dma_addr(const struct pg106_dma *d, uint64_t off, uint64_t *addr,
		   uint64_t *contig);
void *pg106_dma_cpu(const struct pg106_dma *d); // NULL unless allocated
void pg106_dma_free(struct pg106_dma *d);

#endif // PG106_DMA_H
