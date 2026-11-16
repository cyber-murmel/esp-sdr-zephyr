/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The host side of apps/osmosdr_serial's serial transport (esdr_proto.h), whatever carries the
 * bytes (SDIO through esdr_sdio_host.h, or a UART): requests go out as frames and wait for
 * their reply; the radio's byte stream is split into reply frames, handed to the waiting
 * request, and receive blocks, handed to the app. Shared by the WHY2025 badge's P4 apps (the
 * bridge and MicroPython's esdr_remote.c).
 *
 * The splitter skips the zero padding between frames, drops bytes until a magic and a reply
 * whose length is no reply's. One thread feeds it; any thread may send.
 */

#ifndef ESDR_LINK_HOST_H_
#define ESDR_LINK_HOST_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include "esdr_proto.h"

/* A request waits this long for its reply. */
#define ESDR_LINK_TIMEOUT_MS 1000

struct esdr_link {
	/*
	 * The app's, set before esdr_link_init(). send(): one frame, @p a then @p b (@p nb may
	 * be 0), out whole; called from any thread, so it keeps frames from mixing. 0 or a
	 * negative errno.
	 */
	int (*send)(const void *a, size_t na, const void *b, size_t nb);
	/* A buffer of ESDR_BLOCK_BYTES for the block that starts, NULL to drop it. */
	uint8_t *(*get_block)(void *user);
	/* The block is in, in get_block()'s buffer (NULL: dropped). */
	void (*on_block)(uint8_t *blk, void *user);
	void *user;

	/* Bytes dropped looking for a magic, frames cut short by esdr_link_abort(). */
	uint32_t resyncs;

	/* The request waiting for its reply. */
	struct k_mutex req_lock;
	struct k_sem reply_sem;
	struct k_spinlock pend_lock;
	struct {
		uint8_t req;
		bool waiting;
		void *buf;
		uint16_t max, len;
		int status;
	} pend;
	/* The frame or block in hand: a reply's header and data, or the block's buffer. */
	uint8_t hdr[sizeof(struct esdr_ser_hdr) + ESDR_SER_MAX_DATA] __aligned(4);
	uint8_t *blk;
	uint32_t have;
	bool in_blk;
};

void esdr_link_init(struct esdr_link *l);

/*
 * Request @p req with @p value: an OUT request with @p len bytes of @p data, or with @p reply
 * an IN request for up to @p reply_max bytes into it. The reply's length, its negative status,
 * -ETIMEDOUT or send()'s errno. One request at a time; a reply that comes after its request
 * gave up is dropped.
 */
int esdr_link_request(struct esdr_link *l, uint8_t req, uint16_t value, const void *data,
		      uint16_t len, void *reply, uint16_t reply_max);

/* A transmit frame (ESDR_SER_TX_DATA) of @p len bytes of samples. 0 or send()'s errno. */
int esdr_link_send_tx(struct esdr_link *l, const void *data, size_t len);

/* The next @p n bytes of the radio's stream. */
void esdr_link_feed(struct esdr_link *l, const uint8_t *p, size_t n);

/*
 * Drops the frame or block in hand (it lost bytes: the radio sends one in a go). Returns the
 * buffer of a block cut short, the app's again, or NULL.
 */
uint8_t *esdr_link_abort(struct esdr_link *l);

/* Whether a frame or block is in hand. */
static inline bool esdr_link_busy(const struct esdr_link *l)
{
	return l->have != 0U;
}

#endif /* ESDR_LINK_HOST_H_ */
