/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file
 * @brief Software IEEE 802.15.4 2.4 GHz O-QPSK PHY, portable C.
 *
 * All sample buffers are interleaved int16 I/Q at 4 MS/s (2 samples per
 * chip). The receiver's state is a struct ieee154_phy_rx: the instance
 * functions (suffix _i where a function of the same name exists for the
 * one static instance behind the original API) take it first, and any
 * number of receivers run side by side. The chip tables are global:
 * ieee154_phy_init() once, before any receiver.
 */

#ifndef ESP_SDR_IEEE154_PHY_H_
#define ESP_SDR_IEEE154_PHY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup ieee154_phy
 * @{
 */

/** Longest PSDU, FCS included. */
#define IEEE154_PHY_PSDU_MAX 127

/** Complex samples ieee154_phy_modulate() needs for a PSDU of @p len bytes. */
#define IEEE154_PHY_MOD_SAMPLES(len) ((6 + (size_t)(len)) * 128 + 4)

/**
 * Samples a frame of a PSDU of @p len bytes takes on the air at 2 samples
 * a chip: 4 preamble bytes, the SFD, the PHR and the PSDU, 2 symbols a
 * byte, 64 samples a symbol.
 */
#define IEEE154_PHY_FRAME_SAMPLES(len) ((6U + (uint32_t)(len)) * 128U)

/**
 * Called for each CRC-valid frame: @p psdu includes the 2-byte FCS, @p lqi
 * is 0 to 255 from the mean chip-error count, @p user is the pointer given
 * to ieee154_phy_set_rx_cb().
 */
typedef void (*ieee154_phy_frame_cb_t)(const uint8_t *psdu, uint8_t len, int lqi, void *user);

/**
 * Called for every complete frame, whether its FCS matched or not: @p psdu
 * includes the 2-byte FCS, @p lqi is 0 to 255 from the mean chip-error
 * count, @p fcs_ok says whether the FCS matched, @p user is the pointer
 * given to ieee154_phy_set_all_cb_i(). A frame with a good FCS also goes
 * to the ieee154_phy_frame_cb_t callback and stops the other phase tracker
 * delivering it again. Both trackers may complete a frame whose FCS fails, a
 * sample or two apart: the first is delivered, a second bad one of the same
 * end position (within IEEE154_PHY_DUP_SAMPLES) is not, but a good one is.
 * So a frame may come as a bad copy and then as a good one: a consumer that
 * wants every frame once holds a bad frame back until the position has
 * moved more than IEEE154_PHY_DUP_SAMPLES past its end, or a good copy has
 * replaced it.
 */
typedef void (*ieee154_phy_all_cb_t)(const uint8_t *psdu, uint8_t len, int lqi, bool fcs_ok,
				     void *user);

/**
 * End positions this close, in samples, belong to the same frame completed
 * by both phase trackers (ieee154_phy_all_cb_t).
 */
#define IEEE154_PHY_DUP_SAMPLES 4

/**
 * Called while a frame comes in, from the PHR on: @p len is the PSDU length
 * (FCS included), @p have the PSDU bytes received so far (0 at the PHR, at
 * most len - 1), @p end_pos the receiver's position (ieee154_phy_rx_position_i(),
 * ieee154_phy_rx_position() for the static receiver) at which the frame's
 * last symbol will be decided. Called again with each further byte while it
 * returns true. Both phase trackers may report the same frame.
 */
typedef bool (*ieee154_phy_hdr_cb_t)(const uint8_t *psdu, uint8_t have, uint8_t len,
				     uint32_t end_pos, void *user);

/**
 * One phase tracker of a receiver. Private to ieee154_phy.c; in the header
 * so that a receiver can be allocated by its user.
 */
struct ieee154_phy_trk {
	/* Chip shift register, chips seen, the chip count of the last symbol boundary. */
	uint32_t win, nchip, anchor;
	/* Preamble hits in a row, state, nibble assembly, PSDU length and bytes so far. */
	uint8_t run, st, nib_have, nib_lo, len, pos;
	/* The header callback still wants bytes of this frame. */
	uint8_t hdr_want;
	/* Chip errors summed over the PHR and PSDU symbols, and their count. */
	uint16_t err_sum, nsym;
	/* Sample count at which the frame's last symbol will be decided. */
	uint32_t end_pos;
	uint8_t buf[IEEE154_PHY_PSDU_MAX];
};

/**
 * A receiver: two phase trackers (one per sampling phase), the DC and
 * discriminator history, the sample count and the callbacks. Set up with
 * ieee154_phy_rx_init(); the fields are private to ieee154_phy.c.
 */
struct ieee154_phy_rx {
	struct ieee154_phy_trk trk[2];
	ieee154_phy_frame_cb_t cb;
	void *user;
	ieee154_phy_hdr_cb_t hdr_cb;
	void *hdr_user;
	ieee154_phy_all_cb_t all_cb;
	void *all_user;
	/* Samples fed before the block, or up to the sample being decided in it. */
	uint32_t pos;
	/* Samples fed over all blocks (wraps). */
	uint32_t n_total;
	/* DC removal state (Q8) and the discriminator's two previous samples. */
	int32_t dc_i, dc_q, pi1, pq1, pi2, pq2;
	/* The frame being delivered: the position of its first preamble sample. */
	uint32_t frame_start;
	/* The last frame delivered with a bad FCS: its end position, and whether there was one. */
	uint32_t bad_end;
	bool bad_seen;
};

/** Build the chip tables. Call once before anything else. */
void ieee154_phy_init(void);

/**
 * Set up a receiver: idle trackers, no history, position 0, no callbacks.
 *
 * @param rx The receiver.
 */
void ieee154_phy_rx_init(struct ieee154_phy_rx *rx);

/**
 * Set a receiver's frame callback (NULL: good frames are dropped).
 *
 * @param rx The receiver.
 * @param cb Called for each CRC-valid frame.
 * @param user Passed to @p cb.
 */
void ieee154_phy_set_rx_cb_i(struct ieee154_phy_rx *rx, ieee154_phy_frame_cb_t cb, void *user);

/**
 * Set a receiver's header callback (NULL: none).
 *
 * @param rx The receiver.
 * @param cb Called from the PHR on while a frame comes in.
 * @param user Passed to @p cb.
 */
void ieee154_phy_set_hdr_cb_i(struct ieee154_phy_rx *rx, ieee154_phy_hdr_cb_t cb, void *user);

/**
 * Set a receiver's callback for every complete frame, bad FCS included
 * (NULL: none, frames with a bad FCS are dropped).
 *
 * @param rx The receiver.
 * @param cb Called for each complete frame with its FCS verdict.
 * @param user Passed to @p cb.
 */
void ieee154_phy_set_all_cb_i(struct ieee154_phy_rx *rx, ieee154_phy_all_cb_t cb, void *user);

/**
 * Feed a receiver the next @p n complex samples; blocks must be consecutive.
 *
 * @param rx The receiver.
 * @param iq Interleaved int16 I/Q at 4 MS/s.
 * @param n Complex samples in @p iq.
 */
void ieee154_phy_rx_block_i(struct ieee154_phy_rx *rx, const int16_t *iq, size_t n);

/**
 * Drop any frame in progress, before a block that does not follow the last
 * one. The sample count goes on.
 *
 * @param rx The receiver.
 */
void ieee154_phy_rx_reset_i(struct ieee154_phy_rx *rx);

/**
 * A receiver's sample count, over all blocks (wraps). In the callbacks: the
 * samples fed up to and including the one that completed the symbol just
 * decided (for a frame, its last symbol). Between blocks it keeps what the
 * last block left: the samples fed before that block, or the position of its
 * last symbol decision (not the samples fed so far).
 *
 * @param rx The receiver.
 * @return The sample count.
 */
uint32_t ieee154_phy_rx_position_i(const struct ieee154_phy_rx *rx);

/**
 * In the frame callbacks: the position of the frame's first preamble
 * sample, its end position (ieee154_phy_rx_position_i()) less
 * IEEE154_PHY_FRAME_SAMPLES() of its length, on the same wrapping count
 * (so it may stand for a sample before the first one fed: subtract the
 * position the stream of interest started at and read the difference as
 * signed).
 *
 * @param rx The receiver.
 * @return The frame's first sample's position.
 */
uint32_t ieee154_phy_rx_frame_start_i(const struct ieee154_phy_rx *rx);

/** Set the static receiver's frame callback (NULL: frames are dropped). */
void ieee154_phy_set_rx_cb(ieee154_phy_frame_cb_t cb, void *user);

/** Set the static receiver's header callback (NULL: none). */
void ieee154_phy_set_hdr_cb(ieee154_phy_hdr_cb_t cb, void *user);

/** Feed the static receiver the next @p n complex samples; blocks must be consecutive. */
void ieee154_phy_rx_block(const int16_t *iq, size_t n);

/** Drop any frame in progress, before a block that does not follow the last one. */
void ieee154_phy_rx_reset(void);

/**
 * ieee154_phy_rx_position_i() of the static receiver: in the frame callback,
 * the samples fed up to and including the frame's last sample, which
 * completed its last symbol.
 */
uint32_t ieee154_phy_rx_position(void);

/** ieee154_phy_rx_frame_start_i() of the static receiver. */
uint32_t ieee154_phy_rx_frame_start(void);

/** The 32 chips of symbol @p sym (0..15), chip c0 in the MSB. */
uint32_t ieee154_phy_chips(int sym);

/**
 * Modulate SHR + PHR + @p psdu (FCS already appended) into @p out.
 * Returns the complex samples written, 0 if @p max_samples is too small.
 */
size_t ieee154_phy_modulate(const uint8_t *psdu, uint8_t len, int16_t amp, int16_t *out,
			    size_t max_samples);

/** Append the little-endian CRC-16 FCS to buf[0..n-1]; returns n + 2. */
size_t ieee154_phy_append_fcs(uint8_t *buf, size_t n);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ESP_SDR_IEEE154_PHY_H_ */
