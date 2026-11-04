// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/src/klibc/string.h — string surface for the portable core when
// it is compiled into the XNU kext.
//
// The core uses memcpy/memset (core/src/acr.c, core/src/mmu.c) and includes
// <string.h>. In a kext those functions are declared by libkern/libkern.h
// (MacKernelSDK ships libkern with memcpy, memmove, memset, memcmp, bzero and
// friends); there is nothing else to declare here, so on KERNEL builds this
// header only pulls libkern in. On Linux it falls through with #include_next.

#ifndef PG106_KLIBC_STRING_H
#define PG106_KLIBC_STRING_H

#if defined(KERNEL)
#include <libkern/libkern.h>
#else
#include_next <string.h>
#endif

#endif // PG106_KLIBC_STRING_H
