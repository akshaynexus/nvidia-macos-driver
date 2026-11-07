/* SPDX-License-Identifier: MIT */
/*
 * loopback_gpu - a pg106_br_transport for Linux tests: the bridge's
 * recording mock (pascal/bridge/src/mock.c) answers every frame, and on
 * each accepted EXEC this file plays GPU for the host methods the pg106
 * backend relies on: it follows the VA table the accepted VM_BIND frames
 * built, reads each push out of the mock's BO backing store, and executes
 * NVC06F SEMAPHOREA..D releases and ACQ_GEQ acquires (clc06f.h). Every
 * other method is counted and ignored.
 *
 * SIMULATION ONLY. It proves that the backend's pushes, VAs and wire
 * frames line up; it says nothing about the GP106's PBDMA.
 */
#ifndef LOOPBACK_GPU_H
#define LOOPBACK_GPU_H

#include <stddef.h>
#include <stdint.h>

#include "pg106_br_bridge.h"
#include "pg106_br_mock.h"

struct loopback_gpu;

struct loopback_stats {
   uint32_t exec_frames;
   uint32_t pushes;
   uint32_t releases;
   uint32_t acquires;
   uint32_t acquires_unsatisfied; /* would have blocked the channel */
   uint32_t wfis;
   uint32_t membars;
   uint32_t other_methods;
   uint32_t bad_push_va;          /* push outside every mapping */
   uint32_t bad_sem_va;           /* semaphore outside every mapping */
};

/* Knobs every new connection starts with (set them before the backend
 * opens its transports). */
struct loopback_config {
   uint32_t kext_major, kext_minor;
   uint32_t ib_max;
   uint16_t pci_device;      /* 0 = the mock's 0x1C02 */
   int drop_releases;        /* "hung GPU": never write semaphores */
   int32_t fail_next_exec;   /* errno answered to the next EXEC, 0 = none */
   int32_t fail_getparam;    /* errno answered to every GETPARAM, 0 = none */
};

extern struct loopback_config loopback_config;

/* The backend's transport hook pair (nvkmd_pg106_set_transport_hook). */
int loopback_transport_open(struct pg106_br_transport *tr);
void loopback_transport_close(struct pg106_br_transport *tr);

/* Connections opened so far (pdev probe = 0, first nvkmd_dev = 1...). */
size_t loopback_count(void);
struct loopback_gpu *loopback_get(size_t index);
struct pg106_br_mock *loopback_mock(struct loopback_gpu *gpu);
const struct loopback_stats *loopback_stats(const struct loopback_gpu *gpu);
void loopback_set_drop_releases(struct loopback_gpu *gpu, int drop);
void loopback_reset_all(void);

/* CPU pointer of a GPU VA under the loopback's VA table, NULL if
 * unmapped. */
void *loopback_va_ptr(struct loopback_gpu *gpu, uint64_t va, size_t len);

#endif
