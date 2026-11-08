// SPDX-License-Identifier: MIT
/*
 * transport_iokit.c - pg106_br_transport over PG106UserClient (see
 * include/pg106_br_iokit.h). IOKit.framework calls, all from
 * IOKit/IOKitLib.h: IOServiceMatching, IOServiceGetMatchingService,
 * IOServiceOpen, IOServiceClose, IOObjectRelease, IOConnectCallStructMethod,
 * IOConnectMapMemory64, IOConnectUnmapMemory64. MACH_PORT_NULL selects the
 * default main port (avoids the kIOMasterPortDefault/kIOMainPortDefault
 * rename, the same choice as pascal/kext/tools/pg106_probe.c).
 *
 * Not compiled or run anywhere yet: Linux has no IOKit headers, so this
 * file is checked on the owner's Mac only (research/evidence/BLOCKERS.md
 * B6). Off macOS it builds to stubs that return -ENOSYS.
 */
#include "pg106_br_iokit.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)

#include <IOKit/IOKitLib.h>
#include <mach/mach.h>

#include "frame_io.h"

struct iok {
    io_connect_t conn;
    uint8_t *req, *resp;
};

static int kr_errno(kern_return_t kr)
{
    switch (kr) {
    case KERN_SUCCESS:            return 0;
    case kIOReturnNotPrivileged:  return -EACCES;
    case kIOReturnNoDevice:
    case kIOReturnNotReady:       return -ENODEV;
    case kIOReturnNoMemory:       return -ENOMEM;
    case kIOReturnBadArgument:    return -EINVAL;
    case kIOReturnBusy:           return -EBUSY;
    default:                      return -EIO;
    }
}

static int iok_call(struct pg106_br_transport *tr,
                    const struct pg106_br_frame *req, const void *req_payload,
                    struct pg106_br_frame *resp, void *resp_payload,
                    size_t resp_cap, size_t *resp_len_out)
{
    struct iok *k = tr->user;
    size_t in_len = 0, out_len = PG106_BR_RESP_BUF_MAX;
    kern_return_t kr;
    int rc;

    rc = br_frame_pack(k->req, req, req_payload, &in_len);
    if (rc != 0) {
        return rc;
    }
    /* Inputs above 4 KiB reach the kext as a memory descriptor; the
     * user client handles both forms (PG106UserClient::sSubmit). */
    kr = IOConnectCallStructMethod(k->conn, PG106_BR_IOKIT_SEL_SUBMIT,
                                   k->req, in_len, k->resp, &out_len);
    if (kr != KERN_SUCCESS) {
        return kr_errno(kr);
    }
    return br_frame_unpack(k->resp, out_len, resp, resp_payload, resp_cap,
                           resp_len_out);
}

static int iok_map(struct pg106_br_transport *tr, uint64_t cookie,
                   uint64_t size_B, void **ptr_out)
{
    struct iok *k = tr->user;
    mach_vm_address_t addr = 0;
    mach_vm_size_t size = 0;
    kern_return_t kr;

    if (cookie == 0 || cookie > UINT32_MAX) {
        return -EINVAL; /* memory types are 32-bit */
    }
    kr = IOConnectMapMemory64(k->conn, (uint32_t)cookie, mach_task_self(),
                              &addr, &size, kIOMapAnywhere);
    if (kr != KERN_SUCCESS) {
        return kr_errno(kr);
    }
    if (size < size_B) {
        IOConnectUnmapMemory64(k->conn, (uint32_t)cookie, mach_task_self(),
                               addr);
        return -EPROTO;
    }
    *ptr_out = (void *)(uintptr_t)addr;
    return 0;
}

static void iok_unmap(struct pg106_br_transport *tr, uint64_t cookie,
                      void *ptr)
{
    struct iok *k = tr->user;

    (void)IOConnectUnmapMemory64(k->conn, (uint32_t)cookie, mach_task_self(),
                                 (mach_vm_address_t)(uintptr_t)ptr);
}

int pg106_br_iokit_transport_open(struct pg106_br_transport *tr_out)
{
    io_service_t svc;
    struct iok *k;
    kern_return_t kr;

    if (tr_out == NULL) {
        return -EINVAL;
    }
    k = calloc(1, sizeof(*k));
    if (k == NULL) {
        return -ENOMEM;
    }
    k->req = malloc(PG106_BR_REQ_BUF_MAX);
    k->resp = malloc(PG106_BR_RESP_BUF_MAX);
    if (k->req == NULL || k->resp == NULL) {
        free(k->req);
        free(k->resp);
        free(k);
        return -ENOMEM;
    }
    svc = IOServiceGetMatchingService(MACH_PORT_NULL,
                                      IOServiceMatching(PG106_BR_IOKIT_CLASS));
    if (svc == IO_OBJECT_NULL) {
        free(k->req);
        free(k->resp);
        free(k);
        return -ENODEV;
    }
    kr = IOServiceOpen(svc, mach_task_self(), PG106_BR_IOKIT_TYPE, &k->conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS) {
        free(k->req);
        free(k->resp);
        free(k);
        /* A refused initWithTask (not root) comes back as a generic
         * error; kIOReturnNotPrivileged when IOKit says so. */
        return kr == kIOReturnNotPrivileged ? -EACCES : -EIO;
    }
    memset(tr_out, 0, sizeof(*tr_out));
    tr_out->call = iok_call;
    tr_out->map_cookie = iok_map;
    tr_out->unmap_cookie = iok_unmap;
    tr_out->user = k;
    return 0;
}

void pg106_br_iokit_transport_close(struct pg106_br_transport *tr)
{
    struct iok *k;

    if (tr == NULL || tr->user == NULL) {
        return;
    }
    k = tr->user;
    IOServiceClose(k->conn);
    free(k->req);
    free(k->resp);
    free(k);
    memset(tr, 0, sizeof(*tr));
}

#else /* !__APPLE__ */

int pg106_br_iokit_transport_open(struct pg106_br_transport *tr_out)
{
    (void)tr_out;
    return -ENOSYS;
}

void pg106_br_iokit_transport_close(struct pg106_br_transport *tr)
{
    (void)tr;
}

#endif
