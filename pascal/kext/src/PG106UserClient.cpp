// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/src/PG106UserClient.cpp — selectors, the per-connection
// dispatcher (pascal/core kdisp) and its IOKit memory backend.
//
// The dispatch-table shape is NVRMUserClient::externalMethod
// (kexts/NVRM/NVRM.cpp:1921-1939). IOKit calls used (MacKernelSDK
// 3f750085 headers): IOUserClient.h (clientHasPrivilege, externalMethod,
// clientMemoryForType), IOBufferMemoryDescriptor.h (inTaskWithPhysicalMask),
// IOMemoryDescriptor.h (withAddressRange, prepare/complete,
// getPhysicalSegment with kIOMemoryMapperNone, readBytes).
//
// What is real and what is not (pascal/kext/README.md has the full list):
//   real   — framing, validation, handles, VM table, BO memory (wired host
//            pages), host import (wired task pages), CPU mappings,
//            syncobjs; bus addresses from IODMACommand (VT-d safe); the
//            client VM's page tables in VRAM (uploaded + TLB-invalidated
//            by vm_flush) once the GPU memory layer is up; SYNCOBJ_WAIT
//            sleeps on the fence (non-stall) interrupt with the gate
//            dropped
//   channel — claimChannel binds a GR channel with its context (PG106.cpp)
//            until the GR context is ported, so EXEC is -ENODEV), VRAM
//            BOs come from host memory

#include "PG106UserClient.hpp"

#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <sys/errno.h>

#include "pg106_br_bridge.h" // PG106_BR_ABI_* (MIT, numbers only)
#include "pg106_dma.h"
#include "pg106_br_wire.h"
#include "pg106_klibc.h"

#include "pg106_log.h"
#define UC_LOG(fmt, ...) PG106_LOGLINE("PG106-uc: ", fmt, ##__VA_ARGS__)

#define super IOUserClient

OSDefineMetaClassAndStructors(PG106UserClient, IOUserClient)

static_assert(PG106_UC_SEL_SUBMIT == PG106_BR_IOKIT_SEL_SUBMIT,
	      "bridge and kext disagree on the SUBMIT selector");
static_assert(PG106_UC_TYPE_RENDER == PG106_BR_IOKIT_TYPE,
	      "bridge and kext disagree on the render client type");

#define REQ_BUF_MAX  (sizeof(struct pg106_br_frame) + PG106_BR_WIRE_MAX_PAYLOAD)
#define RESP_BUF_MAX (sizeof(struct pg106_br_frame) + PG106_BR_WIRE_MAX_RESP)
// Per-connection VRAM for the client VM's page tables (1024 tables).
#define PG106_UC_VM_TABLES 0x400000ull

struct pg106_kmem {
	IOMemoryDescriptor *md;        // what clientMemoryForType hands out
				       // (a BAR1 window's: owned by the
				       // provider's registry, uwinAdd)
	IOBufferMemoryDescriptor *bmd; // set for kernel-allocated BOs
	struct pg106_dma *dma;         // IODMACommand mapping (bus addresses)
	uint64_t size;
	bool prepared;                 // imported: prepare() done
	// PG106_BR_BO_VRAM: device memory from the VRAM allocator; with
	// CAN_MAP a BAR1 window (md = its physical range, write-combined).
	bool vram, bar1;
	uint64_t vram_pa, bar1_off;
};

// ---------------------------------------------------------------------
// kdisp env callbacks
// ---------------------------------------------------------------------

static PG106UserClient *
uc(void *u)
{
	return static_cast<PG106UserClient *>(u);
}

static int
kdMemAlloc(void *u, uint64_t size, uint64_t, uint32_t flags, void **mem)
{
	return uc(u)->memAlloc(size, flags, reinterpret_cast<struct pg106_kmem **>(mem));
}

static int
kdMemImport(void *u, uint64_t addr, uint64_t size, uint32_t, void **mem)
{
	return uc(u)->memImport(addr, size, reinterpret_cast<struct pg106_kmem **>(mem));
}

static void
kdMemFree(void *u, void *mem)
{
	uc(u)->memFree(static_cast<struct pg106_kmem *>(mem));
}

static int
kdMemDma(void *, void *mem, uint64_t off, uint64_t *addr, uint64_t *contig)
{
	struct pg106_kmem *m = static_cast<struct pg106_kmem *>(mem);

	if (off >= m->size)
		return -EFAULT;
	if (m->vram) { // contiguous VRAM
		*addr = m->vram_pa + off;
		if (contig)
			*contig = m->size - off;
		return 0;
	}
	// The system mapper's IOVA (VT-d on) or the physical address (off).
	return pg106_dma_addr(m->dma, off, addr, contig);
}

static int
kdMemTarget(void *, void *mem)
{
	struct pg106_kmem *m = static_cast<struct pg106_kmem *>(mem);

	return m->vram ? PG106_TARGET_VRAM : PG106_TARGET_SYS_COHERENT;
}

static void *
kdMemCpu(void *, void *mem)
{
	struct pg106_kmem *m = static_cast<struct pg106_kmem *>(mem);

	return m->bmd ? m->bmd->getBytesNoCopy() : nullptr;
}

static int
kdMemMap(void *u, void *mem, uint32_t handle, uint64_t *cookie)
{
	struct pg106_kmem *m = static_cast<struct pg106_kmem *>(mem);
	int rc;

	if (!m->md)
		return -EACCES; // VRAM without CAN_MAP: no BAR1 window
	rc = uc(u)->mapAdd(handle, m->md,
			   m->bar1 ? kIOMapWriteCombineCache : kIOMapDefaultCache);

	if (!rc)
		*cookie = handle; // the IOConnectMapMemory64 memory type
	return rc;
}

static void
kdMemUnmap(void *u, void *, uint64_t cookie)
{
	uc(u)->mapRemove((uint32_t)cookie);
}

static int
kdChanBind(void *u, const struct pg106_kd_chan *ch)
{
	return uc(u)->owner()->claimChannel(u, uc(u)->kd(), ch);
}

static void
kdChanUnbind(void *u)
{
	uc(u)->owner()->releaseChannel(u, uc(u)->kd());
}

static void
kdKick(void *u, uint32_t put)
{
	uc(u)->owner()->chanKick(u, put);
}

static int
kdVmFlush(void *u, const struct pg106_mmu *mmu)
{
	return uc(u)->vmFlush(mmu);
}

static void
kdWait(void *u, uint32_t us)
{
	uc(u)->waitEvent(us);
}

static void
kdDelay(void *, uint32_t us)
{
	if (us >= 1000)
		IOSleep(us / 1000);
	else
		IODelay(us);
}

static uint64_t
kdNow(void *u)
{
	uint64_t abs = 0, ns = 0;

	// A closing connection ends its sleeping waits as timed out.
	if (uc(u)->closing())
		return UINT64_MAX / 2;

	clock_get_uptime(&abs);
	absolutetime_to_nanoseconds(abs, &ns);
	return ns;
}

static uint64_t
kdPtimer(void *u)
{
	return uc(u)->owner()->ptimerNs();
}

static bool
kdReady(void *u)
{
	return !uc(u)->stale() && uc(u)->owner()->gpuReady();
}

bool
PG106UserClient::stale(void) const
{
	return fOwner && fEpoch != fOwner->memEpoch();
}

// ---------------------------------------------------------------------
// Memory backend
// ---------------------------------------------------------------------

int
PG106UserClient::memAlloc(uint64_t size, uint32_t flags, struct pg106_kmem **out)
{
	struct pg106_kmem *m = static_cast<struct pg106_kmem *>(
		pg106_kcalloc(1, sizeof(*m)));
	int rc;

	if (!m)
		return -ENOMEM;
	if ((flags & PG106_BR_BO_VRAM) && stale()) {
		pg106_kfree(m);
		return -ENODEV;
	}
	if (flags & PG106_BR_BO_VRAM) {
		// Windows whose last user mapping went away since: back to the
		// allocators first.
		fOwner->uwinReap();
		// Device memory: the VRAM allocator (zeroed through PRAMIN,
		// like nouveau's zeroed VRAM BOs), and for CAN_MAP a BAR1
		// window whose physical range the client maps write-combined
		// (nouveau: ttm's WC BAR1 mmap).
		rc = fOwner->vramAlloc(size, 0x1000, PG106_VRAM_ZERO, &m->vram_pa);
		if (rc) {
			pg106_kfree(m);
			return rc;
		}
		m->vram = true;
		m->size = size;
		if (flags & PG106_BR_BO_CAN_MAP) {
			rc = fOwner->bar1Map(m->vram_pa, size, &m->bar1_off);
			if (rc) {
				fOwner->vramFree(m->vram_pa, size);
				pg106_kfree(m);
				return rc;
			}
			m->bar1 = true;
			// TASK_NULL: the range is physical (withPhysicalAddress).
			m->md = IOMemoryDescriptor::withAddressRange(
				fOwner->bar1Phys() + m->bar1_off, size, kIODirectionInOut,
				TASK_NULL);
			if (!m->md) {
				fOwner->bar1Unmap(m->bar1_off, size);
				fOwner->vramFree(m->vram_pa, size);
				pg106_kfree(m);
				return -ENOMEM;
			}
			// From here the window and its VRAM go back only when no
			// user mapping of m->md is left (PG106::uwinFree).
			rc = fOwner->uwinAdd(m->md, this, m->bar1_off, size, m->vram_pa);
			if (rc) {
				m->md->release();
				fOwner->bar1Unmap(m->bar1_off, size);
				fOwner->vramFree(m->vram_pa, size);
				pg106_kfree(m);
				return rc;
			}
		}
		*out = m;
		return 0;
	}
	// Wired, zeroed, page-aligned host memory below 2^40, mappable into
	// the client (kIOMemoryKernelUserShared), DMA-mapped.
	rc = pg106_dma_alloc(size, &m->dma);
	if (rc) {
		pg106_kfree(m);
		return rc;
	}
	m->bmd = m->dma->bmd;
	m->md = m->dma->md; // owned by m->dma
	m->size = m->dma->size;
	*out = m;
	return 0;
}

int
PG106UserClient::memImport(uint64_t addr, uint64_t size, struct pg106_kmem **out)
{
	struct pg106_kmem *m = static_cast<struct pg106_kmem *>(
		pg106_kcalloc(1, sizeof(*m)));
	IOMemoryDescriptor *md;

	if (!m)
		return -ENOMEM;
	// The caller's pages, wired for the BO lifetime (NOUVEAU-UAPI.md
	// section 6.1 rule 4): prepare() here, complete() in memFree.
	md = IOMemoryDescriptor::withAddressRange(addr, size, kIODirectionInOut, fTask);
	if (!md) {
		pg106_kfree(m);
		return -ENOMEM;
	}
	if (md->prepare(kIODirectionInOut) != kIOReturnSuccess) {
		md->release();
		pg106_kfree(m);
		return -EFAULT;
	}
	m->md = md;
	m->size = size;
	m->prepared = true;
	if (pg106_dma_wrap(md, &m->dma)) {
		md->complete(kIODirectionInOut);
		md->release();
		pg106_kfree(m);
		return -EFAULT;
	}
	*out = m;
	return 0;
}

void
PG106UserClient::memFree(struct pg106_kmem *m)
{
	if (!m)
		return;
	if (m->vram) {
		// A BAR1 window: the registry gives it and its VRAM back now if
		// no user mapping is left, else once the last one goes (a later
		// allocation must never get a window a stale mapping still
		// reaches); a stale one (memory lost since) only goes.
		if (m->bar1 && fOwner->uwinFree(m->md)) {
			pg106_kfree(m);
			return;
		}
		if (m->md) // not registered (cannot happen): the old rule
			m->md->release();
		if (!stale()) { // else the VRAM and BAR1 window died with the GPU
			if (m->bar1)
				fOwner->bar1Unmap(m->bar1_off, m->size);
			fOwner->vramFree(m->vram_pa, m->size);
		}
		pg106_kfree(m);
		return;
	}
	if (!m->prepared) { // kernel BO: the DMA object owns the descriptor
		pg106_dma_free(m->dma);
		pg106_kfree(m);
		return;
	}
	pg106_dma_free(m->dma); // IOVAs gone (the PTEs already were)
	m->md->complete(kIODirectionInOut); // unwire
	m->md->release();
	pg106_kfree(m);
}

int
PG106UserClient::mapAdd(uint32_t handle, IOMemoryDescriptor *md,
			IOOptionBits options)
{
	if (fNMaps == fCapMaps) {
		uint32_t ncap = fCapMaps ? fCapMaps * 2 : 64;
		void *n = pg106_krealloc(fMaps, ncap * sizeof(*fMaps));

		if (!n)
			return -ENOMEM;
		fMaps = static_cast<struct map_ent *>(n);
		fCapMaps = ncap;
	}
	md->retain();
	fMaps[fNMaps].handle = handle;
	fMaps[fNMaps].md = md;
	fMaps[fNMaps].options = options;
	fNMaps++;
	return 0;
}

void
PG106UserClient::mapRemove(uint32_t handle)
{
	for (uint32_t i = 0; i < fNMaps; i++) {
		if (fMaps[i].handle == handle) {
			fMaps[i].md->release(); // live user mappings hold their own
			fMaps[i] = fMaps[--fNMaps];
			return;
		}
	}
}

IOReturn
PG106UserClient::sMapLookup(OSObject *, void *self, void *type, void *options,
			    void *memory)
{
	PG106UserClient *c = static_cast<PG106UserClient *>(self);
	const uint32_t t = (uint32_t)(uintptr_t)type;

	// A stale connection's windows belong to no allocator any more
	// (redirected, or scratch-backed, or cut): no new mapping of them.
	if (c->fClosed || c->stale())
		return kIOReturnNotReady;
	for (uint32_t i = 0; i < c->fNMaps; i++) {
		if (c->fMaps[i].handle == t) {
			c->fMaps[i].md->retain();
			*static_cast<IOMemoryDescriptor **>(memory) = c->fMaps[i].md;
			*static_cast<IOOptionBits *>(options) = c->fMaps[i].options;
			return kIOReturnSuccess;
		}
	}
	return kIOReturnBadArgument;
}

IOReturn
PG106UserClient::clientMemoryForType(UInt32 type, IOOptionBits *options,
				     IOMemoryDescriptor **memory)
{
	// IOConnectMapMemory64(memoryType = cookie): the mapping retains the
	// descriptor, so the pages outlive a later BO free until the client
	// unmaps (IOUserClient::mapClientMemory64). For a BAR1 window that
	// retain is what keeps the window and its VRAM allocated after the BO
	// is freed (PG106::uwinFree). On the gate: the map table changes
	// under SUBMIT, and the registry counts references there.
	if (!fOwner || !options || !memory)
		return kIOReturnBadArgument;
	return fOwner->runOnGate(sMapLookup, this, (void *)(uintptr_t)type, options,
				 memory);
}

// ---------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------

bool
PG106UserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type,
			      OSDictionary *properties)
{
	const bool admin = IOUserClient::clientHasPrivilege(
				   securityID, kIOClientPrivilegeAdministrator) ==
			   kIOReturnSuccess;

	if (type != PG106_UC_TYPE_DEVICE && type != PG106_UC_TYPE_RENDER) {
		UC_LOG("refusing client type %u", (unsigned)type);
		return false;
	}
	// DEVICE is always administrator-only. RENDER is open to every local
	// user unless the owner restricted it (the provider's policy is
	// checked in start(), once the provider is known).
	if (type == PG106_UC_TYPE_DEVICE && !admin) {
		UC_LOG("refusing non-administrator device client");
		return false;
	}
	if (!super::initWithTask(owningTask, securityID, type, properties))
		return false;
	fTask = owningTask;
	fType = type;
	fReq = static_cast<uint8_t *>(IOMalloc(REQ_BUF_MAX));
	fResp = static_cast<uint8_t *>(IOMalloc(RESP_BUF_MAX));
	if (!fReq || !fResp)
		return false;
	// Remember whether the task was an administrator for start().
	if (!admin)
		fClosed = true; // provisional: start() clears it if allowed
	return true;
}

IOReturn
PG106UserClient::sOpen(OSObject *, void *self, void *, void *, void *)
{
	PG106UserClient *c = static_cast<PG106UserClient *>(self);
	struct pg106_kd_env env = {};
	int rc;

	c->fOwner->kdEnvBase(&env);
	c->fEpoch = c->fOwner->memEpoch();
	env.user = c;
	env.mem_alloc = kdMemAlloc;
	env.mem_import = kdMemImport;
	env.mem_free = kdMemFree;
	env.mem_dma = kdMemDma;
	env.mem_target = kdMemTarget;
	env.mem_cpu = kdMemCpu;
	env.mem_map = kdMemMap;
	env.mem_unmap = kdMemUnmap;
	env.chan_bind = kdChanBind;
	env.chan_unbind = kdChanUnbind;
	// The client VM's page tables: in VRAM when the memory layer is up
	// (uploaded + TLB-invalidated on every change), else the host model.
	if (c->fOwner->vramAlloc(PG106_UC_VM_TABLES, 0x1000, PG106_VRAM_TAIL,
				 &c->fTableBase) == 0) {
		c->fTableSize = PG106_UC_VM_TABLES;
		env.mmu_cfg.table_target = PG106_TARGET_VRAM;
		env.mmu_cfg.table_base = c->fTableBase;
		env.mmu_cfg.table_limit = c->fTableBase + c->fTableSize;
		env.vm_flush = kdVmFlush;
	} else {
		env.mmu_cfg.table_base = 0x40000000ull; // host model only
		env.vm_flush = nullptr;
	}
	env.wait_event = kdWait;
	env.kick = kdKick;
	env.delay_us = kdDelay;
	env.now_ns = kdNow;
	env.ptimer_ns = kdPtimer;
	env.ready = kdReady;
	env.abi_major = PG106_BR_ABI_MAJOR;
	env.abi_minor = PG106_BR_ABI_MINOR;
	rc = pg106_kd_create(&env, &c->fKd);
	if (rc)
		return kIOReturnNoMemory;
	if (c->fType == PG106_UC_TYPE_RENDER) {
		// A render session powers the GPU up / waits for bring-up.
		// Failure does not fail the open: ops answer -ENODEV and
		// GETPARAM READY reads 0.
		rc = c->fOwner->openSession();
		c->fSession = rc == 0;
		if (rc)
			UC_LOG("render session: GPU not ready (%d)", rc);
	}
	return kIOReturnSuccess;
}

bool
PG106UserClient::start(IOService *provider)
{
	fOwner = OSDynamicCast(PG106, provider);
	if (!fOwner)
		return false;
	if (fClosed) {
		// Non-administrator: only a RENDER client, and only if the
		// owner allowed it.
		if (fType != PG106_UC_TYPE_RENDER || !fOwner->renderUsersAllowed()) {
			UC_LOG("refusing non-administrator render client "
			       "(restricted by pg106_render_users=0 / PG106RenderUsers)");
			return false;
		}
		fClosed = false;
	}
	if (!super::start(provider))
		return false;
	if (fOwner->runOnGate(sOpen, this) != kIOReturnSuccess)
		return false;
	return true;
}

void
PG106UserClient::closeOnGate(void)
{
	if (fClosed)
		return;
	fClosed = true;
	// Waits sleeping in SYNCOBJ_WAIT (gate dropped) end now: kdNow
	// reports their timeout. kdisp may only be destroyed once none is
	// inside it any more.
	fClosing = true;
	fOwner->fenceWake();
	// Each waiter sleeps 10 ms at most and leaves on its next look at kdNow,
	// so this is short. A close must end even if that ever fails.
	{
		AbsoluteTime deadline;

		clock_interval_to_deadline(5, kSecondScale, &deadline);
		while (fWaiters &&
		       fOwner->gate()->commandSleep(&fWaiters, deadline, THREAD_UNINT) !=
			       THREAD_TIMED_OUT)
			;
	}
	if (fWaiters) {
		// kdisp is still in use: it and this object stay (never
		// released). Bookkeeping below still lets the GPU idle.
		UC_LOG("close: %u SYNCOBJ_WAIT thread(s) still inside after 5 s: "
		       "leaving the connection allocated", fWaiters);
		retain();
	} else {
		// kdisp's teardown order: drain, channel off, unmap, unwire, free.
		pg106_kd_destroy(fKd);
		fKd = nullptr;
		if (fTableSize) {
			if (!stale())
				fOwner->vramFree(fTableBase, fTableSize);
			fTableSize = 0;
		}
		fOwner->releaseChannel(this, nullptr); // kdisp already unbound it
	}
	if (fSession)
		fOwner->closeSession();
	fSession = false;
	// BOs freed above whose windows were not mapped are back already;
	// mapped ones wait for their mappings (IOUserClient::free releases
	// the ones IOKit keeps for this connection).
	fOwner->uwinReap();
}

IOReturn
PG106UserClient::sClose(OSObject *, void *self, void *, void *, void *)
{
	static_cast<PG106UserClient *>(self)->closeOnGate();
	return kIOReturnSuccess;
}

IOReturn
PG106UserClient::clientClose(void)
{
	if (fOwner)
		fOwner->runOnGate(sClose, this);
	if (!isInactive())
		terminate();
	return kIOReturnSuccess;
}

// The windows' map set is going (super::free): no registry entry may name it.
IOReturn
PG106UserClient::sDisown(OSObject *owner, void *self, void *, void *, void *)
{
	PG106 *p = OSDynamicCast(PG106, owner);

	if (p)
		p->uwinDisown(self);
	return kIOReturnSuccess;
}

void
PG106UserClient::uwinCut(IOMemoryDescriptor *md)
{
	IOMemoryMap *map;

	while ((map = removeMappingForDescriptor(md)) != nullptr)
		map->release();
}

IOReturn
PG106UserClient::sReap(OSObject *owner, void *, void *, void *, void *)
{
	PG106 *p = OSDynamicCast(PG106, owner);

	if (p)
		p->uwinReap();
	return kIOReturnSuccess;
}

void
PG106UserClient::free(void)
{
	PG106 *owner = fOwner;

	// clientClose normally ran; this covers a start() that failed after
	// the dispatcher was created.
	if (fKd && fOwner)
		fOwner->runOnGate(sClose, this);
	if (fOwner)
		fOwner->runOnGate(sDisown, this);
	for (uint32_t i = 0; i < fNMaps; i++)
		fMaps[i].md->release();
	if (fMaps)
		pg106_kfree(fMaps);
	if (fReq)
		IOFree(fReq, REQ_BUF_MAX);
	if (fResp)
		IOFree(fResp, RESP_BUF_MAX);
	if (owner)
		owner->retain();
	// IOUserClient::free releases the mappings IOKit kept for this
	// connection: the BAR1 windows they held can go back after it.
	super::free();
	if (owner) {
		owner->runOnGate(sReap);
		owner->release();
	}
}

// ---------------------------------------------------------------------
// Selectors
// ---------------------------------------------------------------------

IOReturn
PG106UserClient::externalMethod(uint32_t selector,
				IOExternalMethodArguments *args,
				IOExternalMethodDispatch *dispatch,
				OSObject *target, void *reference)
{
	// { function, checkScalarInputCount, checkStructureInputSize,
	//   checkScalarOutputCount, checkStructureOutputSize }
	// (IOKit/IOUserClient.h). SUBMIT takes variable-size structures:
	// inputs above 4 KiB arrive as structureInputDescriptor.
	static_assert(PG106_UC_SEL_GET_VERSION == 0 && PG106_UC_SEL_GET_PMC == 1 &&
			      PG106_UC_SEL_SUBMIT == 2 && PG106_UC_SEL_GET_STATUS == 3 &&
			      PG106_UC_SEL_COUNT == 4,
		      "dispatch table order must follow pg106_uc_selector");
	static const IOExternalMethodDispatch tbl[PG106_UC_SEL_COUNT] = {
		{ sGetVersion, 0, 0, 0, sizeof(struct pg106_uc_version) },
		{ sGetPmc, 0, 0, 0, sizeof(struct pg106_uc_pmc) },
		{ sSubmit, 0, kIOUCVariableStructureSize, 0, kIOUCVariableStructureSize },
		{ sGetStatus, 0, 0, 0, sizeof(struct pg106_uc_status) },
	};
	(void)dispatch;
	(void)target;

	if (args == nullptr || selector >= PG106_UC_SEL_COUNT)
		return kIOReturnBadArgument;
	if (args->asyncWakePort != MACH_PORT_NULL)
		return kIOReturnBadArgument;
	if (fType == PG106_UC_TYPE_RENDER && selector == PG106_UC_SEL_GET_PMC)
		return kIOReturnNotPrivileged;
	IOExternalMethodDispatch d = tbl[selector];
	return super::externalMethod(selector, args, &d, this, reference);
}

static bool
outputOk(const IOExternalMethodArguments *args, size_t size)
{
	return args->structureOutput != nullptr &&
	       args->structureOutputDescriptor == nullptr &&
	       args->structureOutputSize == size;
}

IOReturn
PG106UserClient::sGetVersion(OSObject *, void *, IOExternalMethodArguments *args)
{
	struct pg106_uc_version v;

	if (!outputOk(args, sizeof(v)))
		return kIOReturnBadArgument;
	memset(&v, 0, sizeof v);
	v.major = PG106_UC_ABI_MAJOR;
	v.minor = PG106_UC_ABI_MINOR;
	memcpy(args->structureOutput, &v, sizeof v);
	return kIOReturnSuccess;
}

IOReturn
PG106UserClient::sGetPmc(OSObject *target, void *, IOExternalMethodArguments *args)
{
	PG106UserClient *self = OSDynamicCast(PG106UserClient, target);
	struct pg106_uc_pmc out;

	if (!self || !self->fOwner)
		return kIOReturnNotReady;
	if (!outputOk(args, sizeof(out)))
		return kIOReturnBadArgument;
	memset(&out, 0, sizeof out);
	self->fOwner->pmcSnapshot(&out);
	memcpy(args->structureOutput, &out, sizeof out);
	return kIOReturnSuccess;
}

IOReturn
PG106UserClient::sGated(OSObject *, void *self, void *sel, void *args, void *)
{
	PG106UserClient *c = static_cast<PG106UserClient *>(self);
	IOExternalMethodArguments *a = static_cast<IOExternalMethodArguments *>(args);

	if ((uintptr_t)sel == PG106_UC_SEL_SUBMIT)
		return c->submit(a);
	{
		struct pg106_uc_status st;

		c->fOwner->statusSnapshot(&st);
		memcpy(a->structureOutput, &st, sizeof st);
		return kIOReturnSuccess;
	}
}

IOReturn
PG106UserClient::sGetStatus(OSObject *target, void *, IOExternalMethodArguments *args)
{
	PG106UserClient *self = OSDynamicCast(PG106UserClient, target);

	if (!self || !self->fOwner)
		return kIOReturnNotReady;
	if (!outputOk(args, sizeof(struct pg106_uc_status)))
		return kIOReturnBadArgument;
	return self->fOwner->runOnGate(sGated, self,
				       (void *)(uintptr_t)PG106_UC_SEL_GET_STATUS,
				       args);
}

IOReturn
PG106UserClient::sSubmit(OSObject *target, void *, IOExternalMethodArguments *args)
{
	PG106UserClient *self = OSDynamicCast(PG106UserClient, target);

	if (!self || !self->fOwner)
		return kIOReturnNotReady;
	return self->fOwner->runOnGate(sGated, self,
				       (void *)(uintptr_t)PG106_UC_SEL_SUBMIT, args);
}

int
PG106UserClient::vmFlush(const struct pg106_mmu *mmu)
{
	if (stale())
		return -ENODEV; // the tables' VRAM is someone else's now
	return fOwner->vmFlush(const_cast<struct pg106_mmu *>(mmu), fTableBase,
			       fTableSize);
}

void
PG106UserClient::waitEvent(uint32_t us)
{
	fWaiters++;
	fOwner->fenceWait(us);
	if (--fWaiters == 0 && fClosing)
		fOwner->gate()->commandWakeup(&fWaiters);
}

IOReturn
PG106UserClient::submit(IOExternalMethodArguments *args)
{
	// While a SYNCOBJ_WAIT of this connection sleeps (gate dropped),
	// another thread's call runs in its own buffers: the sleeper writes
	// its response into fResp only after it wakes, and holds no pointer
	// into kdisp's tables across the sleep (kdisp.c op_syncobj_wait).
	if (fWaiters) {
		uint8_t *req = static_cast<uint8_t *>(IOMalloc(REQ_BUF_MAX));
		uint8_t *resp = static_cast<uint8_t *>(IOMalloc(RESP_BUF_MAX));
		IOReturn r = kIOReturnNoMemory;

		if (req && resp)
			r = submitWith(args, req, resp);
		if (req)
			IOFree(req, REQ_BUF_MAX);
		if (resp)
			IOFree(resp, RESP_BUF_MAX);
		return r;
	}
	return submitWith(args, fReq, fResp);
}

IOReturn
PG106UserClient::submitWith(IOExternalMethodArguments *args, uint8_t *req,
			    uint8_t *resp)
{
	size_t in_len, out_len = 0;
	int rc;

	if (fClosed || !fKd)
		return kIOReturnNotOpen;
	// Request: inline (<= 4 KiB) or as a descriptor (larger VM_BIND /
	// EXEC batches).
	if (args->structureInputDescriptor) {
		IOMemoryDescriptor *md = args->structureInputDescriptor;

		in_len = md->getLength();
		if (in_len > REQ_BUF_MAX)
			return kIOReturnBadArgument;
		if (md->prepare(kIODirectionOut) != kIOReturnSuccess)
			return kIOReturnVMError;
		if (md->readBytes(0, req, in_len) != in_len) {
			md->complete(kIODirectionOut);
			return kIOReturnVMError;
		}
		md->complete(kIODirectionOut);
	} else {
		in_len = args->structureInputSize;
		if (!args->structureInput || in_len > REQ_BUF_MAX)
			return kIOReturnBadArgument;
		memcpy(req, args->structureInput, in_len);
	}
	if (!args->structureOutput || args->structureOutputDescriptor)
		return kIOReturnBadArgument;
	rc = pg106_kd_handle(fKd, req, in_len, resp, RESP_BUF_MAX, &out_len);
	if (rc)
		return kIOReturnBadArgument; // not a frame at all
	if (args->structureOutputSize < out_len)
		return kIOReturnNoSpace;
	memcpy(args->structureOutput, resp, out_len);
	args->structureOutputSize = (uint32_t)out_len;
	return kIOReturnSuccess;
}
