/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Pcap app's dissector in C (esdr_pcap_dissect.h): eggs/pcap/pcap_view.py's _ble(),
 * _ble_ext(), _ble_data(), _ad(), _wpan(), _wpan_addr() and _burst() statement by statement. A
 * read past the record's end ends the dissection there, as the Python's IndexError does: what
 * was set stays, the kind becomes "malformed" (the Python assigns a statement's result only
 * once all its reads succeeded; so does this).
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esdr_pcap_dissect.h"

#define IF_BLE   0U
#define IF_WPAN  1U
#define IF_BURST 2U
#define ADV_AA   0x8E89BED6U
#define BF_SKIPPED 0x0200U
#define BURST_HDR_LEN 52U

static const char *const adv_types[] = {"ADV_IND",  "ADV_DIRECT",  "ADV_NONCONN",
					"SCAN_REQ", "SCAN_RSP",    "CONNECT_IND",
					"ADV_SCAN", "ADV_EXT",     "AUX_CONN_RSP"};
static const char *const wpan_types[] = {"Beacon", "Data", "Ack",  "Cmd",
					 "Rsvd",   "Multi", "Frag", "Ext"};
static const char *const modulations[] = {"?",   "CW",   "OOK",  "2FSK", "4FSK",
					  "MSK", "BPSK", "QPSK", "Wide"};
static const char *const phys[] = {"LE 1M", "LE 2M", "LE Coded", "?"};
static const char *const aux_phys[] = {"1M", "2M", "Coded", "?"};

struct name {
	uint16_t id;
	const char *s;
};

static const struct name ll_ctrl[] = {
	{0x00, "CONN_UPDATE"}, {0x01, "CHAN_MAP"}, {0x02, "TERMINATE"},  {0x03, "ENC_REQ"},
	{0x04, "ENC_RSP"},     {0x0C, "VERSION"},  {0x14, "LENGTH_REQ"}, {0x15, "LENGTH_RSP"},
};
static const struct name companies[] = {
	{0x004C, "Apple"},  {0x0006, "Microsoft"}, {0x0075, "Samsung"}, {0x00E0, "Google"},
	{0x0059, "Nordic"}, {0x02E5, "Espressif"}, {0x0499, "Ruuvi"},   {0x0157, "Huami"},
	{0x038F, "Xiaomi"}, {0x0087, "Garmin"},
};
static const struct name wpan_cmds[] = {
	{0x01, "AssocReq"}, {0x02, "AssocRsp"},  {0x03, "Disassoc"}, {0x04, "DataReq"},
	{0x05, "PanConflict"}, {0x06, "Orphan"}, {0x07, "BeaconReq"}, {0x08, "Realign"},
	{0x09, "GtsReq"},
};

static const char *lookup(const struct name *t, size_t n, uint32_t id)
{
	for (size_t k = 0; k < n; k++) {
		if (t[k].id == id) {
			return t[k].s;
		}
	}
	return NULL;
}

/* ---- text ---- */

static void t_clear(struct esdr_pcap_text *t)
{
	t->len = 0U;
}

static void t_add(struct esdr_pcap_text *t, const char *s, size_t n)
{
	n = n < ESDR_PCAP_FIELD_MAX - t->len ? n : ESDR_PCAP_FIELD_MAX - t->len;
	memcpy(&t->s[t->len], s, n);
	t->len = (uint16_t)(t->len + n);
}

static void t_str(struct esdr_pcap_text *t, const char *s)
{
	t_add(t, s, strlen(s));
}

static void t_fmt(struct esdr_pcap_text *t, const char *fmt, ...)
{
	char buf[64];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n > 0) {
		t_add(t, buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1U);
	}
}

static void t_set(struct esdr_pcap_text *t, const struct esdr_pcap_text *from)
{
	*t = *from;
}

/*
 * By hand what needs no printf: the board's vsnprintf() took most of a dissection (576 us a
 * packet on the S3 with it).
 */
static const char hexd[] = "0123456789abcdef";

static void t_x2(struct esdr_pcap_text *t, uint32_t b)
{
	const char s[2] = {hexd[(b >> 4) & 15U], hexd[b & 15U]};

	t_add(t, s, 2U);
}

/* "%0*x": at least width digits. */
static void t_x(struct esdr_pcap_text *t, uint32_t v, unsigned int width)
{
	char s[8];
	unsigned int n = 0;

	do {
		s[7U - n++] = hexd[v & 15U];
		v >>= 4;
	} while (v != 0U || n < width);
	t_add(t, &s[8U - n], n);
}

static void t_u(struct esdr_pcap_text *t, uint32_t v)
{
	char s[10];
	unsigned int n = 0;

	do {
		s[9U - n++] = (char)('0' + v % 10U);
		v /= 10U;
	} while (v != 0U);
	t_add(t, &s[10U - n], n);
}

/* binascii.hexlify(bytes(reversed(b)), ":"): an address, least significant byte first. */
static void t_mac(struct esdr_pcap_text *t, const uint8_t *b, size_t n)
{
	t_clear(t);
	for (size_t k = n; k > 0; k--) {
		if (k != n) {
			t_add(t, ":", 1U);
		}
		t_x2(t, b[k - 1U]);
	}
}

static void t_hex(struct esdr_pcap_text *t, const uint8_t *b, size_t n)
{
	for (size_t k = 0; k < n; k++) {
		t_x2(t, b[k]);
	}
}

/* bytes.decode()'s check: well-formed UTF-8, no overlong form, surrogate or code past U+10FFFF. */
static bool utf8_ok(const uint8_t *s, size_t n)
{
	size_t k = 0;

	while (k < n) {
		uint8_t c = s[k];
		size_t more;
		uint32_t cp, min;

		if (c < 0x80U) {
			k++;
			continue;
		} else if ((c & 0xE0U) == 0xC0U) {
			more = 1U;
			cp = c & 0x1FU;
			min = 0x80U;
		} else if ((c & 0xF0U) == 0xE0U) {
			more = 2U;
			cp = c & 0x0FU;
			min = 0x800U;
		} else if ((c & 0xF8U) == 0xF0U) {
			more = 3U;
			cp = c & 0x07U;
			min = 0x10000U;
		} else {
			return false;
		}
		for (size_t j = 1; j <= more; j++) {
			if (k + j >= n || (s[k + j] & 0xC0U) != 0x80U) {
				return false;
			}
			cp = (cp << 6) | (s[k + j] & 0x3FU);
		}
		if (cp < min || cp > 0x10FFFFU || (cp >= 0xD800U && cp <= 0xDFFFU)) {
			return false;
		}
		k += 1U + more;
	}
	return true;
}

/*
 * _eng(): 50000 -> "50 k" + unit, "{:.2f}" of the value over the step, trailing zeros dropped.
 * The hundredths in integers: rounded as printf rounds the double v / div unless v / div lies
 * exactly halfway, where only the double's own error decides (printf then).
 */
static void t_eng(struct esdr_pcap_text *t, uint32_t v, const char *unit, bool space)
{
	static const struct {
		uint32_t div;
		const char *pre;
	} steps[] = {{1000000U, "M"}, {1000U, "k"}};

	for (size_t k = 0; k < 2U; k++) {
		const uint32_t div = steps[k].div;

		if (v >= div) {
			const uint64_t x = (uint64_t)v * 100U;
			uint64_t h = x / div;
			const uint64_t rem = x % div;
			char buf[32];
			int n;

			if (2U * rem == div) {
				n = snprintf(buf, sizeof(buf), "%.2f", (double)v / (double)div);
			} else {
				struct esdr_pcap_text d = {0};

				if (2U * rem > div) {
					h++;
				}
				t_u(&d, (uint32_t)(h / 100U));
				t_add(&d, ".", 1U);
				t_add(&d, &"0123456789"[h % 100U / 10U], 1U);
				t_add(&d, &"0123456789"[h % 10U], 1U);
				memcpy(buf, d.s, d.len);
				n = d.len;
			}
			while (n > 0 && buf[n - 1] == '0') {
				n--;
			}
			while (n > 0 && buf[n - 1] == '.') {
				n--;
			}
			t_add(t, buf, (size_t)n);
			t_str(t, space ? " " : "");
			t_str(t, steps[k].pre);
			t_str(t, unit);
			return;
		}
	}
	t_u(t, v);
	t_str(t, space ? " " : "");
	t_str(t, unit);
}

/* ---- reads that may run past the end ---- */

struct rd {
	const uint8_t *p;
	size_t len;
	bool bad;
};

static uint32_t u8(struct rd *r, size_t at)
{
	if (at >= r->len) {
		r->bad = true;
		return 0U;
	}
	return r->p[at];
}

static uint32_t u16(struct rd *r, size_t at)
{
	return u8(r, at) | (u8(r, at + 1U) << 8);
}

static uint32_t u32(struct rd *r, size_t at)
{
	return u16(r, at) | (u16(r, at + 2U) << 16);
}

/* A slice: from at, n bytes at most (none past the end). */
static size_t span(const struct rd *r, size_t at, size_t n)
{
	if (at >= r->len) {
		return 0U;
	}
	return r->len - at < n ? r->len - at : n;
}

/* ---- Bluetooth LE ---- */

static uint32_t ble_channel(uint32_t rf)
{
	if (rf == 0U) {
		return 37U;
	}
	if (rf == 12U) {
		return 38U;
	}
	if (rf == 39U) {
		return 39U;
	}
	return rf < 12U ? rf - 1U : rf - 2U;
}

static void ad(const uint8_t *d, size_t n, struct esdr_pcap_text *info)
{
	struct esdr_pcap_text name = {0}, maker = {0};
	size_t k = 0;

	while (k + 1U < n) {
		const size_t m = d[k];
		const uint8_t t = d[k + 1U];
		const uint8_t *v = &d[k + 2U];
		const size_t vn = m - 1U;

		if (m == 0U || k + 1U + m > n) {
			break;
		}
		if (t == 0x08U || t == 0x09U) {
			t_clear(&name);
			if (utf8_ok(v, vn)) {
				t_add(&name, (const char *)v, vn);
			} else {
				t_hex(&name, v, vn);
			}
		} else if (t == 0xFFU && vn >= 2U) {
			const uint32_t cid = v[0] | ((uint32_t)v[1] << 8);
			const char *s = lookup(companies, sizeof(companies) / sizeof(companies[0]), cid);

			t_clear(&maker);
			if (s != NULL) {
				t_str(&maker, s);
			} else {
				t_str(&maker, "0x");
				t_x(&maker, cid, 4U);
			}
		}
		k += 1U + m;
	}
	t_clear(info);
	if (name.len != 0U) {
		t_str(info, "\"");
		t_add(info, name.s, name.len);
		t_str(info, "\"");
	}
	if (maker.len != 0U) {
		if (info->len != 0U) {
			t_str(info, " ");
		}
		t_add(info, maker.s, maker.len);
	}
}

static void ble_ext(const uint8_t *pl, size_t n, struct esdr_pcap_fields *f)
{
	const size_t hl = n > 0U ? (pl[0] & 0x3FU) : 0U;
	struct rd r = {pl, n, false};
	uint32_t fl;
	size_t k = 2;

	if (hl == 0U || n < 1U + hl) {
		return;
	}
	fl = pl[1];
	if ((fl & 0x01U) != 0U) {
		t_mac(&f->src, &pl[k < n ? k : n], span(&r, k, 6U));
		k += 6U;
	}
	if ((fl & 0x02U) != 0U) {
		t_mac(&f->dst, &pl[k < n ? k : n], span(&r, k, 6U));
		k += 6U;
	}
	if ((fl & 0x04U) != 0U) {
		k += 1U;
	}
	if ((fl & 0x08U) != 0U) {
		k += 2U;
	}
	if ((fl & 0x10U) != 0U && k + 3U <= 1U + hl) {
		t_clear(&f->info);
		t_str(&f->info, "aux ch ");
		t_u(&f->info, pl[k] & 0x3FU);
		t_str(&f->info, " ");
		t_str(&f->info, aux_phys[(pl[k + 2U] >> 5) & 3U]);
	}
}

static void ble_data(struct esdr_pcap_fields *f, uint32_t aa, uint32_t h0, uint32_t n,
		     struct rd *pl)
{
	const uint32_t llid = h0 & 3U;

	t_clear(&f->src);
	t_str(&f->src, "AA ");
	t_x(&f->src, aa, 8U);
	if (llid == 3U && n != 0U) {
		const uint32_t op = u8(pl, 0);
		const char *s = lookup(ll_ctrl, sizeof(ll_ctrl) / sizeof(ll_ctrl[0]), op);

		if (pl->bad) {
			return;
		}
		t_clear(&f->kind);
		t_str(&f->kind, "LL_");
		if (s != NULL) {
			t_str(&f->kind, s);
		} else {
			t_str(&f->kind, "CTRL 0x");
			t_x(&f->kind, op, 2U);
		}
	} else {
		t_clear(&f->kind);
		t_str(&f->kind, n == 0U ? "Empty" : llid == 2U ? "L2CAP" : "L2CAP+");
	}
	t_clear(&f->info);
	t_str(&f->info, ((h0 >> 3) & 1U) != 0U ? "SN 1" : "SN 0");
	t_str(&f->info, ((h0 >> 2) & 1U) != 0U ? " NESN 1" : " NESN 0");
	t_str(&f->info, (h0 & 0x10U) != 0U ? " MD" : "");
}

/* False: a read past the end. */
static bool ble(struct rd *r, struct esdr_pcap_fields *f)
{
	uint32_t rf, flags, aa, h0, n, t;
	size_t pn;
	const uint8_t *pl;

	rf = u8(r, 0);
	flags = u16(r, 8);
	if (r->bad) {
		return false;
	}
	aa = u32(r, 10);
	if (r->bad) {
		return false;
	}
	t_clear(&f->channel);
	t_u(&f->channel, ble_channel(rf));
	t_clear(&f->phy);
	t_str(&f->phy, phys[(flags >> 14) & 3U]);
	if ((flags & 0x0400U) != 0U) {
		f->ok = (flags & 0x0800U) != 0U ? 1 : 0;
	}
	h0 = u8(r, 14);
	n = u8(r, 15);
	if (r->bad) {
		return false;
	}
	pn = span(r, 16, n);
	pl = &r->p[16U < r->len ? 16U : r->len];
	f->length = n + 2U;
	if (aa != ADV_AA) {
		struct rd prd = {pl, pn, false};

		ble_data(f, aa, h0, n, &prd);
		return !prd.bad;
	}
	t = h0 & 0x0FU;
	t_clear(&f->kind);
	if (t < sizeof(adv_types) / sizeof(adv_types[0])) {
		t_str(&f->kind, adv_types[t]);
	} else {
		t_str(&f->kind, "ADV 0x");
		t_x(&f->kind, t, 1U);
	}
	f->random = (h0 & 0x40U) != 0U ? 1U : 0U;
	if ((t == 0U || t == 2U || t == 4U || t == 6U) && pn >= 6U) {
		t_mac(&f->src, pl, 6U);
		ad(pl + 6U, pn - 6U, &f->info);
	} else if ((t == 1U || t == 3U) && pn >= 12U) {
		t_mac(&f->src, pl, 6U);
		t_mac(&f->dst, pl + 6U, 6U);
	} else if (t == 5U && pn >= 34U) {
		struct rd prd = {pl, pn, false};
		const uint32_t caa = u32(&prd, 12);
		const double interval = u16(&prd, 22) * 1.25;

		t_mac(&f->src, pl, 6U);
		t_mac(&f->dst, pl + 6U, 6U);
		t_clear(&f->info);
		t_fmt(&f->info, "AA %08x %g ms", (unsigned int)caa, interval);
	} else if (t == 7U && pn >= 2U) {
		ble_ext(pl, pn, f);
	}
	return true;
}

/* ---- IEEE 802.15.4 ---- */

/* _wpan_addr(): the address of a mode at k, and where the next field starts; false past the end. */
static bool wpan_addr(struct rd *p, size_t *k, uint32_t mode, struct esdr_pcap_text *out)
{
	t_clear(out);
	if (mode == 2U) {
		const uint32_t a = u8(p, *k) | (u8(p, *k + 1U) << 8);

		if (p->bad) {
			return false;
		}
		t_x(out, a, 4U);
		*k += 2U;
	} else if (mode == 3U) {
		uint8_t b[8];

		for (int j = 7; j >= 0; j--) {
			b[7 - j] = (uint8_t)u8(p, *k + (size_t)j);
			if (p->bad) {
				return false;
			}
		}
		t_hex(out, b, 8U);
		*k += 8U;
	}
	return true;
}

static bool wpan(struct rd *r, struct esdr_pcap_fields *f)
{
	const uint32_t hlen = u16(r, 2);
	struct esdr_pcap_text dpan = {0}, span_ = {0}, dst = {0}, src = {0};
	struct rd p;
	uint32_t fcf, ft, ver, dmode, smode;
	size_t k = 4;
	bool seq_on = false;
	uint32_t seq = 0;

	if (r->bad) {
		return false;
	}
	while (k + 4U <= hlen) {
		const uint32_t t = u16(r, k), n = u16(r, k + 2U);

		if (r->bad) {
			return false;
		}
		if (t == 3U && n >= 2U) {
			const uint32_t ch = u16(r, k + 4U);

			if (r->bad) {
				return false;
			}
			t_clear(&f->channel);
			t_u(&f->channel, ch);
		}
		k += 4U + (n + 3U) / 4U * 4U;
	}
	p.p = &r->p[hlen < r->len ? hlen : r->len];
	p.len = hlen < r->len ? r->len - hlen : 0U;
	p.bad = false;
	t_clear(&f->phy);
	t_str(&f->phy, "O-QPSK");
	f->length = (uint32_t)p.len;
	fcf = u8(&p, 0) | (u8(&p, 1) << 8);
	if (p.bad) {
		return false;
	}
	ft = fcf & 7U;
	t_clear(&f->kind);
	t_str(&f->kind, wpan_types[ft]);
	ver = (fcf >> 12) & 3U;
	dmode = (fcf >> 10) & 3U;
	smode = (fcf >> 14) & 3U;
	k = 2;
	if (!(ver == 2U && (fcf & 0x100U) != 0U)) {
		seq = u8(&p, 2);
		if (p.bad) {
			return false;
		}
		seq_on = true;
		k = 3;
	}
	if (dmode != 0U) {
		const uint32_t v = u8(&p, k) | (u8(&p, k + 1U) << 8);

		if (p.bad) {
			return false;
		}
		t_x(&dpan, v, 4U);
		k += 2U;
	}
	if (!wpan_addr(&p, &k, dmode, &dst)) {
		return false;
	}
	if (smode != 0U) {
		if ((fcf & 0x40U) == 0U) {
			const uint32_t v = u8(&p, k) | (u8(&p, k + 1U) << 8);

			if (p.bad) {
				return false;
			}
			t_x(&span_, v, 4U);
			k += 2U;
		} else {
			t_set(&span_, &dpan);
		}
	}
	if (!wpan_addr(&p, &k, smode, &src)) {
		return false;
	}
	if (src.len != 0U) {
		t_clear(&f->src);
		if (span_.len != 0U) {
			t_add(&f->src, span_.s, span_.len);
			t_str(&f->src, ":");
		}
		t_add(&f->src, src.s, src.len);
	}
	if (dst.len != 0U) {
		t_clear(&f->dst);
		if (dpan.len != 0U) {
			t_add(&f->dst, dpan.s, dpan.len);
			t_str(&f->dst, ":");
		}
		t_add(&f->dst, dst.s, dst.len);
	}
	if (ft == 3U && (fcf & 0x08U) == 0U && (long)k < (long)p.len - 2L) {
		const uint32_t c = p.p[k];
		const char *s = lookup(wpan_cmds, sizeof(wpan_cmds) / sizeof(wpan_cmds[0]), c);

		t_clear(&f->kind);
		if (s != NULL) {
			t_str(&f->kind, s);
		} else {
			t_str(&f->kind, "Cmd 0x");
			t_x(&f->kind, c, 2U);
		}
	}
	t_clear(&f->info);
	if (seq_on) {
		t_str(&f->info, "#");
		t_u(&f->info, seq);
	}
	if ((fcf & 0x20U) != 0U) {
		t_str(&f->info, f->info.len != 0U ? " ack?" : "ack?");
	}
	if ((fcf & 0x08U) != 0U) {
		t_str(&f->info, f->info.len != 0U ? " sec" : "sec");
	}
	if ((fcf & 0x10U) != 0U) {
		t_str(&f->info, f->info.len != 0U ? " pending" : "pending");
	}
	return true;
}

/* ---- burst records ---- */

static bool burst(struct rd *r, struct esdr_pcap_fields *f)
{
	uint32_t flags, bw, rate, dev, mod, nbits;

	if (r->len < BURST_HDR_LEN) {
		return false;
	}
	flags = u16(r, 2);
	bw = u32(r, 16);
	rate = u32(r, 24);
	dev = u32(r, 28);
	mod = u8(r, 32);
	nbits = u32(r, 48);
	t_clear(&f->phy);
	t_str(&f->phy, mod < sizeof(modulations) / sizeof(modulations[0]) ? modulations[mod] : "?");
	t_set(&f->kind, &f->phy);
	f->length = (uint32_t)(((uint64_t)nbits + 7U) / 8U);
	if ((flags & BF_SKIPPED) != 0U) {
		t_clear(&f->kind);
		t_str(&f->kind, "?");
	}
	t_clear(&f->info);
	t_clear(&f->src);
	if (rate != 0U) {
		t_eng(&f->info, rate, "Bd", true);
		t_add(&f->src, f->kind.s, f->kind.len);
		t_str(&f->src, " ");
		t_eng(&f->src, rate, "Bd", false);
	} else {
		t_set(&f->src, &f->kind);
	}
	if (bw != 0U) {
		t_str(&f->info, f->info.len != 0U ? " bw " : "bw ");
		t_eng(&f->info, bw, "Hz", true);
	}
	if (dev != 0U) {
		t_str(&f->info, f->info.len != 0U ? " dev " : "dev ");
		t_eng(&f->info, dev, "Hz", true);
	}
	if (nbits != 0U) {
		t_str(&f->info, f->info.len != 0U ? " " : "");
		t_u(&f->info, nbits);
		t_str(&f->info, " bits");
	}
	if ((flags & BF_SKIPPED) != 0U) {
		t_str(&f->info, f->info.len != 0U ? " not analysed" : "not analysed");
	}
	return true;
}

void esdr_pcap_dissect(uint32_t iface, const uint8_t *rec, size_t len,
		       struct esdr_pcap_fields *f)
{
	struct rd r = {rec, len, false};
	bool good;

	t_clear(&f->channel);
	t_clear(&f->phy);
	t_clear(&f->kind);
	t_clear(&f->src);
	t_clear(&f->dst);
	t_clear(&f->info);
	f->length = 0U;
	f->ok = -1;
	f->random = 0U;
	switch (iface) {
	case IF_BLE:
		good = ble(&r, f);
		break;
	case IF_WPAN:
		good = wpan(&r, f);
		break;
	case IF_BURST:
		good = burst(&r, f);
		break;
	default:
		good = false;
		break;
	}
	if (!good) {
		t_clear(&f->kind);
		t_str(&f->kind, "malformed");
	}
}
