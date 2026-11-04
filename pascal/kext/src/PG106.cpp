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

#include "PG106.hpp"

#include <IOKit/IOLib.h>            // IOLog
#include <IOKit/IOMapTypes.h>       // kIOMapInhibitCache
#include <IOKit/IOReturn.h>         // kIOReturn* status codes
#include <libkern/libkern.h>        // memcpy (libkern declares it for kexts)

#include <pascal/pmc.h>             // PG106_PMC_BOOT_0, pg106_pmc_probe

#define LOG(fmt, ...) IOLog("PG106: " fmt "\n", ##__VA_ARGS__)

OSDefineMetaClassAndStructors(PG106, IOService)

// ---------------------------------------------------------------------
// Personality gate (probe)
// ---------------------------------------------------------------------

IOService *
PG106::probe(IOService *provider, SInt32 *score)
{
	IOService *res = super::probe(provider, score);

	IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
	if (!pci)
		return res; // not ours; another personality will take it

	// Vendor/config word at 0x00/0x02, same reads as NVRM::probe
	// (NVRM.cpp:404). IOPCIDevice::configRead16 — IOKit/pci/IOPCIDevice.h.
	const uint16_t vendor = pci->configRead16(0x00);
	const uint16_t device = pci->configRead16(0x02);

	// The SOFTWARE gate (HANDOFF-PASCAL.md §6 Phase 3): the plist match is
	// a wide net on purpose; this table decides. Only the Pascal Helios
	// 300 GP10x ids pass (HELIOS300-PASCAL-RESEARCH.md §1).
	enum pg106_gate_reason why = PG106_GATE_NO_MATCH;
	if (vendor != 0x10de || !pg106_device_gate(device, &why)) {
		LOG("probe %04x:%04x rejected (not a supported GP10x Helios 300 device)",
		    vendor, device);
		if (score)
			*score = 0;
		return nullptr;
	}

	LOG("probe %04x:%04x accepted (%s)", vendor, device,
	    pg106_gate_chip_name(device));
	fPmc = {}; // fresh identity; BOOT_0 is read at start time
	return res;
}

// ---------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------

bool
PG106::start(IOService *provider)
{
	fPCI = OSDynamicCast(IOPCIDevice, provider);
	if (!fPCI)
		return false;

	if (!super::start(provider))
		return false;

	// Work loop + command gate: all BAR0 access happens on the gate so
	// the UserClient can later serialize requests the same way
	// (IOWorkLoop::workLoop() — IOKit/IOWorkLoop.h;
	// IOCommandGate::commandGate() — IOKit/IOCommandGate.h).
	fWorkLoop = IOWorkLoop::workLoop();
	if (!fWorkLoop) {
		stop(provider);
		return false;
	}
	fGate = IOCommandGate::commandGate(this);
	if (!fGate) {
		stop(provider);
		return false;
	}

	// Make sure PCI memory decoding is on before touching BAR0.
	// IOPCIDevice::setMemoryEnable — IOKit/pci/IOPCIDevice.h.
	fPCI->setMemoryEnable(true);

	// Serialized bring-up through the gate.
	const IOReturn r =
		fGate->runAction(sBringUp); // IOCommandGate::runAction
	if (r != kIOReturnSuccess) {        // kIOReturnSuccess — IOKit/IOReturn.h
		LOG("bring-up failed: 0x%x", r);
		stop(provider);
		return false;
	}
	return true;
}

void
PG106::stop(IOService *provider)
{
	// OSObject::release — libkern/c++/OSObject.h.
	if (fBar0) {
		fBar0->release();
		fBar0 = nullptr;
	}
	if (fGate) {
		fGate->release();
		fGate = nullptr;
	}
	if (fWorkLoop) {
		fWorkLoop->release();
		fWorkLoop = nullptr;
	}
	super::stop(provider);
}

void
PG106::free(void)
{
	super::free();
}

IOWorkLoop *
PG106::workLoop(void)
{
	return fWorkLoop;
}

// ---------------------------------------------------------------------
// Bring-up (command gate context)
// ---------------------------------------------------------------------

IOReturn
PG106::sBringUp(OSObject *owner, void *, void *, void *, void *, void *)
{
	PG106 *self = OSDynamicCast(PG106, owner);
	return self ? self->bringUp() : kIOReturnBadArgument;
}

IOReturn
PG106::bringUp(void)
{
	// BAR0 (index 0) as an IOMemoryDescriptor — IOPCIDevice::
	// getDeviceMemoryWithIndex, IOKit/pci/IOPCIDevice.h.
	IOMemoryDescriptor *bar0 = fPCI->getDeviceMemoryWithIndex(0);
	if (!bar0)
		return kIOReturnNotFound;

	// Map it uncached, the NVRM pattern (NVRM.cpp:634-635).
	// IOMemoryDescriptor::map — IOKit/IOMemoryDescriptor.h;
	// kIOMapInhibitCache — IOKit/IOMapTypes.h.
	fBar0 = bar0->map(kIOMapInhibitCache);
	if (!fBar0)
		return kIOReturnNoMemory;

	// First BAR0 word is PMC_BOOT_0 (include/pascal/pmc.h; nouveau
	// base.c does `boot0 = nvkm_rd32(device, 0x000000)`).
	volatile const uint32_t *bar0va =
		(volatile const uint32_t *)fBar0->getVirtualAddress(); // IOMemoryMap::getVirtualAddress — IOKit/IOMemoryDescriptor.h
	fBoot0 = bar0va[PG106_PMC_BOOT_0 / 4];
	fPmc = pg106_pmc_probe(fBoot0);

	// Cross-check the BOOT_0 identity against the PCI id that probe()
	// already gated (pg106_probe_decision).
	const uint16_t device = fPCI->configRead16(0x02);
	const struct pg106_probe_decision d =
		pg106_probe_decision(device, fBoot0);
	if (!d.accept) {
		LOG("BOOT_0 %08x does not confirm a GP106 (arch %x rev %02x) — refusing",
		    fBoot0, d.boot0_architecture, d.boot0_revision);
		return kIOReturnUnsupported;
	}

	LOG("GP106 alive: BOOT_0 %08x (arch 0x%x rev 0x%02x)", fBoot0,
	    d.boot0_architecture, d.boot0_revision);
	// Phase 4+ continues here: PFIFO/PTIMER discovery, ACR, GR/CE.
	return kIOReturnSuccess;
}

// ---------------------------------------------------------------------
// Identity snapshot for the UserClient
// ---------------------------------------------------------------------

void
PG106::pmcSnapshot(uint32_t *boot0, struct pg106_pmc_probe *out) const
{
	// Written once in bringUp() before start() returns and never mutated
	// afterwards, so no lock is needed for readers that arrived after a
	// successful start (the only way to get a UserClient here).
	*boot0 = fBoot0;
	*out = fPmc;
}
