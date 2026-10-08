/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file
 * @brief Diagnostics of the esp_sdr IEEE 802.15.4 radio driver (device "esp_sdr_154").
 */

#ifndef ESP_SDR_IEEE154_ESP_SDR_H_
#define ESP_SDR_IEEE154_ESP_SDR_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup ieee154_esp_sdr
 * @{
 */

/** Driver counters, ieee154_esp_sdr_get_stats(). */
struct ieee154_esp_sdr_stats {
	/** Receive captures processed, and failed captures. */
	uint32_t captures, capture_errors;
	/** Frames with a good FCS, and those the stack could not take. */
	uint32_t frames, frames_dropped;
	/** Frames transmitted, and transmissions that failed. */
	uint32_t tx_frames, tx_errors;
	/** ACKs received for frames that asked for one, and frames left without. */
	uint32_t acks, no_acks;
	/** Error of the last failed transmission. */
	int32_t tx_last_err;
	/** Last capture's mix/decimate time; PHY time in us (ring: total). */
	uint32_t filter_us, phy_us;
	/**
	 * Ring receive: last run's status; units and pairs lost, summed over all runs; samples
	 * the PHY skipped and read.
	 */
	uint32_t ring_status, ring_units, ring_lost, phy_skipped, phy_read;
	/** Last ring run: filter cycles per pair x 100, longest slice in cycles, units abandoned. */
	uint32_t ring_cycles_x100, ring_slice_max, ring_abandoned;
	/** Last ring run: interrupt windows opened, and the CPU interrupt lines pending in them. */
	uint32_t ring_irq_windows, ring_irq_lines;
};

struct net_pkt;

/**
 * Frame sink without the 802.15.4 L2 (raw use of the radio API): receives
 * each frame without its FCS, LQI set, and owns the packet. It runs on the
 * PHY thread with the PHY lock held: it must not call back into the driver
 * (transmit, channel, diagnostics).
 */
typedef void (*ieee154_esp_sdr_rx_cb_t)(struct net_pkt *pkt);

/** Set the frame sink used when CONFIG_NET_L2_IEEE802154 is off (ignored with it on). */
void ieee154_esp_sdr_set_rx_cb(ieee154_esp_sdr_rx_cb_t cb);

/**
 * Conjugate received samples and/or the transmitted waveform (spectral
 * inversion), to check another radio's I/Q convention. Both off at boot.
 */
void ieee154_esp_sdr_set_invert(bool rx_inv, bool tx_inv);

/**
 * Diagnostic: put the LO 4 MHz above the channel instead of below. Only the
 * subsampling ring receive handles it (transmit and the ACK capture assume
 * the LO below).
 *
 * @retval 0 on success, also before the radio is started (applied at start).
 * @retval <0 the retune failed (esp_sdr_set_freq()); the LO side is unchanged.
 */
int ieee154_esp_sdr_set_lo_above(bool above);

/** Result of ieee154_esp_sdr_measure(). */
struct ieee154_esp_sdr_measurement {
	/** Carrier offset in Hz (meaningless without a signal), wraps at +-500 kHz. */
	int32_t cfo_hz;
	/** Coarse carrier offset in Hz, unambiguous within +-1.5 MHz. */
	int32_t coarse_cfo_hz;
	/** Mean power relative to the receive full scale. */
	int32_t power_dbfs;
	/** Samples above half the mean power that went into the estimate. */
	uint32_t samples_used;
	/**
	 * Histogram of the one-chip phase steps in 45 degree bins from -180
	 * degrees: O-QPSK fills the bins at -90 and +90.
	 */
	uint32_t steps[8];
};

/**
 * Diagnostic: one 1 ms capture at the current channel, its carrier offset
 * against an O-QPSK signal filling it (within +-500 kHz) and its power.
 *
 * @retval 0 on success.
 * @retval -ENETDOWN if the radio was never started.
 * @retval <0 an error of the capture (esp_sdr_rx_capture()).
 */
int ieee154_esp_sdr_measure(struct ieee154_esp_sdr_measurement *m);

/**
 * esp_sdr_rx_set_gain() for the radio: stops the receive ring around the
 * call, which otherwise waits for the radio lock a ring run holds. Calls
 * into esp_sdr that take the radio go through the driver while it runs.
 */
int ieee154_esp_sdr_set_rx_gain(int index);

/** Receives the samples of ieee154_esp_sdr_dump(): @p n interleaved I/Q pairs. */
typedef void (*ieee154_esp_sdr_dump_cb_t)(const int16_t *iq, size_t n, void *user);

/**
 * Diagnostic: one 1 ms capture at the current channel as the PHY would see
 * it (mixed down and filtered to 4 MS/s, interleaved I/Q, +-8192 full
 * scale), handed to @p out while the driver still holds the radio.
 *
 * @retval 0 on success.
 * @retval -ENETDOWN if the radio was never started.
 * @retval <0 an error of the capture (esp_sdr_rx_capture()).
 */
int ieee154_esp_sdr_dump(ieee154_esp_sdr_dump_cb_t out, void *user);

/** Driver state changes kept across resets (ieee154_esp_sdr_get_trail()). */
enum ieee154_esp_sdr_event {
	IEEE154_ESP_SDR_EV_TAKE = 1, /* radio taken, argument: waiters before */
	IEEE154_ESP_SDR_EV_GIVE,     /* radio released */
	IEEE154_ESP_SDR_EV_RING,     /* ring run starts */
	IEEE154_ESP_SDR_EV_RING_END, /* ring run ended, argument: status */
	IEEE154_ESP_SDR_EV_TX,       /* transmission starts, argument: PSDU length */
	IEEE154_ESP_SDR_EV_TX_END,   /* transmission ended, argument: -error */
	IEEE154_ESP_SDR_EV_CAPTURE,  /* bank capture (ACK window, measure, dump, burst receive) */
	IEEE154_ESP_SDR_EV_BOOT,     /* driver init */
};

/**
 * Copy the last driver events, oldest first: event in bits 31:28, CPU in
 * bit 27, argument in bits 26:20, uptime in ms (modulo 2^20) in bits 19:0.
 * Kept in RTC memory across resets. Returns the count.
 */
size_t ieee154_esp_sdr_get_trail(uint32_t *out, size_t max);

/**
 * Diagnostic: CPU 0 PCs (and last load/store addresses) sampled by the
 * receive ring when the PHY thread made no progress for about a second,
 * kept across resets. Returns the count (0 if no stall was seen, and always 0
 * without CONFIG_ESP_SDR_IEEE802154_RING).
 */
size_t ieee154_esp_sdr_get_stall(uint32_t *pc, uint32_t *ls, size_t max);

/** Copy the driver counters into @p stats. */
void ieee154_esp_sdr_get_stats(struct ieee154_esp_sdr_stats *stats);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ESP_SDR_IEEE154_ESP_SDR_H_ */
