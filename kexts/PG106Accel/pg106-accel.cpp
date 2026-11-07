/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

// PG106Accel: the IOAccelerator for the Pascal GP106 (GTX 1060 Mobile) that the PG106 kext drives.
//
// Two roles, one kext (research/evidence/OFFLOAD-DESIGN.md §3.3 and §3.4):
//   - render offload (always): Metal.framework makes an MTLDevice only for an IOAccelerator that names a Metal plugin
//     (MetalPluginName / MetalPluginClassName). The plugin's -initWithAcceleratorPort: gives that port to Apple's
//     MTLIOAccelDevice, which opens the family's user clients on it. The plugin renders through NVK, whose pg106
//     backend talks to PG106's render user client (nvk/pascal, pascal/bridge), not through the family;
//   - the HDMI display (owner decision 2026-11-08; boot-arg pg106accel_display=0 turns it off): the GP106's HDMI
//     IOFramebuffer (PG106FB, GPL, pascal/fb) gets this accelerator's display pipe, so WindowServer composites that
//     screen on the GP106. The internal panel stays on the Intel HD 630: it is wired there, and the display machine
//     refuses every framebuffer but PG106FB (pga_logic.h pga_fb_decide).
//
// A frame on HDMI: WindowServer composites into an IOSurface on the GP106's Metal device. The plugin's Pascal path
// renders with NVK and copies the result out to the IOSurface's system memory before the command buffer completes,
// and WindowServer waits for that completion before it swaps, since NVK's pg106 backend takes no kernel surface marks.
// The swap is a display-pipe transaction here, and a worker on this kext's own work loop takes it from there:
//   - PG106's copy engine copies the IOSurface (mapped into the accelerator VM) into a VRAM back buffer;
//   - PG106FB flips the base channel to that buffer at the next vblank;
//   - FLIP_DONE completes the transaction (signalTransactionInterrupt).
// The pipe has a ring of three VRAM buffers (on screen, waiting for vblank, being filled). Without PG106's copy
// channel, the CPU does the copy, through BAR1, into the same ring. Without VRAM through the kif (PG106KifABI /
// CAP_VRAM not published yet), the CPU copies into PG106FB's own scanout through its BAR1 address, after a HOME flip
// (pga_choose_path). Each step down is logged and shown in PG106AccelFramePath.
//
// What else is real now, each part as far as PG106 offers it (pg106_kif.h INFO caps):
//   - VRAM: family video memory is VRAM from PG106 (VRAM_ALLOC, BAR1-mapped); tmpTotalVRAM / totalTextureMemory and
//     VRAM,totalMB report the GP106's VRAM;
//   - GPU addresses: the family's GPU VAs come from PG106's accelerator VM (VA_ALLOC), and VRAM maps are committed
//     into its page tables (MAP). A system-memory map is committed when every segment of it is wired; otherwise it
//     keeps an address with no page behind it, as before (nothing on the GP106 uses family addresses but this kext:
//     the plugin renders in NVK's own address space);
//   - event stamps: written by the GPU, with a semaphore release on PG106's copy channel, after every copy queued
//     before it. Family command streams themselves never run on the GP106 (they carry no Pascal methods), and NVK's
//     work is ordered by the plugin, which completes a command buffer only after NVK's fence. If a GPU stamp does not
//     land, eventTimeout writes it from the CPU;
//   - contexts and legacy surfaces: still stubs. Their pure virtual slots are not reconstructed well enough to submit
//     anything through them (re/IOGraphicsAccelerator2.h), and Metal on this GPU does not use them to render.
//
// PG106 coupling: IOKit only (PG106 and PG106FB are GPL-2.0; nothing of them is included or linked here). The I/O
// registry (PG106Ready, PG106RenderUsers, PG106KifABI, PG106FBKifABI) and the pg106_kif.h calls, which are
// callPlatformFunction on the other side's service. pg106_kif.h is MIT (pascal/bridge/include), and it is the only
// file from pascal/ in this build (cross-build.sh checks both).
//   - MetalPluginName stays withheld until a PG106 service on the same PCI device has published *and* reported
//     PG106Ready = true once, and is withdrawn again if that service terminates. In display mode it is held another
//     PGA_FB_WAIT_MS at most for PG106FB, which publishes right after PG106Ready, so that WindowServer's first look at
//     the GP106 already shows PG106AccelDisplay = true (the plugin gives WindowServer a device only then). Readiness
//     is latched: PG106Ready goes false again during D3cold, and PG106 boots the GPU when a session opens. The kernel
//     has no property-change notification, so PG106Ready is polled every 2 s while it is false. A PG106 without the
//     property is taken as ready;
//   - render access follows PG106's policy (PG106RenderUsers / pg106_render_users).
//
// Boot-args: -pg106acceloff (do not attach); pg106accel_display=0 (headless: no framebuffer, as before 2026-11-08);
// pg106accel_ce=0 (CPU copies instead of the copy engine); pg106accel_gpustamps=0 (CPU stamps);
// pg106accel_vram=0 (no family VRAM); pg106accel_aperture=1 (let the family map VRAM through BAR1, off by default
// as in NVAccel).

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IORangeAllocator.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <mach/kmod.h>
// Reconstructed IOAcceleratorFamily2 headers (Apple ships none). They are NVAccel's, in the same PolyForm tree, and
// shared rather than copied so the two accelerators always agree on the family's layout.
#include "IOAccelerator.h"
#include "IOGraphicsAccelerator2.h"
#include "IOAccelEventMachineFast2.h"
#include "IOAccelLegacyDisplayMachine.h"
#include "IOAccelLegacyDisplayPipe.h"
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
#include "pga_logic.h"       // decisions, host-tested (tests/test_pga_logic.c); includes pg106_kif.h (MIT)
#include "pga_tahoe_fwd.h"   // macOS 26: inherited slots Apple no longer exports, forwarded through its vtables

#define PGA_LOG(fmt, ...) IOLog("PG106Accel: " fmt "\n", ##__VA_ARGS__)
// Logs that a process or a frame can trigger at will are capped, so nothing can fill the system log.
#define PGA_LOG_CAPPED(counter, cap, fmt, ...) do { if ((counter) < (cap)) { (counter)++; PGA_LOG(fmt, ##__VA_ARGS__); } } while (0)

// kmutil places a kext in a kernel collection only with a _kmod_info symbol (Xcode generates it; these builds do not)
extern "C" kern_return_t _start(kmod_info_t *ki, void *d);
extern "C" kern_return_t _stop(kmod_info_t *ki, void *d);
extern "C" { KMOD_EXPLICIT_DECL(com.nullmoth.PG106Accel, "0.2", _start, _stop) }
extern "C" kern_return_t _start(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
extern "C" kern_return_t _stop(kmod_info_t *ki, void *d)  { return KERN_SUCCESS; }

// IOSurface and the display-pipe transaction: the few exported methods used here (IOSurface.kext,
// IOAcceleratorFamily2; the same declarations as NVAccel's).
class IOSurface {
public:
    IOMemoryDescriptor *getMemoryDescriptor() const;
    unsigned long long  getWidth() const;
    unsigned long long  getHeight() const;
    unsigned long long  getBytesPerRow() const;
    unsigned int        getPixelFormat() const;
    unsigned int        getSurfaceID() const;
};
class IOAccelDisplayPipeTransaction2 {
public:
    IOSurface *getPlaneIOSurface(unsigned int, unsigned int) const;
};
// IOAccelDisplayPipe::signalTransactionInterrupt(void *): the family re-checks isTransactionComplete
extern "C" void _ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv(void *pipe, void *arg);

static const unsigned kPgaStamps = 4096;        // 16 KB of 32-bit stamps, NVAccel's size
static const unsigned kPgaReadyPollMs = 2000;   // chosen: a Metal app started during bring-up waits at most this long
static const unsigned kPgaCopyWaitUs = 100000;  // a copy that has not landed in 100 ms: the GPU is stuck, drop the frame
static const unsigned kPgaSrcSlots = 8;         // IOSurfaces kept mapped into the accelerator VM (WindowServer cycles a few)

class PG106Accel;
static PG106Accel *gPga;                   // one GP106 per machine
static void pgaWriteStamp(int index, unsigned int value);
extern "C" boolean_t preemption_enabled(void);      // Mach KPI
extern "C" boolean_t ml_at_interrupt_context(void); // Libkern KPI

// ---- counters (registry PG106AccelStats, refreshed by the 2 s timer) -------------------------------------------
enum {
    kStFrames, kStFlips, kStCopiesCE, kStCopiesCPU, kStCpuFB, kStDropBusy, kStDropGeom, kStDropCopy, kStDropFlip,
    kStFlipLate, kStFlipAborted, kStFlipTimeout, kStStampsGPU, kStStampsCPU, kStStampTimeouts, kStVramAllocs,
    kStVramRefused, kStVaKif, kStVaPlaceholder, kStMapVram, kStMapSys, kStMapSysSkipped, kStMapBudget, kStKifLost, kStCount
};
static const char *const kStNames[kStCount] = {
    "frames", "flips", "copies_ce", "copies_cpu", "frames_cpu_fb", "dropped_busy", "dropped_geometry",
    "dropped_copy", "dropped_flip", "flip_busy", "flip_aborted", "flip_done_timeout", "stamps_gpu", "stamps_cpu",
    "stamp_timeouts", "vram_allocs", "vram_refused", "va_kif", "va_placeholder", "maps_vram", "maps_sysmem",
    "maps_sysmem_unwired", "maps_over_budget", "kif_lost"
};
static SInt64 gSt[kStCount];
#define PGA_COUNT(k) OSAddAtomic64(1, &gSt[k])

// The kif epoch: bumped on EVENT LOST. PG106 then rebuilds its accelerator VM, VA allocator and map table, and its
// VA_FREE / UNMAP work by address, so an address from an older epoch must never be freed or unmapped: it may belong to
// a new allocation by then (VRAM handles never repeat, so those are safe). Every address PG106Accel holds carries the
// epoch it was allocated in.
static volatile SInt32 gKifEpoch;
static inline uint32_t pgaEpoch() { return (uint32_t)gKifEpoch; }
// PG106's tables are fixed (PG106_kif.cpp: 128 VRAM allocations, 256 maps). The family's resources get a share of them,
// so they cannot crowd out the HDMI ring (3 VRAM allocations) and the IOSurface + stamp maps (9 maps).
static const SInt32 kPgaFamilyVramMax = 96, kPgaFamilyMapsMax = 192;
static volatile SInt32 gFamilyVram, gFamilyMaps;
static bool pgaTake(volatile SInt32 *n, SInt32 max)
{
    if (OSIncrementAtomic(n) >= max) { OSDecrementAtomic(n); return false; }
    return true;
}

// ---- pg106_kif.h calls -----------------------------------------------------------------------------------------
template <class T> static inline void pgaKifInit(T &r) { bzero(&r, sizeof r); r.abi = PG106_KIF_ABI; r.size = sizeof r; }
static IOReturn pgaKif(IOService *svc, const char *fn, void *req)
{
    if (!svc) return kIOReturnNotAttached;
    // Every kif call takes PG106's (or PG106FB's) gate and may sleep. The family's calling contexts are not all
    // reconstructed, so a call from where sleeping is not allowed is refused here, and the caller takes its fallback.
    if (!preemption_enabled() || ml_at_interrupt_context()) return kIOReturnNotPermitted;
    const OSSymbol *sym = OSSymbol::withCString(fn);
    if (!sym) return kIOReturnNoMemory;
    IOReturn r = svc->callPlatformFunction(sym, false, req, (void *)(uintptr_t)PG106_KIF_ABI, nullptr, nullptr);
    sym->release();
    return r;
}
static bool pgaReadAbi(IOService *svc, const char *key, uint32_t *abi)
{
    OSObject *o = svc ? svc->copyProperty(key) : nullptr;
    OSNumber *n = OSDynamicCast(OSNumber, o);
    if (n) *abi = n->unsigned32BitValue();
    if (o) o->release();
    return n != nullptr;
}
static bool pgaBootArg(const char *name, uint32_t *v)
{
    uint32_t x = 0;
    if (!PE_parse_boot_argn(name, &x, sizeof x)) return false;
    *v = x;
    return true;
}
static uint64_t pgaNowUs()
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000;
}

// ---- event machine: GPU stamps through PG106's copy channel, CPU stamps otherwise ------------------------------
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
    void *eventTimeout(int stamp) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(PG106AccelEventMachine, IOAccelEventMachineFast2)

// ---- display machine: only the GP106's HDMI framebuffer joins this accelerator ----------------------------------
// IOAccelDisplayMachine::start(IOPCIDevice *) sweeps the PCI device for framebuffers and adds each one through the
// virtual found_framebuffer (vtable +0x8d8, cross-build check 5); PG106Accel itself offers PG106FB the same way when
// it publishes. Both land here, and pga_fb_decide says yes to one PG106FB on this PCI device and to nothing else.
static bool pgaFramebufferAcceptable(IOFramebuffer *fb, bool log);
class PG106AccelDisplayMachine : public IOAccelLegacyDisplayMachine {
    OSDeclareDefaultStructors(PG106AccelDisplayMachine)
    PGA_FWD(PG106AccelDisplayMachine)
public:
    bool displayModeWillChange() APPLE_KEXT_OVERRIDE { return true; }
    bool displayModeDidChange() APPLE_KEXT_OVERRIDE;
    void found_framebuffer(IOFramebuffer *fb) APPLE_KEXT_OVERRIDE {
        if (!pgaFramebufferAcceptable(fb, true)) return;
        IOAccelLegacyDisplayMachine::found_framebuffer(fb);
    }
};
OSDefineMetaClassAndStructors(PG106AccelDisplayMachine, IOAccelLegacyDisplayMachine)

// ---- GPU addresses ---------------------------------------------------------------------------------------------
// The family's own IOAccelTask::allocate picks one of the task's range allocators by an index stored in the memory
// map (+0x78), so it is replaced here (NVAccel overrides the same two methods, with RM). Addresses come from PG106's
// accelerator VM when it offers one (VA_ALLOC), else from a private placeholder range that is never in any page table.
static IORangeAllocator *pgaVaRange();
static uint64_t pgaKifVaAlloc(uint64_t bytes, uint64_t align, uint32_t *epoch);
static void pgaKifVaFree(uint64_t va, uint64_t bytes, uint32_t epoch);
static void pgaKifUnmap(uint64_t va, uint64_t bytes, uint32_t epoch);
class PG106AccelMemoryMap;
class PG106AccelTask : public IOAccelTask {
    OSDeclareDefaultStructors(PG106AccelTask)
    PGA_FWD(PG106AccelTask)
public:
    unsigned fLogged = 0;
    unsigned long long allocate(const IOAccelMemoryMap *map) APPLE_KEXT_OVERRIDE;
    void deallocate(const IOAccelMemoryMap *map, unsigned long long addr) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(PG106AccelTask, IOAccelTask)

class PG106AccelMemoryMap : public IOAccelMemoryMap {
    OSDeclareDefaultStructors(PG106AccelMemoryMap)
    PGA_FWD(PG106AccelMemoryMap)
public:
    IOAccelMemory *fMem = nullptr;
    bool fKifVa = false;        // the address came from PG106's VA_ALLOC
    uint32_t fEpoch = 0;        // ... in this kif epoch
    bool fBudget = false;       // holds one of the family's map slots
    uint64_t fVa = 0, fVaBytes = 0;
    bool fMapped = false;       // PTEs written by PG106 (MAP)
    IOMemoryDescriptor *fSysMd = nullptr;   // MAP_SYSMEM: the physical ranges, prepared until UNMAP
    bool init(IOGraphicsAccelerator2 *a, IOAccelTask *t, IOAccelMemory *m, unsigned int o) APPLE_KEXT_OVERRIDE {
        if (!IOAccelMemoryMap::init(a, t, m, o)) return false;
        fMem = m;
        if (fMem) fMem->retain();
        return true;
    }
    void free() APPLE_KEXT_OVERRIDE {
        releaseFromGPUPageTable();
        if (fMem) { fMem->release(); fMem = nullptr; }
        IOAccelMemoryMap::free();
    }
    bool commitIntoGPUPageTable() APPLE_KEXT_OVERRIDE;
    void releaseFromGPUPageTable() APPLE_KEXT_OVERRIDE;
    bool updateGPUPageTable() APPLE_KEXT_OVERRIDE { return true; }
};
OSDefineMetaClassAndStructors(PG106AccelMemoryMap, IOAccelMemoryMap)

unsigned long long PG106AccelTask::allocate(const IOAccelMemoryMap *map)
{
    if (!map) return 0;
    PG106AccelMemoryMap *mm = OSDynamicCast(PG106AccelMemoryMap, (OSObject *)map);
    const unsigned long long len = ((IOAccelMemoryMap *)map)->getLength();
    // alignment: the family's own allocate passes the map's +0x80 to IORangeAllocator::allocate
    const unsigned long long align = *(const volatile unsigned long long *)((const unsigned char *)map + 0x80);
    if (!len) return 0;
    if (mm) {
        const uint64_t bytes = (len + 4095) & ~4095ull;
        uint32_t ep = 0;
        if (uint64_t va = pgaKifVaAlloc(bytes, align, &ep)) {
            mm->fKifVa = true; mm->fVa = va; mm->fVaBytes = bytes; mm->fEpoch = ep;
            PGA_COUNT(kStVaKif);
            return va;
        }
    }
    IORangeAllocator *ra = pgaVaRange();
    IORangeScalar at = 0;
    if (!ra || !ra->allocate(len, &at, align)) {
        PGA_LOG_CAPPED(fLogged, 8, "no GPU address for %llu bytes (align %llu)", len, align);
        return 0;
    }
    PGA_COUNT(kStVaPlaceholder);
    return at;
}
void PG106AccelTask::deallocate(const IOAccelMemoryMap *map, unsigned long long addr)
{
    if (!map || !addr) return;
    PG106AccelMemoryMap *mm = OSDynamicCast(PG106AccelMemoryMap, (OSObject *)map);
    if (mm && mm->fKifVa) {
        mm->releaseFromGPUPageTable();
        pgaKifVaFree(mm->fVa, mm->fVaBytes, mm->fEpoch);
        mm->fKifVa = false; mm->fVa = mm->fVaBytes = 0;
        return;
    }
    IORangeAllocator *ra = pgaVaRange();
    if (ra) ra->deallocate(addr, ((IOAccelMemoryMap *)map)->getLength());
}

// ---- memory and resources ------------------------------------------------------------------------------------
class PG106AccelSysMemory : public IOAccelSysMemory {
    OSDeclareDefaultStructors(PG106AccelSysMemory)
    PGA_FWD(PG106AccelSysMemory)
};
OSDefineMetaClassAndStructors(PG106AccelSysMemory, IOAccelSysMemory)

// Family video memory: VRAM from PG106, mapped through BAR1 (the family's physical segment is the BAR1 address, as
// in NVAccel). Refused, as before, when PG106 offers no VRAM or BAR1, or with pg106accel_vram=0.
static bool pgaKifVramAlloc(uint64_t bytes, uint32_t flags, struct pg106_kif_vram *out);
static void pgaKifVramFree(uint64_t handle);
static unsigned gPgaVramLogs;
class PG106AccelVidMemory : public IOAccelVidMemory {
    OSDeclareDefaultStructors(PG106AccelVidMemory)
    PGA_FWD(PG106AccelVidMemory)
public:
    uint64_t fHandle = 0, fVramOffset = 0, fBar1 = 0, fBytes = 0, fCookie = 0;
    void *fKva = nullptr;
    bool allocPhysical() APPLE_KEXT_OVERRIDE {
        if (fHandle) return false;   // already backed (AMD's and NVAccel's answer)
        const unsigned long long want = *(const volatile unsigned long long *)((const unsigned char *)this + 0x40);
        struct pg106_kif_vram v;
        const bool budget = want && pgaTake(&gFamilyVram, kPgaFamilyVramMax);
        if (!budget || !pgaKifVramAlloc(want, PG106_KIF_VRAM_CPU, &v)) {
            if (budget) OSDecrementAtomic(&gFamilyVram);
            PGA_COUNT(kStVramRefused);
            PGA_LOG_CAPPED(gPgaVramLogs, 8, "video memory of %llu bytes refused (PG106 offers no VRAM/BAR1 now, or it is full); "
                           "the family keeps the resource in system memory", want);
            return false;
        }
        fHandle = v.handle; fVramOffset = v.vram_offset; fBar1 = v.bar1_phys; fBytes = v.actual_bytes; fCookie = v.cookie;
        fKva = (void *)(uintptr_t)v.kva;
        *(volatile unsigned long long *)((unsigned char *)this + 0xc8) = fBar1;
        PGA_COUNT(kStVramAllocs);
        return true;
    }
    void deallocPhysical() APPLE_KEXT_OVERRIDE {
        if (!fHandle) return;
        pgaKifVramFree(fHandle);
        OSDecrementAtomic(&gFamilyVram);
        fHandle = fVramOffset = fBar1 = fBytes = fCookie = 0; fKva = nullptr;
        *(volatile unsigned long long *)((unsigned char *)this + 0xc8) = 0;
    }
    void free() APPLE_KEXT_OVERRIDE { deallocPhysical(); IOAccelVidMemory::free(); }
    uint64_t getPhysicalSegment(unsigned long long off, unsigned long long *len) APPLE_KEXT_OVERRIDE {
        const unsigned long long total = *(const volatile unsigned long long *)((const unsigned char *)this + 0x40);
        if (!fBar1 || off >= total) { if (len) *len = 0; return 0; }
        if (len) *len = total - off;
        return fBar1 + off;
    }
};
OSDefineMetaClassAndStructors(PG106AccelVidMemory, IOAccelVidMemory)

static bool gPgaAperture;   // pg106accel_aperture=1
class PG106AccelResource : public IOAccelResource2 {
    OSDeclareDefaultStructors(PG106AccelResource)
    PGA_FWD(PG106AccelResource)
public:
    IOMemoryDescriptor *fApDesc = nullptr;
    void rebuildPagingBuffer() APPLE_KEXT_OVERRIDE {}
    // Mip/slice offsets inside the resource's backing, from the family's own layout fields (NVAccel's NVResource
    // reads the same ones: +0xb8 pitch, +0xc0 backing pitch, +0xae base level, +0xd0 slice size).
    uint64_t getLevelOffset(unsigned char slice, unsigned char level, int *pitch) APPLE_KEXT_OVERRIDE {
        return levelOffset(slice, level, pitch, 0xb8);
    }
    uint64_t getBackingLevelOffset(unsigned char slice, unsigned char level, int *pitch) APPLE_KEXT_OVERRIDE {
        return levelOffset(slice, level, pitch, 0xc0);
    }
    // No VRAM copy of an IOSurface is made on this GPU: the plugin's Pascal path renders into the IOSurface's own
    // system memory, and the display pipe copies from there.
    uint64_t calculateIOSurfaceDeviceCacheVRAMBytes(unsigned long long *bytes, unsigned long long *count) APPLE_KEXT_OVERRIDE {
        if (bytes) *bytes = 0;
        if (count) *count = 0;
        return 0;
    }
    // The BAR1 aperture onto a resource's VRAM: closed by default, as NVAccel's (pg106accel_aperture=1 opens it).
    bool addToAperture() APPLE_KEXT_OVERRIDE { PG106AccelVidMemory *m = vidmem(); return gPgaAperture && m && m->fBar1; }
    void removeFromAperture() APPLE_KEXT_OVERRIDE {}
    void *getApertureMemoryDescriptor(unsigned long long *off) APPLE_KEXT_OVERRIDE {
        if (off) *off = 0;
        if (!gPgaAperture) return nullptr;
        if (fApDesc) return fApDesc;
        PG106AccelVidMemory *m = vidmem();
        if (!m || !m->fBar1 || !m->fBytes) return nullptr;
        fApDesc = IODeviceMemory::withRange((IOPhysicalAddress)m->fBar1, (IOPhysicalLength)m->fBytes);
        return fApDesc;
    }
    void free() APPLE_KEXT_OVERRIDE { if (fApDesc) { fApDesc->release(); fApDesc = nullptr; } IOAccelResource2::free(); }
private:
    PG106AccelVidMemory *vidmem() { return OSDynamicCast(PG106AccelVidMemory, *(OSObject **)((unsigned char *)this + 0x88)); }
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

// ---- the display pipe: WindowServer's swaps for the HDMI screen --------------------------------------------------
class PG106AccelDisplayPipe : public IOAccelLegacyDisplayPipe {
    OSDeclareDefaultStructors(PG106AccelDisplayPipe)
    PGA_FWD(PG106AccelDisplayPipe)
public:
    // The family's framebuffer resource stays its default (none), as NVAccel's does unless its scanout gate is
    // opened: WindowServer's frames come through transactions.
    IOReturn validateTransaction(IOAccelDisplayPipeTransaction2 *) APPLE_KEXT_OVERRIDE { return kIOReturnSuccess; }
    IOReturn performTransaction(IOAccelDisplayPipeTransaction2 *txn) APPLE_KEXT_OVERRIDE;
    bool isTransactionComplete(IOAccelDisplayPipeTransaction2 *) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(PG106AccelDisplayPipe, IOAccelLegacyDisplayPipe)

// ---- surfaces and contexts: present only because the family declares their factories pure ----------------------
// Legacy surfaces are WindowServer's (GL-era) drawables and the contexts are the family's command submission. Neither
// can submit anything here: their slots are not reconstructed well enough, and the plugin draws with Vulkan through
// PG106. Each refuses or does nothing, and says so in the log (capped).
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
    unsigned int fStampWant[kPgaStamps] = {};   // the last value asked for each stamp (eventTimeout writes it)
    // Placeholder addresses (no PG106 VM): one range for every task. The family takes an array of allocators.
    enum { kRanges = 32 };
    IORangeAllocator *fVa = nullptr;
    IORangeAllocator *fRanges[kRanges] = {};
    IOPCIDevice *fPCI = nullptr;
    OSObject *fMetalPlugin = nullptr;      // the personality's MetalPluginName, held back until PG106 is ready
    IOLock *fLock = nullptr;               // fPG106, fOffered, fFB*, the kif state
    IOService *fPG106 = nullptr;           // the PG106 service on fPCI, retained while it is published
    IONotifier *fPubNote = nullptr, *fTermNote = nullptr;
    IOWorkLoop *fPollLoop = nullptr;       // our own loop: the poll must not take the family's gate
    IOTimerEventSource *fPoll = nullptr;   // PG106Ready while false; then the HDMI pairing wait and the stats
    bool fOffered = false;                 // MetalPluginName is set (readiness seen once for this fPG106)
    bool fReady = false;                   // readiness seen once
    uint64_t fReadyAtUs = 0;
    bool fWaitLogged = false;
    unsigned fUcLogged = 0;
    // boot-args
    bool fModeDisplay = true, fCeAllowed = true, fGpuStampsAllowed = true, fVramAllowed = true;
    // PG106 kernel interface (pg106_kif.h)
    bool fKifOK = false;                   // PG106 publishes a compatible PG106KifABI and INFO answered ready
    struct pg106_kif_info fInfo = {};
    uint64_t fChan = 0;                    // our copy-engine channel
    bool fChanDead = false;                // EVENT LOST seen
    uint64_t fStampVa = 0;                 // the stamp page in the accelerator VM
    uint32_t fStampEpoch = 0;
    IOMemoryDescriptor *fStampMapped = nullptr;
    unsigned fKifLogs = 0;
    uint32_t fLoggedCaps = 0;
    // the HDMI framebuffer
    IONotifier *fFBPubNote = nullptr, *fFBTermNote = nullptr;
    IOService *fFBCand = nullptr;          // a PG106FB on fPCI that published, retained
    IOService *fFB = nullptr;              // the one that has the display pipe
    IOLock *fPairLock = nullptr;           // serialises pairing (found_framebuffer may sleep in the family)
    bool fFBDecided = false;               // a verdict other than retry-later was reached for fFBCand
    bool fPairFailed = false;
    enum pga_fb_verdict fFBVerdict = PGA_FB_NOT_READY;
    IOAccelDisplayMachine *fDM = nullptr;
    void *fPipe = nullptr;                 // the family's display pipe for fFB

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
    // The framebuffer count (+0x28) is 1 in display mode (the HDMI head; NVAccel writes its head count there) and the
    // family's own default in headless mode.
    void populateAccelConfig(IOAccelConfig *c) APPLE_KEXT_OVERRIDE {
        uint8_t *p = reinterpret_cast<uint8_t *>(c);
        if (!p || ((uintptr_t)p & 0xFFF) + 0x70 > 0x1000) { PGA_LOG("populateAccelConfig(%p): not touching it", p); return; }
        *(uint64_t *)(p + 0x00) = (uint64_t)(uintptr_t)"NVIDIAAccelerator";
        *(uint32_t *)(p + 0x08) = 0x30844300u;
        *(uint32_t *)(p + 0x0c) = 15u;
        *(uint32_t *)(p + 0x18) = 8u;
        if (fModeDisplay) *(uint32_t *)(p + 0x28) = 1u;
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
        PGA_LOG("accelerator config written (framebuffer count %u)", *(const uint32_t *)(p + 0x28));
    }
    // PG106 owns the PCI device (BARs, bus mastering, power): this kext touches none of it.
    bool configureDevice(IOPCIDevice *) APPLE_KEXT_OVERRIDE { return true; }
    void teardownDevice(IOPCIDevice *) APPLE_KEXT_OVERRIDE {}
    OSObject *newDisplayMachine() APPLE_KEXT_OVERRIDE {
        PG106AccelDisplayMachine *d = OSTypeAlloc(PG106AccelDisplayMachine);
        fDM = d;
        return d;
    }
    OSObject *newDisplayPipe() APPLE_KEXT_OVERRIDE { return OSTypeAlloc(PG106AccelDisplayPipe); }
    OSObject *newGLContext() APPLE_KEXT_OVERRIDE { return OSTypeAlloc(PG106AccelGLContext); }
    OSObject *newCLContext() APPLE_KEXT_OVERRIDE { return OSTypeAlloc(PG106AccelCLContext); }
    OSObject *newSurface() APPLE_KEXT_OVERRIDE { return OSTypeAlloc(PG106AccelSurface); }
    OSObject *new2DContext() APPLE_KEXT_OVERRIDE { return OSTypeAlloc(PG106Accel2DContext); }
    OSObject *newVideoContext() APPLE_KEXT_OVERRIDE { return OSTypeAlloc(PG106AccelVideoContext); }
    OSObject *newSysMemory() APPLE_KEXT_OVERRIDE { return OSTypeAlloc(PG106AccelSysMemory); }
    OSObject *newVidMemory() APPLE_KEXT_OVERRIDE { return OSTypeAlloc(PG106AccelVidMemory); }
    OSObject *newResource() APPLE_KEXT_OVERRIDE { return OSTypeAlloc(PG106AccelResource); }
    OSObject *newMemoryMap() APPLE_KEXT_OVERRIDE { return OSTypeAlloc(PG106AccelMemoryMap); }
    // The GP106's VRAM, once PG106 has said how much there is (the family's default before that).
    uint64_t tmpTotalVRAM() APPLE_KEXT_OVERRIDE { return fInfo.vram_bytes ? fInfo.vram_bytes : IOGraphicsAccelerator2::tmpTotalVRAM(); }
    uint64_t totalTextureMemory() APPLE_KEXT_OVERRIDE {
        return fInfo.vram_bytes ? fInfo.vram_bytes : IOGraphicsAccelerator2::totalTextureMemory();
    }

    // ---- render access: PG106's policy ----
    bool renderUsersOpen() {
        uint32_t v = 0;
        if (PE_parse_boot_argn("pg106_render_users", &v, sizeof v)) return v != 0;
        IOService *pg = copyPG106();
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
    IOService *copyPG106() {
        IOLockLock(fLock);
        IOService *pg = fPG106;
        if (pg) pg->retain();
        IOLockUnlock(fLock);
        return pg;
    }
    IOService *copyFB() {
        IOLockLock(fLock);
        IOService *fb = fFB;
        if (fb) fb->retain();
        IOLockUnlock(fLock);
        return fb;
    }

    // ---- calls from PG106 / PG106FB (pg106_kif.h, the PG106ACCEL_KIF_FN_* names) ----
    IOReturn callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4) APPLE_KEXT_OVERRIDE;

    // ---- PG106 kernel interface ----
    void kifAttach();
    void kifDetach();
    bool kifUsable(uint32_t caps) {
        IOLockLock(fLock);
        const bool ok = fKifOK && (fInfo.caps & caps) == caps;
        IOLockUnlock(fLock);
        return ok;
    }
    bool chanUsable() {
        IOLockLock(fLock);
        const bool ok = fKifOK && fChan && !fChanDead;
        IOLockUnlock(fLock);
        return ok;
    }
    void publishVram();

    // ---- the HDMI framebuffer ----
    bool fbAcceptable(IOFramebuffer *fb, bool log);
    void fbPublished(IOService *fb);
    void fbTerminated(IOService *fb);
    void tryPair(const char *why);
    bool pairLocked(IOService *fb);
    enum pga_path framePath(bool *fbKva = nullptr);
    void setDisplayProps(bool attached, const char *path);

    // ---- PG106 publication and readiness ----
    void setState(const char *st) { setProperty("PG106AccelState", st); }
    void onPublished(IOService *pg) {
        IOLockLock(fLock);
        if (!fPG106) { pg->retain(); fPG106 = pg; fWaitLogged = false; }
        IOLockUnlock(fLock);
        evaluateReady();
    }
    void evaluateReady();
    void offerMetal(const char *why);
    void publishStats();
    static void pollFired(OSObject *owner, IOTimerEventSource *) { ((PG106Accel *)owner)->evaluateReady(); }
    void withdrawMetal(IOService *pg);
    bool isOurs(IOService *s) { return s && s->getProvider() == fPCI; }
    bool fbIsOurs(IOService *fb) {
        IOService *p = fb ? fb->getProvider() : nullptr;
        return p && (p == fPCI || p->getProvider() == fPCI);
    }
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
    static bool fbPublishedCb(void *target, void *, IOService *s, IONotifier *) {
        PG106Accel *me = (PG106Accel *)target;
        if (me) me->fbPublished(s);
        return true;
    }
    static bool fbTerminatedCb(void *target, void *, IOService *s, IONotifier *) {
        PG106Accel *me = (PG106Accel *)target;
        if (me) me->fbTerminated(s);
        return true;
    }

    // ---- lifecycle ----
    IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE {
        int off = 0;
        if (PE_parse_boot_argn("-pg106acceloff", &off, sizeof off)) { PGA_LOG("-pg106acceloff: not attaching"); return nullptr; }
        return IOGraphicsAccelerator2::probe(provider, score);
    }
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(PG106Accel, IOGraphicsAccelerator2)

static IORangeAllocator *pgaVaRange() { return gPga ? gPga->fVa : nullptr; }

// ---- the frame worker: one per accelerator, on its own work loop ------------------------------------------------
// performTransaction (WindowServer's thread) hands a surface over and returns; the worker copies it into the ring and
// flips, and only when the flip has reached the screen (FLIP_DONE) or failed is the transaction complete. One frame is
// in flight at a time; a frame WindowServer presents while one is in flight past PGA's bound replaces the one queued
// behind it (NVAccel's async swap, kexts/NVRM/accel).
struct PgaSrcMap { IOMemoryDescriptor *md; unsigned id; uint64_t bytes, va; uint64_t use; uint32_t epoch; };
struct PgaRingBuf { struct pg106_kif_vram v; };
struct PgaWorker {
    IOLock *lock = nullptr;
    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *kick = nullptr;
    // under lock
    bool busy = false;               // a frame is in flight (the transaction is not complete)
    IOSurface *cur = nullptr, *next = nullptr;   // retained
    void *pipe = nullptr;
    struct pga_ring ring;
    bool flipPending = false;        // a FLIP succeeded and its FLIP_DONE has not arrived
    uint64_t flipCookie = 0;
    uint32_t flipStatus = 0;
    uint32_t seq = 0;
    bool homeFront = true;           // PG106FB's own scanout is on screen (no ring buffer is)
    // worker-only
    PgaRingBuf buf[PGA_RING] = {};
    uint32_t ringEpoch = 0;
    uint32_t ringW = 0, ringH = 0, ringPitch = 0;
    uint32_t ringFlags = 0;
    struct pg106fb_kif_info info = {};
    bool infoValid = false;
    PgaSrcMap src[kPgaSrcSlots] = {};
    uint64_t srcTick = 0;
    IOMemoryMap *cpuSrcMap = nullptr; IOMemoryDescriptor *cpuSrcMd = nullptr; unsigned cpuSrcId = 0;
    IOMemoryMap *fbMap = nullptr; uint64_t fbMapPhys = 0, fbMapBytes = 0;   // PG106FB's scanout through BAR1
    enum pga_path lastPath = PGA_PATH_NONE;
    unsigned logs = 0, ringStuck = 0;
};
static PgaWorker gW;

static void pgaRetainSurface(IOSurface *s) { ((OSObject *)s)->retain(); }
static void pgaReleaseSurface(IOSurface *s) { if (s) ((OSObject *)s)->release(); }

static bool pgaFbInfo(IOService *fb, struct pg106fb_kif_info *out)
{
    pgaKifInit(*out);
    return pgaKif(fb, PG106FB_KIF_FN_INFO, out) == kIOReturnSuccess;
}

// FLIP and wait for its FLIP_DONE, at most two frame periods and 50 ms. kIOReturnSuccess = on screen.
static IOReturn pgaFlipAndWait(IOService *fb, const struct pg106fb_kif_flip *f0, uint32_t refreshUs)
{
    struct pg106fb_kif_flip f = *f0;
    IOLockLock(gW.lock);
    gW.flipPending = true; gW.flipCookie = f.cookie; gW.flipStatus = 0;
    IOLockUnlock(gW.lock);
    IOReturn r = pgaKif(fb, PG106FB_KIF_FN_FLIP, &f);
    if (r != kIOReturnSuccess) {
        IOLockLock(gW.lock); gW.flipPending = false; IOLockUnlock(gW.lock);
        return r;
    }
    uint64_t deadline = 0;
    clock_interval_to_deadline(2 * (refreshUs ? refreshUs : 16667) + 50000, kMicrosecondScale, &deadline);
    IOLockLock(gW.lock);
    while (gW.flipPending && mach_absolute_time() < deadline)
        IOLockSleepDeadline(gW.lock, (event_t)&gW.flipPending, deadline, THREAD_UNINT);
    const bool timedOut = gW.flipPending;
    const IOReturn st = timedOut ? kIOReturnTimeout : (IOReturn)gW.flipStatus;
    IOLockUnlock(gW.lock);
    if (timedOut) PGA_COUNT(kStFlipTimeout);
    return st;
}

// FLIP_DONE, from PG106FB on PG106's work loop: record it and wake the worker; call nothing.
static void pgaFlipDone(const struct pg106accel_kif_flip_done *d)
{
    if (!gW.lock) return;
    IOLockLock(gW.lock);
    const unsigned b = pga_cookie_buf(d->cookie);
    const bool ok = d->status == kIOReturnSuccess;
    if (b == PGA_COOKIE_HOME) { if (ok) { pga_ring_home_done(&gW.ring); gW.homeFront = true; } }
    else { pga_ring_done(&gW.ring, (int)b, ok); if (ok) gW.homeFront = false; }
    if (!ok) PGA_COUNT(kStFlipAborted);
    if (gW.flipPending && gW.flipCookie == d->cookie) {
        gW.flipPending = false; gW.flipStatus = d->status;
        IOLockWakeup(gW.lock, (event_t)&gW.flipPending, false);
    }
    IOLockUnlock(gW.lock);
}

// Put PG106FB's own scanout back on screen (before the ring is freed or a CPU frame is written there).
static bool pgaFlipHome(IOService *fb)
{
    IOLockLock(gW.lock);
    const bool home = gW.homeFront && !pga_ring_count(&gW.ring, PGA_BUF_QUEUED);
    const uint32_t seq = ++gW.seq;
    IOLockUnlock(gW.lock);
    if (home) return true;
    struct pg106fb_kif_flip f; pgaKifInit(f);
    f.flags = PG106FB_KIF_FLIP_HOME; f.generation = gW.info.generation; f.client = gPga; f.format = PG106_KIF_FMT_BGRA;
    f.cookie = pga_cookie(gW.info.generation, seq, PGA_COOKIE_HOME);
    for (int tries = 0; tries < 3; tries++) {
        IOReturn r = pgaFlipAndWait(fb, &f, gW.info.refresh_us);
        if (r == kIOReturnSuccess) return true;
        if (r != kIOReturnBusy) { PGA_LOG_CAPPED(gW.logs, 32, "HOME flip refused (0x%x)", (unsigned)r); return false; }
        IOSleep(gW.info.refresh_us / 1000 + 1);
    }
    return false;
}

static void pgaRingFree(IOService *fb)
{
    bool idle;
    IOLockLock(gW.lock); idle = pga_ring_idle(&gW.ring); IOLockUnlock(gW.lock);
    if (!idle && fb) {
        pgaFlipHome(fb);
        IOLockLock(gW.lock); idle = pga_ring_idle(&gW.ring); IOLockUnlock(gW.lock);
    }
    if (!idle && fb) {
        // still on screen: freeing it would let PG106 hand that VRAM to someone else while the head scans it
        PGA_LOG_CAPPED(gW.logs, 32, "ring still on screen after a HOME flip: keeping its VRAM");
        return;
    }
    for (int i = 0; i < PGA_RING; i++) if (gW.buf[i].v.handle) { pgaKifVramFree(gW.buf[i].v.handle); bzero(&gW.buf[i], sizeof gW.buf[i]); }
    IOLockLock(gW.lock); pga_ring_init(&gW.ring); gW.homeFront = true; IOLockUnlock(gW.lock);
    gW.ringW = gW.ringH = gW.ringPitch = 0; gW.ringFlags = 0;
}

static bool pgaRingEnsure(IOService *fb, uint32_t w, uint32_t h, uint32_t flags)
{
    if (gW.buf[0].v.handle && gW.ringEpoch != pgaEpoch()) {
        // PG106 lost the GPU since the ring was made: its VRAM is gone (VRAM_FREE of the old handles is a no-op), and
        // the head was powered off with it (PG106FB aborted any flip), so the ring is forgotten, not flipped away from
        for (int i = 0; i < PGA_RING; i++) {
            if (gW.buf[i].v.handle) pgaKifVramFree(gW.buf[i].v.handle);
            bzero(&gW.buf[i], sizeof gW.buf[i]);
        }
        IOLockLock(gW.lock); pga_ring_init(&gW.ring); gW.homeFront = true; IOLockUnlock(gW.lock);
        gW.ringW = gW.ringH = gW.ringPitch = 0; gW.ringFlags = 0;
    }
    if (gW.ringW == w && gW.ringH == h && gW.ringFlags == flags && gW.buf[0].v.handle) return true;
    pgaRingFree(fb);
    if (gW.buf[0].v.handle) return false;
    const uint64_t bytes = pga_scanout_bytes(w, h);
    gW.ringEpoch = pgaEpoch();
    for (int i = 0; i < PGA_RING; i++) {
        if (!pgaKifVramAlloc(bytes, PG106_KIF_VRAM_SCANOUT | flags, &gW.buf[i].v) ||
            (gW.buf[i].v.vram_offset & 4095) || ((flags & PG106_KIF_VRAM_GPU_VA) && !gW.buf[i].v.gpu_va) ||
            ((flags & PG106_KIF_VRAM_CPU) && !gW.buf[i].v.kva)) {
            PGA_LOG_CAPPED(gW.logs, 32, "scanout ring: VRAM buffer %d of %llu bytes refused or unusable", i, bytes);
            for (int k = 0; k <= i; k++) if (gW.buf[k].v.handle) { pgaKifVramFree(gW.buf[k].v.handle); bzero(&gW.buf[k], sizeof gW.buf[k]); }
            return false;
        }
    }
    gW.ringW = w; gW.ringH = h; gW.ringPitch = pga_scanout_pitch(w); gW.ringFlags = flags;
    PGA_LOG("scanout ring: %d x %llu bytes of VRAM for %ux%u (pitch %u) at 0x%llx 0x%llx 0x%llx", PGA_RING, bytes, w, h,
            gW.ringPitch, gW.buf[0].v.vram_offset, gW.buf[1].v.vram_offset, gW.buf[2].v.vram_offset);
    return true;
}

// noinline: inlined into pgaSrcVa's loop, clang's xnu fortify check of the bzero below fails to fold (an invalid
// ud1l trap operand), although the index is bounded
__attribute__((noinline)) static void pgaSrcDrop(PgaSrcMap *e)
{
    if (!e->md) return;
    pgaKifUnmap(e->va, e->bytes, e->epoch);
    pgaKifVaFree(e->va, e->bytes, e->epoch);
    e->md->complete(); e->md->release();
    bzero(e, sizeof *e);
}
static void pgaSrcDropAll()
{
    for (unsigned i = 0; i < kPgaSrcSlots; i++) pgaSrcDrop(&gW.src[i]);
    if (gW.cpuSrcMap) { gW.cpuSrcMap->release(); gW.cpuSrcMap = nullptr; }
    if (gW.cpuSrcMd) { gW.cpuSrcMd->complete(); gW.cpuSrcMd->release(); gW.cpuSrcMd = nullptr; }
    gW.cpuSrcId = 0;
    if (gW.fbMap) { gW.fbMap->release(); gW.fbMap = nullptr; gW.fbMapPhys = gW.fbMapBytes = 0; }
}
// The IOSurface's pages in the accelerator VM (read-only), kept for the next frames that use the same surface.
static uint64_t pgaSrcVa(IOSurface *s, IOMemoryDescriptor *md, uint64_t need)
{
    const unsigned id = s->getSurfaceID();
    const uint32_t page = gPga->fInfo.page_bytes ? gPga->fInfo.page_bytes : 4096;
    const uint64_t bytes = (need + page - 1) & ~(uint64_t)(page - 1);
    PgaSrcMap *victim = &gW.src[0];
    for (unsigned i = 0; i < kPgaSrcSlots; i++) {
        PgaSrcMap *e = &gW.src[i];
        if (e->md && e->epoch != pgaEpoch()) { pgaSrcDrop(e); }   // its address died with the last LOST
        if (e->md == md && e->id == id && e->bytes >= bytes) { e->use = ++gW.srcTick; return e->va; }
        if (!e->md) victim = e;
        else if (victim->md && e->use < victim->use) victim = e;
    }
    pgaSrcDrop(victim);
    if (md->getLength() < need || md->prepare() != kIOReturnSuccess) return 0;
    uint32_t ep = 0;
    const uint64_t va = pgaKifVaAlloc(bytes, 0, &ep);
    if (!va) { md->complete(); return 0; }
    struct pg106_kif_map m; pgaKifInit(m);
    m.gpu_va = va; m.bytes = bytes; m.kind = PG106_KIF_MAP_SYSMEM; m.flags = PG106_KIF_MAP_READONLY; m.md = md;
    IOService *pg = gPga->copyPG106();
    const IOReturn mr = pgaKif(pg, PG106_KIF_FN_MAP, &m);
    if (pg) pg->release();
    if (mr != kIOReturnSuccess) { pgaKifVaFree(va, bytes, ep); md->complete(); return 0; }
    md->retain();
    victim->md = md; victim->id = id; victim->bytes = bytes; victim->va = va; victim->use = ++gW.srcTick; victim->epoch = ep;
    return va;
}
static const uint8_t *pgaSrcCpu(IOSurface *s, IOMemoryDescriptor *md, uint64_t need)
{
    const unsigned id = s->getSurfaceID();
    if (gW.cpuSrcMd == md && gW.cpuSrcId == id && gW.cpuSrcMap && gW.cpuSrcMap->getLength() >= need)
        return (const uint8_t *)gW.cpuSrcMap->getVirtualAddress();
    if (gW.cpuSrcMap) { gW.cpuSrcMap->release(); gW.cpuSrcMap = nullptr; }
    if (gW.cpuSrcMd) { gW.cpuSrcMd->complete(); gW.cpuSrcMd->release(); gW.cpuSrcMd = nullptr; }
    if (md->getLength() < need || md->prepare() != kIOReturnSuccess) return nullptr;
    IOMemoryMap *m = md->map(kIOMapReadOnly);
    if (!m) { md->complete(); return nullptr; }
    md->retain(); gW.cpuSrcMd = md; gW.cpuSrcMap = m; gW.cpuSrcId = id;
    return (const uint8_t *)m->getVirtualAddress();
}
// PG106FB's own scanout for the CPU: its kva when it gives one, else its BAR1 address mapped here (write-combined).
// PG106FB publishes the BAR1 address only while its scanout is mapped through BAR1 (not in its shadow-copy mode).
static volatile uint8_t *pgaFbCpu()
{
    if (gW.info.kva) return (volatile uint8_t *)(uintptr_t)gW.info.kva;
    if (!gW.info.bar1_phys || !gW.info.vram_bytes) return nullptr;
    if (gW.fbMap && gW.fbMapPhys == gW.info.bar1_phys && gW.fbMapBytes == gW.info.vram_bytes)
        return (volatile uint8_t *)gW.fbMap->getVirtualAddress();
    if (gW.fbMap) { gW.fbMap->release(); gW.fbMap = nullptr; }
    IODeviceMemory *dm = IODeviceMemory::withRange((IOPhysicalAddress)gW.info.bar1_phys, (IOPhysicalLength)gW.info.vram_bytes);
    if (!dm) return nullptr;
    gW.fbMap = dm->map(kIOMapWriteCombineCache);
    dm->release();
    if (!gW.fbMap) return nullptr;
    gW.fbMapPhys = gW.info.bar1_phys; gW.fbMapBytes = gW.info.vram_bytes;
    return (volatile uint8_t *)gW.fbMap->getVirtualAddress();
}

static void pgaCpuCopy(volatile uint8_t *dst, uint32_t dpitch, const uint8_t *src, uint64_t spitch, uint32_t wbytes, uint32_t h)
{
    for (uint32_t y = 0; y < h; y++) {
        volatile uint32_t *d = (volatile uint32_t *)(dst + (uint64_t)y * dpitch);
        const uint32_t *sp = (const uint32_t *)(src + (uint64_t)y * spitch);
        for (uint32_t x = 0; x < wbytes / 4; x++) d[x] = sp[x];
    }
}

// One frame onto the HDMI screen. Returns when it is on screen, or was dropped (counted).
static void pgaPresent(IOSurface *s)
{
    PG106Accel *a = gPga;
    IOService *fb = a ? a->copyFB() : nullptr;
    if (!fb) return;
    PGA_COUNT(kStFrames);
    if (!gW.infoValid || !gW.info.online) gW.infoValid = pgaFbInfo(fb, &gW.info);
    bool fbKva = false;
    const enum pga_path path = a->framePath(&fbKva);
    if (path != gW.lastPath) {
        PGA_LOG("HDMI frames: %s", pga_path_str(path));
        a->setProperty("PG106AccelFramePath", pga_path_str(path));
        if (gW.lastPath == PGA_PATH_CE_RING) pgaSrcDropAll();
        gW.lastPath = path;
    }
    const uint32_t W = gW.info.width, H = gW.info.height;
    IOMemoryDescriptor *md = s->getMemoryDescriptor();
    const uint64_t sw = s->getWidth(), sh = s->getHeight(), sp = s->getBytesPerRow();
    const enum pga_src_verdict sv = gW.infoValid && gW.info.online && md
        ? pga_src_check(s->getPixelFormat(), sw, sh, sp, md->getLength(), W, H) : PGA_SRC_SIZE;
    if (sv != PGA_SRC_OK || path == PGA_PATH_NONE) {
        PGA_COUNT(kStDropGeom);
        PGA_LOG_CAPPED(gW.logs, 32, "frame kept off HDMI: surface 0x%x %llux%llu pitch %llu fmt 0x%08x vs mode %ux%u online %u "
                       "(verdict %d, path %s)", s->getSurfaceID(), sw, sh, sp, s->getPixelFormat(), W, H, gW.info.online,
                       (int)sv, pga_path_str(path));
        gW.infoValid = false;   // re-read the mode for the next frame
        fb->release();
        return;
    }
    const uint64_t need = sh * sp;
    if (path == PGA_PATH_CPU_FB) {
        const uint8_t *src = pgaSrcCpu(s, md, need);
        volatile uint8_t *dst = pgaFbCpu();
        if (!src || !dst || gW.info.pitch < W * 4 || (uint64_t)gW.info.pitch * H > gW.info.vram_bytes) { PGA_COUNT(kStDropCopy); fb->release(); return; }
        if (!pgaFlipHome(fb)) { PGA_COUNT(kStDropFlip); fb->release(); return; }
        pgaCpuCopy(dst, gW.info.pitch, src, sp, W * 4, H);
        PGA_COUNT(kStCpuFB);
        fb->release();
        return;
    }
    const uint32_t flags = path == PGA_PATH_CE_RING ? PG106_KIF_VRAM_GPU_VA : PG106_KIF_VRAM_CPU;
    if (!pgaRingEnsure(fb, W, H, flags)) { PGA_COUNT(kStDropCopy); fb->release(); return; }
    IOLockLock(gW.lock);
    const int b = pga_ring_acquire(&gW.ring);
    const uint32_t seq = ++gW.seq;
    IOLockUnlock(gW.lock);
    if (b < 0) {
        // Every buffer is on screen or waiting for a FLIP_DONE that has not come (pgaFlipAndWait timed out). After two
        // such frames the ring is reset behind a HOME flip: a late FLIP_DONE then finds no queued buffer and changes
        // nothing, and the worst case is one torn frame, never a buffer freed while scanned out (the VRAM is kept).
        PGA_COUNT(kStDropBusy);
        if (++gW.ringStuck >= 2 && pgaFlipHome(fb)) {
            IOLockLock(gW.lock); pga_ring_init(&gW.ring); gW.homeFront = true; IOLockUnlock(gW.lock);
            gW.ringStuck = 0;
            PGA_LOG_CAPPED(gW.logs, 32, "scanout ring reset: FLIP_DONE never came for its queued buffers");
        }
        fb->release();
        return;
    }
    gW.ringStuck = 0;
    bool copied = false;
    if (path == PGA_PATH_CE_RING) {
        const uint64_t sva = pgaSrcVa(s, md, need);
        IOService *pg = a->copyPG106();
        IOLockLock(a->fLock); const uint64_t chan = a->fChan; IOLockUnlock(a->fLock);
        struct pg106_kif_copy c; pgaKifInit(c);
        c.chan = chan; c.src_va = sva; c.dst_va = gW.buf[b].v.gpu_va; c.src_pitch = (uint32_t)sp;
        c.dst_pitch = gW.ringPitch; c.width_bytes = W * 4; c.height = H;
        if (pg && chan && sva && pgaKif(pg, PG106_KIF_FN_COPY, &c) == kIOReturnSuccess) {
            struct pg106_kif_fence f; pgaKifInit(f);
            f.chan = chan; f.op = PG106_KIF_FENCE_WAIT; f.timeout_us = kPgaCopyWaitUs; f.seqno = c.seqno;
            copied = pgaKif(pg, PG106_KIF_FN_FENCE, &f) == kIOReturnSuccess;
        }
        if (pg) pg->release();
        if (copied) PGA_COUNT(kStCopiesCE);
    } else {
        const uint8_t *src = pgaSrcCpu(s, md, need);
        if (src) { pgaCpuCopy((volatile uint8_t *)(uintptr_t)gW.buf[b].v.kva, gW.ringPitch, src, sp, W * 4, H); copied = true; PGA_COUNT(kStCopiesCPU); }
    }
    if (!copied) {
        IOLockLock(gW.lock); pga_ring_abandon(&gW.ring, b); IOLockUnlock(gW.lock);
        PGA_COUNT(kStDropCopy);
        PGA_LOG_CAPPED(gW.logs, 32, "frame dropped: the %s copy of surface 0x%x did not land", path == PGA_PATH_CE_RING ? "copy-engine" : "CPU",
                       s->getSurfaceID());
        fb->release();
        return;
    }
    struct pg106fb_kif_flip f; pgaKifInit(f);
    f.vram_offset = gW.buf[b].v.vram_offset; f.width = W; f.height = H; f.pitch = gW.ringPitch; f.format = PG106_KIF_FMT_BGRA;
    f.generation = gW.info.generation; f.client = a; f.cookie = pga_cookie(gW.info.generation, seq, (unsigned)b);
    if (!pga_flip_ok(f.vram_offset, W, H, f.pitch, f.format, W, H)) {
        IOLockLock(gW.lock); pga_ring_abandon(&gW.ring, b); IOLockUnlock(gW.lock);
        PGA_COUNT(kStDropFlip); fb->release(); return;
    }
    // FLIP_DONE may arrive before FLIP returns: the buffer is queued first, and abandoned again if FLIP refuses
    IOLockLock(gW.lock); pga_ring_queued(&gW.ring, b); IOLockUnlock(gW.lock);
    const IOReturn r = pgaFlipAndWait(fb, &f, gW.info.refresh_us);
    if (r == kIOReturnSuccess) { PGA_COUNT(kStFlips); fb->release(); return; }
    if (r == kIOReturnTimeout) { fb->release(); return; }   // the FLIP_DONE will still come; the buffer stays queued
    IOLockLock(gW.lock);
    if (gW.ring.st[b] == PGA_BUF_QUEUED) gW.ring.st[b] = PGA_BUF_FREE;   // FLIP refused: nothing was queued
    IOLockUnlock(gW.lock);
    if (r == kIOReturnBusy) PGA_COUNT(kStFlipLate);
    else {
        PGA_COUNT(kStDropFlip);
        if (r == kIOReturnNotReady) gW.infoValid = false;
        PGA_LOG_CAPPED(gW.logs, 32, "flip of surface 0x%x refused (0x%x)", s->getSurfaceID(), (unsigned)r);
    }
    fb->release();
}

static void pgaWorkerRun(OSObject *, IOTimerEventSource *)
{
    for (;;) {
        IOLockLock(gW.lock);
        IOSurface *s = gW.cur; gW.cur = nullptr;
        void *pipe = gW.pipe;
        IOLockUnlock(gW.lock);
        if (s) { pgaPresent(s); pgaReleaseSurface(s); }
        IOLockLock(gW.lock);
        if (gW.next) { gW.cur = gW.next; gW.next = nullptr; IOLockUnlock(gW.lock); continue; }
        gW.busy = false;
        IOLockWakeup(gW.lock, (event_t)&gW.busy, false);
        IOLockUnlock(gW.lock);
        if (pipe) _ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv(pipe, nullptr);
        return;
    }
}

IOReturn PG106AccelDisplayPipe::performTransaction(IOAccelDisplayPipeTransaction2 *txn)
{
    if (!txn || !gW.lock || !gW.kick) return kIOReturnNotReady;
    IOSurface *s = txn->getPlaneIOSurface(0, 0);
    if (!s) return kIOReturnSuccess;   // nothing to show: the screen keeps its picture
    // WindowServer's next present waits here for the frame in flight, for a frame period and a quarter at most
    const uint32_t refresh = gW.info.refresh_us ? gW.info.refresh_us : 16667;
    uint64_t end = 0;
    clock_interval_to_deadline(refresh + refresh / 4, kMicrosecondScale, &end);
    IOLockLock(gW.lock);
    while (gW.busy && mach_absolute_time() < end) IOLockSleepDeadline(gW.lock, (event_t)&gW.busy, end, THREAD_UNINT);
    pgaRetainSurface(s);
    if (gW.busy) {
        IOSurface *old = gW.next; gW.next = s;   // follows the frame in flight; an older queued frame is dropped
        IOLockUnlock(gW.lock);
        if (old) { PGA_COUNT(kStDropBusy); pgaReleaseSurface(old); }
        return kIOReturnSuccess;
    }
    gW.busy = true; gW.cur = s; gW.pipe = this;
    IOLockUnlock(gW.lock);
    gW.kick->setTimeoutUS(1);
    return kIOReturnSuccess;
}
bool PG106AccelDisplayPipe::isTransactionComplete(IOAccelDisplayPipeTransaction2 *)
{
    if (!gW.lock) return true;
    IOLockLock(gW.lock);
    const bool done = !gW.busy || gW.pipe != this;
    IOLockUnlock(gW.lock);
    return done;
}
bool PG106AccelDisplayMachine::displayModeDidChange()
{
    gW.infoValid = false;   // the worker re-reads PG106FB's mode with the next frame (and re-sizes the ring)
    return true;
}

// ---- PG106 kernel interface ----------------------------------------------------------------------------------
static bool pgaKifVramAlloc(uint64_t bytes, uint32_t flags, struct pg106_kif_vram *out)
{
    PG106Accel *a = gPga;
    if (!a || !a->fVramAllowed) return false;
    uint32_t need = PG106_KIF_CAP_VRAM;
    if (flags & PG106_KIF_VRAM_CPU) need |= PG106_KIF_CAP_BAR1;
    if (flags & PG106_KIF_VRAM_GPU_VA) need |= PG106_KIF_CAP_VM;
    if (!a->kifUsable(need)) return false;
    IOService *pg = a->copyPG106();
    if (!pg) return false;
    pgaKifInit(*out);
    out->bytes = bytes; out->align = 4096; out->flags = flags;
    const IOReturn r = pgaKif(pg, PG106_KIF_FN_VRAM_ALLOC, out);
    pg->release();
    return r == kIOReturnSuccess && out->handle && out->actual_bytes >= bytes;
}
static void pgaKifVramFree(uint64_t handle)
{
    PG106Accel *a = gPga;
    IOService *pg = a ? a->copyPG106() : nullptr;
    if (!pg || !handle) { if (pg) pg->release(); return; }
    struct pg106_kif_vram v; pgaKifInit(v); v.handle = handle;
    pgaKif(pg, PG106_KIF_FN_VRAM_FREE, &v);
    pg->release();
}
static uint64_t pgaKifVaAlloc(uint64_t bytes, uint64_t align, uint32_t *epoch)
{
    PG106Accel *a = gPga;
    if (!a || !a->kifUsable(PG106_KIF_CAP_VM)) return 0;
    IOService *pg = a->copyPG106();
    if (!pg) return 0;
    const uint32_t ep = pgaEpoch();
    struct pg106_kif_va v; pgaKifInit(v); v.bytes = bytes; v.align = align;
    const IOReturn r = pgaKif(pg, PG106_KIF_FN_VA_ALLOC, &v);
    pg->release();
    if (r != kIOReturnSuccess || ep != pgaEpoch()) return 0;   // LOST during the call: the address is not ours to keep
    if (epoch) *epoch = ep;
    return v.gpu_va;
}
static void pgaKifVaFree(uint64_t va, uint64_t bytes, uint32_t epoch)
{
    if (!va || epoch != pgaEpoch()) return;   // an older epoch's address may be someone else's now
    PG106Accel *a = gPga;
    IOService *pg = a ? a->copyPG106() : nullptr;
    if (!pg) return;
    struct pg106_kif_va v; pgaKifInit(v); v.gpu_va = va; v.bytes = bytes;
    pgaKif(pg, PG106_KIF_FN_VA_FREE, &v);
    pg->release();
}
static void pgaKifUnmap(uint64_t va, uint64_t bytes, uint32_t epoch)
{
    if (!va || epoch != pgaEpoch()) return;
    PG106Accel *a = gPga;
    IOService *pg = a ? a->copyPG106() : nullptr;
    if (!pg) return;
    struct pg106_kif_map m; pgaKifInit(m); m.gpu_va = va; m.bytes = bytes;
    pgaKif(pg, PG106_KIF_FN_UNMAP, &m);
    pg->release();
}

bool PG106AccelMemoryMap::commitIntoGPUPageTable()
{
    if (fMapped || !fKifVa) return true;   // a placeholder address: nothing to commit, as before
    if (fEpoch != pgaEpoch()) return true;  // allocated before the last LOST: the address is dead
    if (!fBudget) {
        if (!pgaTake(&gFamilyMaps, kPgaFamilyMapsMax)) { PGA_COUNT(kStMapBudget); return true; }
        fBudget = true;
    }
    PG106Accel *a = gPga;
    IOService *pg = a ? a->copyPG106() : nullptr;
    if (!pg) return true;
    struct pg106_kif_map m; pgaKifInit(m);
    m.gpu_va = fVa; m.bytes = fVaBytes;
    if (PG106AccelVidMemory *vm = OSDynamicCast(PG106AccelVidMemory, fMem)) {
        if (vm->fHandle && vm->fBytes >= fVaBytes) {
            m.kind = PG106_KIF_MAP_VRAM; m.vram_handle = vm->fHandle;
            fMapped = pgaKif(pg, PG106_KIF_FN_MAP, &m) == kIOReturnSuccess;
            if (fMapped) PGA_COUNT(kStMapVram);
        }
    } else if (fMem) {
        // System memory: its physical segments, when every one of them is wired now
        enum { kMaxSeg = 64 };
        IOAddressRange r[kMaxSeg]; unsigned n = 0; uint64_t off = 0;
        const uint64_t len = fMem->getLength();
        while (off < len && n < kMaxSeg) {
            unsigned long long sl = 0;
            const uint64_t pa = fMem->getPhysicalSegment(off, &sl);
            if (!pa || !sl) break;
            if (sl > len - off) sl = len - off;
            if (n && r[n - 1].address + r[n - 1].length == pa) r[n - 1].length += sl;
            else { r[n].address = pa; r[n].length = sl; n++; }
            off += sl;
        }
        if (off >= len && len >= fVaBytes - 4095) {
            IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRanges(r, n, kIODirectionInOut | kIOMemoryTypePhysical64, nullptr);
            if (md && md->prepare() == kIOReturnSuccess) {
                m.kind = PG106_KIF_MAP_SYSMEM; m.md = md; m.bytes = (len + 4095) & ~4095ull;
                if (m.bytes > fVaBytes) m.bytes = fVaBytes;
                fMapped = pgaKif(pg, PG106_KIF_FN_MAP, &m) == kIOReturnSuccess;
                if (fMapped) { fSysMd = md; md = nullptr; PGA_COUNT(kStMapSys); }
                else md->complete();
            }
            if (md) md->release();
        } else {
            PGA_COUNT(kStMapSysSkipped);
        }
    }
    pg->release();
    if (!fMapped && fBudget) { OSDecrementAtomic(&gFamilyMaps); fBudget = false; }
    return true;   // the family's mapping stands either way: nothing on the GP106 but this kext uses family addresses
}
void PG106AccelMemoryMap::releaseFromGPUPageTable()
{
    if (!fMapped) return;
    pgaKifUnmap(fVa, fVaBytes, fEpoch);
    fMapped = false;
    if (fBudget) { OSDecrementAtomic(&gFamilyMaps); fBudget = false; }
    if (fSysMd) { fSysMd->complete(); fSysMd->release(); fSysMd = nullptr; }
}

void PG106Accel::kifAttach()
{
    IOService *pg = copyPG106();
    if (!pg) return;
    IOLockLock(fLock);
    const bool lost = fChanDead;
    IOLockUnlock(fLock);
    if (lost) {
        // after EVENT LOST every handle is dead; close them (PG106 accepts that and does nothing) and start over
        kifDetach();
        IOLockLock(fLock); fChanDead = false; IOLockUnlock(fLock);
    }
    uint32_t abi = 0;
    bool ok = false;
    struct pg106_kif_info info; pgaKifInit(info);
    if (!pgaReadAbi(pg, "PG106KifABI", &abi)) {
        PGA_LOG_CAPPED(fKifLogs, 4, "PG106 publishes no PG106KifABI: no VRAM, copy engine or GPU addresses from it yet");
    } else if (!pga_kif_accepts(abi, PG106_KIF_ABI)) {
        PGA_LOG_CAPPED(fKifLogs, 4, "PG106KifABI 0x%08x is incompatible with 0x%08x: not using it", abi, PG106_KIF_ABI);
    } else if (pgaKif(pg, PG106_KIF_FN_INFO, &info) != kIOReturnSuccess || !info.ready) {
        PGA_LOG_CAPPED(fKifLogs, 4, "PG106 kif INFO: not ready (caps 0x%x)", info.caps);
    } else {
        ok = true;
    }
    if (!ok) { pg->release(); return; }
    IOLockLock(fLock);
    const bool had = fKifOK;
    fInfo = info; fKifOK = true;
    IOLockUnlock(fLock);
    if (!had || info.caps != fLoggedCaps) PGA_LOG("PG106 kif 0x%08x: caps 0x%x, VRAM %u MB (%u MB free), BAR1 %u MB, VA window 0x%llx + %u MB",
                      abi, info.caps, pga_mb(info.vram_bytes), pga_mb(info.vram_avail_bytes), pga_mb(info.bar1_bytes),
                      info.va_base, pga_mb(info.va_bytes));
    fLoggedCaps = info.caps;
    publishVram();
    // the copy channel: frames, and GPU stamps
    if (!fChan && (info.caps & (PG106_KIF_CAP_COPY | PG106_KIF_CAP_VM)) == (PG106_KIF_CAP_COPY | PG106_KIF_CAP_VM)) {
        struct pg106_kif_chan c; pgaKifInit(c);
        c.kind = PG106_KIF_CHAN_COPY; c.client = this; c.client_tag = 1;
        if (pgaKif(pg, PG106_KIF_FN_CHAN_OPEN, &c) == kIOReturnSuccess && c.chan) {
            IOLockLock(fLock); fChan = c.chan; fChanDead = false; IOLockUnlock(fLock);
            PGA_LOG("copy channel 0x%llx open", c.chan);
        } else {
            PGA_LOG_CAPPED(fKifLogs, 8, "copy channel refused: frames are copied by the CPU, stamps written by it");
        }
    }
    // the stamp page in the accelerator VM, for GPU-written stamps
    if (fChan && !fStampVa && fStamps && fGpuStampsAllowed && (info.caps & PG106_KIF_CAP_RELEASE)) {
        uint32_t ep = 0;
        const uint64_t va = pgaKifVaAlloc(kPgaStamps * 4, 4096, &ep);
        struct pg106_kif_map m; pgaKifInit(m);
        m.gpu_va = va; m.bytes = kPgaStamps * 4; m.kind = PG106_KIF_MAP_SYSMEM; m.md = fStamps;
        if (va && pgaKif(pg, PG106_KIF_FN_MAP, &m) == kIOReturnSuccess) {
            fStamps->retain(); fStampMapped = fStamps;
            IOLockLock(fLock); fStampVa = va; fStampEpoch = ep; IOLockUnlock(fLock);
            PGA_LOG("event stamps: written by the GPU (semaphore releases on the copy channel), stamp page at 0x%llx", va);
        } else if (va) {
            pgaKifVaFree(va, kPgaStamps * 4, ep);
        }
    }
    pg->release();
}

void PG106Accel::kifDetach()
{
    IOService *pg = copyPG106();
    IOLockLock(fLock);
    const uint64_t chan = fChan, sva = fStampVa;
    const uint32_t sep = fStampEpoch;
    fChan = 0; fStampVa = 0; fKifOK = false;
    IOLockUnlock(fLock);
    pgaKifUnmap(sva, kPgaStamps * 4, sep);    // skipped for an address from before the last LOST
    pgaKifVaFree(sva, kPgaStamps * 4, sep);
    if (fStampMapped) { fStampMapped->release(); fStampMapped = nullptr; }
    if (pg && chan) { struct pg106_kif_chan c; pgaKifInit(c); c.chan = chan; pgaKif(pg, PG106_KIF_FN_CHAN_CLOSE, &c); }
    if (pg) pg->release();
}

void PG106Accel::publishVram()
{
    if (!fInfo.vram_bytes) return;
    setProperty("VRAM,totalMB", pga_mb(fInfo.vram_bytes), 32);
    setProperty("PG106AccelVRAMBytes", fInfo.vram_bytes, 64);
}

IOReturn PG106Accel::callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4)
{
    if (fn && fn->isEqualTo(PG106ACCEL_KIF_FN_FLIP_DONE)) {
        const struct pg106accel_kif_flip_done *d = (const struct pg106accel_kif_flip_done *)p1;
        if (!d || !pga_kif_accepts(PG106_KIF_ABI, (uint32_t)(uintptr_t)p2) || d->size < sizeof *d) return kIOReturnUnsupported;
        pgaFlipDone(d);
        return kIOReturnSuccess;
    }
    if (fn && fn->isEqualTo(PG106ACCEL_KIF_FN_EVENT)) {
        const struct pg106accel_kif_event *e = (const struct pg106accel_kif_event *)p1;
        if (!e || !pga_kif_accepts(PG106_KIF_ABI, (uint32_t)(uintptr_t)p2) || e->size < sizeof *e) return kIOReturnUnsupported;
        if (e->event == PG106_KIF_EVENT_LOST) {
            // every handle is dead: frames and stamps fall back to the CPU until PG106 is ready again
            IOLockLock(fLock);
            const bool first = !fChanDead;
            fChanDead = true; fKifOK = false; fStampVa = 0;   // stamps go to the CPU at once
            IOLockUnlock(fLock);
            if (first) { OSIncrementAtomic(&gKifEpoch); PGA_COUNT(kStKifLost); }
            PGA_LOG("PG106 kif: channel 0x%llx LOST (epoch %u), CPU fallback until PG106 is ready again", e->chan, pgaEpoch());
        }
        // PROGRESS: nothing to do, FENCE WAIT and the family's stamp polling see the progress themselves
        return kIOReturnSuccess;
    }
    return IOGraphicsAccelerator2::callPlatformFunction(fn, wait, p1, p2, p3, p4);
}

// ---- stamps ----------------------------------------------------------------------------------------------------
static void pgaWriteStamp(int index, unsigned int value)
{
    PG106Accel *a = gPga;
    if (!a || !a->fStamps || index < 0 || index >= (int)kPgaStamps) return;
    a->fStampWant[index] = value;
    // The family's calling context for writeStamp is not reconstructed. A GPU release takes PG106's gate, so it is
    // only made from a thread that may block; anywhere else the CPU writes the stamp, as before.
    if (!preemption_enabled() || ml_at_interrupt_context()) {
        volatile unsigned int *b = (volatile unsigned int *)a->fStamps->getBytesNoCopy();
        if (b) b[index] = value;
        PGA_COUNT(kStStampsCPU);
        return;
    }
    IOLockLock(a->fLock);
    const uint64_t va = a->fStampVa, chan = a->fChan;
    const bool gpu = pga_gpu_stamps(chan && !a->fChanDead && a->fKifOK, va, a->fGpuStampsAllowed);
    IOService *pg = gpu ? a->fPG106 : nullptr;
    if (pg) pg->retain();
    IOLockUnlock(a->fLock);
    if (pg) {
        struct pg106_kif_release r; pgaKifInit(r);
        r.chan = chan; r.gpu_va = va + 4ull * (unsigned)index; r.value = value;
        const IOReturn rr = pgaKif(pg, PG106_KIF_FN_RELEASE, &r);
        pg->release();
        if (rr == kIOReturnSuccess) { PGA_COUNT(kStStampsGPU); return; }
    }
    volatile unsigned int *base = (volatile unsigned int *)a->fStamps->getBytesNoCopy();
    if (base) base[index] = value;
    PGA_COUNT(kStStampsCPU);
}
void *PG106AccelEventMachine::eventTimeout(int stamp)
{
    PG106Accel *a = gPga;
    if (a && a->fStamps && stamp >= 0 && stamp < (int)kPgaStamps) {
        volatile unsigned int *base = (volatile unsigned int *)a->fStamps->getBytesNoCopy();
        // A GPU release that never landed: the family's event is completed from the CPU instead of hanging
        if (base && (int)(a->fStampWant[stamp] - base[stamp]) > 0) {
            base[stamp] = a->fStampWant[stamp];
            PGA_COUNT(kStStampTimeouts);
            PGA_LOG_CAPPED(fLogged, 8, "event timeout on stamp %d: GPU release did not land, written by the CPU", stamp);
        }
    }
    return nullptr;
}

// ---- the HDMI framebuffer ------------------------------------------------------------------------------------
enum pga_path PG106Accel::framePath(bool *fbKva)
{
    const bool kva = gW.infoValid && (gW.info.kva || gW.info.bar1_phys);
    if (fbKva) *fbKva = kva;
    IOLockLock(fLock);
    const bool kifOk = fKifOK;
    uint32_t caps = fInfo.caps;
    if (!fChan || fChanDead) caps &= ~PG106_KIF_CAP_COPY;   // no channel: no copy engine
    IOLockUnlock(fLock);
    return pga_choose_path(kifOk && fVramAllowed, caps, fCeAllowed, kva);
}

static bool pgaFramebufferAcceptable(IOFramebuffer *fb, bool log)
{
    return gPga && gPga->fbAcceptable(fb, log);
}
bool PG106Accel::fbAcceptable(IOFramebuffer *fbp, bool log)
{
    IOService *fb = (IOService *)fbp;
    struct pga_fb_facts f; bzero(&f, sizeof f);
    f.is_pg106fb = fb && fb->metaCast("PG106FB") != nullptr;
    f.same_pci = fbIsOurs(fb);
    f.has_abi = pgaReadAbi(fb, "PG106FBKifABI", &f.abi);
    f.mode_display = fModeDisplay;
    IOLockLock(fLock);
    f.pg106_ready = fReady;
    f.already_paired = fFB && fFB != fb;
    IOLockUnlock(fLock);
    if (f.is_pg106fb && f.same_pci && f.has_abi) {
        // a frame path needs either PG106 VRAM or a CPU mapping of PG106FB's own scanout (its kva or BAR1 address)
        struct pg106fb_kif_info info;
        const bool kva = pgaFbInfo(fb, &info) && (info.kva || info.bar1_phys);
        IOLockLock(fLock);
        uint32_t caps = fInfo.caps;
        const bool kifOk = fKifOK && fVramAllowed;
        if (!fChan || fChanDead) caps &= ~PG106_KIF_CAP_COPY;
        IOLockUnlock(fLock);
        f.frame_path = pga_choose_path(kifOk, caps, fCeAllowed, kva) != PGA_PATH_NONE;
    }
    const enum pga_fb_verdict v = pga_fb_decide(&f);
    if (fb && fb == fFBCand) { fFBVerdict = v; fFBDecided = !pga_fb_retry_later(v); }
    static unsigned logs;
    if (log && v != PGA_FB_ACCEPT)
        PGA_LOG_CAPPED(logs, 16, "framebuffer %s (%p) refused: %s", fb ? fb->getName() : "?", fb, pga_fb_verdict_str(v));
    return v == PGA_FB_ACCEPT;
}

void PG106Accel::setDisplayProps(bool attached, const char *path)
{
    setProperty("PG106AccelDisplay", attached ? kOSBooleanTrue : kOSBooleanFalse);
    if (path) setProperty("PG106AccelFramePath", path);
    if (attached) {
        if (OSDictionary *caps = OSDictionary::withCapacity(2)) {
            caps->setObject("DisplayPipeSupported", kOSBooleanTrue);
            caps->setObject("TransactionsSupported", kOSBooleanTrue);
            setProperty("IOAccelDisplayPipeCapabilities", caps);
            caps->release();
        }
    } else {
        removeProperty("IOAccelDisplayPipeCapabilities");
    }
}

// The family adds the framebuffer and stores its pipe at display machine +0x88 + 8 * index, with the framebuffer at
// pipe +0x98 (NVAccel's registerPendingLocked checks the same offsets on macOS 26).
bool PG106Accel::pairLocked(IOService *fb)
{
    if (!fDM) { PGA_LOG("pairing: no display machine"); return false; }
    const unsigned before = fDM->getFramebufferCount();
    fDM->found_framebuffer((IOFramebuffer *)fb);   // through our filter (PG106AccelDisplayMachine)
    const unsigned after = fDM->getFramebufferCount();
    if (after != before + 1) {
        PGA_LOG("pairing %s: the family did not add it (count %u -> %u)", fb->getName(), before, after);
        return false;
    }
    uint8_t *dm = (uint8_t *)fDM;
    void *pipe = before < 16 ? *(void *const volatile *)(dm + 0x88 + 8 * before) : nullptr;
    if (!pipe || *(IOService *const volatile *)((const uint8_t *)pipe + 0x98) != fb) {
        if (!pipe) *(volatile uint32_t *)(dm + 0x108) = before;   // NVAccel: a NULL pipe must not be counted
        PGA_LOG("pairing %s: pipe %p does not drive it; no display pipe this boot", fb->getName(), pipe);
        return false;
    }
    fb->retain();
    IOLockLock(fLock); fFB = fb; fPipe = pipe; IOLockUnlock(fLock);
    fDM->display_mode_will_change(0);
    fDM->display_mode_did_change(0);
    gW.infoValid = pgaFbInfo(fb, &gW.info);
    const enum pga_path path = framePath();
    setDisplayProps(true, pga_path_str(path));
    PGA_LOG("HDMI: %s has display pipe %p (head %u, %ux%u, online %u); frames: %s", fb->getName(), pipe, gW.info.head,
            gW.info.width, gW.info.height, gW.info.online, pga_path_str(path));
    return true;
}

void PG106Accel::tryPair(const char *why)
{
    if (!fPairLock) return;
    IOLockLock(fPairLock);
    IOLockLock(fLock);
    IOService *cand = fFBCand; if (cand) cand->retain();
    const bool paired = fFB != nullptr;
    IOLockUnlock(fLock);
    if (cand && !paired && fbAcceptable((IOFramebuffer *)cand, true)) {
        PGA_LOG("pairing %s with the HDMI framebuffer (%s)", why, cand->getName());
        if (!pairLocked(cand)) { fFBDecided = true; fPairFailed = true; }
    }
    if (cand) cand->release();
    IOLockUnlock(fPairLock);
    setProperty("PG106AccelFBVerdict", fPairFailed ? "accepted, but the family gave it no display pipe (see the log)"
                                                   : pga_fb_verdict_str(fFBVerdict));
}

void PG106Accel::fbPublished(IOService *fb)
{
    if (!fb || !fb->metaCast("PG106FB") || !fbIsOurs(fb)) return;   // not ours: never looked at again
    IOLockLock(fLock);
    IOService *old = fFBCand;
    if (old != fb) { fb->retain(); fFBCand = fb; fFBDecided = false; }
    else old = nullptr;
    IOLockUnlock(fLock);
    if (old) old->release();
    PGA_LOG("PG106FB %s published on %s", fb->getName(), fPCI->getName());
    tryPair("at publish");
    evaluateReady();
}

void PG106Accel::fbTerminated(IOService *fb)
{
    IOLockLock(fLock);
    IOService *paired = fFB == fb ? fFB : nullptr;
    IOService *cand = fFBCand == fb ? fFBCand : nullptr;
    if (paired) { fFB = nullptr; fPipe = nullptr; }
    if (cand) fFBCand = nullptr;
    IOLockUnlock(fLock);
    if (paired) {
        // the head is gone with it: the ring is not scanned out any more
        if (gW.loop) gW.loop->runAction([](OSObject *, void *, void *, void *, void *) -> IOReturn {
            pgaSrcDropAll(); pgaRingFree(nullptr); gW.infoValid = false; return kIOReturnSuccess; }, this);
        setDisplayProps(false, "none");
        PGA_LOG("HDMI framebuffer %s terminated: display pipe gone", fb->getName());
        paired->release();
    }
    if (cand) cand->release();
}

// ---- readiness, Metal, stats -------------------------------------------------------------------------------------
void PG106Accel::evaluateReady()
{
    IOService *pg = copyPG106();
    if (!pg) return;
    OSObject *p = pg->copyProperty("PG106Ready");
    const bool ready = p ? p == kOSBooleanTrue : true;   // no property: a PG106 older than PG106Ready
    if (p) p->release();
    pg->release();
    IOLockLock(fLock);
    const bool offered = fOffered, first = ready && !fReady;
    if (first) { fReady = true; fReadyAtUs = pgaNowUs(); }
    const bool latched = fReady;
    IOLockUnlock(fLock);
    if (!latched) {
        setState("waiting for PG106Ready");
        if (!fWaitLogged) { fWaitLogged = true; PGA_LOG("PG106 published on %s, bring-up not finished (PG106Ready false): "
                                                        "Metal is not offered yet, re-checking every %u ms", fPCI->getName(), kPgaReadyPollMs); }
        if (fPoll) fPoll->setTimeoutMS(kPgaReadyPollMs);
        return;
    }
    // ready (now or before): attach the kernel interface and the HDMI framebuffer as far as they go. INFO is read
    // again on every tick: PG106 sets its cap bits as each piece starts to work.
    IOLockLock(fPairLock);
    kifAttach();
    IOLockUnlock(fPairLock);
    if (fModeDisplay) {
        IOLockLock(fLock);
        const bool retry = fFBCand && !fFB && !fFBDecided;
        IOLockUnlock(fLock);
        if (retry || first) tryPair(first ? "at PG106Ready" : "retry");
    }
    if (!offered) {
        IOLockLock(fLock);
        const bool decided = fFB != nullptr || (fFBCand && fFBDecided);
        IOLockUnlock(fLock);
        const uint32_t waited = (uint32_t)((pgaNowUs() - fReadyAtUs) / 1000);
        if (pga_offer_now(fModeDisplay, decided, waited)) offerMetal(decided ? "HDMI framebuffer decided" : fModeDisplay ? "no PG106FB in time" : "headless");
        else setState("waiting for PG106FB");
    }
    publishStats();
    // display mode keeps the 2 s tick (pairing retries, PG106's caps as they come up, stats); headless stops once the
    // kernel interface and its copy channel are attached
    IOLockLock(fLock);
    const bool more = fModeDisplay || !fOffered || !fKifOK || !fChan;
    IOLockUnlock(fLock);
    if (fPoll && more) fPoll->setTimeoutMS(kPgaReadyPollMs);
}

void PG106Accel::offerMetal(const char *why)
{
    IOLockLock(fLock);
    const bool first = fPG106 && !fOffered;
    if (first) fOffered = true;
    const bool display = fFB != nullptr;
    IOLockUnlock(fLock);
    if (!first) return;
    if (fMetalPlugin) setProperty("MetalPluginName", fMetalPlugin);
    if (!display) setDisplayProps(false, nullptr);
    setState(display ? "Metal offered, HDMI display attached" : "Metal offered, headless");
    PGA_LOG("PG106 ready on %s: MetalPluginName set (%s; %s; render users: %s)", fPCI->getName(), why,
            display ? "WindowServer may composite HDMI on the GP106" : "apps only", renderUsersOpen() ? "everyone" : "administrators");
}

void PG106Accel::publishStats()
{
    OSDictionary *d = OSDictionary::withCapacity(kStCount);
    if (!d) return;
    for (unsigned i = 0; i < kStCount; i++) {
        OSNumber *n = OSNumber::withNumber((unsigned long long)gSt[i], 64);
        if (n) { d->setObject(kStNames[i], n); n->release(); }
    }
    setProperty("PG106AccelStats", d);
    d->release();
}

void PG106Accel::withdrawMetal(IOService *pg)
{
    IOService *old = nullptr;
    bool wasOffered = false;
    IOLockLock(fLock);
    if (fPG106 == pg) { old = fPG106; wasOffered = fOffered; fOffered = false; fReady = false; }
    IOLockUnlock(fLock);
    if (!old) return;
    if (fPoll) fPoll->cancelTimeout();
    removeProperty("MetalPluginName");
    kifDetach();   // its handles die with it (CHAN_CLOSE etc. are accepted and do nothing)
    IOLockLock(fLock); fPG106 = nullptr; IOLockUnlock(fLock);
    setState("waiting for PG106");
    old->release();
    PGA_LOG("PG106 terminated: %s", wasOffered ? "MetalPluginName withdrawn (processes that already made a device keep "
                                                 "it until they exit)" : "Metal was not offered");
}

// ---- lifecycle -----------------------------------------------------------------------------------------------
bool PG106Accel::start(IOService *provider)
{
    fPCI = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCI || gPga) { PGA_LOG("start: %s", fPCI ? "a PG106Accel is already running" : "provider is not a PCI device"); return false; }
    uint32_t v = 0;
    fModeDisplay = pga_mode_display(pgaBootArg("pg106accel_display", &v), v);
    v = 1; pgaBootArg("pg106accel_ce", &v); fCeAllowed = v != 0;
    v = 1; pgaBootArg("pg106accel_gpustamps", &v); fGpuStampsAllowed = v != 0;
    v = 1; pgaBootArg("pg106accel_vram", &v); fVramAllowed = v != 0;
    v = 0; pgaBootArg("pg106accel_aperture", &v); gPgaAperture = v != 0;
    fLock = IOLockAlloc();
    fPairLock = IOLockAlloc();
    gW.lock = IOLockAlloc();
    fVa = IORangeAllocator::withRange(1ULL << 40, 4096, 0, IORangeAllocator::kLocking);
    if (!fLock || !fPairLock || !gW.lock || !fVa) { PGA_LOG("start: out of memory"); return false; }
    pga_ring_init(&gW.ring);
    fVa->allocateRange(0, 0x100000);   // no placeholder address below 1 MB, so none is ever 0
    for (unsigned i = 0; i < kRanges; i++) fRanges[i] = fVa;
    // Held back before the family registers anything, so no process sees a Metal plugin before PG106 is there.
    if (OSObject *mp = getProperty("MetalPluginName")) { mp->retain(); fMetalPlugin = mp; removeProperty("MetalPluginName"); }
    setState("waiting for PG106");
    setProperty("PG106AccelDisplay", kOSBooleanFalse);
    setProperty("PG106AccelMode", fModeDisplay ? "display-attached (HDMI) + render offload" : "headless (pg106accel_display=0)");
    gPga = this;
    if (!IOGraphicsAccelerator2::start(provider)) { PGA_LOG("IOGraphicsAccelerator2::start failed"); gPga = nullptr; return false; }
    enableAccelerator();
    fPollLoop = IOWorkLoop::workLoop();
    fPoll = IOTimerEventSource::timerEventSource(this, &PG106Accel::pollFired);
    if (!fPollLoop || !fPoll || fPollLoop->addEventSource(fPoll) != kIOReturnSuccess) {
        PGA_LOG("start: no readiness poll (out of memory)");
        if (fPoll) { fPoll->release(); fPoll = nullptr; }
    }
    if (fModeDisplay) {
        gW.loop = IOWorkLoop::workLoop();
        gW.kick = IOTimerEventSource::timerEventSource(this, &pgaWorkerRun);
        if (!gW.loop || !gW.kick || gW.loop->addEventSource(gW.kick) != kIOReturnSuccess) {
            PGA_LOG("start: no frame worker (out of memory): headless");
            if (gW.kick) { gW.kick->release(); gW.kick = nullptr; }
            fModeDisplay = false;
        }
    }
    registerService();
    if (fModeDisplay) {
        if (OSDictionary *m = serviceMatching("PG106FB")) {
            fFBTermNote = addMatchingNotification(gIOTerminatedNotification, m, &PG106Accel::fbTerminatedCb, this, nullptr, 0);
            fFBPubNote = addMatchingNotification(gIOFirstPublishNotification, m, &PG106Accel::fbPublishedCb, this, nullptr, 0);
            m->release();
        }
    }
    if (OSDictionary *m = serviceMatching("PG106")) {
        fTermNote = addMatchingNotification(gIOTerminatedNotification, m, &PG106Accel::pg106Terminated, this, nullptr, 0);
        // delivered at once for a PG106 that published before this kext started
        fPubNote = addMatchingNotification(gIOPublishNotification, m, &PG106Accel::pg106Published, this, nullptr, 0);
        m->release();
    }
    PGA_LOG("up on %s %04x:%04x, %s; %s", fPCI->getName(), fPCI->configRead16(0x00), fPCI->configRead16(0x02),
            fModeDisplay ? "display-attached mode (HDMI via PG106FB) + render offload" : "headless (pg106accel_display=0)",
            getProperty("MetalPluginName") ? "Metal offered" : "Metal not offered yet (waiting for PG106 / PG106Ready)");
    return true;
}

void PG106Accel::stop(IOService *provider)
{
    if (fPubNote) { fPubNote->remove(); fPubNote = nullptr; }
    if (fTermNote) { fTermNote->remove(); fTermNote = nullptr; }
    if (fFBPubNote) { fFBPubNote->remove(); fFBPubNote = nullptr; }
    if (fFBTermNote) { fFBTermNote->remove(); fFBTermNote = nullptr; }
    if (fPoll) {
        fPoll->cancelTimeout();
        if (fPollLoop) fPollLoop->removeEventSource(fPoll);
        fPoll->release(); fPoll = nullptr;
    }
    if (fPollLoop) { fPollLoop->release(); fPollLoop = nullptr; }
    if (gW.kick) {
        gW.kick->cancelTimeout();
        if (gW.loop) gW.loop->removeEventSource(gW.kick);   // waits for a frame in flight
        gW.kick->release(); gW.kick = nullptr;
    }
    IOService *fb = copyFB();
    pgaSrcDropAll();
    pgaRingFree(fb);
    if (fb) fb->release();
    if (gW.loop) { gW.loop->release(); gW.loop = nullptr; }
    if (gW.lock) {
        IOLockLock(gW.lock);
        IOSurface *c = gW.cur, *n = gW.next; gW.cur = gW.next = nullptr; gW.busy = false; gW.pipe = nullptr;
        IOLockUnlock(gW.lock);
        pgaReleaseSurface(c); pgaReleaseSurface(n);
    }
    removeProperty("MetalPluginName");
    setDisplayProps(false, "none");
    kifDetach();
    IOService *pg = nullptr, *f = nullptr, *cand = nullptr;
    if (fLock) {
        IOLockLock(fLock);
        pg = fPG106; fPG106 = nullptr; fOffered = false;
        f = fFB; fFB = nullptr; fPipe = nullptr; cand = fFBCand; fFBCand = nullptr;
        IOLockUnlock(fLock);
    }
    if (pg) pg->release();
    if (f) f->release();
    if (cand) cand->release();
    IOGraphicsAccelerator2::stop(provider);
    if (gPga == this) gPga = nullptr;
}

void PG106Accel::free()
{
    if (fStamps) { fStamps->complete(); fStamps->release(); fStamps = nullptr; }
    if (fVa) { fVa->release(); fVa = nullptr; }
    if (fMetalPlugin) { fMetalPlugin->release(); fMetalPlugin = nullptr; }
    if (fLock) { IOLockFree(fLock); fLock = nullptr; }
    if (fPairLock) { IOLockFree(fPairLock); fPairLock = nullptr; }
    if (gW.lock && !gPga) { IOLockFree(gW.lock); gW.lock = nullptr; }
    IOGraphicsAccelerator2::free();
}

PGA_FWD_DEFS
