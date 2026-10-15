/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The receive modes on the 4 MS/s stream: BLE packet sniffing on one channel (MODE_BT_FOLLOW_LE
 * without following, MODE_BT_PROMISC_LE to come). The mode thread runs the demodulator and the packet
 * finder on the samples the ring thread delivers (ubt_radio).
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <esp_sdr/esp_sdr.h>

#include "gfsk_demod.h"
#include "ubt_le.h"
#include "ubt_mode.h"
#include "ubt_radio.h"
#include "ubt_usb.h"

LOG_MODULE_DECLARE(ubt_mode);

/* Samples per pass through the demodulator (64 us). */
#define CHUNK 256U

/* Receive gain index of the sniffer (fixed: the AGC would move the levels between packets). */
#define RX_GAIN 60

/* dBm of a full scale tone at that gain: a guess from the noise floor, see ubt_specan_run.c. */
#define FS_DBM (-66)

/* The squelch opens this far above the noise floor (0.5 dB units) and closes 3 dB under that. */
#define SQUELCH_MARGIN 14U
#define SQUELCH_HOLD   16U

/* The noise floor is measured over this many samples before the squelch is set. */
#define FLOOR_SAMPLES 120000U

struct rx_high {
	int16_t i[CHUNK];
	int16_t q[CHUNK];
	struct gfsk_sym syms[GFSK_OUT_CAP(CHUNK)];
	struct gfsk_demod dm;
	struct ubt_le le;
};

/* The receive state is touched every sample: internal RAM (the high region). */
static struct rx_high rxh ESP_SDR_HIGH_RAM;
static struct ubt_dev *rx_dev;

static void emit_pkt(void *ctx, const usb_pkt_rx *pkt)
{
	struct ubt_dev *d = ctx;
	usb_pkt_rx *slot = ubt_queue_alloc(&d->queue);

	if (slot == NULL) {
		return;
	}
	*slot = *pkt;
	ubt_queue_commit(&d->queue);
	ubt_usb_kick();
}

static uint32_t clk_of_sample(void *ctx, uint32_t sample_index)
{
	struct ubt_dev *d = ctx;
	uint8_t high;
	uint32_t clk;

	ubt_clock_split(ubt_clock_t100(&d->clock, ubt_radio_rx_cycles(sample_index)), &high, &clk);
	return clk;
}

/* Symbol power (0.5 dB steps from -100 dBFS) to the CC2400 style RSSI byte, dBm + 54. */
static int8_t rssi_of_pwr(uint8_t pwr)
{
	int dbm = (int)pwr / 2 - 100 + FS_DBM;
	int raw = dbm + UBT_RSSI_OFFSET;

	return (int8_t)(raw < -128 ? -128 : (raw > 127 ? 127 : raw));
}

/* Run n samples of the stream; returns false if the stream is gone. */
static void process(struct rx_high *h, size_t n, uint32_t gap)
{
	size_t ns;

	if (gap != 0U) {
		gfsk_demod_gap(&h->dm, gap);
	}
	ns = gfsk_demod_push(&h->dm, h->i, h->q, n, h->syms, ARRAY_SIZE(h->syms));
	if (ns != 0U) {
		(void)ubt_le_feed(&h->le, h->syms, ns);
	}
}

/* Feed the stream until @p samples have gone by (noise floor measurement); false on a timeout. */
static bool pump(struct rx_high *h, uint32_t samples, int64_t timeout_ms)
{
	uint32_t done = 0;
	int64_t end = k_uptime_get() + timeout_ms;

	while (done < samples && k_uptime_get() < end) {
		uint32_t first;
		uint32_t gap;
		size_t n = ubt_radio_rx_read(h->i, h->q, CHUNK, &first, &gap);

		if (n == 0U) {
			k_sleep(K_USEC(100));
			continue;
		}
		process(h, n, gap);
		done += (uint32_t)n;
	}
	return done >= samples;
}

void ubt_btle_run(struct ubt_dev *d)
{
	struct rx_high *h = &rxh;
	uint16_t channel = d->channel;
	uint32_t cfg_seen = d->cfg_gen;
	struct ubt_rx_stats st;
	uint8_t floor;
	int ret;

	rx_dev = d;
	if (ubt_le_chan_idx(channel) > 39U) {
		LOG_WRN("channel %u MHz is not a BLE channel", channel);
		d->requested_mode = MODE_IDLE;
		return;
	}
	ret = ubt_radio_rx_start(channel, RX_GAIN);
	if (ret != 0) {
		LOG_ERR("receive stream did not start (%d)", ret);
		d->requested_mode = MODE_IDLE;
		return;
	}
	gfsk_demod_init(&h->dm, GFSK_BLE);
	ubt_le_init(&h->le, emit_pkt, clk_of_sample, rssi_of_pwr, d);
	/* Upstream resets the connection state when a sniffing mode starts: advertising access address. */
	ubt_le_set_channel(&h->le, channel);
	ubt_le_set_crc_verify(&h->le, d->le.crc_verify);
	ubt_le_set_target_raw(&h->le, d->le.target.set, d->le.target.addr, d->le.target.mask);

	/* The squelch follows the noise floor measured on this channel. */
	(void)pump(h, FLOOR_SAMPLES, 400);
	floor = gfsk_demod_level(&h->dm);
	gfsk_demod_set_squelch(&h->dm, (uint8_t)(floor + SQUELCH_MARGIN), SQUELCH_HOLD, false);
	LOG_INF("BLE sniffer on %u MHz, noise floor pwr %u, squelch %u", channel, floor,
		floor + SQUELCH_MARGIN);

	while (d->requested_mode == MODE_BT_FOLLOW_LE) {
		uint32_t first;
		uint32_t gap;
		size_t n;

		if (cfg_seen != d->cfg_gen) {
			cfg_seen = d->cfg_gen;
			if (d->channel != channel && ubt_le_chan_idx(d->channel) <= 39U) {
				/* SET_CHANNEL while sniffing: retune (3 to 4 ms), the finder starts over. */
				channel = d->channel;
				ubt_radio_rx_stop();
				if (ubt_radio_rx_start(channel, RX_GAIN) != 0) {
					break;
				}
				gfsk_demod_reset(&h->dm);
				ubt_le_set_channel(&h->le, channel);
				(void)pump(h, FLOOR_SAMPLES, 400);
				floor = gfsk_demod_level(&h->dm);
				gfsk_demod_set_squelch(&h->dm, (uint8_t)(floor + SQUELCH_MARGIN),
						       SQUELCH_HOLD, false);
			}
			ubt_le_set_crc_verify(&h->le, d->le.crc_verify);
			ubt_le_set_target_raw(&h->le, d->le.target.set, d->le.target.addr,
					      d->le.target.mask);
		}
		n = ubt_radio_rx_read(h->i, h->q, CHUNK, &first, &gap);
		if (n == 0U) {
			k_sleep(K_USEC(100));
			continue;
		}
		process(h, n, gap);
	}
	ubt_radio_rx_get_stats(&st);
	LOG_INF("BLE sniffer stopped: %u hits, %u packets, %u bad CRC, %u overrun samples",
		h->le.stats.hits, h->le.stats.packets, h->le.stats.crc_bad,
		(unsigned int)st.overrun_samples);
	ubt_radio_rx_stop();
}
