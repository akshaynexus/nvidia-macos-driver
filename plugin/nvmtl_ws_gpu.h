/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

// nvmtl_ws_gpu.h: does WindowServer get a Metal device on this GPU? Plain C, no Apple headers, so that the rule is
// tested on the host (kexts/PG106Accel/tests/test_pga_logic.c).
//
// WindowServer composites each display on the GPU whose accelerator drives it. A GPU that drives no display is a
// device for apps only, as on Apple's two-GPU Macs, and WindowServer gets nothing from this plugin there:
//   - AMD/Intel under NMXAccel: WindowServer draws into NMXGPU's framebuffer through Apple's own path;
//   - an NVIDIA GPU under NVAccel that the kernel calls headless (debug.nvaccel_headless);
//   - the Pascal GP106 under PG106Accel, unless PG106Accel drives the GP106's HDMI display. It publishes
//     PG106AccelDisplay = true on itself once that framebuffer has its display pipe (kexts/PG106Accel). Then
//     WindowServer gets the device so that it can composite the HDMI screen on the GP106. The internal panel stays
//     on the Intel GPU: it is wired there, and PG106Accel refuses every framebuffer but PG106FB's.
// The decision is per accelerator. On this machine that means per display, because the GP106's only display is HDMI.

#pragma once
#include <stdint.h>

enum nvmtl_ws_verdict {
	NVMTL_WS_NOT_WINDOWSERVER = 0,  // the rule is not about this process
	NVMTL_WS_ALLOW_PASCAL_DISPLAY,  // WindowServer, Pascal GP106 driving HDMI: device given, whatever the allow-list says
	NVMTL_WS_DECIDE_BY_LIST,        // WindowServer on an NVIDIA GPU with a display: the allow-list decides, as before
	NVMTL_WS_DENY_OTHER_VENDOR,     // AMD/Intel (NMXAccel)
	NVMTL_WS_DENY_PASCAL_HEADLESS,  // Pascal GP106 that drives no display (no HDMI pipe, or pg106accel_display=0)
	NVMTL_WS_DENY_NV_HEADLESS,      // NVAccel with debug.nvaccel_headless = 1
};
static inline enum nvmtl_ws_verdict nvmtl_ws_decide(int is_windowserver, uint16_t vendor, int is_pascal,
						    int pascal_display, int nv_headless)
{
	if (!is_windowserver) return NVMTL_WS_NOT_WINDOWSERVER;
	if (vendor != 0x10de) return NVMTL_WS_DENY_OTHER_VENDOR;
	if (is_pascal) return pascal_display ? NVMTL_WS_ALLOW_PASCAL_DISPLAY : NVMTL_WS_DENY_PASCAL_HEADLESS;
	if (nv_headless) return NVMTL_WS_DENY_NV_HEADLESS;
	return NVMTL_WS_DECIDE_BY_LIST;
}
static inline const char *nvmtl_ws_verdict_str(enum nvmtl_ws_verdict v)
{
	switch (v) {
	case NVMTL_WS_NOT_WINDOWSERVER: return "not WindowServer";
	case NVMTL_WS_ALLOW_PASCAL_DISPLAY: return "Pascal GP106 drives the HDMI display (PG106AccelDisplay): device for WindowServer";
	case NVMTL_WS_DECIDE_BY_LIST: return "NVIDIA GPU with a display: the allow-list decides";
	case NVMTL_WS_DENY_OTHER_VENDOR: return "AMD/Intel GPU: no device for WindowServer (it composites through the framebuffer's own path)";
	case NVMTL_WS_DENY_PASCAL_HEADLESS: return "Pascal GP106 drives no display: no device for WindowServer (it composites on the Intel GPU)";
	case NVMTL_WS_DENY_NV_HEADLESS: return "headless GPU: no device for WindowServer (it composites on the GPU that owns the display)";
	}
	return "?";
}
static inline int nvmtl_ws_denied(enum nvmtl_ws_verdict v)
{
	return v == NVMTL_WS_DENY_OTHER_VENDOR || v == NVMTL_WS_DENY_PASCAL_HEADLESS || v == NVMTL_WS_DENY_NV_HEADLESS;
}
