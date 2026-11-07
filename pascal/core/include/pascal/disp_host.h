// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/disp_host.h — what the display driver
// (pascal/core disp_hw.*, pascal/fb PG106FB.kext) needs from the GPU owner
// (the PG106 kext, or a test harness).
//
// One C ops table so that the framebuffer kext needs neither PG106's C++
// symbols nor an OSBundleLibraries dependency on it. PG106FB gets the table
// from its provider through a virtual call:
//
//   struct pg106_disp_host host;
//   provider->callPlatformFunction(
//       OSSymbol::withCStringNoCopy(PG106_DISP_HOST_FUNCTION), false,
//       &host, (void *)(uintptr_t)PG106_DISP_HOST_ABI, nullptr, nullptr);
//
// PG106 (pascal/kext/src/PG106_disp.cpp, owned by the kext side) answers
// kIOReturnSuccess after filling every field it supports, and
// kIOReturnUnsupported when param2 is not PG106_DISP_HOST_ABI. The table
// stays valid until PG106 stops (PG106FB is its child, so it is terminated
// first).
//
// Calling context: unless a field says otherwise, every hook is called on
// PG106's work loop with its command gate held (PG106FB enters the gate
// through run_on_gate below). Returns are 0 or a negative errno.
//
// Status: the interface is agreed with the kext side (2026-11-07). Nothing
// behind it has run on the GPU yet.

#ifndef PASCAL_DISP_HOST_H
#define PASCAL_DISP_HOST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PG106_DISP_HOST_FUNCTION "org.helios300.pg106.disp_host"
// Bump on any incompatible change to struct pg106_disp_host.
#define PG106_DISP_HOST_ABI 0x00010000u

// PMC_INTR_0 bit for the display engine (nvkm/subdev/mc/gp100.c
// gp100_mc_intr: { NVKM_ENGINE_DISP, 0, 0, 0x04000000 }).
#define PG106_PMC_INTR_DISP 0x04000000u

// power_notify events.
enum pg106_disp_power_event {
	PG106_DISP_POWER_WILL_OFF = 0, // before PG00._OFF (D3cold): state lost
	PG106_DISP_POWER_DID_ON = 1,   // after power-on + POST + boot
};

typedef void (*pg106_disp_intr_fn)(void *ctx);
typedef void (*pg106_disp_power_fn)(void *ctx, int event);
typedef int (*pg106_disp_gated_fn)(void *ctx);

struct pg106_disp_host {
	uint32_t abi;  // PG106_DISP_HOST_ABI, set by the provider
	void *user;    // passed back as `user` to every hook

	// --- BAR0 (required) ------------------------------------------------
	uint32_t (*rd32)(void *user, uint32_t addr);
	void (*wr32)(void *user, uint32_t addr, uint32_t val);
	void (*delay_us)(void *user, uint32_t us);
	// PTIMER in ns (for poll budgets and vblank timestamps). Optional.
	uint64_t (*time_ns)(void *user);

	// --- VRAM (required) --------------------------------------------------
	// Physically contiguous VRAM. `pa` is the VRAM physical address the
	// display ctxdmas and the channel PUSH words use. size/align in bytes.
	int (*vram_alloc)(void *user, uint64_t size, uint64_t align,
			  uint64_t *pa);
	void (*vram_free)(void *user, uint64_t pa, uint64_t size);
	// CPU access to VRAM (PRAMIN window or BAR1; the provider chooses).
	// src NULL = zero fill.
	int (*vram_write)(void *user, uint64_t pa, const void *src,
			  uint32_t len);
	uint32_t (*vram_rd32)(void *user, uint64_t pa);

	// --- BAR1 (optional) ----------------------------------------------------
	// Map [pa, pa+size) into the BAR1 aperture; *bar1_off is the byte
	// offset inside BAR1 (PCI BAR index 1). -ENOSYS while the BAR1 VMM is
	// not up: the framebuffer then runs its shadow-copy fallback.
	int (*bar1_map)(void *user, uint64_t pa, uint64_t size,
			uint64_t *bar1_off);
	void (*bar1_unmap)(void *user, uint64_t bar1_off, uint64_t size);
	uint64_t bar1_phys; // CPU physical address of BAR1 (0 = unknown)
	uint64_t bar1_size;

	// --- interrupts (required for hotplug/vblank/supervisor) -------------
	// Route PMC_INTR_0 & mask to fn(ctx) on the work loop (secondary
	// context, gate held). The provider owns PMC_INTR_EN and sets the bit;
	// fn acks its own engine status (0x6100xx) and the provider re-arms
	// after fn returns. fn NULL unregisters (and clears the enable).
	int (*intr_set)(void *user, uint32_t pmc_mask, pg106_disp_intr_fn fn,
			void *ctx);

	// --- power + lifetime -------------------------------------------------
	// Keep the GPU booted and powered while a display is in use
	// (PG106::openSession / closeSession). session_open may take seconds
	// (POST/boot) and returns -ENODEV when bring-up failed.
	int (*session_open)(void *user);
	void (*session_close)(void *user);
	// Register (fn non-NULL) or drop (fn NULL) the power event sink.
	int (*power_notify)(void *user, pg106_disp_power_fn fn, void *ctx);
	// Run fn(ctx) with the provider's command gate held, from any thread
	// that does not already hold it. Returns fn's result.
	int (*run_on_gate)(void *user, pg106_disp_gated_fn fn, void *ctx);

	// --- firmware tables --------------------------------------------------
	// The VBIOS the provider fetched (ACPI _ROM). Valid while the
	// provider lives. -ENOENT when none was fetched.
	int (*vbios)(void *user, const uint8_t **data, uint32_t *len);
};

#ifdef __cplusplus
}
#endif

#endif // PASCAL_DISP_HOST_H
