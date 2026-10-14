/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GFSK modulator for the transmit DAC, see gfsk_mod.h for the design.
 */

#include "gfsk_mod.h"

#include <string.h>

#include "ubt_attr.h"

_Static_assert(GFSK_LUT_BITS >= 5 && GFSK_LUT_BITS <= 10,
	       "the sine table is cut from a 1024 point grid");
_Static_assert(GFSK_MOD_SPS == 40, "the Gaussian table has 40 points per symbol");

/* Phase accumulators run in 1/65536 turn; the table index is the top GFSK_LUT_BITS of them. */
#define ACC_SHIFT (16U - GFSK_LUT_BITS)
#define ACC_BIAS  (1U << (ACC_SHIFT - 1U))
#define LUT_MASK  (GFSK_LUT_SIZE - 1U)

/* Points of the sine grid the ramp and the table are cut from. */
#define GRID      1024U
#define GRID_QUAD (GRID / 4U)

/*
 * Integrated frequency pulse of a Gaussian filter with BT 0.5 and a rectangle
 * of one symbol, over three symbols (40 points each) centred on the bit,
 * normalised to rise from 0 to 1: Q15, symmetric about the middle
 * (g[i] + g[120 - i] = 32768). Generated with erfc(), see tools/txcheck.c.
 */
static const uint16_t gauss[3 * GFSK_MOD_SPS + 1] = {
	0,     0,     0,     0,     1,     1,     1,     2,     3,     4,     6,     8,     11,
	15,    20,    27,    35,    46,    60,    77,    99,    126,   159,   200,   249,   308,
	378,   461,   559,   673,   805,   956,   1130,  1326,  1547,  1795,  2070,  2373,  2706,
	3070,  3464,  3889,  4345,  4830,  5346,  5890,  6461,  7058,  7680,  8325,  8991,  9676,
	10378, 11096, 11827, 12569, 13321, 14080, 14845, 15614, 16384, 17154, 17923, 18688, 19447,
	20199, 20941, 21672, 22390, 23092, 23777, 24443, 25088, 25710, 26307, 26878, 27422, 27938,
	28423, 28879, 29304, 29698, 30062, 30395, 30698, 30973, 31221, 31442, 31638, 31812, 31963,
	32095, 32209, 32307, 32390, 32460, 32519, 32568, 32609, 32642, 32669, 32691, 32708, 32722,
	32733, 32741, 32748, 32753, 32757, 32760, 32762, 32764, 32765, 32766, 32767, 32767, 32767,
	32768, 32768, 32768, 32768,
};

/* 32768 sin(pi/2 i/256), i = 0 to 256. */
static const uint16_t qsin[GRID_QUAD + 1U] = {
	0,     201,   402,   603,   804,   1005,  1206,  1407,  1608,  1809,  2009,  2210,  2411,
	2611,  2811,  3012,  3212,  3412,  3612,  3812,  4011,  4211,  4410,  4609,  4808,  5007,
	5205,  5404,  5602,  5800,  5998,  6195,  6393,  6590,  6787,  6983,  7180,  7376,  7571,
	7767,  7962,  8157,  8351,  8546,  8740,  8933,  9127,  9319,  9512,  9704,  9896,  10088,
	10279, 10469, 10660, 10850, 11039, 11228, 11417, 11605, 11793, 11980, 12167, 12354, 12540,
	12725, 12910, 13095, 13279, 13463, 13646, 13828, 14010, 14192, 14373, 14553, 14733, 14912,
	15091, 15269, 15447, 15624, 15800, 15976, 16151, 16326, 16500, 16673, 16846, 17018, 17190,
	17361, 17531, 17700, 17869, 18037, 18205, 18372, 18538, 18703, 18868, 19032, 19195, 19358,
	19520, 19681, 19841, 20001, 20160, 20318, 20475, 20632, 20788, 20943, 21097, 21251, 21403,
	21555, 21706, 21856, 22006, 22154, 22302, 22449, 22595, 22740, 22884, 23028, 23170, 23312,
	23453, 23593, 23732, 23870, 24008, 24144, 24279, 24414, 24548, 24680, 24812, 24943, 25073,
	25202, 25330, 25457, 25583, 25708, 25833, 25956, 26078, 26199, 26320, 26439, 26557, 26674,
	26791, 26906, 27020, 27133, 27246, 27357, 27467, 27576, 27684, 27791, 27897, 28002, 28106,
	28209, 28311, 28411, 28511, 28610, 28707, 28803, 28899, 28993, 29086, 29178, 29269, 29359,
	29448, 29535, 29622, 29707, 29792, 29875, 29957, 30038, 30118, 30196, 30274, 30350, 30425,
	30499, 30572, 30644, 30715, 30784, 30853, 30920, 30986, 31050, 31114, 31177, 31238, 31298,
	31357, 31415, 31471, 31527, 31581, 31634, 31686, 31737, 31786, 31834, 31881, 31927, 31972,
	32015, 32058, 32099, 32138, 32177, 32214, 32251, 32286, 32319, 32352, 32383, 32413, 32442,
	32470, 32496, 32522, 32546, 32568, 32590, 32610, 32629, 32647, 32664, 32679, 32693, 32706,
	32718, 32729, 32738, 32746, 32753, 32758, 32762, 32766, 32767, 32768,
};

/* sin(2 pi g / 1024) in Q15 for any g (the quarter wave table mirrored). */
static int32_t sin_q15(uint32_t g)
{
	uint32_t quad = (g / GRID_QUAD) & 3U, r = g % GRID_QUAD;
	int32_t v = qsin[(quad & 1U) != 0U ? GRID_QUAD - r : r];

	return quad >= 2U ? -v : v;
}

/* amp * q / 32768 rounded half away from zero. */
static int32_t scale_q15(int32_t amp, int32_t q)
{
	int32_t p = (amp * (q < 0 ? -q : q) + 16384) >> 15;

	return q < 0 ? -p : p;
}

void gfsk_lut_init(struct gfsk_lut *lut, int amp, bool conj)
{
	if (amp < 1) {
		amp = 1;
	} else if (amp > 511) {
		amp = 511;
	}
	lut->amp = (int16_t)amp;
	for (uint32_t i = 0; i < GFSK_LUT_SIZE; i++) {
		uint32_t g = i * (GRID / GFSK_LUT_SIZE);
		int32_t c = scale_q15(amp, sin_q15(g + GRID_QUAD));
		int32_t s = scale_q15(amp, sin_q15(g));

		lut->w[i] = gfsk_word(c, conj ? -s : s);
	}
}

/* 1/65536 turn of a phase in 2^-32 turn, rounded (mod 1 turn). */
static inline uint16_t round16(uint32_t p)
{
	return (uint16_t)((p + 0x8000U) >> 16);
}

void gfsk_tab_row(const struct gfsk_tab *tab, int bp, int bc, int bn, int16_t row[GFSK_MOD_SPS],
		  int32_t *phi0)
{
	uint32_t f[GFSK_MOD_SPS + 1U];
	int64_t d = tab->bit_step;
	uint32_t p0 = 0;

	/* Phase of the pulses: prev, cur, next bit are the last, middle and first third. */
	for (uint32_t k = 0; k <= GFSK_MOD_SPS; k++) {
		int32_t g = bp * (int32_t)gauss[2U * GFSK_MOD_SPS + k] +
			    bc * (int32_t)gauss[GFSK_MOD_SPS + k] + bn * (int32_t)gauss[k];
		/* floor(d g / 2^15) modulo 2^32: the logical shift keeps the low 32 bits of the
		 * arithmetic one. */
		uint32_t ph = (uint32_t)((uint64_t)(d * g) >> 15);

		if (k == 0U) {
			p0 = ph;
		}
		/* Relative to the symbol start, the IF added. */
		f[k] = ph - p0 + k * (uint32_t)tab->if_inc;
	}
	for (uint32_t k = 0; k < GFSK_MOD_SPS; k++) {
		row[k] = (int16_t)(uint16_t)(round16(f[k + 1U]) - round16(f[k]));
	}
	*phi0 = (int32_t)p0;
}

void gfsk_tab_init(struct gfsk_tab *tab, int32_t dev_hz, int32_t if_hz)
{
	/* Below half a turn per bit, so that the products fit; a CC2400 reaches 496 kHz. */
	if (dev_hz < 0) {
		dev_hz = 0;
	} else if (dev_hz > 499999) {
		dev_hz = 499999;
	}
	tab->bit_step = (int32_t)(((int64_t)dev_hz << 32) / (int64_t)GFSK_MOD_SYM_HZ);
	{
		int64_t num = (int64_t)if_hz * 4294967296LL, half = (int64_t)GFSK_MOD_RATE_HZ / 2;

		tab->if_inc =
			(int32_t)((num + (num < 0 ? -half : half)) / (int64_t)GFSK_MOD_RATE_HZ);
	}
	for (int h = 0; h < 8; h++) {
		gfsk_tab_row(tab, (h & 4) != 0 ? 1 : -1, (h & 2) != 0 ? 1 : -1,
			     (h & 1) != 0 ? 1 : -1, tab->inc[h], &tab->phi0[h]);
	}
}

/* ----------------------------------------------------------------- burst */

uint32_t gfsk_mod_start(struct gfsk_mod *m, const struct gfsk_tab *tab, const struct gfsk_lut *lut,
			const struct gfsk_burst *b)
{
	uint64_t syms;

	m->tab = tab;
	m->lut = lut;
	m->b = *b;
	m->pos = 0;
	m->total = 0;
	m->ramp_step = 0;
	if (b->kind != GFSK_BURST && b->kind != GFSK_LOOP && b->kind != GFSK_CARRIER) {
		return 0;
	}
	if (b->nbits == 0U || (b->kind != GFSK_CARRIER && b->bits == NULL)) {
		return 0;
	}
	syms = b->nbits;
	if (b->kind == GFSK_BURST) {
		syms += (uint64_t)b->lead + b->trail;
	} else {
		m->b.lead = 0;
		m->b.trail = 0;
		m->b.ramp = 0;
	}
	if (syms > (1U << 20)) {
		return 0;
	}
	m->total = (uint32_t)syms * GFSK_MOD_SPS;
	if (m->b.ramp > GFSK_MOD_RAMP_MAX) {
		m->b.ramp = GFSK_MOD_RAMP_MAX;
	}
	if (2U * m->b.ramp > m->total) {
		m->b.ramp = (uint16_t)(m->total / 2U);
	}
	if (m->b.ramp != 0U) {
		m->ramp_step = ((GRID / 4U) << 17) / m->b.ramp;
	}
	return m->total;
}

static inline unsigned int pop8(unsigned int v)
{
	v = v - ((v >> 1) & 0x55U);
	v = (v & 0x33U) + ((v >> 2) & 0x33U);
	return (v + (v >> 4)) & 0x0fU;
}

/* Bit i of the burst's array. */
static inline unsigned int bit_at(const struct gfsk_burst *b, uint32_t i)
{
	return ((unsigned int)b->bits[i >> 3] >> ((i & 7U) ^ (b->msb_first ? 7U : 0U))) & 1U;
}

/* Ones among the first @p d bits. */
static uint32_t ones_before(const struct gfsk_burst *b, uint32_t d)
{
	uint32_t full = d >> 3, rem = d & 7U, cnt = 0;

	for (uint32_t i = 0; i < full; i++) {
		cnt += pop8(b->bits[i]);
	}
	if (rem != 0U) {
		unsigned int v = b->bits[full];

		v &= b->msb_first ? (0xffU << (8U - rem)) & 0xffU : (1U << rem) - 1U;
		cnt += pop8(v);
	}
	return cnt;
}

/* Symbol i outside the bits of a burst, or the wrap of a loop. */
static int sym_edge(const struct gfsk_mod *m, int32_t i)
{
	const struct gfsk_burst *b = &m->b;

	if (b->kind == GFSK_LOOP) {
		int32_t n = (int32_t)b->nbits;

		i %= n;
		if (i < 0) {
			i += n;
		}
		return bit_at(b, (uint32_t)i) != 0U ? 1 : -1;
	}
	if (i >= (int32_t)b->lead && b->end == GFSK_END_HOLD) {
		return bit_at(b, b->nbits - 1U) != 0U ? 1 : -1;
	}
	return 0;
}

/* Symbol i of the burst: +1, -1, or 0 for the carrier outside the bits. */
static inline int sym_at(const struct gfsk_mod *m, int32_t i)
{
	uint32_t d = (uint32_t)(i - (int32_t)m->b.lead);

	if (d < m->b.nbits) {
		return bit_at(&m->b, d) != 0U ? 1 : -1;
	}
	return sym_edge(m, i);
}

/* Sum of the symbols before x. */
static int32_t sym_sum(const struct gfsk_mod *m, int32_t x)
{
	const struct gfsk_burst *b = &m->b;
	int32_t d = x - (int32_t)b->lead;
	uint32_t n;

	if (b->kind == GFSK_LOOP) {
		d = x;
	}
	if (d <= 0) {
		return 0;
	}
	n = (uint32_t)d < b->nbits ? (uint32_t)d : b->nbits;

	int32_t sum = 2 * (int32_t)ones_before(b, n) - (int32_t)n;

	if (b->kind == GFSK_BURST && (uint32_t)d > b->nbits && b->end == GFSK_END_HOLD) {
		sum += (int32_t)((uint32_t)d - b->nbits) *
		       (bit_at(b, b->nbits - 1U) != 0U ? 1 : -1);
	}
	return sum;
}

/* A phase step as an unsigned addend (sign extended, so the sum wraps modulo a turn). */
static inline uint32_t inc_of(int16_t v)
{
	return (uint32_t)(int32_t)v;
}

/* Samples of a symbol run, steady amplitude: the hot loop. */
UBT_HOT static void emit(const uint32_t *lut, uint32_t acc, const int16_t *row, uint32_t cnt,
			 uint32_t *dst)
{
	uint32_t k = 0;

	for (; k + 4U <= cnt; k += 4U) {
		uint32_t a0 = acc, a1 = a0 + inc_of(row[k]), a2 = a1 + inc_of(row[k + 1U]),
			 a3 = a2 + inc_of(row[k + 2U]);

		dst[k] = lut[(a0 >> ACC_SHIFT) & LUT_MASK];
		dst[k + 1U] = lut[(a1 >> ACC_SHIFT) & LUT_MASK];
		dst[k + 2U] = lut[(a2 >> ACC_SHIFT) & LUT_MASK];
		dst[k + 3U] = lut[(a3 >> ACC_SHIFT) & LUT_MASK];
		acc = a3 + inc_of(row[k + 3U]);
	}
	for (; k < cnt; k++) {
		dst[k] = lut[(acc >> ACC_SHIFT) & LUT_MASK];
		acc += inc_of(row[k]);
	}
}

/* A whole symbol of the steady data run: 40 samples, unrolled by 8 (5 loop passes). */
static inline __attribute__((always_inline)) void emit_symbol(const uint32_t *lut, uint32_t acc,
							      const int16_t *row, uint32_t *dst)
{
	for (uint32_t k = 0; k < GFSK_MOD_SPS; k += 8U) {
		uint32_t a1 = acc + inc_of(row[k]), a2 = a1 + inc_of(row[k + 1U]),
			 a3 = a2 + inc_of(row[k + 2U]), a4 = a3 + inc_of(row[k + 3U]),
			 a5 = a4 + inc_of(row[k + 4U]), a6 = a5 + inc_of(row[k + 5U]),
			 a7 = a6 + inc_of(row[k + 6U]);

		dst[k] = lut[(acc >> ACC_SHIFT) & LUT_MASK];
		dst[k + 1U] = lut[(a1 >> ACC_SHIFT) & LUT_MASK];
		dst[k + 2U] = lut[(a2 >> ACC_SHIFT) & LUT_MASK];
		dst[k + 3U] = lut[(a3 >> ACC_SHIFT) & LUT_MASK];
		dst[k + 4U] = lut[(a4 >> ACC_SHIFT) & LUT_MASK];
		dst[k + 5U] = lut[(a5 >> ACC_SHIFT) & LUT_MASK];
		dst[k + 6U] = lut[(a6 >> ACC_SHIFT) & LUT_MASK];
		dst[k + 7U] = lut[(a7 >> ACC_SHIFT) & LUT_MASK];
		acc = a7 + inc_of(row[k + 7U]);
	}
}

/*
 * @p nsym whole symbols from symbol @p s of a burst, every one with its neighbours inside the bits
 * and clear of the ramps: the loop that matters, with the state in registers. @p sum is the sum
 * of the symbols before s - 1.
 */
UBT_HOT static void gen_steady(const struct gfsk_mod *m, uint32_t *dst, uint32_t s, uint32_t nsym,
			       int32_t sum)
{
	const struct gfsk_tab *tab = m->tab;
	const struct gfsk_burst *b = &m->b;
	const uint32_t *lut = m->lut->w;
	const uint32_t step = (uint32_t)tab->bit_step,
		       ifstep = GFSK_MOD_SPS * (uint32_t)tab->if_inc;
	uint32_t d = s - b->lead, ifb = s * ifstep, bs = step * (uint32_t)sum;
	unsigned int h = (bit_at(b, d - 1U) << 2) | (bit_at(b, d) << 1) | bit_at(b, d + 1U);

	while (nsym-- > 0U) {
		uint32_t acc = ((ifb + bs + (uint32_t)tab->phi0[h]) >> 16) + ACC_BIAS;

		emit_symbol(lut, acc, tab->inc[h], dst);
		dst += GFSK_MOD_SPS;
		ifb += ifstep;
		/* The symbol before this one's neighbours joins the sum. */
		bs += (h & 4U) != 0U ? step : -step;
		d++;
		if (nsym > 0U) {
			h = ((h << 1) & 6U) | bit_at(b, d + 1U);
		}
	}
}

/* Raised cosine gain of ramp sample r (0 to ramp - 1), Q15. */
static int32_t ramp_gain(const struct gfsk_mod *m, uint32_t r)
{
	uint32_t g = ((2U * r + 1U) * m->ramp_step) >> 17;

	return (32768 - sin_q15(g + GRID_QUAD)) >> 1;
}

/* A symbol run inside a ramp: samples [u, u + cnt) of the burst, per sample gain. */
UBT_HOT static void emit_ramp(const struct gfsk_mod *m, uint32_t acc, const int16_t *row,
			      uint32_t cnt, uint32_t u, uint32_t *dst)
{
	uint32_t ramp = m->b.ramp, down = m->total - ramp;

	for (uint32_t k = 0; k < cnt; k++, u++) {
		uint32_t w = m->lut->w[(acc >> ACC_SHIFT) & LUT_MASK];

		if (u < ramp || u >= down) {
			int32_t g = ramp_gain(m, u < ramp ? u : m->total - 1U - u);
			int32_t i = (int32_t)(w << 22) >> 22, q = (int32_t)(w << 12) >> 22;

			w = gfsk_word((i * g + 16384) >> 15, (q * g + 16384) >> 15);
		}
		dst[k] = w;
		acc += inc_of(row[k]);
	}
}

/* Words for samples [pos, pos + n) of a burst, loop or carrier, pos + n <= total. */
static void gen_range(const struct gfsk_mod *m, uint32_t *dst, uint32_t pos, uint32_t n)
{
	const struct gfsk_tab *tab = m->tab;
	const bool carrier = m->b.kind == GFSK_CARRIER;
	uint32_t s = pos / GFSK_MOD_SPS, t = pos % GFSK_MOD_SPS;
	uint32_t ifb = s * GFSK_MOD_SPS * (uint32_t)tab->if_inc;
	int bp = 0, bc = 0, bn = 0;
	int32_t sum = 0;
	int16_t zero_row[GFSK_MOD_SPS];
	int32_t zero_phi0 = 0;

	if (carrier) {
		gfsk_tab_row(tab, 0, 0, 0, zero_row, &zero_phi0);
	} else {
		bp = sym_at(m, (int32_t)s - 1);
		bc = sym_at(m, (int32_t)s);
		bn = sym_at(m, (int32_t)s + 1);
		sum = sym_sum(m, (int32_t)s - 1);
		if (m->b.kind == GFSK_LOOP && s == 0U) {
			/* The periodic past nets to zero before bit 0: the sum up to bit -2 drops
			 * bit -1. */
			sum = -bp;
		}
	}
	/* Symbols s_a to s_b - 1 of a burst have all their neighbours in the bits and no ramp in
	 * them. */
	uint32_t s_a = 1, s_b = 0;

	if (m->b.kind == GFSK_BURST) {
		s_a = (uint32_t)m->b.lead + 1U;
		s_b = (uint32_t)m->b.lead + m->b.nbits - 1U;
		if (m->b.ramp != 0U) {
			uint32_t up = (m->b.ramp + GFSK_MOD_SPS - 1U) / GFSK_MOD_SPS;
			uint32_t down = (m->total - m->b.ramp) / GFSK_MOD_SPS;

			s_a = s_a > up ? s_a : up;
			s_b = s_b < down ? s_b : down;
		}
	}
	while (n > 0U) {
		uint32_t cnt = GFSK_MOD_SPS - t < n ? GFSK_MOD_SPS - t : n;
		const int16_t *row;
		int16_t tmp[GFSK_MOD_SPS];
		int32_t phi0;
		uint32_t acc;

		if (t == 0U && n >= GFSK_MOD_SPS && s >= s_a && s < s_b) {
			uint32_t nsym = n / GFSK_MOD_SPS < s_b - s ? n / GFSK_MOD_SPS : s_b - s;

			gen_steady(m, dst, s, nsym, sum);
			dst += nsym * GFSK_MOD_SPS;
			pos += nsym * GFSK_MOD_SPS;
			n -= nsym * GFSK_MOD_SPS;
			s += nsym;
			ifb += nsym * GFSK_MOD_SPS * (uint32_t)tab->if_inc;
			if (n > 0U) {
				/* Back to the general path: its state from the symbols at the new
				 * place. */
				bp = sym_at(m, (int32_t)s - 1);
				bc = sym_at(m, (int32_t)s);
				bn = sym_at(m, (int32_t)s + 1);
				sum = sym_sum(m, (int32_t)s - 1);
			}
			continue;
		}
		if (carrier) {
			row = zero_row;
			phi0 = zero_phi0;
		} else if (bp != 0 && bc != 0 && bn != 0) {
			unsigned int h =
				(bp > 0 ? 4U : 0U) | (bc > 0 ? 2U : 0U) | (bn > 0 ? 1U : 0U);

			row = tab->inc[h];
			phi0 = tab->phi0[h];
		} else {
			gfsk_tab_row(tab, bp, bc, bn, tmp, &phi0);
			row = tmp;
		}
		acc = ((ifb + (uint32_t)tab->bit_step * (uint32_t)sum + (uint32_t)phi0) >> 16) +
		      ACC_BIAS;
		for (uint32_t k = 0; k < t; k++) {
			acc += inc_of(row[k]);
		}
		row += t;
		if (m->b.ramp != 0U && (pos < m->b.ramp || pos + cnt > m->total - m->b.ramp)) {
			emit_ramp(m, acc, row, cnt, pos, dst);
		} else {
			emit(m->lut->w, acc, row, cnt, dst);
		}
		dst += cnt;
		pos += cnt;
		n -= cnt;
		t = 0;
		s++;
		ifb += GFSK_MOD_SPS * (uint32_t)tab->if_inc;
		if (!carrier) {
			sum += bp;
			bp = bc;
			bc = bn;
			bn = sym_at(m, (int32_t)s + 1);
		}
	}
}

UBT_HOT void gfsk_mod_gen(const struct gfsk_mod *m, uint32_t *dst, uint64_t index, uint32_t n)
{
	if (m->b.kind != GFSK_BURST) {
		uint32_t pos = (uint32_t)(index % m->total);

		while (n > 0U) {
			uint32_t cnt = m->total - pos < n ? m->total - pos : n;

			gen_range(m, dst, pos, cnt);
			dst += cnt;
			n -= cnt;
			pos = 0;
		}
		return;
	}
	if (index >= m->total) {
		memset(dst, 0, (size_t)n * sizeof(*dst));
		return;
	}
	if (index + n > m->total) {
		uint32_t cnt = m->total - (uint32_t)index;

		gen_range(m, dst, (uint32_t)index, cnt);
		memset(dst + cnt, 0, (size_t)(n - cnt) * sizeof(*dst));
		return;
	}
	gen_range(m, dst, (uint32_t)index, n);
}

void gfsk_mod_gen_cb(void *user, uint32_t *dst, uint64_t index, uint32_t n)
{
	gfsk_mod_gen(user, dst, index, n);
}

size_t gfsk_mod_fill(struct gfsk_mod *m, uint32_t *dst, size_t n)
{
	if (m->b.kind == GFSK_BURST) {
		if (m->pos >= m->total) {
			return 0;
		}
		if (n > m->total - m->pos) {
			n = m->total - m->pos;
		}
	}
	for (size_t done = 0; done < n;) {
		size_t chunk = n - done > 0x40000000U ? 0x40000000U : n - done;

		gfsk_mod_gen(m, dst + done, m->pos, (uint32_t)chunk);
		m->pos = m->b.kind == GFSK_BURST
				 ? m->pos + (uint32_t)chunk
				 : (uint32_t)(((uint64_t)m->pos + chunk) % m->total);
		done += chunk;
	}
	return n;
}
