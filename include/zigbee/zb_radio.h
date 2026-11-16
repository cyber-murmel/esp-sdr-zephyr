/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What the Zigbee MAC needs from an 802.15.4 radio. Backends: apps/zstack (ESP32-C6 native
 * radio), tests/unit/zigbee_mac (a mock). Frames never include the FCS. All calls come from the
 * MAC thread, one at a time. Received frames go the other way through zb_mac_rx().
 */
#ifndef ZIGBEE_ZB_RADIO_H_
#define ZIGBEE_ZB_RADIO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct zb_radio_ops {
	/* Channel 11..26; takes effect at the next start() (or tx/ed). */
	int (*set_channel)(void *ctx, uint8_t ch);
	/*
	 * Receive filter: PAN ID, short address and extended address (little endian). The radio
	 * auto-ACKs frames that match them.
	 */
	int (*set_ids)(void *ctx, uint16_t pan, uint16_t short_addr, const uint8_t ext[8]);
	/* PAN coordinator flag; promiscuous receives everything and stops auto-ACKs (scans only). */
	int (*set_mode)(void *ctx, bool coordinator, bool promiscuous);
	int (*set_txpower)(void *ctx, int8_t dbm);
	/* Apply the settings and receive. */
	int (*start)(void *ctx);
	/*
	 * Send one frame, blocking. @p csma: one clear channel assessment first. Returns 0, -EBUSY
	 * (channel busy), -ENOMSG (an ACK was requested and did not come), -EIO (a frame is being
	 * received or sent: try again shortly).
	 */
	int (*tx)(void *ctx, const uint8_t *f, size_t n, bool csma);
	/* Energy on the current channel averaged over @p ms; dBm. Blocking, interrupts receive. */
	int (*ed)(void *ctx, uint16_t ms, int8_t *dbm);
};

#endif /* ZIGBEE_ZB_RADIO_H_ */
