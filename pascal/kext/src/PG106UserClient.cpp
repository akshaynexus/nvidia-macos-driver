// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/src/PG106UserClient.cpp — the two Phase 3 selectors.
//
// The dispatch-table shape is NVRMUserClient::externalMethod
// (kexts/NVRM/NVRM.cpp:1921-1939): static IOExternalMethodDispatch entries,
// bounds-check the selector, delegate the checked call to the static handler.
// The ABI lives in pg106_uc_abi.h so the future NVK bridge (Phase 4) can
// compile against it from userspace.

#include "PG106UserClient.hpp"

#include <IOKit/IOLib.h>     // IOLog
#include <libkern/libkern.h> // memcpy

#define UC_LOG(fmt, ...) IOLog("PG106-uc: " fmt "\n", ##__VA_ARGS__)

OSDefineMetaClassAndStructors(PG106UserClient, IOUserClient)

bool
PG106UserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type,
			      OSDictionary *properties)
{
	if (!super::initWithTask(owningTask, securityID, type, properties))
		return false;
	// One client type for now; anything else is refused rather than
	// silently treated as a device client (NVRM.cpp:1784-1794 does the
	// same for its two types).
	if (type != 0) {
		UC_LOG("refusing client type %u", (unsigned)type);
		return false;
	}
	return true;
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
	// IOService::terminate — IOKit/IOService.h; default behaviour like
	// NVRM.cpp:1840-1857's teardown, minus state this skeleton has none of.
	if (!isInactive())
		terminate();
	return kIOReturnSuccess;
}

void
PG106UserClient::free(void)
{
	super::free();
}

IOReturn
PG106UserClient::externalMethod(uint32_t selector,
				IOExternalMethodArguments *args,
				IOExternalMethodDispatch *dispatch,
				OSObject *target, void *reference)
{
	static const IOExternalMethodDispatch tbl[] = {
		// { action, checkScalarInput, checkStructInput,
		//   checkScalarOutput, checkStructOutput } — the field order
		// of IOExternalMethodDispatch (IOKit/IOUserClient.h), used the
		// same way as the NVRM table at NVRM.cpp:1924.
		{ sGetVersion, 0, 0, 0, sizeof(struct pg106_uc_version) },
		{ sGetPmcProbe, 0, 0, 0, sizeof(struct pg106_uc_pmc) },
	};
	if (selector >= sizeof tbl / sizeof tbl[0])
		return kIOReturnBadArgument;
	IOExternalMethodDispatch d = tbl[selector];
	return super::externalMethod(selector, args, &d, this, reference);
}

IOReturn
PG106UserClient::sGetVersion(OSObject *target, void *reference,
			     IOExternalMethodArguments *args)
{
	(void)target;
	(void)reference;
	if (!args->structureOutput ||
	    args->structureOutputSize < sizeof(struct pg106_uc_version))
		return kIOReturnBadArgument;

	struct pg106_uc_version v = { PG106_UC_ABI_MAJOR, PG106_UC_ABI_MINOR,
				      0 };
	memcpy(args->structureOutput, &v, sizeof v);
	return kIOReturnSuccess;
}

IOReturn
PG106UserClient::sGetPmcProbe(OSObject *target, void *reference,
			      IOExternalMethodArguments *args)
{
	PG106UserClient *self = OSDynamicCast(PG106UserClient, target);
	if (!self || !self->fOwner)
		return kIOReturnNotReady;
	if (!args->structureOutput ||
	    args->structureOutputSize < sizeof(struct pg106_uc_pmc))
		return kIOReturnBadArgument;

	struct pg106_uc_pmc out;
	self->fOwner->pmcSnapshot(&out.boot0, &out.probe);
	memcpy(args->structureOutput, &out, sizeof out);
	return kIOReturnSuccess;
}
