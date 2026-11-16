/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The radio on the chip behind the interface of esdr_remote.h, for the classic ESP32 (the
 * MCH2022 badge), which captures bursts only (lib/esp_sdr), and the ESP32-S31 next to a display
 * (its gapless ring needs pads the panel has). The native rates (80, 40 and
 * 16 MS/s, on the S31 80, 40 and 20) come in bursts as captured; the stream rates (esdr_rates.h: 2
 * MS/s, on the S31 5, to 125 kS/s) are 16 MS/s bursts through the library's CIC decimator (the S31:
 * 20 MS/s bursts with the LO 5 MHz below the centre, through the decimator here, dec_*), one after
 * another, contiguous within a
 * burst (about 1 ms) with a gap between bursts that rx_stats counts: good for displays, not for
 * demodulators. Triggered snapshots come from 80 MS/s bursts taken every few milliseconds: one
 * whose power, in windows of 64 samples, rises past the threshold over the quiet power (the
 * lower quartile of the windows, averaged) is a snapshot, the trigger at that window; there is
 * nothing after a burst's end, so post is not honoured. Transmit is a carrier from the PHY's
 * tone generator (as the vendor's CW test, wifiscwout): a generated tone or constant at the LO,
 * the tone's offset added to the LO; the S31 also plays a buffer once, blocking, through the
 * CPU-paced TX DC path (below). Requests take the subset of the osmosdr protocol (esdr_proto.h)
 * that esdr_radio.c sends. Samples are int16 at full scale 16384, as the S3's ring gives them.
 * Plain Zephyr code, built as an app source.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef CONFIG_DISPLAY_ESP32_LCD_RGB
#include <zephyr/drivers/display/esp32_lcd_rgb.h>
#endif
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_place.h>
#include <esp_sdr/esp_sdr_rx.h>
#include <esp_sdr/esp_sdr_tx.h>

#include "esdr_cal.h"
#include "esdr_defs.h"
#include "esdr_mon.h"
#include "esdr_rates.h"
#include "esdr_remote.h"
#include "esdr_rx.h"

#if defined(ESDR_LOCAL_TX_BURSTS)
#include <math.h>

#include <esp_attr.h>
#include <esp_sdr/esp_sdr_dsp.h>
#include <esp_sdr/esp_sdr_paced.h>
#endif
#if defined(CONFIG_ESP_SDR_DUMPRING) && CONFIG_MP_MAX_NUM_CPUS > 1
/* Buffered streams without gaps through the dump ring, on the worker's own CPU (below). */
#define GAPLESS 1
#include <esp_sdr/esp_sdr_dumpring.h>
#if defined(CONFIG_ESP_SDR_RING_STAGE)
/* A gapless stream into the ring stage, for the Pcap app's engine (esdr_pcap.c). */
#define GL_STAGE 1
#include <esp_sdr/esp_sdr_ring_stage.h>
#endif
#endif
#if defined(ESDR_LOCAL_TX_BURSTS) && defined(CONFIG_ESP_SDR_REPLAY)
/* The S31 loops a buffer through its DAC engine (tx_loop() below). */
#define TX_LOOPS 1
#include <esp_sdr/esp_sdr_replay.h>
#endif

/*
 * The stream rates: the top one and halvings, 16 MS/s decimated by 8 to 128, or 20 MS/s by 10 to
 * 160 on the S31, which has no 16 MS/s.
 */
#if defined(CONFIG_SOC_SERIES_ESP32S31)
#define STREAM_IN_RATE  ESP_SDR_RATE_20MSPS
#define STREAM_IN_HZ    20000000U
#define NATIVE_THIRD    ESP_SDR_RATE_20MSPS
/* The stream sits a quarter of the input rate above the LO: see the decimator, dec_*. */
#define STREAM_LO_OFFSET_HZ (STREAM_IN_HZ / 4U)
/*
 * Only the analog filter stands before the bursts' rate: what it passes at the input rate's distance
 * from the stream lands in the band at its full level. The narrowest setting is within 2 dB to 6 MHz
 * from the LO, which holds the band, and over 35 dB down at LO - 15 MHz, where this rate's image is. At
 * 8 MS/s the image would be at LO - 6 MHz, inside the filter whatever its setting.
 */
#define STREAM_LPF_MHZ  13U
/* A burst's filter is as wide as its rate: the PHY's own stops 11 MHz from the LO, in mid-picture. */
#define BURST_LPF_MHZ(rate_mhz) (rate_mhz)
/*
 * A monitor read of more than a burst: up to this many bursts after each read, so that the next read
 * is new as far as they reach: 736 pairs at 125 kS/s, 1552 at 250 kS/s, 3192 at 500 kS/s. A larger FFT
 * repeats its oldest pairs, unless another sink's read starts a second count in between.
 */
#define MON_BURSTS      8U
#define LO_MIN_MHZ      2200U
#define LO_MAX_MHZ      2900U
#else
#define STREAM_IN_RATE  ESP_SDR_RATE_16MSPS
#define STREAM_IN_HZ    16000000U
#define NATIVE_THIRD    ESP_SDR_RATE_16MSPS
#define STREAM_LO_OFFSET_HZ 0U
#define STREAM_LPF_MHZ  0U
#define BURST_LPF_MHZ(rate_mhz) 0U
#define MON_BURSTS      1U
#define LO_MIN_MHZ      ESP_SDR_FREQ_MIN_MHZ
#define LO_MAX_MHZ      ESP_SDR_FREQ_MAX_MHZ
#endif
/* 10-bit samples to full scale 16384, and the decimator's 32767 to it. */
#define NATIVE_SHIFT    5
#define DECIM_SHIFT     1
/* The most the library's CIC gives a burst, at the fastest of esdr_rates.h (ESP32: 2 MS/s). */
#define DECIM_OUT_MAX   (ESP_SDR_SAMPLES_MAX / 8U)

#define WORKER_STACK 2048
/*
 * Ahead of MicroPython (the main thread): a burst at a time, then MicroPython runs. With two cores
 * the worker takes the second first (pinned at the start; the Scan app's spectra wait below it)
 * and takes its bursts back to back:
 * each capture parks the other core meanwhile (0.82 ms of some 2.7 at 2 MS/s), the radio first.
 */
#define WORKER_PRIO  K_PRIO_COOP(4)
#if defined(CONFIG_SCHED_CPU_MASK) && CONFIG_MP_MAX_NUM_CPUS > 1
#define WORKER_OWN_CPU 1
#endif

static struct esdr_info info;
static bool ready;

/* The settings as requested and applied. */
static struct {
	uint64_t freq_hz, center_hz;
	uint32_t lo_mhz, bandwidth_hz;
	int32_t lo_khz;
	/* The stream's centre over the LO (negative: below it): only the stream rates have one. */
	int32_t lo_offset_hz;
	/* The filter's setting for this kind of run at bandwidth 0; 0 leaves it to the PHY. */
	uint32_t lpf_auto_mhz;
	uint16_t rx_gain;
} set = {.rx_gain = ESDR_GAIN_AUTO};

/*
 * Transmit, a carrier (below): power steps 0 (weakest) to TX_POWER_MAX. The ESP32's attenuation
 * per step and at the top are not measured. The S31's is in quarter dB, from its vendor test's
 * least, 24, to the generator's most, 127: 1.5 dB a step (another board heard 1.9 dB).
 */
#define TX_POWER_MAX 15U
#if defined(CONFIG_SOC_SERIES_ESP32S31)
#define TX_ATT_STEP  6
#define TX_ATT_TOP   24
#else
#define TX_ATT_STEP  8
#define TX_ATT_TOP   0
#endif

static struct {
	volatile bool on;
	/* A buffer looping through the DAC engine, not the carrier (TX_LOOPS). */
	bool loop;
	uint32_t rate;
	int64_t offset_hz;
	uint64_t count;
	int64_t t0_ms;
	uint16_t power;
} tx;

/* The receive run; the worker fills buffers under rx_lock, start and stop change them under it. */
static K_MUTEX_DEFINE(rx_lock);
static struct {
	int16_t *buf;
	size_t cap;
	uint64_t count;
	bool wrap;
	volatile bool active;
	/* Native rate (enum esp_sdr_rate) of a burst run, or -1: a stream, STREAM_IN_HZ / decim. */
	int native;
	unsigned int decim;
	/* When the last burst ended (ns), for the gap. */
	uint64_t last_end_ns;
	bool have_burst;
	/* The run goes through the dump ring (GAPLESS), not bursts; its output into the ring stage
	 * (GL_STAGE), not buf. */
	bool gapless, stage;
	struct esdr_remote_rx_stats st;
} rx;

#if ESDR_PANEL_BLANK
volatile bool esdr_panel_quiet = true;
/* Longer than a frame, 17 ms at 58.5 Hz. */
#define PANEL_WAIT      K_MSEC(40)
/*
 * Lines at the multiples of the PSRAM's bus clock (at 200 MHz: 2200 to 2800 MHz in steps of 200)
 * drop while the panel's DMA is idle: the bus under the panel's reads (doc/hardware-quirks.md).
 */
#define PANEL_PSRAM_HZ  ((uint64_t)CONFIG_SPIRAM_SPEED * 1000000ULL)
/* Past the band's edge: the decimator's transition and the line's skirt. */
#define PANEL_MARGIN_HZ 200000U

static const struct device *const panel =
	DEVICE_DT_GET(DT_COMPAT_GET_ANY_STATUS_OKAY(espressif_esp32_lcd_cam_rgb));

/* A multiple of the PSRAM's clock within @p half_hz of @p hz. */
static bool panel_line_at(uint64_t hz, uint32_t half_hz)
{
	uint64_t near = (hz + PANEL_PSRAM_HZ / 2U) / PANEL_PSRAM_HZ * PANEL_PSRAM_HZ;

	return (hz > near ? hz - near : near - hz) <= half_hz;
}

/*
 * Just before a burst, outside esp_sdr's lock. With one of those lines within @p half_hz of the
 * centre, or for a stream of its image (the band mirrored about the LO) or, under a filter set
 * wider than the stream's, of what folds in from the input rate away, the burst starts as a frame
 * does: in the panel's vertical blanking, which with the VSYNC pulse and the board's back porch
 * lasts 1.18 ms, room for one at 20 MS/s. Then a burst a frame: at 250 kS/s a sixth of the bursts.
 */
static void panel_blank(uint32_t half_hz, bool stream)
{
	const uint32_t half = half_hz + PANEL_MARGIN_HZ;
	const uint64_t center = set.center_hz;
	bool line = panel_line_at(center, half);

	if (stream) {
		line = line || panel_line_at((uint64_t)((int64_t)center -
							  2 * (int64_t)set.lo_offset_hz),
					     half);
		if (set.bandwidth_hz > STREAM_LPF_MHZ * 1000000U) {
			line = line || panel_line_at(center + STREAM_IN_HZ, half) ||
			       panel_line_at(center - STREAM_IN_HZ, half);
		}
	}
	if (esdr_panel_quiet && line && device_is_ready(panel)) {
		(void)display_esp32_lcd_rgb_wait_blanking(panel, PANEL_WAIT, NULL);
	}
}
#else
static void panel_blank(uint32_t half_hz, bool stream)
{
	ARG_UNUSED(half_hz);
	ARG_UNUSED(stream);
}
#endif

/* A burst's samples, converted. */
static struct esp_sdr_iq16 burst[ESP_SDR_SAMPLES_MAX] ESP_SDR_DATA(BULK, burst);

static K_SEM_DEFINE(rx_go, 0, 1);
static volatile bool worker_busy;

/* ---- triggered snapshots ---- */

#define TRIG_WIN       64U
#define TRIG_WINS      (ESDR_TRIG_SAMPLES / TRIG_WIN)
/* Between bursts: MicroPython's turn on one core (a burst and its windows take about 1 ms). */
#define TRIG_PERIOD_MS 5

static struct {
	volatile bool on;
	uint32_t ratio_x16;
	/* Quiet window power, 16 times its average (0: not learnt yet). */
	uint32_t base16;
	/* seq counts snapshots; first_seq is where this run began: nothing older is fresh. */
	uint32_t seq, first_seq, trig;
	struct esdr_remote_trig_stats st;
} trg;

static int8_t trg_snap[2U * ESDR_TRIG_SAMPLES] ESP_SDR_DATA(BULK, trg_snap);
static K_MUTEX_DEFINE(trg_lock);

static int cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

	return (x > y) - (x < y);
}

/* One 80 MS/s burst; a snapshot if a window passes the threshold. */
static void trig_burst(void)
{
	/* In PSRAM: internal RAM is full on the S31 (a trigger burst comes every few ms). */
	static uint32_t pw[TRIG_WINS] ESP_SDR_DATA(BULK, pw),
		sorted[TRIG_WINS] ESP_SDR_DATA(BULK, sorted);
	struct esp_sdr_rx_burst b;
	uint32_t base, w;

	/* What the filter, as wide as the rate, passes from a rate away folds in. */
	panel_blank(esp_sdr_rx_rate_hz(ESP_SDR_RATE_80MSPS), false);
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

/*
 * Where the stream's centre is: the whole kHz asked for of the LO, what the crystal correction leaves,
 * and how far above (or below) the LO the stream sits.
 */
static void center_update(void)
{
	set.center_hz = (uint64_t)set.lo_mhz * 1000000U + (int64_t)set.lo_khz * 1000 +
			esp_sdr_get_lo_residual_hz() + (int64_t)set.lo_offset_hz;
}

#if defined(GAPLESS)
static int32_t gl_retune_offset(uint64_t hz);
#endif

static int tune(uint64_t hz)
{
	const int32_t offset = set.lo_offset_hz;
	uint32_t mhz;
	int32_t khz;
	int ret;

#if defined(GAPLESS)
	/* A 2 MS/s run without gaps takes the LO to the side its new centre wants. */
	set.lo_offset_hz = gl_retune_offset(hz);
#endif
	esdr_split_hz((double)hz - set.lo_offset_hz, &mhz, &khz);
	/* Refused before the radio moves: esp_sdr_set_freq() would leave the PLL out of its window. */
	if (mhz < LO_MIN_MHZ || mhz > LO_MAX_MHZ) {
		set.lo_offset_hz = offset;
		return -ERANGE;
	}
	ret = esp_sdr_set_freq(mhz);
	/* The offset rides along with the new MHz: a second retune (8 to 14 ms on the S31) only when
	 * it changes. */
	if (ret == 0 && khz != esp_sdr_get_freq_offset()) {
		ret = esp_sdr_set_freq_offset(khz);
	}
	if (ret != 0) {
		/* The library refuses after moving (the VCO's window): back to the settings. */
		(void)esp_sdr_set_freq(set.lo_mhz);
		(void)esp_sdr_set_freq_offset(set.lo_khz);
		set.lo_offset_hz = offset;
		return ret;
	}
	set.freq_hz = hz;
	set.lo_mhz = mhz;
	set.lo_khz = khz;
	center_update();
	return 0;
}

/* Narrower or wider than the filter goes: its narrowest or widest setting. */
static int lpf_set_mhz(uint32_t mhz)
{
	uint32_t min, max;

	esp_sdr_rx_bandwidth_range(&min, &max);
	return esp_sdr_rx_set_bandwidth(CLAMP(mhz, min, max));
}

/* The filter when the app leaves it to the radio. */
static int lpf_auto(void)
{
	return set.lpf_auto_mhz != 0U ? lpf_set_mhz(set.lpf_auto_mhz)
				      : esp_sdr_rx_set_lpf(ESP_SDR_RX_LPF_AUTO);
}

static int set_bandwidth(uint32_t hz)
{
	set.bandwidth_hz = hz;
	return hz == 0U ? lpf_auto() : lpf_set_mhz(hz / 1000000U);
}

static uint32_t stream_rate(void)
{
	return rx.native >= 0 ? esp_sdr_rx_rate_hz((enum esp_sdr_rate)rx.native)
			      : STREAM_IN_HZ / rx.decim;
}

static bool gl_pause_run(void);
static void gl_resume_run(bool paused);
static int request(uint8_t req, uint16_t value, const void *data, uint16_t len, void *reply,
		   uint16_t reply_max);

/*
 * A gapless run holds the radio's lock on the worker's CPU: requests that take it pause the run
 * for their length. GET_STATE and SET_TX_GAIN touch no radio.
 */
int esdr_remote_request(uint8_t req, uint16_t value, const void *data, uint16_t len,
			void *reply, uint16_t reply_max)
{
	bool radio = req != ESDR_REQ_GET_STATE && req != ESDR_REQ_SET_TX_GAIN;
	bool paused = radio && gl_pause_run();
	int ret = request(req, value, data, len, reply, reply_max);

	gl_resume_run(paused);
	return ret;
}

static int request(uint8_t req, uint16_t value, const void *data, uint16_t len, void *reply,
		   uint16_t reply_max)
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
	case ESDR_REQ_SET_TX_GAIN:
		if (value > TX_POWER_MAX) {
			return -EINVAL;
		}
		tx.power = value;
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
			.tx_gain = tx.power,
			.freq_hz = set.freq_hz,
			.center_hz = set.center_hz,
			.sample_rate_hz = rx.active ? stream_rate() : 0U,
			.bandwidth_hz = set.bandwidth_hz,
			.lo_mhz = set.lo_mhz,
			.lo_offset_khz = set.lo_khz,
			/* tune() refuses an LO out of the window and goes back: never unlocked. */
			.status = esp_sdr_freq_window_checked() ? ESDR_STATUS_LO_CHECKED : 0U,
		};
		memcpy(reply, &st, MIN(reply_max, sizeof(st)));
		return MIN(reply_max, sizeof(st));
	case ESDR_REQ_GET_TEMP: {
		int16_t dc;
		int ret = esp_sdr_get_temp_dc(&dc);

		if (ret != 0) {
			return ret;
		}
		if (reply_max < 2U) {
			return -EINVAL;
		}
		sys_put_le16((uint16_t)dc, reply);
		return 2;
	}
	case ESDR_REQ_GET_CAL:
		return esdr_cal_get(reply, reply_max);
	case ESDR_REQ_SET_CAL: {
		int ret = esdr_cal_set(value, data, len);

		/* The correction retuned: where the LO is now. */
		center_update();
		return ret;
	}
	default:
		return -ENOTSUP;
	}
}

/* ---- receive ---- */

/* @p n pairs into the monitor (esdr_mon.c, the newest of them) and the run's buffer. */
static void rx_store_block(const struct esp_sdr_iq16 *iq, uint32_t n)
{
	uint32_t take = n;

	esdr_mon_put_s16((const int16_t *)iq, n);

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

/* A burst's n samples from first_ns into the run, its gap to the last one counted. */
static void rx_store_burst(uint64_t first_ns, size_t n, uint32_t rate);

#if STREAM_LO_OFFSET_HZ != 0U
/*
 * Stream rates from the receiver's words: a burst's at 20 MS/s (rx_burst()) or the dump ring's
 * (GAPLESS, below). Three things a display would show are left out of the band:
 *
 * - The receiver is zero-IF: at the LO sit its DC, some 2 steps off under the AGC and another for every
 *   burst, and noise that rises towards it. The stream's centre is therefore a quarter of the input rate
 *   (20 MS/s) above the LO and comes down by that rotation, which takes no multiplication. The DC
 *   lands on a null of what follows, at 2 MS/s 48 dB down at the band's edge. (The gapless 2 MS/s
 *   stream differs: see GL_FAST_DECIM.)
 * - A CIC filter falls by 12 dB up to its output's Nyquist frequency. It stops at twice the rate, where
 *   the band is its flat middle, 3 dB down at the band's 80 %.
 * - A half-band filter takes the last halving.
 *
 * A burst's mean goes too, with whatever is left of the LO's DC: at 20 MS/s nothing else sits at the
 * input rate's quarter that rises over the noise. A signal exactly at the centre goes with it.
 *
 * The steps run straight on the words: a loop takes the rotation and the CIC's integrators (with a
 * sum of 4 or 8 words first, where the ring's runs take one) and notes the integrators' values at
 * each output; the combs, the scale and the half-band take the notes in a pass of their own, I and
 * Q apart, so that the loop over the words keeps everything in registers (with them in it, it
 * spilled and ran at 15 cycles a word where 8.5 would do; a TCM load stalls its use some 5
 * cycles). Bursts went through PSRAM before, converted and then decimated in place: 984k cycles
 * a burst at 2 MS/s, the CIC 31 a word of it, against some 510k now, 15 a word in dec_run1().
 */
#define CIC_STAGES 3U
#define HB_TAPS    19U
#define HB_SETTLE  (CIC_STAGES + HB_TAPS)
/* The half-band's history, and what a pass of the combs adds to it before the filter runs. */
#define DEC_HIST  (HB_TAPS - 1U)
#define DEC_BATCH 128U
/* The integrators' values one call of a word loop notes, at most. */
#define DEC_SNAPS 128U

static struct {
	/*
	 * The word loops' state: the integrators (they wrap, and the combs give the right
	 * difference all the same) ii1, ii2, ii3, qi1, qi2, qi3, the sums left to the next note and
	 * the sums a note takes (m4).
	 */
	uint32_t st[8];
	/* Words a sum takes: 1, 4 or 8. */
	uint32_t box;
	/*
	 * Box 1 (dec_run1()): the left shifts that take a word's I and Q to its top, DEC_SH_I and
	 * DEC_SH_Q, or the other way round for a band mirrored about the LO (gl_reset()).
	 */
	uint32_t sh_i, sh_q;
	uint32_t ic0, ic1, ic2, qc0, qc1, qc2;
	/*
	 * Notes the combs still drop (the integrators settling); gain box m4^3 in Q32, the combs'
	 * outputs shifted up by pre first where it would not fit (m 2 and 4).
	 */
	uint32_t settle;
	int32_t scale;
	uint32_t pre;
	/*
	 * The half-band's input, I and Q apart (a window is then two runs of plain int16): the last
	 * DEC_HIST of the pass before, then this pass's; hb_mid is the next window's middle.
	 */
	int16_t hi[DEC_HIST + DEC_BATCH], hq[DEC_HIST + DEC_BATCH];
	uint32_t hn, hb_mid;
	/* The output; full, it goes into the run's buffer (dec_flush()). */
	struct esp_sdr_iq16 *out;
	uint32_t nout, out_cap;
	/* The combs' outputs are the output: no half-band (the 2 MS/s gapless stream). */
	bool direct;
} dec;

static uint32_t dec_snap_i[DEC_SNAPS], dec_snap_q[DEC_SNAPS];

/* A word's I and Q at its top, as esp_sdr_rx_i() and esp_sdr_rx_q() take them out. */
#define DEC_SH_I 12U
#define DEC_SH_Q 22U

/* A decimation by box m4 (box 1: to the CIC straight), its first @p settle notes dropped. */
static void dec_reset(uint32_t box, uint32_t m4, uint32_t settle, struct esp_sdr_iq16 *out,
		      uint32_t out_cap)
{
	memset(&dec, 0, sizeof(dec));
	dec.st[6] = m4;
	dec.st[7] = m4;
	dec.box = box;
	dec.sh_i = DEC_SH_I;
	dec.sh_q = DEC_SH_Q;
	dec.settle = settle;
	/* Gain box m4^3 to the scale of NATIVE_SHIFT, in Q32: m4 may have a factor of 5. */
	int64_t scale = (INT64_C(1) << (32 + NATIVE_SHIFT)) / ((int64_t)box * m4 * m4 * m4);

	while (scale > INT32_MAX) {
		scale >>= 1;
		dec.pre++;
	}
	dec.scale = (int32_t)scale;
	dec.hb_mid = HB_TAPS / 2U;
	dec.out = out;
	dec.out_cap = out_cap;
}

#if defined(GL_STAGE)
/* The stage's index of the next output: from 0 at each run (esp_sdr_stage_run_start()). */
static uint64_t stage_at;
#endif

ESP_SDR_CODE(PACED_CHUNK, dec_flush)
static void dec_flush(void)
{
	if (dec.nout == 0U) {
		return;
	}
#if defined(GL_STAGE)
	if (rx.stage) {
		/* From IRAM into TCM: nothing here waits on the cache the other core fills. */
		esp_sdr_stage_put((const int16_t *)dec.out, dec.nout, stage_at);
		stage_at += dec.nout;
		rx.st.samples += dec.nout;
		dec.nout = 0U;
		return;
	}
#endif
	k_mutex_lock(&rx_lock, K_FOREVER);
	if (rx.active) {
		rx_store_block(dec.out, dec.nout);
	}
	k_mutex_unlock(&rx_lock);
	dec.nout = 0U;
}

/* The half-band over what the combs gave, every other window, then the last DEC_HIST kept. */
ESP_SDR_CODE(PACED_SAMPLE, dec_hb)
static __attribute__((optimize("O2"))) void dec_hb(void)
{
	/* Kaiser windowed, beta 6, in Q15; every other tap but the middle one is zero. */
	uint32_t c = dec.hb_mid;

	for (; c + HB_TAPS / 2U < dec.hn; c += 2U) {
		const int16_t *i = &dec.hi[c], *q = &dec.hq[c];
		const int32_t vi = 16392 * i[0] + 10086 * (i[-1] + i[1]) - 2546 * (i[-3] + i[3]) +
				   837 * (i[-5] + i[5]) - 206 * (i[-7] + i[7]) +
				   17 * (i[-9] + i[9]);
		const int32_t vq = 16392 * q[0] + 10086 * (q[-1] + q[1]) - 2546 * (q[-3] + q[3]) +
				   837 * (q[-5] + q[5]) - 206 * (q[-7] + q[7]) +
				   17 * (q[-9] + q[9]);

		dec.out[dec.nout].i = (int16_t)CLAMP(vi >> 15, INT16_MIN, INT16_MAX);
		dec.out[dec.nout].q = (int16_t)CLAMP(vq >> 15, INT16_MIN, INT16_MAX);
		if (++dec.nout == dec.out_cap) {
			dec_flush();
		}
	}
	if (dec.hn > DEC_HIST) {
		const uint32_t drop = dec.hn - DEC_HIST;

		memmove(dec.hi, &dec.hi[drop], DEC_HIST * sizeof(dec.hi[0]));
		memmove(dec.hq, &dec.hq[drop], DEC_HIST * sizeof(dec.hq[0]));
		dec.hn = DEC_HIST;
		c -= drop;
	}
	dec.hb_mid = c;
}

/* The noted values through the combs (their state in registers) into the half-band's input. */
ESP_SDR_CODE(PACED_SAMPLE, dec_combs)
static __attribute__((optimize("O2"))) void dec_combs(const uint32_t *ni,
								 const uint32_t *nq, uint32_t snaps)
{
	uint32_t ic0 = dec.ic0, ic1 = dec.ic1, ic2 = dec.ic2, qc0 = dec.qc0, qc1 = dec.qc1,
		 qc2 = dec.qc2;
	const int32_t scale = dec.scale;
	const uint32_t pre = dec.pre;
	uint32_t k = 0, hn = dec.hn;

	/* The first outputs still see the integrators settling. */
	for (; k < snaps && dec.settle > 0U; k++, dec.settle--) {
		const uint32_t yi = ni[k], yq = nq[k];
		const uint32_t ai = yi - ic0, aq = yq - qc0, bi = ai - ic1, bq = aq - qc1;

		ic0 = yi;
		qc0 = yq;
		ic1 = ai;
		qc1 = aq;
		ic2 = bi;
		qc2 = bq;
	}
#define DEC_COMB()                                                                                 \
	const uint32_t yi = ni[k], yq = nq[k];                                                     \
	const uint32_t ai = yi - ic0, aq = yq - qc0, bi = ai - ic1, bq = aq - qc1;                 \
	const uint32_t ci = bi - ic2, cq = bq - qc2;                                               \
	const int16_t oi = (int16_t)(((int64_t)(int32_t)(ci << pre) * scale) >> 32);               \
	const int16_t oq = (int16_t)(((int64_t)(int32_t)(cq << pre) * scale) >> 32);               \
                                                                                                   \
	ic0 = yi;                                                                                  \
	qc0 = yq;                                                                                  \
	ic1 = ai;                                                                                  \
	qc1 = aq;                                                                                  \
	ic2 = bi;                                                                                  \
	qc2 = bq

	/*
	 * The output's pointer and room in registers (through dec, GCC read them again at every
	 * output: the stores might alias), the next notes loaded a step ahead (a TCM load stalls
	 * its use some 5 cycles): 25 instructions an output and 2 loads, where it took 32 and 5,
	 * three of them used at once. It reads a note past the last: the word loops' notes
	 * (dec_snap_i, dec_snap_q) are DEC_SNAPS long and gl.chunk keeps them below that.
	 */
	while (dec.direct && k < snaps) {
		struct esp_sdr_iq16 *o = &dec.out[dec.nout];
		const uint32_t m = MIN(snaps - k, dec.out_cap - dec.nout);
		const uint32_t *ri = &ni[k], *rq = &nq[k], *const end = &ni[k + m];
		uint32_t yi = ri[0], yq = rq[0];

		for (; ri < end; ri++, rq++, o++) {
			const uint32_t ai = yi - ic0, aq = yq - qc0, bi = ai - ic1, bq = aq - qc1;
			const uint32_t ci = bi - ic2, cq = bq - qc2;

			ic0 = yi;
			qc0 = yq;
			yi = ri[1];
			yq = rq[1];
			ic1 = ai;
			qc1 = aq;
			ic2 = bi;
			qc2 = bq;
			o->i = (int16_t)(((int64_t)(int32_t)(ci << pre) * scale) >> 32);
			o->q = (int16_t)(((int64_t)(int32_t)(cq << pre) * scale) >> 32);
		}
		k += m;
		dec.nout += m;
		if (dec.nout == dec.out_cap) {
			dec_flush();
		}
	}
	for (; k < snaps; k++) {
		DEC_COMB();
		dec.hi[hn] = oi;
		dec.hq[hn] = oq;
		if (++hn == DEC_HIST + DEC_BATCH) {
			dec.hn = hn;
			dec_hb();
			hn = dec.hn;
		}
	}
#undef DEC_COMB
	dec.ic0 = ic0;
	dec.ic1 = ic1;
	dec.ic2 = ic2;
	dec.qc0 = qc0;
	dec.qc1 = qc1;
	dec.qc2 = qc2;
	dec.hn = hn;
}

#define DEC_INTEGRATE(si, sq)                                                                      \
	do {                                                                                       \
		ii1 += (uint32_t)(si);                                                             \
		qi1 += (uint32_t)(sq);                                                             \
		ii2 += ii1;                                                                        \
		qi2 += qi1;                                                                        \
		ii3 += ii2;                                                                        \
		qi3 += qi2;                                                                        \
	} while (0)

/*
 * Every m4th sum the integrators' values are noted for the combs, through pointers: with an index
 * GCC worked out both addresses again at every word.
 */
#define DEC_NOTE()                                                                                 \
	do {                                                                                       \
		if (--left == 0U) {                                                                \
			*pi++ = ii3;                                                               \
			*pq++ = qi3;                                                               \
			left = m4;                                                                 \
		}                                                                                  \
	} while (0)

#define DEC_LOAD_ST(ni, nq)                                                                        \
	uint32_t *const st = dec.st;                                                               \
	uint32_t ii1 = st[0], ii2 = st[1], ii3 = st[2], qi1 = st[3], qi2 = st[4], qi3 = st[5];    \
	uint32_t left = st[6], *pi = &(ni)[*snaps], *pq = &(nq)[*snaps];                          \
	const uint32_t m4 = st[7]

#define DEC_SAVE_ST(ni)                                                                            \
	do {                                                                                       \
		st[0] = ii1;                                                                       \
		st[1] = ii2;                                                                       \
		st[2] = ii3;                                                                       \
		st[3] = qi1;                                                                       \
		st[4] = qi2;                                                                       \
		st[5] = qi3;                                                                       \
		st[6] = left;                                                                      \
		*snaps = (uint32_t)(pi - (ni));                                                    \
	} while (0)

/* A word's I (sh: sh_i) or Q (sh_q), -512 to 511, the shift in a register as cheap as one given. */
#define DEC_RX(w, sh) ((int32_t)((w) << (sh)) >> 22)

/* Four words rotated down a quarter of the rate, each into the integrators: the stream's 4k on. */
#define DEC_ONE4(w0, w1, w2, w3)                                                                   \
	do {                                                                                       \
		DEC_INTEGRATE(DEC_RX(w0, sh_i), DEC_RX(w0, sh_q));                                 \
		DEC_NOTE();                                                                        \
		DEC_INTEGRATE(DEC_RX(w1, sh_q), -DEC_RX(w1, sh_i));                                \
		DEC_NOTE();                                                                        \
		DEC_INTEGRATE(-DEC_RX(w2, sh_i), -DEC_RX(w2, sh_q));                               \
		DEC_NOTE();                                                                        \
		DEC_INTEGRATE(-DEC_RX(w3, sh_q), DEC_RX(w3, sh_i));                                \
		DEC_NOTE();                                                                        \
	} while (0)

/*
 * Box 1: @p groups of 4 words, the notes into ni and nq. The next group's loads go out before
 * this one's arithmetic, so no load's use waits for it. For bursts, and with GAPLESS for the 2
 * MS/s stream too, which runs it all the time beside the other core: there in IRAM (a shared
 * cache in front of the flash slows CPU 1's loop down while CPU 0 draws). With dec.sh_i and
 * dec.sh_q swapped the words come mirrored about the LO, and the rotation brings down what lies a
 * quarter of the rate below it; the notes swapped back on their way into the combs (gl_notes())
 * give it the right way round (swapped words times (-j)^n, swapped, are the words times j^n).
 * The loop is the same 41 instructions every 4 words either way.
 */
ESP_SDR_CODE(PACED_SAMPLE, dec_run1)
static __noinline __attribute__((optimize("O2"))) void
dec_run1(const uint32_t *p, uint32_t groups, uint32_t *ni, uint32_t *nq, uint32_t *snaps)
{
	DEC_LOAD_ST(ni, nq);
	const uint32_t sh_i = dec.sh_i, sh_q = dec.sh_q;
	uint32_t w0 = p[0], w1 = p[1], w2 = p[2], w3 = p[3];

	for (uint32_t g = 1; g < groups; g++) {
		const uint32_t n0 = p[4], n1 = p[5], n2 = p[6], n3 = p[7];

		DEC_ONE4(w0, w1, w2, w3);
		w0 = n0;
		w1 = n1;
		w2 = n2;
		w3 = n3;
		p += 4;
	}
	DEC_ONE4(w0, w1, w2, w3);
	DEC_SAVE_ST(ni);
}

/*
 * The burst pipeline. A burst's word loop runs after its capture, under the radio's lock (the bank
 * is the next capture's), its notes kept in PSRAM. The rest (the combs, the half-band, the mean,
 * the store) runs during the next capture, as its hook: in TCM group 2 the CPU runs on beside the
 * engine (esp_sdr_rx_set_capture_hook()), which runs on some 0.8 ms past the burst at 20 MS/s
 * while the hook does. Below a CIC ratio of 5 (2.5 and 5 MS/s) the hook would take longer: there
 * the burst goes through as a whole after its capture, its notes in chunks in internal RAM.
 */
#define BURST_NOTES   (ESP_SDR_SAMPLES_MAX / 2U)
#define BURST_HOOK_M  5U
static uint32_t burst_ni[BURST_NOTES] ESP_SDR_DATA(BULK, burst_ni),
	burst_nq[BURST_NOTES] ESP_SDR_DATA(BULK, burst_nq);
static struct {
	/* Notes wait for the combs, n of them; n_out outputs in burst[] wait for the store. */
	bool notes, outputs;
	uint32_t n, n_out, rate;
	uint64_t first_ns;
} bp;

/* A burst's words, a multiple of 4 (esp_sdr_rx_capture_with()): its notes, or all of it. */
static void burst_words(void *user, const uint32_t *w, size_t count)
{
	const uint32_t m = (uint32_t)(uintptr_t)user;

	dec_reset(1U, m, CIC_STAGES, burst, ARRAY_SIZE(burst));
	if (m >= BURST_HOOK_M) {
		uint32_t snaps = 0U;

		if (count >= 4U) {
			dec_run1(w, (uint32_t)(count / 4U), burst_ni, burst_nq, &snaps);
		}
		bp.n = snaps;
		bp.notes = true;
		return;
	}
	/* Words a call of dec_run1() takes: whole groups of 4, fewer notes than DEC_SNAPS. */
	const size_t chunk = (DEC_SNAPS - 1U) * m / 4U * 4U;

	for (size_t k = 0; k + 4U <= count; k += chunk) {
		uint32_t snaps = 0U;

		dec_run1(&w[k], (uint32_t)(MIN(chunk, count - k) / 4U), dec_snap_i, dec_snap_q,
			 &snaps);
		dec_combs(dec_snap_i, dec_snap_q, snaps);
		dec_hb();
	}
	bp.n_out = dec.nout;
	bp.outputs = true;
}

static void bp_store(void);

/* The capture hook: the waiting notes through the combs and the half-band, stored. */
static void bp_hook(void *user)
{
	ARG_UNUSED(user);
	if (!bp.notes) {
		return;
	}
	bp.notes = false;
	dec_combs(burst_ni, burst_nq, bp.n);
	dec_hb();
	bp.n_out = dec.nout;
	bp.outputs = true;
	bp_store();
}

/* The waiting outputs, their mean off, into the run. */
static void bp_store(void)
{
	const uint32_t n = bp.n_out;
	int32_t sum_i = 0, sum_q = 0;

	if (!bp.outputs) {
		return;
	}
	bp.outputs = false;
	if (n > 0U) {
		for (uint32_t t = 0; t < n; t++) {
			sum_i += burst[t].i;
			sum_q += burst[t].q;
		}
		sum_i /= (int32_t)n;
		sum_q /= (int32_t)n;
		for (uint32_t t = 0; t < n; t++) {
			burst[t].i = (int16_t)(burst[t].i - sum_i);
			burst[t].q = (int16_t)(burst[t].q - sum_q);
		}
	}
	rx_store_burst(bp.first_ns, n, bp.rate);
}

/* Whatever of the waiting burst is left, into the run (the worker about to wait). */
static void bp_post(void)
{
	bp_hook(NULL);
	bp_store();
}
#endif

/* One burst of the run into its buffer; false once the run has all it wants. */
static bool rx_burst(void)
{
	uint32_t rate = stream_rate();
	uint64_t first_ns;
	size_t n;
	int ret;

	panel_blank(rx.native >= 0 ? rate : rate / 2U, rx.native < 0);

	if (rx.native >= 0) {
		n = ESP_SDR_SAMPLES_MAX;
		ret = esp_sdr_rx_capture_iq((enum esp_sdr_rate)rx.native, n, burst, &first_ns);
		for (size_t k = 0; ret == 0 && k < n; k++) {
			burst[k].i = (int16_t)(burst[k].i << NATIVE_SHIFT);
			burst[k].q = (int16_t)(burst[k].q << NATIVE_SHIFT);
		}
	} else {
#if STREAM_LO_OFFSET_HZ != 0U
		const uint32_t m = rx.decim / 2U;
		uint64_t at_ns;

		/* The last burst's combs and half-band run during this capture (bp_hook()). */
		ret = esp_sdr_rx_capture_with(STREAM_IN_RATE, ESP_SDR_SAMPLES_MAX, burst_words,
					      (void *)(uintptr_t)m, &at_ns);
		/* What of it the hook left (none, unless the hook ran out of time). */
		if (m >= BURST_HOOK_M) {
			bp_store();
		}
		if (ret == 0) {
			bp.first_ns = at_ns + (uint64_t)HB_SETTLE * m * NSEC_PER_SEC / STREAM_IN_HZ;
			bp.rate = rate;
			/* Through as a whole (CIC ratios below 5): stored now. */
			if (m < BURST_HOOK_M) {
				bp_store();
			}
		}
		n = 0U;
		first_ns = 0U;
#else
		ret = esp_sdr_rx_capture_decimated(STREAM_IN_RATE, ESP_SDR_SAMPLES_MAX,
						   rx.decim, burst, DECIM_OUT_MAX, &n, &first_ns);
		for (size_t k = 0; ret == 0 && k < n; k++) {
			burst[k].i >>= DECIM_SHIFT;
			burst[k].q >>= DECIM_SHIFT;
		}
#endif
	}
	if (ret != 0) {
		rx.st.overflows++;
		return true;
	}
	/* On the S31 bp_post() stores a stream's burst, during the next capture. */
	if (STREAM_LO_OFFSET_HZ == 0U || rx.native >= 0) {
		rx_store_burst(first_ns, n, rate);
	}

	k_mutex_lock(&rx_lock, K_FOREVER);
	bool more = rx.active && (rx.count == 0U || rx.st.samples < rx.count) &&
		    (rx.buf == NULL || rx.wrap || rx.st.samples < rx.cap);
	k_mutex_unlock(&rx_lock);
	return more;
}

static void rx_store_burst(uint64_t first_ns, size_t n, uint32_t rate)
{
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
	k_mutex_unlock(&rx_lock);
}

#if defined(GAPLESS)
/*
 * Buffered streams at 250 kS/s and below, and at 2 MS/s, without gaps: the dump ring
 * (esp_sdr_dumpring.h) on the worker's CPU, its words decimated as a burst's are (dec_*), the same steps carried
 * from one call to the next: the quarter-rate rotation, a sum of 4 or 8 words (a CIC's first
 * stage at the capture rate would take more than the cycles a word the CPU has beside the ring's
 * turns), a 3-stage CIC to twice the rate and the half-band filter. The sums have a null at the
 * LO's DC, a quarter of the rate below the stream's centre: no mean to take off. Requests that
 * need the radio pause the run (gl_pause_run()); each pause counts as a gap.
 */
#define GL_OUT   128U
/*
 * Gapless runs capture at 20 MS/s, as the bursts do, so that only what the analog filter lets
 * through from 20 MHz away folds in (doc/hardware-quirks.md). 20 MS/s leave CPU 1 16 cycles a
 * word, the ring's turns some 1.6 of them (PIE copies: 3.9 before, when this ran at 10 MS/s): the
 * sink takes 12.0 a word at 250 kS/s, 10.9 at 125 kS/s (measured on the VIEWE board). The stream
 * sits a quarter of the rate above the LO.
 */
#define GL_IN_RATE      ESP_SDR_RATE_20MSPS
#define GL_IN_HZ        20000000U
/*
 * 2 MS/s (the Pcap app's rate) is too fast for that: the notes alone would come at 4 MS/s. It
 * captures at 10 MS/s, every word into the CIC (box 1, dec_run1()), ratio 5 straight to the rate
 * with no half-band: a third order sinc, 2.6 dB down at 500 kHz, folds what lies 1.5 MHz from
 * the centre in 30 dB down, and the analog filter what lies 10 MHz away (the 13 MHz setting: some
 * 12 dB down on the LO's side of the centre; on the other 28 measured at 10 MS/s, over 40 in
 * doc/hardware-quirks.md's table). Half the capture rate halves the turns that stop the other
 * core. The centre lies a quarter of the 10 MS/s (2.5 MHz) from the LO, on the side
 * gl_lo_offset() picks; with no sum there is no null at the LO's DC: the sinc leaves it some 42
 * dB down, folded to 500 kHz from the centre.
 *
 * What lies 10 MHz away is the weak point, the more on the LO's side: so the LO goes to the side
 * of the 2.4 GHz band's nearer edge, below the centre in the band's lower half and above it in
 * the upper (GL_SIDE_HZ, the middle), where from 2473.5 MHz on (Bluetooth LE's channel 39,
 * 802.15.4's 25 and 26) what folds in some 12 dB down lies past the band, unless a crystal
 * harmonic would fold in on that side (gl_lo_offset()). dec_run1() takes a
 * band below the LO with I and Q swapped, and gl_notes() swaps them back: not a cycle more. 20
 * MS/s would keep both sides out, but it leaves the sink 14.6 cycles a word and the cheapest
 * chain there (pairs of words, then this CIC) has a word loop of 11.25 instructions a word.
 */
#define GL_FAST_DECIM 10U
#define GL_FAST_RATE  ESP_SDR_RATE_10MSPS
#define GL_FAST_HZ    10000000U
#define GL_SIDE_HZ    2441750000ULL

/* Rates a buffered stream gets without gaps (of STREAM_IN_HZ: see rx_start()). */
static bool gl_decim(unsigned int decim)
{
	return decim == GL_FAST_DECIM || (decim % 16U == 0U && decim >= 64U);
}

/*
 * The crystal's harmonics (every 40 MHz, 2400 MHz the chip's own line among them) within the
 * band's half width of @p hz: a weak image there would bring the line in on the stream.
 */
static bool gl_spur_near(uint64_t hz)
{
	const uint64_t step = 40000000U, near = (hz + step / 2U) / step * step;

	return (hz > near ? hz - near : near - hz) <= 1000000U;
}

/*
 * The stream's centre over the LO: a quarter of the capture rate, at 2 MS/s with the LO above a
 * centre @p hz in the band's upper half (where the LO's range has room), unless the weak image
 * that side (GL_FAST_HZ away, on the LO's side) holds a crystal harmonic and the other does
 * not (802.15.4's 12: 2400 MHz below; its 24: 2480 MHz above).
 */
static int32_t gl_lo_offset(unsigned int decim, uint64_t hz)
{
	const int32_t offset = (int32_t)((decim == GL_FAST_DECIM ? GL_FAST_HZ : GL_IN_HZ) / 4U);
	bool above;

	if (decim != GL_FAST_DECIM || hz + (uint64_t)offset >= (uint64_t)LO_MAX_MHZ * 1000000U) {
		return offset;
	}
	above = hz > GL_SIDE_HZ;
	if (gl_spur_near(above ? hz + GL_FAST_HZ : hz - GL_FAST_HZ) &&
	    !gl_spur_near(above ? hz - GL_FAST_HZ : hz + GL_FAST_HZ)) {
		above = !above;
	}
	return above ? -offset : offset;
}

/* For tune(): the LO's side for a new centre @p hz while a 2 MS/s run goes on, else as it is. */
static int32_t gl_retune_offset(uint64_t hz)
{
	return rx.active && rx.gapless && rx.decim == GL_FAST_DECIM
		       ? gl_lo_offset(GL_FAST_DECIM, hz)
		       : set.lo_offset_hz;
}

static struct {
	/* Words carried to the next call: the sums (box 1: groups of 4) start at the stream's own
	 * word 0. */
	uint32_t carry[8];
	uint32_t ncarry;
	struct esp_sdr_iq16 out[GL_OUT];
	/* The capture rate, the words a sum (or group) takes, sums a word loop takes at most. */
	enum esp_sdr_rate rate;
	uint32_t group, chunk;
	/* The LO above the centre: the band comes mirrored, I and Q swapped (gl_lo_offset()). */
	bool mirror;
	/* A request waits for the run to stop (pause), the worker for it to end (resume). */
	volatile bool pause, paused;
} gl;

static K_SEM_DEFINE(gl_resume, 0, 1);

static void gl_reset(unsigned int decim)
{
	memset(&gl, 0, offsetof(typeof(gl), pause));
	if (decim == GL_FAST_DECIM) {
		gl.rate = GL_FAST_RATE;
		gl.group = 4U;
		dec_reset(1U, (uint32_t)(GL_FAST_HZ / (STREAM_IN_HZ / decim)), CIC_STAGES, gl.out,
			  GL_OUT);
		dec.direct = true;
		gl.mirror = set.lo_offset_hz < 0;
		if (gl.mirror) {
			dec.sh_i = DEC_SH_Q;
			dec.sh_q = DEC_SH_I;
		}
	} else {
		/* The CIC's ratio m = decim / 2 in sums of 8 words where it can, else of 4. */
		const uint32_t box = (decim / 2U) % 8U == 0U ? 8U : 4U;

		gl.rate = GL_IN_RATE;
		gl.group = box;
		dec_reset(box, decim / 2U / box, CIC_STAGES + 1U, gl.out, GL_OUT);
	}
	/* Notes a word loop makes stay below DEC_SNAPS (one more comes from a carried sum). */
	gl.chunk = (DEC_SNAPS - 2U) * dec.st[7] * dec.box / gl.group;
}

/* A sum of 4 words rotated down a quarter of the rate: the stream's 4k to 4k + 3. */
#define DEC_SUM(w0, w1, w2, w3, si, sq)                                                            \
	do {                                                                                       \
		si = esp_sdr_rx_i(w0) + esp_sdr_rx_q(w1) - esp_sdr_rx_i(w2) - esp_sdr_rx_q(w3);    \
		sq = esp_sdr_rx_q(w0) - esp_sdr_rx_i(w1) - esp_sdr_rx_q(w2) + esp_sdr_rx_i(w3);    \
	} while (0)

/*
 * The hot loops, at -O2 (the rest of the file is -Os), each over all of a call's sums: the next
 * sum's loads go out before this one's arithmetic, so no load's use waits for it. A sum of 4 words
 * is 34 instructions; a sum of 8 takes the integrators once (measured 8.4 cycles a word on CPU 1
 * either way: the unpacking is most of it).
 */
ESP_SDR_CODE(PACED_SAMPLE, dec_run4)
static __noinline __attribute__((optimize("O2"))) void
dec_run4(const uint32_t *p, uint32_t sums, uint32_t *snaps)
{
	DEC_LOAD_ST(dec_snap_i, dec_snap_q);
	uint32_t w0 = p[0], w1 = p[1], w2 = p[2], w3 = p[3];

	for (uint32_t b = 1; b < sums; b++) {
		const uint32_t n0 = p[4], n1 = p[5], n2 = p[6], n3 = p[7];
		int32_t si, sq;

		DEC_SUM(w0, w1, w2, w3, si, sq);
		DEC_INTEGRATE(si, sq);
		DEC_NOTE();
		w0 = n0;
		w1 = n1;
		w2 = n2;
		w3 = n3;
		p += 4;
	}
	{
		int32_t si, sq;

		DEC_SUM(w0, w1, w2, w3, si, sq);
		DEC_INTEGRATE(si, sq);
		DEC_NOTE();
	}
	DEC_SAVE_ST(dec_snap_i);
}

/* Reads 4 words past its last sum (the next one's first): the caller makes sure they exist. */
ESP_SDR_CODE(PACED_SAMPLE, dec_run8)
static __noinline __attribute__((optimize("O2"))) void
dec_run8(const uint32_t *p, uint32_t sums, uint32_t *snaps)
{
	DEC_LOAD_ST(dec_snap_i, dec_snap_q);
	uint32_t w0 = p[0], w1 = p[1], w2 = p[2], w3 = p[3];

	for (uint32_t b = 0; b < sums; b++) {
		const uint32_t w4 = p[4], w5 = p[5], w6 = p[6], w7 = p[7];
		int32_t si, sq, ti, tq;

		DEC_SUM(w0, w1, w2, w3, si, sq);
		w0 = p[8];
		w1 = p[9];
		w2 = p[10];
		w3 = p[11];
		/* A scheduling barrier: else the halves interleave, w4..w7 used as they load. */
		__asm__ volatile("");
		DEC_SUM(w4, w5, w6, w7, ti, tq);
		DEC_INTEGRATE(si + ti, sq + tq);
		DEC_NOTE();
		p += 8;
	}
	DEC_SAVE_ST(dec_snap_i);
}

/* Sums of box words (box 1: groups of 4) over w: dec_run8's over-read kept inside w (the last sum
 * through a copy). */
ESP_SDR_CODE(PACED_SAMPLE, gl_sums)
static void gl_sums(const uint32_t *w, uint32_t sums, uint32_t *snaps)
{
	if (dec.box == 1U) {
		dec_run1(w, sums, dec_snap_i, dec_snap_q, snaps);
		return;
	}
	if (dec.box == 4U) {
		dec_run4(w, sums, snaps);
		return;
	}
	if (sums > 1U) {
		dec_run8(w, sums - 1U, snaps);
	}
	uint32_t pad[12];

	for (unsigned int k = 0; k < 8U; k++) {
		pad[k] = w[8U * (sums - 1U) + k];
	}
	dec_run8(pad, 1U, snaps);
}

/* The notes of a word loop through the combs (and the half-band), a mirrored band's swapped back. */
ESP_SDR_CODE(PACED_CHUNK, gl_notes)
static void gl_notes(uint32_t *snaps)
{
	if (gl.mirror) {
		dec_combs(dec_snap_q, dec_snap_i, *snaps);
	} else {
		dec_combs(dec_snap_i, dec_snap_q, *snaps);
	}
	if (!dec.direct) {
		dec_hb();
	}
	*snaps = 0U;
}

ESP_SDR_CODE(PACED_CHUNK, gl_sink)
static void gl_sink(void *user, const uint32_t *w, size_t n)
{
	const uint32_t box = gl.group;
	uint32_t snaps = 0U, sums;
	size_t k = 0;

	ARG_UNUSED(user);
	/* A sum the last call split: completed first. */
	while (gl.ncarry > 0U && gl.ncarry < box && k < n) {
		gl.carry[gl.ncarry++] = w[k++];
	}
	if (gl.ncarry == box) {
		gl_sums(gl.carry, 1U, &snaps);
		gl.ncarry = 0U;
	}
	if (gl.ncarry == 0U) {
		sums = (uint32_t)((n - k) / box);
		while (sums > 0U) {
			const uint32_t m = MIN(sums, gl.chunk);

			gl_sums(&w[k], m, &snaps);
			k += box * m;
			sums -= m;
			if (sums > 0U) {
				gl_notes(&snaps);
			}
		}
	}
	for (; k < n; k++) {
		gl.carry[gl.ncarry++] = w[k];
	}
	gl_notes(&snaps);
	/* The buffer's lock and its PSRAM a few times a millisecond, not twice a turn. */
	if (dec.nout >= GL_OUT / 2U) {
		dec_flush();
	}
	/* Done, stopped or asked to pause: the run ends at its next unit. */
	if (!rx.active || gl.pause || (rx.count != 0U && rx.st.samples >= rx.count) ||
	    (rx.buf != NULL && !rx.wrap && rx.st.samples >= rx.cap)) {
		esp_sdr_dumpring_stop();
	}
}

/* The run until it has what it wants or rx_stop(); restarted after a pause or an error. */
static void gl_run(void)
{
	struct esp_sdr_dumpring_cfg cfg = {.sink = gl_sink};
	bool first = true;

	gl.pause = false;
	k_sem_reset(&gl_resume);
	gl_reset(rx.decim);
	cfg.rate = gl.rate;
	while (rx.active && (rx.count == 0U || rx.st.samples < rx.count) &&
	       (rx.buf == NULL || rx.wrap || rx.st.samples < rx.cap)) {
		int ret;

#if defined(GL_STAGE)
		if (rx.stage) {
			/* Each run a hole in the stage's index, the decimator starting afresh. */
			esp_sdr_stage_run_start();
			stage_at = 0U;
		}
#endif
		ret = esp_sdr_dumpring_run(&cfg, NULL);


		if (gl.pause) {
			gl.paused = true;
			(void)k_sem_take(&gl_resume, K_FOREVER);
			gl.paused = false;
#if defined(GL_STAGE)
			/* The stage afresh after the request (its reader finds itself lapped and
			 * starts over). */
			if (rx.stage) {
				esp_sdr_stage_init(false);
			}
#endif
		} else if (ret == -ENOBUFS) {
			rx.st.overflows++;
		} else if (ret != 0) {
			rx.st.resyncs++;
			/* Not the ring's to have now (a stray capture or replay): bursts again. */
			if (ret != -EIO) {
				k_sleep(K_MSEC(1));
			}
		} else {
			continue;
		}
		/* A restart is a gap once samples went out; the decimator starts afresh behind it. */
		if (!first || rx.st.samples > 0U) {
			rx.st.gaps++;
		}
		gl_reset(rx.decim);
		first = false;
	}
}

/* For a request that needs the radio: the run off meanwhile, and on again by gl_resume_run(). */
static bool gl_pause_run(void)
{
	if (!rx.active || !rx.gapless || !worker_busy) {
		return false;
	}
	gl.pause = true;
	esp_sdr_dumpring_stop();
	while (!gl.paused && worker_busy) {
		k_msleep(1);
	}
	return true;
}

static void gl_resume_run(bool paused)
{
	if (paused) {
		gl.pause = false;
		k_sem_give(&gl_resume);
	}
}
#else
static bool gl_pause_run(void)
{
	return false;
}

static void gl_resume_run(bool paused)
{
	ARG_UNUSED(paused);
}
#endif /* GAPLESS */

static void worker_fn(void *a, void *b, void *c)
{
	uint64_t read_from = 0U;
	unsigned int read_bursts = 0U;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
#if STREAM_LO_OFFSET_HZ != 0U
	/* Every capture finishes the burst before it meanwhile, where one waits. */
	esp_sdr_rx_set_capture_hook(bp_hook, NULL);
#endif
	for (;;) {
		(void)k_sem_take(&rx_go, K_FOREVER);
#if STREAM_LO_OFFSET_HZ != 0U
		/* A burst of the run before is no part of this one. */
		bp.notes = false;
		bp.outputs = false;
#endif
		while (trg.on) {
			trig_burst();
			k_sleep(K_MSEC(TRIG_PERIOD_MS));
		}
#if defined(GAPLESS)
		if (rx.active && rx.gapless) {
			gl_run();
			rx.active = false;
		}
#endif
		while (rx.active) {
			/* A display-only run: what a monitor read needs to be new, up to MON_BURSTS. */
			if (rx.buf == NULL && rx.count == 0U && !esdr_mon_read &&
			    (read_bursts >= MON_BURSTS ||
			     esdr_mon_total - read_from >= esdr_mon_want)) {
#if STREAM_LO_OFFSET_HZ != 0U
				/* The last burst to the monitor now, not with the next capture. */
				if (bp.notes || bp.outputs) {
					bp_post();
					continue;
				}
#endif
				k_sleep(K_MSEC(2));
				continue;
			}
			if (esdr_mon_read) {
				esdr_mon_read = false;
				read_from = esdr_mon_total;
				read_bursts = 0U;
			}
			read_bursts++;
			if (!rx_burst()) {
				rx.active = false;
				break;
			}
#if !defined(WORKER_OWN_CPU)
			/* MicroPython runs between bursts. */
			k_sleep(K_TICKS(1));
#endif
		}
		worker_busy = false;
	}
}

K_THREAD_STACK_DEFINE(worker_stack, WORKER_STACK);
static struct k_thread worker_thread;

/*
 * The LO and the filter move with the kind of run: where the bursts are as captured, the centre is
 * the LO. A centre this kind of run cannot reach changes nothing and is the caller's to report.
 */
static int run_setup(int32_t offset_hz, uint32_t lpf_mhz)
{
	if (offset_hz != set.lo_offset_hz) {
		const int32_t before = set.lo_offset_hz;
		int ret;

		set.lo_offset_hz = offset_hz;
		ret = tune(set.freq_hz);
		if (ret != 0) {
			set.lo_offset_hz = before;
			return ret;
		}
	}
	if (lpf_mhz != set.lpf_auto_mhz) {
		set.lpf_auto_mhz = lpf_mhz;
		if (set.bandwidth_hz == 0U) {
			return lpf_auto();
		}
	}
	return 0;
}

static int rx_start(int native, unsigned int decim, int16_t *buf, size_t cap, uint64_t count,
		    bool wrap, bool stage)
{
	int ret;
#if defined(GAPLESS)
	/*
	 * A buffer wants every sample: at 312.5 kS/s and below, where the CIC's ratio is a multiple
	 * of 8 and the sums take 8 words. At 500 kS/s (sums of 4, twice the notes) the sink falls
	 * behind 20 MS/s: restarts all the time (measured); 625 kS/s and 1.25 MS/s are multiples
	 * of 8 too, with too many notes. 312.5 kS/s restarts some 1.3 times a second (a gap each,
	 * against bursts' 44 % and 1100 gaps), 250 kS/s and below none in 30 to 60 s. And 2 MS/s,
	 * from a capture at 10 MS/s (GL_FAST_DECIM).
	 */
	const bool gapless = native < 0 && (buf != NULL || stage) && gl_decim(decim);
	const int32_t offset =
		gapless ? gl_lo_offset(decim, set.freq_hz) : (int32_t)STREAM_LO_OFFSET_HZ;
#else
	const bool gapless = false;
	const int32_t offset = (int32_t)STREAM_LO_OFFSET_HZ;
#endif

	if (stage && !gapless) {
		return -EINVAL;
	}

	esdr_remote_tx_stop();
	esdr_remote_trig_stop();
	esdr_remote_rx_stop();
	ret = native < 0 ? run_setup(offset, STREAM_LPF_MHZ)
			 : run_setup(0U, BURST_LPF_MHZ(esp_sdr_rx_rate_hz(native) / 1000000U));
	if (ret != 0) {
		return ret;
	}
#if defined(GL_STAGE)
	/* Afresh after the setup, whatever its calibration bursts left. */
	if (stage) {
		esp_sdr_stage_init(false);
	}
#endif
	k_mutex_lock(&rx_lock, K_FOREVER);
	rx.buf = buf;
	rx.cap = cap;
	rx.count = count;
	rx.wrap = wrap;
	rx.native = native;
	rx.decim = decim;
	rx.have_burst = false;
	rx.gapless = gapless;
	rx.stage = stage;
	memset(&rx.st, 0, sizeof(rx.st));
	esdr_mon_read = true;
	worker_busy = true;
	rx.active = true;
	k_mutex_unlock(&rx_lock);
	k_sem_give(&rx_go);
	return 0;
}

#define STREAM_RATE_ITEM(hz) hz,
static const uint32_t stream_rates[] = {ESDR_LOCAL_RATES(STREAM_RATE_ITEM)};

int esdr_remote_rx_start(uint32_t rate, int16_t *buf, size_t cap, uint64_t count, bool wrap)
{
	for (unsigned int k = 0; k < ARRAY_SIZE(stream_rates); k++) {
		if (rate == stream_rates[k]) {
			return rx_start(-1, STREAM_IN_HZ / rate, buf, cap, count, wrap, false);
		}
	}
	return -EINVAL;
}

#if defined(GL_STAGE)
int esdr_remote_rx_start_stage(uint32_t rate)
{
	for (unsigned int k = 0; k < ARRAY_SIZE(stream_rates); k++) {
		if (rate == stream_rates[k] && gl_decim(STREAM_IN_HZ / rate)) {
			return rx_start(-1, STREAM_IN_HZ / rate, NULL, 0U, 0U, false, true);
		}
	}
	return -EINVAL;
}
#endif

int esdr_remote_rx_start_burst(uint16_t mhz, unsigned int decim, int16_t *buf, size_t cap,
			       uint64_t count)
{
	static const enum esp_sdr_rate natives[] = {
		ESP_SDR_RATE_80MSPS, ESP_SDR_RATE_40MSPS, NATIVE_THIRD,
	};

	for (size_t k = 0; k < ARRAY_SIZE(natives); k++) {
		if (decim == 1U && (uint32_t)mhz * 1000000U == esp_sdr_rx_rate_hz(natives[k])) {
			return rx_start(natives[k], 1U, buf, cap, count, false, false);
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
	int ret;

	ARG_UNUSED(post);
	/* Half duplex; a loop also holds the radio's lock, which the worker's bursts would wait on. */
	esdr_remote_tx_stop();
	esdr_remote_rx_stop();
	esdr_remote_trig_stop();
	/* Snapshots are bursts as captured, at 80 MS/s (trig_burst()). */
	ret = run_setup(0U, BURST_LPF_MHZ(80U));
	if (ret != 0) {
		return ret;
	}
	k_mutex_lock(&trg_lock, K_FOREVER);
	trg.ratio_x16 = ratio_x16;
	trg.base16 = 0;
	trg.first_seq = trg.seq;
	k_mutex_unlock(&trg_lock);
	worker_busy = true;
	trg.on = true;
	k_sem_give(&rx_go);
	return 0;
}

void esdr_remote_trig_stop(void)
{
	/*
	 * Only snapshots end here: the worker stays busy for as long as a receive run lasts, which
	 * esdr_remote_rx_stop() ends, so waiting for it without snapshots would never return.
	 */
	if (!trg.on) {
		return;
	}
	trg.on = false;
	while (worker_busy) {
		k_msleep(1);
	}
}

bool esdr_remote_trig_get(int8_t *dst, uint32_t after, uint32_t *seq, uint32_t *trig)
{
	bool fresh;

	k_mutex_lock(&trg_lock, K_FOREVER);
	fresh = trg.seq != trg.first_seq && trg.seq != after;
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

/* ---- transmit: a carrier ---- */

/* Only the ESP32 and the S31 build this file; both have a tone generator. */
#define HAVE_TX 1

#if !defined(CONFIG_SOC_SERIES_ESP32S31)
/*
 * libphy: tone 1 on/off, its offset (kHz, from the LO) and attenuation, the same for tone 2
 * (the PHY table's rom_start_tx_tone_step). wifiscwout() leaves the TX calibration's debug mode
 * before and puts its work mode back after: without the latter the carrier stays on. The S31's
 * library calls do the same (esp_sdr_tx_tone_start()).
 */
extern void ram_start_tx_tone(int en1, int khz1, int att1, int en2, int khz2, int att2);
extern void txcal_debuge_mode(int en);
extern void ram_txcal_work_mode(void);
/* ROM: clears the enable bit of tone 1, 2 or 3 (0x600060b8, bc, c4); other values all three. */
extern void rom_stop_tx_tone(int which);
#endif

static void tx_off(void)
{
#if defined(TX_LOOPS)
	if (tx.loop) {
		esp_sdr_replay_stop();
		tx.loop = false;
		return;
	}
#endif
#if defined(CONFIG_SOC_SERIES_ESP32S31)
	esp_sdr_tx_tone_stop();
#else
	ram_start_tx_tone(0, 0, 0, 0, 0, 0);
	rom_stop_tx_tone(0);
	ram_txcal_work_mode();
#endif
}

/* The LO at the requested frequency plus the tone's offset; the settings' frequency stays. */
static int tx_tune(void)
{
	uint32_t mhz;
	int32_t khz;
	int ret;

	esdr_split_hz((double)(set.freq_hz + tx.offset_hz), &mhz, &khz);
	ret = esp_sdr_set_freq(mhz);
	if (ret == 0) {
		ret = esp_sdr_set_freq_offset(khz);
	}
	return ret;
}

static int tx_on(void)
{
	const int att = TX_ATT_TOP + (int)(TX_POWER_MAX - tx.power) * TX_ATT_STEP;
	int ret = tx_tune();

	if (ret != 0) {
		return ret;
	}
#if defined(CONFIG_SOC_SERIES_ESP32S31)
	return esp_sdr_tx_tone_start(0, (unsigned int)att);
#else
	txcal_debuge_mode(0);
	ram_start_tx_tone(1, 0, att, 0, 0, 0);
	return 0;
#endif
}

static void gen_offset(const struct esdr_gen *gen)
{
	/* A constant is the carrier itself; a tone sits cycles x rate from it. */
	tx.offset_hz = gen->wave == ESDR_WAVE_CONST ? 0 :
		       (int64_t)(gen->cycles * (float)tx.rate);
}

#if defined(ESDR_LOCAL_TX_BURSTS)
/*
 * Transmit, samples (the S31): a buffer played once inside tx_start, through the CPU-paced TX DC
 * path (esp_sdr_paced.h), as the C5 does. Once the stream's start holds the radio, the pairs go
 * into the capture bank in TCM, scaled to the path's full scale and the power step; then the
 * stream runs from IRAM with interrupts locked until the interpolator has emptied: the panel's
 * DMA keeps the picture, LVGL, touch and the console wait. At most the bank's pairs (106 ms at
 * 250 kS/s). doc/hardware-quirks.md has the path's numbers.
 */
/* The TX DC path's full scale: 256 of the DAC's 511 counts, below the S31's knee. */
#define TX_FULL      16384
/* Input samples played after the last one, zeros, for the interpolation filter to empty. */
#define TX_TAIL      16U
/* The highest DAC write rate (input rate times factor) the poll keeps on time. */
#define TX_WRITE_MAX 1000000U
/* The power ladder in dB a step, the carrier's (TX_ATT_STEP quarter dB). */
#define TX_DB_STEP   ((float)TX_ATT_STEP / 4.0f)

extern uint32_t __esp_sdr_bank_start[], __esp_sdr_bank_end[];

static struct {
	const uint32_t *words;
	uint32_t n, pos, calls;
	uint64_t played;
	struct esp_sdr_paced_stats stats;
} txb;

ESP_SDR_CODE(PACED_SAMPLE, txb_input)
static bool txb_input(void *user, int16_t *i, int16_t *q)
{
	uint32_t w = 0;

	ARG_UNUSED(user);
	txb.calls++;
	if (txb.pos < txb.n) {
		w = txb.words[txb.pos++];
	}
	*i = (int16_t)(w & 0xffffU);
	*q = (int16_t)(w >> 16);
	return true;
}

/* The stream until the tail has gone in: everything it runs in IRAM, interrupts locked. */
ESP_SDR_CODE(PACED_CHUNK, txb_run)
static __noinline void txb_run(void)
{
	unsigned int key = irq_lock();

	esp_sdr_paced_sync();
	while (txb.calls < txb.n + TX_TAIL) {
		(void)esp_sdr_tx_paced_poll();
	}
	irq_unlock(key);
}

/* The samples' factor for a scale and the power step: the carrier's ladder, TX_DB_STEP a step. */
static float tx_level(float scale)
{
	return CLAMP(scale, 0.0f, 1.0f) *
	       powf(10.0f, -(float)(TX_POWER_MAX - tx.power) * TX_DB_STEP / 20.0f);
}

#if defined(TX_LOOPS)
/*
 * Transmit, a buffer repeated (the S31): the DAC engine loops it out of the TCM group below the
 * capture bank (esp_sdr_replay.h) while the CPU runs on, until tx_stop() or count samples. The
 * pairs go in at the highest DAC rate, 80, 40 or 20 MS/s, that is a whole multiple L of the
 * buffer's and holds its n x L words: at most 409 pairs at 250 kS/s. A windowed sinc
 * interpolates around the loop, the last pair running into the first, so the loop has no seam.
 * Scaled as the bursts are, to the same DAC counts; tx_scale() refills, a short pause.
 */
#define LOOP_TAPS 16
#define LOOP_HALF (LOOP_TAPS / 2)

static const struct {
	enum esp_sdr_rate rate;
	uint32_t hz;
} loop_rates[] = {
	{ESP_SDR_RATE_80MSPS, 80000000U},
	{ESP_SDR_RATE_40MSPS, 40000000U},
	{ESP_SDR_RATE_20MSPS, 20000000U},
};

static struct {
	const int16_t *buf;
	uint32_t n, l;
	enum esp_sdr_rate rate;
} lp;

/* The loop's words at the DAC rate, k DAC counts per input unit. */
static void loop_fill(uint32_t *w, float k)
{
	const float half_turn = (float)(ESP_SDR_TAU / 2.0);

	for (uint32_t p = 0; p < lp.l; p++) {
		/* Phase p / L between input pairs j and j + 1: taps at j - 7 .. j + 8. */
		float h[LOOP_TAPS], sum = 0.0f;

		for (int t = 0; t < LOOP_TAPS; t++) {
			float x = (float)(t - (LOOP_HALF - 1)) - (float)p / (float)lp.l;
			float win = 0.5f + 0.5f * cosf(half_turn * x / (float)LOOP_HALF);

			h[t] = (x == 0.0f ? 1.0f : sinf(half_turn * x) / (half_turn * x)) * win;
			sum += h[t];
		}
		for (int t = 0; t < LOOP_TAPS; t++) {
			h[t] *= k / sum;
		}
		for (uint32_t j = 0; j < lp.n; j++) {
			uint32_t at = (j + lp.n * LOOP_TAPS - (LOOP_HALF - 1)) % lp.n;
			float i = 0.0f, q = 0.0f;

			for (int t = 0; t < LOOP_TAPS; t++) {
				i += h[t] * (float)lp.buf[2U * at];
				q += h[t] * (float)lp.buf[2U * at + 1U];
				if (++at == lp.n) {
					at = 0U;
				}
			}
			w[j * lp.l + p] = esp_sdr_replay_word(lroundf(i), lroundf(q));
		}
	}
}

/* Fill and start the engine; the chain is tuned. */
static int loop_play(float scale)
{
	struct esp_sdr_replay_cfg cfg = {.rate = lp.rate, .count = lp.n * lp.l, .passes = 0};
	size_t words;

	/* 32767 at the input to TX_FULL at the paced path's input, whose 32767 is 511 counts. */
	loop_fill(esp_sdr_replay_buf(&words),
		  tx_level(scale) * (float)TX_FULL / 32767.0f * 511.0f / 32767.0f);
	return esp_sdr_replay_start(&cfg);
}

static int tx_loop(uint32_t rate, const int16_t *buf, size_t n, uint64_t count, float scale)
{
	size_t words;
	int ret;

	(void)esp_sdr_replay_buf(&words);
	if (n == 0U || rate == 0U) {
		return -EINVAL;
	}
	lp.l = 0U;
	for (size_t k = 0; k < ARRAY_SIZE(loop_rates) && lp.l == 0U; k++) {
		if (loop_rates[k].hz % rate == 0U) {
			lp.l = loop_rates[k].hz / rate;
			lp.rate = loop_rates[k].rate;
			if ((uint64_t)n * lp.l > words) {
				lp.l = 0U;
			}
		}
	}
	if (lp.l == 0U) {
		/* No DAC rate a whole multiple of rate, or none that holds the loop. */
		return 20000000U % rate != 0U ? -EINVAL : -E2BIG;
	}
	esdr_remote_tx_stop();
	esdr_remote_trig_stop();
	esdr_remote_rx_stop();
	tx.offset_hz = 0;
	ret = tx_tune();
	if (ret == 0) {
		lp.buf = buf;
		lp.n = (uint32_t)n;
		ret = loop_play(scale);
	}
	if (ret != 0) {
		(void)tune(set.freq_hz);
		return ret;
	}
	txb.played = 0;
	tx.rate = rate;
	tx.count = count;
	tx.t0_ms = k_uptime_get();
	tx.loop = true;
	tx.on = true;
	return 0;
}
#endif /* TX_LOOPS */

static int tx_burst(uint32_t rate, const int16_t *buf, size_t n, bool repeat, uint64_t count,
		    float scale)
{
	const size_t bank = (size_t)(__esp_sdr_bank_end - __esp_sdr_bank_start);
	unsigned int l = 8U;
	float s;
	int32_t k;
	int ret;

	if (repeat) {
#if defined(TX_LOOPS)
		return tx_loop(rate, buf, n, count, scale);
#else
		return -ENOTSUP;
#endif
	}
	if (count != 0U && count < n) {
		n = (size_t)count;
	}
	if (n == 0U || rate == 0U) {
		return -EINVAL;
	}
	if (n > bank) {
		return -E2BIG;
	}
	/* The largest factor that keeps the poll on time and an integer step. */
	while (l > 1U && ((uint64_t)rate * l > TX_WRITE_MAX || ESP_SDR_CPU_HZ % (rate * l) != 0U)) {
		l /= 2U;
	}
	esdr_remote_tx_stop();
	esdr_remote_trig_stop();
	esdr_remote_rx_stop();
	tx.offset_hz = 0;
	ret = tx_tune();
	if (ret != 0) {
		(void)tune(set.freq_hz);
		return ret;
	}
	s = tx_level(scale);
	/* Q15 of the factor from 32767 at the input to TX_FULL at the DAC's input. */
	k = (int32_t)(s * (float)TX_FULL * 32768.0f / 32767.0f);
	memset(&txb, 0, sizeof(txb));
	txb.words = __esp_sdr_bank_start;
	txb.n = (uint32_t)n;
	ret = esp_sdr_tx_paced_start(rate, l, tx.power, txb_input, NULL);
	if (ret == 0) {
		/* Under the stream's radio lock: no capture uses the bank meanwhile. */
		for (size_t j = 0; j < n; j++) {
			uint16_t i = (uint16_t)((buf[2U * j] * k) >> 15);
			uint16_t q = (uint16_t)((buf[2U * j + 1U] * k) >> 15);

			__esp_sdr_bank_start[j] = (uint32_t)i | ((uint32_t)q << 16);
		}
		txb_run();
		esp_sdr_paced_get_stats(&txb.stats);
		esp_sdr_tx_paced_stop();
		txb.played = n;
	}
	/* Back to receive at the settings' frequency. */
	(void)tune(set.freq_hz);
	return ret;
}

void esdr_local_tx_pacing(uint32_t *late, uint32_t *late_max, uint32_t *missing)
{
	*late = txb.stats.late;
	*late_max = txb.stats.late_max;
	*missing = txb.stats.events;
}
#endif /* ESDR_LOCAL_TX_BURSTS */

int esdr_remote_tx_start(uint32_t rate, const int16_t *buf, size_t n, bool repeat,
			 const struct esdr_gen *gen, uint64_t count, float scale)
{
#if defined(ESDR_LOCAL_TX_BURSTS)
	if (buf != NULL) {
		return tx_burst(rate, buf, n, repeat, count, scale);
	}
	txb.played = 0;
#endif
	ARG_UNUSED(n);
	ARG_UNUSED(repeat);
	ARG_UNUSED(scale);
	if (!HAVE_TX || buf != NULL || gen == NULL ||
	    (gen->wave != ESDR_WAVE_CONST && gen->wave != ESDR_WAVE_COS &&
	     gen->wave != ESDR_WAVE_SIN)) {
		return -ENOTSUP;
	}
	esdr_remote_tx_stop();
	esdr_remote_trig_stop();
	esdr_remote_rx_stop();
	tx.rate = rate;
	tx.count = count;
	gen_offset(gen);
	tx.t0_ms = k_uptime_get();
	tx.on = true;
	return tx_on();
}

void esdr_remote_tx_set_gen(const struct esdr_gen *gen)
{
	if (!tx.on || tx.loop) {
		return;
	}
	gen_offset(gen);
	tx_off();
	(void)tx_on();
}

void esdr_remote_tx_set_scale(float scale)
{
#if defined(TX_LOOPS)
	/* The engine owns its group while it plays: stop, refill, start again. */
	if (tx.on && tx.loop) {
		esp_sdr_replay_stop();
		if (loop_play(scale) != 0) {
			tx.loop = false;
			tx.on = false;
			(void)tune(set.freq_hz);
		}
	}
#else
	ARG_UNUSED(scale);
#endif
}

void esdr_remote_tx_stop(void)
{
	if (!tx.on) {
		return;
	}
#if defined(TX_LOOPS)
	/* What a loop played, kept for tx_stats() as a burst's is. */
	if (tx.loop) {
		uint64_t played = esdr_remote_tx_played();

		txb.played = tx.count != 0U ? MIN(played, tx.count) : played;
	}
#endif
	tx.on = false;
	tx_off();
	/* Back to receive at the settings' frequency. */
	(void)tune(set.freq_hz);
}

uint64_t esdr_remote_tx_played(void)
{
#if defined(ESDR_LOCAL_TX_BURSTS)
	/* A burst played inside tx_start. */
	if (!tx.on) {
		return txb.played;
	}
#endif
	return tx.on ? (uint64_t)(k_uptime_get() - tx.t0_ms) * tx.rate / 1000U : 0U;
}

bool esdr_remote_tx_busy(void)
{
	if (tx.on && tx.count != 0U && esdr_remote_tx_played() >= tx.count) {
		esdr_remote_tx_stop();
	}
	return tx.on;
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
			/* What every kind of run can reach. */
			.freq_min_hz = (uint64_t)LO_MIN_MHZ * 1000000U + STREAM_LO_OFFSET_HZ,
			.freq_max_hz = (uint64_t)LO_MAX_MHZ * 1000000U,
			/* All of them: esdr_rates.h. */
			.rx_rate_max_hz = stream_rates[0],
			.rx_rate_count = ARRAY_SIZE(stream_rates),
			.rx_gain_max = (uint16_t)esp_sdr_rx_gain_max(),
			.tx_gain_max = HAVE_TX ? TX_POWER_MAX : 0U,
			.full_scale16 = 16384,
		};
		strncpy(info.firmware, "esp-sdr " CONFIG_SOC_SERIES " on chip",
			sizeof(info.firmware) - 1);
		set.freq_hz = (uint64_t)esp_sdr_get_freq() * 1000000U;
		set.center_hz = set.freq_hz;
		set.lo_mhz = esp_sdr_get_freq();
		k_thread_create(&worker_thread, worker_stack, K_THREAD_STACK_SIZEOF(worker_stack),
				worker_fn, NULL, NULL, NULL, WORKER_PRIO, 0, K_FOREVER);
#if defined(WORKER_OWN_CPU)
		/*
		 * With two cores the bursts and their filtering run beside the interpreter, which
		 * with CONFIG_SCHED_CPU_MASK_PIN_ONLY stays on CPU 0 with every other thread.
		 */
		(void)k_thread_cpu_pin(&worker_thread, CONFIG_MP_MAX_NUM_CPUS - 1);
#endif
		k_thread_name_set(&worker_thread, "esdr_rx");
		k_thread_start(&worker_thread);
		ready = true;
	}
	if (out != NULL) {
		*out = &info;
	}
	return 0;
}

void esdr_remote_reset(void)
{
	esdr_remote_tx_stop();
	esdr_remote_trig_stop();
	esdr_remote_rx_stop();
	(void)esp_sdr_reset_settings();
	set.lo_offset_hz = 0U;
	set.lpf_auto_mhz = 0U;
	set.rx_gain = ESDR_GAIN_AUTO;
	set.bandwidth_hz = 0;
	set.lo_khz = 0;
	set.lo_mhz = esp_sdr_get_freq();
	set.freq_hz = (uint64_t)set.lo_mhz * 1000000U;
	set.center_hz = set.freq_hz;
#if ESDR_PANEL_BLANK
	esdr_panel_quiet = true;
#endif
}
