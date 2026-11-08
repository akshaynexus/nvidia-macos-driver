// SPDX-License-Identifier: MIT
/*
 * pg106_br_loopback.h - in-process transport: the bridge's frames go to
 * a server function in the same process as the same byte buffers the
 * IOKit transport hands to IOConnectCallStructMethod (header || payload
 * each way). Linux tests plug the kernel-side dispatcher in as the
 * server (pascal/tests/test_kdisp.c); the bridge itself depends on
 * nothing but this header, so it stays MIT and links no kernel code.
 */
#ifndef PG106_BR_LOOPBACK_H
#define PG106_BR_LOOPBACK_H

#include <stddef.h>
#include <stdint.h>

#include "pg106_br_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

struct pg106_br_loopback_server {
    /* One frame in, one frame out. 0 = resp holds a response frame of
     * *resp_len bytes; negative = transport failure (what an
     * IOConnectCallStructMethod error would be). */
    int (*serve)(void *srv, const void *req, size_t req_len, void *resp,
                 size_t resp_cap, size_t *resp_len);
    /* BO map cookie -> CPU pointer (IOConnectMapMemory64 stand-in). */
    int (*map)(void *srv, uint64_t cookie, uint64_t size_B, void **ptr);
    void (*unmap)(void *srv, uint64_t cookie, void *ptr);
    void *srv;
};

struct pg106_br_loopback {
    struct pg106_br_loopback_server server;
    uint8_t *req, *resp;  /* frame buffers, allocated at open */
    size_t calls;
};

/* Bind *tr_out to lb (which must outlive the transport). */
int pg106_br_loopback_open(struct pg106_br_loopback *lb,
                           const struct pg106_br_loopback_server *server,
                           struct pg106_br_transport *tr_out);
void pg106_br_loopback_close(struct pg106_br_loopback *lb);

#ifdef __cplusplus
}
#endif

#endif /* PG106_BR_LOOPBACK_H */
