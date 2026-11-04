// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/src/PG106UserClient.hpp — userspace transport skeleton.
//
// Shaped like NVRMUserClient (kexts/NVRM/NVRM.cpp:298-348): an IOUserClient
// declared in the driver personality (Info.plist IOUserClientClass), created
// by IOKit with the driver as its provider, serving selectors through an
// externalMethod dispatch table (NVRM.cpp:1921-1939). Phase 3 exposes two
// selectors (pg106_uc_abi.h): the version handshake and the PMC identity.

#ifndef PG106USERCLIENT_HPP
#define PG106USERCLIENT_HPP

#include <IOKit/IOUserClient.h> // IOUserClient, IOExternalMethod{,Dispatch}Arguments

#include "PG106.hpp"
#include "pg106_uc_abi.h"

class PG106UserClient : public IOUserClient {
	OSDeclareDefaultStructors(PG106UserClient)

public:
	bool initWithTask(task_t owningTask, void *securityID, UInt32 type,
			  OSDictionary *properties) override;
	bool start(IOService *provider) override;
	IOReturn clientClose(void) override;
	void free(void) override;
	IOReturn externalMethod(uint32_t selector,
				IOExternalMethodArguments *args,
				IOExternalMethodDispatch *dispatch,
				OSObject *target, void *reference) override;

private:
	static IOReturn sGetVersion(OSObject *target, void *reference,
				    IOExternalMethodArguments *args);
	static IOReturn sGetPmcProbe(OSObject *target, void *reference,
				     IOExternalMethodArguments *args);

	PG106 *fOwner = nullptr; // the driver that owns this client
};

#endif // PG106USERCLIENT_HPP
