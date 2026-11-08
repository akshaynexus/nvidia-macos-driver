// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/src/klibc/stdlib.h — the allocation surface of <stdlib.h> for
// the portable core when it is compiled into the XNU kext.
//
// pascal/core uses calloc/realloc/free (core/src/mmu.c, acr_boot.c,
// gr_boot.c) and nothing else from <stdlib.h>. A kext has no libc. The kext
// build puts src/klibc first on the include path, so core's
// `#include <stdlib.h>` lands here.
//
// The three functions are declared under their standard C names but bound to
// different linker symbols with asm labels: core's call to free() becomes a
// call to the symbol pg106_kfree, implemented in src/pg106_klibc.c on
// IOMalloc/IOFree. No macro renames anything, so C++ translation units are
// unaffected (OSObject::free() is a member function and never collides with a
// global function declaration), and no global symbol named free/calloc is
// defined in the kext or interposed in the Linux stub binaries.
//
// Modes:
//   KERNEL                  the real kext build (Xcode defines KERNEL)
//   PG106_KLIBC_KERNEL_SIM  Linux stub harness: same declarations, with the
//                           pg106_klibc.c backend on the host allocator
//   neither                 ordinary host build: the system <stdlib.h>

#ifndef PG106_KLIBC_STDLIB_H
#define PG106_KLIBC_STDLIB_H

#if defined(KERNEL) || defined(PG106_KLIBC_KERNEL_SIM)

#include <stddef.h>

// Linker symbol for a C name: the platform's user label prefix ("_" on
// Mach-O, empty on x86-64 ELF) followed by the name.
#define PG106_KLIBC_STR_(x) #x
#define PG106_KLIBC_STR(x) PG106_KLIBC_STR_(x)
#define PG106_KLIBC_SYM(name) \
	__asm__(PG106_KLIBC_STR(__USER_LABEL_PREFIX__) #name)

#ifdef __cplusplus
extern "C" {
#endif

void *calloc(size_t n, size_t size) PG106_KLIBC_SYM(pg106_kcalloc);
void *realloc(void *p, size_t size) PG106_KLIBC_SYM(pg106_krealloc);
void free(void *p) PG106_KLIBC_SYM(pg106_kfree);

#ifdef __cplusplus
}
#endif

#else
#include_next <stdlib.h>
#endif

#endif // PG106_KLIBC_STDLIB_H
