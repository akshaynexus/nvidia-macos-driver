// SPDX-License-Identifier: MIT
/*
 * frame_io.h - private helpers shared by the byte-buffer transports
 * (transport_loopback.c, transport_iokit.c): both move one request as
 * header || payload and get one response back as header || payload,
 * exactly what PG106UserClient's SUBMIT selector takes and returns.
 */
#ifndef PG106_BR_FRAME_IO_H
#define PG106_BR_FRAME_IO_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "pg106_br_wire.h"

#define PG106_BR_REQ_BUF_MAX  (sizeof(struct pg106_br_frame) + PG106_BR_WIRE_MAX_PAYLOAD)
#define PG106_BR_RESP_BUF_MAX (sizeof(struct pg106_br_frame) + PG106_BR_WIRE_MAX_RESP)

/* header || payload into buf (cap PG106_BR_REQ_BUF_MAX). */
static inline int br_frame_pack(uint8_t *buf, const struct pg106_br_frame *req,
                                const void *payload, size_t *len_out)
{
    if (req->payload_len > PG106_BR_WIRE_MAX_PAYLOAD ||
        (req->payload_len != 0 && payload == NULL)) {
        return -E2BIG;
    }
    memcpy(buf, req, sizeof(*req));
    if (req->payload_len != 0) {
        memcpy(buf + sizeof(*req), payload, req->payload_len);
    }
    *len_out = sizeof(*req) + req->payload_len;
    return 0;
}

/* Split a response buffer. At most resp_cap payload bytes are copied;
 * *resp_len_out is what the kext actually sent, so the bridge's exact
 * length check sees an oversized reply too. */
static inline int br_frame_unpack(const uint8_t *buf, size_t len,
                                  struct pg106_br_frame *resp,
                                  void *resp_payload, size_t resp_cap,
                                  size_t *resp_len_out)
{
    size_t n;

    if (len < sizeof(*resp)) {
        return -EPROTO;
    }
    memcpy(resp, buf, sizeof(*resp));
    n = len - sizeof(*resp);
    if (resp->payload_len != n) {
        return -EPROTO;
    }
    if (resp_payload != NULL && resp_cap != 0) {
        memcpy(resp_payload, buf + sizeof(*resp), n < resp_cap ? n : resp_cap);
    }
    if (resp_len_out != NULL) {
        *resp_len_out = n;
    }
    return 0;
}

#endif /* PG106_BR_FRAME_IO_H */
