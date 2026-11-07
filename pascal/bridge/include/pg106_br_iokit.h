// SPDX-License-Identifier: MIT
/*
 * pg106_br_iokit.h - the real transport on macOS: PG106UserClient over
 * IOKit (src/transport_iokit.c, compiled only on macOS).
 *
 *   open:  IOServiceGetMatchingService(IOServiceMatching("PG106")),
 *          IOServiceOpen(type PG106_BR_IOKIT_TYPE)
 *   call:  IOConnectCallStructMethod(PG106_BR_IOKIT_SEL_SUBMIT,
 *          header || payload -> header || payload)
 *   map:   IOConnectMapMemory64(memoryType = BO map cookie)
 *   unmap: IOConnectUnmapMemory64
 *
 * The user client is root-only (PG106UserClient::initWithTask), and every
 * op except HANDSHAKE/GETPARAM answers -ENODEV until the kext's bring-up
 * has completed (pascal/kext/README.md).
 *
 * Calls on one transport must be serialised by the caller (the kext
 * serialises them too, on its command gate).
 */
#ifndef PG106_BR_IOKIT_H
#define PG106_BR_IOKIT_H

#include "pg106_br_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 0, -ENODEV no PG106 service registered, -EACCES not root,
 * -EIO any other IOServiceOpen failure, -ENOMEM, -ENOSYS not macOS. */
int pg106_br_iokit_transport_open(struct pg106_br_transport *tr_out);
/* Closes the connection (the kext drops every object of this client). */
void pg106_br_iokit_transport_close(struct pg106_br_transport *tr);

#ifdef __cplusplus
}
#endif

#endif /* PG106_BR_IOKIT_H */
