/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Poisoning for the tests of the kernels and the parser: under the address sanitizer (the .asan
 * scenario) a read of a poisoned byte aborts the test; otherwise these do nothing and the tests
 * that need them skip.
 */

#ifndef TXS_ASAN_H_
#define TXS_ASAN_H_

#include <stddef.h>

#ifdef __SANITIZE_ADDRESS__
#define TXS_ASAN 1
void __asan_poison_memory_region(void const volatile *addr, size_t size);
void __asan_unpoison_memory_region(void const volatile *addr, size_t size);
#define TXS_POISON(a, n)   __asan_poison_memory_region((a), (n))
#define TXS_UNPOISON(a, n) __asan_unpoison_memory_region((a), (n))
#else
#define TXS_ASAN 0
#define TXS_POISON(a, n)   ((void)0)
#define TXS_UNPOISON(a, n) ((void)0)
#endif

/* A buffer of `size` bytes in which [start, start + len) is readable and everything else poisoned. */
static inline void txs_guard(void *buf, size_t size, size_t start, size_t len)
{
	TXS_UNPOISON(buf, size);
	TXS_POISON(buf, start);
	TXS_POISON((char *)buf + start + len, size - start - len);
}

#endif /* TXS_ASAN_H_ */
