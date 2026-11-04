// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/src/pg106_klibc_shim.h — libc-surface shim for building the
// portable pascal/core sources inside the XNU kext.
//
// Force-included by the build (-include) for every translation unit in the
// kext target, before any source text (PG106.xcodeproj OTHER_CFLAGS; the
// Linux stub Makefile does the same with -DPG106_KLIBC_KERNEL_SIM). It maps
// the libc calls the core makes onto the right allocator for the environment:
// calloc/free (core/src/mmu.c), realloc (core/src/acr_boot.c):
//
//   - KERNEL (real kext build): kern_os_malloc/kern_os_realloc/kern_os_free
//     — the IOKit kernel allocator family, declared in IOKit/IOLib.h.
//     memcpy/memset come from libkern/libkern.h via src/klibc/string.h.
//   - PG106_KLIBC_KERNEL_SIM (Linux stub harness): the host allocator. This
//     mode exists to prove the shim surface is complete — that the core needs
//     nothing beyond what this shim plus the src/klibc headers provide.
//
// Uncertainty to confirm on the owner's Mac (README.md): if MacKernelSDK
// turns out to ship its own <stdlib.h>/<errno.h>, the src/klibc shadow headers
// still take precedence in this project's include order and the shim below
// remains authoritative; nothing else should need to change.

#ifndef PG106_KLIBC_SHIM_H
#define PG106_KLIBC_SHIM_H

#if defined(KERNEL) || defined(PG106_KLIBC_KERNEL_SIM)

#include <stddef.h>

#if defined(KERNEL)
#include <IOKit/IOLib.h> // kern_os_malloc / kern_os_free
#endif

static inline void *pg106_kcalloc(size_t n, size_t size)
{
	size_t bytes;
	void *p;

	if (size != 0 && n > (size_t)-1 / size)
		return NULL;
	bytes = n * size;
#if defined(KERNEL)
	p = kern_os_malloc(bytes);
#else
	// Host-sim mode: no stdlib.h is visible by design (src/klibc/stdlib.h
	// is deliberately empty), so use the builtin, which needs no header.
	p = __builtin_malloc(bytes);
#endif
	if (p)
		__builtin_memset(p, 0, bytes);
	return p;
}

static inline void *pg106_krealloc(void *p, size_t bytes)
{
#if defined(KERNEL)
	// kern_os_realloc — the growing half of the IOKit allocator pair
	// (IOKit/IOLib.h); it preserves contents like C realloc, which the
	// core relies on (core/src/acr_boot.c emit()).
	return kern_os_realloc(p, bytes);
#else
	return __builtin_realloc(p, bytes);
#endif
}

static inline void pg106_kfree(void *p)
{
#if defined(KERNEL)
	kern_os_free(p);
#else
	__builtin_free(p);
#endif
}

// Rename the call sites, not the declarations: these macros are defined
// before any core source text is seen, so `calloc(1, size)` in core/src/mmu.c
// compiles as pg106_kcalloc(1, size). Header declarations that spell the
// original names never appear because src/klibc/stdlib.h declares nothing.
#define calloc  pg106_kcalloc
#define realloc pg106_krealloc
#define free    pg106_kfree

#endif // KERNEL || PG106_KLIBC_KERNEL_SIM

#endif // PG106_KLIBC_SHIM_H
