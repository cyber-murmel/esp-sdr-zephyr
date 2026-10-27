/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The pcapng writer of pcapng.h: section header, interface description, enhanced packet and
 * interface statistics blocks, one at a time into the caller's buffer, little endian. The size
 * is computed first and nothing is written when the block does not fit. Options go out in
 * ascending code order, only when there is at least one (then with the opt_endofopt
 * terminator); strings carry no NUL. Plain C, no Zephyr.
 */

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "pcapng.h"

/* Bytes of each block without options or packet data: type, lengths and the fixed fields. */
#define SHB_FIXED 28U
#define IDB_FIXED 20U
#define EPB_FIXED 32U
#define ISB_FIXED 24U

/* opt_endofopt: code 0, length 0. */
#define OPT_END_SIZE 4U

/* Rounded up to a multiple of 4. */
static uint64_t pad4(uint64_t n)
{
	return (n + 3U) & ~(uint64_t)3U;
}

/* Bytes an option of @p len value bytes takes: code, length and the padded value. */
static uint64_t opt_size(uint64_t len)
{
	return 4U + pad4(len);
}

static void put_u16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static void put_u64(uint8_t *p, uint64_t v)
{
	put_u32(p, (uint32_t)v);
	put_u32(p + 4, (uint32_t)(v >> 32));
}

/* A timestamp as the blocks carry it: the high 32 bits, then the low. */
static void put_ts(uint8_t *p, uint64_t ts)
{
	put_u32(p, (uint32_t)(ts >> 32));
	put_u32(p + 4, (uint32_t)ts);
}

/* One option: @p len value bytes from @p val, zero padded to 4. Returns the next position. */
static uint8_t *opt_put(uint8_t *p, uint16_t code, const void *val, size_t len)
{
	size_t padded = (size_t)pad4(len);

	put_u16(p, code);
	put_u16(p + 2, (uint16_t)len);
	memcpy(p + 4, val, len);
	memset(p + 4 + len, 0, padded - len);
	return p + 4 + padded;
}

static uint8_t *opt_str(uint8_t *p, uint16_t code, const char *s)
{
	return opt_put(p, code, s, strlen(s));
}

static uint8_t *opt_u8(uint8_t *p, uint16_t code, uint8_t v)
{
	return opt_put(p, code, &v, 1U);
}

static uint8_t *opt_u32(uint8_t *p, uint16_t code, uint32_t v)
{
	uint8_t le[4];

	put_u32(le, v);
	return opt_put(p, code, le, sizeof(le));
}

static uint8_t *opt_u64(uint8_t *p, uint16_t code, uint64_t v)
{
	uint8_t le[8];

	put_u64(le, v);
	return opt_put(p, code, le, sizeof(le));
}

/* A timestamp option: split high and low like the block's own timestamp. */
static uint8_t *opt_ts(uint8_t *p, uint16_t code, uint64_t ts)
{
	uint8_t le[8];

	put_ts(le, ts);
	return opt_put(p, code, le, sizeof(le));
}

static uint8_t *opt_end(uint8_t *p)
{
	memset(p, 0, OPT_END_SIZE);
	return p + OPT_END_SIZE;
}

/* Bytes the option of string @p s takes, 0 without one. */
static uint64_t str_size(const char *s)
{
	return s != NULL ? opt_size(strlen(s)) : 0U;
}

/* 1 when @p s is NULL or fits an option's 16-bit length. */
static int str_ok(const char *s)
{
	return s == NULL || strlen(s) <= UINT16_MAX;
}

/* -ENOSPC or -EINVAL for a block of @p size bytes into @p buf of @p cap, else 0. */
static int check(const uint8_t *buf, size_t cap, uint64_t size)
{
	if (size > cap) {
		return -ENOSPC;
	}
	if (buf == NULL || size > (uint64_t)INT_MAX) {
		return -EINVAL;
	}
	return 0;
}

/* The block's type and both total length words around a block of @p size bytes. */
static void frame(uint8_t *buf, uint32_t type, size_t size)
{
	put_u32(buf, type);
	put_u32(buf + 4, (uint32_t)size);
	put_u32(buf + size - 4, (uint32_t)size);
}

int pcapng_shb(uint8_t *buf, size_t cap, const char *hardware, const char *os,
	       const char *userappl)
{
	uint64_t opts, size;
	uint8_t *p;
	int err;

	if (!str_ok(hardware) || !str_ok(os) || !str_ok(userappl)) {
		return -EINVAL;
	}
	opts = str_size(hardware) + str_size(os) + str_size(userappl);
	if (opts != 0U) {
		opts += OPT_END_SIZE;
	}
	size = SHB_FIXED + opts;
	err = check(buf, cap, size);
	if (err != 0) {
		return err;
	}

	p = buf + 8;
	put_u32(p, PCAPNG_BYTE_ORDER_MAGIC);
	/* Major 1, minor 0; section length unknown (-1). */
	put_u16(p + 4, 1U);
	put_u16(p + 6, 0U);
	put_u64(p + 8, UINT64_MAX);
	p += 16;
	if (opts != 0U) {
		if (hardware != NULL) {
			p = opt_str(p, PCAPNG_SHB_HARDWARE, hardware);
		}
		if (os != NULL) {
			p = opt_str(p, PCAPNG_SHB_OS, os);
		}
		if (userappl != NULL) {
			p = opt_str(p, PCAPNG_SHB_USERAPPL, userappl);
		}
		p = opt_end(p);
	}
	frame(buf, PCAPNG_BT_SHB, (size_t)size);
	return (int)size;
}

int pcapng_idb(uint8_t *buf, size_t cap, const struct pcapng_idb *idb)
{
	uint64_t opts, size;
	uint8_t *p;
	int err;

	if (idb == NULL || !str_ok(idb->name) || !str_ok(idb->description) ||
	    !str_ok(idb->hardware)) {
		return -EINVAL;
	}
	opts = str_size(idb->name) + str_size(idb->description) +
	       (idb->speed_bps != 0U ? opt_size(8U) : 0U) +
	       (idb->tsresol != 0U ? opt_size(1U) : 0U) + str_size(idb->hardware);
	if (opts != 0U) {
		opts += OPT_END_SIZE;
	}
	size = IDB_FIXED + opts;
	err = check(buf, cap, size);
	if (err != 0) {
		return err;
	}

	p = buf + 8;
	put_u16(p, idb->linktype);
	put_u16(p + 2, 0U);
	put_u32(p + 4, idb->snaplen);
	p += 8;
	if (opts != 0U) {
		if (idb->name != NULL) {
			p = opt_str(p, PCAPNG_IF_NAME, idb->name);
		}
		if (idb->description != NULL) {
			p = opt_str(p, PCAPNG_IF_DESCRIPTION, idb->description);
		}
		if (idb->speed_bps != 0U) {
			p = opt_u64(p, PCAPNG_IF_SPEED, idb->speed_bps);
		}
		if (idb->tsresol != 0U) {
			p = opt_u8(p, PCAPNG_IF_TSRESOL, idb->tsresol);
		}
		if (idb->hardware != NULL) {
			p = opt_str(p, PCAPNG_IF_HARDWARE, idb->hardware);
		}
		p = opt_end(p);
	}
	frame(buf, PCAPNG_BT_IDB, (size_t)size);
	return (int)size;
}

static uint64_t epb_size(uint32_t len, size_t comment_len, int with_flags, int with_dropcount)
{
	uint64_t opts = 0U;

	if (comment_len != 0U) {
		opts += opt_size(comment_len);
	}
	if (with_flags) {
		opts += opt_size(4U);
	}
	if (with_dropcount) {
		opts += opt_size(8U);
	}
	if (opts != 0U) {
		opts += OPT_END_SIZE;
	}
	return EPB_FIXED + pad4(len) + opts;
}

size_t pcapng_epb_size(uint32_t len, size_t comment_len, int with_flags, int with_dropcount)
{
	uint64_t size = epb_size(len, comment_len, with_flags, with_dropcount);

	return size > SIZE_MAX ? SIZE_MAX : (size_t)size;
}

int pcapng_epb(uint8_t *buf, size_t cap, const struct pcapng_epb *epb)
{
	size_t comment_len, padded;
	int with_flags, with_dropcount;
	uint64_t size;
	uint8_t *p;
	int err;

	if (epb == NULL || (epb->data == NULL && epb->len != 0U)) {
		return -EINVAL;
	}
	/* An empty comment is no comment. */
	comment_len = epb->comment != NULL ? strlen(epb->comment) : 0U;
	if (comment_len > UINT16_MAX) {
		return -EINVAL;
	}
	with_flags = epb->flags != 0U;
	with_dropcount = epb->dropcount != UINT64_MAX;
	size = epb_size(epb->len, comment_len, with_flags, with_dropcount);
	err = check(buf, cap, size);
	if (err != 0) {
		return err;
	}

	p = buf + 8;
	put_u32(p, epb->interface);
	put_ts(p + 4, epb->timestamp);
	/* Captured and original length: nothing is cut here. */
	put_u32(p + 12, epb->len);
	put_u32(p + 16, epb->len);
	p += 20;
	padded = (size_t)pad4(epb->len);
	if (epb->len != 0U) {
		memcpy(p, epb->data, epb->len);
	}
	memset(p + epb->len, 0, padded - epb->len);
	p += padded;
	if (comment_len != 0U || with_flags || with_dropcount) {
		if (comment_len != 0U) {
			p = opt_put(p, PCAPNG_OPT_COMMENT, epb->comment, comment_len);
		}
		if (with_flags) {
			p = opt_u32(p, PCAPNG_EPB_FLAGS, epb->flags);
		}
		if (with_dropcount) {
			p = opt_u64(p, PCAPNG_EPB_DROPCOUNT, epb->dropcount);
		}
		p = opt_end(p);
	}
	frame(buf, PCAPNG_BT_EPB, (size_t)size);
	return (int)size;
}

int pcapng_isb(uint8_t *buf, size_t cap, const struct pcapng_isb *isb)
{
	uint64_t opts = 0U, size;
	uint8_t *p;
	int err;

	if (isb == NULL) {
		return -EINVAL;
	}
	if (isb->starttime != UINT64_MAX) {
		opts += opt_size(8U);
	}
	if (isb->ifrecv != UINT64_MAX) {
		opts += opt_size(8U);
	}
	if (isb->ifdrop != UINT64_MAX) {
		opts += opt_size(8U);
	}
	if (opts != 0U) {
		opts += OPT_END_SIZE;
	}
	size = ISB_FIXED + opts;
	err = check(buf, cap, size);
	if (err != 0) {
		return err;
	}

	p = buf + 8;
	put_u32(p, isb->interface);
	put_ts(p + 4, isb->timestamp);
	p += 12;
	if (opts != 0U) {
		if (isb->starttime != UINT64_MAX) {
			p = opt_ts(p, PCAPNG_ISB_STARTTIME, isb->starttime);
		}
		if (isb->ifrecv != UINT64_MAX) {
			p = opt_u64(p, PCAPNG_ISB_IFRECV, isb->ifrecv);
		}
		if (isb->ifdrop != UINT64_MAX) {
			p = opt_u64(p, PCAPNG_ISB_IFDROP, isb->ifdrop);
		}
		p = opt_end(p);
	}
	frame(buf, PCAPNG_BT_ISB, (size_t)size);
	return (int)size;
}
