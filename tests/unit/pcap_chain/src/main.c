/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The survey path of apps/pcap for Bluetooth LE, end to end on native_sim: the golden packets
 * of tests/unit/ubertooth_le (le_vectors.h) GFSK modulated (BT 0.5, h 0.5) at 80 MS/s and put
 * off the centre at a bin of the 40-channel bank, noise added, quantized to int8 with the
 * shift the C6 picks, channelized by the fixed-point bank (esdr_pfbq.h, the C kernels) as the
 * firmware runs it, one bin collected into a burst and decoded by dec_ble.c at 4 MS/s. Checks
 * the bins' orientation (channel k above the centre for k up to M / 2), the scale and the
 * decoder on the bank's filtered output.
 */

#include <math.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <esp_sdr/esp_sdr_dsp.h>
#include <test_rng.h>

#include "decode.h"
#include "esdr_pfbq.h"

/* struct le_vec and le_vecs[]: the golden packets. */
#include "le_vectors.h"

#define FS        80e6
#define M         40U
#define D         (M / 2U)
#define GAIN      32.0f
#define SPS       80U
/* Samples of silence before the packet and after it, at 80 MS/s. */
#define LEAD      4000U
#define TAIL      4000U
#define PAIRS_MAX (LEAD + TAIL + 8U * 300U * SPS)
#define PUSH      4096U

static int8_t items[2U * PAIRS_MAX];
static float ph[PAIRS_MAX];
static uint8_t qmem[200U * 1024U] __aligned(16);
static int16_t biq[2U * BURST_MAX_SAMPLES];
static uint8_t scratch[160U * 1024U];
static struct esdr_pfbq pq;
static struct burst bb;
static unsigned int want_k;
static struct dec_ble_pkt got[4];
static int ngot;

/* The on-air bits of @p air (LSB first) GFSK modulated into ph[] (phase), from sample LEAD. */
static uint32_t modulate(const uint8_t *air, unsigned int len)
{
	uint32_t nbits = 8U * len, n = LEAD + nbits * SPS + TAIL;
	static float nrz[PAIRS_MAX];
	double sigma = sqrt(log(2.0)) / (ESP_SDR_TAU * 0.5) * SPS, phase = 0.0;
	int half = (int)(3.0 * sigma);

	for (uint32_t i = 0; i < n; i++) {
		if (i < LEAD || i >= LEAD + nbits * SPS) {
			nrz[i] = 0.0f;
		} else {
			uint32_t bit = (i - LEAD) / SPS;

			nrz[i] = ((air[bit / 8U] >> (bit % 8U)) & 1U) != 0U ? 1.0f : -1.0f;
		}
	}
	for (uint32_t i = 0; i < n; i++) {
		double a = 0.0, s = 0.0;

		for (int j = -half; j <= half; j++) {
			int k = (int)i + j;
			double g = exp(-(double)j * j / (2.0 * sigma * sigma));

			if (k >= 0 && k < (int)n) {
				a += g * (double)nrz[k];
			}
			s += g;
		}
		phase += ESP_SDR_TAU / 4.0 * a / s / SPS;
		ph[i] = (float)phase;
	}
	return n;
}

/*
 * ph[] at @p offset_hz off the centre, amplitude @p amp raw units, noise @p noise raw units rms a
 * component, to int8 items at shift 2 (raw = item << 2, as the C6 packs a loud window). The
 * signal is there only while the packet is (silence is noise alone).
 */
static void quantize(uint32_t n, double offset_hz, double amp, double noise)
{
	for (uint32_t i = 0; i < n; i++) {
		bool on = i >= LEAD && i < n - TAIL;
		double a = (double)ph[i] + ESP_SDR_TAU * offset_hz / FS * i;
		double vi = (on ? amp * cos(a) : 0.0) + noise * test_rng_gauss();
		double vq = (on ? amp * sin(a) : 0.0) + noise * test_rng_gauss();

		items[2U * i] = (int8_t)CLAMP(lrint(vi / 4.0), -128, 127);
		items[2U * i + 1U] = (int8_t)CLAMP(lrint(vq / 4.0), -128, 127);
	}
}

static void quad_cb(uint64_t m0, const int16_t *iq, uint8_t valid, uint8_t complete, void *user)
{
	ARG_UNUSED(complete);
	ARG_UNUSED(user);
	for (unsigned int q = 0; q < 4U; q++) {
		if ((valid & (1U << q)) == 0U || bb.n >= BURST_MAX_SAMPLES) {
			continue;
		}
		if (bb.n == 0U) {
			bb.m0 = m0 + q;
		}
		biq[2U * bb.n] = iq[8U * want_k + 2U * q];
		biq[2U * bb.n + 1U] = iq[8U * want_k + 2U * q + 1U];
		bb.n++;
	}
}

static void emit(const struct dec_ble_pkt *p, void *user)
{
	ARG_UNUSED(user);
	if (ngot < (int)ARRAY_SIZE(got)) {
		got[ngot] = *p;
	}
	ngot++;
}

/* Vector @p v at bin @p k (offset k spacings, k > M / 2 below), decoded from that bin. */
static void run(const struct le_vec *v, unsigned int k, double amp, double noise)
{
	struct esdr_pfbq_cfg cfg = {.channels = M, .quads = 1, .gain = GAIN};
	int off = k <= M / 2U ? (int)k : (int)k - (int)M;
	uint32_t n = modulate(v->air, v->air_len);
	const struct dec_ble_cfg bc = {
		.aa = {{.aa = v->aa, .crc_init = v->crc_init}},
		.aa_count = 1,
		.max_errors = 2,
		.phy_2m = true,
	};

	quantize(n, off * FS / M, amp, noise);
	zassert_ok(esdr_pfbq_init(&pq, &cfg, qmem, sizeof(qmem)));
	memset(&bb, 0, sizeof(bb));
	want_k = k;
	bb.chan = (uint16_t)k;
	bb.channels = M;
	bb.rate_hz = (uint32_t)(2.0 * FS / M);
	/* The decoder dewhitens with the channel of this frequency: the vector's own. */
	bb.freq_hz = (uint64_t)v->mhz * 1000000U;
	bb.iq = biq;
	for (uint32_t o = 0; o < n; o += PUSH) {
		zassert_true(esdr_pfbq_push(&pq, o, &items[2U * o], MIN(PUSH, n - o), 2, 0, 0, quad_cb,
					    NULL) >= 0);
	}
	(void)esdr_pfbq_flush(&pq, quad_cb, NULL);
	bb.loud_from = LEAD / D;
	bb.loud_to = (n - TAIL) / D;
	ngot = 0;
	zassert_true(dec_ble_run(&bb, &bc, scratch, sizeof(scratch), emit, NULL) >= 0);
}

ZTEST(pcap_chain, test_bins)
{
	/* Above and below the centre, near it and at the passband's edge (+-16 MHz). */
	static const unsigned int bins[] = {5, 1, 8, M - 7, M - 2, M - 8};

	test_rng_seed(11);
	for (unsigned int vi = 0; vi < ARRAY_SIZE(le_vecs); vi++) {
		const struct le_vec *v = &le_vecs[vi];

		for (unsigned int bi = 0; bi < ARRAY_SIZE(bins); bi++) {
			unsigned int k = bins[bi];

			run(v, k, 300.0, 2.0);
			zassert_equal(ngot, 1, "%s at bin %u: %d packets", v->name, k, ngot);
			zassert_true(got[0].crc_ok && got[0].phy == DEC_BLE_PHY_1M, "%s at bin %u",
				     v->name, k);
			zassert_mem_equal(got[0].air + 4, v->pdu, v->pdu_len, "%s at bin %u", v->name, k);
			/* The access address starts after LEAD samples and the 8-symbol preamble. */
			zassert_within((double)got[0].start * D, (double)LEAD,
				       2.0 * SPS, "%s at bin %u: start %d", v->name, k,
				       got[0].start);
		}
	}
}

ZTEST(pcap_chain, test_wrong_bin_finds_nothing)
{
	/* The packet at bin 5, decoded from bin 35 (its mirror): nothing, a mirrored spectrum
	 * would decode here. */
	test_rng_seed(12);
	want_k = 0;
	{
		const struct le_vec *v = &le_vecs[0];
		struct esdr_pfbq_cfg cfg = {.channels = M, .quads = 1, .gain = GAIN};
		uint32_t n = modulate(v->air, v->air_len);
		const struct dec_ble_cfg bc = {
			.aa = {{.aa = v->aa, .crc_init = v->crc_init}},
			.aa_count = 1,
			.max_errors = 2,
		};

		quantize(n, 5 * FS / M, 300.0, 2.0);
		zassert_ok(esdr_pfbq_init(&pq, &cfg, qmem, sizeof(qmem)));
		memset(&bb, 0, sizeof(bb));
		want_k = M - 5U;
		bb.chan = (uint16_t)want_k;
		bb.channels = M;
		bb.rate_hz = (uint32_t)(2.0 * FS / M);
		bb.freq_hz = (uint64_t)v->mhz * 1000000U;
		bb.iq = biq;
		for (uint32_t o = 0; o < n; o += PUSH) {
			(void)esdr_pfbq_push(&pq, o, &items[2U * o], MIN(PUSH, n - o), 2, 0, 0,
					     quad_cb, NULL);
		}
		(void)esdr_pfbq_flush(&pq, quad_cb, NULL);
		ngot = 0;
		zassert_true(dec_ble_run(&bb, &bc, scratch, sizeof(scratch), emit, NULL) >= 0);
		zassert_equal(ngot, 0, "decoded from the mirror bin");
	}
}

ZTEST_SUITE(pcap_chain, NULL, NULL, NULL, NULL, NULL);
