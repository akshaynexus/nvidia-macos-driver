// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/src/klibc/stdlib.h — deliberately near-empty.
//
// The portable core includes <stdlib.h> for exactly two things: calloc and
// free, both in core/src/mmu.c (alloc_zero at mmu.c:222-227 and the destroy
// paths). Neither exists in an XNU kext; the kext allocator pair is
// kern_os_malloc/kern_os_free (IOKit/IOLib.h). The mapping from the standard
// names onto the kext allocator is done by src/pg106_klibc_shim.h, which the
// kext build force-includes (-include) before any source — so by the time
// this header is reached, the calloc/free tokens in core sources have already
// been turned into the shim functions, and a real declaration here would only
// conflict with the shim's static inline definitions.
//
// On Linux the stub harness compiles core through this same header (empty is
// correct there too: nothing else from stdlib is used, and the shim provides
// the allocation). Do not add declarations here without checking that
// pascal/core really needs them — that is what the symbol audit enforces.

#ifndef PG106_KLIBC_STDLIB_H
#define PG106_KLIBC_STDLIB_H

#include <stddef.h>

#endif // PG106_KLIBC_STDLIB_H
