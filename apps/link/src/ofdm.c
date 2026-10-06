/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * OFDM physical layer, see ofdm.h. Transmit: subcarriers into the FFT buffer,
 * an unscaled radix-2 inverse FFT, the cyclic prefix and the body out as DAC
 * words. Receive, straight from the capture words: DC from every 4th sample,
 * a Schmidl-Cox search as exact integer sliding sums (O(n)), the carrier
 * offset from its phase, then per symbol a derotation by phasor steps, a
 * forward FFT and per subcarrier one complex multiply by the equalizer the
 * pilot set up. A common phase per data symbol, measured against its own
 * decisions, follows the residual offset across the frame.
 */

#include <math.h>
#include <string.h>

#include "ofdm.h"

#define PI     3.14159265358979323846f
#define TWO_PI 6.28318530717958647692f

#if defined(__ZEPHYR__)
#include <esp_attr.h>
#define OFDM_HOT IRAM_ATTR __attribute__((noinline))
#else
#define OFDM_HOT
#endif

#define PREAMBLE_SEED 0x5a17e4a1U
#define PILOT_SEED    0xc39a2b7fU
/* Least preamble metric for a detection. */
#define METRIC_MIN 0.3f
/* Plateau edge: the earliest start whose metric is within this of the best. */
#define PLATEAU 0.9f

uint32_t (*ofdm_clock)(void);

static inline uint32_t clk(void)
{
	return ofdm_clock ? ofdm_clock() : 0U;
}

static float pn_sign(uint32_t seed, unsigned int k)
{
	uint32_t x = seed + k * 2654435761U;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return (x & 1U) ? 1.0f : -1.0f;
}

void ofdm_test_bits(uint8_t *bits, size_t nbits, uint32_t seed)
{
	uint32_t x = seed | 1U;

	for (size_t i = 0; i < (nbits + 7U) / 8U; i++) {
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		bits[i] = (uint8_t)x;
	}
}

static inline unsigned int get_bit(const uint8_t *b, size_t i)
{
	return (b[i >> 3] >> (7U - (i & 7U))) & 1U;
}

/* Unit energy points: BPSK on I, QPSK Gray (one bit per axis). */
static inline struct ofdm_cf map_point(enum ofdm_mod mod, unsigned int v)
{
	const float a = 0.70710678f;

	if (mod == OFDM_BPSK) {
		return (struct ofdm_cf){v ? 1.0f : -1.0f, 0.0f};
	}
	return (struct ofdm_cf){(v & 2U) ? a : -a, (v & 1U) ? a : -a};
}

static inline unsigned int slice(enum ofdm_mod mod, struct ofdm_cf z)
{
	return mod == OFDM_BPSK ? (z.re > 0.0f) : ((z.re > 0.0f) << 1) | (z.im > 0.0f);
}

/* Frequency index of subcarrier a: -floor(c/2) .. -1, 1 .. ceil(c/2). */
static inline int sub_freq(unsigned int c, unsigned int a)
{
	int f = (int)a - (int)(c / 2U);

	return f >= 0 ? f + 1 : f;
}

int ofdm_init(struct ofdm_ctx *ctx, const struct ofdm_cfg *cfg)
{
	unsigned int c = cfg->channels, nfft = OFDM_NFFT_MIN, log2n = 4;
	float ideal;

	if (cfg->sample_rate_hz == 0U || cfg->bandwidth_hz == 0U || c < 2U ||
	    cfg->mod >= OFDM_MODS || cfg->cp_div < 4U || cfg->cp_div > OFDM_CP_DIV_MAX) {
		return -1;
	}
	/* The power of two nearest (in octaves) to the size that spaces c subcarriers over bw. */
	ideal = (float)cfg->sample_rate_hz * (float)c / (float)cfg->bandwidth_hz;
	while (nfft < OFDM_NFFT_MAX && (float)nfft * 1.41421356f < ideal) {
		nfft <<= 1;
		log2n++;
	}
	/* Clamped at either end: the bandwidth would be off by more than an octave's half. */
	if ((float)nfft * 1.41421356f < ideal || (float)nfft > ideal * 1.41421356f) {
		return -1;
	}
	/* Leave a quarter of the band free around Nyquist (aliases of the DAC images). */
	if (c > nfft * 3U / 4U || c > OFDM_CH_MAX || nfft / cfg->cp_div == 0U) {
		return -1;
	}
	ctx->cfg = *cfg;
	ctx->nfft = nfft;
	ctx->log2n = log2n;
	ctx->cp = nfft / cfg->cp_div;
	ctx->slot = ctx->cp + nfft;
	ctx->spacing_hz = (float)cfg->sample_rate_hz / (float)nfft;
	ctx->bw_hz = ctx->spacing_hz * (float)c;
	if (cfg->symbols == 0U) {
		if (cfg->max_samples < 3U * ctx->slot) {
			return -1;
		}
		ctx->ndata = (unsigned int)(cfg->max_samples / ctx->slot) - 2U;
	} else {
		ctx->ndata = cfg->symbols;
	}
	ctx->len = (2U + ctx->ndata) * ctx->slot;
	if (ctx->len > cfg->max_samples) {
		return -1;
	}
	/* At half the rate the subcarriers must still fit (three quarters of the band). */
	if ((cfg->rx_div != 1U && cfg->rx_div != 2U) || c > (nfft / cfg->rx_div) * 3U / 4U ||
	    ctx->cp % cfg->rx_div != 0U || nfft / cfg->rx_div < OFDM_NFFT_MIN / 2U) {
		return -1;
	}
	ctx->rnfft = nfft / cfg->rx_div;
	ctx->rcp = ctx->cp / cfg->rx_div;
	ctx->rslot = ctx->rcp + ctx->rnfft;
	ctx->rlen = (2U + ctx->ndata) * ctx->rslot;

	for (unsigned int i = 0; i < nfft; i++) {
		unsigned int r = 0;

		for (unsigned int b = 0; b < log2n; b++) {
			r |= ((i >> b) & 1U) << (log2n - 1U - b);
		}
		ctx->rev[i] = (uint16_t)r;
	}
	for (unsigned int a = 0; a < c; a++) {
		int f = sub_freq(c, a);

		unsigned int rb = (unsigned int)(f < 0 ? f + (int)(nfft / cfg->rx_div) : f);

		ctx->bin[a] = (uint16_t)(f < 0 ? f + (int)nfft : f);
		/* Bit reversal over fewer bits: the top one of a smaller index is zero. */
		ctx->binr[a] = (uint16_t)(ctx->rev[rb] >> (cfg->rx_div - 1U));
	}
	/* esp-dsp radix-2 table: cos, sin of 2 pi i / nfft for i < nfft / 2, in bit reversed order. */
	for (unsigned int i = 0; i < nfft / 2U; i++) {
		unsigned int r = ctx->rev[i] >> 1;
		float a = TWO_PI * (float)r / (float)nfft;

		ctx->w[2U * i] = cosf(a);
		ctx->w[2U * i + 1U] = sinf(a);
	}
	return 0;
}

/*
 * Forward FFT in place, unscaled: natural order in, bit reversed out (read
 * through ctx->rev / ctx->binr). The esp-dsp S3 kernel, or the same algorithm
 * in C (host, and the reference it is checked against).
 */
#if defined(CONFIG_APP_OFDM_ESP_DSP)
extern int dsps_fft2r_fc32_aes3_(float *data, int N, float *w);

static inline void fft(struct ofdm_ctx *ctx, unsigned int n)
{
	(void)dsps_fft2r_fc32_aes3_((float *)ctx->buf, (int)n, ctx->w);
}
#else
OFDM_HOT static void fft(struct ofdm_ctx *ctx, unsigned int n)
{
	float *d = (float *)ctx->buf;
	const float *w = ctx->w;

	for (unsigned int n2 = n / 2U, ie = 1; n2 > 0; n2 >>= 1, ie <<= 1) {
		unsigned int ia = 0;

		for (unsigned int j = 0; j < ie; j++, ia += n2) {
			float c = w[2U * j], s = w[2U * j + 1U];

			for (unsigned int i = 0; i < n2; i++, ia++) {
				float *a = &d[2U * ia], *b = &d[2U * (ia + n2)];
				float tr = c * b[0] + s * b[1], ti = c * b[1] - s * b[0];

				b[0] = a[0] - tr;
				b[1] = a[1] - ti;
				a[0] += tr;
				a[1] += ti;
			}
		}
	}
}
#endif

void ofdm_test_fft(struct ofdm_ctx *ctx)
{
	fft(ctx, ctx->nfft);
}

/*
 * One slot out of the FFT buffer: the inverse transform is the forward one of
 * the conjugate, conjugated again here (Q negated); the samples are read
 * through the bit reversal. Cyclic prefix first, scaled and clipped.
 */
OFDM_HOT static void emit(struct ofdm_ctx *ctx, float g, uint32_t *out)
{
	unsigned int n = ctx->nfft;
	uint32_t clipped = 0;

	for (unsigned int i = 0; i < ctx->slot; i++) {
		const struct ofdm_cf *s = &ctx->buf[ctx->rev[(i + n - ctx->cp) & (n - 1U)]];
		int vi = (int)lrintf(s->re * g), vq = (int)lrintf(-s->im * g);

		if (vi > 511 || vi < -511 || vq > 511 || vq < -511) {
			clipped++;
			vi = vi > 511 ? 511 : (vi < -511 ? -511 : vi);
			vq = vq > 511 ? 511 : (vq < -511 ? -511 : vq);
		}
		out[i] = ofdm_tx_word(vi, vq);
	}
	ctx->clipped += clipped;
}

size_t ofdm_tx_build(struct ofdm_ctx *ctx, const uint8_t *bits, uint32_t *out)
{
	unsigned int c = ctx->cfg.channels, bps = ofdm_mod_bits(ctx->cfg.mod);
	/* The unscaled inverse FFT of c unit subcarriers has RMS sqrt(c). */
	float g = (float)ctx->cfg.amp / sqrtf((float)c);
	size_t bi = 0;

	ctx->clipped = 0;
	for (unsigned int s = 0; s < 2U + ctx->ndata; s++) {
		memset(ctx->buf, 0, ctx->nfft * sizeof(ctx->buf[0]));
		for (unsigned int a = 0; a < c; a++) {
			struct ofdm_cf *x = &ctx->buf[ctx->bin[a]];

			if (s == 0) {
				/* Even subcarriers only, at twice the power: the same RMS. */
				if ((sub_freq(c, a) & 1) == 0) {
					*x = (struct ofdm_cf){1.41421356f * pn_sign(PREAMBLE_SEED, a),
							      0.0f};
				}
			} else if (s == 1) {
				*x = (struct ofdm_cf){pn_sign(PILOT_SEED, a), 0.0f};
			} else {
				unsigned int v = 0;

				for (unsigned int b = 0; b < bps; b++, bi++) {
					v = (v << 1) | get_bit(bits, bi);
				}
				*x = map_point(ctx->cfg.mod, v);
				/* Conjugated in: see emit(). */
				x->im = -x->im;
			}
		}
		fft(ctx, ctx->nfft);
		emit(ctx, g, &out[s * ctx->slot]);
	}
	return ctx->len;
}

struct sc_best {
	float num, den;
	int32_t pr, pi;
	int t, start;
};

/*
 * Schmidl-Cox scan over t = t0, t0 + st, .. t1: P(t) = sum conj(x[t + m])
 * x[t + m + L] and both halves' energies over m = 0, st, .. < L, as exact
 * integer sliding sums of the DC-free samples (the preamble's period holds on
 * any subsampling). The metric |P|^2 / (Ra Rb) is at most 1 and compared by
 * cross multiplication (the S3 FPU has no divide). At full resolution (st 1)
 * the cyclic prefix widens the peak to a plateau; best->start is its early
 * edge (later ones cut into the next symbol), from a ring of the last cp + 1
 * metrics.
 */
OFDM_HOT static void sc_scan(struct ofdm_ctx *ctx, const uint32_t *w, int t0, int t1, int st,
			     int dci, int dcq, struct sc_best *b)
{
	enum { MASK = OFDM_RING - 1U };
	int L = (int)ctx->rnfft / 2, cp = (int)ctx->rcp;
	int32_t pr = 0, pi = 0, ra = 0, rb = 0;
	float *rnum = ctx->rnum, *rden = ctx->rden;

	_Static_assert(OFDM_RING > OFDM_NFFT_MAX / 4U && (OFDM_RING & (OFDM_RING - 1U)) == 0U,
		       "ring: a power of two longer than the longest prefix");
	b->num = 0.0f;
	b->den = 1.0f;
	b->t = b->start = -1;
	for (int m = 0; m < L; m += st) {
		uint32_t wa = w[t0 + m], wb = w[t0 + m + L];
		int ai = ofdm_rx_i(wa) - dci, aq = ofdm_rx_q(wa) - dcq;
		int bi = ofdm_rx_i(wb) - dci, bq = ofdm_rx_q(wb) - dcq;

		pr += ai * bi + aq * bq;
		pi += ai * bq - aq * bi;
		ra += ai * ai + aq * aq;
		rb += bi * bi + bq * bq;
	}
	for (int t = t0;; t += st) {
		float fr = (float)pr, fi = (float)pi;
		float num = fr * fr + fi * fi, den = (float)ra * (float)rb;

		if (st == 1) {
			rnum[t & MASK] = num;
			rden[t & MASK] = den;
		}
		/* num / den > b->num / b->den */
		if (num * b->den > b->num * den) {
			b->num = num;
			b->den = den;
			b->pr = pr;
			b->pi = pi;
			b->t = b->start = t;
			for (int k = 1; st == 1 && k <= cp && t - k >= t0; k++) {
				int j = (t - k) & MASK;

				if (rnum[j] * b->den < PLATEAU * b->num * rden[j]) {
					break;
				}
				b->start = t - k;
			}
		}
		if (t + st > t1) {
			break;
		}
		{
			uint32_t wa = w[t], wb = w[t + L], wc = w[t + 2 * L];
			int ai = ofdm_rx_i(wa) - dci, aq = ofdm_rx_q(wa) - dcq;
			int bi = ofdm_rx_i(wb) - dci, bq = ofdm_rx_q(wb) - dcq;
			int ci = ofdm_rx_i(wc) - dci, cq = ofdm_rx_q(wc) - dcq;
			int eb = bi * bi + bq * bq;

			pr += (bi * ci + bq * cq) - (ai * bi + aq * bq);
			pi += (bi * cq - bq * ci) - (ai * bq - aq * bi);
			ra += eb - (ai * ai + aq * aq);
			rb += (ci * ci + cq * cq) - eb;
		}
	}
}

/*
 * The frame start in t = 0 .. tmax: a coarse scan on every 4th sample (both
 * the position and the correlation, which keeps at least 32 terms), then a
 * full resolution one over the prefix around its peak.
 */
static int search(struct ofdm_ctx *ctx, const uint32_t *w, int tmax, int dci, int dcq,
		  float *best_out, float *ang_out)
{
	int L = (int)ctx->rnfft / 2, st = L >= 128 ? 4 : (L >= 64 ? 2 : 1), lo, hi;
	struct sc_best c, f;

	sc_scan(ctx, w, 0, tmax, st, dci, dcq, &c);
	if (c.t < 0) {
		return -1;
	}
	/* The coarse peak is somewhere on the plateau, give or take a step. */
	lo = c.t - (int)ctx->rcp - st;
	hi = c.t + st;
	lo = lo < 0 ? 0 : lo;
	hi = hi > tmax ? tmax : hi;
	sc_scan(ctx, w, lo, hi, 1, dci, dcq, &f);
	*best_out = f.den > 0.0f ? f.num / f.den : 0.0f;
	*ang_out = atan2f((float)f.pi, (float)f.pr);
	return f.start;
}

/*
 * Derotation over one receive FFT window: rot[i] = exp(-j 2 pi eps i). The
 * phase at each window's first sample is the same for all its subcarriers, so
 * it goes into the tracked phase instead (see ofdm_rx_decode()).
 */
static void rot_init(struct ofdm_ctx *ctx, float eps)
{
	float sr = cosf(-TWO_PI * eps), si = sinf(-TWO_PI * eps), rr = 1.0f, ri = 0.0f;

	for (unsigned int i = 0; i < ctx->rnfft; i++) {
		float t;

		/* Exact every 64 samples, steps in between. */
		if ((i & 63U) == 0U) {
			rr = cosf(-TWO_PI * eps * (float)i);
			ri = sinf(-TWO_PI * eps * (float)i);
		}
		ctx->rot[i] = (struct ofdm_cf){rr, ri};
		t = rr * sr - ri * si;
		ri = rr * si + ri * sr;
		rr = t;
	}
}

/*
 * Slot s of the frame at @p start into the FFT buffer, DC removed and
 * derotated, the window cp/4 early inside the prefix (a late start then still
 * stays clear of the next symbol).
 */
OFDM_HOT static void take_slot(struct ofdm_ctx *ctx, const uint32_t *w, int start, unsigned int s,
			       int dci, int dcq)
{
	const uint32_t *__restrict x =
		&w[start + (int)(s * ctx->rslot) + (int)ctx->rcp - (int)(ctx->rcp / 4U)];
	const struct ofdm_cf *__restrict r = ctx->rot;
	struct ofdm_cf *__restrict b = ctx->buf;
	unsigned int n = ctx->rnfft;

	for (unsigned int i = 0; i < n; i++) {
		uint32_t v = x[i];
		float xr = (float)(ofdm_rx_i(v) - dci), xq = (float)(ofdm_rx_q(v) - dcq);
		float rr = r[i].re, ri = r[i].im;

		b[i].re = xr * rr - xq * ri;
		b[i].im = xr * ri + xq * rr;
	}
}

/* Decided bits, packed MSB first and written a byte at a time (the buffer may be in PSRAM). */
struct bitsink {
	uint8_t *out;
	uint32_t acc;
	unsigned int n;
};

static inline void sink_put(struct bitsink *k, unsigned int v, unsigned int nb)
{
	k->acc = (k->acc << nb) | v;
	k->n += nb;
	if (k->n >= 8U) {
		k->n -= 8U;
		*k->out++ = (uint8_t)(k->acc >> k->n);
	}
}

static inline void sink_flush(struct bitsink *k)
{
	if (k->n > 0U) {
		*k->out = (uint8_t)(k->acc << (8U - k->n));
		k->n = 0;
	}
}

/*
 * One data symbol: equalized and rotated by the phase tracked so far and this
 * window's known one (u); this symbol's own residual rotation (against its
 * decisions) is folded into the tracked phase, then the decisions are made
 * again and counted. Recomputed rather than stored: one complex multiply more
 * per subcarrier, no second buffer.
 */
OFDM_HOT static void data_symbol(struct ofdm_ctx *ctx, struct ofdm_cf *trk, struct ofdm_cf u,
				 struct bitsink *k, double *sig, double *err)
{
	enum ofdm_mod mod = ctx->cfg.mod;
	unsigned int c = ctx->cfg.channels, bps = ofdm_mod_bits(mod);
	const struct ofdm_cf *__restrict y = ctx->buf, *__restrict e = ctx->eq;
	const uint16_t *__restrict br = ctx->binr;
	float tr = trk->re * u.re - trk->im * u.im, ti = trk->re * u.im + trk->im * u.re;
	float ar = 0.0f, ai = 0.0f, m, e2 = 0.0f;

	for (unsigned int a = 0; a < c; a++) {
		struct ofdm_cf v = y[br[a]], q = e[a], z, d;
		float zr = v.re * q.re - v.im * q.im, zi = v.re * q.im + v.im * q.re;

		z.re = zr * tr - zi * ti;
		z.im = zr * ti + zi * tr;
		d = map_point(mod, slice(mod, z));
		ar += z.re * d.re + z.im * d.im;
		ai += z.im * d.re - z.re * d.im;
	}
	m = sqrtf(ar * ar + ai * ai);
	if (m > 0.0f) {
		float g = 1.0f / m, cr = ar * g, ci = -ai * g, t = tr * cr - ti * ci;

		ti = tr * ci + ti * cr;
		tr = t;
		t = trk->re * cr - trk->im * ci;
		trk->im = trk->re * ci + trk->im * cr;
		trk->re = t;
	}
	for (unsigned int a = 0; a < c; a++) {
		struct ofdm_cf v = y[br[a]], q = e[a], z, d;
		float zr = v.re * q.re - v.im * q.im, zi = v.re * q.im + v.im * q.re;
		unsigned int s;

		z.re = zr * tr - zi * ti;
		z.im = zr * ti + zi * tr;
		s = slice(mod, z);
		d = map_point(mod, s);
		e2 += (z.re - d.re) * (z.re - d.re) + (z.im - d.im) * (z.im - d.im);
		sink_put(k, s, bps);
	}
	*sig += (double)c;
	*err += (double)e2;
}

int ofdm_rx_decode(struct ofdm_ctx *ctx, const uint32_t *words, size_t n, uint8_t *bits,
		   struct ofdm_rx_info *info)
{
	int32_t si = 0, sq = 0, cnt = 0;
	int tmax = (int)n - (int)ctx->rlen, start, dci, dcq;
	float best = 0.0f, ang = 0.0f, eps, a;
	struct ofdm_cf trk = {1.0f, 0.0f}, u = {1.0f, 0.0f}, us;
	struct bitsink k = {.out = bits};
	double sig = 0.0, err = 0.0;
	uint32_t t0 = clk(), t1, t2;

	memset(info, 0, sizeof(*info));
	if (tmax < 0) {
		return -1;
	}
	/* The frame repeats every rlen samples: one period (plus its prefix plateau) holds a start. */
	if (tmax > (int)(ctx->rlen + ctx->rcp)) {
		tmax = (int)(ctx->rlen + ctx->rcp);
	}
	for (size_t k = 0; k < n; k += 4) {
		si += ofdm_rx_i(words[k]);
		sq += ofdm_rx_q(words[k]);
		cnt++;
	}
	dci = (si + (si >= 0 ? cnt / 2 : -cnt / 2)) / cnt;
	dcq = (sq + (sq >= 0 ? cnt / 2 : -cnt / 2)) / cnt;
	start = search(ctx, words, tmax, dci, dcq, &best, &ang);
	t1 = clk();
	info->prof[0] = t1 - t0;
	info->metric = best;
	if (start < 0 || best < METRIC_MIN) {
		return -1;
	}
	/* Phase over half a symbol: pi per subcarrier spacing. */
	info->cfo_frac = ang / PI;
	info->cfo_hz = info->cfo_frac * ctx->spacing_hz;
	info->start = start;
	/* Cycles per receive sample. */
	eps = info->cfo_frac / (float)ctx->rnfft;
	rot_init(ctx, eps);

	/* Channel from the pilot: the known +-1 sign undone, then conj(H) / |H|^2. */
	take_slot(ctx, words, start, 1, dci, dcq);
	fft(ctx, ctx->rnfft);
	for (unsigned int k = 0; k < ctx->cfg.channels; k++) {
		struct ofdm_cf y = ctx->buf[ctx->binr[k]];
		float p = pn_sign(PILOT_SEED, k), h2 = y.re * y.re + y.im * y.im;

		ctx->eq[k] = h2 > 0.0f ? (struct ofdm_cf){p * y.re / h2, -p * y.im / h2}
				       : (struct ofdm_cf){0.0f, 0.0f};
	}
	t2 = clk();
	info->prof[1] = t2 - t1;

	/*
	 * Each window lacks its first sample's derotation, exp(-j 2 pi eps base),
	 * which advances by 2 pi eps rslot per slot: against the pilot's, data
	 * symbol s comes out turned by exp(+j 2 pi eps rslot (s + 1)), undone by u.
	 */
	a = -TWO_PI * eps * (float)ctx->rslot;
	us = (struct ofdm_cf){cosf(a), sinf(a)};
	for (unsigned int s = 0; s < ctx->ndata; s++) {
		float t = u.re * us.re - u.im * us.im;

		u.im = u.re * us.im + u.im * us.re;
		u.re = t;
		take_slot(ctx, words, start, 2U + s, dci, dcq);
		fft(ctx, ctx->rnfft);
		data_symbol(ctx, &trk, u, &k, &sig, &err);
	}
	sink_flush(&k);
	info->prof[2] = clk() - t2;
	info->mer_db = 10.0f * log10f((float)(sig / (err > 1e-12 ? err : 1e-12)));
	return 0;
}
