/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Single producer, single consumer ring, without locks: the producer owns head and writes an
 * item before it publishes it (release), the consumer owns tail and frees a slot after it has
 * copied the item out (release). Plain C (GCC __atomic builtins) so that it builds on the host.
 */

#ifndef RXB_RING_H_
#define RXB_RING_H_

#include <stdbool.h>
#include <stdint.h>

/* One claimed frame: the buffer to give back, and where the payload is. */
struct rxb_item {
	void *buf;
	uint16_t len;	/* the frame */
	uint16_t off;	/* of the UDP payload */
	uint16_t plen;	/* of the UDP payload */
};

struct rxb_ring {
	uint32_t head;	/* next slot to write, counts up and wraps by 2^32 */
	uint32_t tail;	/* next slot to read */
	uint32_t mask;	/* slots - 1, slots a power of two */
	struct rxb_item *items;
};

static inline void rxb_ring_init(struct rxb_ring *r, struct rxb_item *items, uint32_t slots)
{
	r->head = 0U;
	r->tail = 0U;
	r->mask = slots - 1U;
	r->items = items;
}

/* Producer: false when full. */
static inline bool rxb_ring_push(struct rxb_ring *r, const struct rxb_item *it)
{
	uint32_t head = r->head;

	if (head - __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE) > r->mask) {
		return false;
	}
	r->items[head & r->mask] = *it;
	__atomic_store_n(&r->head, head + 1U, __ATOMIC_RELEASE);
	return true;
}

/* Consumer: false when empty. */
static inline bool rxb_ring_pop(struct rxb_ring *r, struct rxb_item *it)
{
	uint32_t tail = r->tail;

	if (tail == __atomic_load_n(&r->head, __ATOMIC_ACQUIRE)) {
		return false;
	}
	*it = r->items[tail & r->mask];
	__atomic_store_n(&r->tail, tail + 1U, __ATOMIC_RELEASE);
	return true;
}

/* Items in the ring now (either side; a snapshot). */
static inline uint32_t rxb_ring_count(const struct rxb_ring *r)
{
	return __atomic_load_n(&r->head, __ATOMIC_ACQUIRE) -
	       __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
}

#endif /* RXB_RING_H_ */
