/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The transmit engine behind esdr.tx_*(): a feeder thread on CPU 1 keeps the DAC backend's input
 * ring (esp_sdr_tx_write()) about 50 ms ahead from a signal generator or a buffer of
 * interleaved int16 I/Q, until it is stopped or the samples are done.
 */

#ifndef ESDR_TX_H_
#define ESDR_TX_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* gnuradio.analog waveforms (GR_CONST_WAVE and so on, 100 + n). */
enum esdr_tx_wave {
	ESDR_WAVE_CONST,
	ESDR_WAVE_SIN,
	ESDR_WAVE_COS,
	ESDR_WAVE_SQR,
	ESDR_WAVE_TRI,
	ESDR_WAVE_SAW,
};

/* Generator settings; may change while running. @p cycles is the frequency over the rate. */
struct esdr_tx_gen {
	enum esdr_tx_wave wave;
	float cycles;
	float amp, dc_i, dc_q;
};

struct esdr_tx_stats {
	uint64_t fed, played, dropped;
	uint32_t underruns, restarts, queued, runs;
	int last_error;
	/* The DAC backend's timing: us, see struct esp_sdr_tx_dac_stats. */
	uint32_t errors, switches, fill_us_max, wake_late_us_max, idle_us_max, switch_us_max;
	int32_t slack_us_min, start_margin_us;
	bool simd;
};

/*
 * Starts transmitting at @p mhz + @p khz with the DAC backend: @p rate_hz input samples per
 * second, gain index @p gain. @p buf (@p len samples) is played, once or @p repeat, else the
 * generator @p gen; @p scale multiplies a buffer. @p count ends it after that many samples
 * (0: no limit). Returns once the backend is up.
 */
int esdr_tx_start(uint32_t mhz, int32_t khz, uint32_t rate_hz, int gain, const int16_t *buf,
		  size_t len, bool repeat, const struct esdr_tx_gen *gen, float scale,
		  uint64_t count);

void esdr_tx_set_gen(const struct esdr_tx_gen *gen);
void esdr_tx_set_scale(float scale);

void esdr_tx_stop(void);
bool esdr_tx_busy(void);

/* Waits up to @p timeout_ms (negative: for ever) for the transmission to end; true when it has. */
bool esdr_tx_wait(int32_t timeout_ms);

void esdr_tx_get_stats(struct esdr_tx_stats *stats);

/*
 * Loop playback at the DAC rate @p fs (40 or 80 MS/s): one period of the generator @p g (cycles
 * per DAC sample; the nearest whole number of cycles in at most ESP_SDR_SAMPLES_MAX samples) or
 * @p buf (@p len samples) interpolated by @p interp, until esdr_tx_loop_stop(). Call from one
 * thread: the session holds the radio's mutex.
 */
int esdr_tx_loop_start(uint32_t mhz, int32_t khz, uint32_t fs, int gain, const int16_t *buf,
		       size_t len, uint32_t interp, const struct esdr_tx_gen *g, float scale);
/* After esdr_tx_set_gen() or esdr_tx_set_scale(): play the change. */
int esdr_tx_loop_update(void);
void esdr_tx_loop_stop(void);
bool esdr_tx_looping(void);
/* The loop period for @p cycles per sample: @p k whole cycles in @p n samples. */
void esdr_tx_period(float cycles, int32_t *k, uint32_t *n);

#endif /* ESDR_TX_H_ */
