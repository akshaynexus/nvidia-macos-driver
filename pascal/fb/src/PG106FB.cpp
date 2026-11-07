// SPDX-License-Identifier: GPL-2.0-only
// pascal/fb/src/PG106FB.cpp — the GP106 HDMI IOFramebuffer. See PG106FB.hpp.
//
// Log lines are "PG106FB: ..." (the owner runbook's HDMI step lists them).
//
// STATUS: compiled and KPI-audited on Linux against the macOS 26.5 SDK
// (scripts/cross-build-macos.sh). Never loaded: everything below the IOKit
// glue is pascal/core disp_hw, which has only run against a mock display
// engine.

#include "PG106FB.hpp"

#include <IOKit/IOSubMemoryDescriptor.h>
#include <kern/clock.h>
#include <libkern/OSAtomic.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>

#define LOG(fmt, ...) IOLog("PG106FB: " fmt "\n", ##__VA_ARGS__)

#define super IOFramebuffer
OSDefineMetaClassAndStructors(PG106FB, IOFramebuffer)

// Shadow-copy fallback rate when BAR1 cannot map the scanout (Hz).
#define PG106FB_SHADOW_HZ 10u

static IOReturn
ioret(int err)
{
	switch (err) {
	case 0: return kIOReturnSuccess;
	case -EBUSY: return kIOReturnBusy;
	case -ENODEV: return kIOReturnNotReady;
	case -ETIMEDOUT: return kIOReturnTimeout;
	case -ENOMEM: case -ENOSPC: return kIOReturnNoResources;
	case -EINVAL: case -ERANGE: return kIOReturnBadArgument;
	case -ECANCELED: return kIOReturnAborted;
	default: return kIOReturnError;
	}
}

// ---------------------------------------------------------------------------
// C callbacks from disp_hw / PG106 (PG106's work loop, gate held)
// ---------------------------------------------------------------------------

static void cIntr(void *ctx) { static_cast<PG106FB *>(ctx)->onIntr(); }
static void cPower(void *ctx, int ev) { static_cast<PG106FB *>(ctx)->onPower(ev); }
static void cHotplug(void *ctx, bool on) { static_cast<PG106FB *>(ctx)->onHotplug(on); }
static void cVblank(void *ctx, int, uint64_t ns) { static_cast<PG106FB *>(ctx)->onVblank(ns); }
static void cFlip(void *ctx, int st, uint64_t ns) { static_cast<PG106FB *>(ctx)->onFlipDone(st, ns); }

struct gate_call {
	PG106FB *self;
	int (*fn)(PG106FB *, void *);
	void *arg;
};

int
PG106FB::sGateTrampoline(void *ctx)
{
	struct gate_call *c = static_cast<struct gate_call *>(ctx);
	return c->fn(c->self, c->arg);
}

int
PG106FB::gated(GatedFn fn, void *arg)
{
	struct gate_call c = { this, fn, arg };

	if (!fHost.run_on_gate)
		return -ENODEV;
	return fHost.run_on_gate(fHost.user, sGateTrampoline, &c);
}

// ---------------------------------------------------------------------------
// probe / start / stop
// ---------------------------------------------------------------------------

IOService *
PG106FB::probe(IOService *provider, SInt32 *score)
{
	uint32_t v = 1;

	if (PE_parse_boot_argn("pg106fb", &v, sizeof(v)) && v == 0) {
		LOG("disabled by boot-arg pg106fb=0");
		return nullptr;
	}
	// Only a child of PG106 (never Intel's or any other display device).
	if (!provider || !provider->metaCast("PG106")) {
		LOG("provider is not PG106 — not attaching");
		return nullptr;
	}
	return super::probe(provider, score);
}

int
PG106FB::gProbe(PG106FB *self, void *)
{
	self->reprobe();
	return 0;
}

// Gate held: HPD sense, EDID, mode list.
void
PG106FB::reprobe(void)
{
	if (!fDhw || !fInited)
		return;
	if (pg106_dhw_detect(fDhw)) {
		int r = pg106_dhw_read_edid(fDhw);
		if (r)
			LOG("EDID read failed: %d (i2c errors %u)", r, fDhw->st.i2c_errors);
		else
			LOG("EDID: %s \"%s\" %zu bytes, %s sink", fDhw->edid.manufacturer,
			    fDhw->edid.monitor_name, fDhw->edid_len,
			    fDhw->edid.cea.hdmi_vsdb ? "HDMI" : "DVI");
	} else {
		fDhw->edid_valid = false;
	}
	buildModes();
}

void
PG106FB::buildModes(void)
{
	fModeCount = 0;
	if (fDhw->connected && fDhw->edid_valid)
		fModeCount = (unsigned)pg106_dhw_modes(fDhw, fModes, PG106FB_MAX_MODES);
	if (!fModeCount) {
		// No EDID (or nothing plugged in): offer CTA VIC 16 so the mode
		// list is never empty; offline anyway until HPD.
		const struct pg106_edid_timing *t = pg106_edid_cea_timing(16);
		struct pg106_dhw_mode *m = &fModes[0];
		memset(m, 0, sizeof(*m));
		m->m.pixel_clock_khz = t->pixel_clock_khz;
		m->m.hdisplay = t->hactive;
		m->m.hsync_start = (uint16_t)(t->hactive + t->hsync_offset);
		m->m.hsync_end = (uint16_t)(m->m.hsync_start + t->hsync_width);
		m->m.htotal = t->htotal;
		m->m.vdisplay = t->vactive;
		m->m.vsync_start = (uint16_t)(t->vactive + t->vsync_offset);
		m->m.vsync_end = (uint16_t)(m->m.vsync_start + t->vsync_width);
		m->m.vtotal = t->vtotal;
		m->m.hsync_positive = m->m.vsync_positive = true;
		m->m.bpc = 8;
		m->refresh_mhz = 60000;
		m->vic = 16;
		m->preferred = true;
		fModeCount = 1;
	}
	for (unsigned i = 0; i < fModeCount; i++)
		LOG("mode %u: %ux%u @ %u.%03u Hz, %u kHz%s%s", i + 1, fModes[i].m.hdisplay,
		    fModes[i].m.vdisplay, fModes[i].refresh_mhz / 1000,
		    fModes[i].refresh_mhz % 1000, fModes[i].m.pixel_clock_khz,
		    fModes[i].vic ? " (CTA)" : "", fModes[i].preferred ? " preferred" : "");
}

bool
PG106FB::start(IOService *provider)
{
	const OSSymbol *fn;
	const uint8_t *vb = nullptr;
	uint32_t vblen = 0, v = 1;
	IOReturn r;
	int err;

	fProvider = provider;
	if (PE_parse_boot_argn("pg106fb_accel", &v, sizeof(v)))
		fAccel = v != 0;

	// 1. The display host table from PG106 (disp_host.h).
	fn = OSSymbol::withCString(PG106_DISP_HOST_FUNCTION);
	r = fn ? provider->callPlatformFunction(fn, false, &fHost,
					       (void *)(uintptr_t)PG106_DISP_HOST_ABI,
					       nullptr, nullptr) : kIOReturnNoMemory;
	OSSafeReleaseNULL(fn);
	if (r != kIOReturnSuccess || fHost.abi != PG106_DISP_HOST_ABI) {
		LOG("PG106 does not provide %s (0x%x, abi 0x%x) — not attaching",
		    PG106_DISP_HOST_FUNCTION, r, fHost.abi);
		return false;
	}

	// 2. Keep the GPU booted and powered (blocks for the bring-up).
	err = fHost.session_open ? fHost.session_open(fHost.user) : -ENODEV;
	if (err) {
		LOG("GPU bring-up failed (%d) — not attaching", err);
		return false;
	}
	fSession = true;

	// 3. Display engine (pascal/core disp_hw).
	if (fHost.vbios && fHost.vbios(fHost.user, &vb, &vblen))
		vb = nullptr, vblen = 0;
	fDhw = static_cast<struct pg106_dhw *>(IOMallocZero(sizeof(*fDhw)));
	if (!fDhw)
		goto fail;
	err = pg106_dhw_ctor(fDhw, &fHost, vb, vblen,
			     vb ? nullptr : &pg106_dhw_helios300_outp);
	if (err) {
		LOG("no HDMI output in the VBIOS DCB (%d) — not attaching", err);
		goto fail;
	}
	LOG("output: DCB %u TMDS OR 0x%x link %u, connector 0x%02x, CCB %u port %u pad %d, "
	    "HPD GPIO %u%s", fDhw->outp.dcb.index, fDhw->outp.dcb.or_mask,
	    fDhw->outp.dcb.sorconf_link, fDhw->outp.conn_type, fDhw->outp.dcb.i2c_index,
	    fDhw->outp.i2c.drive, fDhw->outp.i2c.share == 0xff ? -1 : fDhw->outp.i2c.share,
	    fDhw->outp.hpd_line, vb ? "" : " (built-in Helios table: no VBIOS)");
	fDhw->hotplug = cHotplug;
	fDhw->vblank = cVblank;
	fDhw->flip_done = cFlip;
	fDhw->cb_ctx = this;
	err = gated([](PG106FB *s, void *) -> int {
		int e = pg106_dhw_init(s->fDhw);
		s->fInited = e == 0;
		if (!e)
			s->reprobe();
		return e;
	});
	if (err) {
		LOG("display engine init failed: %d (chan errors %u)", err,
		    fDhw->st.chan_errors);
		goto fail;
	}
	LOG("display engine up: heads 0x%x SORs 0x%x (HDA 0x%x), inst 0x%llx, scanout 0x%llx "
	    "(%llu MiB), %s", fDhw->heads_mask, fDhw->sors_mask, fDhw->sor_hda_mask,
	    (unsigned long long)fDhw->inst_pa, (unsigned long long)fDhw->fb_pa,
	    (unsigned long long)(fDhw->fb_size >> 20),
	    fDhw->connected ? "monitor connected" : "nothing connected");

	// 4. Interrupts and power events.
	if (fHost.intr_set &&
	    !fHost.intr_set(fHost.user, PG106_PMC_INTR_DISP | 0x00200000u /* GPIO */,
			    cIntr, this))
		fIntrSet = true;
	else
		LOG("no interrupt routing from PG106: hotplug/vblank/flip-done will not fire");
	if (fHost.power_notify && !fHost.power_notify(fHost.user, cPower, this))
		fPowerSet = true;

	fConnectCall = thread_call_allocate(sConnectCall, this);
	fShadowCall = thread_call_allocate(sShadowCall, this);
	if (!fConnectCall || !fShadowCall)
		goto fail;

	// 5. Aperture: BAR1 window onto the scanout, else the shadow copy.
	for (fPCI = provider; fPCI && !fPCI->metaCast("IOPCIDevice"); fPCI = fPCI->getProvider())
		;
	if (!setupAperture())
		goto fail;

	// 6. Pairing with PG106Accel (pg106_kif.h), then IOFramebuffer::start
	// (which ends in registerService()).
	if (fAccel)
		setProperty("PG106FBKifABI", (unsigned long long)PG106_KIF_ABI, 32);
	setProperty("PG106FBAccelerated", fAccel);
	if (!super::start(provider))
		goto fail;
	LOG("started (%s; accelerator pairing %s)",
	    fBar1Mapped ? "BAR1 aperture" : "SHADOW-COPY aperture",
	    fAccel ? "offered" : "off (pg106fb_accel=0)");
	return true;

fail:
	teardown();
	return false;
}

void
PG106FB::stop(IOService *provider)
{
	teardown();
	super::stop(provider);
}

void
PG106FB::teardown(void)
{
	// thread_call_cancel_wait is private KPI: cancel, then wait for any
	// callout already running (fCallsBusy) before freeing.
	fStopping = true;
	if (fShadowCall)
		thread_call_cancel(fShadowCall);
	if (fConnectCall)
		thread_call_cancel(fConnectCall);
	for (unsigned i = 0; fCallsBusy && i < 5000; i++)
		IOSleep(1);
	if (fShadowCall) {
		thread_call_free(fShadowCall);
		fShadowCall = nullptr;
	}
	if (fConnectCall) {
		thread_call_free(fConnectCall);
		fConnectCall = nullptr;
	}
	if (fIntrSet) {
		fHost.intr_set(fHost.user, PG106_PMC_INTR_DISP | 0x00200000u, nullptr, nullptr);
		fIntrSet = false;
	}
	if (fPowerSet) {
		fHost.power_notify(fHost.user, nullptr, nullptr);
		fPowerSet = false;
	}
	if (fDhw && fInited)
		gated([](PG106FB *s, void *) -> int {
			pg106_dhw_fini(s->fDhw, true);
			s->fInited = false;
			return 0;
		});
	releaseAperture();
	if (fDhw) {
		IOFree(fDhw, sizeof(*fDhw));
		fDhw = nullptr;
	}
	if (fSession) {
		fHost.session_close(fHost.user);
		fSession = false;
	}
}

void
PG106FB::free(void)
{
	releaseAperture();
	if (fDhw) {
		IOFree(fDhw, sizeof(*fDhw));
		fDhw = nullptr;
	}
	super::free();
}

// ---------------------------------------------------------------------------
// aperture
// ---------------------------------------------------------------------------

bool
PG106FB::setupAperture(void)
{
	uint64_t off = 0;
	int err = -ENOSYS;

	if (fPCI)
		fBar1 = fPCI->getDeviceMemoryWithIndex(1);
	if (fBar1 && fHost.bar1_map)
		err = fHost.bar1_map(fHost.user, fDhw->fb_pa, fDhw->fb_size, &off);
	if (!err && off + fDhw->fb_size <= fBar1->getLength()) {
		fAperture = IOSubMemoryDescriptor::withSubRange(fBar1, off, fDhw->fb_size,
								 kIODirectionInOut);
		if (fAperture) {
			fBar1Off = off;
			fBar1Mapped = true;
			LOG("aperture: BAR1 + 0x%llx (%llu MiB) -> VRAM 0x%llx",
			    (unsigned long long)off, (unsigned long long)(fDhw->fb_size >> 20),
			    (unsigned long long)fDhw->fb_pa);
			return true;
		}
	}
	if (!err && fHost.bar1_unmap)
		fHost.bar1_unmap(fHost.user, off, fDhw->fb_size);
	// Fallback: WindowServer draws into system memory; a thread call
	// copies it to the VRAM scanout through the host's VRAM writer
	// (PRAMIN) PG106FB_SHADOW_HZ times a second. Slow; for bring-up only.
	fShadow = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut | kIOMemoryKernelUserShared,
							fDhw->fb_size, page_size);
	if (!fShadow) {
		LOG("aperture: no BAR1 (%d) and no shadow buffer", err);
		return false;
	}
	bzero(fShadow->getBytesNoCopy(), fDhw->fb_size);
	fShadow->retain();
	fAperture = fShadow;
	LOG("aperture: BAR1 map unavailable (%d) — SHADOW COPY at %u Hz (slow; bring-up only)",
	    err, PG106FB_SHADOW_HZ);
	return true;
}

// After a re-boot: map the new scanout into BAR1 again. If the BAR1 offset
// moved, the aperture descriptor changes; the connect interrupt that
// follows makes the family re-query getApertureRange (BLOCKERS B16: whether
// WindowServer re-maps it there is a hardware question).
void
PG106FB::remapAperture(void)
{
	uint64_t off = 0;

	if (fShadow || !fBar1 || !fHost.bar1_map)
		return;
	if (fHost.bar1_map(fHost.user, fDhw->fb_pa, fDhw->fb_size, &off)) {
		LOG("aperture: BAR1 re-map after power-on failed; HDMI shows the old frame");
		return;
	}
	fBar1Mapped = true;
	if (off == fBar1Off && fAperture)
		return;
	IOMemoryDescriptor *n = IOSubMemoryDescriptor::withSubRange(fBar1, off, fDhw->fb_size,
								     kIODirectionInOut);
	if (!n)
		return;
	LOG("aperture moved: BAR1 + 0x%llx -> 0x%llx", (unsigned long long)fBar1Off,
	    (unsigned long long)off);
	OSSafeReleaseNULL(fAperture);
	fAperture = n;
	fBar1Off = off;
}

void
PG106FB::releaseAperture(void)
{
	if (fBar1Mapped && fHost.bar1_unmap && fDhw)
		fHost.bar1_unmap(fHost.user, fBar1Off, fDhw->fb_size);
	fBar1Mapped = false;
	OSSafeReleaseNULL(fAperture);
	OSSafeReleaseNULL(fShadow);
	fBar1 = nullptr; // not retained (owned by the IOPCIDevice)
}

void
PG106FB::shadowCopy(void)
{
	gated([](PG106FB *s, void *) -> int {
		if (!s->fShadow || !s->fInited || s->fGpuOff || s->fDhw->head < 0)
			return 0;
		const uint8_t *src = static_cast<const uint8_t *>(s->fShadow->getBytesNoCopy());
		uint64_t bytes = (uint64_t)s->fPitch * s->fDhw->fb_h;
		for (uint64_t o = 0; o < bytes; o += 0x10000) {
			uint32_t n = (uint32_t)((bytes - o) < 0x10000 ? (bytes - o) : 0x10000);
			s->fHost.vram_write(s->fHost.user, s->fDhw->fb_pa + o, src + o, n);
		}
		return 0;
	});
}

void
PG106FB::sShadowCall(thread_call_param_t p0, thread_call_param_t)
{
	PG106FB *self = static_cast<PG106FB *>(p0);
	uint64_t deadline;

	OSIncrementAtomic(&self->fCallsBusy);
	if (!self->fStopping) {
		self->shadowCopy();
		if (self->fShadowCall && self->fShadow) {
			clock_interval_to_deadline(1000 / PG106FB_SHADOW_HZ, kMillisecondScale,
						   &deadline);
			thread_call_enter_delayed(self->fShadowCall, deadline);
		}
	}
	OSDecrementAtomic(&self->fCallsBusy);
}

IODeviceMemory *
PG106FB::getApertureRange(IOPixelAperture aperture)
{
	if (aperture != kIOFBSystemAperture || !fAperture)
		return nullptr;
	// The family releases what we return.
	fAperture->retain();
	return reinterpret_cast<IODeviceMemory *>(fAperture);
}

IODeviceMemory *
PG106FB::getVRAMRange(void)
{
	return getApertureRange(kIOFBSystemAperture);
}

// ---------------------------------------------------------------------------
// modes
// ---------------------------------------------------------------------------

IOReturn
PG106FB::enableController(void)
{
	return fInited ? kIOReturnSuccess : kIOReturnNotReady;
}

const char *
PG106FB::getPixelFormats(void)
{
	static const char formats[] = IO32BitDirectPixels "\0";
	return formats;
}

IOItemCount
PG106FB::getDisplayModeCount(void)
{
	return fModeCount;
}

IOReturn
PG106FB::getDisplayModes(IODisplayModeID *ids)
{
	for (unsigned i = 0; i < fModeCount; i++)
		ids[i] = (IODisplayModeID)(i + 1);
	return kIOReturnSuccess;
}

int
PG106FB::modeIndex(IODisplayModeID id) const
{
	return (id >= 1 && (unsigned)id <= fModeCount) ? (int)id - 1 : -1;
}

IOReturn
PG106FB::getInformationForDisplayMode(IODisplayModeID id, IODisplayModeInformation *info)
{
	int i = modeIndex(id);

	if (i < 0 || !info)
		return kIOReturnBadArgument;
	bzero(info, sizeof(*info));
	info->nominalWidth = fModes[i].m.hdisplay;
	info->nominalHeight = fModes[i].m.vdisplay;
	info->refreshRate = (IOFixed1616)(((uint64_t)fModes[i].refresh_mhz << 16) / 1000);
	info->maxDepthIndex = 0;
	info->flags = kDisplayModeValidFlag | kDisplayModeSafeFlag |
		      (fModes[i].preferred ? kDisplayModeDefaultFlag : 0);
	return kIOReturnSuccess;
}

UInt64
PG106FB::getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex)
{
	return 0;
}

IOReturn
PG106FB::getPixelInformation(IODisplayModeID id, IOIndex depth, IOPixelAperture aperture,
			     IOPixelInformation *pi)
{
	int i = modeIndex(id);

	if (i < 0 || depth != 0 || aperture != kIOFBSystemAperture || !pi)
		return kIOReturnBadArgument;
	bzero(pi, sizeof(*pi));
	pi->activeWidth = fModes[i].m.hdisplay;
	pi->activeHeight = fModes[i].m.vdisplay;
	// Base surface pitch: a multiple of 256 (STORAGE pitch >> 8).
	pi->bytesPerRow = (pi->activeWidth * 4 + 255) & ~255u;
	pi->bitsPerPixel = 32;
	pi->pixelType = kIORGBDirectPixels;
	pi->componentCount = 3;
	pi->bitsPerComponent = 8;
	pi->componentMasks[0] = 0x00ff0000;
	pi->componentMasks[1] = 0x0000ff00;
	pi->componentMasks[2] = 0x000000ff;
	strlcpy(pi->pixelFormat, IO32BitDirectPixels, sizeof(pi->pixelFormat));
	return kIOReturnSuccess;
}

IOReturn
PG106FB::getCurrentDisplayMode(IODisplayModeID *id, IOIndex *depth)
{
	if (id)
		*id = fCurMode ? fCurMode : 1;
	if (depth)
		*depth = 0;
	return kIOReturnSuccess;
}

IOReturn
PG106FB::getStartupDisplayMode(IODisplayModeID *id, IOIndex *depth)
{
	if (id) {
		*id = 1;
		for (unsigned i = 0; i < fModeCount; i++)
			if (fModes[i].preferred) {
				*id = (IODisplayModeID)(i + 1);
				break;
			}
	}
	if (depth)
		*depth = 0;
	return kIOReturnSuccess;
}

struct modeset_arg {
	int index;
	int err;
};

int
PG106FB::gModeset(PG106FB *s, void *a)
{
	struct modeset_arg *m = static_cast<struct modeset_arg *>(a);

	if (s->fGpuOff || !s->fInited)
		return m->err = -ENODEV;
	if (m->index < 0 || !s->fDisplayOn)
		return m->err = pg106_dhw_modeset(s->fDhw, nullptr, 0);
	uint32_t pitch = ((uint32_t)s->fModes[m->index].m.hdisplay * 4 + 255) & ~255u;
	m->err = pg106_dhw_modeset(s->fDhw, &s->fModes[m->index].m, pitch);
	if (!m->err) {
		s->fPitch = pitch;
		if (s->fVbl.proc)
			pg106_dhw_vblank_enable(s->fDhw, true);
	}
	return m->err;
}

IOReturn
PG106FB::setDisplayMode(IODisplayModeID id, IOIndex depth)
{
	struct modeset_arg a = { modeIndex(id), 0 };

	if (a.index < 0 || depth != 0)
		return kIOReturnBadArgument;
	if (!fDhw->connected) {	// remember it; light the head at hotplug
		fCurMode = id;
		return kIOReturnSuccess;
	}
	gated(gModeset, &a);
	LOG("setDisplayMode %d: %ux%u -> %d (supervisors %u/%u/%u, chan errors %u, IED %u run "
	    "%u missing %u failed, VPLL %u ok %u failed)", (int)id, fModes[a.index].m.hdisplay,
	    fModes[a.index].m.vdisplay, a.err, fDhw->st.supv[0], fDhw->st.supv[1],
	    fDhw->st.supv[2], fDhw->st.chan_errors, fDhw->st.ied_run, fDhw->st.ied_missing,
	    fDhw->st.ied_fail, fDhw->st.pll_set, fDhw->st.pll_fail);
	if (fDhw->st.last_init_error[0])
		LOG("  devinit: script 0x%x: %s", fDhw->st.last_init_script,
		    fDhw->st.last_init_error);
	logDiag(a.index);
	if (a.err)
		return ioret(a.err);
	fCurMode = id;
	fGeneration++;
	if (fShadow && fShadowCall)
		thread_call_enter(fShadowCall);
	return kIOReturnSuccess;
}

// Everything needed to diagnose a dark HDMI screen from the kernel log
// alone (no Linux trace exists with a monitor attached: BLOCKERS B16).
void
PG106FB::logDiag(int index)
{
	const struct pg106_dhw_diag *g = &fDhw->diag;
	const struct pg106_disp_mode *m = &fModes[index].m;

	LOG("modeset #%u: %ux%u pclk %u kHz h %u/%u/%u v %u/%u/%u %c%c VIC %u, SOR %d, HDMI %d, "
	    "audio %d, result %d, notifier %08x", g->seq, m->hdisplay, m->vdisplay,
	    m->pixel_clock_khz, m->hsync_start, m->hsync_end, m->htotal, m->vsync_start,
	    m->vsync_end, m->vtotal, m->hsync_positive ? '+' : '-',
	    m->vsync_positive ? '+' : '-', fModes[index].vic, fDhw->sor, fDhw->hdmi_on,
	    fDhw->audio, g->result, g->ntfy);
	for (unsigned i = 0; i < g->nstages; i++)
		LOG("  supervisor %u: head0 mask %08x", g->stages[i], g->stage_mask[i]);
	LOG("  super1 ASY head0: total %ux%u hz %u depth %u | ARM: total %ux%u hz %u", g->asy.htotal,
	    g->asy.vtotal, g->asy.hz, g->asy.depth, g->arm.htotal, g->arm.vtotal, g->arm.hz);
	LOG("  super1 SOR ASY proto_evo %u proto %d link %d head %x | ARM proto_evo %u head %x",
	    g->sor_asy.proto_evo, g->sor_asy.proto, g->sor_asy.link, g->sor_asy.head,
	    g->sor_arm.proto_evo, g->sor_arm.head);
	LOG("  super2.1 VPLL type %x %u kHz -> %d, %u writes:", g->pll_type, g->pll_khz,
	    g->pll_ret, g->pll_nw);
	for (unsigned i = 0; i < g->pll_nw; i++)
		LOG("    %06x = %08x", g->pll_w[i][0], g->pll_w[i][1]);
	for (unsigned i = 0; i < g->ied_n; i++)
		LOG("  IED %s%u script 0x%04x @%u kHz -> %d (%u MMIO writes)",
		    g->ied[i].on ? "OnInt" : "OffInt", g->ied[i].on ? g->ied[i].id + 2 : g->ied[i].id,
		    g->ied[i].script, g->ied[i].khz, g->ied[i].ret, g->ied[i].mmio);
	LOG("  super2.2 SOR clock %08x (speed %u div %u/%u) rgclk %08x", g->sor_clock,
	    (g->sor_clock >> 18) & 0x1f, (g->sor_clock >> 8) & 7, g->sor_clock & 7, g->rgclk);
	LOG("  ARM raster %08x pclk %u head ctrl %08x SOR ctrl %08x | route %08x HDMI ctrl %08x "
	    "SOR pwr %08x/%08x scdc %08x", g->arm_raster, g->arm_pclk, g->arm_head_ctrl,
	    g->arm_sor_ctrl, g->route, g->hdmi_ctrl, g->sor_pwr, g->sor_pwr_stat, g->sor_hdmi_scdc);
}

IOReturn
PG106FB::getTimingInfoForDisplayMode(IODisplayModeID id, IOTimingInformation *info)
{
	int i = modeIndex(id);

	if (i < 0 || !info)
		return kIOReturnBadArgument;
	const struct pg106_disp_mode *m = &fModes[i].m;
	bzero(info, sizeof(*info));
	info->appleTimingID = kIOTimingIDInvalid;
	info->flags = kIODetailedTimingValid;
	IODetailedTimingInformationV2 *t = &info->detailedInfo.v2;
	t->pixelClock = (UInt64)m->pixel_clock_khz * 1000;
	t->minPixelClock = t->pixelClock;
	t->maxPixelClock = t->pixelClock;
	t->horizontalActive = m->hdisplay;
	t->horizontalBlanking = m->htotal - m->hdisplay;
	t->horizontalSyncOffset = m->hsync_start - m->hdisplay;
	t->horizontalSyncPulseWidth = m->hsync_end - m->hsync_start;
	t->verticalActive = m->vdisplay;
	t->verticalBlanking = m->vtotal - m->vdisplay;
	t->verticalSyncOffset = m->vsync_start - m->vdisplay;
	t->verticalSyncPulseWidth = m->vsync_end - m->vsync_start;
	t->horizontalSyncConfig = m->hsync_positive ? kIOSyncPositivePolarity : 0;
	t->verticalSyncConfig = m->vsync_positive ? kIOSyncPositivePolarity : 0;
	t->numLinks = 1;
	return kIOReturnSuccess;
}

// ---------------------------------------------------------------------------
// attributes / connection
// ---------------------------------------------------------------------------

IOItemCount
PG106FB::getConnectionCount(void)
{
	return 1;
}

int
PG106FB::gPowerDisplay(PG106FB *s, void *a)
{
	struct modeset_arg m = { s->fDisplayOn ? s->modeIndex(s->fCurMode) : -1, 0 };
	(void)a;
	if (!s->fDhw->connected || (s->fDisplayOn && m.index < 0))
		return 0;
	return gModeset(s, &m);
}

IOReturn
PG106FB::getAttribute(IOSelect attribute, uintptr_t *value)
{
	switch (attribute) {
	case kIOHardwareCursorAttribute:
		if (value)
			*value = 1;	// kIOFBHWCursorSupported
		return kIOReturnSuccess;
	default:
		return super::getAttribute(attribute, value);
	}
}

IOReturn
PG106FB::setAttribute(IOSelect attribute, uintptr_t value)
{
	if (attribute == kIOPowerAttribute) {
		// Display power (display sleep): head off at 0, back on above.
		bool on = value != 0;
		if (on != fDisplayOn) {
			fDisplayOn = on;
			if (on)
				handleEvent(kIOFBNotifyWillPowerOn);
			else
				handleEvent(kIOFBNotifyWillPowerOff);
			gated(gPowerDisplay);
			if (on)
				handleEvent(kIOFBNotifyDidPowerOn);
			else
				handleEvent(kIOFBNotifyDidPowerOff);
		}
		IOReturn r = super::setAttribute(attribute, value);
		return r == kIOReturnUnsupported ? kIOReturnSuccess : r;
	}
	return super::setAttribute(attribute, value);
}

IOReturn
PG106FB::getAttributeForConnection(IOIndex idx, IOSelect attribute, uintptr_t *value)
{
	if (idx != 0)
		return kIOReturnBadArgument;
	switch (attribute) {
	case kConnectionEnable:
	case kConnectionCheckEnable:
		if (value)
			*value = (fDhw && fDhw->connected && !fGpuOff) ? 1 : 0;
		return kIOReturnSuccess;
	case kConnectionFlags:
	case kConnectionDisplayFlags:
		if (value)
			*value = 0;
		return kIOReturnSuccess;
	case kConnectionSupportsHLDDCSense:
		return (fDhw && fDhw->edid_valid) ? kIOReturnSuccess : kIOReturnUnsupported;
	case kConnectionColorModesSupported:
	case kConnectionColorMode:
		if (value)
			*value = 0x1; // kIODisplayColorModeRGB
		return kIOReturnSuccess;
	case kConnectionColorDepthsSupported:
	case kConnectionControllerDepthsSupported:
	case kConnectionControllerColorDepth:
		if (value)
			*value = 0x2; // kIODisplayRGBColorComponentBits8
		return kIOReturnSuccess;
	case kConnectionSupportsAppleSense:
	case kConnectionSupportsLLDDCSense:
		return kIOReturnUnsupported;
	default:
		return super::getAttributeForConnection(idx, attribute, value);
	}
}

IOReturn
PG106FB::setAttributeForConnection(IOIndex idx, IOSelect attribute, uintptr_t value)
{
	if (idx != 0)
		return kIOReturnBadArgument;
	switch (attribute) {
	case kConnectionProbe:
	case kConnectionChanged:
		gated(gProbe);
		return kIOReturnSuccess;
	case kConnectionPower:
	case kConnectionColorMode:
	case kConnectionColorDepthsSupported:
	case kConnectionControllerColorDepth:
	case kConnectionControllerDepthsSupported:
	case kConnectionColorModesSupported:
	case kConnectionFlags:
	case kConnectionDisplayFlags:
		return kIOReturnSuccess;
	default:
		return super::setAttributeForConnection(idx, attribute, value);
	}
}

bool
PG106FB::hasDDCConnect(IOIndex idx)
{
	return idx == 0 && fDhw && fDhw->edid_valid;
}

IOReturn
PG106FB::getDDCBlock(IOIndex idx, UInt32 block, IOSelect type, IOOptionBits,
		     UInt8 *data, IOByteCount *length)
{
	if (idx != 0 || type != kIODDCBlockTypeEDID || !fDhw || !fDhw->edid_valid ||
	    block == 0 || (size_t)block * 128 > fDhw->edid_len)
		return kIOReturnUnsupported;
	if (!data || !length || *length < 128)
		return kIOReturnBadArgument;
	memcpy(data, fDhw->edid_raw + (block - 1) * 128, 128);
	*length = 128;
	return kIOReturnSuccess;
}

// ---------------------------------------------------------------------------
// interrupts
// ---------------------------------------------------------------------------

IOReturn
PG106FB::registerForInterruptType(IOSelect type, IOFBInterruptProc proc, OSObject *target,
				  void *ref, void **interruptRef)
{
	Irq *irq;

	if (type == kIOFBVBLInterruptType)
		irq = &fVbl;
	else if (type == kIOFBConnectInterruptType)
		irq = &fConnect;
	else
		return kIOReturnUnsupported;
	irq->proc = proc;
	irq->target = target;
	irq->ref = ref;
	irq->enabled = true;
	if (interruptRef)
		*interruptRef = irq;
	if (irq == &fVbl)
		gated([](PG106FB *s, void *) -> int {
			if (s->fInited && s->fDhw->head >= 0)
				pg106_dhw_vblank_enable(s->fDhw, true);
			return 0;
		});
	return kIOReturnSuccess;
}

IOReturn
PG106FB::unregisterInterrupt(void *interruptRef)
{
	Irq *irq = static_cast<Irq *>(interruptRef);

	if (irq != &fVbl && irq != &fConnect)
		return kIOReturnBadArgument;
	irq->enabled = false;
	irq->proc = nullptr;
	return kIOReturnSuccess;
}

IOReturn
PG106FB::setInterruptState(void *interruptRef, UInt32 state)
{
	Irq *irq = static_cast<Irq *>(interruptRef);

	if (irq != &fVbl && irq != &fConnect)
		return kIOReturnBadArgument;
	irq->enabled = state != 0;
	return kIOReturnSuccess;
}

void
PG106FB::onIntr(void)
{
	if (fDhw && fInited && !fGpuOff)
		pg106_dhw_intr(fDhw);
}

void
PG106FB::onVblank(uint64_t ns)
{
	if (fLastVblNs && ns > fLastVblNs)
		fVblDeltaNs = ns - fLastVblNs;
	fLastVblNs = ns;
	// The family's VBL proc is interrupt-safe (it only updates shared
	// memory and signals a semaphore).
	if (fVbl.proc && fVbl.enabled)
		fVbl.proc(fVbl.target, fVbl.ref);
}

void
PG106FB::onHotplug(bool connected)
{
	LOG("hotplug: %s", connected ? "connected" : "disconnected");
	fGeneration++;
	// Re-probe and tell the family off the PG106 gate.
	if (fConnectCall)
		thread_call_enter(fConnectCall);
}

void
PG106FB::sConnectCall(thread_call_param_t p0, thread_call_param_t)
{
	PG106FB *self = static_cast<PG106FB *>(p0);

	OSIncrementAtomic(&self->fCallsBusy);
	if (self->fStopping) {
		OSDecrementAtomic(&self->fCallsBusy);
		return;
	}
	self->gated(gProbe);
	if (self->fDhw->connected && self->fCurMode && self->fDhw->head < 0) {
		struct modeset_arg a = { self->modeIndex(self->fCurMode), 0 };
		if (a.index >= 0)
			self->gated(gModeset, &a);
	}
	if (self->fConnect.proc && self->fConnect.enabled)
		self->fConnect.proc(self->fConnect.target, self->fConnect.ref);
	OSDecrementAtomic(&self->fCallsBusy);
}

// D3cold (PG106 power_notify): the display engine loses everything.
void
PG106FB::onPower(int event)
{
	if (!fDhw)
		return;
	if (event == PG106_DISP_POWER_WILL_OFF) {
		LOG("GPU powering off: display down");
		if (fInited)
			pg106_dhw_fini(fDhw, false);	// aborts a pending flip
		// PG106 rebuilds its VRAM allocator and BAR1 VMM on the next boot:
		// our VRAM objects and the BAR1 mapping die with the power.
		pg106_dhw_vram_lost(fDhw);
		fBar1Mapped = false;
		fInited = false;
		fGpuOff = true;
		fGeneration++;
		return;
	}
	if (event == PG106_DISP_POWER_DID_ON) {
		fGpuOff = false;
		int e = pg106_dhw_init(fDhw);
		fInited = e == 0;
		LOG("GPU powered on + POSTed: display re-init %d, scanout now 0x%llx", e,
		    (unsigned long long)fDhw->fb_pa);
		if (!e)
			remapAperture();
		if (!e) {
			reprobe();
			if (fCurMode && fDhw->connected && fDisplayOn) {
				struct modeset_arg a = { modeIndex(fCurMode), 0 };
				if (a.index >= 0)
					gModeset(this, &a);
				LOG("mode %d restored: %d", (int)fCurMode, a.err);
			}
			if (fCursorVisible) {
				pg106_dhw_cursor_image(fDhw, fCursor);
				pg106_dhw_cursor_show(fDhw, true);
			}
		}
		fGeneration++;
		if (fConnectCall)
			thread_call_enter(fConnectCall);
	}
}

// ---------------------------------------------------------------------------
// cursor (64x64 A8R8G8B8 through the cursor channel)
// ---------------------------------------------------------------------------

struct cursor_arg {
	int op;		// 0 image, 1 state
	SInt32 x, y;
	bool visible;
};

int
PG106FB::gCursor(PG106FB *s, void *a)
{
	struct cursor_arg *c = static_cast<struct cursor_arg *>(a);

	if (!s->fInited || s->fGpuOff)
		return -ENODEV;
	if (c->op == 0)
		return pg106_dhw_cursor_image(s->fDhw, s->fCursor);
	if (c->visible != s->fDhw->curs_on) {
		int e = pg106_dhw_cursor_show(s->fDhw, c->visible);
		if (e)
			return e;
	}
	return c->visible ? pg106_dhw_cursor_move(s->fDhw, c->x, c->y) : 0;
}

IOReturn
PG106FB::setCursorImage(void *cursorImage)
{
	IOHardwareCursorDescriptor desc;
	IOHardwareCursorInfo info;
	static uint32_t conv[64 * 64];
	struct cursor_arg a = { 0, 0, 0, false };

	bzero(&desc, sizeof(desc));
	bzero(&info, sizeof(info));
	desc.majorVersion = kHardwareCursorDescriptorMajorVersion;
	desc.minorVersion = kHardwareCursorDescriptorMinorVersion;
	desc.width = 64;
	desc.height = 64;
	desc.bitDepth = 32;
	info.majorVersion = kHardwareCursorInfoMajorVersion;
	info.minorVersion = kHardwareCursorInfoMinorVersion;
	info.hardwareCursorData = reinterpret_cast<UInt8 *>(conv);
	// cursorImage == NULL is the family's capability probe.
	if (!convertCursorImage(cursorImage, &desc, &info))
		return kIOReturnUnsupported;
	if (!cursorImage)
		return kIOReturnSuccess;
	// The converted rows are cursorWidth (64/32/16) pixels wide; the
	// hardware cursor is always 64x64.
	bzero(fCursor, sizeof(fCursor));
	for (UInt32 y = 0; y < info.cursorHeight && y < 64; y++)
		memcpy(&fCursor[y * 64], &conv[y * info.cursorWidth],
		       (info.cursorWidth < 64 ? info.cursorWidth : 64) * 4);
	return ioret(gated(gCursor, &a));
}

IOReturn
PG106FB::setCursorState(SInt32 x, SInt32 y, bool visible)
{
	struct cursor_arg a = { 1, x, y, visible };

	fCursorVisible = visible;
	return ioret(gated(gCursor, &a));
}

// ---------------------------------------------------------------------------
// flips for PG106Accel (pg106_kif.h)
// ---------------------------------------------------------------------------

void
PG106FB::onFlipDone(int status, uint64_t ns)
{
	IOService *client = fFlipClient;
	const OSSymbol *fn;
	struct pg106accel_kif_flip_done d;

	if (fFlipHome)
		fFlipHome = false;
	if (!client)
		return;	// our own HOME flip (no client)
	fFlipClient = nullptr;
	bzero(&d, sizeof(d));
	d.abi = PG106_KIF_ABI;
	d.size = sizeof(d);
	d.cookie = fFlipCookie;
	d.status = (uint32_t)(status ? ioret(status) : kIOReturnSuccess);
	d.vblank_ns = ns;
	fn = OSSymbol::withCString(PG106ACCEL_KIF_FN_FLIP_DONE);
	if (fn) {
		client->callPlatformFunction(fn, false, &d, (void *)(uintptr_t)PG106_KIF_ABI,
					     nullptr, nullptr);
		fn->release();
	}
	client->release();
}

int
PG106FB::gInfo(PG106FB *s, void *a)
{
	struct pg106fb_kif_info *i = static_cast<struct pg106fb_kif_info *>(a);
	int idx = s->modeIndex(s->fCurMode);

	i->head = 0;
	i->online = (s->fInited && !s->fGpuOff && s->fDhw->head >= 0) ? 1 : 0;
	i->width = s->fDhw->head >= 0 ? s->fDhw->fb_w : 0;
	i->height = s->fDhw->head >= 0 ? s->fDhw->fb_h : 0;
	i->pitch = s->fPitch;
	i->format = PG106_KIF_FMT_BGRA;
	i->vram_offset = s->fDhw->fb_pa;
	i->vram_bytes = s->fDhw->fb_size;
	i->bar1_phys = (s->fBar1Mapped && s->fBar1) ?
		       s->fBar1->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone) + s->fBar1Off : 0;
	i->kva = 0;
	i->refresh_us = (idx >= 0 && s->fModes[idx].refresh_mhz) ?
			(uint32_t)(1000000000ull / s->fModes[idx].refresh_mhz) : 0;
	i->generation = s->fGeneration;
	return 0;
}

IOReturn
PG106FB::kifInfo(struct pg106fb_kif_info *info)
{
	gated(gInfo, info);
	info->abi = PG106_KIF_ABI;
	return kIOReturnSuccess;
}

int
PG106FB::gFlip(PG106FB *s, void *a)
{
	struct pg106fb_kif_flip *f = static_cast<struct pg106fb_kif_flip *>(a);
	bool home = f->flags & PG106FB_KIF_FLIP_HOME;
	int e;

	if (!s->fInited || s->fGpuOff || s->fDhw->head < 0 || f->generation != s->fGeneration)
		return -ENODEV;
	if (s->fDhw->flip_pending)
		return -EBUSY;
	if (!home && (f->format != PG106_KIF_FMT_BGRA || f->width != s->fDhw->fb_w ||
		      f->height != s->fDhw->fb_h || (f->vram_offset & 0xfff)))
		return -EINVAL;
	e = pg106_dhw_flip(s->fDhw, home ? s->fDhw->fb_pa : f->vram_offset,
			   home ? s->fPitch : f->pitch,
			   !(f->flags & PG106FB_KIF_FLIP_ASYNC));
	if (e)
		return e;
	s->fFlipClient = static_cast<IOService *>(f->client);
	if (s->fFlipClient)
		s->fFlipClient->retain();
	s->fFlipCookie = f->cookie;
	s->fFlipHome = home;
	return 0;
}

IOReturn
PG106FB::kifFlip(struct pg106fb_kif_flip *flip)
{
	int e = gated(gFlip, flip);
	flip->abi = PG106_KIF_ABI;
	return ioret(e);
}

IOReturn
PG106FB::callPlatformFunction(const OSSymbol *name, bool wait, void *p1, void *p2, void *p3,
			      void *p4)
{
	if (name && (name->isEqualTo(PG106FB_KIF_FN_INFO) || name->isEqualTo(PG106FB_KIF_FN_FLIP))) {
		uint32_t theirs = (uint32_t)(uintptr_t)p2;
		uint32_t *hdr = static_cast<uint32_t *>(p1);

		if (!fAccel)
			return kIOReturnUnsupported;
		if (!hdr || !PG106_KIF_ABI_ACCEPTS(PG106_KIF_ABI, theirs))
			return kIOReturnUnsupported;
		if (name->isEqualTo(PG106FB_KIF_FN_INFO)) {
			struct pg106fb_kif_info tmp;
			bzero(&tmp, sizeof(tmp));
			IOReturn r = kifInfo(&tmp);
			uint32_t n = hdr[1] < sizeof(tmp) ? hdr[1] : (uint32_t)sizeof(tmp);
			tmp.size = n;
			memcpy(p1, &tmp, n);
			return r;
		}
		if (hdr[1] < sizeof(struct pg106fb_kif_flip))
			return kIOReturnBadArgument;
		return kifFlip(static_cast<struct pg106fb_kif_flip *>(p1));
	}
	return super::callPlatformFunction(name, wait, p1, p2, p3, p4);
}
