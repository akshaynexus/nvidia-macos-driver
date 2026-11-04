// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/src/klibc/errno.h — errno surface for the portable core when it
// is compiled into the XNU kext.
//
// The pascal/core sources return errno-style values (see mmu.h: "Error
// returns are errno-style (0 or negative errno)") and include <errno.h>
// (core/src/acr.c:17, core/src/mmu.c:18) for EINVAL/ENOMEM, and use EEXIST and
// ERANGE. XNU kexts have no userspace <errno.h>; the constants live in
// bsd/sys/errno.h (sys/errno.h), with the same values as Linux for the four
// the core uses: EINVAL 22, ENOMEM 12, EEXIST 17, ERANGE 34.
//
// This header shadows <errno.h> ONLY inside the kext build: the xcodeproj and
// the Makefile put src/klibc first on the include path. On Linux (host/sim
// builds) it falls through with #include_next so glibc's real header is used.

#ifndef PG106_KLIBC_ERRNO_H
#define PG106_KLIBC_ERRNO_H

#if defined(KERNEL)
#define EINVAL 22
#define ENOMEM 12
#define EEXIST 17
#define ERANGE 34
#else
#include_next <errno.h>
#endif

#endif // PG106_KLIBC_ERRNO_H
