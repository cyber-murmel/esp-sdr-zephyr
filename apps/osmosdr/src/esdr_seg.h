/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Segment codec of the gapless blocks (ESDR_BLK_SEG in esdr_proto.h): the ESP32-C6 writes
 * them, the P4 bridge, the MicroPython egg and the host tools read them. Plain C99,
 * header-only, no Zephyr headers.
 *
 * A block is ESDR_BLOCK_BYTES long: a struct esdr_block_hdr, then segments back to back, each
 * a struct esdr_seg_hdr at a 4-byte aligned offset and its pairs (I then Q, int8), padded
 * with zeros to 4 bytes. A zero header (no pairs) ends the payload, or the payload's end does
 * when less than a header remains; zeros follow. The block header's index is the first
 * segment's, its samples the pairs of all segments. A segment's delta counts from that index,
 * so the segments of one block start within 2^32 pairs (53 s at 80 MS/s) of the first; they
 * are in order and do not overlap. Between blocks the index may jump either way (a restart).
 *
 * A pair's items are its raw I and Q (10 bits, the receiver's DC in them) shifted right by the
 * segment's shift and saturated to int8: shifted back left, less the segment's DC (dc_i, dc_q,
 * raw units), they are the sample. The segment's gain is the receive gain index the pairs were
 * taken at. The header's reserved bytes are written as zeros and not looked at.
 *
 * The writer runs on the C6 (RV32IMAC without FPU): no divisions, no 64-bit multiplies; it
 * writes the headers and leaves the pairs to the caller, who quantizes them in place. The
 * reader checks every header and payload against the buffer before it hands out any segment
 * (the bytes come from a link) and takes the buffer at any alignment.
 */

#ifndef ESDR_SEG_H_
#define ESDR_SEG_H_

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esdr_proto.h"

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "esdr_seg.h copies the little endian wire headers as they are"
#endif

/*
 * Attributes of every function here, none by default. The C6 sink writes with interrupts off:
 * it defines this as ESP_SDR_GAPLESS_HOT (IRAM, optimized as hot) before the include, so that
 * a function the compiler keeps out of line does not run from flash.
 */
#ifndef ESDR_SEG_HOT
#define ESDR_SEG_HOT
#endif

#define ESDR_SEG_BLK_HDR_BYTES ((uint32_t)sizeof(struct esdr_block_hdr))
#define ESDR_SEG_HDR_BYTES     ((uint32_t)sizeof(struct esdr_seg_hdr))
/*
 * Largest shift the reader takes (the proto uses 0 to 2). At 7 an item shifted left stays in
 * [-16384, 16256], so it, its negation and the sum or difference of two still fit an int16
 * (8 would fit only the item itself).
 */
#define ESDR_SEG_SHIFT_MAX     7U
/* Block flags of other kinds of blocks, which a segment block never carries. */
#define ESDR_SEG_BLK_OTHER     (ESDR_BLK_KNOWN & ~(ESDR_BLK_SEG | ESDR_BLK_GAP | ESDR_BLK_OVERFLOW))

/* Offsets stay 4-byte aligned only if both headers are whole words; a full block's pairs fit. */
typedef char esdr_seg_check_hdrs
	[ESDR_SEG_BLK_HDR_BYTES == 16U && ESDR_SEG_HDR_BYTES == 16U ? 1 : -1];
typedef char esdr_seg_check_max
	[ESDR_SEG_PAIRS_MAX <= UINT16_MAX && ESDR_SEG_PAIRS_MAX % 2U == 0U ? 1 : -1];

/* ---- writer ---- */

/*
 * A segment's header besides where it is (struct esdr_seg_hdr): the shift, the flags and power
 * of its first window, the receiver's DC left in the items, the receive gain index.
 */
struct esdr_seg_meta {
	uint8_t shift, flags, gain;
	int8_t dc_i, dc_q;
	int16_t power_dbfs_x10;
};

struct esdr_seg_writer {
	uint8_t *buf;
	/* The first segment's index (the block header's), and the index after the last segment. */
	uint64_t index, end;
	/* Bytes in use (where the last segment's pairs end, unpadded); the last header, 0: none. */
	uint32_t pos, last;
	/* Pairs of all segments; the last segment's shift and gain. */
	uint32_t samples;
	uint8_t shift, gain;
};

/* Starts a block in @p buf, ESDR_BLOCK_BYTES long. */
static inline ESDR_SEG_HOT void esdr_seg_writer_begin(struct esdr_seg_writer *w, void *buf)
{
	w->buf = (uint8_t *)buf;
	w->index = 0U;
	w->end = 0U;
	w->pos = ESDR_SEG_BLK_HDR_BYTES;
	w->last = 0U;
	w->samples = 0U;
	w->shift = 0U;
	w->gain = 0U;
}

/* Pairs a new segment could take, 0 when the block is full. Always even. */
static inline ESDR_SEG_HOT uint32_t esdr_seg_writer_room(const struct esdr_seg_writer *w)
{
	uint32_t at = (w->pos + 3U) & ~3U;

	return at + ESDR_SEG_HDR_BYTES < ESDR_BLOCK_BYTES ?
		       (ESDR_BLOCK_BYTES - at - ESDR_SEG_HDR_BYTES) >> 1 : 0U;
}

/*
 * Starts a segment of up to @p pairs pairs at pair @p index (80 MS/s axis), its header from
 * @p m, and points @p dst at its items; returns how many pairs fit, and the caller writes twice
 * that many int8 items there. 0 when the block cannot take the segment: no room for a header
 * and a pair, an index before the last segment's end, or one 2^32 pairs or more past the
 * block's first; the caller then finishes the block and adds to the next. An empty block takes
 * any index, so a fresh block always takes at least one pair.
 */
static inline ESDR_SEG_HOT uint32_t
esdr_seg_writer_add(struct esdr_seg_writer *w, uint64_t index, uint32_t pairs,
		    const struct esdr_seg_meta *m, int8_t **dst)
{
	/* Locals, as the byte stores below may alias the writer and @p m. */
	uint8_t *buf = w->buf;
	uint32_t pos = w->pos, at = (pos + 3U) & ~3U;
	uint32_t n = esdr_seg_writer_room(w);
	struct esdr_seg_hdr h;

	if (pairs < n) {
		n = pairs;
	}
	if (n == 0U) {
		return 0U;
	}
	if (w->last == 0U) {
		w->index = index;
	} else if (index < w->end || index - w->index > UINT32_MAX) {
		return 0U;
	}
	h.delta = (uint32_t)(index - w->index);
	h.pairs = (uint16_t)n;
	h.shift = m->shift;
	h.flags = m->flags;
	h.power_dbfs_x10 = m->power_dbfs_x10;
	h.dc_i = m->dc_i;
	h.dc_q = m->dc_q;
	h.gain = m->gain;
	memset(h.reserved, 0, sizeof(h.reserved));
	/* The last segment's padding: pos is even, so 0 or 2 bytes. */
	if (at != pos) {
		buf[pos] = 0U;
		buf[pos + 1U] = 0U;
	}
	memcpy(buf + at, &h, sizeof(h));
	*dst = (int8_t *)(buf + at + ESDR_SEG_HDR_BYTES);
	w->last = at;
	w->pos = at + ESDR_SEG_HDR_BYTES + 2U * n;
	w->end = index + n;
	w->samples += n;
	w->shift = h.shift;
	w->gain = h.gain;
	return n;
}

/*
 * Appends up to @p pairs pairs at @p index to the last segment when they follow it without a
 * gap at the same shift and gain (@p m's); returns how many fit and points @p dst at their
 * items as esdr_seg_writer_add() does. 0 when they do not follow or the block is full. The
 * segment's flags, power and DC stay those of its first window.
 */
static inline ESDR_SEG_HOT uint32_t
esdr_seg_writer_extend(struct esdr_seg_writer *w, uint64_t index, uint32_t pairs,
		       const struct esdr_seg_meta *m, int8_t **dst)
{
	/* The segment may grow to the block's end: from a word boundary, an even pair count. */
	uint32_t n = (ESDR_BLOCK_BYTES - w->pos) >> 1;
	uint16_t total;

	if (w->last == 0U || index != w->end || m->shift != w->shift || m->gain != w->gain) {
		return 0U;
	}
	if (pairs < n) {
		n = pairs;
	}
	if (n == 0U) {
		return 0U;
	}
	total = (uint16_t)(((w->pos - w->last - ESDR_SEG_HDR_BYTES) >> 1) + n);
	memcpy(w->buf + w->last + offsetof(struct esdr_seg_hdr, pairs), &total, sizeof(total));
	*dst = (int8_t *)(w->buf + w->pos);
	w->pos += 2U * n;
	w->end += n;
	w->samples += n;
	return n;
}

/*
 * A kept window: extends the last segment when the window follows it at the same shift and
 * gain and has no ESDR_SEG_DROPPED or ESDR_SEG_RESTART to report, else starts a segment.
 * Returns what fits as esdr_seg_writer_add() does; a window that fits only in part goes on in
 * the next block.
 */
static inline ESDR_SEG_HOT uint32_t
esdr_seg_writer_put(struct esdr_seg_writer *w, uint64_t index, uint32_t pairs,
		    const struct esdr_seg_meta *m, int8_t **dst)
{
	if ((m->flags & (ESDR_SEG_DROPPED | ESDR_SEG_RESTART)) == 0U) {
		uint32_t n = esdr_seg_writer_extend(w, index, pairs, m, dst);

		/* A following window that found no room would not fit a new segment either. */
		if (n != 0U) {
			return n;
		}
	}
	return esdr_seg_writer_add(w, index, pairs, m, dst);
}

/*
 * Closes the block: its header (ESDR_BLK_SEG and @p flags, such as ESDR_BLK_GAP), the last
 * segment's padding, the zero header if it fits and zeros to the end. Returns the block's
 * length, always ESDR_BLOCK_BYTES (blocks are fixed size on the link). An empty block is
 * valid: no samples, index 0.
 */
static inline ESDR_SEG_HOT size_t esdr_seg_writer_finish(struct esdr_seg_writer *w, uint16_t flags)
{
	struct esdr_block_hdr h;

	h.magic = ESDR_BLOCK_MAGIC;
	h.flags = (uint16_t)(flags | ESDR_BLK_SEG);
	h.samples = (uint16_t)w->samples;
	h.index = w->index;
	memset(w->buf + w->pos, 0, ESDR_BLOCK_BYTES - w->pos);
	memcpy(w->buf, &h, sizeof(h));
	return ESDR_BLOCK_BYTES;
}

/* ---- reader ---- */

struct esdr_seg {
	/* The first pair's index on the 80 MS/s axis. */
	uint64_t index;
	uint16_t pairs;
	uint8_t shift, flags;
	int16_t power_dbfs_x10;
	/* The receiver's DC left in the items (raw units), the receive gain index. */
	int8_t dc_i, dc_q;
	uint8_t gain;
	/* 2 * pairs items, I then Q, inside the block. */
	const int8_t *iq;
};

/* Takes each segment of a block in turn; a negative return stops the walk and is returned. */
typedef int (*esdr_seg_cb)(const struct esdr_seg *seg, void *user);

/*
 * Copies the header of the block at @p block, @p len bytes there, to @p hdr if it is a
 * segment block. 0, -EINVAL when shorter than a header, -EBADMSG when not a segment block
 * (magic, or the flags of another kind).
 */
static inline ESDR_SEG_HOT int esdr_seg_block_hdr(const void *block, size_t len,
						  struct esdr_block_hdr *hdr)
{
	if (block == NULL || len < sizeof(*hdr)) {
		return -EINVAL;
	}
	memcpy(hdr, block, sizeof(*hdr));
	if (hdr->magic != ESDR_BLOCK_MAGIC || (hdr->flags & ESDR_BLK_SEG) == 0U ||
	    (hdr->flags & ESDR_SEG_BLK_OTHER) != 0U) {
		return -EBADMSG;
	}
	return 0;
}

/*
 * The walk behind esdr_seg_parse() over a block of @p len bytes (at most ESDR_BLOCK_BYTES)
 * whose header @p bh passed esdr_seg_block_hdr(): checks each segment, then hands it to @p cb
 * if there is one. The segment count, or -EPROTO for a malformed payload.
 */
static inline ESDR_SEG_HOT int esdr_seg_walk(const uint8_t *b, size_t len,
					     const struct esdr_block_hdr *bh,
					     esdr_seg_cb cb, void *user)
{
	size_t pos = sizeof(*bh);
	/* Delta of the pair after the last segment. */
	uint64_t end = 0U;
	uint32_t total = 0U;
	int count = 0;

	while (len - pos >= sizeof(struct esdr_seg_hdr)) {
		struct esdr_seg_hdr h;
		size_t bytes;

		memcpy(&h, b + pos, sizeof(h));
		if (h.pairs == 0U) {
			break;
		}
		pos += sizeof(h);
		bytes = 2U * (size_t)h.pairs;
		/* Pairs past the end; the first segment not at the block's index; an overlap. */
		if (bytes > len - pos || (count == 0 ? h.delta != 0U : h.delta < end)) {
			return -EPROTO;
		}
		end = (uint64_t)h.delta + h.pairs;
		/* An index past the axis's end; a shift above ESDR_SEG_SHIFT_MAX. */
		if (bh->index > UINT64_MAX - end || h.shift > ESDR_SEG_SHIFT_MAX) {
			return -EPROTO;
		}
		if (cb != NULL) {
			struct esdr_seg s;
			int ret;

			s.index = bh->index + h.delta;
			s.pairs = h.pairs;
			s.shift = h.shift;
			s.flags = h.flags;
			s.power_dbfs_x10 = h.power_dbfs_x10;
			s.dc_i = h.dc_i;
			s.dc_q = h.dc_q;
			s.gain = h.gain;
			s.iq = (const int8_t *)(b + pos);
			ret = cb(&s, user);
			if (ret < 0) {
				return ret;
			}
		}
		total += h.pairs;
		count++;
		/* The padding, which a block cut short right after the pairs may lack. */
		pos += (bytes + 3U) & ~(size_t)3U;
		if (pos > len) {
			pos = len;
		}
	}
	return total == bh->samples ? count : -EPROTO;
}

/*
 * Walks the segment block at @p block (any alignment), @p len bytes there of which the first
 * ESDR_BLOCK_BYTES are looked at; a block cut short after its last segment is still whole.
 * Hands each segment to @p cb (may be NULL: only checks) with @p user, after the whole block
 * passed the checks, so a malformed block yields none. Returns the segment count, a negative
 * error (esdr_seg_block_hdr(), -EPROTO for a malformed payload), or what @p cb returned to
 * stop.
 */
static inline ESDR_SEG_HOT int esdr_seg_parse(const void *block, size_t len,
					      esdr_seg_cb cb, void *user)
{
	struct esdr_block_hdr bh;
	int ret = esdr_seg_block_hdr(block, len, &bh);

	if (ret < 0) {
		return ret;
	}
	if (len > ESDR_BLOCK_BYTES) {
		len = ESDR_BLOCK_BYTES;
	}
	ret = esdr_seg_walk((const uint8_t *)block, len, &bh, NULL, NULL);
	if (ret <= 0 || cb == NULL) {
		return ret;
	}
	return esdr_seg_walk((const uint8_t *)block, len, &bh, cb, user);
}

/* ---- continuity ---- */

/* esdr_seg_track_gap(): the first segment tracked, nothing before it to compare with. */
#define ESDR_SEG_GAP_START (UINT64_MAX - 1U)
/* esdr_seg_track_gap(): the segment starts before the previous one ended (a restart). */
#define ESDR_SEG_GAP_BACK  UINT64_MAX
/* Real gaps are clamped below the two markers. */
#define ESDR_SEG_GAP_MAX   (UINT64_MAX - 2U)

struct esdr_seg_track {
	/* The index after the last segment. */
	uint64_t next;
	bool valid;
};

static inline ESDR_SEG_HOT void esdr_seg_track_reset(struct esdr_seg_track *t)
{
	t->next = 0U;
	t->valid = false;
}

/*
 * Pairs missing before @p seg since the last segment tracked: 0 when it follows without a gap,
 * or ESDR_SEG_GAP_START / ESDR_SEG_GAP_BACK. Any other value than 0 is a discontinuity.
 */
static inline ESDR_SEG_HOT uint64_t esdr_seg_track_gap(struct esdr_seg_track *t,
						       const struct esdr_seg *seg)
{
	uint64_t gap;

	if (!t->valid) {
		gap = ESDR_SEG_GAP_START;
	} else if (seg->index < t->next) {
		gap = ESDR_SEG_GAP_BACK;
	} else {
		gap = seg->index - t->next;
		if (gap > ESDR_SEG_GAP_MAX) {
			gap = ESDR_SEG_GAP_MAX;
		}
	}
	t->next = seg->index + seg->pairs;
	t->valid = true;
	return gap;
}

#endif /* ESDR_SEG_H_ */
