/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit test of apps/common/src/esdr_link_host.c, the host side of the serial transport: the
 * splitter takes a byte stream apart into blocks (zero padding and garbage before a magic, a
 * block split across feeds at every chunk size, a block with no buffer, a block cut short) and
 * replies (data longer than the request takes, a negative status, a length no reply has, a
 * reply nobody waits for, a wake-up without a reply, a reply byte by byte); requests and
 * transmit frames go out with the header the radio expects. The fake transport answers a request from inside send(), as the
 * radio's reply would come in on the reader's thread.
 */
#include <errno.h>
#include <string.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include "esdr_link_host.h"

#define BLK ESDR_BLOCK_BYTES
#define HDR sizeof(struct esdr_ser_hdr)

static struct esdr_link link;

/* Block buffers handed out, and what came back. */
static uint8_t bufs[3][BLK];
static unsigned int next_buf;
static bool no_buf;
static uint8_t *got[8];
static unsigned int ngot;

/* The last frame sent, and the reply send() feeds back (chunk 0: all at once). */
static uint8_t sent[HDR + 2U * BLK];
static size_t nsent;
static uint8_t answer[HDR + 2U * BLK];
static size_t nanswer, answer_chunk;
/* send() also gives the reply semaphore: an earlier reply's give, late. */
static bool stale_give;

static uint8_t *get_block(void *user)
{
	ARG_UNUSED(user);
	if (no_buf) {
		return NULL;
	}
	return bufs[next_buf++ % ARRAY_SIZE(bufs)];
}

static void on_block(uint8_t *blk, void *user)
{
	ARG_UNUSED(user);
	zassert_true(ngot < ARRAY_SIZE(got));
	got[ngot++] = blk;
}

static void feed_chunks(const uint8_t *p, size_t n, size_t chunk)
{
	for (size_t k = 0; k < n;) {
		size_t m = chunk == 0U ? n - k : MIN(chunk, n - k);

		esdr_link_feed(&link, p + k, m);
		k += m;
	}
}

static int fake_send(const void *a, size_t na, const void *b, size_t nb)
{
	zassert_true(na + nb <= sizeof(sent));
	memcpy(sent, a, na);
	if (nb > 0U) {
		memcpy(sent + na, b, nb);
	}
	nsent = na + nb;
	if (stale_give) {
		k_sem_give(&link.reply_sem);
	}
	feed_chunks(answer, nanswer, answer_chunk);
	return 0;
}

/* A reply frame into @p p: header, @p len bytes of data counting up from @p first. */
static size_t make_reply(uint8_t *p, uint8_t req, int16_t status, uint16_t len, uint8_t first)
{
	struct esdr_ser_hdr h = {
		.magic = sys_cpu_to_le32(ESDR_SER_MAGIC),
		.req = req,
		.len = sys_cpu_to_le16(len),
		.status = (int16_t)sys_cpu_to_le16((uint16_t)status),
	};

	memcpy(p, &h, HDR);
	for (uint16_t k = 0; k < len; k++) {
		p[HDR + k] = (uint8_t)(first + k);
	}
	return HDR + len;
}

/* A block into @p p: its magic, then bytes from @p seed. */
static size_t make_block(uint8_t *p, uint8_t seed)
{
	sys_put_le32(ESDR_BLOCK_MAGIC, p);
	for (uint32_t k = 4; k < BLK; k++) {
		p[k] = (uint8_t)(seed + k * 7U);
	}
	return BLK;
}

static void before(void *f)
{
	ARG_UNUSED(f);
	memset(&link, 0, sizeof(link));
	link.send = fake_send;
	link.get_block = get_block;
	link.on_block = on_block;
	esdr_link_init(&link);
	next_buf = 0;
	no_buf = false;
	ngot = 0;
	nsent = 0;
	nanswer = 0;
	answer_chunk = 0;
	stale_give = false;
}

ZTEST_SUITE(esdr_link_host, NULL, NULL, before, NULL, NULL);

/* Padding and garbage, two blocks, at every chunk size from a byte to all at once. */
ZTEST(esdr_link_host, test_blocks)
{
	static uint8_t s[2U * BLK + 64U];
	static const uint8_t junk[] = {0x12, 0x45, 0x42, 0x4c, 0x99, 0x45};
	static const size_t chunks[] = {1, 3, 4, 7, 12, 100, 4095, 4096, 4097, 0};
	size_t n = 0;

	memset(s, 0, 5);
	n += 5;
	memcpy(s + n, junk, sizeof(junk));
	n += sizeof(junk);
	n += make_block(s + n, 1);
	memset(s + n, 0, 3);
	n += 3;
	n += make_block(s + n, 2);

	for (size_t c = 0; c < ARRAY_SIZE(chunks); c++) {
		before(NULL);
		feed_chunks(s, n, chunks[c]);
		zassert_equal(ngot, 2U, "chunk %zu", chunks[c]);
		zassert_mem_equal(got[0], s + 5 + sizeof(junk), BLK, "chunk %zu", chunks[c]);
		zassert_mem_equal(got[1], s + n - BLK, BLK, "chunk %zu", chunks[c]);
		/* Every junk byte: "EBL" after 0x12 starts like a magic and slides out too. */
		zassert_equal(link.resyncs, sizeof(junk), "chunk %zu", chunks[c]);
		zassert_false(esdr_link_busy(&link));
	}
}

/* No buffer: the block is dropped whole, the next one comes through. */
ZTEST(esdr_link_host, test_block_no_buffer)
{
	static uint8_t s[2U * BLK];

	make_block(s, 3);
	make_block(s + BLK, 4);
	no_buf = true;
	esdr_link_feed(&link, s, BLK);
	zassert_equal(ngot, 1U);
	zassert_is_null(got[0]);
	no_buf = false;
	esdr_link_feed(&link, s + BLK, BLK);
	zassert_equal(ngot, 2U);
	zassert_mem_equal(got[1], s + BLK, BLK);
}

/* A block cut short: its buffer comes back, what follows parses. */
ZTEST(esdr_link_host, test_abort)
{
	static uint8_t s[BLK];

	make_block(s, 5);
	esdr_link_feed(&link, s, 1000);
	zassert_true(esdr_link_busy(&link));
	zassert_equal_ptr(esdr_link_abort(&link), bufs[0]);
	zassert_equal(link.resyncs, 1U);
	zassert_false(esdr_link_busy(&link));
	zassert_is_null(esdr_link_abort(&link));
	esdr_link_feed(&link, s, BLK);
	zassert_equal(ngot, 1U);
	zassert_mem_equal(got[0], s, BLK);
}

/* An OUT request: the header and data the radio expects; a reply without data. */
ZTEST(esdr_link_host, test_request_out)
{
	const uint8_t data[5] = {1, 2, 3, 4, 5};
	struct esdr_ser_hdr h;

	nanswer = make_reply(answer, ESDR_REQ_SET_FREQ, 0, 0, 0);
	zassert_equal(esdr_link_request(&link, ESDR_REQ_SET_FREQ, 0x1234, data, sizeof(data), NULL,
					0), 0);
	zassert_equal(nsent, HDR + sizeof(data));
	memcpy(&h, sent, HDR);
	zassert_equal(sys_le32_to_cpu(h.magic), ESDR_SER_MAGIC);
	zassert_equal(h.req, ESDR_REQ_SET_FREQ);
	zassert_equal(h.flags, 0U);
	zassert_equal(sys_le16_to_cpu(h.value), 0x1234);
	zassert_equal(sys_le16_to_cpu(h.len), sizeof(data));
	zassert_mem_equal(sent + HDR, data, sizeof(data));
}

/* An IN request: at most what it takes of the reply's data; byte by byte too. */
ZTEST(esdr_link_host, test_request_in)
{
	uint8_t reply[32], want[32];
	struct esdr_ser_hdr h;

	for (size_t chunk = 0; chunk <= 1U; chunk++) {
		memset(reply, 0xaa, sizeof(reply));
		answer_chunk = chunk;
		nanswer = make_reply(answer, ESDR_REQ_GET_STATE, 0, 20, 40);
		zassert_equal(esdr_link_request(&link, ESDR_REQ_GET_STATE, 0, NULL, 0, reply, 16),
			      16);
		memcpy(&h, sent, HDR);
		zassert_equal(nsent, HDR);
		zassert_equal(h.flags, ESDR_SER_IN);
		zassert_equal(sys_le16_to_cpu(h.len), 16U);
		memset(want, 0xaa, sizeof(want));
		for (int k = 0; k < 16; k++) {
			want[k] = (uint8_t)(40 + k);
		}
		zassert_mem_equal(reply, want, sizeof(want));
		zassert_false(esdr_link_busy(&link));
	}
}

/* The radio's errno comes back as the result. */
ZTEST(esdr_link_host, test_request_status)
{
	nanswer = make_reply(answer, ESDR_REQ_SET_GAPLESS, -ENOTSUP, 0, 0);
	zassert_equal(esdr_link_request(&link, ESDR_REQ_SET_GAPLESS, 0, NULL, 0, NULL, 0),
		      -ENOTSUP);
}

/* Another request's reply does not answer this one; a later one is dropped. */
ZTEST(esdr_link_host, test_request_late_reply)
{
	uint8_t reply[8], s[HDR + 8U];
	size_t n;

	memset(reply, 0x55, sizeof(reply));
	nanswer = make_reply(answer, ESDR_REQ_GET_INFO, 0, 8, 1);
	zassert_equal(esdr_link_request(&link, ESDR_REQ_GET_STATE, 0, NULL, 0, reply,
					sizeof(reply)), -ETIMEDOUT);
	n = make_reply(s, ESDR_REQ_GET_STATE, 0, 8, 1);
	esdr_link_feed(&link, s, n);
	for (size_t k = 0; k < sizeof(reply); k++) {
		zassert_equal(reply[k], 0x55);
	}
	zassert_false(esdr_link_busy(&link));
}

/* A give without a reply (one an earlier request's reply made after its timeout) answers
 * nothing. */
ZTEST(esdr_link_host, test_request_stale_give)
{
	uint8_t reply[8];

	stale_give = true;
	zassert_equal(esdr_link_request(&link, ESDR_REQ_GET_STATE, 0, NULL, 0, reply,
					sizeof(reply)), -ETIMEDOUT);
	nanswer = make_reply(answer, ESDR_REQ_GET_STATE, 0, 8, 1);
	zassert_equal(esdr_link_request(&link, ESDR_REQ_GET_STATE, 0, NULL, 0, reply,
					sizeof(reply)), 8);
}

/* A length no reply has: the header is no frame, the block after it comes through. */
ZTEST(esdr_link_host, test_reply_oversize)
{
	static uint8_t s[HDR + BLK];
	uint8_t reply[4];
	size_t n;

	n = make_reply(s, ESDR_REQ_GET_STATE, 0, 0, 0);
	sys_put_le16(ESDR_SER_MAX_DATA + 1U, s + offsetof(struct esdr_ser_hdr, len));
	n += make_block(s + n, 6);
	memcpy(answer, s, n);
	nanswer = n;
	zassert_equal(esdr_link_request(&link, ESDR_REQ_GET_STATE, 0, NULL, 0, reply,
					sizeof(reply)), -ETIMEDOUT);
	zassert_equal(ngot, 1U);
	zassert_mem_equal(got[0], s + HDR, BLK);
	zassert_equal(link.resyncs, HDR);
}

/* A block between a request and its reply goes to the app, the reply to the request. */
ZTEST(esdr_link_host, test_block_then_reply)
{
	static uint8_t s[BLK + HDR + 4U];
	uint8_t reply[4];
	size_t n;

	n = make_block(s, 7);
	n += make_reply(s + n, ESDR_REQ_GET_STATE, 0, 4, 9);
	memcpy(answer, s, n);
	nanswer = n;
	answer_chunk = 1000;
	zassert_equal(esdr_link_request(&link, ESDR_REQ_GET_STATE, 0, NULL, 0, reply,
					sizeof(reply)), 4);
	zassert_equal(ngot, 1U);
	zassert_mem_equal(got[0], s, BLK);
	zassert_equal(reply[0], 9U);
	zassert_equal(reply[3], 12U);
}

/* A transmit frame: its header, then the samples. */
ZTEST(esdr_link_host, test_send_tx)
{
	const int8_t iq[6] = {1, -1, 2, -2, 3, -3};
	struct esdr_ser_hdr h;

	zassert_equal(esdr_link_send_tx(&link, iq, sizeof(iq)), 0);
	zassert_equal(nsent, HDR + sizeof(iq));
	memcpy(&h, sent, HDR);
	zassert_equal(sys_le32_to_cpu(h.magic), ESDR_SER_MAGIC);
	zassert_equal(h.req, ESDR_SER_TX_DATA);
	zassert_equal(sys_le16_to_cpu(h.len), sizeof(iq));
	zassert_mem_equal(sent + HDR, iq, sizeof(iq));
}
