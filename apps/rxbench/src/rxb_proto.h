/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The payload of a rxbench packet, shared by the board and tools/udp_tx.c (plain C, no Zephyr):
 *   bytes 0..3  sequence number, big endian
 *   bytes 4..   the pattern A5 C3 5A 3C, repeated to the end (the position decides the byte)
 * A payload is at least RXB_MIN_PAYLOAD bytes.
 */

#ifndef RXB_PROTO_H_
#define RXB_PROTO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The one UDP port the board claims. */
#define RXB_PORT 4993

#define RXB_SEQ_BYTES   4U
#define RXB_MIN_PAYLOAD 8U

/* Bytes the sparse check reads at the end of a payload. */
#define RXB_TAIL_CHECK 4U

static inline uint8_t rxb_pat_byte(size_t pos)
{
	static const uint8_t pat[4] = {0xa5, 0xc3, 0x5a, 0x3c};

	return pat[(pos - RXB_SEQ_BYTES) & 3U];
}

/* Fill a payload of len bytes (at least RXB_MIN_PAYLOAD) for sequence number seq. */
static inline void rxb_fill(uint8_t *p, size_t len, uint32_t seq)
{
	p[0] = (uint8_t)(seq >> 24);
	p[1] = (uint8_t)(seq >> 16);
	p[2] = (uint8_t)(seq >> 8);
	p[3] = (uint8_t)seq;
	for (size_t i = RXB_SEQ_BYTES; i < len; i++) {
		p[i] = rxb_pat_byte(i);
	}
}

/*
 * Check a payload and return its sequence number in *seq. Sparse: the first pattern word and the
 * last RXB_TAIL_CHECK bytes (a few cycles, whatever the length); full: every byte.
 */
static inline bool rxb_check(const uint8_t *p, size_t len, bool full, uint32_t *seq)
{
	size_t from = len - RXB_TAIL_CHECK;

	if (len < RXB_MIN_PAYLOAD) {
		return false;
	}
	*seq = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
	for (size_t i = RXB_SEQ_BYTES; i < RXB_MIN_PAYLOAD; i++) {
		if (p[i] != rxb_pat_byte(i)) {
			return false;
		}
	}
	if (full) {
		from = RXB_MIN_PAYLOAD;
	}
	for (size_t i = from; i < len; i++) {
		if (p[i] != rxb_pat_byte(i)) {
			return false;
		}
	}
	return true;
}

#endif /* RXB_PROTO_H_ */
