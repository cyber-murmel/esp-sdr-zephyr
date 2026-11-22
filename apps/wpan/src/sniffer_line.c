/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Adapted from Zephyr samples/net/ieee802154/sniffer (cf45cd71868) for apps/wpan.
 */

#include <stdio.h>

#include "sniffer_line.h"

size_t sniffer_line(char *out, size_t cap, const uint8_t *psdu, size_t len, uint8_t lqi,
		    int16_t rssi_dbm, uint64_t time_us)
{
	static const char hex[] = "0123456789abcdef";
	size_t n = 0;
	char tail[64];
	int tl;

	if (cap == 0) {
		return 0;
	}

	/* Every write stops at cap - 1: the line is cut, never overrun. */
	for (const char *p = "psdu: "; *p != '\0' && n < cap - 1; p++) {
		out[n++] = *p;
	}
	for (size_t i = 0; i < len; i++) {
		if (n < cap - 1) {
			out[n++] = hex[psdu[i] >> 4];
		}
		if (n < cap - 1) {
			out[n++] = hex[psdu[i] & 0xf];
		}
	}
	tl = snprintf(tail, sizeof(tail), " power: %d lqi: %u time: %llu\r\n", (int)rssi_dbm,
		      (unsigned int)lqi, (unsigned long long)time_us);
	for (int i = 0; i < tl && n < cap - 1; i++) {
		out[n++] = tail[i];
	}
	out[n] = '\0';
	return n;
}
