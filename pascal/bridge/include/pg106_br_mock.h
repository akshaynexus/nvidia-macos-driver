// SPDX-License-Identifier: MIT
/*
 * pg106_br_mock.h - recording mock transport: stands in for the PG106
 * kext UserClient in tests and the demo. Implements enough "server
 * side" (handles, GETPARAM answers, handshake fields, failure
 * injection) to exercise the bridge end-to-end without hardware.
 */
#ifndef PG106_BR_MOCK_H
#define PG106_BR_MOCK_H

#include <stdint.h>
#include <stddef.h>

#include "pg106_br_bridge.h"
#include "pg106_br_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PG106_BR_MOCK_MAX_FRAMES 64u
#define PG106_BR_MOCK_MAX_BOS    64u

/* Mock "kext" constants, GP106-shaped (HELIOS300-PASCAL-RESEARCH.md
 * section 3.1; PCI id 10de:1c02 = GP106). */
#define PG106_BR_MOCK_PCI_VENDOR 0x10deu
#define PG106_BR_MOCK_PCI_DEVICE 0x1C02u
#define PG106_BR_MOCK_IB_MAX     940u  /* EXEC_PUSH_MAX = 940/2-1 = 469 */
#define PG106_BR_MOCK_DRM_VERSION 0x01000301u

struct pg106_br_mock;

struct pg106_br_mock *pg106_br_mock_create(void);
void pg106_br_mock_destroy(struct pg106_br_mock *mock);

/* Fills *tr_out with a transport bound to this mock (the mock owns the
 * transport storage; lifetime = mock lifetime). */
void pg106_br_mock_transport(struct pg106_br_mock *mock,
                             struct pg106_br_transport *tr_out);

/* ---- Test hooks ---------------------------------------------------- */

/* What the mock's "kext" side answers the handshake with. */
void pg106_br_mock_set_kext_version(struct pg106_br_mock *mock,
                                    uint32_t major, uint32_t minor);
void pg106_br_mock_set_drm_version(struct pg106_br_mock *mock,
                                   uint32_t drm_version);
void pg106_br_mock_set_ib_max(struct pg106_br_mock *mock, uint32_t ib_max);

/* Arm one-shot failure injection: the next frame with op `op` is
 * recorded but answered with `status`. status 0 disarms. */
void pg106_br_mock_fail_next(struct pg106_br_mock *mock, uint8_t op,
                             int32_t status);

/* ---- Recordings ---------------------------------------------------- */

struct pg106_br_mock_rec {
    uint8_t  op;
    uint32_t seq;
    int32_t  status;        /* what was answered */
    const uint8_t *bytes;   /* full request frame, header + payload */
    size_t   len;
};

size_t pg106_br_mock_frame_count(const struct pg106_br_mock *mock);
const struct pg106_br_mock_rec *pg106_br_mock_frame(
    const struct pg106_br_mock *mock, size_t index);

/* The mock's backing store for BO handle `handle` (what map_cookie
 * returns); NULL after free. */
void *pg106_br_mock_bo_backing(const struct pg106_br_mock *mock,
                               uint32_t handle);

#ifdef __cplusplus
}
#endif

#endif /* PG106_BR_MOCK_H */
