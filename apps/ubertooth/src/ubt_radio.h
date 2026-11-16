/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The radio of the Ubertooth firmware: ownership, tuning and the gapless
 * receive stream. One thread runs the library's ring (esp_sdr_ring_run(), CPU 1,
 * interrupts masked) at 4 MS/s into a sample ring; the DSP thread drains it with
 * ubt_radio_rx_read().
 *
 * The LO sits 4 MHz below the wanted channel: the ring's -fs/4 mix brings the
 * channel to DC, and its sums of 4 pairs (a boxcar) decimate to 4 MS/s with their
 * nulls on what folds onto the channel (the bands 4 and 8 MHz away) and on the LO
 * leakage, which the mix moves to -4 MHz (see gfsk_demod.h). Until 2026-11-12 the
 * LO sat 5 MHz below: the channel at +1 MHz took the bands 3 and 5 MHz away, off
 * the nulls (1 dB). Every 4th pair instead of the sums, kept until then for
 * comparison, would fold the leakage onto a channel at DC.
 */

#ifndef UBT_RADIO_H_
#define UBT_RADIO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The stream, as the ring delivers it. */
#define UBT_RX_RATE_HZ        4000000U
#define UBT_RX_CYCLES_PER_SMP 60U /* 240 MHz CPU clock */

/* The LO this far from the wanted channel, which sits at DC in the stream. */
#define UBT_RX_LO_OFFSET_MHZ (-4)

/* Statistics of the receive stream since ubt_radio_rx_start(). */
struct ubt_rx_stats {
	uint32_t sink_calls;
	uint64_t samples;
	/* Samples lost because the reader was too slow, and times the ring reported a gap. */
	uint64_t overrun_samples;
	uint32_t gaps;
	/*
	 * Since boot: holes skipped while a later one was recorded already (one record of the latest
	 * hole, as before 2026-11-12, would have delivered the earlier one's stale slots), and times
	 * the reader fell so many holes behind that their records were reused (ubt_sring.h).
	 */
	uint32_t holes_queued;
	uint32_t holes_lost;
	/*
	 * Ring run: units, abandoned units, lost pairs, status, filter cycles per pair x 100, the
	 * interrupt windows it opened and their CPU lines, its lateness in pairs at most.
	 */
	uint32_t ring_units;
	uint32_t ring_abandoned;
	uint32_t ring_lost_pairs;
	uint32_t ring_status;
	uint32_t ring_cycles_x100;
	uint32_t ring_irq_windows;
	uint32_t ring_irq_lines;
	uint32_t ring_late_max;
	uint32_t runs;
	/* Microseconds from the start request to the first samples, and how long the retune took. */
	uint32_t start_us;
	uint32_t stop_us;
	uint32_t tune_us;
};

/* Start the ring thread. */
int ubt_radio_init(void);

/*
 * Overrides for experiments from the next start (shell "ubt rx ring"): the ring's unit length in
 * pairs (0: 3072, 192 us; in the shortest the sums did not keep up) and the LO's offset from the
 * channel in MHz (0: UBT_RX_LO_OFFSET_MHZ).
 */
extern uint32_t ubt_radio_unit_pairs;
extern int ubt_radio_lo_offset_mhz;

/*
 * Tune to @p channel_mhz (the LO UBT_RX_LO_OFFSET_MHZ from it), set the gain (-1: AGC)
 * and start the stream. Returns once the radio is tuned and the ring is started (samples follow
 * within ~1 ms).
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

/* The buffer read8 needs: @p max samples, @p hist before them and the alignment. */
#define UBT_RADIO_READ8_BYTES(max, hist) (((hist) + 15U + (max) + 15U) & ~15U)

/*
 * The same as the ring stores it, 8 bits (the stream >> 6), for gfsk_demod_push(): the samples
 * from @p hist before the first (rounded down to a multiple of 16, for a word copy) to the last
 * (rounded up) are copied into @p bi, @p bq (16 byte aligned, UBT_RADIO_READ8_BYTES(max, hist)
 * bytes), the first one at offset @p off. What precedes a gap or a new run is garbage. A chunk
 * the writer lapped during the copy (its history included) is dropped as a gap.
 */
size_t ubt_radio_rx_read8(int8_t *bi, int8_t *bq, size_t max, uint32_t hist, uint32_t *first,
			  uint32_t *gap, size_t *off);

/* Samples waiting. */
size_t ubt_radio_rx_available(void);

/*
 * CPU cycle count at which stream sample @p index was taken (index counted from the start of the
 * run, 64 bit: ubt_rx_sess unwraps the demodulator's 32 bit gfsk_sym.t). The ring's sample clock and
 * the CPU's come from one crystal, so the anchor taken once per run holds for the whole run.
 */
uint64_t ubt_radio_rx_cycles(uint64_t index);

void ubt_radio_rx_get_stats(struct ubt_rx_stats *st);

/*
 * The sample ring's memory (16 KiB of internal RAM, 16 byte aligned) for a mode without the stream
 * (spectrum analyser, transmit) while the radio is held (ubt_radio_take()); nothing in it survives
 * ubt_radio_rx_start().
 */
void *ubt_radio_scratch(size_t *bytes);

/* Keep other radio users out (transmit, burst captures): blocks until the ring let go. */
void ubt_radio_take(void);
void ubt_radio_give(void);

#endif /* UBT_RADIO_H_ */
