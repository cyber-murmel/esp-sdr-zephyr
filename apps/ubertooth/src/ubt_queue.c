/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See ubt_queue.h.
 */

#include <string.h>

#include "ubt_queue.h"

#define MASK (UBT_QUEUE_SLOTS - 1U)

void ubt_queue_init(struct ubt_queue *q, usb_pkt_rx *slots)
{
	q->slots = slots;
	ubt_queue_reset(q);
	q->dropped = 0;
}

void ubt_queue_reset(struct ubt_queue *q)
{
	__atomic_store_n(&q->head, 0U, __ATOMIC_RELEASE);
	__atomic_store_n(&q->tail, 0U, __ATOMIC_RELEASE);
	q->pending_status = 0;
	memset(q->slots, 0, UBT_QUEUE_SLOTS * sizeof(*q->slots));
}

usb_pkt_rx *ubt_queue_alloc(struct ubt_queue *q)
{
	uint32_t tail = __atomic_load_n(&q->tail, __ATOMIC_RELAXED);
	uint32_t head = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);

	if (((tail + 1U) & MASK) == (head & MASK)) {
		q->pending_status |= FIFO_OVERFLOW;
		q->dropped++;
		return NULL;
	}
	return &q->slots[tail & MASK];
}

void ubt_queue_commit(struct ubt_queue *q)
{
	uint32_t tail = __atomic_load_n(&q->tail, __ATOMIC_RELAXED);

	q->slots[tail & MASK].status |= q->pending_status;
	q->pending_status = 0;
	__atomic_store_n(&q->tail, (tail + 1U) & MASK, __ATOMIC_RELEASE);
}

const usb_pkt_rx *ubt_queue_peek(const struct ubt_queue *q)
{
	uint32_t head = __atomic_load_n(&q->head, __ATOMIC_RELAXED);
	uint32_t tail = __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);

	return (head & MASK) == (tail & MASK) ? NULL : &q->slots[head & MASK];
}

void ubt_queue_pop(struct ubt_queue *q)
{
	uint32_t head = __atomic_load_n(&q->head, __ATOMIC_RELAXED);

	__atomic_store_n(&q->head, (head + 1U) & MASK, __ATOMIC_RELEASE);
}

bool ubt_queue_take(struct ubt_queue *q, usb_pkt_rx *out)
{
	uint32_t head = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);

	for (;;) {
		uint32_t tail = __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);

		if ((head & MASK) == (tail & MASK)) {
			return false;
		}
		/* The producer cannot reuse the slot before head moves; if it did, the exchange fails. */
		memcpy(out, &q->slots[head & MASK], sizeof(*out));
		if (__atomic_compare_exchange_n(&q->head, &head, (head + 1U) & MASK, false,
						__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			return true;
		}
	}
}

size_t ubt_queue_count(const struct ubt_queue *q)
{
	uint32_t head = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);
	uint32_t tail = __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);

	return (tail - head) & MASK;
}
