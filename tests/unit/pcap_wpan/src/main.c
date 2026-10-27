/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit test of the 802.15.4 decoder of apps/pcap (apps/pcap/src/dec_wpan.c) and of the receiver
 * instances behind it (lib/ieee802154/ieee154_phy.c), end to end: O-QPSK frames modulated at
 * the C6's 80 MS/s, mixed to a channel of the M = 40 bank with noise and a carrier offset,
 * packed to int8 as the C6 does, through the fixed-point channelizer (apps/common/src/esdr_pfbq.c)
 * into burst buffers, decoded from one channel and from the combination of two neighbours; the
 * channel mode's 2 MS/s (the C6's boxcar over a chip); the combination's sign convention on
 * tones; frames with a bad FCS, both phase trackers completing one frame, noise only, two
 * receivers side by side. Runs on native_sim.
 */
#include <math.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <esp_sdr/esp_sdr_dsp.h>
#include <esp_sdr/ieee154_phy.h>
#include <test_rng.h>

#include "decode.h"
#include "esdr_pfbq.h"

/* dec_wpan.c's combination, not in decode.h: the tone tests check its convention directly. */
void dec_wpan_combine(const struct burst *lo, const struct burst *hi, uint64_t m, uint32_t n,
		      int16_t *out);

#define RNG_SEED 88172645463325252ULL

/* The C6's stream and the bank. */
#define FS_HZ     80000000.0
#define M         40U
#define D         (M / 2U)
#define TAPS      8U
#define L         (M * TAPS)
#define CHAN_RATE 4000000U
#define GAIN      32.0
/*
 * The band centre: channel 5 (+10 MHz) then sits at 2444 MHz, off the 802.15.4 grid, and the
 * band between channels 5 and 6 (+11 MHz) at 2445 MHz, 802.15.4 channel 19.
 */
#define CENTRE_HZ 2434000000ULL
#define CH_LO     5U
#define CH_HI     6U
#define CH_HZ(k)  ((double)(k) * FS_HZ / (double)M)

/* O-QPSK at 80 MS/s: 2 Mchip/s, 40 samples a chip, a half-sine pulse over two chips. */
#define SPC   40U
#define PULSE (2U * SPC)

/* The stream: a quiet lead, one frame, a quiet tail; the longest frame is the 127-byte PSDU. */
#define LEAD_PAIRS 4000U
#define TAIL_PAIRS 4000U
#define FRAME_PAIRS(len) (((6U + (uint32_t)(len)) * 64U + 1U) * SPC)
#define STREAM_MAX (LEAD_PAIRS + FRAME_PAIRS(IEEE154_PHY_PSDU_MAX) + TAIL_PAIRS)
/* Pairs a push carries. */
#define PUSH_PAIRS 4000U
/* The ADC's full scale, raw units. */
#define ADC_MAX 511

/*
 * Where the frame's first preamble sample lands in a channel's stream: pair LEAD_PAIRS delayed
 * by the prototype's group delay (L - 1) / 2 pairs, in frames of D pairs, rounded.
 */
#define EXPECT_START ((int32_t)((LEAD_PAIRS * 2U + L - 1U + D) / (2U * D)))

static int16_t raw[2U * STREAM_MAX];
static int8_t items[2U * STREAM_MAX];
static uint8_t qmem[40000] __aligned(16);
static struct esdr_pfbq pq;

/* A burst buffer of one channel, filled from the quads. */
struct tb {
	struct burst b;
	bool started;
	int16_t iq[2U * BURST_MAX_SAMPLES];
};

static struct tb tb_lo, tb_hi;

static uint8_t scratch[16384] __aligned(8);

/* The packets a decode emitted. */
#define PKT_MAX 8
static struct dec_wpan_pkt pkts[PKT_MAX];
static int npkts;

static void emit(const struct dec_wpan_pkt *p, void *user)
{
	ARG_UNUSED(user);
	if (npkts < PKT_MAX) {
		pkts[npkts] = *p;
	}
	npkts++;
}

static int decode(const struct burst *b, bool keep_bad)
{
	struct dec_wpan_cfg cfg = {.keep_bad = keep_bad};

	npkts = 0;
	return dec_wpan_run(b, &cfg, scratch, sizeof(scratch), emit, NULL);
}

static int decode_pair(const struct burst *lo, const struct burst *hi, bool keep_bad)
{
	struct dec_wpan_cfg cfg = {.keep_bad = keep_bad};

	npkts = 0;
	return dec_wpan_run_pair(lo, hi, &cfg, scratch, sizeof(scratch), emit, NULL);
}

/* A random PSDU of len bytes, FCS included. */
static void make_psdu(uint8_t *psdu, uint8_t len)
{
	for (int i = 0; i < len - 2; i++) {
		psdu[i] = (uint8_t)test_rng_u32();
	}
	(void)ieee154_phy_append_fcs(psdu, (size_t)len - 2U);
}

/* ---- the 80 MS/s modulator ---- */

/* Symbols of SHR, PHR and the PSDU as chip words (c0 in the MSB), low nibble first. */
static uint32_t sym[2U * (6U + IEEE154_PHY_PSDU_MAX)];

static uint32_t build_chips(const uint8_t *psdu, uint8_t len)
{
	uint32_t nbytes = 6U + len;

	for (uint32_t b = 0; b < nbytes; b++) {
		uint8_t v = b < 4U ? 0x00U : (b == 4U ? 0xA7U : (b == 5U ? len : psdu[b - 6U]));

		sym[2U * b] = ieee154_phy_chips(v & 0xFU);
		sym[2U * b + 1U] = ieee154_phy_chips(v >> 4);
	}
	return nbytes * 64U;
}

static inline double chip_sign(uint32_t k)
{
	return ((sym[k / 32U] >> (31U - k % 32U)) & 1U) ? 1.0 : -1.0;
}

/*
 * The baseband at sample s of the frame: chip k's half-sine pulse spans samples [k SPC, k SPC +
 * PULSE) on I for even k and Q for odd k, so Q runs a chip behind I (ieee154_phy_modulate() at 2
 * samples a chip, scaled up). At s the pulses of chips s / SPC and s / SPC - 1 overlap.
 */
static void oqpsk_sample(uint32_t nchips, uint32_t s, double *i, double *q)
{
	uint32_t k1 = s / SPC;

	*i = 0.0;
	*q = 0.0;
	for (uint32_t k = k1 == 0U ? 0U : k1 - 1U; k <= k1; k++) {
		double v;

		if (k >= nchips) {
			continue;
		}
		v = chip_sign(k) * sin(ESP_SDR_TAU / 2.0 * (double)(s - k * SPC) / (double)PULSE);
		if (k & 1U) {
			*q += v;
		} else {
			*i += v;
		}
	}
}

/*
 * Noise per component at 80 MS/s for a signal of amplitude amp (MSK: power amp^2) to stand
 * snr_db over the noise in a 4 MHz channel bandwidth (the PHY test's convention: SNR over the
 * 4 MS/s sample bandwidth).
 */
static double noise_sigma(double amp, double snr_db)
{
	return amp * sqrt(10.0 / pow(10.0, snr_db / 10.0));
}

/* raw[] to items[] with the shift the C6 would pick for its peak; returns the shift. */
static unsigned int pack_items(uint32_t pairs)
{
	int peak = 0;
	unsigned int shift = 0;

	for (uint32_t i = 0; i < 2U * pairs; i++) {
		int a = raw[i] < 0 ? -raw[i] : raw[i];

		peak = a > peak ? a : peak;
	}
	while ((peak >> shift) > 127) {
		shift++;
	}
	for (uint32_t i = 0; i < 2U * pairs; i++) {
		items[i] = (int8_t)(raw[i] >> shift);
	}
	return shift;
}

static inline int16_t adc(double v)
{
	long r = lrint(v);

	return (int16_t)(r > ADC_MAX ? ADC_MAX : (r < -ADC_MAX ? -ADC_MAX : r));
}

/*
 * The frame psdu (len bytes, FCS included) at amplitude amp after LEAD_PAIRS of quiet, mixed to
 * offset_hz from the band centre, AWGN of sigma a component (0: none), the ADC's clipping, int8
 * items with the C6's shift. Returns the pairs; *shift gets the shift.
 */
static uint32_t make_stream(const uint8_t *psdu, uint8_t len, double amp, double offset_hz,
			    double sigma, unsigned int *shift)
{
	uint32_t nchips = build_chips(psdu, len);
	uint32_t frame = (nchips + 1U) * SPC, total = LEAD_PAIRS + frame + TAIL_PAIRS;
	double phase0 = test_rng_uniform() * ESP_SDR_TAU;

	zassert_true(total <= STREAM_MAX);
	for (uint32_t s = 0; s < total; s++) {
		double i = 0.0, q = 0.0, th, c, sn, ri, rq;

		if (s >= LEAD_PAIRS && s < LEAD_PAIRS + frame) {
			oqpsk_sample(nchips, s - LEAD_PAIRS, &i, &q);
		}
		th = phase0 + ESP_SDR_TAU * offset_hz * (double)s / FS_HZ;
		c = cos(th);
		sn = sin(th);
		ri = amp * (i * c - q * sn);
		rq = amp * (i * sn + q * c);
		if (sigma > 0.0) {
			ri += sigma * test_rng_gauss();
			rq += sigma * test_rng_gauss();
		}
		raw[2U * s] = adc(ri);
		raw[2U * s + 1U] = adc(rq);
	}
	*shift = pack_items(total);
	return total;
}

/* A complex tone of amplitude amp at offset_hz for pairs pairs (shift 0). */
static void make_tone(double amp, double offset_hz, uint32_t pairs)
{
	zassert_true(pairs <= STREAM_MAX && amp <= 127.0);
	for (uint32_t s = 0; s < pairs; s++) {
		double th = ESP_SDR_TAU * offset_hz * (double)s / FS_HZ;

		items[2U * s] = (int8_t)lrint(amp * cos(th));
		items[2U * s + 1U] = (int8_t)lrint(amp * sin(th));
	}
}

/* ---- the channelizer into burst buffers ---- */

static void tb_reset(struct tb *t, unsigned int k)
{
	double hz = CH_HZ(k > M / 2U ? (int)k - (int)M : (int)k);

	memset(&t->b, 0, sizeof(t->b));
	t->b.chan = (uint16_t)k;
	t->b.channels = M;
	t->b.rate_hz = CHAN_RATE;
	t->b.freq_hz = (uint64_t)((double)CENTRE_HZ + hz);
	t->b.iq = t->iq;
	t->started = false;
}

static void collect(struct tb *t, uint64_t m0, const int16_t *iq, uint8_t valid)
{
	for (unsigned int q = 0; q < 4U; q++) {
		uint64_t m = m0 + q;

		if ((valid & (1U << q)) == 0U) {
			continue;
		}
		if (!t->started) {
			t->b.m0 = m;
			t->started = true;
		}
		zassert_equal(m, t->b.m0 + t->b.n, "channel %u: frame %llu, %u after %llu",
			      t->b.chan, (unsigned long long)m, t->b.n,
			      (unsigned long long)t->b.m0);
		zassert_true(t->b.n < BURST_MAX_SAMPLES, "burst buffer full");
		t->iq[2U * t->b.n] = iq[8U * t->b.chan + 2U * q];
		t->iq[2U * t->b.n + 1U] = iq[8U * t->b.chan + 2U * q + 1U];
		t->b.n++;
	}
}

static void quad_cb(uint64_t m0, const int16_t *iq, uint8_t valid, uint8_t complete, void *user)
{
	ARG_UNUSED(complete);
	ARG_UNUSED(user);
	collect(&tb_lo, m0, iq, valid);
	collect(&tb_hi, m0, iq, valid);
}

/* items[] (pairs of them, shift) from absolute index 0 through the bank into tb_lo and tb_hi. */
static void channelize(uint32_t pairs, unsigned int shift)
{
	struct esdr_pfbq_cfg cfg = {.channels = M, .taps = TAPS, .quads = 1, .gain = (float)GAIN};

	zassert_true(esdr_pfbq_mem_size(&cfg) <= sizeof(qmem), "qmem %u",
		     (unsigned int)esdr_pfbq_mem_size(&cfg));
	zassert_ok(esdr_pfbq_init(&pq, &cfg, qmem, sizeof(qmem)));
	tb_reset(&tb_lo, CH_LO);
	tb_reset(&tb_hi, CH_HI);
	for (uint32_t off = 0; off < pairs; off += PUSH_PAIRS) {
		uint32_t n = pairs - off < PUSH_PAIRS ? pairs - off : PUSH_PAIRS;
		int ret = esdr_pfbq_push(&pq, off, &items[2U * off], n, shift, 0, 0, quad_cb, NULL);

		zassert_true(ret >= 0, "push %d", ret);
	}
	zassert_true(esdr_pfbq_flush(&pq, quad_cb, NULL) >= 0);
	zassert_true(tb_lo.b.n > LEAD_PAIRS / D && tb_hi.b.n == tb_lo.b.n, "frames %u and %u",
		     tb_lo.b.n, tb_hi.b.n);
}

/* The one packet of a decode against the frame sent; prints its metadata. */
static void check_packet(const char *what, int ret, const uint8_t *psdu, uint8_t len, bool combined,
			 double cfo_hz, double amp)
{
	const struct dec_wpan_pkt *p = &pkts[0];
	double want_pw = (double)GAIN * amp * (double)GAIN * amp;

	zassert_equal(ret, 1, "%s: %d frames (want 1)", what, ret);
	zassert_equal(npkts, 1);
	zassert_true(p->fcs_ok, "%s: FCS failed", what);
	zassert_equal(p->len, len, "%s: length %u", what, p->len);
	zassert_mem_equal(p->psdu, psdu, len, "%s: PSDU differs", what);
	zassert_equal(p->combined, combined);
	printk("%s: start %d (want %d), offset %d Hz (want %.0f), power %.2f of (gain A)^2, "
	       "lqi %u, channel %u\n", what, p->start, EXPECT_START, p->offset_hz, cfo_hz,
	       (double)p->power / want_pw, p->lqi, p->channel);
	zassert_within(p->start, EXPECT_START, 6, "%s: start %d, want %d", what, p->start,
		       EXPECT_START);
	zassert_within((double)p->offset_hz, cfo_hz, 20e3, "%s: offset %d Hz, want %.0f", what,
		       p->offset_hz, cfo_hz);
	zassert_true((double)p->power > 0.25 * want_pw && (double)p->power < 1.5 * want_pw,
		     "%s: power %.0f, (gain A)^2 %.0f", what, (double)p->power, want_pw);
}

static void *suite_setup(void)
{
	ieee154_phy_init();
	return NULL;
}

static void before_each(void *fixture)
{
	ARG_UNUSED(fixture);
	test_rng_seed(RNG_SEED);
	npkts = 0;
}

ZTEST_SUITE(pcap_wpan, NULL, suite_setup, before_each, NULL, NULL);

/* The 2.4 GHz grid: 2405 + 5 (c - 11) MHz within +-100 kHz, c = 11 .. 26. */
ZTEST(pcap_wpan, test_channel_grid)
{
	zassert_equal(dec_wpan_channel(2405000000ULL), 11);
	zassert_equal(dec_wpan_channel(2480000000ULL), 26);
	zassert_equal(dec_wpan_channel(2445000000ULL), 19);
	zassert_equal(dec_wpan_channel(2445100000ULL), 19);
	zassert_equal(dec_wpan_channel(2444900000ULL), 19);
	zassert_equal(dec_wpan_channel(2445100001ULL), 0);
	zassert_equal(dec_wpan_channel(2444899999ULL), 0);
	zassert_equal(dec_wpan_channel(2444000000ULL), 0);
	zassert_equal(dec_wpan_channel(2404900000ULL), 11);
	zassert_equal(dec_wpan_channel(2404800000ULL), 0);
	zassert_equal(dec_wpan_channel(2400000000ULL), 0);
	zassert_equal(dec_wpan_channel(2485000000ULL), 0);
	zassert_equal(dec_wpan_channel(2480100000ULL), 26);
	zassert_equal(dec_wpan_channel(0), 0);
	zassert_equal(dec_wpan_channel((uint64_t)CENTRE_HZ + 11000000ULL), 19);
}

/*
 * A frame at channel 5's centre decodes from channel 5 alone: 20 and 120 byte PSDUs, 20 and
 * 8 dB SNR (the amplitude as the C6's gain servo would leave it: noise peaks within the ADC),
 * carrier offsets 0 and +-50 kHz.
 */
ZTEST(pcap_wpan, test_single_channel)
{
	static const uint8_t lens[] = {20, 120};
	static const double snrs[] = {20.0, 8.0}, amps[] = {200.0, 100.0};
	static const double cfos[] = {0.0, 50e3, -50e3};
	uint8_t psdu[IEEE154_PHY_PSDU_MAX];
	char what[64];

	ARRAY_FOR_EACH(lens, li) {
		ARRAY_FOR_EACH(snrs, si) {
			ARRAY_FOR_EACH(cfos, ci) {
				unsigned int shift;
				uint32_t pairs;
				int ret;

				make_psdu(psdu, lens[li]);
				pairs = make_stream(psdu, lens[li], amps[si],
						    CH_HZ(CH_LO) + cfos[ci],
						    noise_sigma(amps[si], snrs[si]), &shift);
				channelize(pairs, shift);
				ret = decode(&tb_lo.b, false);
				snprintf(what, sizeof(what), "len %u snr %.0f cfo %.0f (shift %u)",
					 lens[li], snrs[si], cfos[ci], shift);
				check_packet(what, ret, psdu, lens[li], false, cfos[ci], amps[si]);
				zassert_equal(pkts[0].channel, dec_wpan_channel(tb_lo.b.freq_hz));
				zassert_equal(pkts[0].channel, 0, "2444 MHz is off the grid");
			}
		}
	}
}

/* A view of a burst without its first skip frames. */
static struct burst sliced(const struct burst *b, uint32_t skip, uint32_t drop)
{
	struct burst v = *b;

	v.m0 += skip;
	v.iq += 2U * skip;
	v.n -= skip + drop;
	return v;
}

/*
 * A frame between channels 5 and 6 decodes from their combination, with the middle frequency's
 * channel; the single channels are tried too and their result printed.
 */
ZTEST(pcap_wpan, test_pair)
{
	static const double snrs[] = {20.0, 8.0}, amps[] = {200.0, 100.0};
	static const double cfos[] = {0.0, 50e3, -50e3};
	const uint8_t len = 60;
	uint8_t psdu[IEEE154_PHY_PSDU_MAX];
	char what[64];
	int alone_lo = 0, alone_hi = 0, runs = 0;

	ARRAY_FOR_EACH(snrs, si) {
		ARRAY_FOR_EACH(cfos, ci) {
			unsigned int shift;
			uint32_t pairs;
			int ret;

			make_psdu(psdu, len);
			pairs = make_stream(psdu, len, amps[si], CH_HZ(CH_LO) + 1e6 + cfos[ci],
					    noise_sigma(amps[si], snrs[si]), &shift);
			channelize(pairs, shift);
			ret = decode_pair(&tb_lo.b, &tb_hi.b, false);
			snprintf(what, sizeof(what), "pair snr %.0f cfo %.0f (shift %u)", snrs[si],
				 cfos[ci], shift);
			check_packet(what, ret, psdu, len, true, cfos[ci], amps[si]);
			zassert_equal(pkts[0].channel, 19, "2445 MHz is channel 19 (got %u)",
				      pkts[0].channel);
			alone_lo += decode(&tb_lo.b, false) == 1;
			alone_hi += decode(&tb_hi.b, false) == 1;
			runs++;
		}
	}
	printk("the single channels alone decoded the frame between them: lo %d, hi %d of %d\n",
	       alone_lo, alone_hi, runs);

	/* The last point again from bursts that start and end apart: the overlap, start from lo. */
	{
		struct burst lo = sliced(&tb_lo.b, 0, 50), hi = sliced(&tb_hi.b, 37, 0);
		int ret = decode_pair(&lo, &hi, false);

		zassert_equal(ret, 1, "sliced pair: %d", ret);
		zassert_true(pkts[0].fcs_ok && pkts[0].combined);
		zassert_within(pkts[0].start, EXPECT_START, 6, "sliced pair: start %d",
			       pkts[0].start);
		/* The frames of a wrap pair (the last channel and channel 0) are neighbours too. */
		lo.chan = M - 1U;
		hi.chan = 0;
		zassert_equal(decode_pair(&lo, &hi, false), 1);
		/* Not neighbours, the wrong order, another bank. */
		zassert_equal(decode_pair(&hi, &lo, false), -EINVAL);
		lo.chan = 4;
		zassert_equal(decode_pair(&lo, &hi, false), -EINVAL);
		lo.chan = 5;
		hi.chan = 6;
		hi.channels = 80;
		zassert_equal(decode_pair(&lo, &hi, false), -EINVAL);
		hi.channels = M;
		hi.rate_hz = 2000000;
		zassert_equal(decode_pair(&lo, &hi, false), -ENOTSUP);
		hi.rate_hz = CHAN_RATE;
		zassert_equal(dec_wpan_run_pair(&lo, &hi, NULL, scratch, sizeof(scratch), emit,
						NULL), -EINVAL);
		/* No overlap: nothing. */
		hi = sliced(&tb_hi.b, tb_hi.b.n - 10U, 0);
		zassert_equal(decode_pair(&lo, &hi, false), 0);
	}
}

/*
 * Mean phase step a frame (the angle of the summed products z[i + 1] conj(z[i]), so that steps
 * near pi do not average out) and mean magnitude of n complex int16 samples.
 */
static void tone_stats(const int16_t *z, uint32_t n, double *step, double *mag)
{
	double sr = 0.0, si = 0.0, a = 0.0;

	for (uint32_t i = 0; i + 1U < n; i++) {
		double xr = z[2U * i], xi = z[2U * i + 1U];
		double yr = z[2U * i + 2U], yi = z[2U * i + 3U];

		sr += yr * xr + yi * xi;
		si += yi * xr - yr * xi;
	}
	for (uint32_t i = 0; i < n; i++) {
		a += hypot(z[2U * i], z[2U * i + 1U]);
	}
	*step = atan2(si, sr);
	*mag = a / (double)n;
}

/* An alternative convention, in double: lo exp(+-j pi m / 2) + hi exp(-+j pi m / 2), or swapped. */
static void combine_alt(const struct burst *lo, const struct burst *hi, uint64_t m, uint32_t n,
			bool conj, bool swap, int16_t *out)
{
	for (uint32_t i = 0; i < n; i++) {
		uint64_t mm = m + i;
		double ph = (conj ? 1.0 : -1.0) * (ESP_SDR_TAU / 4.0) * (double)(mm & 3U);
		const int16_t *a = lo->iq + 2U * (uint32_t)(mm - lo->m0);
		const int16_t *b = hi->iq + 2U * (uint32_t)(mm - hi->m0);
		double zr, zi;

		if (swap) {
			const int16_t *t = a;

			a = b;
			b = t;
		}
		zr = a[0] * cos(ph) - a[1] * sin(ph) + b[0] * cos(-ph) - b[1] * sin(-ph);
		zi = a[0] * sin(ph) + a[1] * cos(ph) + b[0] * sin(-ph) + b[1] * cos(-ph);
		out[2U * i] = (int16_t)lrint(zr);
		out[2U * i + 1U] = (int16_t)lrint(zi);
	}
}

/*
 * The combination's sign convention: a tone between channels 5 and 6 (+11 MHz) comes out at DC
 * with the two halves adding up (gain times the amplitude: each channel's edge is -6 dB), a tone
 * at +11.5 MHz at +0.5 MHz. The alternatives (conjugate factors, swapped order) are printed.
 */
ZTEST(pcap_wpan, test_combine_convention)
{
	static int16_t z[2U * 1200U];
	const double amp = 100.0;
	const uint32_t frames = 1000U, pairs = 40000U;

	ARRAY_FOR_EACH(((const double[]){0.0, 0.5e6}), fi) {
		const double off = fi == 0 ? 0.0 : 0.5e6;
		const double want_step = ESP_SDR_TAU * off / (double)CHAN_RATE;
		uint64_t m;
		double step, mag;

		make_tone(amp, CH_HZ(CH_LO) + 1e6 + off, pairs);
		channelize(pairs, 0);
		/* Past the bank's transient and the ADC's startup, well inside both bursts. */
		m = tb_lo.b.m0 + 100U;
		zassert_true(tb_lo.b.n > 100U + frames && tb_hi.b.m0 <= m &&
			     tb_hi.b.m0 + tb_hi.b.n >= m + frames);
		dec_wpan_combine(&tb_lo.b, &tb_hi.b, m, frames, z);
		tone_stats(z, frames, &step, &mag);
		printk("tone at %+.1f MHz combined (doc/pcap.md): step %.4f rad a frame "
		       "(want %.4f), magnitude %.0f (gain A %.0f)\n",
		       (CH_HZ(CH_LO) + 1e6 + off) / 1e6, step, want_step, mag, GAIN * amp);
		zassert_within(step, want_step, 0.05, "step %.4f, want %.4f", step, want_step);
		if (fi == 0) {
			zassert_within(mag, (double)GAIN * amp, 0.15 * GAIN * amp, "magnitude %.0f",
				       mag);
		} else {
			zassert_true(mag > 0.5 * GAIN * amp, "magnitude %.0f", mag);
		}
		for (int alt = 1; alt < 4; alt++) {
			combine_alt(&tb_lo.b, &tb_hi.b, m, frames, (alt & 1) != 0,
				    (alt & 2) != 0, z);
			tone_stats(z, frames, &step, &mag);
			printk("  alternative%s%s: step %.4f, magnitude %.0f\n",
			       (alt & 1) ? " conjugate factors" : "",
			       (alt & 2) ? " swapped order" : "", step, mag);
		}
		/* The same values from the test's own double arithmetic of the convention. */
		combine_alt(&tb_lo.b, &tb_hi.b, m, frames, false, false, z);
		tone_stats(z, frames, &step, &mag);
		zassert_within(step, want_step, 0.05);
	}
}

/* ---- bursts straight from the reference modulator at 4 MS/s ---- */

#define DIRECT_LEAD 1000U
#define DIRECT_TAIL 500U
#define DIRECT_AMP  3000
#define DIRECT_MAX  (IEEE154_PHY_MOD_SAMPLES(IEEE154_PHY_PSDU_MAX) + DIRECT_LEAD + DIRECT_TAIL)
static int16_t dbuf[2U * DIRECT_MAX];

/* psdu as a 4 MS/s burst (lead of zeros, the frame, a tail), rotated by cfo_hz. */
static struct burst direct_burst(const uint8_t *psdu, uint8_t len, double cfo_hz)
{
	struct burst b;
	size_t ns = ieee154_phy_modulate(psdu, len, DIRECT_AMP, dbuf + 2U * DIRECT_LEAD,
					 IEEE154_PHY_MOD_SAMPLES(IEEE154_PHY_PSDU_MAX));

	zassert_not_equal(ns, 0, "modulator buffer too small");
	memset(dbuf, 0, 2U * DIRECT_LEAD * sizeof(int16_t));
	memset(dbuf + 2U * (DIRECT_LEAD + ns), 0, 2U * DIRECT_TAIL * sizeof(int16_t));
	memset(&b, 0, sizeof(b));
	b.chan = 5;
	b.channels = M;
	b.rate_hz = CHAN_RATE;
	b.freq_hz = 2450000000ULL;
	b.m0 = 123456789ULL;
	b.n = DIRECT_LEAD + (uint32_t)ns + DIRECT_TAIL;
	b.iq = dbuf;
	if (cfo_hz != 0.0) {
		for (uint32_t k = 0; k < b.n; k++) {
			double th = ESP_SDR_TAU * cfo_hz * (double)k / (double)CHAN_RATE;
			double i = dbuf[2U * k], q = dbuf[2U * k + 1U];

			dbuf[2U * k] = (int16_t)lrint(i * cos(th) - q * sin(th));
			dbuf[2U * k + 1U] = (int16_t)lrint(i * sin(th) + q * cos(th));
		}
	}
	return b;
}

/*
 * The channel mode: the C6's paced receive sums 40 samples of its 80 MS/s, one chip, into each
 * sample at 2 MS/s, at whatever offset to the chips; the decoder reads that at 4 MS/s. A frame at
 * 8 dB over the 2 MHz band with a 40 kHz carrier offset decodes at three offsets, its start in
 * the burst's own samples.
 */
ZTEST(pcap_wpan, test_half_rate)
{
	static const uint32_t taus[] = {0U, 13U, 27U};
	const double amp = 3000.0, cfo_hz = 40e3, snr_db = 8.0;
	const double sigma = amp * sqrt(0.5 / pow(10.0, snr_db / 10.0));
	const uint8_t len = 40U;
	const uint32_t lead = 200U;
	uint8_t psdu[IEEE154_PHY_PSDU_MAX];
	uint32_t nchips, n;
	struct burst b;

	make_psdu(psdu, len);
	nchips = build_chips(psdu, len);
	n = lead + (nchips * SPC + PULSE) / SPC + 200U;
	zassert_true(n <= ARRAY_SIZE(dbuf) / 2U);
	ARRAY_FOR_EACH(taus, ti) {
		for (uint32_t m = 0; m < n; m++) {
			double si = 0.0, sq = 0.0;

			for (uint32_t j = 0; j < SPC; j++) {
				int64_t s = (int64_t)m * SPC + j + taus[ti] - (int64_t)lead * SPC;
				double th = ESP_SDR_TAU * cfo_hz * (double)(m * SPC + j) / FS_HZ;
				double i = 0.0, q = 0.0;

				if (s >= 0) {
					oqpsk_sample(nchips, (uint32_t)s, &i, &q);
				}
				si += i * cos(th) - q * sin(th);
				sq += i * sin(th) + q * cos(th);
			}
			dbuf[2U * m] = (int16_t)lrint(amp * si / SPC + sigma * test_rng_gauss());
			dbuf[2U * m + 1U] = (int16_t)lrint(amp * sq / SPC + sigma * test_rng_gauss());
		}
		memset(&b, 0, sizeof(b));
		b.channels = 1U;
		b.rate_hz = 2000000U;
		b.freq_hz = 2425000000ULL;
		b.n = n;
		b.iq = dbuf;
		zassert_equal(decode(&b, false), 1, "offset %u: %d frames", taus[ti], npkts);
		zassert_true(pkts[0].fcs_ok);
		zassert_equal(pkts[0].len, len);
		zassert_mem_equal(pkts[0].psdu, psdu, len);
		zassert_equal(pkts[0].channel, 15);
		printk("2 MS/s, offset %u: start %d (want %u), offset %d Hz, lqi %u\n", taus[ti],
		       pkts[0].start, lead, pkts[0].offset_hz, pkts[0].lqi);
		zassert_within(pkts[0].start, (int32_t)lead, 3, "start %d", pkts[0].start);
		zassert_within((double)pkts[0].offset_hz, cfo_hz, 3e3, "offset %d Hz",
			       pkts[0].offset_hz);
	}
}

/* A receiver's own report of its good frames: count, the last one's LQI and start. */
struct rxl {
	struct ieee154_phy_rx *rx;
	int n, lqi;
	uint32_t start;
};

static void rxl_cb(const uint8_t *p, uint8_t len, int lqi, void *user)
{
	struct rxl *l = user;

	ARG_UNUSED(p);
	ARG_UNUSED(len);
	l->n++;
	l->lqi = lqi;
	l->start = ieee154_phy_rx_frame_start_i(l->rx);
}

/*
 * A frame with one PSDU bit flipped (its FCS then fails) is emitted only with keep_bad, flagged;
 * the intact frame is emitted once either way. The start accessor against the frame's first
 * sample, the power against the modulator's amplitude, the offset against a rotation.
 */
ZTEST(pcap_wpan, test_bad_fcs)
{
	uint8_t psdu[IEEE154_PHY_PSDU_MAX], bad[IEEE154_PHY_PSDU_MAX];
	const uint8_t len = 40;
	struct burst b;

	make_psdu(psdu, len);
	b = direct_burst(psdu, len, 0.0);
	zassert_equal(decode(&b, false), 1);
	zassert_true(pkts[0].fcs_ok);
	zassert_mem_equal(pkts[0].psdu, psdu, len);
	zassert_equal(pkts[0].channel, 20);
	zassert_false(pkts[0].combined);
	printk("direct frame: start %d (first sample %u), offset %d Hz, power %.3f A^2, lqi %u\n",
	       pkts[0].start, DIRECT_LEAD, pkts[0].offset_hz,
	       (double)pkts[0].power / ((double)DIRECT_AMP * DIRECT_AMP), pkts[0].lqi);
	zassert_within(pkts[0].start, (int32_t)DIRECT_LEAD, 4, "start %d", pkts[0].start);
	zassert_within((double)pkts[0].offset_hz, 0.0, 2e3, "offset %d", pkts[0].offset_hz);
	zassert_within((double)pkts[0].power, (double)DIRECT_AMP * DIRECT_AMP,
		       0.1 * DIRECT_AMP * DIRECT_AMP, "power %.0f", (double)pkts[0].power);
	/* The LQI and the start are the PHY's, as a receiver of its own reports them. */
	{
		static struct ieee154_phy_rx rx;
		struct rxl l = {.rx = &rx};

		ieee154_phy_rx_init(&rx);
		ieee154_phy_set_rx_cb_i(&rx, rxl_cb, &l);
		ieee154_phy_rx_block_i(&rx, b.iq, b.n);
		zassert_equal(l.n, 1);
		zassert_equal(pkts[0].lqi, l.lqi, "lqi %u, the receiver's %d", pkts[0].lqi, l.lqi);
		zassert_equal(pkts[0].start, (int32_t)l.start);
	}
	zassert_equal(decode(&b, true), 1, "the intact frame once with keep_bad");
	zassert_true(pkts[0].fcs_ok);

	b = direct_burst(psdu, len, 50e3);
	zassert_equal(decode(&b, false), 1);
	printk("direct frame rotated by 50 kHz: offset %d Hz\n", pkts[0].offset_hz);
	zassert_within((double)pkts[0].offset_hz, 50e3, 5e3, "offset %d", pkts[0].offset_hz);

	memcpy(bad, psdu, len);
	bad[3] ^= 0x10U;
	b = direct_burst(bad, len, 0.0);
	zassert_equal(decode(&b, false), 0, "a bad frame without keep_bad");
	zassert_equal(decode(&b, true), 1, "a bad frame with keep_bad");
	zassert_false(pkts[0].fcs_ok);
	zassert_equal(pkts[0].len, len);
	zassert_mem_equal(pkts[0].psdu, bad, len);
	zassert_within(pkts[0].start, (int32_t)DIRECT_LEAD, 4);
}

/* ---- both phase trackers on one frame ---- */

/*
 * psdu at 4 MS/s from the 80 MS/s modulator, sampled a quarter chip off the pulses' peaks, so
 * that both sampling phases see the chips alike. Returns the samples written.
 */
static uint32_t quarter_wave(const uint8_t *psdu, uint8_t len, double amp, int16_t *out)
{
	uint32_t nchips = build_chips(psdu, len);
	uint32_t ns = 2U * (nchips + 1U);

	for (uint32_t n = 0; n < ns; n++) {
		double i, q;

		oqpsk_sample(nchips, (SPC / 2U) * n + SPC / 4U, &i, &q);
		out[2U * n] = (int16_t)lrint(amp * i);
		out[2U * n + 1U] = (int16_t)lrint(amp * q);
	}
	return ns;
}

/* What a receiver delivered: the all-frames callback's verdicts and end positions in order. */
struct seq {
	struct ieee154_phy_rx *rx;
	int n, good;
	bool ok[4];
	uint32_t end[4];
};

static void seq_any(const uint8_t *p, uint8_t len, int lqi, bool fcs_ok, void *user)
{
	struct seq *q = user;

	ARG_UNUSED(p);
	ARG_UNUSED(len);
	ARG_UNUSED(lqi);
	if (q->n < 4) {
		q->ok[q->n] = fcs_ok;
		q->end[q->n] = ieee154_phy_rx_position_i(q->rx);
	}
	q->n++;
}

static void seq_good(const uint8_t *p, uint8_t len, int lqi, void *user)
{
	struct seq *q = user;

	ARG_UNUSED(p);
	ARG_UNUSED(len);
	ARG_UNUSED(lqi);
	q->good++;
}

/*
 * One frame, each sampling phase seeing its own copy: a sample's discriminator looks two
 * samples back, so the even samples go to one phase tracker and the odd ones to the other. With
 * a byte changed in one copy (its FCS fails), one tracker completes the frame good and the other
 * bad, a sample apart, the order set by which phase carries the bad copy. The receiver delivers
 * the good one alone (it stops the other tracker) or the bad one and then the good one, never a
 * second bad one; both copies bad, it delivers one. The decoder emits the frame once either way.
 */
ZTEST(pcap_wpan, test_phase_copies)
{
	static int16_t wa[2U * 6000U], wb[2U * 6000U];
	static struct ieee154_phy_rx rx;
	uint8_t good[IEEE154_PHY_PSDU_MAX], bad[IEEE154_PHY_PSDU_MAX];
	const uint8_t len = 30;
	int bad_first = 0;
	uint32_t ns;
	struct burst b;

	make_psdu(good, len);
	memcpy(bad, good, len);
	bad[7] ^= 0x04U;
	ns = quarter_wave(good, len, DIRECT_AMP, wa);
	zassert_true(ns <= 6000U && quarter_wave(bad, len, DIRECT_AMP, wb) == ns);
	memset(&b, 0, sizeof(b));
	b.chan = 5;
	b.channels = M;
	b.rate_hz = CHAN_RATE;
	b.freq_hz = 2450000000ULL;
	b.n = DIRECT_LEAD + ns + DIRECT_TAIL;
	b.iq = dbuf;

	/* 0: the bad copy on the odd samples, 1: on the even ones, 2: on both. */
	for (unsigned int v = 0; v < 3U; v++) {
		struct seq q = {.rx = &rx};

		memset(dbuf, 0, 2U * b.n * sizeof(int16_t));
		for (uint32_t k = 0; k < ns; k++) {
			uint32_t i = DIRECT_LEAD + k;
			bool use_bad = v == 2U || (i & 1U) == (v == 0U ? 1U : 0U);
			const int16_t *src = use_bad ? wb : wa;

			dbuf[2U * i] = src[2U * k];
			dbuf[2U * i + 1U] = src[2U * k + 1U];
		}
		ieee154_phy_rx_init(&rx);
		ieee154_phy_set_rx_cb_i(&rx, seq_good, &q);
		ieee154_phy_set_all_cb_i(&rx, seq_any, &q);
		ieee154_phy_rx_block_i(&rx, dbuf, b.n);
		printk("phase copies %u: %d deliveries, %s at %u, then %s at %u; %d good\n", v, q.n,
		       q.n > 0 ? (q.ok[0] ? "good" : "bad") : "none", q.end[0],
		       q.n > 1 ? (q.ok[1] ? "good" : "bad") : "none", q.n > 1 ? q.end[1] : 0U,
		       q.good);
		if (v == 2U) {
			zassert_equal(q.n, 1, "both copies bad: %d deliveries", q.n);
			zassert_false(q.ok[0]);
			zassert_equal(q.good, 0);
			zassert_equal(decode(&b, true), 1);
			zassert_false(pkts[0].fcs_ok);
			zassert_mem_equal(pkts[0].psdu, bad, len);
			zassert_equal(decode(&b, false), 0);
			continue;
		}
		zassert_equal(q.good, 1, "copies %u: %d good frames", v, q.good);
		if (q.n == 2) {
			zassert_true(!q.ok[0] && q.ok[1], "copies %u: bad after good", v);
			zassert_true(q.end[1] - q.end[0] <= IEEE154_PHY_DUP_SAMPLES,
				     "copies %u: ends %u and %u", v, q.end[0], q.end[1]);
			bad_first++;
		} else {
			zassert_equal(q.n, 1, "copies %u: %d deliveries", v, q.n);
			zassert_true(q.ok[0]);
		}
		zassert_equal(decode(&b, true), 1, "copies %u with keep_bad: %d packets", v, npkts);
		zassert_true(pkts[0].fcs_ok);
		zassert_mem_equal(pkts[0].psdu, good, len);
		zassert_within(pkts[0].start, (int32_t)DIRECT_LEAD, 4, "start %d", pkts[0].start);
		zassert_equal(decode(&b, false), 1);
		zassert_true(pkts[0].fcs_ok);
	}
	zassert_equal(bad_first, 1, "the bad copy came first in %d of the two orders", bad_first);
}

/* Noise only emits nothing, with keep_bad too, alone and through the bank; the argument checks. */
ZTEST(pcap_wpan, test_noise_only)
{
	struct burst b;
	const uint32_t n = ARRAY_SIZE(dbuf) / 2U, pairs = 200000U;

	/* Through the bank: noise filling the ADC as the C6's gain servo would leave it. */
	for (uint32_t s = 0; s < pairs; s++) {
		raw[2U * s] = adc(60.0 * test_rng_gauss());
		raw[2U * s + 1U] = adc(60.0 * test_rng_gauss());
	}
	channelize(pairs, pack_items(pairs));
	zassert_equal(decode(&tb_lo.b, true), 0);
	zassert_equal(decode(&tb_hi.b, true), 0);
	zassert_equal(decode_pair(&tb_lo.b, &tb_hi.b, true), 0);

	for (uint32_t k = 0; k < 2U * n; k++) {
		dbuf[k] = (int16_t)lrint(2000.0 * test_rng_gauss());
	}
	memset(&b, 0, sizeof(b));
	b.chan = 5;
	b.channels = M;
	b.rate_hz = CHAN_RATE;
	b.freq_hz = 2450000000ULL;
	b.n = n;
	b.iq = dbuf;
	zassert_equal(decode(&b, false), 0);
	zassert_equal(decode(&b, true), 0);
	b.rate_hz = 2000000U;
	zassert_equal(decode(&b, true), 0);
	b.rate_hz = 1000000U;
	zassert_equal(decode(&b, true), -ENOTSUP);
	b.rate_hz = CHAN_RATE;
	zassert_equal(dec_wpan_run(&b, &(struct dec_wpan_cfg){0}, scratch, 100, emit, NULL),
		      -ENOMEM);
	zassert_true(dec_wpan_scratch(n) <= sizeof(scratch) && dec_wpan_scratch(0) > 0U);
	b.n = 0;
	zassert_equal(decode(&b, true), 0);
	b.n = n;
	b.iq = NULL;
	zassert_equal(decode(&b, true), 0);
}

/* ---- the receiver instances themselves ---- */

struct rxc {
	uint8_t psdu[IEEE154_PHY_PSDU_MAX];
	uint8_t len;
	int got, wrong, bad;
	uint32_t start;
	struct ieee154_phy_rx *rx;
};

static void rx_good(const uint8_t *p, uint8_t len, int lqi, void *user)
{
	struct rxc *c = user;

	ARG_UNUSED(lqi);
	if (len == c->len && memcmp(p, c->psdu, len) == 0) {
		c->got++;
	} else {
		c->wrong++;
	}
	/* rx NULL: the static receiver behind the original API. */
	c->start = c->rx != NULL ? ieee154_phy_rx_frame_start_i(c->rx)
				 : ieee154_phy_rx_frame_start();
}

static void rx_any(const uint8_t *p, uint8_t len, int lqi, bool fcs_ok, void *user)
{
	struct rxc *c = user;

	ARG_UNUSED(p);
	ARG_UNUSED(len);
	ARG_UNUSED(lqi);
	if (!fcs_ok) {
		c->bad++;
	}
}

/*
 * Two receivers fed alternately with different frames each decode their own, once, and report
 * the frame's start; the static receiver behind the original API works alongside them.
 */
ZTEST(pcap_wpan, test_two_receivers)
{
	static struct ieee154_phy_rx rxa, rxb;
	static int16_t bufa[2U * (IEEE154_PHY_MOD_SAMPLES(60) + 2000U)];
	static int16_t bufb[2U * (IEEE154_PHY_MOD_SAMPLES(60) + 2000U)];
	struct rxc ca = {.rx = &rxa}, cb = {.rx = &rxb}, cg = {.rx = NULL};
	const uint32_t lead_a = 1500U, lead_b = 1711U;
	size_t na, nb, total;

	ca.len = 60;
	cb.len = 33;
	make_psdu(ca.psdu, ca.len);
	make_psdu(cb.psdu, cb.len);
	memset(bufa, 0, sizeof(bufa));
	memset(bufb, 0, sizeof(bufb));
	na = ieee154_phy_modulate(ca.psdu, ca.len, 1500, bufa + 2U * lead_a,
				  IEEE154_PHY_MOD_SAMPLES(60));
	nb = ieee154_phy_modulate(cb.psdu, cb.len, 1500, bufb + 2U * lead_b,
				  IEEE154_PHY_MOD_SAMPLES(60));
	zassert_true(na > 0U && nb > 0U);
	total = lead_a + na + 300U;
	zassert_true(lead_b + nb + 300U <= total);

	ieee154_phy_rx_init(&rxa);
	ieee154_phy_set_rx_cb_i(&rxa, rx_good, &ca);
	ieee154_phy_set_all_cb_i(&rxa, rx_any, &ca);
	ieee154_phy_rx_init(&rxb);
	ieee154_phy_set_rx_cb_i(&rxb, rx_good, &cb);
	/* The static receiver gets frame b through the original API. */
	memcpy(cg.psdu, cb.psdu, cb.len);
	cg.len = cb.len;
	ieee154_phy_set_rx_cb(rx_good, &cg);
	ieee154_phy_rx_reset();
	for (size_t p = 0; p < total; p += 256U) {
		size_t n = total - p < 256U ? total - p : 256U;

		ieee154_phy_rx_block_i(&rxa, bufa + 2U * p, n);
		ieee154_phy_rx_block_i(&rxb, bufb + 2U * p, n);
		ieee154_phy_rx_block(bufb + 2U * p, n);
	}
	zassert_equal(ca.got, 1, "receiver a: %d frames", ca.got);
	zassert_equal(cb.got, 1, "receiver b: %d frames", cb.got);
	zassert_equal(cg.got, 1, "the static receiver: %d frames", cg.got);
	zassert_equal(ca.wrong + cb.wrong + cg.wrong + ca.bad, 0);
	/* Between blocks a position stays within the last block (ieee154_phy_rx_position_i()). */
	zassert_true(ieee154_phy_rx_position_i(&rxa) <= total &&
		     ieee154_phy_rx_position_i(&rxa) + 256U >= total);
	printk("frame starts: a %u (first sample %u), b %u (first sample %u)\n", ca.start, lead_a,
	       cb.start, lead_b);
	zassert_within((int32_t)ca.start, (int32_t)lead_a, 4);
	zassert_within((int32_t)cb.start, (int32_t)lead_b, 4);
	/* No other test feeds the static receiver: its count started at 0 here, as rxb's. */
	zassert_equal(cb.start, cg.start);
	ieee154_phy_set_rx_cb(NULL, NULL);
}
