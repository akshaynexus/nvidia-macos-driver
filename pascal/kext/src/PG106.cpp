// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/src/PG106.cpp — GP106 driver: personality gate, BAR0 map, PMC
// identity, work loop + command gate. Phase 3 skeleton: bring-up stops after
// identification; engines (FIFO/GR/CE), interrupts, power and display are
// later phases (see README.md, "Not implemented yet").
//
// Patterns mirrored from this repo's NVRM kext (PolyForm-NC — reference only,
// no code copied; this tree is GPL-2.0-only, see README.md):
//   - probe() PCI config reads: NVRM.cpp:396-406
//   - BAR0 map via IOMemoryDescriptor + map(kIOMapInhibitCache): NVRM.cpp:634-635
//   - IOUserClient wiring: PG106UserClient.cpp
//
// API references are MacKernelSDK headers (github.com/acidanthera/MacKernelSDK):
// IOKit/IOService.h, IOKit/IOWorkLoop.h, IOKit/IOCommandGate.h,
// IOKit/pci/IOPCIDevice.h, IOKit/IOMemoryDescriptor.h.

#include "PG106.hpp"

#include <IOKit/IOLib.h>      // IOLog
#include <IOKit/IOReturn.h>   // kIOReturn*

#include <pascal/pmc.h>       // PG106_PMC_BOOT_0, pg106_pmc_probe

#include "pg106_device_gate.h"

#define LOG(fmt, ...) IOLog("PG106: " fmt "\n", ##__VA_ARGS__)

// libkern's OSDeclareDefaultStructors does not declare a `super` alias;
// the IOKit convention is this define, scoped to the .cpp.
#define super IOService

OSDefineMetaClassAndStructors(PG106, IOService)

// ---------------------------------------------------------------------
// Personality gate (probe)
// ---------------------------------------------------------------------

IOService *
PG106::probe(IOService *provider, SInt32 *score)
{
	IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
	if (!pci)
		return nullptr;

	// Vendor/device words at config 0x00/0x02, the same reads as
	// NVRM::probe (NVRM.cpp:404). configRead16(IOByteCount) is the public
	// accessor in IOPCIDevice.h.
	const uint16_t vendor = pci->configRead16(0x00);
	const uint16_t device = pci->configRead16(0x02);

	// The plist match is a wide net on purpose (HANDOFF-PASCAL.md §6
	// Phase 3); this table decides. Only GP106-die ids pass, because
	// start() can only accept a GP106 BOOT_0.
	enum pg106_gate_reason why = PG106_GATE_NO_MATCH;
	if (vendor != 0x10de || !pg106_device_gate(device, &why)) {
		const char *name = pg106_gate_chip_name(device);
		LOG("probe %04x:%04x rejected (%s)", vendor, device,
		    name ? name : "not a GP106 mobile id");
		return nullptr;
	}

	LOG("probe %04x:%04x accepted (%s)", vendor, device,
	    pg106_gate_chip_name(device));
	return super::probe(provider, score);
}

// ---------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------

bool
PG106::start(IOService *provider)
{
	IOReturn r;

	if (!super::start(provider))
		return false;

	fPCI = OSDynamicCast(IOPCIDevice, provider);
	if (!fPCI)
		goto fail;
	fPCI->retain();

	// Own work loop + command gate. A command gate does nothing until it
	// is added to a work loop: runAction() on a gate with no work loop
	// returns kIOReturnNotReady (IOCommandGate.h, runAction).
	fWorkLoop = IOWorkLoop::workLoop();
	if (!fWorkLoop)
		goto fail;
	fGate = IOCommandGate::commandGate(this);
	if (!fGate)
		goto fail;
	r = fWorkLoop->addEventSource(fGate);
	if (r != kIOReturnSuccess) {
		LOG("addEventSource(gate) failed: 0x%x", r);
		goto fail;
	}
	fGateAdded = true;

	// PCI memory decode must be on before BAR0 is touched. The previous
	// state is restored in teardown() (setMemoryEnable returns it).
	fMemWasEnabled = fPCI->setMemoryEnable(true);
	fMemEnableTouched = true;

	r = fGate->runAction(sBringUp);
	if (r != kIOReturnSuccess) {
		LOG("bring-up failed: 0x%x", r);
		goto fail;
	}

	// Publish the service so userspace (IOServiceOpen) and IOKit matching
	// can find it; without this the UserClient is unreachable.
	registerService();
	return true;

fail:
	teardown();
	super::stop(provider);
	return false;
}

void
PG106::stop(IOService *provider)
{
	// IOKit terminates and stops our user clients (children in the
	// registry) before calling stop() on the provider, so nothing else
	// is using BAR0 or the gate here.
	teardown();
	super::stop(provider);
}

void
PG106::free(void)
{
	// stop() (or a failed start()) already tore everything down; this only
	// matters if the object is freed without start() having completed its
	// own unwinding, and teardown() is idempotent.
	teardown();
	super::free();
}

void
PG106::teardown(void)
{
	OSSafeReleaseNULL(fBar0); // drops the mapping

	if (fMemEnableTouched && fPCI) {
		fPCI->setMemoryEnable(fMemWasEnabled);
		fMemEnableTouched = false;
	}

	if (fGate) {
		if (fGateAdded && fWorkLoop)
			fWorkLoop->removeEventSource(fGate);
		fGateAdded = false;
		OSSafeReleaseNULL(fGate);
	}
	OSSafeReleaseNULL(fWorkLoop);
	OSSafeReleaseNULL(fPCI);
}

IOWorkLoop *
PG106::getWorkLoop(void) const
{
	// Before start() created our loop, fall back to the default
	// behaviour (walk the provider chain), as IOService does.
	return fWorkLoop ? fWorkLoop : super::getWorkLoop();
}

// ---------------------------------------------------------------------
// Bring-up (command gate context)
// ---------------------------------------------------------------------

IOReturn
PG106::sBringUp(OSObject *owner, void *, void *, void *, void *)
{
	PG106 *self = OSDynamicCast(PG106, owner);
	return self ? self->bringUp() : kIOReturnBadArgument;
}

IOReturn
PG106::bringUp(void)
{
	// BAR0 is config BAR index 0 (offset 0x10). getDeviceMemoryWithIndex
	// returns an IODeviceMemory the provider owns: not retained here.
	IOMemoryDescriptor *bar0 = fPCI->getDeviceMemoryWithIndex(0);
	if (!bar0)
		return kIOReturnNotFound;

	// Uncached mapping, the NVRM pattern (NVRM.cpp:634-635).
	fBar0 = bar0->map(kIOMapInhibitCache);
	if (!fBar0)
		return kIOReturnNoMemory;
	if (fBar0->getLength() < PG106_PMC_BOOT_0 + sizeof(uint32_t))
		return kIOReturnNoResources;

	// First BAR0 word is PMC_BOOT_0 (include/pascal/pmc.h; nouveau
	// base.c does `boot0 = nvkm_rd32(device, 0x000000)`).
	volatile const uint32_t *regs =
		reinterpret_cast<volatile const uint32_t *>(
			fBar0->getVirtualAddress());
	const uint32_t boot0 = regs[PG106_PMC_BOOT_0 / 4];

	// Cross-check BOOT_0 against the PCI id probe() already gated. An
	// unreadable BAR (0 or all-ones) is rejected here, not decoded.
	const uint16_t device = fPCI->configRead16(0x02);
	const struct pg106_start_decision d = pg106_start_decision(device, boot0);
	if (!d.accept) {
		if (!d.boot0_readable)
			LOG("BOOT_0 read back %08x: BAR0 not decoding — refusing",
			    boot0);
		else
			LOG("BOOT_0 %08x does not confirm a GP106 (arch 0x%x rev 0x%02x) — refusing",
			    boot0, d.boot0_architecture, d.boot0_revision);
		return kIOReturnUnsupported;
	}

	const struct pg106_pmc_probe pmc = pg106_pmc_probe(boot0);
	fPmc = {};
	fPmc.boot0 = boot0;
	fPmc.architecture = pmc.architecture;
	fPmc.implementation = pmc.implementation;
	fPmc.revision = pmc.revision;
	fPmc.status = static_cast<uint32_t>(pmc.status);
	fPmc.flags = (pmc.is_gp10x ? PG106_UC_PMC_IS_GP10X : 0u) |
		     (pmc.is_gp106 ? PG106_UC_PMC_IS_GP106 : 0u) |
		     (pmc.legacy_nv04 ? PG106_UC_PMC_LEGACY_NV4 : 0u);

	LOG("GP106 identified: BOOT_0 %08x (arch 0x%x rev 0x%02x)", boot0,
	    d.boot0_architecture, d.boot0_revision);
	// Phase 4+ continues here: PFIFO/PTIMER discovery, ACR, GR/CE.
	return kIOReturnSuccess;
}

// ---------------------------------------------------------------------
// Identity snapshot for the UserClient
// ---------------------------------------------------------------------

void
PG106::pmcSnapshot(struct pg106_uc_pmc *out) const
{
	*out = fPmc;
}
