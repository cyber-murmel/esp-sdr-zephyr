/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Shell commands for bench work: "ubt status", and the measurements of the
 * radio path (retune time, ring start, the spectrum of the 4 MS/s stream).
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <esp_attr.h>
#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_dsp.h>
#include <esp_sdr/esp_sdr_rx.h>

#include "ubt_mode.h"
#include "ubt_plat.h"
#include "ubt_radio.h"
#include "ubt_usb.h"

#define CYC_PER_US (CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / 1000000U)

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "mode %u (requested %u) channel %u hop mode %u modulation %u", ubt.mode,
		    ubt.requested_mode, ubt.channel, ubt.hop_mode, ubt.modulation);
	shell_print(sh, "usb %s, queue %u packets, %u dropped", ubt_usb_enabled() ? "up" : "down",
		    (unsigned int)ubt_queue_count(&ubt.queue), ubt.queue.dropped);
	shell_print(sh, "clkn %u", ubt_clock_clkn(&ubt.clock, ubt.plat->cycles()));
	return 0;
}

/* ubt tune <mhz a> <mhz b> [n]: time esp_sdr_set_freq() alternating between two frequencies. */
static int cmd_tune(const struct shell *sh, size_t argc, char **argv)
{
	unsigned long a = strtoul(argv[1], NULL, 10);
	unsigned long b = strtoul(argv[2], NULL, 10);
	unsigned int n = argc > 3 ? (unsigned int)strtoul(argv[3], NULL, 10) : 10U;
	uint32_t tmin = UINT32_MAX;
	uint32_t tmax = 0;
	uint64_t sum = 0;

	if (ubt_radio_rx_running()) {
		shell_error(sh, "stop the receive stream first");
		return -EBUSY;
	}
	ubt_radio_take();
	for (unsigned int k = 0; k < n; k++) {
		uint64_t t0 = k_cycle_get_64();
		int ret = esp_sdr_set_freq((uint32_t)((k & 1U) != 0U ? b : a));
		uint32_t us = (uint32_t)((k_cycle_get_64() - t0) / CYC_PER_US);

		if (ret != 0) {
			shell_error(sh, "set_freq failed (%d)", ret);
			break;
		}
		tmin = MIN(tmin, us);
		tmax = MAX(tmax, us);
		sum += us;
	}
	ubt_radio_give();
	shell_print(sh, "%u retunes %lu <-> %lu MHz: min %u us, mean %u us, max %u us", n, a, b, tmin,
		    (unsigned int)(sum / MAX(n, 1U)), tmax);
	return 0;
}

static int cmd_rx_start(const struct shell *sh, size_t argc, char **argv)
{
	unsigned long ch = strtoul(argv[1], NULL, 10);
	int gain = argc > 2 ? atoi(argv[2]) : ESP_SDR_RX_GAIN_AUTO;
	struct ubt_rx_stats st;
	int ret = ubt_radio_rx_start((uint16_t)ch, gain);

	ubt_radio_rx_get_stats(&st);
	shell_print(sh, "rx start %lu MHz gain %d: %d, retune %u us, first samples after %u us (stop of the previous run took %u us)",
		    ch, gain, ret, st.tune_us, st.start_us, st.stop_us);
	return ret;
}

static int cmd_rx_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	ubt_radio_rx_stop();
	shell_print(sh, "rx stopped");
	return 0;
}

static int cmd_rx_stats(const struct shell *sh, size_t argc, char **argv)
{
	struct ubt_rx_stats st;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	ubt_radio_rx_get_stats(&st);
	shell_print(sh, "running %d: %u samples in %u sink calls, %u ring gaps, %u units (%u abandoned, %u pairs lost), status %u",
		    ubt_radio_rx_running(), (unsigned int)st.samples, st.sink_calls, st.gaps,
		    st.ring_units, st.ring_abandoned, st.ring_lost_pairs, st.ring_status);
	shell_print(sh, "ring cost %u.%02u cycles per pair, reader overruns %u samples",
		    st.ring_cycles_x100 / 100U, st.ring_cycles_x100 % 100U,
		    (unsigned int)st.overrun_samples);
	return 0;
}

/* ---- spectrum of the stream ------------------------------------------------------------- */

#define FFT_N      256U
#define SPEC_GROUP 8U /* bins per printed line: 125 kHz */

static EXT_RAM_BSS_ATTR float fre[FFT_N], fim[FFT_N], twr[FFT_N / 2U], twi[FFT_N / 2U];
static EXT_RAM_BSS_ATTR float acc[FFT_N];
static EXT_RAM_BSS_ATTR int16_t blk_i[FFT_N], blk_q[FFT_N];

/* Average power spectrum of the running stream into acc[], mean I/Q into mi/mq; frames done. */
static unsigned int spectrum(unsigned int frames, double *mi, double *mq)
{
	unsigned int done = 0;
	int64_t t_end = k_uptime_get() + 2000;

	*mi = 0.0;
	*mq = 0.0;
	esp_sdr_fft_twiddles(twr, twi, FFT_N);
	memset(acc, 0, sizeof(acc));
	while (done < frames && k_uptime_get() < t_end) {
		uint32_t first;
		uint32_t gap;

		if (ubt_radio_rx_available() < FFT_N) {
			k_sleep(K_USEC(200));
			continue;
		}
		if (ubt_radio_rx_read(blk_i, blk_q, FFT_N, &first, &gap) < FFT_N) {
			continue;
		}
		for (unsigned int k = 0; k < FFT_N; k++) {
			float w = esp_sdr_window(ESP_SDR_WINDOW_HANN, k, FFT_N);

			fre[k] = w * (float)blk_i[k];
			fim[k] = w * (float)blk_q[k];
			*mi += blk_i[k];
			*mq += blk_q[k];
		}
		esp_sdr_fft(fre, fim, FFT_N, twr, twi, FFT_N);
		for (unsigned int k = 0; k < FFT_N; k++) {
			acc[k] += fre[k] * fre[k] + fim[k] * fim[k];
		}
		done++;
	}
	*mi /= (double)(MAX(done, 1U) * FFT_N);
	*mq /= (double)(MAX(done, 1U) * FFT_N);
	return done;
}

/* Peak bin of a full scale tone: A * sum(w), A = 8192, sum(w) = N / 2. */
#define SPEC_REF (8192.0f * (float)FFT_N / 2.0f)

/* Power in dBFS (per bin) of the bins of a band [f0, f1) MHz, averaged over the frames. */
static float band_db(unsigned int frames, double f0, double f1)
{
	double sum = 0.0;
	unsigned int n = 0;

	for (unsigned int k = 0; k < FFT_N; k++) {
		/* FFT order to frequency: 0 .. N/2 - 1 positive, then negative. */
		double f = (k < FFT_N / 2U ? (double)k : (double)k - (double)FFT_N) * 4.0 / (double)FFT_N;

		if (f >= f0 && f < f1) {
			sum += (double)acc[k] / (double)frames / ((double)SPEC_REF * (double)SPEC_REF);
			n++;
		}
	}
	return 10.0f * log10f((float)(sum / (double)MAX(n, 1U)) + 1e-20f);
}

/*
 * ubt rx spec [frames]: average power spectrum of the running stream (Hann window, 256 points,
 * 15.6 kHz bins) in groups of 8 bins, -2 to +2 MHz; dBFS of a full scale tone (8192) in one bin.
 * Also the mean I/Q (the DC term).
 */
static int cmd_rx_spec(const struct shell *sh, size_t argc, char **argv)
{
	unsigned int frames = argc > 1 ? (unsigned int)strtoul(argv[1], NULL, 10) : 64U;
	double mi;
	double mq;
	unsigned int done;

	if (!ubt_radio_rx_running()) {
		shell_error(sh, "start the receive stream first (ubt rx start <mhz>)");
		return -EINVAL;
	}
	done = spectrum(frames, &mi, &mq);
	if (done == 0U) {
		shell_error(sh, "no samples");
		return -EIO;
	}
	shell_print(sh, "%u frames, mean I %.1f Q %.1f (LSB, full scale 8192)", done, mi, mq);
	for (unsigned int g = 0; g < FFT_N / SPEC_GROUP; g++) {
		float p = 0.0f;
		float pmax = 0.0f;

		for (unsigned int b = 0; b < SPEC_GROUP; b++) {
			unsigned int bin = (g * SPEC_GROUP + b + FFT_N / 2U) % FFT_N;
			float v = acc[bin] / (float)done / (SPEC_REF * SPEC_REF);

			p += v;
			pmax = MAX(pmax, v);
		}
		p /= (float)SPEC_GROUP;
		shell_print(sh, "%+6.3f MHz  mean %6.1f  max %6.1f dBFS", -2.0 + (double)g * 0.125 + 0.0625,
			    (double)(10.0f * log10f(p + 1e-20f)), (double)(10.0f * log10f(pmax + 1e-20f)));
	}
	return 0;
}

/*
 * ubt rx floor [frames]: noise floor (the outer bins), the DC spur (within 100 kHz of 0 Hz), the
 * in-channel band (+0.5 to +1.5 MHz) and the mean I/Q, all in dBFS per bin: one line per gain.
 */
static int cmd_rx_floor(const struct shell *sh, size_t argc, char **argv)
{
	unsigned int frames = argc > 1 ? (unsigned int)strtoul(argv[1], NULL, 10) : 128U;
	double mi;
	double mq;
	unsigned int done;
	float outer;

	if (!ubt_radio_rx_running()) {
		shell_error(sh, "start the receive stream first");
		return -EINVAL;
	}
	done = spectrum(frames, &mi, &mq);
	if (done == 0U) {
		return -EIO;
	}
	outer = 0.5f * (band_db(done, -2.0, -1.4) + band_db(done, 1.6, 2.0));
	shell_print(sh, "gain %d: floor %.1f dBFS/bin, DC spur %.1f, in-channel %.1f, mean I %.0f Q %.0f",
		    esp_sdr_rx_get_gain(), (double)outer, (double)band_db(done, -0.1, 0.1),
		    (double)band_db(done, 0.5, 1.5), mi, mq);
	return 0;
}

static int cmd_specan_fs(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ubt_specan_set_fs_dbm((int)(atof(argv[1]) * 256.0));
	shell_print(sh, "full scale tone = %s dBm at the spectrum analyser's gain", argv[1]);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(rx_cmds,
	SHELL_CMD_ARG(start, NULL, "<channel mhz> [gain index]", cmd_rx_start, 2, 1),
	SHELL_CMD(stop, NULL, "Stop the stream", cmd_rx_stop),
	SHELL_CMD(stats, NULL, "Stream statistics", cmd_rx_stats),
	SHELL_CMD_ARG(spec, NULL, "[frames]: power spectrum of the stream", cmd_rx_spec, 1, 1),
	SHELL_CMD_ARG(floor, NULL, "[frames]: noise floor, DC spur, in-channel power", cmd_rx_floor, 1, 1),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(ubt_cmds,
	SHELL_CMD(status, NULL, "Device state", cmd_status),
	SHELL_CMD_ARG(tune, NULL, "<mhz a> <mhz b> [n]: time retunes", cmd_tune, 3, 1),
	SHELL_CMD(rx, &rx_cmds, "Receive stream", NULL),
	SHELL_CMD_ARG(specanfs, NULL, "<dbm of a full scale tone>: spectrum analyser calibration",
		      cmd_specan_fs, 2, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(ubt, &ubt_cmds, "Ubertooth firmware", NULL);
