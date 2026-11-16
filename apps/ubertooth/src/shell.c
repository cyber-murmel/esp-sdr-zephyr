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
#include <esp_sdr/esp_sdr_place.h>

#include "app_crash.h"
#include "gfsk_demod.h"
#include "ubt_rx_sess.h"
#include "ubt_mode.h"
#include "ubt_plat.h"
#include "ubt_radio.h"
#include "ubt_rssi.h"
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

/* ubt rx gain [guard|index|auto]: the gain of the demodulating modes from their next start. */
static int cmd_rx_gain(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		int g;

		if (strcmp(argv[1], "guard") == 0) {
			g = UBT_RX_GAIN_GUARD;
		} else if (strcmp(argv[1], "auto") == 0) {
			g = ESP_SDR_RX_GAIN_AUTO;
		} else {
			g = atoi(argv[1]);
			if (g < 0 || g > esp_sdr_rx_gain_max()) {
				shell_error(sh, "guard, auto or a gain index 0 to %d",
					    esp_sdr_rx_gain_max());
				return -EINVAL;
			}
		}
		ubt_rx_gain = g;
	}
	if (ubt_rx_gain == UBT_RX_GAIN_GUARD) {
		shell_print(sh, "demodulating modes' gain: clip guard (BLE %d down to %d, BR %d)",
			    ubt_guard_gain[0], ubt_guard_gain[UBT_GUARD_LEVELS - 1U], UBT_RX_GAIN);
	} else if (ubt_rx_gain == ESP_SDR_RX_GAIN_AUTO) {
		shell_print(sh, "demodulating modes' gain: hardware AGC");
	} else {
		shell_print(sh, "demodulating modes' gain: %d", ubt_rx_gain);
	}
	return 0;
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
	shell_print(sh, "ring irq windows %u, lines 0x%08x, late max %u pairs", st.ring_irq_windows,
		    st.ring_irq_lines, st.ring_late_max);
	shell_print(sh, "since boot: %u holes skipped with a later one queued, %u records lost",
		    st.holes_queued, st.holes_lost);
	return 0;
}

/* ---- demodulator bench ------------------------------------------------------------------ */

#define BENCH_N 16384U
/* The capture as the ring stores it, stream sample k at GFSK_IN_HIST + k, and a tail. */
static ESP_SDR_DATA(BULK, bench_i8) int8_t bench_i8[GFSK_IN_HIST + BENCH_N + 16U];
static ESP_SDR_DATA(BULK, bench_q8) int8_t bench_q8[GFSK_IN_HIST + BENCH_N + 16U];
/* The same as the reference reads it: int16, (x << 6) + 32, each sample at the middle of its step. */
static ESP_SDR_DATA(BULK, bench_i) int16_t bench_i[BENCH_N], bench_q[BENCH_N];

/* The squelch passes open at the floor of an always emitting pass plus the modes' margin. */
#define BENCH_SQ_MARGIN 14U
#define BENCH_SQ_HOLD   16U

/* Chunk sizes of a pass: UBT_RX_CHUNK each, or pseudo-random 1 to UBT_RX_CHUNK from a seed. */
static uint32_t bench_rng;

static size_t bench_chunk(bool random, uint32_t left)
{
	size_t n = UBT_RX_CHUNK;

	if (random) {
		bench_rng = bench_rng * 1664525U + 1013904223U;
		n = 1U + (bench_rng >> 8) % UBT_RX_CHUNK;
	}
	return MIN(n, (size_t)left);
}

static void bench_setup(struct gfsk_demod *d, uint8_t squelch)
{
	gfsk_demod_init(d, GFSK_BLE);
	if (squelch != 0U) {
		gfsk_demod_set_squelch(d, squelch, BENCH_SQ_HOLD, false);
	}
}

/*
 * Chunk @p at of the capture (n samples) into the session's planes with its history (untimed),
 * as the reader hands it over; returns the offset of the chunk's first sample in them.
 */
static size_t bench_load(struct ubt_rx_sess *s, uint32_t at, size_t n)
{
	/*
	 * As the ring reader lays the planes out: their first byte is a sample whose index is a
	 * multiple of 16, the history before the chunk and its tail rounded up to 16.
	 */
	uint32_t from = at & ~15U;
	size_t len = GFSK_IN_HIST + ROUND_UP((at & 15U) + n, 16U);

	for (size_t k = 0; k < len; k++) {
		s->bi[k] = bench_i8[from + k];
		s->bq[k] = bench_q8[from + k];
	}
	return GFSK_IN_HIST + (at & 15U);
}

/*
 * The demodulator over the capture: cycles per sample. @p squelch 0 is always emitting (its
 * full path, the cost while the squelch is open); otherwise the modes' squelch at that level.
 */
static uint32_t bench_pass(struct ubt_rx_sess *s, uint8_t squelch, bool random, uint32_t *prof)
{
	uint64_t cyc = 0;

	bench_setup(&s->dm, squelch);
	bench_rng = 1;
#ifdef GFSK_PROF
	memset(&gfsk_prof, 0, sizeof(gfsk_prof));
#endif
	for (uint32_t at = 0; at < BENCH_N;) {
		size_t n = bench_chunk(random, BENCH_N - at);
		size_t off = bench_load(s, at, n);
		uint64_t t0;

		t0 = k_cycle_get_64();
		(void)gfsk_demod_push(&s->dm, s->bi + off, s->bq + off, n, s->syms,
				      ARRAY_SIZE(s->syms));
		cyc += k_cycle_get_64() - t0;
		at += (uint32_t)n;
	}
#ifdef GFSK_PROF
	prof[0] = gfsk_prof.hot / BENCH_N;
	prof[1] = (gfsk_prof.groups - gfsk_prof.slice) / BENCH_N;
	prof[2] = gfsk_prof.slice / BENCH_N;
#else
	prof[0] = prof[1] = prof[2] = 0;
#endif
	return (uint32_t)(cyc / BENCH_N);
}

#ifdef CONFIG_APP_UBERTOOTH_GFSK_REF
#include "gfsk_ref.h"

static ESP_SDR_DATA(BULK, bench_ref) struct gfsk_ref bench_ref;
static ESP_SDR_DATA(BULK, bench_ref_syms) struct gfsk_sym
	bench_ref_syms[GFSK_REF_OUT_CAP(UBT_RX_CHUNK)];

struct bench_cmp {
	uint32_t syms, pushes, mismatches;
	/* The first mismatch: push, symbol and what differed. */
	uint32_t push, sym;
	const char *what;
};

static void bench_differ(struct bench_cmp *c, uint32_t sym, const char *what)
{
	if (c->mismatches++ == 0U) {
		c->push = c->pushes;
		c->sym = sym;
		c->what = what;
	}
}

static void bench_cmp_syms(struct bench_cmp *c, const struct gfsk_sym *a, size_t na,
			   const struct gfsk_sym *b, size_t nb)
{
	if (na != nb) {
		bench_differ(c, (uint32_t)MIN(na, nb), "count");
	}
	for (size_t k = 0; k < MIN(na, nb); k++) {
		if (a[k].t != b[k].t) {
			bench_differ(c, (uint32_t)k, "t");
		} else if (a[k].bit != b[k].bit) {
			bench_differ(c, (uint32_t)k, "bit");
		} else if (a[k].pwr != b[k].pwr) {
			bench_differ(c, (uint32_t)k, "pwr");
		}
	}
	c->syms += (uint32_t)nb;
}

/*
 * gfsk_demod against the frozen reference on the same capture cut into the same pushes: every
 * symbol, and after each push the level, the floor, the horizon and the sample counter.
 */
static void bench_compare(struct ubt_rx_sess *s, uint8_t squelch, bool random, struct bench_cmp *c)
{
	size_t na, nb;

	memset(c, 0, sizeof(*c));
	bench_setup(&s->dm, squelch);
	gfsk_ref_init(&bench_ref, GFSK_BLE);
	if (squelch != 0U) {
		gfsk_ref_set_squelch(&bench_ref, squelch, BENCH_SQ_HOLD, false);
	}
	bench_rng = 7;
	for (uint32_t at = 0; at < BENCH_N;) {
		size_t n = bench_chunk(random, BENCH_N - at);
		size_t off = bench_load(s, at, n);

		na = gfsk_demod_push(&s->dm, s->bi + off, s->bq + off, n, s->syms, ARRAY_SIZE(s->syms));
		nb = gfsk_ref_push(&bench_ref, &bench_i[at], &bench_q[at], n, bench_ref_syms,
				   ARRAY_SIZE(bench_ref_syms));
		bench_cmp_syms(c, s->syms, na, bench_ref_syms, nb);
		if (gfsk_demod_level(&s->dm) != gfsk_ref_level(&bench_ref)) {
			bench_differ(c, 0, "level");
		}
		if (gfsk_demod_noise_floor(&s->dm) != gfsk_ref_noise_floor(&bench_ref)) {
			bench_differ(c, 0, "floor");
		}
		if (gfsk_demod_horizon(&s->dm) != gfsk_ref_horizon(&bench_ref)) {
			bench_differ(c, 0, "horizon");
		}
		if (gfsk_demod_now(&s->dm) != gfsk_ref_now(&bench_ref)) {
			bench_differ(c, 0, "now");
		}
		c->pushes++;
		at += (uint32_t)n;
	}
	na = gfsk_demod_flush(&s->dm, s->syms, ARRAY_SIZE(s->syms));
	nb = gfsk_ref_flush(&bench_ref, bench_ref_syms, ARRAY_SIZE(bench_ref_syms));
	bench_cmp_syms(c, s->syms, na, bench_ref_syms, nb);
}

static void bench_compare_all(const struct shell *sh, struct ubt_rx_sess *s, uint8_t squelch)
{
	static const char *const name[2] = {"always", "squelch"};

	for (unsigned int m = 0; m < 2U; m++) {
		for (unsigned int r = 0; r < 2U; r++) {
			struct bench_cmp c;

			bench_compare(s, m != 0U ? squelch : 0U, r != 0U, &c);
			if (c.mismatches == 0U) {
				shell_print(sh, "compare %s, %s pushes: %u symbols in %u pushes, identical",
					    name[m], r != 0U ? "random" : "256", c.syms, c.pushes);
			} else {
				shell_print(sh, "compare %s, %s pushes: %u MISMATCHES in %u symbols, first in push %u, symbol %u (%s)",
					    name[m], r != 0U ? "random" : "256", c.mismatches, c.syms, c.push,
					    c.sym, c.what);
			}
		}
	}
}
#endif

/*
 * ubt rx bench <mhz>: 4 ms of the stream through the demodulator with the ring running and
 * stopped: always emitting (the full path, the cost while the squelch is open) and behind the
 * modes' squelch. The budget is 60 cycles a sample; the reader's int16 conversion comes on top.
 * A build with CONFIG_APP_UBERTOOTH_GFSK_PROF splits the full path into the sample loop, the per
 * group logic and the slicer; one with CONFIG_APP_UBERTOOTH_GFSK_REF compares every symbol with
 * the reference demodulator, in both modes, with pushes of 256 and of random sizes.
 */
ESP_SDR_CODE(COLD, cmd_rx_bench)
static int cmd_rx_bench(const struct shell *sh, size_t argc, char **argv)
{
	unsigned long ch = strtoul(argv[1], NULL, 10);
	struct ubt_rx_sess *s = ubt_rx_sess_open(&ubt, GFSK_BLE);
	uint32_t got = 0, prof[3], always, closed;
	uint64_t rd_cyc = 0;
	uint8_t squelch;
	int64_t end;

	if (ubt_radio_rx_start((uint16_t)ch, UBT_RX_GAIN) != 0) {
		shell_error(sh, "no stream");
		return -EIO;
	}
	end = k_uptime_get() + 1000;
	while (got < BENCH_N && k_uptime_get() < end) {
		uint32_t first, gap;
		size_t off = 0;
		uint64_t t0 = k_cycle_get_64();
		/* As the modes read: chunks into the session's planes in internal RAM. */
		size_t n = ubt_radio_rx_read8(s->bi, s->bq, MIN(UBT_RX_CHUNK, BENCH_N - got),
					      GFSK_IN_HIST, &first, &gap, &off);

		rd_cyc += k_cycle_get_64() - t0;
		for (size_t k = 0; k < n; k++) {
			bench_i8[GFSK_IN_HIST + got + k] = s->bi[off + k];
			bench_q8[GFSK_IN_HIST + got + k] = s->bq[off + k];
			bench_i[got + k] = (int16_t)(s->bi[off + k] * 64 + 32);
			bench_q[got + k] = (int16_t)(s->bq[off + k] * 64 + 32);
		}
		got += (uint32_t)n;
		if (n == 0U) {
			k_sleep(K_USEC(200));
		}
	}
	if (got < BENCH_N) {
		ubt_radio_rx_stop();
		shell_error(sh, "only %u of %u samples", got, BENCH_N);
		return -EIO;
	}
	always = bench_pass(s, 0, false, prof);
	squelch = (uint8_t)MIN(255U, gfsk_demod_noise_floor(&s->dm) + BENCH_SQ_MARGIN);
	shell_print(sh, "reader (int8 copy with the history): %u cycles per sample",
		    (uint32_t)(rd_cyc / BENCH_N));
	shell_print(sh, "FIR on the vector unit against C: %u samples differ",
		    gfsk_demod_fir_check(k_cycle_get_32()));
	shell_print(sh, "slicer in assembly against C: %u of 4000 cases differ",
		    gfsk_demod_slice_check(k_cycle_get_32()));
	shell_print(sh, "group logic in assembly against C: %u of 4000 cases differ",
		    gfsk_demod_groups_check(k_cycle_get_32()));
	shell_print(sh, "ring running: always %u cycles per sample (loop %u, groups %u, slicer %u)",
		    always, prof[0], prof[1], prof[2]);
	closed = bench_pass(s, squelch, false, prof);
	shell_print(sh, "  behind the squelch (pwr %u): %u cycles per sample", squelch, closed);
	ubt_radio_rx_stop();
	always = bench_pass(s, 0, false, prof);
	shell_print(sh, "ring stopped: always %u cycles per sample (loop %u, groups %u, slicer %u)",
		    always, prof[0], prof[1], prof[2]);
	closed = bench_pass(s, squelch, false, prof);
	shell_print(sh, "  behind the squelch (pwr %u): %u cycles per sample", squelch, closed);
#ifdef CONFIG_APP_UBERTOOTH_GFSK_REF
	bench_compare_all(sh, s, squelch);
#endif
	return 0;
}

/* ---- spectrum of the stream ------------------------------------------------------------- */

#define FFT_N      256U
#define SPEC_GROUP 8U /* bins per printed line: 125 kHz */

static ESP_SDR_DATA(BULK, fre) float fre[FFT_N], fim[FFT_N], twr[FFT_N / 2U], twi[FFT_N / 2U];
static ESP_SDR_DATA(BULK, acc) float acc[FFT_N];
static ESP_SDR_DATA(BULK, blk_i) int16_t blk_i[FFT_N], blk_q[FFT_N];

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

/*
 * ubt rx ring [unit pairs] [LO MHz]: the ring's unit length and the LO's offset from the channel
 * from the next start (none or 0: 3072 pairs, UBT_RX_LO_OFFSET_MHZ).
 */
static int cmd_rx_ring(const struct shell *sh, size_t argc, char **argv)
{
	ubt_radio_unit_pairs = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 0U;
	ubt_radio_lo_offset_mhz = argc > 2 ? atoi(argv[2]) : 0;
	shell_print(sh, "ring from the next start: %u pairs a unit, the LO %d MHz from the channel",
		    ubt_radio_unit_pairs != 0U ? ubt_radio_unit_pairs : 3072U,
		    ubt_radio_lo_offset_mhz != 0 ? ubt_radio_lo_offset_mhz : UBT_RX_LO_OFFSET_MHZ);
	return 0;
}

/* ubt pollhold [ms]: how long an empty POLL waits for a packet in the BLE modes. */
static int cmd_pollhold(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		ubt_poll_hold_ms = CLAMP(atoi(argv[1]), 0, 100);
	}
	shell_print(sh, "empty POLLs wait up to %d ms for a packet in the BLE modes",
		    ubt_poll_hold_ms);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(rx_cmds,
	SHELL_CMD_ARG(start, NULL, "<channel mhz> [gain index]", cmd_rx_start, 2, 1),
	SHELL_CMD(stop, NULL, "Stop the stream", cmd_rx_stop),
	SHELL_CMD_ARG(gain, NULL, "[guard|index|auto]: gain of the demodulating modes", cmd_rx_gain, 1, 1),
	SHELL_CMD_ARG(ring, NULL, "[unit pairs] [LO MHz]: the ring's unit and LO offset (0: default)",
		      cmd_rx_ring, 1, 2),
	SHELL_CMD(stats, NULL, "Stream statistics", cmd_rx_stats),
	SHELL_CMD_ARG(bench, NULL, "<mhz>: demodulator cycles with and without the ring", cmd_rx_bench, 2, 0),
	SHELL_CMD_ARG(spec, NULL, "[frames]: power spectrum of the stream", cmd_rx_spec, 1, 1),
	SHELL_CMD_ARG(floor, NULL, "[frames]: noise floor, DC spur, in-channel power", cmd_rx_floor, 1, 1),
	SHELL_SUBCMD_SET_END);

/* ubt crash: what ended the previous boot, if it ended badly (app_crash.h). */
static int cmd_crash(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (!app_crash_print(sh)) {
		shell_print(sh, "no crash recorded");
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(ubt_cmds,
	SHELL_CMD(status, NULL, "Device state", cmd_status),
	SHELL_CMD(crash, NULL, "What ended the previous boot", cmd_crash),
	SHELL_CMD_ARG(tune, NULL, "<mhz a> <mhz b> [n]: time retunes", cmd_tune, 3, 1),
	SHELL_CMD(rx, &rx_cmds, "Receive stream", NULL),
	SHELL_CMD_ARG(specanfs, NULL, "<dbm of a full scale tone>: spectrum analyser calibration",
		      cmd_specan_fs, 2, 0),
	SHELL_CMD_ARG(pollhold, NULL, "[ms]: an empty POLL waits for a packet (BLE modes, 0: off)",
		      cmd_pollhold, 1, 1),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(ubt, &ubt_cmds, "Ubertooth firmware", NULL);
