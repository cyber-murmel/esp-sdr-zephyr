/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host simulator for gfsk_demod.c: Bluetooth LE and BR bursts as GFSK at 16 MS/s with the
 * impairments of the S3 receive path, then the exact ring front end (analog low pass, 10 bit
 * ADC, mix by -fs/4, every 4th sample), then the real demodulator. Prints symbol and packet
 * error rates against SNR for variants of the demodulator's parameters.
 *
 *   cc -O2 -Wall -DGFSK_TUNABLE -I../src -I../../../include ../src/gfsk_demod.c ../src/ubt_rssi.c gfsk_sim.c \
 *      -lm -o gfsk_sim
 *   MOD=ble SNRS=8:20:1 ./gfsk_sim                 (BER table, one line per SNR)
 *   MOD=br CFO=100000 PPM=-100 SWEEP=disc:1,2 ./gfsk_sim
 *   MODE=squelch SQ=130 HOLD=8 GAP=300 ./gfsk_sim  (detector: opening, false alarms, preamble)
 *   MODE=power ./gfsk_sim                          (reported pwr against the true level)
 *
 * SNR is the carrier power over the noise power in 1 MHz (Eb/N0 at 1 Mb/s). Environment:
 *   MOD (ble, br), SNRS (min:max:step), SNR (one point), NB (bursts per chunk), MINBITS,
 *   MAXERR, PL (payload bits), CFO (Hz), PPM (symbol clock offset), DC (ADC codes, I and Q),
 *   AMP (signal dBFS), ACI (dB, a neighbour channel at ACIOFF MHz, default 1 for br and 2 for
 *   ble), WIFI (dB, band limited noise WBW MHz wide, WFC MHz from the LO), GAP (us), SEED,
 *   CHUNK (largest push), ALWAYS (1/0), SQ (squelch pwr), HOLD, SWEEP=name:v1,v2,..,
 *   VARIANTS="name=v,name=v;..." with the names of struct gfsk_tune, REF (1: also run a
 *   floating point discriminator receiver at 16 MS/s with genie timing), QUANT (ADC bits).
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_sdr/esp_sdr_dsp.h>

#include "gfsk_demod.h"

#define FS16   16.0e6

/* ---------------------------------------------------------------------------------------- */
/* Environment, random numbers                                                              */
/* ---------------------------------------------------------------------------------------- */

static double env_f(const char *k, double d)
{
	const char *v = getenv(k);

	return v ? atof(v) : d;
}

static const char *env_s(const char *k, const char *d)
{
	const char *v = getenv(k);

	return v ? v : d;
}

static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;

static uint32_t rnd32(void)
{
	rng_state ^= rng_state >> 12;
	rng_state ^= rng_state << 25;
	rng_state ^= rng_state >> 27;
	return (uint32_t)((rng_state * 0x2545F4914F6CDD1DULL) >> 32);
}

static double rnd01(void)
{
	return (rnd32() + 0.5) / 4294967296.0;
}

static double randn(void)
{
	static int have;
	static double spare;
	double u, v, r;

	if (have) {
		have = 0;
		return spare;
	}
	u = rnd01();
	v = rnd01();
	r = sqrt(-2.0 * log(u));
	spare = r * sin(ESP_SDR_TAU * v);
	have = 1;
	return r * cos(ESP_SDR_TAU * v);
}

/* ---------------------------------------------------------------------------------------- */
/* Packets and the GFSK modulator                                                           */
/* ---------------------------------------------------------------------------------------- */

struct cf {
	float re, im;
};

struct burst {
	double t16;     /* start of the first symbol, 16 MS/s samples from the run start */
	double ts16;    /* symbol period in samples */
	uint32_t nsym, nwarm, naa;
	uint8_t *bits;
};

static void put_bits(uint8_t *b, uint32_t *n, uint32_t v, unsigned int len)
{
	for (unsigned int k = 0; k < len; k++) {
		b[(*n)++] = (v >> k) & 1U; /* LSB first, as on air */
	}
}

/* Advertising-like LE packet: 0xAA preamble, access address, random PDU and CRC. */
static void make_ble(struct burst *bu, unsigned int payload_bits)
{
	uint32_t n = 0;

	bu->bits = malloc(8 + 32 + payload_bits + 8);
	put_bits(bu->bits, &n, 0xAAU, 8);
	put_bits(bu->bits, &n, 0x8E89BED6U, 32);
	bu->naa = 32;
	bu->nwarm = n;
	for (unsigned int k = 0; k < payload_bits; k++) {
		bu->bits[n++] = rnd32() & 1U;
	}
	bu->nsym = n;
}

/* BR-like: 4 bit preamble, 64 bit sync word (random), 4 bit trailer, random payload. */
static void make_br(struct burst *bu, unsigned int payload_bits)
{
	uint32_t n = 0;
	uint8_t sync[64];

	bu->bits = malloc(4 + 64 + 4 + payload_bits + 8);
	for (int k = 0; k < 64; k++) {
		sync[k] = rnd32() & 1U;
	}
	/* alternating, the last bit opposite to the first sync bit */
	for (int k = 0; k < 4; k++) {
		bu->bits[n++] = (uint8_t)(((3 - k) & 1) ? !sync[0] : sync[0]);
	}
	for (int k = 0; k < 64; k++) {
		bu->bits[n++] = sync[k];
	}
	for (int k = 0; k < 4; k++) {
		bu->bits[n++] = (uint8_t)(((k & 1) == 0) ? !sync[63] : sync[63]);
	}
	bu->naa = 64;
	bu->nwarm = n;
	for (unsigned int k = 0; k < payload_bits; k++) {
		bu->bits[n++] = rnd32() & 1U;
	}
	bu->nsym = n;
}

#define GH 18
static double gauss_taps[2 * GH + 1];

static void gauss_init(double bt)
{
	double sigma = sqrt(log(2.0)) / (ESP_SDR_TAU * bt * 1.0e6) * FS16; /* in samples */
	double sum = 0;

	for (int j = -GH; j <= GH; j++) {
		gauss_taps[j + GH] = exp(-0.5 * (j / sigma) * (j / sigma));
		sum += gauss_taps[j + GH];
	}
	for (int j = 0; j <= 2 * GH; j++) {
		gauss_taps[j] /= sum;
	}
}

/*
 * Add one GFSK burst to dst[0..len): symbols a[0..nsym) starting at sample t0 (fractional),
 * period ts, modulation index h, amplitude amp, frequency f_off + cfo from the LO, global
 * sample index base (for the carrier phase).
 */
static void modulate(struct cf *dst, size_t len, const uint8_t *bits, uint32_t nsym, double t0,
		     double ts, double h, double amp, double f_hz, double phase0, int64_t base,
		     int ramp)
{
	double *nrz = calloc(len, sizeof(double));
	double phi = 0;

	for (uint32_t k = 0; k < nsym; k++) {
		double a = bits[k] ? 1.0 : -1.0;
		double s = t0 + k * ts, e = s + ts;
		long n0 = (long)ceil(s - 0.5), n1 = (long)floor(e + 0.5);

		for (long n = n0; n <= n1; n++) {
			double lo = s > n - 0.5 ? s : n - 0.5, hi = e < n + 0.5 ? e : n + 0.5;

			if (n >= 0 && (size_t)n < len && hi > lo) {
				nrz[n] += a * (hi - lo);
			}
		}
	}
	for (size_t n = 0; n < len; n++) {
		double acc = 0, env = 1.0;
		double tend = t0 + nsym * ts;

		for (int j = -GH; j <= GH; j++) {
			long m = (long)n - j;

			if (m >= 0 && (size_t)m < len) {
				acc += gauss_taps[j + GH] * nrz[m];
			}
		}
		phi += ESP_SDR_TAU * (h * 1.0e6 / 2.0 * acc) / FS16;
		if (ramp) {
			double x = (double)n;

			if (x < t0 - 16) {
				env = 0;
			} else if (x < t0) {
				env = 0.5 - 0.5 * cos(ESP_SDR_TAU / 2.0 * (x - (t0 - 16)) / 16.0);
			} else if (x > tend + 16) {
				env = 0;
			} else if (x > tend) {
				env = 0.5 + 0.5 * cos(ESP_SDR_TAU / 2.0 * (x - tend) / 16.0);
			}
		}
		if (env > 0) {
			double ang = phi + ESP_SDR_TAU * f_hz * (double)(base + (int64_t)n) / FS16 + phase0;

			dst[n].re += (float)(amp * env * cos(ang));
			dst[n].im += (float)(amp * env * sin(ang));
		}
	}
	free(nrz);
}

/* ---------------------------------------------------------------------------------------- */
/* Front end                                                                                */
/* ---------------------------------------------------------------------------------------- */

struct biquad {
	double b0, b1, b2, a1, a2, z1, z2;
};

/* 4th order Butterworth low pass, two sections. */
static void lpf_design(struct biquad s[2], double fc)
{
	double w = tan(ESP_SDR_TAU / 2.0 * fc / FS16);

	for (int k = 0; k < 2; k++) {
		double th = ESP_SDR_TAU / 2.0 * (2 * k + 1) / 8.0 + ESP_SDR_TAU / 4.0; /* pole angles */
		double q = -1.0 / (2.0 * cos(th));
		double n = 1.0 + w / q + w * w;

		s[k].b0 = w * w / n;
		s[k].b1 = 2.0 * s[k].b0;
		s[k].b2 = s[k].b0;
		s[k].a1 = 2.0 * (w * w - 1.0) / n;
		s[k].a2 = (1.0 - w / q + w * w) / n;
		s[k].z1 = s[k].z2 = 0;
	}
}

static double lpf_run(struct biquad s[2], double x)
{
	for (int k = 0; k < 2; k++) {
		double y = s[k].b0 * x + s[k].z1;

		s[k].z1 = s[k].b1 * x - s[k].a1 * y + s[k].z2;
		s[k].z2 = s[k].b2 * x - s[k].a2 * y;
		x = y;
	}
	return x;
}

struct params {
	int ble;
	double snr_db, h, bt;
	double cfo, ppm, dc, amp_dbfs, aci_db, aci_off, wifi_db, wbw, wfc, gap_us;
	int quant, payload, ref, sigoff;
};

struct stream {
	int16_t *i, *q;
	size_t n;
	struct burst *bu;
	int nb;
	int ph0;          /* subsample phase */
	size_t clipped;
	/* genie reference receiver decisions, per burst per symbol (or NULL) */
	uint8_t **ref_bits;
};

static double amp_of(const struct params *p)
{
	return 8192.0 * pow(10.0, p->amp_dbfs / 20.0);
}


/*
 * Genie reference: floating point at 16 MS/s on the noisy signal before the analog filter, exact
 * carrier derotation and symbol timing, a Gaussian IF filter, delay-1 phase discriminator
 * integrated over the symbol. The best a non-coherent FM receiver gets without the 4 MS/s front end.
 */
static void ref_demod(struct stream *st, const struct params *p, const float *yi, const float *yq,
		      size_t len)
{
	enum { RH = 40 };
	double bw = env_f("RBW", 0.6), sigma = sqrt(log(2.0)) / (ESP_SDR_TAU * bw * 1e6) * FS16;
	double taps[2 * RH + 1], sum = 0;
	double *di = malloc(len * sizeof(double)), *dq = malloc(len * sizeof(double));
	double *ph = calloc(len, sizeof(double));
	double f = -3.0e6 + p->cfo;

	for (int j = -RH; j <= RH; j++) {
		taps[j + RH] = exp(-0.5 * (j / sigma) * (j / sigma));
		sum += taps[j + RH];
	}
	for (size_t n = 0; n < len; n++) {
		double a = -ESP_SDR_TAU * f * (double)n / FS16;

		di[n] = yi[n] * cos(a) - yq[n] * sin(a);
		dq[n] = yi[n] * sin(a) + yq[n] * cos(a);
	}
	st->ref_bits = calloc((size_t)st->nb, sizeof(uint8_t *));
	for (int b = 0; b < st->nb; b++) {
		const struct burst *bu = &st->bu[b];
		size_t s0 = (size_t)(bu->t16 - 20.0), s1 = (size_t)(bu->t16 + bu->nsym * bu->ts16 + 20.0);
		double pi = 0, pq = 0;

		st->ref_bits[b] = malloc(bu->nsym);
		memset(st->ref_bits[b], 0, bu->nsym);
		if (s1 > len - RH - 2) {
			s1 = len - RH - 2;
		}
		for (size_t n = s0; n < s1; n++) {
			double a = 0, c = 0;

			for (int j = -RH; j <= RH; j++) {
				a += taps[j + RH] * di[n + (size_t)(long)j];
				c += taps[j + RH] * dq[n + (size_t)(long)j];
			}
			a /= sum;
			c /= sum;
			ph[n] = atan2(c * pi - a * pq, a * pi + c * pq); /* arg(y[n] conj(y[n-1])) */
			pi = a;
			pq = c;
		}
		for (uint32_t k = 0; k < bu->nsym; k++) {
			double lo = bu->t16 + k * bu->ts16, hi = lo + bu->ts16, acc = 0;

			for (long n = (long)ceil(lo + 0.5); (double)n - 0.5 < hi; n++) {
				if ((size_t)n > s0 && (size_t)n < s1) {
					acc += ph[n];
				}
			}
			st->ref_bits[b][k] = acc > 0;
		}
	}
	free(di);
	free(dq);
	free(ph);
}

/* Build one run: nb bursts, noise, interferers, front end. */
static void make_stream(struct stream *st, const struct params *p, int nb)
{
	double amp = amp_of(p);
	double ts16 = FS16 / 1.0e6 * (1.0 + p->ppm * 1e-6);
	double t = 200.0;
	struct burst *bu = calloc((size_t)nb, sizeof(*bu));
	size_t len;
	struct cf *x;
	struct biquad li[2], lq[2];
	size_t n4 = 0;

	for (int b = 0; b < nb; b++) {
		if (p->ble) {
			make_ble(&bu[b], (unsigned int)p->payload);
		} else {
			make_br(&bu[b], (unsigned int)p->payload);
		}
		t += p->gap_us * 16.0 * (0.5 + rnd01()) + 16.0 * 20.0;
		bu[b].t16 = t + rnd01();
		bu[b].ts16 = ts16;
		t = bu[b].t16 + bu[b].nsym * ts16 + 16.0 * 12.0;
	}
	len = (size_t)(t + p->gap_us * 16.0 + 4000.0);
	len &= ~(size_t)3;
	x = calloc(len, sizeof(*x));

	for (int b = 0; b < nb; b++) {
		/* each burst into its own span so the dst array stays small */
		size_t s0 = (size_t)(bu[b].t16 - 64.0), s1 = (size_t)(bu[b].t16 + bu[b].nsym * ts16 + 64.0);

		if (s1 > len) {
			s1 = len;
		}
		modulate(x + s0, s1 - s0, bu[b].bits, bu[b].nsym, bu[b].t16 - (double)s0, ts16, p->h,
			 p->sigoff ? 0.0 : amp, -3.0e6 + p->cfo, ESP_SDR_TAU * rnd01(), (int64_t)s0, 1);
	}
	if (p->aci_db > -200.0) {
		/* continuous neighbour with random data */
		size_t nsym = (size_t)((double)len / 16.0) + 4;
		uint8_t *rb = malloc(nsym);

		for (size_t k = 0; k < nsym; k++) {
			rb[k] = rnd32() & 1U;
		}
		modulate(x, len, rb, (uint32_t)nsym, 16.0 * rnd01(), 16.0, p->h,
			 amp * pow(10.0, p->aci_db / 20.0), (-3.0 + p->aci_off) * 1.0e6, ESP_SDR_TAU * rnd01(),
			 0, 0);
		free(rb);
	}
	if (p->wifi_db > -200.0) {
		enum { WT = 64 };
		double taps[WT + 1], sum = 0;
		double pw = amp * amp * pow(10.0, p->wifi_db / 10.0);
		float *wi = malloc((len + WT) * sizeof(float)), *wq = malloc((len + WT) * sizeof(float));
		double fc = p->wbw / 2.0 * 1e6 / FS16;

		for (int j = 0; j <= WT; j++) {
			double m = j - WT / 2.0;
			double s = m == 0 ? 2.0 * fc : sin(ESP_SDR_TAU * fc * m) / (ESP_SDR_TAU / 2.0 * m);

			taps[j] = s * (0.54 - 0.46 * cos(ESP_SDR_TAU * j / WT));
			sum += taps[j] * taps[j];
		}
		for (size_t n = 0; n < len + WT; n++) {
			wi[n] = (float)randn();
			wq[n] = (float)randn();
		}
		for (size_t n = 0; n < len; n++) {
			double a = 0, b = 0, g, ang;

			for (int j = 0; j <= WT; j++) {
				a += taps[j] * wi[n + (size_t)j];
				b += taps[j] * wq[n + (size_t)j];
			}
			/* complex noise: unit power per complex sample after normalisation */
			g = sqrt(pw / (2.0 * sum));
			ang = ESP_SDR_TAU * p->wfc * 1e6 * (double)n / FS16;
			x[n].re += (float)(g * (a * cos(ang) - b * sin(ang)));
			x[n].im += (float)(g * (a * sin(ang) + b * cos(ang)));
		}
		free(wi);
		free(wq);
	}

	/* receiver: noise, analog low pass, ADC, mix by -fs/4, every 4th pair */
	lpf_design(li, 6.5e6);
	lpf_design(lq, 6.5e6);
	st->ph0 = (int)(rnd32() & 3U);
	st->i = malloc(len / 4 * sizeof(int16_t) + 16);
	st->q = malloc(len / 4 * sizeof(int16_t) + 16);
	st->clipped = 0;
	float *ry = NULL, *rq = NULL;

	if (p->ref) {
		ry = malloc(len * sizeof(float));
		rq = malloc(len * sizeof(float));
	}
	{
		double sn = sqrt(16.0 * amp * amp / pow(10.0, p->snr_db / 10.0) / 2.0);
		double lsb = 16.0 * pow(2.0, 10 - p->quant);
		int hi = (1 << (p->quant - 1)) - 1, lo = -(1 << (p->quant - 1));
		double scale = (double)(1 << (p->quant - 10)); /* code units after scaling to 10 bit */

		(void)scale;
		for (size_t n = 0; n < len; n++) {
			double ni = x[n].re + sn * randn(), nq = x[n].im + sn * randn();
			double vi = lpf_run(li, ni) + p->dc * 16.0;
			double vq = lpf_run(lq, nq) + p->dc * 16.0;

			if (ry) {
				ry[n] = (float)ni;
				rq[n] = (float)nq;
			}
			long ci = lrint(vi / lsb), cq = lrint(vq / lsb);

			if (ci > hi || ci < lo || cq > hi || cq < lo) {
				st->clipped++;
			}
			ci = ci > hi ? hi : (ci < lo ? lo : ci);
			cq = cq > hi ? hi : (cq < lo ? lo : cq);
			if (((n - (size_t)st->ph0) & 3U) == 0U && n >= (size_t)st->ph0) {
				long mi = ci * (long)lsb, mq = cq * (long)lsb, oi, oq;

				switch (n & 3U) { /* (-j)^n */
				case 0: oi = mi; oq = mq; break;
				case 1: oi = mq; oq = -mi; break;
				case 2: oi = -mi; oq = -mq; break;
				default: oi = -mq; oq = mi; break;
				}
				st->i[n4] = (int16_t)oi;
				st->q[n4] = (int16_t)oq;
				n4++;
			}
		}
	}
	st->n = n4;
	st->bu = bu;
	st->nb = nb;
	st->ref_bits = NULL;

	if (p->ref) {
		ref_demod(st, p, ry, rq, len);
		free(ry);
		free(rq);
	}
	free(x);
}

static void free_stream(struct stream *st)
{
	for (int b = 0; b < st->nb; b++) {
		free(st->bu[b].bits);
		if (st->ref_bits) {
			free(st->ref_bits[b]);
		}
	}
	free(st->ref_bits);
	free(st->bu);
	free(st->i);
	free(st->q);
}

/* ---------------------------------------------------------------------------------------- */
/* Evaluation                                                                               */
/* ---------------------------------------------------------------------------------------- */

static int trace_left;

struct stats {
	uint64_t bits, errs;       /* payload */
	uint64_t wbits, werrs;     /* preamble and sync/AA */
	uint64_t pkts, pkt_ok, aa_ok, miss;
	uint64_t syms, dups, samples;
	uint64_t ref_bits, ref_errs, ref_pkt_ok;
	double sum_t_err;
	uint64_t n_t;
	double pw_sum, pw_sum2;
	uint64_t pw_n;
};

/*
 * Run the demodulator over the stream (random push sizes) and compare with the burst data.
 * Symbols map to transmitted ones by their time stamp.
 */
static void evaluate(const struct stream *st, const struct params *p, struct stats *sx,
		     int sq_pwr, int hold, int always, size_t max_chunk, enum gfsk_modulation mod)
{
	struct gfsk_demod *d = malloc(sizeof(*d));
	size_t cap = GFSK_OUT_CAP(max_chunk) + 8, total = 0, cap_all = st->n / 2 + 64;
	struct gfsk_sym *buf = malloc(cap * sizeof(*buf));
	struct gfsk_sym *all = malloc(cap_all * sizeof(*all));
	size_t pos = 0;

	gfsk_demod_init(d, mod);
	gfsk_demod_set_squelch(d, (uint8_t)sq_pwr, (uint16_t)hold, always != 0);
	while (pos < st->n) {
		size_t c = 128 + rnd32() % (max_chunk - 127);
		size_t got;

		if (pos + c > st->n) {
			c = st->n - pos;
		}
		got = gfsk_demod_push(d, st->i + pos, st->q + pos, c, buf, cap);
		if (total + got <= cap_all) {
			memcpy(all + total, buf, got * sizeof(*buf));
			total += got;
		}
		pos += c;
	}
	{
		size_t got = gfsk_demod_flush(d, buf, cap);

		if (total + got <= cap_all) {
			memcpy(all + total, buf, got * sizeof(*buf));
			total += got;
		}
	}
	sx->syms += total;
	sx->samples += st->n;

	/* compare */
	{
		size_t di = 0;

		for (int b = 0; b < st->nb; b++) {
			const struct burst *bu = &st->bu[b];
			double ts4 = bu->ts16 / 4.0;
			double tc0 = (bu->t16 + 0.5 * bu->ts16 - st->ph0) / 4.0;
			int *got = malloc(bu->nsym * sizeof(int));
			double *dist = malloc(bu->nsym * sizeof(double));
			uint32_t nerr_p = 0, nerr_w = 0, nerr_aa = 0;

			for (uint32_t k = 0; k < bu->nsym; k++) {
				got[k] = -1;
				dist[k] = 1e9;
			}
			while (di < total && (double)all[di].t < tc0 - 6.0 * ts4) {
				di++;
			}
			for (size_t j = di; j < total && (double)all[j].t < tc0 + (bu->nsym + 6) * ts4; j++) {
				double kf = ((double)all[j].t - tc0) / ts4;
				long k = lround(kf);

				if (k < 0 || (uint32_t)k >= bu->nsym) {
					continue;
				}
				if (got[k] >= 0) {
					sx->dups++;
					if (fabs(kf - (double)k) >= dist[k]) {
						continue;
					}
				}
				got[k] = all[j].bit;
				dist[k] = fabs(kf - (double)k);
				if ((uint32_t)k >= bu->nwarm) {
					sx->pw_sum += all[j].pwr;
					sx->pw_sum2 += (double)all[j].pwr * all[j].pwr;
					sx->pw_n++;
				}
				sx->sum_t_err += kf - (double)k;
				sx->n_t++;
			}
			for (uint32_t k = 0; k < bu->nsym; k++) {
				int bad = got[k] != bu->bits[k];

				if (got[k] < 0) {
					sx->miss++;
				}
				if (k >= bu->nwarm) {
					nerr_p += bad;
				} else {
					nerr_w += bad;
					if (k >= bu->nwarm - bu->naa) {
						nerr_aa += bad;
					}
				}
			}
			if (getenv("TRACE") && nerr_p + nerr_w > 0 && trace_left > 0) {
				trace_left--;
				printf("burst %d: t4=%.1f first demod t=%u\n exp: ", b, tc0, di < total ? all[di].t : 0);
				for (uint32_t k = 0; k < bu->nsym && k < 140; k++) {
					putchar('0' + bu->bits[k]);
				}
				printf("\n got: ");
				for (uint32_t k = 0; k < bu->nsym && k < 140; k++) {
					putchar(got[k] < 0 ? '.' : '0' + got[k]);
				}
				printf("\n err: ");
				for (uint32_t k = 0; k < bu->nsym && k < 140; k++) {
					putchar(got[k] != bu->bits[k] ? 'X' : ' ');
				}
				printf("\n errpos:");
				for (uint32_t k = 0; k < bu->nsym; k++) {
					if (got[k] != bu->bits[k]) {
						printf(" %u", k);
					}
				}
				printf("\n");
			}
			if (st->ref_bits) {
				uint32_t e = 0, ew = 0;

				for (uint32_t k = 0; k < bu->nsym; k++) {
					if (st->ref_bits[b][k] != bu->bits[k]) {
						if (k >= bu->nwarm) {
							e++;
						} else {
							ew++;
						}
					}
				}
				sx->ref_bits += bu->nsym - bu->nwarm;
				sx->ref_errs += e;
				sx->ref_pkt_ok += (e + ew == 0);
			}
			sx->bits += bu->nsym - bu->nwarm;
			sx->errs += nerr_p;
			sx->wbits += bu->nwarm;
			sx->werrs += nerr_w;
			sx->pkts++;
			sx->pkt_ok += (nerr_p + nerr_w == 0);
			sx->aa_ok += p->ble ? (nerr_aa == 0) : (nerr_aa <= 2);
			free(got);
			free(dist);
		}
	}
	free(d);
	free(buf);
	free(all);
}

#ifdef GFSK_TUNABLE
static const struct {
	const char *name;
	int *ref;
} tune_names[] = {
	{"disc", &gfsk_tune.disc},           {"win", &gfsk_tune.win},
	{"dd_k", &gfsk_tune.dd_k},           {"nseed", &gfsk_tune.nseed},
	{"seed_skip", &gfsk_tune.seed_skip}, {"el_period", &gfsk_tune.el_period},
	{"el_shift", &gfsk_tune.el_shift},   {"el_stride", &gfsk_tune.el_stride},   {"sq_shift", &gfsk_tune.sq_shift},
	{"toff", &gfsk_tune.toff},           {"floor_up", &gfsk_tune.floor_up},
};

static struct gfsk_tune tune_default;

static void tune_set(const char *name, int v)
{
	for (size_t k = 0; k < sizeof(tune_names) / sizeof(tune_names[0]); k++) {
		if (strcmp(tune_names[k].name, name) == 0) {
			*tune_names[k].ref = v;
			return;
		}
	}
	fprintf(stderr, "unknown parameter %s\n", name);
	exit(2);
}

/* "a=1,b=2", on top of the defaults */
static void tune_apply(const char *spec)
{
	char *s = strdup(spec), *tok, *save;

	gfsk_tune = tune_default;
	for (tok = strtok_r(s, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		char *eq = strchr(tok, '=');

		if (eq) {
			*eq = 0;
			tune_set(tok, atoi(eq + 1));
		}
	}
	free(s);
}
#endif

/* ---------------------------------------------------------------------------------------- */
/* Main                                                                                     */
/* ---------------------------------------------------------------------------------------- */

static double q_inv_cross(const double *snr, const double *ber, int n, double target)
{
	for (int k = 0; k + 1 < n; k++) {
		if (ber[k] >= target && ber[k + 1] < target && ber[k + 1] > 0) {
			double l0 = log10(ber[k]), l1 = log10(ber[k + 1]), lt = log10(target);

			return snr[k] + (snr[k + 1] - snr[k]) * (lt - l0) / (l1 - l0);
		}
	}
	return NAN;
}

int main(void)
{
	struct params p;
	const char *mods = env_s("MOD", "ble");
	const char *mode = env_s("MODE", "ber");
	double smin = 8, smax = 20, sstep = 1;
	int nb = (int)env_f("NB", 100), sq = (int)env_f("SQ", 0), hold = (int)env_f("HOLD", 8);
	int always = (int)env_f("ALWAYS", sq == 0 ? 1 : 0);
	uint64_t minbits = (uint64_t)env_f("MINBITS", 120000), maxerr = (uint64_t)env_f("MAXERR", 400);
	size_t max_chunk = (size_t)env_f("CHUNK", 700);
	const char *snrs = env_s("SNRS", "8:20:1");
	const char *sweep = env_s("SWEEP", "");
	const char *variants = env_s("VARIANTS", "");
	char *vlist[256];
	int nvar = 0;
	double snr_pts[64];
	int nsnr = 0;
	enum gfsk_modulation mod;

	memset(&p, 0, sizeof(p));
#ifdef GFSK_TUNABLE
	tune_default = gfsk_tune;
#endif
	p.ble = strcmp(mods, "br") != 0;
	mod = p.ble ? GFSK_BLE : GFSK_BR;
	p.h = p.ble ? 0.5 : 0.32;
	p.bt = 0.5;
	p.cfo = env_f("CFO", 0);
	p.ppm = env_f("PPM", 0);
	p.dc = env_f("DC", 0);
	p.amp_dbfs = env_f("AMP", -24);
	p.aci_db = getenv("ACI") ? env_f("ACI", 0) : -300;
	p.aci_off = env_f("ACIOFF", p.ble ? 2 : 1);
	p.wifi_db = getenv("WIFI") ? env_f("WIFI", 0) : -300;
	p.wbw = env_f("WBW", 4);
	p.wfc = env_f("WFC", 5);
	p.gap_us = env_f("GAP", 40);
	p.quant = (int)env_f("QUANT", 10);
	p.payload = (int)env_f("PL", p.ble ? 320 : 300);
	p.ref = (int)env_f("REF", 0);
	p.sigoff = (int)env_f("SIGOFF", 0);
	trace_left = (int)env_f("TRACE", 0);
	rng_state ^= (uint64_t)env_f("SEED", 1) * 0x9e3779b97f4a7c15ULL;
	gauss_init(p.bt);

	if (getenv("SNR")) {
		smin = smax = env_f("SNR", 15);
	} else {
		sscanf(snrs, "%lf:%lf:%lf", &smin, &smax, &sstep);
	}
	for (double s = smin; s <= smax + 1e-9 && nsnr < 64; s += sstep) {
		snr_pts[nsnr++] = s;
	}

#ifdef GFSK_TUNABLE
	/* variants: explicit list first, else a one parameter sweep, else the defaults */
	if (variants[0]) {
		char *s = strdup(variants), *save, *tok;

		for (tok = strtok_r(s, ";", &save); tok && nvar < 256; tok = strtok_r(NULL, ";", &save)) {
			vlist[nvar++] = strdup(tok);
		}
		free(s);
	} else if (sweep[0]) {
		char *s = strdup(sweep), *colon = strchr(s, ':'), *save, *tok;

		if (colon) {
			*colon = 0;
			for (tok = strtok_r(colon + 1, ",", &save); tok && nvar < 256;
			     tok = strtok_r(NULL, ",", &save)) {
				char tmp[64];

				snprintf(tmp, sizeof(tmp), "%s=%s", s, tok);
				vlist[nvar++] = strdup(tmp);
			}
		}
		free(s);
	}
#endif
	if (nvar == 0) {
		vlist[nvar++] = strdup("");
	}

	if (strcmp(mode, "ber") == 0) {
		static struct stats st[256][64];
		static double ber[256][64];
		int first_run = 1;

		memset(st, 0, sizeof(st));
		for (int s = 0; s < nsnr; s++) {
			uint64_t seed_base = rng_state;
			int chunk = 0;
			int done = 0;

			p.snr_db = snr_pts[s];
			if (getenv("SQREL")) {
				/* threshold above the noise floor seen by the detector (5.1 dB folding loss) */
				sq = (int)lrint(200.0 + 2.0 * (5.1 + p.amp_dbfs - p.snr_db + env_f("SQREL", 8)));
			}
			while (!done) {
				struct stream stm;
				int all_done = 1;

				rng_state = seed_base ^ ((uint64_t)(chunk + 1) * 0xD1B54A32D192ED03ULL);
				make_stream(&stm, &p, nb);
				for (int v = 0; v < nvar; v++) {
#ifdef GFSK_TUNABLE
					tune_apply(vlist[v]);
#endif
					evaluate(&stm, &p, &st[v][s], sq, hold, always, max_chunk, mod);
					if (st[v][s].bits < minbits && st[v][s].errs < maxerr) {
						all_done = 0;
					}
				}
				free_stream(&stm);
				chunk++;
				done = all_done || chunk > 200;
			}
			first_run = 0;
		}
		(void)first_run;
		printf("# mod=%s cfo=%g ppm=%g dc=%g amp=%g dBFS aci=%g wifi=%g payload=%d quant=%d\n",
		       mods, p.cfo, p.ppm, p.dc, p.amp_dbfs, p.aci_db, p.wifi_db, p.payload, p.quant);
		if (getenv("SUMMARY")) {
			printf("# summary: BER at each SNR, then SNR at 1e-3 / 1e-2\n#");
			for (int s = 0; s < nsnr; s++) {
				printf("   %5.1f dB ", snr_pts[s]);
			}
			printf("\n");
			for (int v = 0; v < nvar; v++) {
				double lo3, lo2;

				for (int s = 0; s < nsnr; s++) {
					ber[v][s] = (double)st[v][s].errs / (double)st[v][s].bits;
					printf(" %.2e ", ber[v][s]);
				}
				lo3 = q_inv_cross(snr_pts, ber[v], nsnr, 1e-3);
				lo2 = q_inv_cross(snr_pts, ber[v], nsnr, 1e-2);
				{
					const struct stats *x = &st[v][nsnr - 1];

					printf(" | %5.2f %5.2f | per %.3f sync %.3f warm %.1e | %s\n", lo3, lo2,
					       1.0 - (double)x->pkt_ok / (double)x->pkts,
					       1.0 - (double)x->aa_ok / (double)x->pkts,
					       (double)x->werrs / (double)x->wbits, vlist[v]);
				}
			}
			return 0;
		}
		for (int v = 0; v < nvar; v++) {
			double pe[64];

			printf("# variant [%s]\n", vlist[v]);
			printf("#  snr    ber     per   syncfail  warmerr  missing\n");
			for (int s = 0; s < nsnr; s++) {
				const struct stats *x = &st[v][s];

				ber[v][s] = (double)x->errs / (double)x->bits;
				pe[s] = 1.0 - (double)x->pkt_ok / (double)x->pkts;
				printf("  %5.1f  %.2e  %.3f  %.3f  %.2e  %llu", snr_pts[s], ber[v][s], pe[s],
				       1.0 - (double)x->aa_ok / (double)x->pkts,
				       (double)x->werrs / (double)x->wbits, (unsigned long long)x->miss);
				if (x->ref_bits) {
					printf("   ref %.2e  %.3f", (double)x->ref_errs / (double)x->ref_bits,
					       1.0 - (double)x->ref_pkt_ok / (double)x->pkts);
				}
				printf("  emit/burst %.1f of %d  (%.1f sym/ms)", (double)x->syms / (double)x->pkts,
				       p.payload + (p.ble ? 40 : 72), (double)x->syms / ((double)x->samples / 4000.0));
				if (x->pw_n) {
					double m = x->pw_sum / (double)x->pw_n;

					printf("  pwr %.1f +-%.1f", m, sqrt(x->pw_sum2 / (double)x->pw_n - m * m));
				}
				if (x->n_t) {
					printf("  t-err %.2f smp", 4.0 * x->sum_t_err / (double)x->n_t);
				}
				printf("\n");
			}
			printf("# variant [%s] SNR at BER 1e-3: %.2f dB, 1e-2: %.2f dB\n", vlist[v],
			       q_inv_cross(snr_pts, ber[v], nsnr, 1e-3), q_inv_cross(snr_pts, ber[v], nsnr, 1e-2));
		}
	}
	return 0;
}
