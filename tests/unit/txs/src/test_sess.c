/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The control state machine of a network run (apps/txstream_eth/src/tx_sess.c): the answers to TXGO
 * and TXEN, the RF gate, the retried TXGO, a busy stream, a start that fails.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "tx_proto.h"
#include "tx_sess.h"

static struct {
	bool rf_ok;
	int start_ret;
	unsigned int starts, stops, draws;
	uint32_t id, rate;
	bool rf;
} fake;

static bool f_rf_ok(void *ctx, uint8_t rate)
{
	ARG_UNUSED(ctx);
	fake.rate = rate;
	return fake.rf_ok;
}

static int f_start(void *ctx, const struct txp_go *go, uint32_t id, bool rf)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(go);
	fake.starts++;
	fake.id = id;
	fake.rf = rf;
	return fake.start_ret;
}

static void f_stop(void *ctx)
{
	ARG_UNUSED(ctx);
	fake.stops++;
}

static uint32_t f_rand(void *ctx)
{
	ARG_UNUSED(ctx);
	/* the first draw is zero: drawn again */
	return fake.draws++ == 0U ? 0U : 0x1000U + fake.draws;
}

static const struct tx_sess_ops ops = {.rf_ok = f_rf_ok, .start = f_start, .stop = f_stop, .rand32 = f_rand};
static struct tx_sess sess;

static void before(void *unused)
{
	ARG_UNUSED(unused);
	memset(&fake, 0, sizeof(fake));
	tx_sess_init(&sess, 12U, 32768U);
}

ZTEST_SUITE(tx_sess, NULL, NULL, before, NULL, NULL);

static struct txp_go go(uint8_t rate, uint8_t fmt, uint16_t flags, uint32_t nonce)
{
	return (struct txp_go){.rate_msps = rate, .fmt = fmt, .flags = flags, .seconds = 3, .nonce = nonce};
}

ZTEST(tx_sess, test_start)
{
	struct txp_go m = go(20, TXP_FMT_IQ8, 0, 7);
	struct txp_ak ak;

	tx_sess_go(&sess, &ops, NULL, &m, &ak);
	zassert_equal(ak.code, TXP_OK);
	zassert_equal(ak.nonce, 7U);
	zassert_equal(ak.flags, 0U);
	zassert_equal(ak.stream_id, fake.id);
	zassert_not_equal(ak.stream_id, 0U);
	zassert_equal(ak.max_words, 702U);
	zassert_equal(ak.depth, 10U * 702U);
	zassert_equal(ak.group, 32768U);
	zassert_false(fake.rf);
	/* w32: half the words in a packet, the depth in words follows */
	tx_sess_ended(&sess);
	m = go(40, TXP_FMT_WORD32, 0, 8);
	tx_sess_go(&sess, &ops, NULL, &m, &ak);
	zassert_equal(ak.code, TXP_OK);
	zassert_equal(ak.max_words, 351U);
	zassert_equal(ak.depth, 10U * 351U);
}

ZTEST(tx_sess, test_stream_id_never_zero)
{
	struct txp_go m = go(20, TXP_FMT_IQ8, 0, 1);
	struct txp_ak ak;

	tx_sess_go(&sess, &ops, NULL, &m, &ak);
	zassert_equal(fake.draws, 2U, "the zero was drawn again");
	zassert_not_equal(ak.stream_id, 0U);
}

ZTEST(tx_sess, test_bad_values)
{
	struct txp_ak ak;
	struct txp_go m = go(30, TXP_FMT_IQ8, 0, 1);

	tx_sess_go(&sess, &ops, NULL, &m, &ak);
	zassert_equal(ak.code, TXP_BAD_RATE);
	m = go(20, 2, 0, 2);
	tx_sess_go(&sess, &ops, NULL, &m, &ak);
	zassert_equal(ak.code, TXP_BAD_FMT);
	m = go(20, TXP_FMT_IQ8, 0x8000U, 3);
	tx_sess_go(&sess, &ops, NULL, &m, &ak);
	zassert_equal(ak.code, TXP_BAD_FLAGS);
	zassert_equal(ak.nonce, 3U);
	zassert_equal(fake.starts, 0U, "nothing started");
}

ZTEST(tx_sess, test_rf_gate)
{
	struct txp_ak ak;
	struct txp_go m = go(20, TXP_FMT_IQ8, TXP_GO_RF, 1);

	/* not armed (or not inited, or outside the band): code 5, nothing started */
	fake.rf_ok = false;
	tx_sess_go(&sess, &ops, NULL, &m, &ak);
	zassert_equal(ak.code, TXP_RF_REFUSED);
	zassert_equal(ak.flags, 0U);
	zassert_equal(fake.starts, 0U);
	zassert_equal(fake.rate, 20U);
	/* armed: RF granted and said so */
	fake.rf_ok = true;
	m.nonce = 2;
	tx_sess_go(&sess, &ops, NULL, &m, &ak);
	zassert_equal(ak.code, TXP_OK);
	zassert_equal(ak.flags, TXP_AK_RF);
	zassert_true(fake.rf);
	/* without the flag the gate is not asked and RF stays off */
	tx_sess_ended(&sess);
	fake.rf_ok = false;
	m = go(20, TXP_FMT_IQ8, TXP_GO_VERIFY, 3);
	tx_sess_go(&sess, &ops, NULL, &m, &ak);
	zassert_equal(ak.code, TXP_OK);
	zassert_equal(ak.flags, 0U);
	zassert_false(fake.rf);
}

ZTEST(tx_sess, test_busy_and_retry)
{
	struct txp_ak a1, a2, a3;
	struct txp_go m = go(20, TXP_FMT_IQ8, 0, 10);

	tx_sess_go(&sess, &ops, NULL, &m, &a1);
	zassert_equal(a1.code, TXP_OK);
	zassert_equal(fake.starts, 1U);
	/* the same nonce while it runs: the same answer, no second start */
	tx_sess_go(&sess, &ops, NULL, &m, &a2);
	zassert_mem_equal(&a1, &a2, sizeof(a1));
	zassert_equal(fake.starts, 1U);
	/* another nonce while it runs: busy */
	m.nonce = 11;
	tx_sess_go(&sess, &ops, NULL, &m, &a3);
	zassert_equal(a3.code, TXP_BUSY);
	zassert_equal(a3.nonce, 11U);
	zassert_equal(fake.starts, 1U);
	/* after the end: the old nonce is a stale duplicate (code 7, nothing started), a new one starts a run */
	tx_sess_ended(&sess);
	m.nonce = 10;
	tx_sess_go(&sess, &ops, NULL, &m, &a3);
	zassert_equal(a3.code, TXP_NO_RESOURCE);
	zassert_equal(a3.nonce, 10U);
	zassert_equal(fake.starts, 1U);
	m.nonce = 12;
	tx_sess_go(&sess, &ops, NULL, &m, &a3);
	zassert_equal(a3.code, TXP_OK);
	zassert_equal(fake.starts, 2U);
	zassert_not_equal(a3.stream_id, a1.stream_id, "a stream ID a session");
}

ZTEST(tx_sess, test_start_fails)
{
	struct txp_ak ak;
	struct txp_go m = go(20, TXP_FMT_IQ8, 0, 1);

	fake.start_ret = -EBUSY;
	tx_sess_go(&sess, &ops, NULL, &m, &ak);
	zassert_equal(ak.code, TXP_BUSY);
	fake.start_ret = -EAGAIN;
	m.nonce = 2;
	tx_sess_go(&sess, &ops, NULL, &m, &ak);
	zassert_equal(ak.code, TXP_NO_RESOURCE);
	/* a failed start leaves no run: the next TXGO may start one */
	fake.start_ret = 0;
	m.nonce = 3;
	tx_sess_go(&sess, &ops, NULL, &m, &ak);
	zassert_equal(ak.code, TXP_OK);
}

ZTEST(tx_sess, test_end)
{
	struct txp_ak ak;
	struct txp_en e = {.nonce = 99};
	struct txp_go m = go(20, TXP_FMT_IQ8, 0, 1);

	/* nothing runs: answered, nothing stopped */
	tx_sess_en(&sess, &ops, NULL, &e, &ak);
	zassert_equal(ak.code, TXP_OK);
	zassert_equal(ak.nonce, 99U);
	zassert_equal(ak.stream_id + ak.max_words + ak.depth + ak.group, 0U);
	zassert_equal(fake.stops, 0U);
	tx_sess_go(&sess, &ops, NULL, &m, &ak);
	tx_sess_en(&sess, &ops, NULL, &e, &ak);
	zassert_equal(fake.stops, 1U);
	/* still busy until the stream says it ended */
	m.nonce = 2;
	tx_sess_go(&sess, &ops, NULL, &m, &ak);
	zassert_equal(ak.code, TXP_BUSY);
	tx_sess_ended(&sess);
	tx_sess_go(&sess, &ops, NULL, &m, &ak);
	zassert_equal(ak.code, TXP_OK);
}

ZTEST(tx_sess, test_reset)
{
	struct txp_ak ak;
	struct txp_go m = go(20, TXP_FMT_IQ8, 0, 1);

	tx_sess_go(&sess, &ops, NULL, &m, &ak);
	tx_sess_reset(&sess);
	/* a new subscriber: the old nonce is nobody's, and nothing runs */
	tx_sess_go(&sess, &ops, NULL, &m, &ak);
	zassert_equal(ak.code, TXP_OK);
	zassert_equal(fake.starts, 2U);
}
