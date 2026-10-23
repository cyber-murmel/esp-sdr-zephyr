/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The osmosdr protocol over a serial link (esdr_proto.h, serial transport):
 * USB-Serial-JTAG (ESP32-C6, ESP32-C5) or a UART to another chip (the WHY2025
 * badge) or to a USB bridge (the MCH2022 badge's RP2040), driven through its
 * FIFOs. One thread: idle, it answers request frames; streaming, it runs the
 * CPU-paced receiver or transmitter (esp_sdr_paced.h) with interrupts locked
 * and moves the bytes between samples. A chip without the paced receiver (the
 * classic ESP32) streams decimated bursts instead, with gaps, and does not
 * transmit.
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
#if !defined(CONFIG_APP_LINK_SDIO) && !DT_HAS_CHOSEN(esdr_link_uart)
#include <soc/usb_serial_jtag_reg.h>
#endif

#include <esp_sdr/esp_sdr.h>
#if defined(CONFIG_ESP_SDR_PACED)
#include <esp_sdr/esp_sdr_paced.h>
#endif
#if defined(CONFIG_APP_RX_DIAG)
#include <esp_sdr/esp_sdr_diag.h>
#endif
#include <esp_sdr/esp_sdr_rx.h>
#if defined(CONFIG_ESP_SDR_TRIG)
#include <esp_sdr/esp_sdr_trig.h>
#endif
#if defined(CONFIG_ESP_SDR_GAPLESS)
#include <esp_sdr/esp_sdr_gapless.h>
#endif

#include "esdr_dev.h"
#include "esdr_proto.h"
#if defined(CONFIG_ESP_SDR_GAPLESS)
#if !defined(CONFIG_APP_LINK_SDIO)
#error "the gapless receiver needs the SDIO link (CONFIG_APP_LINK_SDIO)"
#endif
/* The sink writes blocks with interrupts off: the codec's functions in IRAM too. */
#define ESDR_SEG_HOT ESP_SDR_GAPLESS_HOT
#include "esdr_seg.h"
/* What the gapless run calls with interrupts off lives in IRAM. */
#define APP_HOT ESP_SDR_GAPLESS_HOT
/*
 * The ring takes SRAM blocks 1 and 2 and the image must fit block 0: the large buffers go to
 * block 3 (not zeroed at boot: main() clears them).
 */
#define APP_HIGH ESP_SDR_HIGH_RAM
#else
#define APP_HOT
#define APP_HIGH
#endif
#if defined(CONFIG_APP_LINK_SDIO)
#include "link_sdio.h"
#endif
#if defined(CONFIG_APP_FW_UPDATE)
#include "fw_update.h"
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
#elif !defined(CONFIG_ESP_SDR_PACED)
/*
 * Without the paced receiver: 16 MS/s bursts decimated by 8 to 128 (esp_sdr_rx.h), each about
 * 1 ms contiguous, as many as the link carries; the gaps between them move the block index.
 */
#define BURST_IN_HZ 16000000U
#define LINK_RATE_MAX_HZ 2000000U
#define LINK_RATE_COUNT 5U
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
	/* ESDR_REQ_SET_GAPLESS. */
	struct esdr_gapless_cfg gapless;
};

/* As the device starts, and again after ESDR_REQ_RESET. */
#define SETTINGS_START                                                                             \
	{                                                                                          \
		.mode = ESDR_MODE_OFF, .bits = 8, .rx_gain = ESDR_GAIN_AUTO, .tx_gain = 8,         \
		.dgain = ESDR_DGAIN_DEFAULT, .freq_hz = (uint64_t)CONFIG_APP_FREQ_MHZ * 1000000U,  \
		.rate_hz = RATE_MAX_HZ, .trig_ratio = 128, .trig_post = ESDR_TRIG_SAMPLES / 2U,     \
		.gapless = {.ratio_x16 = 64, .pre = 1, .post = 1},                                 \
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
} out APP_HIGH;

#if defined(CONFIG_APP_LINK_SDIO)
/*
 * SDIO: committed blocks go to the DMA as they are (out.queued counts them); a block is sent
 * once the host has read its chain. Replies are copied into a bounce buffer behind them.
 */
static uint32_t out_queued;
static uint8_t bounce[2][ESDR_BLOCK_BYTES] __aligned(4) APP_HIGH;
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

#if defined(CONFIG_ESP_SDR_PACED)
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
#endif

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
static APP_HOT void req_feed(uint8_t b)
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
static APP_HOT void in_service(uint32_t max)
{
	while (max-- > 0U && !req.ready && link_readable()) {
		if (req.tx_left > 0U && txr.head - txr.tail >= TX_RING_BYTES) {
			return;
		}
		req_feed(link_getc());
	}
}

#if defined(CONFIG_ESP_SDR_PACED)
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
#endif

/* Transmit power steps; none without the paced transmitter. */
static unsigned int tx_power_max(void)
{
#if defined(CONFIG_ESP_SDR_PACED)
	return esp_sdr_tx_paced_power_max();
#else
	return 0U;
#endif
}

static unsigned int dgain_db(void)
{
	if (want.dgain != ESDR_DGAIN_DEFAULT) {
		return want.dgain;
	}
	/* The paced sums of a few inputs sit low in 8-bit items; the bursts' filter fills int16. */
	return want.bits == 8U && IS_ENABLED(CONFIG_ESP_SDR_PACED) ? 24U : 0U;
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
#if defined(CONFIG_ESP_SDR_GAPLESS)
	esp_sdr_gapless_reset();
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

#if defined(CONFIG_ESP_SDR_GAPLESS)
/*
 * ESDR_MODE_GAPLESS: the whole 80 MHz without a gap (esp_sdr_gapless.h), its kept windows as
 * segments (esdr_seg.h) in the stream's blocks, queued to the link straight from them.
 * Every sample keeps its index on the 80 MS/s axis: a run's own index counts from the
 * systimer's time at its start, so the axis goes on across the runs a request or a ring error
 * starts anew (the first segment of a run is flagged ESDR_SEG_RESTART).
 */
/* The sink's blocks are the stream's (out.blk). */
#define GL_BLOCKS OUT_BLOCKS
/* A block that has waited this long goes out part full (1 ms). */
#define GL_FLUSH_CYC (ESP_SDR_CPU_HZ / 1000U)
/* Payload bytes per second the SDIO link carries, for the threshold servo. */
#define GL_LINK_BPS 11000000U

BUILD_ASSERT(ESDR_GAPLESS_RATE_HZ % CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC == 0,
	     "the gapless axis counts in whole systimer cycles");

static struct {
	struct esdr_seg_writer w;
	/* Blocks finished, queued to the link and read by the host (running counts). */
	uint32_t filled, queued, done, done_base;
	bool open;
	uint32_t t_open;
	/* The run's index 0 on the stream's axis, and the flags of its first segment. */
	uint64_t base;
	uint8_t first_flags;
} gl;

static struct esdr_gapless_stats gl_stats;

static APP_HOT void gl_finish(void)
{
	(void)esdr_seg_writer_finish(&gl.w, 0U);
	gl.filled++;
	gl.open = false;
}

static APP_HOT uint32_t gl_reserve(const struct esp_sdr_gapless_seg *seg, int8_t **dst,
				   void *user)
{
	const struct esdr_seg_meta m = {
		.shift = seg->shift,
		.flags = seg->flags | gl.first_flags,
		.gain = seg->gain,
		.dc_i = seg->dc_i,
		.dc_q = seg->dc_q,
		.power_dbfs_x10 = seg->power_dbfs_x10,
	};

	ARG_UNUSED(user);
	/* Into the open block, else into a fresh one. */
	for (int k = 0; k < 2; k++) {
		uint32_t n;

		if (!gl.open) {
			if (gl.filled - gl.done >= GL_BLOCKS) {
				gl_stats.link_full++;
				return 0U;
			}
			esdr_seg_writer_begin(&gl.w, out.blk[gl.filled % GL_BLOCKS]);
			gl.open = true;
			gl.t_open = esp_sdr_paced_cycles();
		}
		n = esdr_seg_writer_put(&gl.w, gl.base + seg->index, seg->pairs, &m, dst);
		if (n > 0U) {
			gl.first_flags = 0U;
			return n;
		}
		gl_finish();
	}
	return 0U;
}

static APP_HOT void gl_commit(uint32_t pairs, void *user)
{
	ARG_UNUSED(pairs);
	ARG_UNUSED(user);
}

/*
 * Full blocks, and part full ones that waited GL_FLUSH_CYC, to the link; requests in. Finishing
 * zeroes the block's tail (the ROM's memset, a cycle a byte at worst): a part full one waits
 * for a budget that covers it.
 */
static APP_HOT bool gl_service(uint32_t budget, void *user)
{
	ARG_UNUSED(user);
	if (gl.open && gl.w.samples != 0U &&
	    (esdr_seg_writer_room(&gl.w) == 0U ||
	     (esp_sdr_paced_cycles() - gl.t_open >= GL_FLUSH_CYC &&
	      budget >= ESDR_BLOCK_BYTES - gl.w.pos + 200U))) {
		gl_finish();
	}
	while (gl.queued != gl.filled &&
	       sdio_link_queue(out.blk[gl.queued % GL_BLOCKS], ESDR_BLOCK_BYTES, true)) {
		gl.queued++;
	}
	sdio_link_pump();
	gl.done = sdio_link_blocks_done() - gl.done_base;
	in_service(16);
	return true;
}

static APP_HOT bool gl_stop(void *user)
{
	ARG_UNUSED(user);
	return req.ready;
}

static void gl_account(const struct esp_sdr_gapless_stats *st, int ret)
{
	gl_stats.pairs += st->pairs;
	gl_stats.kept_pairs += st->kept_pairs;
	gl_stats.dropped_pairs += st->dropped_pairs;
	gl_stats.abandoned_pairs += st->abandoned_pairs;
	gl_stats.runs++;
	gl_stats.units += st->units;
	gl_stats.windows += st->windows;
	gl_stats.loud_windows += st->loud_windows;
	gl_stats.kept_windows += st->kept_windows;
	if (ret == -EIO) {
		gl_stats.ring_errors++;
		gl_stats.ring_last_error = st->error;
		gl_stats.ring_last_detail = st->detail;
	}
	gl_stats.overrun_activity = st->overrun_activity;
	gl_stats.overrun_cycles = st->overrun_cycles;
	gl_stats.overrun_room = st->overrun_room;
	gl_stats.late_max = MAX(gl_stats.late_max, st->late_max);
	gl_stats.idle_cycles_min = gl_stats.runs == 1U ? st->idle_cycles_min
						       : MIN(gl_stats.idle_cycles_min, st->idle_cycles_min);
	gl_stats.stage_bytes = CONFIG_ESP_SDR_GAPLESS_STAGE_KB * 1024U;
	gl_stats.stage_peak_bytes = MAX(gl_stats.stage_peak_bytes, st->stage_peak * 4U);
	gl_stats.baseline = st->baseline;
	gl_stats.ratio_x16 = st->ratio_x16;
	gl_stats.gain = st->gain;
	gl_stats.gain_steps += st->gain_steps;
	stats.rx_samples += st->kept_pairs;
}

static void run_gapless(void)
{
	const struct esdr_gapless_cfg *g = &want.gapless;
	struct esp_sdr_gapless_cfg cfg = {
		.ratio_x16 = g->ratio_x16,
		.pre = g->pre == 0xffU ? 1U : g->pre,
		.post = g->post == 0xffU ? 1U : g->post,
		.level = g->level,
		.budget_bps = (g->flags & ESDR_GAPLESS_F_FIXED_THR) != 0U ? 0U
			      : g->budget_bps != 0U ? MIN(g->budget_bps, GL_LINK_BPS)
						    : GL_LINK_BPS,
		.headroom_lo = g->headroom_lo,
		.headroom_hi = g->headroom_hi,
		.gain_servo = (g->flags & ESDR_GAPLESS_F_GAIN_SERVO) != 0U ||
			      want.rx_gain == ESDR_GAIN_AUTO,
	};
	const struct esp_sdr_gapless_sink sink = {
		.reserve = gl_reserve,
		.commit = gl_commit,
		.service = gl_service,
		.stop = gl_stop,
	};
	struct esp_sdr_gapless_stats st;
	int64_t end;

	memset(&gl, 0, sizeof(gl));
	gl.done_base = sdio_link_blocks_done();
	/*
	 * The whole 80 MHz: the PHY's 40 MHz mode and the widest low-pass (unless the host set a
	 * bandwidth); the 20 MHz mode's filters would leave little beyond +-10 MHz.
	 */
	(void)esp_sdr_set_channel_bw(2);
	if (want.bandwidth_hz == 0U) {
		(void)esp_sdr_rx_set_lpf(0);
	}
	while (!req.ready && want.mode == ESDR_MODE_GAPLESS) {
		int ret;

		gl.base = k_cycle_get_64() *
			  (ESDR_GAPLESS_RATE_HZ / CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC);
		gl.first_flags = ESDR_SEG_RESTART;
		ret = esp_sdr_gapless_run(&cfg, &sink, &st);
		gl_account(&st, ret);
		if (ret != 0 && ret != -EIO) {
			want.mode = ESDR_MODE_OFF;
			break;
		}
	}
	/* What is left goes out before the reply. */
	if (gl.open && gl.w.samples != 0U) {
		gl_finish();
	}
	end = k_uptime_get() + 100;
	do {
		(void)gl_service(UINT32_MAX, NULL);
	} while (gl.done != gl.filled && k_uptime_get() <= end);
	gl_stats.blocks_sent += gl.done;
	stats.rx_blocks += gl.done;
	stats.usb_in_bytes += gl.done * ESDR_BLOCK_BYTES;
	/*
	 * The link counted the run's blocks too: the stream's counters (sdio_out()) go on from
	 * its count, with the blocks the host has yet to read still out. Behind, out.sent would
	 * pass out.committed and the stream would find no free block again.
	 */
	out.sent = sdio_link_blocks_done();
	out.committed = out.sent + (gl.queued - gl.done);
	out_queued = out.committed;
	/* The other modes' filters come back with the next radio_apply(). */
	(void)esp_sdr_set_channel_bw(0);
	radio_dirty = true;
}

static int set_gapless(const uint8_t *data, size_t len)
{
	struct esdr_gapless_cfg g;

	if (len != sizeof(g)) {
		return -EINVAL;
	}
	memcpy(&g, data, sizeof(g));
	g.ratio_x16 = sys_le16_to_cpu(g.ratio_x16);
	g.level = sys_le32_to_cpu(g.level);
	g.budget_bps = sys_le32_to_cpu(g.budget_bps);
	g.flags = sys_le16_to_cpu(g.flags);
	if (g.ratio_x16 == 0U) {
		g.ratio_x16 = 64U;
	}
	if ((g.pre > 4U && g.pre != 0xffU) || (g.post > 4U && g.post != 0xffU) ||
	    g.headroom_lo > g.headroom_hi || g.headroom_hi >= 50U || g.ratio_x16 <= 16U) {
		return -EINVAL;
	}
	want.gapless = g;
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
#if defined(CONFIG_ESP_SDR_GAPLESS)
	case ESDR_REQ_SET_GAPLESS:
		return set_gapless(data, len);
#endif
	case ESDR_REQ_CAPTURE_BURST:
		return capture_burst(value, data, len);
#if defined(CONFIG_APP_FW_UPDATE)
	case ESDR_REQ_FW_BEGIN:
		return len == 4U ? fw_update_begin(sys_get_le32(data)) : -EINVAL;
	case ESDR_REQ_FW_WRITE:
		return len > 4U ? fw_update_write(sys_get_le32(data), data + 4, len - 4U) : -EINVAL;
	case ESDR_REQ_FW_END:
		return len == 4U ? fw_update_end(sys_get_le32(data)) : -EINVAL;
#endif
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
		if (value != ESDR_MODE_OFF && value != ESDR_MODE_RX &&
		    !(IS_ENABLED(CONFIG_ESP_SDR_PACED) && value == ESDR_MODE_TX) &&
		    !(IS_ENABLED(CONFIG_ESP_SDR_TRIG) && value == ESDR_MODE_TRIGGER) &&
		    !(IS_ENABLED(CONFIG_ESP_SDR_GAPLESS) && value == ESDR_MODE_GAPLESS)) {
			return -ENOTSUP;
		}
		want.mode = value;
		return 0;
	case ESDR_REQ_SET_FREQ:
		if (len != 8U) {
			return -EINVAL;
		}
		if (!esdr_freq_ok(sys_get_le64(data))) {
			return -EINVAL;
		}
		want.freq_hz = sys_get_le64(data);
		radio_dirty = true;
		return 0;
	case ESDR_REQ_SET_SAMPLE_RATE:
		if (len != 4U) {
			return -EINVAL;
		}
		want.rate_hz =
			esdr_rate_round(LINK_RATE_MAX_HZ, LINK_RATE_COUNT, sys_get_le32(data));
		return 0;
	case ESDR_REQ_SET_RX_GAIN:
		if (value != ESDR_GAIN_AUTO && value > (uint16_t)esp_sdr_rx_gain_max()) {
			return -EINVAL;
		}
		want.rx_gain = value;
		radio_dirty = true;
		return 0;
	case ESDR_REQ_SET_TX_GAIN:
		if (value > tx_power_max()) {
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
		if (!esdr_dgain_ok(value)) {
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
		struct esdr_info info;

		esdr_info_init(&info, LINK_RATE_MAX_HZ, LINK_RATE_COUNT,
			       (uint16_t)esp_sdr_rx_gain_max(), (uint16_t)tx_power_max(),
			       "osmosdr_serial " APP_VERSION_STRING);
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
	case ESDR_REQ_GET_TEMP: {
		int16_t dc;
		int ret = esp_sdr_get_temp_dc(&dc);

		if (ret != 0) {
			return ret;
		}
		if (max < 2U) {
			return -EINVAL;
		}
		sys_put_le16((uint16_t)dc, buf);
		return 2;
	}
#if defined(CONFIG_ESP_SDR_GAPLESS)
	case ESDR_REQ_GET_GAPLESS_STATS:
		memcpy(buf, &gl_stats, MIN(max, sizeof(gl_stats)));
		return (int)MIN(max, sizeof(gl_stats));
#endif
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

#if defined(CONFIG_ESP_SDR_PACED)
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
#endif

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

#if !defined(CONFIG_ESP_SDR_PACED)
/*
 * Decimated bursts: each one's samples into the blocks, then the link takes them; the next
 * burst starts once the blocks are out, its gap since the last one moves the block index and
 * flags the block. Their CIC's full scale is int16's; the digital gain shifts it up.
 */
static void run_rx(void)
{
	static struct esp_sdr_iq16 iq[ESP_SDR_SAMPLES_MAX / (BURST_IN_HZ / LINK_RATE_MAX_HZ)];
	unsigned int m = BURST_IN_HZ / want.rate_hz;
	int32_t g = 1 << (dgain_db() / 6U);
	uint64_t next_ns = 0;
	bool first = true;

	rx_begin();
	while (!req.ready) {
		uint64_t first_ns;
		size_t n;

		if (esp_sdr_rx_capture_decimated(ESP_SDR_RATE_16MSPS, ESP_SDR_SAMPLES_MAX, m, iq,
						 ARRAY_SIZE(iq), &n, &first_ns) != 0) {
			want.mode = ESDR_MODE_OFF;
			break;
		}
		if (!first && first_ns > next_ns) {
			/* Close the block in progress, move on by the gap, flag it. */
			if (out.fill_n != 0U) {
				out_commit();
			}
			out.index += (first_ns - next_ns) * want.rate_hz / NSEC_PER_SEC;
			out.next_flags |= ESDR_BLK_GAP;
		}
		first = false;
		next_ns = first_ns + (uint64_t)n * NSEC_PER_SEC / want.rate_hz;
		for (size_t j = 0; j < n; j++) {
			rx_emit(CLAMP(iq[j].i * g, -32767, 32767), CLAMP(iq[j].q * g, -32767, 32767));
		}
		while (!req.ready && out.sent != out.committed) {
			out_service(64);
			in_service(16);
		}
		in_service(16);
	}
	stats.ring_runs++;
	rx_end();
}
#else
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
#endif /* CONFIG_ESP_SDR_PACED */

int main(void)
{
	uint32_t stale_ms = 0;
	int ret;

	printk("osmosdr_serial %s\n", APP_VERSION_STRING);
	memset(&out, 0, sizeof(out));
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
#if defined(CONFIG_ESP_SDR_PACED)
		} else if (want.mode == ESDR_MODE_TX) {
			run_tx();
#endif
#if defined(CONFIG_ESP_SDR_TRIG)
		} else if (want.mode == ESDR_MODE_TRIGGER) {
			run_trig();
#endif
#if defined(CONFIG_ESP_SDR_GAPLESS)
		} else if (want.mode == ESDR_MODE_GAPLESS) {
			run_gapless();
#endif
		} else {
			uint32_t got = req.got + req.tx_left;

			in_service(64);
			/* Sleep only while nothing arrives: a link without flow control overruns. */
			if (!req.ready && !link_readable()) {
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
