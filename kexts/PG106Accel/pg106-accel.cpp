/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

// PG106Accel: the IOAccelerator for the Pascal GP106 (GTX 1060 Mobile) that the PG106 kext drives.
//
// Metal.framework only makes an MTLDevice for an IOAccelerator that names a Metal plugin (MetalPluginName /
// MetalPluginClassName). The plugin's -initWithAcceleratorPort: passes that accelerator to Apple's MTLIOAccelDevice,
// which opens the family's user clients on it, so the accelerator has to be a working IOGraphicsAccelerator2. That
// is the only reason this kext exists. The GPU work does not go through it: the plugin runs NVK, and NVK's pg106
// backend talks to the PG106 render user client (nvk/pascal, pascal/bridge). Nothing here touches the GPU.
//
// It is NVAccel (kexts/NVRM/accel) with every NVRM, display, flip, VRAM, GPU VA, surface-share, temperature and
// utilisation path removed. What is left, and why:
//   - the IOGraphicsAccelerator2 subclass and the factories the family declares pure: the family cannot start
//     without them (re/IOGraphicsAccelerator2.h lists the 17);
//   - stamp memory plus an event machine whose stamps the CPU writes at once: no family command stream ever reaches
//     the GP106, so every family event has to complete when it is issued, not wait for a GPU that never sees it;
//   - enableAccelerator() at start: IOAccelSharedUserClient2::new_resource sleeps until it has run
//     (re/IOGraphicsAccelerator2.h, acceleratorWaitEnabled), and NVAccel's only other caller of it is a display
//     mode change, which never happens here;
//   - a GPU task with its own address bookkeeping, and memory maps whose page-table calls succeed without doing
//     anything: the family gives every resource it maps a GPU address, but the GP106's page tables belong to PG106
//     and NVK, so these addresses are placeholders that are never written into the GPU's MMU;
//   - system-memory resources only: the family's video memory (VRAM) is refused, because only PG106 can allocate
//     GP106 VRAM and it offers no way to do that through IOKit. The plugin's Pascal path renders through Vulkan and
//     copies frames into the IOSurface's system memory (OFFLOAD-DESIGN.md §2.1), so it never needs family VRAM;
//   - a display machine that refuses every framebuffer, and no display pipe: the Intel HD 630 and WindowServer own
//     every display (OFFLOAD-DESIGN.md §1). This accelerator never drives a display, and nothing here can make it
//     WindowServer's GPU. The plugin also refuses WindowServer on Pascal (plugin/NVMTLDevice.m).
//
// PG106 coupling, through the I/O registry only (PG106 is GPL-2.0; nothing of it is included or linked here):
//   - MetalPluginName stays withheld until a PG106 service on the same PCI device has published *and* reported
//     PG106Ready = true once, and is withdrawn again if that service terminates. Until bring-up has finished, no
//     Metal process sees the GP106, so none of them loads the plugin for it or creates family objects here. The
//     readiness is latched: PG106Ready goes false again while the GPU is powered off (D3cold), and PG106 boots it
//     again when a render session opens, so taking the name away then would hide the GPU from exactly the process
//     that would wake it. The kernel has no property-change notification, so PG106Ready is polled every 2 s while
//     it is false. A PG106 without the property (older than 87630db) is taken as ready, as before;
//   - render access follows PG106's policy. Open to every local user by default; pg106_render_users=0 (or
//     PG106RenderUsers = false in PG106's personality) restricts it to administrators. PG106 enforces it on its
//     render user client; this accelerator refuses its own user clients to the same processes, so a restricted user
//     does not see a Metal device that cannot render.
//
// Boot-arg -pg106acceloff: do not start (a rollback that needs no uninstall).

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IORangeAllocator.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>
// Reconstructed IOAcceleratorFamily2 headers (Apple ships none). They are NVAccel's, in the same PolyForm tree, and
// shared rather than copied so the two accelerators always agree on the family's layout.
#include "IOAccelerator.h"
#include "IOGraphicsAccelerator2.h"
#include "IOAccelEventMachineFast2.h"
#include "IOAccelLegacyDisplayMachine.h"
#include "IOAccelTask.h"
#include "IOAccelMemoryMap.h"
#include "IOAccelResource2.h"
#include "IOAccelSysMemory.h"
#include "IOAccelVidMemory.h"
#include "IOAccelLegacySurface.h"
#include "IOAccelGLContext2.h"
#include "IOAccel2DContext2.h"
#include "IOAccelCLContext2.h"
#include "IOAccelVideoContext2.h"
#include "pga_tahoe_fwd.h"   // macOS 26: inherited slots Apple no longer exports, forwarded through its vtables

#define PGA_LOG(fmt, ...) IOLog("PG106Accel: " fmt "\n", ##__VA_ARGS__)
// Logs that a process can trigger at will are capped, so a stuck client cannot fill the system log.
#define PGA_LOG_CAPPED(counter, cap, fmt, ...) do { if ((counter) < (cap)) { (counter)++; PGA_LOG(fmt, ##__VA_ARGS__); } } while (0)

// kmutil places a kext in a kernel collection only with a _kmod_info symbol (Xcode generates it; these builds do not)
extern "C" kern_return_t _start(kmod_info_t *ki, void *d);
extern "C" kern_return_t _stop(kmod_info_t *ki, void *d);
extern "C" { KMOD_EXPLICIT_DECL(com.nullmoth.PG106Accel, "0.1", _start, _stop) }
extern "C" kern_return_t _start(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
extern "C" kern_return_t _stop(kmod_info_t *ki, void *d)  { return KERN_SUCCESS; }

static const unsigned kPgaStamps = 4096;   // 16 KB of 32-bit stamps, NVAccel's size
static const unsigned kPgaReadyPollMs = 2000;   // chosen: a Metal app started during bring-up waits at most this long

class PG106Accel;
static PG106Accel *gPga;                   // one GP106 per machine; the event machine writes its stamps through it
static void pgaWriteStamp(int index, unsigned int value);

// ---- event machine: every family stamp is written by the CPU when it is issued --------------------------------
class PG106AccelEventMachine : public IOAccelEventMachineFast2 {
    OSDeclareDefaultStructors(PG106AccelEventMachine)
    PGA_FWD(PG106AccelEventMachine)
public:
    unsigned fLogged = 0;
    void writeStamp(int s, vendevtCommandRec *r, unsigned int v) APPLE_KEXT_OVERRIDE { (void)r; pgaWriteStamp(s, v); }
    void prepareBarrier(vendevtBarrierRec *) APPLE_KEXT_OVERRIDE {}
    void completeBarrier(vendevtBarrierRec *) APPLE_KEXT_OVERRIDE {}
    void writeBarrierElement(vendevtBarrierRec *, int, unsigned int) APPLE_KEXT_OVERRIDE {}
    bool checkGPUProgress() APPLE_KEXT_OVERRIDE { return true; }
    bool checkChannelProgress(int) APPLE_KEXT_OVERRIDE { return true; }
    void *eventTimeout(int stamp) APPLE_KEXT_OVERRIDE {
        PGA_LOG_CAPPED(fLogged, 8, "event timeout on stamp %d ignored: no family channel runs on the GP106", stamp);
        return nullptr;
    }
};
OSDefineMetaClassAndStructors(PG106AccelEventMachine, IOAccelEventMachineFast2)

// ---- display machine: no framebuffer ever joins this accelerator ---------------------------------------------
// IOAccelDisplayMachine::start(IOPCIDevice *) sweeps the PCI device for framebuffers and adds each one through the
// virtual found_framebuffer. The GP106 has no IOFramebuffer (PG106 publishes none, and its only output, HDMI, is
// not driven), so the sweep should find nothing; refusing here makes sure that an accident cannot give WindowServer
// a display pipe on this GPU.
class PG106AccelDisplayMachine : public IOAccelLegacyDisplayMachine {
    OSDeclareDefaultStructors(PG106AccelDisplayMachine)
    PGA_FWD(PG106AccelDisplayMachine)
public:
    bool displayModeWillChange() APPLE_KEXT_OVERRIDE { return true; }
    bool displayModeDidChange() APPLE_KEXT_OVERRIDE { return true; }
    void found_framebuffer(IOFramebuffer *fb) APPLE_KEXT_OVERRIDE {
        PGA_LOG("framebuffer %p refused: the GP106 drives no display (the Intel GPU owns them all)", fb);
    }
};
OSDefineMetaClassAndStructors(PG106AccelDisplayMachine, IOAccelLegacyDisplayMachine)

// ---- GPU address bookkeeping ----------------------------------------------------------------------------------
// The family's own IOAccelTask::allocate picks one of the task's range allocators by an index stored in the memory
// map (+0x78), so one allocator would not do. Allocating here, from one locked allocator, avoids depending on how
// many ranges the family expects; NVAccel overrides the same two methods (with RM).
static IORangeAllocator *pgaVaRange();
class PG106AccelTask : public IOAccelTask {
    OSDeclareDefaultStructors(PG106AccelTask)
    PGA_FWD(PG106AccelTask)
public:
    unsigned fLogged = 0;
    unsigned long long allocate(const IOAccelMemoryMap *map) APPLE_KEXT_OVERRIDE {
        IORangeAllocator *ra = pgaVaRange();
        if (!ra || !map) return 0;
        const unsigned long long len = ((IOAccelMemoryMap *)map)->getLength();
        // alignment: the family's own allocate passes the map's +0x80 to IORangeAllocator::allocate
        const unsigned long long align = *(const volatile unsigned long long *)((const unsigned char *)map + 0x80);
        IORangeScalar at = 0;
        if (!len || !ra->allocate(len, &at, align)) {
            PGA_LOG_CAPPED(fLogged, 8, "no placeholder GPU address for %llu bytes (align %llu)", len, align);
            return 0;
        }
        return at;
    }
    void deallocate(const IOAccelMemoryMap *map, unsigned long long addr) APPLE_KEXT_OVERRIDE {
        IORangeAllocator *ra = pgaVaRange();
        if (ra && map && addr) ra->deallocate(addr, ((IOAccelMemoryMap *)map)->getLength());
    }
};
OSDefineMetaClassAndStructors(PG106AccelTask, IOAccelTask)

// The placeholder address is never written into the GP106's page tables, so there is nothing to commit or release.
class PG106AccelMemoryMap : public IOAccelMemoryMap {
    OSDeclareDefaultStructors(PG106AccelMemoryMap)
    PGA_FWD(PG106AccelMemoryMap)
public:
    bool commitIntoGPUPageTable() APPLE_KEXT_OVERRIDE { return true; }
    void releaseFromGPUPageTable() APPLE_KEXT_OVERRIDE {}
    bool updateGPUPageTable() APPLE_KEXT_OVERRIDE { return true; }
};
OSDefineMetaClassAndStructors(PG106AccelMemoryMap, IOAccelMemoryMap)

// ---- memory and resources: system memory only -----------------------------------------------------------------
class PG106AccelSysMemory : public IOAccelSysMemory {
    OSDeclareDefaultStructors(PG106AccelSysMemory)
    PGA_FWD(PG106AccelSysMemory)
};
OSDefineMetaClassAndStructors(PG106AccelSysMemory, IOAccelSysMemory)

static unsigned gPgaVramRefused;
class PG106AccelVidMemory : public IOAccelVidMemory {
    OSDeclareDefaultStructors(PG106AccelVidMemory)
    PGA_FWD(PG106AccelVidMemory)
public:
    bool allocPhysical() APPLE_KEXT_OVERRIDE {
        PGA_LOG_CAPPED(gPgaVramRefused, 8, "video memory refused: GP106 VRAM belongs to PG106 (system memory only here)");
        return false;
    }
    void deallocPhysical() APPLE_KEXT_OVERRIDE {}
    uint64_t getPhysicalSegment(unsigned long long, unsigned long long *len) APPLE_KEXT_OVERRIDE { if (len) *len = 0; return 0; }
};
OSDefineMetaClassAndStructors(PG106AccelVidMemory, IOAccelVidMemory)

class PG106AccelResource : public IOAccelResource2 {
    OSDeclareDefaultStructors(PG106AccelResource)
    PGA_FWD(PG106AccelResource)
public:
    void rebuildPagingBuffer() APPLE_KEXT_OVERRIDE {}
    // Mip/slice offsets inside the resource's backing, from the family's own layout fields (NVAccel's NVResource
    // reads the same ones: +0xb8 pitch, +0xc0 backing pitch, +0xae base level, +0xd0 slice size).
    uint64_t getLevelOffset(unsigned char slice, unsigned char level, int *pitch) APPLE_KEXT_OVERRIDE {
        return levelOffset(slice, level, pitch, 0xb8);
    }
    uint64_t getBackingLevelOffset(unsigned char slice, unsigned char level, int *pitch) APPLE_KEXT_OVERRIDE {
        return levelOffset(slice, level, pitch, 0xc0);
    }
    // No VRAM copy of an IOSurface is ever made on this GPU.
    uint64_t calculateIOSurfaceDeviceCacheVRAMBytes(unsigned long long *bytes, unsigned long long *count) APPLE_KEXT_OVERRIDE {
        if (bytes) *bytes = 0;
        if (count) *count = 0;
        return 0;
    }
    // The aperture is the BAR1 window onto VRAM, which belongs to PG106.
    bool addToAperture() APPLE_KEXT_OVERRIDE { return false; }
    void removeFromAperture() APPLE_KEXT_OVERRIDE {}
    void *getApertureMemoryDescriptor(unsigned long long *off) APPLE_KEXT_OVERRIDE { if (off) *off = 0; return nullptr; }
private:
    uint64_t levelOffset(unsigned char slice, unsigned char level, int *pitch, unsigned pitchField) {
        const unsigned char *r = (const unsigned char *)this;
        if (pitch) {
            const unsigned long long p = *(const volatile unsigned long long *)(r + pitchField);
            const unsigned base = r[0xae];
            *pitch = (int)(p >> ((level >= base) ? (unsigned)(level - base) : 0u));
        }
        return (unsigned long long)slice * *(const volatile unsigned long long *)(r + 0xd0);
    }
};
OSDefineMetaClassAndStructors(PG106AccelResource, IOAccelResource2)

// ---- surfaces and contexts: present only because the family declares their factories pure ----------------------
// Legacy surfaces are WindowServer's (GL-era) drawables and the contexts are the family's command submission. Neither
// can do anything on this GPU: the plugin draws with Vulkan through PG106. Each refuses or does nothing, and says so
// in the log (capped).
static unsigned gPgaStubLogs;
#define PGA_STUB(what) PGA_LOG_CAPPED(gPgaStubLogs, 32, "%s: not supported on the GP106 (no family command stream)", what)
class PG106AccelSurface : public IOAccelLegacySurface {
    OSDeclareDefaultStructors(PG106AccelSurface)
    PGA_FWD(PG106AccelSurface)
public:
    void setSyncCommand(unsigned int *, unsigned int &, unsigned int &, _AMDSurfaceSwapSyncOptions const &) const APPLE_KEXT_OVERRIDE { PGA_STUB("surface setSyncCommand"); }
    void _pure335() APPLE_KEXT_OVERRIDE { PGA_STUB("surface slot 335"); }
    bool copyFromBuffer(int, int, int, int, unsigned int, unsigned int, IOAccelEvent *, IOAccelResource2 *, IOAccelSysMemory *, unsigned int, unsigned long long, unsigned int) APPLE_KEXT_OVERRIDE { PGA_STUB("surface copyFromBuffer"); return false; }
    bool copyToBuffer(int, int, int, int, unsigned int, unsigned int, IOAccelEvent *, IOAccelResource2 *, IOAccelSysMemory *, unsigned int, unsigned long long, unsigned int) APPLE_KEXT_OVERRIDE { PGA_STUB("surface copyToBuffer"); return false; }
    void submitSwapCopy(IOAccelEvent *, IOAccelResource2 *, IOAccelResource2 *) APPLE_KEXT_OVERRIDE { PGA_STUB("surface submitSwapCopy"); }
    void shapeSurface(unsigned int, unsigned short, unsigned short) APPLE_KEXT_OVERRIDE { PGA_STUB("surface shapeSurface"); }
    uint32_t getDirtyBufferBitsFromPrivateModeBits(unsigned long long) APPLE_KEXT_OVERRIDE { return 0; }
};
OSDefineMetaClassAndStructors(PG106AccelSurface, IOAccelLegacySurface)

class PG106AccelGLContext : public IOAccelGLContext2 {
    OSDeclareDefaultStructors(PG106AccelGLContext)
    PGA_FWD(PG106AccelGLContext)
public:
    void _pure362() APPLE_KEXT_OVERRIDE { PGA_STUB("GL context slot 362"); }
    void _pure363() APPLE_KEXT_OVERRIDE { PGA_STUB("GL context slot 363"); }
    void _pure364() APPLE_KEXT_OVERRIDE { PGA_STUB("GL context slot 364"); }
    void _pure367() APPLE_KEXT_OVERRIDE { PGA_STUB("GL context slot 367"); }
};
OSDefineMetaClassAndStructors(PG106AccelGLContext, IOAccelGLContext2)

class PG106Accel2DContext : public IOAccel2DContext2 {
    OSDeclareDefaultStructors(PG106Accel2DContext)
    PGA_FWD(PG106Accel2DContext)
public:
    void _pure362() APPLE_KEXT_OVERRIDE { PGA_STUB("2D context slot 362"); }
    void _pure363() APPLE_KEXT_OVERRIDE { PGA_STUB("2D context slot 363"); }
};
OSDefineMetaClassAndStructors(PG106Accel2DContext, IOAccel2DContext2)

class PG106AccelCLContext : public IOAccelCLContext2 {
    OSDeclareDefaultStructors(PG106AccelCLContext)
    PGA_FWD(PG106AccelCLContext)
};
OSDefineMetaClassAndStructors(PG106AccelCLContext, IOAccelCLContext2)

class PG106AccelVideoContext : public IOAccelVideoContext2 {
    OSDeclareDefaultStructors(PG106AccelVideoContext)
    PGA_FWD(PG106AccelVideoContext)
};
OSDefineMetaClassAndStructors(PG106AccelVideoContext, IOAccelVideoContext2)

// ---- the accelerator -------------------------------------------------------------------------------------------
class PG106Accel : public IOGraphicsAccelerator2 {
    OSDeclareDefaultStructors(PG106Accel)
    PGA_FWD(PG106Accel)
public:
    IOBufferMemoryDescriptor *fStamps = nullptr;
    // Every task gets the same placeholder range (see PG106AccelTask). The family takes an array of allocators.
    enum { kRanges = 32 };
    IORangeAllocator *fVa = nullptr;
    IORangeAllocator *fRanges[kRanges] = {};
    IOPCIDevice *fPCI = nullptr;
    OSObject *fMetalPlugin = nullptr;      // the personality's MetalPluginName, held back until PG106 publishes
    IOLock *fLock = nullptr;
    IOService *fPG106 = nullptr;           // the PG106 service on fPCI, retained while it is published
    IONotifier *fPubNote = nullptr, *fTermNote = nullptr;
    IOWorkLoop *fPollLoop = nullptr;       // our own loop: the poll must not take the family's gate
    IOTimerEventSource *fPoll = nullptr;   // re-reads PG106Ready while it is false
    bool fOffered = false;                 // MetalPluginName is set (readiness seen once for this fPG106)
    bool fWaitLogged = false;
    unsigned fUcLogged = 0;

    // ---- the family's vendor interface (the pure virtuals in re/IOGraphicsAccelerator2.h) ----
    uint64_t getStampMemory(unsigned int *count) APPLE_KEXT_OVERRIDE {
        if (!fStamps) {
            fStamps = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
                                                                  kPgaStamps * 4, 4096);
            if (fStamps) { fStamps->prepare(); bzero(fStamps->getBytesNoCopy(), kPgaStamps * 4); }
        }
        if (count) *count = fStamps ? kPgaStamps : 0;
        return (uint64_t)fStamps;
    }
    OSObject *newEventMachine() APPLE_KEXT_OVERRIDE { return OSTypeAlloc(PG106AccelEventMachine); }
    OSObject *newGPUTask() {
        PG106AccelTask *t = OSTypeAlloc(PG106AccelTask);
        if (t && !t->init(this, 1, fRanges)) { t->release(); t = nullptr; }
        if (!t) PGA_LOG("GPU task creation failed");
        return t;
    }
    OSObject *createUserGPUTask() APPLE_KEXT_OVERRIDE { return newGPUTask(); }
    OSObject *createKernelGPUTask() APPLE_KEXT_OVERRIDE { return newGPUTask(); }
    // IOAccelConfig: NVAccel's values, which the plugin's MTLIOAccelDevice has run with on every NVIDIA GPU so far.
    // The framebuffer count (+0x28) is left as the family pre-filled it: no framebuffer joins this accelerator
    // anyway (PG106AccelDisplayMachine), and the family's own default is the least surprising value for a field
    // whose other uses are unknown.
    void populateAccelConfig(IOAccelConfig *c) APPLE_KEXT_OVERRIDE {
        uint8_t *p = reinterpret_cast<uint8_t *>(c);
        if (!p || ((uintptr_t)p & 0xFFF) + 0x70 > 0x1000) { PGA_LOG("populateAccelConfig(%p): not touching it", p); return; }
        *(uint64_t *)(p + 0x00) = (uint64_t)(uintptr_t)"NVIDIAAccelerator";
        *(uint32_t *)(p + 0x08) = 0x30844300u;
        *(uint32_t *)(p + 0x0c) = 15u;
        *(uint32_t *)(p + 0x18) = 8u;
        *(uint32_t *)(p + 0x30) = 0x40004000u;   // 16384 x 16384 textures (the GP106 limit; NVK reports the same)
        *(uint32_t *)(p + 0x34) = 0x00008000u;
        *(uint32_t *)(p + 0x38) = 0x00008000u;
        *(uint8_t  *)(p + 0x44) = 0;
        *(uint8_t  *)(p + 0x46) = 1;
        *(uint8_t  *)(p + 0x47) = 1;
        *(uint64_t *)(p + 0x48) = 0x0000001F0000000FULL;
        *(uint64_t *)(p + 0x50) = 0x7FFFFFFF0000001FULL;
        *(uint64_t *)(p + 0x58) = 0x0000000C00000000ULL;
        *(uint32_t *)(p + 0x64) = 0x4Bu;
        PGA_LOG("accelerator config written (framebuffer count left at the family's %u)", *(const uint32_t *)(p + 0x28));
    }
    // PG106 owns the PCI device (BARs, bus mastering, power): this kext touches none of it.
    bool configureDevice(IOPCIDevice *) APPLE_KEXT_OVERRIDE { return true; }
    void teardownDevice(IOPCIDevice *) APPLE_KEXT_OVERRIDE {}
    OSObject *newDisplayMachine() APPLE_KEXT_OVERRIDE { return OSTypeAlloc(PG106AccelDisplayMachine); }
    OSObject *newGLContext() APPLE_KEXT_OVERRIDE { return OSTypeAlloc(PG106AccelGLContext); }
    OSObject *newCLContext() APPLE_KEXT_OVERRIDE { return OSTypeAlloc(PG106AccelCLContext); }
    OSObject *newSurface() APPLE_KEXT_OVERRIDE { return OSTypeAlloc(PG106AccelSurface); }
    OSObject *new2DContext() APPLE_KEXT_OVERRIDE { return OSTypeAlloc(PG106Accel2DContext); }
    OSObject *newVideoContext() APPLE_KEXT_OVERRIDE { return OSTypeAlloc(PG106AccelVideoContext); }
    OSObject *newSysMemory() APPLE_KEXT_OVERRIDE { return OSTypeAlloc(PG106AccelSysMemory); }
    OSObject *newVidMemory() APPLE_KEXT_OVERRIDE { return OSTypeAlloc(PG106AccelVidMemory); }
    OSObject *newResource() APPLE_KEXT_OVERRIDE { return OSTypeAlloc(PG106AccelResource); }
    OSObject *newMemoryMap() APPLE_KEXT_OVERRIDE { return OSTypeAlloc(PG106AccelMemoryMap); }

    // ---- render access: PG106's policy ----
    bool renderUsersOpen() {
        uint32_t v = 0;
        if (PE_parse_boot_argn("pg106_render_users", &v, sizeof v)) return v != 0;
        IOService *pg = nullptr;
        IOLockLock(fLock);
        if ((pg = fPG106)) pg->retain();
        IOLockUnlock(fLock);
        bool open = true;   // the owner's default (2026-11-07)
        if (pg) {
            OSObject *p = pg->copyProperty("PG106RenderUsers");
            open = p != kOSBooleanFalse;
            if (p) p->release();
            pg->release();
        }
        return open;
    }
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler) APPLE_KEXT_OVERRIDE {
        if (!renderUsersOpen() &&
            IOUserClient::clientHasPrivilege(owningTask, kIOClientPrivilegeAdministrator) != kIOReturnSuccess) {
            PGA_LOG_CAPPED(fUcLogged, 16, "user client type %u refused: GP106 rendering is for administrators "
                           "(pg106_render_users=0 / PG106RenderUsers)", (unsigned)type);
            return kIOReturnNotPrivileged;
        }
        return IOGraphicsAccelerator2::newUserClient(owningTask, securityID, type, handler);
    }

    // ---- PG106 publication and readiness ----
    // MetalPluginName is the switch: Metal offers no device for an accelerator without it.
    void setState(const char *st) { setProperty("PG106AccelState", st); }
    void onPublished(IOService *pg) {
        IOLockLock(fLock);
        if (!fPG106) { pg->retain(); fPG106 = pg; fWaitLogged = false; }
        IOLockUnlock(fLock);
        evaluateReady();
    }
    void evaluateReady() {
        IOLockLock(fLock);
        IOService *pg = fPG106;
        if (pg) pg->retain();
        const bool offered = fOffered;
        IOLockUnlock(fLock);
        if (!pg) return;
        OSObject *p = pg->copyProperty("PG106Ready");
        const bool ready = p ? p == kOSBooleanTrue : true;   // no property: a PG106 older than PG106Ready
        if (p) p->release();
        pg->release();
        if (offered) return;
        if (!ready) {
            setState("waiting for PG106Ready");
            if (!fWaitLogged) { fWaitLogged = true; PGA_LOG("PG106 published on %s, bring-up not finished (PG106Ready false): "
                                                            "Metal is not offered yet, re-checking every %u ms", fPCI->getName(), kPgaReadyPollMs); }
            if (fPoll) fPoll->setTimeoutMS(kPgaReadyPollMs);
            return;
        }
        IOLockLock(fLock);
        const bool first = fPG106 && !fOffered;
        if (first) fOffered = true;
        IOLockUnlock(fLock);
        if (!first) return;
        if (fMetalPlugin) setProperty("MetalPluginName", fMetalPlugin);
        setState("Metal offered");
        PGA_LOG("PG106 ready on %s: MetalPluginName set, new Metal processes can find the GP106 (render users: %s)",
                fPCI->getName(), renderUsersOpen() ? "everyone" : "administrators");
    }
    static void pollFired(OSObject *owner, IOTimerEventSource *) { ((PG106Accel *)owner)->evaluateReady(); }
    void withdrawMetal(IOService *pg) {
        IOService *old = nullptr;
        bool wasOffered = false;
        IOLockLock(fLock);
        if (fPG106 == pg) { old = fPG106; fPG106 = nullptr; wasOffered = fOffered; fOffered = false; }
        IOLockUnlock(fLock);
        if (!old) return;
        if (fPoll) fPoll->cancelTimeout();
        removeProperty("MetalPluginName");
        setState("waiting for PG106");
        old->release();
        PGA_LOG("PG106 terminated: %s", wasOffered ? "MetalPluginName withdrawn (processes that already made a device keep "
                                                     "it until they exit)" : "Metal was not offered");
    }
    bool isOurs(IOService *s) { return s && s->getProvider() == fPCI; }
    static bool pg106Published(void *target, void *, IOService *s, IONotifier *) {
        PG106Accel *me = (PG106Accel *)target;
        if (me && me->isOurs(s)) me->onPublished(s);
        return true;
    }
    static bool pg106Terminated(void *target, void *, IOService *s, IONotifier *) {
        PG106Accel *me = (PG106Accel *)target;
        if (me) me->withdrawMetal(s);
        return true;
    }

    // ---- lifecycle ----
    IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE {
        int off = 0;
        if (PE_parse_boot_argn("-pg106acceloff", &off, sizeof off)) { PGA_LOG("-pg106acceloff: not attaching"); return nullptr; }
        return IOGraphicsAccelerator2::probe(provider, score);
    }
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE {
        fPCI = OSDynamicCast(IOPCIDevice, provider);
        if (!fPCI || gPga) { PGA_LOG("start: %s", fPCI ? "a PG106Accel is already running" : "provider is not a PCI device"); return false; }
        fLock = IOLockAlloc();
        fVa = IORangeAllocator::withRange(1ULL << 40, 4096, 0, IORangeAllocator::kLocking);
        if (!fLock || !fVa) { PGA_LOG("start: out of memory"); return false; }
        fVa->allocateRange(0, 0x100000);   // no placeholder address below 1 MB, so none is ever 0
        for (unsigned i = 0; i < kRanges; i++) fRanges[i] = fVa;
        // Held back before the family registers anything, so no process sees a Metal plugin before PG106 is there.
        if (OSObject *mp = getProperty("MetalPluginName")) { mp->retain(); fMetalPlugin = mp; removeProperty("MetalPluginName"); }
        setState("waiting for PG106");
        gPga = this;
        if (!IOGraphicsAccelerator2::start(provider)) { PGA_LOG("IOGraphicsAccelerator2::start failed"); gPga = nullptr; return false; }
        enableAccelerator();
        fPollLoop = IOWorkLoop::workLoop();
        fPoll = IOTimerEventSource::timerEventSource(this, &PG106Accel::pollFired);
        if (!fPollLoop || !fPoll || fPollLoop->addEventSource(fPoll) != kIOReturnSuccess) {
            // without the poll, a PG106 that publishes before it is ready is only re-checked when it publishes again
            PGA_LOG("start: no readiness poll (out of memory)");
            if (fPoll) { fPoll->release(); fPoll = nullptr; }
        }
        registerService();
        if (OSDictionary *m = serviceMatching("PG106")) {
            fTermNote = addMatchingNotification(gIOTerminatedNotification, m, &PG106Accel::pg106Terminated, this, nullptr, 0);
            // delivered at once for a PG106 that published before this kext started
            fPubNote = addMatchingNotification(gIOPublishNotification, m, &PG106Accel::pg106Published, this, nullptr, 0);
            m->release();
        }
        PGA_LOG("up on %s %04x:%04x, headless; %s", fPCI->getName(), fPCI->configRead16(0x00), fPCI->configRead16(0x02),
                getProperty("MetalPluginName") ? "Metal offered" : "Metal not offered yet (waiting for PG106 / PG106Ready)");
        return true;
    }
    void stop(IOService *provider) APPLE_KEXT_OVERRIDE {
        if (fPubNote) { fPubNote->remove(); fPubNote = nullptr; }
        if (fTermNote) { fTermNote->remove(); fTermNote = nullptr; }
        if (fPoll) {
            fPoll->cancelTimeout();
            if (fPollLoop) fPollLoop->removeEventSource(fPoll);
            fPoll->release(); fPoll = nullptr;
        }
        if (fPollLoop) { fPollLoop->release(); fPollLoop = nullptr; }
        removeProperty("MetalPluginName");
        IOService *pg = nullptr;
        if (fLock) { IOLockLock(fLock); pg = fPG106; fPG106 = nullptr; fOffered = false; IOLockUnlock(fLock); }
        if (pg) pg->release();
        IOGraphicsAccelerator2::stop(provider);
        if (gPga == this) gPga = nullptr;
    }
    void free() APPLE_KEXT_OVERRIDE {
        if (fStamps) { fStamps->complete(); fStamps->release(); fStamps = nullptr; }
        if (fVa) { fVa->release(); fVa = nullptr; }
        if (fMetalPlugin) { fMetalPlugin->release(); fMetalPlugin = nullptr; }
        if (fLock) { IOLockFree(fLock); fLock = nullptr; }
        IOGraphicsAccelerator2::free();
    }
};
OSDefineMetaClassAndStructors(PG106Accel, IOGraphicsAccelerator2)

static IORangeAllocator *pgaVaRange() { return gPga ? gPga->fVa : nullptr; }

static void pgaWriteStamp(int index, unsigned int value)
{
    PG106Accel *a = gPga;
    if (!a || !a->fStamps || index < 0 || index >= (int)kPgaStamps) return;
    volatile unsigned int *base = (volatile unsigned int *)a->fStamps->getBytesNoCopy();
    if (base) base[index] = value;
}

PGA_FWD_DEFS
