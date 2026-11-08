// SPDX-License-Identifier: MIT
/*
 * transport_loopback.c - in-process byte-buffer transport (see
 * include/pg106_br_loopback.h).
 */
#include "pg106_br_loopback.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "frame_io.h"

static int lb_call(struct pg106_br_transport *tr,
                   const struct pg106_br_frame *req, const void *req_payload,
                   struct pg106_br_frame *resp, void *resp_payload,
                   size_t resp_cap, size_t *resp_len_out)
{
    struct pg106_br_loopback *lb = tr->user;
    size_t in_len = 0, out_len = 0;
    int rc;

    rc = br_frame_pack(lb->req, req, req_payload, &in_len);
    if (rc != 0) {
        return rc;
    }
    lb->calls++;
    rc = lb->server.serve(lb->server.srv, lb->req, in_len, lb->resp,
                          PG106_BR_RESP_BUF_MAX, &out_len);
    if (rc != 0) {
        return rc;
    }
    if (out_len > PG106_BR_RESP_BUF_MAX) {
        return -EPROTO;
    }
    return br_frame_unpack(lb->resp, out_len, resp, resp_payload, resp_cap,
                           resp_len_out);
}

static int lb_map(struct pg106_br_transport *tr, uint64_t cookie,
                  uint64_t size_B, void **ptr_out)
{
    struct pg106_br_loopback *lb = tr->user;

    if (lb->server.map == NULL) {
        return -EOPNOTSUPP;
    }
    return lb->server.map(lb->server.srv, cookie, size_B, ptr_out);
}

static void lb_unmap(struct pg106_br_transport *tr, uint64_t cookie, void *ptr)
{
    struct pg106_br_loopback *lb = tr->user;

    if (lb->server.unmap != NULL) {
        lb->server.unmap(lb->server.srv, cookie, ptr);
    }
}

int pg106_br_loopback_open(struct pg106_br_loopback *lb,
                           const struct pg106_br_loopback_server *server,
                           struct pg106_br_transport *tr_out)
{
    if (lb == NULL || server == NULL || server->serve == NULL || tr_out == NULL) {
        return -EINVAL;
    }
    memset(lb, 0, sizeof(*lb));
    lb->server = *server;
    lb->req = malloc(PG106_BR_REQ_BUF_MAX);
    lb->resp = malloc(PG106_BR_RESP_BUF_MAX);
    if (lb->req == NULL || lb->resp == NULL) {
        pg106_br_loopback_close(lb);
        return -ENOMEM;
    }
    memset(tr_out, 0, sizeof(*tr_out));
    tr_out->call = lb_call;
    tr_out->map_cookie = lb_map;
    tr_out->unmap_cookie = lb_unmap;
    tr_out->user = lb;
    return 0;
}

void pg106_br_loopback_close(struct pg106_br_loopback *lb)
{
    if (lb == NULL) {
        return;
    }
    free(lb->req);
    free(lb->resp);
    lb->req = NULL;
    lb->resp = NULL;
}
