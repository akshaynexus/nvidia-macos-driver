// SPDX-License-Identifier: GPL-2.0-only
// pascal/kext/src/pg106_klibc.h — prototypes for src/pg106_klibc.c, the
// implementation behind the calloc/realloc/free declarations in
// src/klibc/stdlib.h (which binds those C names to these symbols).

#ifndef PG106_KLIBC_H
#define PG106_KLIBC_H

#include <stdlib.h> // src/klibc/stdlib.h: PG106_KLIBC_SYM, the bindings

#ifdef __cplusplus
extern "C" {
#endif

void *pg106_kcalloc(size_t n, size_t size);
void *pg106_krealloc(void *p, size_t bytes);
void pg106_kfree(void *p);

#ifdef __cplusplus
}
#endif

#endif // PG106_KLIBC_H
