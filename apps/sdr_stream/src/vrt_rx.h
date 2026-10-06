/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SDR_STREAM_VRT_RX_H_
#define SDR_STREAM_VRT_RX_H_

#include <stdint.h>

#include <esp_sdr/esp_sdr.h>

/* One captured burst, handed from the capture core to the stream core. */
struct rx_burst {
	uint32_t seq;
	uint32_t count;
	uint64_t timestamp_ns;
	uint32_t freq_mhz;
	uint32_t rate_hz;
	int gain; /* ESP_SDR_RX_GAIN_AUTO or a fixed index */
	int32_t full_scale; /* 512 raw (10-bit), 32768 decimated */
	struct esp_sdr_iq16 iq[CONFIG_APP_RX_BURST_SAMPLES];
};

/* Smallest decimation: a decimated burst (16380 / m - 3 samples) must fit a burst. */
#define RX_DECIM_MIN 16U

/* Receive stream rate: the capture rate (raw) or capture rate / m, RX_DECIM_MIN..160. */
int rx_set_rate(uint32_t hz);
uint32_t rx_get_rate(void);

/* Decimation algorithm applied at rx_set_rate()'s factor, m > 1. */
enum rx_decim_mode {
	RX_DECIM_CIC,  /* low-pass + decimate: a clean narrowband signal, out-of-band dropped */
	RX_DECIM_FOLD, /* fold (decimate in frequency): full-band power estimate, aliased */
};

int rx_set_mode(enum rx_decim_mode mode);
enum rx_decim_mode rx_get_mode(void);

/* Signal data item size: 8, 12 or 16 bits (see iq_pack.h). */
int vrt_rx_set_bits(unsigned int bits);
unsigned int vrt_rx_get_bits(void);

int vrt_rx_init(void);
/* Called on the process core for every burst. */
void vrt_rx_burst(const struct rx_burst *b);
/* Called once per second after the statistics line. */
void vrt_rx_report(void);

#endif /* SDR_STREAM_VRT_RX_H_ */
