/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit test of apps/link/src/qam.c: the frame bookkeeping, both CRCs against their check
 * values, and frames from QPSK to 256-QAM with each FEC looped like the DAC plays them, cut
 * at a random offset, sent through gain, carrier offset, phase noise, a short ISI filter, DC,
 * noise and 10-bit quantization, and decoded (one frame, and both frames of a pair). The SNRs
 * keep a margin over where tools/qam_sim.c starts to lose frames. Runs on native_sim.
 */
#include <math.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <esp_sdr/esp_sdr_dsp.h>
#include <test_rng.h>

#include "hamming.h"
#include "qam.h"
#include "rs.h"

#define RNG_SEED 0x2f6b9e44a1c3d507ULL
#define WIN      QAM_WINDOW

struct chan {
	double snr_db, cfo_hz, pn_rad, dc, gain;
	double isi[3];
};

/* A channel like the bench's: what tools/qam_sim.c uses. */
static const struct chan bench = {
	.cfo_hz = 25e3, .pn_rad = 0.002, .dc = 6, .gain = 0.4, .isi = {0.1, 0.25, 1.0}};
/* Everything but noise off. */
static const struct chan ideal = {.gain = 1.0, .isi = {0.0, 0.0, 1.0}};

static struct qam_tx tx;
static struct qam_rx rx, rx2;
static uint8_t cw1[QAM_RX_CW_BYTES], cw2[QAM_RX_CW_BYTES];
static uint32_t frame[QAM_FRAME_MAX_SAMPLES], win[WIN];
static uint8_t data[QAM_NCW_MAX * QAM_UNIT], data_b[QAM_NCW_MAX * QAM_UNIT];
static uint8_t got[QAM_NCW_MAX * QAM_UNIT];
static double xi[WIN], xq[WIN];

static void before_each(void *f)
{
	ARG_UNUSED(f);
	test_rng_seed(RNG_SEED);
	memset(&rx, 0, sizeof(rx));
	memset(&rx2, 0, sizeof(rx2));
	rx.cw = cw1;
	rx2.cw = cw2;
}

static void *suite_setup(void)
{
	rs_init();
	ham_init();
	return NULL;
}

ZTEST_SUITE(link_qam, NULL, suite_setup, before_each, NULL, NULL);

/* Loop the frame, cut WIN samples at a random offset, apply the channel (transmit words in,
 * receive words out: I in bits 9:0 becomes I in bits 19:10).
 */
static void channel(const uint32_t *f, size_t len, const struct chan *c, double snr_db)
{
	size_t off = test_rng_u32() % len;
	double ph = ESP_SDR_TAU * test_rng_uniform(), sig = 0.0, prev[2][2] = {{0}};
	double fr = c->cfo_hz / 80e6, s;

	for (size_t n = 0; n < WIN; n++) {
		uint32_t w = f[(off + n) % len];
		double i = (double)((int32_t)(w << 22) >> 22), q = (double)((int32_t)(w << 12) >> 22);
		double yi = c->isi[0] * prev[1][0] + c->isi[1] * prev[0][0] + c->isi[2] * i;
		double yq = c->isi[0] * prev[1][1] + c->isi[1] * prev[0][1] + c->isi[2] * q;

		prev[1][0] = prev[0][0];
		prev[1][1] = prev[0][1];
		prev[0][0] = i;
		prev[0][1] = q;
		ph += ESP_SDR_TAU * fr + c->pn_rad * test_rng_gauss();
		xi[n] = c->gain * (yi * cos(ph) - yq * sin(ph));
		xq[n] = c->gain * (yi * sin(ph) + yq * cos(ph));
		sig += xi[n] * xi[n] + xq[n] * xq[n];
	}
	s = sqrt(sig / WIN / 2.0 / pow(10.0, snr_db / 10.0));
	for (size_t n = 0; n < WIN; n++) {
		long vi = lround(xi[n] + c->dc + s * test_rng_gauss());
		long vq = lround(xq[n] - c->dc + s * test_rng_gauss());

		vi = CLAMP(vi, -512, 511);
		vq = CLAMP(vq, -512, 511);
		win[n] = ((uint32_t)vq & 0x3ffU) | (((uint32_t)vi & 0x3ffU) << 10);
	}
}

static void fill(uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		p[i] = (uint8_t)test_rng_u32();
	}
}

/* Frames sent and decoded at one setting; returns the frames that came back intact. */
static int trials(enum qam_mod mod, enum qam_fec fec, unsigned int ncw, const struct chan *c,
		  double snr_db, int n)
{
	struct qam_hdr h = {.mod = mod, .fec = fec, .ncw = ncw, .full = ncw == 0U,
			    .dst = 2, .src = 1};
	size_t bytes = qam_hdr_payload_bytes(&h);
	int ok = 0;

	zassert_true(bytes > 0, "mod %d fec %d: no payload", mod, fec);
	qam_tx_init(&tx, mod, 150);
	for (int t = 0; t < n; t++) {
		struct qam_hdr rh;
		struct qam_rx_info info;
		size_t len;
		int ret;

		h.seq = (uint16_t)test_rng_u32();
		fill(data, bytes);
		len = qam_tx_build(&tx, &h, data, frame);
		zassert_equal(len, qam_hdr_frame_samples(&h), "mod %d fec %d", mod, fec);
		channel(frame, len, c, snr_db);
		ret = qam_rx_decode(&rx, win, WIN, &rh, &info);
		if (ret != 0) {
			continue;
		}
		qam_rx_data(&rx, &rh, got);
		ok += rh.seq == h.seq && rh.mod == h.mod && rh.fec == h.fec && rh.dst == 2 &&
		      rh.src == 1 && memcmp(got, data, bytes) == 0;
	}
	return ok;
}

ZTEST(link_qam, test_crc_check_values)
{
	static const uint8_t check[] = "123456789";

	/* CRC-16/CCITT-FALSE and CRC-32 (IEEE) check values. */
	zassert_equal(qam_crc16(check, 9), 0x29b1, "crc16 0x%04x", qam_crc16(check, 9));
	zassert_equal(qam_crc32(check, 9), 0xcbf43926U, "crc32 0x%08x", qam_crc32(check, 9));
	zassert_equal(qam_crc32(check, 0), 0U);
}

ZTEST(link_qam, test_bookkeeping)
{
	zassert_equal(qam_unit_coded(QAM_FEC_RS), QAM_UNIT);
	zassert_equal(qam_unit_data(QAM_FEC_RS), QAM_UNIT - RS_NROOTS);
	zassert_equal(qam_unit_data(QAM_FEC_HAMMING), 225U);
	zassert_equal(qam_unit_coded(QAM_FEC_HAMMING), 240U);
	zassert_equal(qam_unit_data(QAM_FEC_NONE), QAM_UNIT);
	for (int mod = 0; mod < QAM_MODS; mod++) {
		zassert_equal(qam_bits((enum qam_mod)mod), 2U * (unsigned int)(mod + 1));
		for (int fec = 0; fec < QAM_FECS; fec++) {
			unsigned int max = qam_ncw_max((enum qam_mod)mod, (enum qam_fec)fec);
			size_t prev = 0;

			zassert_true(max >= 1U && max <= QAM_NCW_MAX, "mod %d fec %d: %u", mod,
				     fec, max);
			for (unsigned int ncw = 1; ncw <= max; ncw++) {
				size_t len = qam_frame_samples((enum qam_mod)mod,
							       (enum qam_fec)fec, ncw);

				zassert_true(len > prev && len <= QAM_FRAME_MAX_SAMPLES,
					     "mod %d fec %d ncw %u: %zu", mod, fec, ncw, len);
				prev = len;
			}
			if (max < QAM_NCW_MAX) {
				zassert_equal(qam_frame_samples((enum qam_mod)mod, (enum qam_fec)fec,
								max + 1U),
					      0U);
			}
			zassert_equal(qam_payload_bytes((enum qam_fec)fec, max),
				      max * qam_unit_data((enum qam_fec)fec) - QAM_CRC_BYTES);
			zassert_true(qam_full_payload_bytes((enum qam_mod)mod, (enum qam_fec)fec) >=
					     qam_payload_bytes((enum qam_fec)fec, max),
				     "mod %d fec %d", mod, fec);
		}
	}
	zassert_equal(qam_payload_bytes(QAM_FEC_RS, 0), 0U);
}

ZTEST(link_qam, test_bad_headers)
{
	struct qam_hdr h = {.mod = QAM_QAM16, .fec = QAM_FEC_RS, .ncw = 1};

	qam_tx_init(&tx, QAM_QPSK, 150);
	/* The modulation must match qam_tx_init(). */
	zassert_equal(qam_tx_build(&tx, &h, data, frame), 0U);
	h.mod = QAM_QPSK;
	h.ncw = QAM_NCW_MAX + 1U;
	zassert_equal(qam_tx_build(&tx, &h, data, frame), 0U);
	zassert_equal(qam_hdr_frame_samples(&h), 0U);
	h.ncw = 1;
	h.fec = QAM_FECS;
	zassert_equal(qam_hdr_frame_samples(&h), 0U);
	zassert_equal(qam_hdr_payload_bytes(&h), 0U);
}

ZTEST(link_qam, test_noise_only)
{
	struct qam_hdr rh;
	struct qam_rx_info info;

	for (size_t n = 0; n < WIN; n++) {
		long vi = lround(40.0 * test_rng_gauss()), vq = lround(40.0 * test_rng_gauss());

		win[n] = ((uint32_t)vq & 0x3ffU) | (((uint32_t)vi & 0x3ffU) << 10);
	}
	zassert_equal(qam_rx_decode(&rx, win, WIN, &rh, &info), QAM_RX_NONE);
	/* Mean power of the noise, about 2 x 40^2. */
	zassert_within(qam_rx_power(win, WIN), 3200.0f, 200.0f);
}

ZTEST(link_qam, test_rs_bench_channel)
{
	zassert_equal(trials(QAM_QPSK, QAM_FEC_RS, 0, &bench, 16.0, 4), 4);
	zassert_equal(trials(QAM_QAM16, QAM_FEC_RS, 0, &bench, 28.0, 4), 4);
	zassert_equal(trials(QAM_QAM64, QAM_FEC_RS, 0, &bench, 36.0, 4), 4);
}

ZTEST(link_qam, test_256qam_ideal_channel)
{
	zassert_equal(trials(QAM_QAM256, QAM_FEC_RS, 0, &ideal, 45.0, 3), 3);
}

ZTEST(link_qam, test_other_fecs)
{
	zassert_equal(trials(QAM_QAM16, QAM_FEC_HAMMING, 0, &bench, 30.0, 3), 3);
	zassert_equal(trials(QAM_QAM16, QAM_FEC_NONE, 0, &bench, 34.0, 3), 3);
	/* A short frame of whole units. */
	zassert_equal(trials(QAM_QPSK, QAM_FEC_RS, 2, &bench, 20.0, 3), 3);
}

ZTEST(link_qam, test_too_noisy)
{
	/* Far below where 64-QAM decodes: the payload fails its RS or CRC, never wrong data. */
	struct qam_hdr h = {.mod = QAM_QAM64, .fec = QAM_FEC_RS, .full = 1, .dst = 2, .src = 1};
	struct qam_hdr rh;
	struct qam_rx_info info;
	size_t len;

	qam_tx_init(&tx, QAM_QAM64, 150);
	fill(data, qam_hdr_payload_bytes(&h));
	len = qam_tx_build(&tx, &h, data, frame);
	channel(frame, len, &bench, 14.0);
	zassert_equal(qam_rx_decode(&rx, win, WIN, &rh, &info), QAM_RX_DATA);
	zassert_true(info.failed > 0, "failed units %d", info.failed);
}

ZTEST(link_qam, test_pair)
{
	struct qam_hdr ha = {.mod = QAM_QAM16, .fec = QAM_FEC_RS, .full = 1, .pair = 1, .seq = 10};
	struct qam_hdr hb = ha, rh, rh2;
	struct qam_rx_info info, info2;
	size_t bytes = qam_hdr_payload_bytes(&ha), total;
	int other;

	hb.seq = 11;
	qam_tx_init(&tx, QAM_QAM16, 150);
	fill(data, bytes);
	fill(data_b, bytes);
	total = qam_tx_build_pair(&tx, &ha, data, &hb, data_b, frame);
	zassert_true(total > 0 && total <= QAM_FRAME_MAX_SAMPLES, "total %zu", total);
	channel(frame, total, &bench, 30.0);
	zassert_equal(qam_rx_begin(&rx, win, WIN, &rh, &info), 0);
	zassert_equal(qam_rx_finish(&rx, &rh, &info), 0);
	zassert_true(rh.pair && (rh.seq == 10 || rh.seq == 11), "seq %u", rh.seq);
	qam_rx_data(&rx, &rh, got);
	zassert_mem_equal(got, rh.seq == 10 ? data : data_b, bytes);
	/* The other frame starts one frame length later (or earlier), as tools/qam_sim.c finds it. */
	other = info.start + (int)(total / 2U);
	if (other + QAM_SEARCH_MARGIN > WIN) {
		other -= (int)total;
	}
	if (other < 0) {
		other = info.start - (int)(total / 2U);
	}
	zassert_equal(qam_rx_begin_at(&rx2, win, WIN, other, info.cfo_hz, &rh2, &info2), 0);
	zassert_equal(qam_rx_finish(&rx2, &rh2, &info2), 0);
	zassert_equal(rh2.seq, rh.seq ^ 1U);
	qam_rx_data(&rx2, &rh2, got);
	zassert_mem_equal(got, rh2.seq == 10 ? data : data_b, bytes);
}
