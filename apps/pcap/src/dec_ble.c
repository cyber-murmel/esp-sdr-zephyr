/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Bluetooth LE decoder of apps/pcap (decode.h, doc/pcap.md "Decoders"): LE 1M and LE 2M
 * packets in a closed burst at 4 or 2 MS/s, the algorithm of apps/chancap/tools/ble_pcap.py. An
 * FM discriminator (dec_step(), a binary angle a sample) summed over a symbol at every sampling
 * phase, the access addresses looked for at every position whatever the carrier offset, the
 * bits sliced at the preamble's level, dewhitened and CRC checked with the vendored upstream
 * apps/ubertooth/src/upstream/bluetooth_le.c (its directory on the include path, its source
 * linked in). Plain C, no Zephyr: tests/unit/pcap_ble runs it on native_sim.
 *
 * The search runs per sampling phase, in sample order, in two steps. The coarse step, at every
 * position (of every other phase at 4 samples a symbol): each soft symbol sliced once against
 * the mean of the 32 symbols from it on (all of them signal where an access address starts up
 * to 31 symbols before, whatever precedes the packet), the last 32 decisions compared with
 * every address. Where an address comes within max_errors + GATE_SLACK bits (and
 * |ones - zeros| / 8 more for an unbalanced one), the fine step is ble_pcap.py's find_aa() at
 * the positions from there to one symbol past the first match: the normalised correlation of
 * the 32 soft symbols with the address's +-1 pattern and their bit errors against the window's
 * own midpoint. The best match with at most max_errors errors is taken (fewest errors, then
 * highest correlation). Matches are taken in time order: ble_pcap.py takes the best match of
 * the whole rest of a capture, which can skip a packet before a better one.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "bluetooth_le.h"
#include "dec_util.h"
#include "decode.h"

#define SYM_RATE_1M 1000000U
#define SYM_RATE_2M 2000000U

/* Symbols of the preambles, of the access address, and of the shortest packet after the
 * preamble (address, PDU header, CRC). */
#define PRE_1M     8U
#define PRE_2M     16U
#define AA_SYMS    32U
#define MIN_SYMS   (32U + 16U + 24U)
/* Preamble symbols before the address whose mean is the slicing level: alternating, so the
 * Gaussian filter's intersymbol interference cancels. */
#define LEVEL_SYMS 6U
/* The coarse step's bit errors beyond max_errors (its level is not the window's midpoint). */
#define GATE_SLACK 4U

/* Longer bursts are refused: sample indices stay well within 32 bits. */
#define N_MAX (1U << 24)

/* The RF channels: 2402 MHz + 2 MHz c for c = 0 .. 39, a frequency within 500 kHz. */
#define RF_BASE_HZ 2402000000LL
#define RF_STEP_HZ 2000000LL
#define RF_TOL_HZ  500000LL
#define RF_COUNT   40

/* CONNECT_IND: advertising PDU type 5, a payload of InitA, AdvA and 22 bytes of LLData whose
 * access address and CRCInit sit at these offsets of dec_ble_pkt.air (ubt_le.h). */
#define PDU_CONNECT_IND 5U
#define CONNECT_LEN     34U
#define AIR_CONN_AA     18U
#define AIR_CONN_CRC    22U

/*
 * LE 1M packets whose samples LE 2M skips, at most: one takes 80 symbols or more, 320 samples
 * at 4 MS/s (the only rate with LE 2M).
 */
#define CLAIMS(n) ((n) / 256U + 2U)

/* A configured access address as the search uses it. */
struct pat {
	uint32_t aa;
	uint32_t crc_init;
	/* Its ones, zeros, ones - zeros, and 1024 - (ones - zeros)^2 (32^2 / 32 times the
	 * variance of its +-1 pattern). */
	int32_t n1, n0, psum;
	int64_t varp;
	/* The coarse step's bit errors at most. */
	uint32_t gate;
};

/* One PHY's view of the burst: the symbol sums per sampling phase. */
struct pass {
	const int16_t *s[4];
	uint32_t k[4];
	uint32_t sps;
	uint32_t pre;
	uint32_t sym_rate;
	enum dec_ble_phy phy;
};

/* Samples [from, to) of an LE 1M packet: LE 2M looks for nothing there. */
struct claim {
	int32_t from, to;
};

struct ctx {
	const struct burst *b;
	const struct dec_ble_cfg *cfg;
	struct pat pat[DEC_BLE_AA_MAX];
	unsigned int npat;
	/* The channel's centre, MHz, for le_dewhiten(). */
	unsigned int mhz;
	/* n binary angles (the last one 0), n symbol sums, the claims. */
	int16_t *d;
	int16_t *sums;
	struct claim *claims;
	uint32_t nclaims, claims_max;
	dec_ble_emit emit;
	void *user;
	int count;
};

/* A position the fine step accepted: phase, symbol, address, bit errors, correlation (Q14). */
struct match {
	uint32_t p, k;
	const struct pat *pat;
	uint32_t errors;
	int32_t r;
};

/* In the coarse step's loop: inline at -Os too. */
static inline __attribute__((always_inline)) uint32_t popcount32(uint32_t v)
{
	v = v - ((v >> 1) & 0x55555555U);
	v = (v & 0x33333333U) + ((v >> 2) & 0x33333333U);
	v = (v + (v >> 4)) & 0x0F0F0F0FU;
	return (v * 0x01010101U) >> 24;
}

/* CRCInit as the CONNECT_IND carries it to the start state of btle_crcgen_lut(): bit reversed. */
static uint32_t rev24(uint32_t v)
{
	uint32_t r = 0;

	for (unsigned int i = 0; i < 24U; i++) {
		r |= ((v >> i) & 1U) << (23U - i);
	}
	return r;
}

static uint32_t isqrt64(uint64_t v)
{
	uint64_t r = 0, bit = 1ULL << 62;

	while (bit > v) {
		bit >>= 2;
	}
	while (bit != 0U) {
		if (v >= r + bit) {
			v -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
		bit >>= 2;
	}
	return (uint32_t)r;
}

int dec_ble_rf_channel(uint64_t freq_hz)
{
	int64_t d, c, dev;

	if (freq_hz > (uint64_t)INT64_MAX / 2U) {
		return -1;
	}
	d = (int64_t)freq_hz - RF_BASE_HZ + RF_STEP_HZ / 2;
	if (d < 0) {
		return -1;
	}
	c = d / RF_STEP_HZ;
	dev = d - c * RF_STEP_HZ - RF_STEP_HZ / 2;
	if (c >= RF_COUNT || dev <= -RF_TOL_HZ || dev >= RF_TOL_HZ) {
		return -1;
	}
	return (int)c;
}

size_t dec_ble_scratch(uint32_t n)
{
	/* Alignment, the claims, the angles and the symbol sums. */
	return 4U + (size_t)CLAIMS(n) * sizeof(struct claim) + 2U * (size_t)n * sizeof(int16_t);
}

static unsigned int patterns(const struct dec_ble_cfg *cfg, struct pat *pt)
{
	unsigned int n = 0;

	for (unsigned int i = 0; i < cfg->aa_count; i++) {
		int32_t ones = (int32_t)popcount32(cfg->aa[i].aa);
		int32_t psum = 2 * ones - 32;

		/* A constant pattern correlates with nothing. */
		if (ones == 0 || ones == 32) {
			continue;
		}
		pt[n].aa = cfg->aa[i].aa;
		pt[n].crc_init = cfg->aa[i].crc_init & 0xFFFFFFU;
		pt[n].n1 = ones;
		pt[n].n0 = 32 - ones;
		pt[n].psum = psum;
		pt[n].varp = 1024 - psum * psum;
		pt[n].gate =
			cfg->max_errors + GATE_SLACK + (uint32_t)(psum < 0 ? -psum : psum) / 8U;
		n++;
	}
	return n;
}

/* The phase step from every sample to the next: one dec_step() a sample. */
static void angles(const struct burst *b, int16_t *d)
{
	const int16_t *iq = b->iq;
	uint32_t i;

	for (i = 0; i + 1U < b->n; i++, iq += 2) {
		/* +pi (32768) wraps to -pi: the same angle. */
		d[i] = (int16_t)dec_step(iq[0], iq[1], iq[2], iq[3]);
	}
	d[b->n - 1U] = 0;
}

/*
 * ble_pcap.py's symbol_sums() and phase split: the angles summed over a symbol (sps of them)
 * from every sample on, divided by sps (rounded, to stay within int16), the sums of the
 * samples p, p + sps, p + 2 sps, ... in a row for phase p.
 */
static void symbol_sums(struct ctx *c, struct pass *ps)
{
	const uint32_t sps = ps->sps;
	const uint32_t shift = sps == 4U ? 2U : 1U;
	const int32_t half = (int32_t)(sps >> 1);
	const uint32_t total = c->b->n - sps;
	const uint32_t stride = (total + sps - 1U) / sps;
	const int16_t *d = c->d;
	int16_t *const sums = c->sums;
	int32_t run = 0;
	uint32_t i, j, k, p;

	for (p = 0; p < sps; p++) {
		ps->s[p] = sums + p * stride;
		ps->k[p] = total > p ? (total - p + sps - 1U) / sps : 0U;
	}
	for (i = 0; i < sps; i++) {
		run += d[i];
	}
	for (j = 0, k = 0; j < total; k++) {
		for (p = 0; p < sps && j < total; p++, j++) {
			sums[p * stride + k] = (int16_t)((run + half) >> shift);
			run += d[j + sps] - d[j];
		}
	}
}

/*
 * find_aa() at one position: the normalised correlation (Q14) of the 32 soft symbols from @p s
 * with the address's +-1 pattern, and the bit errors against the window's own midpoint (the
 * middle between the mean of the symbols where the address has ones and that where it has
 * zeros), returned. A flat window matches nothing.
 */
static uint32_t exact(const int16_t *s, const struct pat *pt, int32_t *r)
{
	int32_t s1 = 0, a1 = 0, a0, num, den;
	int64_t s2 = 0, c, var;
	uint32_t errors = 0, i;

	for (i = 0; i < AA_SYMS; i++) {
		int32_t v = s[i];

		s1 += v;
		s2 += (int64_t)v * v;
		if (((pt->aa >> i) & 1U) != 0U) {
			a1 += v;
		}
	}
	var = 32 * s2 - (int64_t)s1 * s1;
	if (var <= 0) {
		*r = 0;
		return AA_SYMS;
	}
	a0 = s1 - a1;
	c = 32 * (int64_t)(a1 - a0) - (int64_t)pt->psum * s1;
	*r = (int32_t)(c * 16384 / (int64_t)isqrt64((uint64_t)var * (uint64_t)pt->varp));
	num = a1 * pt->n0 + a0 * pt->n1;
	den = 2 * pt->n1 * pt->n0;
	for (i = 0; i < AA_SYMS; i++) {
		errors += (uint32_t)(s[i] * den > num) ^ ((pt->aa >> i) & 1U);
	}
	return errors;
}

/*
 * The fine step from sample position @p x0 (phase x0 % sps, symbol x0 / sps) to one symbol past
 * the first position that matches; positions up to @p done were looked at before. True with
 * the best match in @p best.
 */
static bool fine(const struct ctx *c, const struct pass *ps, int32_t x0, uint32_t k0min,
		 int32_t *done, struct match *best)
{
	const int32_t sps = (int32_t)ps->sps;
	int32_t x = x0 > *done ? x0 : *done + 1;
	int32_t xend = x0 + sps - 1;
	bool found = false;

	for (; x <= xend; x++) {
		uint32_t p = (uint32_t)x % ps->sps, k = (uint32_t)x / ps->sps;

		*done = x;
		if (k < k0min || ps->k[p] < MIN_SYMS || k > ps->k[p] - MIN_SYMS) {
			continue;
		}
		for (unsigned int j = 0; j < c->npat; j++) {
			int32_t r;
			uint32_t e = exact(ps->s[p] + k, &c->pat[j], &r);

			if (e > c->cfg->max_errors) {
				continue;
			}
			if (!found) {
				xend = x + sps - 1;
			}
			if (!found || e < best->errors || (e == best->errors && r > best->r)) {
				best->p = p;
				best->k = k;
				best->pat = &c->pat[j];
				best->errors = e;
				best->r = r;
				found = true;
			}
		}
	}
	return found;
}

/*
 * @p nbytes bytes of the symbols from @p s, a one where a symbol lies above the level
 * num / den, the first symbol on air in each byte's bit 7 (as le_dewhiten() takes them).
 */
static void slice(const int16_t *s, int32_t num, int32_t den, uint8_t *out, uint32_t nbytes)
{
	for (uint32_t i = 0; i < nbytes; i++, s += 8) {
		uint32_t v = 0;

		for (uint32_t j = 0; j < 8U; j++) {
			v = (v << 1) | (uint32_t)(s[j] * den > num);
		}
		out[i] = (uint8_t)v;
	}
}

/* Mean I^2 + Q^2 over samples [from, to) of the burst. */
static float power(const struct burst *b, int32_t from, int32_t to)
{
	uint64_t acc = 0;

	if (from < 0) {
		from = 0;
	}
	if (to > (int32_t)b->n) {
		to = (int32_t)b->n;
	}
	if (to <= from) {
		return 0.0f;
	}
	for (int32_t i = from; i < to; i++) {
		int32_t vi = b->iq[2 * i], vq = b->iq[2 * i + 1];

		acc += (uint32_t)(vi * vi) + (uint32_t)(vq * vq);
	}
	return (float)acc / (float)(to - from);
}

static uint32_t le_get(const uint8_t *p, unsigned int bytes)
{
	uint32_t v = 0;

	for (unsigned int i = 0; i < bytes; i++) {
		v |= (uint32_t)p[i] << (8U * i);
	}
	return v;
}

/*
 * The packet whose access address the fine step found: sliced at the mean of the preamble's
 * last LEVEL_SYMS symbols (at the window's midpoint without room for them), the header
 * dewhitened for the length, then all of it; the CRC checked, the packet emitted unless its
 * CRC failed and bad ones are not kept. Returns the symbol the search goes on from: after the
 * packet, or after the address when the CRC failed (its length is not to be trusted) or the
 * burst ends before the packet does (ble_pcap.py's decode()).
 */
static uint32_t decode(struct ctx *c, const struct pass *ps, const struct match *m)
{
	const struct pat *pt = m->pat;
	const int16_t *s = ps->s[m->p] + m->k;
	const uint32_t avail = ps->k[m->p] - m->k;
	const uint32_t sps = ps->sps;
	struct dec_ble_pkt pkt;
	int32_t num = 0, den, x, end;
	uint32_t rx = 0, len, nbytes, i;
	uint8_t hdr[2];
	bool trusted;

	if (m->k >= LEVEL_SYMS) {
		for (i = 1; i <= LEVEL_SYMS; i++) {
			num += s[-(int32_t)i];
		}
		den = (int32_t)LEVEL_SYMS;
	} else {
		int32_t a1 = 0, a0 = 0;

		for (i = 0; i < AA_SYMS; i++) {
			if (((pt->aa >> i) & 1U) != 0U) {
				a1 += s[i];
			} else {
				a0 += s[i];
			}
		}
		num = a1 * pt->n0 + a0 * pt->n1;
		den = 2 * pt->n1 * pt->n0;
	}
	for (i = 0; i < AA_SYMS; i++) {
		rx |= (uint32_t)(s[i] * den > num) << i;
	}
	slice(s + AA_SYMS, num, den, hdr, 2U);
	le_dewhiten(hdr, 2U, c->mhz);
	len = hdr[1];
	nbytes = 2U + len + 3U;
	if (AA_SYMS + 8U * nbytes > avail) {
		return m->k + AA_SYMS;
	}

	memset(&pkt, 0, sizeof(pkt));
	pkt.phy = ps->phy;
	pkt.aa = pt->aa;
	pkt.aa_errors = (uint8_t)popcount32(rx ^ pt->aa);
	for (i = 0; i < 4U; i++) {
		pkt.air[i] = (uint8_t)(pt->aa >> (8U * i));
	}
	slice(s + AA_SYMS, num, den, pkt.air + 4, nbytes);
	le_dewhiten(pkt.air + 4, nbytes, c->mhz);
	pkt.len = (uint16_t)(4U + nbytes);
	pkt.crc_checked = pt->crc_init != 0U;
	if (pkt.crc_checked) {
		pkt.crc_ok = btle_crcgen_lut(rev24(pt->crc_init), pkt.air + 4, (int)(2U + len)) ==
			     le_get(pkt.air + 6U + len, 3U);
	}
	x = (int32_t)(m->p + m->k * sps);
	pkt.start = x - (int32_t)(ps->pre * sps);
	end = x + (int32_t)((AA_SYMS + 8U * nbytes) * sps);
	/* The level is a mean angle a sample: times sps a symbol. */
	pkt.offset_hz = (int32_t)lrintf(
		dec_angle_hz((float)num / (float)den * (float)sps, (float)ps->sym_rate));
	pkt.power = power(c->b, pkt.start, end);
	if (pt->aa == DEC_BLE_ADV_AA && pkt.crc_ok && (pkt.air[4] & 0x0FU) == PDU_CONNECT_IND &&
	    len >= CONNECT_LEN) {
		pkt.conn_aa = le_get(pkt.air + AIR_CONN_AA, 4U);
		pkt.conn_crc_init = le_get(pkt.air + AIR_CONN_CRC, 3U);
	}

	trusted = !pkt.crc_checked || pkt.crc_ok;
	if (trusted && ps->phy == DEC_BLE_PHY_1M && c->nclaims < c->claims_max) {
		c->claims[c->nclaims].from = pkt.start;
		c->claims[c->nclaims].to = end;
		c->nclaims++;
	}
	if (trusted || c->cfg->keep_bad) {
		c->count++;
		if (c->emit != NULL) {
			c->emit(&pkt, c->user);
		}
	}
	return trusted ? m->k + AA_SYMS + 8U * nbytes : m->k + AA_SYMS;
}

/*
 * The coarse step of one sampling phase, resumable: the next symbol to push, the first push
 * that completes a window to test, the end (an address from symbol K - 72 on leaves no room
 * for a packet, so pushes stop at K - 41), the last 32 decisions (the newest in bit 31: the
 * address's bit 0, first on air, in bit 0), the sum of the 32 symbols from the next one on,
 * and the pending hit: the sample position of its window's first symbol, -1 for none.
 */
struct coarse {
	const int16_t *s;
	uint32_t p;
	uint32_t k, ktest, kend;
	uint32_t reg;
	int32_t fsum;
	int32_t hit;
};

static void coarse_start(struct coarse *cs, const struct pass *ps, uint32_t p, uint32_t k0)
{
	const uint32_t kn = ps->k[p];

	cs->s = ps->s[p];
	cs->p = p;
	cs->k = k0;
	cs->ktest = k0 + AA_SYMS - 1U;
	cs->kend = kn >= MIN_SYMS ? kn - (MIN_SYMS - AA_SYMS) : 0U;
	cs->reg = 0;
	cs->fsum = 0;
	cs->hit = -1;
	if (k0 < cs->kend) {
		for (uint32_t i = 0; i < AA_SYMS; i++) {
			cs->fsum += cs->s[k0 + i];
		}
	}
}

/* One symbol into the decisions: above the mean of the 32 from it on, or not. */
#define COARSE_PUSH()                                                                              \
	do {                                                                                       \
		int32_t v_ = s[k];                                                                 \
                                                                                                   \
		reg = (reg >> 1) | ((uint32_t)(32 * v_ > fsum) << 31);                             \
		fsum += s[k + AA_SYMS] - v_;                                                       \
		k++;                                                                               \
	} while (0)

/* The phase's next hit: the next window within an address's gate (cs->hit, -1 at the end). */
static void coarse_next(struct coarse *cs, const struct ctx *c, uint32_t sps)
{
	const int16_t *s = cs->s;
	const uint32_t kend = cs->kend;
	uint32_t k = cs->k, reg = cs->reg;
	int32_t fsum = cs->fsum;

	cs->hit = -1;
	while (k < kend && k < cs->ktest) {
		COARSE_PUSH();
	}
	if (c->npat == 1U) {
		const uint32_t aa = c->pat[0].aa, gate = c->pat[0].gate;

		while (k < kend) {
			COARSE_PUSH();
			if (popcount32(reg ^ aa) <= gate) {
				cs->hit = (int32_t)(cs->p + (k - AA_SYMS) * sps);
				break;
			}
		}
	} else {
		while (k < kend && cs->hit < 0) {
			COARSE_PUSH();
			for (unsigned int j = 0; j < c->npat; j++) {
				if (popcount32(reg ^ c->pat[j].aa) <= c->pat[j].gate) {
					cs->hit = (int32_t)(cs->p + (k - AA_SYMS) * sps);
					break;
				}
			}
		}
	}
	cs->k = k;
	cs->reg = reg;
	cs->fsum = fsum;
}

/* Every scanned phase from symbol @p k0 on, each to its first hit. */
static void coarse_restart(struct coarse *cs, uint32_t nc, uint32_t step, const struct ctx *c,
			   const struct pass *ps, uint32_t k0)
{
	for (uint32_t i = 0; i < nc; i++) {
		coarse_start(&cs[i], ps, i * step, k0);
		coarse_next(&cs[i], c, ps->sps);
	}
}

/*
 * Every packet of one PHY, in time order (see the top of the file): the phases' coarse hits
 * merged in sample order, the fine step at each. At 4 samples a symbol the coarse step scans
 * every other phase (a symbol sum one sample off the eye's centre still decides right) and the
 * fine step starts a sample before the hit.
 */
static void scan(struct ctx *c, const struct pass *ps)
{
	const uint32_t step = ps->sps == 4U ? 2U : 1U, nc = ps->sps / step;
	struct coarse cs[4];
	struct match m = {0};
	uint32_t k0min = 0, ci = 0;
	int32_t done = -1;

	coarse_restart(cs, nc, step, c, ps, 0U);
	for (;;) {
		uint32_t first = nc;
		int32_t x0;

		for (uint32_t i = 0; i < nc; i++) {
			if (cs[i].hit >= 0 && (first == nc || cs[i].hit < cs[first].hit)) {
				first = i;
			}
		}
		if (first == nc) {
			break;
		}
		x0 = cs[first].hit;
		if (ps->phy == DEC_BLE_PHY_2M) {
			while (ci < c->nclaims && c->claims[ci].to <= x0) {
				ci++;
			}
			if (ci < c->nclaims && c->claims[ci].from <= x0) {
				k0min = ((uint32_t)c->claims[ci].to + ps->sps - 1U) / ps->sps;
				coarse_restart(cs, nc, step, c, ps, k0min);
				continue;
			}
		}
		x0 -= (int32_t)(step - 1U);
		if (fine(c, ps, x0 > 0 ? x0 : 0, k0min, &done, &m)) {
			k0min = decode(c, ps, &m);
			coarse_restart(cs, nc, step, c, ps, k0min);
			continue;
		}
		coarse_next(&cs[first], c, ps->sps);
	}
}

int dec_ble_run(const struct burst *b, const struct dec_ble_cfg *cfg, void *scratch,
		size_t scratch_len, dec_ble_emit emit, void *user)
{
	struct ctx c;
	struct pass ps;
	int rf;

	if (b == NULL || cfg == NULL) {
		return -EINVAL;
	}
	if (b->rate_hz != 4000000U && b->rate_hz != 2000000U) {
		return -ENOTSUP;
	}
	if (cfg->aa_count > DEC_BLE_AA_MAX || cfg->max_errors > 2U || b->n > N_MAX ||
	    (b->n != 0U && b->iq == NULL)) {
		return -EINVAL;
	}
	/* Dewhitening needs the channel: a frequency off the grid is the caller's mistake. */
	rf = dec_ble_rf_channel(b->freq_hz);
	if (rf < 0) {
		return -EINVAL;
	}
	if (scratch == NULL || scratch_len < dec_ble_scratch(b->n)) {
		return -ENOMEM;
	}

	memset(&c, 0, sizeof(c));
	c.b = b;
	c.cfg = cfg;
	c.emit = emit;
	c.user = user;
	c.mhz = 2402U + 2U * (unsigned int)rf;
	c.npat = patterns(cfg, c.pat);
	/* Not even the shortest packet at 2 samples a symbol. */
	if (c.npat == 0U || b->n < 2U * MIN_SYMS) {
		return 0;
	}
	c.claims = (struct claim *)(((uintptr_t)scratch + 3U) & ~(uintptr_t)3U);
	c.claims_max = CLAIMS(b->n);
	c.d = (int16_t *)(c.claims + c.claims_max);
	c.sums = c.d + b->n;
	angles(b, c.d);

	if (!cfg->skip_1m) {
		ps.sps = b->rate_hz / SYM_RATE_1M;
		ps.pre = PRE_1M;
		ps.sym_rate = SYM_RATE_1M;
		ps.phy = DEC_BLE_PHY_1M;
		symbol_sums(&c, &ps);
		scan(&c, &ps);
	}

	if (cfg->phy_2m && b->rate_hz == 4000000U) {
		ps.sps = 2U;
		ps.pre = PRE_2M;
		ps.sym_rate = SYM_RATE_2M;
		ps.phy = DEC_BLE_PHY_2M;
		symbol_sums(&c, &ps);
		scan(&c, &ps);
	}
	return c.count;
}
