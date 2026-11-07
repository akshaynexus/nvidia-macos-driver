// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/src/PG106_disp.cpp — PG106's side of the display host
// interface (pascal/core/include/pascal/disp_host.h, ABI 1.0, agreed with
// the display side in 6b46a85): BAR0, VRAM, BAR1, interrupt routing,
// sessions, power events, the gate and the VBIOS, for PG106FB.
//
// Every hook except run_on_gate runs with PG106's gate held (PG106FB
// enters through run_on_gate), as disp_host.h requires. VRAM and BAR1
// exist once the bring-up reached PTOP and pg106_gpu brought the memory
// layer up; before that vram_alloc / bar1_map answer -ENODEV.
//
// SIMULATION status: compiles and links (cross-build); the core it calls
// is tested on Linux (test_vmm, test_intr); never run on a Mac.

#include "PG106.hpp"

#include <IOKit/IOLib.h>
#include <sys/errno.h>

static PG106 *
self(void *u)
{
	return static_cast<PG106 *>(u);
}

static uint32_t
dhRd32(void *u, uint32_t a)
{
	return self(u)->rd32(a);
}

static void
dhWr32(void *u, uint32_t a, uint32_t v)
{
	self(u)->wr32(a, v);
}

static void
dhDelay(void *, uint32_t us)
{
	if (us >= 1000) {
		IOSleep(us / 1000);
		us %= 1000;
	}
	if (us)
		IODelay(us);
}

static uint64_t
dhTime(void *u)
{
	return self(u)->ptimerNs();
}

static int
dhVramAlloc(void *u, uint64_t size, uint64_t align, uint64_t *pa)
{
	// Head allocation: kernel objects (page tables, instance blocks,
	// WPR) come from the tail, like nouveau's nvkm_ram_get(back = true).
	return self(u)->vramAlloc(size, align, 0, pa);
}

static void
dhVramFree(void *u, uint64_t pa, uint64_t size)
{
	self(u)->vramFree(pa, size);
}

static int
dhVramWrite(void *u, uint64_t pa, const void *src, uint32_t len)
{
	return self(u)->vramWrite(pa, src, len);
}

static uint32_t
dhVramRd32(void *u, uint64_t pa)
{
	return self(u)->vramRd32(pa);
}

static int
dhBar1Map(void *u, uint64_t pa, uint64_t size, uint64_t *off)
{
	return self(u)->bar1Map(pa, size, off);
}

static void
dhBar1Unmap(void *u, uint64_t off, uint64_t size)
{
	self(u)->bar1Unmap(off, size);
}

static void
dhIntrTrampoline(void *ctx, uint32_t)
{
	PG106 *p = static_cast<PG106 *>(ctx);

	if (p->fDispIntrFn)
		p->fDispIntrFn(p->fDispIntrCtx);
}

static int
dhIntrSet(void *u, uint32_t mask, pg106_disp_intr_fn fn, void *ctx)
{
	PG106 *p = self(u);
	int ret;

	if (p->fDispIntrMask) // drop the old route first
		p->intrRoute(p->fDispIntrMask, nullptr, nullptr);
	p->fDispIntrFn = fn;
	p->fDispIntrCtx = fn ? ctx : nullptr;
	p->fDispIntrMask = fn ? mask : 0;
	if (!fn)
		return 0;
	// PG106FB asks for PG106_PMC_INTR_DISP | 0x00200000 (GPIO/I2C, HPD):
	// both bits go to fn.
	ret = p->intrRoute(mask, dhIntrTrampoline, p);
	if (ret) {
		p->fDispIntrFn = nullptr;
		p->fDispIntrMask = 0;
	}
	return ret;
}

static int
dhSessionOpen(void *u)
{
	return self(u)->openSession();
}

static void
dhSessionClose(void *u)
{
	self(u)->closeSession();
}

static int
dhPowerNotify(void *u, pg106_disp_power_fn fn, void *ctx)
{
	self(u)->fDispPowerFn = fn;
	self(u)->fDispPowerCtx = fn ? ctx : nullptr;
	return 0;
}

static IOReturn
dhGated(OSObject *, void *fn, void *ctx, void *ret, void *)
{
	*static_cast<int *>(ret) = reinterpret_cast<pg106_disp_gated_fn>(fn)(ctx);
	return kIOReturnSuccess;
}

static int
dhRunOnGate(void *u, pg106_disp_gated_fn fn, void *ctx)
{
	int ret = -ENODEV;

	if (self(u)->runOnGate(dhGated, reinterpret_cast<void *>(fn), ctx, &ret) !=
	    kIOReturnSuccess)
		return -ENODEV;
	return ret;
}

static int
dhVbios(void *u, const uint8_t **data, uint32_t *len)
{
	const struct pg106_vbios_blob *b = self(u)->vbiosBlob();

	if (!b->data || !b->size)
		return -ENOENT;
	*data = b->data;
	*len = b->size;
	return 0;
}

void
PG106::dispFill(struct pg106_disp_host *h)
{
	memset(h, 0, sizeof(*h));
	h->abi = PG106_DISP_HOST_ABI;
	h->user = this;
	h->rd32 = dhRd32;
	h->wr32 = dhWr32;
	h->delay_us = dhDelay;
	h->time_ns = dhTime;
	h->vram_alloc = dhVramAlloc;
	h->vram_free = dhVramFree;
	h->vram_write = dhVramWrite;
	h->vram_rd32 = dhVramRd32;
	h->bar1_map = dhBar1Map;
	h->bar1_unmap = dhBar1Unmap;
	h->bar1_phys = fBar1Phys;
	h->bar1_size = fBar1Len;
	h->intr_set = dhIntrSet;
	h->session_open = dhSessionOpen;
	h->session_close = dhSessionClose;
	h->power_notify = dhPowerNotify;
	h->run_on_gate = dhRunOnGate;
	h->vbios = dhVbios;
}

void
PG106::dispPower(int event)
{
	if (fDispPowerFn)
		fDispPowerFn(fDispPowerCtx, event);
}
