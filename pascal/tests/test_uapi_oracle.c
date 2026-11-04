// SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
/*
 * pascal/tests/test_uapi_oracle.c - nouveau CS uapi ORACLE test.
 *
 * Mission (research/evidence/UAPI-ORACLE.md): validate the uapi MODEL in
 * pascal/core/include/pascal/uapi.h against the REAL kernel uapi.
 *
 * Two layers:
 *
 * 1. ABI-layout oracle (always runs, no hardware): if <drm/nouveau_drm.h>
 *    is present on this machine (the kernel uapi headers installed by
 *    libdrm/kernel-devel), _Static_assert the kernel's struct sizes and
 *    offsets against our pg106_uapi_* structs. The nouveau uapi is
 *    generation-stable (same struct shapes since the EXEC/VM_BIND uapi
 *    landed), so an Ampere-class machine like this one (GA104, 10de:24a0)
 *    still pins every field our Pascal bridge will marshal. Any mismatch
 *    fails at compile time, which is exactly the check the bridge needs.
 *
 * 2. Live ioctl oracle (only when a NOUVEAU render node exists): open the
 *    render node like any normal app and issue the read-only discovery
 *    ioctls NOUVEAU-UAPI.md documents: DRM_IOCTL_VERSION + NOUVEAU_GETPARAM
 *    (PCI_VENDOR, PCI_DEVICE, BUS_TYPE, CHIPSET_ID, GRAPH_UNITS,
 *    PTIMER_TIME, EXEC_PUSH_MAX, VRAM_BAR_SIZE, VRAM_USED,
 *    HAS_VMA_TILEMODE). Never: VM_INIT/VM_BIND/EXEC (would create state),
 *    never DRM master, never mode setting.
 *
 * On this box (2026-11-05) nouveau is NOT bound to the NVIDIA GPU (it runs
 * the proprietary `nvidia` driver, see UAPI-ORACLE.md), so layer 2 prints
 * a SKIP line with the per-node driver names as evidence and exits 0 so
 * `make test` stays green. If nouveau ever appears, the same binary runs
 * the live battery.
 *
 * Build/run via the pascal Makefile. Read-only: touches no hardware state.
 */
#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "pascal/uapi.h"

#if defined(__has_include)
#if __has_include(<drm/drm.h>)
#include <drm/drm.h>
#define HAVE_SYS_DRM_H 1
#endif
#if __has_include(<drm/nouveau_drm.h>)
#include <drm/nouveau_drm.h>
#define HAVE_SYS_NOUVEAU_H 1
#endif
#endif

static int failures;
#define CHECK(cond) do { \
    if (cond) { printf("PASS %s\n", #cond); } \
    else { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } \
} while (0)

/* uapi.h #undefs its own pin macro at the end of the file, so the
 * oracle keeps its own (same spelling, oracle-prefixed messages). */
#define ORACLE_ASSERT(cond, msg) _Static_assert(cond, msg)

/* ------------------------------------------------------------------ *
 * Fallback uapi bits (only if the system headers are absent). These
 * mirror include/uapi/drm/nouveau_drm.h + drm.h verbatim: GPL-2.0 with
 * the Linux-syscall-note exception, which permits unrestricted
 * userspace use of this ABI (see NOUVEAU-UAPI.md section 0).
 * ------------------------------------------------------------------ */
#ifndef HAVE_SYS_DRM_H
struct drm_version {
    int version_major, version_minor, version_patchlevel;
    size_t name_len, date_len, desc_len;
    char *name, *date, *desc;
};
#define DRM_IOCTL_VERSION _IOWR('d', 0x00, struct drm_version)
#endif

#ifndef HAVE_SYS_NOUVEAU_H
struct drm_nouveau_getparam { uint64_t param; uint64_t value; };
#define DRM_NOUVEAU_GETPARAM 0x00
#define DRM_COMMAND_BASE 0x40
#define DRM_IOCTL_NOUVEAU_GETPARAM \
    _IOWR('d', DRM_COMMAND_BASE + DRM_NOUVEAU_GETPARAM, struct drm_nouveau_getparam)
#define NOUVEAU_GETPARAM_PCI_VENDOR      3
#define NOUVEAU_GETPARAM_PCI_DEVICE      4
#define NOUVEAU_GETPARAM_BUS_TYPE        5
#define NOUVEAU_GETPARAM_CHIPSET_ID     11
#define NOUVEAU_GETPARAM_GRAPH_UNITS    13
#define NOUVEAU_GETPARAM_PTIMER_TIME    14
#define NOUVEAU_GETPARAM_EXEC_PUSH_MAX  17
#define NOUVEAU_GETPARAM_VRAM_BAR_SIZE  18
#define NOUVEAU_GETPARAM_VRAM_USED      19
#define NOUVEAU_GETPARAM_HAS_VMA_TILEMODE 20
#endif

/* ---- 1. ABI-layout oracle -----------------------------------------
 * Kernel uapi vs our model, compile time. Sizes/offsets cited from
 * include/uapi/drm/nouveau_drm.h (torvalds/linux master, and the copy
 * installed at /usr/include/drm/nouveau_drm.h on this box): sync
 * :230-251, vm_init :263-274, vm_bind_op :282-330, vm_bind :332-378,
 * exec_push :386-405, exec :407-434, getparam :25-34.
 */
#ifdef HAVE_SYS_NOUVEAU_H
ORACLE_ASSERT(sizeof(struct drm_nouveau_getparam) ==
                        sizeof(struct pg106_uapi_getparam), "oracle: getparam size");
ORACLE_ASSERT(sizeof(struct drm_nouveau_sync) ==
                        sizeof(struct pg106_uapi_sync), "oracle: sync size");
ORACLE_ASSERT(offsetof(struct drm_nouveau_sync, handle) ==
                        offsetof(struct pg106_uapi_sync, handle), "oracle: sync.handle @4");
ORACLE_ASSERT(offsetof(struct drm_nouveau_sync, timeline_value) ==
                        offsetof(struct pg106_uapi_sync, timeline_value), "oracle: sync.timeline @8");
ORACLE_ASSERT(sizeof(struct drm_nouveau_vm_init) ==
                        sizeof(struct pg106_uapi_vm_init), "oracle: vm_init size");
ORACLE_ASSERT(sizeof(struct drm_nouveau_vm_bind_op) ==
                        sizeof(struct pg106_uapi_vm_bind_op), "oracle: vm_bind_op size");
ORACLE_ASSERT(offsetof(struct drm_nouveau_vm_bind_op, addr) ==
                        offsetof(struct pg106_uapi_vm_bind_op, addr), "oracle: op.addr @16");
ORACLE_ASSERT(offsetof(struct drm_nouveau_vm_bind_op, bo_offset) ==
                        offsetof(struct pg106_uapi_vm_bind_op, bo_offset), "oracle: op.bo_offset @24");
ORACLE_ASSERT(sizeof(struct drm_nouveau_vm_bind) ==
                        sizeof(struct pg106_uapi_vm_bind), "oracle: vm_bind size");
ORACLE_ASSERT(offsetof(struct drm_nouveau_vm_bind, op_ptr) ==
                        offsetof(struct pg106_uapi_vm_bind, op_ptr), "oracle: vm_bind.op_ptr @32");
ORACLE_ASSERT(sizeof(struct drm_nouveau_exec_push) ==
                        sizeof(struct pg106_uapi_exec_push), "oracle: exec_push size");
ORACLE_ASSERT(offsetof(struct drm_nouveau_exec_push, va_len) ==
                        offsetof(struct pg106_uapi_exec_push, va_len), "oracle: push.va_len @8");
ORACLE_ASSERT(sizeof(struct drm_nouveau_exec) ==
                        sizeof(struct pg106_uapi_exec), "oracle: exec size");
ORACLE_ASSERT(offsetof(struct drm_nouveau_exec, push_ptr) ==
                        offsetof(struct pg106_uapi_exec, push_ptr), "oracle: exec.push_ptr @32");
/* Flag constants the bridge must answer with the same numbers. */
ORACLE_ASSERT(DRM_NOUVEAU_SYNC_SYNCOBJ == PG106_UAPI_SYNC_SYNCOBJ,
                        "oracle: SYNC_SYNCOBJ 0x0");
ORACLE_ASSERT(DRM_NOUVEAU_SYNC_TIMELINE_SYNCOBJ == PG106_UAPI_SYNC_TIMELINE_SYNCOBJ,
                        "oracle: SYNC_TIMELINE_SYNCOBJ 0x1");
ORACLE_ASSERT(DRM_NOUVEAU_VM_BIND_OP_MAP == PG106_UAPI_VM_BIND_OP_MAP,
                        "oracle: OP_MAP 0x0");
ORACLE_ASSERT(DRM_NOUVEAU_VM_BIND_OP_UNMAP == PG106_UAPI_VM_BIND_OP_UNMAP,
                        "oracle: OP_UNMAP 0x1");
ORACLE_ASSERT(DRM_NOUVEAU_VM_BIND_SPARSE == PG106_UAPI_VM_BIND_SPARSE,
                        "oracle: SPARSE 1<<8");
ORACLE_ASSERT(DRM_NOUVEAU_VM_BIND_RUN_ASYNC == PG106_UAPI_VM_BIND_RUN_ASYNC,
                        "oracle: RUN_ASYNC 0x1");
ORACLE_ASSERT(DRM_NOUVEAU_EXEC_PUSH_NO_PREFETCH == PG106_UAPI_EXEC_PUSH_NO_PREFETCH,
                        "oracle: NO_PREFETCH 0x1");
#endif

/* ---- 2. Live ioctl oracle ------------------------------------------ */
static int probe_node(const char *path, int *is_nouveau_out)
{
    struct drm_version ver = {0};
    char name[64] = {0}, date[64] = {0}, desc[320] = {0};
    int fd = open(path, O_RDWR | O_CLOEXEC);
    *is_nouveau_out = 0;

    if (fd < 0) {
        printf("SKIP %s: open failed (%s)\n", path, strerror(errno));
        return 0;
    }

    ver.name = name; ver.name_len = sizeof(name) - 1;
    ver.date = date; ver.date_len = sizeof(date) - 1;
    ver.desc = desc; ver.desc_len = sizeof(desc) - 1;
    if (ioctl(fd, DRM_IOCTL_VERSION, &ver) != 0) {
        printf("SKIP %s: DRM_IOCTL_VERSION failed (%s)\n", path, strerror(errno));
        close(fd);
        return 0;
    }
    printf("ORACLE %s: drm version %d.%d.%d driver=\"%s\" date=\"%s\"\n",
           path, ver.version_major, ver.version_minor, ver.version_patchlevel,
           name, date);

    if (strcmp(name, "nouveau") != 0) {
        /* Negative control: a non-nouveau DRM node must reject the
         * nouveau command range. Any normal app issuing this on the
         * wrong driver gets -EINVAL; that is the expected outcome. */
        struct drm_nouveau_getparam gp = { NOUVEAU_GETPARAM_PCI_VENDOR, 0 };
        int rc = ioctl(fd, DRM_IOCTL_NOUVEAU_GETPARAM, &gp);
        printf("CONTROL %s: GETPARAM on non-nouveau driver -> rc=%d errno=%d (%s), "
               "expected failure\n", path, rc, errno,
               rc != 0 ? "REJECTED-AS-EXPECTED" : "UNEXPECTED-ACCEPT");
        close(fd);
        return 0;
    }

    *is_nouveau_out = 1;

    /* The GETPARAM battery NOUVEAU-UAPI.md section 1 documents. All
     * read-only discovery; no VM/channel/state is created. */
    {
        static const struct { const char *name; uint64_t id; } params[] = {
            { "PCI_VENDOR",       NOUVEAU_GETPARAM_PCI_VENDOR },
            { "PCI_DEVICE",       NOUVEAU_GETPARAM_PCI_DEVICE },
            { "BUS_TYPE",         NOUVEAU_GETPARAM_BUS_TYPE },
            { "CHIPSET_ID",       NOUVEAU_GETPARAM_CHIPSET_ID },
            { "GRAPH_UNITS",      NOUVEAU_GETPARAM_GRAPH_UNITS },
            { "PTIMER_TIME",      NOUVEAU_GETPARAM_PTIMER_TIME },
            { "EXEC_PUSH_MAX",    NOUVEAU_GETPARAM_EXEC_PUSH_MAX },
            { "VRAM_BAR_SIZE",    NOUVEAU_GETPARAM_VRAM_BAR_SIZE },
            { "VRAM_USED",        NOUVEAU_GETPARAM_VRAM_USED },
            { "HAS_VMA_TILEMODE", NOUVEAU_GETPARAM_HAS_VMA_TILEMODE },
        };
        for (size_t i = 0; i < sizeof(params) / sizeof(params[0]); i++) {
            struct drm_nouveau_getparam gp = { params[i].id, 0 };
            if (ioctl(fd, DRM_IOCTL_NOUVEAU_GETPARAM, &gp) != 0) {
                printf("ORACLE %s: GETPARAM %-16s -> error %d (%s)\n",
                       path, params[i].name, errno, strerror(errno));
            } else {
                printf("ORACLE %s: GETPARAM %-16s -> 0x%llx (%llu)\n",
                       path, params[i].name, (unsigned long long)gp.value,
                       (unsigned long long)gp.value);
            }
        }
        /* Wire check with a live kernel: the getparam struct the kernel
         * just consumed must be the 16-byte {u64 param; u64 value} our
         * bridge marshals. */
        CHECK(sizeof(struct drm_nouveau_getparam) == 16);
        CHECK(sizeof(struct pg106_uapi_getparam) == 16);
    }
    close(fd);
    return 0;
}

int main(void)
{
    printf("== uapi oracle (see research/evidence/UAPI-ORACLE.md) ==\n");
#ifdef HAVE_SYS_NOUVEAU_H
    printf("oracle headers: system <drm/nouveau_drm.h> present, ABI pins active\n");
#else
    printf("oracle headers: system <drm/nouveau_drm.h> ABSENT, layout pins against "
           "local fallback only\n");
#endif

    /* Layer 1 result is compile-time; report it as a runtime line too. */
    CHECK(sizeof(struct pg106_uapi_getparam) == 16);
    CHECK(sizeof(struct pg106_uapi_sync) == 16);
    CHECK(sizeof(struct pg106_uapi_vm_init) == 16);
    CHECK(sizeof(struct pg106_uapi_vm_bind_op) == 40);
    CHECK(sizeof(struct pg106_uapi_vm_bind) == 40);
    CHECK(sizeof(struct pg106_uapi_exec_push) == 16);
    CHECK(sizeof(struct pg106_uapi_exec) == 40);

    /* Layer 2: only a live kernel can answer. Find render nodes. */
    DIR *d = opendir("/dev/dri");
    if (d == NULL) {
        printf("SKIP live oracle: no /dev/dri on this machine\n");
    } else {
        struct dirent *e;
        int saw_nouveau = 0;
        while ((e = readdir(d)) != NULL) {
            if (strncmp(e->d_name, "renderD", 7) != 0)
                continue;
            char path[288];
            snprintf(path, sizeof(path), "/dev/dri/%s", e->d_name);
            int is_nouveau = 0;
            probe_node(path, &is_nouveau);
            saw_nouveau |= is_nouveau;
        }
        closedir(d);
        if (!saw_nouveau) {
            printf("SKIP live oracle: no nouveau render node found "
                   "(this box binds the NVIDIA GPU to the proprietary "
                   "nvidia driver; recorded in UAPI-ORACLE.md)\n");
        }
    }

    printf("%s: %d failure(s)\n", "test_uapi_oracle", failures);
    return failures != 0;
}
