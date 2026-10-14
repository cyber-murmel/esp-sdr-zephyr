/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * MODE_SPECAN on the radio: a few 80 MS/s burst captures at the LO placements of ubt_specan_plan(),
 * an FFT per frame on the vector unit, and the channel powers as upstream's SPECAN packets.
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
 * dBm of a full scale tone at the antenna with SPECAN_GAIN, times 256. Not calibrated: from the noise
 * floor measured in doc/ubertooth.md and a 7 dB noise figure. "ubt specan cal" sets it from a source.
 */
static int32_t fs_dbm_q8 = -66 * 256;

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

static int plan_sweep(struct specan_ctx *c)
{
	struct ubt_specan_cfg cfg;

	ubt_specan_cfg_default(&cfg);
	/* The PLL locks about 1817 to 2793 MHz (esp_sdr.h); stay inside. */
	cfg.lo_min_mhz = 1900;
	cfg.lo_max_mhz = 2700;
	return ubt_specan_plan(&c->plan, c->dev->low_freq, c->dev->high_freq, &cfg);
}

void ubt_specan_run(struct ubt_dev *d)
{
	struct specan_ctx *c;
	size_t bytes;
	uint32_t cfg_seen;
	struct ubt_specan_cal cal = {.fs_dbm_q8 = fs_dbm_q8, .flat = NULL, .mirror = false};
	int ret;

	ubt_radio_rx_stop();
	ubt_radio_take();
	c = ubt_radio_scratch(&bytes);
	if (bytes < sizeof(*c)) {
		LOG_ERR("specan state does not fit (%u > %u)", (unsigned int)sizeof(*c),
			(unsigned int)bytes);
		goto out;
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
	cfg_seen = d->cfg_gen;
	if (plan_sweep(c) != 0) {
		LOG_ERR("specan: no plan for %u to %u MHz", d->low_freq, d->high_freq);
		d->requested_mode = MODE_IDLE;
		goto restore;
	}
	while (d->requested_mode == MODE_SPECAN) {
		struct esp_sdr_rx_burst burst;

		if (cfg_seen != d->cfg_gen) {
			cfg_seen = d->cfg_gen;
			if (plan_sweep(c) != 0) {
				d->requested_mode = MODE_IDLE;
				break;
			}
		}
		for (unsigned int k = 0; k < c->plan.n_lo; k++) {
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
		ubt_specan_pack_sweep(&c->packer, c->plan.low, c->plan.high, c->raw);
	}
restore:
	(void)esp_sdr_rx_set_bandwidth(13);
	(void)esp_sdr_rx_set_gain(ESP_SDR_RX_GAIN_AUTO);
out:
	ubt_radio_give();
}
