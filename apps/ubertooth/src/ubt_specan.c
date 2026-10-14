/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Spectrum analyser math of the SPECAN mode, see ubt_specan.h.
 */

#include "ubt_specan.h"

#include <errno.h>
#include <string.h>

#include "ubt_attr.h"

#define HALF     (UBT_SPECAN_N / 2U)
/* Bin cell width 5/16 MHz and channel width 1 MHz in 1/32 MHz units. */
#define CELL_32  10
#define CHAN_32  32
#define LOG2_PTS 32U

/* 32768 (1 - cos(2 pi n / 256)) / 2 for n = 0 to 128: the periodic Hann window, Q15. */
static const uint16_t hann[HALF + 1U] = {
	0,     5,     20,    44,    79,    123,   177,   241,   315,   398,   491,   593,   705,
	827,   958,   1098,  1247,  1406,  1573,  1749,  1935,  2128,  2331,  2542,  2761,  2989,
	3224,  3468,  3719,  3978,  4244,  4518,  4799,  5087,  5381,  5682,  5990,  6304,  6624,
	6950,  7282,  7619,  7961,  8308,  8661,  9018,  9379,  9745,  10114, 10487, 10864, 11245,
	11628, 12014, 12403, 12794, 13188, 13583, 13980, 14378, 14778, 15179, 15580, 15982, 16384,
	16786, 17188, 17589, 17990, 18390, 18788, 19185, 19580, 19974, 20365, 20754, 21140, 21523,
	21904, 22281, 22654, 23023, 23389, 23750, 24107, 24460, 24807, 25149, 25486, 25818, 26144,
	26464, 26778, 27086, 27387, 27681, 27969, 28250, 28524, 28790, 29049, 29300, 29544, 29779,
	30007, 30226, 30437, 30640, 30833, 31019, 31195, 31362, 31521, 31670, 31810, 31941, 32063,
	32175, 32277, 32370, 32453, 32527, 32591, 32645, 32689, 32724, 32748, 32763, 32768,
};

/* 65536 log2(1 + i / 32). */
static const uint32_t log2_tab[LOG2_PTS + 1U] = {
	0,     2909,  5732,  8473,  11136, 13727, 16248, 18704, 21098, 23433, 25711,
	27936, 30109, 32234, 34312, 36346, 38336, 40286, 42196, 44068, 45904, 47705,
	49472, 51207, 52911, 54584, 56229, 57845, 59434, 60997, 62534, 64047, 65536,
};

const int16_t ubt_specan_w256[UBT_SPECAN_N] __attribute__((aligned(16))) = {
	32767, 0,     0,      32767, 23170, 23170, -23170, 23170, 30273, 12539, -12539, 30273,
	12539, 30273, -30273, 12539, 32137, 6393,  -6393,  32137, 18204, 27245, -27245, 18204,
	27245, 18204, -18204, 27245, 6393,  32137, -32137, 6393,  32609, 3212,  -3212,  32609,
	20787, 25329, -25329, 20787, 28898, 15446, -15446, 28898, 9512,  31356, -31356, 9512,
	31356, 9512,  -9512,  31356, 15446, 28898, -28898, 15446, 25329, 20787, -20787, 25329,
	3212,  32609, -32609, 3212,  32728, 1608,  -1608,  32728, 22005, 24279, -24279, 22005,
	29621, 14010, -14010, 29621, 11039, 30852, -30852, 11039, 31785, 7962,  -7962,  31785,
	16846, 28105, -28105, 16846, 26319, 19519, -19519, 26319, 4808,  32412, -32412, 4808,
	32412, 4808,  -4808,  32412, 19519, 26319, -26319, 19519, 28105, 16846, -16846, 28105,
	7962,  31785, -31785, 7962,  30852, 11039, -11039, 30852, 14010, 29621, -29621, 14010,
	24279, 22005, -22005, 24279, 1608,  32728, -32728, 1608,  32757, 804,   -804,   32757,
	22594, 23731, -23731, 22594, 29956, 13279, -13279, 29956, 11793, 30571, -30571, 11793,
	31971, 7179,  -7179,  31971, 17530, 27683, -27683, 17530, 26790, 18868, -18868, 26790,
	5602,  32285, -32285, 5602,  32521, 4011,  -4011,  32521, 20159, 25832, -25832, 20159,
	28510, 16151, -16151, 28510, 8739,  31580, -31580, 8739,  31113, 10278, -10278, 31113,
	14732, 29268, -29268, 14732, 24811, 21403, -21403, 24811, 2410,  32678, -32678, 2410,
	32678, 2410,  -2410,  32678, 21403, 24811, -24811, 21403, 29268, 14732, -14732, 29268,
	10278, 31113, -31113, 10278, 31580, 8739,  -8739,  31580, 16151, 28510, -28510, 16151,
	25832, 20159, -20159, 25832, 4011,  32521, -32521, 4011,  32285, 5602,  -5602,  32285,
	18868, 26790, -26790, 18868, 27683, 17530, -17530, 27683, 7179,  31971, -31971, 7179,
	30571, 11793, -11793, 30571, 13279, 29956, -29956, 13279, 23731, 22594, -22594, 23731,
	804,   32757, -32757, 804,
};

void ubt_specan_fft_ref(int16_t *iq)
{
	const int16_t *w = ubt_specan_w256;
	unsigned int ie = 1;

	for (unsigned int n2 = HALF; n2 > 0U; n2 >>= 1, ie <<= 1) {
		unsigned int ia = 0;

		for (unsigned int j = 0; j < ie; j++, ia += n2) {
			int64_t c = w[2U * j], s = w[2U * j + 1U];

			for (unsigned int i = 0; i < n2; i++, ia++) {
				int16_t *a = &iq[2U * ia], *b = &iq[2U * (ia + n2)];
				int64_t tr = c * b[0] + s * b[1], ti = c * b[1] - s * b[0];
				int64_t ar = (int64_t)a[0] * 0x7fff, ai = (int64_t)a[1] * 0x7fff;

				/* (a -+ t) / 2 with the kernel's rounding: sum + 0x7fff, then
				 * >> 16. */
				b[0] = (int16_t)((ar - tr + 0x7fff) >> 16);
				b[1] = (int16_t)((ai - ti + 0x7fff) >> 16);
				a[0] = (int16_t)((ar + tr + 0x7fff) >> 16);
				a[1] = (int16_t)((ai + ti + 0x7fff) >> 16);
			}
		}
	}
}

void ubt_specan_init(struct ubt_specan *s, ubt_specan_fft_fn fft, unsigned int in_shift)
{
	s->fft = fft;
	s->in_shift = (uint8_t)(in_shift > 5U ? 5U : in_shift);
	ubt_specan_begin(s);
}

void ubt_specan_begin(struct ubt_specan *s)
{
	s->frames = 0;
	memset(s->acc, 0, sizeof(s->acc));
}

void ubt_specan_dc(const uint32_t *w, size_t n, int *dc_i, int *dc_q)
{
	int32_t si = 0, sq = 0;
	size_t cnt = 0;

	for (size_t k = 0; k < n; k += 8U, cnt++) {
		si += (int32_t)(w[k] << 12) >> 22;
		sq += (int32_t)(w[k] << 22) >> 22;
	}
	if (cnt == 0U) {
		*dc_i = 0;
		*dc_q = 0;
		return;
	}
	/* Rounded to nearest. */
	*dc_i = (int)((si + (int32_t)(cnt / 2U) * (si < 0 ? -1 : 1)) / (int32_t)cnt);
	*dc_q = (int)((sq + (int32_t)(cnt / 2U) * (sq < 0 ? -1 : 1)) / (int32_t)cnt);
}

UBT_HOT void ubt_specan_decode(const uint32_t *w, int dc_i, int dc_q, unsigned int in_shift,
			       int16_t *iq)
{
	const unsigned int sh = 15U - in_shift;
	const int32_t rnd = 1 << (sh - 1U);

	for (unsigned int n = 0; n < UBT_SPECAN_N; n++) {
		uint32_t v = w[n];
		int32_t win = hann[n <= HALF ? n : UBT_SPECAN_N - n];
		int32_t i = ((int32_t)(v << 12) >> 22) - dc_i,
			q = ((int32_t)(v << 22) >> 22) - dc_q;

		iq[2U * n] = (int16_t)((i * win + rnd) >> sh);
		iq[2U * n + 1U] = (int16_t)((q * win + rnd) >> sh);
	}
}

UBT_HOT void ubt_specan_accum(uint32_t *acc, const int16_t *iq)
{
	for (unsigned int k = 0; k < UBT_SPECAN_N; k++) {
		int32_t re = iq[2U * k], im = iq[2U * k + 1U];

		acc[k] += (uint32_t)(re * re + im * im);
	}
}

UBT_HOT bool ubt_specan_frame(struct ubt_specan *s, const uint32_t *w, int dc_i, int dc_q)
{
	if (s->frames >= UBT_SPECAN_FRAMES_MAX) {
		return false;
	}
	ubt_specan_decode(w, dc_i, dc_q, s->in_shift, s->iq);
	s->fft(s->iq);
	ubt_specan_accum(s->acc, s->iq);
	s->frames++;
	return true;
}

unsigned int ubt_specan_capture(struct ubt_specan *s, const uint32_t *w, size_t n,
				unsigned int stride, unsigned int max_frames)
{
	unsigned int done = 0;
	int dc_i, dc_q;

	if (n < UBT_SPECAN_N || stride == 0U) {
		return 0;
	}
	ubt_specan_dc(w, n, &dc_i, &dc_q);
	for (size_t at = 0; at + UBT_SPECAN_N <= n && done < max_frames; at += stride) {
		if (!ubt_specan_frame(s, w + at, dc_i, dc_q)) {
			break;
		}
		done++;
	}
	return done;
}

/* ---------------------------------------------------------------- levels */

static int floor_div(int a, int b)
{
	int q = a / b;

	return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

static unsigned int rev8(unsigned int v)
{
	v = ((v & 0xf0U) >> 4) | ((v & 0x0fU) << 4);
	v = ((v & 0xccU) >> 2) | ((v & 0x33U) << 2);
	return ((v & 0xaaU) >> 1) | ((v & 0x55U) << 1);
}

uint64_t ubt_specan_chan_power(const struct ubt_specan *s, int offset_mhz,
			       const struct ubt_specan_cal *cal)
{
	int d = cal != NULL && cal->mirror ? -offset_mhz : offset_mhz;
	int lo = (2 * d - 1) * (CHAN_32 / 2), hi = (2 * d + 1) * (CHAN_32 / 2);
	int kmin = floor_div(lo, CELL_32) - 1, kmax = floor_div(hi, CELL_32) + 1;
	uint64_t sum = 0;

	for (int k = kmin; k <= kmax; k++) {
		int cl = (2 * k - 1) * (CELL_32 / 2), ch = (2 * k + 1) * (CELL_32 / 2);
		int ov = (hi < ch ? hi : ch) - (lo > cl ? lo : cl);
		unsigned int nk = (unsigned int)k & (UBT_SPECAN_N - 1U);

		if (ov <= 0 || k < -(int)HALF || k >= (int)HALF) {
			continue;
		}
		sum += (uint64_t)s->acc[rev8(nk)] * (uint64_t)ov *
		       (cal != NULL && cal->flat != NULL ? cal->flat[nk] : 4096U);
	}
	return sum >> 12;
}

/* 65536 log2(x), x >= 1. */
static uint32_t log2_q16(uint64_t x)
{
	unsigned int msb = 63U - (unsigned int)__builtin_clzll(x);
	uint32_t m = (uint32_t)(msb >= 31U ? x >> (msb - 31U) : x << (31U - msb)) & 0x7fffffffU;
	unsigned int idx = m >> 26;
	uint32_t t = (m >> 10) & 0xffffU;

	return (msb << 16) + log2_tab[idx] +
	       (uint32_t)(((uint64_t)(log2_tab[idx + 1U] - log2_tab[idx]) * t) >> 16);
}

/* 256 * 10 log10(2) * log2: 6165 / 2^19 * 65536 = 770.6. */
static int32_t db_q8(uint32_t log2q16)
{
	return (int32_t)(((uint64_t)log2q16 * 6165U) >> 19);
}

int32_t ubt_specan_dbfs_q8(uint64_t power, unsigned int frames, unsigned int in_shift)
{
	/* 256 * (10 dB for the 10 per bin weights + 10 log10(0.375 * 2^18) for a full scale tone)
	 */
	const int32_t ref = 2560 + 12781;

	if (power == 0U || frames == 0U) {
		return INT32_MIN;
	}
	/* The tone's power gains 6.0206 dB per bit of input gain. */
	return db_q8(log2_q16(power)) - db_q8(log2_q16(frames)) - ref - (int32_t)(in_shift * 1541U);
}

int8_t ubt_specan_channel_raw(const struct ubt_specan *s, int offset_mhz,
			      const struct ubt_specan_cal *cal)
{
	int32_t l = ubt_specan_dbfs_q8(ubt_specan_chan_power(s, offset_mhz, cal), s->frames,
				       s->in_shift);
	int32_t v;

	if (l == INT32_MIN) {
		return INT8_MIN;
	}
	v = (l + cal->fs_dbm_q8 + UBT_RSSI_OFFSET * 256 + 128) >> 8;
	return (int8_t)(v < INT8_MIN ? INT8_MIN : (v > INT8_MAX ? INT8_MAX : v));
}

int32_t ubt_specan_cal_fs_dbm(const struct ubt_specan *s, int offset_mhz,
			      const struct ubt_specan_cal *cal, int32_t known_dbm_q8)
{
	int32_t l = ubt_specan_dbfs_q8(ubt_specan_chan_power(s, offset_mhz, cal), s->frames,
				       s->in_shift);

	return l == INT32_MIN ? INT32_MIN : known_dbm_q8 - l;
}

/* ---------------------------------------------------------------- planning */

void ubt_specan_cfg_default(struct ubt_specan_cfg *cfg)
{
	cfg->span_mhz = UBT_SPECAN_SPAN_MHZ;
	cfg->excl_mhz = UBT_SPECAN_EXCL_MHZ;
	cfg->lo_min_mhz = 1;
	cfg->lo_max_mhz = UINT16_MAX;
}

static int iabs(int v)
{
	return v < 0 ? -v : v;
}

/* Whether every channel of [low, high] has a placement among lo[0..n). */
static bool plan_covers(const struct ubt_specan_plan *p, const int *lo, unsigned int n)
{
	for (int f = p->low; f <= p->high; f++) {
		bool ok = false;

		for (unsigned int i = 0; i < n && !ok; i++) {
			int d = iabs(f - lo[i]);

			ok = d >= p->excl_mhz && d <= p->span_mhz;
		}
		if (!ok) {
			return false;
		}
	}
	return true;
}

static int clamp_lo(int v, const struct ubt_specan_cfg *cfg)
{
	return v < cfg->lo_min_mhz ? cfg->lo_min_mhz : (v > cfg->lo_max_mhz ? cfg->lo_max_mhz : v);
}

int ubt_specan_plan(struct ubt_specan_plan *plan, uint16_t low, uint16_t high,
		    const struct ubt_specan_cfg *cfg)
{
	struct ubt_specan_cfg def;
	int lo[UBT_SPECAN_LO_MAX];
	int span, excl, smax, mid, range;

	if (cfg == NULL) {
		ubt_specan_cfg_default(&def);
		cfg = &def;
	}
	if (low > high || cfg->excl_mhz < 1U || cfg->span_mhz <= cfg->excl_mhz) {
		return -EINVAL;
	}
	span = cfg->span_mhz;
	excl = cfg->excl_mhz;
	smax = span - excl + 1;
	mid = (span + excl) / 2;
	range = high - low + 1;
	memset(plan, 0, sizeof(*plan));
	plan->low = low;
	plan->high = high;
	plan->span_mhz = (uint8_t)span;
	plan->excl_mhz = (uint8_t)excl;

	for (unsigned int n = range <= smax ? 1U : 2U; n <= UBT_SPECAN_LO_MAX; n++) {
		if (n == 1U) {
			/* Below the range, then above it: the whole range in the window of one
			 * side. */
			int centre = (low + high) / 2;

			for (int side = 0; side < 2; side++) {
				int v;

				if (side == 0) {
					v = centre - mid;
					v = low - v < excl ? low - excl : v;
					v = high - v > span ? high - span : v;
				} else {
					v = centre + mid;
					v = v - high < excl ? high + excl : v;
					v = v - low > span ? low + span : v;
				}
				lo[0] = clamp_lo(v, cfg);
				if (plan_covers(plan, lo, 1U)) {
					goto found;
				}
			}
		} else {
			int cov = 2 * span + 1 + (int)(n - 1U) * smax;
			int lo1;

			if (cov < range) {
				continue;
			}
			lo1 = low + span - (cov - range) / 2;
			for (unsigned int i = 0; i < n; i++) {
				lo[i] = clamp_lo(lo1 + (int)i * smax, cfg);
			}
			if (plan_covers(plan, lo, n)) {
				goto found;
			}
		}
		continue;
found:
		plan->n_lo = (uint8_t)n;
		for (unsigned int i = 0; i < n; i++) {
			plan->lo_mhz[i] = (uint16_t)lo[i];
		}
		return 0;
	}
	return -ERANGE;
}

int ubt_specan_lo_for(const struct ubt_specan_plan *plan, uint16_t mhz)
{
	int best = -1, best_score = 0, mid = (plan->span_mhz + plan->excl_mhz) / 2;

	for (unsigned int i = 0; i < plan->n_lo; i++) {
		int d = iabs((int)mhz - (int)plan->lo_mhz[i]);
		int score = iabs(d - mid);

		if (d >= plan->excl_mhz && d <= plan->span_mhz &&
		    (best < 0 || score < best_score)) {
			best = (int)i;
			best_score = score;
		}
	}
	return best;
}

unsigned int ubt_specan_collect(const struct ubt_specan *s, const struct ubt_specan_plan *plan,
				unsigned int lo_idx, const struct ubt_specan_cal *cal, int8_t *raw)
{
	unsigned int n = 0;

	for (unsigned int f = plan->low; f <= plan->high; f++) {
		if (ubt_specan_lo_for(plan, (uint16_t)f) == (int)lo_idx) {
			raw[f - plan->low] =
				ubt_specan_channel_raw(s, (int)f - (int)plan->lo_mhz[lo_idx], cal);
			n++;
		}
	}
	return n;
}

/* ----------------------------------------------------------------- packets */

void ubt_specan_packer_init(struct ubt_specan_packer *p,
			    void (*clock)(void *, uint8_t *, uint32_t *),
			    void (*emit)(void *, const usb_pkt_rx *), void *user)
{
	memset(p, 0, sizeof(*p));
	p->clock = clock;
	p->emit = emit;
	p->user = user;
}

void ubt_specan_pack(struct ubt_specan_packer *p, uint16_t mhz, int8_t rssi)
{
	uint8_t *d = &p->pkt.data[3U * p->n];

	d[0] = (uint8_t)(mhz >> 8);
	d[1] = (uint8_t)mhz;
	d[2] = (uint8_t)rssi;
	if (++p->n < UBT_SPECAN_TRIPLES) {
		return;
	}
	p->n = 0;
	/* Upstream enqueue() for SPECAN sets only these; bytes 48 and 49 are never written. */
	p->pkt.pkt_type = SPECAN;
	p->pkt.status = 0;
	p->pkt.channel = 0;
	p->pkt.clkn_high = 0;
	p->pkt.clk100ns = 0;
	p->pkt.rssi_max = 0;
	p->pkt.rssi_min = 0;
	p->pkt.rssi_avg = 0;
	p->pkt.rssi_count = 0;
	p->pkt.reserved[0] = 0;
	p->pkt.reserved[1] = 0;
	p->pkt.data[3U * UBT_SPECAN_TRIPLES] = 0;
	p->pkt.data[3U * UBT_SPECAN_TRIPLES + 1U] = 0;
	if (p->clock != NULL) {
		p->clock(p->user, &p->pkt.clkn_high, &p->pkt.clk100ns);
	}
	if (p->emit != NULL) {
		p->emit(p->user, &p->pkt);
	}
}

void ubt_specan_pack_sweep(struct ubt_specan_packer *p, uint16_t low, uint16_t high,
			   const int8_t *raw)
{
	for (unsigned int f = low; f <= high; f++) {
		ubt_specan_pack(p, (uint16_t)f, raw[f - low]);
	}
}

/* -------------------------------------------------------------- cost model */

uint32_t ubt_specan_sweep_us(unsigned int n_lo, unsigned int frames,
			     const struct ubt_specan_model *m)
{
	uint32_t rc = m->retune_us + m->capture_us;
	uint32_t dsp = frames * (m->decode_us + m->fft_us + m->accum_us);

	if (m->overlap) {
		return n_lo * (rc > dsp ? rc : dsp);
	}
	return n_lo * (rc + dsp) + m->sweep_us;
}
