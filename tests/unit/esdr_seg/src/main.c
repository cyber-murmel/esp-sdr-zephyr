/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Unit test of apps/osmosdr/src/esdr_seg.h, the gapless block's segment codec: random layouts
 * written and read back against a model of the format (segment merges, offsets, header fields
 * on the wire and their reserved bytes, contents, padding, the zero header and tail,
 * continuity), window extension (the shift and gain must match, the DC need not), full blocks,
 * the largest segment, and malformed blocks (cut at every length, pairs past the end, bad magic
 * and flags, deltas that overlap or overflow, random corruption), which must be rejected before
 * any segment is handed out. The reader always gets a copy that ends where its length does; in
 * the .asan scenario the bytes after it are poisoned, so a read past them aborts the test.
 */
#include <string.h>

#include <zephyr/ztest.h>

/* The codec's functions with attributes, as the C6 sink gives them (there: IRAM and hot). */
#define ESDR_SEG_HOT __attribute__((section(".text.esdr_seg"), hot))
#include "esdr_seg.h"

#if defined(__SANITIZE_ADDRESS__)
#define HAVE_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define HAVE_ASAN 1
#endif
#endif

#ifdef HAVE_ASAN
void __asan_poison_memory_region(void const volatile *addr, size_t size);
void __asan_unpoison_memory_region(void const volatile *addr, size_t size);
#endif

#define BLK     ESDR_BLOCK_BYTES
#define HDR     ESDR_SEG_BLK_HDR_BYTES
#define SHDR    ESDR_SEG_HDR_BYTES
/* Segments in a block: each takes a header and a padded pair at least. */
#define SEGS_MAX ((BLK - HDR) / (SHDR + 4U))

/* Header field offsets on the wire. */
#define BH_FLAGS   4U
#define BH_SAMPLES 6U
#define BH_INDEX   8U
#define SH_DELTA   0U
#define SH_PAIRS   4U
#define SH_SHIFT   6U
#define SH_FLAGS   7U
#define SH_POWER   8U
#define SH_DC_I    10U
#define SH_DC_Q    11U
#define SH_GAIN    12U
#define SH_RSVD    13U

/* Header fields in struct esdr_seg_meta's order: shift, flags, gain, dc_i, dc_q, power. */
#define META_G(s, f, g, di, dq, p) (&(const struct esdr_seg_meta){(s), (f), (g), (di), (dq), (p)})
/* The same at gain 66 with a DC of (5, -7). */
#define META(s, f, p) META_G(s, f, 66, 5, -7, p)

ZTEST_SUITE(esdr_seg, NULL, NULL, NULL, NULL, NULL);

/* ---- helpers ---- */

static uint32_t rng;

static uint32_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

/* Uniform in [0, n). */
static uint32_t rnd_below(uint32_t n)
{
	return (uint32_t)(((uint64_t)rnd() * n) >> 32);
}

static uint32_t round4(uint32_t n)
{
	return (n + 3U) & ~3U;
}

/* Item q (0: I, 1: Q) of the pair at @p index: a hash of both, so that misplaced pairs show. */
static int8_t item(uint64_t index, unsigned int q)
{
	uint32_t x = (uint32_t)index ^ ((uint32_t)(index >> 32) * 0x9e3779b9U) ^ (q * 0x5bd1e995U);

	x ^= x >> 16;
	x *= 0x7feb352dU;
	x ^= x >> 15;
	x *= 0x846ca68bU;
	x ^= x >> 16;
	return (int8_t)x;
}

static void fill(int8_t *dst, uint64_t index, uint32_t n)
{
	for (uint32_t k = 0; k < n; k++) {
		dst[2U * k] = item(index + k, 0);
		dst[2U * k + 1U] = item(index + k, 1);
	}
}

static bool items_match(const int8_t *iq, uint64_t index, uint32_t n)
{
	for (uint32_t k = 0; k < n; k++) {
		if (iq[2U * k] != item(index + k, 0) || iq[2U * k + 1U] != item(index + k, 1)) {
			return false;
		}
	}
	return true;
}

static void put16(uint8_t *p, uint16_t v)
{
	memcpy(p, &v, sizeof(v));
}

static void put32(uint8_t *p, uint32_t v)
{
	memcpy(p, &v, sizeof(v));
}

static void put64(uint8_t *p, uint64_t v)
{
	memcpy(p, &v, sizeof(v));
}

static uint16_t get16(const uint8_t *p)
{
	uint16_t v;

	memcpy(&v, p, sizeof(v));
	return v;
}

static uint32_t get32(const uint8_t *p)
{
	uint32_t v;

	memcpy(&v, p, sizeof(v));
	return v;
}

static uint64_t get64(const uint8_t *p)
{
	uint64_t v;

	memcpy(&v, p, sizeof(v));
	return v;
}

/* Segment @p k as the reader handed it out against the header fields it was written with. */
static void check_meta(const struct esdr_seg *s, const struct esdr_seg_meta *e, unsigned int k)
{
	zassert_equal(s->shift, e->shift, "segment %u shift", k);
	zassert_equal(s->flags, e->flags, "segment %u flags", k);
	zassert_equal(s->gain, e->gain, "segment %u gain %u, written %u", k, s->gain, e->gain);
	zassert_equal(s->dc_i, e->dc_i, "segment %u dc_i %d, written %d", k, s->dc_i, e->dc_i);
	zassert_equal(s->dc_q, e->dc_q, "segment %u dc_q %d, written %d", k, s->dc_q, e->dc_q);
	zassert_equal(s->power_dbfs_x10, e->power_dbfs_x10, "segment %u power", k);
}

/* A segment header on the wire: each field where the format puts it, the reserved bytes zero. */
static void check_wire(const uint8_t *h, uint32_t delta, uint32_t pairs,
		       const struct esdr_seg_meta *e)
{
	zassert_equal(get32(h + SH_DELTA), delta);
	zassert_equal(get16(h + SH_PAIRS), pairs);
	zassert_equal(h[SH_SHIFT], e->shift);
	zassert_equal(h[SH_FLAGS], e->flags);
	zassert_equal((int16_t)get16(h + SH_POWER), e->power_dbfs_x10);
	zassert_equal((int8_t)h[SH_DC_I], e->dc_i);
	zassert_equal((int8_t)h[SH_DC_Q], e->dc_q);
	zassert_equal(h[SH_GAIN], e->gain);
	for (uint32_t i = SH_RSVD; i < SHDR; i++) {
		zassert_equal(h[i], 0U, "reserved byte %u is %#x", i, h[i]);
	}
}

/*
 * The reader's input: a copy of @p len bytes at offset @p skew (any alignment) that ends where
 * the arena's addressable bytes do. Without the sanitizer the rest is filled with a pattern.
 */
static uint8_t arena[2U * BLK + 16U] __aligned(8);

static const uint8_t *fence(const void *src, size_t len, unsigned int skew)
{
	zassert_true(skew + len <= sizeof(arena));
#ifdef HAVE_ASAN
	__asan_unpoison_memory_region(arena, sizeof(arena));
#endif
	memset(arena, 0xa5, sizeof(arena));
	memcpy(arena + skew, src, len);
#ifdef HAVE_ASAN
	__asan_poison_memory_region(arena + skew + len, sizeof(arena) - skew - len);
#endif
	return arena + skew;
}

/* A callback that collects the segments and checks that each lies inside the block. */
struct collect {
	const uint8_t *base;
	size_t len;
	unsigned int n;
	int stop_at, stop_ret;
	struct esdr_seg seg[SEGS_MAX];
};

static int collect_cb(const struct esdr_seg *seg, void *user)
{
	struct collect *c = user;
	const uint8_t *iq = (const uint8_t *)seg->iq;

	zassert_true(iq >= c->base + HDR + SHDR && iq + 2U * seg->pairs <= c->base + c->len,
		     "segment %u outside the block", c->n);
	zassert_true(seg->pairs > 0U);
	zassert_true(c->n < SEGS_MAX);
	c->seg[c->n++] = *seg;
	if (c->stop_at != 0 && (int)c->n == c->stop_at) {
		return c->stop_ret;
	}
	return 0;
}

/* esdr_seg_parse() on a fenced copy; @p c (may be NULL) collects the segments. */
static int parse(const void *block, size_t len, unsigned int skew, struct collect *c)
{
	const uint8_t *b = fence(block, len, skew);

	if (c == NULL) {
		return esdr_seg_parse(b, len, NULL, NULL);
	}
	c->base = b;
	c->len = len;
	c->n = 0;
	return esdr_seg_parse(b, len, collect_cb, c);
}

/* Header offset of a segment the reader collected in @p c. */
static uint32_t coff(const struct collect *c, unsigned int k)
{
	return (uint32_t)((const uint8_t *)c->seg[k].iq - c->base) - SHDR;
}

/* The model: what the block must hold, and where the format puts it. */
struct mseg {
	uint64_t index;
	uint32_t pairs, offset;
	struct esdr_seg_meta meta;
};

struct model {
	unsigned int n;
	/* Where the last segment's pairs end (unpadded). */
	uint32_t pos;
	struct mseg seg[SEGS_MAX];
};

static void model_begin(struct model *m)
{
	m->n = 0;
	m->pos = HDR;
}

static uint64_t mend(const struct model *m)
{
	return m->seg[m->n - 1U].index + m->seg[m->n - 1U].pairs;
}

/* Continuity as the model sees it, across blocks. */
struct mtrack {
	uint64_t next;
	bool valid;
};

static uint64_t mtrack_gap(struct mtrack *t, uint64_t index, uint32_t pairs)
{
	uint64_t gap = !t->valid ? ESDR_SEG_GAP_START :
		       index < t->next ? ESDR_SEG_GAP_BACK : index - t->next;

	t->next = index + pairs;
	t->valid = true;
	return gap;
}

/*
 * A finished block against the model: header, segments and items (read at a random alignment,
 * whole and cut after the last pair), segment headers on the wire, offsets back to back from the
 * payload's start, zeros everywhere else, and the continuity tracker.
 */
static void check_block(const uint8_t *buf, const struct model *m, uint16_t flags,
			struct esdr_seg_track *t, struct mtrack *mt)
{
	static struct collect c;
	static bool used[BLK];
	uint32_t samples = 0, at = HDR;
	int ret;

	for (unsigned int k = 0; k < m->n; k++) {
		samples += m->seg[k].pairs;
	}
	zassert_equal(get32(buf), ESDR_BLOCK_MAGIC);
	zassert_equal(get16(buf + BH_FLAGS), flags | ESDR_BLK_SEG);
	zassert_equal(get16(buf + BH_SAMPLES), samples);
	zassert_equal(get64(buf + BH_INDEX), m->n != 0U ? m->seg[0].index : 0U);

	ret = parse(buf, BLK, rnd_below(8), &c);
	zassert_equal(ret, (int)m->n, "parsed %d segments, model %u", ret, m->n);
	zassert_equal(c.n, m->n);
	memset(used, 0, sizeof(used));
	memset(used, 1, HDR);
	for (unsigned int k = 0; k < m->n; k++) {
		const struct esdr_seg *s = &c.seg[k];
		const struct mseg *e = &m->seg[k];
		uint32_t off = coff(&c, k);

		zassert_equal(s->index, e->index, "segment %u index %llu, model %llu", k,
			      (unsigned long long)s->index, (unsigned long long)e->index);
		zassert_equal(s->pairs, e->pairs, "segment %u pairs %u, model %u", k, s->pairs,
			      e->pairs);
		check_meta(s, &e->meta, k);
		zassert_equal(off, at, "segment %u at %u, expected %u", k, off, at);
		zassert_equal(off, e->offset);
		check_wire(buf + off, (uint32_t)(e->index - m->seg[0].index), e->pairs, &e->meta);
		zassert_true(items_match(s->iq, s->index, s->pairs), "segment %u items", k);
		zassert_equal(mtrack_gap(mt, e->index, e->pairs), esdr_seg_track_gap(t, s));
		memset(used + off, 1, SHDR + 2U * s->pairs);
		at = round4(off + SHDR + 2U * s->pairs);
	}
	for (uint32_t i = 0; i < BLK; i++) {
		zassert_true(used[i] || buf[i] == 0U, "byte %u not zero", i);
	}
	/* Cut right after the last pair: still the whole block. */
	ret = parse(buf, MAX(m->pos, HDR), rnd_below(8), NULL);
	zassert_equal(ret, (int)m->n, "cut block parsed %d", ret);
}

/* A block of segments of @p pairs, @p gap pairs apart, from @p base; returns the count. */
static unsigned int build(uint8_t *buf, uint64_t base, const uint32_t *pairs, unsigned int n,
			  uint32_t gap)
{
	struct esdr_seg_writer w;

	memset(buf, 0xa5, BLK);
	esdr_seg_writer_begin(&w, buf);
	for (unsigned int k = 0; k < n; k++) {
		int8_t *dst;

		zassert_equal(esdr_seg_writer_add(&w, base, pairs[k], META(1, ESDR_SEG_LOUD, -100),
						  &dst), pairs[k]);
		fill(dst, base, pairs[k]);
		base += pairs[k] + gap;
	}
	zassert_equal(esdr_seg_writer_finish(&w, 0), BLK);
	return n;
}

/* Header offset of segment @p k of a block built by build(). */
static uint32_t seg_off(const uint32_t *pairs, unsigned int k)
{
	uint32_t off = HDR;

	for (unsigned int i = 0; i < k; i++) {
		off += SHDR + round4(2U * pairs[i]);
	}
	return off;
}

/* ---- writer and reader together ---- */

ZTEST(esdr_seg, test_round_trip_random)
{
	static uint8_t buf[BLK];
	static struct model m;
	struct esdr_seg_writer w;
	struct esdr_seg_track t;
	struct mtrack mt = {0};
	struct esdr_seg_meta meta = {.shift = 1, .gain = 60};
	uint64_t index = 0x123456789ULL, pairs_total = 0;
	unsigned int blocks = 0, segs = 0, merged = 0, splits = 0, refused = 0;
	unsigned int dc_merged = 0, gain_splits = 0;

	rng = 0x2545f491U;
	esdr_seg_track_reset(&t);
	memset(buf, 0xa5, sizeof(buf));
	esdr_seg_writer_begin(&w, buf);
	model_begin(&m);

	for (unsigned int win = 0; win < 40000U; win++) {
		uint32_t r = rnd_below(100), pairs;
		int16_t power = -(int16_t)rnd_below(900);
		bool use_add = rnd_below(8) == 0U;

		/* Where the window starts: mostly after the last, sometimes far, rarely back. */
		if (r < 45U) {
		} else if (r < 70U) {
			index += 1U + rnd_below(5000);
		} else if (r < 85U) {
			index += (uint64_t)(1U + rnd_below(64)) * ESDR_GAPLESS_WINDOW_PAIRS;
		} else if (r < 94U) {
			index += rnd();
		} else if (r < 98U) {
			/* Around the limit of the block's deltas. */
			if (m.n != 0U && m.seg[0].index + UINT32_MAX - 1U >= index) {
				index = m.seg[0].index + UINT32_MAX - 1U + rnd_below(3);
			} else {
				index += 1U;
			}
		} else {
			index -= 1U + rnd_below(100000);
		}
		r = rnd_below(100);
		if (r < 60U) {
			pairs = ESDR_GAPLESS_WINDOW_PAIRS;
		} else if (r < 75U) {
			pairs = 1U + rnd_below(2U * ESDR_GAPLESS_WINDOW_PAIRS);
		} else if (r < 85U) {
			pairs = 1U + rnd_below(8);
		} else if (r < 95U) {
			pairs = (2U + rnd_below(3)) * ESDR_GAPLESS_WINDOW_PAIRS;
		} else {
			pairs = (uint32_t)ESDR_SEG_PAIRS_MAX - 1U + rnd_below(3);
		}
		if (rnd_below(10) < 3U) {
			meta.shift = (uint8_t)rnd_below(3);
		}
		/* The gain servo steps now and then (to automatic gain too), the DC moves often. */
		if (rnd_below(10) == 0U) {
			r = rnd_below(50);
			meta.gain = r < 40U ? (uint8_t)(40U + r) : ESDR_SEG_GAIN_AUTO;
		}
		if (rnd_below(2) == 0U) {
			meta.dc_i = (int8_t)((int32_t)rnd_below(256) - 128);
			meta.dc_q = (int8_t)((int32_t)rnd_below(256) - 128);
		}
		r = rnd_below(32);
		meta.flags = (r & 1U ? ESDR_SEG_LOUD : 0U) | (r < 2U ? ESDR_SEG_DROPPED : 0U) |
			     (r == 2U || r == 5U ? ESDR_SEG_RESTART : 0U);
		meta.power_dbfs_x10 = power;

		while (pairs > 0U) {
			struct mseg *last = m.n != 0U ? &m.seg[m.n - 1U] : NULL;
			/* Follows the last segment as extension needs it, but for the gain. */
			bool cont = last != NULL && !use_add && index == mend(&m) &&
				    meta.shift == last->meta.shift &&
				    (meta.flags & (ESDR_SEG_DROPPED | ESDR_SEG_RESTART)) == 0U;
			bool follows = cont && meta.gain == last->meta.gain;
			uint32_t at = round4(m.pos), want, n;
			int8_t *dst = NULL;

			/* What the format leaves room for. */
			if (follows) {
				want = MIN(pairs, (BLK - m.pos) / 2U);
				if (want == 0U) {
					follows = false;
				}
			}
			if (!follows) {
				bool fits = last == NULL || (index >= mend(&m) &&
							     index - m.seg[0].index <= UINT32_MAX);

				want = fits && at + SHDR < BLK ? MIN(pairs, (BLK - at - SHDR) / 2U) : 0U;
			}
			n = use_add ? esdr_seg_writer_add(&w, index, pairs, &meta, &dst)
				    : esdr_seg_writer_put(&w, index, pairs, &meta, &dst);
			zassert_equal(n, want, "window %u: %u pairs fit, model %u", win, n, want);
			zassert_true(n != 0U || last != NULL, "a fresh block takes a segment");
			if (n != 0U) {
				uint32_t off = follows ? m.pos : at + SHDR;

				zassert_equal_ptr(dst, (int8_t *)buf + off);
				fill(dst, index, n);
				if (follows) {
					/* The header stays the first window's, DC included. */
					dc_merged += meta.dc_i != last->meta.dc_i ||
						     meta.dc_q != last->meta.dc_q;
					last->pairs += n;
					merged++;
				} else {
					gain_splits += cont && meta.gain != last->meta.gain;
					m.seg[m.n++] = (struct mseg){
						.index = index, .pairs = n, .offset = at,
						.meta = meta,
					};
				}
				m.pos = off + 2U * n;
				index += n;
				pairs -= n;
				pairs_total += n;
			}
			/* Full (or the window does not go here), and now and then a flush. */
			if (pairs > 0U || rnd_below(64) == 0U) {
				uint16_t bf = (uint16_t)(rnd_below(4) & (ESDR_BLK_GAP | ESDR_BLK_OVERFLOW));

				splits += pairs > 0U && n > 0U;
				refused += n == 0U;
				zassert_equal(esdr_seg_writer_finish(&w, bf), BLK);
				check_block(buf, &m, bf, &t, &mt);
				blocks++;
				segs += m.n;
				memset(buf, 0xa5, sizeof(buf));
				esdr_seg_writer_begin(&w, buf);
				model_begin(&m);
			}
		}
	}
	printk("%u blocks, %u segments, %u windows merged (%u at another DC), %u split by the "
	       "gain, %u split, %u refused, payload use %u per mille\n", blocks, segs, merged,
	       dc_merged, gain_splits, splits, refused,
	       (unsigned int)(pairs_total * 2000U / ((uint64_t)blocks * ESDR_BLOCK_PAYLOAD)));
	zassert_true(blocks > 1000U && merged > 1000U && splits > 1000U && refused > 100U);
	zassert_true(dc_merged > 1000U && gain_splits > 300U);
}

ZTEST(esdr_seg, test_extend_windows)
{
	static uint8_t buf[BLK];
	static struct collect c;
	enum { PUT, ADD, EXT };
	/* Header fields: shift, flags, gain, dc_i, dc_q, power. */
	static const struct {
		uint8_t op;
		uint32_t index, pairs, fit;
		struct esdr_seg_meta m;
	} step[] = {
		/*
		 * A fresh block has nothing to extend, though index 0, shift 0 and gain 0 match the
		 * writer's reset state: put starts a segment there.
		 */
		{EXT, 0, 5, 0, {0, 0, 0, 0, 0, 0}},
		{PUT, 0, 5, 5, {0, 0, 0, 0, 0, 0}},
		/* Two windows in a row, the DC moved: one segment, the first window's header. */
		{PUT, 1000, 800, 800, {1, ESDR_SEG_LOUD, 60, 5, -6, -200}},
		{PUT, 1800, 800, 800, {1, 0, 60, 9, -1, -300}},
		/* Another gain, another shift, a drop to report, a gap: new segments each. */
		{PUT, 2600, 30, 30, {1, 0, 61, 9, -1, -305}},
		{PUT, 2630, 100, 100, {2, 0, 61, 9, -1, -310}},
		{PUT, 2730, 101, 101, {2, ESDR_SEG_DROPPED, 61, 9, -1, -320}},
		{PUT, 2930, 50, 50, {2, 0, 61, 9, -1, -330}},
		/* Extension: no gap or overlap, the same shift and gain, some pairs; any DC. */
		{EXT, 2980, 10, 0, {1, 0, 61, 9, -1, -330}},
		{EXT, 2980, 10, 0, {2, 0, 62, 9, -1, -330}},
		{EXT, 2980, 10, 0, {2, 0, ESDR_SEG_GAIN_AUTO, 9, -1, -330}},
		{EXT, 2981, 10, 0, {2, 0, 61, 9, -1, -330}},
		{EXT, 2979, 10, 0, {2, 0, 61, 9, -1, -330}},
		{EXT, 2980, 0, 0, {2, 0, 61, 9, -1, -330}},
		{EXT, 2980, 10, 10, {2, ESDR_SEG_LOUD, 61, -128, 127, -1}},
		/* Add always starts a segment, which extends; automatic gain is a gain like any. */
		{ADD, 2990, 7, 7, {2, ESDR_SEG_LOUD, 61, 1, 2, -340}},
		{EXT, 2997, 9, 9, {2, 0, 61, 3, 3, -345}},
		{PUT, 3006, 5, 5, {2, 0, ESDR_SEG_GAIN_AUTO, 0, 0, -350}},
		{PUT, 3011, 5, 5, {2, 0, ESDR_SEG_GAIN_AUTO, 4, 4, -360}},
	};
	static const struct {
		uint64_t index;
		uint16_t pairs;
		struct esdr_seg_meta m;
		uint64_t gap;
	} want[] = {
		{0, 5, {0, 0, 0, 0, 0, 0}, ESDR_SEG_GAP_START},
		{1000, 1600, {1, ESDR_SEG_LOUD, 60, 5, -6, -200}, 995},
		{2600, 30, {1, 0, 61, 9, -1, -305}, 0},
		{2630, 100, {2, 0, 61, 9, -1, -310}, 0},
		{2730, 101, {2, ESDR_SEG_DROPPED, 61, 9, -1, -320}, 0},
		{2930, 60, {2, 0, 61, 9, -1, -330}, 99},
		{2990, 16, {2, ESDR_SEG_LOUD, 61, 1, 2, -340}, 0},
		{3006, 10, {2, 0, ESDR_SEG_GAIN_AUTO, 0, 0, -350}, 0},
	};
	struct esdr_seg_writer w;
	struct esdr_seg_track t;
	uint32_t samples = 0;

	memset(buf, 0xa5, sizeof(buf));
	esdr_seg_writer_begin(&w, buf);
	for (unsigned int k = 0; k < ARRAY_SIZE(step); k++) {
		const struct esdr_seg_meta *m = &step[k].m;
		int8_t *dst = NULL;
		uint32_t n;

		if (step[k].op == PUT) {
			n = esdr_seg_writer_put(&w, step[k].index, step[k].pairs, m, &dst);
		} else if (step[k].op == ADD) {
			n = esdr_seg_writer_add(&w, step[k].index, step[k].pairs, m, &dst);
		} else {
			n = esdr_seg_writer_extend(&w, step[k].index, step[k].pairs, m, &dst);
		}
		zassert_equal(n, step[k].fit, "step %u: %u pairs fit", k, n);
		if (n != 0U) {
			fill(dst, step[k].index, n);
		}
	}
	esdr_seg_writer_finish(&w, 0);

	zassert_equal(parse(buf, BLK, 3, &c), ARRAY_SIZE(want));
	esdr_seg_track_reset(&t);
	for (unsigned int k = 0; k < ARRAY_SIZE(want); k++) {
		zassert_equal(c.seg[k].index, want[k].index, "segment %u", k);
		zassert_equal(c.seg[k].pairs, want[k].pairs, "segment %u", k);
		check_meta(&c.seg[k], &want[k].m, k);
		check_wire(buf + coff(&c, k), (uint32_t)(want[k].index - want[0].index),
			   want[k].pairs, &want[k].m);
		zassert_true(items_match(c.seg[k].iq, c.seg[k].index, c.seg[k].pairs));
		zassert_equal(esdr_seg_track_gap(&t, &c.seg[k]), want[k].gap, "segment %u", k);
		samples += want[k].pairs;
	}
	zassert_equal(get16(buf + BH_SAMPLES), samples);
	zassert_equal(samples, 5 + 1600 + 30 + 100 + 101 + 60 + 16 + 10);
}

ZTEST(esdr_seg, test_header_fields)
{
	static uint8_t buf[BLK];
	static struct collect c;
	/* shift, flags, gain, dc_i, dc_q, power: the ends of each field's range. */
	static const struct esdr_seg_meta m[] = {
		{0, ESDR_SEG_LOUD, 0, -128, 127, -1},
		{2, ESDR_SEG_DROPPED | ESDR_SEG_RESTART, ESDR_SEG_GAIN_AUTO, 0, -1, INT16_MIN},
		{ESDR_SEG_SHIFT_MAX, 0xff, 254, 127, -128, INT16_MAX},
		{1, 0, 1, -1, 0, 0},
	};
	struct esdr_seg_writer w;
	const uint64_t b = 0x7700000000ULL;
	int8_t *dst;

	memset(buf, 0xa5, sizeof(buf));
	esdr_seg_writer_begin(&w, buf);
	for (unsigned int k = 0; k < ARRAY_SIZE(m); k++) {
		zassert_equal(esdr_seg_writer_add(&w, b + 5U * k, 3, &m[k], &dst), 3);
		fill(dst, b + 5U * k, 3);
	}
	esdr_seg_writer_finish(&w, 0);
	/* The writer zeroes the reserved bytes over the 0xa5; the reader does not look at them. */
	for (unsigned int pass = 0; pass < 2U; pass++) {
		zassert_equal(parse(buf, BLK, pass, &c), ARRAY_SIZE(m), "pass %u", pass);
		for (unsigned int k = 0; k < ARRAY_SIZE(m); k++) {
			uint8_t *h = buf + HDR + k * (SHDR + 8U);

			zassert_equal(coff(&c, k), HDR + k * (SHDR + 8U));
			zassert_equal(c.seg[k].index, b + 5U * k);
			zassert_equal(c.seg[k].pairs, 3);
			check_meta(&c.seg[k], &m[k], k);
			zassert_true(items_match(c.seg[k].iq, c.seg[k].index, 3));
			if (pass == 0U) {
				check_wire(h, 5U * k, 3, &m[k]);
				memset(h + SH_RSVD, 0xff - (int)k, SHDR - SH_RSVD);
			}
		}
	}
}

ZTEST(esdr_seg, test_extend_fills_block)
{
	static uint8_t buf[BLK];
	static struct collect c;
	struct esdr_seg_writer w;
	struct esdr_seg_track t;
	const uint32_t win = ESDR_GAPLESS_WINDOW_PAIRS, tail = ESDR_SEG_PAIRS_MAX - 2U * win;
	const uint64_t b = 0xfffffc00ULL;
	int8_t *dst;

	esdr_seg_writer_begin(&w, buf);
	zassert_equal(esdr_seg_writer_put(&w, b, win, META(0, ESDR_SEG_LOUD, -50), &dst), win);
	fill(dst, b, win);
	zassert_equal(esdr_seg_writer_put(&w, b + win, win, META(0, ESDR_SEG_LOUD, -50), &dst),
		      win);
	fill(dst, b + win, win);
	/* The third window fits in part: the block ends full, without a zero header. */
	zassert_equal(esdr_seg_writer_put(&w, b + 2U * win, win, META(0, 0, -60), &dst), tail);
	fill(dst, b + 2U * win, tail);
	zassert_equal(w.pos, BLK);
	zassert_equal(esdr_seg_writer_room(&w), 0);
	zassert_equal(esdr_seg_writer_put(&w, b + 2U * win + tail, 1, META(0, 0, -60), &dst), 0);
	zassert_equal(esdr_seg_writer_add(&w, b + 2U * win + tail, 1, META(0, 0, -60), &dst), 0);
	esdr_seg_writer_finish(&w, 0);
	esdr_seg_track_reset(&t);
	zassert_equal(parse(buf, BLK, 1, &c), 1);
	zassert_equal(c.seg[0].pairs, ESDR_SEG_PAIRS_MAX);
	zassert_true(items_match(c.seg[0].iq, b, ESDR_SEG_PAIRS_MAX));
	zassert_equal(esdr_seg_track_gap(&t, &c.seg[0]), ESDR_SEG_GAP_START);

	/* The rest of the window opens the next block, without a gap. */
	esdr_seg_writer_begin(&w, buf);
	zassert_equal(esdr_seg_writer_put(&w, b + ESDR_SEG_PAIRS_MAX, win - tail, META(0, 0, -60),
					  &dst), win - tail);
	fill(dst, b + ESDR_SEG_PAIRS_MAX, win - tail);
	esdr_seg_writer_finish(&w, 0);
	zassert_equal(parse(buf, BLK, 2, &c), 1);
	zassert_equal(c.seg[0].index, b + ESDR_SEG_PAIRS_MAX);
	zassert_true(items_match(c.seg[0].iq, c.seg[0].index, win - tail));
	zassert_equal(esdr_seg_track_gap(&t, &c.seg[0]), 0);
}

ZTEST(esdr_seg, test_block_full)
{
	static uint8_t buf[BLK];
	static struct collect c;
	struct esdr_seg_writer w;
	int8_t *dst;

	/* A first segment that leaves 16 bytes: a zero header but no pair fits. */
	memset(buf, 0xa5, sizeof(buf));
	esdr_seg_writer_begin(&w, buf);
	zassert_equal(esdr_seg_writer_add(&w, 0, 2024, META(0, 0, 0), &dst), 2024);
	fill(dst, 0, 2024);
	zassert_equal(esdr_seg_writer_room(&w), 0);
	zassert_equal(esdr_seg_writer_add(&w, 5000, 1, META(0, 0, 0), &dst), 0);
	esdr_seg_writer_finish(&w, 0);
	for (uint32_t i = BLK - SHDR; i < BLK; i++) {
		zassert_equal(buf[i], 0, "zero header byte %u", i);
	}
	zassert_equal(parse(buf, BLK, 0, &c), 1);
	/* The reader looks at a header that ends with the block: one with pairs runs past it. */
	put16(buf + BLK - SHDR + SH_PAIRS, 1);
	zassert_equal(parse(buf, BLK, 0, &c), -EPROTO);
	zassert_equal(c.n, 0);

	/* 12 bytes left, less than a header: the payload's end ends the block. */
	memset(buf, 0xa5, sizeof(buf));
	esdr_seg_writer_begin(&w, buf);
	zassert_equal(esdr_seg_writer_add(&w, 0, 2026, META(0, 0, 0), &dst), 2026);
	fill(dst, 0, 2026);
	zassert_equal(esdr_seg_writer_add(&w, 5000, 1, META(0, 0, 0), &dst), 0);
	esdr_seg_writer_finish(&w, 0);
	for (uint32_t i = BLK - 12U; i < BLK; i++) {
		zassert_equal(buf[i], 0, "tail byte %u", i);
	}
	zassert_equal(parse(buf, BLK, 5, &c), 1);
	zassert_equal(c.seg[0].pairs, 2026);

	/* 20 bytes left: a header and two pairs. */
	memset(buf, 0xa5, sizeof(buf));
	esdr_seg_writer_begin(&w, buf);
	zassert_equal(esdr_seg_writer_add(&w, 0, 2022, META(0, 0, 0), &dst), 2022);
	fill(dst, 0, 2022);
	zassert_equal(esdr_seg_writer_room(&w), 2);
	zassert_equal(esdr_seg_writer_add(&w, 5000, 5, META(1, 0, 0), &dst), 2);
	fill(dst, 5000, 2);
	zassert_equal(w.pos, BLK);
	esdr_seg_writer_finish(&w, 0);
	zassert_equal(parse(buf, BLK, 6, &c), 2);
	zassert_equal(c.seg[1].index, 5000);
	zassert_equal(c.seg[1].pairs, 2);

	/* An odd segment's padding counts: no new segment, but the segment itself grows. */
	memset(buf, 0xa5, sizeof(buf));
	esdr_seg_writer_begin(&w, buf);
	zassert_equal(esdr_seg_writer_add(&w, 0, 2023, META(2, 0, 0), &dst), 2023);
	fill(dst, 0, 2023);
	zassert_equal(esdr_seg_writer_room(&w), 0);
	zassert_equal(esdr_seg_writer_add(&w, 9000, 1, META(2, 0, 0), &dst), 0);
	zassert_equal(esdr_seg_writer_extend(&w, 2023, 100, META(2, 0, 0), &dst),
		      ESDR_SEG_PAIRS_MAX - 2023U);
	fill(dst, 2023, ESDR_SEG_PAIRS_MAX - 2023U);
	esdr_seg_writer_finish(&w, 0);
	zassert_equal(parse(buf, BLK, 7, &c), 1);
	zassert_equal(c.seg[0].pairs, ESDR_SEG_PAIRS_MAX);
	zassert_true(items_match(c.seg[0].iq, 0, ESDR_SEG_PAIRS_MAX));
}

ZTEST(esdr_seg, test_padding_and_terminator)
{
	static uint8_t buf[BLK];
	static struct collect c;
	static const uint32_t pairs[] = {1, 3, 2, 5};
	static const uint32_t offs[] = {16, 36, 60, 80};

	build(buf, 77, pairs, ARRAY_SIZE(pairs), 10);
	zassert_equal(parse(buf, BLK, 1, &c), ARRAY_SIZE(pairs));
	for (unsigned int k = 0; k < ARRAY_SIZE(pairs); k++) {
		uint32_t off = coff(&c, k);

		zassert_equal(off, offs[k], "segment %u at %u", k, off);
		zassert_equal(off, seg_off(pairs, k));
	}
	/* The padding after odd segments, then the zero header and the tail. */
	zassert_true(buf[34] == 0U && buf[35] == 0U && buf[58] == 0U && buf[59] == 0U &&
		     buf[106] == 0U && buf[107] == 0U);
	for (uint32_t i = 108; i < BLK; i++) {
		zassert_equal(buf[i], 0, "byte %u", i);
	}
	/* What follows the zero header is not looked at. */
	put32(buf + 124 + SH_DELTA, 1000);
	put16(buf + 124 + SH_PAIRS, 5);
	memset(buf + 124 + SHDR, 0x5a, BLK - 124U - SHDR);
	zassert_equal(parse(buf, BLK, 2, &c), ARRAY_SIZE(pairs));
	/* Where the zero header was, the same header is a segment the samples do not count. */
	memcpy(buf + 108, buf + 124, SHDR);
	zassert_equal(parse(buf, BLK, 2, &c), -EPROTO);
}

ZTEST(esdr_seg, test_writer_index_limits)
{
	static uint8_t buf[BLK];
	static struct collect c;
	struct esdr_seg_writer w;
	const uint64_t b = 0xfffff000ULL;
	int8_t *dst;

	esdr_seg_writer_begin(&w, buf);
	/* No pairs: nothing starts, the block stays empty. */
	zassert_equal(esdr_seg_writer_add(&w, 12345, 0, META(0, 0, 0), &dst), 0);
	zassert_equal(esdr_seg_writer_add(&w, b, 10, META(1, 0, -1), &dst), 10);
	fill(dst, b, 10);
	/* The largest delta, then one past it, behind the end and inside the last segment. */
	zassert_equal(esdr_seg_writer_add(&w, b + UINT32_MAX, 1, META(1, 0, -2), &dst), 1);
	fill(dst, b + UINT32_MAX, 1);
	zassert_equal(esdr_seg_writer_add(&w, b + UINT32_MAX + 2U, 1, META(1, 0, -3), &dst), 0);
	zassert_equal(esdr_seg_writer_add(&w, b + 5U, 1, META(1, 0, -3), &dst), 0);
	zassert_equal(esdr_seg_writer_add(&w, b + UINT32_MAX, 1, META(1, 0, -3), &dst), 0);
	zassert_equal(esdr_seg_writer_put(&w, b + UINT32_MAX + 3U, 1, META(1, 0, -3), &dst), 0);
	/* Extension goes on past the delta range: only a segment's start is a delta. */
	zassert_equal(esdr_seg_writer_put(&w, b + UINT32_MAX + 1U, 3, META(1, ESDR_SEG_LOUD, -4),
					  &dst), 3);
	fill(dst, b + UINT32_MAX + 1U, 3);
	esdr_seg_writer_finish(&w, ESDR_BLK_GAP);
	zassert_equal(get16(buf + BH_FLAGS), ESDR_BLK_SEG | ESDR_BLK_GAP);
	zassert_equal(get64(buf + BH_INDEX), b);
	zassert_equal(parse(buf, BLK, 0, &c), 2);
	zassert_equal(c.seg[1].index, b + UINT32_MAX);
	zassert_equal(c.seg[1].pairs, 4);
	zassert_equal(c.seg[1].power_dbfs_x10, -2);
	zassert_true(items_match(c.seg[1].iq, c.seg[1].index, 4));

	/* A fresh block takes what the last refused, behind or far ahead alike. */
	esdr_seg_writer_begin(&w, buf);
	zassert_equal(esdr_seg_writer_add(&w, b + 5U, 1, META(0, 0, 0), &dst), 1);
	fill(dst, b + 5U, 1);
	esdr_seg_writer_finish(&w, 0);
	zassert_equal(parse(buf, BLK, 0, &c), 1);
	zassert_equal(c.seg[0].index, b + 5U);
}

ZTEST(esdr_seg, test_max_pairs)
{
	static uint8_t buf[BLK], two[2U * BLK];
	static struct collect c;
	struct esdr_seg_writer w;
	const uint64_t b = 0x0000123400000000ULL;
	int8_t *dst;

	zassert_equal(HDR + SHDR + 2U * ESDR_SEG_PAIRS_MAX, BLK);
	memset(buf, 0xa5, sizeof(buf));
	esdr_seg_writer_begin(&w, buf);
	zassert_equal(esdr_seg_writer_room(&w), ESDR_SEG_PAIRS_MAX);
	zassert_equal(esdr_seg_writer_add(&w, b, ESDR_SEG_PAIRS_MAX + 100U,
					  META(2, ESDR_SEG_LOUD, -1), &dst), ESDR_SEG_PAIRS_MAX);
	fill(dst, b, ESDR_SEG_PAIRS_MAX);
	zassert_equal(esdr_seg_writer_room(&w), 0);
	zassert_equal(esdr_seg_writer_extend(&w, b + ESDR_SEG_PAIRS_MAX, 1, META(2, 0, 0), &dst),
		      0);
	esdr_seg_writer_finish(&w, 0);
	zassert_equal(get16(buf + BH_SAMPLES), ESDR_SEG_PAIRS_MAX);
	zassert_equal(get16(buf + HDR + SH_PAIRS), ESDR_SEG_PAIRS_MAX);
	zassert_equal(parse(buf, BLK, 3, &c), 1);
	zassert_equal(c.seg[0].index, b);
	zassert_equal(c.seg[0].pairs, ESDR_SEG_PAIRS_MAX);
	zassert_true(items_match(c.seg[0].iq, b, ESDR_SEG_PAIRS_MAX));

	/*
	 * Full to the last byte, so no zero header ends the walk: given more than a block, the
	 * reader must stop at ESDR_BLOCK_BYTES and not read on into a pattern or the next block.
	 */
	memcpy(two, buf, BLK);
	memset(two + BLK, 0x5a, BLK);
	zassert_equal(parse(two, 2U * BLK, 1, &c), 1);
	zassert_equal(c.seg[0].pairs, ESDR_SEG_PAIRS_MAX);
	esdr_seg_writer_begin(&w, two + BLK);
	zassert_equal(esdr_seg_writer_add(&w, b + ESDR_SEG_PAIRS_MAX, 5, META(0, 0, -1), &dst), 5);
	fill(dst, b + ESDR_SEG_PAIRS_MAX, 5);
	esdr_seg_writer_finish(&w, 0);
	zassert_equal(parse(two, 2U * BLK, 2, &c), 1);
	zassert_equal(c.seg[0].pairs, ESDR_SEG_PAIRS_MAX);
	zassert_true(items_match(c.seg[0].iq, b, ESDR_SEG_PAIRS_MAX));
	zassert_equal(parse(two + BLK, BLK, 0, &c), 1);
	zassert_equal(c.seg[0].index, b + ESDR_SEG_PAIRS_MAX);

	/* One pair more would end past the block. */
	put16(buf + HDR + SH_PAIRS, ESDR_SEG_PAIRS_MAX + 1U);
	put16(buf + BH_SAMPLES, ESDR_SEG_PAIRS_MAX + 1U);
	zassert_equal(parse(buf, BLK, 3, NULL), -EPROTO);
}

ZTEST(esdr_seg, test_empty_block)
{
	static uint8_t buf[BLK];
	static struct collect c;
	struct esdr_seg_writer w;

	memset(buf, 0xa5, sizeof(buf));
	esdr_seg_writer_begin(&w, buf);
	zassert_equal(esdr_seg_writer_finish(&w, ESDR_BLK_OVERFLOW), BLK);
	zassert_equal(get16(buf + BH_SAMPLES), 0);
	zassert_equal(get64(buf + BH_INDEX), 0);
	for (uint32_t i = HDR; i < BLK; i++) {
		zassert_equal(buf[i], 0, "byte %u", i);
	}
	zassert_equal(parse(buf, BLK, 0, &c), 0);
	zassert_equal(c.n, 0);
	zassert_equal(parse(buf, HDR, 4, &c), 0);
	put16(buf + BH_SAMPLES, 1);
	zassert_equal(parse(buf, BLK, 0, &c), -EPROTO);
}

ZTEST(esdr_seg, test_track)
{
	struct esdr_seg_track t;
	struct esdr_seg s = {.index = 100, .pairs = 10};

	esdr_seg_track_reset(&t);
	zassert_equal(esdr_seg_track_gap(&t, &s), ESDR_SEG_GAP_START);
	s.index = 110;
	zassert_equal(esdr_seg_track_gap(&t, &s), 0);
	s.index = 125;
	zassert_equal(esdr_seg_track_gap(&t, &s), 5);
	/* Back into the last segment, and back before it. */
	s.index = 134;
	zassert_equal(esdr_seg_track_gap(&t, &s), ESDR_SEG_GAP_BACK);
	s.index = 0;
	zassert_equal(esdr_seg_track_gap(&t, &s), ESDR_SEG_GAP_BACK);
	s.index = 10;
	zassert_equal(esdr_seg_track_gap(&t, &s), 0);
	/* Gaps as large as the axis stay clear of the markers. */
	esdr_seg_track_reset(&t);
	s.index = 0;
	s.pairs = 1;
	zassert_equal(esdr_seg_track_gap(&t, &s), ESDR_SEG_GAP_START);
	s.index = UINT64_MAX - 1U;
	zassert_equal(esdr_seg_track_gap(&t, &s), ESDR_SEG_GAP_MAX);
	esdr_seg_track_reset(&t);
	s.index = 0;
	zassert_equal(esdr_seg_track_gap(&t, &s), ESDR_SEG_GAP_START);
	s.index = UINT64_MAX;
	s.pairs = 0;
	zassert_equal(esdr_seg_track_gap(&t, &s), ESDR_SEG_GAP_MAX);
}

/* ---- malformed input ---- */

ZTEST(esdr_seg, test_reject_header)
{
	static uint8_t buf[BLK];
	static const uint32_t pairs[] = {5, 6};
	static const uint16_t other[] = {ESDR_BLK_16BIT, ESDR_BLK_TEST, ESDR_BLK_BURST,
					 ESDR_BLK_4BIT, ESDR_BLK_TRIGGER, ESDR_BLK_CHAN};
	struct esdr_block_hdr h;

	build(buf, 1, pairs, ARRAY_SIZE(pairs), 0);
	zassert_equal(esdr_seg_parse(NULL, BLK, NULL, NULL), -EINVAL);
	for (size_t len = 0; len < HDR; len++) {
		zassert_equal(parse(buf, len, len & 7U, NULL), -EINVAL, "len %zu", len);
	}
	zassert_equal(esdr_seg_block_hdr(fence(buf, HDR, 1), HDR, &h), 0);
	zassert_equal(h.flags, ESDR_BLK_SEG);
	zassert_equal(h.samples, 11);

	buf[0] ^= 1U;
	zassert_equal(parse(buf, BLK, 0, NULL), -EBADMSG);
	buf[0] ^= 1U;
	put16(buf + BH_FLAGS, 0);
	zassert_equal(parse(buf, BLK, 0, NULL), -EBADMSG);
	put16(buf + BH_FLAGS, ESDR_BLK_GAP | ESDR_BLK_OVERFLOW);
	zassert_equal(parse(buf, BLK, 0, NULL), -EBADMSG);
	for (unsigned int k = 0; k < ARRAY_SIZE(other); k++) {
		put16(buf + BH_FLAGS, ESDR_BLK_SEG | other[k]);
		zassert_equal(parse(buf, BLK, 0, NULL), -EBADMSG, "flags %#x", other[k]);
	}
	put16(buf + BH_FLAGS, ESDR_BLK_SEG | ESDR_BLK_GAP | ESDR_BLK_OVERFLOW);
	zassert_equal(parse(buf, BLK, 0, NULL), 2);
}

ZTEST(esdr_seg, test_reject_truncated)
{
	static uint8_t buf[2U * BLK];
	static struct collect c;
	static const uint32_t pairs[] = {800, 3, 801, 2, 1};
	const uint32_t end = seg_off(pairs, 4) + SHDR + 2U;

	build(buf, 0x55aa55aa55ULL, pairs, ARRAY_SIZE(pairs), 1000);
	for (size_t len = 0; len <= BLK; len++) {
		int ret = parse(buf, len, len & 7U, &c);

		if (len < HDR) {
			zassert_equal(ret, -EINVAL, "len %zu", len);
		} else if (len < end) {
			zassert_equal(ret, -EPROTO, "len %zu: %d", len, ret);
			zassert_equal(c.n, 0, "len %zu: segments handed out", len);
		} else {
			zassert_equal(ret, ARRAY_SIZE(pairs), "len %zu: %d", len, ret);
		}
	}
	/* More than a block (this one ends at its zero header anyway, see test_max_pairs). */
	memset(buf + BLK, 0x5a, BLK);
	zassert_equal(parse(buf, 2U * BLK, 0, &c), ARRAY_SIZE(pairs));
	zassert_equal(c.seg[4].index, 0x55aa55aa55ULL + 1606U + 4U * 1000U);
}

ZTEST(esdr_seg, test_reject_payload)
{
	static uint8_t ref[BLK], buf[BLK];
	static struct collect c;
	static const uint32_t pairs[] = {40, 41, 42};
	const uint32_t s0 = seg_off(pairs, 0), s1 = seg_off(pairs, 1), s2 = seg_off(pairs, 2);

	build(ref, 5000, pairs, ARRAY_SIZE(pairs), 7);

	/* Pairs past the end of the block. */
	for (uint32_t p = 42U + 1U; p <= UINT16_MAX; p += 97U) {
		memcpy(buf, ref, BLK);
		put16(buf + s2 + SH_PAIRS, (uint16_t)p);
		put16(buf + BH_SAMPLES, (uint16_t)(81U + p));
		/* Into the zeros after the segment still adds up; past the block it does not. */
		if (p <= (BLK - s2 - SHDR) / 2U) {
			zassert_equal(parse(buf, BLK, 1, &c), 3, "pairs %u", p);
		} else {
			zassert_equal(parse(buf, BLK, 1, &c), -EPROTO, "pairs %u", p);
			zassert_equal(c.n, 0);
		}
	}
	memcpy(buf, ref, BLK);
	put16(buf + s0 + SH_PAIRS, UINT16_MAX);
	zassert_equal(parse(buf, BLK, 1, &c), -EPROTO);
	zassert_equal(c.n, 0);

	/* The samples do not add up. */
	memcpy(buf, ref, BLK);
	put16(buf + BH_SAMPLES, 40 + 41 + 42 + 1);
	zassert_equal(parse(buf, BLK, 2, &c), -EPROTO);
	put16(buf + BH_SAMPLES, 40 + 41 + 42 - 1);
	zassert_equal(parse(buf, BLK, 2, &c), -EPROTO);
	zassert_equal(c.n, 0);

	/* The first segment off the block's index; an overlap; touching is fine. */
	memcpy(buf, ref, BLK);
	put32(buf + s0 + SH_DELTA, 1);
	zassert_equal(parse(buf, BLK, 3, &c), -EPROTO);
	memcpy(buf, ref, BLK);
	put32(buf + s2 + SH_DELTA, get32(buf + s1 + SH_DELTA) + 40U);
	zassert_equal(parse(buf, BLK, 3, &c), -EPROTO);
	put32(buf + s2 + SH_DELTA, get32(buf + s1 + SH_DELTA) + 41U);
	zassert_equal(parse(buf, BLK, 3, &c), 3);
	zassert_equal(c.seg[2].index, c.seg[1].index + 41U);

	/* Deltas that run off the end of the index axis. */
	memcpy(buf, ref, BLK);
	put32(buf + s2 + SH_DELTA, UINT32_MAX);
	zassert_equal(parse(buf, BLK, 4, &c), 3);
	put64(buf + BH_INDEX, UINT64_MAX - UINT32_MAX - 42U);
	zassert_equal(parse(buf, BLK, 4, &c), 3);
	zassert_equal(c.seg[2].index + c.seg[2].pairs, UINT64_MAX);
	put64(buf + BH_INDEX, UINT64_MAX - UINT32_MAX - 41U);
	zassert_equal(parse(buf, BLK, 4, &c), -EPROTO);
	put64(buf + BH_INDEX, UINT64_MAX);
	zassert_equal(parse(buf, BLK, 4, &c), -EPROTO);
	zassert_equal(c.n, 0);

	/* Shifts up to ESDR_SEG_SHIFT_MAX (7) are taken, 8 is not. */
	memcpy(buf, ref, BLK);
	buf[s1 + SH_SHIFT] = ESDR_SEG_SHIFT_MAX;
	zassert_equal(parse(buf, BLK, 5, &c), 3);
	buf[s1 + SH_SHIFT] = ESDR_SEG_SHIFT_MAX + 1U;
	zassert_equal(parse(buf, BLK, 5, &c), -EPROTO);
	zassert_equal(c.n, 0);

	/* A callback stops the walk with its own value. */
	c.stop_at = 2;
	c.stop_ret = -ECANCELED;
	zassert_equal(parse(ref, BLK, 6, &c), -ECANCELED);
	zassert_equal(c.n, 2);
	c.stop_at = 0;
}

ZTEST(esdr_seg, test_reject_corrupt)
{
	static uint8_t buf[BLK];
	static struct collect c;
	struct esdr_seg_writer w;
	unsigned int ok = 0, bad = 0;

	rng = 0x9e3779b9U;
	for (unsigned int it = 0; it < 30000U; it++) {
		uint64_t index = (uint64_t)rnd() << rnd_below(33);
		unsigned int muts = 1U + rnd_below(4);
		size_t len = rnd_below(8) == 0U ? rnd_below(BLK + 1U) : BLK;
		uint32_t sum = 0;
		int ret;

		esdr_seg_writer_begin(&w, buf);
		for (unsigned int k = rnd_below(12); k > 0U; k--) {
			int8_t *dst;
			uint32_t n = esdr_seg_writer_add(&w, index, 1U + rnd_below(600),
							 META(1, 0, -5), &dst);

			fill(dst, index, n);
			index += n + rnd_below(3000);
		}
		esdr_seg_writer_finish(&w, 0);
		if (rnd_below(16) == 0U) {
			/* Random bytes behind a good header. */
			for (uint32_t i = HDR; i < BLK; i++) {
				buf[i] = (uint8_t)rnd();
			}
		}
		for (unsigned int k = 0; k < muts; k++) {
			uint32_t at = rnd_below(4U) != 0U ? rnd_below(160) : rnd_below(BLK);

			if (rnd_below(4) == 0U && at + 2U <= BLK) {
				put16(buf + at, (uint16_t)rnd());
			} else {
				buf[at] = (uint8_t)rnd();
			}
		}
		ret = parse(buf, len, rnd_below(8), &c);
		zassert_true(ret >= 0 || ret == -EINVAL || ret == -EBADMSG || ret == -EPROTO,
			     "returned %d", ret);
		if (ret < 0) {
			zassert_equal(c.n, 0, "segments of a rejected block");
			bad++;
			continue;
		}
		zassert_equal(c.n, (unsigned int)ret);
		for (unsigned int k = 0; k < c.n; k++) {
			sum += c.seg[k].pairs;
			zassert_true(k == 0U || c.seg[k].index >=
						       c.seg[k - 1U].index + c.seg[k - 1U].pairs);
		}
		zassert_equal(sum, get16(c.base + BH_SAMPLES));
		ok++;
	}
	printk("corrupt blocks: %u parsed, %u rejected\n", ok, bad);
	zassert_true(ok > 1000U && bad > 1000U);
}
