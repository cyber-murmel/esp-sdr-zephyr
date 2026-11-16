/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * OFDM physical layer, see ofdm.h. Transmit: subcarriers into the FFT buffer,
 * an unscaled radix-2 inverse FFT, the cyclic prefix and the body out as DAC
 * words. Receive, straight from the capture words: DC from every 4th sample,
 * a Schmidl-Cox search as exact integer sliding sums, the carrier offset from
 * its phase, then per symbol a derotation by table, a forward FFT and per
 * subcarrier one complex multiply by the equalizer the pilots set up. A
 * common phase per symbol, measured against its own decisions, follows the
 * residual offset across the frame.
 */

#include <math.h>
#include <string.h>

#include <esp_sdr/esp_sdr_dsp.h>

#include "ofdm.h"

#if defined(__ZEPHYR__)
#include <esp_attr.h>
#include <zephyr/kernel.h>
#define OFDM_HOT IRAM_ATTR __attribute__((noinline))
#else
#define OFDM_HOT
#endif

/* Input scale of the 16-bit receive FFT (ofdm_init(), take_slot()). */
#define FFT16_GAIN 32.0f

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

/*
 * Square QAM, Gray coded per axis (I bits high), unit average energy: k
 * levels per axis at (2 l - (k - 1)) s. BPSK is +-1 on I alone.
 */
static const struct cons {
	uint8_t k, q;
	float s, inv2s, off;
} cons[OFDM_MODS] = {
	[OFDM_BPSK] = {2, 1, 1.0f, 0.5f, 0.5f},
	[OFDM_QPSK] = {2, 1, 0.70710678f, 0.70710678f, 0.5f},
	[OFDM_16QAM] = {4, 2, 0.31622777f, 1.58113883f, 1.5f},
	[OFDM_64QAM] = {8, 3, 0.15430335f, 3.24037035f, 3.5f},
};

/* Gray code to level index. */
static const uint8_t gray_dec[8] = {0, 1, 3, 2, 7, 6, 4, 5};

static inline struct ofdm_cf map_point(enum ofdm_mod mod, unsigned int v)
{
	const struct cons *k = &cons[mod];

	if (mod == OFDM_BPSK) {
		return (struct ofdm_cf){v ? 1.0f : -1.0f, 0.0f};
	}
	return (struct ofdm_cf){(float)(2 * gray_dec[v >> k->q] - (k->k - 1)) * k->s,
				(float)(2 * gray_dec[v & (k->k - 1U)] - (k->k - 1)) * k->s};
}

/* Nearest level on one axis: its Gray code, and the level through @p d. */
static inline unsigned int slice_axis(const struct cons *k, float x, float *d)
{
	float t = x * k->inv2s + k->off + 0.5f;
	int l = t < 0.0f ? 0 : (int)t;

	l = l > k->k - 1 ? k->k - 1 : l;
	*d = (float)(2 * l - (k->k - 1)) * k->s;
	return (unsigned int)(l ^ (l >> 1));
}

/* The bits of the nearest point to @p z, and that point through @p d. */
static inline unsigned int decide(enum ofdm_mod mod, struct ofdm_cf z, struct ofdm_cf *d)
{
	const struct cons *k = &cons[mod];
	unsigned int gi, gq;

	if (mod == OFDM_BPSK) {
		d->re = z.re > 0.0f ? 1.0f : -1.0f;
		d->im = 0.0f;
		return z.re > 0.0f;
	}
	gi = slice_axis(k, z.re, &d->re);
	gq = slice_axis(k, z.im, &d->im);
	return (gi << k->q) | gq;
}

/* Frequency index of subcarrier a: -floor(c/2) .. -1, 1 .. ceil(c/2). */
static inline int sub_freq(unsigned int c, unsigned int a)
{
	int f = (int)a - (int)(c / 2U);

	return f >= 0 ? f + 1 : f;
}

int ofdm_init(struct ofdm_ctx *ctx, const struct ofdm_cfg *cfg)
{
	unsigned int c = cfg->channels, nfft = OFDM_NFFT_MIN, log2n = 4, nsym, p = cfg->pilots;
	float ideal;

	if (cfg->sample_rate_hz == 0U || cfg->bandwidth_hz == 0U || c < 2U || cfg->cp_div < 4U ||
	    cfg->cp_div > OFDM_CP_DIV_MAX || p < 1U || p > OFDM_PILOTS_MAX ||
	    cfg->hdr_bits > OFDM_HDR_BITS_MAX) {
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
		if (cfg->max_samples < (2U + p) * ctx->slot) {
			return -1;
		}
		nsym = (unsigned int)(cfg->max_samples / ctx->slot) - 1U - p;
	} else {
		nsym = cfg->symbols;
	}
	ctx->nhdr = (cfg->hdr_bits + c - 1U) / c;
	if (nsym <= ctx->nhdr + (cfg->mid_pilot ? 3U : 0U)) {
		return -1;
	}
	ctx->ndata = nsym - ctx->nhdr - (cfg->mid_pilot ? 1U : 0U);
	ctx->mid = ctx->ndata;
	if (cfg->mid_pilot) {
		/* Near the middle, the second half starting a byte at any modulation. */
		for (unsigned int m = ctx->ndata / 2U; m > 0U; m--) {
			if ((m * c) % 8U == 0U) {
				ctx->mid = m;
				break;
			}
		}
		if (ctx->mid == ctx->ndata) {
			return -1;
		}
	}
	ctx->len = (1U + p + nsym) * ctx->slot;
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
	ctx->rlen = (1U + p + nsym) * ctx->rslot;

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
	/* esp-dsp radix-2 table: cos, sin of tau i / nfft for i < nfft / 2, bit reversed. */
	for (unsigned int i = 0; i < nfft / 2U; i++) {
		unsigned int r = ctx->rev[i] >> 1;
		float a = ESP_SDR_TAU_F * (float)r / (float)nfft;

		ctx->w[2U * i] = cosf(a);
		ctx->w[2U * i + 1U] = sinf(a);
		ctx->w16[2U * i] = (int16_t)lrintf(32767.0f * ctx->w[2U * i]);
		ctx->w16[2U * i + 1U] = (int16_t)lrintf(32767.0f * ctx->w[2U * i + 1U]);
	}
	ctx->fscale = (float)ctx->rnfft / FFT16_GAIN;
	/* The context may sit in RAM that is not cleared at boot. */
	ctx->sc_err = NULL;
	ctx->havg_n = 0;
	ctx->w0.e16_src = NULL;
	ctx->rx.dslot = 0.0f;
	ctx->veq[0].sar = 0;
	ctx->veq[1].sar = 0;
	return 0;
}

/*
 * Forward FFT of @p buf in place, unscaled: natural order in, bit reversed out
 * (read through ctx->rev / ctx->binr). The esp-dsp S3 kernel, or the same
 * algorithm in C (host, and the reference it is checked against).
 */
#ifndef OFDM_FFT16
#define OFDM_FFT16 1
#endif

#if defined(CONFIG_APP_OFDM_ESP_DSP)
extern int dsps_fft2r_fc32_aes3_(float *data, int N, float *w);
extern int dsps_fft2r_sc16_aes3_(int16_t *data, int N, int16_t *w);

/*
 * What the esp-dsp 16-bit kernel expects of its surroundings: it checks that the power of
 * two N is one and that its table (here ours, passed in) was initialised.
 */
uint8_t dsps_fft2r_sc16_initialized = 1;

/* Hidden: the kernel's call8 cannot take a dynamic symbol (multiapp's link extension). */
__attribute__((visibility("hidden"))) int dsp_is_power_of_two(int x)
{
	return x > 0 && (x & (x - 1)) == 0;
}

/*
 * The receive FFT in 16 bit: the input is scaled by FFT16_GAIN (10 bit samples, so 16 bit
 * after derotation with room to spare), every stage halves, so the output is X * G / N.
 * In place on the worker's buffer, bit reversed like the float one.
 */
static inline void pie_enable(void)
{
	uint32_t cp;

	__asm__ volatile("rsr.cpenable %0" : "=r"(cp));
	cp |= 1U << 3;
	__asm__ volatile("wsr.cpenable %0\n\trsync" : : "r"(cp));
}

/*
 * The vector registers are not part of the thread context: no other thread may run on
 * this CPU while they are live. Interrupts masked on this CPU for the 7 us (a scheduler
 * lock would take the global spinlock of an SMP kernel).
 */
static inline void fft16(const struct ofdm_ctx *ctx, int16_t *q, unsigned int n)
{
	unsigned int key = arch_irq_lock();

	pie_enable();
	(void)dsps_fft2r_sc16_aes3_(q, (int)n, (int16_t *)ctx->w16);
	arch_irq_unlock(key);
}

static inline void fft(const struct ofdm_ctx *ctx, struct ofdm_cf *buf, unsigned int n)
{
	(void)dsps_fft2r_fc32_aes3_((float *)buf, (int)n, (float *)ctx->w);
}
#else
OFDM_HOT static void fft(const struct ofdm_ctx *ctx, struct ofdm_cf *buf, unsigned int n)
{
	float *d = (float *)buf;
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
	fft(ctx, ctx->w0.buf, ctx->nfft);
}

/*
 * One slot out of the FFT buffer: the inverse transform is the forward one of
 * the conjugate, conjugated again here (Q negated); the samples are read
 * through the bit reversal. Cyclic prefix first, scaled and clipped.
 */
OFDM_HOT static void emit(const struct ofdm_ctx *ctx, struct ofdm_txbuf *tb, float g,
			  uint32_t *out)
{
	unsigned int n = ctx->nfft;
	uint32_t clipped = 0;

	for (unsigned int i = 0; i < ctx->slot; i++) {
		const struct ofdm_cf *s = &tb->buf[ctx->rev[(i + n - ctx->cp) & (n - 1U)]];
		float fi = s->re * g, fq = -s->im * g;
		/* Rounded inline: lrintf() is a library call (in flash on the target). */
		int vi = (int)(fi + (fi >= 0.0f ? 0.5f : -0.5f));
		int vq = (int)(fq + (fq >= 0.0f ? 0.5f : -0.5f));

		if (vi > 511 || vi < -511 || vq > 511 || vq < -511) {
			clipped++;
			vi = vi > 511 ? 511 : (vi < -511 ? -511 : vi);
			vq = vq > 511 ? 511 : (vq < -511 ? -511 : vq);
		}
		out[i] = esp_sdr_tx_word(vi, vq);
	}
	tb->clipped += clipped;
}

#if defined(CONFIG_APP_OFDM_ESP_DSP) && OFDM_FFT16
/*
 * The transmit side on the 16-bit kernel: subcarrier values scaled to S, the output (the
 * transform over nfft, so S / nfft per unit subcarrier) a power of two K times the DAC value,
 * K the biggest of 8, 4, 2, 1 that keeps the largest input (the preamble's sqrt(2) points
 * and a margin) inside 16 bit. Bits are read a byte at a time, the constellation through a
 * level table by the Gray coded value.
 */
struct bitrd {
	const uint8_t *p;
	uint32_t acc;
	unsigned int n;
};

static inline unsigned int rd_bits(struct bitrd *r, unsigned int nb)
{
	while (r->n < nb) {
		r->acc = (r->acc << 8) | *r->p++;
		r->n += 8U;
	}
	r->n -= nb;
	return (r->acc >> r->n) & ((1U << nb) - 1U);
}

/*
 * Samples n0 .. n0 + cnt - 1 (natural order, read through the bit reversal) of the 16-bit
 * FFT output as DAC words: a constant shift (rounded), clipped to +-511, Q negated.
 */
static inline __attribute__((always_inline)) uint32_t emit_run(const uint32_t *__restrict q32,
							       const uint16_t *__restrict rev,
							       unsigned int n0, unsigned int cnt,
							       const int sh,
							       uint32_t *__restrict out)
{
	const int rnd = 1 << (sh - 1);
	uint32_t clipped = 0;

	for (unsigned int i = 0; i < cnt; i++) {
		uint32_t w = q32[rev[n0 + i]];
		int vi = ((int)(int16_t)w + rnd) >> sh;
		int vq = (rnd - (int)(int16_t)(w >> 16)) >> sh;
		int ci = vi > 511 ? 511 : (vi < -511 ? -511 : vi);
		int cq = vq > 511 ? 511 : (vq < -511 ? -511 : vq);

		clipped += (uint32_t)((ci != vi) | (cq != vq));
		out[i] = ((uint32_t)ci & 0x3ffU) | (((uint32_t)cq & 0x3ffU) << 10);
	}
	return clipped;
}

/*
 * One slot out of the 16-bit FFT buffer, prefix first: its clipped samples. A loop per
 * shift (the shift a constant: no shift register set per sample) and from flash (internal
 * RAM is short; the loop stays in the cache).
 */
static __attribute__((noinline)) uint32_t emit16(const struct ofdm_ctx *ctx, const int16_t *q,
						 unsigned int shift, uint32_t *out)
{
	const uint32_t *q32 = (const uint32_t *)q;
	const uint16_t *rev = ctx->rev;
	unsigned int n = ctx->nfft, cp = ctx->cp;

	switch (shift) {
	case 1:
		return emit_run(q32, rev, n - cp, cp, 1, out) + emit_run(q32, rev, 0, n, 1, &out[cp]);
	case 2:
		return emit_run(q32, rev, n - cp, cp, 2, out) + emit_run(q32, rev, 0, n, 2, &out[cp]);
	default:
		return emit_run(q32, rev, n - cp, cp, 3, out) + emit_run(q32, rev, 0, n, 3, &out[cp]);
	}
}

/*
 * One frame's symbols. A shared build (the work buffer given to ofdm_tx_share()) hands its
 * slots out to helpers on other CPUs (ofdm_tx_help()): word holds the build's generation (high
 * 16 bits) and the next slot (low 16), claimed by compare and swap, so a slot always belongs to
 * the build it was claimed from; that build waits for all its slots before it returns, and
 * the next one closes the word before it changes the fields.
 */
struct txjob {
	const struct ofdm_ctx *ctx;
	enum ofdm_mod mod;
	const uint8_t *hdr, *bits;
	uint32_t *out;
	unsigned int shift, nslots, h0, d0, mp;
	int16_t one, root2, lv[8];
	uint32_t word, done, clipped;
};

static struct txjob shared_job;
static struct ofdm_txbuf *shared_tb;
/* The last build the helper helped. */
static uint32_t help_gen;

/* Slot s of the job into the 16-bit FFT buffer @p q, transformed and emitted: its clipped samples
 * (from flash, like the whole build was: internal RAM is short). */
/* A subcarrier's bits to its packed 16-bit point (I low, Q high, conjugated: see emit()). */
static void tx_lut(const struct txjob *j, uint32_t lut[64])
{
	unsigned int bps = ofdm_mod_bits(j->mod), qb = cons[j->mod].q, mask = (1U << qb) - 1U;

	for (unsigned int v = 0; v < (1U << bps); v++) {
		int16_t i = j->mod == OFDM_BPSK ? j->lv[v] : j->lv[v >> qb];
		int16_t q = j->mod == OFDM_BPSK ? 0 : (int16_t)-j->lv[v & mask];

		lut[v] = (uint32_t)(uint16_t)i | ((uint32_t)(uint16_t)q << 16);
	}
}

static __attribute__((noinline)) uint32_t tx_slot16(const struct txjob *j, const uint32_t *lut,
						    int16_t *q, unsigned int s)
{
	const struct ofdm_ctx *ctx = j->ctx;
	unsigned int c = ctx->cfg.channels, bps = ofdm_mod_bits(j->mod), nh = ctx->cfg.hdr_bits;
	memset(q, 0, ctx->nfft * 2U * sizeof(int16_t));
	if (s >= j->d0 && s != j->mp) {
		/*
		 * Payload symbol i (the mid pilot's slot skipped) starts at bit i c bps: a byte at a
		 * time into the bit buffer, a table lookup and one store per subcarrier.
		 */
		size_t i = s - j->d0 - (s > j->mp ? 1U : 0U), bp = i * c * bps;
		const uint8_t *__restrict p = &j->bits[bp >> 3];
		const uint16_t *__restrict bin = ctx->bin;
		uint32_t *__restrict q32 = (uint32_t *)q;
		uint32_t acc = 0, m = (1U << bps) - 1U;
		unsigned int nb = 0;

		if ((bp & 7U) != 0U) {
			nb = 8U - (unsigned int)(bp & 7U);
			acc = *p++ & ((1U << nb) - 1U);
		}
		for (unsigned int a = 0; a < c; a++) {
			if (nb < bps) {
				acc = (acc << 8) | *p++;
				nb += 8U;
			}
			nb -= bps;
			q32[bin[a]] = lut[(acc >> nb) & m];
		}
	}
	for (unsigned int a = 0; s < j->d0 && a < c; a++) {
		int16_t *x = &q[2U * ctx->bin[a]];

		if (s == 0) {
			if ((sub_freq(c, a) & 1) == 0) {
				x[0] = pn_sign(PREAMBLE_SEED, a) > 0.0f ? j->root2 : (int16_t)-j->root2;
			}
		} else if (s < j->h0) {
			x[0] = pn_sign(PILOT_SEED, a) > 0.0f ? j->one : (int16_t)-j->one;
		} else {
			x[0] = get_bit(j->hdr, ((s - j->h0) * c + a) % nh) ? j->one : (int16_t)-j->one;
		}
	}
	if (s == j->mp) {
		for (unsigned int a = 0; a < c; a++) {
			q[2U * ctx->bin[a]] = pn_sign(PILOT_SEED, a) > 0.0f ? j->one : (int16_t)-j->one;
		}
	}
	fft16(ctx, q, ctx->nfft);
	return emit16(ctx, q, j->shift, &j->out[s * ctx->slot]);
}

/* Claim and build slots of the build @p gen until none are left: false if it is not current. */
static bool tx_work(struct txjob *j, int16_t *q, uint32_t gen, bool (*keep_going)(void *),
		    void *arg)
{
	uint32_t lut[64];
	bool first = true;

	for (;;) {
		uint32_t v = __atomic_load_n(&j->word, __ATOMIC_ACQUIRE), cl;

		if ((v >> 16) != gen) {
			return false;
		}
		if ((v & 0xffffU) >= j->nslots || (keep_going != NULL && !keep_going(arg))) {
			return true;
		}
		if (!__atomic_compare_exchange_n(&j->word, &v, v + 1U, false, __ATOMIC_ACQ_REL,
						 __ATOMIC_ACQUIRE)) {
			continue;
		}
		if (first) {
			/* This worker's table, once it has a slot of the build. */
			tx_lut(j, lut);
			first = false;
		}
		cl = tx_slot16(j, lut, q, v & 0xffffU);
		if (cl != 0U) {
			__atomic_fetch_add(&j->clipped, cl, __ATOMIC_RELAXED);
		}
		__atomic_fetch_add(&j->done, 1U, __ATOMIC_RELEASE);
	}
}

void ofdm_tx_share(struct ofdm_txbuf *tb)
{
	shared_tb = tb;
}

void ofdm_tx_help(struct ofdm_txbuf *tb, bool (*keep_going)(void *), void *arg)
{
	struct txjob *j = &shared_job;

	while (keep_going(arg)) {
		uint32_t v = __atomic_load_n(&j->word, __ATOMIC_ACQUIRE), gen = v >> 16;

		if (gen == help_gen) {
			continue;
		}
		help_gen = gen;
		/*
		 * A build not seen yet: help it if it has slots left. One already done (the last
		 * frame, when this thread asks before the next one opens) only counts as seen.
		 */
		if ((v & 0xffffU) < j->nslots) {
			(void)tx_work(j, (int16_t *)tb->buf, gen, keep_going, arg);
			return;
		}
	}
}

static size_t ofdm_tx_build16(const struct ofdm_ctx *ctx, struct ofdm_txbuf *tb, enum ofdm_mod mod,
			      const uint8_t *hdr, const uint8_t *bits, uint32_t *out,
			      unsigned int shift)
{
	unsigned int c = ctx->cfg.channels, p = ctx->cfg.pilots;
	unsigned int h0 = 1U + p, d0 = h0 + ctx->nhdr;
	unsigned int mp = ctx->cfg.mid_pilot ? d0 + ctx->mid : ~0U;
	unsigned int nslots = bits == NULL ? d0 : d0 + ctx->ndata + (ctx->cfg.mid_pilot ? 1U : 0U);
	/* One unit subcarrier in 16 bit: amp / sqrt(c) of the DAC value per unit of the sum. */
	float sc = (float)(1U << shift) * (float)ctx->cfg.amp * (float)ctx->nfft / sqrtf((float)c);
	const struct cons *k = &cons[mod];
	/* The shared build of a whole frame lets helpers in; the rest (ACKs too) build alone. */
	bool shared = tb == shared_tb && bits != NULL;
	struct txjob local, *j = shared ? &shared_job : &local;
	uint32_t old = shared ? __atomic_load_n(&j->word, __ATOMIC_RELAXED) >> 16 : 0U;
	uint32_t gen = (old + 1U) & 0xffffU;

	/*
	 * Closed first (slot 0xffff, never claimed): a helper still holding the last build's
	 * counter would otherwise check it against this build's slot count and claim with it
	 * (found by a host stress test of this protocol).
	 */
	if (shared) {
		__atomic_store_n(&j->word, (old << 16) | 0xffffU, __ATOMIC_RELEASE);
	}
	j->ctx = ctx;
	j->mod = mod;
	j->hdr = hdr;
	j->bits = bits;
	j->out = out;
	j->shift = shift;
	j->nslots = nslots;
	j->h0 = h0;
	j->d0 = d0;
	j->mp = mp;
	j->one = (int16_t)lrintf(sc);
	j->root2 = (int16_t)lrintf(sc * 1.41421356f);
	memset(j->lv, 0, sizeof(j->lv));
	if (mod == OFDM_BPSK) {
		j->lv[0] = (int16_t)-j->one;
		j->lv[1] = j->one;
	} else {
		/* Indexed by the Gray coded bits of one axis. */
		for (unsigned int v = 0; v < k->k; v++) {
			j->lv[v] = (int16_t)lrintf((float)(2 * gray_dec[v] - (int)(k->k - 1U)) * k->s *
						   sc);
		}
	}
	if (!shared) {
		/* Alone (an ACK, or no helper registered): no atomics, its memory may be anywhere. */
		uint32_t lut[64], cl = 0;

		tx_lut(j, lut);
		for (unsigned int s = 0; s < nslots; s++) {
			cl += tx_slot16(j, lut, (int16_t *)tb->buf, s);
		}
		tb->clipped = cl;
		return (size_t)nslots * ctx->slot;
	}
	j->done = 0;
	j->clipped = 0;
	/* Open: the generation with slot 0 next (the fields above published with it). */
	__atomic_store_n(&j->word, gen << 16, __ATOMIC_RELEASE);
	(void)tx_work(j, (int16_t *)tb->buf, gen, NULL, NULL);
	/* Slots a helper claimed may still be in progress. */
	while (__atomic_load_n(&j->done, __ATOMIC_ACQUIRE) < nslots) {
	}
	tb->clipped = __atomic_load_n(&j->clipped, __ATOMIC_RELAXED);
	return (size_t)nslots * ctx->slot;
}
#else
/* No 16-bit transmit path: every build stays on its own CPU. */
void ofdm_tx_share(struct ofdm_txbuf *tb)
{
	(void)tb;
}

void ofdm_tx_help(struct ofdm_txbuf *tb, bool (*keep_going)(void *), void *arg)
{
	(void)tb;
	(void)keep_going;
	(void)arg;
}
#endif

size_t ofdm_tx_build(const struct ofdm_ctx *ctx, struct ofdm_txbuf *tb, enum ofdm_mod mod,
		     const uint8_t *hdr, const uint8_t *bits, uint32_t *out)
{
	unsigned int c = ctx->cfg.channels, bps = ofdm_mod_bits(mod), p = ctx->cfg.pilots;
	unsigned int h0 = 1U + p, d0 = h0 + ctx->nhdr, nh = ctx->cfg.hdr_bits;
	/* The mid pilot's slot (none: past the end). */
	unsigned int mp = ctx->cfg.mid_pilot ? d0 + ctx->mid : ~0U;
	/* No payload: a header-only frame (an ACK), the preamble, pilots and header. */
	unsigned int nslots = bits == NULL ? d0 : d0 + ctx->ndata + (ctx->cfg.mid_pilot ? 1U : 0U);
	/* The unscaled inverse FFT of c unit subcarriers has RMS sqrt(c). */
	float g = (float)ctx->cfg.amp / sqrtf((float)c);
	size_t bi = 0;

	if (mod >= OFDM_MODS) {
		return 0;
	}
#if defined(CONFIG_APP_OFDM_ESP_DSP) && OFDM_FFT16
	{
		/* The largest input is the preamble's sqrt(2) points (and 5 % more as a margin). */
		float peak = 1.5f * (float)ctx->cfg.amp * (float)ctx->nfft / sqrtf((float)c);

		for (unsigned int shift = 3; shift > 0U; shift--) {
			if (peak * (float)(1U << shift) < 32000.0f) {
				return ofdm_tx_build16(ctx, tb, mod, hdr, bits, out, shift);
			}
		}
	}
#endif
	tb->clipped = 0;
	for (unsigned int s = 0; s < nslots; s++) {
		memset(tb->buf, 0, ctx->nfft * sizeof(tb->buf[0]));
		for (unsigned int a = 0; a < c; a++) {
			struct ofdm_cf *x = &tb->buf[ctx->bin[a]];

			if (s == 0) {
				/* Even subcarriers only, at twice the power: the same RMS. */
				if ((sub_freq(c, a) & 1) == 0) {
					*x = (struct ofdm_cf){1.41421356f * pn_sign(PREAMBLE_SEED, a),
							      0.0f};
				}
			} else if (s < h0 || s == mp) {
				*x = (struct ofdm_cf){pn_sign(PILOT_SEED, a), 0.0f};
			} else if (s < d0) {
				/* The header bits repeated over all header subcarriers. */
				unsigned int k = ((s - h0) * c + a) % nh;

				*x = (struct ofdm_cf){get_bit(hdr, k) ? 1.0f : -1.0f, 0.0f};
			} else {
				unsigned int v = 0;

				for (unsigned int b = 0; b < bps; b++, bi++) {
					v = (v << 1) | get_bit(bits, bi);
				}
				*x = map_point(mod, v);
				/* Conjugated in: see emit(). */
				x->im = -x->im;
			}
		}
		fft(ctx, tb->buf, ctx->nfft);
		emit(ctx, tb, g, &out[s * ctx->slot]);
	}
	return (size_t)nslots * ctx->slot;
}

#define sc_best ofdm_sc_best

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
static __attribute__((noinline)) void sc_scan(struct ofdm_ctx *ctx, const uint32_t *w, int t0,
					      int t1, int st, int dci, int dcq, struct sc_best *b)
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
		int ai = esp_sdr_rx_i(wa) - dci, aq = esp_sdr_rx_q(wa) - dcq;
		int bi = esp_sdr_rx_i(wb) - dci, bq = esp_sdr_rx_q(wb) - dcq;

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
			int ai = esp_sdr_rx_i(wa) - dci, aq = esp_sdr_rx_q(wa) - dcq;
			int bi = esp_sdr_rx_i(wb) - dci, bq = esp_sdr_rx_q(wb) - dcq;
			int ci = esp_sdr_rx_i(wc) - dci, cq = esp_sdr_rx_q(wc) - dcq;
			int eb = bi * bi + bq * bq;

			pr += (bi * ci + bq * cq) - (ai * bi + aq * bq);
			pi += (bi * cq - bq * ci) - (ai * bq - aq * bi);
			ra += eb - (ai * ai + aq * aq);
			rb += (ci * ci + cq * cq) - eb;
		}
	}
}

/*
 * The frame start in t = 0 .. tmax (tlim: the last start a whole frame fits): a coarse scan on every 2nd or 4th sample
 * (both the position and the correlation, which keeps at least 32 terms),
 * then a full resolution one over the prefix around its peak.
 */
/*
 * The coarse scan (st >= 2): sc_scan()'s sums and metric, with a cheap integer pre-test in
 * front of the metric (floating point, about 40 % of a step): |P| <= |pr| + |pi| and
 * Ra Rb >= min(Ra, Rb)^2, so a step whose |pr| + |pi| stays below sqrt(best) min(Ra, Rb) cannot
 * beat the best. Rounded so that it never skips one that could: the same peak as sc_scan().
 */
OFDM_HOT static void sc_scan_coarse(struct ofdm_ctx *ctx, const uint32_t *w, int t0, int t1,
				    int st, int dci, int dcq, struct sc_best *b)
{
	int L = (int)ctx->rnfft / 2;
	int32_t pr = 0, pi = 0, ra = 0, rb = 0, sb = 0;

	b->num = 0.0f;
	b->den = 1.0f;
	b->t = b->start = -1;
	for (int m = 0; m < L; m += st) {
		uint32_t wa = w[t0 + m], wb = w[t0 + m + L];
		int ai = esp_sdr_rx_i(wa) - dci, aq = esp_sdr_rx_q(wa) - dcq;
		int bi = esp_sdr_rx_i(wb) - dci, bq = esp_sdr_rx_q(wb) - dcq;

		pr += ai * bi + aq * bq;
		pi += ai * bq - aq * bi;
		ra += ai * ai + aq * aq;
		rb += bi * bi + bq * bq;
	}
	for (int t = t0;; t += st) {
		/* In units of 64 (sums up to 2^27): sb is sqrt(best) in Q8. */
		int32_t ap = ((pr < 0 ? -pr : pr) + (pi < 0 ? -pi : pi)) >> 6;
		int32_t mr = (ra < rb ? ra : rb) >> 6;

		if (ap + 1 >= ((mr * sb) >> 8)) {
			float fr = (float)pr, fi = (float)pi;
			float num = fr * fr + fi * fi, den = (float)ra * (float)rb;

			/* num / den > b->num / b->den */
			if (num * b->den > b->num * den) {
				b->num = num;
				b->den = den;
				b->pr = pr;
				b->pi = pi;
				b->t = b->start = t;
				/* Rounded down: the test stays on the safe side. */
				sb = den > 0.0f ? (int32_t)(sqrtf(num / den) * 256.0f) : 0;
			}
		}
		if (t + st > t1) {
			break;
		}
		{
			uint32_t wa = w[t], wb = w[t + L], wc = w[t + 2 * L];
			int ai = esp_sdr_rx_i(wa) - dci, aq = esp_sdr_rx_q(wa) - dcq;
			int bi = esp_sdr_rx_i(wb) - dci, bq = esp_sdr_rx_q(wb) - dcq;
			int ci = esp_sdr_rx_i(wc) - dci, cq = esp_sdr_rx_q(wc) - dcq;
			int eb = bi * bi + bq * bq;

			pr += (bi * ci + bq * cq) - (ai * bi + aq * bq);
			pi += (bi * cq - bq * ci) - (ai * bq - aq * bi);
			ra += eb - (ai * ai + aq * aq);
			rb += (ci * ci + cq * cq) - eb;
		}
	}
}

void (*ofdm_scan_async)(struct ofdm_ctx *ctx, const uint32_t *w, int t0, int t1, int st, int dci,
			int dcq, struct ofdm_sc_best *b);
void (*ofdm_scan_wait)(void);

void ofdm_sc_scan(struct ofdm_ctx *ctx, const uint32_t *w, int t0, int t1, int st, int dci,
		  int dcq, struct ofdm_sc_best *b)
{
	sc_scan_coarse(ctx, w, t0, t1, st, dci, dcq, b);
}

/* sc_scan() over 0 .. tmax, the second half on the other CPU when there is one. */
static void sc_scan_split(struct ofdm_ctx *ctx, const uint32_t *w, int tmax, int st, int dci,
			  int dcq, struct sc_best *b)
{
	struct sc_best b2;
	int mid = (tmax / 2 / st) * st;
	/* Read once: the shell may switch the split meanwhile (ofdm split). */
	void (*scan_async)(struct ofdm_ctx *, const uint32_t *, int, int, int, int, int,
			   struct ofdm_sc_best *) = ofdm_scan_async;
	void (*scan_wait)(void) = ofdm_scan_wait;

	if (st < 2) {
		/* Full resolution: the plain scan (its prefix ring needs every metric). */
		sc_scan(ctx, w, 0, tmax, st, dci, dcq, b);
		return;
	}
	/* Split only when each half is worth the hand-over. */
	if (scan_async == NULL || scan_wait == NULL || mid < 256 * st) {
		sc_scan_coarse(ctx, w, 0, tmax, st, dci, dcq, b);
		return;
	}
	scan_async(ctx, w, mid, tmax, st, dci, dcq, &b2);
	sc_scan_coarse(ctx, w, 0, mid - st, st, dci, dcq, b);
	scan_wait();
	if (b2.t >= 0 && (b->t < 0 || b2.num * b->den > b->num * b2.den)) {
		*b = b2;
	}
}

/* A coarse peak this clear (normalized metric) needs no finer coarse scan. */
#define COARSE_CLEAR 0.7f

static int search(struct ofdm_ctx *ctx, const uint32_t *w, int tmax, int tlim, int period,
		  int dci, int dcq, float *best_out, float *ang_out)
{
	int L = (int)ctx->rnfft / 2, st = L >= 128 ? 4 : (L >= 64 ? 2 : 1), lo, hi;
	struct sc_best c, f;

	/*
	 * Twice the stride first: at a good SNR its peak is clear and the scan
	 * costs half. A weak one (fewer correlation terms are noisier) is
	 * scanned again at the normal stride.
	 */
	if (L / (2 * st) >= 16) {
		sc_scan_split(ctx, w, tmax, 2 * st, dci, dcq, &c);
		if (c.t < 0 || c.num < COARSE_CLEAR * c.den) {
			sc_scan_split(ctx, w, tmax, st, dci, dcq, &c);
		} else {
			st *= 2;
		}
	} else {
		sc_scan_split(ctx, w, tmax, st, dci, dcq, &c);
	}
	if (c.t < 0) {
		return -1;
	}
	/*
	 * The coarse peak is somewhere on the plateau, give or take a step. Near
	 * the window's beginning the plateau may be cut (the copy began before
	 * the window, its edge would come out late): look at the next copy, one
	 * period on, instead; up to tlim it lies whole in the window.
	 */
	lo = c.t - (int)ctx->rcp - st;
	if (lo < 0 && c.t + period + st <= tlim) {
		c.t += period;
		lo += period;
		tmax = tlim;
	}
	hi = c.t + st;
	lo = lo < 0 ? 0 : lo;
	hi = hi > tmax ? tmax : hi;
	sc_scan(ctx, w, lo, hi, 1, dci, dcq, &f);
	*best_out = f.den > 0.0f ? f.num / f.den : 0.0f;
	*ang_out = atan2f((float)f.pi, (float)f.pr);
	return f.start;
}

/*
 * Derotation over one receive FFT window: rot[i] = exp(-j tau eps i). The
 * phase at each window's first sample is the same for all its subcarriers, so
 * it goes into the tracked phase instead (rx.u).
 */
static void rot_init(struct ofdm_ctx *ctx, float eps)
{
	float a = -ESP_SDR_TAU_F * eps;
	float sr = cosf(a), si = sinf(a), rr = 1.0f, ri = 0.0f;

	for (unsigned int i = 0; i < ctx->rnfft; i++) {
		float t;

		/* Exact every 64 samples, steps in between. */
		if ((i & 63U) == 0U) {
			rr = cosf(a * (float)i);
			ri = sinf(a * (float)i);
		}
		ctx->rot[i] = (struct ofdm_cf){rr, ri};
		ctx->rot16[i][0] = (int16_t)lrintf(rr * 32767.0f);
		ctx->rot16[i][1] = (int16_t)lrintf(ri * 32767.0f);
		t = rr * sr - ri * si;
		ri = rr * si + ri * sr;
		rr = t;
	}
}

bool ofdm_vec_derot = true;
float ofdm_carrier_hz = 2412e6f;
int ofdm_sfo = -1;

#if defined(CONFIG_APP_OFDM_ESP_DSP) && OFDM_FFT16
/*
 * The derotation of 4 n4 receive words (any alignment: ee.ld.128.usar loads the aligned
 * 16 bytes and ee.src.q.qup shifts the pair of chunks by the address's low bits) into
 * int16 pairs scaled by FFT16_GAIN, on the vector unit. Per word w (Q in bits 9:0, I in
 * 19:10): I 64 = (w >> 4) & 0xffc0 in the low half, Q 64 = w << 22 in the high half, minus
 * the DC in the same scale (saturating), clipped to +-23170 so that the rotation (one
 * complex multiply by rot16, Q15, >> 16: the floor of x 32 rot) stays in 16 bit. SAR is
 * written before each shift; the vector unit takes the new value without a sync.
 */
__attribute__((noinline)) static void derot_kernel(int16_t *q, const uint32_t *x,
						   const int16_t *rot16, unsigned int n4,
						   const uint32_t *dcw, const uint32_t *maskw,
						   const int16_t clip[2])
{
	const uint32_t s4 = 4, s22 = 22, s16 = 16;

	__asm__ volatile("ee.vldbc.32.ip q7, %[dc], 0\n\t"
			 "ee.vldbc.32.ip q6, %[mk], 0\n\t"
			 "ee.vldbc.16.ip q4, %[cl], 2\n\t"
			 "ee.vldbc.16.ip q5, %[cl], 0\n\t"
			 "ee.ld.128.usar.ip q0, %[x], 16\n\t"
			 "loopnez %[n], 1f\n\t"
			 "ee.ld.128.usar.ip q1, %[x], 16\n\t"
			 "ee.src.q.qup q2, q0, q1\n\t"
			 "wsr.sar %[s4]\n\t"
			 "ee.vsr.32 q3, q2\n\t"
			 "ee.andq q3, q3, q6\n\t"
			 "wsr.sar %[s22]\n\t"
			 "ee.vsl.32 q2, q2\n\t"
			 "ee.orq q3, q3, q2\n\t"
			 "ee.vsubs.s16 q3, q3, q7\n\t"
			 "ee.vmin.s16 q3, q3, q5\n\t"
			 "ee.vmax.s16 q3, q3, q4\n\t"
			 "ee.vld.128.ip q2, %[r], 16\n\t"
			 "wsr.sar %[s16]\n\t"
			 "ee.cmul.s16 q1, q3, q2, 0\n\t"
			 "ee.cmul.s16 q1, q3, q2, 1\n\t"
			 "ee.vst.128.ip q1, %[q], 16\n\t"
			 "1:\n\t"
			 : [q] "+r"(q), [x] "+r"(x), [r] "+r"(rot16), [cl] "+r"(clip)
			 : [n] "r"(n4), [dc] "r"(dcw), [mk] "r"(maskw), [s4] "r"(s4),
			   [s22] "r"(s22), [s16] "r"(s16)
			 : "memory");
}

/* derot_kernel() with the vector unit to itself (see fft16()). */
static void derot16(const struct ofdm_ctx *ctx, int16_t *q, const uint32_t *x, unsigned int n)
{
	static const uint32_t maskw = 0x0000ffc0U;
	static const int16_t clip[2] = {-23170, 23170};
	uint32_t dcw = ((uint32_t)(uint16_t)(int16_t)(ctx->rx.dcq * 64) << 16) |
		       (uint16_t)(int16_t)(ctx->rx.dci * 64);
	unsigned int key = arch_irq_lock();

	pie_enable();
	derot_kernel(q, x, (const int16_t *)ctx->rot16, n / 4U, &dcw, &maskw, clip);
	arch_irq_unlock(key);
}
#endif

/*
 * Slot s of the frame at rx.start into the FFT buffer, DC removed and
 * derotated, the window cp/4 early inside the prefix (a late start then still
 * stays clear of the next symbol), and the FFT. Steps rx.u to this slot:
 * against the first pilot's, slot s comes out turned by exp(+j tau eps rslot
 * (s - 1)), which u = exp(-j tau eps rslot (s - 1)) undoes.
 */
OFDM_HOT static void take_slot(const struct ofdm_ctx *ctx, struct ofdm_worker *wk,
			       const uint32_t *w, unsigned int s, bool fast)
{
	int pos = ctx->rx.start + (int)(s * ctx->rslot) + (int)ctx->rcp - (int)(ctx->rcp / 4U);
	const uint32_t *__restrict x;
	bool wrapped = pos + (int)ctx->rnfft > ctx->rx.n;

	/* Past the window's end: the same samples one period earlier (the frame loops). */
	if (wrapped) {
		pos -= ctx->rx.per;
	}
	x = &w[pos];
	const struct ofdm_cf *__restrict r = ctx->rot;
	struct ofdm_cf *__restrict b = wk->buf;
	unsigned int n = ctx->rnfft;
	int dci = ctx->rx.dci, dcq = ctx->rx.dcq;

#if defined(CONFIG_APP_OFDM_ESP_DSP) && OFDM_FFT16
	wk->q16 = fast;
	if (fast && ofdm_vec_derot) {
		derot16(ctx, (int16_t *)b, x, n);
		fft16(ctx, (int16_t *)b, n);
	} else if (fast) {
		int16_t *q = (int16_t *)b;

		for (unsigned int i = 0; i < n; i++) {
			uint32_t v = x[i];
			float xr = (float)(esp_sdr_rx_i(v) - dci) * FFT16_GAIN;
			float xq = (float)(esp_sdr_rx_q(v) - dcq) * FFT16_GAIN;
			float rr = r[i].re, ri = r[i].im;

			/* A capture with a big offset or a transient must clip, not wrap. */
			int vr = (int)(xr * rr - xq * ri), vi = (int)(xr * ri + xq * rr);

			vr = vr < -32767 ? -32767 : (vr > 32767 ? 32767 : vr);
			vi = vi < -32767 ? -32767 : (vi > 32767 ? 32767 : vi);
			q[2U * i] = (int16_t)vr;
			q[2U * i + 1U] = (int16_t)vi;
		}
		fft16(ctx, q, n);
	} else {
		for (unsigned int i = 0; i < n; i++) {
			uint32_t v = x[i];
			float xr = (float)(esp_sdr_rx_i(v) - dci), xq = (float)(esp_sdr_rx_q(v) - dcq);
			float rr = r[i].re, ri = r[i].im;

			b[i].re = xr * rr - xq * ri;
			b[i].im = xr * ri + xq * rr;
		}
		fft(ctx, wk->buf, n);
	}
#else
	(void)fast;
	for (unsigned int i = 0; i < n; i++) {
		uint32_t v = x[i];
		float xr = (float)(esp_sdr_rx_i(v) - dci), xq = (float)(esp_sdr_rx_q(v) - dcq);
		float rr = r[i].re, ri = r[i].im;

		b[i].re = xr * rr - xq * ri;
		b[i].im = xr * ri + xq * rr;
	}
	fft(ctx, wk->buf, n);
#endif
	{
		struct ofdm_cf u = wk->u, us = ctx->rx.us, cw = ctx->rx.cw;

		/* The last slot's wrap undone, the step, this slot's wrap. */
		if (wk->wrapped) {
			u = (struct ofdm_cf){u.re * cw.re + u.im * cw.im, u.im * cw.re - u.re * cw.im};
		}
		if (s > 1U) {
			u = (struct ofdm_cf){u.re * us.re - u.im * us.im, u.re * us.im + u.im * us.re};
		}
		if (wrapped) {
			u = (struct ofdm_cf){u.re * cw.re - u.im * cw.im, u.re * cw.im + u.im * cw.re};
		}
		wk->u = u;
		wk->wrapped = wrapped;
	}
	wk->slot = s;
	wk->tslot = (float)s - (wrapped ? (float)ctx->rx.per / (float)ctx->rslot : 0.0f);
}

/* Output bin @p i of the worker's receive FFT as the float FFT would have it. */
static inline struct ofdm_cf fft_out(const struct ofdm_ctx *ctx, const struct ofdm_worker *wk,
				     unsigned int i)
{
#if defined(CONFIG_APP_OFDM_ESP_DSP) && OFDM_FFT16
	if (wk->q16) {
		const int16_t *q = (const int16_t *)wk->buf;

		return (struct ofdm_cf){(float)q[2U * i] * ctx->fscale,
				       (float)q[2U * i + 1U] * ctx->fscale};
	}
#endif
	return wk->buf[i];
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

/* Subcarrier a of the worker's FFT output, equalized and rotated by (cr, ci). */
static inline struct ofdm_cf eq_rot(const struct ofdm_ctx *ctx, const struct ofdm_worker *wk,
				    unsigned int a, float cr, float ci)
{
	struct ofdm_cf y = fft_out(ctx, wk, ctx->binr[a]), e = ctx->eq[a];
	float zr = y.re * e.re - y.im * e.im, zi = y.re * e.im + y.im * e.re;

	return (struct ofdm_cf){zr * cr - zi * ci, zr * ci + zi * cr};
}

static inline struct ofdm_cf cmul(struct ofdm_cf a, struct ofdm_cf b)
{
	return (struct ofdm_cf){a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re};
}

/* Gain of the rate update: a residual offset shows as a steady step in the measured phase. */
#define RATE_GAIN 0.25f

/* The sample clock drift's turn per unit of subcarrier frequency for the worker's slot. */
static inline float sfo_theta(const struct ofdm_ctx *ctx, const struct ofdm_worker *wk)
{
	return ctx->rx.dslot * (wk->tslot - ctx->rx.sref);
}

/* exp(j th k) for subcarrier a's frequency k, stepped along the subcarriers by sfo_next(). */
static inline struct ofdm_cf sfo_first(unsigned int c, float th)
{
	float a = th * (float)sub_freq(c, 0);

	return (struct ofdm_cf){cosf(a), sinf(a)};
}

static inline struct ofdm_cf sfo_next(unsigned int c, unsigned int a, struct ofdm_cf ph,
				      struct ofdm_cf st)
{
	for (int d = sub_freq(c, a + 1U) - sub_freq(c, a); d > 0; d--) {
		ph = (struct ofdm_cf){ph.re * st.re - ph.im * st.im, ph.re * st.im + ph.im * st.re};
	}
	return ph;
}

/*
 * The rotation for the symbol in the worker's buffer: the tracked phase advanced by its
 * rate, and this window's known phase; then the symbol's own residual
 * rotation (against its decisions at @p mod) corrects the phase in full and
 * the rate in part (a second order loop: a residual carrier offset is
 * followed without lag).
 */
static inline __attribute__((always_inline)) struct ofdm_cf track(const struct ofdm_ctx *ctx,
								  struct ofdm_worker *wk,
								  const enum ofdm_mod mod)
{
	struct ofdm_worker *rx = wk;
	struct ofdm_cf p = cmul(rx->trk, rx->rate), r = cmul(p, rx->u);
	float ar = 0.0f, ai = 0.0f, m;
	/*
	 * The middle half of the band, every second subcarrier: the strongest
	 * (the band edges sit 8 dB lower, where decisions are least sure) and a
	 * quarter of the work; plenty of them for a phase estimate.
	 */
	unsigned int c = ctx->cfg.channels, a0 = c / 4U, a1 = c - a0;
	/* Each subcarrier turned against the sample clock drift too, as the decoder will. */
	float th = sfo_theta(ctx, wk);
	struct ofdm_cf ph = {1.0f, 0.0f}, st = {1.0f, 0.0f};

	if (th != 0.0f) {
		float ang = th * (float)sub_freq(c, a0);

		ph = (struct ofdm_cf){cosf(ang), sinf(ang)};
		st = (struct ofdm_cf){cosf(th), sinf(th)};
	}
	for (unsigned int a = a0; a < a1; a += 2U) {
		struct ofdm_cf ra = cmul(r, ph);
		struct ofdm_cf z = eq_rot(ctx, wk, a, ra.re, ra.im), d;

		if (a + 2U < a1) {
			ph = sfo_next(c, a + 1U, sfo_next(c, a, ph, st), st);
		}

		(void)decide(mod, z, &d);
		ar += z.re * d.re + z.im * d.im;
		ai += z.im * d.re - z.re * d.im;
	}
	m = sqrtf(ar * ar + ai * ai);
	if (m > 0.0f) {
		struct ofdm_cf c = {ar / m, -ai / m}, q;
		float qm;

		p = cmul(p, c);
		r = cmul(r, c);
		/* Rate: a step of the way towards it times this correction. */
		q = (struct ofdm_cf){1.0f + RATE_GAIN * (c.re - 1.0f), RATE_GAIN * c.im};
		q = cmul(rx->rate, q);
		qm = 1.0f / sqrtf(q.re * q.re + q.im * q.im);
		rx->rate = (struct ofdm_cf){q.re * qm, q.im * qm};
	}
	rx->trk = p;
	return r;
}

OFDM_HOT static struct ofdm_cf track_any(const struct ofdm_ctx *ctx, struct ofdm_worker *wk,
					 enum ofdm_mod mod)
{
	return track(ctx, wk, mod);
}

/* One payload symbol: rotation, decisions into @p k and their error. */
static inline __attribute__((always_inline)) void data_symbol_m(const struct ofdm_ctx *ctx,
								struct ofdm_worker *wk,
								const enum ofdm_mod mod,
								struct bitsink *k, float *err)
{
	struct ofdm_cf r = track(ctx, wk, mod);
	unsigned int c = ctx->cfg.channels, bps = ofdm_mod_bits(mod);
	float th = sfo_theta(ctx, wk);
	struct ofdm_cf ph = sfo_first(c, th), st = {cosf(th), sinf(th)};

	float e2 = 0.0f;

	float *se = ctx->sc_err;

	for (unsigned int a = 0; a < c; a++) {
		struct ofdm_cf ra = {r.re * ph.re - r.im * ph.im, r.re * ph.im + r.im * ph.re};
		struct ofdm_cf z = eq_rot(ctx, wk, a, ra.re, ra.im), d;

		if (a + 1U < c) {
			ph = sfo_next(c, a, ph, st);
		}
		unsigned int v = decide(mod, z, &d);
		float ee = (z.re - d.re) * (z.re - d.re) + (z.im - d.im) * (z.im - d.im);

		e2 += ee;
		if (se != NULL) {
			se[a] += ee;
		}
		sink_put(k, v, bps);
	}
#ifdef OFDM_DEBUG
	{
		extern void ofdm_debug_symbol(float e2, unsigned int c, struct ofdm_cf r);

		ofdm_debug_symbol(e2, c, r);
	}
#endif
	*err += e2;
}

bool ofdm_vec_enable = true;

#if defined(CONFIG_APP_OFDM_ESP_DSP) && OFDM_FFT16
/*
 * The payload symbol on the vector unit, four subcarriers per step, in a function of its own
 * (it sets SAR, which the compiler does not know about). Per group: z = (g E) >> sar, rotated
 * by r (scaled by 2^sar, so the same shift), t = z + k 128 clamped to 0 .. 256 k - 1, the
 * level l = t >> 8 (by a multiply and the same shift), the error t0 - (t & 0xff00) - 128
 * summed squared in the accumulator. g in place becomes l; returns the error energy.
 * cv: five vectors of eight, 128 k, 256 k - 1, 2^(sar - 8), 0xff00, 128.
 */
__attribute__((noinline)) static int32_t veq_kernel(uint32_t *g, const int16_t *e, unsigned int ng,
						     unsigned int sar, const uint32_t *rq32,
						     const int16_t cv[5][8])
{
	const uint32_t *gl = g;
	uint32_t *gs = g;
	int32_t acc;

	__asm__ volatile("wsr.sar %[sar]\n\t"
			 "rsync\n\t"
			 "ee.vldbc.32.ip q7, %[rq], 0\n\t"
			 "ee.vld.128.ip q6, %[ca], 0\n\t"
			 "ee.vld.128.ip q5, %[ch], 0\n\t"
			 "ee.zero.accx\n\t"
			 "loopnez %[n], 1f\n\t"
			 "ee.vld.128.ip q0, %[gl], 16\n\t"
			 "ee.vld.128.ip q1, %[e], 16\n\t"
			 "ee.cmul.s16 q2, q0, q1, 0\n\t"
			 "ee.cmul.s16 q2, q0, q1, 1\n\t"
			 "ee.cmul.s16 q3, q2, q7, 0\n\t"
			 "ee.cmul.s16 q3, q2, q7, 1\n\t"
			 "ee.vadds.s16 q3, q3, q6\n\t"
			 "ee.zero.q q1\n\t"
			 "ee.vmax.s16 q2, q3, q1\n\t"
			 "ee.vmin.s16 q2, q2, q5\n\t"
			 "ee.vld.128.ip q0, %[cm], 0\n\t"
			 "ee.vmul.s16 q0, q2, q0\n\t"
			 "ee.vst.128.ip q0, %[gs], 16\n\t"
			 "ee.vld.128.ip q1, %[ck], 0\n\t"
			 "ee.andq q1, q2, q1\n\t"
			 "ee.vsubs.s16 q3, q3, q1\n\t"
			 "ee.vld.128.ip q1, %[cb], 0\n\t"
			 "ee.vsubs.s16 q3, q3, q1\n\t"
			 "ee.vmulas.s16.accx q3, q3\n\t"
			 "1:\n\t"
			 "movi %[acc], 0\n\t"
			 "ee.srs.accx %[acc], %[acc], 0\n\t"
			 : [gl] "+r"(gl), [gs] "+r"(gs), [e] "+r"(e), [acc] "=&r"(acc)
			 : [sar] "r"(sar), [rq] "r"(rq32), [ca] "r"(cv[0]), [ch] "r"(cv[1]),
			   [cm] "r"(cv[2]), [ck] "r"(cv[3]), [cb] "r"(cv[4]), [n] "r"(ng)
			 : "memory");
	return acc;
}

/* (level I, level Q) to the Gray coded bits of the subcarrier, 64-QAM (3 + 3 bits) and 16-QAM (2 + 2). */
static const uint8_t pack64[64] = {
#define G3(l) (((l) ^ ((l) >> 1)) & 7)
#define ROW(i) (G3(i) << 3 | G3(0)), (G3(i) << 3 | G3(1)), (G3(i) << 3 | G3(2)), (G3(i) << 3 | G3(3)), \
	       (G3(i) << 3 | G3(4)), (G3(i) << 3 | G3(5)), (G3(i) << 3 | G3(6)), (G3(i) << 3 | G3(7))
	ROW(0), ROW(1), ROW(2), ROW(3), ROW(4), ROW(5), ROW(6), ROW(7),
#undef ROW
#undef G3
};
static const uint8_t pack16[16] = {
#define G2(l) (((l) ^ ((l) >> 1)) & 3)
#define ROW(i) (G2(i) << 2 | G2(0)), (G2(i) << 2 | G2(1)), (G2(i) << 2 | G2(2)), (G2(i) << 2 | G2(3))
	ROW(0), ROW(1), ROW(2), ROW(3),
#undef ROW
#undef G2
};

static inline __attribute__((always_inline)) void data_symbol_v(const struct ofdm_ctx *ctx,
								struct ofdm_worker *wk,
								const enum ofdm_mod mod,
								struct bitsink *k, float *err)
{
	const struct ofdm_veq *v = &ctx->veq[mod == OFDM_64QAM];
	const struct cons *cn = &cons[mod];
	struct ofdm_cf r = track(ctx, wk, mod);
	const int16_t *ep = &v->e[0][0];
	unsigned int c = ctx->cfg.channels, ng = c / 4U, nv = 4U * ng, bps = ofdm_mod_bits(mod);
	unsigned int sh = (unsigned int)v->sar, qb = cn->q;
	/* The gathered subcarriers, in the unused half of the FFT buffer (the FFT takes <= 1 KB). */
	const uint32_t *fo = (const uint32_t *)wk->buf;
	uint32_t *gb = (uint32_t *)wk->buf + 256;
	int16_t cv[5][8] __attribute__((aligned(16)));
	uint32_t rq32;
	int rr = (int)(r.re * (float)(1 << sh) + (r.re >= 0.0f ? 0.5f : -0.5f));
	int ri = (int)(r.im * (float)(1 << sh) + (r.im >= 0.0f ? 0.5f : -0.5f));
	float e2;
	unsigned int irq;

	for (unsigned int a = 0; a < nv; a++) {
		gb[a] = fo[ctx->binr[a]];
	}
	for (unsigned int i = 0; i < 8U; i++) {
		cv[0][i] = (int16_t)(cn->k * 128U);
		cv[1][i] = (int16_t)(256U * cn->k - 1U);
		cv[2][i] = (int16_t)(1U << (sh - 8U));
		cv[3][i] = (int16_t)0xff00;
		cv[4][i] = 128;
	}
	rq32 = (uint32_t)(uint16_t)rr | ((uint32_t)(uint16_t)ri << 16);
	if (ctx->rx.dslot != 0.0f) {
		/* The equalizer turned against the drift, again once the band edge moved 0.02 rad. */
		float th = sfo_theta(ctx, wk);

		if (wk->e16_src != v || wk->e16_gen != ctx->rx.gen ||
		    fabsf(th - wk->e16_th) * (float)(c / 2U) > 0.02f) {
			struct ofdm_cf ph = sfo_first(c, th), st = {cosf(th), sinf(th)};

			for (unsigned int a = 0; a < c; a++) {
				float er = (float)v->e[a][0], ei = (float)v->e[a][1];
				float yr = er * ph.re - ei * ph.im, yi = er * ph.im + ei * ph.re;

				yr = yr > 32767.0f ? 32767.0f : (yr < -32767.0f ? -32767.0f : yr);
				yi = yi > 32767.0f ? 32767.0f : (yi < -32767.0f ? -32767.0f : yi);
				wk->e16[a][0] = (int16_t)(yr + (yr >= 0.0f ? 0.5f : -0.5f));
				wk->e16[a][1] = (int16_t)(yi + (yi >= 0.0f ? 0.5f : -0.5f));
				if (a + 1U < c) {
					ph = sfo_next(c, a, ph, st);
				}
			}
			wk->e16_src = v;
			wk->e16_gen = ctx->rx.gen;
			wk->e16_th = th;
		}
		ep = &wk->e16[0][0];
	}
	irq = arch_irq_lock();
	pie_enable();
	{
		/* Squared error in units of 1 / U, U = 128 / s per unit of the constellation. */
		float acc = (float)veq_kernel(gb, ep, ng, sh, &rq32, cv);

		e2 = acc * (cn->s * cn->s) * (1.0f / 16384.0f);
	}
	arch_irq_unlock(irq);
	if (k->n == 0U && ((nv * bps) & 7U) == 0U && (bps == 6U || bps == 4U)) {
		/*
		 * Whole bytes: the two Gray coded levels of a subcarrier are one table entry,
		 * and 4 subcarriers (6 bits each) or 2 (4 bits) make 3 or 1 bytes.
		 */
		uint8_t *o = k->out;

		if (bps == 6U) {
			for (unsigned int a = 0; a + 4U <= nv; a += 4U) {
				uint32_t w0 = gb[a], w1 = gb[a + 1U], w2 = gb[a + 2U], w3 = gb[a + 3U];
				uint32_t v0 = pack64[((w0 & 7U) << 3) | (w0 >> 16)];
				uint32_t v1 = pack64[((w1 & 7U) << 3) | (w1 >> 16)];
				uint32_t v2 = pack64[((w2 & 7U) << 3) | (w2 >> 16)];
				uint32_t v3 = pack64[((w3 & 7U) << 3) | (w3 >> 16)];
				uint32_t x = (v0 << 18) | (v1 << 12) | (v2 << 6) | v3;

				o[0] = (uint8_t)(x >> 16);
				o[1] = (uint8_t)(x >> 8);
				o[2] = (uint8_t)x;
				o += 3;
			}
		} else {
			for (unsigned int a = 0; a + 2U <= nv; a += 2U) {
				uint32_t w0 = gb[a], w1 = gb[a + 1U];

				*o++ = (uint8_t)((pack16[((w0 & 3U) << 2) | (w0 >> 16)] << 4) |
						 pack16[((w1 & 3U) << 2) | (w1 >> 16)]);
			}
		}
		k->out = o;
	} else {
		for (unsigned int a = 0; a < nv; a++) {
			uint32_t w = gb[a];
			unsigned int li = w & 0xffffU, lq = w >> 16;

			sink_put(k, ((li ^ (li >> 1)) << qb) | (lq ^ (lq >> 1)), bps);
		}
	}
	/* Subcarriers past the last whole group: floats. */
	for (unsigned int a = nv; a < c; a++) {
		float ta = sfo_theta(ctx, wk) * (float)sub_freq(c, a);
		struct ofdm_cf ph = {cosf(ta), sinf(ta)};
		struct ofdm_cf z = eq_rot(ctx, wk, a, r.re * ph.re - r.im * ph.im,
					  r.re * ph.im + r.im * ph.re), d;
		unsigned int vv = decide(mod, z, &d);

		e2 += (z.re - d.re) * (z.re - d.re) + (z.im - d.im) * (z.im - d.im);
		sink_put(k, vv, bps);
	}
	*err += e2;
}
#endif

/*
 * The loops specialised by modulation, so that the constellation constants
 * fold, for the two that carry the data (from flash: internal RAM is short,
 * and a loop this tight runs from the cache); the others run the generic one.
 */
static __attribute__((noinline)) void data_symbol_16(const struct ofdm_ctx *ctx,
						     struct ofdm_worker *wk, struct bitsink *k,
						     float *err)
{
#if defined(CONFIG_APP_OFDM_ESP_DSP) && OFDM_FFT16
	if (wk->q16 && ofdm_vec_enable && ctx->veq[0].sar != 0 && ctx->sc_err == NULL) {
		data_symbol_v(ctx, wk, OFDM_16QAM, k, err);
		return;
	}
#endif
	data_symbol_m(ctx, wk, OFDM_16QAM, k, err);
}

static __attribute__((noinline)) void data_symbol_64(const struct ofdm_ctx *ctx,
						     struct ofdm_worker *wk, struct bitsink *k,
						     float *err)
{
#if defined(CONFIG_APP_OFDM_ESP_DSP) && OFDM_FFT16
	if (wk->q16 && ofdm_vec_enable && ctx->veq[1].sar != 0 && ctx->sc_err == NULL) {
		data_symbol_v(ctx, wk, OFDM_64QAM, k, err);
		return;
	}
#endif
	data_symbol_m(ctx, wk, OFDM_64QAM, k, err);
}

OFDM_HOT static void data_symbol_any(const struct ofdm_ctx *ctx, struct ofdm_worker *wk,
				     enum ofdm_mod mod, struct bitsink *k, float *err)
{
	data_symbol_m(ctx, wk, mod, k, err);
}

static inline void data_symbol(const struct ofdm_ctx *ctx, struct ofdm_worker *wk,
			       enum ofdm_mod mod, struct bitsink *k, float *err)
{
	if (mod == OFDM_64QAM) {
		data_symbol_64(ctx, wk, k, err);
	} else if (mod == OFDM_16QAM) {
		data_symbol_16(ctx, wk, k, err);
	} else {
		data_symbol_any(ctx, wk, mod, k, err);
	}
}

#if defined(CONFIG_APP_OFDM_ESP_DSP) && OFDM_FFT16
/* Rounded to 16 bit inline (lrintf() is a library call). */
static inline int16_t rnd16(float x)
{
	return (int16_t)(x + (x >= 0.0f ? 0.5f : -0.5f));
}

/*
 * The vector decoder's equalizer, from ctx->eq: in units where the constellation spacing is
 * 256 (a power of two, so slicing is an add, a clamp and a shift), times 2^sar for the
 * 16-bit multiply (sar 10 to 14: the slicing shift needs at least 8). Too big for that
 * (a very weak subcarrier): sar 0, the float decoder takes the frame.
 */
static void veq_build(struct ofdm_ctx *ctx)
{
	unsigned int c = ctx->cfg.channels;

	for (unsigned int m = 0; m < 2U; m++) {
		float f = ctx->fscale * 128.0f / cons[m ? OFDM_64QAM : OFDM_16QAM].s, fmax = 0.0f;
		int sar = 14;

		for (unsigned int a = 0; a < c; a++) {
			float v = ctx->eq[a].re < 0.0f ? -ctx->eq[a].re : ctx->eq[a].re;
			float w = ctx->eq[a].im < 0.0f ? -ctx->eq[a].im : ctx->eq[a].im;

			fmax = v > fmax ? v : fmax;
			fmax = w > fmax ? w : fmax;
		}
		fmax *= f;
		while (sar >= 10 && fmax * (float)(1 << sar) > 30000.0f) {
			sar--;
		}
		if (sar < 10) {
			ctx->veq[m].sar = 0;
			continue;
		}
		for (unsigned int a = 0; a < c; a++) {
			ctx->veq[m].e[a][0] = rnd16(ctx->eq[a].re * f * (float)(1 << sar));
			ctx->veq[m].e[a][1] = rnd16(ctx->eq[a].im * f * (float)(1 << sar));
		}
		ctx->veq[m].sar = (int16_t)sar;
	}
}
#endif

bool ofdm_chavg;
bool ofdm_finerot = true;

/*
 * The channel does not change from frame to frame (the same cable, filters and gain); a
 * frame's own window start and carrier phase do. ctx->havg averages the pilots' response
 * over frames; each frame fits it a complex gain and a phase slope over the subcarriers
 * (its timing) against its own pilots, adds itself to the average in that frame of
 * reference, and takes the average, so turned, as its channel: 2 numbers from the pilots
 * instead of a noisy value per subcarrier. A poor fit (the gain or the path changed)
 * starts the average over. Returns false while it has too few frames (h unchanged).
 */
static bool chan_avg(struct ofdm_ctx *ctx, struct ofdm_cf *h)
{
	unsigned int c = ctx->cfg.channels;
	struct ofdm_cf *av = ctx->havg, r = {0.0f, 0.0f}, s = {0.0f, 0.0f}, zp = {0.0f, 0.0f};
	struct ofdm_cf ph, st, g, gi;
	float ea = 0.0f, eh = 0.0f, th, a0, al, g2;

	if (ctx->havg_n == 0U) {
		memcpy(av, h, c * sizeof(h[0]));
		ctx->havg_n = 1;
		return false;
	}
	/* The slope: the step of h conj(av) between neighbouring subcarriers. */
	for (unsigned int a = 0; a < c; a++) {
		struct ofdm_cf z = {h[a].re * av[a].re + h[a].im * av[a].im,
				    h[a].im * av[a].re - h[a].re * av[a].im};

		if (a > 0U && sub_freq(c, a) - sub_freq(c, a - 1U) == 1) {
			r.re += z.re * zp.re + z.im * zp.im;
			r.im += z.im * zp.re - z.re * zp.im;
		}
		zp = z;
	}
	th = (r.re != 0.0f || r.im != 0.0f) ? atan2f(r.im, r.re) : 0.0f;
	/* The gain: h against av turned by the slope, by least squares. */
	a0 = -th * (float)sub_freq(c, 0);
	ph = (struct ofdm_cf){cosf(a0), sinf(a0)};
	st = (struct ofdm_cf){cosf(th), -sinf(th)};
	for (unsigned int a = 0; a < c; a++) {
		struct ofdm_cf z = {h[a].re * av[a].re + h[a].im * av[a].im,
				    h[a].im * av[a].re - h[a].re * av[a].im};

		z = cmul(z, ph);
		s.re += z.re;
		s.im += z.im;
		ea += av[a].re * av[a].re + av[a].im * av[a].im;
		eh += h[a].re * h[a].re + h[a].im * h[a].im;
		if (a + 1U < c) {
			for (int d = sub_freq(c, a + 1U) - sub_freq(c, a); d > 0; d--) {
				ph = cmul(ph, st);
			}
		}
	}
	if (ea <= 0.0f || eh <= 0.0f || s.re * s.re + s.im * s.im < 0.5f * ea * eh) {
		/* Not the same channel any more: start over from this frame. */
		memcpy(av, h, c * sizeof(h[0]));
		ctx->havg_n = 1;
		return false;
	}
	g = (struct ofdm_cf){s.re / ea, s.im / ea};
	g2 = g.re * g.re + g.im * g.im;
	gi = (struct ofdm_cf){g.re / g2, -g.im / g2};
	/* Up to 16 frames by count, then 1/16 of each new one. */
	al = 1.0f / (float)(ctx->havg_n < 16U ? ctx->havg_n + 1U : 16U);
	ph = (struct ofdm_cf){cosf(-a0), sinf(-a0)};
	st = (struct ofdm_cf){st.re, -st.im};
	for (unsigned int a = 0; a < c; a++) {
		/* This frame's turn of the average: g exp(j th f). */
		struct ofdm_cf m = cmul(g, ph), mi = cmul(gi, (struct ofdm_cf){ph.re, -ph.im});
		struct ofdm_cf al_h = cmul(h[a], mi);

		av[a].re += al * (al_h.re - av[a].re);
		av[a].im += al * (al_h.im - av[a].im);
		h[a] = cmul(av[a], m);
		if (a + 1U < c) {
			for (int d = sub_freq(c, a + 1U) - sub_freq(c, a); d > 0; d--) {
				ph = cmul(ph, st);
			}
		}
	}
	if (ctx->havg_n < 0xffffU) {
		ctx->havg_n++;
	}
	return true;
}

/*
 * Channel per subcarrier from the pilots (into ctx->eq; equalizer() inverts it): each one's
 * known sign undone and its
 * window phase (rx.u) too, averaged; optionally smoothed over the neighbours
 * (3 taps 1 2 1, after taking out the phase slope between subcarriers that a
 * timing offset puts there); then conj(H) / |H|^2.
 */
static void channel(struct ofdm_ctx *ctx, const uint32_t *w)
{
	unsigned int c = ctx->cfg.channels, np = ctx->cfg.pilots;
	struct ofdm_cf *h = ctx->eq, rho = {0.0f, 0.0f}, rho2;
	float g = 1.0f / (float)np;

	struct ofdm_cf dr = {0.0f, 0.0f};

	float tsum = 0.0f;

	memset(h, 0, c * sizeof(h[0]));
	for (unsigned int p = 0; p < np; p++) {
		struct ofdm_cf u;

		take_slot(ctx, &ctx->w0, w, 1U + p, false);
		/* When the pilots were captured: a pilot read one period earlier counts as such. */
		tsum += ctx->w0.tslot;
		u = ctx->w0.u;
		for (unsigned int a = 0; a < c; a++) {
			struct ofdm_cf y = cmul(fft_out(ctx, &ctx->w0, ctx->binr[a]), u);
			float s = pn_sign(PILOT_SEED, a);

			/* Pilot p against the sum of the ones before: the phase step per slot. */
			if (p > 0U) {
				struct ofdm_cf q = h[a];

				dr.re += s * (y.re * q.re + y.im * q.im);
				dr.im += s * (y.im * q.re - y.re * q.im);
			}
			h[a].re += s * g * y.re;
			h[a].im += s * g * y.im;
		}
	}
	/* The sample clock drift's time reference: the channel is measured at the pilots. */
	ctx->rx.sref = tsum / (float)np;
	/*
	 * A residual carrier offset turns each pilot by the same step theta: the
	 * tracking starts undoing it, rate exp(-j theta) per slot, from the
	 * channel's reference halfway through the pilots (the average).
	 */
	{
		float th = (dr.re != 0.0f || dr.im != 0.0f) ? atan2f(dr.im, dr.re) : 0.0f;
		float a = -0.5f * th * (float)(np - 1U);

		ctx->w0.rate = (struct ofdm_cf){cosf(th), -sinf(th)};
		ctx->w0.trk = (struct ofdm_cf){cosf(a), sinf(a)};
	}
	/*
	 * The phase step between neighbouring subcarriers: a window d samples
	 * early in the prefix turns subcarrier k by -tau k d / N, the channel's
	 * delay adds to it. Kept as rx.delay for the fine timing.
	 */
	for (unsigned int a = 0; a + 1U < c; a++) {
		if (sub_freq(c, a + 1U) - sub_freq(c, a) == 1) {
			rho.re += h[a + 1].re * h[a].re + h[a + 1].im * h[a].im;
			rho.im += h[a + 1].im * h[a].re - h[a + 1].re * h[a].im;
		}
	}
	ctx->rx.delay = (rho.re != 0.0f || rho.im != 0.0f)
				? -atan2f(rho.im, rho.re) * (float)ctx->rnfft / ESP_SDR_TAU_F
				: 0.0f;
	if (ofdm_chavg && chan_avg(ctx, h)) {
		return;
	}
	if (ctx->cfg.smooth) {
		struct ofdm_cf *hs = ctx->w0.buf;
		float m;

		m = sqrtf(rho.re * rho.re + rho.im * rho.im);
		rho = m > 0.0f ? (struct ofdm_cf){rho.re / m, rho.im / m} : (struct ofdm_cf){1, 0};
		rho2 = (struct ofdm_cf){rho.re * rho.re - rho.im * rho.im, 2.0f * rho.re * rho.im};
		for (unsigned int a = 0; a < c; a++) {
			struct ofdm_cf acc = {2.0f * h[a].re, 2.0f * h[a].im};
			float ws = 2.0f;

			/* A neighbour df subcarriers away, turned back by rho^-df. */
			if (a > 0U) {
				struct ofdm_cf r = sub_freq(c, a) - sub_freq(c, a - 1U) == 1 ? rho : rho2;

				acc.re += h[a - 1].re * r.re - h[a - 1].im * r.im;
				acc.im += h[a - 1].re * r.im + h[a - 1].im * r.re;
				ws += 1.0f;
			}
			if (a + 1U < c) {
				struct ofdm_cf r = sub_freq(c, a + 1U) - sub_freq(c, a) == 1 ? rho : rho2;

				acc.re += h[a + 1].re * r.re + h[a + 1].im * r.im;
				acc.im += h[a + 1].im * r.re - h[a + 1].re * r.im;
				ws += 1.0f;
			}
			/* ws is 3 or 4 (the S3 FPU has no divide). */
			ws = ws > 3.5f ? 0.25f : 0.33333333f;
			hs[a] = (struct ofdm_cf){acc.re * ws, acc.im * ws};
		}
		memcpy(h, hs, c * sizeof(h[0]));
	}
}

/*
 * The channel ctx->eq holds (H, measured at a window @p shift samples earlier) as the
 * equalizer, conj(H') / |H'|^2: moving the window later by shift samples turns subcarrier
 * f by 2 pi f shift / N, and the carrier offset adds 2 pi eps shift to all of them.
 */
static void equalizer(struct ofdm_ctx *ctx, int shift)
{
	unsigned int c = ctx->cfg.channels;
	struct ofdm_cf *h = ctx->eq, ph = {1.0f, 0.0f}, st = {1.0f, 0.0f};

	if (shift != 0) {
		float a0 = ESP_SDR_TAU_F * (float)shift *
			   ((float)sub_freq(c, 0) / (float)ctx->rnfft + ctx->rx.eps);
		float as = ESP_SDR_TAU_F * (float)shift / (float)ctx->rnfft;

		ph = (struct ofdm_cf){cosf(a0), sinf(a0)};
		st = (struct ofdm_cf){cosf(as), sinf(as)};
	}
	for (unsigned int a = 0; a < c; a++) {
		struct ofdm_cf v = cmul(h[a], ph);
		float h2 = v.re * v.re + v.im * v.im, inv = h2 > 0.0f ? 1.0f / h2 : 0.0f;

		h[a] = (struct ofdm_cf){v.re * inv, -v.im * inv};
		if (a + 1U < c) {
			for (int d = sub_freq(c, a + 1U) - sub_freq(c, a); d > 0; d--) {
				ph = cmul(ph, st);
			}
		}
	}
#if defined(CONFIG_APP_OFDM_ESP_DSP) && OFDM_FFT16
	veq_build(ctx);
#endif
}

int ofdm_rx_begin(struct ofdm_ctx *ctx, const uint32_t *words, size_t n, uint8_t *hdr,
		  struct ofdm_rx_info *info)
{
	return ofdm_rx_begin_period(ctx, words, n, ctx->rlen, hdr, info);
}

int ofdm_rx_begin_period(struct ofdm_ctx *ctx, const uint32_t *words, size_t n, size_t period,
			 uint8_t *hdr, struct ofdm_rx_info *info)
{
	struct ofdm_rxstate *rx = &ctx->rx;
	int32_t si = 0, sq = 0, cnt = 0;
	int per = (int)period, tmax = (int)n - per;
	float best = 0.0f, ang = 0.0f, a;
	unsigned int c = ctx->cfg.channels, nh = ctx->cfg.hdr_bits, h0 = 1U + ctx->cfg.pilots;
	uint32_t t0 = clk(), t1;

	int tlim = tmax;

	memset(info, 0, sizeof(*info));
	rx->n = (int)n;
	rx->per = per;
	if ((int)n >= per + (int)ctx->rnfft) {
		/*
		 * Every slot lies whole in the window or one period earlier: a start anywhere
		 * the search can still measure it will do, and so will the next copy's.
		 */
		tmax = tlim = (int)n - (int)ctx->rnfft - 1;
	}
	if (tmax < 0) {
		return -1;
	}
	/* The frame repeats every rlen samples: one period (plus its prefix plateau) holds a start. */
	if (tmax > per + (int)ctx->rcp) {
		tmax = per + (int)ctx->rcp;
	}
	for (size_t k = 0; k < n; k += 4) {
		si += esp_sdr_rx_i(words[k]);
		sq += esp_sdr_rx_q(words[k]);
		cnt++;
	}
	rx->dci = (si + (si >= 0 ? cnt / 2 : -cnt / 2)) / cnt;
	rx->dcq = (sq + (sq >= 0 ? cnt / 2 : -cnt / 2)) / cnt;
	rx->cw = (struct ofdm_cf){1.0f, 0.0f};
	if ((int)n > per) {
		/* The overlap: the same samples a period apart, turned by the carrier offset. */
		int64_t cr = 0, ci = 0;
		float m;

		for (int i = per; i < (int)n; i++) {
			int ai = esp_sdr_rx_i(words[i]) - rx->dci, aq = esp_sdr_rx_q(words[i]) - rx->dcq;
			int bi = esp_sdr_rx_i(words[i - per]) - rx->dci;
			int bq = esp_sdr_rx_q(words[i - per]) - rx->dcq;

			cr += ai * bi + aq * bq;
			ci += aq * bi - ai * bq;
		}
		m = sqrtf((float)cr * (float)cr + (float)ci * (float)ci);
		if (m > 0.0f) {
			rx->cw = (struct ofdm_cf){(float)cr / m, (float)ci / m};
		}
	}
	rx->start = search(ctx, words, tmax, tlim, per, rx->dci, rx->dcq, &best, &ang);
	t1 = clk();
	info->prof[0] = t1 - t0;
	info->metric = best;
	if (rx->start < 0 || best < METRIC_MIN) {
		return -1;
	}
	/* Phase over half a symbol: half a turn per subcarrier spacing. */
	info->cfo_frac = ang / (ESP_SDR_TAU_F / 2.0f);
	info->cfo_hz = info->cfo_frac * ctx->spacing_hz;
	info->start = rx->start;
	/* Cycles per receive sample. */
	rx->eps = info->cfo_frac / (float)ctx->rnfft;
	rot_init(ctx, rx->eps);
	rx->gen++;
	/* Until channel() measures it from the pilots' capture times. */
	rx->sref = 0.5f * (float)(1U + ctx->cfg.pilots);
	rx->dslot = ofdm_sfo != 0 && ofdm_carrier_hz > 0.0f
			    ? (float)ofdm_sfo * info->cfo_hz / ofdm_carrier_hz * ESP_SDR_TAU_F *
				      (float)ctx->rslot / (float)ctx->rnfft
			    : 0.0f;
	a = -ESP_SDR_TAU_F * rx->eps * (float)ctx->rslot;
	rx->us = (struct ofdm_cf){cosf(a), sinf(a)};
	ctx->w0.u = (struct ofdm_cf){1.0f, 0.0f};
	ctx->w0.wrapped = false;

	channel(ctx, words);
	/*
	 * Fine timing from the pilots: the window should sit in the middle of
	 * the prefix, where a receive filter's ringing on either side clears it
	 * (the preamble's plateau edge is only good to some samples). Moved,
	 * and the channel measured again, if it is a sample or more off.
	 */
	{
		/* Later by how much more than half the prefix the window is early. */
		int shift = (int)lrintf(rx->delay - (float)ctx->rcp / 2.0f);

		if (shift != 0 && shift > -(int)ctx->rcp && shift < (int)ctx->rcp &&
		    rx->start + shift >= 0 && rx->start + shift <= tlim) {
			rx->start += shift;
			info->start = rx->start;
			if (!ofdm_finerot) {
				/* Measured again at the new window. */
				ctx->w0.u = (struct ofdm_cf){1.0f, 0.0f};
				ctx->w0.wrapped = false;
				channel(ctx, words);
				shift = 0;
			}
		} else {
			shift = 0;
		}
		/* The channel turned to the new window (exactly: a whole number of samples). */
		equalizer(ctx, shift);
	}

	/* Header: BPSK, its bits repeated over the subcarriers and summed soft. */
	memset(rx->soft, 0, sizeof(rx->soft));
	for (unsigned int s = 0; s < ctx->nhdr; s++) {
		struct ofdm_cf r;

		take_slot(ctx, &ctx->w0, words, h0 + s, false);
		r = track_any(ctx, &ctx->w0, OFDM_BPSK);
		for (unsigned int k = 0; k < c; k++) {
			rx->soft[(s * c + k) % nh] += eq_rot(ctx, &ctx->w0, k, r.re, r.im).re;
		}
	}
	if (nh > 0U) {
		memset(hdr, 0, (nh + 7U) / 8U);
		for (unsigned int k = 0; k < nh; k++) {
			if (rx->soft[k] > 0.0f) {
				hdr[k >> 3] |= (uint8_t)(0x80U >> (k & 7U));
			}
		}
	}
	rx->slot = h0 + ctx->nhdr;
	info->prof[1] = clk() - t1;
	return 0;
}

/* Slot of payload symbol s (after the mid pilot one further). */
static inline unsigned int data_slot(const struct ofdm_ctx *ctx, unsigned int s)
{
	return ctx->rx.slot + s + (ctx->cfg.mid_pilot && s >= ctx->mid ? 1U : 0U);
}

void ofdm_rx_fork(const struct ofdm_ctx *ctx, struct ofdm_worker *wk, const uint32_t *words)
{
	unsigned int ps = ctx->rx.slot + ctx->mid;
	/* The window phase before the pilot's slot (take_slot() steps it once). */
	float a = -ESP_SDR_TAU_F * ctx->rx.eps * (float)ctx->rslot * (float)(ps - 2U);
	struct ofdm_cf acc = {0.0f, 0.0f};
	float m;

	wk->u = (struct ofdm_cf){cosf(a), sinf(a)};
	wk->wrapped = false;
	wk->rate = ctx->w0.rate;
	take_slot(ctx, wk, words, ps, false);
	/* The pilot equalized: its phase against the channel's is the one to undo. */
	for (unsigned int k = 0; k < ctx->cfg.channels; k++) {
		struct ofdm_cf z = eq_rot(ctx, wk, k, wk->u.re, wk->u.im);
		float sg = pn_sign(PILOT_SEED, k);

		acc.re += sg * z.re;
		acc.im += sg * z.im;
	}
	m = sqrtf(acc.re * acc.re + acc.im * acc.im);
	wk->trk = m > 0.0f ? (struct ofdm_cf){acc.re / m, -acc.im / m} : (struct ofdm_cf){1, 0};
}

void ofdm_rx_part(const struct ofdm_ctx *ctx, struct ofdm_worker *wk, const uint32_t *words,
		  enum ofdm_mod mod, unsigned int s0, unsigned int s1, uint8_t *bits, float *err)
{
	struct bitsink k = {.out = bits};

	for (unsigned int s = s0; s < s1; s++) {
		take_slot(ctx, wk, words, data_slot(ctx, s), true);
		data_symbol(ctx, wk, mod, &k, err);
	}
	sink_flush(&k);
}

float ofdm_rx_mer(const struct ofdm_ctx *ctx, float err)
{
	return 10.0f * log10f((float)ctx->ndata * (float)ctx->cfg.channels /
			      (err > 1e-12f ? err : 1e-12f));
}

void ofdm_rx_finish(struct ofdm_ctx *ctx, const uint32_t *words, enum ofdm_mod mod, uint8_t *bits,
		    struct ofdm_rx_info *info)
{
	float err = 0.0f;
	uint32_t t0 = clk();

	ofdm_rx_part(ctx, &ctx->w0, words, mod, 0, ctx->mid, bits, &err);
	if (ctx->mid < ctx->ndata) {
		/* On one CPU too: re-anchored on the mid pilot. */
		size_t off = (size_t)ctx->mid * ctx->cfg.channels * ofdm_mod_bits(mod) / 8U;

		ofdm_rx_fork(ctx, &ctx->w0, words);
		ofdm_rx_part(ctx, &ctx->w0, words, mod, ctx->mid, ctx->ndata, bits + off, &err);
	}
	info->prof[2] = clk() - t0;
	info->mer_db = ofdm_rx_mer(ctx, err);
}
