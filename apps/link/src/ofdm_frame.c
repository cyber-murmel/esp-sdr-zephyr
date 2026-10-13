/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Link frames on the OFDM PHY, see ofdm_frame.h. The payload capacity (whole
 * bytes of the payload symbols) is split into the fewest units (RS) or blocks
 * (BCH) the code allows, all the same length: shortened codes. RS: the missing
 * data bytes are virtual zeros in front, frame byte j belongs to unit
 * j % units. BCH: block u is bytes u len .. (u + 1) len - 1, its data bytes
 * first; the user data and its CRC-32 run through the blocks' data bytes in
 * order.
 */

#include <string.h>

#include "ofdm_frame.h"
#include "qam.h"
#include "rs.h"

#define HDR_VERSION 0xa5U

/* The codes by id: BCH parameters (m 0: not a BCH code). */
static const struct {
	uint8_t m, t;
	char name[9];
} fec_tab[OFDM_FECS] = {
	[OFDM_FEC_RS] = {0, 0, "rs"},       [OFDM_FEC_NONE] = {0, 0, "none"},
	{9, 1, "bch1-64"},   {9, 2, "bch2-64"},   {9, 3, "bch3-64"},   {9, 4, "bch4-64"},
	{10, 1, "bch1-128"}, {10, 2, "bch2-128"}, {10, 3, "bch3-128"}, {10, 4, "bch4-128"},
	{11, 1, "bch1-256"}, {11, 2, "bch2-256"}, {11, 3, "bch3-256"}, {11, 4, "bch4-256"},
	{6, 1, "bch1-8"},
};

const char *ofdm_fec_name(unsigned int fec)
{
	return fec < OFDM_FECS ? fec_tab[fec].name : NULL;
}

int ofdm_fec_parse(const char *name)
{
	for (unsigned int f = 0; f < OFDM_FECS; f++) {
		if (strcmp(name, fec_tab[f].name) == 0) {
			return (int)f;
		}
	}
	return -1;
}

struct layout {
	unsigned int fec, units, len, k;
	size_t cap, user;
};

static bool layout_of(const struct ofdm_ctx *ctx, enum ofdm_mod mod, unsigned int fec,
		      struct layout *l)
{
	unsigned int nmax, parity;

	if (mod >= OFDM_MODS || fec >= OFDM_FECS) {
		return false;
	}
	l->fec = fec;
	l->cap = ofdm_frame_bits(ctx, mod) / 8U;
	/* The work buffer holds the payload bits rounded up to bytes. */
	if (l->cap >= OFDM_FRAME_CAP_MAX) {
		return false;
	}
	if (fec == OFDM_FEC_RS) {
		nmax = RS_N;
		parity = RS_NROOTS;
	} else if (fec == OFDM_FEC_NONE) {
		nmax = (unsigned int)l->cap;
		parity = 0;
	} else {
		unsigned int r, kmax;

		if (!bch_params(fec_tab[fec].m, fec_tab[fec].t, &r, &parity, &kmax)) {
			return false;
		}
		nmax = kmax + parity;
	}
	if (nmax == 0U) {
		return false;
	}
	/* The fewest units, all shortened alike so that they fill the frame. */
	l->units = (unsigned int)((l->cap + nmax - 1U) / nmax);
	if (l->units == 0U) {
		return false;
	}
	l->len = (unsigned int)(l->cap / l->units);
	/* A BCH code may do better with whole blocks and a few bytes unused (short blocks). */
	if (fec != OFDM_FEC_RS && fec != OFDM_FEC_NONE && l->cap / nmax > 0U &&
	    (l->cap / nmax) * (nmax - parity) > l->units * (l->len - parity)) {
		l->units = (unsigned int)(l->cap / nmax);
		l->len = nmax;
	}
	/* A unit needs its parity and some data (RS: 8 bytes); the user data its CRC. */
	if (l->len < parity + (fec == OFDM_FEC_RS ? 8U : 1U) ||
	    (size_t)l->units * (l->len - parity) < 5U) {
		return false;
	}
	l->k = l->len - parity;
	l->user = (size_t)l->units * l->k - 4U;
	return true;
}

size_t ofdm_frame_user_bytes(const struct ofdm_ctx *ctx, enum ofdm_mod mod, unsigned int fec)
{
	struct layout l;

	return layout_of(ctx, mod, fec, &l) ? l.user : 0U;
}

/* The work memory's BCH code set up for @p fec. */
static const struct bch_code *code_for(struct ofdm_fwork *wk, unsigned int fec)
{
	uint32_t key = 0xbc000000U | ((uint32_t)fec_tab[fec].m << 8) | fec_tab[fec].t;

	if (wk->bch_key != key) {
		if (bch_init(&wk->bch, fec_tab[fec].m, fec_tab[fec].t) != 0) {
			return NULL;
		}
		wk->bch_key = key;
	}
	return &wk->bch;
}

/* The user data and its CRC-32 (little endian) into the blocks' data bytes. */
static void blocks_put(const struct layout *l, uint8_t *bytes, const uint8_t *data, uint32_t crc)
{
	const uint8_t tail[4] = {(uint8_t)crc, (uint8_t)(crc >> 8), (uint8_t)(crc >> 16),
				 (uint8_t)(crc >> 24)};
	size_t d = 0;

	for (unsigned int u = 0; u < l->units; u++, d += l->k) {
		uint8_t *dst = &bytes[(size_t)u * l->len];
		size_t nu = d < l->user ? l->user - d : 0U;

		nu = nu > l->k ? l->k : nu;
		memcpy(dst, &data[d], nu);
		for (size_t i = nu; i < l->k; i++) {
			dst[i] = tail[d + i - l->user];
		}
	}
}

/* The user data out of the blocks and its CRC-32 checked: 0, or -3 on a mismatch. */
static int blocks_get(const struct layout *l, const uint8_t *bytes, uint8_t *data)
{
	uint8_t tail[4];
	uint32_t crc = 0;
	size_t d = 0;

	for (unsigned int u = 0; u < l->units; u++, d += l->k) {
		const uint8_t *src = &bytes[(size_t)u * l->len];
		size_t nu = d < l->user ? l->user - d : 0U;

		nu = nu > l->k ? l->k : nu;
		memcpy(&data[d], src, nu);
		crc = qam_crc32_cont(crc, src, nu);
		for (size_t i = nu; i < l->k; i++) {
			tail[d + i - l->user] = src[i];
		}
	}
	return crc == ((uint32_t)tail[0] | ((uint32_t)tail[1] << 8) | ((uint32_t)tail[2] << 16) |
		       ((uint32_t)tail[3] << 24))
		       ? 0
		       : -3;
}

static void hdr_pack(const struct ofdm_fhdr *h, uint8_t b[8])
{
	uint16_t crc;

	b[0] = (uint8_t)(((h->type & 1U) << 7) | ((unsigned int)h->mod << 4) | (h->fec & 0x0fU));
	b[1] = h->dst;
	b[2] = h->src;
	b[3] = (uint8_t)h->seq;
	b[4] = (uint8_t)(h->seq >> 8);
	b[5] = HDR_VERSION;
	crc = qam_crc16(b, 6);
	b[6] = (uint8_t)crc;
	b[7] = (uint8_t)(crc >> 8);
}

static bool hdr_unpack(const uint8_t b[8], struct ofdm_fhdr *h)
{
	if (b[5] != HDR_VERSION || qam_crc16(b, 6) != (uint16_t)(b[6] | (b[7] << 8)) ||
	    (b[0] & 0x0fU) >= OFDM_FECS || ((b[0] >> 4) & 7U) >= OFDM_MODS) {
		return false;
	}
	h->type = b[0] >> 7;
	h->mod = (enum ofdm_mod)((b[0] >> 4) & 7U);
	h->fec = b[0] & 0x0fU;
	h->dst = b[1];
	h->src = b[2];
	h->seq = (uint16_t)(b[3] | (b[4] << 8));
	return true;
}

/* Unit u's byte p as sent (data, then parity), inside its 255 byte buffer. */
static inline uint8_t *unit_byte(struct ofdm_fwork *wk, const struct layout *l, unsigned int u,
				 unsigned int p)
{
	return &wk->units[u][RS_N - l->len + p];
}

/* The coded payload's symbols, the time of both stages into tb->prof. */
static size_t symbols(const struct ofdm_ctx *ctx, struct ofdm_txbuf *tb, const struct ofdm_fhdr *h,
		      const uint8_t *hb, struct ofdm_fwork *wk, uint32_t *out, uint32_t t0)
{
	uint32_t t1 = ofdm_clock != NULL ? ofdm_clock() : 0U;
	size_t len = ofdm_tx_build(ctx, tb, h->mod, hb, wk->bytes, out);

	tb->prof[0] = t1 - t0;
	tb->prof[1] = (ofdm_clock != NULL ? ofdm_clock() : 0U) - t1;
	return len;
}

size_t ofdm_frame_build(const struct ofdm_ctx *ctx, struct ofdm_txbuf *tb, struct ofdm_fwork *wk,
			const struct ofdm_fhdr *h, const uint8_t *data, uint32_t *out)
{
	struct layout l;
	uint8_t hb[8];
	uint32_t crc, t0 = ofdm_clock != NULL ? ofdm_clock() : 0U;
	size_t j;

	if (!layout_of(ctx, h->mod, h->fec, &l)) {
		return 0;
	}
	crc = qam_crc32(data, l.user);
	if (h->fec != OFDM_FEC_RS) {
		/* Blocks in place, each followed by its parity. */
		const struct bch_code *c = h->fec == OFDM_FEC_NONE ? NULL : code_for(wk, h->fec);

		if (h->fec != OFDM_FEC_NONE && c == NULL) {
			return 0;
		}
		blocks_put(&l, wk->bytes, data, crc);
		for (unsigned int u = 0; c != NULL && u < l.units; u++) {
			uint8_t *blk = &wk->bytes[(size_t)u * l.len];

			bch_encode(c, blk, l.k, &blk[l.k]);
		}
		j = (size_t)l.units * l.len;
		memset(&wk->bytes[j], 0, l.cap - j + 1U);
		hdr_pack(h, hb);
		return symbols(ctx, tb, h, hb, wk, out, t0);
	}
	/* User data, then its CRC (little endian), over the units' data bytes. */
	for (unsigned int u = 0; u < l.units; u++) {
		memset(wk->units[u], 0, RS_N);
	}
	/* Unit by unit (a division per byte is 15 cycles or more with a divisor that varies). */
	for (unsigned int u = 0, d = 0; u < l.units && d < l.user + 4U; u++) {
		uint8_t *dst = unit_byte(wk, &l, u, 0);

		for (unsigned int pos = 0; pos < l.k && d < l.user + 4U; pos++, d++) {
			dst[pos] = d < l.user ? data[d] : (uint8_t)(crc >> (8U * (d - l.user)));
		}
	}
	for (unsigned int u = 0; u < l.units; u++) {
		rs_encode(wk->units[u]);
	}
	/* Interleaved: byte p of every unit, then byte p + 1. */
	for (unsigned int pos = 0; pos < l.len; pos++) {
		uint8_t *out_p = &wk->bytes[(size_t)pos * l.units];

		for (unsigned int u = 0; u < l.units; u++) {
			out_p[u] = *unit_byte(wk, &l, u, pos);
		}
	}
	j = (size_t)l.units * l.len;
	memset(&wk->bytes[j], 0, l.cap - j + 1U);
	hdr_pack(h, hb);
	return symbols(ctx, tb, h, hb, wk, out, t0);
}

size_t ofdm_frame_build_hdr(const struct ofdm_ctx *ctx, struct ofdm_txbuf *tb,
			    const struct ofdm_fhdr *h, uint32_t *out)
{
	uint8_t hb[8];

	hdr_pack(h, hb);
	return ofdm_tx_build(ctx, tb, OFDM_BPSK, hb, NULL, out);
}

int ofdm_frame_begin_hdr(struct ofdm_ctx *ctx, const uint32_t *words, size_t n,
			 struct ofdm_fhdr *h, struct ofdm_rx_info *info)
{
	uint8_t hb[8];

	if (ofdm_rx_begin_period(ctx, words, n, ofdm_hdr_rsamples(ctx), hb, info) != 0) {
		return -1;
	}
	return hdr_unpack(hb, h) ? 0 : -2;
}

int ofdm_frame_begin(struct ofdm_ctx *ctx, const uint32_t *words, size_t n, struct ofdm_fhdr *h,
		     struct ofdm_rx_info *info)
{
	uint8_t hb[8];

	if (ofdm_rx_begin(ctx, words, n, hb, info) != 0) {
		return -1;
	}
	return hdr_unpack(hb, h) ? 0 : -2;
}

int ofdm_frame_finish(struct ofdm_ctx *ctx, const uint32_t *words, const struct ofdm_fhdr *h,
		      struct ofdm_fwork *wk, uint8_t *data, int *corrected,
		      struct ofdm_rx_info *info)
{
	struct layout l;

	if (!layout_of(ctx, h->mod, h->fec, &l)) {
		*corrected = 0;
		return -3;
	}
	ofdm_rx_finish(ctx, words, h->mod, wk->bytes, info);
	return ofdm_frame_fec(ctx, h, wk, data, corrected);
}

int ofdm_frame_fec_units(const struct ofdm_ctx *ctx, const struct ofdm_fhdr *h,
			 struct ofdm_fwork *wk, unsigned int u0, unsigned int u1, int *corrected)
{
	struct layout l;

	*corrected = 0;
	if (h->fec != OFDM_FEC_RS || !layout_of(ctx, h->mod, h->fec, &l)) {
		return -3;
	}
	u1 = u1 > l.units ? l.units : u1;
	for (unsigned int u = u0; u < u1; u++) {
		/* Decoded on the stack: the work memory may be PSRAM. */
		uint8_t cw[RS_N];
		int r;

		memset(cw, 0, RS_N - l.len);
		for (unsigned int p = 0; p < l.len; p++) {
			cw[RS_N - l.len + p] = wk->bytes[(size_t)p * l.units + u];
		}
		r = rs_decode(cw);
		if (r < 0) {
			return -3;
		}
		/* A miscorrection may land in the virtual zeros. */
		for (unsigned int i = 0; i < RS_N - l.len; i++) {
			if (cw[i] != 0U) {
				return -3;
			}
		}
		memcpy(wk->units[u], cw, RS_N);
		*corrected += r;
	}
	return 0;
}

unsigned int ofdm_frame_units(const struct ofdm_ctx *ctx, enum ofdm_mod mod)
{
	struct layout l;

	return layout_of(ctx, mod, OFDM_FEC_RS, &l) ? l.units : 0U;
}

int ofdm_frame_check(const struct ofdm_ctx *ctx, const struct ofdm_fhdr *h, struct ofdm_fwork *wk,
		     uint8_t *data)
{
	struct layout l;
	uint32_t crc = 0;

	if (!layout_of(ctx, h->mod, h->fec, &l)) {
		return -3;
	}
	if (h->fec != OFDM_FEC_RS) {
		return blocks_get(&l, wk->bytes, data);
	}
	for (unsigned int u = 0, d = 0; u < l.units && d < l.user + 4U; u++) {
		const uint8_t *src = unit_byte(wk, &l, u, 0);

		for (unsigned int pos = 0; pos < l.k && d < l.user + 4U; pos++, d++) {
			if (d < l.user) {
				data[d] = src[pos];
			} else {
				crc |= (uint32_t)src[pos] << (8U * (d - l.user));
			}
		}
	}
	return qam_crc32(data, l.user) == crc ? 0 : -3;
}

int ofdm_frame_fec(const struct ofdm_ctx *ctx, const struct ofdm_fhdr *h, struct ofdm_fwork *wk,
		   uint8_t *data, int *corrected)
{
	const struct bch_code *c;
	struct layout l;
	int ret;

	if (h->fec == OFDM_FEC_RS) {
		ret = ofdm_frame_fec_units(ctx, h, wk, 0, ~0U, corrected);
		return ret != 0 ? ret : ofdm_frame_check(ctx, h, wk, data);
	}
	*corrected = 0;
	if (!layout_of(ctx, h->mod, h->fec, &l)) {
		return -3;
	}
	/* The CRC first: a clean frame (the usual one at a low error rate) needs no decoding. */
	if (blocks_get(&l, wk->bytes, data) == 0) {
		return 0;
	}
	c = h->fec == OFDM_FEC_NONE ? NULL : code_for(wk, h->fec);
	if (c == NULL) {
		return -3;
	}
	for (unsigned int u = 0; u < l.units; u++) {
		uint8_t *blk = &wk->bytes[(size_t)u * l.len];
		int r = bch_decode(c, blk, l.k, &blk[l.k]);

		if (r < 0) {
			return -3;
		}
		*corrected += r;
	}
	/* Nothing to correct, yet the CRC failed: errors the blocks cannot see. */
	return *corrected > 0 ? blocks_get(&l, wk->bytes, data) : -3;
}
