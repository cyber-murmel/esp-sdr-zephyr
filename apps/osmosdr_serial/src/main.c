/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The osmosdr protocol over a serial link (esdr_proto.h, serial transport):
 * USB-Serial-JTAG (ESP32-C6, ESP32-C5) or a UART to another chip (the WHY2025
 * badge), driven through its FIFOs. One thread: idle, it answers request
 * frames; streaming, it runs the CPU-paced receiver or transmitter
 * (esp_sdr_paced.h) with interrupts locked and moves the bytes between
 * samples.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/app_version.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include <hal/uart_ll.h>
#include <soc/usb_serial_jtag_reg.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_paced.h>
#if defined(CONFIG_APP_RX_DIAG)
#include <esp_sdr/esp_sdr_diag.h>
#endif
#include <esp_sdr/esp_sdr_rx.h>
#if defined(CONFIG_ESP_SDR_TRIG)
#include <esp_sdr/esp_sdr_trig.h>
#endif

#include "esdr_proto.h"
#if defined(CONFIG_APP_LINK_SDIO)
#include "link_sdio.h"
#endif

/* The paced receiver one sample at a time, either item width: what a UART or USB-Serial-JTAG
 * carries. */
#define RATE_MAX_HZ 250000U
#define RATE_COUNT 5U
/*
 * Receive rates offered: over SDIO up to 2 MS/s, 8-bit items above RATE_MAX_HZ (the tight
 * loop, esp_sdr_rx_paced_fill_iq8(); 2.5 MS/s leaves 3 % of the samples late).
 */
#if defined(CONFIG_APP_LINK_SDIO)
#define LINK_RATE_MAX_HZ 2000000U
#define LINK_RATE_COUNT 8U
#define OUT_BLOCKS 8U
#else
#define LINK_RATE_MAX_HZ RATE_MAX_HZ
#define LINK_RATE_COUNT RATE_COUNT
#endif
/*
 * Transmit DAC rate limit: the interpolating poll plus reading the samples
 * from USB keep up with 250 kS/s at the DAC on the C6 (125 kS/s x 2), 500
 * on the C5's faster CPU (125 kS/s x 4).
 */
#if defined(CONFIG_SOC_SERIES_ESP32C5)
#define TX_DAC_MAX_HZ 500000U
#else
#define TX_DAC_MAX_HZ 250000U
#endif
/* The diagnostic-bus ring delivers 2.5 MS/s divided by the decimation. */
#define DIAG_IQ_HZ 2500000U
#ifndef OUT_BLOCKS
#define OUT_BLOCKS 3U
#endif
/* The diagnostic-bus build has no RAM for more. */
#if defined(CONFIG_APP_RX_DIAG)
#define TX_RING_BYTES 8192U
#else
#define TX_RING_BYTES 16384U
#endif
/* Transmit prebuffer (tx_prebuffer()): its length, how long the first byte may take, and the
 * pause that ends a burst shorter than it. */
#define TX_PREBUF_MS 20U
#define TX_FIRST_MS  50
#define TX_QUIET_MS  3
/* Cycles one FIFO byte access takes, and the margin left before a sample is due. */
#define IO_SLACK_CYCLES 40

/* Settings as requested by the host. */
struct settings {
	uint16_t mode, bits, rx_gain, tx_gain, dgain;
	uint64_t freq_hz;
	uint32_t rate_hz, bandwidth_hz;
	int32_t corr_ppb;
	/* ESDR_REQ_SET_TRIGGER: threshold over the baseline in sixteenths, samples after. */
	uint16_t trig_ratio;
	uint32_t trig_post;
};

/* As the device starts, and again after ESDR_REQ_RESET. */
#define SETTINGS_START                                                                             \
	{                                                                                          \
		.mode = ESDR_MODE_OFF, .bits = 8, .rx_gain = ESDR_GAIN_AUTO, .tx_gain = 8,         \
		.dgain = ESDR_DGAIN_DEFAULT, .freq_hz = (uint64_t)CONFIG_APP_FREQ_MHZ * 1000000U,  \
		.rate_hz = RATE_MAX_HZ, .trig_ratio = 128, .trig_post = ESDR_TRIG_SAMPLES / 2U,     \
	}

static const struct settings want_start = SETTINGS_START;
static struct settings want = SETTINGS_START;

static uint64_t center_hz;
static uint32_t lo_mhz;
static int32_t lo_khz;
static bool radio_dirty = true;
static struct esdr_stats stats;

/*
 * The link to the host, driven through its FIFOs: the SDIO slave (CONFIG_APP_LINK_SDIO: the
 * WHY2025 badge's ESP32-C6 to its ESP32-P4, link_sdio.c), a UART where the board names one
 * (chosen esdr,link-uart), else USB-Serial-JTAG's endpoint 1 (Zephyr's driver is disabled in
 * the board overlay).
 */
#define LINK_UART DT_CHOSEN(esdr_link_uart)

#if defined(CONFIG_APP_LINK_SDIO)
static void link_init(void)
{
	sdio_link_init();
}

static inline bool link_readable(void)
{
	return sdio_link_readable();
}

static inline uint8_t link_getc(void)
{
	return sdio_link_getc();
}

static inline void link_flush(void)
{
}
#elif DT_NODE_EXISTS(LINK_UART)
static uart_dev_t *const link_hw = (uart_dev_t *)DT_REG_ADDR(LINK_UART);

/*
 * Zephyr's driver sets the UART up (current-speed); the app only polls its FIFOs. RTS drops
 * once the receive FIFO holds LINK_RTS_THRESHOLD bytes, so the host pauses while the transmit
 * ring is full, as USB does; there is no line the other way.
 */
#define LINK_RTS_THRESHOLD 96U

static void link_init(void)
{
	uart_ll_set_hw_flow_ctrl(link_hw, UART_HW_FLOWCTRL_RTS, LINK_RTS_THRESHOLD);
}

static inline bool link_readable(void)
{
	return uart_ll_get_rxfifo_len(link_hw) > 0U;
}

static inline bool link_writable(void)
{
	return uart_ll_get_txfifo_len(link_hw) > 0U;
}

static inline uint8_t link_getc(void)
{
	uint8_t b;

	uart_ll_read_rxfifo(link_hw, &b, 1);
	return b;
}

static inline void link_putc(uint8_t b)
{
	uart_ll_write_txfifo(link_hw, &b, 1);
}

static inline void link_flush(void)
{
}
#else
static uint32_t usj_pkt;

static void link_init(void)
{
}

static inline bool link_readable(void)
{
	return (sys_read32(USB_SERIAL_JTAG_EP1_CONF_REG) & USB_SERIAL_JTAG_SERIAL_OUT_EP_DATA_AVAIL) != 0U;
}

static inline bool link_writable(void)
{
	return (sys_read32(USB_SERIAL_JTAG_EP1_CONF_REG) & USB_SERIAL_JTAG_SERIAL_IN_EP_DATA_FREE) != 0U;
}

static inline uint8_t link_getc(void)
{
	return (uint8_t)sys_read32(USB_SERIAL_JTAG_EP1_REG);
}

/* A full 64-byte packet goes to the host at once. */
static inline void link_flush(void)
{
	sys_write32(USB_SERIAL_JTAG_WR_DONE, USB_SERIAL_JTAG_EP1_CONF_REG);
	usj_pkt = 0;
}

static inline void link_putc(uint8_t b)
{
	sys_write32(b, USB_SERIAL_JTAG_EP1_REG);
	if (++usj_pkt == 64U) {
		link_flush();
	}
}
#endif

/* Bytes to the host: whole receive blocks, then at most one reply frame. */
static struct {
	uint8_t blk[OUT_BLOCKS][ESDR_BLOCK_BYTES] __aligned(4);
	/* Blocks committed and sent (counters); offset into the block being sent. */
	uint32_t committed, sent, off;
	/* The block being filled, and its sample count. */
	uint32_t fill_n;
	uint16_t next_flags;
	uint64_t index;
} out;

#if defined(CONFIG_APP_LINK_SDIO)
/*
 * SDIO: committed blocks go to the DMA as they are (out.queued counts them); a block is sent
 * once the host has read its chain. Replies are copied into a bounce buffer behind them.
 */
static uint32_t out_queued;
static uint8_t bounce[2][ESDR_BLOCK_BYTES] __aligned(4);
static uint32_t bounce_next;

static void sdio_out(void)
{
	while (out_queued != out.committed &&
	       sdio_link_queue(out.blk[out_queued % OUT_BLOCKS], ESDR_BLOCK_BYTES, true)) {
		out_queued++;
	}
	sdio_link_pump();
	while (out.sent != sdio_link_blocks_done()) {
		out.sent++;
		stats.rx_blocks++;
		stats.usb_in_bytes += ESDR_BLOCK_BYTES;
	}
}

static void out_service(uint32_t max)
{
	ARG_UNUSED(max);
	sdio_out();
}

static void out_until(uint32_t due)
{
	ARG_UNUSED(due);
	sdio_out();
}

/*
 * Send a buffer now (idle, or after a stream stopped), zero padded to whole words (the host
 * reads words; the parser skips zeros between frames), giving up after 100 ms. Waits until
 * the host has read it: the bounce buffer is used again.
 */
static void write_now(const uint8_t *p, size_t n)
{
	int64_t end = k_uptime_get() + 100;
	uint8_t *b = bounce[bounce_next++ % ARRAY_SIZE(bounce)];
	size_t padded = (n + 3U) & ~3U;

	if (padded > sizeof(bounce[0])) {
		return;
	}
	memcpy(b, p, n);
	memset(b + n, 0, padded - n);
	while (!sdio_link_queue(b, padded, false)) {
		sdio_out();
		if (k_uptime_get() > end) {
			return;
		}
	}
	do {
		sdio_out();
	} while (sdio_link_pending() > 0U && k_uptime_get() <= end);
}
#else
/* Write what is queued while the FIFO takes it; at most @p max bytes. */
static void out_service(uint32_t max)
{
	while (max-- > 0U && out.sent != out.committed && link_writable()) {
		link_putc(out.blk[out.sent % OUT_BLOCKS][out.off++]);
		if (out.off == ESDR_BLOCK_BYTES) {
			out.off = 0;
			out.sent++;
			stats.rx_blocks++;
			stats.usb_in_bytes += ESDR_BLOCK_BYTES;
		}
	}
}

/*
 * Write queued bytes until @p due is near. Each byte needs room first: a
 * write to a full FIFO stalls the CPU until the host takes a packet.
 */
static void out_until(uint32_t due)
{
	while (out.sent != out.committed &&
	       (int32_t)(due - esp_sdr_paced_cycles()) > IO_SLACK_CYCLES) {
		if (!link_writable()) {
			return;
		}
		link_putc(out.blk[out.sent % OUT_BLOCKS][out.off++]);
		if (out.off == ESDR_BLOCK_BYTES) {
			out.off = 0;
			out.sent++;
			stats.rx_blocks++;
			stats.usb_in_bytes += ESDR_BLOCK_BYTES;
		}
	}
}

/* Send a short buffer now (idle, or after a stream stopped), giving up after 100 ms. */
static void write_now(const uint8_t *p, size_t n)
{
	int64_t end = k_uptime_get() + 100;

	for (size_t k = 0; k < n; k++) {
		while (!link_writable()) {
			if (k_uptime_get() > end) {
				return;
			}
		}
		link_putc(p[k]);
	}
	link_flush();
}
#endif

/* Transmit samples from the host, as received (int8 or int16 little endian pairs). */
static struct {
	uint8_t buf[TX_RING_BYTES];
	uint32_t head, tail;
} txr;

/* Request frames from the host. */
static struct {
	union {
		struct esdr_ser_hdr h;
		uint8_t raw[sizeof(struct esdr_ser_hdr)];
	};
	uint8_t data[ESDR_SER_MAX_DATA];
	uint32_t got, tx_left;
	bool ready;
} req;

/* One byte from the host. */
static void req_feed(uint8_t b)
{
	if (req.tx_left > 0U) {
		txr.buf[txr.head++ % TX_RING_BYTES] = b;
		stats.usb_out_bytes++;
		req.tx_left--;
		return;
	}
	if (req.got < sizeof(req.raw)) {
		req.raw[req.got++] = b;
		/* Resynchronize on the magic. */
		while (req.got > 0U && req.got <= 4U &&
		       memcmp(req.raw, &(uint32_t){sys_cpu_to_le32(ESDR_SER_MAGIC)}, req.got) != 0) {
			memmove(req.raw, req.raw + 1, --req.got);
		}
		if (req.got == sizeof(req.raw)) {
			if (req.h.req == ESDR_SER_TX_DATA) {
				req.tx_left = sys_le16_to_cpu(req.h.len);
				req.got = 0;
			} else if ((req.h.flags & ESDR_SER_IN) != 0U || req.h.len == 0U) {
				req.ready = true;
			} else if (sys_le16_to_cpu(req.h.len) > ESDR_SER_MAX_DATA) {
				req.h.len = 0;
				req.ready = true;
			}
		}
		return;
	}
	req.data[req.got++ - sizeof(req.raw)] = b;
	if (req.got - sizeof(req.raw) == sys_le16_to_cpu(req.h.len)) {
		req.ready = true;
	}
}

/* Read from the host while there is room for transmit data; at most @p max bytes. */
static void in_service(uint32_t max)
{
	while (max-- > 0U && !req.ready && link_readable()) {
		if (req.tx_left > 0U && txr.head - txr.tail >= TX_RING_BYTES) {
			return;
		}
		req_feed(link_getc());
	}
}

/* Read from the host until @p due is near (or nothing waits, or the transmit ring is full). */
static void in_until(uint32_t due)
{
	while (!req.ready && (int32_t)(due - esp_sdr_paced_cycles()) > IO_SLACK_CYCLES &&
	       link_readable()) {
		if (req.tx_left > 0U) {
			/* Transmit payload straight into the ring. */
			if (txr.head - txr.tail >= TX_RING_BYTES) {
				return;
			}
			txr.buf[txr.head++ % TX_RING_BYTES] =
				link_getc();
			req.tx_left--;
			stats.usb_out_bytes++;
			continue;
		}
		req_feed(link_getc());
	}
}

static uint32_t rate_round(uint32_t hz)
{
	for (uint32_t r = LINK_RATE_MAX_HZ; r > (LINK_RATE_MAX_HZ >> (LINK_RATE_COUNT - 1U));
	     r /= 2U) {
		/* Above the geometric mean of r and r / 2. */
		if (2U * (uint64_t)hz * hz >= (uint64_t)r * r) {
			return r;
		}
	}
	return LINK_RATE_MAX_HZ >> (LINK_RATE_COUNT - 1U);
}

static unsigned int dgain_db(void)
{
	if (want.dgain != ESDR_DGAIN_DEFAULT) {
		return want.dgain;
	}
	return want.bits == 8U ? 24U : 0U;
}

/* Frequency, correction, gain and filter into the radio. */
static int radio_apply(void)
{
	/* A fast crystal puts the LO high: program the opposite offset. */
	int64_t hz = (int64_t)want.freq_hz - (int64_t)want.freq_hz * want.corr_ppb / 1000000000LL;
	int ret;

	lo_mhz = (uint32_t)(hz / 1000000);
	lo_khz = (int32_t)((hz - (int64_t)lo_mhz * 1000000 + 500) / 1000);
	ret = esp_sdr_set_freq(lo_mhz);
	if (ret == 0 && lo_khz != 0) {
		ret = esp_sdr_set_freq_offset(lo_khz);
	}
	if (ret == 0) {
		ret = esp_sdr_rx_set_gain(want.rx_gain == ESDR_GAIN_AUTO ? ESP_SDR_RX_GAIN_AUTO
									 : (int)want.rx_gain);
	}
	if (ret == 0) {
		ret = want.bandwidth_hz == 0U ? esp_sdr_rx_set_lpf(ESP_SDR_RX_LPF_AUTO)
					      : esp_sdr_rx_set_bandwidth(want.bandwidth_hz / 1000000U);
	}
	/* Where the LO lands, back in the host's frequency scale. */
	center_hz = ((uint64_t)lo_mhz * 1000000U + (int64_t)lo_khz * 1000) +
		    (int64_t)want.freq_hz * want.corr_ppb / 1000000000LL;
	/* Applied once: a setting the radio refuses keeps its previous value. */
	radio_dirty = false;
#if defined(CONFIG_ESP_SDR_TRIG)
	/* The trigger's quiet baseline moves with the frequency and the gain. */
	esp_sdr_trig_reset();
#endif
	return ret;
}

/*
 * ESDR_REQ_CAPTURE_BURST: one burst at a native rate (@p mhz), sent as int16 blocks (the raw
 * 10 bits times 32, as the S3's bursts) flagged ESDR_BLK_BURST before the reply. The radio is
 * idle meanwhile (main() runs requests only between streams).
 */
static int capture_burst(uint16_t mhz, const uint8_t *data, size_t len)
{
	static uint8_t blk[ESDR_BLOCK_BYTES];
	const size_t per = ESDR_BLOCK_PAYLOAD / 4U;
	struct esp_sdr_rx_burst b;
	enum esp_sdr_rate rate;
	uint32_t n;
	int ret;

	if (len != 4U) {
		return -EINVAL;
	}
	n = sys_get_le32(data);
	for (rate = ESP_SDR_RATE_80MSPS; rate <= ESP_SDR_RATE_16MSPS; rate++) {
		if (esp_sdr_rx_rate_hz(rate) == (uint32_t)mhz * 1000000U) {
			break;
		}
	}
	if (rate > ESP_SDR_RATE_16MSPS || n < ESP_SDR_SAMPLES_MIN || n > ESP_SDR_SAMPLES_MAX) {
		return -EINVAL;
	}
	if (radio_dirty) {
		ret = radio_apply();
		if (ret != 0) {
			return ret;
		}
	}
	ret = esp_sdr_rx_capture(rate, n, &b);
	if (ret != 0) {
		return ret;
	}
	for (size_t k = 0; k < b.count; k += per) {
		size_t m = MIN(per, b.count - k);
		struct esdr_block_hdr h = {
			.magic = sys_cpu_to_le32(ESDR_BLOCK_MAGIC),
			.flags = sys_cpu_to_le16(ESDR_BLK_BURST | ESDR_BLK_16BIT),
			.samples = sys_cpu_to_le16((uint16_t)m),
			.index = sys_cpu_to_le64(k),
		};
		uint8_t *p = blk + sizeof(h);

		memcpy(blk, &h, sizeof(h));
		memset(p + 4U * m, 0, ESDR_BLOCK_PAYLOAD - 4U * m);
		for (size_t j = 0; j < m; j++) {
			sys_put_le16((uint16_t)(esp_sdr_rx_i(b.words[k + j]) * 32), p + 4U * j);
			sys_put_le16((uint16_t)(esp_sdr_rx_q(b.words[k + j]) * 32), p + 4U * j + 2U);
		}
		write_now(blk, sizeof(blk));
		stats.rx_blocks++;
		stats.usb_in_bytes += sizeof(blk);
	}
	return 0;
}

#if defined(CONFIG_ESP_SDR_TRIG)
static bool trig_stop(void *user)
{
	ARG_UNUSED(user);
	in_service(64);
	return req.ready;
}

/*
 * One snapshot to the host: ESDR_TRIG_BLOCKS int8 blocks built in the snapshot's scratch (the
 * free half of the capture bank), the newest ESDR_TRIG_SAMPLES of the ring, sent from there
 * before the ring may run again.
 */
static void trig_send(const struct esp_sdr_trig_snap *snap)
{
	const uint32_t per = ESDR_BLOCK_PAYLOAD / 2U;
	const uint32_t skip = ESP_SDR_TRIG_RING - ESDR_TRIG_SAMPLES;

	for (uint32_t k = 0; k < ESDR_TRIG_BLOCKS; k++) {
		uint8_t *blk = snap->scratch + k * ESDR_BLOCK_BYTES;
		uint32_t from = skip + k * per;
		struct esdr_block_hdr h = {
			.magic = sys_cpu_to_le32(ESDR_BLOCK_MAGIC),
			.flags = sys_cpu_to_le16(ESDR_BLK_TRIGGER | (k == 0U ? ESDR_BLK_GAP : 0U)),
			.samples = sys_cpu_to_le16((uint16_t)per),
			.index = sys_cpu_to_le64((uint64_t)((int64_t)from - (int64_t)snap->trig)),
		};

		memcpy(blk, &h, sizeof(h));
		esp_sdr_trig_pack_iq8(snap, from, per, (int8_t *)(blk + sizeof(h)));
	}
#if defined(CONFIG_APP_LINK_SDIO)
	int64_t end = k_uptime_get() + 200;

	for (uint32_t k = 0; k < ESDR_TRIG_BLOCKS; k++) {
		while (!sdio_link_queue(snap->scratch + k * ESDR_BLOCK_BYTES, ESDR_BLOCK_BYTES,
					false)) {
			sdio_link_pump();
			if (k_uptime_get() > end) {
				return;
			}
		}
	}
	do {
		sdio_link_pump();
	} while (sdio_link_pending() > 0U && k_uptime_get() <= end);
#else
	for (uint32_t k = 0; k < ESDR_TRIG_BLOCKS; k++) {
		write_now(snap->scratch + k * ESDR_BLOCK_BYTES, ESDR_BLOCK_BYTES);
	}
#endif
	stats.rx_blocks += ESDR_TRIG_BLOCKS;
	stats.usb_in_bytes += ESDR_TRIG_BLOCKS * ESDR_BLOCK_BYTES;
	stats.rx_samples += ESDR_TRIG_SAMPLES;
}

/* ESDR_MODE_TRIGGER: snapshots until a request comes. */
static void run_trig(void)
{
	struct esp_sdr_trig_cfg cfg = {
		.ratio_x16 = want.trig_ratio,
		.post = want.trig_post,
		.stop = trig_stop,
	};
	struct esp_sdr_trig_snap snap;

	while (!req.ready) {
		int ret = esp_sdr_trig_wait(&cfg, &snap);

		if (ret == -EINTR) {
			break;
		}
		if (ret != 0) {
			want.mode = ESDR_MODE_OFF;
			break;
		}
		stats.ring_runs++;
		trig_send(&snap);
	}
}

static int set_trigger(uint16_t value, const uint8_t *data, size_t len)
{
	uint32_t post = ESDR_TRIG_SAMPLES / 2U;

	if (len == 4U) {
		post = sys_get_le32(data);
	} else if (len != 0U) {
		return -EINVAL;
	}
	if (value == 0U) {
		value = 128U;
	}
	if (value <= 16U || post == 0U || post >= ESDR_TRIG_SAMPLES) {
		return -EINVAL;
	}
	want.trig_ratio = value;
	want.trig_post = post;
	return 0;
}
#endif

static int handle_out(uint8_t r, uint16_t value, const uint8_t *data, size_t len)
{
	switch (r) {
#if defined(CONFIG_ESP_SDR_TRIG)
	case ESDR_REQ_SET_TRIGGER:
		return set_trigger(value, data, len);
#endif
	case ESDR_REQ_CAPTURE_BURST:
		return capture_burst(value, data, len);
	case ESDR_REQ_RESET:
		/* radio_apply() then tunes and sets the gain and filter (and resets the trigger's
		 * baseline) before the next stream.
		 */
		want = want_start;
		radio_dirty = true;
		return 0;
	case ESDR_REQ_SET_MODE:
		/* run_tx() played what was queued before taking an ESDR_MODE_F_DRAIN request. */
		if (value == (ESDR_MODE_OFF | ESDR_MODE_F_DRAIN)) {
			value = ESDR_MODE_OFF;
		}
		if (value != ESDR_MODE_OFF && value != ESDR_MODE_RX && value != ESDR_MODE_TX &&
		    !(IS_ENABLED(CONFIG_ESP_SDR_TRIG) && value == ESDR_MODE_TRIGGER)) {
			return -ENOTSUP;
		}
		want.mode = value;
		return 0;
	case ESDR_REQ_SET_FREQ:
		if (len != 8U) {
			return -EINVAL;
		}
		{
			uint64_t hz = sys_get_le64(data);

			if (hz < (uint64_t)ESP_SDR_FREQ_MIN_MHZ * 1000000U ||
			    hz > (uint64_t)ESP_SDR_FREQ_MAX_MHZ * 1000000U) {
				return -EINVAL;
			}
			want.freq_hz = hz;
		}
		radio_dirty = true;
		return 0;
	case ESDR_REQ_SET_SAMPLE_RATE:
		if (len != 4U) {
			return -EINVAL;
		}
		want.rate_hz = rate_round(sys_get_le32(data));
		return 0;
	case ESDR_REQ_SET_RX_GAIN:
		if (value != ESDR_GAIN_AUTO && value > (uint16_t)esp_sdr_rx_gain_max()) {
			return -EINVAL;
		}
		want.rx_gain = value;
		radio_dirty = true;
		return 0;
	case ESDR_REQ_SET_TX_GAIN:
		if (value > esp_sdr_tx_paced_power_max()) {
			return -EINVAL;
		}
		want.tx_gain = value;
		return 0;
	case ESDR_REQ_SET_BANDWIDTH:
		if (len != 4U) {
			return -EINVAL;
		}
		want.bandwidth_hz = sys_get_le32(data);
		radio_dirty = true;
		return 0;
	case ESDR_REQ_SET_FORMAT:
		if (value != 8U && value != 16U) {
			return -EINVAL;
		}
		want.bits = value;
		return 0;
	case ESDR_REQ_SET_FREQ_CORR:
		if (len != 4U) {
			return -EINVAL;
		}
		want.corr_ppb = (int32_t)sys_get_le32(data);
		radio_dirty = true;
		return 0;
	case ESDR_REQ_SET_OPTIONS:
		/* No offset tuning: the paced receiver sums around the LO itself. */
		return 0;
	case ESDR_REQ_SET_DIGITAL_GAIN:
		if (value != ESDR_DGAIN_DEFAULT && (value > ESDR_DGAIN_MAX_DB || value % 6U != 0U)) {
			return -EINVAL;
		}
		want.dgain = value;
		return 0;
	default:
		return -ENOTSUP;
	}
}

static int handle_in(uint8_t r, uint8_t *buf, size_t max)
{
	switch (r) {
	case ESDR_REQ_GET_INFO: {
		struct esdr_info info = {
			.magic = ESDR_INFO_MAGIC,
			.proto_version = ESDR_PROTO_VERSION,
			.info_size = sizeof(info),
			.freq_min_hz = (uint64_t)ESP_SDR_FREQ_MIN_MHZ * 1000000U,
			.freq_max_hz = (uint64_t)ESP_SDR_FREQ_MAX_MHZ * 1000000U,
			.rx_rate_max_hz = LINK_RATE_MAX_HZ,
			.rx_rate_count = LINK_RATE_COUNT,
			.rx_gain_max = (uint16_t)esp_sdr_rx_gain_max(),
			.tx_gain_max = (uint16_t)esp_sdr_tx_paced_power_max(),
			.block_bytes = ESDR_BLOCK_BYTES,
			.full_scale16 = 32767,
		};

		strncpy(info.firmware, "osmosdr_serial " APP_VERSION_STRING, sizeof(info.firmware) - 1);
		memcpy(buf, &info, MIN(max, sizeof(info)));
		return (int)MIN(max, sizeof(info));
	}
	case ESDR_REQ_GET_STATE: {
		struct esdr_state st = {
			.mode = want.mode,
			.bits = want.bits,
			.rx_gain = want.rx_gain,
			.tx_gain = want.tx_gain,
			.digital_gain_db = (uint16_t)dgain_db(),
			.freq_hz = want.freq_hz,
			.center_hz = center_hz,
			.sample_rate_hz = want.rate_hz,
			.bandwidth_hz = want.bandwidth_hz,
			.freq_corr_ppb = want.corr_ppb,
			.lo_mhz = lo_mhz,
			.lo_offset_khz = lo_khz,
		};

		memcpy(buf, &st, MIN(max, sizeof(st)));
		return (int)MIN(max, sizeof(st));
	}
	case ESDR_REQ_GET_STATS:
		memcpy(buf, &stats, MIN(max, sizeof(stats)));
		return (int)MIN(max, sizeof(stats));
	default:
		return -ENOTSUP;
	}
}

/* Answer the pending request frame. */
static void req_answer(void)
{
	struct {
		struct esdr_ser_hdr h;
		uint8_t data[ESDR_SER_MAX_DATA];
	} __packed rep = {.h = {.magic = sys_cpu_to_le32(ESDR_SER_MAGIC), .req = req.h.req}};
	uint16_t len = sys_le16_to_cpu(req.h.len);
	int ret;

	if ((req.h.flags & ESDR_SER_IN) != 0U) {
		ret = handle_in(req.h.req, rep.data, MIN(len, sizeof(rep.data)));
	} else {
		ret = handle_out(req.h.req, sys_le16_to_cpu(req.h.value), req.data, len);
	}
	if (ret > 0) {
		rep.h.len = sys_cpu_to_le16((uint16_t)ret);
	} else {
		rep.h.status = sys_cpu_to_le16((uint16_t)(int16_t)ret);
	}
	write_now((const uint8_t *)&rep, sizeof(rep.h) + (ret > 0 ? (size_t)ret : 0U));
	req.ready = false;
	req.got = 0;
}

/* Scale factor from a sum of W inputs (each up to 512) to int16, with the digital gain, Q16. */
static uint32_t rx_scale(void)
{
	uint32_t w = ESP_SDR_PACED_INPUT_HZ / want.rate_hz;

	return (uint32_t)(((uint64_t)64U << (16U + dgain_db() / 6U)) / w);
}

/*
 * The paced loop's timing, in the fields the S3's ring reports its own in:
 * ring_errors counts polls a whole period late, ring_late_max is the worst
 * lateness in CPU cycles.
 */
static void pacing_stats(void)
{
	struct esp_sdr_paced_stats ps;

	esp_sdr_paced_get_stats(&ps);
	stats.ring_runs++;
	stats.ring_errors += ps.late;
	stats.ring_late_max = MAX(stats.ring_late_max, ps.late_max);
}

static void out_commit(void)
{
	struct esdr_block_hdr *h = (struct esdr_block_hdr *)out.blk[out.committed % OUT_BLOCKS];

	h->magic = sys_cpu_to_le32(ESDR_BLOCK_MAGIC);
	h->flags = sys_cpu_to_le16(out.next_flags | (want.bits == 16U ? ESDR_BLK_16BIT : 0U));
	h->samples = sys_cpu_to_le16((uint16_t)out.fill_n);
	h->index = sys_cpu_to_le64(out.index);
	out.index += out.fill_n;
	out.committed++;
	out.next_flags = 0;
	out.fill_n = 0;
	stats.rx_samples += sys_le16_to_cpu(h->samples);
}

/* Receive samples into the output blocks: 16-bit or 8-bit items, a block at a time. */
static bool rx_full;
static uint32_t rx_per_block;

static void rx_begin(void)
{
	rx_per_block = ESDR_BLOCK_PAYLOAD / (want.bits == 16U ? 4U : 2U);
	rx_full = false;
	out.fill_n = 0;
}

/* One sample, int16 full scale; commits the block when it is full. */
static void rx_emit(int32_t vi, int32_t vq)
{
	uint8_t *blk = out.blk[out.committed % OUT_BLOCKS] + sizeof(struct esdr_block_hdr);

	if (rx_full) {
		/* The block index moves on: the host sees the gap. */
		stats.rx_overflow_samples++;
		out.index++;
	} else if (want.bits == 16U) {
		sys_put_le16((uint16_t)vi, blk + 4U * out.fill_n);
		sys_put_le16((uint16_t)vq, blk + 4U * out.fill_n + 2U);
		out.fill_n++;
	} else {
		blk[2U * out.fill_n] = (uint8_t)(int8_t)CLAMP((vi + 128) >> 8, -127, 127);
		blk[2U * out.fill_n + 1U] = (uint8_t)(int8_t)CLAMP((vq + 128) >> 8, -127, 127);
		out.fill_n++;
	}
	if (out.fill_n == rx_per_block) {
		out_commit();
	}
	/* No free block: drop until one is, then mark the gap. */
	rx_full = out.committed - out.sent >= OUT_BLOCKS;
	if (rx_full && out.next_flags == 0U) {
		out.next_flags = ESDR_BLK_OVERFLOW | ESDR_BLK_GAP;
		stats.rx_overflows++;
	}
}

/* After a stream: whole blocks only, the partial one is dropped and the restart marked. */
static void rx_end(void)
{
	int64_t end = k_uptime_get() + 200;

	/* A host that stopped reading leaves them after 200 ms. */
	while (out.sent != out.committed && k_uptime_get() <= end) {
		out_service(64);
	}
	link_flush();
	out.fill_n = 0;
	out.next_flags = ESDR_BLK_GAP;
}

#if defined(CONFIG_APP_RX_DIAG)
/* The diagnostic-bus ring: the library decimates, this takes its blocks of samples. */
static uint64_t diag_expect;
static unsigned int diag_dgain_shift;

static void diag_iq_sink(void *user, const int32_t *iq, size_t n, uint64_t index)
{
	ARG_UNUSED(user);
	if (index != diag_expect) {
		/* Blocks of the ring were skipped: close the block in progress, move on, flag the gap. */
		if (out.fill_n != 0U) {
			out_commit();
		}
		out.index += index - diag_expect;
		out.next_flags |= ESDR_BLK_GAP;
	}
	diag_expect = index + n;
	for (size_t j = 0; j < n; j++) {
		/* Raw counts times 16 to int16 with the digital gain: x * 2^g / 16. */
		int32_t vi = (iq[2U * j] << diag_dgain_shift) >> 4;
		int32_t vq = (iq[2U * j + 1U] << diag_dgain_shift) >> 4;

		rx_emit(CLAMP(vi, -32767, 32767), CLAMP(vq, -32767, 32767));
	}
	/* Between blocks, with interrupts open: what the USB FIFO takes, and a look for a request. */
	out_service(256);
	in_service(16);
	if (req.ready) {
		esp_sdr_diag_stop();
	}
}

static void run_rx_diag(void)
{
	struct esp_sdr_diag_stats ds;
	struct esp_sdr_diag_cfg cfg = {
		.decim = (uint8_t)(DIAG_IQ_HZ / want.rate_hz),
		.lsb = ESP_SDR_DIAG_LSB_AUTO,
		.iq_sink = diag_iq_sink,
	};
	int ret;

	rx_begin();
	diag_expect = 0;
	diag_dgain_shift = dgain_db() / 6U;
	ret = esp_sdr_diag_run(&cfg, &ds);
	if (ret != 0) {
		/* Could not start, or the ring overran: the host sees the stream stop. */
		want.mode = ESDR_MODE_OFF;
	}
	stats.ring_runs++;
	stats.ring_errors += ds.skips + (ds.overrun ? 1U : 0U);
	stats.rx_ring_lost_pairs += (uint64_t)ds.skipped_blocks * (ESP_SDR_DIAG_BLOCK / 16U) / cfg.decim;
	stats.ring_cycles_x100 = ds.sink_cycles_x100;
	rx_end();
}
#endif

#if defined(CONFIG_APP_LINK_SDIO)
/* Samples per esp_sdr_rx_paced_fill_iq8() call: a quarter block, then the link's turn. */
#define FAST_CHUNK (ESDR_BLOCK_PAYLOAD / 2U / 4U)

/*
 * Above RATE_MAX_HZ, 8-bit items: the tight loop fills the blocks in place, a quarter block
 * at a time, and the DMA takes them as they are. The gain is a shift: sums of 80 MHz / rate
 * inputs, times 2^(gain / 6) / 4 per input, to the nearest power of 2 (within 3 dB of the
 * one-sample path's scale).
 */
static void run_rx_fast(void)
{
	static int8_t drop[2U * FAST_CHUNK] __aligned(4);
	uint32_t v = 4U * (ESP_SDR_PACED_INPUT_HZ / want.rate_hz);
	/* log2(v) to the nearest integer, less the digital gain. */
	int shift = 31 - __builtin_clz(v);
	unsigned int key;

	if (v - (1U << shift) > (1U << shift) / 2U) {
		shift++;
	}
	shift = MAX(0, shift - (int)(dgain_db() / 6U));
	if (esp_sdr_rx_paced_start(want.rate_hz) != 0) {
		want.mode = ESDR_MODE_OFF;
		return;
	}
	rx_begin();
	key = irq_lock();
	esp_sdr_paced_sync();
	while (!req.ready) {
		if (out.committed - out.sent >= OUT_BLOCKS) {
			/* No free block: the samples go, the next block marks the gap. */
			esp_sdr_rx_paced_fill_iq8(drop, FAST_CHUNK, (unsigned int)shift);
			out.index += FAST_CHUNK;
			stats.rx_overflow_samples += FAST_CHUNK;
			if (out.next_flags == 0U) {
				out.next_flags = ESDR_BLK_OVERFLOW | ESDR_BLK_GAP;
				stats.rx_overflows++;
			}
		} else {
			uint8_t *blk = out.blk[out.committed % OUT_BLOCKS] +
				       sizeof(struct esdr_block_hdr);

			esp_sdr_rx_paced_fill_iq8((int8_t *)blk + 2U * out.fill_n, FAST_CHUNK,
						  (unsigned int)shift);
			out.fill_n += FAST_CHUNK;
			if (out.fill_n == rx_per_block) {
				out_commit();
			}
		}
		sdio_out();
		while (!req.ready && link_readable()) {
			req_feed(link_getc());
		}
	}
	irq_unlock(key);
	esp_sdr_rx_paced_stop();
	pacing_stats();
	rx_end();
}
#endif

static void run_rx(void)
{
	uint32_t k = rx_scale();
	unsigned int key;
	int ret;

#if defined(CONFIG_APP_RX_DIAG)
	run_rx_diag();
	return;
#endif
	if (want.rate_hz > RATE_MAX_HZ) {
#if defined(CONFIG_APP_LINK_SDIO)
		if (want.bits == 8U) {
			run_rx_fast();
			return;
		}
#endif
		/* 16-bit items only up to RATE_MAX_HZ. */
		want.mode = ESDR_MODE_OFF;
		return;
	}
	ret = esp_sdr_rx_paced_start(want.rate_hz);
	if (ret != 0) {
		want.mode = ESDR_MODE_OFF;
		return;
	}
	rx_begin();
	key = irq_lock();
	esp_sdr_paced_sync();
	while (!req.ready) {
		int32_t i, q;
		if (esp_sdr_rx_paced_poll(&i, &q)) {
			rx_emit(CLAMP((int32_t)(((int64_t)i * k) >> 16), -32767, 32767),
				CLAMP((int32_t)(((int64_t)q * k) >> 16), -32767, 32767));
		}
		/* Sample instants stay exact: I/O only while a byte fits before the next one is due. */
		out_until(esp_sdr_paced_deadline());
		in_until(esp_sdr_paced_deadline());
	}
	irq_unlock(key);
	esp_sdr_rx_paced_stop();
	pacing_stats();
	rx_end();
}

static bool tx_input(void *user, int16_t *i, int16_t *q)
{
	uint32_t item = want.bits == 16U ? 4U : 2U;

	ARG_UNUSED(user);
	if (txr.head - txr.tail < item) {
		return false;
	}
	if (item == 4U) {
		*i = (int16_t)(txr.buf[txr.tail % TX_RING_BYTES] |
			       txr.buf[(txr.tail + 1U) % TX_RING_BYTES] << 8);
		*q = (int16_t)(txr.buf[(txr.tail + 2U) % TX_RING_BYTES] |
			       txr.buf[(txr.tail + 3U) % TX_RING_BYTES] << 8);
	} else {
		*i = (int16_t)((int8_t)txr.buf[txr.tail % TX_RING_BYTES] * 256);
		*q = (int16_t)((int8_t)txr.buf[(txr.tail + 1U) % TX_RING_BYTES] * 256);
	}
	txr.tail += item;
	return true;
}

/*
 * Fill the ring before the stream starts, as the S3's DAC prebuffer: TX_PREBUF_MS of samples
 * (at most three quarters of the ring), or what the host sent before it paused (a burst
 * shorter than that), or nothing if it sends nothing for TX_FIRST_MS. Without it a link as
 * fast as the stream (a UART at 5 Mbaud carries 250 kS/s of 8-bit pairs, just) leaves the
 * transmitter short from its first sample and a burst plays as noise.
 */
static void tx_prebuffer(void)
{
	uint32_t item = want.bits == 16U ? 4U : 2U;
	uint32_t fill = (uint32_t)MIN((uint64_t)want.rate_hz * item * TX_PREBUF_MS / 1000U,
				      TX_RING_BYTES / 4U * 3U);
	uint32_t quiet = TX_FIRST_MS;
	int64_t last = k_uptime_get();

	while (!req.ready && txr.head - txr.tail < fill) {
		if (link_readable()) {
			in_service(256);
			last = k_uptime_get();
			quiet = TX_QUIET_MS;
		} else if (k_uptime_get() - last >= quiet) {
			break;
		}
	}
}

/* The request waiting is ESDR_MODE_OFF with ESDR_MODE_F_DRAIN and samples are still queued. */
static bool tx_draining(void)
{
	uint32_t item = want.bits == 16U ? 4U : 2U;

	return req.h.req == ESDR_REQ_SET_MODE && (req.h.flags & ESDR_SER_IN) == 0U &&
	       sys_le16_to_cpu(req.h.value) == (ESDR_MODE_OFF | ESDR_MODE_F_DRAIN) &&
	       txr.head - txr.tail >= item;
}

static void run_tx(void)
{
	unsigned int l = 8;
	struct esp_sdr_paced_stats ps;
	unsigned int key;

	if (want.rate_hz > RATE_MAX_HZ) {
		/* Transmit stays at the one-sample rates. */
		want.mode = ESDR_MODE_OFF;
		return;
	}
	tx_prebuffer();

	/* The top rate leaves no time to interpolate: it goes to the DAC as it is. */
	uint32_t dac_max = want.rate_hz >= RATE_MAX_HZ ? RATE_MAX_HZ : TX_DAC_MAX_HZ;

	while (l > 1U &&
	       (want.rate_hz * l > dac_max || ESP_SDR_CPU_HZ % (want.rate_hz * l) != 0U)) {
		l /= 2U;
	}
	if (esp_sdr_tx_paced_start(want.rate_hz, l, want.tx_gain, tx_input, NULL) != 0) {
		want.mode = ESDR_MODE_OFF;
		return;
	}
	stats.tx_sessions++;
	key = irq_lock();
	esp_sdr_paced_sync();
	while (!req.ready || tx_draining()) {
		(void)esp_sdr_tx_paced_poll();
		in_until(esp_sdr_paced_deadline());
	}
	irq_unlock(key);
	esp_sdr_tx_paced_stop();
	pacing_stats();
	esp_sdr_paced_get_stats(&ps);
	stats.tx_samples += ps.samples - ps.events;
	stats.tx_underruns += ps.events;
}

int main(void)
{
	uint32_t stale_ms = 0;
	int ret;

	printk("osmosdr_serial %s\n", APP_VERSION_STRING);
	link_init();
	ret = esp_sdr_init();
	if (ret != 0) {
		printk("esp_sdr_init: %d\n", ret);
		return 0;
	}
	for (;;) {
		if (radio_dirty) {
			(void)radio_apply();
		}
		if (want.mode == ESDR_MODE_RX) {
			run_rx();
		} else if (want.mode == ESDR_MODE_TX) {
			run_tx();
#if defined(CONFIG_ESP_SDR_TRIG)
		} else if (want.mode == ESDR_MODE_TRIGGER) {
			run_trig();
#endif
		} else {
			uint32_t got = req.got + req.tx_left;

			in_service(64);
			if (!req.ready) {
				k_msleep(1);
				/* A frame the host abandoned halfway (it crashed): drop it after 200 ms. */
				if (got != 0U && got == req.got + req.tx_left) {
					if (++stale_ms >= 200U) {
						req.got = 0;
						req.tx_left = 0;
						stale_ms = 0;
					}
				} else {
					stale_ms = 0;
				}
			}
		}
		if (req.ready) {
			req_answer();
		}
		if (want.mode == ESDR_MODE_OFF) {
			/* Transmit data that came after the stream stopped is stale. */
			txr.tail = txr.head;
		}
	}
	return 0;
}
