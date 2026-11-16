/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Signal processing of the diagnostic-bus ring (esp_sdr_diag.h), plain C on 32-bit integers so that
 * the same code runs in the unit test on the host.
 *
 * A ring byte is two signed nibbles, Q in bits 3:0 and I in bits 7:4, at 40 MS/s. box16() sums 16
 * bytes (a boxcar of 16, 2.5 MS/s out); the third-order CIC then decimates by R. With S = the sum of
 * 16 signed nibbles, the CIC output is S summed through three boxcars of R and scaled to
 * 16 * nibble * 2^lsb, that is raw 10-bit counts times 16, by esp_sdr_diag_norm().
 */

#ifndef ESP_SDR_DIAG_DSP_H_
#define ESP_SDR_DIAG_DSP_H_

#include <stddef.h>
#include <stdint.h>

/* Bytes per box16() output. */
#define DIAG_BOX 16U

/* Decimation limits of the CIC: R^3 must keep the 32-bit reciprocal of diag_cic_inv() in range. */
#define DIAG_CIC_R_MIN 8U
#define DIAG_CIC_R_MAX 255U

/*
 * Sum of the signed nibbles of 16 bytes (4 words): the low nibbles into *sq, the high ones into *si.
 * Flipping the top bit of each nibble turns the signed value v into v + 8 (0 to 15); four words add
 * up to at most 60 per byte lane, and the four lanes of the multiply by 0x01010101 to at most 240.
 */
static inline void diag_box16(const uint32_t *w, int32_t *si, int32_t *sq)
{
	const uint32_t m = 0x0f0f0f0fU, x = 0x88888888U;
	uint32_t a = w[0] ^ x, b = w[1] ^ x, c = w[2] ^ x, d = w[3] ^ x;
	uint32_t lo = (a & m) + (b & m) + (c & m) + (d & m);
	uint32_t hi = ((a >> 4) & m) + ((b >> 4) & m) + ((c >> 4) & m) + ((d >> 4) & m);

	*sq = (int32_t)((lo * 0x01010101U) >> 24) - 128;
	*si = (int32_t)((hi * 0x01010101U) >> 24) - 128;
}

struct diag_cic {
	/* Integrators, running at the input rate and wrapping at 32 bits on purpose. */
	uint32_t i1, i2, i3, q1, q2, q3;
	/* Comb delays, running at the output rate. */
	uint32_t di1, di2, di3, dq1, dq2, dq3;
	uint32_t r, phase;
	/* Outputs still to drop while the filter fills. */
	uint32_t settle;
};

static inline void diag_cic_init(struct diag_cic *c, uint32_t r)
{
	*c = (struct diag_cic){.r = r, .phase = 0, .settle = 3};
}

/*
 * Feed @p nbytes (a multiple of DIAG_BOX, 4-byte aligned) of the ring. Appends the CIC outputs as
 * interleaved I, Q to @p out (unscaled: the sum through the three boxcars, see diag_norm()) and
 * returns their number of pairs. The first three outputs after diag_cic_init() are dropped.
 *
 * The state lives in locals for the call: out is int32_t and the state uint32_t, which may alias,
 * so through *c every store to out reloaded it. The boxes up to the next output go in a loop of
 * their own, without the phase's test.
 */
static inline size_t diag_cic_run(struct diag_cic *c, const uint32_t *w, size_t nbytes, int32_t *out)
{
	size_t n = 0;
	uint32_t i1 = c->i1, i2 = c->i2, i3 = c->i3, q1 = c->q1, q2 = c->q2, q3 = c->q3;
	uint32_t di1 = c->di1, di2 = c->di2, di3 = c->di3, dq1 = c->dq1, dq2 = c->dq2,
		 dq3 = c->dq3;
	const uint32_t r = c->r;
	uint32_t phase = c->phase, settle = c->settle;
	const uint32_t *p = w;
	size_t boxes = nbytes / DIAG_BOX;

	while (boxes > 0U) {
		const uint32_t m = (uint32_t)(boxes < r - phase ? boxes : r - phase);

		for (uint32_t j = 0; j < m; j++, p += 4) {
			int32_t si, sq;

			diag_box16(p, &si, &sq);
			i1 += (uint32_t)si;
			i2 += i1;
			i3 += i2;
			q1 += (uint32_t)sq;
			q2 += q1;
			q3 += q2;
		}
		boxes -= m;
		phase += m;
		if (phase == r) {
			/* Three combs, x minus its previous value, three times. */
			const uint32_t ti = i3 - di1, tq = q3 - dq1;
			const uint32_t ui = ti - di2, uq = tq - dq2;

			phase = 0;
			di1 = i3;
			dq1 = q3;
			di2 = ti;
			dq2 = tq;
			if (settle != 0U) {
				settle--;
			} else {
				out[2U * n] = (int32_t)(ui - di3);
				out[2U * n + 1U] = (int32_t)(uq - dq3);
				n++;
			}
			di3 = ui;
			dq3 = uq;
		}
	}
	*c = (struct diag_cic){.i1 = i1, .i2 = i2, .i3 = i3, .q1 = q1, .q2 = q2, .q3 = q3,
			       .di1 = di1, .di2 = di2, .di3 = di3, .dq1 = dq1, .dq2 = dq2,
			       .dq3 = dq3, .r = r, .phase = phase, .settle = settle};
	return n;
}

/* Reciprocal for diag_norm(): 2^(32 + lsb) / R^3, rounded. Compute once per stream. */
static inline uint32_t diag_cic_inv(uint32_t r, unsigned int lsb)
{
	uint64_t r3 = (uint64_t)r * r * r;

	return (uint32_t)(((1ULL << (32U + lsb)) + r3 / 2U) / r3);
}

/* One CIC output scaled to raw 10-bit counts times 16: acc * 2^lsb / R^3. */
static inline int32_t diag_norm(int32_t acc, uint32_t inv)
{
	return (int32_t)(((int64_t)acc * (int64_t)inv) >> 32);
}

#endif /* ESP_SDR_DIAG_DSP_H_ */
