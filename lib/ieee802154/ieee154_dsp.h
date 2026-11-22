/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Signal processing of the esp_sdr 802.15.4 driver, portable C so the unit
 * test (tests/unit/ieee154_dsp) runs the same code: the transmit
 * waveform from slot templates at the DAC rate, and the receive mix and
 * decimation of 16 MS/s capture words to the PHY's 4 MS/s.
 */

#ifndef IEEE154_DSP_H_
#define IEEE154_DSP_H_

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#include <esp_sdr/esp_sdr_dsp.h>
#include <esp_sdr/esp_sdr_rx.h>
#include <esp_sdr/esp_sdr_tx.h>
#include <esp_sdr/ieee154_phy.h>

/* The LO sits this far below the channel: its leakage and the DC offset stay out of the band. */
#define IEEE154_DSP_LO_OFFSET_MHZ 4U

/* ------------------------------------------------------------- transmit */

/* DAC rate 40 MS/s: one chip period (0.5 us) is one slot. */
#define IEEE154_DSP_SLOT      20U
#define IEEE154_DSP_CHIPS_MAX ((6U + IEEE154_PHY_PSDU_MAX) * 2U * 32U)

/*
 * Slot s carries the first half of chip s's half-sine pulse and the second
 * half of chip s - 1's, on I for even chips and Q for odd ones (O-QPSK). The
 * offset rotation (period 10 samples at 40 MS/s) restarts every slot, so 2
 * parities x 3 x 3 chip states (none, 0, 1) cover every slot.
 */
typedef uint32_t ieee154_dsp_tmpl_t[2][3][3][IEEE154_DSP_SLOT] __aligned(16);

struct ieee154_dsp_tx {
	uint32_t chips[IEEE154_DSP_CHIPS_MAX / 32U];
	uint32_t nchips;
	const ieee154_dsp_tmpl_t *tmpl;
};

/*
 * A waveform generator: DAC words [index, index + n) of the frame in @p user (a struct
 * ieee154_dsp_tx) into dst, the shape of esp_sdr_ring_gen_t and esp_sdr_tx_dac_gen_t. The type of
 * the driver's hot-swap stage tx_gen (lib/ieee802154/ieee154_esp_sdr.c; its kernels are in
 * apps/wpan/kernels, which share this header: it is the stage's ABI).
 */
typedef void (*ieee154_dsp_tx_gen_fn_t)(void *user, uint32_t *dst, uint64_t index, uint32_t n);

/* DAC words of every slot kind at amplitude amp (of 511); conj mirrors the spectrum. */
static inline void ieee154_dsp_tmpl_init(ieee154_dsp_tmpl_t *tmpl, float amp, bool conj)
{
	for (int p = 0; p < 2; p++) {
		for (int c = 0; c < 3; c++) {
			for (int v = 0; v < 3; v++) {
				for (unsigned int t = 0; t < IEEE154_DSP_SLOT; t++) {
					float cur = c == 0 ? 0.0f : (c == 2 ? 1.0f : -1.0f);
					float prv = v == 0 ? 0.0f : (v == 2 ? 1.0f : -1.0f);
					float x = ESP_SDR_TAU_F * (float)t;
					float y = ESP_SDR_TAU_F * (float)(t + 20U);
					float a = cur * sinf(x / 80.0f);
					float b = prv * sinf(y / 80.0f);
					float i = p == 0 ? a : b, q = p == 0 ? b : a;
					float th = ESP_SDR_TAU_F *
						   (float)IEEE154_DSP_LO_OFFSET_MHZ / 40.0f *
						   (float)t;
					float ri = amp * (i * cosf(th) - q * sinf(th));
					float rq = amp * (i * sinf(th) + q * cosf(th));

					(*tmpl)[p][c][v][t] = esp_sdr_tx_word(
						(int16_t)lrintf(ri), (int16_t)lrintf(conj ? -rq : rq));
				}
			}
		}
	}
}

/* Chips of SHR, PHR and psdu (FCS included), symbols low nibble first. */
static inline void ieee154_dsp_tx_build(struct ieee154_dsp_tx *tx, const uint8_t *psdu,
					uint8_t len)
{
	uint32_t nbytes = 6U + len;

	for (uint32_t b = 0; b < nbytes; b++) {
		uint8_t v = b < 4U ? 0x00U : (b == 4U ? 0xa7U : (b == 5U ? len : psdu[b - 6U]));

		tx->chips[2U * b] = ieee154_phy_chips(v & 0xf);
		tx->chips[2U * b + 1U] = ieee154_phy_chips(v >> 4);
	}
	tx->nchips = nbytes * 64U;
}

/* DAC samples of the built frame, including the last chip's second half. */
static inline uint64_t ieee154_dsp_tx_samples(const struct ieee154_dsp_tx *tx)
{
	return (uint64_t)(tx->nchips + 1U) * IEEE154_DSP_SLOT;
}

/* State of chip n: 0 none, 1 for a 0 chip, 2 for a 1 chip. Inlined: IRAM callers must not call out. */
static inline __attribute__((always_inline)) int ieee154_dsp_chip(const struct ieee154_dsp_tx *tx,
								  uint32_t n)
{
	if (n >= tx->nchips) {
		return 0;
	}
	return 1 + (int)((tx->chips[n / 32U] >> (31U - n % 32U)) & 1U);
}

#if defined(CONFIG_SOC_SERIES_ESP32S3)
/* PS.INTLEVEL: nonzero while interrupts are masked (arch_irq_lock()). */
#define IEEE154_DSP_PS_INTLEVEL 0xfU
/* CPENABLE bit of the vector unit (coprocessor 3). */
#define IEEE154_DSP_CPENABLE_PIE BIT(3)

/*
 * Whether the slot copy may use the vector unit: the caller (the ring, the DAC filler) enabled
 * it on this CPU and holds interrupts off, as nothing saves its registers. Any other caller
 * (a thread, the ACK precompute) takes the scalar copy.
 */
static inline bool ieee154_dsp_vec_ok(void)
{
	uint32_t cp, ps;

	__asm__ volatile("rsr.cpenable %0" : "=r"(cp));
	__asm__ volatile("rsr.ps %0" : "=r"(ps));
	return (cp & IEEE154_DSP_CPENABLE_PIE) != 0U && (ps & IEEE154_DSP_PS_INTLEVEL) != 0U;
}

/* One 80 byte slot, both pointers 16 byte aligned: five 128-bit loads and stores (q0 to q3). */
static inline __attribute__((always_inline)) void ieee154_dsp_copy_slot_vec(uint32_t *dst,
									    const uint32_t *src)
{
	__asm__ volatile("ee.vld.128.ip q0, %[s], 16\n\t"
			 "ee.vld.128.ip q1, %[s], 16\n\t"
			 "ee.vld.128.ip q2, %[s], 16\n\t"
			 "ee.vst.128.ip q0, %[d], 16\n\t"
			 "ee.vld.128.ip q3, %[s], 16\n\t"
			 "ee.vst.128.ip q1, %[d], 16\n\t"
			 "ee.vld.128.ip q0, %[s], 16\n\t"
			 "ee.vst.128.ip q2, %[d], 16\n\t"
			 "ee.vst.128.ip q3, %[d], 16\n\t"
			 "ee.vst.128.ip q0, %[d], 16\n\t"
			 : [s] "+r"(src), [d] "+r"(dst)
			 :
			 : "memory");
}
#else
static inline bool ieee154_dsp_vec_ok(void)
{
	return false;
}

static inline void ieee154_dsp_copy_slot_vec(uint32_t *dst, const uint32_t *src)
{
	ARG_UNUSED(dst);
	ARG_UNUSED(src);
}
#endif

/* One slot as groups of 4: plain word stores, a memcpy call per slot costs more than the copy. */
static inline __attribute__((always_inline)) void ieee154_dsp_copy_slot(uint32_t *dst,
									const uint32_t *src)
{
	for (uint32_t k = 0; k < IEEE154_DSP_SLOT; k += 4U) {
		uint32_t w0 = src[k], w1 = src[k + 1U], w2 = src[k + 2U], w3 = src[k + 3U];

		dst[k] = w0;
		dst[k + 1U] = w1;
		dst[k + 2U] = w2;
		dst[k + 3U] = w3;
	}
}

/*
 * DAC words for samples [index, index + n) of the frame (an esp_sdr_tx_dac_gen_t
 * body). Has to beat the DAC (40 MS/s) with room to spare.
 *
 * Whole slots that have a next chip run off a shift register of the chips: the next slot's
 * template is its parity, the next chip and this chip, added as word offsets (no divide, bounds
 * check or variable shift per slot). The slot goes in five 128-bit copies where the vector unit
 * is on and the addresses are aligned, else as scalar groups of 4. The first and last slot of a
 * chunk and the last slot of the frame (no next chip) are copied word by word. The words are
 * those of the per-slot chip lookup this replaced (tests/unit/ieee154_dsp).
 */
static inline void ieee154_dsp_tx_gen(const struct ieee154_dsp_tx *tx, uint32_t *dst,
				      uint64_t index, uint32_t n)
{
	/* Word strides in the template array: parity, chip, previous chip. */
	enum { PAR = 9 * IEEE154_DSP_SLOT, CUR = 3 * IEEE154_DSP_SLOT, PRV = IEEE154_DSP_SLOT };
	const uint32_t *base = &(*tx->tmpl)[0][0][0][0];
	const uint32_t nchips = tx->nchips;
	uint32_t s = (uint32_t)(index / IEEE154_DSP_SLOT);
	uint32_t o = (uint32_t)(index % IEEE154_DSP_SLOT);
	const bool vec = ieee154_dsp_vec_ok() && (((uintptr_t)dst | (uintptr_t)base) & 15U) == 0U;

	while (n > 0U) {
		uint32_t run = n / IEEE154_DSP_SLOT;
		/* Chip states: 0 none (past the end), 1, 2. */
		const uint32_t cur = (uint32_t)ieee154_dsp_chip(tx, s);
		const uint32_t prv = s == 0U ? 0U : (uint32_t)ieee154_dsp_chip(tx, s - 1U);

		if (o != 0U || run == 0U || s + 1U >= nchips) {
			const uint32_t *src = base + (s & 1U) * PAR + cur * CUR + prv * PRV;
			uint32_t cnt = MIN(n, IEEE154_DSP_SLOT - o);

			for (uint32_t k = 0; k < cnt; k++) {
				dst[k] = src[o + k];
			}
			dst += cnt;
			n -= cnt;
			o = 0U;
			s++;
			continue;
		}
		/* Slots s .. s + run - 1, each followed by an existing chip. */
		run = MIN(run, nchips - 1U - s);
		n -= run * IEEE154_DSP_SLOT;

		/* c20 and c60: cur at both strides; p20: prv. */
		uint32_t par = (s & 1U) != 0U ? PAR : 0U;
		uint32_t c20 = cur * PRV;
		uint32_t c60 = cur * CUR;
		uint32_t p20 = prv * PRV;
		uint32_t c = s + 1U;

		s += run;
		while (run > 0U) {
			/* The chips from c on, as many as this word of them holds. */
			uint32_t w = tx->chips[c / 32U] << (c % 32U);
			uint32_t k = MIN(run, 32U - c % 32U);

			run -= k;
			c += k;
			do {
				const uint32_t m = 0U - (w >> 31);
				const uint32_t n20 = PRV + (PRV & m), n60 = CUR + (CUR & m);

				if (vec) {
					ieee154_dsp_copy_slot_vec(dst, base + par + c60 + p20);
				} else {
					ieee154_dsp_copy_slot(dst, base + par + c60 + p20);
				}
				dst += IEEE154_DSP_SLOT;
				w <<= 1;
				/* Parity flips, this chip becomes the previous one. */
				par = PAR - par;
				p20 = c20;
				c20 = n20;
				c60 = n60;
			} while (--k > 0U);
		}
	}
}

/* -------------------------------------------------------------- receive */

#define IEEE154_DSP_DECIM 4U
#define IEEE154_DSP_NTAPS 27U

/* Outputs of ieee154_dsp_mix_decimate() for n input words. */
#define IEEE154_DSP_DECIM_OUT(n) (((n) - IEEE154_DSP_NTAPS) / IEEE154_DSP_DECIM + 1U)

/* Hamming windowed sinc, cutoff 1.6 MHz at 16 MS/s, Q14: -0.8 dB at 1 MHz, -51 dB from 3 MHz. */
static const int16_t ieee154_dsp_taps[IEEE154_DSP_NTAPS] = {
	30, 38, 37, 0, -95, -233, -342, -303, 0, 612, 1457, 2342, 3015, 3266,
	3015, 2342, 1457, 612, 0, -303, -342, -233, -95, 0, 37, 38, 30,
};

/*
 * 16 MS/s receive words to interleaved I/Q at 4 MS/s, +-8192 full scale:
 * output k = sum over t of taps[t] x[4k + t] (-j)^(4k + t). qsign -1
 * conjugates the result. Returns the output count.
 *
 * Every word lies under the taps of about 7 outputs: unpacked tap by tap,
 * each was unpacked 7 times (4 operations each). So a first pass turns each
 * word in place into its mixed I and Q, two int16 (I in the low half), with
 * the mix by -fs/4 as swaps and signs; then the taps, symmetric, take the
 * sum of two mirrored samples a multiply, and the zero taps none. The same
 * products in integers: the result is the tap-by-tap one exactly
 * (tests/unit/ieee154_dsp). Overwrites w.
 */
static inline int16_t *ieee154_dsp_premix(uint32_t *w, size_t n)
{
	size_t m = 0;

	for (; m + 4U <= n; m += 4U) {
		const int32_t i0 = esp_sdr_rx_i(w[m]), q0 = esp_sdr_rx_q(w[m]);
		const int32_t i1 = esp_sdr_rx_i(w[m + 1U]), q1 = esp_sdr_rx_q(w[m + 1U]);
		const int32_t i2 = esp_sdr_rx_i(w[m + 2U]), q2 = esp_sdr_rx_q(w[m + 2U]);
		const int32_t i3 = esp_sdr_rx_i(w[m + 3U]), q3 = esp_sdr_rx_q(w[m + 3U]);

		/* (-j)^n: 1, -j, -1, j. */
		w[m] = (uint16_t)i0 | ((uint32_t)(uint16_t)q0 << 16);
		w[m + 1U] = (uint16_t)q1 | ((uint32_t)(uint16_t)-i1 << 16);
		w[m + 2U] = (uint16_t)-i2 | ((uint32_t)(uint16_t)-q2 << 16);
		w[m + 3U] = (uint16_t)-q3 | ((uint32_t)(uint16_t)i3 << 16);
	}
	for (; m < n; m++) {
		const int32_t i = esp_sdr_rx_i(w[m]), q = esp_sdr_rx_q(w[m]);
		static const int8_t ci[4] = {1, 0, -1, 0}, cq[4] = {0, 1, 0, -1};
		const int32_t a = ci[m & 3U] * i + cq[m & 3U] * q;
		const int32_t b = ci[m & 3U] * q - cq[m & 3U] * i;

		w[m] = (uint16_t)a | ((uint32_t)(uint16_t)b << 16);
	}
	/* The build does not assume strict aliasing (Zephyr's -fno-strict-aliasing). */
	return (int16_t *)w;
}

static inline size_t ieee154_dsp_mix_decimate(uint32_t *w, size_t n, int16_t *out,
					      int32_t qsign)
{
	const int16_t *h = ieee154_dsp_taps;
	const size_t outs = IEEE154_DSP_DECIM_OUT(n);
	const int16_t *x = ieee154_dsp_premix(w, n);

	BUILD_ASSERT(IEEE154_DSP_NTAPS % 2U == 1U, "a middle tap");
	for (size_t k = 0; k < outs; k++) {
		const int16_t *p = &x[2U * IEEE154_DSP_DECIM * k];
		const size_t mid = IEEE154_DSP_NTAPS / 2U;
		int32_t ai = h[mid] * p[2U * mid], aq = h[mid] * p[2U * mid + 1U];

		for (size_t t = 0; t < mid; t++) {
			const size_t u = IEEE154_DSP_NTAPS - 1U - t;

			if (h[t] != 0) {
				ai += h[t] * (p[2U * t] + p[2U * u]);
				aq += h[t] * (p[2U * t + 1U] + p[2U * u + 1U]);
			}
		}
		/* 10-bit input, Q14 taps. */
		out[2 * k] = (int16_t)(ai >> 10);
		out[2 * k + 1] = (int16_t)(qsign * (aq >> 10));
	}
	return outs;
}

/* ----------------------------------------------------------- measurement */

/*
 * Carrier offset in Hz of an O-QPSK signal in 4 MS/s I/Q (2 samples per
 * chip): x[n] conj(x[n - 2]) turns by +-90 degrees per chip (MSK) plus
 * tau f 0.5 us; its square loses the sign, so the mean square points at
 * 180 degrees + 2 tau f 0.5 us. Uses samples above half the mean power (the
 * signal, when it fills most of the block); unambiguous within +-500 kHz.
 * Sets *power to the mean |x|^2 and *used to the samples counted.
 */
static inline float ieee154_dsp_cfo(const int16_t *iq, size_t n, float *power, size_t *used)
{
	double p = 0.0, mi = 0.0, mq = 0.0, sr = 0.0, si = 0.0;
	size_t cnt = 0, nm = 0;

	for (size_t k = 0; k < n; k++) {
		p += (double)iq[2 * k] * iq[2 * k] + (double)iq[2 * k + 1] * iq[2 * k + 1];
	}
	p = n != 0U ? p / (double)n : 0.0;
	/* Carrier feedthrough sits at the channel centre, 0 Hz here: remove the signal's mean. */
	for (size_t k = 0; k < n; k++) {
		double xi = iq[2 * k], xq = iq[2 * k + 1];

		if (xi * xi + xq * xq >= p / 2.0) {
			mi += xi;
			mq += xq;
			nm++;
		}
	}
	if (nm != 0U) {
		mi /= (double)nm;
		mq /= (double)nm;
	}
	for (size_t k = 2; k < n; k++) {
		double xi = iq[2 * k] - mi, xq = iq[2 * k + 1] - mq;
		double yi = iq[2 * k - 4] - mi, yq = iq[2 * k - 3] - mq;

		if ((double)iq[2 * k] * iq[2 * k] + (double)iq[2 * k + 1] * iq[2 * k + 1] < p / 2.0) {
			continue;
		}
		/* z = x conj(y), then z^2 */
		double zr = xi * yi + xq * yq, zi = xq * yi - xi * yq;

		sr += zr * zr - zi * zi;
		si += 2.0 * zr * zi;
		cnt++;
	}
	*power = (float)p;
	*used = cnt;
	if (cnt == 0U) {
		return 0.0f;
	}
	/* arg(-S) = 2 tau f T, T = 0.5 us */
	return (float)(atan2(-si, -sr) / (2.0 * ESP_SDR_TAU * 0.5e-6));
}

/*
 * Coarse carrier offset in Hz: the mean instantaneous frequency (phase step
 * per sample at 4 MS/s) of the samples above half the mean power, mean
 * removed. MSK's +-500 kHz deviation averages out over random chips; the
 * estimate is unambiguous within +-1.5 MHz but noisier than
 * ieee154_dsp_cfo().
 */
static inline float ieee154_dsp_freq(const int16_t *iq, size_t n)
{
	double p = 0.0, mi = 0.0, mq = 0.0, sum = 0.0;
	size_t cnt = 0, nm = 0;

	for (size_t k = 0; k < n; k++) {
		p += (double)iq[2 * k] * iq[2 * k] + (double)iq[2 * k + 1] * iq[2 * k + 1];
	}
	p = n != 0U ? p / (double)n : 0.0;
	for (size_t k = 0; k < n; k++) {
		double xi = iq[2 * k], xq = iq[2 * k + 1];

		if (xi * xi + xq * xq >= p / 2.0) {
			mi += xi;
			mq += xq;
			nm++;
		}
	}
	if (nm != 0U) {
		mi /= (double)nm;
		mq /= (double)nm;
	}
	for (size_t k = 1; k < n; k++) {
		double xi = iq[2 * k] - mi, xq = iq[2 * k + 1] - mq;
		double yi = iq[2 * k - 2] - mi, yq = iq[2 * k - 1] - mq;

		if ((double)iq[2 * k] * iq[2 * k] + (double)iq[2 * k + 1] * iq[2 * k + 1] < p / 2.0) {
			continue;
		}
		sum += atan2(xq * yi - xi * yq, xi * yi + xq * yq);
		cnt++;
	}
	/* One sample is 0.25 us. */
	return cnt != 0U ? (float)(sum / (double)cnt / (ESP_SDR_TAU * 0.25e-6)) : 0.0f;
}

/*
 * Histogram of the one-chip phase steps arg(x[n] conj(x[n - 2])) of the
 * samples above half the mean power, in 45 degree bins from -180: an
 * O-QPSK (MSK) signal fills the bins around -90 and +90: bins 1 or 2 and
 * 5 or 6, by the sign of the carrier offset.
 */
static inline void ieee154_dsp_steps(const int16_t *iq, size_t n, uint32_t hist[8])
{
	double p = 0.0;

	for (int b = 0; b < 8; b++) {
		hist[b] = 0U;
	}
	for (size_t k = 0; k < n; k++) {
		p += (double)iq[2 * k] * iq[2 * k] + (double)iq[2 * k + 1] * iq[2 * k + 1];
	}
	p = n != 0U ? p / (double)n : 0.0;
	for (size_t k = 2; k < n; k++) {
		double xi = iq[2 * k], xq = iq[2 * k + 1];
		double yi = iq[2 * k - 4], yq = iq[2 * k - 3];

		if (xi * xi + xq * xq < p / 2.0) {
			continue;
		}
		double a = atan2(xq * yi - xi * yq, xi * yi + xq * yq);
		int b = (int)((a + ESP_SDR_TAU / 2.0) / (ESP_SDR_TAU / 8.0));

		hist[b < 0 ? 0 : (b > 7 ? 7 : b)]++;
	}
}

#endif /* IEEE154_DSP_H_ */
