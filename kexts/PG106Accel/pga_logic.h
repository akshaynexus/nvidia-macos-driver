/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

// pga_logic.h: the decisions PG106Accel makes, with no kernel dependency, so that they can be tested on the host
// (tests/test_pga_logic.c, run by tests/run.sh and by cross-build.sh). The kernel code gathers the facts from the
// registry and the pg106_kif.h calls, and acts on the answers here. All of it is plain C99, header-only, and the same
// code in both places.

#pragma once
#include <stdint.h>
#include "pg106_kif.h"   // pascal/bridge/include, SPDX MIT (the cross-build checks that)

#ifdef __cplusplus
extern "C" {
#endif

// ---- versions -------------------------------------------------------------------------------------------------
// The pg106_kif.h rule as a function (its macro, given two constants, trips -Wtype-limits while the minor is 0).
static inline int pga_kif_accepts(uint32_t mine, uint32_t theirs) { return PG106_KIF_ABI_ACCEPTS(mine, theirs); }

// ---- mode (boot-args) ----------------------------------------------------------------------------------------
// pg106accel_display=0 puts the accelerator back in its headless role (offscreen Metal only, no framebuffer). It is
// the rollback for the display-attached mode, and needs no reinstall. Absent, or any other value: display-attached.
// -pg106acceloff (checked in probe) stops the accelerator altogether.
static inline int pga_mode_display(int present, uint32_t value) { return !present || value != 0; }

// ---- which framebuffer the accelerator takes -------------------------------------------------------------------
// Only the GP106's own HDMI framebuffer (PG106FB, a child of the PG106 on this accelerator's PCI device), only one,
// only in display mode, only after PG106 has been ready once, and only when a frame can actually reach it. The Intel
// panel's framebuffer is on another PCI device and is not a PG106FB, so it fails twice.
struct pga_fb_facts {
	int is_pg106fb;        // fb->metaCast("PG106FB")
	int same_pci;          // fb -> PG106 -> the accelerator's own IOPCIDevice
	int has_abi;           // fb publishes PG106FBKifABI
	uint32_t abi;          // its value
	int mode_display;      // pga_mode_display()
	int pg106_ready;       // PG106Ready seen true once (the gate MetalPluginName also waits for)
	int already_paired;    // another PG106FB already has this accelerator's pipe
	int frame_path;        // pga_choose_path() != PGA_PATH_NONE
};
enum pga_fb_verdict {
	PGA_FB_ACCEPT = 0,
	PGA_FB_NOT_PG106FB,    // e.g. the Intel panel (AppleIntelFramebuffer) or anything else
	PGA_FB_OTHER_GPU,      // a PG106FB on another PCI device
	PGA_FB_NO_ABI,         // pg106fb_accel=0, or a PG106FB older than pg106_kif.h
	PGA_FB_ABI_MISMATCH,
	PGA_FB_HEADLESS,       // pg106accel_display=0
	PGA_FB_NOT_READY,      // PG106Ready never true yet: try again when it is
	PGA_FB_ALREADY,        // one display pipe per accelerator
	PGA_FB_NO_FRAME_PATH,  // no VRAM/copy from PG106 and no CPU mapping of the scanout: frames could not reach the screen
};
static inline enum pga_fb_verdict pga_fb_decide(const struct pga_fb_facts *f)
{
	if (!f->is_pg106fb) return PGA_FB_NOT_PG106FB;
	if (!f->same_pci) return PGA_FB_OTHER_GPU;
	if (!f->mode_display) return PGA_FB_HEADLESS;
	if (!f->has_abi) return PGA_FB_NO_ABI;
	if (!pga_kif_accepts(f->abi, PG106_KIF_ABI)) return PGA_FB_ABI_MISMATCH;
	if (!f->pg106_ready) return PGA_FB_NOT_READY;
	if (f->already_paired) return PGA_FB_ALREADY;
	if (!f->frame_path) return PGA_FB_NO_FRAME_PATH;
	return PGA_FB_ACCEPT;
}
static inline const char *pga_fb_verdict_str(enum pga_fb_verdict v)
{
	switch (v) {
	case PGA_FB_ACCEPT: return "accepted";
	case PGA_FB_NOT_PG106FB: return "not the GP106's HDMI framebuffer (not a PG106FB)";
	case PGA_FB_OTHER_GPU: return "a PG106FB on another PCI device";
	case PGA_FB_NO_ABI: return "no PG106FBKifABI (pg106fb_accel=0, or an older PG106FB)";
	case PGA_FB_ABI_MISMATCH: return "PG106FBKifABI is incompatible";
	case PGA_FB_HEADLESS: return "pg106accel_display=0 (headless)";
	case PGA_FB_NOT_READY: return "PG106 not ready yet";
	case PGA_FB_ALREADY: return "another PG106FB already has the display pipe";
	case PGA_FB_NO_FRAME_PATH: return "no way to put a frame on it yet (no PG106 VRAM/copy, no scanout CPU mapping)";
	}
	return "?";
}
// PGA_FB_NOT_READY and PGA_FB_NO_FRAME_PATH can change without the framebuffer publishing again.
static inline int pga_fb_retry_later(enum pga_fb_verdict v) { return v == PGA_FB_NOT_READY || v == PGA_FB_NO_FRAME_PATH; }

// ---- how a frame reaches the HDMI screen ---------------------------------------------------------------------
enum pga_path {
	PGA_PATH_NONE = 0,
	PGA_PATH_CPU_FB,   // the CPU copies the IOSurface into PG106FB's own scanout (its kva); no flip
	PGA_PATH_CPU_RING, // the CPU copies into a BAR1-mapped VRAM back buffer, then a vblank flip to it
	PGA_PATH_CE_RING,  // PG106's copy engine copies into a VRAM back buffer, then a vblank flip to it
};
#define PGA_CAPS_CE_RING (PG106_KIF_CAP_VRAM | PG106_KIF_CAP_VM | PG106_KIF_CAP_COPY)
#define PGA_CAPS_CPU_RING (PG106_KIF_CAP_VRAM | PG106_KIF_CAP_BAR1)
// kif_ok: PG106 publishes a compatible PG106KifABI and INFO answered ready. ce_allowed: pg106accel_ce is not 0.
static inline enum pga_path pga_choose_path(int kif_ok, uint32_t caps, int ce_allowed, int fb_kva)
{
	if (kif_ok && ce_allowed && (caps & PGA_CAPS_CE_RING) == PGA_CAPS_CE_RING) return PGA_PATH_CE_RING;
	if (kif_ok && (caps & PGA_CAPS_CPU_RING) == PGA_CAPS_CPU_RING) return PGA_PATH_CPU_RING;
	if (fb_kva) return PGA_PATH_CPU_FB;
	return PGA_PATH_NONE;
}
static inline const char *pga_path_str(enum pga_path p)
{
	switch (p) {
	case PGA_PATH_NONE: return "none";
	case PGA_PATH_CPU_FB: return "CPU copy into the framebuffer's scanout";
	case PGA_PATH_CPU_RING: return "CPU copy into a VRAM back buffer + vblank flip";
	case PGA_PATH_CE_RING: return "copy engine into a VRAM back buffer + vblank flip";
	}
	return "?";
}

// ---- geometry ------------------------------------------------------------------------------------------------
// PG106FB's flip rules (pg106_kif.h FB_FLIP): pitch a multiple of 256 and >= 4 * width, size = the mode, 'BGRA'.
static inline uint32_t pga_scanout_pitch(uint32_t width) { return (uint32_t)(((uint64_t)width * 4u + 255u) & ~255ull); }
static inline uint64_t pga_scanout_bytes(uint32_t width, uint32_t height)
{
	// 4 KiB multiple, so that consecutive ring buffers keep the 4 KiB offset alignment
	return (((uint64_t)pga_scanout_pitch(width) * height) + 4095u) & ~4095ull;
}
enum pga_src_verdict { PGA_SRC_OK = 0, PGA_SRC_FORMAT, PGA_SRC_SIZE, PGA_SRC_PITCH, PGA_SRC_SHORT };
// An IOSurface WindowServer handed to the display pipe: can it be copied 1:1 onto a mode of mode_w x mode_h?
static inline enum pga_src_verdict pga_src_check(uint32_t fmt, uint64_t w, uint64_t h, uint64_t pitch, uint64_t bytes,
						  uint32_t mode_w, uint32_t mode_h)
{
	if (fmt != PG106_KIF_FMT_BGRA) return PGA_SRC_FORMAT;
	if (!w || !h || w != mode_w || h != mode_h) return PGA_SRC_SIZE;
	if (pitch < 4 * w || (pitch & 3) || pitch > 16384ull * 8) return PGA_SRC_PITCH;
	if (h * pitch > bytes) return PGA_SRC_SHORT;
	return PGA_SRC_OK;
}
static inline int pga_flip_ok(uint64_t vram_offset, uint32_t w, uint32_t h, uint32_t pitch, uint32_t fmt,
			      uint32_t mode_w, uint32_t mode_h)
{
	return !(vram_offset & 4095u) && fmt == PG106_KIF_FMT_BGRA && w == mode_w && h == mode_h && w && h &&
	       !(pitch & 255u) && pitch >= 4u * w;
}

// ---- the scanout ring ----------------------------------------------------------------------------------------
// Three VRAM back buffers: one on screen (FRONT), one waiting for its vblank (QUEUED), one being filled. A buffer is
// written only when it is FREE, so the copy never lands in the buffer being scanned out or about to be.
#define PGA_RING 3
enum { PGA_BUF_FREE = 0, PGA_BUF_FILLING, PGA_BUF_QUEUED, PGA_BUF_FRONT };
struct pga_ring {
	uint8_t st[PGA_RING];
};
static inline void pga_ring_init(struct pga_ring *r) { for (int i = 0; i < PGA_RING; i++) r->st[i] = PGA_BUF_FREE; }
static inline int pga_ring_acquire(struct pga_ring *r)
{
	for (int i = 0; i < PGA_RING; i++) if (r->st[i] == PGA_BUF_FREE) { r->st[i] = PGA_BUF_FILLING; return i; }
	return -1;
}
// the copy failed, or PG106FB answered anything but success: nothing of it reached the screen
static inline void pga_ring_abandon(struct pga_ring *r, int i) { if (i >= 0 && i < PGA_RING && r->st[i] == PGA_BUF_FILLING) r->st[i] = PGA_BUF_FREE; }
static inline void pga_ring_queued(struct pga_ring *r, int i) { if (i >= 0 && i < PGA_RING && r->st[i] == PGA_BUF_FILLING) r->st[i] = PGA_BUF_QUEUED; }
// FLIP_DONE for buffer i: on screen now (the one before it is free again), or aborted (head/GPU off: free).
static inline void pga_ring_done(struct pga_ring *r, int i, int on_screen)
{
	if (i < 0 || i >= PGA_RING || r->st[i] != PGA_BUF_QUEUED) return;
	if (!on_screen) { r->st[i] = PGA_BUF_FREE; return; }
	for (int k = 0; k < PGA_RING; k++) if (r->st[k] == PGA_BUF_FRONT) r->st[k] = PGA_BUF_FREE;
	r->st[i] = PGA_BUF_FRONT;
}
// FLIP_DONE of a HOME flip (the framebuffer's own scanout is back): no ring buffer is on screen any more.
static inline void pga_ring_home_done(struct pga_ring *r)
{
	for (int k = 0; k < PGA_RING; k++) if (r->st[k] == PGA_BUF_FRONT) r->st[k] = PGA_BUF_FREE;
}
static inline int pga_ring_count(const struct pga_ring *r, int state)
{
	int n = 0;
	for (int i = 0; i < PGA_RING; i++) n += r->st[i] == state;
	return n;
}
// The ring's VRAM may be freed (mode change, teardown) only when nothing of it is on screen or queued.
static inline int pga_ring_idle(const struct pga_ring *r)
{
	return !pga_ring_count(r, PGA_BUF_QUEUED) && !pga_ring_count(r, PGA_BUF_FRONT) && !pga_ring_count(r, PGA_BUF_FILLING);
}

// FLIP cookies: the mode generation, a sequence number and the ring index (PGA_COOKIE_HOME for a HOME flip).
#define PGA_COOKIE_HOME 0xfu
static inline uint64_t pga_cookie(uint32_t generation, uint32_t seq, unsigned buf)
{
	return ((uint64_t)generation << 32) | ((uint64_t)(seq & 0x0fffffffu) << 4) | (buf & 0xfu);
}
static inline unsigned pga_cookie_buf(uint64_t c) { return (unsigned)(c & 0xfu); }
static inline uint32_t pga_cookie_generation(uint64_t c) { return (uint32_t)(c >> 32); }

// ---- event stamps --------------------------------------------------------------------------------------------
// GPU stamps: a semaphore release on PG106's copy channel writes the family's stamp, after every copy queued before
// it there. Only when the channel exists and the stamp page is mapped into the accelerator VM; else the CPU writes it.
static inline int pga_gpu_stamps(int chan_open, uint64_t stamp_va, int allowed) { return chan_open && stamp_va && allowed; }

// ---- reporting -----------------------------------------------------------------------------------------------
static inline uint32_t pga_mb(uint64_t bytes) { return (uint32_t)(bytes >> 20); }

// ---- when Metal is offered -----------------------------------------------------------------------------------
// In display mode MetalPluginName is held after PG106Ready until the HDMI framebuffer has been offered a pipe, for at
// most PGA_FB_WAIT_MS: a WindowServer that finds the GP106 should already see PG106AccelDisplay = true, or it refuses
// the device (plugin/NVMTLDevice.m) and would not composite HDMI on it until it restarts.
#define PGA_FB_WAIT_MS 10000u
static inline int pga_offer_now(int mode_display, int fb_decided, uint32_t waited_ms)
{
	return !mode_display || fb_decided || waited_ms >= PGA_FB_WAIT_MS;
}

#ifdef __cplusplus
}
#endif
