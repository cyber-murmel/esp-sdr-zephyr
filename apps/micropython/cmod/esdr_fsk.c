/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The (G)FSK modem of the gnuradio.digital stand-ins (esdr_fsk.h); the algorithm and its
 * reasons are in modules/gnuradio/digital/_fsk.py, which this follows line by line.
 */

#include <math.h>
#include <string.h>

#include "esdr_fsk.h"

#define TURN          65536
#define TRIG_BITS     32U
#define TRIG_ERRORS   4U
#define MEAN_SYMBOLS  32
#define FLOOR_START   8
#define FLOOR_SHIFT   12
#define FLOOR_GATE    8
#define TAU_F         6.28318530717958647692f
#define PI_F          (TAU_F / 2.0f)
/* The modulator's cosine: a table over a turn, interpolated (within 0.2 of an LSB). */
#define COS_BITS      10U
#define COS_N         (1U << COS_BITS)

enum { SEARCH, PEND, HEADER, PAYLOAD };

static uint32_t hist_entries(uint32_t sps, uint32_t code_len)
{
	uint32_t size = 1;

	while (size < (code_len + 2U) * sps || size <= (MEAN_SYMBOLS + 1U) * sps) {
		size *= 2U;
	}
	return size;
}

size_t esdr_fsk_rx_size(uint32_t sps, uint32_t code_len)
{
	return sizeof(struct esdr_fsk_rx) + hist_entries(sps, code_len) * sizeof(int32_t);
}

int esdr_fsk_rx_init(struct esdr_fsk_rx *rx, size_t size, uint32_t sps, uint64_t code,
		     uint32_t code_len, uint32_t threshold, uint32_t max_len)
{
	if (sps < 2U || sps > ESDR_FSK_SPS_MAX || code_len < 8U || code_len > 64U || max_len < 1U ||
	    max_len > ESDR_FSK_LEN_MAX || size < esdr_fsk_rx_size(sps, code_len)) {
		return -1;
	}
	memset(rx, 0, sizeof(*rx));
	rx->sps = sps;
	rx->code_len = code_len;
	rx->code = code_len < 64U ? code & ((1ULL << code_len) - 1U) : code;
	rx->threshold = threshold;
	rx->max_len = max_len;
	rx->mask = hist_entries(sps, code_len) - 1U;

	uint32_t tb = code_len < TRIG_BITS ? code_len : TRIG_BITS;

	rx->trig_mask = tb < 32U ? (1U << tb) - 1U : 0xFFFFFFFFU;
	rx->trig = (uint32_t)rx->code & rx->trig_mask;
	rx->trig_errors = threshold < TRIG_ERRORS ? threshold : TRIG_ERRORS;
	rx->csum = 0;
	for (uint32_t i = 0; i < code_len; i++) {
		rx->csum += ((rx->code >> (code_len - 1U - i)) & 1U) ? 1 : -1;
	}
	esdr_fsk_rx_reset(rx);
	return 0;
}

void esdr_fsk_rx_reset(struct esdr_fsk_rx *rx)
{
	memset(rx->hist, 0, (rx->mask + 1U) * sizeof(int32_t));
	memset(rx->dring, 0, sizeof(rx->dring));
	memset(rx->mean, 0, sizeof(rx->mean));
	memset(rx->reg, 0, sizeof(rx->reg));
	rx->dsum = 0;
	rx->pi = 0;
	rx->pq = 0;
	rx->n = 0;
	rx->ph = 0;
	rx->fl = 0;
	rx->state = SEARCH;
}

uint32_t esdr_fsk_rx_state(const struct esdr_fsk_rx *rx)
{
	return rx->state;
}

int32_t esdr_fsk_angle(int32_t re, int32_t im)
{
	uint32_t ax = re >= 0 ? (uint32_t)re : -(uint32_t)re;
	uint32_t ay = im >= 0 ? (uint32_t)im : -(uint32_t)im;
	uint32_t mx = ax >= ay ? ax : ay, mn = ax >= ay ? ay : ax;
	int32_t a;

	if (mx == 0U) {
		return 0;
	}
	/* Down to 15 bits, as the Python loop shifts one bit at a time (no clz on every core). */
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
	return (int32_t)(((uint32_t)(a + 32768) & 0xFFFFU)) - 32768;
}

static inline int code_bit(const struct esdr_fsk_rx *rx, uint32_t i)
{
	return (int)((rx->code >> (rx->code_len - 1U - i)) & 1U);
}

static inline int32_t hist_at(const struct esdr_fsk_rx *rx, uint32_t idx)
{
	return rx->hist[idx & rx->mask];
}

static int verify(struct esdr_fsk_rx *rx)
{
	uint32_t sps = rx->sps, L = rx->code_len;
	int64_t best_corr = 0;
	uint32_t best_e = 0;
	int have = 0;

	for (uint32_t e = rx->pend - sps / 2U; e != rx->pend + sps; e++) {
		int64_t corr = 0;

		for (uint32_t i = 0; i < L; i++) {
			int32_t v = hist_at(rx, e - (L - 1U - i) * sps);

			corr += code_bit(rx, i) ? v : -v;
		}
		if (!have || corr > best_corr) {
			best_corr = corr;
			best_e = e;
			have = 1;
		}
	}

	uint32_t e = best_e;
	int64_t corr = best_corr, M = 0, S = rx->csum;

	for (uint32_t i = 0; i < L; i++) {
		M += hist_at(rx, e - (L - 1U - i) * sps);
	}
	int64_t den = (int64_t)L * L - S * S;
	int64_t num = M * L - corr * S;
	uint32_t errors = 0;

	for (uint32_t i = 0; i < L; i++) {
		int64_t v = hist_at(rx, e - (L - 1U - i) * sps);

		if ((v * den > num) != (code_bit(rx, i) != 0)) {
			errors++;
		}
	}
	if (errors > rx->threshold) {
		return 0;
	}
	rx->num = num;
	rx->den = den;

	double dc = (double)num / (double)den;
	double amp = (double)(corr * L - M * S) / (double)den;
	double var = 0.0;

	for (uint32_t i = 0; i < L; i++) {
		double r = (double)hist_at(rx, e - (L - 1U - i) * sps) - dc -
			   (code_bit(rx, i) ? amp : -amp);

		var += r * r;
	}
	rx->cur.errors = errors;
	rx->cur.phase = e % sps;
	rx->cur.dc = dc;
	rx->cur.amp = amp;
	rx->cur.var = var / (double)L;
	rx->cur.floor = (uint32_t)(rx->fl >> FLOOR_SHIFT);
	rx->state = HEADER;
	rx->bits = 0;
	rx->acc = 0;
	rx->power = 0;
	rx->npower = 0;
	rx->next = e + sps;
	return 1;
}

/* The next bit of a frame; true when it completed one (in rx->cur). */
static int take_bit(struct esdr_fsk_rx *rx, int32_t v, uint32_t nn)
{
	rx->acc = (rx->acc << 1) | ((int64_t)v * rx->den > rx->num ? 1U : 0U);
	rx->bits++;
	if (rx->state == HEADER) {
		if (rx->bits == 32U) {
			uint32_t l0 = rx->acc >> 16, l1 = rx->acc & 0xFFFFU;

			if (l0 != l1 || l0 == 0U || l0 > rx->max_len) {
				rx->state = SEARCH;
			} else {
				rx->state = PAYLOAD;
				rx->want = 8U * l0;
				rx->cur.len = l0;
				rx->bits = 0;
				rx->acc = 0;
			}
		}
		return 0;
	}
	if ((rx->bits & 7U) == 0U) {
		rx->cur.data[(rx->bits >> 3) - 1U] = (uint8_t)rx->acc;
		rx->acc = 0;
	}
	if (rx->bits < rx->want) {
		return 0;
	}
	rx->state = SEARCH;
	rx->cur.end = nn;
	rx->cur.power = (double)rx->power / (double)(rx->npower > 0U ? rx->npower : 1U);
	return 1;
}

void esdr_fsk_rx_process(struct esdr_fsk_rx *rx, const int16_t *iq, size_t n, esdr_fsk_emit_t emit,
			 void *user)
{
	uint32_t sps = rx->sps, mask = rx->mask;
	int32_t pi = rx->pi, pq = rx->pq;

	for (size_t k = 0; k < n; k++) {
		/* Halved: the products stay within 32 bits. */
		int32_t i = iq[2U * k] >> 1, q = iq[2U * k + 1U] >> 1;
		int32_t d = esdr_fsk_angle(i * pi + q * pq, q * pi - i * pq);
		uint32_t p = rx->ph, nn = rx->n;

		pi = i;
		pq = q;
		rx->dsum += d - rx->dring[p];
		rx->dring[p] = d;

		int32_t v = rx->dsum;

		rx->hist[nn & mask] = v;
		rx->mean[p] += v - rx->hist[(nn - MEAN_SYMBOLS * sps) & mask];
		rx->reg[p] = (rx->reg[p] << 1) | (v * MEAN_SYMBOLS > rx->mean[p] ? 1U : 0U);

		uint32_t st = rx->state;

		if (st <= PEND) {
			int64_t pw = (int64_t)i * i + (int64_t)q * q;

			if (nn < (1U << FLOOR_START)) {
				rx->fl += pw << (FLOOR_SHIFT - FLOOR_START);
			} else if ((pw << FLOOR_SHIFT) <= FLOOR_GATE * rx->fl) {
				rx->fl += pw - (rx->fl >> FLOOR_SHIFT);
			}
		}
		if (st == SEARCH) {
			if (nn >= (rx->code_len + 1U) * sps &&
			    (uint32_t)__builtin_popcount((rx->reg[p] ^ rx->trig) & rx->trig_mask) <=
				    rx->trig_errors) {
				rx->state = PEND;
				rx->pend = nn;
			}
		} else if (st == PEND) {
			if (nn == rx->pend + sps - 1U) {
				if (verify(rx)) {
					/* The first bits after the code may be behind already. */
					while (rx->state != SEARCH && rx->next <= nn) {
						uint32_t at = rx->next;

						rx->next += sps;
						if (take_bit(rx, hist_at(rx, at), at)) {
							emit(user, &rx->cur);
						}
					}
				} else {
					rx->state = SEARCH;
				}
			}
		} else {
			rx->power += (uint64_t)((int64_t)i * i + (int64_t)q * q);
			rx->npower++;
			if (nn == rx->next) {
				rx->next += sps;
				if (take_bit(rx, v, nn)) {
					emit(user, &rx->cur);
				}
			}
		}
		rx->n = nn + 1U;
		rx->ph = p + 1U < sps ? p + 1U : 0U;
	}
	rx->pi = pi;
	rx->pq = pq;
}

/*
 * cos and sin from a table: cosf() and sinf() for every sample took the ESP32-C5 (no FPU) some
 * 0.6 s for a 30-byte packet. The output differs from _fsk.py's by an LSB at most.
 */
/* round(32767 cos(tau k / COS_N)) for a quarter turn and two more (in flash: the S3's DRAM has
 * no room for it). */
static const int16_t cos_tab[COS_N / 4U + 2U] = {
	32767, 32766, 32765, 32761, 32757, 32752, 32745, 32737, 32728, 32717, 32705, 32692,
	32678, 32663, 32646, 32628, 32609, 32589, 32567, 32545, 32521, 32495, 32469, 32441,
	32412, 32382, 32351, 32318, 32285, 32250, 32213, 32176, 32137, 32098, 32057, 32014,
	31971, 31926, 31880, 31833, 31785, 31736, 31685, 31633, 31580, 31526, 31470, 31414,
	31356, 31297, 31237, 31176, 31113, 31050, 30985, 30919, 30852, 30783, 30714, 30643,
	30571, 30498, 30424, 30349, 30273, 30195, 30117, 30037, 29956, 29874, 29791, 29706,
	29621, 29534, 29447, 29358, 29268, 29177, 29085, 28992, 28898, 28803, 28706, 28609,
	28510, 28411, 28310, 28208, 28105, 28001, 27896, 27790, 27683, 27575, 27466, 27356,
	27245, 27133, 27019, 26905, 26790, 26674, 26556, 26438, 26319, 26198, 26077, 25955,
	25832, 25708, 25582, 25456, 25329, 25201, 25072, 24942, 24811, 24680, 24547, 24413,
	24279, 24143, 24007, 23870, 23731, 23592, 23452, 23311, 23170, 23027, 22884, 22739,
	22594, 22448, 22301, 22154, 22005, 21856, 21705, 21554, 21403, 21250, 21096, 20942,
	20787, 20631, 20475, 20317, 20159, 20000, 19841, 19680, 19519, 19357, 19195, 19032,
	18868, 18703, 18537, 18371, 18204, 18037, 17869, 17700, 17530, 17360, 17189, 17018,
	16846, 16673, 16499, 16325, 16151, 15976, 15800, 15623, 15446, 15269, 15090, 14912,
	14732, 14553, 14372, 14191, 14010, 13828, 13645, 13462, 13279, 13094, 12910, 12725,
	12539, 12353, 12167, 11980, 11793, 11605, 11417, 11228, 11039, 10849, 10659, 10469,
	10278, 10087, 9896, 9704, 9512, 9319, 9126, 8933, 8739, 8545, 8351, 8157,
	7962, 7767, 7571, 7375, 7179, 6983, 6786, 6590, 6393, 6195, 5998, 5800,
	5602, 5404, 5205, 5007, 4808, 4609, 4410, 4210, 4011, 3811, 3612, 3412,
	3212, 3012, 2811, 2611, 2410, 2210, 2009, 1809, 1608, 1407, 1206, 1005,
	804, 603, 402, 201, 0, -201,
};

/* cos of p / 2^32 turns, p within a quarter turn (to 2^30 inclusive), Q15. */
static inline int32_t cos_quarter(uint32_t p)
{
	uint32_t i = p >> (32U - COS_BITS);
	int32_t f = (int32_t)((p >> (16U - COS_BITS)) & 0xffffU);
	int32_t c0 = cos_tab[i];

	return c0 + (((cos_tab[i + 1U] - c0) * f) >> 16);
}

/* cos of u / 2^32 turns, Q15. */
static inline int32_t cos_turn(uint32_t u)
{
	uint32_t p = u & 0x3fffffffU, q = (1U << 30) - p;

	switch (u >> 30) {
	case 0:
		return cos_quarter(p);
	case 1:
		return -cos_quarter(q);
	case 2:
		return -cos_quarter(p);
	default:
		return cos_quarter(q);
	}
}

/* a (counts at full scale, within 2 x 32767) times c (Q15), clipped to +-32767. */
static inline int16_t scale16(int32_t a, int32_t c)
{
	int32_t r = (a * c + (1 << 14)) >> 15;

	return (int16_t)(r > 32767 ? 32767 : (r < -32767 ? -32767 : r));
}

size_t esdr_fsk_mod(const uint8_t *bits, size_t nbits, uint32_t sps, const float *taps,
		    size_t ntaps, float sensitivity, float amp, int16_t *out, size_t cap,
		    float *phase)
{
	size_t nsym = nbits + (ntaps + sps - 1U) / sps;
	size_t n = nsym * sps;
	float ph = *phase, af = amp * 32767.0f;
	int32_t a = (int32_t)lroundf(af > 65534.0f ? 65534.0f : (af < -65534.0f ? -65534.0f : af));

	if (sps == 0U || n > cap) {
		return 0;
	}
	for (size_t k = 0; k < nsym; k++) {
		for (uint32_t j = 0; j < sps; j++) {
			float y = 0.0f;

			for (size_t m = 0; j + m * sps < ntaps; m++) {
				if (m <= k && k - m < nbits) {
					size_t s = k - m;
					int b = (bits[s >> 3] >> (7U - (s & 7U))) & 1;

					y += b ? taps[j + m * sps] : -taps[j + m * sps];
				}
			}
			ph += sensitivity * y;
			if (ph > PI_F || ph < -PI_F) {
				ph -= TAU_F * floorf((ph + PI_F) / TAU_F);
			}
			/* ph within a half turn either way: as a turn of 2^32. */
			uint32_t u = (uint32_t)(int64_t)(ph * (4294967296.0f / TAU_F));

			out[2U * (k * sps + j)] = scale16(a, cos_turn(u));
			out[2U * (k * sps + j) + 1U] = scale16(a, cos_turn(u - (1U << 30)));
		}
	}
	*phase = ph;
	return n;
}
