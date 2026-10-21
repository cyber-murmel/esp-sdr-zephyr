/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Fixed-point polyphase channelizer of the gapless stream and its burst detector: the bank of
 * esdr_pfb.h (same prototype, channels, oversampling and absolute frame grid) in int16, four
 * frames at a time. On the ESP32-P4 the kernels are PIE SIMD (esdr_pfbq_pie.S, when the build
 * found Espressif's assembler: ESDR_PFBQ_PIE), elsewhere portable C that computes the same
 * values bit for bit.
 *
 * Quads: quad m0 (m0 a multiple of 4) holds frames m0 .. m0 + 3 channel-major, iq[8 k + 2 q] and
 * iq[8 k + 2 q + 1] I and Q of channel k in frame m0 + q, cfg.gain times the input's raw units
 * (esdr_pfb.h), saturated to int16. Bit q of valid: frame m0 + q was produced (its pair
 * (m0 + q) D was received); of complete: its whole window was (else zeros stood in: a
 * transient). Frames of a quad that are not valid hold no meaningful values.
 *
 * Fixed point: the history holds twice the raw units (the C6's half steps are exact), the taps
 * are the prototype times 2^qt (qt the largest up to 14 that keeps them in int16), a branch sum
 * is rounded once from its exact 40-bit sum, the FFT's twiddles are Q15 (products floored), its
 * radix-5 butterflies exact sums rounded once, and every step saturates. Against the float bank
 * the error stays within a few units of the output; keep cfg.gain such that a full-scale
 * carrier (512 raw units) stays below 32767 (gain up to 63), and above 8 for the taps' precision.
 *
 * Runs: a push that leaves a gap shorter than L fills it with zeros (its frames are not valid,
 * those whose windows reach into it not complete); a longer gap ends the run (its last quad goes
 * out with the frames after its last pair not valid) and starts a new one at the next quad.
 * esdr_pfbq_flush() sends the quad the input has stopped in the same way but keeps the run: data
 * that continues it lands in the frames after.
 *
 * Use from one thread only: the PIE kernels' vector state is not saved across context switches,
 * so no other thread on the core may use PIE.
 */

#ifndef ESDR_PFBQ_H_
#define ESDR_PFBQ_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esdr_pfb.h"

#define ESDR_PFBQ_FRAMES 4U
/* Taps a branch: 4, 8 or 12. */
#define ESDR_PFBQ_T_MAX  12U
/* Largest item shift of a segment (an int8 item << shift + 1 stays in int16). */
#define ESDR_PFBQ_SHIFT_MAX 6U

struct esdr_pfbq_cfg {
	/* M: 16, 32, 40, 64 or 80. */
	uint16_t channels;
	/* T, taps per branch: 4, 8 or 12 (0: 8). */
	uint16_t taps;
	/* Quads kept: a quad stays valid until this many newer ones exist (0: 1). */
	uint16_t quads;
	uint16_t reserved;
	/* Input pairs converted per pass, beyond the history (0: 2 L). */
	uint32_t chunk;
	/* Stop band of the Kaiser window, dB (0: 60). */
	float stop_db;
	/* Output units per raw unit (0: 1). */
	float gain;
};

/* Quad m0: M channels of 4 frames (layout above); @p iq stays valid until cfg.quads newer quads. */
typedef void (*esdr_pfbq_quad_cb)(uint64_t m0, const int16_t *iq, uint8_t valid, uint8_t complete,
				  void *user);

struct esdr_pfbq_kern;

/* State; the fields are private. */
struct esdr_pfbq {
	uint16_t m, d, t, l;
	uint16_t quads, slot;
	uint8_t radix[4], stages, sh;
	uint32_t cap, fill;
	/* Taps per group of 4 branches and tap: 8 int16, branch 4 g + 3 - j twice in lanes 2 j. */
	int16_t *taps;
	/* History, int16 pairs: hist[2 * (fill - (next - i))] holds pair i, twice its raw units. */
	int16_t *hist;
	/* FFT input (M vectors of 4 frames), the output ring, the twiddles. */
	int16_t *work, *ring, *tw;
	/* Byte offset in work of the branch in each group's lane j; the sums' rounding addend. */
	uint16_t *off;
	int16_t *rnd;
	const int16_t *tws[4];
	const struct esdr_pfbq_kern *k;
	bool live;
	/* Next pair expected; first frame of the next quad. */
	uint64_t next, nq;
	/* Frames nq + i (bit i) that will not be valid, not complete. */
	uint64_t inval, incompl;
};

/* Bytes of working memory for @p cfg, 0 if it is not supported. */
size_t esdr_pfbq_mem_size(const struct esdr_pfbq_cfg *cfg);

/*
 * Designs the prototype (esdr_pfb_proto()), quantizes it and sets the bank up in @p mem
 * (esdr_pfbq_mem_size() bytes, 4-byte aligned). 0, -EINVAL for an unsupported @p cfg (or a gain
 * the taps do not fit in int16 for), -ENOMEM if @p len is short.
 */
int esdr_pfbq_init(struct esdr_pfbq *p, const struct esdr_pfbq_cfg *cfg, void *mem, size_t len);

/* Forgets the history without sending anything: the next push starts a new run. */
void esdr_pfbq_reset(struct esdr_pfbq *p);

/*
 * One segment, as esdr_pfb_push(): @p pairs pairs from absolute index @p index, int8 I then Q, a
 * sample (item << @p shift) + ((1 << @p shift) - 1) / 2 - dc. Calls @p cb for every quad it
 * completes, in order, before it returns. Returns the quads produced, -ERANGE if @p index lies
 * before the end of the last segment, -EINVAL for a shift above ESDR_PFBQ_SHIFT_MAX.
 */
int esdr_pfbq_push(struct esdr_pfbq *p, uint64_t index, const int8_t *iq, size_t pairs,
		   unsigned int shift, int32_t dc_i, int32_t dc_q, esdr_pfbq_quad_cb cb, void *user);

/*
 * Sends the quad the input stopped in, its frames after the last pair not valid; the run goes
 * on. Returns the quads produced (0 or 1).
 */
int esdr_pfbq_flush(struct esdr_pfbq *p, esdr_pfbq_quad_cb cb, void *user);

/* Taps the bank runs with: tap @p n (0 .. L - 1) as int16 and the scale qt (tap = h 2^qt). */
int16_t esdr_pfbq_tap(const struct esdr_pfbq *p, unsigned int n);
unsigned int esdr_pfbq_qt(const struct esdr_pfbq *p);

/* True when the build has the PIE kernels (banks use them unless told otherwise). */
bool esdr_pfbq_pie(void);

/* Runs @p p on the PIE kernels (@p pie) or the C ones: 0, -ENOTSUP without PIE. For tests. */
int esdr_pfbq_kernels(struct esdr_pfbq *p, bool pie);

/* ---- burst detector on quads ---- */

/*
 * The detector of esdr_pfb.h on quads: blocks of B frames (B a multiple of 4) on the absolute
 * frame grid, complete frames only, the same floor, opening, hang and relearning. Events name
 * quad slots; esdr_detq_take() hands out a channel's int16 samples.
 */

struct esdr_detq_cfg {
	/* M, as the channelizer's. */
	uint16_t channels;
	/* B, frames per block: a multiple of 4 (0: 16). */
	uint16_t block;
	/* Quiet blocks an open channel still sends before it closes (0: 4). */
	uint16_t hang;
	uint16_t reserved;
	/* Block power over the floor that opens a channel (0: 4, 6 dB). */
	float open_ratio;
};

/*
 * One channel's quads to send at the end of a block: slots [first, first + n) of the detector's
 * ring, oldest first. power is the block's mean power per complete frame, floor the channel's,
 * both in output units squared.
 */
struct esdr_detq_ev {
	uint16_t chan, flags;
	uint32_t first, n;
	float power, floor;
};

struct esdr_detq;

/* Take the event's samples (esdr_detq_take()) before returning: the ring moves on. */
typedef void (*esdr_detq_cb)(const struct esdr_detq *d, const struct esdr_detq_ev *ev,
			     void *user);

struct esdr_detq_slot {
	const int16_t *iq;
	uint64_t m0;
	uint8_t valid, complete;
};

/* State; the fields are private. */
struct esdr_detq {
	uint16_t m, b, hang, nslot;
	float ratio;
	/* Ring of the previous and the current block's quads (2 B / 4 slots). */
	struct esdr_detq_slot *slot;
	/* Per channel: floor, the learning blocks' powers, block power sums, state. */
	float *floor, *learn, *pw;
	uint32_t *sum;
	/* A quad of zeros (fills a short group of the power kernel); masked copies of a block's
	 * quads with frames that are not complete.
	 */
	int16_t *zero, *scratch;
	uint8_t *learnt, *open;
	uint16_t *quiet;
	uint32_t *loud;
	/* Per channel: the frame after the last one sent. */
	uint64_t *sent;
	const int16_t **use;
	uint16_t wr, cur_first, cur_n, prev_first, prev_n;
	uint64_t cur_start, prev_start, last_m;
	bool have_cur, have_prev, have_last;
	/* Blocks decided (with every channel's power); the last one's first frame. */
	uint32_t decided;
	uint64_t decided_start;
};

/* Quads the producer must keep (esdr_pfbq_cfg.quads) for a detector of @p block frames. */
#define ESDR_DETQ_QUADS(block) (2U * (block) / 4U + 1U)

size_t esdr_detq_mem_size(const struct esdr_detq_cfg *cfg);

/* 0, -EINVAL for an unsupported @p cfg, -ENOMEM if @p len is short. */
int esdr_detq_init(struct esdr_detq *d, const struct esdr_detq_cfg *cfg, void *mem, size_t len);

/* Forgets the floors and the blocks: every channel learns again. */
void esdr_detq_reset(struct esdr_detq *d);

/*
 * One quad of the channelizer (esdr_pfbq_quad_cb's arguments). @p iq must stay valid until
 * 2 B / 4 newer quads have been passed in (ESDR_DETQ_QUADS()). A quad in a new block ends the
 * current one: its decisions come as @p cb calls, one per channel with quads to send.
 */
void esdr_detq_quad(struct esdr_detq *d, uint64_t m0, const int16_t *iq, uint8_t valid,
		    uint8_t complete, esdr_detq_cb cb, void *user);

/* Ends the current block now (the stream stopped). */
void esdr_detq_flush(struct esdr_detq *d, esdr_detq_cb cb, void *user);

/*
 * For a consumer that watches every channel, not only the ones that open: blocks decided so
 * far (a block of mostly transients decides nothing), and of the last one its first frame, each
 * channel's mean power per complete frame and floor (output units squared) and whether the
 * channel is open (1) after it, M entries each. Valid until the next esdr_detq_quad() or
 * esdr_detq_flush().
 */
uint32_t esdr_detq_decided(const struct esdr_detq *d, uint64_t *start);
const float *esdr_detq_power(const struct esdr_detq *d);
const float *esdr_detq_floor(const struct esdr_detq *d);
const uint8_t *esdr_detq_open(const struct esdr_detq *d);

/*
 * Up to @p max samples of @p ev's channel from its frame @p *pos on (frames counted over the
 * event's quads, 4 each), consecutive in m, valid and of one completeness, to @p iq (I then Q,
 * 4-byte aligned);
 * their first frame index to @p m0, ESDR_PFB_COMPLETE in @p flags if they are complete. Advances
 * @p *pos past them and past frames that are not valid; returns the count, 0 at the end.
 */
unsigned int esdr_detq_take(const struct esdr_detq *d, const struct esdr_detq_ev *ev,
			    uint32_t *pos, int16_t *iq, unsigned int max, uint64_t *m0,
			    uint32_t *flags);

#endif /* ESDR_PFBQ_H_ */
