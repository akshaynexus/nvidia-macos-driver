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
#include <IOKit/pci/IOPCIDevice.h>
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
		else {
			const struct pg106_edid *e = &fDhw->edid;
			LOG("EDID: %s \"%s\" %zu bytes, %s sink", e->manufacturer, e->monitor_name,
			    fDhw->edid_len, e->cea.hdmi_vsdb ? "HDMI" : "DVI");
			LOG("  HDMI VSDB Max_TMDS %u kHz | HF %s rate %u kHz SCDC %d LTE_340 %d | "
			    "4:2:0-only VICs %u | DisplayID %u timings | audio: %u SADs%s -> TMDS limit "
			    "%u kHz%s, VPLL max %u kHz", e->cea.hdmi_max_tmds_khz,
			    e->cea.hf_vsdb ? "yes" : "no", e->cea.max_tmds_khz, e->cea.scdc_present,
			    e->cea.lte_340_scramble, e->cea.y420_vic_count, e->displayid.dtd_count,
			    e->cea.sad_count, e->cea.basic_audio ? " + basic" : "",
			    pg106_dhw_tmds_limit_khz(fDhw),
			    pg106_dhw_sink_scrambling(fDhw) ? " (HDMI 2.0 scrambling)" : "",
			    fDhw->vpll_max_khz);
		}
	} else {
		fDhw->edid_valid = false;
		clearAudio();
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
		LOG("mode %u: %ux%u @ %u.%03u Hz, %u kHz%s%s%s", i + 1, fModes[i].m.hdisplay,
		    fModes[i].m.vdisplay, fModes[i].refresh_mhz / 1000,
		    fModes[i].refresh_mhz % 1000, fModes[i].m.pixel_clock_khz,
		    fModes[i].vic ? " (CTA)" : "",
		    fModes[i].m.pixel_clock_khz > PG106_DHW_TMDS_LEGACY_KHZ ? " HDMI2.0-scrambled" : "",
		    fModes[i].preferred ? " preferred" : "");
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
	v = 1;
	if (PE_parse_boot_argn("pg106fb_hda", &v, sizeof(v)))
		fHdaEnable = v != 0;

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
	fHdaRescanCall = thread_call_allocate(sHdaRescanCall, this);
	if (!fConnectCall || !fShadowCall || !fHdaRescanCall)
		goto fail;

	// 5. Aperture: BAR1 window onto the scanout, else the shadow copy.
	for (fPCI = provider; fPCI && !fPCI->metaCast("IOPCIDevice"); fPCI = fPCI->getProvider())
		;
	if (!setupAperture())
		goto fail;

	// 5b. HDMI audio: codec/pin override, then the HDA PCI function.
	{
		OSData *ci = OSDynamicCast(OSData, getProperty("PG106FBAudioCodecInfo"));
		if (!ci && fPCI)
			ci = OSDynamicCast(OSData, fPCI->getProperty("PG106FBAudioCodecInfo"));
		if (ci && ci->getLength() == 4) {
			memcpy(fCodecInfo, ci->getBytesNoCopy(), 4);
			fCodecOverride = true;
			LOG("audio-codec-info override %02x %02x %02x %02x", fCodecInfo[0],
			    fCodecInfo[1], fCodecInfo[2], fCodecInfo[3]);
		}
	}
	gated([](PG106FB *s, void *) -> int {
		s->hdaFunctionCheck("start");
		return 0;
	});

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
	if (fHdaRescanCall)
		thread_call_cancel(fHdaRescanCall);
	for (unsigned i = 0; fCallsBusy && i < 5000; i++)
		IOSleep(1);
	if (fHdaRescanCall) {
		thread_call_free(fHdaRescanCall);
		fHdaRescanCall = nullptr;
	}
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
		      (fModes[i].preferred ? kDisplayModeDefaultFlag | kDisplayModeNativeFlag : 0);
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
	if (m->index < 0 || !s->fDisplayOn) {
		m->err = pg106_dhw_modeset(s->fDhw, nullptr, 0);
		s->clearAudio();
		return m->err;
	}
	uint32_t pitch = ((uint32_t)s->fModes[m->index].m.hdisplay * 4 + 255) & ~255u;
	m->err = pg106_dhw_modeset(s->fDhw, &s->fModes[m->index].m, pitch);
	if (!m->err) {
		s->fPitch = pitch;
		if (s->fVbl.proc)
			pg106_dhw_vblank_enable(s->fDhw, true);
	}
	// After the ELD went to the HDA (pg106_dhw_modeset, post-notifier).
	if (!m->err && s->fDhw->audio)
		s->publishAudio();
	else
		s->clearAudio();
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
	logScdc();
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

// HDMI 2.0 link + audio state after a modeset (OWNER-RUNBOOK 4e.0).
void
PG106FB::logScdc(void)
{
	const struct pg106_dhw *d = fDhw;

	if (!d->hdmi_on) {
		LOG("  link: %s", d->head >= 0 ? "TMDS without HDMI packets (DVI sink, or SCDC failed)" :
						 "off");
		return;
	}
	LOG("  HDMI: %u kHz, %s, max_ac %d; SCDC %s (sink version %u, TMDS_Config %02x -> "
	    "Scrambling_Status %02x Status_Flags_0 %02x after %u ms, err %d, re-arms %u)",
	    d->mode.pixel_clock_khz, d->high_speed ? "high speed (1/40, div2, scrambled)" :
	    d->scdc.scrambling ? "scrambled (LTE_340)" : "1.4 TMDS",
	    pg106_dhw_ac_packets(&d->mode), d->scdc.used ? "written" : "not used",
	    d->scdc.sink_version, d->scdc.tmds_config, d->scdc.scrambler_status,
	    d->scdc.status_flags0, d->scdc.status_wait_us / 1000, d->scdc.err, d->scdc.rearms);
	if (d->scdc.used && d->scdc.scrambling && !(d->scdc.scrambler_status & 1))
		LOG("  WARNING: the sink does not report Scrambling_Status — expect no picture");
	LOG("  audio: %s, SOR %d -> HDA codec 0 pin NID %d, ELD %zu bytes%s",
	    d->audio ? "on" : "off", d->sor, 4 + d->sor, d->eld_len,
	    fAudioPublished ? ", AppleHDA pairing published" : "");
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
	case kConnectionEnableAudio:
		// 1 while the HDMI head carries audio (ELD on the HDA).
		if (!fDhw || !fDhw->hdmi_on || !fDhw->eld_len)
			return kIOReturnUnsupported;
		if (value)
			*value = fDhw->audio ? 1 : 0;
		return kIOReturnSuccess;
	case kConnectionAudioStreaming:
		if (!fDhw || !fDhw->hdmi_on || !fDhw->eld_len)
			return kIOReturnUnsupported;
		if (value)
			*value = fAudioStreaming;
		return kIOReturnSuccess;
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
	case kConnectionEnableAudio: {
		// Who sends this on macOS 26 is not known (IOGraphicsTypes.h
		// only defines it); honour it as audio on/off for the head.
		struct audio_arg { bool on; int err; } a = { value != 0, 0 };
		gated([](PG106FB *s, void *p) -> int {
			audio_arg *x = static_cast<audio_arg *>(p);
			x->err = pg106_dhw_audio_enable(s->fDhw, x->on);
			if (!x->err && s->fDhw->audio)
				s->publishAudio();
			else if (!x->err)
				s->clearAudio();
			return 0;
		}, &a);
		LOG("kConnectionEnableAudio %lu -> %d", (unsigned long)value, a.err);
		return ioret(a.err);
	}
	case kConnectionAudioStreaming:
		fAudioStreaming = value;
		LOG("kConnectionAudioStreaming %lu", (unsigned long)value);
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
	} else if (self->fDhw->connected && self->fDhw->head >= 0) {
		// Head still lit across the HPD pulse: a power-cycled HDMI 2.0
		// sink has forgotten TMDS_Config (pg106_dhw_scdc_check).
		self->gated([](PG106FB *s, void *) -> int {
			int r = pg106_dhw_scdc_check(s->fDhw);
			if (r)
				LOG("hotplug: SCDC re-arm %d (TMDS_Config %02x, Scrambling_Status %02x)",
				    r, s->fDhw->scdc.tmds_config, s->fDhw->scdc.scrambler_status);
			return 0;
		});
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
		clearAudio();
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
			hdaFunctionCheck("power-on");	// 0x488 bit 25 is lost in D3cold
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
// HDMI audio
// ---------------------------------------------------------------------------
//
// What AppleHDA needs from a GPU framebuffer to bind a display-audio engine
// to an HDMI port, as far as it is known (DISP-PLAN.md §13): the GPU's HDA
// controller and this framebuffer carry the same "hda-gfx" value (OpenCore
// DeviceProperties, e.g. "onboard-2"), and the framebuffer carries the
// display's codec route: "audio-codec-info" (codec address, function group
// NID, pin NID, 0), "connector-type" (0x800 = HDMI), "port-number",
// "av-signal-type", plus "IODisplayEDID" and the ELD. These property names
// are the interface NVRMFB (kexts/NVRMFB, PolyForm-NC, studied, not copied)
// publishes and its submitter validated on an RTX 5080 Laptop with NMHDA;
// they are re-implemented here from that description only. The codec
// itself learns the ELD and the sink's presence from the GPU (disp_hw's
// gf119 hda_eld after the modeset), which raises the codec's unsolicited
// ELD/presence event that AppleHDA acts on, so the properties are set
// before that point is reached again on the next modeset and kept while
// the head carries audio. NMHDA (kexts/NMHDA) is what makes AppleHDA
// accept the 10de:10f1 function at all and sends the hda-gfx link
// announcement (0xE3FF8000) for the GPU; PG106FB does not do either.
//
// Pin: the NVIDIA HDMI codec (0x10de0084 on GP106) has pins 0x04..0x07;
// SOR n's ELD (0x10ec00 + 0x30 * n) shows on pin 4 + n (sound/hda/codecs/
// hdmi/nvhdmi.c:51-63 pin2port = pin - 4, and nouveau notifies port = SOR
// id, dispnv50/disp.c:747). On the Helios only SOR3 is HDA-capable
// (0x10ebb0 = 0x40010802 in the owner's trace): pin NID 7.

OSObject *
PG106FB::copyHdaGfx(void)
{
	OSObject *v = nullptr;
	IORegistryEntry *bus;
	OSIterator *it;

	if (!fPCI)
		return nullptr;
	v = fPCI->copyProperty("hda-gfx");
	if (OSDynamicCast(OSData, v) || OSDynamicCast(OSString, v))
		return v;
	OSSafeReleaseNULL(v);
	// Else the HDA function's own value (01:00.1, same parent bus).
	bus = fPCI->getParentEntry(gIOServicePlane);
	it = bus ? bus->getChildIterator(gIOServicePlane) : nullptr;
	if (!it)
		return nullptr;
	while (OSObject *o = it->getNextObject()) {
		IORegistryEntry *e = OSDynamicCast(IORegistryEntry, o);
		OSData *vid = e ? OSDynamicCast(OSData, e->getProperty("vendor-id")) : nullptr;
		OSData *cls = e ? OSDynamicCast(OSData, e->getProperty("class-code")) : nullptr;
		uint32_t vv = 0, cc = 0;
		if (!vid || !cls || vid->getLength() < 2 || cls->getLength() < 4)
			continue;
		memcpy(&vv, vid->getBytesNoCopy(), 2);
		memcpy(&cc, cls->getBytesNoCopy(), 4);
		if (vv != 0x10de || (cc & 0xffff00) != 0x040300)
			continue;
		v = e->copyProperty("hda-gfx");
		if (OSDynamicCast(OSData, v) || OSDynamicCast(OSString, v))
			break;
		OSSafeReleaseNULL(v);
	}
	it->release();
	return v;
}

void
PG106FB::publishAudio(void)
{
	const struct pg106_dhw *d = fDhw;
	uint8_t codec[4] = { 0x00, 0x01, (uint8_t)(4 + d->sor), 0x00 };
	const uint32_t conn = 0x00000800u;	// HDMI
	const uint32_t port = 0;		// one external display
	const uint32_t signal = 8;
	OSObject *pair;

	if (!d->audio || !d->eld_len || d->sor < 0) {
		clearAudio();
		return;
	}
	if (fCodecOverride)
		memcpy(codec, fCodecInfo, sizeof(codec));
	setProperty("nvda-eld", (void *)d->eld, (unsigned)d->eld_len);
	setProperty("IODisplayEDID", (void *)d->edid_raw, (unsigned)d->edid_len);
	pair = copyHdaGfx();
	if (!pair) {
		if (!fHdaGfxWarned)
			LOG("audio: no \"hda-gfx\" on the GPU (01:00.0) or its HDA function "
			    "(01:00.1) - set it in OpenCore DeviceProperties (README); AppleHDA "
			    "cannot pair the HDMI port without it");
		fHdaGfxWarned = true;
		removeProperty("hda-gfx");
		removeProperty("audio-codec-info");
		fAudioPublished = false;
		return;
	}
	setProperty("hda-gfx", pair);
	pair->release();
	setProperty("connector-type", (void *)&conn, sizeof(conn));
	setProperty("port-number", (void *)&port, sizeof(port));
	setProperty("av-signal-type", (void *)&signal, sizeof(signal));
	setProperty("audio-codec-info", codec, sizeof(codec));
	if (!fAudioPublished)
		LOG("audio: AppleHDA pairing published (codec %u AFG %u pin NID %u, ELD %zu bytes)",
		    codec[0], codec[1], codec[2], d->eld_len);
	fAudioPublished = true;
}

void
PG106FB::clearAudio(void)
{
	if (!fAudioPublished && !getProperty("nvda-eld"))
		return;
	removeProperty("nvda-eld");
	removeProperty("hda-gfx");
	removeProperty("connector-type");
	removeProperty("port-number");
	removeProperty("av-signal-type");
	removeProperty("audio-codec-info");
	removeProperty("IODisplayEDID");
	if (fAudioPublished)
		LOG("audio: AppleHDA pairing removed");
	fAudioPublished = false;
}

// Gate held. The Helios firmware enables the HDA function only when an HDMI
// cable is in at its _ON/_PS0 (SSDT11 MLTF), and D3cold clears it; Linux
// sets it unconditionally (quirk_nvidia_hda, also at resume).
void
PG106FB::hdaFunctionCheck(const char *why)
{
	bool was;

	if (!fHdaEnable || !fDhw)
		return;
	was = pg106_dhw_hda_function(fDhw, true);
	if (was)
		return;
	LOG("HDA function (01:00.1) was hidden at %s (0x488 bit 25 clear): enabled, "
	    "rescanning the root port", why);
	if (fHdaRescanCall)
		thread_call_enter(fHdaRescanCall);
}

// The function just appeared: have IOPCIFamily scan the GPU's bus again
// (the call IOPCIFamily itself makes on a link change, IOPCIBridge.cpp
// fBridgeDevice->kernelRequestProbe(Done | NeedsScan)). Off the gate: the
// configurator may block.
void
PG106FB::sHdaRescanCall(thread_call_param_t p0, thread_call_param_t)
{
	PG106FB *self = static_cast<PG106FB *>(p0);
	IORegistryEntry *bridge;
	IOService *port = nullptr;
	IOReturn r = kIOReturnNotFound;

	OSIncrementAtomic(&self->fCallsBusy);
	if (!self->fStopping && self->fPCI) {
		bridge = self->fPCI->getParentEntry(gIOServicePlane);	// IOPCI2PCIBridge
		if (bridge && bridge->metaCast("IOService"))
			port = static_cast<IOService *>(bridge)->getProvider();	// root port
		if (port && port->metaCast("IOPCIDevice"))
			r = static_cast<IOPCIDevice *>(port)->kernelRequestProbe(
				kIOPCIProbeOptionDone | kIOPCIProbeOptionNeedsScan);
		LOG("HDA function: rescan of %s -> 0x%x", port ? port->getName() : "(no root port)", r);
	}
	OSDecrementAtomic(&self->fCallsBusy);
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
