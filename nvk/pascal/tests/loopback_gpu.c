/* SPDX-License-Identifier: MIT */
#include "loopback_gpu.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* clc06f.h method offsets (host class, any subchannel) */
#define M_SEMAPHOREA 0x10u
#define M_SEMAPHOREB 0x14u
#define M_SEMAPHOREC 0x18u
#define M_SEMAPHORED 0x1cu
#define M_MEM_OP_D   0x34u
#define M_WFI        0x78u
#define SEMD_OP_MASK      0x1fu
#define SEMD_OP_RELEASE   0x2u
#define SEMD_OP_ACQ_GEQ   0x4u
#define SEMD_SIZE_4BYTE   (1u << 24)
#define MEMOPD_OP_MEMBAR  0x5u

struct lb_map {
   uint64_t addr, range, bo_offset;
   uint32_t handle;
};

struct lb_import {
   uint32_t handle;
   uint8_t *ptr;
   uint64_t size;
};

struct loopback_gpu {
   struct pg106_br_mock *mock;
   struct lb_import imports[16];
   size_t n_imports;
   struct pg106_br_transport inner;
   struct lb_map *maps;
   size_t n_maps, cap_maps;
   struct loopback_stats stats;
   int drop_releases;
   int32_t fail_next_exec;
   int32_t fail_getparam;
   int32_t fail_vm_init;
   uint32_t sem_a, sem_b, sem_c; /* host semaphore state (one channel) */
};

struct loopback_config loopback_config;

#define MAX_GPUS 64
static struct loopback_gpu *gpus[MAX_GPUS];
static size_t n_gpus;

/* ---- VA table ------------------------------------------------------ */

static void
lb_unmap(struct loopback_gpu *g, uint64_t addr, uint64_t range)
{
   const uint64_t end = addr + range;
   for (size_t i = 0; i < g->n_maps;) {
      struct lb_map m = g->maps[i];
      const uint64_t mend = m.addr + m.range;
      if (mend <= addr || m.addr >= end) {
         i++;
         continue;
      }
      /* remove, then re-add the pieces outside [addr, end) */
      g->maps[i] = g->maps[--g->n_maps];
      if (m.addr < addr) {
         struct lb_map lo = m;
         lo.range = addr - m.addr;
         g->maps[g->n_maps++] = lo;
      }
      if (mend > end) {
         struct lb_map hi = m;
         hi.addr = end;
         hi.range = mend - end;
         hi.bo_offset = m.bo_offset + (end - m.addr);
         if (g->n_maps == g->cap_maps) {
            g->cap_maps = g->cap_maps ? g->cap_maps * 2 : 16;
            g->maps = realloc(g->maps, g->cap_maps * sizeof(*g->maps));
         }
         g->maps[g->n_maps++] = hi;
      }
   }
}

static void
lb_map(struct loopback_gpu *g, const struct pg106_uapi_vm_bind_op *op)
{
   lb_unmap(g, op->addr, op->range);
   if (g->n_maps + 2 > g->cap_maps) {
      g->cap_maps = g->cap_maps ? g->cap_maps * 2 : 16;
      g->maps = realloc(g->maps, g->cap_maps * sizeof(*g->maps));
   }
   g->maps[g->n_maps++] = (struct lb_map) {
      .addr = op->addr, .range = op->range,
      .bo_offset = op->bo_offset, .handle = op->handle,
   };
}

void *
loopback_va_ptr(struct loopback_gpu *g, uint64_t va, size_t len)
{
   for (size_t i = 0; i < g->n_maps; i++) {
      const struct lb_map *m = &g->maps[i];
      if (va >= m->addr && va + len <= m->addr + m->range) {
         uint8_t *base = pg106_br_mock_bo_backing(g->mock, m->handle);
         /* imported host pages have no mock backing: the client owns them */
         for (size_t k = 0; base == NULL && k < g->n_imports; k++)
            if (g->imports[k].handle == m->handle)
               base = g->imports[k].ptr;
         return base ? base + m->bo_offset + (va - m->addr) : NULL;
      }
   }
   return NULL;
}

/* ---- host method execution ----------------------------------------- */

static void
lb_semaphore(struct loopback_gpu *g, uint32_t d)
{
   const uint64_t addr = ((uint64_t)(g->sem_a & 0xff) << 32) | (g->sem_b & ~3u);
   uint32_t *word = loopback_va_ptr(g, addr, 4);
   if (word == NULL) {
      g->stats.bad_sem_va++;
      return;
   }
   switch (d & SEMD_OP_MASK) {
   case SEMD_OP_RELEASE:
      g->stats.releases++;
      if (!g->drop_releases) {
         *word = g->sem_c;
         if (!(d & SEMD_SIZE_4BYTE)) {
            /* 16-byte release: payload, 0, 64-bit timestamp */
            uint32_t *rest = loopback_va_ptr(g, addr + 4, 12);
            if (rest)
               memset(rest, 0, 12);
         }
      }
      break;
   case SEMD_OP_ACQ_GEQ:
      g->stats.acquires++;
      if ((int32_t)(*word - g->sem_c) < 0)
         g->stats.acquires_unsatisfied++;
      break;
   default:
      g->stats.other_methods++;
      break;
   }
}

static void
lb_method(struct loopback_gpu *g, uint32_t mthd, uint32_t data)
{
   switch (mthd) {
   case M_SEMAPHOREA: g->sem_a = data; break;
   case M_SEMAPHOREB: g->sem_b = data; break;
   case M_SEMAPHOREC: g->sem_c = data; break;
   case M_SEMAPHORED: lb_semaphore(g, data); break;
   case M_WFI:        g->stats.wfis++; break;
   case M_MEM_OP_D:
      if ((data >> 27) == MEMOPD_OP_MEMBAR)
         g->stats.membars++;
      break;
   default:
      if (mthd > M_MEM_OP_D)
         g->stats.other_methods++;
      break;
   }
}

/* clc06f.h NVC06F_DMA_*: SEC_OP [31:29], COUNT [28:16], SUBCH [15:13],
 * ADDRESS [11:0] (dwords). */
static void
lb_run_push(struct loopback_gpu *g, const uint32_t *w, size_t n)
{
   size_t i = 0;
   while (i < n) {
      const uint32_t hdr = w[i++];
      const uint32_t sec = hdr >> 29;
      const uint32_t count = (hdr >> 16) & 0x1fff;
      uint32_t mthd = (hdr & 0xfff) << 2;
      switch (sec) {
      case 1: /* INC_METHOD */
         for (uint32_t k = 0; k < count && i < n; k++, mthd += 4)
            lb_method(g, mthd, w[i++]);
         break;
      case 3: /* NON_INC_METHOD */
         for (uint32_t k = 0; k < count && i < n; k++)
            lb_method(g, mthd, w[i++]);
         break;
      case 4: /* IMMD_DATA_METHOD: data in COUNT */
         lb_method(g, mthd, count);
         break;
      case 5: /* ONE_INC */
         for (uint32_t k = 0; k < count && i < n; k++) {
            lb_method(g, mthd, w[i++]);
            if (k == 0)
               mthd += 4;
         }
         break;
      default: /* GRP0/GRP2 control words: nothing to model */
         g->stats.other_methods++;
         break;
      }
   }
}

static void
lb_exec(struct loopback_gpu *g, const uint8_t *payload, size_t len)
{
   struct pg106_uapi_exec hdr;
   if (len < sizeof(hdr))
      return;
   memcpy(&hdr, payload, sizeof(hdr));
   const struct pg106_uapi_exec_push *pushes =
      (const void *)(payload + sizeof(hdr));
   g->stats.exec_frames++;
   for (uint32_t p = 0; p < hdr.push_count; p++) {
      struct pg106_uapi_exec_push push;
      memcpy(&push, &pushes[p], sizeof(push));
      g->stats.pushes++;
      const uint32_t *w = loopback_va_ptr(g, push.va, push.va_len);
      if (w == NULL) {
         g->stats.bad_push_va++;
         continue;
      }
      lb_run_push(g, w, push.va_len / 4);
   }
}

static void
lb_vm_bind(struct loopback_gpu *g, const uint8_t *payload, size_t len)
{
   struct pg106_uapi_vm_bind hdr;
   if (len < sizeof(hdr))
      return;
   memcpy(&hdr, payload, sizeof(hdr));
   for (uint32_t i = 0; i < hdr.op_count; i++) {
      struct pg106_uapi_vm_bind_op op;
      memcpy(&op, payload + sizeof(hdr) + i * sizeof(op), sizeof(op));
      if (op.op == PG106_UAPI_VM_BIND_OP_MAP) {
         if (!(op.flags & PG106_UAPI_VM_BIND_SPARSE))
            lb_map(g, &op);
      } else {
         lb_unmap(g, op.addr, op.range);
      }
   }
}

/* ---- transport ------------------------------------------------------ */

static int
lb_call(struct pg106_br_transport *tr, const struct pg106_br_frame *req,
        const void *req_payload, struct pg106_br_frame *resp,
        void *resp_payload, size_t resp_cap, size_t *resp_len_out)
{
   struct loopback_gpu *g = tr->user;

   if (req->op == PG106_BR_OP_EXEC && g->fail_next_exec) {
      pg106_br_mock_fail_next(g->mock, PG106_BR_OP_EXEC, g->fail_next_exec);
      g->fail_next_exec = 0;
   }
   if (req->op == PG106_BR_OP_GETPARAM && g->fail_getparam)
      pg106_br_mock_fail_next(g->mock, PG106_BR_OP_GETPARAM, g->fail_getparam);
   if (req->op == PG106_BR_OP_VM_INIT && g->fail_vm_init)
      pg106_br_mock_fail_next(g->mock, PG106_BR_OP_VM_INIT, g->fail_vm_init);

   /* The bridge may answer into the request buffer (bridge.c br_call
    * passes one buffer for both), so keep what is needed from it. */
   struct pg106_br_bo_import_host req_im = { 0 };
   if (req->op == PG106_BR_OP_BO_IMPORT_HOST && req->payload_len == sizeof(req_im))
      memcpy(&req_im, req_payload, sizeof(req_im));

   int rc = g->inner.call(&g->inner, req, req_payload, resp, resp_payload,
                          resp_cap, resp_len_out);
   if (rc != 0 || resp->status != 0)
      return rc;

   if (req->op == PG106_BR_OP_GETPARAM && loopback_config.pci_device &&
       *resp_len_out == sizeof(struct pg106_uapi_getparam)) {
      struct pg106_uapi_getparam gp;
      memcpy(&gp, resp_payload, sizeof(gp));
      if (gp.param == PG106_UAPI_GETPARAM_PCI_DEVICE) {
         gp.value = loopback_config.pci_device;
         memcpy(resp_payload, &gp, sizeof(gp));
      }
   }
   if (req->op == PG106_BR_OP_GETPARAM && loopback_config.boot0) {
      struct pg106_uapi_getparam gp;
      memcpy(&gp, resp_payload, sizeof(gp));
      if (gp.param == PG106_BR_GETPARAM_BOOT0) {
         gp.value = loopback_config.boot0;
         memcpy(resp_payload, &gp, sizeof(gp));
      }
   }
   if (req->op == PG106_BR_OP_BO_IMPORT_HOST && g->n_imports < 16) {
      struct pg106_br_bo_import_host resp_im;
      memcpy(&resp_im, resp_payload, sizeof(resp_im));
      g->imports[g->n_imports++] = (struct lb_import) {
         .handle = resp_im.handle,
         .ptr = (uint8_t *)(uintptr_t)req_im.host_addr,
         .size = req_im.size_B,
      };
   }
   if (req->op == PG106_BR_OP_VM_BIND)
      lb_vm_bind(g, req_payload, req->payload_len);
   if (req->op == PG106_BR_OP_EXEC)
      lb_exec(g, req_payload, req->payload_len);
   return 0;
}

static int
lb_map_cookie(struct pg106_br_transport *tr, uint64_t cookie, uint64_t size_B,
              void **ptr_out)
{
   struct loopback_gpu *g = tr->user;
   return g->inner.map_cookie(&g->inner, cookie, size_B, ptr_out);
}

static void
lb_unmap_cookie(struct pg106_br_transport *tr, uint64_t cookie, void *ptr)
{
   struct loopback_gpu *g = tr->user;
   if (g->inner.unmap_cookie)
      g->inner.unmap_cookie(&g->inner, cookie, ptr);
}

int
loopback_transport_open(struct pg106_br_transport *tr)
{
   if (n_gpus == MAX_GPUS)
      return -ENOMEM;
   struct loopback_gpu *g = calloc(1, sizeof(*g));
   if (g == NULL)
      return -ENOMEM;
   g->mock = pg106_br_mock_create();
   if (g->mock == NULL) {
      free(g);
      return -ENOMEM;
   }
   if (loopback_config.kext_major || loopback_config.kext_minor)
      pg106_br_mock_set_kext_version(g->mock, loopback_config.kext_major,
                                     loopback_config.kext_minor);
   if (loopback_config.ib_max)
      pg106_br_mock_set_ib_max(g->mock, loopback_config.ib_max);
   g->drop_releases = loopback_config.drop_releases;
   g->fail_next_exec = loopback_config.fail_next_exec;
   g->fail_getparam = loopback_config.fail_getparam;
   g->fail_vm_init = loopback_config.fail_vm_init;
   pg106_br_mock_transport(g->mock, &g->inner);

   *tr = (struct pg106_br_transport) {
      .call = lb_call,
      .map_cookie = lb_map_cookie,
      .unmap_cookie = lb_unmap_cookie,
      .user = g,
   };
   gpus[n_gpus++] = g;
   return 0;
}

void
loopback_transport_close(struct pg106_br_transport *tr)
{
   /* Kept alive until loopback_reset_all so tests can inspect it. */
   (void)tr;
}

size_t loopback_count(void) { return n_gpus; }
struct loopback_gpu *loopback_get(size_t i) { return i < n_gpus ? gpus[i] : NULL; }
struct pg106_br_mock *loopback_mock(struct loopback_gpu *g) { return g->mock; }
const struct loopback_stats *loopback_stats(const struct loopback_gpu *g) { return &g->stats; }
void loopback_set_drop_releases(struct loopback_gpu *g, int drop) { g->drop_releases = drop; }

void
loopback_reset_all(void)
{
   for (size_t i = 0; i < n_gpus; i++) {
      pg106_br_mock_destroy(gpus[i]->mock);
      free(gpus[i]->maps);
      free(gpus[i]);
   }
   n_gpus = 0;
   memset(&loopback_config, 0, sizeof(loopback_config));
}
