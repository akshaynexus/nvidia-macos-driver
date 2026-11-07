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
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/OSKextLib.h>

#include <pascal/devboot.h>
#include <pascal/hwexec.h>
#include <pascal/kdisp.h>
#include <pascal/pm.h>
#include <pascal/vbios_fetch.h>

#include "pg106_uc_abi.h"

#define PG106_FW_MAX 16u

class PG106 : public IOService {
	OSDeclareDefaultStructors(PG106)

public:
	IOService *probe(IOService *provider, SInt32 *score) override;
	bool start(IOService *provider) override;
	void stop(IOService *provider) override;
	void free(void) override;
	IOWorkLoop *getWorkLoop(void) const override;

	// Identity captured at start(); never changes afterwards.
	void pmcSnapshot(struct pg106_uc_pmc *out) const;

	// ---- for PG106UserClient (call with the gate held: runOnGate) ----
	IOCommandGate *gate(void) const { return fGate; }
	// A render session opens: powers up / boots if needed (may take
	// seconds), returns 0 or a negative errno (-ENODEV: bring-up failed).
	int openSession(void);
	void closeSession(void);
	bool gpuReady(void) const;
	// The single hardware channel (v0): one render client at a time.
	int claimChannel(const void *owner);
	void releaseChannel(const void *owner);
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

private:
	// start() helpers
	IOReturn identify(void);
	void findAcpi(void);
	void fetchVbios(void);
	void requestFirmware(void);
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
	void armIdleTimer(void);
	static void sIdleTimer(OSObject *owner, IOTimerEventSource *sender);
	uint64_t nowMs(void) const;
	static IOReturn sStartBoot(OSObject *owner, void *, void *, void *,
				   void *);

	void teardown(void);

	// devboot memory-object writer (ACR WPR/IMG) and event log
	static int sMemWrite(void *user, uint8_t space, uint64_t off,
			     const uint8_t *src, uint32_t len);
	static void sBootEvent(void *user, const struct pg106_devboot_event *e);

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
	struct pg106_devboot fBoot = {};
	bool fBootValid = false;
	struct pg106_pm fPm = {};
	uint64_t fVramSize = 0, fAcrBase = 0;
	uint64_t fPraminWindow = ~0ull;
	const void *fChannelOwner = nullptr;
	bool fRenderUsers = false;
	uint8_t fStopAfter = PG106_DB_READY; // boot-arg pg106_stop_after
};

#endif // PG106_HPP
