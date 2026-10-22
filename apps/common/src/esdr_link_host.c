/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The host side of the serial transport (esdr_link_host.h).
 */

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "esdr_link_host.h"

void esdr_link_init(struct esdr_link *l)
{
	k_mutex_init(&l->req_lock);
	k_sem_init(&l->reply_sem, 0, 1);
	memset(&l->pend, 0, sizeof(l->pend));
	l->blk = NULL;
	l->have = 0;
	l->in_blk = false;
	l->resyncs = 0;
}

int esdr_link_request(struct esdr_link *l, uint8_t req, uint16_t value, const void *data,
		      uint16_t len, void *reply, uint16_t reply_max)
{
	struct esdr_ser_hdr h = {
		.magic = sys_cpu_to_le32(ESDR_SER_MAGIC),
		.req = req,
		.flags = reply != NULL ? ESDR_SER_IN : 0U,
		.value = sys_cpu_to_le16(value),
		.len = sys_cpu_to_le16(reply != NULL ? reply_max : len),
	};
	k_spinlock_key_t key;
	int ret;

	k_mutex_lock(&l->req_lock, K_FOREVER);
	k_sem_reset(&l->reply_sem);
	key = k_spin_lock(&l->pend_lock);
	l->pend.req = req;
	l->pend.buf = reply;
	l->pend.max = reply_max;
	l->pend.len = 0;
	l->pend.status = 0;
	l->pend.waiting = true;
	k_spin_unlock(&l->pend_lock, key);
	ret = l->send(&h, sizeof(h), data, reply == NULL ? len : 0U);
	if (ret == 0) {
		k_timepoint_t end = sys_timepoint_calc(K_MSEC(ESDR_LINK_TIMEOUT_MS));
		bool waiting = true;

		/* A give without the reply is an earlier request's, late: wait on. */
		while (waiting && k_sem_take(&l->reply_sem, sys_timepoint_timeout(end)) == 0) {
			key = k_spin_lock(&l->pend_lock);
			waiting = l->pend.waiting;
			k_spin_unlock(&l->pend_lock, key);
		}
		if (waiting) {
			ret = -ETIMEDOUT;
		} else {
			ret = l->pend.status < 0 ? l->pend.status : (int)l->pend.len;
		}
	}
	/* A reply after this finds nobody: the buffer may be gone (a USB request's). */
	key = k_spin_lock(&l->pend_lock);
	l->pend.waiting = false;
	k_spin_unlock(&l->pend_lock, key);
	k_mutex_unlock(&l->req_lock);
	return ret;
}

int esdr_link_send_tx(struct esdr_link *l, const void *data, size_t len)
{
	struct esdr_ser_hdr h = {
		.magic = sys_cpu_to_le32(ESDR_SER_MAGIC),
		.req = ESDR_SER_TX_DATA,
		.len = sys_cpu_to_le16((uint16_t)len),
	};

	return l->send(&h, sizeof(h), data, len);
}

/* A reply is in: to its request, if it still waits. */
static void reply_done(struct esdr_link *l)
{
	struct esdr_ser_hdr h;
	k_spinlock_key_t key;
	bool done;

	memcpy(&h, l->hdr, sizeof(h));
	key = k_spin_lock(&l->pend_lock);
	done = l->pend.waiting && h.req == l->pend.req;
	if (done) {
		l->pend.status = (int16_t)sys_le16_to_cpu((uint16_t)h.status);
		l->pend.len = MIN(sys_le16_to_cpu(h.len), l->pend.max);
		if (l->pend.buf != NULL) {
			memcpy(l->pend.buf, l->hdr + sizeof(h), l->pend.len);
		}
		l->pend.waiting = false;
	}
	k_spin_unlock(&l->pend_lock, key);
	/* Outside the lock (it masks interrupts); the request checks that its reply came. */
	if (done) {
		k_sem_give(&l->reply_sem);
	}
}

/*
 * A frame or block starts with its magic: drop bytes until what is in hand starts like one. A
 * block's magic starts the block: what is in hand goes to its buffer.
 */
static void resync(struct esdr_link *l)
{
	static const uint32_t magics[] = {ESDR_SER_MAGIC, ESDR_BLOCK_MAGIC};

	while (l->have > 0U) {
		for (size_t m = 0; m < ARRAY_SIZE(magics); m++) {
			uint8_t want[4];

			sys_put_le32(magics[m], want);
			if (memcmp(l->hdr, want, MIN(l->have, 4U)) != 0) {
				continue;
			}
			if (l->have >= 4U && magics[m] == ESDR_BLOCK_MAGIC) {
				l->in_blk = true;
				l->blk = l->get_block(l->user);
				if (l->blk != NULL) {
					memcpy(l->blk, l->hdr, l->have);
				}
			}
			return;
		}
		memmove(l->hdr, l->hdr + 1, --l->have);
		l->resyncs++;
	}
}

void esdr_link_feed(struct esdr_link *l, const uint8_t *p, size_t n)
{
	while (n > 0U) {
		uint32_t k, len;

		if (l->in_blk) {
			k = MIN((uint32_t)n, ESDR_BLOCK_BYTES - l->have);
			if (l->blk != NULL) {
				memcpy(l->blk + l->have, p, k);
			}
			l->have += k;
			p += k;
			n -= k;
			if (l->have == ESDR_BLOCK_BYTES) {
				uint8_t *b = l->blk;

				l->in_blk = false;
				l->blk = NULL;
				l->have = 0;
				l->on_block(b, l->user);
			}
			continue;
		}
		if (l->have < 4U) {
			uint8_t c = *p++;

			n--;
			if (l->have == 0U && c == 0U) {
				/* Padding to whole words between frames (SDIO). */
				continue;
			}
			l->hdr[l->have++] = c;
			resync(l);
			continue;
		}
		/* A reply: its header, then its data. */
		len = sizeof(struct esdr_ser_hdr);
		if (l->have >= len) {
			len += sys_get_le16(l->hdr + offsetof(struct esdr_ser_hdr, len));
		}
		k = MIN((uint32_t)n, len - l->have);
		memcpy(l->hdr + l->have, p, k);
		l->have += k;
		p += k;
		n -= k;
		if (l->have < sizeof(struct esdr_ser_hdr)) {
			continue;
		}
		len = sys_get_le16(l->hdr + offsetof(struct esdr_ser_hdr, len));
		if (len > ESDR_SER_MAX_DATA) {
			/* Not a reply: look for the next magic. */
			memmove(l->hdr, l->hdr + 1, --l->have);
			l->resyncs++;
			resync(l);
			continue;
		}
		if (l->have == sizeof(struct esdr_ser_hdr) + len) {
			reply_done(l);
			l->have = 0;
		}
	}
}

uint8_t *esdr_link_abort(struct esdr_link *l)
{
	uint8_t *b = l->blk;

	if (l->have != 0U) {
		l->resyncs++;
	}
	l->have = 0;
	l->in_blk = false;
	l->blk = NULL;
	return b;
}
