/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * BLE packet finder on the symbol stream and the promiscuous mode helpers, see ubt_le.h.
 *
 * The finder keeps the last 32 symbols in a shift register, newest in bit 0, so the first
 * transmitted bit of the access address ends in bit 31 and the correlator target is the
 * bit reversed AA. After a hit the same register yields the bytes: the low 8 bits of it at a
 * byte boundary are the byte's bits in reverse (bit 0 of the byte is the oldest symbol).
 * The AA correlator keeps running while a packet is collected, so a packet that was started
 * by a false hit or whose length field is corrupted never hides the next one.
 *
 * The logic is ported from the Project Ubertooth firmware (bluetooth_rxtx.c, Copyright
 * 2010-2013 Michael Ossmann and 2011-2013 Dominic Spill; le_phy.c, Copyright 2017 Mike Ryan),
 * released under the GPL version 2 or later.
 */

#include <string.h>

#include "ubt_attr.h"
#include "ubt_le.h"
#include "upstream/bluetooth_le.h"

enum {
	ST_HUNT = 0,
	ST_COLLECT = 1,
};

/*
 * Symbols to shift in untested after a reset or gap: the register is valid from the 32nd
 * symbol on (the promiscuous scanner needs the 32 AA and 16 header symbols).
 */
#define WARM_AA      31U
#define WARM_PROMISC 47U

/* Gap scan: compare timestamps this many symbols apart first. */
#define GAP_STRIDE 16U

#define LE_BASECLK 12500U /* 1.25 ms in 100 ns units */

/* The whitening sequence (127 bits, upstream's whitening[]) twice, packed LSB first. */
static const uint8_t wseq[18] = {
	0xaf, 0x42, 0x7b, 0x4e, 0xcd, 0x60, 0xeb, 0x62, 0x22,
	0x90, 0x2c, 0xef, 0xf0, 0xc7, 0x8d, 0xd2, 0x57, 0xa1,
};

/* Bit reversed bytes (Bit Twiddling Hacks). */
#define R2(n) n, n + 2 * 64, n + 1 * 64, n + 3 * 64
#define R4(n) R2(n), R2(n + 2 * 16), R2(n + 1 * 16), R2(n + 3 * 16)
#define R6(n) R4(n), R4(n + 2 * 4), R4(n + 1 * 4), R4(n + 3 * 4)
static const uint8_t rev8[256] = {R6(0), R6(2), R6(1), R6(3)};

/* The 8 whitening bits starting at sequence position p (0..126), bit 0 first on air. */
static inline uint8_t wmask(unsigned int p)
{
	unsigned int q = p >> 3;

	return (uint8_t)(((wseq[q] | ((unsigned int)wseq[q + 1U] << 8)) >> (p & 7U)) & 0xffU);
}

static inline uint8_t wnext(unsigned int p)
{
	p += 8U;
	return (uint8_t)(p >= 127U ? p - 127U : p);
}

static int8_t default_rssi(uint8_t pwr)
{
	return (int8_t)((int)(pwr >> 1) - 46);
}

static uint32_t default_clk(void *ctx, uint32_t t)
{
	(void)ctx;
	return (uint32_t)(((uint64_t)t * 5U / 2U) % UBT_CLK100NS_WRAP);
}

static uint32_t le_clk(const struct ubt_le *le, uint32_t t)
{
	return le->clk != NULL ? le->clk(le->ctx, t) : default_clk(NULL, t);
}

static int8_t le_rssi(const struct ubt_le *le, uint32_t pwr)
{
	return le->rssi != NULL ? le->rssi((uint8_t)pwr) : default_rssi((uint8_t)pwr);
}

/* ---- helpers and configuration ---- */

unsigned int ubt_le_chan_idx(unsigned int mhz)
{
	if (mhz < 2402U || mhz > 2480U || (mhz & 1U) != 0U) {
		return 40U;
	}
	return btle_channel_index((uint16_t)mhz);
}

bool ubt_le_aa_valid(uint32_t aa)
{
	uint32_t x = (aa ^ (aa >> 1)) & 0x7fffffffU; /* bit k set: symbols k and k + 1 differ */
	uint32_t b0 = aa & 0xffU;
	unsigned int k;

	if (aa == UBT_LE_ADV_AA || ubt_popcount32(aa ^ UBT_LE_ADV_AA) == 1U) {
		return false;
	}
	if (b0 == ((aa >> 8) & 0xffU) && b0 == ((aa >> 16) & 0xffU) && b0 == (aa >> 24)) {
		return false;
	}
	if (ubt_popcount32(x) > 24U || ubt_popcount32((x >> 26) & 0x1fU) < 2U) {
		return false;
	}
	for (k = 0; k + 6U <= 30U; k++) {
		if (((x >> k) & 0x3fU) == 0U) { /* seven equal symbols in a row */
			return false;
		}
	}
	return true;
}

void ubt_le_reset(struct ubt_le *le)
{
	le->state = ST_HUNT;
	le->warm = WARM_AA;
	le->nb = 0;
	le->sr = 0;
	le->have_t = false;
}

void ubt_le_set_aa(struct ubt_le *le, uint32_t aa)
{
	le->aa = aa;
	le->aa_rev = ubt_rev32(aa);
	ubt_le_reset(le);
}

void ubt_le_set_crc_init(struct ubt_le *le, uint32_t crc_init)
{
	le->crc_init = crc_init & 0xffffffU;
	le->crc_state0 = ubt_rev24(crc_init);
}

void ubt_le_set_crc_verify(struct ubt_le *le, bool on)
{
	le->crc_verify = on;
}

int ubt_le_set_tolerance(struct ubt_le *le, unsigned int errors)
{
	if (errors > UBT_LE_MAX_TOLERANCE) {
		return -1;
	}
	le->tolerance = (uint8_t)errors;
	return 0;
}

void ubt_le_set_max_len(struct ubt_le *le, uint8_t adv, uint8_t data)
{
	le->max_len_adv = adv;
	le->max_len_data = data;
}

void ubt_le_set_start_back(struct ubt_le *le, int samples)
{
	le->start_back = (int16_t)samples;
}

int ubt_le_set_target(struct ubt_le *le, const uint8_t mac[6], unsigned int mask_bits)
{
	unsigned int i, left = mask_bits;

	if (mask_bits > 48U) {
		return -1;
	}
	memset(le->target, 0, sizeof(le->target));
	memset(le->target_mask, 0, sizeof(le->target_mask));
	le->target_set = mask_bits != 0U;
	if (mask_bits == 0U) {
		return 0;
	}
	/* Sent most significant octet first, on air least significant first. */
	for (i = 0; i < 6U; i++) {
		unsigned int take = left >= 8U ? 8U : left;

		le->target_mask[5U - i] = (uint8_t)(0xffU << (8U - take));
		le->target[5U - i] = mac[i] & le->target_mask[5U - i];
		left -= take;
	}
	return 0;
}

void ubt_le_set_target_raw(struct ubt_le *le, bool set, const uint8_t addr[6],
			   const uint8_t mask[6])
{
	unsigned int i;

	le->target_set = set;
	for (i = 0; i < 6U; i++) {
		le->target_mask[i] = set ? mask[i] : 0U;
		le->target[i] = set ? (uint8_t)(addr[i] & mask[i]) : 0U;
	}
}

int ubt_le_set_channel(struct ubt_le *le, uint16_t mhz)
{
	unsigned int idx = ubt_le_chan_idx(mhz);

	if (idx >= 40U) {
		return -1;
	}
	le->mhz = mhz;
	le->chan_idx = (uint8_t)idx;
	le->w0 = whitening_index[idx];
	ubt_le_reset(le);
	return 0;
}

void ubt_le_init(struct ubt_le *le, ubt_le_emit_fn emit, ubt_le_clk_fn clk, ubt_le_rssi_fn rssi,
		 void *ctx)
{
	memset(le, 0, sizeof(*le));
	le->emit = emit;
	le->clk = clk;
	le->rssi = rssi;
	le->ctx = ctx;
	le->max_len_adv = UBT_LE_MAX_LEN_ADV;
	le->max_len_data = UBT_LE_MAX_LEN_DATA;
	le->start_back = UBT_LE_START_BACK;
	ubt_le_set_crc_init(le, UBT_LE_ADV_CRC_INIT);
	ubt_le_set_channel(le, 2402);
	ubt_le_set_aa(le, UBT_LE_ADV_AA);
}

/* ---- hunting: the per-symbol hot loop ---- */

static inline __attribute__((always_inline)) bool near_aa(uint32_t x, const unsigned int tol)
{
	uint32_t y;

	if (tol == 0U) {
		return x == 0U;
	}
	y = x & (x - 1U); /* clears the lowest set bit */
	if (tol == 1U) {
		return y == 0U;
	}
	return (y & (y - 1U)) == 0U;
}

/*
 * Shift symbols in until the register is within tol bit errors of the AA. Returns the index
 * of the symbol that completed the match, or n. The register is written back either way.
 */
static inline __attribute__((always_inline)) size_t hunt_t(struct ubt_le *le,
							   const struct gfsk_sym *s, size_t n,
							   const unsigned int tol)
{
	uint32_t sr = le->sr;
	const uint32_t tgt = le->aa_rev;
	const struct gfsk_sym *p = s, *end = s + n;

	for (; p != end; p++) {
		sr = (sr << 1) + p->bit;
		if (near_aa(sr ^ tgt, tol)) {
			break;
		}
	}
	le->sr = sr;
	return (size_t)(p - s);
}

UBT_HOT static size_t hunt0(struct ubt_le *le, const struct gfsk_sym *s, size_t n)
{
	return hunt_t(le, s, n, 0U);
}

UBT_HOT static size_t hunt1(struct ubt_le *le, const struct gfsk_sym *s, size_t n)
{
	return hunt_t(le, s, n, 1U);
}

UBT_HOT static size_t hunt2(struct ubt_le *le, const struct gfsk_sym *s, size_t n)
{
	return hunt_t(le, s, n, 2U);
}

/* ---- collecting a packet ---- */

static void begin_packet(struct ubt_le *le, uint32_t t_hit)
{
	usb_pkt_rx *p = &le->pkt;

	le->stats.hits++;
	le->aa_rx = ubt_rev32(le->sr);
	le->t_hit = t_hit;
	memset(p, 0, sizeof(*p));
	p->pkt_type = LE_PACKET;
	p->channel = (uint8_t)(le->mhz - 2402U);
	p->data[0] = (uint8_t)le->aa;
	p->data[1] = (uint8_t)(le->aa >> 8);
	p->data[2] = (uint8_t)(le->aa >> 16);
	p->data[3] = (uint8_t)(le->aa >> 24);
	le->state = ST_COLLECT;
	le->nb = 0;
	le->bsum = 0;
	le->idx = 0;
	le->pdu_end = 2;
	le->crc = le->crc_state0;
	le->crc_rx = 0;
	le->wpos = le->w0;
	le->pmin = UINT32_MAX;
	le->pmax = 0;
	le->ptotal = 0;
	le->nbytes = 0;
}

static bool addr_match(const struct ubt_le *le, const uint8_t *a)
{
	unsigned int i;

	for (i = 0; i < 6U; i++) {
		if ((a[i] & le->target_mask[i]) != le->target[i]) {
			return false;
		}
	}
	return true;
}

/* filter_match() of le_phy.c: advertising PDUs by address, data channel packets always pass. */
static bool target_match(const struct ubt_le *le, unsigned int pdu_len)
{
	const uint8_t *d = le->pkt.data;

	if (!le->target_set || le->chan_idx < 37U) {
		return true;
	}
	switch (d[4] & 0x0fU) {
	case 0x00: /* ADV_IND */
	case 0x02: /* ADV_NONCONN_IND */
	case 0x04: /* SCAN_RSP */
	case 0x06: /* ADV_SCAN_IND */
		return pdu_len >= 2U + 6U && addr_match(le, &d[6]);
	case 0x01: /* ADV_DIRECT_IND */
	case 0x03: /* SCAN_REQ */
	case 0x05: /* CONNECT_REQ */
		return pdu_len >= 2U + 6U + 6U && (addr_match(le, &d[6]) || addr_match(le, &d[12]));
	default:
		return false;
	}
}

static void finish_packet(struct ubt_le *le)
{
	usb_pkt_rx *p = &le->pkt;
	bool ok = le->crc_rx == le->crc;
	unsigned int pdu_len = le->pdu_end;

	le->state = ST_HUNT;
	le->stats.packets++;
	if (!ok) {
		le->stats.crc_bad++;
		if (le->crc_verify) {
			return;
		}
	}
	if (!target_match(le, pdu_len)) {
		le->stats.filtered++;
		return;
	}
	if (pdu_len + 3U > UBT_LE_USB_BODY) {
		le->stats.truncated++;
	}
	le->info.t_start = le->t_hit - (uint32_t)(int32_t)le->start_back;
	le->info.t_hit = le->t_hit;
	le->info.aa_rx = le->aa_rx;
	le->info.pdu_len = (uint16_t)pdu_len;
	le->info.crc_ok = ok;
	p->clk100ns = le_clk(le, le->info.t_start);
	p->rssi_max = le_rssi(le, le->pmax);
	p->rssi_min = le_rssi(le, le->pmin);
	p->rssi_avg = le_rssi(le, le->ptotal / le->nbytes);
	le->stats.emitted++;
	if (le->emit != NULL) {
		le->emit(le->ctx, p);
	}
}

/* One more byte (raw, as received) of the packet; true when the packet is over. */
UBT_HOT static bool byte_done(struct ubt_le *le, uint8_t raw, uint32_t bsum)
{
	uint8_t b = raw ^ wmask(le->wpos);
	uint32_t idx = le->idx;
	uint32_t m = bsum >> 3;

	le->wpos = wnext(le->wpos);
	le->ptotal += m;
	le->nbytes++;
	if (m < le->pmin) {
		le->pmin = m;
	}
	if (m > le->pmax) {
		le->pmax = m;
	}
	if (idx < le->pdu_end) {
		/* one byte at a time: the CRC table is private to the upstream file */
		le->crc = btle_crcgen_lut(le->crc, &b, 1);
		if (idx == 1U) {
			unsigned int limit =
				le->chan_idx >= 37U ? le->max_len_adv : le->max_len_data;

			if (b > limit) {
				le->stats.len_rejected++;
				le->state = ST_HUNT;
				return true;
			}
			le->pdu_end = 2U + b;
		}
	} else {
		le->crc_rx |= (uint32_t)b << (8U * (idx - le->pdu_end));
	}
	if (idx < UBT_LE_USB_BODY) {
		le->pkt.data[4U + idx] = b;
	}
	le->idx = (uint16_t)(idx + 1U);
	if (le->idx == le->pdu_end + 3U) {
		finish_packet(le);
		return true;
	}
	return false;
}

/*
 * Consume symbols of the packet in progress; returns how many. Eight symbols at a time when
 * aligned to a byte, one by one across chunk boundaries. A correlator hit inside the packet
 * starts a new one from that symbol.
 */
static inline __attribute__((always_inline)) size_t collect_t(struct ubt_le *le,
							      const struct gfsk_sym *s, size_t n,
							      const unsigned int tol)
{
	uint32_t sr = le->sr;
	const uint32_t tgt = le->aa_rev;
	size_t i = 0;

	while (i < n) {
		if (le->nb == 0U && n - i >= 8U) {
			uint32_t bs = 0;
			unsigned int k;

			for (k = 0; k < 8U; k++) {
				sr = (sr << 1) + s[i + k].bit;
				bs += s[i + k].pwr;
				if (near_aa(sr ^ tgt, tol)) {
					i += k + 1U;
					goto restart;
				}
			}
			i += 8U;
			le->sr = sr;
			if (byte_done(le, rev8[sr & 0xffU], bs)) {
				return i;
			}
		} else {
			sr = (sr << 1) + s[i].bit;
			le->bsum += s[i].pwr;
			i++;
			if (near_aa(sr ^ tgt, tol)) {
				goto restart;
			}
			if (++le->nb == 8U) {
				uint32_t bs = le->bsum;

				le->nb = 0;
				le->bsum = 0;
				le->sr = sr;
				if (byte_done(le, rev8[sr & 0xffU], bs)) {
					return i;
				}
			}
		}
		continue;
restart:
		le->sr = sr;
		le->stats.restarts++;
		begin_packet(le, s[i - 1U].t);
	}
	le->sr = sr;
	return n;
}

UBT_HOT static size_t collect0(struct ubt_le *le, const struct gfsk_sym *s, size_t n)
{
	return collect_t(le, s, n, 0U);
}

UBT_HOT static size_t collect1(struct ubt_le *le, const struct gfsk_sym *s, size_t n)
{
	return collect_t(le, s, n, 1U);
}

UBT_HOT static size_t collect2(struct ubt_le *le, const struct gfsk_sym *s, size_t n)
{
	return collect_t(le, s, n, 2U);
}

/* A segment of the stream without gaps. */
static void run(struct ubt_le *le, const struct gfsk_sym *s, size_t n)
{
	size_t i = 0;

	while (i < n) {
		if (le->state == ST_HUNT) {
			size_t j;

			if (le->warm != 0U) {
				size_t m = n - i < le->warm ? n - i : le->warm;
				uint32_t sr = le->sr;
				size_t k;

				for (k = 0; k < m; k++) {
					sr = (sr << 1) + s[i + k].bit;
				}
				le->sr = sr;
				le->warm = (uint8_t)(le->warm - m);
				i += m;
				continue;
			}
			switch (le->tolerance) {
			case 0:
				j = hunt0(le, s + i, n - i);
				break;
			case 1:
				j = hunt1(le, s + i, n - i);
				break;
			default:
				j = hunt2(le, s + i, n - i);
				break;
			}
			if (j == n - i) {
				return;
			}
			i += j + 1U;
			begin_packet(le, s[i - 1U].t);
		} else {
			switch (le->tolerance) {
			case 0:
				i += collect0(le, s + i, n - i);
				break;
			case 1:
				i += collect1(le, s + i, n - i);
				break;
			default:
				i += collect2(le, s + i, n - i);
				break;
			}
		}
	}
}

/* Index of the first symbol after a gap in s[i..n), or n. */
static size_t find_gap(const struct gfsk_sym *s, size_t i, size_t n)
{
	size_t k = i;

	while (k + GAP_STRIDE < n) {
		if ((uint32_t)(s[k + GAP_STRIDE].t - s[k].t) > (GAP_STRIDE - 1U) * 4U + 5U) {
			size_t j;

			for (j = k; j < k + GAP_STRIDE; j++) {
				if ((uint32_t)(s[j + 1U].t - s[j].t) > UBT_LE_GAP_SAMPLES) {
					return j + 1U;
				}
			}
		}
		k += GAP_STRIDE;
	}
	for (; k + 1U < n; k++) {
		if ((uint32_t)(s[k + 1U].t - s[k].t) > UBT_LE_GAP_SAMPLES) {
			return k + 1U;
		}
	}
	return n;
}

static void gap_reset(struct ubt_le *le)
{
	le->stats.gaps++;
	if (le->state != ST_HUNT) {
		le->stats.aborted++;
	}
	le->state = ST_HUNT;
	le->warm = WARM_AA;
	le->nb = 0;
	le->sr = 0;
}

size_t ubt_le_feed(struct ubt_le *le, const struct gfsk_sym *s, size_t n)
{
	size_t i = 0;
	uint32_t before = le->stats.emitted;

	if (n == 0U) {
		return 0;
	}
	if (le->have_t && (uint32_t)(s[0].t - le->t_last) > UBT_LE_GAP_SAMPLES) {
		gap_reset(le);
	}
	le->have_t = true;
	while (i < n) {
		size_t end = find_gap(s, i, n);

		run(le, s + i, end - i);
		i = end;
		if (i < n) {
			gap_reset(le);
		}
	}
	le->t_last = s[n - 1U].t;
	return le->stats.emitted - before;
}

/* ---- promiscuous mode ---- */

static void pm_message(struct ubt_promisc *pm, uint8_t kind, uint32_t val, unsigned int nbytes)
{
	usb_pkt_rx m;
	unsigned int i;

	memset(&m, 0, sizeof(m));
	m.pkt_type = LE_PROMISC;
	m.channel = (uint8_t)(pm->le->mhz - 2402U);
	m.data[0] = kind;
	for (i = 0; i < nbytes; i++) {
		m.data[1U + i] = (uint8_t)(val >> (8U * i));
	}
	pm->nemit++;
	if (pm->emit != NULL) {
		pm->emit(pm->emit_ctx, &m);
	}
}

static void pm_set_pattern(struct ubt_promisc *pm)
{
	uint8_t w = pm->le->w0;
	uint8_t b0 = 0x01U ^ wmask(w);
	uint8_t b1 = wmask(wnext(w));

	/* NESN and SN (header bits 2 and 3, window bits 13 and 12) match either value */
	pm->pat = (uint16_t)(((rev8[b0] << 8) | rev8[b1]) & 0xcfffU);
}

static void pm_scan_reset(struct ubt_promisc *pm)
{
	pm->h0 = 0;
	pm->h1 = 0;
	pm->warm = WARM_PROMISC;
	pm->crc_left = 0;
	pm->have_t = false;
}

void ubt_promisc_start(struct ubt_promisc *pm, uint16_t mhz)
{
	memset(pm->cache, 0, sizeof(pm->cache));
	pm->phase = UBT_PM_AA;
	pm->scan_mhz = mhz;
	pm->want_mhz = 0;
	ubt_le_set_channel(pm->le, mhz);
	ubt_le_set_aa(pm->le, UBT_LE_ADV_AA);
	ubt_le_set_crc_verify(pm->le, false);
	pm_set_pattern(pm);
	pm_scan_reset(pm);
	pm->activity_set = false;
	pm->crc_agree = 0;
	pm->prev_valid = false;
	pm->smallest = UINT64_MAX;
	pm->est_interval = 0;
	pm->consec = 0;
	pm->interval = 0;
	pm->hop = 0;
}

/* see_aa(): count an AA in the 32 entry cache, evicting the least often seen. Returns its count. */
static uint32_t pm_see_aa(struct ubt_promisc *pm, uint32_t aa)
{
	unsigned int i, kill = 0;
	uint32_t min = UINT32_MAX;

	for (i = 0; i < UBT_PROMISC_AA_SLOTS; i++) {
		if (pm->cache[i].aa == aa && pm->cache[i].count != 0U) {
			return ++pm->cache[i].count;
		}
	}
	for (i = 0; i < UBT_PROMISC_AA_SLOTS; i++) {
		if (pm->cache[i].count < min) {
			min = pm->cache[i].count;
			kill = i;
		}
	}
	pm->cache[kill].aa = aa;
	pm->cache[kill].count = 1;
	return 1;
}

/* A candidate (AA, empty PDU header, CRC) is complete: report it and count its AA. */
static bool pm_candidate(struct ubt_promisc *pm)
{
	struct ubt_le *le = pm->le;
	usb_pkt_rx p;
	uint8_t w = le->w0;
	uint32_t c = pm->h0 & 0xffffffU;
	uint32_t t_start = pm->cand_t - 16U * 4U - (uint32_t)(int32_t)le->start_back;
	unsigned int i;
	uint32_t cnt;

	if (pm->aa_validate != 0U && !ubt_le_aa_valid(pm->cand_aa)) {
		return false;
	}
	memset(&p, 0, sizeof(p));
	p.pkt_type = LE_PACKET;
	p.channel = (uint8_t)(le->mhz - 2402U);
	for (i = 0; i < 4U; i++) {
		p.data[i] = (uint8_t)(pm->cand_aa >> (8U * i));
	}
	p.data[4] = rev8[pm->cand_hdr >> 8] ^ wmask(w);
	w = wnext(w);
	p.data[5] = rev8[pm->cand_hdr & 0xffU] ^ wmask(w);
	for (i = 0; i < 3U; i++) {
		w = wnext(w);
		p.data[6U + i] = rev8[(c >> (16U - 8U * i)) & 0xffU] ^ wmask(w);
	}
	p.clk100ns = le_clk(le, t_start);
	p.rssi_max = le_rssi(le, pm->pmax);
	p.rssi_min = le_rssi(le, pm->pmin);
	p.rssi_avg = le_rssi(le, pm->psum / 24U);
	pm->nemit++;
	if (pm->emit != NULL) {
		pm->emit(pm->emit_ctx, &p);
	}
	cnt = pm_see_aa(pm, pm->cand_aa);
	return cnt >= UBT_PROMISC_AA_ADOPT;
}

/*
 * Scan the raw stream for whitened empty data PDUs. Returns the symbols used: all of them, or
 * up to the one that completed a candidate whose AA has been seen often enough (*adopted).
 */
static size_t pm_scan(struct ubt_promisc *pm, const struct gfsk_sym *s, size_t n, bool *adopted)
{
	uint32_t h0 = pm->h0, h1 = pm->h1;
	size_t i;

	for (i = 0; i < n; i++) {
		h1 = (h1 << 1) | (h0 >> 31);
		h0 = (h0 << 1) + s[i].bit;
		if (pm->warm != 0U) {
			pm->warm--;
			continue;
		}
		if (pm->crc_left != 0U) {
			uint32_t p = s[i].pwr;

			pm->psum += p;
			if (p < pm->pmin) {
				pm->pmin = p;
			}
			if (p > pm->pmax) {
				pm->pmax = p;
			}
			if (--pm->crc_left == 0U) {
				bool adopt;

				pm->h0 = h0;
				adopt = pm_candidate(pm);
				if (adopt) {
					pm->h1 = h1;
					*adopted = true;
					return i + 1U;
				}
			}
			continue;
		}
		if ((h0 & 0xcfffU) == pm->pat) {
			/* the 16 bits are a header; the 32 before them, oldest first, are the AA */
			pm->cand_aa = ubt_rev32((h1 << 16) | (h0 >> 16));
			pm->cand_hdr = (uint16_t)h0;
			pm->cand_t = s[i].t;
			pm->crc_left = 24;
			pm->psum = 0;
			pm->pmin = UINT32_MAX;
			pm->pmax = 0;
		}
	}
	pm->h0 = h0;
	pm->h1 = h1;
	return n;
}

static void pm_adopt(struct ubt_promisc *pm)
{
	pm->aa = pm->cand_aa;
	pm->phase = UBT_PM_CRC;
	pm->crc_agree = 0;
	ubt_le_set_channel(pm->le, pm->scan_mhz);
	ubt_le_set_aa(pm->le, pm->aa);
	ubt_le_set_crc_verify(pm->le, false);
	pm_message(pm, UBT_PM_MSG_AA, pm->aa, 4);
}

void ubt_promisc_start_aa(struct ubt_promisc *pm, uint16_t mhz, uint32_t aa)
{
	ubt_promisc_start(pm, mhz);
	pm->cand_aa = aa;
	pm_adopt(pm);
}

static void pm_le_emit(void *ctx, const usb_pkt_rx *pkt)
{
	struct ubt_promisc *pm = ctx;
	uint64_t t100 = ubt_clk_unwrap(&pm->unwrap, pkt->clk100ns);

	pm->nemit++;
	if (pm->emit != NULL) {
		pm->emit(pm->emit_ctx, pkt);
	}
	(void)ubt_promisc_packet(pm, pm->le->mhz, t100, pkt);
}

void ubt_promisc_init(struct ubt_promisc *pm, struct ubt_le *le, ubt_le_emit_fn emit, void *ctx)
{
	memset(pm, 0, sizeof(*pm));
	pm->le = le;
	pm->emit = emit;
	pm->emit_ctx = ctx;
	pm->aa_validate = 1;
	le->emit = pm_le_emit;
	le->ctx = pm;
}

size_t ubt_promisc_feed(struct ubt_promisc *pm, const struct gfsk_sym *s, size_t n)
{
	size_t i = 0;
	uint32_t before = pm->nemit;

	if (n == 0U) {
		return 0;
	}
	if (pm->phase != UBT_PM_AA) {
		if (pm->phase != UBT_PM_IDLE && pm->phase != UBT_PM_DONE) {
			(void)ubt_le_feed(pm->le, s, n);
		}
		return pm->nemit - before;
	}
	if (pm->have_t && (uint32_t)(s[0].t - pm->t_last) > UBT_LE_GAP_SAMPLES) {
		pm_scan_reset(pm);
	}
	pm->have_t = true;
	while (i < n) {
		size_t end = find_gap(s, i, n);
		bool adopted = false;
		size_t used = pm_scan(pm, s + i, end - i, &adopted);

		if (adopted) { /* an AA was adopted: the rest goes to the finder */
			pm_adopt(pm);
			i += used;
			(void)ubt_le_feed(pm->le, s + i, n - i);
			pm->t_last = s[n - 1U].t;
			return pm->nemit - before;
		}
		i = end;
		if (i < n) {
			pm_scan_reset(pm);
		}
	}
	pm->t_last = s[n - 1U].t;
	return pm->nemit - before;
}

static uint64_t pm_interval_ticks(const struct ubt_promisc *pm)
{
	return (uint64_t)pm->interval * LE_BASECLK;
}

unsigned int ubt_promisc_packet(struct ubt_promisc *pm, unsigned int mhz, uint64_t t100,
				const usb_pkt_rx *pkt)
{
	unsigned int next = 0;

	pm->activity_t = t100;
	pm->activity_set = true;
	switch (pm->phase) {
	case UBT_PM_CRC:
		/* An empty data PDU (LLID 1, length 0): run the CRC backwards to CRCInit. */
		if (pkt->data[5] == 0U && (pkt->data[4] & 0x03U) == 0x01U) {
			uint8_t hdr[2] = {pkt->data[4], pkt->data[5]};
			uint32_t crc = ((uint32_t)pkt->data[8] << 16) |
				       ((uint32_t)pkt->data[7] << 8) | pkt->data[6];
			uint32_t init = btle_reverse_crc(crc, hdr, 2);

			if (pm->crc_agree != 0U && init == pm->crc_cand) {
				pm->crc_agree++;
			} else {
				pm->crc_cand = init;
				pm->crc_agree = 1;
			}
			if (pm->crc_agree >= UBT_PROMISC_CRC_AGREE) {
				pm->crc_init = init;
				ubt_le_set_crc_init(pm->le, init);
				ubt_le_set_crc_verify(pm->le, true);
				pm->phase = UBT_PM_INTERVAL;
				pm->prev_valid = false;
				pm->smallest = UINT64_MAX;
				pm->est_interval = 0;
				pm->consec = 0;
				pm_message(pm, UBT_PM_MSG_CRC_INIT, init, 3);
			}
		}
		break;
	case UBT_PM_INTERVAL:
		if (!pm->prev_valid) {
			pm->prev_t = t100;
			pm->prev_valid = true;
		} else if (t100 - pm->prev_t >= 2U * LE_BASECLK) { /* a later visit */
			uint64_t diff = t100 - pm->prev_t;
			uint64_t unit = 37U * (uint64_t)LE_BASECLK;
			uint64_t obs;

			if (diff < pm->smallest) {
				pm->smallest = diff;
			}
			obs = (pm->smallest + unit / 2U) / unit;
			if (obs >= 6U && obs <= 3200U && obs == pm->est_interval) {
				if (++pm->consec >= UBT_PROMISC_INTERVALS) {
					pm->interval = (uint16_t)obs;
					pm->phase = UBT_PM_HOP_A;
					pm_message(pm, UBT_PM_MSG_INTERVAL, pm->interval, 2);
					next = 2404;
				}
			} else {
				pm->est_interval = (obs >= 6U && obs <= 3200U) ? (uint16_t)obs : 0U;
				pm->consec = 0;
			}
			pm->prev_t = t100;
		}
		break;
	case UBT_PM_HOP_A:
		/* A packet of channel 1 is the leftover of a previous round, ignore it. */
		if (mhz == 2404U) {
			pm->first_t = t100;
			pm->phase = UBT_PM_HOP_B;
			next = 2406;
		} else if (mhz != 2406U) {
			next = 2404;
		}
		break;
	case UBT_PM_HOP_B:
		if (mhz == 2406U) {
			uint64_t ticks = pm_interval_ticks(pm);
			uint64_t hops = (t100 - pm->first_t + ticks / 2U) / ticks;

			if (hops >= 1U && hops <= 36U) {
				pm->hop = hop_interval_lut[hops];
				pm->last_idx = 1;
				pm->t_last_pkt = t100;
				pm->phase = UBT_PM_DONE;
				pm_message(pm, UBT_PM_MSG_INCREMENT, pm->hop, 1);
			} else {
				pm->phase = UBT_PM_HOP_A;
				next = 2404;
			}
		} else if (mhz != 2404U) {
			/* A packet of channel 0 answers the event that started this round. */
			pm->phase = UBT_PM_HOP_A;
			next = 2404;
		}
		break;
	default:
		break;
	}
	if (next != 0U) {
		pm->want_mhz = (uint16_t)next;
	}
	return next;
}

unsigned int ubt_promisc_take_retune(struct ubt_promisc *pm)
{
	unsigned int m = pm->want_mhz;

	if (m != 0U) {
		pm->want_mhz = 0;
		ubt_le_set_channel(pm->le, (uint16_t)m);
	}
	return m;
}

unsigned int ubt_promisc_tick(struct ubt_promisc *pm, uint64_t now100)
{
	/* a channel is visited every 37 intervals, up to 148 s apart for the slowest connections */
	uint64_t limit =
		pm->interval != 0U ? 100000000ULL + 80U * pm_interval_ticks(pm) : 1600000000ULL;

	if (pm->phase < UBT_PM_CRC || pm->phase == UBT_PM_DONE) {
		return 0;
	}
	if (!pm->activity_set) {
		pm->activity_t = now100;
		pm->activity_set = true;
	}
	if (now100 - pm->activity_t <= limit) {
		return 0;
	}
	ubt_promisc_start(pm, pm->scan_mhz);
	pm->want_mhz = pm->scan_mhz;
	return pm->scan_mhz;
}
