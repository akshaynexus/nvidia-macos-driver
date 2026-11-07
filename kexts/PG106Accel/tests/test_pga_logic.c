/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

// Host tests for PG106Accel's decisions (pga_logic.h) and the plugin's WindowServer rule (plugin/nvmtl_ws_gpu.h).
// Run by tests/run.sh and by cross-build.sh. These test the logic only: nothing here touches IOKit or a GPU.
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "pga_logic.h"
#include "nvmtl_ws_gpu.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static struct pga_fb_facts good(void)
{
	struct pga_fb_facts f;
	memset(&f, 0, sizeof f);
	f.is_pg106fb = 1; f.same_pci = 1; f.has_abi = 1; f.abi = PG106_KIF_ABI; f.mode_display = 1;
	f.pg106_ready = 1; f.already_paired = 0; f.frame_path = 1;
	return f;
}

static void test_fb(void)
{
	struct pga_fb_facts f = good();
	CHECK(pga_fb_decide(&f) == PGA_FB_ACCEPT);
	// the Intel panel: not a PG106FB, and on the Intel PCI device
	f = good(); f.is_pg106fb = 0; f.same_pci = 0; f.has_abi = 0;
	CHECK(pga_fb_decide(&f) == PGA_FB_NOT_PG106FB);
	// even if someone gave it the property, a non-PG106FB is never taken
	f = good(); f.is_pg106fb = 0;
	CHECK(pga_fb_decide(&f) == PGA_FB_NOT_PG106FB);
	f = good(); f.same_pci = 0;
	CHECK(pga_fb_decide(&f) == PGA_FB_OTHER_GPU);
	f = good(); f.mode_display = 0;
	CHECK(pga_fb_decide(&f) == PGA_FB_HEADLESS);
	f = good(); f.has_abi = 0;          // pg106fb_accel=0
	CHECK(pga_fb_decide(&f) == PGA_FB_NO_ABI);
	f = good(); f.abi = 0x00020000u;    // a newer major
	CHECK(pga_fb_decide(&f) == PGA_FB_ABI_MISMATCH);
	f = good(); f.abi = PG106_KIF_ABI | 3u;   // a newer minor of the same major is fine for the provider
	CHECK(pga_fb_decide(&f) == PGA_FB_ACCEPT);
	f = good(); f.pg106_ready = 0;
	CHECK(pga_fb_decide(&f) == PGA_FB_NOT_READY && pga_fb_retry_later(PGA_FB_NOT_READY));
	f = good(); f.already_paired = 1;
	CHECK(pga_fb_decide(&f) == PGA_FB_ALREADY && !pga_fb_retry_later(PGA_FB_ALREADY));
	f = good(); f.frame_path = 0;
	CHECK(pga_fb_decide(&f) == PGA_FB_NO_FRAME_PATH && pga_fb_retry_later(PGA_FB_NO_FRAME_PATH));
	for (int v = PGA_FB_ACCEPT; v <= PGA_FB_NO_FRAME_PATH; v++) CHECK(strcmp(pga_fb_verdict_str((enum pga_fb_verdict)v), "?"));
}

static void test_abi(void)
{
	CHECK(PG106_KIF_ABI == 0x00010000u);
	CHECK(pga_kif_accepts(0x00010000u, 0x00010000u));
	CHECK(pga_kif_accepts(0x00010002u, 0x00010001u));   // callee newer minor: fine
	CHECK(!pga_kif_accepts(0x00010000u, 0x00010001u));  // caller newer minor: refused
	CHECK(!pga_kif_accepts(0x00020000u, 0x00010000u));
	// every request starts with { abi, size }
	CHECK(offsetof(struct pg106_kif_vram, size) == 4 && offsetof(struct pg106fb_kif_flip, size) == 4);
	CHECK(offsetof(struct pg106accel_kif_flip_done, cookie) == 8);
}

static void test_mode_path(void)
{
	CHECK(pga_mode_display(0, 0) == 1);   // absent: display-attached
	CHECK(pga_mode_display(1, 0) == 0);   // pg106accel_display=0: headless
	CHECK(pga_mode_display(1, 1) == 1);
	const uint32_t all = PG106_KIF_CAP_VRAM | PG106_KIF_CAP_BAR1 | PG106_KIF_CAP_VM | PG106_KIF_CAP_COPY | PG106_KIF_CAP_RELEASE;
	CHECK(pga_choose_path(1, all, 1, 1) == PGA_PATH_CE_RING);
	CHECK(pga_choose_path(1, all, 0, 1) == PGA_PATH_CPU_RING);   // pg106accel_ce=0
	CHECK(pga_choose_path(1, PG106_KIF_CAP_VRAM | PG106_KIF_CAP_VM, 1, 1) == PGA_PATH_CPU_FB);   // no copy, no BAR1
	CHECK(pga_choose_path(1, PG106_KIF_CAP_VRAM | PG106_KIF_CAP_BAR1, 1, 0) == PGA_PATH_CPU_RING);
	CHECK(pga_choose_path(0, all, 1, 1) == PGA_PATH_CPU_FB);     // caps mean nothing without a compatible kif
	CHECK(pga_choose_path(0, 0, 1, 0) == PGA_PATH_NONE);         // kdev3's first owner run: no kif, BAR1 not up
	CHECK(pga_choose_path(1, 0, 1, 0) == PGA_PATH_NONE);
}

static void test_geometry(void)
{
	CHECK(pga_scanout_pitch(1920) == 7680);   // already a multiple of 256
	CHECK(pga_scanout_pitch(1366) == 5632);   // 5464 -> 5632
	CHECK(pga_scanout_pitch(1) == 256);
	CHECK(pga_scanout_bytes(1920, 1080) == 7680ull * 1080);   // 8294400 = 2025 * 4096
	CHECK(pga_scanout_bytes(1366, 768) % 4096 == 0 && pga_scanout_bytes(1366, 768) >= 5632ull * 768);
	const uint32_t B = PG106_KIF_FMT_BGRA;
	CHECK(pga_src_check(B, 1920, 1080, 7680, 7680ull * 1080, 1920, 1080) == PGA_SRC_OK);
	CHECK(pga_src_check(B, 1920, 1080, 7936, 7936ull * 1080, 1920, 1080) == PGA_SRC_OK);   // IOSurface row padding
	CHECK(pga_src_check(0x34323076u, 1920, 1080, 7680, 7680ull * 1080, 1920, 1080) == PGA_SRC_FORMAT);   // '420v'
	CHECK(pga_src_check(B, 2560, 1440, 10240, 10240ull * 1440, 1920, 1080) == PGA_SRC_SIZE);   // no scaler
	CHECK(pga_src_check(B, 1920, 1080, 7676, 1ull << 30, 1920, 1080) == PGA_SRC_PITCH);
	CHECK(pga_src_check(B, 1920, 1080, 7682, 1ull << 30, 1920, 1080) == PGA_SRC_PITCH);   // not word aligned
	CHECK(pga_src_check(B, 1920, 1080, 7680, 7680ull * 1079, 1920, 1080) == PGA_SRC_SHORT);
	CHECK(pga_flip_ok(0x100000, 1920, 1080, 7680, B, 1920, 1080));
	CHECK(!pga_flip_ok(0x100100, 1920, 1080, 7680, B, 1920, 1080));   // 256 B but not 4 KiB aligned
	CHECK(!pga_flip_ok(0x100000, 1920, 1080, 7696, B, 1920, 1080));   // pitch not a multiple of 256
	CHECK(!pga_flip_ok(0x100000, 1280, 720, 5120, B, 1920, 1080));
}

static void test_ring(void)
{
	struct pga_ring r; pga_ring_init(&r);
	CHECK(pga_ring_idle(&r));
	int a = pga_ring_acquire(&r); CHECK(a == 0);
	pga_ring_queued(&r, a);
	int b = pga_ring_acquire(&r); CHECK(b == 1);     // filling while a waits for vblank
	pga_ring_done(&r, a, 1);                          // a on screen
	CHECK(r.st[a] == PGA_BUF_FRONT && !pga_ring_idle(&r));
	pga_ring_queued(&r, b);
	int c = pga_ring_acquire(&r); CHECK(c == 2);
	CHECK(pga_ring_acquire(&r) == -1);               // front + queued + filling: no fourth buffer, nothing on screen is written
	pga_ring_abandon(&r, c);                          // copy failed / flip busy
	CHECK(r.st[c] == PGA_BUF_FREE);
	pga_ring_done(&r, b, 1);                          // b replaces a: a is free again
	CHECK(r.st[a] == PGA_BUF_FREE && r.st[b] == PGA_BUF_FRONT);
	CHECK(pga_ring_count(&r, PGA_BUF_FRONT) == 1);
	// aborted flip (head off): the buffer is free, the front stays
	c = pga_ring_acquire(&r); pga_ring_queued(&r, c); pga_ring_done(&r, c, 0);
	CHECK(r.st[c] == PGA_BUF_FREE && r.st[b] == PGA_BUF_FRONT);
	// a done for a buffer that was not queued changes nothing
	pga_ring_done(&r, a, 1); CHECK(r.st[a] == PGA_BUF_FREE && r.st[b] == PGA_BUF_FRONT);
	pga_ring_done(&r, 7, 1); pga_ring_queued(&r, -1); pga_ring_abandon(&r, 9);
	// HOME: the framebuffer's own scanout is back, the ring may be freed
	pga_ring_home_done(&r);
	CHECK(pga_ring_idle(&r));
	// cookies
	uint64_t k = pga_cookie(0x1234, 0x7fffffff, 2);
	CHECK(pga_cookie_buf(k) == 2 && pga_cookie_generation(k) == 0x1234);
	CHECK(pga_cookie_buf(pga_cookie(1, 5, PGA_COOKIE_HOME)) == PGA_COOKIE_HOME);
	CHECK(pga_cookie(1, 1, 0) != pga_cookie(1, 2, 0));
}

static void test_misc(void)
{
	CHECK(pga_gpu_stamps(1, 0x1000, 1) && !pga_gpu_stamps(0, 0x1000, 1) && !pga_gpu_stamps(1, 0, 1) && !pga_gpu_stamps(1, 0x1000, 0));
	CHECK(pga_mb(6ull << 30) == 6144);
	CHECK(pga_offer_now(0, 0, 0));                         // headless: offer at once
	CHECK(!pga_offer_now(1, 0, 100));                       // display: wait for PG106FB
	CHECK(pga_offer_now(1, 1, 100));                        // it was decided (accepted or refused)
	CHECK(pga_offer_now(1, 0, PGA_FB_WAIT_MS));             // or the wait ran out
}

static void test_ws(void)
{
	CHECK(nvmtl_ws_decide(0, 0x10de, 1, 0, 0) == NVMTL_WS_NOT_WINDOWSERVER);   // apps: the rule does not apply
	CHECK(nvmtl_ws_decide(1, 0x10de, 1, 1, 0) == NVMTL_WS_ALLOW_PASCAL_DISPLAY);
	CHECK(nvmtl_ws_decide(1, 0x10de, 1, 0, 0) == NVMTL_WS_DENY_PASCAL_HEADLESS);
	CHECK(nvmtl_ws_decide(1, 0x10de, 1, 1, 1) == NVMTL_WS_ALLOW_PASCAL_DISPLAY);   // NVAccel's sysctl means nothing on Pascal
	CHECK(nvmtl_ws_decide(1, 0x8086, 0, 0, 0) == NVMTL_WS_DENY_OTHER_VENDOR);
	CHECK(nvmtl_ws_decide(1, 0x8086, 0, 1, 0) == NVMTL_WS_DENY_OTHER_VENDOR);
	CHECK(nvmtl_ws_decide(1, 0x10de, 0, 0, 1) == NVMTL_WS_DENY_NV_HEADLESS);
	CHECK(nvmtl_ws_decide(1, 0x10de, 0, 0, 0) == NVMTL_WS_DECIDE_BY_LIST);
	CHECK(nvmtl_ws_denied(NVMTL_WS_DENY_PASCAL_HEADLESS) && !nvmtl_ws_denied(NVMTL_WS_ALLOW_PASCAL_DISPLAY) &&
	      !nvmtl_ws_denied(NVMTL_WS_DECIDE_BY_LIST));
}

int main(void)
{
	test_abi(); test_fb(); test_mode_path(); test_geometry(); test_ring(); test_misc(); test_ws();
	printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
	return fails != 0;
}
