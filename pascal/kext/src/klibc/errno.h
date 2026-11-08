// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/src/klibc/errno.h — <errno.h> for the portable core when it is
// compiled into the XNU kext.
//
// pascal/core returns negative errno values ("0 or negative errno", e.g.
// mmu.h) and includes <errno.h>. A kext has no userspace <errno.h>; the
// constants live in the kernel's <sys/errno.h> (MacKernelSDK
// Headers/sys/errno.h), which defines every name the core uses (EINVAL,
// ENOMEM, ENOENT, EEXIST, ERANGE, EFAULT, ENODEV, ENOSPC, EOVERFLOW, EPROTO).
// An earlier version hand-defined four of them and missed EOVERFLOW/ENOSPC
// (core/src/acr.c); scripts/kext-syntax-check.sh caught it.
//
// Values are XNU's, not Linux's, for some names (EOVERFLOW is 84 on XNU, 75
// on Linux; EPROTO 100 vs 71). The core only compares errno values
// symbolically, so that is harmless inside the kext; anything that crosses
// to userspace as a Linux errno must be translated explicitly.
//
// The kext build puts src/klibc first on the include path; other builds
// fall through to the system header.

#ifndef PG106_KLIBC_ERRNO_H
#define PG106_KLIBC_ERRNO_H

#if defined(KERNEL)
#include <sys/errno.h>
#else
#include_next <errno.h>
#endif

#endif // PG106_KLIBC_ERRNO_H
