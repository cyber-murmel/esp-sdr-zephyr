/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit test of apps/pcap's pcapng writer (pcapng.c) and packet records (records.c): every block
 * type byte for byte against vectors spelled out from the specification, -ENOSPC one byte short
 * and success at the exact size with nothing written past it, a whole stream parsed back by a
 * reader of its own, the BLE header's flag combinations, the TAP header's TLVs, the burst
 * record's bit padding, and every branch of the comment's format.
 */
#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "pcapng.h"
#include "records.h"

#define GUARD 0xA5U

static uint8_t buf[1024];

static bool all_guard(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (p[i] != GUARD) {
			return false;
		}
	}
	return true;
}

/* Byte for byte, naming the first difference. */
static void expect_bytes(const uint8_t *got, const uint8_t *exp, size_t n, const char *what)
{
	for (size_t i = 0; i < n; i++) {
		zassert_equal(got[i], exp[i], "%s: byte %zu is 0x%02x, expected 0x%02x", what, i,
			      got[i], exp[i]);
	}
}

/*
 * @p call, an expression over buf and cap, with cap one byte short: -ENOSPC and the buffer
 * untouched; with cap exact: the length, the expected bytes, the guard after them.
 */
#define CHECK_WRITE(what, exp, call)                                                               \
	do {                                                                                       \
		size_t cap = sizeof(exp) - 1U;                                                     \
		int r;                                                                             \
		memset(buf, GUARD, sizeof(buf));                                                   \
		r = (call);                                                                        \
		zassert_equal(r, -ENOSPC, "%s: cap %zu returned %d", what, cap, r);                \
		zassert_true(all_guard(buf, sizeof(buf)), "%s: wrote at -ENOSPC", what);           \
		cap = sizeof(exp);                                                                 \
		r = (call);                                                                        \
		zassert_equal(r, (int)sizeof(exp), "%s: cap %zu returned %d", what, cap, r);       \
		expect_bytes(buf, exp, sizeof(exp), what);                                         \
		zassert_true(all_guard(buf + sizeof(exp), sizeof(buf) - sizeof(exp)),              \
			     "%s: wrote past the block", what);                                    \
	} while (0)

/* ---- readers for the checks ---- */

static uint16_t get_u16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
	return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_u64(const uint8_t *p)
{
	return get_u32(p) | ((uint64_t)get_u32(p + 4) << 32);
}

/* A pcapng timestamp: high word first. */
static uint64_t get_ts(const uint8_t *p)
{
	return ((uint64_t)get_u32(p) << 32) | get_u32(p + 4);
}

struct block {
	uint32_t type;
	/* Between the two length words. */
	const uint8_t *body;
	size_t len;
};

/* The block at *pos of the stream s (n bytes), its framing checked; false at the end. */
static bool next_block(const uint8_t *s, size_t n, size_t *pos, struct block *b)
{
	size_t total;

	if (*pos == n) {
		return false;
	}
	zassert_true(*pos + 12U <= n, "block truncated at %zu", *pos);
	b->type = get_u32(s + *pos);
	total = get_u32(s + *pos + 4);
	zassert_true(total % 4U == 0U && total >= 12U && *pos + total <= n,
		     "bad total length %zu at %zu", total, *pos);
	zassert_equal(get_u32(s + *pos + total - 4U), (uint32_t)total, "trailer at %zu", *pos);
	b->body = s + *pos + 8;
	b->len = total - 12U;
	*pos += total;
	return true;
}

/*
 * The first option @p code of the list opts (n bytes, 0 for no list), its value length in *len;
 * NULL when absent. The list must end with opt_endofopt and nothing after it.
 */
static const uint8_t *find_opt(const uint8_t *opts, size_t n, uint16_t code, size_t *len)
{
	const uint8_t *found = NULL;
	size_t pos = 0;

	if (n == 0U) {
		return NULL;
	}
	for (;;) {
		uint16_t c, l;

		zassert_true(pos + 4U <= n, "options truncated at %zu", pos);
		c = get_u16(opts + pos);
		l = get_u16(opts + pos + 2);
		pos += 4U;
		if (c == PCAPNG_OPT_ENDOFOPT) {
			zassert_equal(l, 0, "opt_endofopt with length %u", l);
			zassert_equal(pos, n, "%zu bytes after opt_endofopt", n - pos);
			break;
		}
		zassert_true(pos + ((l + 3U) & ~3U) <= n, "option %u truncated", c);
		if (c == code && found == NULL) {
			found = opts + pos;
			*len = l;
		}
		pos += (l + 3U) & ~3U;
	}
	return found;
}

static void expect_str_opt(const uint8_t *opts, size_t n, uint16_t code, const char *exp)
{
	size_t len = 0;
	const uint8_t *v = find_opt(opts, n, code, &len);

	zassert_not_null(v, "option %u missing", code);
	zassert_equal(len, strlen(exp), "option %u: length %zu", code, len);
	zassert_mem_equal(v, exp, len, "option %u differs", code);
}

static uint64_t expect_u64_opt(const uint8_t *opts, size_t n, uint16_t code)
{
	size_t len = 0;
	const uint8_t *v = find_opt(opts, n, code, &len);

	zassert_not_null(v, "option %u missing", code);
	zassert_equal(len, 8, "option %u: length %zu", code, len);
	return get_u64(v);
}

/* ---- the vectors, spelled out from the specification ---- */

/* SHB: shb_hardware "hw", shb_os "os1", shb_userappl "app!" (padding 2, 1 and 0). */
static const uint8_t shb3[] = {
	0x0A, 0x0D, 0x0D, 0x0A, 0x38, 0x00, 0x00, 0x00,
	0x4D, 0x3C, 0x2B, 0x1A, 0x01, 0x00, 0x00, 0x00,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0x02, 0x00, 0x02, 0x00, 'h', 'w', 0x00, 0x00,
	0x03, 0x00, 0x03, 0x00, 'o', 's', '1', 0x00,
	0x04, 0x00, 0x04, 0x00, 'a', 'p', 'p', '!',
	0x00, 0x00, 0x00, 0x00,
	0x38, 0x00, 0x00, 0x00,
};

/* SHB without options. */
static const uint8_t shb0[] = {
	0x0A, 0x0D, 0x0D, 0x0A, 0x1C, 0x00, 0x00, 0x00,
	0x4D, 0x3C, 0x2B, 0x1A, 0x01, 0x00, 0x00, 0x00,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0x1C, 0x00, 0x00, 0x00,
};

/* SHB with shb_os "Zephyr" alone. */
static const uint8_t shb_os[] = {
	0x0A, 0x0D, 0x0D, 0x0A, 0x2C, 0x00, 0x00, 0x00,
	0x4D, 0x3C, 0x2B, 0x1A, 0x01, 0x00, 0x00, 0x00,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0x03, 0x00, 0x06, 0x00, 'Z', 'e', 'p', 'h', 'y', 'r', 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
	0x2C, 0x00, 0x00, 0x00,
};

/* IDB: link type 256, snaplen 65536, if_name "ble", if_description "d", if_speed 1 Mbit/s,
 * if_tsresol 9.
 */
static const uint8_t idb_full[] = {
	0x01, 0x00, 0x00, 0x00, 0x3C, 0x00, 0x00, 0x00,
	0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
	0x02, 0x00, 0x03, 0x00, 'b', 'l', 'e', 0x00,
	0x03, 0x00, 0x01, 0x00, 'd', 0x00, 0x00, 0x00,
	0x08, 0x00, 0x08, 0x00, 0x40, 0x42, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x09, 0x00, 0x01, 0x00, 0x09, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
	0x3C, 0x00, 0x00, 0x00,
};

/* IDB without options: link type 147, snaplen 0. */
static const uint8_t idb_bare[] = {
	0x01, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00,
	0x93, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x14, 0x00, 0x00, 0x00,
};

/* IDB with if_hardware "radio" alone: link type 283. */
static const uint8_t idb_hw[] = {
	0x01, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00,
	0x1B, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x0F, 0x00, 0x05, 0x00, 'r', 'a', 'd', 'i', 'o', 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
	0x24, 0x00, 0x00, 0x00,
};

/* EPB, no data: interface 2, timestamp 0x0000000100000002. */
static const uint8_t epb0[] = {
	0x06, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x20, 0x00, 0x00, 0x00,
};

/* EPB, 1 data byte: interface 0, timestamp 0. */
static const uint8_t epb1[] = {
	0x06, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0xAB, 0x00, 0x00, 0x00,
	0x24, 0x00, 0x00, 0x00,
};

/* EPB, 2 data bytes: interface 1, timestamp 0x00000000FFFFFFFF. */
static const uint8_t epb2[] = {
	0x06, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
	0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0xAB, 0xCD, 0x00, 0x00,
	0x24, 0x00, 0x00, 0x00,
};

/* EPB, 3 data bytes: interface 2, timestamp 0x0000000100000000. */
static const uint8_t epb3[] = {
	0x06, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x03, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
	0xAB, 0xCD, 0xEF, 0x00,
	0x24, 0x00, 0x00, 0x00,
};

/* EPB, 4 data bytes: interface 0, timestamp 0x123456789ABCDEF0. */
static const uint8_t epb4[] = {
	0x06, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
	0x78, 0x56, 0x34, 0x12, 0xF0, 0xDE, 0xBC, 0x9A,
	0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
	0xAB, 0xCD, 0xEF, 0x01,
	0x24, 0x00, 0x00, 0x00,
};

/* EPB, 3 data bytes, opt_comment "hi", epb_flags inbound, epb_dropcount 5: interface 1,
 * timestamp 0x123456789ABCDEF0.
 */
static const uint8_t epb_opts[] = {
	0x06, 0x00, 0x00, 0x00, 0x44, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00,
	0x78, 0x56, 0x34, 0x12, 0xF0, 0xDE, 0xBC, 0x9A,
	0x03, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
	0x01, 0x02, 0x03, 0x00,
	0x01, 0x00, 0x02, 0x00, 'h', 'i', 0x00, 0x00,
	0x02, 0x00, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x08, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
	0x44, 0x00, 0x00, 0x00,
};

/* EPB, 4 data bytes, epb_dropcount 0 alone: interface 0, timestamp 1. */
static const uint8_t epb_drop0[] = {
	0x06, 0x00, 0x00, 0x00, 0x34, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
	0xDE, 0xAD, 0xBE, 0xEF,
	0x04, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
	0x34, 0x00, 0x00, 0x00,
};

/* EPB, no data, opt_comment "abc" alone: interface 0, timestamp 0. */
static const uint8_t epb_comment[] = {
	0x06, 0x00, 0x00, 0x00, 0x2C, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x03, 0x00, 'a', 'b', 'c', 0x00,
	0x00, 0x00, 0x00, 0x00,
	0x2C, 0x00, 0x00, 0x00,
};

/* EPB, no data, epb_flags inbound alone: interface 0, timestamp 0. */
static const uint8_t epb_flags[] = {
	0x06, 0x00, 0x00, 0x00, 0x2C, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
	0x2C, 0x00, 0x00, 0x00,
};

/* ISB without options: interface 1, timestamp 0x0000000200000003. */
static const uint8_t isb_bare[] = {
	0x05, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
	0x18, 0x00, 0x00, 0x00,
};

/* ISB with isb_starttime 0x00000000FFFFFFFF (high word first), isb_ifrecv 0x0102030405060708,
 * isb_ifdrop 7: interface 0, timestamp 0x0000000100000000.
 */
static const uint8_t isb_full[] = {
	0x05, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
	0x04, 0x00, 0x08, 0x00, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
	0x05, 0x00, 0x08, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
	0x40, 0x00, 0x00, 0x00,
};

/* ISB with isb_ifrecv 1 alone: interface 2, timestamp 0. */
static const uint8_t isb_recv[] = {
	0x05, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x08, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
	0x28, 0x00, 0x00, 0x00,
};

/* ---- the blocks ---- */

ZTEST(pcapng, test_shb)
{
	static char long_str[65537];

	CHECK_WRITE("shb3", shb3, pcapng_shb(buf, cap, "hw", "os1", "app!"));
	CHECK_WRITE("shb0", shb0, pcapng_shb(buf, cap, NULL, NULL, NULL));
	CHECK_WRITE("shb_os", shb_os, pcapng_shb(buf, cap, NULL, "Zephyr", NULL));

	/* No buffer with room, a string beyond an option's 16-bit length. */
	zassert_equal(pcapng_shb(NULL, 100, "hw", NULL, NULL), -EINVAL);
	zassert_equal(pcapng_shb(NULL, 0, "hw", NULL, NULL), -ENOSPC);
	memset(long_str, 'a', sizeof(long_str) - 1U);
	long_str[sizeof(long_str) - 1U] = '\0';
	zassert_equal(pcapng_shb(buf, sizeof(buf), long_str, NULL, NULL), -EINVAL);
	long_str[65535] = '\0';
	zassert_equal(pcapng_shb(buf, sizeof(buf), long_str, NULL, NULL), -ENOSPC);
}

ZTEST(pcapng, test_idb)
{
	const struct pcapng_idb full = {
		.linktype = 256,
		.snaplen = 65536,
		.name = "ble",
		.description = "d",
		.tsresol = 9,
		.speed_bps = 1000000,
	};
	const struct pcapng_idb bare = {.linktype = 147};
	const struct pcapng_idb hw = {.linktype = 283, .hardware = "radio"};

	CHECK_WRITE("idb_full", idb_full, pcapng_idb(buf, cap, &full));
	CHECK_WRITE("idb_bare", idb_bare, pcapng_idb(buf, cap, &bare));
	CHECK_WRITE("idb_hw", idb_hw, pcapng_idb(buf, cap, &hw));
	zassert_equal(pcapng_idb(buf, sizeof(buf), NULL), -EINVAL);
}

ZTEST(pcapng, test_epb)
{
	static const uint8_t data[] = {0xAB, 0xCD, 0xEF, 0x01};
	static const uint8_t dead[] = {0xDE, 0xAD, 0xBE, 0xEF};
	static const uint8_t one23[] = {1, 2, 3};
	struct pcapng_epb e = {.dropcount = UINT64_MAX};
	int r;

	e.interface = 2;
	e.timestamp = 0x0000000100000002ULL;
	CHECK_WRITE("epb0", epb0, pcapng_epb(buf, cap, &e));

	e.interface = 0;
	e.timestamp = 0;
	e.data = data;
	e.len = 1;
	CHECK_WRITE("epb1", epb1, pcapng_epb(buf, cap, &e));
	/* An empty comment is no option. */
	e.comment = "";
	CHECK_WRITE("epb1 empty comment", epb1, pcapng_epb(buf, cap, &e));
	e.comment = NULL;

	e.interface = 1;
	e.timestamp = 0x00000000FFFFFFFFULL;
	e.len = 2;
	CHECK_WRITE("epb2", epb2, pcapng_epb(buf, cap, &e));

	e.interface = 2;
	e.timestamp = 0x0000000100000000ULL;
	e.len = 3;
	CHECK_WRITE("epb3", epb3, pcapng_epb(buf, cap, &e));

	e.interface = 0;
	e.timestamp = 0x123456789ABCDEF0ULL;
	e.len = 4;
	CHECK_WRITE("epb4", epb4, pcapng_epb(buf, cap, &e));

	e.interface = 1;
	e.data = one23;
	e.len = 3;
	e.comment = "hi";
	e.flags = PCAPNG_EPB_FLAG_INBOUND;
	e.dropcount = 5;
	CHECK_WRITE("epb_opts", epb_opts, pcapng_epb(buf, cap, &e));

	e.interface = 0;
	e.timestamp = 1;
	e.data = dead;
	e.len = 4;
	e.comment = NULL;
	e.flags = 0;
	e.dropcount = 0;
	CHECK_WRITE("epb_drop0", epb_drop0, pcapng_epb(buf, cap, &e));

	e.timestamp = 0;
	e.data = NULL;
	e.len = 0;
	e.comment = "abc";
	e.dropcount = UINT64_MAX;
	CHECK_WRITE("epb_comment", epb_comment, pcapng_epb(buf, cap, &e));

	e.comment = NULL;
	e.flags = PCAPNG_EPB_FLAG_INBOUND;
	CHECK_WRITE("epb_flags", epb_flags, pcapng_epb(buf, cap, &e));

	/* Data missing, a comment beyond an option's length. */
	e.flags = 0;
	e.len = 1;
	zassert_equal(pcapng_epb(buf, sizeof(buf), &e), -EINVAL);
	e.len = 0;
	zassert_equal(pcapng_epb(buf, sizeof(buf), NULL), -EINVAL);

	/* The size function agrees with the writer for every option combination. */
	for (uint32_t len = 0; len < 9U; len++) {
		for (unsigned int opt = 0; opt < 4U; opt++) {
			static const char *const comments[] = {NULL, "", "a", "abcd", "abcde"};
			static const uint8_t grid_data[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9};

			for (size_t c = 0; c < ARRAY_SIZE(comments); c++) {
				size_t comment_len = comments[c] != NULL ? strlen(comments[c]) : 0U;
				size_t exp;

				e.data = grid_data;
				e.len = len;
				e.comment = comments[c];
				e.flags = (opt & 1U) ? PCAPNG_EPB_FLAG_INBOUND : 0U;
				e.dropcount = (opt & 2U) ? 0U : UINT64_MAX;
				exp = pcapng_epb_size(len, comment_len, e.flags != 0U,
						      e.dropcount != UINT64_MAX);
				r = pcapng_epb(buf, sizeof(buf), &e);
				zassert_equal(r, (int)exp, "len %u opt %u comment %zu: %d vs %zu",
					      len, opt, comment_len, r, exp);
				zassert_equal(get_u32(buf + 4), (uint32_t)exp, "length word");
				zassert_equal(get_u32(buf + exp - 4U), (uint32_t)exp, "trailer");
			}
		}
	}
	zassert_equal(pcapng_epb_size(0, 0, 0, 0), 32, "bare EPB");
}

ZTEST(pcapng, test_isb)
{
	const struct pcapng_isb bare = {
		.interface = 1,
		.timestamp = 0x0000000200000003ULL,
		.starttime = UINT64_MAX,
		.ifrecv = UINT64_MAX,
		.ifdrop = UINT64_MAX,
	};
	const struct pcapng_isb full = {
		.interface = 0,
		.timestamp = 0x0000000100000000ULL,
		.starttime = 0x00000000FFFFFFFFULL,
		.ifrecv = 0x0102030405060708ULL,
		.ifdrop = 7,
	};
	const struct pcapng_isb recv = {
		.interface = 2,
		.starttime = UINT64_MAX,
		.ifrecv = 1,
		.ifdrop = UINT64_MAX,
	};

	CHECK_WRITE("isb_bare", isb_bare, pcapng_isb(buf, cap, &bare));
	CHECK_WRITE("isb_full", isb_full, pcapng_isb(buf, cap, &full));
	CHECK_WRITE("isb_recv", isb_recv, pcapng_isb(buf, cap, &recv));
	zassert_equal(pcapng_isb(buf, sizeof(buf), NULL), -EINVAL);
}

/* A stream as the firmware writes one, read back block by block. */
ZTEST(pcapng, test_stream)
{
	static uint8_t s[2048];
	static const struct pcapng_idb idbs[PCAP_IF_COUNT] = {
		{.linktype = PCAP_LINKTYPE_BLE_LL_WITH_PHDR, .name = "ble",
		 .description = "Bluetooth LE", .tsresol = 9},
		{.linktype = PCAP_LINKTYPE_IEEE802_15_4_TAP, .name = "wpan",
		 .description = "IEEE 802.15.4", .tsresol = 9, .speed_bps = 250000},
		{.linktype = PCAP_LINKTYPE_USER0, .name = "burst", .description = "esdr bursts",
		 .tsresol = 9, .hardware = "C6 gapless"},
	};
	static const uint8_t data[PCAP_IF_COUNT][5] = {
		{0xD6, 0xBE, 0x89, 0x8E, 0x40}, {0x41, 0x88, 0x01}, {1}};
	static const uint32_t lens[PCAP_IF_COUNT] = {5, 3, 1};
	const uint64_t epoch = 1700000000ULL * 1000000000ULL;
	size_t n = 0, pos = 0, len;
	struct block b;
	const uint8_t *v;
	int r;

	r = pcapng_shb(s + n, sizeof(s) - n, "why2025_badge", "Zephyr", "esp-sdr pcap 1");
	zassert_true(r > 0, "shb %d", r);
	n += (size_t)r;
	for (uint32_t i = 0; i < PCAP_IF_COUNT; i++) {
		r = pcapng_idb(s + n, sizeof(s) - n, &idbs[i]);
		zassert_true(r > 0, "idb %u: %d", i, r);
		n += (size_t)r;
	}
	for (uint32_t i = 0; i < PCAP_IF_COUNT; i++) {
		const struct pcapng_epb e = {
			.interface = i,
			.timestamp = epoch + i,
			.data = data[i],
			.len = lens[i],
			.comment = i == 0U ? "esdr: test" : NULL,
			.flags = i == 1U ? PCAPNG_EPB_FLAG_INBOUND : 0U,
			.dropcount = i == 2U ? 3U : UINT64_MAX,
		};

		r = pcapng_epb(s + n, sizeof(s) - n, &e);
		zassert_true(r > 0, "epb %u: %d", i, r);
		n += (size_t)r;
	}
	for (uint32_t i = 0; i < PCAP_IF_COUNT; i++) {
		const struct pcapng_isb isb = {
			.interface = i,
			.timestamp = epoch + 1000000000ULL,
			.starttime = i == 0U ? epoch : UINT64_MAX,
			.ifrecv = 1U + i,
			.ifdrop = i == 2U ? UINT64_MAX : 0U,
		};

		r = pcapng_isb(s + n, sizeof(s) - n, &isb);
		zassert_true(r > 0, "isb %u: %d", i, r);
		n += (size_t)r;
	}

	/* SHB. */
	zassert_true(next_block(s, n, &pos, &b), "no SHB");
	zassert_equal(b.type, PCAPNG_BT_SHB, "type 0x%08x", b.type);
	zassert_true(b.len >= 16U, "SHB body %zu", b.len);
	zassert_equal(get_u32(b.body), PCAPNG_BYTE_ORDER_MAGIC, "magic");
	zassert_equal(get_u16(b.body + 4), 1, "major");
	zassert_equal(get_u16(b.body + 6), 0, "minor");
	zassert_equal(get_u64(b.body + 8), UINT64_MAX, "section length");
	expect_str_opt(b.body + 16, b.len - 16U, PCAPNG_SHB_HARDWARE, "why2025_badge");
	expect_str_opt(b.body + 16, b.len - 16U, PCAPNG_SHB_OS, "Zephyr");
	expect_str_opt(b.body + 16, b.len - 16U, PCAPNG_SHB_USERAPPL, "esp-sdr pcap 1");
	zassert_is_null(find_opt(b.body + 16, b.len - 16U, PCAPNG_OPT_COMMENT, &len), "comment");

	/* IDBs. */
	for (uint32_t i = 0; i < PCAP_IF_COUNT; i++) {
		zassert_true(next_block(s, n, &pos, &b), "no IDB %u", i);
		zassert_equal(b.type, PCAPNG_BT_IDB, "IDB %u type 0x%08x", i, b.type);
		zassert_equal(get_u16(b.body), idbs[i].linktype, "IDB %u link type", i);
		zassert_equal(get_u16(b.body + 2), 0, "IDB %u reserved", i);
		zassert_equal(get_u32(b.body + 4), 0, "IDB %u snaplen", i);
		expect_str_opt(b.body + 8, b.len - 8U, PCAPNG_IF_NAME, idbs[i].name);
		expect_str_opt(b.body + 8, b.len - 8U, PCAPNG_IF_DESCRIPTION, idbs[i].description);
		v = find_opt(b.body + 8, b.len - 8U, PCAPNG_IF_TSRESOL, &len);
		zassert_not_null(v, "IDB %u tsresol", i);
		zassert_true(len == 1U && v[0] == 9U, "IDB %u tsresol %zu 0x%02x", i, len, v[0]);
		v = find_opt(b.body + 8, b.len - 8U, PCAPNG_IF_SPEED, &len);
		if (i == 1U) {
			zassert_true(v != NULL && len == 8U && get_u64(v) == 250000U, "IDB speed");
		} else {
			zassert_is_null(v, "IDB %u speed", i);
		}
		v = find_opt(b.body + 8, b.len - 8U, PCAPNG_IF_HARDWARE, &len);
		if (i == 2U) {
			expect_str_opt(b.body + 8, b.len - 8U, PCAPNG_IF_HARDWARE, "C6 gapless");
		} else {
			zassert_is_null(v, "IDB %u hardware", i);
		}
	}

	/* EPBs. */
	for (uint32_t i = 0; i < PCAP_IF_COUNT; i++) {
		size_t padded = (lens[i] + 3U) & ~3U;
		const uint8_t *opts;
		size_t optlen;

		zassert_true(next_block(s, n, &pos, &b), "no EPB %u", i);
		zassert_equal(b.type, PCAPNG_BT_EPB, "EPB %u type 0x%08x", i, b.type);
		zassert_equal(get_u32(b.body), i, "EPB %u interface", i);
		zassert_equal(get_ts(b.body + 4), epoch + i, "EPB %u timestamp", i);
		zassert_equal(get_u32(b.body + 12), lens[i], "EPB %u caplen", i);
		zassert_equal(get_u32(b.body + 16), lens[i], "EPB %u origlen", i);
		zassert_mem_equal(b.body + 20, data[i], lens[i], "EPB %u data", i);
		for (size_t k = lens[i]; k < padded; k++) {
			zassert_equal(b.body[20 + k], 0, "EPB %u padding", i);
		}
		opts = b.body + 20 + padded;
		optlen = b.len - 20U - padded;
		v = find_opt(opts, optlen, PCAPNG_OPT_COMMENT, &len);
		if (i == 0U) {
			expect_str_opt(opts, optlen, PCAPNG_OPT_COMMENT, "esdr: test");
		} else {
			zassert_is_null(v, "EPB %u comment", i);
		}
		v = find_opt(opts, optlen, PCAPNG_EPB_FLAGS, &len);
		if (i == 1U) {
			zassert_true(v != NULL && len == 4U &&
				     get_u32(v) == PCAPNG_EPB_FLAG_INBOUND, "EPB flags");
		} else {
			zassert_is_null(v, "EPB %u flags", i);
		}
		v = find_opt(opts, optlen, PCAPNG_EPB_DROPCOUNT, &len);
		if (i == 2U) {
			zassert_equal(expect_u64_opt(opts, optlen, PCAPNG_EPB_DROPCOUNT), 3,
				      "EPB dropcount");
		} else {
			zassert_is_null(v, "EPB %u dropcount", i);
		}
	}

	/* ISBs. */
	for (uint32_t i = 0; i < PCAP_IF_COUNT; i++) {
		const uint8_t *opts;
		size_t optlen;

		zassert_true(next_block(s, n, &pos, &b), "no ISB %u", i);
		opts = b.body + 12;
		optlen = b.len - 12U;
		zassert_equal(b.type, PCAPNG_BT_ISB, "ISB %u type 0x%08x", i, b.type);
		zassert_equal(get_u32(b.body), i, "ISB %u interface", i);
		zassert_equal(get_ts(b.body + 4), epoch + 1000000000ULL, "ISB %u timestamp", i);
		v = find_opt(opts, optlen, PCAPNG_ISB_STARTTIME, &len);
		if (i == 0U) {
			zassert_true(v != NULL && len == 8U && get_ts(v) == epoch, "ISB starttime");
		} else {
			zassert_is_null(v, "ISB %u starttime", i);
		}
		zassert_equal(expect_u64_opt(opts, optlen, PCAPNG_ISB_IFRECV), 1U + i,
			      "ISB %u ifrecv", i);
		v = find_opt(opts, optlen, PCAPNG_ISB_IFDROP, &len);
		if (i == 2U) {
			zassert_is_null(v, "ISB 2 ifdrop");
		} else {
			zassert_equal(expect_u64_opt(opts, optlen, PCAPNG_ISB_IFDROP), 0,
				      "ISB %u ifdrop", i);
		}
	}

	zassert_false(next_block(s, n, &pos, &b), "blocks after the last ISB");
	zassert_equal(pos, n, "stream length");
}

/* ---- the records ---- */

/* The BLE record: RF channel 12, 1 offense, the advertising access address, LE 2M, CRC checked
 * and valid (flags 0x4C31), then 7 on-air bytes.
 */
static const uint8_t ble_rec[] = {
	0x0C, 0x00, 0x00, 0x01, 0xD6, 0xBE, 0x89, 0x8E, 0x31, 0x4C,
	0xD6, 0xBE, 0x89, 0x8E, 0x40, 0x05, 0x01,
};

ZTEST(pcapng, test_rec_ble)
{
	static const uint8_t air[] = {0xD6, 0xBE, 0x89, 0x8E, 0x40, 0x05, 0x01};
	struct dec_ble_pkt p = {
		.phy = DEC_BLE_PHY_2M,
		.aa = DEC_BLE_ADV_AA,
		.aa_errors = 1,
		.crc_checked = true,
		.crc_ok = true,
		.len = sizeof(air),
	};
	int r;

	memcpy(p.air, air, sizeof(air));
	CHECK_WRITE("ble_rec", ble_rec, rec_ble(buf, cap, &p, 12));

	/* Every flag combination. */
	for (unsigned int combo = 0; combo < 8U; combo++) {
		uint16_t exp = REC_BLE_F_DEWHITENED | REC_BLE_F_REF_AA_VALID |
			       REC_BLE_F_OFFENSES_VALID;

		p.phy = (combo & 1U) ? DEC_BLE_PHY_2M : DEC_BLE_PHY_1M;
		p.crc_checked = (combo & 2U) != 0U;
		p.crc_ok = (combo & 4U) != 0U;
		p.aa = 0x12345678U;
		p.aa_errors = (uint8_t)combo;
		if (p.phy == DEC_BLE_PHY_2M) {
			exp |= REC_BLE_PHY_2M << REC_BLE_F_PHY_SHIFT;
		}
		if (p.crc_checked) {
			exp |= REC_BLE_F_CRC_CHECKED;
		}
		if (p.crc_ok) {
			exp |= REC_BLE_F_CRC_VALID;
		}
		memset(buf, GUARD, sizeof(buf));
		r = rec_ble(buf, sizeof(buf), &p, 39);
		zassert_equal(r, (int)(REC_BLE_HDR + sizeof(air)), "combo %u: %d", combo, r);
		zassert_equal(buf[0], 39, "combo %u: channel", combo);
		zassert_equal(buf[1], 0, "combo %u: signal power", combo);
		zassert_equal(buf[2], 0, "combo %u: noise power", combo);
		zassert_equal(buf[3], (uint8_t)combo, "combo %u: offenses", combo);
		zassert_equal(get_u32(buf + 4), 0x12345678U, "combo %u: access address", combo);
		zassert_equal(get_u16(buf + 8), exp, "combo %u: flags 0x%04x, expected 0x%04x",
			      combo, get_u16(buf + 8), exp);
		zassert_mem_equal(buf + REC_BLE_HDR, air, sizeof(air), "combo %u: air", combo);
		zassert_true(all_guard(buf + r, sizeof(buf) - (size_t)r), "combo %u: tail", combo);
		/* PDU type, signal and noise validity stay clear. */
		zassert_equal(get_u16(buf + 8) & (REC_BLE_F_PDU_TYPE_MASK | REC_BLE_F_SIGNAL_VALID |
						  REC_BLE_F_NOISE_VALID), 0, "combo %u", combo);
	}

	/* Channels off the grid, a packet longer than its buffer, the longest packet. */
	zassert_equal(rec_ble(buf, sizeof(buf), &p, -1), -EINVAL);
	zassert_equal(rec_ble(buf, sizeof(buf), &p, 40), -EINVAL);
	p.len = DEC_BLE_AIR_MAX + 1U;
	zassert_equal(rec_ble(buf, sizeof(buf), &p, 0), -EINVAL);
	p.len = DEC_BLE_AIR_MAX;
	r = rec_ble(buf, sizeof(buf), &p, 0);
	zassert_equal(r, (int)REC_BLE_MAX, "longest: %d", r);
	zassert_equal(rec_ble(buf, REC_BLE_MAX - 1U, &p, 0), -ENOSPC);
	zassert_equal(rec_ble(NULL, sizeof(buf), &p, 0), -EINVAL);
}

/* The TAP record: FCS type 1, bit rate 250000, channel 12 page 0, LQI 200, 2426000.0 kHz
 * (0x4A141240), then 5 PSDU bytes.
 */
static const uint8_t tap_rec[] = {
	0x00, 0x00, 0x2C, 0x00,
	0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x04, 0x00, 0x90, 0xD0, 0x03, 0x00,
	0x03, 0x00, 0x03, 0x00, 0x0C, 0x00, 0x00, 0x00,
	0x0A, 0x00, 0x01, 0x00, 0xC8, 0x00, 0x00, 0x00,
	0x0B, 0x00, 0x04, 0x00, 0x40, 0x12, 0x14, 0x4A,
	0x41, 0x88, 0x01, 0xAA, 0xBB,
};

struct tlv {
	uint16_t type, len;
	const uint8_t *val;
};

/* The TLVs of the TAP header at @p rec (its length field trusted after a check); their count. */
static size_t parse_tap(const uint8_t *rec, int rec_len, struct tlv *tlvs, size_t max)
{
	size_t hdr, pos = 4, count = 0;

	zassert_true(rec_len >= 4, "record %d", rec_len);
	zassert_equal(rec[0], 0, "TAP version");
	zassert_equal(rec[1], 0, "TAP reserved");
	hdr = get_u16(rec + 2);
	zassert_true(hdr % 4U == 0U && hdr >= 4U && (int)hdr <= rec_len, "TAP header %zu", hdr);
	while (pos < hdr) {
		zassert_true(pos + 4U <= hdr, "TLV truncated at %zu", pos);
		zassert_true(count < max, "too many TLVs");
		tlvs[count].type = get_u16(rec + pos);
		tlvs[count].len = get_u16(rec + pos + 2);
		tlvs[count].val = rec + pos + 4;
		pos += 4U + ((tlvs[count].len + 3U) & ~3U);
		zassert_true(pos <= hdr, "TLV %u runs past the header", tlvs[count].type);
		for (size_t k = tlvs[count].len; k < ((tlvs[count].len + 3U) & ~3U); k++) {
			zassert_equal(tlvs[count].val[k], 0, "TLV %u padding", tlvs[count].type);
		}
		count++;
	}
	return count;
}

static const struct tlv *find_tlv(const struct tlv *tlvs, size_t n, uint16_t type)
{
	for (size_t i = 0; i < n; i++) {
		if (tlvs[i].type == type) {
			return &tlvs[i];
		}
	}
	return NULL;
}

static float tlv_float(const struct tlv *t)
{
	uint32_t bits;
	float f;

	zassert_not_null(t, "float TLV missing");
	zassert_equal(t->len, 4, "float TLV length %u", t->len);
	bits = get_u32(t->val);
	memcpy(&f, &bits, sizeof(f));
	return f;
}

ZTEST(pcapng, test_rec_wpan)
{
	static const uint8_t psdu[] = {0x41, 0x88, 0x01, 0xAA, 0xBB};
	struct dec_wpan_pkt p = {.len = sizeof(psdu), .fcs_ok = true, .lqi = 200, .channel = 12};
	struct tlv tlvs[8];
	const struct tlv *t;
	size_t n;
	int r;

	memcpy(p.psdu, psdu, sizeof(psdu));
	CHECK_WRITE("tap_rec", tap_rec, rec_wpan(buf, cap, &p, 2426000000ULL));

	/* The TLVs read back, with the channel. */
	r = rec_wpan(buf, sizeof(buf), &p, 2426000000ULL);
	zassert_equal(r, (int)sizeof(tap_rec), "%d", r);
	n = parse_tap(buf, r, tlvs, ARRAY_SIZE(tlvs));
	zassert_equal(n, 5, "%zu TLVs", n);
	t = find_tlv(tlvs, n, REC_TAP_TLV_FCS_TYPE);
	zassert_true(t != NULL && t->len == 1U && t->val[0] == REC_TAP_FCS_CRC16, "FCS type");
	t = find_tlv(tlvs, n, REC_TAP_TLV_BIT_RATE);
	zassert_true(t != NULL && t->len == 4U && get_u32(t->val) == 250000U, "bit rate");
	t = find_tlv(tlvs, n, REC_TAP_TLV_CHANNEL_ASSIGNMENT);
	zassert_true(t != NULL && t->len == 3U && get_u16(t->val) == 12U && t->val[2] == 0U,
		     "channel assignment");
	t = find_tlv(tlvs, n, REC_TAP_TLV_LQI);
	zassert_true(t != NULL && t->len == 1U && t->val[0] == 200U, "LQI");
	zassert_true(tlv_float(find_tlv(tlvs, n, REC_TAP_TLV_CHANNEL_FREQUENCY)) == 2426000.0f,
		     "frequency %f kHz", (double)tlv_float(find_tlv(tlvs, n, 11)));
	zassert_is_null(find_tlv(tlvs, n, REC_TAP_TLV_RSS), "RSS written");
	zassert_mem_equal(buf + get_u16(buf + 2), psdu, sizeof(psdu), "PSDU");

	/* Off the channel grid: no channel TLV; an odd megahertz and a sub-kHz frequency. */
	p.channel = 0;
	p.lqi = 0;
	memset(buf, GUARD, sizeof(buf));
	r = rec_wpan(buf, sizeof(buf), &p, 2425000000ULL);
	zassert_equal(r, 36 + (int)sizeof(psdu), "%d", r);
	zassert_equal(get_u16(buf + 2), 36, "header length");
	n = parse_tap(buf, r, tlvs, ARRAY_SIZE(tlvs));
	zassert_equal(n, 4, "%zu TLVs", n);
	zassert_is_null(find_tlv(tlvs, n, REC_TAP_TLV_CHANNEL_ASSIGNMENT), "channel TLV");
	t = find_tlv(tlvs, n, REC_TAP_TLV_LQI);
	zassert_true(t != NULL && t->val[0] == 0U, "LQI 0");
	zassert_true(tlv_float(find_tlv(tlvs, n, REC_TAP_TLV_CHANNEL_FREQUENCY)) == 2425000.0f,
		     "odd MHz");
	zassert_mem_equal(buf + 36, psdu, sizeof(psdu), "PSDU");
	zassert_true(all_guard(buf + r, sizeof(buf) - (size_t)r), "tail");
	r = rec_wpan(buf, sizeof(buf), &p, 2426000500ULL);
	n = parse_tap(buf, r, tlvs, ARRAY_SIZE(tlvs));
	zassert_true(tlv_float(find_tlv(tlvs, n, REC_TAP_TLV_CHANNEL_FREQUENCY)) == 2426000.5f,
		     "sub-kHz");

	/* The longest frame, a frame beyond its buffer, no room. */
	p.channel = 26;
	p.len = 127;
	r = rec_wpan(buf, sizeof(buf), &p, 2480000000ULL);
	zassert_equal(r, (int)REC_TAP_MAX, "longest: %d", r);
	zassert_equal(rec_wpan(buf, REC_TAP_MAX - 1U, &p, 2480000000ULL), -ENOSPC);
	p.len = 128;
	zassert_equal(rec_wpan(buf, sizeof(buf), &p, 2480000000ULL), -EINVAL);
	zassert_equal(rec_wpan(buf, sizeof(buf), NULL, 0), -EINVAL);
	p.len = 0;
	zassert_equal(rec_wpan(buf, sizeof(buf), &p, 0), 44, "empty PSDU");
}

ZTEST(pcapng, test_rec_burst)
{
	static const uint8_t bits[PCAP_BURST_BITS_MAX / 8U] = {0xA5, 0x5A, 0x80, 0xFF};
	struct pcap_burst_hdr h = {
		/* Both overwritten by the record. */
		.version = 0xEE,
		.hdr_len = 0xEE,
		.flags = PCAP_BF_BITS | PCAP_BF_NEIGHBOUR,
		.freq_hz = 2426000000ULL,
		.offset_hz = -12345,
		.bandwidth_hz = 1200000,
		.duration_us = 376,
		.symbol_rate = 1000000,
		.deviation_hz = 250000,
		.modulation = PCAP_MOD_2FSK,
		.rx_gain = 30,
		.channel = 12,
		.channels = 40,
		.power_dbfs_x10 = -421,
		.floor_dbfs_x10 = -780,
		.peak_dbfs_x10 = -300,
		.sample_rate_hz = 4000000,
		.nbits = 17,
	};
	static const uint8_t freq_le[] = {0x80, 0xD2, 0x99, 0x90, 0x00, 0x00, 0x00, 0x00};
	static const uint8_t offset_le[] = {0xC7, 0xCF, 0xFF, 0xFF};
	static const struct {
		uint32_t nbits;
		int len;
	} pads[] = {{0, 52}, {1, 53}, {7, 53}, {8, 53}, {9, 54}, {17, 55}, {2047, 308},
		    {2048, 308}};
	int r;

	zassert_equal(sizeof(h), 52, "header %zu bytes", sizeof(h));
	zassert_equal(REC_BURST_MAX, 308, "longest record %zu", REC_BURST_MAX);

	memset(buf, GUARD, sizeof(buf));
	r = rec_burst(buf, sizeof(buf), &h, bits);
	zassert_equal(r, 55, "%d", r);
	zassert_equal(buf[0], PCAP_BURST_VERSION, "version");
	zassert_equal(buf[1], 52, "hdr_len");
	zassert_mem_equal(buf + 2, (const uint8_t *)&h + 2, sizeof(h) - 2U, "header");
	zassert_mem_equal(buf + 52, bits, 3, "bits");
	zassert_true(all_guard(buf + 55, sizeof(buf) - 55U), "tail");
	/* Fields at their wire offsets, little endian. */
	zassert_equal(get_u16(buf + 2), PCAP_BF_BITS | PCAP_BF_NEIGHBOUR, "flags");
	zassert_mem_equal(buf + 4, freq_le, sizeof(freq_le), "freq_hz");
	zassert_mem_equal(buf + 12, offset_le, sizeof(offset_le), "offset_hz");
	zassert_equal(get_u32(buf + 16), 1200000, "bandwidth_hz");
	zassert_equal(get_u32(buf + 20), 376, "duration_us");
	zassert_equal(get_u32(buf + 24), 1000000, "symbol_rate");
	zassert_equal(get_u32(buf + 28), 250000, "deviation_hz");
	zassert_equal(buf[32], PCAP_MOD_2FSK, "modulation");
	zassert_equal(buf[33], 30, "rx_gain");
	zassert_equal(buf[34], 12, "channel");
	zassert_equal(buf[35], 40, "channels");
	zassert_equal(get_u16(buf + 36), (uint16_t)-421, "power");
	zassert_equal(get_u16(buf + 38), (uint16_t)-780, "floor");
	zassert_equal(get_u16(buf + 40), (uint16_t)-300, "peak");
	zassert_equal(get_u16(buf + 42), 0, "reserved");
	zassert_equal(get_u32(buf + 44), 4000000, "sample_rate_hz");
	zassert_equal(get_u32(buf + 48), 17, "nbits");

	/* Bit padding: the bytes after the header. */
	for (size_t i = 0; i < ARRAY_SIZE(pads); i++) {
		h.nbits = pads[i].nbits;
		memset(buf, GUARD, sizeof(buf));
		r = rec_burst(buf, sizeof(buf), &h, bits);
		zassert_equal(r, pads[i].len, "%u bits: %d", h.nbits, r);
		zassert_equal(get_u32(buf + 48), h.nbits, "%u bits: nbits", h.nbits);
		zassert_mem_equal(buf + 52, bits, (size_t)pads[i].len - 52U, "%u bits: bytes",
				  h.nbits);
		zassert_true(all_guard(buf + r, sizeof(buf) - (size_t)r), "%u bits: tail", h.nbits);
		zassert_equal(rec_burst(buf, (size_t)pads[i].len - 1U, &h, bits), -ENOSPC,
			      "%u bits: -ENOSPC", h.nbits);
	}
	h.nbits = 0;
	zassert_equal(rec_burst(buf, sizeof(buf), &h, NULL), 52, "no bits, NULL");
	h.nbits = 1;
	zassert_equal(rec_burst(buf, sizeof(buf), &h, NULL), -EINVAL, "NULL bits");
	h.nbits = PCAP_BURST_BITS_MAX + 1U;
	zassert_equal(rec_burst(buf, sizeof(buf), &h, bits), -EINVAL, "too many bits");
	zassert_equal(rec_burst(buf, sizeof(buf), NULL, bits), -EINVAL, "no header");
}

/* The exact text, -ENOSPC one byte short (the text truncated and terminated), the exact fit. */
static void check_comment(const struct rec_meta *m, const char *exp)
{
	char s[REC_COMMENT_MAX + 16];
	size_t n = strlen(exp);
	int r;

	memset(s, 'x', sizeof(s));
	r = rec_comment(s, sizeof(s), m);
	zassert_equal(r, (int)n, "'%s': %d (got '%s')", exp, r, s);
	zassert_str_equal(s, exp, "got '%s'", s);
	memset(s, 'x', sizeof(s));
	r = rec_comment(s, n, m);
	zassert_equal(r, -ENOSPC, "'%s' at cap %zu: %d", exp, n, r);
	zassert_equal(s[n - 1U], '\0', "truncated text not terminated");
	zassert_mem_equal(s, exp, n - 1U, "truncated text differs");
	zassert_equal(s[n], 'x', "wrote past cap");
	memset(s, 'x', sizeof(s));
	r = rec_comment(s, n + 1U, m);
	zassert_equal(r, (int)n, "'%s' at cap %zu: %d", exp, n + 1U, r);
	zassert_str_equal(s, exp, "got '%s'", s);
	zassert_equal(s[n + 1U], 'x', "wrote past cap");
}

ZTEST(pcapng, test_rec_comment)
{
	struct rec_meta m = {
		.freq_hz = 2426000000ULL,
		.offset_hz = 12300,
		.power_dbfs_x10 = -421,
		.floor_dbfs_x10 = -780,
		.gain = 30,
		.channel = 12,
		.channels = 40,
		.rate_hz = 4000000,
		.duration_us = 376,
		.phy = "LE 1M",
	};
	char s[4];

	check_comment(&m, "esdr: 2426.000 MHz, ch 12/40 at 4.000 MS/s, LE 1M, offset +12.3 kHz, "
			  "power -42.1 dBFS, floor -78.0 dBFS, snr 35.9 dB, gain 30, 376 us");

	/* Automatic gain, no PHY, a negative offset. */
	m.freq_hz = 2425000000ULL;
	m.channel = 11;
	m.phy = NULL;
	m.offset_hz = -12345;
	m.power_dbfs_x10 = -600;
	m.gain = REC_GAIN_AUTO;
	m.duration_us = 1000;
	check_comment(&m, "esdr: 2425.000 MHz, ch 11/40 at 4.000 MS/s, offset -12.3 kHz, "
			  "power -60.0 dBFS, floor -78.0 dBFS, snr 18.0 dB, gain auto, 1000 us");

	/* Rounding half away from zero, zero offsets signed "+", a negative snr. */
	m.freq_hz = 2402000499ULL;
	m.rate_hz = 2000000;
	m.channel = 0;
	m.channels = 0;
	m.offset_hz = 0;
	m.power_dbfs_x10 = -800;
	m.gain = 0;
	m.duration_us = 0;
	m.phy = "O-QPSK";
	check_comment(&m, "esdr: 2402.000 MHz, ch 0/0 at 2.000 MS/s, O-QPSK, offset +0.0 kHz, "
			  "power -80.0 dBFS, floor -78.0 dBFS, snr -2.0 dB, gain 0, 0 us");
	m.freq_hz = 2402000500ULL;
	m.rate_hz = 1500;
	m.offset_hz = -40;
	m.power_dbfs_x10 = -5;
	m.floor_dbfs_x10 = -1234;
	check_comment(&m, "esdr: 2402.001 MHz, ch 0/0 at 0.002 MS/s, O-QPSK, offset +0.0 kHz, "
			  "power -0.5 dBFS, floor -123.4 dBFS, snr 122.9 dB, gain 0, 0 us");
	m.rate_hz = 0;
	m.offset_hz = -50;
	m.power_dbfs_x10 = 3;
	check_comment(&m, "esdr: 2402.001 MHz, ch 0/0 at 0.000 MS/s, O-QPSK, offset -0.1 kHz, "
			  "power 0.3 dBFS, floor -123.4 dBFS, snr 123.7 dB, gain 0, 0 us");
	m.offset_hz = 49;
	check_comment(&m, "esdr: 2402.001 MHz, ch 0/0 at 0.000 MS/s, O-QPSK, offset +0.0 kHz, "
			  "power 0.3 dBFS, floor -123.4 dBFS, snr 123.7 dB, gain 0, 0 us");
	m.offset_hz = 50;
	check_comment(&m, "esdr: 2402.001 MHz, ch 0/0 at 0.000 MS/s, O-QPSK, offset +0.1 kHz, "
			  "power 0.3 dBFS, floor -123.4 dBFS, snr 123.7 dB, gain 0, 0 us");
	m.offset_hz = 12350;
	check_comment(&m, "esdr: 2402.001 MHz, ch 0/0 at 0.000 MS/s, O-QPSK, offset +12.4 kHz, "
			  "power 0.3 dBFS, floor -123.4 dBFS, snr 123.7 dB, gain 0, 0 us");
	m.offset_hz = -12350;
	check_comment(&m, "esdr: 2402.001 MHz, ch 0/0 at 0.000 MS/s, O-QPSK, offset -12.4 kHz, "
			  "power 0.3 dBFS, floor -123.4 dBFS, snr 123.7 dB, gain 0, 0 us");

	/* The extremes with a 20-byte PHY name fit REC_COMMENT_MAX. */
	m.freq_hz = UINT64_MAX;
	m.offset_hz = INT32_MIN;
	m.power_dbfs_x10 = INT16_MIN;
	m.floor_dbfs_x10 = INT16_MAX;
	m.gain = REC_GAIN_AUTO;
	m.channel = UINT16_MAX;
	m.channels = UINT16_MAX;
	m.rate_hz = UINT32_MAX;
	m.duration_us = UINT32_MAX;
	m.phy = "12345678901234567890";
	check_comment(&m, "esdr: 18446744073709.552 MHz, ch 65535/65535 at 4294.967 MS/s, "
			  "12345678901234567890, offset -2147483.6 kHz, power -3276.8 dBFS, "
			  "floor 3276.7 dBFS, snr -6553.5 dB, gain auto, 4294967295 us");
	zassert_true(strlen("esdr: 18446744073709.552 MHz, ch 65535/65535 at 4294.967 MS/s, "
			    "12345678901234567890, offset -2147483.6 kHz, power -3276.8 dBFS, "
			    "floor 3276.7 dBFS, snr -6553.5 dB, gain auto, 4294967295 us") <
		     REC_COMMENT_MAX, "REC_COMMENT_MAX too small");

	/* Tiny and absent buffers. */
	zassert_equal(rec_comment(NULL, 0, &m), -ENOSPC);
	zassert_equal(rec_comment(NULL, 10, &m), -EINVAL);
	zassert_equal(rec_comment(s, sizeof(s), NULL), -EINVAL);
	memset(s, 'x', sizeof(s));
	zassert_equal(rec_comment(s, 1, &m), -ENOSPC);
	zassert_equal(s[0], '\0', "cap 1 not terminated");
	zassert_equal(s[1], 'x', "cap 1 wrote past");
	memset(s, 'x', sizeof(s));
	zassert_equal(rec_comment(s, 3, &m), -ENOSPC);
	zassert_true(s[0] == 'e' && s[1] == 's' && s[2] == '\0' && s[3] == 'x', "cap 3: '%s'", s);
}

ZTEST_SUITE(pcapng, NULL, NULL, NULL, NULL, NULL);
