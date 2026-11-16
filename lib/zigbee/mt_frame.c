/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Z-Stack MT framing: see include/zigbee/mt.h.
 */
#include <stdbool.h>
#include <string.h>

#include <zigbee/mt.h>

void mt_parser_init(struct mt_parser *p)
{
	memset(p, 0, sizeof(*p));
}

static uint8_t xor_bytes(const uint8_t *p, size_t n)
{
	uint8_t x = 0;

	while (n-- > 0U) {
		x ^= *p++;
	}
	return x;
}

/* Take one byte; MT_FEED_FRAME when raw[] holds a good frame (copied to p->frame). */
static int push(struct mt_parser *p, uint8_t b)
{
	if (p->n == 0U) {
		if (b == MT_SOF) {
			p->raw[p->n++] = b;
		}
		return MT_FEED_MORE;
	}

	p->raw[p->n++] = b;
	if (p->n == 2U && p->raw[1] > MT_MAX_PAYLOAD) {
		return -1;
	}
	if (p->n < 2U || p->n < (uint16_t)p->raw[1] + 5U) {
		return MT_FEED_MORE;
	}

	uint8_t len = p->raw[1];

	if (xor_bytes(&p->raw[1], (size_t)len + 3U) != p->raw[len + 4U]) {
		return -1;
	}
	p->frame.len = len;
	p->frame.type = p->raw[2] >> 5;
	p->frame.sub = p->raw[2] & 0x1F;
	p->frame.cmd = p->raw[3];
	memcpy(p->frame.data, &p->raw[4], len);
	p->n = 0;
	return MT_FEED_FRAME;
}

enum mt_feed mt_parser_feed(struct mt_parser *p, uint8_t byte)
{
	int r = push(p, byte);

	if (r >= 0) {
		return (enum mt_feed)r;
	}

	/*
	 * Bad frame: the real SOF may sit inside what was swallowed, so run the bytes after the
	 * failed SOF through the parser again; a failure while doing so restarts after the SOF
	 * it started at (strictly later each time). A frame found that way is returned, bytes
	 * of a frame still in flight behind it are dropped (the next SOF resyncs).
	 */
	uint8_t tmp[MT_MAX_WIRE];
	uint16_t n = p->n;
	uint16_t i = 1, sof = 0;

	memcpy(tmp, p->raw, n);
	p->n = 0;
	p->errors++;
	while (i < n) {
		bool idle = p->n == 0U;

		r = push(p, tmp[i]);
		if (idle && p->n == 1U) {
			sof = i;
		}
		if (r == MT_FEED_FRAME) {
			return MT_FEED_FRAME;
		}
		if (r < 0) {
			p->n = 0;
			p->errors++;
			i = sof + 1U;
			continue;
		}
		i++;
	}
	return MT_FEED_MORE;
}

size_t mt_encode(const struct mt_frame *f, uint8_t *out)
{
	out[0] = MT_SOF;
	out[1] = f->len;
	out[2] = (uint8_t)((f->type << 5) | (f->sub & 0x1F));
	out[3] = f->cmd;
	memcpy(&out[4], f->data, f->len);
	out[4 + f->len] = xor_bytes(&out[1], (size_t)f->len + 3U);
	return (size_t)f->len + 5U;
}
