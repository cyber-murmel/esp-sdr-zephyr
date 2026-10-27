/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Small helpers the decoders share (decode.h): binary angles from int16 I/Q, dB scales. Plain
 * C, header only.
 */

#ifndef DEC_UTIL_H_
#define DEC_UTIL_H_

#include <math.h>
#include <stdint.h>

/* A full-scale carrier (512 raw units) in the channelizer's output: cfg.gain 32 times. */
#define DEC_FULL_SCALE 16384.0f

/*
 * atan2(im, re) as a binary angle, -32768 .. 32767 for -pi .. pi (65536 a turn), within 0.25
 * degrees; apps/micropython/cmod/esdr_fsk.c's esdr_fsk_angle(), inline for the decoders.
 */
static inline int32_t dec_angle(int32_t re, int32_t im)
{
	uint32_t ax = re >= 0 ? (uint32_t)re : -(uint32_t)re;
	uint32_t ay = im >= 0 ? (uint32_t)im : -(uint32_t)im;
	uint32_t mx = ax >= ay ? ax : ay, mn = ax >= ay ? ay : ax;
	int32_t a;

	if (mx == 0U) {
		return 0;
	}
	while (mx >= (1U << 19)) {
		mx >>= 4;
		mn >>= 4;
	}
	while (mx >= (1U << 15)) {
		mx >>= 1;
		mn >>= 1;
	}
	int32_t r = (int32_t)((mn << 15) / mx);

	a = (r >> 2) + ((((r * (32768 - r)) >> 15) * 2847) >> 15);
	if (ay > ax) {
		a = 16384 - a;
	}
	if (re < 0) {
		a = 32768 - a;
	}
	if (im < 0) {
		a = -a;
	}
	return a;
}

/*
 * The phase step from sample a to sample b (int16 I, Q): the angle of b conj(a), a binary angle.
 * The inputs are halved so that the products stay within 32 bits.
 */
static inline int32_t dec_step(int16_t ai, int16_t aq, int16_t bi, int16_t bq)
{
	int32_t pi = ai >> 1, pq = aq >> 1, i = bi >> 1, q = bq >> 1;

	return dec_angle(i * pi + q * pq, q * pi - i * pq);
}

/* A binary angle a sample (or a symbol) as a frequency, Hz, at @p rate samples (symbols) a second. */
static inline float dec_angle_hz(float angle, float rate)
{
	return angle * rate / 65536.0f;
}

/* dB relative to the receiver's full scale, times 10, of a power in output units squared. */
static inline int16_t dec_dbfs_x10(float power)
{
	float db;

	if (power <= 0.0f) {
		return -32768;
	}
	db = 100.0f * log10f(power / (DEC_FULL_SCALE * DEC_FULL_SCALE));
	if (db < -32767.0f) {
		return -32767;
	}
	if (db > 32767.0f) {
		return 32767;
	}
	return (int16_t)lrintf(db);
}

#endif /* DEC_UTIL_H_ */
