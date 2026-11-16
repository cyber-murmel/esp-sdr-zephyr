/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Queue of USB packets between the radio threads (one producer at a time) and
 * the USB thread (the only consumer, for bulk IN and for POLL): the 128 slot
 * ring of upstream's ubertooth_usb.c, 127 usable. A full queue drops the
 * packet and flags FIFO_OVERFLOW on the next one that gets in.
 *
 * Indices live in this struct (internal RAM, atomics); the slots can sit in
 * PSRAM, which has no usable atomics but is only touched under the indices'
 * acquire/release ordering.
 */

#ifndef UBT_QUEUE_H_
#define UBT_QUEUE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ubt_proto.h"

#define UBT_QUEUE_SLOTS 128U

struct ubt_queue {
	usb_pkt_rx *slots;
	uint32_t head; /* consumer */
	uint32_t tail; /* producer */
	uint8_t pending_status;
	uint32_t dropped;
};

/* @p slots: UBT_QUEUE_SLOTS packets. */
void ubt_queue_init(struct ubt_queue *q, usb_pkt_rx *slots);

/* Empty the queue (upstream usb_queue_init() at mode starts). Producers and the consumer must be quiet. */
void ubt_queue_reset(struct ubt_queue *q);

/* A free slot to fill, or NULL if the queue is full. Then ubt_queue_commit(). */
usb_pkt_rx *ubt_queue_alloc(struct ubt_queue *q);
void ubt_queue_commit(struct ubt_queue *q);

/* The oldest packet, or NULL; ubt_queue_pop() releases it. One consumer thread only. */
const usb_pkt_rx *ubt_queue_peek(const struct ubt_queue *q);
void ubt_queue_pop(struct ubt_queue *q);

/*
 * Copy out and remove the oldest packet; false if empty. Safe for several consumer threads (the
 * USB thread answering POLL and the thread feeding the bulk endpoint), unlike peek and pop.
 */
bool ubt_queue_take(struct ubt_queue *q, usb_pkt_rx *out);

size_t ubt_queue_count(const struct ubt_queue *q);

#endif /* UBT_QUEUE_H_ */
