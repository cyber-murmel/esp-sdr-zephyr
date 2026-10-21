/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Polyphase channelizer of the gapless 80 MS/s stream (ESDR_BLK_SEG segments, esdr_proto.h)
 * and a per-channel burst detector: their definition, the flags and the prototype design.
 * esdr_pfbq.h computes them in fixed point on the P4; the float bank of the tests
 * (tests/common/include/esdr_pfb_ref.h) evaluates them directly, as their reference.
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
 * frames stay in the channelizer's ring, the detector keeps pointers to them.
 */

#ifndef ESDR_PFB_H_
#define ESDR_PFB_H_

#define ESDR_PFB_M_MAX     80U
#define ESDR_PFB_T_DEFAULT 8U

/* Frame flags. */
/* The frame's window [m D - L + 1, m D] holds received pairs only (else zeros: a transient). */
#define ESDR_PFB_COMPLETE 0x01U
/* The first frame, or frames before it were left out: m does not follow the last frame's. */
#define ESDR_PFB_GAP      0x02U

/*
 * The prototype of a bank of @p m channels and @p t taps a branch (the one the banks design):
 * L = m t taps to @p h in natural order, DC gain @p gain, for a stop band of @p stop_db.
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

#endif /* ESDR_PFB_H_ */
