/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit test of the Bluetooth LE decoder of apps/pcap (apps/pcap/src/dec_ble.c). The twelve
 * golden packets of tests/unit/ubertooth_le (le_vectors.h), GFSK modulated as
 * tests/host/test_ble_pcap.py does (BT 0.5, h 0.5, 40 samples of noise in front) with noise and
 * carrier offsets, as LE 1M at 4 and 2 samples a symbol and as LE 2M at 4 MS/s; two packets in a
 * burst, broken CRCs, noise only, cut packets, a CONNECT_IND's access address and CRCInit
 * followed on a data channel, an unbalanced access address, an SNR sweep and the cost per
 * sample. The link layer reference (whitening, CRC-24, channel index) is written here after
 * apps/ubertooth/tools/le_ref.py, not taken from the upstream code the decoder uses. Runs on
 * native_sim.
 */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <esp_sdr/esp_sdr_dsp.h>
#include <test_rng.h>

#include "decode.h"

#include "../../ubertooth_le/src/le_vectors.h"

#define RNG_SEED 0x2545F4914F6CDD1DULL

/* test_ble_pcap.py's gfsk(): Gaussian filter BT, modulation index. */
#define BT         0.5
#define H          0.5
/* The unit amplitude in int16 units; silence (noise) before the first packet, samples. */
#define AMP        4000.0
#define LEAD       40U
/* The vectors' SNR (against the unit amplitude, over the sample rate), the offset tolerance. */
#define SNR_DB     25.0
#define OFFSET_TOL 15000
#define T_IFS_US   150U

#define CAP_MAX 32768U
#define GOT_MAX 16

static int16_t iq[2U * CAP_MAX] __aligned(4);
static double sig_i[CAP_MAX], sig_q[CAP_MAX];
static uint8_t scratch[4U * CAP_MAX + 2048U] __aligned(4);

struct got {
	int n;
	struct dec_ble_pkt p[GOT_MAX];
};

static struct got got;

#if defined(CONFIG_ARCH_POSIX)
uint64_t test_host_ns(void);
uint64_t test_host_cycles(void);
#endif

/* ---- the link layer, after le_ref.py ---- */

/* Channel index (0 .. 36 data, 37 .. 39 advertising) of a frequency, MHz. */
static int ref_channel_index(int mhz)
{
	int k = (mhz - 2402) / 2;

	if (mhz == 2402) {
		return 37;
	}
	if (mhz == 2426) {
		return 38;
	}
	if (mhz == 2480) {
		return 39;
	}
	return k < 12 ? k - 1 : k - 2;
}

/*
 * Whitening of @p n bytes (LSB first) on channel @p idx: the 7-bit LFSR x^7 + x^4 + 1, position 0
 * starting as 1, positions 1 .. 6 holding the index MSB first, the output position 6 feeding
 * position 0 and, XORed, position 4.
 */
static void ref_whiten(int idx, uint8_t *data, uint32_t n)
{
	uint8_t s[7] = {1};

	for (int i = 0; i < 6; i++) {
		s[1 + i] = (uint8_t)((idx >> (5 - i)) & 1);
	}
	for (uint32_t k = 0; k < 8U * n; k++) {
		uint8_t o = s[6];

		data[k / 8U] ^= (uint8_t)(o << (k % 8U));
		s[6] = s[5];
		s[5] = s[4];
		s[4] = s[3] ^ o;
		s[3] = s[2];
		s[2] = s[1];
		s[1] = s[0];
		s[0] = o;
	}
}

/* The CRC register after the PDU, CRCInit @p init (0x555555 on the advertising channels). */
static uint32_t ref_crc24(uint32_t init, const uint8_t *pdu, uint32_t n)
{
	uint32_t r = init & 0xFFFFFFU;

	for (uint32_t i = 0; i < n; i++) {
		for (int j = 0; j < 8; j++) {
			uint32_t fb = ((r >> 23) & 1U) ^ ((pdu[i] >> j) & 1U);

			r = (r << 1) & 0xFFFFFFU;
			if (fb != 0U) {
				r ^= 0x00065BU;
			}
		}
	}
	return r;
}

/* The CRC's three bytes as dewhitened: register position 23 goes first. */
static void ref_crc_bytes(uint32_t r, uint8_t *out)
{
	memset(out, 0, 3);
	for (int k = 0; k < 24; k++) {
		out[k / 8] |= (uint8_t)(((r >> (23 - k)) & 1U) << (k % 8));
	}
}

/* What dec_ble_pkt.air holds for a packet: access address, PDU and CRC, dewhitened. */
static uint32_t ref_clear(uint32_t aa, const uint8_t *pdu, uint32_t n, uint32_t crc_init,
			  uint8_t *out)
{
	for (int i = 0; i < 4; i++) {
		out[i] = (uint8_t)(aa >> (8 * i));
	}
	memcpy(out + 4, pdu, n);
	ref_crc_bytes(ref_crc24(crc_init, pdu, n), out + 4 + n);
	return 4U + n + 3U;
}

/* The on-air bytes: @p pre preamble bytes, the access address, the whitened PDU and CRC. */
static uint32_t ref_air(uint32_t aa, int mhz, const uint8_t *pdu, uint32_t n, uint32_t crc_init,
			uint32_t pre, uint8_t *out)
{
	uint32_t len = 0;

	for (uint32_t i = 0; i < pre; i++) {
		out[len++] = (aa & 1U) != 0U ? 0x55 : 0xAA;
	}
	len += ref_clear(aa, pdu, n, crc_init, out + len);
	ref_whiten(ref_channel_index(mhz), out + pre + 4U, n + 3U);
	return len;
}

/* ---- modulation and impairments, after test_ble_pcap.py ---- */

/* GFSK baseband of on-air bytes (LSB first) at @p sps samples a symbol, unit amplitude. */
static uint32_t gfsk(const uint8_t *air, uint32_t len, uint32_t sps, double *oi, double *oq)
{
	const int taps = 3 * (int)sps + 1, half = 3 * (int)sps / 2;
	const int n = 8 * (int)len * (int)sps;
	const double sigma = sqrt(log(2.0)) / (ESP_SDR_TAU * BT);
	double g[13], sum = 0.0, phase = 0.0;

	for (int m = 0; m < taps; m++) {
		double t = (double)(m - half) / sps;

		g[m] = exp(-t * t / (2.0 * sigma * sigma));
		sum += g[m];
	}
	/* np.convolve(nrz, g / g.sum(), mode="same"), then the phase as its running sum. */
	for (int i = 0; i < n; i++) {
		double f = 0.0;

		for (int m = 0; m < taps; m++) {
			int at = i + half - m;

			if (at >= 0 && at < n) {
				int bit = at / (int)sps;

				f += g[m] * (((air[bit / 8] >> (bit % 8)) & 1) != 0 ? 1.0 : -1.0);
			}
		}
		phase += f / sum * (ESP_SDR_TAU / 2.0) * H / sps;
		oi[i] = cos(phase);
		oq[i] = sin(phase);
	}
	return (uint32_t)n;
}

struct part {
	const uint8_t *air;
	uint32_t len;
	/* Samples a symbol, samples of silence after it. */
	uint32_t sps, gap;
};

static int16_t to16(double v)
{
	long r = lrint(v);

	return (int16_t)(r > INT16_MAX ? INT16_MAX : (r < INT16_MIN ? INT16_MIN : r));
}

/*
 * LEAD samples of silence, then every part's packet and gap, at @p rate samples a second; a
 * carrier offset; noise at @p snr_db against the unit amplitude; scaled to AMP
 * (test_ble_pcap.py's capture()). @p b holds it as a channel at @p mhz.
 */
static void capture(struct burst *b, const struct part *parts, size_t nparts, uint32_t rate,
		    int mhz, double offset_hz, double snr_db, uint64_t seed)
{
	const double sigma = pow(10.0, -snr_db / 20.0) / sqrt(2.0);
	uint32_t n = LEAD;

	memset(sig_i, 0, sizeof(sig_i[0]) * LEAD);
	memset(sig_q, 0, sizeof(sig_q[0]) * LEAD);
	for (size_t j = 0; j < nparts; j++) {
		zassert_true(n + 8U * parts[j].len * parts[j].sps + parts[j].gap <= CAP_MAX,
			     "capture too long");
		n += gfsk(parts[j].air, parts[j].len, parts[j].sps, sig_i + n, sig_q + n);
		memset(sig_i + n, 0, sizeof(sig_i[0]) * parts[j].gap);
		memset(sig_q + n, 0, sizeof(sig_q[0]) * parts[j].gap);
		n += parts[j].gap;
	}
	test_rng_seed(seed);
	for (uint32_t i = 0; i < n; i++) {
		double ph = ESP_SDR_TAU * offset_hz / rate * i;
		double c = cos(ph), s = sin(ph);
		double re = sig_i[i] * c - sig_q[i] * s + sigma * test_rng_gauss();
		double im = sig_i[i] * s + sig_q[i] * c + sigma * test_rng_gauss();

		iq[2U * i] = to16(AMP * re);
		iq[2U * i + 1U] = to16(AMP * im);
	}
	memset(b, 0, sizeof(*b));
	b->rate_hz = rate;
	b->freq_hz = (uint64_t)mhz * 1000000U;
	b->n = n;
	b->loud_to = n - 1U;
	b->iq = iq;
}

/* @p n samples of complex Gaussian noise, @p sigma int16 units a component. */
static void noise(struct burst *b, uint32_t n, uint32_t rate, double sigma, uint64_t seed)
{
	test_rng_seed(seed);
	for (uint32_t i = 0; i < 2U * n; i++) {
		iq[i] = to16(sigma * test_rng_gauss());
	}
	memset(b, 0, sizeof(*b));
	b->rate_hz = rate;
	b->freq_hz = 2402000000ULL;
	b->n = n;
	b->iq = iq;
}

/* ---- decoding ---- */

static void collect(const struct dec_ble_pkt *pkt, void *user)
{
	struct got *g = user;

	if (g->n < GOT_MAX) {
		g->p[g->n] = *pkt;
	}
	g->n++;
}

/* The advertising access address with its CRCInit, two bit errors, LE 1M only, good CRCs only. */
static void cfg_init(struct dec_ble_cfg *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->aa[0].aa = DEC_BLE_ADV_AA;
	cfg->aa[0].crc_init = DEC_BLE_ADV_CRC_INIT;
	cfg->aa_count = 1;
	cfg->max_errors = 2;
}

static void cfg_add(struct dec_ble_cfg *cfg, uint32_t aa, uint32_t crc_init)
{
	cfg->aa[cfg->aa_count].aa = aa;
	cfg->aa[cfg->aa_count].crc_init = crc_init;
	cfg->aa_count++;
}

static int run(const struct burst *b, const struct dec_ble_cfg *cfg)
{
	int rc;

	got.n = 0;
	rc = dec_ble_run(b, cfg, scratch, sizeof(scratch), collect, &got);
	if (rc >= 0) {
		zassert_equal(rc, got.n, "count %d, emitted %d", rc, got.n);
	}
	return rc;
}

static const struct le_vec *vec(const char *name)
{
	for (size_t i = 0; i < ARRAY_SIZE(le_vecs); i++) {
		if (strcmp(le_vecs[i].name, name) == 0) {
			return &le_vecs[i];
		}
	}
	zassert_unreachable("no vector %s", name);
	return NULL;
}

/* Whether @p p is golden packet @p v, received intact. */
static bool is_vec(const struct dec_ble_pkt *p, const struct le_vec *v)
{
	uint8_t clear[DEC_BLE_AIR_MAX];
	uint32_t len = ref_clear(v->aa, v->pdu, v->pdu_len, v->crc_init, clear);

	return p->aa == v->aa && p->crc_checked && p->crc_ok && p->len == len &&
	       memcmp(p->air, clear, len) == 0;
}

/*
 * Golden packet @p v as LE 1M at @p sps samples a symbol (4 or 2 MS/s) or as LE 2M (4 MS/s, 2
 * samples a symbol, 16 preamble bits), with LE 2M searched too at 4 MS/s: exactly one packet,
 * this one, intact, its offset and start right.
 */
static void check_vec(const struct le_vec *v, uint32_t sps, bool le2m, double offset_hz,
		      uint64_t seed)
{
	uint8_t air[DEC_BLE_AIR_MAX + 2U], clear[DEC_BLE_AIR_MAX];
	const uint32_t rate = le2m ? 4000000U : sps * 1000000U;
	struct part part = {air, 0, sps, 30};
	struct dec_ble_cfg cfg;
	const struct dec_ble_pkt *p = &got.p[0];
	uint32_t len;
	struct burst b;

	if (le2m) {
		air[part.len++] = v->air[0];
	}
	memcpy(air + part.len, v->air, v->air_len);
	part.len += v->air_len;
	capture(&b, &part, 1, rate, v->mhz, offset_hz, SNR_DB, seed);
	cfg_init(&cfg);
	if (v->aa != DEC_BLE_ADV_AA) {
		cfg_add(&cfg, v->aa, v->crc_init);
	}
	cfg.phy_2m = rate == 4000000U;
	zassert_equal(run(&b, &cfg), 1, "%s sps %u%s %d Hz: %d packets", v->name, sps,
		      le2m ? " LE 2M" : "", (int)offset_hz, got.n);
	zassert_equal(p->phy, le2m ? DEC_BLE_PHY_2M : DEC_BLE_PHY_1M, "%s: PHY", v->name);
	zassert_equal(p->aa, v->aa, "%s: aa", v->name);
	zassert_equal(p->aa_errors, 0, "%s: %u aa errors", v->name, p->aa_errors);
	zassert_true(p->crc_checked && p->crc_ok, "%s sps %u%s: CRC", v->name, sps,
		     le2m ? " LE 2M" : "");
	len = ref_clear(v->aa, v->pdu, v->pdu_len, v->crc_init, clear);
	zassert_equal(p->len, len, "%s: len %u", v->name, p->len);
	zassert_mem_equal(p->air, clear, len, "%s: air", v->name);
	zassert_within(p->offset_hz, (int32_t)offset_hz, OFFSET_TOL, "%s sps %u: offset %d for %d",
		       v->name, sps, p->offset_hz, (int)offset_hz);
	zassert_within(p->start, (int32_t)LEAD, (int32_t)sps, "%s sps %u%s: start %d", v->name, sps,
		       le2m ? " LE 2M" : "", p->start);
	zassert_true((double)p->power > 0.5 * AMP * AMP && (double)p->power < 1.5 * AMP * AMP,
		     "%s: power %f", v->name, (double)p->power);
}

static void *suite_setup(void)
{
	zassert_true(dec_ble_scratch(CAP_MAX) <= sizeof(scratch), "scratch %u",
		     (unsigned int)dec_ble_scratch(CAP_MAX));
	return NULL;
}

ZTEST_SUITE(pcap_ble, NULL, suite_setup, NULL, NULL, NULL);

/* The test's own reference against the golden vectors: on-air bytes, and the 50 USB bytes. */
ZTEST(pcap_ble, test_reference)
{
	uint8_t air[DEC_BLE_AIR_MAX + 1U], clear[DEC_BLE_AIR_MAX];

	for (size_t i = 0; i < ARRAY_SIZE(le_vecs); i++) {
		const struct le_vec *v = &le_vecs[i];
		uint32_t n = ref_air(v->aa, v->mhz, v->pdu, v->pdu_len, v->crc_init, 1U, air);
		uint32_t c = ref_clear(v->aa, v->pdu, v->pdu_len, v->crc_init, clear);

		zassert_equal(n, v->air_len, "%s", v->name);
		zassert_mem_equal(air, v->air, n, "%s: air", v->name);
		zassert_mem_equal(clear, v->usb, MIN(c, 50U), "%s: usb", v->name);
	}
}

ZTEST(pcap_ble, test_rf_channel)
{
	static const struct {
		uint64_t hz;
		int ch;
	} t[] = {
		{2402000000ULL, 0},  {2404000000ULL, 1},  {2426000000ULL, 12}, {2480000000ULL, 39},
		{2402400000ULL, 0},  {2479600000ULL, 39}, {2441700000ULL, 20}, {2401000000ULL, -1},
		{2403000000ULL, -1}, {2402500000ULL, -1}, {2401500000ULL, -1}, {2400000000ULL, -1},
		{2482000000ULL, -1}, {2481000000ULL, -1}, {0ULL, -1},          {UINT64_MAX, -1},
	};

	for (size_t i = 0; i < ARRAY_SIZE(t); i++) {
		zassert_equal(dec_ble_rf_channel(t[i].hz), t[i].ch, "%u Hz",
			      (unsigned int)(t[i].hz % 4000000000ULL));
	}
}

ZTEST(pcap_ble, test_arguments)
{
	const struct le_vec *v = vec("adv_ind_37");
	struct part part = {v->air, v->air_len, 4, 30};
	struct dec_ble_cfg cfg;
	struct burst b;

	capture(&b, &part, 1, 4000000U, v->mhz, 0.0, SNR_DB, RNG_SEED);
	cfg_init(&cfg);
	zassert_equal(run(&b, &cfg), 1);
	zassert_equal(dec_ble_run(&b, &cfg, scratch, sizeof(scratch), NULL, NULL), 1,
		      "no callback");
	zassert_equal(dec_ble_run(&b, &cfg, scratch, dec_ble_scratch(b.n) - 1U, collect, &got),
		      -ENOMEM);
	zassert_equal(dec_ble_run(&b, &cfg, NULL, sizeof(scratch), collect, &got), -ENOMEM);
	/* The scratch need not be aligned. */
	got.n = 0;
	zassert_equal(dec_ble_run(&b, &cfg, scratch + 1, dec_ble_scratch(b.n), collect, &got), 1);
	b.rate_hz = 3200000U;
	zassert_equal(run(&b, &cfg), -ENOTSUP);
	b.rate_hz = 8000000U;
	zassert_equal(run(&b, &cfg), -ENOTSUP);
	b.rate_hz = 4000000U;
	b.freq_hz = 2403000000ULL;
	zassert_equal(run(&b, &cfg), -EINVAL, "off the grid");
	b.freq_hz = 2402000000ULL;
	cfg.max_errors = 3;
	zassert_equal(run(&b, &cfg), -EINVAL);
	cfg.max_errors = 0;
	zassert_equal(run(&b, &cfg), 1);
	cfg.aa_count = DEC_BLE_AA_MAX + 1U;
	zassert_equal(run(&b, &cfg), -EINVAL);
	cfg.aa_count = 0;
	zassert_equal(run(&b, &cfg), 0, "no address");
	cfg_init(&cfg);
	b.n = 100;
	zassert_equal(run(&b, &cfg), 0, "too short");
	b.n = 0;
	zassert_equal(run(&b, &cfg), 0, "empty");
}

ZTEST(pcap_ble, test_vectors_1m)
{
	static const double offsets[] = {-150e3, 0.0, 150e3};

	for (uint32_t sps = 4; sps >= 2U; sps -= 2U) {
		for (size_t o = 0; o < ARRAY_SIZE(offsets); o++) {
			for (size_t i = 0; i < ARRAY_SIZE(le_vecs); i++) {
				check_vec(&le_vecs[i], sps, false, offsets[o],
					  RNG_SEED + 100U * sps + 10U * o + i);
			}
		}
	}
}

ZTEST(pcap_ble, test_vectors_2m)
{
	static const double offsets[] = {-150e3, 0.0, 150e3};

	for (size_t o = 0; o < ARRAY_SIZE(offsets); o++) {
		for (size_t i = 0; i < ARRAY_SIZE(le_vecs); i++) {
			check_vec(&le_vecs[i], 2, true, offsets[o], RNG_SEED + 1000U + 10U * o + i);
		}
	}
}

/* An ADV_IND and, T_IFS later, a SCAN_RSP: both, in order, the second after the first's end. */
ZTEST(pcap_ble, test_two_packets)
{
	const struct le_vec *adv = vec("adv_ind_37"), *rsp = vec("scan_rsp_37");
	uint8_t adv2[DEC_BLE_AIR_MAX + 2U], rsp2[DEC_BLE_AIR_MAX + 2U];
	struct dec_ble_cfg cfg;
	struct burst b;

	adv2[0] = adv->air[0];
	memcpy(adv2 + 1, adv->air, adv->air_len);
	rsp2[0] = rsp->air[0];
	memcpy(rsp2 + 1, rsp->air, rsp->air_len);
	cfg_init(&cfg);
	cfg.phy_2m = true;

	for (uint32_t sps = 4; sps >= 2U; sps -= 2U) {
		struct part parts[] = {
			{adv->air, adv->air_len, sps, T_IFS_US * sps},
			{rsp->air, rsp->air_len, sps, 30},
		};

		capture(&b, parts, 2, sps * 1000000U, 2402, -25e3, SNR_DB, RNG_SEED + sps);
		zassert_equal(run(&b, &cfg), 2, "sps %u: %d packets", sps, got.n);
		zassert_true(is_vec(&got.p[0], adv) && is_vec(&got.p[1], rsp), "sps %u", sps);
		zassert_within(got.p[0].start, (int32_t)LEAD, (int32_t)sps);
		zassert_true(got.p[1].start > got.p[0].start + (int32_t)(8U * adv->air_len * sps),
			     "sps %u: %d after %d", sps, got.p[1].start, got.p[0].start);
		zassert_within(got.p[1].start,
			       (int32_t)(LEAD + 8U * adv->air_len * sps + T_IFS_US * sps),
			       (int32_t)sps, "sps %u: second at %d", sps, got.p[1].start);
	}

	/* The same as LE 2M at 4 MS/s. */
	{
		struct part parts[] = {
			{adv2, adv->air_len + 1U, 2, T_IFS_US * 4U},
			{rsp2, rsp->air_len + 1U, 2, 30},
		};

		capture(&b, parts, 2, 4000000U, 2402, 25e3, SNR_DB, RNG_SEED + 5U);
		zassert_equal(run(&b, &cfg), 2, "LE 2M: %d packets", got.n);
		zassert_true(is_vec(&got.p[0], adv) && is_vec(&got.p[1], rsp), "LE 2M");
		zassert_true(got.p[0].phy == DEC_BLE_PHY_2M && got.p[1].phy == DEC_BLE_PHY_2M);
		zassert_true(got.p[1].start >
			     got.p[0].start + (int32_t)(16U * (adv->air_len + 1U)));
	}

	/* An LE 1M packet and an LE 2M one: LE 1M packets come out first. */
	{
		struct part parts[] = {
			{adv->air, adv->air_len, 4, T_IFS_US * 4U},
			{rsp2, rsp->air_len + 1U, 2, 30},
		};

		capture(&b, parts, 2, 4000000U, 2402, 0.0, SNR_DB, RNG_SEED + 6U);
		zassert_equal(run(&b, &cfg), 2, "mixed: %d packets", got.n);
		zassert_true(is_vec(&got.p[0], adv) && got.p[0].phy == DEC_BLE_PHY_1M);
		zassert_true(is_vec(&got.p[1], rsp) && got.p[1].phy == DEC_BLE_PHY_2M);
		zassert_within(got.p[1].start, (int32_t)(LEAD + 32U * adv->air_len + T_IFS_US * 4U),
			       2);
		cfg.phy_2m = false;
		zassert_equal(run(&b, &cfg), 1, "LE 1M only: %d packets", got.n);
	}
}

/* A PDU bit flipped: emitted only with keep_bad, flagged, the bit wrong and nothing else. */
ZTEST(pcap_ble, test_bad_crc)
{
	const struct le_vec *adv = vec("adv_ind_37"), *rsp = vec("scan_rsp_37");
	uint8_t broken[DEC_BLE_AIR_MAX + 1U], clear[DEC_BLE_AIR_MAX];
	struct dec_ble_cfg cfg;
	struct burst b;
	uint32_t len = ref_clear(rsp->aa, rsp->pdu, rsp->pdu_len, rsp->crc_init, clear);

	memcpy(broken, rsp->air, rsp->air_len);
	/* The second payload byte: dec_ble_pkt.air[7]. */
	broken[8] ^= 0x10;
	clear[7] ^= 0x10;

	for (uint32_t sps = 4; sps >= 2U; sps -= 2U) {
		struct part part = {broken, rsp->air_len, sps, 30};
		struct part parts[] = {
			{adv->air, adv->air_len, sps, T_IFS_US * sps},
			{broken, rsp->air_len, sps, 30},
		};

		capture(&b, &part, 1, sps * 1000000U, 2402, -25e3, SNR_DB, RNG_SEED + sps);
		cfg_init(&cfg);
		zassert_equal(run(&b, &cfg), 0, "sps %u: emitted", sps);
		cfg.keep_bad = true;
		zassert_equal(run(&b, &cfg), 1, "sps %u: not kept", sps);
		zassert_true(got.p[0].crc_checked && !got.p[0].crc_ok);
		zassert_equal(got.p[0].aa_errors, 0);
		zassert_equal(got.p[0].len, len);
		zassert_mem_equal(got.p[0].air, clear, len);

		/* ble_pcap.py's test: an advertisement, then the broken one. */
		capture(&b, parts, 2, sps * 1000000U, 2402, -25e3, SNR_DB, RNG_SEED + 10U + sps);
		zassert_equal(run(&b, &cfg), 2, "sps %u: %d packets", sps, got.n);
		zassert_true(is_vec(&got.p[0], adv) && got.p[1].crc_checked && !got.p[1].crc_ok);
		zassert_true(got.p[1].start > got.p[0].start + (int32_t)(8U * adv->air_len * sps));
		cfg.keep_bad = false;
		zassert_equal(run(&b, &cfg), 1);
		zassert_true(is_vec(&got.p[0], adv));
	}
}

/* Noise, and silence: nothing, bad CRCs kept, two bit errors allowed, four addresses. */
ZTEST(pcap_ble, test_noise_only)
{
	struct dec_ble_cfg cfg;
	struct burst b;

	cfg_init(&cfg);
	cfg_add(&cfg, 0x332211D6U, 0x012345U);
	cfg_add(&cfg, 0x8F1E3A71U, 0xABCDEFU);
	cfg_add(&cfg, 0x6A5C3B21U, 0U);
	cfg.keep_bad = true;
	cfg.phy_2m = true;
	for (unsigned int t = 0; t < 4U; t++) {
		noise(&b, CAP_MAX, t < 2U ? 4000000U : 2000000U, 500.0 + 1500.0 * t, RNG_SEED + t);
		zassert_equal(run(&b, &cfg), 0, "noise %u: %d packets", t, got.n);
	}
	noise(&b, CAP_MAX, 4000000U, 0.0, RNG_SEED);
	zassert_equal(run(&b, &cfg), 0, "silence");
	/* A carrier 300 kHz off the centre. */
	noise(&b, CAP_MAX, 4000000U, 100.0, RNG_SEED);
	for (uint32_t i = 0; i < CAP_MAX; i++) {
		double ph = ESP_SDR_TAU * 300e3 / 4e6 * i;

		iq[2U * i] = to16(iq[2U * i] + AMP * cos(ph));
		iq[2U * i + 1U] = to16(iq[2U * i + 1U] + AMP * sin(ph));
	}
	zassert_equal(run(&b, &cfg), 0, "carrier");
}

/* A packet the burst's end cuts: not emitted; one before it still is. */
ZTEST(pcap_ble, test_cut)
{
	const struct le_vec *adv = vec("adv_ind_37"), *rsp = vec("scan_rsp_37");
	struct dec_ble_cfg cfg;
	struct burst b;

	cfg_init(&cfg);
	cfg.keep_bad = true;
	for (uint32_t sps = 4; sps >= 2U; sps -= 2U) {
		/* One symbol after the packet: room for the sampling phases after the best. */
		struct part part = {adv->air, adv->air_len, sps, sps};
		struct part parts[] = {
			{rsp->air, rsp->air_len, sps, T_IFS_US * sps},
			{adv->air, adv->air_len, sps, 0},
		};
		uint32_t end = LEAD + 8U * adv->air_len * sps;

		capture(&b, &part, 1, sps * 1000000U, 2402, 30e3, SNR_DB, RNG_SEED + sps);
		zassert_equal(b.n, end + sps);
		zassert_equal(run(&b, &cfg), 1, "sps %u: whole", sps);
		b.n = end - sps;
		zassert_equal(run(&b, &cfg), 0, "sps %u: a symbol short", sps);
		b.n = end / 2U;
		zassert_equal(run(&b, &cfg), 0, "sps %u: half", sps);

		capture(&b, parts, 2, sps * 1000000U, 2402, 30e3, SNR_DB, RNG_SEED + 10U + sps);
		b.n -= 8U * 3U * sps;
		zassert_equal(run(&b, &cfg), 1, "sps %u: the first of two", sps);
		zassert_true(is_vec(&got.p[0], rsp));
	}
}

/*
 * The burst starts inside the preamble: two preamble symbols left, the level from the address's
 * own window, the start before the burst's first sample.
 */
ZTEST(pcap_ble, test_preamble_cut)
{
	const struct le_vec *v = vec("scan_req_39");
	struct dec_ble_cfg cfg;
	struct burst b;

	cfg_init(&cfg);
	for (uint32_t sps = 4; sps >= 2U; sps -= 2U) {
		struct part part = {v->air, v->air_len, sps, 30};
		const uint32_t cut = LEAD + 6U * sps;

		capture(&b, &part, 1, sps * 1000000U, v->mhz, 40e3, SNR_DB, RNG_SEED + sps);
		b.iq += 2U * cut;
		b.n -= cut;
		zassert_equal(run(&b, &cfg), 1, "sps %u", sps);
		zassert_true(is_vec(&got.p[0], v) && got.p[0].aa_errors == 0U, "sps %u", sps);
		zassert_within(got.p[0].start, (int32_t)LEAD - (int32_t)cut, (int32_t)sps,
			       "sps %u: start %d", sps, got.p[0].start);
		/* The address's own level is biased by its runs: loosely. */
		zassert_within(got.p[0].offset_hz, 40000, 40000, "sps %u: offset %d", sps,
			       got.p[0].offset_hz);
	}
}

/* Access address bit errors: up to max_errors, counted in aa_errors (the CRC skips the address). */
ZTEST(pcap_ble, test_aa_errors)
{
	const struct le_vec *v = vec("adv_ind_37");
	uint8_t air[DEC_BLE_AIR_MAX + 1U];
	struct part part = {air, v->air_len, 4, 30};
	struct dec_ble_cfg cfg;
	struct burst b;

	memcpy(air, v->air, v->air_len);
	/* Address bits 9 and 26. */
	air[2] ^= 0x02;
	air[4] ^= 0x04;
	capture(&b, &part, 1, 4000000U, v->mhz, -60e3, SNR_DB, RNG_SEED);
	cfg_init(&cfg);
	for (uint8_t e = 0; e <= 2U; e++) {
		cfg.max_errors = e;
		zassert_equal(run(&b, &cfg), e == 2U ? 1 : 0, "max_errors %u: %d packets", e,
			      got.n);
	}
	zassert_true(is_vec(&got.p[0], v));
	zassert_equal(got.p[0].aa_errors, 2);
	zassert_equal(got.p[0].aa, DEC_BLE_ADV_AA);
}

/* A CONNECT_IND announces its access address and CRCInit; they find the connection's packets. */
ZTEST(pcap_ble, test_connect_ind)
{
	const struct le_vec *v = vec("connect_req_37"), *data = vec("data_l2cap_20");
	/* LLData follows the header, InitA and AdvA: AA, then CRCInit, little endian. */
	const uint8_t *ll = v->pdu + 2 + 12;
	const uint32_t aa = ll[0] | ll[1] << 8 | ll[2] << 16 | (uint32_t)ll[3] << 24;
	const uint32_t crc_init = ll[4] | ll[5] << 8 | ll[6] << 16;
	uint8_t broken[DEC_BLE_AIR_MAX + 1U];
	struct part part = {v->air, v->air_len, 4, 30};
	struct dec_ble_cfg cfg;
	struct burst b;

	cfg_init(&cfg);
	capture(&b, &part, 1, 4000000U, v->mhz, 60e3, SNR_DB, RNG_SEED);
	zassert_equal(run(&b, &cfg), 1);
	zassert_true(is_vec(&got.p[0], v));
	zassert_equal(got.p[0].conn_aa, aa, "conn_aa %08x", got.p[0].conn_aa);
	zassert_equal(got.p[0].conn_crc_init, crc_init, "conn_crc_init %06x",
		      got.p[0].conn_crc_init);
	zassert_equal(aa, data->aa);
	zassert_equal(crc_init, data->crc_init);

	/* Other PDUs, and a CONNECT_IND whose CRC failed, announce nothing. */
	part.air = vec("adv_ind_37")->air;
	part.len = vec("adv_ind_37")->air_len;
	capture(&b, &part, 1, 4000000U, 2402, 0.0, SNR_DB, RNG_SEED);
	zassert_equal(run(&b, &cfg), 1);
	zassert_true(got.p[0].conn_aa == 0U && got.p[0].conn_crc_init == 0U);
	memcpy(broken, v->air, v->air_len);
	broken[8] ^= 0x01;
	part.air = broken;
	part.len = v->air_len;
	capture(&b, &part, 1, 4000000U, v->mhz, 0.0, SNR_DB, RNG_SEED);
	cfg.keep_bad = true;
	zassert_equal(run(&b, &cfg), 1);
	zassert_true(!got.p[0].crc_ok && got.p[0].conn_aa == 0U && got.p[0].conn_crc_init == 0U);

	/* The connection's packets on a data channel: found with the announced address only. */
	part.air = data->air;
	part.len = data->air_len;
	for (uint32_t sps = 4; sps >= 2U; sps -= 2U) {
		part.sps = sps;
		capture(&b, &part, 1, sps * 1000000U, data->mhz, -80e3, SNR_DB, RNG_SEED + sps);
		cfg_init(&cfg);
		zassert_equal(run(&b, &cfg), 0, "advertising address only");
		cfg_add(&cfg, aa, crc_init);
		zassert_equal(run(&b, &cfg), 1, "sps %u: followed", sps);
		zassert_true(is_vec(&got.p[0], data));
		zassert_true(got.p[0].conn_aa == 0U && got.p[0].conn_crc_init == 0U);
	}
}

/* Several addresses; CRCInit unknown; an address with 24 ones (the coarse step's harder case). */
ZTEST(pcap_ble, test_data_channel)
{
	static const uint32_t odd_aa = 0xF7B5ED6FU, odd_crc = 0x5A5A5AU;
	const struct le_vec *v = vec("data_long_15");
	uint8_t pdu[22], air[DEC_BLE_AIR_MAX + 1U], clear[DEC_BLE_AIR_MAX];
	struct part part = {v->air, v->air_len, 4, 30};
	struct dec_ble_cfg cfg;
	struct burst b;
	uint32_t len;
	unsigned int found = 0, tries = 0;

	cfg_init(&cfg);
	cfg_add(&cfg, 0x332211D6U, 0x012345U);
	cfg_add(&cfg, 0x6A5C3B21U, 0x7F00A1U);
	cfg_add(&cfg, v->aa, v->crc_init);
	capture(&b, &part, 1, 4000000U, v->mhz, 120e3, SNR_DB, RNG_SEED);
	zassert_equal(run(&b, &cfg), 1);
	zassert_true(is_vec(&got.p[0], v));
	cfg.aa[3].crc_init = 0;
	zassert_equal(run(&b, &cfg), 1);
	zassert_true(!got.p[0].crc_checked && !got.p[0].crc_ok && got.p[0].aa == v->aa);
	zassert_equal(got.p[0].len, 4U + v->pdu_len + 3U);
	zassert_mem_equal(got.p[0].air + 4, v->pdu, v->pdu_len);

	/* A data PDU (LLID 2, 20 bytes) under an unbalanced access address, at 14 dB. */
	pdu[0] = 0x02;
	pdu[1] = 20;
	for (int i = 0; i < 20; i++) {
		pdu[2 + i] = (uint8_t)(13 * i + 7);
	}
	len = ref_air(odd_aa, 2410, pdu, sizeof(pdu), odd_crc, 1U, air);
	ref_clear(odd_aa, pdu, sizeof(pdu), odd_crc, clear);
	cfg_init(&cfg);
	cfg_add(&cfg, odd_aa, odd_crc);
	for (uint32_t sps = 4; sps >= 2U; sps -= 2U) {
		for (unsigned int t = 0; t < 8U; t++) {
			struct part p = {air, len, sps, 30};

			capture(&b, &p, 1, sps * 1000000U, 2410, -100e3 + 30e3 * t, 14.0,
				RNG_SEED + 20U * sps + t);
			tries++;
			if (run(&b, &cfg) == 1 && got.p[0].crc_ok && got.p[0].aa == odd_aa &&
			    memcmp(got.p[0].air, clear, 4U + sizeof(pdu) + 3U) == 0) {
				found++;
			}
		}
	}
	printk("address %08x (24 ones) at 14 dB: %u of %u\n", odd_aa, found, tries);
	zassert_equal(found, tries);

	/* The longest PDU: 255 bytes of payload, DEC_BLE_AIR_MAX bytes from the address on. */
	{
		static uint8_t big[2U + 255U];
		struct part p = {air, 0, 4, 30};

		big[0] = 0x02;
		big[1] = 255;
		for (int i = 0; i < 255; i++) {
			big[2 + i] = (uint8_t)(i * 29 + 1);
		}
		p.len = ref_air(odd_aa, 2410, big, sizeof(big), odd_crc, 1U, air);
		len = ref_clear(odd_aa, big, sizeof(big), odd_crc, clear);
		zassert_equal(len, DEC_BLE_AIR_MAX);
		capture(&b, &p, 1, 4000000U, 2410, 50e3, SNR_DB, RNG_SEED);
		zassert_equal(run(&b, &cfg), 1);
		zassert_true(got.p[0].crc_ok && got.p[0].len == len);
		zassert_mem_equal(got.p[0].air, clear, len);
	}
}

/* Five vectors at 8 to 20 dB, 10 tries each (offsets -50, 0, +50 kHz): printed; all at 20 dB. */
ZTEST(pcap_ble, test_snr_sweep)
{
	static const char *const names[] = {"adv_ind_37", "scan_req_39", "connect_req_37",
					    "data_l2cap_20", "data_long_15"};
	static const double snrs[] = {8.0, 11.0, 14.0, 17.0, 20.0};
	static const struct {
		const char *name;
		uint32_t sps, rate;
		bool le2m;
	} modes[] = {
		{"LE 1M, 4 MS/s", 4, 4000000U, false},
		{"LE 1M, 2 MS/s", 2, 2000000U, false},
		{"LE 2M, 4 MS/s", 2, 4000000U, true},
	};
	const unsigned int tries = 10U;

	for (size_t m = 0; m < ARRAY_SIZE(modes); m++) {
		unsigned int ok[ARRAY_SIZE(snrs)] = {0};

		for (size_t s = 0; s < ARRAY_SIZE(snrs); s++) {
			for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
				const struct le_vec *v = vec(names[i]);
				uint8_t air[DEC_BLE_AIR_MAX + 2U];
				struct part part = {air, 0, modes[m].sps, 30};
				struct dec_ble_cfg cfg;
				struct burst b;

				if (modes[m].le2m) {
					air[part.len++] = v->air[0];
				}
				memcpy(air + part.len, v->air, v->air_len);
				part.len += v->air_len;
				cfg_init(&cfg);
				if (v->aa != DEC_BLE_ADV_AA) {
					cfg_add(&cfg, v->aa, v->crc_init);
				}
				cfg.phy_2m = modes[m].rate == 4000000U;
				for (unsigned int t = 0; t < tries; t++) {
					capture(&b, &part, 1, modes[m].rate, v->mhz,
						50e3 * ((int)(t % 3U) - 1), snrs[s],
						RNG_SEED + 1000U * m + 100U * s + 10U * i + t);
					if (run(&b, &cfg) == 1 && is_vec(&got.p[0], v)) {
						ok[s]++;
					}
				}
			}
		}
		printk("SNR sweep %s:", modes[m].name);
		for (size_t s = 0; s < ARRAY_SIZE(snrs); s++) {
			printk(" %.0f dB %u/%u", snrs[s], ok[s],
			       tries * (unsigned int)ARRAY_SIZE(names));
		}
		printk("\n");
		zassert_equal(ok[ARRAY_SIZE(snrs) - 1U], tries * ARRAY_SIZE(names), "%s at 20 dB",
			      modes[m].name);
	}
}

/*
 * The host's time a sample: a burst of the twelve packets (whitened for 2402 MHz, T_IFS apart)
 * at 4 MS/s with LE 2M searched too and at 2 MS/s, and noise at 4 MS/s. Printed.
 */
ZTEST(pcap_ble, test_cost)
{
#if defined(CONFIG_ARCH_POSIX)
	static uint8_t airs[ARRAY_SIZE(le_vecs)][DEC_BLE_AIR_MAX + 1U];
	struct part parts[ARRAY_SIZE(le_vecs)];
	const unsigned int reps = 20U;
	struct dec_ble_cfg cfg;
	struct burst b;

	cfg_init(&cfg);
	cfg_add(&cfg, 0x332211D6U, 0x012345U);
	cfg_add(&cfg, 0x8F1E3A71U, 0xABCDEFU);
	cfg_add(&cfg, 0x6A5C3B21U, 0x7F00A1U);
	cfg.phy_2m = true;
	for (unsigned int k = 0; k < 3U; k++) {
		uint32_t rate = k == 1U ? 2000000U : 4000000U;
		uint64_t t0, c0, ns, cyc;
		int count = 0;

		if (k < 2U) {
			for (size_t i = 0; i < ARRAY_SIZE(le_vecs); i++) {
				const struct le_vec *v = &le_vecs[i];

				parts[i].air = airs[i];
				parts[i].len = ref_air(v->aa, 2402, v->pdu, v->pdu_len, v->crc_init,
						       1U, airs[i]);
				parts[i].sps = rate / 1000000U;
				parts[i].gap = T_IFS_US * parts[i].sps;
			}
			capture(&b, parts, ARRAY_SIZE(parts), rate, 2402, 20e3, 20.0, RNG_SEED);
		} else {
			noise(&b, CAP_MAX, rate, 1000.0, RNG_SEED);
		}
		/* Once untimed: caches and clock warm. */
		(void)dec_ble_run(&b, &cfg, scratch, sizeof(scratch), NULL, NULL);
		t0 = test_host_ns();
		c0 = test_host_cycles();
		for (unsigned int r = 0; r < reps; r++) {
			count = dec_ble_run(&b, &cfg, scratch, sizeof(scratch), NULL, NULL);
		}
		ns = test_host_ns() - t0;
		cyc = test_host_cycles() - c0;
		printk("cost, %s at %u MS/s (%u samples, %d packets, LE 2M %s): %.1f ns, %.1f host "
		       "cycles a sample\n",
		       k < 2U ? "12 packets" : "noise", rate / 1000000U, b.n, count,
		       rate == 4000000U ? "too" : "no", (double)ns / reps / b.n,
		       (double)cyc / reps / b.n);
		if (k < 2U) {
			zassert_equal(count, (int)ARRAY_SIZE(le_vecs), "%d packets", count);
		} else {
			zassert_equal(count, 0, "%d packets in noise", count);
		}
	}
#else
	ztest_test_skip();
#endif
}
