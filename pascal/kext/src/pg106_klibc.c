// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/src/pg106_klibc.c — calloc/realloc/free for pascal/core inside
// the kext, behind the asm-labelled declarations in src/klibc/stdlib.h.
//
// Backend: IOMalloc/IOFree (IOKit/IOLib.h). IOFree needs the size of the
// original allocation and C free() does not pass one, so every block carries
// a 16-byte header holding the backend size. kern_os_malloc/kern_os_free are
// not used: they are not declared in any public SDK header (MacKernelSDK
// Headers/ has no declaration). IOMallocZero is avoided because it is not
// established that kernels back to the 10.13 deployment target export it;
// zeroing is done here instead.
//
// Under PG106_KLIBC_KERNEL_SIM (the Linux stub harness) the same code runs on
// the host allocator, so the header/size/overflow logic is exercised by
// stub/test_core_shim.c.

#include "pg106_klibc.h"

#include <stdint.h> // SIZE_MAX
#include <string.h> // memcpy, memset (src/klibc/string.h → libkern in the kext)

#if defined(KERNEL)
#include <IOKit/IOLib.h>
#define PG106_BACKEND_ALLOC(n)   IOMalloc((vm_size_t)(n))
#define PG106_BACKEND_FREE(p, n) IOFree((p), (vm_size_t)(n))
#elif defined(PG106_KLIBC_KERNEL_SIM)
// The host's malloc/free under other C names (the standard names are bound
// to pg106_k* by src/klibc/stdlib.h in this mode).
void *pg106_host_malloc(size_t n) PG106_KLIBC_SYM(malloc);
void pg106_host_free(void *p) PG106_KLIBC_SYM(free);
#define PG106_BACKEND_ALLOC(n)   pg106_host_malloc(n)
#define PG106_BACKEND_FREE(p, n) ((void)(n), pg106_host_free(p))
#else
#error "pg106_klibc.c is only built for the kext (KERNEL) or the stub harness"
#endif

// 16 bytes so the pointer handed to the core keeps the backend's alignment
// modulo 16 (IOMalloc returns kalloc memory, 16-byte aligned in practice).
struct pg106_kmem_hdr {
	size_t total; // bytes passed to the backend, header included
	size_t user;  // bytes the caller asked for
};
_Static_assert(sizeof(struct pg106_kmem_hdr) == 16, "header keeps 16B alignment");

#define PG106_HDR_SIZE sizeof(struct pg106_kmem_hdr)

static void *
pg106_kmem_alloc(size_t bytes, int zero)
{
	struct pg106_kmem_hdr *h;

	if (bytes > SIZE_MAX - PG106_HDR_SIZE)
		return NULL;
	h = PG106_BACKEND_ALLOC(bytes + PG106_HDR_SIZE);
	if (h == NULL)
		return NULL;
	h->total = bytes + PG106_HDR_SIZE;
	h->user = bytes;
	if (zero)
		memset(h + 1, 0, bytes);
	return h + 1;
}

void *
pg106_kcalloc(size_t n, size_t size)
{
	if (size != 0 && n > SIZE_MAX / size)
		return NULL;
	return pg106_kmem_alloc(n * size, 1);
}

void
pg106_kfree(void *p)
{
	struct pg106_kmem_hdr *h;

	if (p == NULL)
		return;
	h = (struct pg106_kmem_hdr *)p - 1;
	PG106_BACKEND_FREE(h, h->total);
}

void *
pg106_krealloc(void *p, size_t bytes)
{
	const struct pg106_kmem_hdr *h;
	void *n;

	if (p == NULL)
		return pg106_kmem_alloc(bytes, 0);
	// realloc(p, 0): free and return NULL, the glibc behaviour the host
	// tests ran against. The core never asks for 0 bytes.
	if (bytes == 0) {
		pg106_kfree(p);
		return NULL;
	}
	h = (const struct pg106_kmem_hdr *)p - 1;
	n = pg106_kmem_alloc(bytes, 0);
	if (n == NULL)
		return NULL; // p stays valid, as with C realloc
	memcpy(n, p, h->user < bytes ? h->user : bytes);
	pg106_kfree(p);
	return n;
}
