/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The float polyphase channelizer and burst detector: esdr_pfb.h's definition evaluated in
 * float, the reference the fixed-point bank of esdr_pfbq.h is tested against
 * (tests/unit/esdr_pfbq) and the one tested against numpy (tests/unit/esdr_pfb).
 */

#ifndef ESDR_PFB_REF_H_
#define ESDR_PFB_REF_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esdr_pfb.h"

#define ESDR_PFB_T_MAX 16U
/* Branch sums run over four frames of one parity at once: eight frames. */
#define ESDR_PFB_GROUP 8U

struct esdr_pfb_cfg {
	/* M: 16, 20, 32, 40, 64 or 80. */
	uint16_t channels;
	/* T, taps per branch: 4, 8, 12 or 16 (0: 8). */
	uint16_t taps;
	/* Output frames kept: a frame stays valid until this many newer ones exist (0: 1). */
	uint16_t frames;
	uint16_t reserved;
	/* Input pairs converted per pass, beyond the history: at least 4 M (0: 2 L). */
	uint32_t chunk;
	/* Stop band the Kaiser window is designed for, dB (0: 60). */
	float stop_db;
	/* DC gain (0: 1). */
	float gain;
};

/*
 * Frame m: M complex outputs, I then Q, channel 0 first. @p iq stays valid until cfg.frames
 * newer frames exist.
 */
typedef void (*esdr_pfb_frame_cb)(uint64_t m, const float *iq, uint32_t flags, void *user);

/* State; the fields are private. */
struct esdr_pfb {
	uint16_t m, d, t, l;
	uint16_t frames, slot;
	uint32_t cap, fill;
	/* Branch-major taps: h[r T + p] = gain h_proto[r + p M]. */
	float *h;
	/* History, complex: hist[2 * (fill - (next - i))] holds pair i. */
	float *hist;
	/* Branch sums of a group, in each frame's FFT input order. */
	float *u;
	float *ring;
	/* FFT input of branch r per frame parity: reversed, shifted by M / 2, digit reversed. */
	uint16_t *widx;
	const float *tw[4];
	uint8_t radix[4], stages;
	/* Kernels for this M and T: branch sums of four frames, of one frame; the FFT. */
	void (*b4)(const struct esdr_pfb *p, const float *xn, float *u, const uint16_t *widx);
	void (*b1)(const struct esdr_pfb *p, const float *xn, float *u, const uint16_t *widx);
	void (*fft)(const struct esdr_pfb *p, const float *src, float *dst);
	bool live, gap;
	/* Next pair expected; newest pair of the first complete frame; next frame and pair. */
	uint64_t next, full, nm, nn;
};

/* Bytes of working memory for @p cfg, 0 if it is not supported. */
size_t esdr_pfb_mem_size(const struct esdr_pfb_cfg *cfg);

/*
 * Designs the prototype and sets up the bank in @p mem (esdr_pfb_mem_size() bytes, 4-byte
 * aligned). 0, -EINVAL for an unsupported @p cfg, -ENOMEM if @p len is short.
 */
int esdr_pfb_init(struct esdr_pfb *p, const struct esdr_pfb_cfg *cfg, void *mem, size_t len);

/* Forgets the history: the next push starts a new run. */
void esdr_pfb_reset(struct esdr_pfb *p);

/*
 * One segment: @p pairs pairs from absolute index @p index, int8 I then Q. An I sample is
 * (item << @p shift) + h - @p dc_i, a Q sample (item << @p shift) + h - @p dc_q, with
 * h = ((1 << @p shift) - 1) / 2: the C6 packs raw >> shift, so an item stands for the middle of
 * its step (without h the floor would leave -h of DC); @p dc_i and @p dc_q are the receiver's DC
 * in raw units, as esdr_seg_hdr carries it. Calls @p cb for every frame it completes, in order,
 * before it returns. Returns the frames produced, -ERANGE if @p index lies before the end of
 * the last segment, -EINVAL for a shift above 15.
 */
int esdr_pfb_push(struct esdr_pfb *p, uint64_t index, const int8_t *iq, size_t pairs,
		  unsigned int shift, int32_t dc_i, int32_t dc_q, esdr_pfb_frame_cb cb,
		  void *user);

/* esdr_pfb_push() with float pairs, taken as they are. */
int esdr_pfb_push_f32(struct esdr_pfb *p, uint64_t index, const float *iq, size_t pairs,
		      esdr_pfb_frame_cb cb, void *user);

/* Prototype tap @p n (0 .. L - 1), gain included. */
float esdr_pfb_tap(const struct esdr_pfb *p, unsigned int n);

/* ---- burst detector ---- */

struct esdr_det_cfg {
	/* M, as the channelizer's. */
	uint16_t channels;
	/* B, frames per block (0: 16). */
	uint16_t block;
	/* Quiet blocks an open channel still sends before it closes (0: 4). */
	uint16_t hang;
	uint16_t reserved;
	/* Block power over the floor that opens a channel (0: 4, 6 dB). */
	float open_ratio;
};

/*
 * One channel's frames to send at the end of a block: slots [first, first + n) of the
 * detector's ring, oldest first (esdr_det_take()). power is the block's mean power per frame
 * (complete frames only), floor the channel's, both in output units squared.
 */
struct esdr_det_ev {
	uint16_t chan, flags;
	uint32_t first, n;
	float power, floor;
};

struct esdr_det;

/* Take the event's frames (esdr_det_take()) before returning: the ring moves on. */
typedef void (*esdr_det_cb)(const struct esdr_det *d, const struct esdr_det_ev *ev, void *user);

struct esdr_det_slot {
	const float *iq;
	uint64_t m;
	uint32_t flags;
};

/* State; the fields are private. */
struct esdr_det {
	uint16_t m, b, hang, nslot;
	float ratio;
	/* Ring of the previous and the current block's frames (2 B slots). */
	struct esdr_det_slot *slot;
	/* Per channel: floor, the learning blocks' powers, block power, state. */
	float *floor, *learn, *pw;
	uint8_t *learnt, *open;
	uint16_t *quiet;
	uint32_t *loud;
	/* Per channel: the frame after the last one sent. */
	uint64_t *sent;
	const float **use;
	/* Next slot; the current and the previous block's first slot and frames. */
	uint16_t wr, cur_first, cur_n, prev_first, prev_n;
	uint64_t cur_start, prev_start, last_m;
	bool have_cur, have_prev, have_last;
};

/* Frames the producer must keep (esdr_pfb_cfg.frames) for a detector of @p block frames. */
#define ESDR_DET_FRAMES(block) (2U * (block) + 1U)

size_t esdr_det_mem_size(const struct esdr_det_cfg *cfg);

/* 0, -EINVAL for an unsupported @p cfg, -ENOMEM if @p len is short. */
int esdr_det_init(struct esdr_det *d, const struct esdr_det_cfg *cfg, void *mem, size_t len);

/* Forgets the floors and the blocks: every channel learns again. */
void esdr_det_reset(struct esdr_det *d);

/*
 * One frame of the channelizer (esdr_pfb_frame_cb's arguments). @p iq must stay valid until
 * 2 B newer frames have been passed in (ESDR_DET_FRAMES()). Frame m in a new block ends the
 * current one: its decisions come as @p cb calls, one per channel with frames to send.
 */
void esdr_det_frame(struct esdr_det *d, uint64_t m, const float *iq, uint32_t flags,
		    esdr_det_cb cb, void *user);

/* Ends the current block now (the stream stopped). */
void esdr_det_flush(struct esdr_det *d, esdr_det_cb cb, void *user);

/*
 * Up to @p max samples of @p ev's channel from its frame @p *pos on, consecutive in m and of
 * one completeness, to @p iq (I then Q); their first frame index to @p m0, the frames'
 * ESDR_PFB_* flags (of the first) to @p flags. Advances @p *pos; returns the count, 0 at the
 * end.
 */
unsigned int esdr_det_take(const struct esdr_det *d, const struct esdr_det_ev *ev,
			   uint32_t *pos, float *iq, unsigned int max, uint64_t *m0,
			   uint32_t *flags);

#endif /* ESDR_PFB_REF_H_ */
