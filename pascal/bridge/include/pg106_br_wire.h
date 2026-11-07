// SPDX-License-Identifier: MIT
/*
 * pg106_br_wire.h - the byte format spoken between the pg106 bridge
 * (userspace, linked into NVK) and the PG106 kext UserClient.
 *
 * Design rule (research/evidence/NOUVEAU-UAPI.md section 2): the uapi
 * ops are transported as the *verbatim* pg106_uapi_* structs from
 * pascal/core (byte-identical to include/uapi/drm/nouveau_drm.h). The
 * *_ptr fields of pg106_uapi_vm_bind / pg106_uapi_exec are user
 * pointers on Linux; on this wire they are sent as 0 and the arrays
 * they point at are flattened inline after the header struct, in the
 * order the Linux kernel copies them in with u_memcpya():
 * nouveau_uvmm_vm_bind_ucopy (nouveau_uvmm.c:1749-1800: ops, in_sync,
 * out_sync) and nouveau_exec_ucopy (nouveau_exec.c:291-339: pushes,
 * in_sync, out_sync):
 *
 *   VM_BIND frame:  pg106_br_frame
 *                   struct pg106_uapi_vm_bind        (ptrs == 0)
 *                   struct pg106_uapi_vm_bind_op[..] (ops)
 *                   struct pg106_uapi_sync[..]       (waits)
 *                   struct pg106_uapi_sync[..]       (sigs)
 *
 *   EXEC frame:     pg106_br_frame
 *                   struct pg106_uapi_exec           (ptrs == 0)
 *                   struct pg106_uapi_exec_push[..]  (pushes)
 *                   struct pg106_uapi_sync[..]       (waits)
 *                   struct pg106_uapi_sync[..]       (sigs)
 *
 * The receiver splits each payload by the counts in the header struct;
 * payload_len must equal the sum exactly. Positions carry no
 * semantics: waits complete before the job runs and signals fire after
 * its done fence regardless of layout (nouveau_sched.c
 * nouveau_job_submit).
 *
 * Everything else (handshake, BO lifecycle) has no Linux-uapi
 * counterpart; those payloads are bridge-owned structs, fixed-layout,
 * little-endian, 8-byte aligned.
 */
#ifndef PG106_BR_WIRE_H
#define PG106_BR_WIRE_H

#include <stddef.h>
#include <stdint.h>

/* pascal/uapi.h (MIT): the pg106_uapi_* structs and constants. The
 * bridge also links core/src/uapi.c (MIT) for the validators; see
 * README.md, "Licensing". */
#include <pascal/uapi.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PG106_BR_WIRE_VERSION 0u

/* Bytes 50 47 31 30 = "PG10" when read sequentially off the wire. */
#define PG106_BR_WIRE_MAGIC 0x30314750u

/* Mesa refuses to run below this nouveau drm version
 * (nouveau_device.c:504-505, see NOUVEAU-UAPI.md section 1); the
 * handshake reports the kext's faked version and the bridge gates on
 * it exactly like the winsys does. */
#define PG106_BR_DRM_VERSION_MIN 0x01000301u

/* Every frame is one request or one response: this 24-byte header plus
 * payload_len bytes. A transport only has to move these two byte blobs
 * per op. */
struct pg106_br_frame {
    uint32_t magic;       /* PG106_BR_WIRE_MAGIC                       */
    uint8_t  op;          /* enum pg106_br_wire_op                     */
    uint8_t  wire_version;/* PG106_BR_WIRE_VERSION                     */
    uint16_t flags;       /* must be 0                                 */
    uint32_t payload_len; /* bytes following the header                */
    uint32_t seq;         /* request sequence, echoed by the response  */
    uint32_t reserved;    /* must be 0                                 */
    int32_t  status;      /* response: 0 or negative errno; req: 0     */
};

enum pg106_br_wire_op {
    PG106_BR_OP_HANDSHAKE = 0,
    PG106_BR_OP_GETPARAM  = 1,
    PG106_BR_OP_VM_INIT   = 2,
    PG106_BR_OP_BO_ALLOC  = 3,
    PG106_BR_OP_BO_FREE   = 4,
    PG106_BR_OP_BO_MAP    = 5,
    PG106_BR_OP_BO_UNMAP  = 6,
    PG106_BR_OP_VM_BIND   = 7,
    PG106_BR_OP_EXEC      = 8,
};

/* ------------------------------------------------------------------ */
/* Payloads                                                            */
/* ------------------------------------------------------------------ */

/* OP_HANDSHAKE, both directions. Request fills client_*; the response
 * fills kext_*, ib_max (the channel GPFIFO ring capacity - the same
 * number GETPARAM EXEC_PUSH_MAX must return, nouveau_exec.h:45-52),
 * drm_version and va_space_end (2^47, nouveau_uvmm.c:35-37). */
struct pg106_br_handshake {
    uint32_t client_major;
    uint32_t client_minor;
    uint32_t kext_major;
    uint32_t kext_minor;
    uint32_t ib_max;
    uint32_t drm_version;
    uint64_t va_space_end;
};

/* OP_GETPARAM: pg106_uapi_getparam verbatim (value filled in the
 * response copy). */

/* OP_VM_INIT: pg106_uapi_vm_init verbatim. */

/* BO placement flags - values mirror enum nvkmd_mem_flags
 * (mesa 759935be src/nouveau/vulkan/nvkmd/nvkmd.h:35-58) so the future
 * pg106_br -> nvkmd shim can pass them through 1:1. */
#define PG106_BR_BO_LOCAL    (1u << 0)
#define PG106_BR_BO_GART     (1u << 1)
#define PG106_BR_BO_VRAM     (1u << 2)
#define PG106_BR_BO_CAN_MAP  (1u << 3)
#define PG106_BR_BO_SHARED   (1u << 4)
#define PG106_BR_BO_COHERENT (1u << 5)

/* OP_BO_ALLOC: request size_B/align_B/flags; response carries handle
 * (the GEM-handle analogue the VM_BIND ops reference). */
struct pg106_br_bo_alloc {
    uint64_t size_B;
    uint64_t align_B;
    uint32_t flags;   /* PG106_BR_BO_*                            */
    uint32_t handle;  /* response; request must send 0            */
};

struct pg106_br_bo_free {
    uint32_t handle;
    uint32_t pad;     /* must be 0                                */
};

/* OP_BO_MAP: request handle+flags; response carries map_cookie, which
 * the transport's map_cookie() hook turns into a CPU pointer (on the
 * kext this is IOConnectMapMemory64; NOUVEAU-UAPI.md section 4.1). */
struct pg106_br_bo_map {
    uint32_t handle;
    uint32_t flags;   /* PG106_BR_BO_* cache hints                */
    uint64_t map_cookie; /* response; request must send 0         */
};

struct pg106_br_bo_unmap {
    uint32_t handle;
    uint32_t pad;     /* must be 0                                */
};

/* OP_VM_BIND / OP_EXEC: pg106_uapi_vm_bind / pg106_uapi_exec headers
 * + flattened arrays as documented at the top of this file. */

/* Wire sizes, pinned so a payload_len miscalculation fails loudly. */
#define PG106_BR_WIRE_FRAME_LEN(len_payload) (sizeof(struct pg106_br_frame) + (len_payload))
#ifndef __cplusplus
_Static_assert(sizeof(struct pg106_br_frame) == 24, "pg106_br_frame is 24 bytes");
#endif

#ifdef __cplusplus
}
#endif

#endif /* PG106_BR_WIRE_H */
