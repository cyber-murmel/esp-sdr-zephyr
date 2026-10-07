/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RX_STREAM_STREAM_H_
#define RX_STREAM_STREAM_H_

#include <stdint.h>

struct iq16 {
	int16_t i;
	int16_t q;
};

/* One captured burst, handed from the capture core to the stream core. */
struct burst {
	uint32_t seq;
	uint32_t count;
	uint64_t timestamp_ns;
	uint32_t freq_mhz;
	uint32_t rate_hz;
	int gain; /* ESP_SDR_GAIN_AUTO or a fixed index */
	int32_t full_scale; /* 512 raw (10-bit), 32768 decimated */
	struct iq16 iq[CONFIG_APP_BURST_SAMPLES];
};

/* Smallest decimation: a decimated burst (16380 / m - 3 samples) must fit a burst. */
#define RX_DECIM_MIN 16U

/* Receive stream rate: the capture rate (raw) or capture rate / m, RX_DECIM_MIN..160. */
int rx_set_rate(uint32_t hz);
uint32_t rx_get_rate(void);

/* Signal data item size: 8, 12 or 16 bits (see iq_pack.h). */
int stream_set_bits(unsigned int bits);
unsigned int stream_get_bits(void);

int stream_init(void);
/* Called on the process core for every burst. */
void stream_burst(const struct burst *b);
/* Called once per second after the statistics line. */
void stream_report(void);

#endif /* RX_STREAM_STREAM_H_ */
