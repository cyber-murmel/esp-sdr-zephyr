/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit test of apps/ubertooth/src/ubt_le.c (BLE packet finder, promiscuous helpers) and
 * le_conn.c (connection follower). The golden vectors in le_vectors.h come from the
 * independent Python model apps/ubertooth/tools/le_ref.py (regenerate with --emit-c); the C
 * model below is bit serial straight from the specification text and is checked against
 * those vectors before it builds the random streams. Runs on native_sim.
 */
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <test_rng.h>

#include "le_conn.h"
#include "ubt_le.h"
/* the vendored upstream code, only to cross-check against: it defines u8 and friends */
#include "upstream/bluetooth_le.h"

#include "aa_oracle.h"
#include "le_vectors.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* ---- random numbers (tests/common/include/test_rng.h) ---- */

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);
	test_rng_seed(0x9e3779b97f4a7c15ULL);
}

ZTEST_SUITE(ubertooth_le, NULL, NULL, before_each, NULL, NULL);

/* ---- bit serial model of the specification (independent of the code under test) ---- */

/* Whitening LFSR x^7 + x^4 + 1: position 0 = 1, positions 1..6 the channel index MSB first. */
static void ref_whiten(unsigned int idx, uint8_t *bits, unsigned int n)
{
	uint8_t s[7];
	unsigned int k;

	s[0] = 1;
	for (k = 0; k < 6; k++) {
		s[1 + k] = (idx >> (5 - k)) & 1;
	}
	for (k = 0; k < n; k++) {
		uint8_t o = s[6];

		bits[k] = o;
		s[6] = s[5];
		s[5] = s[4];
		s[4] = s[3] ^ o;
		s[3] = s[2];
		s[2] = s[1];
		s[1] = s[0];
		s[0] = o;
	}
}

/* CRC-24, polynomial 0x00065B, register MSB first, data LSB first, init = CRCInit. */
static uint32_t ref_crc(uint32_t init, const uint8_t *pdu, unsigned int len)
{
	uint32_t r = init & 0xffffff;
	unsigned int i, j;

	for (i = 0; i < len; i++) {
		for (j = 0; j < 8; j++) {
			uint32_t fb = ((r >> 23) & 1) ^ ((pdu[i] >> j) & 1);

			r = (r << 1) & 0xffffff;
			if (fb) {
				r ^= 0x00065B;
			}
		}
	}
	return r;
}

static void ref_crc_bytes(uint32_t r, uint8_t out[3])
{
	unsigned int k, j;

	memset(out, 0, 3);
	for (k = 0; k < 24; k++) { /* register bit 23 goes first, each byte is sent LSB first */
		j = k / 8;
		out[j] |= (uint8_t)(((r >> (23 - k)) & 1) << (k % 8));
	}
}

/* Chip index (0..39) of a frequency. */
static unsigned int ref_idx(unsigned int mhz)
{
	unsigned int k;

	if (mhz == 2402) {
		return 37;
	}
	if (mhz == 2426) {
		return 38;
	}
	if (mhz == 2480) {
		return 39;
	}
	k = (mhz - 2402) / 2;
	return k < 12 ? k - 1 : k - 2;
}

static unsigned int ref_mhz(unsigned int idx)
{
	if (idx == 37) {
		return 2402;
	}
	if (idx == 38) {
		return 2426;
	}
	if (idx == 39) {
		return 2480;
	}
	return idx < 11 ? 2404 + 2 * idx : 2428 + 2 * (idx - 11);
}

#define MAX_BITS 3200

/* On-air bits of a packet: preamble (optional), AA, whitened PDU and CRC. Returns the count. */
static unsigned int ref_air(uint8_t *bits, uint32_t aa, unsigned int idx, const uint8_t *pdu,
			    unsigned int len, uint32_t crc_init, bool preamble)
{
	uint8_t body[300], w[8 * 300];
	unsigned int n = 0, i, j;
	uint8_t pre = (aa & 1) ? 0x55 : 0xaa;

	memcpy(body, pdu, len);
	ref_crc_bytes(ref_crc(crc_init, pdu, len), body + len);
	if (preamble) {
		for (j = 0; j < 8; j++) {
			bits[n++] = (pre >> j) & 1;
		}
	}
	for (i = 0; i < 32; i++) {
		bits[n++] = (aa >> i) & 1;
	}
	ref_whiten(idx, w, 8 * (len + 3));
	for (i = 0; i < len + 3; i++) {
		for (j = 0; j < 8; j++) {
			bits[n++] = ((body[i] >> j) & 1) ^ w[8 * i + j];
		}
	}
	return n;
}

static void pack_bits(const uint8_t *bits, unsigned int n, uint8_t *out)
{
	unsigned int i;

	memset(out, 0, (n + 7) / 8);
	for (i = 0; i < n; i++) {
		out[i / 8] |= (uint8_t)(bits[i] << (i % 8));
	}
}

/* ---- symbol streams ---- */

#define MAX_SYMS 24000

static struct gfsk_sym syms[MAX_SYMS];

struct stream {
	struct gfsk_sym *s;
	size_t n;
	uint32_t t;
	uint8_t pwr;
};

static void st_init(struct stream *st, uint32_t t0)
{
	st->s = syms;
	st->n = 0;
	st->t = t0;
	st->pwr = 100;
}

static void st_bit(struct stream *st, unsigned int bit)
{
	zassert_true(st->n < MAX_SYMS, "symbol buffer");
	st->s[st->n].t = st->t;
	st->s[st->n].bit = (uint8_t)bit;
	st->s[st->n].pwr = st->pwr;
	st->n++;
	st->t += 4;
}

static void st_noise(struct stream *st, unsigned int n)
{
	while (n--) {
		st_bit(st, test_rng_u32() & 1);
	}
}

static void st_bits(struct stream *st, const uint8_t *bits, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++) {
		st_bit(st, bits[i]);
	}
}

/* ---- capture of what the finder emits ---- */

#define MAX_CAP 64

static struct cap {
	usb_pkt_rx pk[MAX_CAP];
	struct ubt_le_info info[MAX_CAP];
	unsigned int n;
	uint32_t clk_arg[MAX_CAP];
	unsigned int nclk;
	struct ubt_le *le;
} cap;

static void cap_emit(void *ctx, const usb_pkt_rx *p)
{
	ARG_UNUSED(ctx);
	if (cap.n < MAX_CAP) {
		cap.pk[cap.n] = *p;
		if (cap.le != NULL) {
			cap.info[cap.n] = cap.le->info;
		}
		cap.n++;
	}
}

static uint32_t cap_clk(void *ctx, uint32_t t)
{
	ARG_UNUSED(ctx);
	if (cap.nclk < MAX_CAP) {
		cap.clk_arg[cap.nclk++] = t;
	}
	return (uint32_t)(((uint64_t)t * 5) / 2 % UBT_CLK100NS_WRAP);
}

/* raw RSSI = power count - 100: a mapping unlike the default */
static int8_t cap_rssi(uint8_t pwr)
{
	return (int8_t)((int)pwr - 100);
}

static struct ubt_le le;

static void le_setup(uint16_t mhz, uint32_t aa, uint32_t crc_init)
{
	memset(&cap, 0, sizeof(cap));
	cap.le = &le;
	ubt_le_init(&le, cap_emit, cap_clk, cap_rssi, NULL);
	zassert_equal(ubt_le_set_channel(&le, mhz), 0);
	ubt_le_set_aa(&le, aa);
	ubt_le_set_crc_init(&le, crc_init);
}

/* Feed the stream in chunks of the given size (0: all at once). */
static size_t feed(struct ubt_le *l, const struct stream *st, size_t chunk)
{
	size_t i = 0, got = 0;

	if (chunk == 0) {
		return ubt_le_feed(l, st->s, st->n);
	}
	while (i < st->n) {
		size_t m = st->n - i < chunk ? st->n - i : chunk;

		got += ubt_le_feed(l, st->s + i, m);
		i += m;
	}
	return got;
}

/* A packet on air in a noise bed: lead noise, the packet, tail noise. Returns the t of the first
 * preamble symbol. */
static uint32_t st_packet(struct stream *st, uint32_t aa, unsigned int mhz, const uint8_t *pdu,
			  unsigned int len, uint32_t crc_init, unsigned int lead, unsigned int tail)
{
	static uint8_t bits[MAX_BITS];
	unsigned int n = ref_air(bits, aa, ref_idx(mhz), pdu, len, crc_init, true);
	uint32_t t_pre;

	st_noise(st, lead);
	t_pre = st->t;
	st_bits(st, bits, n);
	st_noise(st, tail);
	return t_pre;
}

/* ---- tests of the models and helpers ---- */

ZTEST(ubertooth_le, test_ref_model_matches_python_vectors)
{
	static uint8_t bits[MAX_BITS];
	uint8_t air[400];
	unsigned int i;

	for (i = 0; i < ARRAY_LEN(le_vecs); i++) {
		const struct le_vec *v = &le_vecs[i];
		unsigned int n = ref_air(bits, v->aa, ref_idx(v->mhz), v->pdu, v->pdu_len,
					 v->crc_init, true);

		zassert_equal(n, 8u * v->air_len, "%s", v->name);
		pack_bits(bits, n, air);
		zassert_mem_equal(air, v->air, v->air_len, "%s", v->name);
		zassert_equal(ref_idx(v->mhz), ref_idx(ref_mhz(ref_idx(v->mhz))));
	}
}

ZTEST(ubertooth_le, test_upstream_conventions)
{
	unsigned int i, j;

	/* CRC: the table CRC with the bit reversed init gives the wire bytes of the model */
	for (i = 0; i < ARRAY_LEN(le_vecs); i++) {
		const struct le_vec *v = &le_vecs[i];
		uint8_t pdu[300], crc[3];
		uint32_t st;

		memcpy(pdu, v->pdu, v->pdu_len);
		st = btle_crcgen_lut(ubt_rev24(v->crc_init), pdu, v->pdu_len);
		ref_crc_bytes(ref_crc(v->crc_init, v->pdu, v->pdu_len), crc);
		zassert_equal(st & 0xff, crc[0]);
		zassert_equal((st >> 8) & 0xff, crc[1]);
		zassert_equal((st >> 16) & 0xff, crc[2]);
		/* running it backwards from the wire CRC recovers CRCInit */
		zassert_equal(btle_reverse_crc(st, pdu, v->pdu_len), v->crc_init, "%s", v->name);
	}
	/* le_dewhiten() takes the bytes as the CC2400 delivers them (bit reversed) */
	for (i = 0; i < ARRAY_LEN(le_vecs); i++) {
		const struct le_vec *v = &le_vecs[i];
		uint8_t raw[300], exp[300], crc[3];
		unsigned int body = v->pdu_len + 3;

		ref_crc_bytes(ref_crc(v->crc_init, v->pdu, v->pdu_len), crc);
		memcpy(exp, v->pdu, v->pdu_len);
		memcpy(exp + v->pdu_len, crc, 3);
		for (j = 0; j < body; j++) {
			uint8_t a = v->air[5 + j], r = 0;
			unsigned int b;

			for (b = 0; b < 8; b++) {
				r |= (uint8_t)(((a >> b) & 1) << (7 - b));
			}
			raw[j] = r;
		}
		le_dewhiten(raw, body, v->mhz);
		zassert_mem_equal(raw, exp, body, "%s", v->name);
	}
	/* channel index helpers */
	for (i = 0; i < 40; i++) {
		zassert_equal(btle_channel_index(ref_mhz(i)), i);
		zassert_equal(btle_channel_index_to_phys(i), ref_mhz(i));
		zassert_equal(ubt_le_chan_idx(ref_mhz(i)), i);
	}
	zassert_equal(ubt_le_chan_idx(2403), 40);
	zassert_equal(ubt_le_chan_idx(2400), 40);
	zassert_equal(ubt_le_chan_idx(2482), 40);
}

ZTEST(ubertooth_le, test_helpers)
{
	unsigned int i;

	for (i = 0; i < 2000; i++) {
		uint32_t v = test_rng_u32(), r = 0, r24 = 0;
		unsigned int pc = 0, b;

		for (b = 0; b < 32; b++) {
			pc += (v >> b) & 1;
			r |= ((v >> b) & 1) << (31 - b);
		}
		for (b = 0; b < 24; b++) {
			r24 |= ((v >> b) & 1) << (23 - b);
		}
		zassert_equal(ubt_popcount32(v), pc);
		zassert_equal(ubt_rev32(v), r);
		zassert_equal(ubt_rev24(v), r24);
	}
	zassert_equal(ubt_rev24(0x555555), 0xaaaaaa);
	for (i = 0; i < ARRAY_LEN(aa_oracle); i++) {
		zassert_equal(ubt_le_aa_valid(aa_oracle[i].aa), aa_oracle[i].valid, "AA %08x",
			      aa_oracle[i].aa);
	}
}

ZTEST(ubertooth_le, test_clk_unwrap)
{
	struct ubt_clk_unwrap u = {0};

	zassert_equal(ubt_clk_unwrap(&u, 100), 100);
	zassert_equal(ubt_clk_unwrap(&u, 3000000000U), 3000000000ULL);
	zassert_equal(ubt_clk_unwrap(&u, 1000), 3276800000ULL + 1000);
	zassert_equal(ubt_clk_unwrap(&u, 2000), 3276800000ULL + 2000);
	zassert_equal(ubt_clk_unwrap(&u, 3276799999U), 3276800000ULL + 3276799999U);
	zassert_equal(ubt_clk_unwrap(&u, 5), 2 * 3276800000ULL + 5);
}

/* ---- the finder ---- */

ZTEST(ubertooth_le, test_golden_vectors)
{
	unsigned int i, tol;

	for (tol = 0; tol <= 2; tol++) {
		for (i = 0; i < ARRAY_LEN(le_vecs); i++) {
			const struct le_vec *v = &le_vecs[i];
			struct stream st;
			uint32_t t0 = test_rng_u32(), t_pre, exp_start;
			unsigned int lead = 33 + test_rng_u32() % 120;
			const usb_pkt_rx *p = &cap.pk[0];

			le_setup(v->mhz, v->aa, v->crc_init);
			zassert_equal(ubt_le_set_tolerance(&le, tol), 0);
			st_init(&st, t0);
			t_pre = st_packet(&st, v->aa, v->mhz, v->pdu, v->pdu_len, v->crc_init, lead,
					  50);
			zassert_equal(feed(&le, &st, 0), 1, "%s tol %u", v->name, tol);
			zassert_equal(cap.n, 1);
			/* the preamble's first symbol is decided in mid symbol */
			exp_start = t_pre - 2;
			zassert_mem_equal(p->data, v->usb, DMA_SIZE, "%s", v->name);
			zassert_equal(p->pkt_type, LE_PACKET);
			zassert_equal(p->status, 0);
			zassert_equal(p->channel, v->mhz - 2402);
			zassert_equal(p->clkn_high, 0);
			zassert_equal(p->rssi_count, 0);
			zassert_equal(cap.nclk, 1);
			zassert_equal(cap.clk_arg[0], exp_start, "%s start", v->name);
			zassert_equal(p->clk100ns,
				      (uint32_t)((uint64_t)exp_start * 5 / 2 % UBT_CLK100NS_WRAP));
			zassert_true(cap.info[0].crc_ok, "%s", v->name);
			zassert_equal(cap.info[0].t_start, exp_start);
			zassert_equal(cap.info[0].aa_rx, v->aa);
			zassert_equal(cap.info[0].pdu_len, v->pdu_len);
			zassert_equal(le.stats.hits, 1);
			zassert_equal(le.stats.crc_bad, 0);
			zassert_false(ubt_le_busy(&le));
		}
	}
}

ZTEST(ubertooth_le, test_rssi_statistics)
{
	const struct le_vec *v = &le_vecs[0];
	static uint8_t bits[MAX_BITS];
	struct stream st;
	unsigned int n =
		ref_air(bits, v->aa, ref_idx(v->mhz), v->pdu, v->pdu_len, v->crc_init, true);
	unsigned int i, nbytes = v->pdu_len + 3;
	uint32_t sum = 0;

	le_setup(v->mhz, v->aa, v->crc_init);
	st_init(&st, 1000);
	st_noise(&st, 40);
	/* preamble and AA at 90, then byte b of the PDU and CRC at 100 + b */
	for (i = 0; i < n; i++) {
		st.pwr = i < 40 ? 90 : (uint8_t)(100 + (i - 40) / 8);
		st_bit(&st, bits[i]);
	}
	st.pwr = 20;
	st_noise(&st, 30);
	zassert_equal(feed(&le, &st, 7), 1);
	for (i = 0; i < nbytes; i++) {
		sum += 100 + i;
	}
	zassert_equal(cap.pk[0].rssi_min, 0);
	zassert_equal(cap.pk[0].rssi_max, nbytes - 1);
	zassert_equal(cap.pk[0].rssi_avg, (int)(sum / nbytes) - 100);
	/* without callbacks: raw = dBFS + 54 = pwr / 2 - 46 */
	memset(&cap, 0, sizeof(cap));
	cap.le = &le;
	ubt_le_init(&le, cap_emit, NULL, NULL, NULL);
	ubt_le_set_channel(&le, v->mhz);
	ubt_le_set_aa(&le, v->aa);
	ubt_le_set_crc_init(&le, v->crc_init);
	zassert_equal(feed(&le, &st, 0), 1);
	zassert_equal(cap.pk[0].rssi_min, 100 / 2 - 46);
	zassert_equal(cap.pk[0].rssi_max, (100 + (int)nbytes - 1) / 2 - 46);
	zassert_equal(cap.pk[0].clk100ns, (uint32_t)((uint64_t)cap.info[0].t_start * 5 / 2));
}

ZTEST(ubertooth_le, test_crc_verify)
{
	const struct le_vec *v = &le_vecs[1];
	static uint8_t bits[MAX_BITS];
	unsigned int n =
		ref_air(bits, v->aa, ref_idx(v->mhz), v->pdu, v->pdu_len, v->crc_init, true);
	unsigned int flip, verify;

	for (flip = 40; flip < n; flip += 7) {
		for (verify = 0; verify < 2; verify++) {
			struct stream st;
			uint8_t exp[DMA_SIZE];

			le_setup(v->mhz, v->aa, v->crc_init);
			ubt_le_set_crc_verify(&le, verify);
			st_init(&st, 77);
			st_noise(&st, 50);
			bits[flip] ^= 1;
			st_bits(&st, bits, n);
			bits[flip] ^= 1;
			st_noise(&st, 50);
			if (flip >= 48 && flip < 56) {
				/* a flipped length bit changes where the packet ends: only check
				 * that nothing breaks */
				(void)feed(&le, &st, 0);
				continue;
			}
			zassert_equal(feed(&le, &st, 0), verify ? 0 : 1, "flip %u", flip);
			zassert_equal(le.stats.crc_bad, 1);
			zassert_equal(le.stats.packets, 1);
			if (!verify) {
				unsigned int byte = (flip - 40) / 8;

				memcpy(exp, v->usb, DMA_SIZE);
				exp[4 + byte] ^= (uint8_t)(1u << ((flip - 40) % 8));
				zassert_mem_equal(cap.pk[0].data, exp, DMA_SIZE, "flip %u", flip);
				zassert_false(cap.info[0].crc_ok);
			}
		}
	}
}

ZTEST(ubertooth_le, test_aa_tolerance)
{
	const struct le_vec *v = &le_vecs[0];
	static uint8_t bits[MAX_BITS];
	unsigned int n =
		ref_air(bits, v->aa, ref_idx(v->mhz), v->pdu, v->pdu_len, v->crc_init, true);
	unsigned int errs, tol, trial;

	for (errs = 0; errs <= 3; errs++) {
		for (tol = 0; tol <= 2; tol++) {
			for (trial = 0; trial < 12; trial++) {
				struct stream st;
				uint32_t aa_rx, mask = 0;
				unsigned int k;
				bool exp = errs <= tol;

				le_setup(v->mhz, v->aa, v->crc_init);
				zassert_equal(ubt_le_set_tolerance(&le, tol), 0);
				/* distinct bit errors in the AA; trials 0 and 1 take the first and
				 * the last bits */
				while (ubt_popcount32(mask) < errs) {
					unsigned int e = ubt_popcount32(mask);

					mask |= 1u << (trial == 0 ? e
								  : (trial == 1 ? 31 - e
										: test_rng_u32() % 32));
				}
				aa_rx = v->aa ^ mask;
				for (k = 0; k < 32; k++) {
					bits[8 + k] = (aa_rx >> k) & 1;
				}
				st_init(&st, 5);
				st_noise(&st, 60);
				st_bits(&st, bits, n);
				st_noise(&st, 40);
				zassert_equal(feed(&le, &st, 11), exp ? 1 : 0,
					      "errs %u tol %u trial %u", errs, tol, trial);
				if (exp) {
					/* the reported AA is the configured one, the received one
					 * is in info */
					zassert_mem_equal(cap.pk[0].data, v->usb, DMA_SIZE);
					zassert_equal(cap.info[0].aa_rx, aa_rx);
				}
				for (k = 0; k < 32; k++) {
					bits[8 + k] = (v->aa >> k) & 1;
				}
			}
		}
	}
	zassert_equal(ubt_le_set_tolerance(&le, 3), -1);
	zassert_equal(le.tolerance, 2);
}

/* ---- target filter ---- */

/* The control request's MAC (most significant octet first) of an address as it is on air (least
 * significant first). */
static void air_to_mac(const uint8_t *air, uint8_t mac[6])
{
	unsigned int i;

	for (i = 0; i < 6; i++) {
		mac[i] = air[5 - i];
	}
}

/* Independent model: the first m bits of the two MACs agree. */
static bool prefix_equal(const uint8_t a[6], const uint8_t b[6], unsigned int m)
{
	unsigned int k;

	for (k = 0; k < m; k++) {
		if (((a[k / 8] ^ b[k / 8]) >> (7 - k % 8)) & 1) {
			return false;
		}
	}
	return true;
}

ZTEST(ubertooth_le, test_target_filter)
{
	unsigned int i, m, k;

	for (i = 0; i < ARRAY_LEN(le_vecs); i++) {
		const struct le_vec *v = &le_vecs[i];
		unsigned int type = v->pdu[0] & 0xf;
		bool adv = ref_idx(v->mhz) >= 37;
		uint8_t mac[6], first[6], second[6];
		bool two = type == 1 || type == 3 || type == 5;

		memset(first, 0, 6);
		memset(second, 0, 6);
		if (adv) {
			air_to_mac(&v->pdu[2], first);
		}
		if (adv && two) {
			air_to_mac(&v->pdu[8], second);
		}
		for (k = 0; k < 49;
		     k += 3) { /* bit k of the target address differs from the first address */
			memcpy(mac, first, 6);
			if (k < 48) {
				mac[k / 8] ^= (uint8_t)(0x80 >> (k % 8));
			}
			for (m = 0; m <= 48; m++) {
				struct stream st;
				bool pass;

				le_setup(v->mhz, v->aa, v->crc_init);
				zassert_equal(ubt_le_set_target(&le, mac, m), 0);
				st_init(&st, 1);
				st_packet(&st, v->aa, v->mhz, v->pdu, v->pdu_len, v->crc_init, 40,
					  40);
				if (!adv || m == 0) {
					pass = true;
				} else if (type == 0 || type == 2 || type == 4 || type == 6) {
					pass = prefix_equal(mac, first, m);
				} else if (two) {
					pass = prefix_equal(mac, first, m) ||
					       prefix_equal(mac, second, m);
				} else {
					pass = false;
				}
				zassert_equal(feed(&le, &st, 0), pass ? 1 : 0, "%s k %u m %u",
					      v->name, k, m);
				if (!pass) {
					zassert_equal(le.stats.filtered, 1);
				}
			}
		}
	}
	/* the filter in on-air order (what ubt_ctrl keeps) behaves like the request form */
	{
		const struct le_vec *v = &le_vecs[0];
		uint8_t mac[6] = {0xc6, 0x55, 0x44, 0x33, 0x22, 0x11}, addr[6], mask[6];
		struct stream st;
		unsigned int m;

		for (m = 0; m <= 48; m += 5) {
			le_setup(v->mhz, v->aa, v->crc_init);
			zassert_equal(ubt_le_set_target(&le, mac, m), 0);
			memcpy(addr, le.target, 6);
			memcpy(mask, le.target_mask, 6);
			le_setup(v->mhz, v->aa, v->crc_init);
			ubt_le_set_target_raw(&le, m != 0, addr, mask);
			st_init(&st, 1);
			st_packet(&st, v->aa, v->mhz, v->pdu, v->pdu_len, v->crc_init, 40, 40);
			zassert_equal(feed(&le, &st, 0), 1, "mask %u", m);
			/* a wrong address in the masked part is dropped */
			addr[5] ^= 0x80;
			ubt_le_set_target_raw(&le, true, addr, mask);
			st_init(&st, 1);
			st_packet(&st, v->aa, v->mhz, v->pdu, v->pdu_len, v->crc_init, 40, 40);
			zassert_equal(feed(&le, &st, 0), (m == 0) ? 1 : 0, "mask %u", m);
		}
	}
	/* limits */
	{
		uint8_t mac[6] = {1, 2, 3, 4, 5, 6};

		le_setup(2402, UBT_LE_ADV_AA, UBT_LE_ADV_CRC_INIT);
		zassert_equal(ubt_le_set_target(&le, mac, 49), -1);
		zassert_false(le.target_set);
		zassert_equal(ubt_le_set_target(&le, mac, 48), 0);
		zassert_true(le.target_set);
		zassert_equal(ubt_le_set_target(&le, mac, 0), 0);
		zassert_false(le.target_set);
	}
}

ZTEST(ubertooth_le, test_target_filter_unknown_types_drop)
{
	/* an extended advertising PDU (type 7) has no address at a fixed place: dropped when a
	 * target is set */
	uint8_t pdu[] = {0x07, 8, 1, 2, 3, 4, 5, 6, 7, 8};
	uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
	struct stream st;

	le_setup(2426, UBT_LE_ADV_AA, UBT_LE_ADV_CRC_INIT);
	st_init(&st, 1);
	st_packet(&st, UBT_LE_ADV_AA, 2426, pdu, sizeof(pdu), UBT_LE_ADV_CRC_INIT, 40, 40);
	zassert_equal(feed(&le, &st, 0), 1);
	ubt_le_set_target(&le, mac, 1);
	st_init(&st, 1);
	st_packet(&st, UBT_LE_ADV_AA, 2426, pdu, sizeof(pdu), UBT_LE_ADV_CRC_INIT, 40, 40);
	zassert_equal(feed(&le, &st, 0), 0);
}

/* ---- lengths ---- */

ZTEST(ubertooth_le, test_truncation_to_the_usb_packet)
{
	unsigned int len;

	for (len = 36; len <= 46; len++) {
		uint8_t pdu[300];
		uint8_t crc[3];
		struct stream st;
		unsigned int i, total = 2 + len + 3;
		uint8_t exp[64];

		pdu[0] = 0x02;
		pdu[1] = (uint8_t)len;
		for (i = 0; i < len; i++) {
			pdu[2 + i] = (uint8_t)(test_rng_u32());
		}
		ref_crc_bytes(ref_crc(0xabcdef, pdu, 2 + len), crc);
		le_setup(2436, 0x8f1e3a71, 0xabcdef);
		st_init(&st, 9);
		st_packet(&st, 0x8f1e3a71, 2436, pdu, 2 + len, 0xabcdef, 60, 60);
		zassert_equal(feed(&le, &st, 13), 1, "len %u", len);
		memset(exp, 0, sizeof(exp));
		exp[0] = 0x71;
		exp[1] = 0x3a;
		exp[2] = 0x1e;
		exp[3] = 0x8f;
		memcpy(&exp[4], pdu, 2 + len);
		memcpy(&exp[4 + 2 + len], crc, 3);
		zassert_mem_equal(cap.pk[0].data, exp, DMA_SIZE, "len %u", len);
		zassert_equal(cap.info[0].pdu_len, 2 + len);
		zassert_true(cap.info[0].crc_ok);
		zassert_equal(le.stats.truncated, total > UBT_LE_USB_BODY ? 1 : 0, "len %u", len);
	}
}

ZTEST(ubertooth_le, test_length_bounds)
{
	uint8_t pdu[300];
	struct stream st;
	unsigned int i;

	/* data channels take 255 byte payloads, advertising channels stop at 64 */
	pdu[0] = 0x02;
	pdu[1] = 255;
	for (i = 0; i < 255; i++) {
		pdu[2 + i] = (uint8_t)test_rng_u32();
	}
	le_setup(2440, 0x12345678, 0x111111);
	st_init(&st, 1);
	st_packet(&st, 0x12345678, 2440, pdu, 257, 0x111111, 50, 50);
	zassert_equal(feed(&le, &st, 0), 1);
	zassert_true(cap.info[0].crc_ok);
	zassert_equal(cap.info[0].pdu_len, 257);
	pdu[0] = 0x00;
	pdu[1] = 65;
	le_setup(2402, UBT_LE_ADV_AA, UBT_LE_ADV_CRC_INIT);
	st_init(&st, 1);
	st_packet(&st, UBT_LE_ADV_AA, 2402, pdu, 67, UBT_LE_ADV_CRC_INIT, 50, 50);
	zassert_equal(feed(&le, &st, 0), 0);
	zassert_equal(le.stats.len_rejected, 1);
	pdu[1] = 64;
	le_setup(2402, UBT_LE_ADV_AA, UBT_LE_ADV_CRC_INIT);
	st_init(&st, 1);
	st_packet(&st, UBT_LE_ADV_AA, 2402, pdu, 66, UBT_LE_ADV_CRC_INIT, 50, 50);
	zassert_equal(feed(&le, &st, 0), 1);
	/* the bounds are settable */
	le_setup(2402, UBT_LE_ADV_AA, UBT_LE_ADV_CRC_INIT);
	ubt_le_set_max_len(&le, 37, 27);
	st_init(&st, 1);
	st_packet(&st, UBT_LE_ADV_AA, 2402, pdu, 66, UBT_LE_ADV_CRC_INIT, 50, 50);
	zassert_equal(feed(&le, &st, 0), 0);
}

/* ---- noise, false hits, back to back packets, gaps ---- */

ZTEST(ubertooth_le, test_noise_and_false_hits)
{
	unsigned int tol, chunk;

	for (tol = 0; tol <= 2; tol++) {
		struct stream st;
		unsigned int round;
		uint32_t base = 0;

		le_setup(2402, UBT_LE_ADV_AA, UBT_LE_ADV_CRC_INIT);
		ubt_le_set_tolerance(&le, tol);
		for (round = 0; round < 20; round++) {
			st_init(&st, base);
			st_noise(&st, 10000);
			base = st.t;
			chunk = 1 + test_rng_u32() % 300;
			feed(&le, &st, chunk);
		}
		/* 200000 random symbols hit a tolerance 0 correlator with probability 5e-5 */
		if (tol == 0) {
			zassert_equal(le.stats.hits, 0);
		}
		zassert_true(le.stats.hits <= 3, "tol %u hits %u", tol,
			     (unsigned int)le.stats.hits);
		zassert_equal(le.stats.gaps, 0);
	}
	/* a stream of nothing but zeros or alternating bits never matches the advertising AA */
	{
		struct stream st;
		unsigned int i;

		le_setup(2402, UBT_LE_ADV_AA, UBT_LE_ADV_CRC_INIT);
		ubt_le_set_tolerance(&le, 2);
		st_init(&st, 0);
		for (i = 0; i < 4000; i++) {
			st_bit(&st, i & 1);
		}
		for (i = 0; i < 4000; i++) {
			st_bit(&st, 0);
		}
		feed(&le, &st, 0);
		zassert_equal(le.stats.hits, 0);
	}
}

ZTEST(ubertooth_le, test_false_hit_does_not_hide_the_next_packet)
{
	const struct le_vec *v = &le_vecs[0];
	static uint8_t bits[MAX_BITS], w[64];
	struct stream st;
	unsigned int n_real =
		ref_air(bits, v->aa, ref_idx(v->mhz), v->pdu, v->pdu_len, v->crc_init, true);
	unsigned int cut, i;

	/* (a) a false hit whose length field is out of bounds ends after 16 bits, the packet
	 * follows at once */
	for (cut = 0; cut < 40; cut += 9) {
		static uint8_t junk[64];
		unsigned int j = 0;
		uint8_t hdr[2] = {0x40, 200};

		le_setup(v->mhz, v->aa, v->crc_init);
		/* AA, then a header whitened to read 0x40 200 */
		for (i = 0; i < 32; i++) {
			junk[j++] = (v->aa >> i) & 1;
		}
		ref_whiten(ref_idx(v->mhz), w, 16);
		for (i = 0; i < 16; i++) {
			junk[j++] = ((hdr[i / 8] >> (i % 8)) & 1) ^ w[i];
		}
		st_init(&st, 3);
		st_noise(&st, 50);
		st_bits(&st, junk, j - (cut % 4)); /* sometimes a few header bits are missing */
		st_noise(&st, cut);
		st_bits(&st, bits, n_real);
		st_noise(&st, 50);
		(void)feed(&le, &st, 0);
		zassert_true(cap.n >= 1, "cut %u", cut);
		zassert_mem_equal(cap.pk[cap.n - 1].data, v->usb, DMA_SIZE);
	}
	/* (b) the first packet claims 100 bytes but the next packet starts after a few: the next
	 * one wins */
	v = &le_vecs[7]; /* data channel, 27 byte payload */
	n_real = ref_air(bits, v->aa, ref_idx(v->mhz), v->pdu, v->pdu_len, v->crc_init, true);
	for (cut = 4; cut < 24; cut += 5) {
		uint8_t pdu1[102];
		static uint8_t b1[MAX_BITS];

		pdu1[0] = 0x02;
		pdu1[1] = 100;
		for (i = 0; i < 100; i++) {
			pdu1[2 + i] = (uint8_t)test_rng_u32();
		}
		le_setup(v->mhz, v->aa, v->crc_init);
		(void)ref_air(b1, v->aa, ref_idx(v->mhz), pdu1, 102, v->crc_init, true);
		st_init(&st, 3);
		st_noise(&st, 50);
		st_bits(&st, b1, 40 + 8 * cut);
		st_bits(&st, bits, n_real);
		st_noise(&st, 50);
		zassert_equal(feed(&le, &st, 17), 1, "cut %u", cut);
		zassert_mem_equal(cap.pk[0].data, v->usb, DMA_SIZE);
		zassert_equal(le.stats.restarts, 1);
		zassert_equal(le.stats.hits, 2);
	}
}

ZTEST(ubertooth_le, test_back_to_back_packets)
{
	static uint8_t bits[MAX_BITS];
	unsigned int gap;

	for (gap = 0; gap <= 150; gap += 25) {
		struct stream st;
		unsigned int i, b = 0;
		const struct le_vec *v1 = &le_vecs[b], *v2 = &le_vecs[4];
		unsigned int n;
		uint32_t t1, t2;

		/* both on channel 37 with the advertising AA */
		le_setup(2402, UBT_LE_ADV_AA, UBT_LE_ADV_CRC_INIT);
		st_init(&st, 1000);
		st_noise(&st, 30);
		t1 = st.t;
		n = ref_air(bits, v1->aa, 37, v1->pdu, v1->pdu_len, v1->crc_init, true);
		st_bits(&st, bits, n);
		st_noise(&st, gap);
		t2 = st.t;
		n = ref_air(bits, v2->aa, 37, v2->pdu, v2->pdu_len, v2->crc_init, true);
		st_bits(&st, bits, n);
		st_noise(&st, 30);
		for (i = 1; i < 40; i += 6) { /* any chunking */
			memset(&cap, 0, sizeof(cap));
			cap.le = &le;
			le.stats = (struct ubt_le_stats){0};
			ubt_le_reset(&le);
			zassert_equal(feed(&le, &st, i), 2, "gap %u chunk %u", gap, i);
			zassert_mem_equal(cap.pk[0].data, v1->usb, DMA_SIZE);
			zassert_mem_equal(cap.pk[1].data, v2->usb, DMA_SIZE);
			zassert_equal(cap.info[0].t_start, t1 - 2);
			zassert_equal(cap.info[1].t_start, t2 - 2);
		}
	}
}

ZTEST(ubertooth_le, test_gaps_in_the_stream)
{
	const struct le_vec *v = &le_vecs[0];
	static uint8_t bits[MAX_BITS];
	struct stream st;
	unsigned int n =
		ref_air(bits, v->aa, ref_idx(v->mhz), v->pdu, v->pdu_len, v->crc_init, true);
	unsigned int cut;

	/* (a) the squelch closes in the middle of a packet: nothing comes out, the next packet is
	 * found */
	for (cut = 41; cut < n - 1; cut += 17) {
		le_setup(v->mhz, v->aa, v->crc_init);
		st_init(&st, 500);
		st_noise(&st, 40);
		st_bits(&st, bits, cut);
		st.t += 400; /* 100 symbols of silence */
		st_bits(&st, bits + cut,
			n - cut); /* the rest of the first packet: garbage to the finder */
		st.t += 400;
		/* a packet that starts right at the first symbol after the gap */
		st_bits(&st, bits, n);
		st_noise(&st, 20);
		zassert_equal(feed(&le, &st, 0), 1, "cut %u", cut);
		zassert_mem_equal(cap.pk[0].data, v->usb, DMA_SIZE);
		zassert_equal(le.stats.gaps, 2);
		zassert_equal(le.stats.aborted, 1, "cut %u", cut);
	}
	/* (b) the gap falls on a call boundary */
	le_setup(v->mhz, v->aa, v->crc_init);
	st_init(&st, 500);
	st_bits(&st, bits, 100);
	{
		size_t first = st.n;

		st.t += 1000;
		st_bits(&st, bits, n);
		zassert_equal(ubt_le_feed(&le, st.s, first), 0);
		zassert_equal(ubt_le_feed(&le, st.s + first, st.n - first), 1);
		zassert_equal(le.stats.gaps, 1);
	}
	/* (c) timing jitter of the symbol clock (3 to 5 samples per symbol) is not a gap */
	le_setup(v->mhz, v->aa, v->crc_init);
	st_init(&st, 500);
	{
		unsigned int i;
		uint32_t t = 5000;

		st_noise(&st, 20);
		t = st.t;
		for (i = 0; i < n; i++) {
			st.s[st.n].t = t;
			st.s[st.n].bit = bits[i];
			st.s[st.n].pwr = 100;
			st.n++;
			t += 3 + (test_rng_u32() % 3); /* 3, 4 or 5 */
		}
		st.t = t;
		st_noise(&st, 20);
		zassert_equal(feed(&le, &st, 0), 1);
		zassert_equal(le.stats.gaps, 0);
	}
	/* (d) the timestamp wraps at 2^32 */
	le_setup(v->mhz, v->aa, v->crc_init);
	st_init(&st, 0xffffffffU - 4 * 100);
	st_noise(&st, 30);
	st_bits(&st, bits, n);
	st_noise(&st, 20);
	zassert_equal(feed(&le, &st, 0), 1);
	zassert_equal(le.stats.gaps, 0);
}

ZTEST(ubertooth_le, test_chunking_is_irrelevant)
{
	static uint8_t bits[MAX_BITS];
	unsigned int tol, trial;

	for (tol = 0; tol <= 2; tol++) {
		for (trial = 0; trial < 6; trial++) {
			struct stream st;
			usb_pkt_rx ref[16];
			uint32_t ref_start[16];
			size_t nref, i;
			unsigned int k;

			st_init(&st, 12345);
			st.pwr = 130;
			/* a few packets of different channels' content are the same AA on channel
			 * 37 here */
			for (k = 0; k < 6; k++) {
				const struct le_vec *v = &le_vecs[k % 6];
				unsigned int n = ref_air(bits, UBT_LE_ADV_AA, 37, v->pdu,
							 v->pdu_len, UBT_LE_ADV_CRC_INIT, true);

				st_noise(&st, 1 + test_rng_u32() % 200);
				st_bits(&st, bits, n);
			}
			st_noise(&st, 100);
			le_setup(2402, UBT_LE_ADV_AA, UBT_LE_ADV_CRC_INIT);
			ubt_le_set_tolerance(&le, tol);
			zassert_equal(feed(&le, &st, 0), 6);
			nref = cap.n;
			for (i = 0; i < nref; i++) {
				ref[i] = cap.pk[i];
				ref_start[i] = cap.info[i].t_start;
			}
			le_setup(2402, UBT_LE_ADV_AA, UBT_LE_ADV_CRC_INIT);
			ubt_le_set_tolerance(&le, tol);
			zassert_equal(feed(&le, &st, 1 + test_rng_u32() % 70), 6, "tol %u", tol);
			for (i = 0; i < nref; i++) {
				zassert_mem_equal(&cap.pk[i], &ref[i], sizeof(usb_pkt_rx));
				zassert_equal(cap.info[i].t_start, ref_start[i]);
			}
			/* and one symbol at a time */
			le_setup(2402, UBT_LE_ADV_AA, UBT_LE_ADV_CRC_INIT);
			ubt_le_set_tolerance(&le, tol);
			zassert_equal(feed(&le, &st, 1), 6);
			for (i = 0; i < nref; i++) {
				zassert_mem_equal(&cap.pk[i], &ref[i], sizeof(usb_pkt_rx));
			}
		}
	}
}

ZTEST(ubertooth_le, test_channel_and_config)
{
	const struct le_vec *v = &le_vecs[0]; /* channel 37 */
	struct stream st;

	le_setup(v->mhz, v->aa, v->crc_init);
	st_init(&st, 1);
	st_packet(&st, v->aa, v->mhz, v->pdu, v->pdu_len, v->crc_init, 40, 40);
	/* a receiver on the wrong channel dewhitens garbage: the CRC fails */
	zassert_equal(ubt_le_set_channel(&le, 2426), 0);
	zassert_equal(feed(&le, &st, 0), 1);
	zassert_false(cap.info[0].crc_ok);
	zassert_equal(cap.pk[0].channel, 2426 - 2402);
	/* wrong CRCInit */
	le_setup(v->mhz, v->aa, 0x123456);
	zassert_equal(feed(&le, &st, 0), 1);
	zassert_false(cap.info[0].crc_ok);
	/* odd or out of band frequencies are refused and leave the channel alone */
	zassert_equal(ubt_le_set_channel(&le, 2403), -1);
	zassert_equal(ubt_le_set_channel(&le, 2500), -1);
	zassert_equal(le.mhz, 2402);
	/* AA change: the old AA is ignored */
	le_setup(v->mhz, 0x11223344, v->crc_init);
	zassert_equal(feed(&le, &st, 0), 0);
	/* a changed AA bit reverses the register target */
	le_setup(v->mhz, v->aa, v->crc_init);
	zassert_equal(feed(&le, &st, 0), 1);
	/* every channel whitens differently: all 40 round trip */
	{
		unsigned int idx;
		uint8_t pdu[] = {0x02, 5, 1, 2, 3, 4, 5};

		for (idx = 0; idx < 40; idx++) {
			le_setup(ref_mhz(idx), 0x4a5b6c7d, 0x13579b);
			st_init(&st, 1);
			st_packet(&st, 0x4a5b6c7d, ref_mhz(idx), pdu, sizeof(pdu), 0x13579b, 40,
				  40);
			zassert_equal(feed(&le, &st, 0), 1, "channel index %u", idx);
			zassert_true(cap.info[0].crc_ok, "channel index %u", idx);
			zassert_mem_equal(&cap.pk[0].data[4], pdu, sizeof(pdu));
		}
	}
}

/* ---- promiscuous mode ---- */

#define PM_CAP 4096

static struct pmcap {
	usb_pkt_rx pk[PM_CAP];
	unsigned int n;
	unsigned int msgs;
	uint8_t msg_kind[16];
	uint32_t msg_val[16];
	unsigned int aa_hits; /* candidate packets that carry the true AA */
	uint32_t true_aa;
} pmcap;

static struct ubt_promisc pm;

static void pm_emit(void *ctx, const usb_pkt_rx *p)
{
	ARG_UNUSED(ctx);
	if (p->pkt_type == LE_PROMISC) {
		zassert_true(pmcap.msgs < 16);
		pmcap.msg_kind[pmcap.msgs] = p->data[0];
		pmcap.msg_val[pmcap.msgs] = p->data[1] | (p->data[2] << 8) |
					    ((uint32_t)p->data[3] << 16) |
					    ((uint32_t)p->data[4] << 24);
		pmcap.msgs++;
	} else if (pmcap.n < PM_CAP) {
		uint32_t aa = p->data[0] | (p->data[1] << 8) | ((uint32_t)p->data[2] << 16) |
			      ((uint32_t)p->data[3] << 24);

		if (aa == pmcap.true_aa) {
			pmcap.aa_hits++;
		}
		pmcap.pk[pmcap.n++] = *p;
	}
}

static void pm_setup(uint32_t true_aa)
{
	memset(&pmcap, 0, sizeof(pmcap));
	pmcap.true_aa = true_aa;
	ubt_le_init(&le, NULL, NULL, NULL, NULL);
	ubt_promisc_init(&pm, &le, pm_emit, NULL);
}

/* A simulated connection with all 37 channels in use and empty PDUs in every event. */
struct simconn {
	uint32_t aa, crc_init;
	unsigned int interval; /* 1.25 ms units */
	unsigned int hop;
	uint64_t t0_us;    /* anchor of event 0 */
	unsigned int idx0; /* unmapped channel of event 0 is (idx0 + hop) mod 37 */
};

static unsigned int sim_idx(const struct simconn *c, uint32_t ev)
{
	return (unsigned int)((c->idx0 + (uint64_t)(ev + 1) * c->hop) % 37);
}

static uint64_t sim_anchor_us(const struct simconn *c, uint32_t ev)
{
	return c->t0_us + (uint64_t)ev * c->interval * 1250;
}

/* Header of an empty PDU with the NESN/SN bits taken from n. */
static uint8_t empty_hdr(unsigned int n)
{
	return (uint8_t)(0x01 | ((n & 1) << 2) | (((n >> 1) & 1) << 3));
}

/* One event on the air as symbols: noise, master's empty PDU, 150 us, slave's empty PDU, noise. */
static void st_event(struct stream *st, const struct simconn *c, uint32_t ev, unsigned int mhz)
{
	static uint8_t bits[MAX_BITS];
	uint8_t pdu[2] = {empty_hdr(ev), 0};
	unsigned int n, idx = ref_idx(mhz);
	uint32_t t_first = (uint32_t)(4 * sim_anchor_us(c, ev) + 2);

	st->s = syms;
	st->n = 0;
	st->t = t_first - 4 * 24;
	st_noise(st, 24);
	n = ref_air(bits, c->aa, idx, pdu, 2, c->crc_init, true);
	st_bits(st, bits, n);
	st_noise(st, 150);
	pdu[0] = empty_hdr(ev * 7 + 3);
	n = ref_air(bits, c->aa, idx, pdu, 2, c->crc_init, true);
	st_bits(st, bits, n);
	st_noise(st, 24);
}

struct radio {
	unsigned int mhz;
	uint64_t ready_us;
};

#define RETUNE_US 4000

/*
 * The glue loop of the tests: events are delivered when the radio is on their channel and
 * settled; the recovery's retune requests are honoured after the chunk that raised them (the
 * rest of the event is then lost, as it would be on the air).
 */
static uint32_t promisc_done_ev;

static void promisc_run(const struct simconn *c, unsigned int scan_mhz, uint32_t max_ev, bool noise,
			bool known_aa)
{
	struct radio radio = {scan_mhz, 0};
	struct stream st;
	uint32_t ev;

	pm_setup(c->aa);
	if (known_aa) {
		ubt_promisc_start_aa(&pm, scan_mhz, c->aa);
	} else {
		ubt_promisc_start(&pm, scan_mhz);
	}
	for (ev = 0; ev < max_ev && pm.phase != UBT_PM_DONE; ev++) {
		unsigned int mhz = ref_mhz(sim_idx(c, ev));
		uint64_t t_us = sim_anchor_us(c, ev);
		size_t i = 0, chunk;

		if (noise && radio.mhz == scan_mhz && pm.phase == UBT_PM_AA) {
			/* some energy on the channel that is not our connection */
			st.s = syms;
			st.n = 0;
			st.t = (uint32_t)(4 * (t_us + 5000));
			st_noise(&st, 600);
			(void)ubt_promisc_feed(&pm, st.s, st.n);
		}
		if (mhz != radio.mhz || t_us < radio.ready_us + 100) {
			(void)ubt_promisc_tick(&pm, t_us * 10);
			continue;
		}
		st_event(&st, c, ev, mhz);
		while (i < st.n) {
			unsigned int r;

			chunk = 20 + test_rng_u32() % 80;
			if (chunk > st.n - i) {
				chunk = st.n - i;
			}
			(void)ubt_promisc_feed(&pm, st.s + i, chunk);
			i += chunk;
			r = ubt_promisc_take_retune(&pm);
			if (r != 0) {
				radio.mhz = r;
				radio.ready_us = t_us + i / 4 + RETUNE_US;
				break;
			}
		}
		if (pm.phase == UBT_PM_DONE) {
			promisc_done_ev = ev;
		}
	}
}

static unsigned int inv37(unsigned int k)
{
	unsigned int x;

	for (x = 1; x < 37; x++) {
		if ((x * k) % 37 == 1) {
			return x;
		}
	}
	return 0;
}

ZTEST(ubertooth_le, test_promisc_recovers_a_connection_from_the_stream)
{
	static const unsigned int intervals[] = {6, 9, 24, 80, 400};
	static const unsigned int scans[] = {2440, 2404, 2406, 2478};
	unsigned int i, h;

	for (i = 0; i < ARRAY_LEN(intervals); i++) {
		struct simconn c = {0x332211d6, 0x012345, intervals[i], 7, 1000000, 5};

		c.hop = 5 + i * 3; /* 5, 8, 11, 14, 17 */
		promisc_run(&c, scans[i % ARRAY_LEN(scans)], 20000, i != 1, i == 4);
		zassert_equal(pm.phase, UBT_PM_DONE, "interval %u hop %u phase %u", c.interval,
			      c.hop, pm.phase);
		zassert_equal(pm.aa, c.aa);
		zassert_equal(pm.crc_init, c.crc_init);
		zassert_equal(pm.interval, c.interval);
		zassert_equal(pm.hop, c.hop);
		zassert_equal(pm.last_idx, 1);
		/* messages in the order of the upstream states, with their values */
		zassert_equal(pmcap.msgs, 4);
		zassert_equal(pmcap.msg_kind[0], UBT_PM_MSG_AA);
		zassert_equal(pmcap.msg_val[0], c.aa);
		zassert_equal(pmcap.msg_kind[1], UBT_PM_MSG_CRC_INIT);
		zassert_equal(pmcap.msg_val[1] & 0xffffff, c.crc_init);
		zassert_equal(pmcap.msg_kind[2], UBT_PM_MSG_INTERVAL);
		zassert_equal(pmcap.msg_val[2] & 0xffff, c.interval);
		zassert_equal(pmcap.msg_kind[3], UBT_PM_MSG_INCREMENT);
		zassert_equal(pmcap.msg_val[3] & 0xff, c.hop);
		/* the true AA was seen at least 4 times (unless the host gave it: the last case);
		 * the final packet time is that of a packet on channel 1 */
		zassert_true(pmcap.aa_hits >= 4 || i == 4);
		{
			uint32_t ev;
			bool found = false;

			for (ev = 0; ev < 20000 && !found; ev++) {
				if (sim_idx(&c, ev) == 1) {
					uint64_t a = sim_anchor_us(&c, ev) * 10;

					found = pm.t_last_pkt >= a && pm.t_last_pkt <= a + 2400;
				}
			}
			zassert_true(found, "t_last_pkt %llu", (unsigned long long)pm.t_last_pkt);
		}
		/* and the hand over to the follower is consistent: inverse relation of the table */
		zassert_equal((pm.hop * inv37(pm.hop)) % 37, 1);
	}
	/* every increment */
	for (h = 1; h <= 36; h++) {
		struct simconn c = {0x6a5c3b21, 0xabcdef, 6, h, 5000000, 20};

		promisc_run(&c, 2428, 40000, false, false);
		zassert_equal(pm.phase, UBT_PM_DONE, "hop %u", h);
		zassert_equal(pm.hop, h);
		zassert_equal(pm.crc_init, c.crc_init);
		zassert_equal(pm.interval, 6);
	}
}

/* An empty data PDU as the finder would emit it, for the event driven tests. */
static void mk_empty_pkt(usb_pkt_rx *p, uint32_t aa, unsigned int mhz, uint32_t crc_init,
			 uint8_t hdr, bool corrupt)
{
	uint8_t pdu[2] = {hdr, 0}, crc[3];
	unsigned int i;

	memset(p, 0, sizeof(*p));
	p->pkt_type = LE_PACKET;
	p->channel = (uint8_t)(mhz - 2402);
	for (i = 0; i < 4; i++) {
		p->data[i] = (uint8_t)(aa >> (8 * i));
	}
	p->data[4] = hdr;
	p->data[5] = 0;
	ref_crc_bytes(ref_crc(crc_init, pdu, 2), crc);
	if (corrupt) {
		crc[1] ^= 0x10;
	}
	memcpy(&p->data[6], crc, 3);
}

ZTEST(ubertooth_le, test_promisc_aa_cache_and_adoption)
{
	uint32_t aa = 0x332211d6;
	unsigned int k, i;
	struct stream st;
	static uint8_t bits[MAX_BITS];
	uint8_t pdu[2];
	uint32_t t = 1000;

	/* the scanner sees a packet of AA `a` on the listened channel; returns after feeding it */
	pm_setup(aa);
	ubt_promisc_start(&pm, 2440);
	for (k = 0; k < 6; k++) {
		uint32_t a = aa;

		/* distinct junk AAs between the real ones fill and churn the 32 entry cache (a
		 * count of 1 is evicted first, so more than 31 between two sightings of a new AA
		 * would lose it) */
		for (i = 0; i < 20; i++) {
			uint32_t junk = 0x40000000u + 0x01234567u * (k * 20 + i + 1);

			if (!ubt_le_aa_valid(junk)) {
				continue;
			}
			pdu[0] = empty_hdr(i);
			pdu[1] = 0;
			st.s = syms;
			st.n = 0;
			st.t = t;
			st_noise(&st, 30);
			st_bits(&st, bits,
				ref_air(bits, junk, ref_idx(2440), pdu, 2, 0x111111, true));
			st_noise(&st, 30);
			t = st.t + 4000;
			(void)ubt_promisc_feed(&pm, st.s, st.n);
			zassert_equal(pm.phase, UBT_PM_AA, "junk adopted");
		}
		pdu[0] = empty_hdr(k);
		st.s = syms;
		st.n = 0;
		st.t = t;
		st_noise(&st, 30);
		st_bits(&st, bits, ref_air(bits, a, ref_idx(2440), pdu, 2, 0x012345, true));
		if (k != 3) {
			st_noise(
				&st,
				30); /* the 4th sighting ends the chunk right at its last CRC bit */
		}
		t = st.t + 4000;
		(void)ubt_promisc_feed(&pm, st.s, st.n);
		/* count > 3: the fourth sighting adopts */
		zassert_equal(pm.phase, k >= 3 ? UBT_PM_CRC : UBT_PM_AA, "after sighting %u",
			      k + 1);
		if (k >= 3) {
			break;
		}
	}
	zassert_equal(pm.aa, aa);
	zassert_equal(pmcap.msgs, 1);
	zassert_equal(pmcap.msg_kind[0], UBT_PM_MSG_AA);
	zassert_equal(pmcap.msg_val[0], aa);
	zassert_equal(pmcap.aa_hits, 4);
	/* the finder now follows that AA with the CRC check off */
	zassert_equal(pm.le->aa, aa);
	zassert_false(pm.le->crc_verify);
}

ZTEST(ubertooth_le, test_promisc_rejects_invalid_aa_candidates)
{
	struct stream st;
	static uint8_t bits[MAX_BITS];
	uint8_t pdu[2] = {0x01, 0};
	unsigned int k;

	/* 0x8e89bed6 and friends cannot be data channel AAs: seen five times, still not adopted */
	pm_setup(0);
	ubt_promisc_start(&pm, 2440);
	for (k = 0; k < 6; k++) {
		uint32_t bad = (k & 1) ? 0x8e89bed6 : 0x55555555;

		st.s = syms;
		st.n = 0;
		st.t = 1000 + k * 10000;
		st_noise(&st, 30);
		st_bits(&st, bits, ref_air(bits, bad, ref_idx(2440), pdu, 2, 0x555555, true));
		st_noise(&st, 30);
		(void)ubt_promisc_feed(&pm, st.s, st.n);
	}
	zassert_equal(pm.phase, UBT_PM_AA);
	zassert_equal(pmcap.n, 0);
	/* with the validation off they are adopted like upstream would */
	pm_setup(0);
	pm.aa_validate = 0;
	ubt_promisc_start(&pm, 2440);
	for (k = 0; k < 4; k++) {
		st.s = syms;
		st.n = 0;
		st.t = 1000 + k * 10000;
		st_noise(&st, 30);
		st_bits(&st, bits,
			ref_air(bits, 0x8e89bed6, ref_idx(2440), pdu, 2, 0x555555, true));
		st_noise(&st, 30);
		(void)ubt_promisc_feed(&pm, st.s, st.n);
	}
	zassert_equal(pm.phase, UBT_PM_CRC);
	zassert_equal(pm.aa, 0x8e89bed6);
}

ZTEST(ubertooth_le, test_promisc_crc_needs_two_agreeing_packets)
{
	uint32_t aa = 0x332211d6;
	usb_pkt_rx p;

	pm_setup(aa);
	ubt_promisc_start(&pm, 2440);
	pm.phase = UBT_PM_CRC; /* as after an adoption */
	pm.aa = aa;
	/* a non empty PDU and a LLID 3 PDU tell nothing */
	mk_empty_pkt(&p, aa, 2440, 0x012345, 0x02, false);
	zassert_equal(ubt_promisc_packet(&pm, 2440, 1000, &p), 0);
	p.data[5] = 3;
	zassert_equal(ubt_promisc_packet(&pm, 2440, 1100, &p), 0);
	zassert_equal(pm.phase, UBT_PM_CRC);
	/* a corrupted packet, then two good ones */
	mk_empty_pkt(&p, aa, 2440, 0x012345, 0x01, true);
	ubt_promisc_packet(&pm, 2440, 2000, &p);
	zassert_equal(pm.phase, UBT_PM_CRC);
	mk_empty_pkt(&p, aa, 2440, 0x012345, 0x05, false);
	ubt_promisc_packet(&pm, 2440, 3000, &p);
	zassert_equal(pm.phase, UBT_PM_CRC, "one good packet is not enough");
	mk_empty_pkt(&p, aa, 2440, 0x012345, 0x0d, false);
	ubt_promisc_packet(&pm, 2440, 4000, &p);
	zassert_equal(pm.phase, UBT_PM_INTERVAL);
	zassert_equal(pm.crc_init, 0x012345);
	zassert_true(pm.le->crc_verify);
	zassert_equal(pm.le->crc_init, 0x012345);
	zassert_equal(pmcap.msgs, 1);
	zassert_equal(pmcap.msg_kind[0], UBT_PM_MSG_CRC_INIT);
	zassert_equal(pmcap.msg_val[0] & 0xffffff, 0x012345);
}

/* Visit times of a channel 37 intervals apart; skipped visits and jitter must not fool the
 * estimate. */
ZTEST(ubertooth_le, test_promisc_interval_estimate)
{
	static const unsigned int intervals[] = {6, 7, 24, 100, 3200};
	unsigned int i, miss;

	for (i = 0; i < ARRAY_LEN(intervals); i++) {
		for (miss = 0; miss < 3; miss++) {
			uint64_t visit = 37ULL * intervals[i] * 12500;
			uint64_t t = 5000000;
			usb_pkt_rx p;
			unsigned int k, steps = 0;

			pm_setup(1);
			ubt_promisc_start(&pm, 2440);
			pm.phase = UBT_PM_INTERVAL;
			pm.aa = 0x332211d6;
			mk_empty_pkt(&p, pm.aa, 2440, 0x012345, 0x01, false);
			ubt_promisc_packet(&pm, 2440, t, &p);
			for (k = 0; k < 40 && pm.phase == UBT_PM_INTERVAL; k++) {
				/* the slave's packet 230 us later is the same event: ignored */
				ubt_promisc_packet(&pm, 2440, t + 2300, &p);
				/* every `miss`th visit is skipped, jitter of a few us on the rest
				 */
				t += visit * (1 + (miss != 0 && (k % (miss + 2)) == 1 ? 1 : 0));
				ubt_promisc_packet(&pm, 2440, t + test_rng_u32() % 60, &p);
				steps++;
			}
			zassert_equal(pm.phase, UBT_PM_HOP_A, "interval %u miss %u after %u visits",
				      intervals[i], miss, steps);
			zassert_equal(pm.interval, intervals[i]);
			zassert_equal(pm.want_mhz, 2404);
			zassert_equal(pmcap.msgs, 1);
			zassert_equal(pmcap.msg_kind[0], UBT_PM_MSG_INTERVAL);
			zassert_equal(pmcap.msg_val[0] & 0xffff, intervals[i]);
		}
	}
}

ZTEST(ubertooth_le, test_promisc_hop_increment_from_two_channels)
{
	unsigned int n;

	for (n = 1; n <= 36; n++) {
		uint64_t ticks = 24ULL * 12500; /* 30 ms */
		uint64_t t = 90000000;
		usb_pkt_rx p;

		pm_setup(1);
		ubt_promisc_start(&pm, 2440);
		pm.phase = UBT_PM_HOP_A;
		pm.interval = 24;
		mk_empty_pkt(&p, 0x332211d6, 2404, 0x012345, 0x01, false);
		/* a stale packet of the other channel changes nothing */
		zassert_equal(ubt_promisc_packet(&pm, 2406, t - 10, &p), 0);
		zassert_equal(pm.phase, UBT_PM_HOP_A);
		zassert_equal(ubt_promisc_packet(&pm, 2404, t, &p), 2406);
		zassert_equal(pm.phase, UBT_PM_HOP_B);
		/* the slave's reply on the old channel before the retune completes is stale as well
		 */
		zassert_equal(ubt_promisc_packet(&pm, 2404, t + 2300, &p), 0);
		zassert_equal(pm.phase, UBT_PM_HOP_B);
		zassert_equal(ubt_promisc_packet(&pm, 2406, t + n * ticks + 40, &p), 0);
		zassert_equal(pm.phase, UBT_PM_DONE, "n %u", n);
		zassert_equal((pm.hop * n) % 37, 1, "n %u hop %u", n, pm.hop);
		zassert_equal(pmcap.msg_kind[0], UBT_PM_MSG_INCREMENT);
	}
	/* 0 or 37 or more intervals apart cannot be a hop count: start over on 2404 */
	{
		uint64_t ticks = 24ULL * 12500;
		usb_pkt_rx p;
		unsigned int bad[] = {0, 37, 50};
		unsigned int i;

		mk_empty_pkt(&p, 0x332211d6, 2404, 0x012345, 0x01, false);
		for (i = 0; i < ARRAY_LEN(bad); i++) {
			pm_setup(1);
			ubt_promisc_start(&pm, 2440);
			pm.phase = UBT_PM_HOP_A;
			pm.interval = 24;
			ubt_promisc_packet(&pm, 2404, 1000000, &p);
			zassert_equal(ubt_promisc_packet(&pm, 2406, 1000000 + bad[i] * ticks, &p),
				      2404);
			zassert_equal(pm.phase, UBT_PM_HOP_A);
			zassert_equal(pmcap.msgs, 0);
		}
	}
}

ZTEST(ubertooth_le, test_promisc_retune_requests_and_restart)
{
	usb_pkt_rx p;

	pm_setup(1);
	ubt_promisc_start(&pm, 2440);
	pm.phase = UBT_PM_HOP_A;
	pm.interval = 24;
	mk_empty_pkt(&p, 0x332211d6, 2404, 0x012345, 0x01, false);
	ubt_promisc_packet(&pm, 2404, 1000, &p);
	zassert_equal(pm.want_mhz, 2406);
	/* taking the request points the finder at the channel and clears it */
	zassert_equal(ubt_promisc_take_retune(&pm), 2406);
	zassert_equal(pm.le->mhz, 2406);
	zassert_equal(ubt_promisc_take_retune(&pm), 0);
	/* silence: restart in phase 1 after 10 s + 80 intervals */
	zassert_equal(ubt_promisc_tick(&pm, 1000 + 100000000ULL + 80 * 24 * 12500ULL), 0);
	zassert_equal(ubt_promisc_tick(&pm, 1000 + 100000000ULL + 80 * 24 * 12500ULL + 1), 2440);
	zassert_equal(pm.phase, UBT_PM_AA);
	zassert_equal(pm.want_mhz, 2440);
	/* nothing to restart in phase 1 or when done */
	zassert_equal(ubt_promisc_tick(&pm, 1ULL << 50), 0);
	pm.phase = UBT_PM_DONE;
	zassert_equal(ubt_promisc_tick(&pm, 1ULL << 51), 0);
}

/* ---- connection following ---- */

/* Channel selection from the specification text, for the master model and the comparisons. */
static unsigned int ref_used(const uint8_t chm[5], unsigned int *list)
{
	unsigned int n = 0, i;

	for (i = 0; i < 37; i++) {
		if ((chm[i / 8] >> (i % 8)) & 1) {
			list[n++] = i;
		}
	}
	return n;
}

static unsigned int ref_csa1(uint32_t ev, unsigned int hop, const uint8_t chm[5])
{
	unsigned int list[37], n = ref_used(chm, list);
	unsigned int u = (unsigned int)(((uint64_t)(ev + 1) * hop) % 37), i;

	for (i = 0; i < n; i++) {
		if (list[i] == u) {
			return u;
		}
	}
	return list[u % n];
}

static uint16_t ref_perm(uint16_t x)
{
	uint16_t y = 0;
	unsigned int byte, bit;

	for (byte = 0; byte < 2; byte++) {
		for (bit = 0; bit < 8; bit++) {
			y |= (uint16_t)(((x >> (8 * byte + bit)) & 1) << (8 * byte + 7 - bit));
		}
	}
	return y;
}

static unsigned int ref_csa2(uint32_t counter, uint32_t aa, const uint8_t chm[5])
{
	uint16_t cid = (uint16_t)((aa >> 16) ^ (aa & 0xffff));
	uint16_t p = (uint16_t)(counter & 0xffff) ^ cid;
	unsigned int list[37], n = ref_used(chm, list), i, u;
	unsigned int round;

	for (round = 0; round < 3; round++) {
		p = ref_perm(p);
		p = (uint16_t)((17u * p + cid) & 0xffff);
	}
	p ^= cid;
	u = p % 37;
	for (i = 0; i < n; i++) {
		if (list[i] == u) {
			return u;
		}
	}
	return list[(n * (unsigned int)p) >> 16];
}

static const uint8_t chm_all[5] = {0xff, 0xff, 0xff, 0xff, 0x1f};

static void conn_start_from_vector(struct le_conn *c, bool csa2, const uint8_t chm[5],
				   unsigned int hop)
{
	/* a CONNECT_REQ built by hand, used to give the channel selection something to chew on */
	uint8_t pdu[36] = {0};
	struct le_conn_pkt p;
	struct le_conn_cfg cfg;

	le_conn_cfg_default(&cfg);
	le_conn_init(c, &cfg);
	pdu[0] = (uint8_t)(0x05 | (csa2 ? 0x20 : 0));
	pdu[1] = 34;
	pdu[14] = 0xd6; /* AA 0x8e89bed6: the CSA #2 sample data identifier 0x305f */
	pdu[15] = 0xbe;
	pdu[16] = 0x89;
	pdu[17] = 0x8e;
	pdu[18] = 0x45;
	pdu[19] = 0x23;
	pdu[20] = 0x01;
	pdu[21] = 3;
	pdu[22] = 0;
	pdu[24] = 24;
	pdu[28] = 100;
	memcpy(&pdu[30], chm, 5);
	pdu[35] = (uint8_t)(hop | (7 << 5));
	p.mhz = 2402;
	p.t = 1000000;
	p.pdu = pdu;
	p.len = sizeof(pdu);
	p.crc_ok = true;
	(void)le_conn_packet(c, &p, p.t + LE_PKT_TICKS(34));
	zassert_equal(c->state, LE_CONN_FIRST);
}

ZTEST(ubertooth_le, test_conn_channel_selection)
{
	struct le_conn c;
	unsigned int ev, trial, i;

	/* the spec's CSA #2 sample data and the python model's tables */
	zassert_equal(le_conn_csa2_prn(0, 0x8e89bed6), 56857);
	zassert_equal(le_conn_csa2_prn(1, 0x8e89bed6), 1685);
	zassert_equal(le_conn_csa2_prn(2, 0x8e89bed6), 38301);
	zassert_equal(le_conn_csa2_prn(3, 0x8e89bed6), 27475);
	conn_start_from_vector(&c, false, chm_all, 7);
	for (ev = 0; ev < 40; ev++) {
		zassert_equal(le_conn_chan_idx(&c, ev), csa1_full[ev], "ev %u", ev);
	}
	conn_start_from_vector(&c, false, csa_chm_part, 7);
	for (ev = 0; ev < 40; ev++) {
		zassert_equal(le_conn_chan_idx(&c, ev), csa1_part[ev], "ev %u", ev);
	}
	conn_start_from_vector(&c, true, chm_all, 7);
	for (ev = 0; ev < 40; ev++) {
		zassert_equal(le_conn_chan_idx(&c, ev), csa2_full[ev], "ev %u", ev);
	}
	conn_start_from_vector(&c, true, csa_chm_part, 7);
	for (ev = 0; ev < 40; ev++) {
		zassert_equal(le_conn_chan_idx(&c, ev), csa2_part[ev], "ev %u", ev);
	}
	/* random maps and hops against the C model and against the vendored upstream remapping */
	for (trial = 0; trial < 300; trial++) {
		uint8_t chm[5];
		le_channel_remapping_t remap;
		unsigned int hop = 5 + test_rng_u32() % 12;
		unsigned int n_used;

		for (i = 0; i < 5; i++) {
			chm[i] = (uint8_t)test_rng_u32();
		}
		chm[4] &= 0x1f;
		if (trial % 3 == 0) { /* sparse maps */
			chm[1] = chm[2] = chm[3] = 0;
			chm[0] &= 0x41;
		}
		{
			unsigned int list[37];

			n_used = ref_used(chm, list);
		}
		if (n_used < 2) {
			continue;
		}
		le_parse_channel_map(chm, &remap);
		conn_start_from_vector(&c, false, chm, hop);
		for (ev = 0; ev < 200; ev++) {
			unsigned int u = ((ev + 1) * hop) % 37;

			zassert_equal(le_conn_chan_idx(&c, ev), ref_csa1(ev, hop, chm));
			zassert_equal(le_conn_chan_idx(&c, ev), le_map_channel(u, &remap));
		}
		conn_start_from_vector(&c, true, chm, hop);
		for (ev = 0; ev < 200; ev += 3) {
			zassert_equal(le_conn_chan_idx(&c, ev), ref_csa2(ev, 0x8e89bed6, chm));
		}
		zassert_equal(le_conn_chan_idx(&c, 0x10005), ref_csa2(0x10005, 0x8e89bed6, chm));
	}
}

/* ---- the master and the radio of the connection tests ---- */

struct cc { /* a test case */
	uint32_t aa;
	uint32_t crc_init;
	unsigned int interval; /* 1.25 ms */
	unsigned int hop;
	bool csa2;
	uint8_t chm[5];
	int drift_ppm;        /* the master's clock against ours */
	unsigned int sca;     /* code in the CONNECT_REQ */
	unsigned int timeout; /* 10 ms */
	unsigned int win_size, win_offset;
	unsigned int loss_pct, slave_miss_pct, md_pct;
	unsigned int events;
	unsigned int upd_at, upd_instant, upd_interval, upd_woff, upd_wsize, upd_timeout;
	unsigned int chm_at, chm_instant;
	uint8_t chm_new[5];
	unsigned int term_at;
	unsigned int vanish_at;
	uint32_t retune_cfg, retune_real;
	unsigned int max_skip;
	bool run_out;       /* keep ticking after the last event until the follower gives up */
	bool promisc_start; /* the follower joins at event promisc_at with le_conn_start_known() */
	unsigned int promisc_at;
};

static void cc_default(struct cc *cs)
{
	memset(cs, 0, sizeof(*cs));
	cs->aa = 0x4a5b6c7d;
	cs->crc_init = 0x1a2b3c;
	cs->interval = 24;
	cs->hop = 7;
	memcpy(cs->chm, chm_all, 5);
	cs->sca = 7;
	cs->timeout = 300;
	cs->win_size = 3;
	cs->win_offset = 0;
	cs->events = 200;
	cs->retune_cfg = 40000;
	cs->retune_real = 35000;
	cs->max_skip = 64;
}

struct cres {
	unsigned int events;      /* master events simulated */
	unsigned int ev_heard;    /* of those, with a packet delivered to the follower */
	unsigned int ev_followed; /* of those, at least one the follower accepted */
	unsigned int wrong_channel;
	unsigned int tunes;
	bool ended;
	uint8_t reason;
	uint64_t t_end;
	uint64_t t_last_master;
	uint32_t last_event;
	uint32_t skipped_total;
	struct le_conn_stats st;
	struct le_conn c;
};

struct mpkt {
	uint64_t ts, te;
	uint8_t pdu[40];
	uint8_t plen;
	bool md;
};

struct csim {
	const struct cc *cs;
	struct le_conn c;
	struct cres *res;
	uint16_t radio_mhz;
	uint64_t radio_ready;
	uint64_t deadline;
	uint64_t now;
	uint32_t ev;   /* master's event being played */
	uint32_t seen; /* followed counter at the start of the event */
	bool heard;
};

static unsigned int pct(void)
{
	return test_rng_u32() % 100;
}

static uint64_t scale(uint64_t ticks, int ppm)
{
	return (uint64_t)((int64_t)ticks + (int64_t)ticks * ppm / 1000000);
}

/* The channel index the master hops to in event ev. */
static unsigned int sim_true_idx(const struct cc *cs, uint32_t ev)
{
	const uint8_t *chm = (cs->chm_at != 0 && ev >= cs->chm_instant) ? cs->chm_new : cs->chm;

	return cs->csa2 ? ref_csa2(ev, cs->aa, chm) : ref_csa1(ev, cs->hop, chm);
}

static void sim_apply(struct csim *s, const struct le_conn_action *a)
{
	struct cres *r = s->res;

	switch (a->act) {
	case LE_ACT_TUNE: {
		uint32_t ev = s->ev;

		/* the event the plan is for: the first one from the master's current one with that
		 * counter */
		uint32_t base = s->cs->promisc_start ? s->cs->promisc_at : 0;

		while ((uint16_t)(ev - base) != a->event) {
			ev++;
		}
		if (a->mhz != ref_mhz(sim_true_idx(s->cs, ev))) {
			r->wrong_channel++;
		}
		zassert_equal(a->aa, s->cs->aa);
		zassert_equal(a->crc_init, s->cs->crc_init);
		zassert_true(a->t_listen <= a->t_end);
		zassert_true(a->t_retune <= a->t_listen);
		r->tunes++;
		r->skipped_total += a->skipped;
		if (a->mhz != s->radio_mhz) {
			s->radio_mhz = a->mhz;
			s->radio_ready =
				(a->t_retune > s->now ? a->t_retune : s->now) + s->cs->retune_real;
		}
		s->deadline = a->t_end;
		break;
	}
	case LE_ACT_WAIT:
		s->deadline = a->t_end;
		break;
	case LE_ACT_ADV:
		r->ended = true;
		r->reason = a->reason;
		r->t_end = s->now;
		r->last_event = s->ev;
		break;
	default:
		break;
	}
}

static void sim_tick_until(struct csim *s, uint64_t t)
{
	while (!s->res->ended && s->c.state != LE_CONN_ADV && s->deadline < t) {
		struct le_conn_action a;

		s->now = s->deadline + 500; /* the glue notices a little late */
		a = le_conn_tick(&s->c, s->now);
		if (a.act == LE_ACT_NONE) {
			s->deadline = s->now + 1; /* not due yet by its own reckoning */
		}
		sim_apply(s, &a);
	}
}

static void make_pdu(struct mpkt *m, uint8_t hdr, const uint8_t *payload, unsigned int plen)
{
	m->pdu[0] = hdr;
	m->pdu[1] = (uint8_t)plen;
	if (plen != 0) {
		memcpy(&m->pdu[2], payload, plen);
	}
	m->plen = (uint8_t)plen;
	m->md = ((hdr >> 4) & 1) != 0;
}

/* Play one packet to the follower (if the radio is on the channel and settled) and tick up to it.
 */
static void sim_packet(struct csim *s, const struct mpkt *m, unsigned int mhz)
{
	struct le_conn_pkt p;
	struct le_conn_action a;
	bool heard =
		pct() >= s->cs->loss_pct && s->radio_mhz == mhz && s->radio_ready + 200 <= m->ts;

	sim_tick_until(s, m->ts);
	if (s->res->ended || !heard) {
		return;
	}
	p.mhz = (uint16_t)mhz;
	p.t = m->ts + test_rng_u32() % 21; /* the demodulator's timestamp noise: 0 to 2 us late */
	p.pdu = m->pdu;
	p.len = (uint16_t)(2 + m->plen);
	p.crc_ok = true;
	s->now = m->te;
	s->heard = true;
	a = le_conn_packet(&s->c, &p, m->te);
	sim_apply(s, &a);
}

static const uint8_t ctrl_terminate[] = {0x02, 0x13};

static void mk_update(uint8_t *pl, const struct cc *cs)
{
	pl[0] = 0x00;
	pl[1] = (uint8_t)cs->upd_wsize;
	pl[2] = (uint8_t)cs->upd_woff;
	pl[3] = (uint8_t)(cs->upd_woff >> 8);
	pl[4] = (uint8_t)cs->upd_interval;
	pl[5] = (uint8_t)(cs->upd_interval >> 8);
	pl[6] = 0;
	pl[7] = 0;
	pl[8] = (uint8_t)cs->upd_timeout;
	pl[9] = (uint8_t)(cs->upd_timeout >> 8);
	pl[10] = (uint8_t)cs->upd_instant;
	pl[11] = (uint8_t)(cs->upd_instant >> 8);
}

static void mk_chm(uint8_t *pl, const struct cc *cs)
{
	pl[0] = 0x01;
	memcpy(&pl[1], cs->chm_new, 5);
	pl[6] = (uint8_t)cs->chm_instant;
	pl[7] = (uint8_t)(cs->chm_instant >> 8);
}

/*
 * Run a connection: a CONNECT_REQ at a fixed time, a master that follows the case (windows,
 * drift, updates, channel map changes, termination, packet loss), and the follower driven as the
 * glue would. Returns the master's view and the follower's.
 */
static void conn_run(const struct cc *cs, struct cres *res)
{
	static struct csim s;
	struct le_conn_cfg cfg;
	uint8_t req[36] = {0};
	struct le_conn_pkt p;
	struct le_conn_action a;
	uint64_t t_req = 30000000, connect_end, ws, interval, anchor;
	uint32_t ev;
	unsigned int timeout_u = cs->timeout;

	memset(&s, 0, sizeof(s));
	memset(res, 0, sizeof(*res));
	s.cs = cs;
	s.res = res;
	le_conn_cfg_default(&cfg);
	cfg.retune_ticks = cs->retune_cfg;
	cfg.max_skip = (uint8_t)cs->max_skip;
	le_conn_init(&s.c, &cfg);
	s.radio_mhz = 2402;

	/* the CONNECT_REQ */
	req[0] = (uint8_t)(0x05 | (cs->csa2 ? 0x20 : 0));
	req[1] = 34;
	req[14] = (uint8_t)cs->aa;
	req[15] = (uint8_t)(cs->aa >> 8);
	req[16] = (uint8_t)(cs->aa >> 16);
	req[17] = (uint8_t)(cs->aa >> 24);
	req[18] = (uint8_t)cs->crc_init;
	req[19] = (uint8_t)(cs->crc_init >> 8);
	req[20] = (uint8_t)(cs->crc_init >> 16);
	req[21] = (uint8_t)cs->win_size;
	req[22] = (uint8_t)cs->win_offset;
	req[23] = (uint8_t)(cs->win_offset >> 8);
	req[24] = (uint8_t)cs->interval;
	req[25] = (uint8_t)(cs->interval >> 8);
	req[28] = (uint8_t)timeout_u;
	req[29] = (uint8_t)(timeout_u >> 8);
	memcpy(&req[30], cs->chm, 5);
	req[35] = (uint8_t)(cs->hop | (cs->sca << 5));
	p.mhz = 2402;
	p.t = t_req;
	p.pdu = req;
	p.len = sizeof(req);
	p.crc_ok = true;
	connect_end = t_req + LE_PKT_TICKS(34);
	s.now = connect_end;
	if (!cs->promisc_start) {
		a = le_conn_packet(&s.c, &p, connect_end);
		if (a.act == LE_ACT_NONE) {
			return;
		}
		sim_apply(&s, &a);
	}

	/* the master's anchors, in our clock: it starts in the transmit window and drifts */
	ws = connect_end + LE_TICKS_PER_UNIT + (uint64_t)cs->win_offset * LE_TICKS_PER_UNIT;
	interval = (uint64_t)cs->interval * LE_TICKS_PER_UNIT;
	anchor = ws + test_rng_u32() % 10000;
	for (ev = 0; ev < cs->events && !res->ended; ev++) {
		struct mpkt pk[4];
		unsigned int n = 0, i, mhz;
		uint64_t t;
		bool md = pct() < cs->md_pct;
		uint8_t payload[40];

		if (ev != 0) {
			uint64_t step = scale(interval, cs->drift_ppm);

			if (cs->upd_at != 0 && ev == cs->upd_instant) {
				uint64_t span = (uint64_t)cs->upd_wsize * LE_TICKS_PER_UNIT;

				anchor += step +
					  scale((uint64_t)cs->upd_woff * LE_TICKS_PER_UNIT,
						cs->drift_ppm) +
					  (span > 4000 ? test_rng_u32() % (span - 4000) : 0);
				interval = (uint64_t)cs->upd_interval * LE_TICKS_PER_UNIT;
			} else {
				anchor += step;
			}
		}
		s.ev = ev;
		s.heard = false;
		s.seen = s.c.stats.followed;
		res->events++;
		mhz = ref_mhz(sim_true_idx(cs, ev));
		if (cs->vanish_at != 0 && ev >= cs->vanish_at) {
			continue; /* the master is gone: only time passes */
		}
		/* what is said in this event: one exchange, or two when the master sets MD */
		t = anchor;
		for (i = 0; i < (md ? 4u : 2u); i++) {
			bool master = (i & 1) == 0;
			bool more = md && i < 2;
			uint8_t hdr = (uint8_t)(0x01 | ((test_rng_u32() & 1) << 2) | ((test_rng_u32() & 1) << 3));
			unsigned int plen = 0;

			if (master && i == 0 && cs->upd_at != 0 && ev >= cs->upd_at &&
			    ev < cs->upd_at + 3) {
				mk_update(payload, cs);
				hdr = (uint8_t)(0x03 | (hdr & 0x0c));
				plen = 12;
			} else if (master && i == 0 && cs->chm_at != 0 && ev >= cs->chm_at &&
				   ev < cs->chm_at + 3) {
				mk_chm(payload, cs);
				hdr = (uint8_t)(0x03 | (hdr & 0x0c));
				plen = 8;
			} else if (master && i == 0 && cs->term_at != 0 && ev >= cs->term_at &&
				   ev < cs->term_at + 2) {
				memcpy(payload, ctrl_terminate, 2);
				hdr = (uint8_t)(0x03 | (hdr & 0x0c));
				plen = 2;
			} else if (master && (test_rng_u32() % 4) == 0) {
				unsigned int k;

				plen = 1 + test_rng_u32() % 27;
				for (k = 0; k < plen; k++) {
					payload[k] = (uint8_t)test_rng_u32();
				}
				hdr = (uint8_t)(0x02 | (hdr & 0x0c));
			}
			if (more) {
				hdr |= 0x10; /* both sides carry MD while the event goes on */
			}
			if (!master && pct() < cs->slave_miss_pct) {
				break; /* no answer: the event ends */
			}
			make_pdu(&pk[n], hdr, payload, plen);
			pk[n].ts = t;
			pk[n].te = t + LE_PKT_TICKS(plen);
			t = pk[n].te + LE_T_IFS_TICKS;
			n++;
		}
		if (cs->promisc_start && ev == cs->promisc_at) {
			struct le_conn_known k = {cs->aa,
						  cs->crc_init,
						  (uint16_t)cs->interval,
						  (uint8_t)cs->hop,
						  (uint8_t)sim_true_idx(cs, ev),
						  pk[0].ts};

			s.now = pk[0].te;
			a = le_conn_start_known(&s.c, &k, pk[0].te);
			sim_apply(&s, &a);
			continue;
		}
		if (cs->promisc_start && ev < cs->promisc_at) {
			continue;
		}
		for (i = 0; i < n; i++) {
			sim_packet(&s, &pk[i], mhz);
			if (res->ended) {
				break;
			}
			res->t_last_master = pk[i].te;
		}
		/* the glue's loop notices the quiet after the last packet: the event's deadline
		 * passes */
		if (n != 0 && !res->ended) {
			sim_tick_until(&s, pk[n - 1].te + 5000);
		}
		if (s.heard) {
			res->ev_heard++;
		}
		if (s.c.stats.followed != s.seen) {
			res->ev_followed++;
		}
	}
	if (!res->ended && cs->run_out) {
		/* let time run out so that a supervision timeout can fire */
		sim_tick_until(&s, anchor + 40ULL * 100000000ULL);
	}
	res->st = s.c.stats;
	res->c = s.c;
}

static unsigned int timeout_for(unsigned int interval)
{
	unsigned int u = interval * 3 * 125 / 1000 + 10;

	return u > 3200 ? 3200 : u;
}

ZTEST(ubertooth_le, test_conn_follows_all_intervals)
{
	static const unsigned int intervals[] = {6, 7, 8, 12, 24, 40, 80, 160, 400, 800, 3200};
	unsigned int i, woff;

	for (i = 0; i < ARRAY_LEN(intervals); i++) {
		for (woff = 0; woff <= 3; woff += 3) {
			struct cc cs;
			static struct cres r;

			cc_default(&cs);
			cs.interval = intervals[i];
			cs.timeout = timeout_for(intervals[i]);
			cs.win_offset = woff;
			cs.hop = 5 + i;
			cs.drift_ppm = (int)(test_rng_u32() % 61) - 30;
			cs.sca = 5 + (i % 3);
			cs.events = intervals[i] > 800 ? 40 : 150;
			conn_run(&cs, &r);
			zassert_false(r.ended, "interval %u woff %u: ended reason %u at event %u",
				      cs.interval, woff, r.reason, r.last_event);
			zassert_equal(r.wrong_channel, 0, "interval %u", cs.interval);
			/* every event but the first two or three is followed */
			zassert_true(r.ev_followed + 3 >= r.events,
				     "interval %u woff %u: followed %u of %u", cs.interval, woff,
				     r.ev_followed, r.events);
			zassert_true(r.st.skipped <= 2, "interval %u woff %u skipped %u",
				     cs.interval, woff, (unsigned int)r.st.skipped);
			zassert_equal(r.c.state, LE_CONN_TRACK);
			zassert_true(r.c.verified);
		}
	}
}

ZTEST(ubertooth_le, test_conn_survives_packet_loss)
{
	static const unsigned int loss[] = {5, 15, 30};
	unsigned int i, k;

	for (i = 0; i < ARRAY_LEN(loss); i++) {
		for (k = 0; k < 4; k++) {
			static const unsigned int intervals[] = {6, 24, 100, 800};
			struct cc cs;
			static struct cres r;

			cc_default(&cs);
			cs.interval = intervals[k];
			cs.timeout = timeout_for(intervals[k]);
			cs.loss_pct = loss[i];
			cs.slave_miss_pct = 10;
			cs.md_pct = 20;
			cs.events = intervals[k] >= 800 ? 120 : 400;
			cs.drift_ppm = (int)(test_rng_u32() % 101) - 50;
			cs.sca = 4;
			cs.hop = 5 + k * 3;
			conn_run(&cs, &r);
			zassert_false(r.ended, "loss %u interval %u: ended reason %u at event %u",
				      loss[i], cs.interval, r.reason, r.last_event);
			zassert_equal(r.wrong_channel, 0);
			/* the retune always fits: at most the first window is passed over */
			zassert_true(r.st.skipped <= 2, "skipped %u", (unsigned int)r.st.skipped);
			/* an event is lost when the master's packet and the answer are: 0.8 % at 5
			 * %, 11 % at 30 % */
			zassert_true(
				r.ev_followed * 100 >=
					r.events * (loss[i] == 30 ? 78 : (loss[i] == 15 ? 88 : 93)),
				"loss %u interval %u: followed %u of %u", loss[i], cs.interval,
				r.ev_followed, r.events);
		}
	}
}

ZTEST(ubertooth_le, test_conn_update_and_channel_map)
{
	static const struct {
		unsigned int from, to, woff, wsize;
		bool csa2;
	} up[] = {
		{24, 40, 2, 3, false}, {24, 6, 0, 2, false},    {6, 80, 5, 8, false},
		{80, 24, 0, 1, true},  {40, 400, 20, 8, false}, {24, 24, 0, 1, true},
		{400, 6, 0, 3, false},
	};
	unsigned int i, loss;

	for (loss = 0; loss <= 10; loss += 10) {
		for (i = 0; i < ARRAY_LEN(up); i++) {
			struct cc cs;
			static struct cres r;
			uint8_t newmap[5] = {0xfe, 0x7b, 0xff, 0x0f, 0x10};

			cc_default(&cs);
			cs.interval = up[i].from;
			cs.timeout =
				timeout_for(up[i].from > up[i].to ? up[i].from : up[i].to) + 100;
			cs.loss_pct = loss;
			cs.slave_miss_pct = 5;
			cs.csa2 = up[i].csa2;
			cs.events = 160;
			cs.upd_at = 20;
			cs.upd_instant = 30;
			cs.upd_interval = up[i].to;
			cs.upd_woff = up[i].woff;
			cs.upd_wsize = up[i].wsize;
			cs.upd_timeout = timeout_for(up[i].to) + 100;
			cs.chm_at = 90;
			cs.chm_instant = 100;
			memcpy(cs.chm_new, newmap, 5);
			cs.hop = 5 + i;
			conn_run(&cs, &r);
			zassert_false(r.ended, "%u -> %u loss %u: ended reason %u at event %u",
				      up[i].from, up[i].to, loss, r.reason, r.last_event);
			zassert_equal(r.wrong_channel, 0, "%u -> %u", up[i].from, up[i].to);
			zassert_equal(r.st.updates, 1);
			zassert_equal(r.st.chm_updates, 1);
			zassert_true(r.ev_followed * 100 >= r.events * (loss ? 80 : 94),
				     "%u -> %u loss %u: followed %u of %u", up[i].from, up[i].to,
				     loss, r.ev_followed, r.events);
			zassert_equal(r.c.interval, (uint64_t)up[i].to * LE_TICKS_PER_UNIT);
		}
	}
}

ZTEST(ubertooth_le, test_conn_csa2)
{
	static const unsigned int intervals[] = {6, 24, 200};
	unsigned int i;

	for (i = 0; i < ARRAY_LEN(intervals); i++) {
		struct cc cs;
		static struct cres r;
		uint8_t sparse[5] = {0x15, 0x22, 0x00, 0x80, 0x01};

		cc_default(&cs);
		cs.csa2 = true;
		cs.interval = intervals[i];
		cs.timeout = timeout_for(intervals[i]);
		memcpy(cs.chm, sparse, 5);
		cs.loss_pct = 5;
		conn_run(&cs, &r);
		zassert_false(r.ended, "interval %u reason %u", intervals[i], r.reason);
		zassert_equal(r.wrong_channel, 0);
		zassert_true(r.ev_followed * 100 >= r.events * 90);
	}
}

ZTEST(ubertooth_le, test_conn_slow_retune_follows_every_nth_event)
{
	static const struct {
		unsigned int interval;
		uint32_t retune;
		unsigned int every; /* the event after the retune that the window opens for */
	} t[] = {
		{6, 80000, 2},   {6, 150000, 3},  {6, 400000, 6},
		{24, 150000, 1}, {24, 400000, 2}, {100, 3000000, 3},
	};
	unsigned int i;

	for (i = 0; i < ARRAY_LEN(t); i++) {
		struct cc cs;
		static struct cres r;

		cc_default(&cs);
		cs.interval = t[i].interval;
		cs.timeout = 3000;
		cs.retune_cfg = t[i].retune;
		cs.retune_real = t[i].retune - 5000;
		cs.events = 240;
		cs.drift_ppm = 40;
		cs.hop = 11;
		conn_run(&cs, &r);
		zassert_false(r.ended, "interval %u retune %u: ended reason %u at event %u",
			      t[i].interval, (unsigned int)t[i].retune, r.reason, r.last_event);
		zassert_equal(r.wrong_channel, 0, "channels stay right across skipped events");
		zassert_true(r.st.skipped > 0 || t[i].every == 1);
		zassert_within(r.ev_followed * t[i].every, r.events, 4 * t[i].every,
			       "interval %u retune %u: followed %u of %u", t[i].interval,
			       (unsigned int)t[i].retune, r.ev_followed, r.events);
	}
}

ZTEST(ubertooth_le, test_conn_gives_up_when_it_cannot_keep_up)
{
	struct cc cs;
	static struct cres r;

	cc_default(&cs);
	cs.interval = 6;
	cs.timeout = 1000;
	cs.retune_cfg = 10000000; /* 1 s per retune: 133 events to skip */
	cs.retune_real = 9000000;
	cs.max_skip = 32;
	conn_run(&cs, &r);
	zassert_true(r.ended);
	zassert_equal(r.reason, LE_REASON_TOO_FAST);
	/* the same with room enough */
	cs.max_skip = 255;
	cc_default(&cs);
	cs.interval = 6;
	cs.timeout = 3200;
	cs.retune_cfg = 600000; /* 60 ms: 8 events */
	cs.retune_real = 550000;
	cs.max_skip = 16;
	cs.events = 300;
	conn_run(&cs, &r);
	zassert_false(r.ended, "reason %u", r.reason);
	zassert_equal(r.wrong_channel, 0);
}

ZTEST(ubertooth_le, test_conn_supervision_timeout)
{
	static const unsigned int intervals[] = {6, 24, 400};
	unsigned int i;

	for (i = 0; i < ARRAY_LEN(intervals); i++) {
		struct cc cs;
		static struct cres r;
		uint64_t timeout_ticks;

		/* the master disappears after event 30 */
		cc_default(&cs);
		cs.interval = intervals[i];
		cs.timeout = timeout_for(intervals[i]);
		cs.vanish_at = 30;
		cs.events = 60;
		cs.run_out = true;
		conn_run(&cs, &r);
		timeout_ticks = (uint64_t)cs.timeout * LE_TICKS_PER_10MS;
		zassert_true(r.ended);
		zassert_equal(r.reason, LE_REASON_TIMEOUT);
		/* it hangs on for the timeout after the last packet it heard, and not much longer
		 */
		zassert_true(r.t_end > r.t_last_master + timeout_ticks -
					       3 * (uint64_t)cs.interval * LE_TICKS_PER_UNIT,
			     "interval %u ended early", cs.interval);
		zassert_true(r.t_end < r.t_last_master + timeout_ticks +
					       4 * (uint64_t)cs.interval * LE_TICKS_PER_UNIT +
					       2000000,
			     "interval %u ended late: %llu vs %llu", cs.interval,
			     (unsigned long long)(r.t_end - r.t_last_master),
			     (unsigned long long)timeout_ticks);
		/* a connection that never starts: 6 events after the first window */
		cc_default(&cs);
		cs.interval = intervals[i];
		cs.timeout = timeout_for(intervals[i]);
		cs.loss_pct = 100;
		cs.events = 20;
		cs.run_out = true;
		conn_run(&cs, &r);
		zassert_true(r.ended);
		zassert_equal(r.reason, LE_REASON_TIMEOUT);
		zassert_true(r.t_end < 30000000ULL + 20000 +
					       (uint64_t)(8 + 6) * cs.interval * LE_TICKS_PER_UNIT +
					       2000000);
		zassert_equal(r.st.followed, 0);
	}
}

ZTEST(ubertooth_le, test_conn_terminate)
{
	struct cc cs;
	static struct cres r;

	cc_default(&cs);
	cs.term_at = 40;
	cs.events = 100;
	conn_run(&cs, &r);
	zassert_true(r.ended);
	zassert_equal(r.reason, LE_REASON_TERMINATE);
	zassert_true(r.last_event >= 40 && r.last_event <= 42, "ended at event %u", r.last_event);
}

ZTEST(ubertooth_le, test_conn_joined_from_promiscuous_mode)
{
	static const unsigned int intervals[] = {6, 24, 100};
	unsigned int i, k;

	for (i = 0; i < ARRAY_LEN(intervals); i++) {
		for (k = 0; k < 2; k++) {
			struct cc cs;
			static struct cres r;

			cc_default(&cs);
			cs.interval = intervals[i];
			cs.timeout = timeout_for(intervals[i]);
			cs.promisc_start = true;
			cs.promisc_at = 7 + k;
			cs.events = 200;
			cs.loss_pct = 5;
			cs.drift_ppm = 30;
			conn_run(&cs, &r);
			zassert_false(r.ended, "interval %u: ended reason %u", cs.interval,
				      r.reason);
			zassert_equal(r.wrong_channel, 0, "interval %u", cs.interval);
			zassert_true(r.ev_followed * 100 >= (r.events - cs.promisc_at) * 85,
				     "interval %u: followed %u of %u", cs.interval, r.ev_followed,
				     r.events);
			zassert_false(r.c.counter_known);
		}
	}
}

static struct le_conn_pkt mkpkt(const uint8_t *pdu, unsigned int len, unsigned int mhz, uint64_t t,
				bool ok)
{
	struct le_conn_pkt p = {(uint16_t)mhz, t, pdu, (uint16_t)len, ok};

	return p;
}

static void build_req(uint8_t req[36], unsigned int interval, unsigned int woff, unsigned int wsize,
		      unsigned int timeout, const uint8_t chm[5], unsigned int hop,
		      unsigned int latency)
{
	memset(req, 0, 36);
	req[0] = 0xc5;
	req[1] = 34;
	req[14] = 0x7d;
	req[15] = 0x6c;
	req[16] = 0x5b;
	req[17] = 0x4a;
	req[21] = (uint8_t)wsize;
	req[22] = (uint8_t)woff;
	req[23] = (uint8_t)(woff >> 8);
	req[24] = (uint8_t)interval;
	req[25] = (uint8_t)(interval >> 8);
	req[26] = (uint8_t)latency;
	req[27] = (uint8_t)(latency >> 8);
	req[28] = (uint8_t)timeout;
	req[29] = (uint8_t)(timeout >> 8);
	memcpy(&req[30], chm, 5);
	req[35] = (uint8_t)hop;
}

ZTEST(ubertooth_le, test_conn_connect_req_validation)
{
	struct le_conn c;
	uint8_t req[36];
	struct le_conn_pkt p;
	struct le_conn_action a;
	uint8_t two[5] = {0x03, 0, 0, 0, 0}, one[5] = {0x01, 0, 0, 0, 0};
	struct {
		unsigned int interval, woff, wsize, timeout, hop, latency;
		const uint8_t *chm;
	} bad[] = {
		{5, 0, 3, 100, 7, 0, chm_all},    /* interval below 7.5 ms */
		{3201, 0, 3, 100, 7, 0, chm_all}, /* above 4 s */
		{24, 25, 3, 100, 7, 0, chm_all},  /* window offset beyond the interval */
		{24, 0, 0, 100, 7, 0, chm_all},   /* no window */
		{24, 0, 9, 100, 7, 0, chm_all},   /* window above 10 ms */
		{6, 0, 6, 100, 7, 0, chm_all},    /* window not below the interval */
		{24, 0, 3, 9, 7, 0, chm_all},     /* timeout under 100 ms */
		{24, 0, 3, 3201, 7, 0, chm_all},  /* above 32 s */
		{24, 0, 3, 100, 0, 0, chm_all},   /* hop 0 never leaves a channel */
		{24, 0, 3, 100, 7, 500, chm_all}, /* latency */
		{24, 0, 3, 100, 7, 0, one},       /* one channel */
	};
	unsigned int i;

	for (i = 0; i < ARRAY_LEN(bad); i++) {
		le_conn_init(&c, NULL);
		build_req(req, bad[i].interval, bad[i].woff, bad[i].wsize, bad[i].timeout,
			  bad[i].chm, bad[i].hop, bad[i].latency);
		p = mkpkt(req, 36, 2402, 100000, true);
		a = le_conn_packet(&c, &p, 100000 + LE_PKT_TICKS(34));
		zassert_equal(a.act, LE_ACT_ADV, "case %u", i);
		zassert_equal(a.reason, LE_REASON_BAD_PARAMS);
		zassert_equal(c.state, LE_CONN_ADV);
	}
	/* good ones: the edges */
	{
		unsigned int ok_cases[][6] = {
			{6, 0, 1, 10, 5, 0},
			{3200, 3200, 8, 3200, 16, 499},
			{24, 24, 8, 100, 31, 0},
		};

		for (i = 0; i < ARRAY_LEN(ok_cases); i++) {
			le_conn_init(&c, NULL);
			build_req(req, ok_cases[i][0], ok_cases[i][1], ok_cases[i][2],
				  ok_cases[i][3], two, ok_cases[i][4], ok_cases[i][5]);
			p = mkpkt(req, 36, 2402, 100000, true);
			a = le_conn_packet(&c, &p, 100000 + LE_PKT_TICKS(34));
			zassert_equal(a.act, LE_ACT_TUNE, "edge %u", i);
			zassert_equal(c.state, LE_CONN_FIRST);
		}
	}
	/* not a connection request, too short, wrong length field, a bad CRC */
	le_conn_init(&c, NULL);
	build_req(req, 24, 0, 3, 100, chm_all, 7, 0);
	req[0] = 0xc3;
	p = mkpkt(req, 36, 2402, 100000, true);
	zassert_equal(le_conn_packet(&c, &p, 200000).act, LE_ACT_NONE);
	req[0] = 0xc5;
	p = mkpkt(req, 35, 2402, 100000, true);
	zassert_equal(le_conn_packet(&c, &p, 200000).act, LE_ACT_NONE);
	req[1] = 33;
	p = mkpkt(req, 36, 2402, 100000, true);
	zassert_equal(le_conn_packet(&c, &p, 200000).act, LE_ACT_NONE);
	req[1] = 34;
	p = mkpkt(req, 36, 2402, 100000, false);
	zassert_equal(le_conn_packet(&c, &p, 200000).act, LE_ACT_NONE);
	zassert_equal(c.state, LE_CONN_ADV);
	c.cfg.connect_crc = false;
	zassert_equal(le_conn_packet(&c, &p, 200000).act, LE_ACT_TUNE);
	/* cancel and the idle tick */
	a = le_conn_cancel(&c);
	zassert_equal(a.act, LE_ACT_ADV);
	zassert_equal(a.reason, LE_REASON_CANCEL);
	zassert_equal(a.mhz, 2402);
	zassert_equal(a.aa, 0x8e89bed6);
	zassert_equal(a.crc_init, 0x555555);
	zassert_false(le_conn_following(&c));
	zassert_equal(le_conn_tick(&c, 1ULL << 40).act, LE_ACT_NONE);
}

ZTEST(ubertooth_le, test_conn_first_window_timing)
{
	struct le_conn c;
	struct le_conn_cfg cfg;
	uint8_t req[36];
	struct le_conn_pkt p;
	struct le_conn_action a;
	uint64_t t_req = 1000000, end = t_req + LE_PKT_TICKS(34);

	/* the window opens 1.25 ms + WinOffset after the request; a retune that fits is scheduled
	 * just in time */
	le_conn_cfg_default(&cfg);
	cfg.retune_ticks = 10000; /* 1 ms */
	le_conn_init(&c, &cfg);
	build_req(req, 24, 4, 3, 100, chm_all, 7, 0);
	req[35] = 7 | (7 << 5);
	p = mkpkt(req, 36, 2402, t_req, true);
	a = le_conn_packet(&c, &p, end);
	zassert_equal(a.act, LE_ACT_TUNE);
	zassert_equal(a.event, 0);
	zassert_equal(a.mhz, ref_mhz(7));
	zassert_equal(a.skipped, 0);
	zassert_equal(a.aa, 0x4a5b6c7d);
	{
		uint64_t ws = end + 12500 + 4 * 12500;
		uint64_t ww = (ws - end) * (20 + cfg.own_ppm) / 1000000 + cfg.jitter_ticks;

		zassert_equal(a.t_listen, ws - ww - cfg.pre_ticks);
		zassert_equal(a.t_retune, a.t_listen - 10000);
		zassert_equal(a.t_end, ws + 3 * 12500 + ww + cfg.detect_ticks + cfg.slave_ticks);
	}
	/* a window that opens before the retune can be done: event 0 is passed over, event 1
	 * planned */
	cfg.retune_ticks = 40000;
	le_conn_init(&c, &cfg);
	build_req(req, 24, 0, 3, 100, chm_all, 7, 0);
	p = mkpkt(req, 36, 2402, t_req, true);
	a = le_conn_packet(&c, &p, end);
	zassert_equal(a.act, LE_ACT_TUNE);
	zassert_equal(a.event, 1);
	zassert_equal(a.skipped, 1);
	zassert_equal(a.mhz, ref_mhz(14));
	zassert_equal(c.stats.skipped, 1);
}

ZTEST(ubertooth_le, test_conn_ignores_what_it_must)
{
	struct le_conn c;
	struct le_conn_cfg cfg;
	uint8_t req[36], pdu[20];
	struct le_conn_pkt p;
	struct le_conn_action a;
	uint64_t t_req = 1000000, end = t_req + LE_PKT_TICKS(34);

	le_conn_cfg_default(&cfg);
	le_conn_init(&c, &cfg);
	build_req(req, 24, 4, 3, 100, chm_all, 7, 0);
	p = mkpkt(req, 36, 2402, t_req, true);
	a = le_conn_packet(&c, &p, end);
	zassert_equal(a.act, LE_ACT_TUNE);
	/* a packet of another channel and one with a bad CRC change nothing */
	memset(pdu, 0, sizeof(pdu));
	pdu[0] = 0x01;
	p = mkpkt(pdu, 2, a.mhz + 2, a.t_listen + 1000, true);
	zassert_equal(le_conn_packet(&c, &p, a.t_listen + 2000).act, LE_ACT_NONE);
	zassert_equal(c.stats.stale, 1);
	p = mkpkt(pdu, 2, a.mhz, a.t_listen + 1000, false);
	zassert_equal(le_conn_packet(&c, &p, a.t_listen + 2000).act, LE_ACT_NONE);
	zassert_equal(c.stats.bad_crc, 1);
	zassert_equal(c.npk, 0);
	/* a good packet: wait for the answer */
	p = mkpkt(pdu, 2, a.mhz, a.t_listen + 1000, true);
	a = le_conn_packet(&c, &p, a.t_listen + 1000 + LE_PKT_TICKS(0));
	zassert_equal(a.act, LE_ACT_WAIT);
	zassert_equal(a.t_end,
		      p.t + LE_PKT_TICKS(0) + LE_T_IFS_TICKS + cfg.ifs_slack + cfg.detect_ticks);
	/* the deadline is respected by tick */
	zassert_equal(le_conn_tick(&c, a.t_end - 1).act, LE_ACT_NONE);
	a = le_conn_tick(&c, a.t_end);
	zassert_equal(a.act, LE_ACT_TUNE);
	zassert_equal(a.event, 1);
	zassert_equal(c.stats.followed, 1);
}

ZTEST(ubertooth_le, test_conn_encryption_hides_control_pdus)
{
	struct cc cs;
	struct le_conn c;
	uint8_t req[36], pdu[20];
	struct le_conn_pkt p;
	struct le_conn_action a;
	uint64_t t_req = 1000000, end = t_req + LE_PKT_TICKS(34), t;

	(void)cs;
	le_conn_init(&c, NULL);
	build_req(req, 24, 4, 3, 100, chm_all, 7, 0);
	p = mkpkt(req, 36, 2402, t_req, true);
	a = le_conn_packet(&c, &p, end);
	t = a.t_listen + 3000;
	/* LL_START_ENC_REQ: from here on the control PDUs are encrypted, the opcode byte is noise
	 */
	memset(pdu, 0, sizeof(pdu));
	pdu[0] = 0x03;
	pdu[1] = 1;
	pdu[2] = 0x05;
	p = mkpkt(pdu, 3, a.mhz, t, true);
	(void)le_conn_packet(&c, &p, t + LE_PKT_TICKS(1));
	zassert_true(c.encrypted);
	memset(pdu, 0, sizeof(pdu));
	pdu[0] = 0x03;
	pdu[1] = 12;
	pdu[2] = 0x00;
	pdu[3] = 1;
	pdu[6] = 100;
	pdu[10] = 100;
	pdu[12] = 40;
	p = mkpkt(pdu, 14, a.mhz, t + 5000, true);
	(void)le_conn_packet(&c, &p, t + 5000 + LE_PKT_TICKS(12));
	zassert_false(c.upd_pending);
	/* before encryption it would have been taken */
	le_conn_init(&c, NULL);
	p = mkpkt(req, 36, 2402, t_req, true);
	a = le_conn_packet(&c, &p, end);
	p = mkpkt(pdu, 14, a.mhz, t, true);
	(void)le_conn_packet(&c, &p, t + LE_PKT_TICKS(12));
	zassert_true(c.upd_pending);
	zassert_equal(c.upd_instant, 40);
	zassert_equal(c.upd_interval, 100 * 12500ULL);
	/* an instant in the past (or the current event) is not taken */
	pdu[12] = 0;
	pdu[13] = 0;
	c.upd_pending = false;
	p = mkpkt(pdu, 14, a.mhz, t + 100000, true);
	(void)le_conn_packet(&c, &p, t + 100000 + LE_PKT_TICKS(12));
	zassert_false(c.upd_pending);
}

/* ---- the whole chain: symbols, finder, follower ---- */

static struct e2e {
	struct ubt_le le;
	struct le_conn c;
	struct ubt_clk_unwrap unwrap;
	struct le_conn_action pending;
	unsigned int emitted;
	uint64_t last_start;
	uint64_t last_end;
} e2e;

static void e2e_emit(void *ctx, const usb_pkt_rx *pk)
{
	struct le_conn_pkt p;
	struct le_conn_action a;

	ARG_UNUSED(ctx);
	p = le_conn_pkt_from_usb(pk, &e2e.le.info, &e2e.unwrap);
	e2e.emitted++;
	e2e.last_start = p.t;
	a = le_conn_packet(&e2e.c, &p, p.t + LE_PKT_TICKS(pk->data[5]));
	e2e.last_end = p.t + LE_PKT_TICKS(pk->data[5]);
	if (a.act != LE_ACT_NONE) {
		e2e.pending = a; /* the callback must not retune the finder: that is done after the
				    feed */
	}
}

struct e2e_res {
	unsigned int played;
	unsigned int followed;
	unsigned int wrong_chan;
	unsigned int wrong_anchor;
};

/*
 * Play events first_ev.. of the simulated connection c as symbols into the finder, and the
 * finder's packets into the follower, applying its plans after the feed that produced them as
 * the glue has to. The follower's event 0 is the master's event ev_base.
 */
static void e2e_play(const struct simconn *c, uint32_t ev_base, uint32_t first_ev, uint32_t nev,
		     struct e2e_res *r)
{
	static uint8_t bits[MAX_BITS];
	uint64_t radio_ready = 0;
	unsigned int radio_mhz = e2e.le.mhz;
	uint32_t ev;

	memset(r, 0, sizeof(*r));
	for (ev = first_ev; ev < first_ev + nev; ev++) {
		unsigned int mhz = ref_mhz(sim_idx(c, ev));
		uint64_t a_us = sim_anchor_us(c, ev);
		struct le_conn_action a = e2e.pending;
		struct stream st;
		uint8_t pdu[2] = {empty_hdr(ev), 0};
		unsigned int n, before = e2e.emitted;

		if (a.act == LE_ACT_TUNE) {
			zassert_equal(ubt_le_set_channel(&e2e.le, a.mhz), 0);
			ubt_le_set_aa(&e2e.le, a.aa);
			ubt_le_set_crc_init(&e2e.le, a.crc_init);
			if (a.mhz != ref_mhz(sim_idx(c, ev_base + a.event))) {
				r->wrong_chan++;
			}
			if (a.mhz != radio_mhz) {
				radio_mhz = a.mhz;
				radio_ready =
					(a.t_retune > e2e.last_end ? a.t_retune : e2e.last_end) +
					35000;
			}
			e2e.pending.act = LE_ACT_NONE;
		}
		r->played++;
		if (mhz != radio_mhz || radio_ready + 400 > a_us * 10) {
			continue; /* not there in time */
		}
		/* master's empty PDU, 150 us, slave's empty PDU, in a bed of noise */
		st.s = syms;
		st.n = 0;
		st.t = (uint32_t)(4 * a_us + 2) - 4 * 30;
		st.pwr = 100;
		st_noise(&st, 30);
		n = ref_air(bits, c->aa, ref_idx(mhz), pdu, 2, c->crc_init, true);
		st_bits(&st, bits, n);
		st_noise(&st, 150);
		pdu[0] = empty_hdr(ev + 1);
		n = ref_air(bits, c->aa, ref_idx(mhz), pdu, 2, c->crc_init, true);
		st_bits(&st, bits, n);
		st_noise(&st, 30);
		zassert_equal(ubt_le_feed(&e2e.le, st.s, st.n), 2, "event %u", ev);
		zassert_equal(e2e.emitted - before, 2);
		/* from the third event on the anchor is the first packet of the event, to the
		 * sample */
		if (ev >= first_ev + 3 && e2e.c.anchored && e2e.c.ref_t != a_us * 10) {
			r->wrong_anchor++;
		}
		r->followed++;
	}
}

static void e2e_init(void)
{
	struct le_conn_cfg cfg;

	memset(&e2e, 0, sizeof(e2e));
	ubt_le_init(&e2e.le, e2e_emit, NULL, NULL, NULL);
	le_conn_cfg_default(&cfg);
	le_conn_init(&e2e.c, &cfg);
}

ZTEST(ubertooth_le, test_end_to_end_from_symbols)
{
	static uint8_t bits[MAX_BITS];
	uint8_t req[36];
	struct stream st;
	const uint64_t tc_us = 1000000;
	struct simconn c = {0x4a5b6c7d, 0x1a2b3c, 24, 9, 0, 0};
	struct e2e_res r;
	unsigned int n;

	e2e_init();
	/* CONNECT_REQ on channel 37 */
	build_req(req, 24, 2, 3, 100, chm_all, 9, 0);
	req[14] = 0x7d;
	req[18] = 0x3c;
	req[19] = 0x2b;
	req[20] = 0x1a;
	st_init(&st, (uint32_t)(4 * tc_us + 2) - 4 * 40);
	st_noise(&st, 40);
	n = ref_air(bits, UBT_LE_ADV_AA, 37, req, 36, UBT_LE_ADV_CRC_INIT, true);
	st_bits(&st, bits, n);
	st_noise(&st, 40);
	zassert_equal(ubt_le_feed(&e2e.le, st.s, st.n), 1);
	zassert_equal(e2e.pending.act, LE_ACT_TUNE, "the CONNECT_REQ starts the connection");
	zassert_equal(e2e.c.stats.connects, 1);
	/* the finder's time stamp is the true start of the packet */
	zassert_equal(e2e.last_start, tc_us * 10);
	/* the master's first packet 100 us into the transmit window, 3.75 ms after the window opens
	 */
	c.t0_us = tc_us + 352 + 1250 + 2 * 1250 + 100;
	e2e_play(&c, 0, 0, 150, &r);
	zassert_equal(r.wrong_chan, 0);
	zassert_equal(r.wrong_anchor, 0, "anchors off the true start");
	zassert_true(r.followed >= 148, "followed %u of %u", r.followed, r.played);
	zassert_equal(e2e.c.stats.missed, 0);
	zassert_true(e2e.c.verified);
	zassert_equal(e2e.le.stats.crc_bad, 0);
}

ZTEST(ubertooth_le, test_end_to_end_promiscuous_to_follower)
{
	static const unsigned int intervals[] = {6, 24, 80};
	unsigned int i;

	for (i = 0; i < ARRAY_LEN(intervals); i++) {
		struct simconn c = {0x332211d6, 0x012345, intervals[i], 11, 7000000, 3};
		struct le_conn_known k;
		struct e2e_res r;

		promisc_run(&c, 2440, 40000, true, false);
		zassert_equal(pm.phase, UBT_PM_DONE, "interval %u", c.interval);
		/* hand over: the recovered parameters and the packet that completed the recovery */
		k = le_conn_known_from_promisc(&pm);
		zassert_equal(k.aa, c.aa);
		zassert_equal(k.crc_init, c.crc_init);
		zassert_equal(k.interval, c.interval);
		zassert_equal(k.hop, c.hop);
		zassert_equal(k.chan_idx, 1);
		zassert_equal(sim_idx(&c, promisc_done_ev), 1);
		e2e_init();
		e2e.pending = le_conn_start_known(&e2e.c, &k, k.t + LE_PKT_TICKS(0));
		zassert_equal(e2e.pending.act, LE_ACT_TUNE);
		e2e.last_end = k.t + LE_PKT_TICKS(0);
		/* the finder inherits the channel of the last promiscuous packet */
		ubt_le_set_channel(&e2e.le, 2406);
		e2e_play(&c, promisc_done_ev, promisc_done_ev + 1, 120, &r);
		zassert_equal(r.wrong_chan, 0, "interval %u", c.interval);
		zassert_equal(r.wrong_anchor, 0);
		zassert_true(r.followed * 100 >= r.played * 95, "interval %u: followed %u of %u",
			     c.interval, r.followed, r.played);
		zassert_true(e2e.c.verified);
	}
}

ZTEST(ubertooth_le, test_conn_anchor_from_the_slave_is_corrected)
{
	struct le_conn c;
	struct le_conn_cfg cfg;
	uint8_t req[36], pdu[2] = {0x01, 0x00};
	struct le_conn_pkt p;
	struct le_conn_action a;
	uint64_t t_req = 1000000, end = t_req + LE_PKT_TICKS(34);
	uint64_t i_ticks = 24ULL * 12500, ws;
	uint64_t m0, s0;

	le_conn_cfg_default(&cfg);
	le_conn_init(&c, &cfg);
	build_req(req, 24, 4, 3, 100, chm_all, 7, 0);
	p = mkpkt(req, 36, 2402, t_req, true);
	a = le_conn_packet(&c, &p, end);
	zassert_equal(a.act, LE_ACT_TUNE);
	ws = end + 12500 + 4 * 12500;
	m0 = ws + 3000; /* the master's first packet, lost */
	s0 = m0 + LE_PKT_TICKS(0) + LE_T_IFS_TICKS;
	/* only the slave's answer is heard: it becomes a provisional anchor */
	p = mkpkt(pdu, 2, a.mhz, s0, true);
	a = le_conn_packet(&c, &p, s0 + LE_PKT_TICKS(0));
	zassert_equal(a.act, LE_ACT_WAIT);
	a = le_conn_tick(&c, a.t_end);
	zassert_equal(a.act, LE_ACT_TUNE);
	zassert_true(c.anchored);
	zassert_false(c.verified);
	zassert_equal(c.stats.followed, 1);
	/* the next window reaches back far enough for the master's packet, which is earlier */
	zassert_true(a.t_listen + 500 <= m0 + i_ticks, "window starts %llu, master at %llu",
		     (unsigned long long)a.t_listen, (unsigned long long)(m0 + i_ticks));
	p = mkpkt(pdu, 2, a.mhz, m0 + i_ticks, true);
	a = le_conn_packet(&c, &p, m0 + i_ticks + LE_PKT_TICKS(0));
	zassert_equal(a.act, LE_ACT_WAIT);
	p = mkpkt(pdu, 2, c.cur_mhz, m0 + i_ticks + LE_PKT_TICKS(0) + LE_T_IFS_TICKS, true);
	a = le_conn_packet(&c, &p, p.t + LE_PKT_TICKS(0));
	zassert_equal(a.act, LE_ACT_TUNE, "a pair closes the event");
	zassert_true(c.verified);
	zassert_equal(c.ref_t, m0 + i_ticks);
	/* the event after that is planned around the master's packet, not the slave's */
	zassert_true(a.t_listen > m0 + 2 * i_ticks - 2000 && a.t_listen < m0 + 2 * i_ticks);
}

ZTEST(ubertooth_le, test_conn_start_known_validation_and_state)
{
	struct le_conn c;
	struct le_conn_action a;
	struct le_conn_known k = {0x4a5b6c7d, 0x1a2b3c, 24, 7, 1, 5000000};

	le_conn_init(&c, NULL);
	a = le_conn_start_known(&c, &k, 5000000 + LE_PKT_TICKS(0));
	zassert_equal(a.act, LE_ACT_TUNE);
	zassert_equal(c.state, LE_CONN_TRACK);
	/* the packet was on channel 1, so the next event is on (1 + 7) mod 37 */
	zassert_equal(a.mhz, ref_mhz(8));
	zassert_equal(a.aa, 0x4a5b6c7d);
	zassert_equal(a.crc_init, 0x1a2b3c);
	zassert_false(c.counter_known);
	zassert_true(c.timeout >= 5000000);
	k.interval = 5;
	zassert_equal(le_conn_start_known(&c, &k, 6000000).act, LE_ACT_ADV);
	k.interval = 24;
	k.hop = 0;
	zassert_equal(le_conn_start_known(&c, &k, 6000000).act, LE_ACT_ADV);
	k.hop = 7;
	k.chan_idx = 37;
	zassert_equal(le_conn_start_known(&c, &k, 6000000).act, LE_ACT_ADV);
}

ZTEST(ubertooth_le, test_finder_busy_flag_and_stats)
{
	const struct le_vec *v = &le_vecs[0];
	struct stream st;
	size_t half;

	le_setup(v->mhz, v->aa, v->crc_init);
	st_init(&st, 1000);
	st_packet(&st, v->aa, v->mhz, v->pdu, v->pdu_len, v->crc_init, 40, 40);
	half = 40 + 8 + 32 + 8 * 6;
	zassert_equal(ubt_le_feed(&le, st.s, half), 0);
	zassert_true(ubt_le_busy(&le), "inside the packet");
	zassert_equal(ubt_le_feed(&le, st.s + half, st.n - half), 1);
	zassert_false(ubt_le_busy(&le));
	zassert_equal(le.stats.packets, 1);
	zassert_equal(le.stats.emitted, 1);
	zassert_equal(le.stats.hits, 1);
	zassert_equal(ubt_le_feed(&le, st.s, 0), 0);
	/* reset drops a packet in progress */
	le_setup(v->mhz, v->aa, v->crc_init);
	st_init(&st, 1000);
	st_packet(&st, v->aa, v->mhz, v->pdu, v->pdu_len, v->crc_init, 40, 40);
	zassert_equal(ubt_le_feed(&le, st.s, half), 0);
	ubt_le_reset(&le);
	zassert_false(ubt_le_busy(&le));
	zassert_equal(ubt_le_feed(&le, st.s + half, st.n - half), 0);
}

ZTEST(ubertooth_le, test_conn_leaves_at_a_phy_change)
{
	struct le_conn c;
	uint8_t req[36], pdu[16], empty[2] = {0x01, 0x00};
	struct le_conn_pkt p;
	struct le_conn_action a;
	uint64_t t_req = 1000000, end = t_req + LE_PKT_TICKS(34), t;
	unsigned int k;

	le_conn_init(&c, NULL);
	build_req(req, 24, 4, 3, 100, chm_all, 7, 0);
	p = mkpkt(req, 36, 2402, t_req, true);
	a = le_conn_packet(&c, &p, end);
	t = end + 12500 + 4 * 12500 + 3000;
	/* a PHY update to 1M in both directions changes nothing, to 2M takes effect at its instant
	 */
	for (k = 0; k < 2; k++) {
		memset(pdu, 0, sizeof(pdu));
		pdu[0] = 0x03;
		pdu[1] = 5;
		pdu[2] = 0x18;
		pdu[3] = k == 0 ? 1 : 2; /* master to slave */
		pdu[4] = k == 0 ? 1 : 0;
		pdu[5] = 10;
		p = mkpkt(pdu, 7, a.mhz, t, true);
		a = le_conn_packet(&c, &p, t + LE_PKT_TICKS(5));
		zassert_equal(a.act, LE_ACT_WAIT);
		zassert_equal(c.phy_pending, k == 1);
		/* the answer, then the plan for the next events */
		p = mkpkt(empty, 2, c.cur_mhz, t + LE_PKT_TICKS(5) + LE_T_IFS_TICKS, true);
		a = le_conn_packet(&c, &p, p.t + LE_PKT_TICKS(0));
		zassert_equal(a.act, LE_ACT_TUNE);
		t += 24ULL * 12500;
		for (; c.ev < 12; t += 24ULL * 12500) {
			p = mkpkt(empty, 2, a.mhz, t, true);
			a = le_conn_packet(&c, &p, t + LE_PKT_TICKS(0));
			if (a.act == LE_ACT_WAIT) {
				p = mkpkt(empty, 2, c.cur_mhz, t + LE_PKT_TICKS(0) + LE_T_IFS_TICKS,
					  true);
				a = le_conn_packet(&c, &p, p.t + LE_PKT_TICKS(0));
			}
			if (a.act == LE_ACT_ADV) {
				break;
			}
		}
		if (k == 0) {
			zassert_true(c.ev >= 12 && a.act == LE_ACT_TUNE);
		} else {
			zassert_equal(a.act, LE_ACT_ADV);
			zassert_equal(a.reason, LE_REASON_PHY);
			zassert_equal(c.ev, 10, "left at the instant");
		}
		le_conn_init(&c, NULL);
		p = mkpkt(req, 36, 2402, t_req, true);
		a = le_conn_packet(&c, &p, end);
		t = end + 12500 + 4 * 12500 + 3000;
	}
}

ZTEST(ubertooth_le, test_packet_at_the_very_start_of_a_segment)
{
	/* the squelch may open late: a segment can start at the first AA symbol, with no preamble
	 * left */
	static uint8_t bits[MAX_BITS];
	const struct le_vec *v = &le_vecs[2];
	struct stream st;
	unsigned int n =
		ref_air(bits, v->aa, ref_idx(v->mhz), v->pdu, v->pdu_len, v->crc_init, false);
	unsigned int skip;

	for (skip = 0; skip < 4; skip++) {
		/* skip == 0: starts at the first AA symbol; later: the AA is cut, nothing may come
		 * out */
		le_setup(v->mhz, v->aa, v->crc_init);
		st_init(&st, 99);
		st_bits(&st, bits + skip, n - skip);
		st_noise(&st, 20);
		zassert_equal(feed(&le, &st, 0), skip == 0 ? 1 : 0, "skip %u", skip);
		if (skip == 0) {
			zassert_mem_equal(cap.pk[0].data, v->usb, DMA_SIZE);
		}
	}
	/* the same after a gap in a running stream */
	le_setup(v->mhz, v->aa, v->crc_init);
	st_init(&st, 99);
	st_noise(&st, 100);
	st.t += 1000;
	st_bits(&st, bits, n);
	st_noise(&st, 20);
	zassert_equal(feed(&le, &st, 0), 1);
	/* and the promiscuous scanner: an empty data PDU right at the start of the stream */
	{
		uint8_t pdu[2] = {0x01, 0};
		unsigned int m;

		pm_setup(0x332211d6);
		pm.aa_validate = 1;
		ubt_promisc_start(&pm, 2440);
		st_init(&st, 5);
		m = ref_air(bits, 0x332211d6, ref_idx(2440), pdu, 2, 0x012345, false);
		st_bits(&st, bits, m);
		st_noise(&st, 4);
		(void)ubt_promisc_feed(&pm, st.s, st.n);
		zassert_equal(pmcap.n, 1, "candidate right at the start of the stream");
	}
}
