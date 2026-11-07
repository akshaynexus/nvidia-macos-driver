// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/src/PG106UserClient.hpp — the bridge's kernel endpoint.
//
// Shaped like NVRMUserClient (kexts/NVRM/NVRM.cpp:298-348): an IOUserClient
// declared in the personality (IOUserClientClass), created by IOKit with the
// driver as provider, serving selectors through an externalMethod dispatch
// table (NVRM.cpp:1921-1939). Two client types (pg106_uc_abi.h): DEVICE
// (root diagnostics) and RENDER (the bridge). Each connection owns one
// pascal/core dispatcher (kdisp.h): its own VM, BOs, syncobjs and handles.
// Every selector runs with the provider's command gate held.

#ifndef PG106USERCLIENT_HPP
#define PG106USERCLIENT_HPP

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>

#include "PG106.hpp"
#include "pg106_uc_abi.h"

struct pg106_kmem; // a BO's backing (PG106UserClient.cpp)

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
	IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options,
				     IOMemoryDescriptor **memory) override;

	// kdisp memory backend (static C callbacks reach these).
	int memAlloc(uint64_t size, uint32_t flags, struct pg106_kmem **out);
	int memImport(uint64_t addr, uint64_t size, struct pg106_kmem **out);
	void memFree(struct pg106_kmem *m);
	int mapAdd(uint32_t handle, IOMemoryDescriptor *md, IOOptionBits options);
	void mapRemove(uint32_t handle);
	PG106 *owner(void) const { return fOwner; }
	struct pg106_kd *kd(void) const { return fKd; }
	int vmFlush(const struct pg106_mmu *mmu);
	void waitEvent(uint32_t us);
	bool closing(void) const { return fClosing; }

private:
	static IOReturn sGetVersion(OSObject *target, void *reference,
				    IOExternalMethodArguments *args);
	static IOReturn sGetPmc(OSObject *target, void *reference,
				IOExternalMethodArguments *args);
	static IOReturn sSubmit(OSObject *target, void *reference,
				IOExternalMethodArguments *args);
	static IOReturn sGetStatus(OSObject *target, void *reference,
				   IOExternalMethodArguments *args);
	static IOReturn sGated(OSObject *owner, void *self, void *sel,
			       void *args, void *);
	static IOReturn sOpen(OSObject *owner, void *self, void *, void *, void *);
	static IOReturn sClose(OSObject *owner, void *self, void *, void *, void *);
	IOReturn submit(IOExternalMethodArguments *args);
	IOReturn submitWith(IOExternalMethodArguments *args, uint8_t *req,
			    uint8_t *resp);
	void closeOnGate(void);

	PG106 *fOwner = nullptr;
	task_t fTask = nullptr;
	UInt32 fType = 0;
	bool fSession = false;
	bool fClosed = false;
	struct pg106_kd *fKd = nullptr;
	uint64_t fTableBase = 0, fTableSize = 0; // client VM tables in VRAM
	uint32_t fWaiters = 0;                   // threads asleep in WAIT
	bool fClosing = false;
	uint8_t *fReq = nullptr;  // request frame buffer
	uint8_t *fResp = nullptr; // response frame buffer
	// BO map cookies (= handles) -> descriptor, for clientMemoryForType.
	struct map_ent {
		uint32_t handle;
		IOMemoryDescriptor *md;
		IOOptionBits options; // cache mode of the user mapping
	} *fMaps = nullptr;
	uint32_t fNMaps = 0, fCapMaps = 0;
};

#endif // PG106USERCLIENT_HPP
