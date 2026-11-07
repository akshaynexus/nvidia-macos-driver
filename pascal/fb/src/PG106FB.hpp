// SPDX-License-Identifier: GPL-2.0-only
// pascal/fb/src/PG106FB.hpp — IOFramebuffer for the GP106's HDMI head.
//
// Provider: the PG106 service (pascal/kext), whose provider is the GP106
// IOPCIDevice. PG106FB reaches the GPU only through the disp_host ops
// table PG106 hands out (pascal/core/include/pascal/disp_host.h) and drives
// the display engine with pascal/core disp_hw (compiled into this kext).
// PG106Accel pairs with it through pg106_kif.h (MIT,
// pascal/bridge/include) — "PG106FBKifABI", INFO and FLIP — and gets
// FLIP_DONE back on its own service. No code is shared with PG106 or
// PG106Accel; every call between kexts is a virtual IOService method.
//
// It never touches the internal panel: the Helios panel is wired to the
// Intel iGPU, the dGPU's DCB has a single TMDS output (HDMI), and this
// class only exists as a child of PG106.
//
// API references: MacOSX26.5.sdk Kernel.framework IOKit/graphics/
// IOFramebuffer.h + IOGraphicsTypes.h. Behaviour of the IOGraphics family
// (start() ends in registerService(); updateOnline() asks
// kConnectionCheckEnable; the cursor probe is setCursorImage(NULL) through
// convertCursorImage, whose output rows may shrink to 32/16 pixels) was
// read in apple-oss-distributions/IOGraphics IOFramebuffer.cpp; no code
// from it or from this repo's PolyForm-NC NVRMFB is used here.

#ifndef PG106FB_HPP
#define PG106FB_HPP

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <kern/thread_call.h>

#include <pascal/disp_host.h>
#include <pascal/disp_hw.h>

#include "pg106_kif.h"

#define PG106FB_MAX_MODES 32u

class PG106FB : public IOFramebuffer {
	OSDeclareDefaultStructors(PG106FB)

public:
	// IOService
	IOService *probe(IOService *provider, SInt32 *score) override;
	bool start(IOService *provider) override;
	void stop(IOService *provider) override;
	void free(void) override;
	IOReturn callPlatformFunction(const OSSymbol *functionName,
				      bool waitForFunction, void *param1,
				      void *param2, void *param3,
				      void *param4) override;

	// IOFramebuffer
	IOReturn enableController(void) override;
	IODeviceMemory *getApertureRange(IOPixelAperture aperture) override;
	IODeviceMemory *getVRAMRange(void) override;
	const char *getPixelFormats(void) override;
	IOItemCount getDisplayModeCount(void) override;
	IOReturn getDisplayModes(IODisplayModeID *allDisplayModes) override;
	IOReturn getInformationForDisplayMode(IODisplayModeID mode,
					      IODisplayModeInformation *info) override;
	UInt64 getPixelFormatsForDisplayMode(IODisplayModeID mode,
					     IOIndex depth) override;
	IOReturn getPixelInformation(IODisplayModeID mode, IOIndex depth,
				     IOPixelAperture aperture,
				     IOPixelInformation *info) override;
	IOReturn getCurrentDisplayMode(IODisplayModeID *mode,
				       IOIndex *depth) override;
	IOReturn setDisplayMode(IODisplayModeID mode, IOIndex depth) override;
	IOReturn getTimingInfoForDisplayMode(IODisplayModeID mode,
					     IOTimingInformation *info) override;
	IOReturn getStartupDisplayMode(IODisplayModeID *mode,
				       IOIndex *depth) override;
	IOItemCount getConnectionCount(void) override;
	IOReturn getAttribute(IOSelect attribute, uintptr_t *value) override;
	IOReturn setAttribute(IOSelect attribute, uintptr_t value) override;
	IOReturn getAttributeForConnection(IOIndex connectIndex,
					   IOSelect attribute,
					   uintptr_t *value) override;
	IOReturn setAttributeForConnection(IOIndex connectIndex,
					   IOSelect attribute,
					   uintptr_t value) override;
	bool hasDDCConnect(IOIndex connectIndex) override;
	IOReturn getDDCBlock(IOIndex connectIndex, UInt32 blockNumber,
			     IOSelect blockType, IOOptionBits options,
			     UInt8 *data, IOByteCount *length) override;
	IOReturn registerForInterruptType(IOSelect interruptType,
					  IOFBInterruptProc proc,
					  OSObject *target, void *ref,
					  void **interruptRef) override;
	IOReturn unregisterInterrupt(void *interruptRef) override;
	IOReturn setInterruptState(void *interruptRef, UInt32 state) override;
	IOReturn setCursorImage(void *cursorImage) override;
	IOReturn setCursorState(SInt32 x, SInt32 y, bool visible) override;

	// Called (as C callbacks) from PG106's work loop, gate held.
	void onIntr(void);
	void onPower(int event);
	void onHotplug(bool connected);
	void onVblank(uint64_t ns);
	void onFlipDone(int status, uint64_t ns);

private:
	struct Irq {
		IOFBInterruptProc proc;
		OSObject *target;
		void *ref;
		bool enabled;
	};

	// gate helpers: run `fn` on PG106's gate
	typedef int (*GatedFn)(PG106FB *self, void *arg);
	int gated(GatedFn fn, void *arg = nullptr);
	static int sGateTrampoline(void *ctx);

	static int gProbe(PG106FB *self, void *arg);
	static int gModeset(PG106FB *self, void *arg);
	static int gCursor(PG106FB *self, void *arg);
	static int gFlip(PG106FB *self, void *arg);
	static int gInfo(PG106FB *self, void *arg);
	static int gPowerDisplay(PG106FB *self, void *arg);

	void teardown(void);
	void reprobe(void);             // gate held
	void buildModes(void);          // gate held
	int modeIndex(IODisplayModeID id) const;
	bool setupAperture(void);
	void releaseAperture(void);
	void remapAperture(void);
	void shadowCopy(void);
	static void sConnectCall(thread_call_param_t p0, thread_call_param_t p1);
	static void sShadowCall(thread_call_param_t p0, thread_call_param_t p1);
	IOReturn kifInfo(struct pg106fb_kif_info *info);
	IOReturn kifFlip(struct pg106fb_kif_flip *flip);

	IOService *fProvider = nullptr;
	IOService *fPCI = nullptr;
	struct pg106_disp_host fHost = {};
	struct pg106_dhw *fDhw = nullptr;
	bool fSession = false, fInited = false, fIntrSet = false, fPowerSet = false;
	bool fAccel = true;         // boot-arg pg106fb_accel=0 clears
	bool fGpuOff = false;       // D3cold between WILL_OFF and DID_ON
	bool fDisplayOn = true;     // kIOPowerAttribute
	uint32_t fGeneration = 1;

	struct pg106_dhw_mode fModes[PG106FB_MAX_MODES];
	unsigned fModeCount = 0;
	IODisplayModeID fCurMode = 0;   // 0 = none set
	uint32_t fPitch = 0;

	IOMemoryDescriptor *fBar1 = nullptr;      // BAR1 device memory
	IOMemoryDescriptor *fAperture = nullptr;  // subrange of BAR1 or shadow
	IOBufferMemoryDescriptor *fShadow = nullptr;
	uint64_t fBar1Off = 0;
	bool fBar1Mapped = false;

	Irq fVbl = {}, fConnect = {};
	thread_call_t fConnectCall = nullptr, fShadowCall = nullptr;
	volatile SInt32 fCallsBusy = 0;
	volatile bool fStopping = false;
	uint64_t fLastVblNs = 0, fVblDeltaNs = 16666667;

	uint32_t fCursor[64 * 64] = {};
	bool fCursorVisible = false;

	// kif flip in flight (PG106Accel)
	IOService *fFlipClient = nullptr;
	uint64_t fFlipCookie = 0;
	bool fFlipHome = false;
};

#endif // PG106FB_HPP
