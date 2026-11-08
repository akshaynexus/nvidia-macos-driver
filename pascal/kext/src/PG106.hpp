// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/src/PG106.hpp — the GP106 kext driver class.
//
// An IOService (IOKit/IOService.h) matched through the wide NVIDIA
// personality in Info.plist, gated in probe() by the GP106 device-id table
// (pg106_device_gate.h). start() identifies the GPU, fetches its VBIOS,
// requests the firmware from the bundle and then brings the GPU up on the
// work loop through pascal/core (devboot, pm, hwexec). The user client
// (PG106UserClient) reaches the GPU only through the methods below, all of
// which run with the command gate held.
//
// Class declaration conventions are libkern's (libkern/c++/OSObject.h,
// OSMetaClass.h).

#ifndef PG106_HPP
#define PG106_HPP

#include <IOKit/IOCommandGate.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSArray.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <kern/thread_call.h>
#include <libkern/OSKextLib.h>

#include <pascal/devboot.h>
#include <pascal/disp_host.h>
#include <pascal/gpu.h>
#include <pascal/gr_ctx.h>
#include <pascal/hwexec.h>
#include <pascal/intr.h>
#include <pascal/kdisp.h>
#include <pascal/pm.h>
#include <pascal/suspend.h>
#include <pascal/vbios_fetch.h>

#include "pg106_dma.h"
#include "pg106_uc_abi.h"

struct pg106_kif_state; // PG106_kif.cpp

#define PG106_FW_MAX 20u
#define PG106_BOOTLOG_MAX 128u

class PG106 : public IOService {
	OSDeclareDefaultStructors(PG106)

public:
	IOService *probe(IOService *provider, SInt32 *score) override;
	bool start(IOService *provider) override;
	void stop(IOService *provider) override;
	void free(void) override;
	IOWorkLoop *getWorkLoop(void) const override;
	// System sleep / wake (IOPM, two power states; README "Sleep and
	// wake"). Sleep quiesces and drops everything on the gate before it
	// returns; wake is acknowledged at once and the bring-up runs on a
	// thread call (PG106Ready stays No until it is done).
	IOReturn setPowerState(unsigned long state, IOService *whatDevice) override;

	// Identity captured at start(); never changes afterwards.
	void pmcSnapshot(struct pg106_uc_pmc *out) const;

	// ---- for PG106UserClient (call with the gate held: runOnGate) ----
	IOCommandGate *gate(void) const { return fGate; }
	// A render session opens: powers up / boots if needed (may take
	// seconds), returns 0 or a negative errno (-ENODEV: bring-up failed).
	int openSession(void);
	void closeSession(void);
	bool gpuReady(void) const;
	// Bumps whenever the GPU's memory is lost (sleep, D3cold, re-boot):
	// a user client opened under an older epoch is stale (device lost).
	uint32_t memEpoch(void) const { return fMemEpoch; }
	// The single hardware render channel (v0): one render client at a
	// time. claimChannel binds `kd`'s client VM to a channel on GR's
	// runlist with its own GR context (the golden image is generated at
	// the first claim after a boot, kept across re-boots); the context
	// objects are mapped into the client VM through pg106_kd_map_vram.
	int claimChannel(const void *owner, struct pg106_kd *kd,
			 const struct pg106_kd_chan *ch);
	void releaseChannel(const void *owner, struct pg106_kd *kd);
	void chanKick(uint32_t put);
	// Device facts for the dispatcher (pg106_kd_env).
	void kdEnvBase(struct pg106_kd_env *env) const;
	uint64_t ptimerNs(void);
	IOPCIDevice *pciDevice(void) const { return fPCI; }
	bool renderUsersAllowed(void) const { return fRenderUsers; }
	void statusSnapshot(struct pg106_uc_status *out) const;

	// Runs fn(owner, a0..a3) with the command gate held.
	IOReturn runOnGate(IOCommandGate::Action fn, void *a0 = nullptr,
			   void *a1 = nullptr, void *a2 = nullptr,
			   void *a3 = nullptr);

	// MMIO bus over BAR0 (public so the static C callbacks can use it).
	uint32_t rd32(uint32_t addr) const;
	void wr32(uint32_t addr, uint32_t val);

	// ---- GPU memory + interrupts (gate held), for PG106_disp.cpp,
	// PG106_kif.cpp and PG106UserClient. Memory exists while the
	// bring-up reached PTOP and the memory layer is up (pg106_gpu).
	struct pg106_gpu *gpu(void) const { return fGpuValid ? fGpu : nullptr; }
	bool memUp(void) const;
	int vramAlloc(uint64_t size, uint64_t align, uint32_t flags, uint64_t *pa);
	void vramFree(uint64_t pa, uint64_t size);
	int vramWrite(uint64_t pa, const void *src, uint32_t len);
	uint32_t vramRd32(uint64_t pa);
	int bar1Map(uint64_t pa, uint64_t size, uint64_t *off, bool tail = false);
	void bar1Unmap(uint64_t off, uint64_t size);
	uint64_t bar1Phys(void) const { return fBar1Phys; }
	uint64_t bar1Len(void) const { return fBar1Len; }
	volatile uint8_t *bar1Kva(void) const { return fBar1; }
	// Route PMC_INTR_0 bits to fn (kept across re-boots); fn NULL drops.
	int intrRoute(uint32_t mask, pg106_intr_fn fn, void *ctx);
	// Upload a client VM's changed tables (VRAM region [base, base+size))
	// and invalidate its PDB.
	int vmFlush(struct pg106_mmu *mmu, uint64_t base, uint64_t size);
	// Sleep (gate dropped) until the next fence interrupt or us passed.
	void fenceWait(uint32_t us);
	void fenceWake(void) { fFenceEv++; fGate->commandWakeup(&fFenceEv); }
	const struct pg106_vbios_blob *vbiosBlob(void) const { return &fVbios; }
	void bar1Write32(uint64_t off, uint32_t v);
	uint32_t bar1Read32(uint64_t off) const;

	// disp_host / kif providers (callPlatformFunction).
	IOReturn callPlatformFunction(const OSSymbol *functionName,
				      bool waitForFunction, void *param1,
				      void *param2, void *param3,
				      void *param4) override;
	// PG106_disp.cpp
	void dispFill(struct pg106_disp_host *h);
	void dispPower(int event);
	pg106_disp_power_fn fDispPowerFn = nullptr;
	void *fDispPowerCtx = nullptr;
	pg106_disp_intr_fn fDispIntrFn = nullptr;
	void *fDispIntrCtx = nullptr;
	uint32_t fDispIntrMask = 0;
	// PG106_kif.cpp
	IOReturn kifCall(const char *name, void *req, uintptr_t abi);
	void kifEvent(void);       // completed seqnos may have advanced
	void kifLost(void);        // GPU going away
	void kifTeardown(void);
	struct pg106_kif_state *fKif = nullptr;

private:
	// start() helpers
	IOReturn identify(void);
	void findAcpi(void);
	void fetchVbios(void);
	void embedFirmware(void);
	void requestFirmware(void);
	void scheduleBoot(void);
	static void sBootCall(thread_call_param_t p0, thread_call_param_t p1);
	void bootEvent(const struct pg106_devboot_event *e);
	static void fwCallback(OSKextRequestTag tag, OSReturn result,
			       const void *data, uint32_t len, void *ctx);
	static IOReturn sFwArrived(OSObject *owner, void *idx, void *res,
				   void *data, void *len);
	void fwArrived(unsigned idx, OSReturn res, const void *data,
		       uint32_t len);
	void readPolicy(void);

	// power / bring-up (gate held)
	void pmPerform(uint8_t action);
	int doBoot(void);
	int doPowerOff(void);
	int doPowerOn(void);
	int doSuspend(void);
	int doResume(void);
	void gpuDrop(void); // everything on the GPU goes (memory epoch bumps)
	int acpiPower(bool on, const char *why);
	static IOReturn sSleep(OSObject *owner, void *, void *, void *, void *);
	static IOReturn sWake(OSObject *owner, void *, void *, void *, void *);
	static void sWakeCall(thread_call_param_t p0, thread_call_param_t p1);
	void wakeDone(void);
	void armIdleTimer(void);
	static void sIdleTimer(OSObject *owner, IOTimerEventSource *sender);
	uint64_t nowMs(void) const;
	static IOReturn sStartBoot(OSObject *owner, void *, void *, void *,
				   void *);

	void teardown(void);

	// devboot event log, host pages for the HS image
	static void sBootEvent(void *user, const struct pg106_devboot_event *e);
	static int sHostAlloc(void *user, uint64_t size, void **cpu, void **handle);
	static int sHostBus(void *user, void *handle, uint64_t off, uint64_t *addr);
	static void sHostFree(void *user, void *handle);
	// interrupts
	void setupInterrupts(void);
	static bool sIntrFilter(OSObject *owner, IOFilterInterruptEventSource *src);
	static void sIntrAction(OSObject *owner, IOInterruptEventSource *src, int count);
	static void sNonstall(void *ctx);
	void intrArm(void);
	void runSelftest(void);
	// HD-audio PCI function (01:00.1): config 0x488 bit 25, cleared by
	// D3cold. PG106 owns it (Linux: quirk_nvidia_hda at enable and
	// resume_early); PG106FB only reads it.
	void hdaFunctionEnsure(const char *why);
public:
	bool hdaRescanOk(void); // gate held
private:
	static void sHdaRescanCall(thread_call_param_t p0, thread_call_param_t p1);

	IOPCIDevice *fPCI = nullptr;
	IOWorkLoop *fWorkLoop = nullptr;
	IOCommandGate *fGate = nullptr;
	IOTimerEventSource *fIdleTimer = nullptr;
	bool fGateAdded = false, fTimerAdded = false;
	bool fMemWasEnabled = false, fMemEnableTouched = false;
	bool fBusMasterTouched = false;
	IOMemoryMap *fBar0 = nullptr;
	volatile uint8_t *fRegs = nullptr;
	uint64_t fBar0Len = 0, fBar1Len = 0;
	uint16_t fDeviceId = 0;
	struct pg106_uc_pmc fPmc = {};

	IOACPIPlatformDevice *fAcpi = nullptr;     // dGPU companion (PEGP)
	IOACPIPlatformDevice *fAcpiPort = nullptr; // its parent (PEG0)

	struct pg106_vbios_blob fVbios = {};
	uint32_t fVbiosSource = 0;

	struct pg106_devboot_fw fFw = {};
	uint8_t *fFwBuf[PG106_FW_MAX] = {};
	uint32_t fFwLen[PG106_FW_MAX] = {};
	OSKextRequestTag fFwTag[PG106_FW_MAX] = {};
	bool fFwPendingSlot[PG106_FW_MAX] = {};
	uint32_t fFwPending = 0, fFwPresent = 0, fFwMissing = 0;
	bool fStopping = false;

	struct pg106_hw_bus fBus = {};
	struct pg106_gpu *fGpu = nullptr; // pg106_kcalloc'd at start
	bool fGpuValid = false;
	struct pg106_devboot_hostmem fHostMem = {};
	struct pg106_dma *fFlushPage = nullptr; // 0x100c10 sysmem flush page
	IOMemoryMap *fBar1Map = nullptr;
	volatile uint8_t *fBar1 = nullptr;
	uint64_t fBar1Phys = 0, fBar3Len = 0;
	IOFilterInterruptEventSource *fIntrSrc = nullptr;
	bool fIntrAdded = false;
	struct pg106_intr fIntr = {};
	bool fIntrUp = false; // fIntr initialised (GPU writable)
	struct {
		uint32_t mask;
		pg106_intr_fn fn;
		void *ctx;
	} fRoute[4] = {};
	uint32_t fFenceEv = 0;
	uint32_t fIntrCount = 0;
	uint32_t fSelftest = 0; // boot-arg pg106_selftest=1 (CE), 2 (+ GR)
	struct pg106_pm fPm = {};
	uint64_t fVramSize = 0;
	const void *fChannelOwner = nullptr;
	// GR engine context (pascal/core gr_ctx.h), alive while the GPU is
	// booted; the golden image survives re-boots in fGolden.
	struct pg106_gr *fGr = nullptr;
	bool fGrValid = false;
	uint32_t *fGolden = nullptr;
	uint32_t fGoldenSize = 0;
	struct pg106_chan *fChan = nullptr; // the render channel
	struct pg106_grctx_chan fGrChan = {};
	struct pg106_kd *fChanKd = nullptr;
	int grEnsure(void);
	void grDrop(void);
	bool fRenderUsers = false;
	uint8_t fStopAfter = PG106_DB_READY; // boot-arg pg106_stop_after
	bool fHdaEnable = true; // boot-arg pg106_hda=0 leaves the bit alone
	thread_call_t fHdaRescanCall = nullptr;
	thread_call_t fBootCall = nullptr;   // bring-up off the IOConfig thread
	uint64_t fBootT0 = 0;                // nowMs() when the bring-up began
	uint32_t fFwEmbedded = 0;            // rows served by pg106_fw_embedded
	OSArray *fBootLog = nullptr;         // PG106BootLog: one string per boot event
	volatile SInt32 fCallsBusy = 0;
	// system sleep / wake
	bool fPMInited = false;
	bool fPMEnable = true;               // boot-arg pg106_pm=0: no power driver
	bool fSleepD3cold = false;           // boot-arg pg106_sleep_d3cold=1: PG00._OFF at sleep
	volatile bool fSystemAsleep = false; // between setPowerState(0) and (1)
	volatile bool fHwOff = false;        // no BAR0/BAR1 access (rd32/wr32 refuse)
	bool fSleptD3cold = false;           // this sleep evaluated PG00._OFF
	bool fGrWasUp = false;               // GR engine was set up at sleep: set it up at wake
	bool fSentOff = false;               // PG106_KIF_MSG_GPU_OFF sent; GPU_ON owed once ready
	thread_call_t fWakeCall = nullptr;
	uint64_t fWakeT0 = 0;
	uint32_t fMemEpoch = 0;
	uint32_t fSleeps = 0, fWakes = 0;
	char fWakeNote[160] = {};            // what doResume did, for PG106WakeResult
};

#endif // PG106_HPP
