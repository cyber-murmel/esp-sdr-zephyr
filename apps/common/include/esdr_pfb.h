/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Polyphase channelizer of the gapless 80 MS/s stream (ESDR_BLK_SEG segments, esdr_proto.h)
 * and a per-channel burst detector. Plain C99 and libm, no allocation: the P4 apps, the
 * MicroPython modules and host tools build the same code.
 *
 * Channelizer: M channels (16, 32, 40, 64 or 80) spaced fs / M apart, oversampled twice:
 * decimation D = M / 2, output rate 2 fs / M. Channel k is centred at k fs / M (k > M / 2:
 * k - M, below the centre). The prototype low pass h[n], n = 0 .. L - 1, L = M T, gives
 *
 *   y_k[m] = sum_n h[n] x[m D - n] exp(-j tau k (m D - n) / M)
 *
 * with x[i] the pair of absolute index i less the receiver's DC, 0 where none was received: the
 * absolute stream mixed down by k fs / M, filtered and decimated on the absolute grid m D, so
 * that every channel keeps its phase against the absolute index across gaps. Frame m is
 * produced only when pair m D was received; it is ESDR_PFB_COMPLETE when its whole window
 * [m D - L + 1, m D] was. A gap shorter than L is filled with zeros, a longer one discards the
 * history (both exact, since x is 0 there).
 *
 * Prototype: Kaiser-windowed sinc, -6 dB at the channel edge fs / (2 M) (neighbours sum flat),
 * beta from Kaiser's formula for cfg.stop_db; with T = 8 and the default 60 dB the response from
 * fs / M (the output Nyquist) on measures 69 dB down. DC gain cfg.gain: a complex tone of
 * amplitude A at channel k's centre gives |y_k| = gain A, with the tone's phase at index 0.
 * Input units are raw: an int8 item with shift s counts (item << s) + (2^s - 1) / 2 - dc, the
 * middle of the step the C6's shift (a floor) maps to it, less dc, the segment's DC
 * (esdr_seg_hdr); full scale 512.
 *
 * Detector: the power of every channel over blocks of B frames on the absolute frame grid
 * (m / B), complete frames only (a block of fewer than B / 2 decides nothing), against a floor
 * per channel learnt as the lower quartile of its first blocks and then following its quiet
 * ones; a quiet block 10 dB below the floor replaces it (a pruned stream may bring a channel's
 * bursts first and its noise only later). A channel opens on a block above floor * open_ratio and closes after hang quiet blocks;
 * while open, every block's frames go to the consumer, the first time with the block before as
 * pre-roll. A channel loud for 2048 blocks in a row closes and learns its floor again. The
 * frames stay in the channelizer's ring (cfg.frames), the detector keeps pointers to them.
 */

#ifndef ESDR_PFB_H_
#define ESDR_PFB_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ESDR_PFB_M_MAX     80U
#define ESDR_PFB_T_MAX     16U
#define ESDR_PFB_T_DEFAULT 8U
/* Branch sums run over four frames of one parity at once: eight frames. */
#define ESDR_PFB_GROUP     8U

/* Frame flags. */
/* The frame's window [m D - L + 1, m D] holds received pairs only (else zeros: a transient). */
#define ESDR_PFB_COMPLETE 0x01U
/* The first frame, or frames before it were left out: m does not follow the last frame's. */
#define ESDR_PFB_GAP      0x02U

struct esdr_pfb_cfg {
	/* M: 16, 32, 40, 64 or 80. */
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

/*
 * The prototype of a bank of @p m channels and @p t taps a branch (the one esdr_pfb_init()
 * designs): L = m t taps to @p h in natural order, DC gain @p gain, for a stop band of
 * @p stop_db.
 */
void esdr_pfb_proto(float *h, unsigned int m, unsigned int t, float stop_db, float gain);

/* ---- burst detector ---- */

/* Event flags. */
/* The channel opened at this block: the frames start with the block before (pre-roll). */
#define ESDR_DET_OPEN  0x01U
/* The channel closes after this block's frames. */
#define ESDR_DET_CLOSE 0x02U

/* Blocks the first floor of a channel is learnt over (lower quartile). */
#define ESDR_DET_LEARN 16U

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

#endif /* ESDR_PFB_H_ */
