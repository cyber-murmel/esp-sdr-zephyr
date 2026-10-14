/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The radio of the Ubertooth firmware: ownership, tuning and the gapless
 * receive stream. One thread runs the library's ring (esp_sdr_ring_run(), CPU 1,
 * interrupts masked) at 4 MS/s into a sample ring; the DSP thread drains it with
 * ubt_radio_rx_read().
 *
 * The LO sits 3 MHz above the wanted channel, so with the ring's -fs/4 mix and
 * its subsampling the channel appears at +1 MHz in the 4 MS/s stream and the LO
 * leakage at 0 Hz (see gfsk_demod.h).
 */

#ifndef UBT_RADIO_H_
#define UBT_RADIO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The stream, as the ring delivers it. */
#define UBT_RX_RATE_HZ        4000000U
#define UBT_RX_CYCLES_PER_SMP 60U /* 240 MHz CPU clock */

/* The wanted channel sits at this offset in the stream, and the LO this far above it. */
#define UBT_RX_CHANNEL_OFFSET_HZ 1000000
#define UBT_RX_LO_ABOVE_MHZ      3

/* Statistics of the receive stream since ubt_radio_rx_start(). */
struct ubt_rx_stats {
	uint32_t sink_calls;
	uint64_t samples;
	/* Samples lost because the reader was too slow, and times the ring reported a gap. */
	uint64_t overrun_samples;
	uint32_t gaps;
	/* Ring run: units, abandoned units, lost pairs, status, filter cycles per pair x 100. */
	uint32_t ring_units;
	uint32_t ring_abandoned;
	uint32_t ring_lost_pairs;
	uint32_t ring_status;
	uint32_t ring_cycles_x100;
	uint32_t runs;
	/* Microseconds from the start request to the first samples, and how long the retune took. */
	uint32_t start_us;
	uint32_t stop_us;
	uint32_t tune_us;
};

/* Start the ring thread. */
int ubt_radio_init(void);

/*
 * Tune to @p channel_mhz (LO 3 MHz above), set the gain (-1: AGC) and start the stream.
 * Returns once the radio is tuned and the ring is started (samples follow within ~1 ms).
 */
int ubt_radio_rx_start(uint16_t channel_mhz, int gain);

/* Stop the stream and release the radio. */
void ubt_radio_rx_stop(void);

bool ubt_radio_rx_running(void);

/*
 * Copy the oldest unread samples, at most @p max. @p first is the stream index of the first one,
 * @p gap the number of samples lost since the previous call (0 normally). Returns the count, 0 if none.
 */
size_t ubt_radio_rx_read(int16_t *i, int16_t *q, size_t max, uint32_t *first, uint32_t *gap);

/* Samples waiting. */
size_t ubt_radio_rx_available(void);

/*
 * CPU cycle count at which stream sample @p index was taken (index counted from the start of the
 * run, wrapping at 2^32 like gfsk_sym.t); valid within a few seconds of the run's anchor.
 */
uint64_t ubt_radio_rx_cycles(uint32_t index);

void ubt_radio_rx_get_stats(struct ubt_rx_stats *st);

/*
 * The sample ring's memory (internal RAM, 16 byte aligned) for another mode while the stream is
 * stopped and the radio is held (ubt_radio_take()); nothing in it survives ubt_radio_rx_start().
 */
void *ubt_radio_scratch(size_t *bytes);

/* Keep other radio users out (transmit, burst captures): blocks until the ring let go. */
void ubt_radio_take(void);
void ubt_radio_give(void);

#endif /* UBT_RADIO_H_ */
