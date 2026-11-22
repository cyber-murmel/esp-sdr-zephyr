/*
 * Copyright (c) 2026 Espressif Systems (Shanghai) Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Adapted from Zephyr samples/net/ieee802154/sniffer (cf45cd71868) for apps/wpan.
 */

#ifndef SNIFFER_H_
#define SNIFFER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Start the emitter thread; capture stays off until `sniffer start`. */
void sniffer_init(void);

/**
 * Hand one received frame (FCS already stripped) to the capture; a no-op unless capturing.
 * Only a memcpy and a queue put: it runs in the radio's receive thread.
 */
void sniffer_frame(const uint8_t *psdu, size_t len, uint8_t lqi, int16_t rssi, uint64_t time_us);

/** True while `sniffer start` is in effect. */
bool sniffer_capturing(void);

#endif /* SNIFFER_H_ */
