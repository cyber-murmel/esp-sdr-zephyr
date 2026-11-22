/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Transmit stream, credits and the status record: see txs_credit.h.
 */

#include <esp_sdr/esp_sdr_place.h>

#include "txs_credit.h"

ESP_SDR_CODE(PACED_CHUNK, txs_status_make)
void txs_status_make(struct txs_status *st, uint32_t session, uint64_t play,
		     const struct txs_jit *j, const struct txs_flip *f, uint32_t depth)
{
	st->session = session;
	st->play = play;
	st->contig = j->cursor;
	/* before the first data word only the window itself: the sender fills it, the engine starts, and
	 * the room beyond it opens with the data (a flood into the buffers before that is not asked for) */
	st->credit = txs_credit_limit(play, j->cursor, j->started || j->hi == 0U ? depth : 0U, j->hi);
	st->restart = txs_jit_restart(j);
	st->underruns = j->underruns;
	st->lost = txs_jit_lost(j);
	st->late = j->late;
	st->stale = f->stale_words;
}

static inline __attribute__((always_inline)) void put32(uint8_t *o, uint32_t v)
{
	o[0] = (uint8_t)(v >> 24);
	o[1] = (uint8_t)(v >> 16);
	o[2] = (uint8_t)(v >> 8);
	o[3] = (uint8_t)v;
}

static inline __attribute__((always_inline)) void put64(uint8_t *o, uint64_t v)
{
	put32(o, (uint32_t)(v >> 32));
	put32(o + 4, (uint32_t)v);
}

static inline __attribute__((always_inline)) uint32_t get32(const uint8_t *i)
{
	return ((uint32_t)i[0] << 24) | ((uint32_t)i[1] << 16) | ((uint32_t)i[2] << 8) | i[3];
}

static inline __attribute__((always_inline)) uint64_t get64(const uint8_t *i)
{
	return ((uint64_t)get32(i) << 32) | get32(i + 4);
}

ESP_SDR_CODE(PACED_CHUNK, txs_status_put)
void txs_status_put(uint8_t *out, const struct txs_status *st)
{
	put32(out, st->session);
	put64(out + 4, st->play);
	put64(out + 12, st->contig);
	put64(out + 20, st->credit);
	put64(out + 28, st->restart);
	put32(out + 36, st->underruns);
	put64(out + 40, st->lost);
	put64(out + 48, st->late);
	put64(out + 56, st->stale);
}

/* the decode: for the shell, the tests and the host tool; nothing on the stream's path reads a record back */
ESP_SDR_CODE(COLD, txs_status_get)
int txs_status_get(struct txs_status *st, const uint8_t *in, uint32_t len)
{
	if (len != TXS_STATUS_BYTES) {
		return -1;
	}
	st->session = get32(in);
	st->play = get64(in + 4);
	st->contig = get64(in + 12);
	st->credit = get64(in + 20);
	st->restart = get64(in + 28);
	st->underruns = get32(in + 36);
	st->lost = get64(in + 40);
	st->late = get64(in + 48);
	st->stale = get64(in + 56);
	return 0;
}

void txs_snap_init(struct txs_snap *s)
{
	atomic_init(&s->seq, 0);
	for (uint32_t i = 0; i < TXS_STATUS_BYTES / 4U; i++) {
		atomic_init(&s->w[i], 0);
	}
}

ESP_SDR_CODE(PACED_CHUNK, txs_snap_begin)
void txs_snap_begin(struct txs_snap *s)
{
	uint32_t q = atomic_load_explicit(&s->seq, memory_order_relaxed);

	atomic_store_explicit(&s->seq, q + 1U, memory_order_relaxed);
	atomic_thread_fence(memory_order_release);
}

ESP_SDR_CODE(PACED_CHUNK, txs_snap_put)
void txs_snap_put(struct txs_snap *s, const struct txs_status *st)
{
	uint8_t raw[TXS_STATUS_BYTES];

	txs_status_put(raw, st);
	for (uint32_t i = 0; i < TXS_STATUS_BYTES / 4U; i++) {
		atomic_store_explicit(&s->w[i], get32(raw + 4U * i), memory_order_relaxed);
	}
}

ESP_SDR_CODE(PACED_CHUNK, txs_snap_end)
void txs_snap_end(struct txs_snap *s)
{
	uint32_t q = atomic_load_explicit(&s->seq, memory_order_relaxed);

	atomic_store_explicit(&s->seq, q + 1U, memory_order_release);
}

ESP_SDR_CODE(PACED_CHUNK, txs_snap_read_raw)
bool txs_snap_read_raw(const struct txs_snap *s, uint8_t raw[TXS_STATUS_BYTES])
{
	uint32_t q0 = atomic_load_explicit(&s->seq, memory_order_acquire);

	if ((q0 & 1U) != 0U) {
		return false;
	}
	for (uint32_t i = 0; i < TXS_STATUS_BYTES / 4U; i++) {
		put32(raw + 4U * i, atomic_load_explicit(&s->w[i], memory_order_relaxed));
	}
	atomic_thread_fence(memory_order_acquire);
	return atomic_load_explicit(&s->seq, memory_order_relaxed) == q0;
}

bool txs_snap_read(const struct txs_snap *s, struct txs_status *st)
{
	uint8_t raw[TXS_STATUS_BYTES];

	return txs_snap_read_raw(s, raw) && txs_status_get(st, raw, sizeof(raw)) == 0;
}
