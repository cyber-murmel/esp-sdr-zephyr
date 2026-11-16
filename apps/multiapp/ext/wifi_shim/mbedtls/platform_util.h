/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * All the supplicant uses of mbedTLS (port/os_xtensa.c): the module builds its internal crypto
 * and no mbedTLS.
 */

#ifndef MULTIAPP_WIFI_SHIM_PLATFORM_UTIL_H_
#define MULTIAPP_WIFI_SHIM_PLATFORM_UTIL_H_

#include <stddef.h>
#include <string.h>

static inline void mbedtls_platform_zeroize(void *buf, size_t len)
{
	memset(buf, 0, len);
	/* The compiler may not drop the memset of a buffer about to be freed. */
	__asm__ volatile("" : : "r"(buf) : "memory");
}

#endif /* MULTIAPP_WIFI_SHIM_PLATFORM_UTIL_H_ */
