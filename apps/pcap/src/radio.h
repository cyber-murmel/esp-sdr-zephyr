/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The ESP32-C6 radio (apps/osmosdr_serial) over SDIO, as the bridge has it: requests out,
 * replies back, the radio's receive blocks (4096 bytes, esdr_proto.h) handed to a sink. One
 * radio; any thread may request.
 */

#ifndef RADIO_H_
#define RADIO_H_

#include <stdbool.h>
#include <stdint.h>

#include "esdr_proto.h"

/* A block the radio sent, in a pool buffer that the sink returns with radio_block_free(). */
typedef void (*radio_block_fn)(uint8_t *blk);

/* The C6 out of reset, the link up (or retrying), the reader thread started. */
int radio_init(void);

/* Where blocks go (NULL: dropped). From the reader thread. */
void radio_set_sink(radio_block_fn fn);
void radio_block_free(uint8_t *blk);

/* One request (esdr_link_request()): the reply's length, its negative status or an errno. */
int radio_request(uint8_t req, uint16_t value, const void *data, uint16_t len, void *reply,
		  uint16_t reply_max);

/* USB-C port J8 to the C6 for @p seconds (the WHY2025 badge), then back: -ENOTSUP elsewhere. */
int radio_usb_to_c6(uint32_t seconds);

struct radio_stats {
	uint32_t blocks, dropped, resyncs;
};

void radio_get_stats(struct radio_stats *st);

#endif /* RADIO_H_ */
