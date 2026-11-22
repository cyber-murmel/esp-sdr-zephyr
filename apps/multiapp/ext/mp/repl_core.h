/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The MicroPython REPL's input from an SSH session (mp_glue.c), without Zephyr or MicroPython so that
 * tests/host/test_mp_repl.py builds it for the PC. The server's thread fills a ring from the channel
 * (and sees Ctrl-C there, as the REPL may be in a busy loop that reads nothing); the REPL's thread
 * empties it. One producer and one consumer, no lock: the indexes are atomics.
 */

#ifndef MULTIAPP_MP_REPL_CORE_H_
#define MULTIAPP_MP_REPL_CORE_H_

#include <stddef.h>
#include <stdint.h>

#define REPL_RING_SIZE 512U /* a power of two */

struct repl_ring {
	uint8_t buf[REPL_RING_SIZE];
	uint32_t head; /* next byte to read (consumer) */
	uint32_t tail; /* next free slot (producer) */
	uint32_t dropped;
};

static inline void repl_ring_reset(struct repl_ring *r)
{
	__atomic_store_n(&r->head, 0U, __ATOMIC_RELAXED);
	__atomic_store_n(&r->tail, 0U, __ATOMIC_RELAXED);
	r->dropped = 0U;
}

static inline size_t repl_ring_count(const struct repl_ring *r)
{
	return (size_t)(__atomic_load_n(&r->tail, __ATOMIC_ACQUIRE) - __atomic_load_n(&r->head, __ATOMIC_ACQUIRE));
}

/* Producer: 0, or -1 when full (the byte is counted in dropped). */
static inline int repl_ring_put(struct repl_ring *r, uint8_t c)
{
	uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);

	if (tail - __atomic_load_n(&r->head, __ATOMIC_ACQUIRE) >= REPL_RING_SIZE) {
		r->dropped++;
		return -1;
	}
	r->buf[tail & (REPL_RING_SIZE - 1U)] = c;
	__atomic_store_n(&r->tail, tail + 1U, __ATOMIC_RELEASE);
	return 0;
}

/* Consumer: the next byte, or -1 when empty. */
static inline int repl_ring_get(struct repl_ring *r)
{
	uint32_t head = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
	int c;

	if (head == __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE)) {
		return -1;
	}
	c = r->buf[head & (REPL_RING_SIZE - 1U)];
	__atomic_store_n(&r->head, head + 1U, __ATOMIC_RELEASE);
	return c;
}

/* Free slots */
static inline size_t repl_ring_space(const struct repl_ring *r)
{
	return REPL_RING_SIZE - repl_ring_count(r);
}

/*
 * Move what the channel has into the ring: read() as in struct multiapp_console_io (how many bytes,
 * 0 for none, negative for an error). The interrupt character (@p intr, or -1 for none) is not stored
 * and counts: the caller raises KeyboardInterrupt, once is what the REPL needs.
 * It reads only while the ring has room for a whole chunk: what is left stays in the channel, whose
 * flow control (the SSH window) holds the client's rest back, and the REPL's thread pulls it as it
 * empties the ring. (A Ctrl-C behind more than a ring's worth of unread input is not seen until the
 * REPL has read up to it.)
 * Returns the interrupts seen; *got is the bytes read in all.
 */
static inline unsigned int repl_feed(struct repl_ring *r, int (*read)(void *ctx, void *data, size_t len),
				     void *ctx, int intr, size_t *got)
{
	uint8_t chunk[64];
	unsigned int ints = 0U;
	size_t total = 0U;
	int n;

	while (repl_ring_space(r) >= sizeof(chunk) && (n = read(ctx, chunk, sizeof(chunk))) > 0) {
		total += (size_t)n;
		for (int i = 0; i < n; i++) {
			if ((int)chunk[i] == intr) {
				ints++;
			} else {
				(void)repl_ring_put(r, chunk[i]);
			}
		}
	}
	if (got != NULL) {
		*got = total;
	}
	return ints;
}

#endif /* MULTIAPP_MP_REPL_CORE_H_ */
