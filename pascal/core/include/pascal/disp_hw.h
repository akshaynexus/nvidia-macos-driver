// SPDX-License-Identifier: GPL-2.0-only
// pascal/core/include/pascal/disp_hw.h — the GP106 display driver proper:
// the hardware paths that disp_core.h / disp_mode.h only modelled.
//
// It runs the display engine through a pg106_disp_host (disp_host.h): the
// PG106 kext on the Mac, a register-semantics mock in the tests
// (tests/test_disp_hw.c). Everything nouveau does for one HDMI head,
// in nouveau's order:
//
//   oneinit   display instance memory (64 KiB @ 64 KiB, VRAM) with the
//             RAMHT at its head; nv50_disp_oneinit (nvkm/engine/disp/
//             nv50.c:1637-1642), nvkm_ramht_new (core/ramht.c:141-162)
//   init      gf119_disp_init + IOR power (pg106_disp_init, disp_core.c)
//   channels  core (ctrl/user 0) and base (ctrl/user 1 + head) DMA
//             channels with 4 KiB pushbuffers in VRAM (dispnv50/disp.c:
//             250-263: VRAM on Pascal, max = 0x400 - 1 - 12 dwords);
//             gp102_disp_core_init / dmac_init (gp102.c:33-127); per-chan
//             interrupt enable 0x6100a0 (gf119.c:504-515); cursor PIO
//             channel ctrl 13 user 17 (gp102.c:62-67, gf119.c:536-560)
//   ctxdmas   NV50_DISP_HANDLE_SYNCBUF 0xf0000000 (4 KiB sync buffer) and
//             NV50_DISP_HANDLE_VRAM 0xf0000001 (all VRAM) per DMA channel
//             (dispnv50/disp.c:291-312), the base channel's framebuffer
//             ctxdma 0xfb000000 (NV50_DISP_HANDLE_WNDW_CTX(kind 0)); each a
//             24-byte gf119 dmaobj (engine/dma/usergf119.c:39-60, flags0
//             = kind<<20 | page<<6 | 9 for VRAM) inserted into the RAMHT
//             with context chid<<27 | 1 | offset<<9 (gf119_disp_dmac_bind,
//             gf119.c:568-573; ramht.c:59-97 hash)
//   core init SET_CONTEXT_DMA_NOTIFIER(sync) (core507d.c:131-141)
//   i2c       DCB CCB entry of the output (bios/i2c.c dcb_i2c_parse): GM200
//             hybrid pad mode (padgm200.c:29-50) + gf119 bit-bang bus at
//             0x00d014 + port*0x20 (busgf119.c), nouveau's internal
//             bit-bang algorithm (i2c/bit.c) for DDC/EDID at 0x50
//   hpd       connector HPD -> GPIO function -> line (engine/disp/
//             conn.c:44-75); sense 0x00d610 + line*4 bit 14
//             (gpio/gf119.c:78-82) compared to log[1] & 1
//             (gpio/base.c:96-109); interrupt enable/status 0x00dc08/
//             0x00dc88 + 0x00dc00/0x00dc80 (gpio/gk104.c:28-62)
//   modeset   SOR acquire (outp.c:115-200; HDA-capable SORs from
//             0x08a15c / 0x10ebb0, gp100.c:46-56), route (gm200.c:98-114),
//             HDMI ctrl + AVI infoframe (gk104.c:33-104 via
//             nv50_hdmi_enable, dispnv50/disp.c:777-850), ELD
//             (gf119.c:46-72), base stream then core stream
//             (disp_mode.h), notifier wait (core507d.c:62-71) while
//             servicing supervisor 1/2/3 inline (gf119.c:1011-1066 with the
//             VPLL through devinit pll_set and IED scripts through the
//             devinit interpreter, nv50.c:1012-1331)
//   vblank    0x6100c0 bit 0 enable, 0x610088 bit 24+head, ack 0x6100bc
//             (gf119.c:400-414, 1143-1153)
//   cursor    HEAD_SET_CONTROL_CURSOR / OFFSET / CONTEXT_DMA (head907d.c:
//             153-193) + the PIO channel's SET_CURSOR_HOT_SPOT_POINT_OUT
//             and UPDATE (curs507a.c:38-61)
//   fini      head disable stream, HDMI off, channel fini (gf119.c:517-
//             533, 575-595, 946-964), 0x6100b0 = 0 (gf119.c:1157-1162)
//
// STATUS: hardware code paths, exercised only against the mock display
// engine in tests/test_disp_hw.c (register semantics: channels fetch
// their pushbuffers from VRAM, resolve handles through the RAMHT, run the
// supervisor handshake, an emulated I2C EDID EEPROM on open-drain lines,
// GPIO HPD). It has NOT run on a GP106.
//
// Locking: every call must hold the host's gate (single-threaded).

#ifndef PASCAL_DISP_HW_H
#define PASCAL_DISP_HW_H

#include <pascal/disp_core.h>
#include <pascal/disp_host.h>
#include <pascal/disp_mode.h>
#include <pascal/disp_vbios.h>
#include <pascal/edid.h>
#include <pascal/vbios_bit.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PG106_DHW_HANDLE_SYNCBUF 0xf0000000u
#define PG106_DHW_HANDLE_VRAM    0xf0000001u
#define PG106_DHW_HANDLE_FB      0xfb000000u // WNDW_CTX(kind 0, pitch)

#define PG106_DHW_INST_SIZE   0x10000u
#define PG106_DHW_RAMHT_SIZE  0x1000u // bytes -> 512 entries, 9 bits
#define PG106_DHW_PUSH_SIZE   0x1000u
#define PG106_DHW_SYNC_SIZE   0x1000u
#define PG106_DHW_CURS_W      64u
#define PG106_DHW_CURS_SIZE   (64u * 64u * 4u)
// Largest scanout the framebuffer kext asks for (4096x2160 A8R8G8B8).
#define PG106_DHW_FB_MAX_W    4096u
#define PG106_DHW_FB_MAX_H    2160u
// TMDS limit without SCDC scrambling (gm200_sor_hdmi_scdc high_speed is
// khz > 340000; the sink's SCDC handshake is not implemented).
#define PG106_DHW_TMDS_MAX_KHZ 340000u

// Records of the devinit trace each IED/PLL run may use (a run that writes
// more aborts, as devinit does with a full trace).
#define PG106_DHW_INIT_TRACE 1024u

#define PG106_DHW_CURS_CTRL  13u
#define PG106_DHW_CURS_USER  17u

// The output the driver drives, from the DCB (or the Helios fallback).
struct pg106_dhw_outp {
	bool valid;
	struct pg106_dcb_outp dcb; // DCB entry (IED script context)
	uint8_t conn_type;         // 0x61 HDMI-A
	struct pg106_dcb_i2c i2c;  // CCB entry dcb.i2c_index
	uint8_t hpd_line;          // GPIO line (0xff = no HPD GPIO)
	uint8_t hpd_log1;          // GPIO log[1]: present when sense == log1&1
};

// The Helios 300 (86.06.4F.00.08) values pg106_dhw_outp_from_vbios
// returns, measured from research/firmware/vbios/helios300-gp106.rom
// (test_disp_hw checks they still agree when the ROM is present). Used
// when no VBIOS is available (mock tests).
extern const struct pg106_dhw_outp pg106_dhw_helios300_outp;

// First TMDS output whose connector is HDMI (type 0x61), else the first
// TMDS output. -ENOENT none.
int pg106_dhw_outp_from_vbios(const struct pg106_vbios_bit *vb,
			      struct pg106_dhw_outp *o);

// One mode WindowServer can pick (from the EDID).
struct pg106_dhw_mode {
	struct pg106_disp_mode m;  // DRM-style timing (m.head ignored)
	uint32_t refresh_mhz;      // vertical refresh, milli-Hz
	uint8_t vic;               // CTA VIC (0 = none)
	bool preferred;
};

struct pg106_dhw_chan {
	uint64_t push_pa;
	uint32_t cur, put, max; // dwords
	unsigned ctrl, user;
};

struct pg106_dhw_ramht_ent {
	int32_t chid; // -1 free
	uint32_t handle;
};

typedef void (*pg106_dhw_hotplug_fn)(void *ctx, bool connected);
typedef void (*pg106_dhw_vblank_fn)(void *ctx, int head, uint64_t ns);
// status 0 = the flip is on screen; -ECANCELED = aborted (head off/power).
typedef void (*pg106_dhw_flip_fn)(void *ctx, int status, uint64_t ns);

struct pg106_dhw_stats {
	uint32_t intr, supv[3], vblank, hpd_events, chan_errors;
	uint32_t ied_run, ied_missing, ied_fail, pll_set, pll_fail;
	uint32_t i2c_xfers, i2c_errors, ntfy_timeouts;
	uint32_t last_error_chid, last_error_mthd, last_error_data;
	char last_init_error[160]; // devinit's message for the last IED/PLL failure
	uint32_t last_init_script; // its script offset (0 = pll_set)
};

struct pg106_dhw {
	const struct pg106_disp_host *host;
	struct pg106_vbios_bit vb;
	bool have_vb;
	struct pg106_dhw_outp outp;
	struct pg106_dbus bus;      // over host (for disp_core.c)
	struct pg106_dtrace *trace; // optional

	unsigned heads_mask, sors_mask;
	uint8_t sor_hda_mask;
	uint64_t vram_size;         // limit of the VRAM ctxdmas

	// VRAM objects (physical addresses).
	uint64_t inst_pa, sync_pa, curs_pa, fb_pa;
	uint64_t fb_size;
	bool mem_allocated;
	uint32_t heap_next;         // next free byte in inst after the RAMHT
	struct pg106_dhw_ramht_ent ramht[PG106_DHW_RAMHT_SIZE >> 3];

	struct pg106_dhw_chan core, base;
	bool inited, chans_up;

	// Output state.
	bool connected;
	int head;                   // head driving the output (-1 none)
	int sor;                    // acquired SOR (-1 none)
	uint32_t sor_ctrl;          // last SOR_SET_CONTROL value sent
	bool high_speed, hdmi_on, audio;
	struct pg106_disp_mode mode;
	uint32_t fb_pitch;
	uint16_t fb_w, fb_h;
	struct pg106_disp_sor_sw sw[8];
	struct pg106_disp_super_result super_res; // latched by supervisor 1
	bool vblank_on;
	bool curs_on;
	bool flip_pending;
	uint64_t flip_pa, scan_pa; // pending flip / what is scanned out
	int16_t curs_x, curs_y;

	// EDID.
	uint8_t edid_raw[512];
	size_t edid_len;
	struct pg106_edid edid;
	bool edid_valid;

	// Events (called from pg106_dhw_intr, gate held).
	pg106_dhw_hotplug_fn hotplug;
	pg106_dhw_vblank_fn vblank;
	pg106_dhw_flip_fn flip_done; // once per accepted pg106_dhw_flip
	void *cb_ctx;

	struct pg106_dhw_stats st;
	// devinit's trace buffer (struct pg106_init_trace_rec, devinit.h),
	// kept opaque so C++ users need not include devinit.h (its `or` field
	// is a C++ keyword); disp_hw.c asserts the record size.
	uint32_t init_trace[PG106_DHW_INIT_TRACE * 6];
};

// Bind to a host. vbios/len may be NULL/0 (then `outp` must be given);
// outp NULL = derive from the VBIOS. Does not touch the GPU. -ENOENT no
// output, -EINVAL bad args.
int pg106_dhw_ctor(struct pg106_dhw *d, const struct pg106_disp_host *host,
		   const uint8_t *vbios, uint32_t vbios_len,
		   const struct pg106_dhw_outp *outp);

// Allocate the VRAM objects (first call only), then bring the engine up:
// disp init, channels, ctxdmas, core init, I2C pad/bus, HPD interrupts.
// Safe to call again after a power cycle (re-writes everything).
int pg106_dhw_init(struct pg106_dhw *d);

// Head off (if lit), channels down, display interrupts off. Keeps the VRAM
// allocations unless `release` (driver unload): those are freed.
void pg106_dhw_fini(struct pg106_dhw *d, bool release);

// HPD sense now (no EDID read). Updates d->connected.
bool pg106_dhw_detect(struct pg106_dhw *d);

// Read the EDID over DDC (base block + extensions, E-DDC segment pointer
// for blocks >= 2) and parse it. 0, -EIO/-ETIMEDOUT bus, -EBADMSG parse.
int pg106_dhw_read_edid(struct pg106_dhw *d);

// Modes from the parsed EDID, preferred first, duplicates removed, those
// above PG106_DHW_TMDS_MAX_KHZ / the FB limit dropped. Returns the count
// written (<= max).
size_t pg106_dhw_modes(const struct pg106_dhw *d, struct pg106_dhw_mode *out,
		       size_t max);

// Light the HDMI head with `mode`, scanning out the framebuffer at
// d->fb_pa with `pitch` bytes per row. mode NULL = turn the head off.
// Waits (polling) for the core notifier, running supervisors inline.
int pg106_dhw_modeset(struct pg106_dhw *d, const struct pg106_disp_mode *mode,
		      uint32_t pitch);

// Interrupt service: display (supervisor, vblank, channel errors) and the
// GPIO HPD lines. Call for PMC_INTR_0 & (DISP | GPIO).
void pg106_dhw_intr(struct pg106_dhw *d);

void pg106_dhw_vblank_enable(struct pg106_dhw *d, bool on);

// Flip the head's base channel to the surface at VRAM `pa` (256 B aligned;
// the kif contract asks 4 KiB), `pitch` (multiple of 256, >= width*4),
// at the current mode's size, A8R8G8B8. vsync: NON_TEARING interval 1,
// else IMMEDIATE. Returns 0 queued (flip_done follows once, from the
// vblank interrupt, when the base notifier leaves NOT_BEGUN), -EBUSY a
// previous flip has not landed, -ENODEV no head, -EINVAL geometry.
// (base907c_image_set + base507c_ntfy_set/update, dispnv50/base*.c.)
int pg106_dhw_flip(struct pg106_dhw *d, uint64_t pa, uint32_t pitch,
		   bool vsync);
bool pg106_dhw_flip_landed(struct pg106_dhw *d);

// 64x64 A8R8G8B8 cursor image (premultiplied, as WindowServer hands it).
int pg106_dhw_cursor_image(struct pg106_dhw *d, const uint32_t *argb64x64);
int pg106_dhw_cursor_show(struct pg106_dhw *d, bool on);
int pg106_dhw_cursor_move(struct pg106_dhw *d, int x, int y);

// Builders (pure, also used by tests).
// CTA-861 AVI InfoFrame (HDMI 1.4: RGB full range, VIC, aspect). Returns
// 17 (bytes: header[3], checksum, PB1..PB13).
int pg106_dhw_avi_infoframe(const struct pg106_disp_mode *m, uint8_t vic,
			    uint8_t out[17]);
// ELD (version 2) from the EDID: monitor name + CTA SADs. Returns the
// length (multiple of 4), 0 when the sink has no audio.
size_t pg106_dhw_eld(const uint8_t *edid, size_t edid_len, uint8_t *eld,
		     size_t max);
// The CTA VIC whose timing equals `m` (the table in edid.c's CEA list),
// 0 = none.
uint8_t pg106_dhw_mode_vic(const struct pg106_disp_mode *m);

// RAMHT hash (core/ramht.c:27-38) for 9 bits.
uint32_t pg106_dhw_ramht_hash(int chid, uint32_t handle);

#ifdef __cplusplus
}
#endif

#endif // PASCAL_DISP_HW_H
