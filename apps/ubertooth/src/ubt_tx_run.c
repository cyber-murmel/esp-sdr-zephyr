/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The transmit modes on the DAC: BTLE_SLAVE (an ADV_IND every 100 ms), TX_TEST (a looped pattern or
 * carrier) and TX_GENERIC (one CC2400 style packet). The waveform comes from gfsk_mod, a pure function
 * of the sample index that the DAC backend's filler calls for its blocks; the tables and the bits stay in
 * internal RAM (the sample ring's, the receive stream being stopped) until the playback returns.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_tx.h>

#include "gfsk_mod.h"
#include "ubt_mode.h"
#include "ubt_radio.h"
#include "ubt_tx.h"

LOG_MODULE_DECLARE(ubt_mode);

/* Advertising interval of BTLE_SLAVE (upstream msleep(100)). */
#define ADV_INTERVAL_MS 100

/* Digital amplitude: the analog chain compresses, the power is set with the TX gain step. */
#define TX_AMP GFSK_MOD_AMP

struct tx_ctx {
	struct gfsk_lut lut;
	struct gfsk_tab tab;
	struct ubt_tx_pkt pkt;
	struct gfsk_mod mod;
};

/* PALEVEL 0 to 7 (CC2400 FREND bits) onto the TX gain steps 0 to 17. */
static int tx_gain_for(const struct ubt_dev *d)
{
	int g = (int)d->pa_level * 17 / 7;
	int max = esp_sdr_tx_gain_max();

	return g > max ? max : g;
}

static struct tx_ctx *tx_begin(struct ubt_dev *d)
{
	struct tx_ctx *c;
	size_t bytes;

	ubt_radio_rx_stop();
	ubt_radio_take();
	c = ubt_radio_scratch(&bytes);
	if (bytes < sizeof(*c)) {
		LOG_ERR("tx state does not fit (%u > %u)", (unsigned int)sizeof(*c), (unsigned int)bytes);
		ubt_radio_give();
		return NULL;
	}
	memset(c, 0, sizeof(*c));
	gfsk_lut_init(&c->lut, TX_AMP, false);
	if (esp_sdr_tx_set_gain(tx_gain_for(d)) != 0) {
		LOG_WRN("tx gain %d refused", tx_gain_for(d));
	}
	return c;
}

static void tx_end(void)
{
	ubt_radio_give();
}

/* Tables and modulator for the packet in c->pkt; the LO goes below the channel by the IF. Samples, or 0. */
static uint32_t tx_prepare(struct tx_ctx *c)
{
	struct gfsk_burst b;
	uint32_t total;
	int ret;

	ubt_tx_burst(&c->pkt, &b);
	gfsk_tab_init(&c->tab, c->pkt.dev_hz, GFSK_MOD_IF_HZ);
	total = gfsk_mod_start(&c->mod, &c->tab, &c->lut, &b);
	if (total == 0U) {
		return 0;
	}
	ret = esp_sdr_set_freq((uint32_t)c->pkt.channel_mhz - GFSK_MOD_LO_OFFSET_MHZ);
	if (ret != 0) {
		LOG_WRN("tune to %u MHz failed (%d)", c->pkt.channel_mhz, ret);
		return 0;
	}
	return total;
}

static void idle_wait(struct ubt_dev *d, uint8_t mode, int ms)
{
	while (ms > 0 && d->requested_mode == mode) {
		k_sleep(K_MSEC(MIN(ms, 10)));
		ms -= 10;
	}
}

void ubt_btle_slave_run(struct ubt_dev *d)
{
	struct tx_ctx *c = tx_begin(d);
	uint32_t total;
	int ret;

	if (c == NULL) {
		d->requested_mode = MODE_IDLE;
		return;
	}
	ret = ubt_tx_build_adv(&c->pkt, d->slave_mac, d->adv_data, d->adv_len, d->channel,
			       UBT_TX_ADV_CRC_INIT_REV);
	if (ret < 0) {
		LOG_ERR("advertising packet: %d", ret);
		d->requested_mode = MODE_IDLE;
		tx_end();
		return;
	}
	total = tx_prepare(c);
	if (total == 0U) {
		d->requested_mode = MODE_IDLE;
		tx_end();
		return;
	}
	d->plat->led(2, true);
	/* Spam advertising packets, as upstream does. */
	while (d->requested_mode == MODE_BT_SLAVE_LE) {
		ret = esp_sdr_tx_dac_play_gen(gfsk_mod_gen_cb, &c->mod, total);
		if (ret != 0) {
			LOG_WRN("advertising playback: %d", ret);
		}
		idle_wait(d, MODE_BT_SLAVE_LE, ADV_INTERVAL_MS);
	}
	d->plat->led(2, false);
	tx_end();
}

void ubt_tx_test_run(struct ubt_dev *d)
{
	struct tx_ctx *c = tx_begin(d);
	uint32_t total;
	uint32_t *buf;
	size_t words;

	if (c == NULL) {
		d->requested_mode = MODE_IDLE;
		return;
	}
	if (ubt_tx_build_test(&c->pkt, d->modulation, d->channel, UBT_TX_TEST_PRBS, 0x1ff) < 0) {
		d->requested_mode = MODE_IDLE;
		tx_end();
		return;
	}
	total = tx_prepare(c);
	if (total == 0U || esp_sdr_tx_loop_begin(ESP_SDR_RATE_40MSPS) != 0) {
		LOG_WRN("tx test did not start");
		d->requested_mode = MODE_IDLE;
		tx_end();
		return;
	}
	buf = esp_sdr_tx_loop_buf(0, &words);
	if (buf == NULL || words < total) {
		esp_sdr_tx_loop_end();
		d->requested_mode = MODE_IDLE;
		tx_end();
		return;
	}
	gfsk_mod_gen(&c->mod, buf, 0, total);
	if (esp_sdr_tx_loop_start(0, total) == 0) {
		d->plat->led(2, true);
		idle_wait(d, MODE_TX_TEST, INT32_MAX / 2);
	}
	esp_sdr_tx_loop_halt();
	esp_sdr_tx_loop_end();
	d->plat->led(2, false);
	tx_end();
}

void ubt_tx_generic_run(struct ubt_dev *d)
{
	struct tx_ctx *c = tx_begin(d);
	struct ubt_tx_generic g;
	uint32_t total;

	if (c == NULL) {
		d->requested_mode = MODE_IDLE;
		return;
	}
	g.synch = d->tx_pkt.synch;
	g.syncl = d->tx_pkt.syncl;
	g.channel = d->tx_pkt.channel;
	g.length = d->tx_pkt.length;
	g.pa_level = d->tx_pkt.pa_level;
	memcpy(g.data, d->tx_pkt.data, sizeof(g.data));
	if (ubt_tx_build_generic(&c->pkt, &g) >= 0) {
		total = tx_prepare(c);
		if (total != 0U) {
			d->plat->led(2, true);
			(void)esp_sdr_tx_dac_play_gen(gfsk_mod_gen_cb, &c->mod, total);
			d->plat->led(2, false);
		}
	}
	/* One packet, then back (upstream restores the previous mode). */
	d->requested_mode = MODE_IDLE;
	tx_end();
}
