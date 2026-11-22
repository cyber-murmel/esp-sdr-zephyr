/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Adapted from Zephyr samples/net/ieee802154/sniffer (cf45cd71868) for apps/wpan.
 */

#ifndef SNIFFER_LINE_H_
#define SNIFFER_LINE_H_

#include <stddef.h>
#include <stdint.h>

/**
 * Format one captured frame as the sample's text line
 * `psdu: <hex> power: <dBm> lqi: <n> time: <us>\r\n`, NUL-terminated.
 *
 * @return the line's length (without the NUL); the line is cut at @p cap - 1 characters when it
 *         does not fit. 0 when @p cap is 0.
 */
size_t sniffer_line(char *out, size_t cap, const uint8_t *psdu, size_t len, uint8_t lqi,
		    int16_t rssi_dbm, uint64_t time_us);

/** Longest line: 127 PSDU bytes, the fields, CR LF, NUL. */
#define SNIFFER_LINE_MAX 320

#endif /* SNIFFER_LINE_H_ */
