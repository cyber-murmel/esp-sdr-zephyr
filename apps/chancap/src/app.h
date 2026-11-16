/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Shared between main, the shell and the encoder: the settings and the run control.
 */

#ifndef CHANCAP_APP_H_
#define CHANCAP_APP_H_

#include <stdbool.h>
#include <stdint.h>

#include <esp_sdr/esp_sdr_chancap.h>

struct cc_settings {
	struct esp_sdr_chancap_cfg cfg;
	/* ESP_SDR_RX_GAIN_AUTO or a gain index; bandwidth in MHz (0 widest, -1 the PHY's). */
	int rx_gain;
	int rx_bw_mhz;
};

struct cc_settings *cc_settings(void);
/* Apply the settings to the radio and start; stop. */
int cc_start(void);
void cc_stop(void);

struct vrt_cc_stats {
	uint32_t captures, packets, context, blocks, bytes;
	/* Captures dropped for want of a USB block, and USB errors. */
	uint32_t dropped, errors;
	/* Blocks free right now. */
	uint32_t blocks_free;
};

int vrt_cc_init(void);
/* The capture callback (esp_sdr_chancap_cb_t). */
void vrt_cc_capture(void *user, const struct esp_sdr_chancap *cap);
void vrt_cc_get_stats(struct vrt_cc_stats *st);

#endif /* CHANCAP_APP_H_ */
