// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/src/PG106UserClient.cpp — the two Phase 3 selectors.
//
// The dispatch-table shape is NVRMUserClient::externalMethod
// (kexts/NVRM/NVRM.cpp:1921-1939): static IOExternalMethodDispatch entries,
// bounds-check the selector, let IOUserClient::externalMethod enforce the
// table's argument counts and sizes, then the static handler runs. The ABI
// lives in pg106_uc_abi.h so userspace compiles against the same header.

#include "PG106UserClient.hpp"

#include <IOKit/IOLib.h>     // IOLog
#include <libkern/libkern.h> // memcpy

#define UC_LOG(fmt, ...) IOLog("PG106-uc: " fmt "\n", ##__VA_ARGS__)

// libkern's OSDeclareDefaultStructors does not declare a `super` alias;
// the IOKit convention is this define, scoped to the .cpp.
#define super IOUserClient

OSDefineMetaClassAndStructors(PG106UserClient, IOUserClient)

bool
PG106UserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type,
			      OSDictionary *properties)
{
	// The selectors read raw MMIO-derived state and the later phases put
	// command submission here: root only. IOUserClient::clientHasPrivilege
	// with kIOClientPrivilegeAdministrator checks the opening task's
	// credentials (IOKit/IOUserClient.h); securityID is that task's token.
	if (IOUserClient::clientHasPrivilege(securityID,
					     kIOClientPrivilegeAdministrator) !=
	    kIOReturnSuccess) {
		UC_LOG("refusing non-administrator client");
		return false;
	}
	// One client type; anything else is refused rather than treated as a
	// device client (NVRM.cpp:1784-1794 does the same for its two types).
	if (type != PG106_UC_TYPE_DEVICE) {
		UC_LOG("refusing client type %u", (unsigned)type);
		return false;
	}
	return super::initWithTask(owningTask, securityID, type, properties);
}

bool
PG106UserClient::start(IOService *provider)
{
	// The personality's IOUserClientClass makes IOKit hand us the driver
	// as provider (same wiring as NVRMUserClient::start, NVRM.cpp:1778).
	fOwner = OSDynamicCast(PG106, provider);
	if (!fOwner)
		return false;
	return super::start(provider);
}

IOReturn
PG106UserClient::clientClose(void)
{
	// Default teardown, like NVRM.cpp:1840-1857 minus the per-client
	// state this skeleton does not have.
	if (!isInactive())
		terminate();
	return kIOReturnSuccess;
}

IOReturn
PG106UserClient::externalMethod(uint32_t selector,
				IOExternalMethodArguments *args,
				IOExternalMethodDispatch *dispatch,
				OSObject *target, void *reference)
{
	// { function, checkScalarInputCount, checkStructureInputSize,
	//   checkScalarOutputCount, checkStructureOutputSize } — field order
	// of IOExternalMethodDispatch (IOKit/IOUserClient.h). With fixed
	// (non-kIOUCVariableStructureSize) values, IOUserClient::externalMethod
	// rejects any call whose counts/sizes differ with kIOReturnBadArgument,
	// so no scalar or structure input is ever accepted.
	// Indexed by enum pg106_uc_selector, in declaration order.
	static_assert(PG106_UC_SEL_GET_VERSION == 0 && PG106_UC_SEL_GET_PMC == 1 &&
			      PG106_UC_SEL_COUNT == 2,
		      "dispatch table order must follow pg106_uc_selector");
	static const IOExternalMethodDispatch tbl[PG106_UC_SEL_COUNT] = {
		{ sGetVersion, 0, 0, 0, sizeof(struct pg106_uc_version) },
		{ sGetPmc, 0, 0, 0, sizeof(struct pg106_uc_pmc) },
	};
	(void)dispatch;
	(void)target;

	if (args == nullptr || selector >= PG106_UC_SEL_COUNT)
		return kIOReturnBadArgument;
	// Both selectors are synchronous; refuse the async (wake port) form.
	if (args->asyncWakePort != MACH_PORT_NULL)
		return kIOReturnBadArgument;

	IOExternalMethodDispatch d = tbl[selector];
	return super::externalMethod(selector, args, &d, this, reference);
}

// Shared output check for the fixed-size selectors: the dispatch table
// already pinned structureOutputSize, but the struct must also have come in
// inline (outputs above 4 KiB arrive as structureOutputDescriptor instead and
// structureOutput is NULL; ours are far smaller).
static bool
outputOk(const IOExternalMethodArguments *args, size_t size)
{
	return args->structureOutput != nullptr &&
	       args->structureOutputDescriptor == nullptr &&
	       args->structureOutputSize == size;
}

IOReturn
PG106UserClient::sGetVersion(OSObject *target, void *reference,
			     IOExternalMethodArguments *args)
{
	(void)target;
	(void)reference;
	if (!outputOk(args, sizeof(struct pg106_uc_version)))
		return kIOReturnBadArgument;

	// Zero-initialized whole struct: no field or padding byte reaches
	// userspace unwritten.
	struct pg106_uc_version v;
	memset(&v, 0, sizeof v);
	v.major = PG106_UC_ABI_MAJOR;
	v.minor = PG106_UC_ABI_MINOR;
	memcpy(args->structureOutput, &v, sizeof v);
	return kIOReturnSuccess;
}

IOReturn
PG106UserClient::sGetPmc(OSObject *target, void *reference,
			 IOExternalMethodArguments *args)
{
	(void)reference;
	PG106UserClient *self = OSDynamicCast(PG106UserClient, target);
	if (!self || !self->fOwner)
		return kIOReturnNotReady;
	if (!outputOk(args, sizeof(struct pg106_uc_pmc)))
		return kIOReturnBadArgument;

	struct pg106_uc_pmc out;
	memset(&out, 0, sizeof out);
	self->fOwner->pmcSnapshot(&out);
	memcpy(args->structureOutput, &out, sizeof out);
	return kIOReturnSuccess;
}
