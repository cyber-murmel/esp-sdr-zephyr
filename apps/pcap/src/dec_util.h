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
#include <string.h>

/* A full-scale carrier (512 raw units) in the channelizer's output: cfg.gain 32 times. */
#define DEC_FULL_SCALE 16384.0f

/*
 * The leading zeros of @p x, 1 .. 2^31: NSAU on the S3. RISC-V without Zbb (the P4's and S31's
 * rv32imafc) has no such instruction and __builtin_clz() calls __clzsi2() there: the exponent
 * of x as a float instead, one less where rounding carried it to the next power of 2.
 */
static inline int dec_clz(uint32_t x)
{
#if defined(__riscv) && !defined(__riscv_zbb) && defined(__riscv_flen)
	const float f = (float)x;
	uint32_t u;
	int e;

	memcpy(&u, &f, sizeof(u));
	e = (int)(u >> 23) - 127;
	e -= (int)((x >> e) == 0U);
	return 31 - e;
#else
	return __builtin_clz(x);
#endif
}

/*
 * atan2(im, re) as a binary angle, -32768 .. 32768 for -pi .. pi (65536 a turn; +32768 at pi
 * itself, re < 0 and im == 0, which callers that store an int16, dec_ble.c's angles() and
 * dec_generic.c's wrap16(), make -32768, the same angle), within 0.25 degrees; the angle of
 * apps/micropython/cmod/esdr_fsk.c's esdr_fsk_angle(), inline for the decoders. The quotient on
 * RISC-V (the P4, the S31) from a reciprocal in a table with one correction, which beat divu
 * (the P4's Bluetooth LE decoder 16.5 against 20.0 ms on a fixed stream); on the S3 quou beat
 * the table (21.1 against 23.3 ms).
 */
static inline int32_t dec_angle(int32_t re, int32_t im)
{
#if defined(__riscv)
	/*
	 * 2^31 / (2^14 + 64 i) rounded up in bits 9 .. 26, its step down to entry i + 1 in bits
	 * 0 .. 8: the reciprocal of a 15-bit mx from its top 8 bits, interpolated linearly, never
	 * below 1 / mx (the chord of a convex curve): the quotient comes out right or one too large
	 * (every pair of 15-bit mx and mn <= mx checked).
	 */
	static const uint32_t recip[256] = {
		0x40001FE, 0x3FC05FA, 0x3F811F6, 0x3F425F2, 0x3F041EE, 0x3EC65EB, 0x3E88FE7,
		0x3E4C1E3, 0x3E0FBE0, 0x3DD3BDC, 0x3D983D8, 0x3D5D3D5, 0x3D229D2, 0x3CE85CE,
		0x3CAE9CA, 0x3C755C8, 0x3C3C5C3, 0x3C03FC1, 0x3BCBDBD, 0x3B943BA, 0x3B5CFB7,
		0x3B261B4, 0x3AEF9B1, 0x3AB97AD, 0x3A83DAB, 0x3A4E7A7, 0x3A199A5, 0x39E4FA1,
		0x39B0D9F, 0x397CF9B, 0x3949999, 0x3916796, 0x38E3B93, 0x38B1591, 0x387F38D,
		0x384D98B, 0x381C388, 0x37EB386, 0x37BA783, 0x378A180, 0x375A17E, 0x372A57B,
		0x36FAF78, 0x36CBF76, 0x369D374, 0x366EB71, 0x364096F, 0x3612B6C, 0x35E536A,
		0x35B7F67, 0x358B166, 0x355E563, 0x3531F60, 0x3505F5E, 0x34DA35C, 0x34AEB5A,
		0x3483758, 0x3458755, 0x342DD53, 0x3403752, 0x33D934F, 0x33AF54C, 0x3385D4B,
		0x335C749, 0x3333547, 0x330A744, 0x32E1F43, 0x32B9940, 0x329193F, 0x3269B3D,
		0x324213B, 0x321AB38, 0x31F3B37, 0x31CCD35, 0x31A6334, 0x317FB31, 0x315992F,
		0x3133B2E, 0x310DF2C, 0x30E872A, 0x30C3328, 0x309E327, 0x3079525, 0x3054B23,
		0x3030521, 0x300C320, 0x2FE831E, 0x2FC471D, 0x2FA0D1A, 0x2F7D919, 0x2F5A718,
		0x2F37716, 0x2F14B14, 0x2EF2313, 0x2ECFD11, 0x2EADB0F, 0x2E8BD0E, 0x2E6A10D,
		0x2E4870B, 0x2E27109, 0x2E05F08, 0x2DE4F07, 0x2DC4105, 0x2DA3704, 0x2D82F02,
		0x2D62B01, 0x2D428FF, 0x2D22AFE, 0x2D02EFD, 0x2CE34FB, 0x2CC3EFA, 0x2CA4AF8,
		0x2C85AF7, 0x2C66CF6, 0x2C480F4, 0x2C298F3, 0x2C0B2F2, 0x2BECEF1, 0x2BCECEF,
		0x2BB0EEE, 0x2B932ED, 0x2B758EB, 0x2B582EA, 0x2B3AEE9, 0x2B1DCE8, 0x2B00CE7,
		0x2AE3EE5, 0x2AC74E4, 0x2AAACE3, 0x2A8E6E2, 0x2A722E1, 0x2A560DF, 0x2A3A2DE,
		0x2A1E6DD, 0x2A02CDD, 0x29E72DA, 0x29CBEDA, 0x29B0AD9, 0x29958D8, 0x297A8D6,
		0x295FCD6, 0x29450D4, 0x292A8D3, 0x29102D2, 0x28F5ED2, 0x28DBAD0, 0x28C1ACF,
		0x28A7CCE, 0x288E0CD, 0x28746CC, 0x285AECB, 0x28418CA, 0x28284C9, 0x280F2C8,
		0x27F62C8, 0x27DD2C6, 0x27C46C5, 0x27ABCC4, 0x27934C3, 0x277AEC3, 0x27628C1,
		0x274A6C1, 0x27324BF, 0x271A6BF, 0x27028BE, 0x26EACBD, 0x26D32BC, 0x26BBABB,
		0x26A44BA, 0x268D0B9, 0x2675EB9, 0x265ECB7, 0x2647EB7, 0x26310B6, 0x261A4B5,
		0x2603AB4, 0x25ED2B4, 0x25D6AB2, 0x25C06B2, 0x25AA2B1, 0x25940B0, 0x257E0AF,
		0x25682AF, 0x25524AD, 0x253CAAD, 0x25270AC, 0x25118AC, 0x24FC0AA, 0x24E6CAA,
		0x24D18A9, 0x24BC6A9, 0x24A74A7, 0x24926A7, 0x247D8A6, 0x2468CA5, 0x24542A5,
		0x243F8A4, 0x242B0A3, 0x2416AA2, 0x24026A2, 0x23EE2A1, 0x23DA0A1, 0x23C5E9F,
		0x23B209F, 0x239E29E, 0x238A69E, 0x2376A9D, 0x236309C, 0x234F89B, 0x233C29B,
		0x2328C9A, 0x231589A, 0x2302499, 0x22EF298, 0x22DC298, 0x22C9297, 0x22B6496,
		0x22A3896, 0x2290C95, 0x227E294, 0x226BA94, 0x2259293, 0x2246C92, 0x2234892,
		0x2222492, 0x2210090, 0x21FE091, 0x21EBE8F, 0x21DA08F, 0x21C828E, 0x21B668E,
		0x21A4A8D, 0x219308D, 0x218168C, 0x216FE8C, 0x215E68A, 0x214D28B, 0x213BC8A,
		0x212A889, 0x2119688, 0x2108689, 0x20F7487, 0x20E6687, 0x20D5887, 0x20C4A86,
		0x20B3E85, 0x20A3485, 0x2092A84, 0x2082284, 0x2071A83, 0x2061483, 0x2050E82,
		0x2040A82, 0x2030681, 0x2020481, 0x2010281,
	};
#endif
	uint32_t ax = re >= 0 ? (uint32_t)re : -(uint32_t)re;
	uint32_t ay = im >= 0 ? (uint32_t)im : -(uint32_t)im;
	uint32_t mx = ax >= ay ? ax : ay, mn = ax >= ay ? ay : ax;
	int32_t a;

	if (mx == 0U) {
		return 0;
	}
	/*
	 * mx's top one to bit 31 (mn <= mx loses nothing), then both to 15 bits: what mx had beyond
	 * 15 bits cut off, a smaller pair shifted up (the same quotient).
	 */
	const int z = dec_clz(mx);
	const uint32_t x = mx << z, y = mn << z;
	const uint32_t m15 = x >> 17, n15 = y >> 17;
#if defined(__riscv)
	/* (n15 << 15) / m15: n15 times the reciprocal (2^31 / m15, at most 2^17), corrected once. */
	const uint32_t e = recip[(x >> 23) & 255U];
	const uint32_t rcp = (e >> 9) - (((e & 511U) * ((x >> 17) & 63U)) >> 6);
	uint32_t q = (n15 * rcp) >> 16;

	q -= (uint32_t)(q * m15 > (n15 << 15));
	int32_t r = (int32_t)q;
#else
	int32_t r = (int32_t)((n15 << 15) / m15);
#endif

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
