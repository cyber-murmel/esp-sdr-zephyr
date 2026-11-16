/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The receive session of the demodulating modes (BLE sniffing, following and promiscuous mode,
 * the BR symbol stream): the 4 MS/s stream of ubt_radio on one channel through the GFSK
 * demodulator, a chunk per poll, across retunes, ring gaps and ring restarts.
 *
 * Time. The demodulator counts samples from the last tune (gfsk_sym.t, 32 bit); the session
 * keeps that count in 64 bits and turns sample indices into the device's 64 bit timeline (100 ns,
 * ubt_clock): the time base of le_conn and the promiscuous mode, and the source of the packets'
 * clkn_high and clk100ns. ubt_rx_sess_now() is the time of the newest sample demodulated (the
 * stream's own "now", behind the wall clock by the samples still waiting in the ring).
 */

#ifndef UBT_RX_SESS_H_
#define UBT_RX_SESS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/util.h>

#include "gfsk_demod.h"
#include "ubt_dev.h"
#include "ubt_le.h"
#include "ubt_radio.h"

/* Samples per poll (64 us). */
#define UBT_RX_CHUNK 256U

/* The int8 planes of a chunk: UBT_RX_CHUNK samples, the demodulator's history and tail. */
#define UBT_RX_IN_BYTES UBT_RADIO_READ8_BYTES(UBT_RX_CHUNK, GFSK_IN_HIST)

/* Receive gain index of the demodulating modes (fixed: the hardware AGC would move the levels). */
#define UBT_RX_GAIN 60

/*
 * ubt_rx_gain for the BLE modes' clip guard (esp_sdr_gain_guard.h) from UBT_RX_GAIN; BR keeps
 * that.
 */
#define UBT_RX_GAIN_GUARD (-2)

/*
 * The modes' gain (shell "ubt rx gain"): UBT_RX_GAIN (the default), another fixed index (a
 * connection within a metre or so clips at 60: some 40 for it), UBT_RX_GAIN_GUARD (experimental:
 * on the bench it stepped back up between the host's bursts of traffic and stepped down again on
 * the next), or ESP_SDR_RX_GAIN_AUTO (the hardware AGC, which idles above 60 and costs packets).
 */
extern int ubt_rx_gain;

struct ubt_rx_sess {
	struct ubt_dev *d;
	struct gfsk_demod dm;
	/* The BLE packet finder: per symbol work, so it lives here in internal RAM too. */
	struct ubt_le le;
	/* The chunk as the ring stores it (8 bits), with the history the demodulator replays from. */
	int8_t bi[UBT_RX_IN_BYTES] __aligned(16);
	int8_t bq[UBT_RX_IN_BYTES] __aligned(16);
	struct gfsk_sym syms[GFSK_OUT_CAP(UBT_RX_CHUNK)];

	uint16_t mhz;
	/* The receive gain index the next tune applies. */
	int gain;
	/* Stream index of the next sample expected, and the demodulator's count in 64 bits. */
	uint32_t next;
	uint64_t n64;
	/* Wall time of the last tune request and of its stream's sample 0. */
	uint64_t t_tune, t_first;

	uint32_t tunes;
	uint32_t restarts;
	uint32_t gaps;
	uint64_t gap_samples;
	/* Longest time from a tune request to the stream's sample 0 (100 ns). */
	uint32_t tune_max;
	/* CPU cycles in the demodulator and the samples it took. */
	uint64_t dm_cycles, dm_samples;
	/* ... of them in pushes where the full path ran (the squelch open at some point). */
	uint64_t dm_open_cycles, dm_open_samples;
	/* CPU cycles in the ring reader (its copy and conversion), polls without samples included. */
	uint64_t rd_cycles;
	/* Gaps found while the demodulator's full path ran (the squelch open). */
	uint32_t gaps_open;
};

/* The result of a poll. */
struct ubt_rx_poll {
	/* Samples read (0: none were waiting, sleep a little). */
	size_t samples;
	/* Symbols the demodulator emitted, in s->syms. */
	size_t nsym;
	/* Samples the ring lost before these (the demodulator has jumped over them). */
	uint32_t gap;
	/* The ring started over by itself (the stream index went back to 0): forget the stream. */
	bool restarted;
};

/* The one session (internal RAM), set up for @p mod with the automatic squelch; no stream yet. */
struct ubt_rx_sess *ubt_rx_sess_open(struct ubt_dev *d, enum gfsk_modulation mod);

/*
 * Stream from @p mhz (the channel; the LO goes 3 MHz above): the old stream stops, the new one
 * starts (3 to 4 ms of retune), the demodulator restarts at sample 0 with its squelch kept.
 */
int ubt_rx_sess_tune(struct ubt_rx_sess *s, uint16_t mhz);

/* Stop the stream. */
void ubt_rx_sess_close(struct ubt_rx_sess *s);

/* Read up to a chunk of samples and demodulate them. */
void ubt_rx_sess_poll(struct ubt_rx_sess *s, struct ubt_rx_poll *p);

/*
 * Run the stream for @p samples (at most @p timeout_ms) always emitting, and return the noise
 * floor, pwr units: a low percentile of the symbols' power (ubt_floor_hist). @p tracker gets the
 * demodulator's own tracked floor, for the log. The symbols are dropped.
 */
uint8_t ubt_rx_sess_floor(struct ubt_rx_sess *s, uint32_t samples, int64_t timeout_ms,
			  uint8_t *tracker);

/* Timeline (100 ns) of stream sample @p index of the current tune (at or before the newest). */
uint64_t ubt_rx_sess_t100(const struct ubt_rx_sess *s, uint32_t index);

/* Timeline of the newest sample demodulated; the wall clock before the first. */
uint64_t ubt_rx_sess_now(const struct ubt_rx_sess *s);

/* Timeline now, from the CPU clock. */
uint64_t ubt_rx_sess_wall(const struct ubt_rx_sess *s);

/* Sleep until the timeline reaches @p t100 (returns at once if it has). */
void ubt_rx_sess_wait_until(const struct ubt_rx_sess *s, uint64_t t100);

/* clkn_high and clk100ns of stream sample @p index. */
void ubt_rx_sess_split(const struct ubt_rx_sess *s, uint32_t index, uint8_t *clkn_high,
		       uint32_t *clk100ns);

/* Zero the loss and CPU counters (after the floor measurement, which loses samples by design). */
void ubt_rx_sess_stats_reset(struct ubt_rx_sess *s);

/* Log the CPU 0 cost of the stream: reader, demodulator closed and open, cycles a sample. */
void ubt_rx_sess_log_cpu(const struct ubt_rx_sess *s);

/* Put a packet in the USB queue (a full queue drops it and flags the next one). */
void ubt_rx_sess_emit(struct ubt_dev *d, const usb_pkt_rx *pkt);

#endif /* UBT_RX_SESS_H_ */
