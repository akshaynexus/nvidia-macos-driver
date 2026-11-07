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
//            syncobjs
//   stub   — no hardware channel (claimChannel answers -ENOSYS, so EXEC is
//            -ENODEV), page tables live only in the host model (vm_flush
//            NULL), VRAM BOs come from host memory, bus addresses are CPU
//            physical addresses (no IOMMU/IODMACommand: VT-d must be off)

#include "PG106UserClient.hpp"

#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <sys/errno.h>

#include "pg106_br_bridge.h" // PG106_BR_ABI_* (MIT, numbers only)
#include "pg106_br_wire.h"
#include "pg106_klibc.h"

#define UC_LOG(fmt, ...) IOLog("PG106-uc: " fmt "\n", ##__VA_ARGS__)

#define super IOUserClient

OSDefineMetaClassAndStructors(PG106UserClient, IOUserClient)

static_assert(PG106_UC_SEL_SUBMIT == PG106_BR_IOKIT_SEL_SUBMIT,
	      "bridge and kext disagree on the SUBMIT selector");
static_assert(PG106_UC_TYPE_RENDER == PG106_BR_IOKIT_TYPE,
	      "bridge and kext disagree on the render client type");

#define REQ_BUF_MAX  (sizeof(struct pg106_br_frame) + PG106_BR_WIRE_MAX_PAYLOAD)
#define RESP_BUF_MAX (sizeof(struct pg106_br_frame) + PG106_BR_WIRE_MAX_RESP)
// The GPU addresses physical memory up to 40 bits through this path
// (PG106_SUB_PUSH_ADDR_LIMIT, pascal/dispatch.h).
#define PG106_DMA_MASK 0x000000fffffff000ull

struct pg106_kmem {
	IOMemoryDescriptor *md;        // what clientMemoryForType hands out
	IOBufferMemoryDescriptor *bmd; // set for kernel-allocated BOs
	uint64_t size;
	bool prepared;                 // imported: prepare() done
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
	IOByteCount len = 0;
	addr64_t pa;

	if (off >= m->size)
		return -EFAULT;
	pa = m->md->getPhysicalSegment(off, &len, kIOMemoryMapperNone);
	if (!pa || !len)
		return -EFAULT;
	*addr = pa;
	*contig = len;
	return 0;
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
	int rc = uc(u)->mapAdd(handle, m->md);

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
kdChanBind(void *u, const struct pg106_kd_chan *)
{
	return uc(u)->owner()->claimChannel(u);
}

static void
kdChanUnbind(void *u)
{
	uc(u)->owner()->releaseChannel(u);
}

static void
kdKick(void *, uint32_t)
{
	// Unreachable until claimChannel binds a hardware channel.
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
kdNow(void *)
{
	uint64_t abs = 0, ns = 0;

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
	return uc(u)->owner()->gpuReady();
}

// ---------------------------------------------------------------------
// Memory backend
// ---------------------------------------------------------------------

int
PG106UserClient::memAlloc(uint64_t size, uint32_t, struct pg106_kmem **out)
{
	struct pg106_kmem *m = static_cast<struct pg106_kmem *>(
		pg106_kcalloc(1, sizeof(*m)));
	IOBufferMemoryDescriptor *b;

	if (!m)
		return -ENOMEM;
	// Wired, page-aligned host memory below 2^40, mappable into the
	// client (kIOMemoryKernelUserShared). VRAM placement is not
	// implemented: PG106_BR_BO_VRAM lands here too.
	b = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
		kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared,
		(size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1), PG106_DMA_MASK);
	if (!b) {
		pg106_kfree(m);
		return -ENOMEM;
	}
	memset(b->getBytesNoCopy(), 0, b->getLength());
	m->md = b;
	m->bmd = b;
	m->size = b->getLength();
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
	*out = m;
	return 0;
}

void
PG106UserClient::memFree(struct pg106_kmem *m)
{
	if (!m)
		return;
	if (m->prepared)
		m->md->complete(kIODirectionInOut); // unwire after the PTEs went
	m->md->release();
	pg106_kfree(m);
}

int
PG106UserClient::mapAdd(uint32_t handle, IOMemoryDescriptor *md)
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
PG106UserClient::clientMemoryForType(UInt32 type, IOOptionBits *options,
				     IOMemoryDescriptor **memory)
{
	// IOConnectMapMemory64(memoryType = cookie): the mapping retains the
	// descriptor, so the pages outlive a later BO free until the client
	// unmaps (IOUserClient::mapClientMemory64).
	for (uint32_t i = 0; i < fNMaps; i++) {
		if (fMaps[i].handle == type) {
			fMaps[i].md->retain();
			*memory = fMaps[i].md;
			*options = 0;
			return kIOReturnSuccess;
		}
	}
	return kIOReturnBadArgument;
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
	// DEVICE is always administrator-only. RENDER is too, unless the
	// owner opted in (the provider's policy is checked in start(), once
	// the provider is known).
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
	env.user = c;
	env.mem_alloc = kdMemAlloc;
	env.mem_import = kdMemImport;
	env.mem_free = kdMemFree;
	env.mem_dma = kdMemDma;
	env.mem_cpu = kdMemCpu;
	env.mem_map = kdMemMap;
	env.mem_unmap = kdMemUnmap;
	env.chan_bind = kdChanBind;
	env.chan_unbind = kdChanUnbind;
	env.vm_flush = nullptr; // page tables are a host model (stub)
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
			       "(boot-arg pg106_render_users=1 allows it)");
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
	// kdisp's teardown order: drain, channel off, unmap, unwire, free.
	pg106_kd_destroy(fKd);
	fKd = nullptr;
	fOwner->releaseChannel(this);
	if (fSession)
		fOwner->closeSession();
	fSession = false;
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

void
PG106UserClient::free(void)
{
	// clientClose normally ran; this covers a start() that failed after
	// the dispatcher was created.
	if (fKd && fOwner)
		fOwner->runOnGate(sClose, this);
	for (uint32_t i = 0; i < fNMaps; i++)
		fMaps[i].md->release();
	if (fMaps)
		pg106_kfree(fMaps);
	if (fReq)
		IOFree(fReq, REQ_BUF_MAX);
	if (fResp)
		IOFree(fResp, RESP_BUF_MAX);
	super::free();
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

IOReturn
PG106UserClient::submit(IOExternalMethodArguments *args)
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
		if (md->readBytes(0, fReq, in_len) != in_len) {
			md->complete(kIODirectionOut);
			return kIOReturnVMError;
		}
		md->complete(kIODirectionOut);
	} else {
		in_len = args->structureInputSize;
		if (!args->structureInput || in_len > REQ_BUF_MAX)
			return kIOReturnBadArgument;
		memcpy(fReq, args->structureInput, in_len);
	}
	if (!args->structureOutput || args->structureOutputDescriptor)
		return kIOReturnBadArgument;
	rc = pg106_kd_handle(fKd, fReq, in_len, fResp, RESP_BUF_MAX, &out_len);
	if (rc)
		return kIOReturnBadArgument; // not a frame at all
	if (args->structureOutputSize < out_len)
		return kIOReturnNoSpace;
	memcpy(args->structureOutput, fResp, out_len);
	args->structureOutputSize = (uint32_t)out_len;
	return kIOReturnSuccess;
}
