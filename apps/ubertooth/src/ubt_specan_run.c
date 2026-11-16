/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * MODE_SPECAN on the radio: a few 80 MS/s burst captures at the LO placements of ubt_specan_plan(),
 * an FFT per frame on the vector unit, and the channel powers as upstream's SPECAN packets.
 * MODE_LED_SPECAN measures the same way.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_rx.h>

#include "ubt_mode.h"
#include "ubt_pie.h"
#include "ubt_radio.h"
#include "ubt_rssi.h"
#include "ubt_specan.h"
#include "ubt_usb.h"

LOG_MODULE_DECLARE(ubt_mode);

/* Frames averaged per placement (at most UBT_SPECAN_FRAMES_MAX; one capture holds 63). */
#define FRAMES 32U

/* Receive gain index of the spectrum analyser: fixed, the AGC would move the levels. */
#define SPECAN_GAIN 60

/* Analog filter bandwidth in MHz (two-sided) for the wide captures. */
#define SPECAN_BW_MHZ 69

/*
 * dBm of a full scale tone at the antenna with SPECAN_GAIN, times 256: the receiver's placeholder
 * calibration (ubt_rssi.h). "ubt specan cal" sets it from a source.
 */
static int32_t fs_dbm_q8 = UBT_RSSI_K_DEFAULT * 128;

struct specan_ctx {
	struct ubt_specan sp;
	_Alignas(16) int16_t w[UBT_SPECAN_N];
	struct ubt_specan_packer packer;
	struct ubt_specan_plan plan;
	struct ubt_dev *dev;
	int8_t raw[UBT_SPECAN_LO_MAX * 2U + 128U];
};

static void packer_clock(void *user, uint8_t *clkn_high, uint32_t *clk100ns)
{
	struct specan_ctx *c = user;

	ubt_clock_split(ubt_clock_t100(&c->dev->clock, c->dev->plat->cycles()), clkn_high, clk100ns);
}

static void packer_emit(void *user, const usb_pkt_rx *pkt)
{
	struct specan_ctx *c = user;
	usb_pkt_rx *slot = ubt_queue_alloc(&c->dev->queue);

	if (slot == NULL) {
		return;
	}
	*slot = *pkt;
	ubt_queue_commit(&c->dev->queue);
	ubt_usb_kick();
}

void ubt_specan_set_fs_dbm(int dbm_q8)
{
	fs_dbm_q8 = dbm_q8;
}

static void specan_close(void);

/* Radio and state for a sweep of low..high MHz; NULL when it cannot be had. */
static struct specan_ctx *specan_open(struct ubt_dev *d, uint16_t low, uint16_t high)
{
	struct ubt_specan_cfg cfg;
	struct specan_ctx *c;
	size_t bytes;
	int ret;

	ubt_radio_rx_stop();
	ubt_radio_take();
	c = ubt_radio_scratch(&bytes);
	if (bytes < sizeof(*c)) {
		LOG_ERR("specan state does not fit (%u > %u)", (unsigned int)sizeof(*c),
			(unsigned int)bytes);
		ubt_radio_give();
		return NULL;
	}
	memset(c, 0, sizeof(*c));
	c->dev = d;
	ubt_pie_init(c->w);
	ubt_specan_init(&c->sp, ubt_pie_fft256, UBT_SPECAN_IN_SHIFT);
	ubt_specan_packer_init(&c->packer, packer_clock, packer_emit, c);
	(void)esp_sdr_rx_set_bandwidth(SPECAN_BW_MHZ);
	ret = esp_sdr_rx_set_gain(SPECAN_GAIN);
	if (ret != 0) {
		LOG_WRN("fixed gain not available (%d): AGC", ret);
		(void)esp_sdr_rx_set_gain(ESP_SDR_RX_GAIN_AUTO);
	}
	ubt_specan_cfg_default(&cfg);
	/* The PLL locks about 1817 to 2793 MHz (esp_sdr.h); stay inside. */
	cfg.lo_min_mhz = 1900;
	cfg.lo_max_mhz = 2700;
	if (ubt_specan_plan(&c->plan, low, high, &cfg) != 0) {
		LOG_ERR("specan: no plan for %u to %u MHz", low, high);
		specan_close();
		return NULL;
	}
	return c;
}

static void specan_close(void)
{
	(void)esp_sdr_rx_set_bandwidth(13);
	(void)esp_sdr_rx_set_gain(ESP_SDR_RX_GAIN_AUTO);
	ubt_radio_give();
}

/* One sweep of the plan into c->raw (the raw RSSI byte per MHz from plan.low). */
static void sweep(struct specan_ctx *c)
{
	struct ubt_specan_cal cal = {.fs_dbm_q8 = fs_dbm_q8, .flat = NULL, .mirror = false};

	for (unsigned int k = 0; k < c->plan.n_lo; k++) {
		struct esp_sdr_rx_burst burst;

		if (esp_sdr_set_freq(c->plan.lo_mhz[k]) != 0 ||
		    esp_sdr_rx_capture_bank(ESP_SDR_RATE_80MSPS, UBT_SPECAN_CAPTURE, 0, &burst) != 0) {
			LOG_WRN("specan capture failed");
			k_sleep(K_MSEC(10));
			continue;
		}
		ubt_specan_begin(&c->sp);
		(void)ubt_specan_capture(&c->sp, burst.words, burst.count, UBT_SPECAN_N, FRAMES);
		(void)ubt_specan_collect(&c->sp, &c->plan, k, &cal, c->raw);
	}
}

void ubt_specan_run(struct ubt_dev *d)
{
	struct specan_ctx *c = specan_open(d, d->low_freq, d->high_freq);
	uint32_t cfg_seen = d->cfg_gen;

	if (c == NULL) {
		d->requested_mode = MODE_IDLE;
		return;
	}
	while (d->requested_mode == MODE_SPECAN) {
		if (cfg_seen != d->cfg_gen) {
			cfg_seen = d->cfg_gen;
			specan_close();
			c = specan_open(d, d->low_freq, d->high_freq);
			if (c == NULL) {
				d->requested_mode = MODE_IDLE;
				return;
			}
		}
		sweep(c);
		ubt_specan_pack_sweep(&c->packer, c->plan.low, c->plan.high, c->raw);
	}
	specan_close();
}

/*
 * MODE_LED_SPECAN (upstream led_specan()): the power at the centres of Wi-Fi channels 1, 6 and 11
 * against the threshold of the request lights the TX, RX and USR LED. Here one sweep measures all
 * three; the request's LED state follows upstream (GET_TXLED and friends read it), the board's
 * one LED lights when any of the three is above the threshold.
 */
void ubt_led_specan_run(struct ubt_dev *d)
{
	static const uint16_t wifi[3] = {2412, 2437, 2462};
	struct specan_ctx *c = specan_open(d, wifi[0], wifi[2]);

	if (c == NULL) {
		d->requested_mode = MODE_IDLE;
		return;
	}
	while (d->requested_mode == MODE_LED_SPECAN) {
		bool any = false;

		sweep(c);
		for (unsigned int i = 0; i < 3U; i++) {
			bool on = c->raw[wifi[i] - c->plan.low] > d->rssi_threshold;

			/* Upstream: channel 1 on the TX LED (2), 6 on RX (1), 11 on USR (0). */
			d->leds[2U - i] = on;
			any = any || on;
		}
		if (d->plat->led != NULL) {
			d->plat->led(0, any);
		}
		/* Upstream reads each channel about every 0.3 ms; a few sweeps a second light the LED. */
		k_sleep(K_MSEC(20));
	}
	if (d->plat->led != NULL) {
		d->plat->led(0, d->leds[0]);
	}
	specan_close();
}
