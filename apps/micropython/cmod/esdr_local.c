/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The radio on the chip behind the interface of esdr_remote.h, for the classic ESP32 (the
 * MCH2022 badge), which captures bursts only (lib/esp_sdr). The native rates (80, 40 and
 * 16 MS/s) come in bursts as captured; the stream rates (2 MS/s to 125 kS/s) are 16 MS/s bursts
 * through the library's CIC decimator, one after another, contiguous within a burst (about
 * 1 ms) with a gap between bursts that rx_stats counts: good for displays, not for
 * demodulators. Triggered snapshots come from 80 MS/s bursts taken every few milliseconds: one
 * whose power, in windows of 64 samples, rises past the threshold over the quiet power (the
 * lower quartile of the windows, averaged) is a snapshot, the trigger at that window; there is
 * nothing after a burst's end, so post is not honoured. Requests take the subset of the osmosdr protocol (esdr_proto.h) that
 * esdr_radio.c sends. Samples are int16 at full scale 16384, as the S3's ring gives them.
 * Plain Zephyr code, built as an app source.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_rx.h>

#include "esdr_remote.h"
#include "esdr_rx.h"

/* The stream rates: the top one and halvings, 16 MS/s decimated by 8 to 128. */
#define STREAM_IN_HZ    16000000U
#define STREAM_MAX_HZ   2000000U
#define STREAM_RATES    5U
/* 10-bit samples to full scale 16384, and the decimator's 32767 to it. */
#define NATIVE_SHIFT    5
#define DECIM_SHIFT     1
/* The most a decimated burst gives (16380 / 8). */
#define DECIM_OUT_MAX   (ESP_SDR_SAMPLES_MAX / (STREAM_IN_HZ / STREAM_MAX_HZ))

#define WORKER_STACK 2048
/* Ahead of MicroPython (the main thread): a burst at a time, then MicroPython runs. */
#define WORKER_PRIO  K_PRIO_COOP(4)

static struct esdr_info info;
static bool ready;

/* The settings as requested and applied. */
static struct {
	uint64_t freq_hz, center_hz;
	uint32_t lo_mhz, bandwidth_hz;
	int32_t lo_khz;
	uint16_t rx_gain;
} set = {.rx_gain = ESDR_GAIN_AUTO};

/* The receive run; the worker fills buffers under rx_lock, start and stop change them under it. */
static K_MUTEX_DEFINE(rx_lock);
static struct {
	int16_t *buf;
	size_t cap;
	uint64_t count;
	bool wrap;
	volatile bool active;
	/* Native rate (enum esp_sdr_rate) of a burst run, or -1: a stream at 16 MS/s / decim. */
	int native;
	unsigned int decim;
	/* When the last burst ended (ns), for the gap. */
	uint64_t last_end_ns;
	bool have_burst;
	struct esdr_remote_rx_stats st;
} rx;

/* The monitor: the newest ESDR_MON_SAMPLES pairs, and how many came in all. */
static uint32_t mon[ESDR_MON_SAMPLES] __attribute__((section(".ext_ram.bss")));
static volatile uint64_t mon_total;
/* A display-only run captures again once the monitor has been read. */
static volatile bool mon_read;

/* A burst's samples, converted. */
static struct esp_sdr_iq16 burst[ESP_SDR_SAMPLES_MAX] __attribute__((section(".ext_ram.bss")));

static K_SEM_DEFINE(rx_go, 0, 1);
static volatile bool worker_busy;

/* ---- triggered snapshots ---- */

#define TRIG_WIN       64U
#define TRIG_WINS      (ESDR_TRIG_SAMPLES / TRIG_WIN)
/* Between bursts: MicroPython's turn (a burst and its windows take about 1 ms). */
#define TRIG_PERIOD_MS 5

static struct {
	volatile bool on;
	uint32_t ratio_x16;
	/* Quiet window power, 16 times its average (0: not learnt yet). */
	uint32_t base16;
	uint32_t seq, trig;
	struct esdr_remote_trig_stats st;
} trg;

static int8_t trg_snap[2U * ESDR_TRIG_SAMPLES] __attribute__((section(".ext_ram.bss")));
static K_MUTEX_DEFINE(trg_lock);

static int cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

	return (x > y) - (x < y);
}

/* One 80 MS/s burst; a snapshot if a window passes the threshold. */
static void trig_burst(void)
{
	static uint32_t pw[TRIG_WINS], sorted[TRIG_WINS];
	struct esp_sdr_rx_burst b;
	uint32_t base, w;

	if (esp_sdr_rx_capture(ESP_SDR_RATE_80MSPS, ESDR_TRIG_SAMPLES, &b) != 0) {
		trg.st.broken++;
		return;
	}
	for (w = 0; w < TRIG_WINS; w++) {
		uint32_t sum = 0;

		for (uint32_t k = w * TRIG_WIN; k < (w + 1U) * TRIG_WIN; k++) {
			int32_t i = esp_sdr_rx_i(b.words[k]), q = esp_sdr_rx_q(b.words[k]);

			sum += (uint32_t)(i * i + q * q);
		}
		pw[w] = sum;
	}
	memcpy(sorted, pw, sizeof(pw));
	qsort(sorted, TRIG_WINS, sizeof(sorted[0]), cmp_u32);
	base = MAX(sorted[TRIG_WINS / 4U], 1U);
	trg.base16 = trg.base16 == 0U ? 16U * base : trg.base16 - trg.base16 / 16U + base;
	for (w = 0; w < TRIG_WINS; w++) {
		if ((uint64_t)pw[w] * 256U > (uint64_t)trg.ratio_x16 * trg.base16) {
			break;
		}
	}
	if (w == TRIG_WINS) {
		return;
	}
	k_mutex_lock(&trg_lock, K_FOREVER);
	for (uint32_t k = 0; k < ESDR_TRIG_SAMPLES; k++) {
		trg_snap[2U * k] = (int8_t)(esp_sdr_rx_i(b.words[k]) >> 2);
		trg_snap[2U * k + 1U] = (int8_t)(esp_sdr_rx_q(b.words[k]) >> 2);
	}
	trg.trig = w * TRIG_WIN;
	trg.seq++;
	trg.st.snapshots++;
	k_mutex_unlock(&trg_lock);
}

/* ---- requests ---- */

static int tune(uint64_t hz)
{
	uint32_t mhz = (uint32_t)((hz + 500000U) / 1000000U);
	int64_t rest = (int64_t)hz - (int64_t)mhz * 1000000;
	int32_t khz = (int32_t)((rest + (rest >= 0 ? 500 : -500)) / 1000);
	int ret;

	ret = esp_sdr_set_freq(mhz);
	if (ret == 0) {
		ret = esp_sdr_set_freq_offset(khz);
	}
	if (ret != 0) {
		return ret;
	}
	set.freq_hz = hz;
	set.lo_mhz = mhz;
	set.lo_khz = khz;
	set.center_hz = (uint64_t)mhz * 1000000U + (int64_t)khz * 1000;
	return 0;
}

static int set_bandwidth(uint32_t hz)
{
	uint32_t min, max;

	set.bandwidth_hz = hz;
	if (hz == 0U) {
		return esp_sdr_rx_set_lpf(ESP_SDR_RX_LPF_AUTO);
	}
	/* Narrower or wider than the filter goes: its narrowest or widest setting. */
	esp_sdr_rx_bandwidth_range(&min, &max);
	return esp_sdr_rx_set_bandwidth(CLAMP(hz / 1000000U, min, max));
}

static uint32_t stream_rate(void)
{
	return rx.native >= 0 ? esp_sdr_rx_rate_hz((enum esp_sdr_rate)rx.native)
			      : STREAM_IN_HZ / rx.decim;
}

int esdr_remote_request(uint8_t req, uint16_t value, const void *data, uint16_t len,
			void *reply, uint16_t reply_max)
{
	struct esdr_state st;

	switch (req) {
	case ESDR_REQ_SET_FREQ:
		if (len != 8U) {
			return -EINVAL;
		}
		return tune(sys_get_le64(data));
	case ESDR_REQ_SET_RX_GAIN:
		if (esp_sdr_rx_set_gain(value == ESDR_GAIN_AUTO ? ESP_SDR_RX_GAIN_AUTO
								  : (int)value) != 0) {
			return -EINVAL;
		}
		set.rx_gain = value;
		return 0;
	case ESDR_REQ_SET_BANDWIDTH:
		if (len != 4U) {
			return -EINVAL;
		}
		return set_bandwidth(sys_get_le32(data));
	case ESDR_REQ_GET_STATE:
		st = (struct esdr_state){
			.mode = rx.active ? ESDR_MODE_RX : ESDR_MODE_OFF,
			.bits = 16,
			.rx_gain = set.rx_gain,
			.freq_hz = set.freq_hz,
			.center_hz = set.center_hz,
			.sample_rate_hz = rx.active ? stream_rate() : 0U,
			.bandwidth_hz = set.bandwidth_hz,
			.lo_mhz = set.lo_mhz,
			.lo_offset_khz = set.lo_khz,
		};
		memcpy(reply, &st, MIN(reply_max, sizeof(st)));
		return MIN(reply_max, sizeof(st));
	default:
		return -ENOTSUP;
	}
}

/* ---- receive ---- */

/* @p n pairs into the monitor (the newest of them) and the run's buffer. */
static void rx_store_block(const struct esp_sdr_iq16 *iq, uint32_t n)
{
	uint32_t skip = n > ESDR_MON_SAMPLES ? n - ESDR_MON_SAMPLES : 0U;
	uint32_t take = n;

	for (uint32_t k = skip; k < n;) {
		uint32_t at = (uint32_t)((mon_total + k) % ESDR_MON_SAMPLES);
		uint32_t m = MIN(n - k, ESDR_MON_SAMPLES - at);

		memcpy(&mon[at], &iq[k], m * sizeof(*iq));
		k += m;
	}
	mon_total += n;

	if (rx.count != 0U) {
		take = (uint32_t)MIN((uint64_t)take,
				     rx.count > rx.st.samples ? rx.count - rx.st.samples : 0U);
	}
	if (rx.buf == NULL) {
		rx.st.samples += take;
		return;
	}
	for (uint32_t k = 0; k < take;) {
		uint64_t at = rx.wrap ? rx.st.samples % rx.cap : rx.st.samples;
		uint32_t m;

		if (at >= rx.cap) {
			return;
		}
		m = (uint32_t)MIN((uint64_t)(take - k), rx.cap - at);
		memcpy(&rx.buf[2U * at], &iq[k], m * sizeof(*iq));
		rx.st.samples += m;
		k += m;
	}
}

/* One burst of the run into its buffer; false once the run has all it wants. */
static bool rx_burst(void)
{
	uint32_t rate = stream_rate();
	uint64_t first_ns;
	size_t n;
	int ret;

	if (rx.native >= 0) {
		n = ESP_SDR_SAMPLES_MAX;
		ret = esp_sdr_rx_capture_iq((enum esp_sdr_rate)rx.native, n, burst, &first_ns);
		for (size_t k = 0; ret == 0 && k < n; k++) {
			burst[k].i = (int16_t)(burst[k].i << NATIVE_SHIFT);
			burst[k].q = (int16_t)(burst[k].q << NATIVE_SHIFT);
		}
	} else {
		ret = esp_sdr_rx_capture_decimated(ESP_SDR_RATE_16MSPS, ESP_SDR_SAMPLES_MAX,
						   rx.decim, burst, DECIM_OUT_MAX, &n, &first_ns);
		for (size_t k = 0; ret == 0 && k < n; k++) {
			burst[k].i >>= DECIM_SHIFT;
			burst[k].q >>= DECIM_SHIFT;
		}
	}
	if (ret != 0) {
		rx.st.overflows++;
		return true;
	}

	k_mutex_lock(&rx_lock, K_FOREVER);
	if (rx.active) {
		if (rx.have_burst && first_ns > rx.last_end_ns) {
			rx.st.gaps++;
			rx.st.gap_samples += (first_ns - rx.last_end_ns) * rate / NSEC_PER_SEC;
		}
		rx.have_burst = true;
		rx.last_end_ns = first_ns + (uint64_t)n * NSEC_PER_SEC / rate;
		rx_store_block(burst, (uint32_t)n);
	}
	bool more = rx.active && (rx.count == 0U || rx.st.samples < rx.count) &&
		    (rx.buf == NULL || rx.wrap || rx.st.samples < rx.cap);
	k_mutex_unlock(&rx_lock);
	return more;
}

static void worker_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		(void)k_sem_take(&rx_go, K_FOREVER);
		while (trg.on) {
			trig_burst();
			k_sleep(K_MSEC(TRIG_PERIOD_MS));
		}
		while (rx.active) {
			/* A display-only run: a burst per monitor read, no more. */
			if (rx.buf == NULL && rx.count == 0U && !mon_read) {
				k_sleep(K_MSEC(2));
				continue;
			}
			mon_read = false;
			if (!rx_burst()) {
				rx.active = false;
				break;
			}
			/* MicroPython runs between bursts. */
			k_sleep(K_TICKS(1));
		}
		worker_busy = false;
	}
}

K_THREAD_STACK_DEFINE(worker_stack, WORKER_STACK);
static struct k_thread worker_thread;

uint64_t esdr_rx_latest(int16_t *dst, size_t n)
{
	uint32_t *out = (uint32_t *)dst;
	uint64_t total = mon_total;
	size_t have = (size_t)MIN(total, (uint64_t)ESDR_MON_SAMPLES);
	size_t lead = 0;

	n = MIN(n, (size_t)ESDR_MON_SAMPLES);
	if (have < n) {
		lead = n - have;
		memset(out, 0, lead * sizeof(*out));
	}
	for (size_t k = lead; k < n; k++) {
		out[k] = mon[(total - (n - k)) % ESDR_MON_SAMPLES];
	}
	mon_read = true;
	return total;
}

static int rx_start(int native, unsigned int decim, int16_t *buf, size_t cap, uint64_t count,
		    bool wrap)
{
	esdr_remote_trig_stop();
	esdr_remote_rx_stop();
	k_mutex_lock(&rx_lock, K_FOREVER);
	rx.buf = buf;
	rx.cap = cap;
	rx.count = count;
	rx.wrap = wrap;
	rx.native = native;
	rx.decim = decim;
	rx.have_burst = false;
	memset(&rx.st, 0, sizeof(rx.st));
	mon_read = true;
	worker_busy = true;
	rx.active = true;
	k_mutex_unlock(&rx_lock);
	k_sem_give(&rx_go);
	return 0;
}

int esdr_remote_rx_start(uint32_t rate, int16_t *buf, size_t cap, uint64_t count, bool wrap)
{
	for (unsigned int k = 0; k < STREAM_RATES; k++) {
		if (rate == STREAM_MAX_HZ >> k) {
			return rx_start(-1, STREAM_IN_HZ / rate, buf, cap, count, wrap);
		}
	}
	return -EINVAL;
}

int esdr_remote_rx_start_burst(uint16_t mhz, unsigned int decim, int16_t *buf, size_t cap,
			       uint64_t count)
{
	static const enum esp_sdr_rate natives[] = {
		ESP_SDR_RATE_80MSPS, ESP_SDR_RATE_40MSPS, ESP_SDR_RATE_16MSPS,
	};

	for (size_t k = 0; k < ARRAY_SIZE(natives); k++) {
		if (decim == 1U && (uint32_t)mhz * 1000000U == esp_sdr_rx_rate_hz(natives[k])) {
			return rx_start(natives[k], 1U, buf, cap, count, false);
		}
	}
	return -EINVAL;
}

void esdr_remote_rx_stop(void)
{
	/* Once the worker is out of the burst it was storing, nothing writes into the buffer. */
	k_mutex_lock(&rx_lock, K_FOREVER);
	rx.active = false;
	rx.buf = NULL;
	k_mutex_unlock(&rx_lock);
	while (worker_busy) {
		k_msleep(1);
	}
}

bool esdr_remote_rx_busy(void)
{
	return rx.active;
}

uint64_t esdr_remote_rx_count(void)
{
	return rx.st.samples;
}

void esdr_remote_rx_stats(struct esdr_remote_rx_stats *st)
{
	*st = rx.st;
}

int esdr_remote_trig_start(uint16_t ratio_x16, uint32_t post)
{
	ARG_UNUSED(post);
	esdr_remote_rx_stop();
	esdr_remote_trig_stop();
	k_mutex_lock(&trg_lock, K_FOREVER);
	trg.ratio_x16 = ratio_x16;
	trg.base16 = 0;
	k_mutex_unlock(&trg_lock);
	worker_busy = true;
	trg.on = true;
	k_sem_give(&rx_go);
	return 0;
}

void esdr_remote_trig_stop(void)
{
	trg.on = false;
	while (worker_busy) {
		k_msleep(1);
	}
}

bool esdr_remote_trig_get(int8_t *dst, uint32_t after, uint32_t *seq, uint32_t *trig)
{
	bool fresh;

	k_mutex_lock(&trg_lock, K_FOREVER);
	fresh = trg.seq != 0U && trg.seq != after;
	if (fresh) {
		memcpy(dst, trg_snap, sizeof(trg_snap));
		*seq = trg.seq;
		*trig = trg.trig;
	}
	k_mutex_unlock(&trg_lock);
	return fresh;
}

void esdr_remote_trig_stats(struct esdr_remote_trig_stats *st)
{
	*st = trg.st;
}

/* ---- setup ---- */

int esdr_remote_init(const struct esdr_info **out)
{
	int ret;

	if (!ready) {
		ret = esp_sdr_init();
		if (ret != 0) {
			return ret;
		}
		info = (struct esdr_info){
			.magic = ESDR_INFO_MAGIC,
			.proto_version = ESDR_PROTO_VERSION,
			.info_size = sizeof(info),
			.freq_min_hz = (uint64_t)ESP_SDR_FREQ_MIN_MHZ * 1000000U,
			.freq_max_hz = (uint64_t)ESP_SDR_FREQ_MAX_MHZ * 1000000U,
			.rx_rate_max_hz = STREAM_MAX_HZ,
			.rx_rate_count = STREAM_RATES,
			.rx_gain_max = (uint16_t)esp_sdr_rx_gain_max(),
			.full_scale16 = 16384,
		};
		strncpy(info.firmware, "esp-sdr esp32 on chip", sizeof(info.firmware) - 1);
		set.freq_hz = (uint64_t)esp_sdr_get_freq() * 1000000U;
		set.center_hz = set.freq_hz;
		set.lo_mhz = esp_sdr_get_freq();
		k_thread_create(&worker_thread, worker_stack, K_THREAD_STACK_SIZEOF(worker_stack),
				worker_fn, NULL, NULL, NULL, WORKER_PRIO, 0, K_NO_WAIT);
		k_thread_name_set(&worker_thread, "esdr_rx");
		ready = true;
	}
	if (out != NULL) {
		*out = &info;
	}
	return 0;
}

void esdr_remote_reset(void)
{
	esdr_remote_trig_stop();
	esdr_remote_rx_stop();
	(void)esp_sdr_reset_settings();
	set.rx_gain = ESDR_GAIN_AUTO;
	set.bandwidth_hz = 0;
	set.lo_khz = 0;
	set.lo_mhz = esp_sdr_get_freq();
	set.freq_hz = (uint64_t)set.lo_mhz * 1000000U;
	set.center_hz = set.freq_hz;
}
