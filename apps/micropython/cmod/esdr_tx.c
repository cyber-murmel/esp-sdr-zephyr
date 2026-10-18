/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The transmit engine: a cooperative thread pinned to CPU 0 (the fillers of the DAC backend run on
 * CPU 1, MicroPython shares CPU 0) generates or copies samples into the backend's ring whenever
 * less than about 50 ms are queued. The generator's parameters live in internal RAM, written by
 * the REPL's CPU and read here; a chunk may mix two settings, which is harmless.
 *
 * Periodic signals can instead loop from a DAC bank at the DAC's own 40 or 80 MS/s (loop
 * playback, esdr_tx_loop_*()): no interpolation, no feeding, the full DAC bandwidth. A loop holds
 * a whole number of periods in at most ESP_SDR_SAMPLES_MAX samples; a change is written into the
 * idle bank and playback restarts on it. Loop sessions run in the MicroPython thread (the radio
 * lock is a mutex its taker must release).
 */

#include <errno.h>
#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <esp_attr.h>
#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_dsp.h>
#include <esp_sdr/esp_sdr_tx.h>

#include "esdr_rx.h"
#include "esdr_tx.h"

/*
 * Not CPU 1: the leading filler of the DAC backend (CONFIG_ESP_SDR_TX_DAC_LEADER_CPU) runs there with
 * the scheduler and interrupts locked in turns, and a feeder below it starves. Cooperative, to
 * run ahead of the MicroPython thread that shares CPU 0; it sleeps between short bursts.
 */
#define TX_CPU 0
#define CHUNK 256U
#define SINE_N 1024U

#if !defined(CONFIG_ESP_SDR_TX_DAC)
#error "esdr_tx needs CONFIG_ESP_SDR_TX_DAC"
#endif

static EXT_RAM_BSS_ATTR int16_t sine_tab[SINE_N + 1U];
static struct esp_sdr_iq16 chunk[CHUNK] ESP_SDR_HIGH_RAM;

static struct {
	uint32_t mhz, rate;
	int32_t khz;
	int gain;
	const int16_t *buf;
	size_t len, pos;
	bool repeat;
	uint64_t count, sent;
	int start_ret;
} job;

/* Written by the REPL's CPU while running. */
static struct esdr_tx_gen gen;
static float scale = 1.0f;
static uint32_t phase;

static atomic_t busy, stop_req;
static struct esdr_tx_stats stats;
/* A loop session is open (see the loop playback section). */
static bool loop_on;
static K_SEM_DEFINE(start_sem, 0, 1);
static K_SEM_DEFINE(ready_sem, 0, 1);
static K_SEM_DEFINE(done_sem, 0, 1);

static inline int32_t sine(uint32_t ph)
{
	uint32_t idx = ph >> 22;
	int32_t a = sine_tab[idx], b = sine_tab[idx + 1U];

	return a + (((b - a) * (int32_t)((ph >> 6) & 0xFFFFU)) >> 16);
}

static inline int16_t clip(int32_t v)
{
	return (int16_t)(v > 32767 ? 32767 : (v < -32767 ? -32767 : v));
}

/* The waveform at phase @p ph (a full turn is 2^32), full scale +-32767. */
static inline void wave_at(enum esdr_tx_wave w, uint32_t ph, int32_t *i, int32_t *q)
{
	*q = 0;
	switch (w) {
	case ESDR_WAVE_SIN:
		*i = sine(ph);
		*q = -sine(ph + 0x40000000U);
		break;
	case ESDR_WAVE_COS:
		*i = sine(ph + 0x40000000U);
		*q = sine(ph);
		break;
	case ESDR_WAVE_SQR:
		*i = ph < 0x80000000U ? 32767 : -32767;
		break;
	case ESDR_WAVE_TRI: {
		int32_t t = (int32_t)(ph >> 16) - 32768;   /* -32768 .. 32767 sawtooth */

		*i = (t < 0 ? -t : t) * 2 - 32767;
		break;
	}
	case ESDR_WAVE_SAW:
		*i = (int32_t)(ph >> 16) - 32768;
		break;
	default:
		*i = 32767;
		break;
	}
}

/* Next @p n generator samples. */
static void generate(struct esp_sdr_iq16 *out, size_t n)
{
	struct esdr_tx_gen g = gen;
	/* Q14, so that a 16 bit sample times it fits 32 bits (amplitudes up to 2). */
	int32_t amp = (int32_t)(MIN(g.amp, 1.99f) * 16384.0f);
	int32_t dci = (int32_t)(g.dc_i * 32767.0f), dcq = (int32_t)(g.dc_q * 32767.0f);
	uint32_t inc = (uint32_t)(int32_t)(int64_t)(g.cycles * 4294967296.0f);

	for (size_t k = 0; k < n; k++) {
		int32_t i, q;

		wave_at(g.wave, phase, &i, &q);
		out[k].i = clip(((i * amp) >> 14) + dci);
		out[k].q = clip(((q * amp) >> 14) + dcq);
		phase += inc;
	}
}

/* Next samples of the buffer; fewer than @p n when it ends. */
static size_t from_buffer(struct esp_sdr_iq16 *out, size_t n)
{
	int32_t s = (int32_t)(MIN(scale, 1.99f) * 16384.0f);
	size_t k = 0;

	while (k < n) {
		if (job.pos >= job.len) {
			if (!job.repeat || job.len == 0U) {
				break;
			}
			job.pos = 0;
		}
		out[k].i = clip((job.buf[2U * job.pos] * s) >> 14);
		out[k].q = clip((job.buf[2U * job.pos + 1U] * s) >> 14);
		job.pos++;
		k++;
	}
	return k;
}

static void tx_run(void)
{
	struct esp_sdr_tx_dac_stats ds;
	uint32_t target = MIN(job.rate / 20U + CHUNK, CONFIG_ESP_SDR_TX_DAC_RING_SAMPLES / 2U);
	bool done = false;
	int ret;

	ret = esp_sdr_set_freq_offset(job.khz);
	if (ret == 0) {
		ret = esp_sdr_tx_set_gain(job.gain);
	}
	if (ret == 0) {
		ret = esp_sdr_tx_start((uint64_t)job.mhz * 1000000U, job.rate);
	}
	job.start_ret = ret;
	k_sem_give(&ready_sem);
	if (ret != 0) {
		stats.last_error = ret;
		return;
	}

	while (!atomic_get(&stop_req)) {
		if (done || esp_sdr_tx_dac_queued() >= target) {
			if (done && esp_sdr_tx_dac_queued() == 0U) {
				/* The last samples need a moment to reach the air. */
				k_sleep(K_MSEC(30));
				break;
			}
			k_sleep(K_MSEC(4));
			continue;
		}

		size_t n = CHUNK;

		if (job.count != 0U && job.count - job.sent < n) {
			n = (size_t)(job.count - job.sent);
		}
		if (job.buf != NULL) {
			n = from_buffer(chunk, n);
		} else {
			generate(chunk, n);
		}
		if (n == 0U || (job.count != 0U && job.sent + n >= job.count)) {
			done = true;
		}
		for (size_t off = 0; off < n && !atomic_get(&stop_req);) {
			ret = esp_sdr_tx_write(&chunk[off], n - off);
			if (ret < 0) {
				stats.last_error = ret;
				atomic_set(&stop_req, 1);
				break;
			}
			if (ret == 0) {
				k_sleep(K_MSEC(2));
			}
			off += (size_t)ret;
			job.sent += (uint64_t)ret;
		}
	}

	esp_sdr_tx_dac_get_stats(&ds);
	stats.fed = job.sent;
	stats.played = ds.played;
	stats.dropped = ds.dropped;
	stats.underruns = ds.underruns;
	stats.restarts = ds.restarts;
	stats.errors = ds.errors;
	stats.switches = ds.switches;
	stats.fill_us_max = ds.fill_us_max;
	stats.wake_late_us_max = ds.wake_late_us_max;
	stats.idle_us_max = ds.idle_us_max;
	stats.switch_us_max = ds.switch_us_max;
	stats.slack_us_min = ds.slack_us_min;
	stats.start_margin_us = ds.start_margin_us;
	stats.simd = ds.simd;
	(void)esp_sdr_tx_stop();
}

static void tx_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		(void)k_sem_take(&start_sem, K_FOREVER);
		tx_run();
		atomic_set(&busy, 0);
		k_sem_give(&done_sem);
	}
}

ESP_SDR_HIGH_STACK_DEFINE(esdr_tx_stack, 2048);
static struct k_thread tx_thread;

static void tables(void)
{
	static bool ready;

	if (ready) {
		return;
	}
	for (uint32_t k = 0; k <= SINE_N; k++) {
		sine_tab[k] =
			(int16_t)lroundf(32767.0f * sinf(ESP_SDR_TAU_F * (float)k / (float)SINE_N));
	}
	ready = true;
}

int esdr_tx_start(uint32_t mhz, int32_t khz, uint32_t rate_hz, int gain, const int16_t *buf,
		  size_t len, bool repeat, const struct esdr_tx_gen *g, float scale_in,
		  uint64_t count)
{
	static bool started;
	int ret = esdr_rx_init();

	if (ret != 0) {
		return ret;
	}
	if (esdr_rx_busy()) {
		return -EBUSY;
	}
	if (!atomic_cas(&busy, 0, 1)) {
		return -EBUSY;
	}
	tables();
	if (!started) {
		k_thread_create(&tx_thread, esdr_tx_stack, K_KERNEL_STACK_SIZEOF(esdr_tx_stack),
				tx_thread_fn, NULL, NULL, NULL, K_PRIO_COOP(3), K_FP_REGS, K_FOREVER);
		k_thread_name_set(&tx_thread, "esdr_tx");
#if defined(CONFIG_SCHED_CPU_MASK)
		if (arch_num_cpus() > 1) {
			k_thread_cpu_pin(&tx_thread, TX_CPU);
		}
#endif
		k_thread_start(&tx_thread);
		started = true;
	}
	job.mhz = mhz;
	job.khz = khz;
	job.rate = rate_hz;
	job.gain = gain;
	job.buf = buf;
	job.len = len;
	job.pos = 0;
	job.repeat = repeat;
	job.count = count;
	job.sent = 0;
	if (g != NULL) {
		gen = *g;
	}
	scale = scale_in;
	phase = 0;
	memset(&stats, 0, sizeof(stats));
	atomic_set(&stop_req, 0);
	k_sem_reset(&ready_sem);
	k_sem_reset(&done_sem);
	k_sem_give(&start_sem);
	k_sem_take(&ready_sem, K_FOREVER);
	if (job.start_ret != 0) {
		/* The thread has ended the run. */
		(void)k_sem_take(&done_sem, K_FOREVER);
		return job.start_ret;
	}
	return 0;
}

void esdr_tx_set_gen(const struct esdr_tx_gen *g)
{
	gen = *g;
}

void esdr_tx_set_scale(float s)
{
	scale = s;
}

void esdr_tx_stop(void)
{
	if (atomic_get(&busy)) {
		atomic_set(&stop_req, 1);
	}
}

bool esdr_tx_busy(void)
{
	return atomic_get(&busy) != 0;
}

bool esdr_tx_wait(int32_t timeout_ms)
{
	if (!atomic_get(&busy)) {
		return true;
	}
	if (k_sem_take(&done_sem, timeout_ms < 0 ? K_FOREVER : K_MSEC(timeout_ms)) == 0) {
		k_sem_give(&done_sem);
		return true;
	}
	return !atomic_get(&busy);
}

void esdr_tx_get_stats(struct esdr_tx_stats *out)
{
	*out = stats;
	/* A loop does not go through the streaming backend: its counters are another session's. */
	if (atomic_get(&busy) && !loop_on) {
		struct esp_sdr_tx_dac_stats ds;

		esp_sdr_tx_dac_get_stats(&ds);
		out->fed = job.sent;
		out->played = ds.played;
		out->dropped = ds.dropped;
		out->underruns = ds.underruns;
		out->restarts = ds.restarts;
		out->errors = ds.errors;
		out->switches = ds.switches;
		out->fill_us_max = ds.fill_us_max;
		out->wake_late_us_max = ds.wake_late_us_max;
		out->idle_us_max = ds.idle_us_max;
		out->switch_us_max = ds.switch_us_max;
		out->slack_us_min = ds.slack_us_min;
		out->start_margin_us = ds.start_margin_us;
		out->simd = ds.simd;
	}
	out->queued = esp_sdr_tx_dac_queued();
}


/* ---- loop playback at the DAC rate ---- */

static int loop_bank;
static const int16_t *loop_src;
static size_t loop_len;
static uint32_t loop_interp;

void esdr_tx_period(float cycles, int32_t *k_out, uint32_t *n_out)
{
	/* Best k / n with n <= ESP_SDR_SAMPLES_MAX: continued fraction convergents of |cycles|,
	 * then the best semiconvergent below the limit. */
	const int64_t nmax = ESP_SDR_SAMPLES_MAX;
	double x = fabs((double)cycles), target = x;
	int64_t h0 = 0, h1 = 1, k0 = 1, k1 = 0;
	int64_t bh = 0, bk = 1;

	for (int it = 0; it < 40; it++) {
		int64_t a = (int64_t)floor(x);
		int64_t h2 = a * h1 + h0, k2 = a * k1 + k0;

		if (k2 > nmax) {
			int64_t t = (nmax - k0) / k1;
			int64_t sh = t * h1 + h0, sk = t * k1 + k0;

			if (t > 0 && fabs((double)sh / (double)sk - target) <
					     fabs((double)bh / (double)bk - target)) {
				bh = sh;
				bk = sk;
			}
			break;
		}
		h0 = h1;
		k0 = k1;
		h1 = h2;
		k1 = k2;
		bh = h1;
		bk = k1;
		double frac = x - (double)a;

		if (frac < 1e-12) {
			break;
		}
		x = 1.0 / frac;
	}
	/* At least ESP_SDR_SAMPLES_MIN samples, a multiple of 4: whole repeats of the period. */
	int64_t m = 1;

	while (bk * m < ESP_SDR_SAMPLES_MIN || (bk * m) % 4 != 0) {
		m++;
	}
	if (bk * m > nmax) {
		m = 1;
		while (bk * m < ESP_SDR_SAMPLES_MIN) {
			m++;
		}
	}
	*k_out = (int32_t)(cycles < 0 ? -bh * m : bh * m);
	*n_out = (uint32_t)(bk * m);
}

/* One period of the generator: @p k whole cycles in @p n samples, exact phases. */
static void loop_fill_gen(uint32_t *w, uint32_t n, int32_t k)
{
	struct esdr_tx_gen g = gen;
	int32_t amp = (int32_t)(MIN(g.amp * scale, 1.99f) * 16384.0f);
	int32_t dci = (int32_t)(g.dc_i * 32767.0f), dcq = (int32_t)(g.dc_q * 32767.0f);
	/* The phase step k / n turns, as 2^32 * k / n = step + rest / n, accumulated exactly. */
	uint64_t turn = ((uint64_t)((int64_t)k % (int64_t)n + (int64_t)n) % n) << 32;
	uint32_t step = (uint32_t)(turn / n), rest = (uint32_t)(turn % n);
	uint32_t ph = 0, acc = 0;

	for (uint32_t j = 0; j < n; j++) {
		int32_t i, q;

		wave_at(g.wave, ph, &i, &q);
		w[j] = esp_sdr_tx_word(clip(((i * amp) >> 14) + dci) >> 6,
				       clip(((q * amp) >> 14) + dcq) >> 6);
		ph += step;
		acc += rest;
		if (acc >= n) {
			acc -= n;
			ph++;
		}
	}
}

/* The buffer interpolated linearly by loop_interp, cyclically. */
static uint32_t loop_fill_buf(uint32_t *w)
{
	int32_t s = (int32_t)(MIN(scale, 1.99f) * 16384.0f);
	uint32_t L = loop_interp, o = 0;

	for (size_t j = 0; j < loop_len; j++) {
		size_t j1 = j + 1U == loop_len ? 0U : j + 1U;
		int32_t i0 = (loop_src[2U * j] * s) >> 14, q0 = (loop_src[2U * j + 1U] * s) >> 14;
		int32_t i1 = (loop_src[2U * j1] * s) >> 14, q1 = (loop_src[2U * j1 + 1U] * s) >> 14;

		for (uint32_t t = 0; t < L; t++) {
			w[o++] = esp_sdr_tx_word(clip(i0 + (i1 - i0) * (int32_t)t / (int32_t)L) >> 6,
						 clip(q0 + (q1 - q0) * (int32_t)t / (int32_t)L) >> 6);
		}
	}
	return o;
}

static int loop_play(int bank)
{
	size_t words;
	uint32_t *w = esp_sdr_tx_loop_buf(bank, &words);
	uint32_t n;

	if (w == NULL) {
		return -EINVAL;
	}
	if (loop_src != NULL) {
		n = loop_fill_buf(w);
	} else {
		int32_t k;

		esdr_tx_period(gen.cycles, &k, &n);
		loop_fill_gen(w, n, k);
	}
	return esp_sdr_tx_loop_start(bank, n);
}

int esdr_tx_loop_start(uint32_t mhz, int32_t khz, uint32_t fs, int gain, const int16_t *buf,
		       size_t len, uint32_t interp, const struct esdr_tx_gen *g, float scale_in)
{
	enum esp_sdr_rate rate = fs == 80000000U ? ESP_SDR_RATE_80MSPS : ESP_SDR_RATE_40MSPS;
	int ret = esdr_rx_init();

	if (ret != 0) {
		return ret;
	}
	if (esdr_rx_busy() || !atomic_cas(&busy, 0, 1)) {
		return -EBUSY;
	}
	if (buf != NULL && (len == 0U || interp == 0U || len * interp < ESP_SDR_SAMPLES_MIN ||
			    len * interp > ESP_SDR_SAMPLES_MAX)) {
		atomic_set(&busy, 0);
		return -EINVAL;
	}
	tables();
	loop_src = buf;
	loop_len = len;
	loop_interp = interp;
	if (g != NULL) {
		gen = *g;
	}
	scale = scale_in;
	memset(&stats, 0, sizeof(stats));
	k_sem_reset(&done_sem);
	ret = esp_sdr_set_freq(mhz);
	if (ret == 0) {
		ret = esp_sdr_set_freq_offset(khz);
	}
	if (ret == 0) {
		ret = esp_sdr_tx_set_gain(gain);
	}
	if (ret == 0) {
		ret = esp_sdr_tx_loop_begin(rate);
	}
	if (ret != 0) {
		atomic_set(&busy, 0);
		return ret;
	}
	loop_on = true;
	loop_bank = 0;
	ret = loop_play(0);
	if (ret != 0) {
		esdr_tx_loop_stop();
	}
	return ret;
}

bool esdr_tx_looping(void)
{
	return loop_on;
}

int esdr_tx_loop_update(void)
{
	if (!loop_on) {
		return -ENOTCONN;
	}
	/* The idle bank, then playback moves over to it. */
	int b = ESP_SDR_BANKS > 1 ? 1 - loop_bank : 0;

	if (ESP_SDR_BANKS == 1) {
		esp_sdr_tx_loop_halt();
	}
	loop_bank = b;
	return loop_play(b);
}

void esdr_tx_loop_stop(void)
{
	if (!loop_on) {
		return;
	}
	esp_sdr_tx_loop_end();
	loop_on = false;
	loop_src = NULL;
	atomic_set(&busy, 0);
	k_sem_give(&done_sem);
}
