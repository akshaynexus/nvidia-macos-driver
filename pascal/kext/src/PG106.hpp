// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/src/PG106.hpp — the GP106 kext driver class.
//
// An IOService (IOKit/IOService.h) matched through the wide NVIDIA
// personality in Info.plist, gated in probe() by the GP10x device-id table
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

#include <pascal/pmc.h> // struct pg106_pmc_probe

#include "pg106_device_gate.h"

class PG106 : public IOService {
	OSDeclareDefaultStructors(PG106)

public:
	// Personality gate: rejects any provider whose device id is not in
	// the GP10x Helios 300 table (pg106_device_gate.h).
	IOService *probe(IOService *provider, SInt32 *score) override;

	bool start(IOService *provider) override;
	void stop(IOService *provider) override;
	void free(void) override;

	// Our work loop (below) so IOKit serializes notifications against us.
	IOWorkLoop *workLoop(void) override;

	// Snapshot of the identity taken at start: raw BOOT_0 word and the
	// pascal/core decode. Read-only after bring-up; safe for the
	// UserClient to copy out.
	void pmcSnapshot(uint32_t *boot0, struct pg106_pmc_probe *out) const;

private:
	static IOReturn sBringUp(OSObject *owner, void *arg0, void *arg1,
				 void *arg2, void *arg3, void *arg4);
	IOReturn bringUp(void); // runs on the command gate

	IOPCIDevice *fPCI = nullptr;          // IOPCIDevice (IOPCIDevice.h)
	IOWorkLoop *fWorkLoop = nullptr;      // IOWorkLoop (IOWorkLoop.h)
	IOCommandGate *fGate = nullptr;       // IOCommandGate (IOCommandGate.h)
	IOMemoryMap *fBar0 = nullptr;         // IOMemoryMap (IOMemoryDescriptor.h)
	uint32_t fBoot0 = 0;                  // raw PMC_BOOT_0
	struct pg106_pmc_probe fPmc = {};     // decode via pg106_pmc_probe
};

#endif // PG106_HPP
