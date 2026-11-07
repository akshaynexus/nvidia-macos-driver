// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/src/PG106.hpp — the GP106 kext driver class.
//
// An IOService (IOKit/IOService.h) matched through the wide NVIDIA
// personality in Info.plist, gated in probe() by the GP106 device-id table
// (pg106_device_gate.h). BAR0 mapping mirrors the NVRM pattern
// (kexts/NVRM/NVRM.cpp:634-635: IOMemoryDescriptor + map(kIOMapInhibitCache)).
//
// Class declaration conventions (OSDeclareDefaultStructors / meta classes)
// are libkern: libkern/c++/OSObject.h and libkern/c++/OSMetaClass.h.

#ifndef PG106_HPP
#define PG106_HPP

#include <IOKit/IOCommandGate.h>       // IOCommandGate
#include <IOKit/IOService.h>           // IOService
#include <IOKit/IOMemoryDescriptor.h>  // IOMemoryDescriptor, IOMemoryMap
#include <IOKit/IOWorkLoop.h>          // IOWorkLoop
#include <IOKit/pci/IOPCIDevice.h>     // IOPCIDevice

#include "pg106_uc_abi.h" // struct pg106_uc_pmc

class PG106 : public IOService {
	OSDeclareDefaultStructors(PG106)

public:
	IOService *probe(IOService *provider, SInt32 *score) override;
	bool start(IOService *provider) override;
	void stop(IOService *provider) override;
	void free(void) override;

	// IOService::getWorkLoop() const (IOKit/IOService.h). Returns the
	// driver's own loop once start() created it, so the command gate and
	// anything IOKit schedules against this service share one loop.
	IOWorkLoop *getWorkLoop(void) const override;

	// Identity captured by bringUp(). Written once before start() returns
	// true and never changed afterwards; a UserClient can only exist after
	// a successful start(), so readers need no lock.
	void pmcSnapshot(struct pg106_uc_pmc *out) const;

private:
	static IOReturn sBringUp(OSObject *owner, void *arg0, void *arg1,
				 void *arg2, void *arg3);
	IOReturn bringUp(void); // runs with the command gate held
	void teardown(void);    // releases everything start() acquired

	IOPCIDevice *fPCI = nullptr;      // retained in start()
	IOWorkLoop *fWorkLoop = nullptr;  // created in start()
	IOCommandGate *fGate = nullptr;   // created in start()
	bool fGateAdded = false;          // fGate is on fWorkLoop
	bool fMemWasEnabled = false;      // PCI memory decode before start()
	bool fMemEnableTouched = false;   // we called setMemoryEnable(true)
	IOMemoryMap *fBar0 = nullptr;     // BAR0, uncached
	struct pg106_uc_pmc fPmc = {};    // see pmcSnapshot()
};

#endif // PG106_HPP
