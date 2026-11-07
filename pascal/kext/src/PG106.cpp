// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/src/PG106.cpp — GP106 driver: personality gate, BAR0 map, PMC
// identity, VBIOS from ACPI _ROM (or the PCI ROM BAR), firmware from the
// bundle, bring-up and runtime power through pascal/core.
//
// What happens at load (each line below is an IOLog "PG106: ..." line; the
// owner runbook lists them):
//   probe   gate on the GP106 mobile ids
//   start   work loop + gate + idle timer, PCI memory + bus master on,
//           BAR0/BAR1, BOOT_0 identity, registerService()
//   acpi    the PCI function's ACPI companion (PEGP) and its parent (PEG0,
//           where power resource PG00 lives on the Helios)
//   vbios   ACPI _ROM in 0x1000-byte calls, else the PCI ROM BAR; accepted
//           only if image 0's PCIR ids are this function's (vbios_fetch.h)
//   fw      one OSKextRequestResource per pg106_devboot_fw_names entry
//   boot    once every request has answered: pm_start -> devboot (POST if
//           needed, PTOP, ACR, GR); every stage logged; a failure stops
//           there and leaves the GPU as it is, with userspace refused
//   power   when policy allows (boot-arg pg106_d3cold=1): idle -> PG00._OFF;
//           next session -> PG00._ON -> boot again
//
// Patterns mirrored from this repo's NVRM kext (PolyForm-NC — reference only,
// no code copied; this tree is GPL-2.0-only, see README.md): probe() config
// reads NVRM.cpp:396-406, BAR0 map NVRM.cpp:634-635.
//
// API references are MacKernelSDK headers (github.com/acidanthera/MacKernelSDK
// @ 3f750085): IOKit/IOService.h, IOWorkLoop.h, IOCommandGate.h,
// IOTimerEventSource.h, pci/IOPCIDevice.h, acpi/IOACPIPlatformDevice.h,
// acpi/IOACPITypes.h (gIOACPIPlane), libkern/OSKextLib.h
// (OSKextRequestResource, OSKextCancelRequest), pexpert/pexpert.h
// (PE_parse_boot_argn), kern/clock.h.

#include "PG106.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IOReturn.h>
#include <IOKit/acpi/IOACPITypes.h>
#include <kern/clock.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>

#include <pascal/pmc.h>

#include "pg106_klibc.h"
#include "pg106_kif.h"

#include "pg106_device_gate.h"

#define LOG(fmt, ...) IOLog("PG106: " fmt "\n", ##__VA_ARGS__)

#define super IOService

OSDefineMetaClassAndStructors(PG106, IOService)

// GPFIFO entries per channel ring (power of two, kdisp.h).
#define PG106_IB_MAX 1024u
// nouveau_device.c:504-505 floor; BLOCKERS B9 leaves the exact value open.
#define PG106_DRM_VERSION 0x01000301u
// Power resource the Helios SSDT11 defines under PEG0 (HELIOS300-HW-PROBE.md).
#define PG106_ACPI_PWR_OFF "PG00._OFF"
#define PG106_ACPI_PWR_ON  "PG00._ON"

// Per-request context for OSKextRequestResource (the callback gets one
// pointer).
struct pg106_fw_ctx {
	PG106 *self;
	unsigned idx;
};
static struct pg106_fw_ctx sFwCtx[PG106_FW_MAX];

// ---------------------------------------------------------------------
// Personality gate (probe)
// ---------------------------------------------------------------------

IOService *
PG106::probe(IOService *provider, SInt32 *score)
{
	IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
	if (!pci)
		return nullptr;

	const uint16_t vendor = pci->configRead16(0x00);
	const uint16_t device = pci->configRead16(0x02);
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
// MMIO bus (pascal/hwexec.h struct pg106_hw_bus)
// ---------------------------------------------------------------------

uint32_t
PG106::rd32(uint32_t addr) const
{
	if (!fRegs || (uint64_t)addr + 4 > fBar0Len || (addr & 3))
		return 0xffffffffu;
	return *reinterpret_cast<volatile const uint32_t *>(fRegs + addr);
}

void
PG106::wr32(uint32_t addr, uint32_t val)
{
	if (!fRegs || (uint64_t)addr + 4 > fBar0Len || (addr & 3))
		return;
	*reinterpret_cast<volatile uint32_t *>(fRegs + addr) = val;
	__sync_synchronize(); // ring/memory writes before a doorbell
}

static uint32_t
busRd32(void *u, uint32_t a)
{
	return static_cast<PG106 *>(u)->rd32(a);
}

static void
busWr32(void *u, uint32_t a, uint32_t v)
{
	static_cast<PG106 *>(u)->wr32(a, v);
}

static void
busDelay(void *, uint32_t us)
{
	// IOSleep blocks (the work loop is held, nothing else runs on it);
	// short waits spin.
	if (us >= 1000) {
		IOSleep(us / 1000);
		us %= 1000;
	}
	if (us)
		IODelay(us);
}

uint64_t
PG106::nowMs(void) const
{
	uint64_t abs = 0, ns = 0;

	clock_get_uptime(&abs);
	absolutetime_to_nanoseconds(abs, &ns);
	return ns / 1000000ull;
}

// PTIMER TIME_1/TIME_0 (0x9410/0x9400, SDK gm107/dev_timer.h), read
// hi-lo-hi like nv04_timer_read (nvkm/subdev/timer/nv04.c).
uint64_t
PG106::ptimerNs(void)
{
	uint32_t hi, lo;

	do {
		hi = rd32(0x9410);
		lo = rd32(0x9400);
	} while (hi != rd32(0x9410));
	return ((uint64_t)hi << 32) | lo;
}

// ---------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------

void
PG106::readPolicy(void)
{
	uint32_t v = 0;
	OSObject *p;

	pg106_pm_init(&fPm, PG106_PM_IDLE_MS_DEFAULT, false);
	// D3cold stays off unless the owner turns it on: the PG00 path has
	// never run under macOS (BLOCKERS B5, B8, B12).
	if (PE_parse_boot_argn("pg106_d3cold", &v, sizeof(v)))
		fPm.d3cold = v != 0;
	if (PE_parse_boot_argn("pg106_idle_ms", &v, sizeof(v)))
		fPm.idle_ms = v;
	// Owner decision (2026-11-07): render connections are open to every
	// local user by default; pg106_render_users=0 (or Info.plist
	// PG106RenderUsers = false) restricts them to administrators.
	p = getProperty("PG106RenderUsers");
	fRenderUsers = p != kOSBooleanFalse;
	if (PE_parse_boot_argn("pg106_render_users", &v, sizeof(v)))
		fRenderUsers = v != 0;
	// Safe mode: stop the bring-up after this devboot stage (1 = identify
	// + posted check: no GPU register is written).
	if (PE_parse_boot_argn("pg106_stop_after", &v, sizeof(v)) &&
	    v < PG106_DB_READY)
		fStopAfter = (uint8_t)v;
	if (fStopAfter < PG106_DB_READY)
		fPm.d3cold = false; // never cut power under a half-brought-up GPU
	// GPU-work self-test after bring-up: a channel on the copy engine's
	// runlist does a host semaphore release and a 64 KiB CE copy
	// (pg106_gpu_selftest); logged as "PG106: selftest: ...".
	if (PE_parse_boot_argn("pg106_selftest", &v, sizeof(v)))
		fSelftest = v != 0;
	LOG("policy: d3cold=%d idle_ms=%u render_users=%d stop_after=%s",
	    fPm.d3cold, fPm.idle_ms, fRenderUsers,
	    pg106_devboot_stage_name(fStopAfter));
}

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
	fIdleTimer = IOTimerEventSource::timerEventSource(this, sIdleTimer);
	if (!fIdleTimer || fWorkLoop->addEventSource(fIdleTimer) != kIOReturnSuccess)
		goto fail;
	fTimerAdded = true;

	fBus.user = this;
	fBus.rd32 = busRd32;
	fBus.wr32 = busWr32;
	fBus.delay_us = busDelay;
	fBus.mem_write = nullptr; // devboot serves the ACR objects itself
	fBus.poll_us = 10;
	fHostMem.user = this;
	fHostMem.alloc = sHostAlloc;
	fHostMem.bus = sHostBus;
	fHostMem.free = sHostFree;
	fHostMem.coherent = true; // NVKM_MEM_TARGET_HOST (core/firmware.c:175)
	fGpu = static_cast<struct pg106_gpu *>(pg106_kcalloc(1, sizeof(*fGpu)));
	if (!fGpu)
		goto fail;
	readPolicy();

	// Decode before BAR0 is touched; DMA (falcons, PBDMA) needs bus
	// mastering. Both are restored in teardown().
	fMemWasEnabled = fPCI->setMemoryEnable(true);
	fMemEnableTouched = true;
	fPCI->setBusMasterEnable(true);
	fBusMasterTouched = true;

	r = identify();
	if (r != kIOReturnSuccess) {
		LOG("start: identification failed (0x%x); not attaching", r);
		goto fail;
	}
	findAcpi();
	fetchVbios();
	setupInterrupts();

	// Userspace can find the service (pg106_probe, the bridge) from here
	// on; render sessions wait for the bring-up below.
	setProperty("PG106Ready", kOSBooleanFalse);
	// pg106_kif.h: the ABI PG106Accel may call (PG106_kif.cpp).
	setProperty("PG106KifABI", (unsigned long long)PG106_KIF_ABI, 32);
	registerService();
	requestFirmware();
	return true;

fail:
	teardown();
	super::stop(provider);
	return false;
}

void
PG106::stop(IOService *provider)
{
	// User clients are terminated before the provider stops.
	teardown();
	super::stop(provider);
}

void
PG106::free(void)
{
	teardown();
	super::free();
}

void
PG106::teardown(void)
{
	unsigned i;

	fStopping = true;
	// Firmware requests still queued in kextd: cancel; their contexts are
	// static, the retain taken per request is dropped here.
	for (i = 0; i < PG106_FW_MAX; i++) {
		if (fFwPendingSlot[i]) {
			void *ctx = nullptr;

			if (OSKextCancelRequest(fFwTag[i], &ctx) == kOSReturnSuccess) {
				fFwPendingSlot[i] = false;
				release();
			}
		}
	}
	if (fIdleTimer)
		fIdleTimer->cancelTimeout();
	if (fIntrSrc) {
		fIntrSrc->disable();
		if (fIntrAdded && fWorkLoop)
			fWorkLoop->removeEventSource(fIntrSrc);
		fIntrAdded = false;
		OSSafeReleaseNULL(fIntrSrc);
	}
	kifTeardown();
	if (fGpu) {
		if (fGpuValid)
			pg106_gpu_fini(fGpu);
		fGpuValid = false;
		if (fIntrUp)
			pg106_intr_init(&fIntr, &fBus); // everything blocked
		fIntrUp = false;
		pg106_kfree(fGpu);
		fGpu = nullptr;
	}
	if (fFlushPage) {
		pg106_dma_free(fFlushPage);
		fFlushPage = nullptr;
	}
	fBar1 = nullptr;
	OSSafeReleaseNULL(fBar1Map);
	for (i = 0; i < PG106_FW_MAX; i++) {
		if (fFwBuf[i])
			IOFree(fFwBuf[i], fFwLen[i]);
		fFwBuf[i] = nullptr;
		fFwLen[i] = 0;
	}
	pg106_vbios_blob_free(&fVbios);
	fRegs = nullptr;
	OSSafeReleaseNULL(fBar0);
	OSSafeReleaseNULL(fAcpiPort);
	OSSafeReleaseNULL(fAcpi);
	if (fPCI && fBusMasterTouched) {
		fPCI->setBusMasterEnable(false);
		fBusMasterTouched = false;
	}
	if (fMemEnableTouched && fPCI) {
		fPCI->setMemoryEnable(fMemWasEnabled);
		fMemEnableTouched = false;
	}
	if (fIdleTimer) {
		if (fTimerAdded && fWorkLoop)
			fWorkLoop->removeEventSource(fIdleTimer);
		fTimerAdded = false;
		OSSafeReleaseNULL(fIdleTimer);
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
	return fWorkLoop ? fWorkLoop : super::getWorkLoop();
}

IOReturn
PG106::runOnGate(IOCommandGate::Action fn, void *a0, void *a1, void *a2,
		 void *a3)
{
	if (!fGate)
		return kIOReturnNotReady;
	return fGate->runAction(fn, a0, a1, a2, a3);
}

// ---------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------

IOReturn
PG106::identify(void)
{
	IOMemoryDescriptor *bar0 = fPCI->getDeviceMemoryWithIndex(0);
	IOMemoryDescriptor *bar1 = fPCI->getDeviceMemoryWithIndex(1);

	if (!bar0)
		return kIOReturnNotFound;
	fBar0 = bar0->map(kIOMapInhibitCache);
	if (!fBar0)
		return kIOReturnNoMemory;
	fBar0Len = fBar0->getLength();
	fRegs = reinterpret_cast<volatile uint8_t *>(fBar0->getVirtualAddress());
	if (fBar0Len < PG106_PMC_BOOT_0 + sizeof(uint32_t))
		return kIOReturnNoResources;
	// BAR1 is the VRAM aperture (256 MiB on the Helios,
	// HELIOS300-HW-PROBE.md): the BAR1 VMM maps VRAM into it, USERD's
	// GP_PUT kick goes through it. Mapped uncached for the kernel; BAR3
	// (32 MiB) is the BAR2 VMM's range (NVKM_BAR2_INST).
	fBar1Len = bar1 ? bar1->getLength() : 0;
	if (bar1) {
		fBar1Phys = bar1->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
		fBar1Map = bar1->map(kIOMapInhibitCache);
		if (fBar1Map)
			fBar1 = reinterpret_cast<volatile uint8_t *>(
				fBar1Map->getVirtualAddress());
	}
	{
		IODeviceMemory *bar3 =
			fPCI->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress3);

		fBar3Len = bar3 ? bar3->getLength() : 0;
	}

	const uint32_t boot0 = rd32(PG106_PMC_BOOT_0);
	fDeviceId = fPCI->configRead16(0x02);
	const struct pg106_start_decision d = pg106_start_decision(fDeviceId, boot0);
	if (!d.accept) {
		if (!d.boot0_readable)
			LOG("BOOT_0 read back %08x: BAR0 not decoding (GPU in D3cold?) — refusing",
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
	LOG("GP106 identified: BOOT_0 %08x (arch 0x%x rev 0x%02x), BAR0 %llu MiB, BAR1 %llu MiB",
	    boot0, d.boot0_architecture, d.boot0_revision,
	    (unsigned long long)(fBar0Len >> 20), (unsigned long long)(fBar1Len >> 20));
	return kIOReturnSuccess;
}

void
PG106::pmcSnapshot(struct pg106_uc_pmc *out) const
{
	*out = fPmc;
}

// ---------------------------------------------------------------------
// ACPI companion
// ---------------------------------------------------------------------

void
PG106::findAcpi(void)
{
	// AppleACPIPlatform links a PCI device to its ACPI node through the
	// "acpi-device" property (an IOACPIPlatformDevice) and "acpi-path"
	// (its IOACPIPlane path). Neither has been seen on this machine yet
	// (BLOCKERS B12): log what is found.
	OSObject *o = fPCI->getProperty("acpi-device");
	IORegistryEntry *parent;

	fAcpi = OSDynamicCast(IOACPIPlatformDevice, o);
	if (fAcpi) {
		fAcpi->retain();
	} else {
		OSString *path = OSDynamicCast(OSString, fPCI->getProperty("acpi-path"));
		IORegistryEntry *e = path ? IORegistryEntry::fromPath(path->getCStringNoCopy())
					  : nullptr;

		fAcpi = OSDynamicCast(IOACPIPlatformDevice, e);
		if (!fAcpi)
			OSSafeReleaseNULL(e); // fromPath returns a retained entry
	}
	if (!fAcpi) {
		LOG("acpi: no ACPI companion (no acpi-device / acpi-path property)");
		return;
	}
	parent = fAcpi->getParentEntry(gIOACPIPlane);
	fAcpiPort = OSDynamicCast(IOACPIPlatformDevice, parent);
	if (fAcpiPort)
		fAcpiPort->retain();
	LOG("acpi: companion %s, parent %s; _ROM %s, %s %s",
	    fAcpi->getName(), fAcpiPort ? fAcpiPort->getName() : "(none)",
	    fAcpi->validateObject("_ROM") == kIOReturnSuccess ? "present" : "absent",
	    PG106_ACPI_PWR_OFF,
	    fAcpiPort && fAcpiPort->validateObject(PG106_ACPI_PWR_OFF) == kIOReturnSuccess
		    ? "present" : "absent");
}

// ---------------------------------------------------------------------
// VBIOS (pascal/vbios_fetch.h)
// ---------------------------------------------------------------------

// _ROM(offset, length) (ACPI spec 10.2.2 / nouveau shadowacpi.c:25-57):
// the result buffer may be shorter than asked.
static int
acpiRomRead(void *user, uint32_t off, uint32_t len, uint8_t *dst)
{
	IOACPIPlatformDevice *acpi = static_cast<IOACPIPlatformDevice *>(user);
	OSObject *params[2] = { OSNumber::withNumber(off, 32),
				OSNumber::withNumber(len, 32) };
	OSObject *result = nullptr;
	OSData *data;
	IOReturn r;
	int got = -EIO;

	if (params[0] && params[1]) {
		r = acpi->evaluateObject("_ROM", &result, params, 2, 0);
		data = OSDynamicCast(OSData, result);
		if (r == kIOReturnSuccess && data) {
			uint32_t n = data->getLength();

			if (n > len)
				n = len;
			memcpy(dst, data->getBytesNoCopy(), n);
			got = (int)n;
		}
	}
	OSSafeReleaseNULL(result);
	OSSafeReleaseNULL(params[0]);
	OSSafeReleaseNULL(params[1]);
	return got;
}

struct pg106_rom_map {
	volatile const uint8_t *p;
	uint64_t len;
};

static int
pciRomRead(void *user, uint32_t off, uint32_t len, uint8_t *dst)
{
	const struct pg106_rom_map *m = static_cast<struct pg106_rom_map *>(user);
	uint32_t i;

	if (off >= m->len)
		return 0;
	if (len > m->len - off)
		len = (uint32_t)(m->len - off);
	for (i = 0; i < len; i++) // byte reads: ROM BARs need not do 32-bit
		dst[i] = m->p[off + i];
	return (int)len;
}

void
PG106::fetchVbios(void)
{
	struct pg106_vbios_src acpiSrc = {};
	struct pg106_vbios_src pciSrc = {};
	const struct pg106_vbios_src *srcs[2] = { nullptr, nullptr };
	struct pg106_vbios_fetch_result res[2] = {};
	struct pg106_rom_map rom = {};
	IOMemoryMap *romMap = nullptr;
	uint32_t romBar = 0;
	unsigned which = 0;
	int ret;

	if (fAcpi && fAcpi->validateObject("_ROM") == kIOReturnSuccess) {
		acpiSrc.name = "ACPI _ROM";
		acpiSrc.read = acpiRomRead;
		acpiSrc.user = fAcpi;
		acpiSrc.chunk = PG106_VBIOS_ACPI_CHUNK;
		srcs[0] = &acpiSrc;
	}
	// The PCI expansion ROM (config 0x30). The Helios has no ROM chip
	// (the read fails on Linux too), but other boards do.
	{
		IODeviceMemory *mem =
			fPCI->getDeviceMemoryWithRegister(kIOPCIConfigExpansionROMBase);

		if (mem) {
			romBar = fPCI->configRead32(kIOPCIConfigExpansionROMBase);
			fPCI->configWrite32(kIOPCIConfigExpansionROMBase, romBar | 1u);
			romMap = mem->map(kIOMapInhibitCache);
			if (romMap) {
				rom.p = reinterpret_cast<volatile const uint8_t *>(
					romMap->getVirtualAddress());
				rom.len = romMap->getLength();
				pciSrc.name = "PCI ROM";
				pciSrc.read = pciRomRead;
				pciSrc.user = &rom;
				pciSrc.chunk = 0;
				pciSrc.require_checksum = true;
				srcs[1] = &pciSrc;
			}
		}
	}
	ret = pg106_vbios_fetch_first(srcs, 2, 0x10de, fDeviceId, &fVbios,
				      &which, res);
	OSSafeReleaseNULL(romMap);
	if (srcs[1])
		fPCI->configWrite32(kIOPCIConfigExpansionROMBase, romBar);
	for (unsigned i = 0; i < 2; i++) {
		if (!srcs[i])
			LOG("vbios: %s not available", i ? "PCI ROM" : "ACPI _ROM");
		else
			LOG("vbios: %s: ret %d (%s), %u bytes, %u images, %u reads, "
			    "PCIR %04x:%04x, score %d",
			    srcs[i]->name, res[i].ret, res[i].error ? res[i].error : "ok",
			    res[i].size, res[i].images, res[i].reads,
			    res[i].pcir_vendor, res[i].pcir_device, res[i].score);
	}
	if (ret) {
		LOG("vbios: no usable VBIOS (%d): POST will fail if the GPU is not posted",
		    ret);
		fVbiosSource = 0;
		return;
	}
	fVbiosSource = which + 1;
	LOG("vbios: using %s, %u bytes", srcs[which]->name, fVbios.size);
}

// ---------------------------------------------------------------------
// Firmware from the bundle (Contents/Resources, staged by build-macos.sh)
// ---------------------------------------------------------------------

void
PG106::requestFirmware(void)
{
	const char *bundle = OSKextGetCurrentIdentifier();
	unsigned i;

	for (i = 0; i < pg106_devboot_fw_count && i < PG106_FW_MAX; i++) {
		OSReturn r;

		sFwCtx[i].self = this;
		sFwCtx[i].idx = i;
		retain(); // dropped in fwArrived or by OSKextCancelRequest
		fFwPendingSlot[i] = true;
		fFwPending++;
		r = OSKextRequestResource(bundle, pg106_devboot_fw_names[i].resource,
					  fwCallback, &sFwCtx[i], &fFwTag[i]);
		if (r != kOSReturnSuccess) {
			LOG("fw: request %s failed: 0x%x",
			    pg106_devboot_fw_names[i].resource, r);
			fFwPendingSlot[i] = false;
			fFwPending--;
			fFwMissing |= 1u << i;
			release();
		}
	}
	LOG("fw: %u resource requests queued (kextd/kernelmanagerd answers them)",
	    fFwPending);
	if (fFwPending == 0)
		runOnGate(sStartBoot);
}

void
PG106::fwCallback(OSKextRequestTag, OSReturn result, const void *data,
		  uint32_t len, void *ctx)
{
	struct pg106_fw_ctx *c = static_cast<struct pg106_fw_ctx *>(ctx);
	PG106 *self = c->self;
	uint8_t *copy = nullptr;

	// The data is only valid during this call: copy it first, then hand
	// it to the gate.
	if (result == kOSReturnSuccess && data && len) {
		copy = static_cast<uint8_t *>(IOMalloc(len));
		if (copy)
			memcpy(copy, data, len);
		else
			result = kOSReturnError;
	}
	if (self->runOnGate(sFwArrived, reinterpret_cast<void *>((uintptr_t)c->idx),
			    reinterpret_cast<void *>((intptr_t)result), copy,
			    reinterpret_cast<void *>((uintptr_t)len)) != kIOReturnSuccess &&
	    copy)
		IOFree(copy, len);
	self->release();
}

IOReturn
PG106::sFwArrived(OSObject *owner, void *idx, void *res, void *data, void *len)
{
	PG106 *self = OSDynamicCast(PG106, owner);

	if (!self)
		return kIOReturnBadArgument;
	self->fwArrived((unsigned)(uintptr_t)idx, (OSReturn)(intptr_t)res, data,
			(uint32_t)(uintptr_t)len);
	return kIOReturnSuccess;
}

void
PG106::fwArrived(unsigned idx, OSReturn res, const void *data, uint32_t len)
{
	uint8_t *copy = static_cast<uint8_t *>(const_cast<void *>(data));
	const struct pg106_fw_name *nm;

	if (idx >= PG106_FW_MAX || idx >= pg106_devboot_fw_count ||
	    !fFwPendingSlot[idx]) {
		if (copy)
			IOFree(copy, len);
		return;
	}
	nm = &pg106_devboot_fw_names[idx];
	fFwPendingSlot[idx] = false;
	fFwPending--;
	if (fStopping) {
		if (copy)
			IOFree(copy, len);
		return;
	}
	if (res == kOSReturnSuccess && copy) {
		struct pg106_fw_blob *b = reinterpret_cast<struct pg106_fw_blob *>(
			reinterpret_cast<uint8_t *>(&fFw) + nm->offset);

		fFwBuf[idx] = copy;
		fFwLen[idx] = len;
		b->data = copy;
		b->size = len;
		fFwPresent |= 1u << idx;
		LOG("fw: %s (%s) %u bytes", nm->resource, nm->path, len);
	} else {
		fFwMissing |= 1u << idx;
		LOG("fw: %s (%s) missing: 0x%x — stage %s will fail", nm->resource,
		    nm->path, res, pg106_devboot_stage_name(nm->required_for));
	}
	if (fFwPending == 0) {
		fGate->commandWakeup(&fFwPending);
		sStartBoot(this, nullptr, nullptr, nullptr, nullptr);
	}
}

IOReturn
PG106::sStartBoot(OSObject *owner, void *, void *, void *, void *)
{
	PG106 *self = OSDynamicCast(PG106, owner);

	if (!self)
		return kIOReturnBadArgument;
	LOG("fw: %u present, %u missing; starting bring-up",
	    (unsigned)__builtin_popcount(self->fFwPresent),
	    (unsigned)__builtin_popcount(self->fFwMissing));
	self->pmPerform(pg106_pm_start(&self->fPm));
	return kIOReturnSuccess;
}

// ---------------------------------------------------------------------
// Bring-up + power (gate held)
// ---------------------------------------------------------------------

void
PG106::sBootEvent(void *, const struct pg106_devboot_event *e)
{
	IOLog("PG106: boot %-14s %-5s ret=%d a=0x%llx b=0x%llx %s\n",
	      pg106_devboot_stage_name(e->stage), pg106_devboot_ev_name(e->ev),
	      e->ret, (unsigned long long)e->a, (unsigned long long)e->b,
	      e->detail ? e->detail : "");
}

// HS ACR image pages (struct pg106_devboot_hostmem): wired, DMA-mapped.
int
PG106::sHostAlloc(void *, uint64_t size, void **cpu, void **handle)
{
	struct pg106_dma *d = nullptr;
	int ret = pg106_dma_alloc(size, &d);

	if (ret)
		return ret;
	*cpu = pg106_dma_cpu(d);
	*handle = d;
	return 0;
}

int
PG106::sHostBus(void *, void *handle, uint64_t off, uint64_t *addr)
{
	return pg106_dma_addr(static_cast<struct pg106_dma *>(handle), off, addr,
			      nullptr);
}

void
PG106::sHostFree(void *, void *handle)
{
	pg106_dma_free(static_cast<struct pg106_dma *>(handle));
}

static void
bar1IoWr(void *u, uint64_t off, uint32_t v)
{
	static_cast<PG106 *>(u)->bar1Write32(off, v);
}

static uint32_t
bar1IoRd(void *u, uint64_t off)
{
	return static_cast<PG106 *>(u)->bar1Read32(off);
}

void
PG106::bar1Write32(uint64_t off, uint32_t v)
{
	if (!fBar1 || off + 4 > fBar1Len || (off & 3))
		return;
	*reinterpret_cast<volatile uint32_t *>(fBar1 + off) = v;
	__sync_synchronize();
}

uint32_t
PG106::bar1Read32(uint64_t off) const
{
	if (!fBar1 || off + 4 > fBar1Len || (off & 3))
		return 0xffffffffu;
	return *reinterpret_cast<volatile const uint32_t *>(fBar1 + off);
}

void
PG106::intrArm(void)
{
	unsigned i;

	// gp100_mc_intr_unarm state, then every routed unit (display, ...);
	// pg106_gpu_boot adds the FIFO itself.
	pg106_intr_init(&fIntr, &fBus);
	fIntrUp = true;
	for (i = 0; i < sizeof(fRoute) / sizeof(fRoute[0]); i++)
		if (fRoute[i].fn)
			pg106_intr_set(&fIntr, fRoute[i].mask, fRoute[i].fn,
				       fRoute[i].ctx);
}

void
PG106::sNonstall(void *ctx)
{
	PG106 *self = static_cast<PG106 *>(ctx);

	self->fFenceEv++;
	self->fGate->commandWakeup(&self->fFenceEv);
	self->kifEvent();
}

int
PG106::doBoot(void)
{
	struct pg106_gpu_cfg c = {};
	int ret;

	if (fGpuValid) {
		kifLost();
		pg106_gpu_fini(fGpu);
		fGpuValid = false;
	}
	c.bus = &fBus;
	c.vbios = fVbios.data;
	c.vbios_size = fVbios.size;
	c.fw = &fFw;
	c.hostmem = &fHostMem;
	c.acr_vma = 0; // nvkm_vmm_get first fit in a fresh VMM (falcon/fw.c:139)
	c.bar1_size = fBar1 ? fBar1Len : 0;
	c.bar2_size = fBar3Len;
	c.bar1_io.user = this;
	c.bar1_io.wr32 = bar1IoWr;
	c.bar1_io.rd32 = bar1IoRd;
	c.stop_after = fStopAfter;
	c.event = sBootEvent;
	c.sec2_max_polls = 1000;
	c.intr = &fIntr;
	if (!c.bar1_size) {
		LOG("boot: BAR1 not mapped; memory layer and FIFO unavailable");
		c.bar1_size = 0x1000; // keeps devboot running; FIFO init fails
	}
	// No GPU register may be written in the identify/posted safe modes;
	// the interrupt router's init writes 0x180, so it waits for POST.
	if (fStopAfter > PG106_DB_POSTED) {
		intrArm();
		// gf100_fb_sysmem_flush_page_init (fb/gf100.c:81-87): one host
		// page the GPU flushes through.
		if (!fFlushPage && pg106_dma_alloc(PAGE_SIZE, &fFlushPage))
			fFlushPage = nullptr;
		if (fFlushPage) {
			uint64_t a = 0;

			if (pg106_dma_addr(fFlushPage, 0, &a, nullptr) == 0)
				c.sysmem_flush_bus = a;
		}
	}
	ret = pg106_gpu_boot(fGpu, &c);
	fGpuValid = true;
	fVramSize = fGpu->vram_size;
	if (fGpu->fifo_up)
		pg106_gpu_set_nonstall_cb(fGpu, sNonstall, this);
	LOG("boot: ret %d, stage %s, VRAM %llu MiB, BAR1/BAR2 %s/%s, FIFO %s, "
	    "PMC interrupts routed 0x%08x",
	    ret, pg106_devboot_stage_name(fGpu->stage),
	    (unsigned long long)(fVramSize >> 20), fGpu->bar1_up ? "up" : "down",
	    fGpu->bar2_up ? "up" : "down", fGpu->fifo_up ? "up" : "down",
	    fIntrUp ? fIntr.allowed : 0);
	if (fGpu->fifo_up && fSelftest)
		runSelftest();
	if (!ret && fStopAfter < PG106_DB_READY)
		LOG("boot: safe mode, stopped after %s",
		    pg106_devboot_stage_name(fStopAfter));
	return ret;
}

void
PG106::runSelftest(void)
{
	struct pg106_gpu_selftest st;
	int ret = pg106_gpu_selftest(fGpu, 1000000, &st);

	LOG("selftest: ret %d%s%s; CE runlist %d chid %u; host semaphore 0x%08x "
	    "after %llu us; CE semaphore 0x%08x after %llu us; %u bytes copied, %u "
	    "words differ; GP_GET %u; PFIFO_INTR 0x%08x; non-stall IRQs %llu",
	    ret, st.failed_at ? " at: " : "", st.failed_at ? st.failed_at : "",
	    st.runlist, st.chid, st.sem_value, (unsigned long long)st.sem_wait_us,
	    st.ce_sem_value, (unsigned long long)st.ce_wait_us, st.ce_bytes,
	    st.ce_mismatch, st.gp_get, st.pfifo_intr,
	    (unsigned long long)st.nonstall_events);
}

int
PG106::doPowerOff(void)
{
	IOReturn r;

	if (!fAcpiPort || fAcpiPort->validateObject(PG106_ACPI_PWR_OFF) != kIOReturnSuccess)
		return -ENODEV;
	// Everything in VRAM is lost: display and accelerator let go first.
	dispPower(PG106_DISP_POWER_WILL_OFF);
	kifLost();
	if (fIntrUp)
		pg106_intr_unarm(&fIntr);
	fPCI->saveDeviceState();
	r = fAcpiPort->evaluateObject(PG106_ACPI_PWR_OFF, nullptr, nullptr, 0, 0);
	if (r != kIOReturnSuccess) {
		LOG("power: %s failed 0x%x", PG106_ACPI_PWR_OFF, r);
		fPCI->restoreDeviceState();
		return -EIO;
	}
	LOG("power: %s done (D3cold)", PG106_ACPI_PWR_OFF);
	return 0;
}

int
PG106::doPowerOn(void)
{
	IOReturn r;
	uint32_t boot0;

	if (!fAcpiPort)
		return -ENODEV;
	r = fAcpiPort->evaluateObject(PG106_ACPI_PWR_ON, nullptr, nullptr, 0, 0);
	if (r != kIOReturnSuccess) {
		LOG("power: %s failed 0x%x", PG106_ACPI_PWR_ON, r);
		return -EIO;
	}
	// PCIe: 100 ms from power-up before config requests (Base spec 6.6.1).
	IOSleep(100);
	fPCI->restoreDeviceState();
	fPCI->setMemoryEnable(true);
	fPCI->setBusMasterEnable(true);
	boot0 = rd32(PG106_PMC_BOOT_0);
	LOG("power: %s done, BOOT_0 %08x", PG106_ACPI_PWR_ON, boot0);
	return boot0 == fPmc.boot0 ? 0 : -EIO;
}

void
PG106::pmPerform(uint8_t action)
{
	while (action != PG106_PM_ACT_NONE) {
		int ret;

		LOG("pm: %s -> %s", pg106_pm_state_name(fPm.state),
		    pg106_pm_action_name(action));
		switch (action) {
		case PG106_PM_ACT_BOOT:
			ret = doBoot();
			if (!ret)
				dispPower(PG106_DISP_POWER_DID_ON);
			break;
		// Sessions are 0 when the policy asks: every client's channel
		// work has been drained by kdisp's teardown already.
		case PG106_PM_ACT_QUIESCE: ret = 0; break;
		case PG106_PM_ACT_POWER_OFF: ret = doPowerOff(); break;
		case PG106_PM_ACT_POWER_ON: ret = doPowerOn(); break;
		default: ret = -EINVAL; break;
		}
		action = pg106_pm_done(&fPm, action, ret, nowMs());
		if (ret)
			LOG("pm: action failed %d -> state %s", ret,
			    pg106_pm_state_name(fPm.state));
	}
	// Registry readiness for PG106Accel (pascal/kext/README.md): true
	// only while bring-up reached READY and the GPU is up.
	setProperty("PG106Ready", gpuReady() ? kOSBooleanTrue : kOSBooleanFalse);
	if (fPm.state == PG106_PM_FAILED)
		LOG("pm: FAILED — render sessions are refused until the kext is reloaded");
	else if (pg106_pm_ready(&fPm))
		LOG("pm: ready (generation %u)", fPm.generation);
	fGate->commandWakeup(&fPm);
	armIdleTimer();
}

void
PG106::armIdleTimer(void)
{
	uint32_t ms = pg106_pm_timer_ms(&fPm, nowMs());

	if (!fIdleTimer)
		return;
	fIdleTimer->cancelTimeout();
	if (ms != UINT32_MAX)
		fIdleTimer->setTimeoutMS(ms ? ms : 1);
}

void
PG106::sIdleTimer(OSObject *owner, IOTimerEventSource *)
{
	PG106 *self = OSDynamicCast(PG106, owner);

	// Timer actions run on the work loop, i.e. with the gate held.
	if (self && !self->fStopping)
		self->pmPerform(pg106_pm_tick(&self->fPm, self->nowMs()));
}

int
PG106::openSession(void)
{
	uint8_t action = PG106_PM_ACT_NONE;
	uint8_t r;

	// Bring-up waits for the firmware; so does a session opened that
	// early. commandSleep drops the gate while asleep.
	while (fFwPending && !fStopping) {
		AbsoluteTime deadline;

		clock_interval_to_deadline(30, kSecondScale, &deadline);
		if (fGate->commandSleep(&fFwPending, deadline, THREAD_UNINT) ==
		    THREAD_TIMED_OUT)
			return -ETIMEDOUT;
	}
	r = pg106_pm_open(&fPm, nowMs(), &action);
	if (r == PG106_PM_OPEN_FAILED)
		return -ENODEV;
	pmPerform(action);
	while (!pg106_pm_ready(&fPm) && fPm.state != PG106_PM_FAILED && !fStopping) {
		AbsoluteTime deadline;

		clock_interval_to_deadline(30, kSecondScale, &deadline);
		if (fGate->commandSleep(&fPm, deadline, THREAD_UNINT) == THREAD_TIMED_OUT)
			break;
	}
	if (!pg106_pm_ready(&fPm)) {
		pg106_pm_close(&fPm, nowMs());
		return -ENODEV;
	}
	return 0;
}

void
PG106::closeSession(void)
{
	pg106_pm_close(&fPm, nowMs());
	armIdleTimer();
}

bool
PG106::gpuReady(void) const
{
	return pg106_pm_ready(&fPm) && fGpuValid && pg106_gpu_ready(fGpu);
}

int
PG106::claimChannel(const void *owner)
{
	if (fChannelOwner && fChannelOwner != owner)
		return -EBUSY;
	fChannelOwner = owner;
	// NVK's channel runs on GR's runlist (PASCAL_COMPUTE_B/PASCAL_A),
	// which needs a GR context per channel: the golden context image
	// (gf100_grctx_generate + gp100/gp102 grctx tables) and its bind at
	// inst+0x210 (gk104_ectx_bind). That is not ported, so the render
	// channel stays unbound: VM_INIT succeeds, EXEC answers -ENODEV
	// (kdisp.h chan_bind contract). Copy-engine channels need no context
	// and do run (pg106_gpu_selftest, PG106_kif.cpp).
	return -ENOSYS;
}

void
PG106::releaseChannel(const void *owner)
{
	if (fChannelOwner == owner)
		fChannelOwner = nullptr;
}

void
PG106::kdEnvBase(struct pg106_kd_env *env) const
{
	const struct pg106_devboot *db = fGpuValid ? &fGpu->db : nullptr;
	const struct pg106_grboot_config *g = db ? &db->gr_cfg : nullptr;

	env->pci_vendor = 0x10de;
	env->pci_device = fDeviceId;
	env->vram_bar_size = fBar1Len;
	env->vram_size = fVramSize;
	env->boot0 = fPmc.boot0;
	// gf100_gr_units (gr/gf100.c:1161-1171).
	env->graph_units = db ? ((uint64_t)g->gpc_nr |
				 ((uint64_t)db->gr_info.tpc_total << 8) |
				 ((uint64_t)db->gr_info.rop_nr << 32))
			      : 0;
	env->ib_max = PG106_IB_MAX;
	env->drm_version = PG106_DRM_VERSION;
	// The user client places each client VM's tables in VRAM it
	// allocates per connection (PG106UserClient::sOpen).
	env->mmu_cfg.table_target = PG106_TARGET_VRAM;
}

void
PG106::statusSnapshot(struct pg106_uc_status *s) const
{
	memset(s, 0, sizeof(*s));
	s->pm_state = fPm.state;
	s->boot_stage = fGpuValid ? fGpu->stage : 0;
	s->boot_ret = fGpuValid ? (uint32_t)fGpu->ret : 0;
	s->boot_step = fGpuValid && fGpu->ret ? (uint32_t)fGpu->db.exec.step : 0;
	s->generation = fPm.generation;
	s->power_cycles = fPm.power_cycles;
	s->sessions = fPm.sessions;
	s->vbios_size = fVbios.size;
	s->vbios_source = fVbiosSource;
	s->fw_present = fFwPresent;
	s->fw_missing = fFwMissing;
	s->vram_mib = (uint32_t)(fVramSize >> 20);
	s->bar1_mib = (uint32_t)(fBar1Len >> 20);
	s->flags = (gpuReady() ? PG106_UC_ST_READY : 0u) |
		   (fPm.d3cold ? PG106_UC_ST_D3COLD : 0u) |
		   (fPm.d3cold_broken ? PG106_UC_ST_D3COLD_OFF : 0u) |
		   (fChannelOwner ? PG106_UC_ST_CHANNEL_HELD : 0u) |
		   (fAcpi ? PG106_UC_ST_ACPI : 0u) |
		   (fRenderUsers ? PG106_UC_ST_RENDER_USERS : 0u);
}

// ---------------------------------------------------------------------
// Interrupts: MSI (or the legacy line) through an
// IOFilterInterruptEventSource. The filter runs in primary interrupt
// context: it checks PMC_INTR_0 against what is routed and, if anything
// is pending, blocks everything (gp100_mc_intr_unarm) and schedules the
// action. The action runs on the work loop (gate held): it calls the
// routed handlers (FIFO non-stall -> fence waiters + PG106Accel; display)
// and re-arms (gp100_mc_intr_rearm). nouveau's split is the same:
// nvkm_intr (subdev/mc + core/intr.c).
// ---------------------------------------------------------------------

void
PG106::setupInterrupts(void)
{
	int idx = 0, type = 0, i;

	// Prefer an MSI source (nouveau enables MSI on GP10x: nvkm_pci msi).
	for (i = 0; i < 8; i++) {
		if (fPCI->getInterruptType(i, &type) != kIOReturnSuccess)
			break;
		if (type & kIOInterruptTypePCIMessaged) {
			idx = i;
			break;
		}
	}
	fIntrSrc = IOFilterInterruptEventSource::filterInterruptEventSource(
		this, sIntrAction, sIntrFilter, fPCI, idx);
	if (!fIntrSrc || fWorkLoop->addEventSource(fIntrSrc) != kIOReturnSuccess) {
		LOG("intr: no interrupt source (index %d); fences fall back to polling",
		    idx);
		OSSafeReleaseNULL(fIntrSrc);
		return;
	}
	fIntrAdded = true;
	fIntrSrc->enable();
	LOG("intr: %s source %d", (type & kIOInterruptTypePCIMessaged) ? "MSI" : "legacy",
	    idx);
}

bool
PG106::sIntrFilter(OSObject *owner, IOFilterInterruptEventSource *)
{
	PG106 *self = static_cast<PG106 *>(owner);

	if (!self->fIntrUp || !self->fRegs || !pg106_intr_pending(&self->fIntr))
		return false;
	pg106_intr_unarm(&self->fIntr);
	return true;
}

void
PG106::sIntrAction(OSObject *owner, IOInterruptEventSource *, int)
{
	PG106 *self = OSDynamicCast(PG106, owner);

	if (!self || !self->fIntrUp)
		return;
	self->fIntrCount++;
	pg106_intr_service(&self->fIntr);
	pg106_intr_rearm(&self->fIntr);
}

int
PG106::intrRoute(uint32_t mask, pg106_intr_fn fn, void *ctx)
{
	unsigned i, slot = 4;

	for (i = 0; i < 4; i++) {
		if (fRoute[i].fn && fRoute[i].mask == mask) {
			slot = i;
			break;
		}
		if (!fRoute[i].fn && slot == 4)
			slot = i;
	}
	if (slot == 4)
		return -ENOSPC;
	if (fIntrUp)
		pg106_intr_set(&fIntr, mask, nullptr, nullptr);
	fRoute[slot].mask = fn ? mask : 0;
	fRoute[slot].fn = fn;
	fRoute[slot].ctx = fn ? ctx : nullptr;
	if (fn && fIntrUp)
		return pg106_intr_set(&fIntr, mask, fn, ctx);
	return 0;
}

void
PG106::fenceWait(uint32_t us)
{
	AbsoluteTime deadline;
	uint32_t seen = fFenceEv;

	if (!fIntrSrc || !fIntrUp) {
		IOSleep(us >= 1000 ? us / 1000 : 1);
		return;
	}
	clock_interval_to_deadline(us ? us : 1, kMicrosecondScale, &deadline);
	while (fFenceEv == seen && !fStopping)
		if (fGate->commandSleep(&fFenceEv, deadline, THREAD_ABORTSAFE) !=
		    THREAD_AWAKENED)
			break;
}

// ---------------------------------------------------------------------
// GPU memory services (gate held)
// ---------------------------------------------------------------------

bool
PG106::memUp(void) const
{
	return fGpuValid && fGpu->vram_up && fGpu->bar1_up;
}

int
PG106::vramAlloc(uint64_t size, uint64_t align, uint32_t flags, uint64_t *pa)
{
	if (!memUp())
		return -ENODEV;
	return pg106_vram_alloc(&fGpu->vram, size, align, flags, pa);
}

void
PG106::vramFree(uint64_t pa, uint64_t size)
{
	if (memUp())
		pg106_vram_free(&fGpu->vram, pa, size);
}

int
PG106::vramWrite(uint64_t pa, const void *src, uint32_t len)
{
	if (!memUp())
		return -ENODEV;
	return pg106_vram_write(&fGpu->vram, pa, src, len);
}

uint32_t
PG106::vramRd32(uint64_t pa)
{
	if (!memUp())
		return 0xffffffffu;
	return pg106_vram_rd32(&fGpu->vram, pa);
}

int
PG106::bar1Map(uint64_t pa, uint64_t size, uint64_t *off)
{
	if (!memUp())
		return -ENODEV;
	return pg106_bar_map(&fGpu->bar1, pa, size, PG106_TARGET_VRAM, off);
}

void
PG106::bar1Unmap(uint64_t off, uint64_t size)
{
	if (memUp())
		pg106_bar_unmap(&fGpu->bar1, off, size);
}

int
PG106::vmFlush(struct pg106_mmu *mmu, uint64_t base, uint64_t size)
{
	if (!memUp())
		return -ENODEV;
	return pg106_mmu_upload(&fGpu->vram, mmu, base, size, true);
}

// ---------------------------------------------------------------------
// IOService::callPlatformFunction: the display host table (disp_host.h)
// and PG106Accel's interface (pg106_kif.h)
// ---------------------------------------------------------------------

IOReturn
PG106::callPlatformFunction(const OSSymbol *functionName, bool waitForFunction,
			    void *param1, void *param2, void *param3, void *param4)
{
	const char *name = functionName ? functionName->getCStringNoCopy() : nullptr;

	if (name && !strcmp(name, PG106_DISP_HOST_FUNCTION)) {
		if ((uintptr_t)param2 != PG106_DISP_HOST_ABI || !param1)
			return kIOReturnUnsupported;
		dispFill(static_cast<struct pg106_disp_host *>(param1));
		return kIOReturnSuccess;
	}
	if (name && !strncmp(name, "org.helios300.pg106.kif.", 24))
		return kifCall(name, param1, (uintptr_t)param2);
	return super::callPlatformFunction(functionName, waitForFunction, param1,
					   param2, param3, param4);
}
