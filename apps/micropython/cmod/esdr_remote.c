/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The radio behind a link (esdr_remote.h): the WHY2025 badge's ESP32-C6 running
 * apps/osmosdr_serial, over SDIO where the board names the host slot as chosen esdr,link-sdio
 * (function 1, the C6's SLC protocol: up to 2 MS/s), else over the UART it names as chosen
 * esdr,link-uart (the P4's UART1 at 5 Mbaud, CTS on GPIO14: the C6 holds the P4 off while its
 * transmit ring is full; up to 250 kS/s).
 *
 * Receive: the UART's interrupt or the SDIO reader thread fills a byte ring, a parser thread
 * splits it into reply frames (struct esdr_ser_hdr) and sample blocks (struct
 * esdr_block_hdr) by the transport's host side, shared with the bridge app
 * (../../common/src/esdr_link_host.c). Samples go to the capture buffer and the monitor
 * (esdr_rx_latest()), the gapless stream's segments to the monitor and a hook (esdr_gl.c).
 * Transmit: requests and transmit frames go out one frame at a time; a feeder thread streams a
 * buffer.
 */

#include <errno.h>
#include <math.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <esp_assert.h>
#include <hal/uart_ll.h>

#include "esdr_defs.h"
#include "esdr_link_host.h"
#include "esdr_mon.h"
#include "esdr_remote.h"
#include "esdr_rx.h"
#include "esdr_seg.h"

#define LINK_SDIO DT_CHOSEN(esdr_link_sdio)
#define LINK_UART DT_CHOSEN(esdr_link_uart)

#if DT_NODE_EXISTS(LINK_SDIO)
#include "esdr_sdio_host.h"
#endif

/* At 500 kB/s: 130 ms of bytes while the parser is held up (over SDIO at 4 MB/s, 16 ms). */
#define RING_BYTES 65536U
#define TX_FRAME_PAIRS (ESDR_BLOCK_BYTES / 2U)

#define PARSER_STACK 2048
/* Cooperative, above MicroPython (the main thread, 0): a GUI that draws holds the link up
 * otherwise, the ring overflows and a block loses bytes. */
#define PARSER_PRIO  K_PRIO_COOP(2)
#define FEEDER_STACK 2048
/* Cooperative too: 250 kS/s takes all the link has, and MicroPython running does not hold the
 * feeder up (it sleeps on the transmit FIFO, not in a spin). */
#define FEEDER_PRIO  K_PRIO_COOP(3)
/* The transmit FIFO wakes the writer this long at most (CTS held: the radio's ring is full). */
#define TX_ROOM_WAIT K_MSEC(2)
/* The burst requester waits on replies; it may wait behind MicroPython too. */
#define BURST_STACK 1024
#define BURST_PRIO  7

static struct esdr_info info;
static bool opened;

/* ---- receive: the byte ring ---- */

/* In PSRAM, as the monitor: internal RAM is the kernel's and the drivers'. */
static uint8_t ring[RING_BYTES] ESDR_PSRAM;
static volatile uint32_t ring_head, ring_tail;
static K_SEM_DEFINE(ring_sem, 0, 1);

#if DT_NODE_EXISTS(LINK_SDIO)
/* ---- the link: SDIO ---- */

/* The radio's SLC protocol: ../../common/src/esdr_sdio_host.c, shared with the bridge app. */
#define SDIO_CHUNK      16384U
#define SDIO_READY_MS   3000
/* The reader: cooperative, above MicroPython, as the parser; it sleeps a tick when idle. */
#define READER_STACK    2048
#define READER_PRIO     K_PRIO_COOP(3)

static const struct device *const sdhc = DEVICE_DT_GET(LINK_SDIO);
/* A DMA buffer in internal RAM. */
static uint8_t chunk[SDIO_CHUNK] __aligned(64);

/* What the radio offers, into the ring as it has room (else the radio's blocks wait). */
static void reader_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		uint32_t room = RING_BYTES - (ring_head - ring_tail);
		int n = esdr_sdio_read(chunk, MIN((uint32_t)SDIO_CHUNK, room));

		if (n <= 0) {
			k_sleep(K_TICKS(1));
			continue;
		}
		for (uint32_t k = 0; k < (uint32_t)n;) {
			uint32_t at = ring_head % RING_BYTES;
			uint32_t m = MIN((uint32_t)n - k, RING_BYTES - at);

			memcpy(&ring[at], chunk + k, m);
			ring_head += m;
			k += m;
		}
		k_sem_give(&ring_sem);
	}
}
K_THREAD_STACK_DEFINE(reader_stack, READER_STACK);
static struct k_thread reader_thread;

static int link_send(const void *a, size_t na, const void *b, size_t nb)
{
	return esdr_sdio_send(a, na, b, nb, ESDR_LINK_TIMEOUT_MS);
}

static int link_open(void)
{
	int ret = esdr_sdio_open(sdhc, SDIO_READY_MS);

	if (ret != 0) {
		return ret;
	}
	k_thread_create(&reader_thread, reader_stack, K_THREAD_STACK_SIZEOF(reader_stack),
			reader_fn, NULL, NULL, NULL, READER_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&reader_thread, "esdr_sdio");
	return 0;
}
#else
/* ---- the link: a UART ---- */

static const struct device *const link_uart = DEVICE_DT_GET(LINK_UART);
static uart_dev_t *const link_hw = (uart_dev_t *)DT_REG_ADDR(LINK_UART);
/* The transmit FIFO is down to CONFIG_UART_ESP32_TX_FIFO_THRESH bytes. */
static K_SEM_DEFINE(tx_room, 0, 1);
/* One writer at a time (a frame goes out whole). */
static K_MUTEX_DEFINE(write_lock);

static void link_isr(const struct device *dev, void *user_data)
{
	uint8_t tmp[128];

	ARG_UNUSED(user_data);
	uart_irq_update(dev);
	if (uart_irq_tx_ready(dev) != 0) {
		uart_irq_tx_disable(dev);
		k_sem_give(&tx_room);
	}
	while (uart_irq_rx_ready(dev) != 0) {
		int n = uart_fifo_read(dev, tmp, sizeof(tmp));

		if (n <= 0) {
			break;
		}
		for (int i = 0; i < n; i++) {
			uint32_t h = ring_head;

			if (h - ring_tail >= RING_BYTES) {
				/* Counted as lost by the parser through the resync. */
				break;
			}
			ring[h % RING_BYTES] = tmp[i];
			ring_head = h + 1U;
		}
	}
	k_sem_give(&ring_sem);
}

static void link_write(const void *p, size_t n)
{
	const uint8_t *b = p;

	while (n > 0U) {
		uint32_t room = uart_ll_get_txfifo_len(link_hw);

		if (room == 0U) {
			/* 128 bytes leave in 256 us, unless the radio holds CTS. (A tick's sleep here
			 * took 2 ms: 64 kB/s.) */
			k_sem_reset(&tx_room);
			uart_irq_tx_enable(link_uart);
			(void)k_sem_take(&tx_room, TX_ROOM_WAIT);
			continue;
		}
		room = MIN(room, (uint32_t)n);
		uart_ll_write_txfifo(link_hw, b, room);
		b += room;
		n -= room;
	}
}

static int link_send(const void *a, size_t na, const void *b, size_t nb)
{
	k_mutex_lock(&write_lock, K_FOREVER);
	link_write(a, na);
	if (nb > 0U) {
		link_write(b, nb);
	}
	k_mutex_unlock(&write_lock);
	return 0;
}

static int link_open(void)
{
	const struct uart_config cfg = {
		.baudrate = DT_PROP(LINK_UART, current_speed),
		.parity = UART_CFG_PARITY_NONE,
		.stop_bits = UART_CFG_STOP_BITS_1,
		.data_bits = UART_CFG_DATA_BITS_8,
		.flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
	};
	int ret;

	if (!device_is_ready(link_uart)) {
		return -ENODEV;
	}
	/* The rate set at boot from current-speed does not hold on the P4: set it again. */
	ret = uart_configure(link_uart, &cfg);
	if (ret < 0) {
		return ret;
	}
	/* CTS (the radio's RTS) holds the transmitter while the radio's ring is full. */
	uart_ll_set_hw_flow_ctrl(link_hw, UART_HW_FLOWCTRL_CTS, 0);
	uart_irq_callback_user_data_set(link_uart, link_isr, NULL);
	uart_irq_rx_enable(link_uart);
	return 0;
}
#endif

/* ---- requests ---- */

static uint8_t *get_frame(void *user);
static void frame_done(uint8_t *blk, void *user);

static struct esdr_link link = {
	.send = link_send,
	.get_block = get_frame,
	.on_block = frame_done,
};

int esdr_remote_request(uint8_t req, uint16_t value, const void *data, uint16_t len,
			void *reply, uint16_t reply_max)
{
	return esdr_link_request(&link, req, value, data, len, reply, reply_max);
}

static int link_init(void)
{
	esdr_link_init(&link);
	return 0;
}
SYS_INIT(link_init, APPLICATION, 0);

/* ---- receive: samples ---- */

static struct {
	volatile bool active;
	int16_t *buf;
	size_t cap;
	uint64_t count;
	bool wrap;
	bool have_index;
	uint64_t next_index;
	struct esdr_remote_rx_stats st;
	/* Bursts (esdr_remote_rx_start_burst()): the radio's native rate in MHz (0: a stream),
	 * kept at 1 / decim; the half-band filter's last inputs and the input's parity. */
	uint16_t burst_mhz;
	uint8_t decim;
	int32_t hb_i[7], hb_q[7];
	uint32_t hb_n;
	/* The link's resyncs when the run started. */
	uint32_t resyncs0;
} rx;

static void rx_done_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	(void)esdr_remote_request(ESDR_REQ_SET_MODE, ESDR_MODE_OFF, NULL, 0, NULL, 0);
}
static K_WORK_DEFINE(rx_done_work, rx_done_work_fn);

/* The parser fills buffers under it; stop and start change them under it. */
static K_MUTEX_DEFINE(rx_lock);

/* One pair into the monitor (esdr_mon.c) and the buffer. */
static void rx_store(int16_t i, int16_t q)
{
	uint32_t pair = (uint16_t)i | ((uint32_t)(uint16_t)q << 16);

	esdr_mon_put(i, q);
	if (rx.count != 0U && rx.st.samples >= rx.count) {
		return;
	}
	if (rx.buf != NULL) {
		uint64_t at = rx.wrap ? rx.st.samples % rx.cap : rx.st.samples;

		if (at >= rx.cap) {
			return;
		}
		memcpy(&rx.buf[2U * at], &pair, sizeof(pair));
	}
	rx.st.samples++;
}

/*
 * A stream block's @p n 8-bit pairs, as rx_store() one by one but in runs: into the buffer
 * (up to its wrap or its end), and the newest into the monitor. 2 MS/s over SDIO did not
 * fit one sample at a time.
 */
static void rx_store_block(const int8_t *iq, uint32_t n)
{
	uint32_t take = n;

	esdr_mon_put_s8(iq, n);

	if (rx.count != 0U) {
		take = (uint32_t)MIN((uint64_t)take, rx.count > rx.st.samples ?
						     rx.count - rx.st.samples : 0U);
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
		esdr_widen(&rx.buf[2U * at], iq + 2U * k, m);
		rx.st.samples += m;
		k += m;
	}
}

/*
 * A burst's block: as it is, or halved by the half-band filter (-1 0 9 16 9 0 -1) / 32, the
 * burst's own (a new burst starts it again). Bursts follow each other with gaps.
 */
static void on_burst_block(const struct esdr_block_hdr *h, const uint8_t *p, uint32_t n)
{
	bool wide = (sys_le16_to_cpu(h->flags) & ESDR_BLK_16BIT) != 0U;

	if (h->index == 0U) {
		if (rx.have_index) {
			rx.st.gaps++;
		}
		rx.have_index = true;
		rx.hb_n = 0;
		memset(rx.hb_i, 0, sizeof(rx.hb_i));
		memset(rx.hb_q, 0, sizeof(rx.hb_q));
	}
	for (uint32_t k = 0; k < n; k++) {
		int32_t i, q;

		if (wide) {
			i = (int16_t)sys_get_le16(p + 4U * k);
			q = (int16_t)sys_get_le16(p + 4U * k + 2U);
		} else {
			i = (int8_t)p[2U * k];
			q = (int8_t)p[2U * k + 1U];
		}
		if (rx.decim == 1U) {
			rx_store((int16_t)i, (int16_t)q);
			continue;
		}
		memmove(&rx.hb_i[1], &rx.hb_i[0], 6U * sizeof(rx.hb_i[0]));
		memmove(&rx.hb_q[1], &rx.hb_q[0], 6U * sizeof(rx.hb_q[0]));
		rx.hb_i[0] = i;
		rx.hb_q[0] = q;
		if ((++rx.hb_n & 1U) != 0U || rx.hb_n < 7U) {
			continue;
		}
		i = 16 * rx.hb_i[3] + 9 * (rx.hb_i[2] + rx.hb_i[4]) - (rx.hb_i[0] + rx.hb_i[6]);
		q = 16 * rx.hb_q[3] + 9 * (rx.hb_q[2] + rx.hb_q[4]) - (rx.hb_q[0] + rx.hb_q[6]);
		rx_store((int16_t)(i / 32), (int16_t)(q / 32));
	}
}

/* ---- triggered snapshots (ESDR_MODE_TRIGGER) ---- */

/* Two snapshot buffers: one being filled, the newest complete one for esdr_remote_trig_get(). */
static int8_t trg_buf[2][2U * ESDR_TRIG_SAMPLES] ESDR_PSRAM;
static K_MUTEX_DEFINE(trg_lock);
static struct {
	volatile bool active;
	/* The buffer being filled, its samples so far and the first one's index (trigger 0). */
	uint8_t fill;
	uint32_t got;
	int64_t first;
	/* The newest complete snapshot: buffer, sequence (from 1), samples before the trigger. */
	int8_t done;
	uint32_t seq, trig;
	struct esdr_remote_trig_stats st;
} trg = {.done = -1};

/* The gapless stream (ESDR_MODE_GAPLESS, below). */
static struct {
	volatile bool active;
	bool monitor;
	void (*volatile hook)(const uint8_t *blk);
	struct esdr_seg_track track;
	struct esdr_remote_gl_stats st;
} gl;

static void on_trig_block(const struct esdr_block_hdr *h, const uint8_t *p, uint32_t n)
{
	int64_t index = (int64_t)h->index;

	if (!trg.active || n > ESDR_BLOCK_PAYLOAD / 2U) {
		return;
	}
	k_mutex_lock(&trg_lock, K_FOREVER);
	if ((sys_le16_to_cpu(h->flags) & ESDR_BLK_GAP) != 0U) {
		if (trg.got != 0U) {
			trg.st.broken++;
		}
		trg.fill = trg.done == 0 ? 1U : 0U;
		trg.got = 0;
		trg.first = index;
	}
	if (index != trg.first + (int64_t)trg.got || trg.got + n > ESDR_TRIG_SAMPLES) {
		/* A block missing: this snapshot is lost, the next starts with ESDR_BLK_GAP. */
		if (trg.got != 0U) {
			trg.st.broken++;
		}
		trg.got = 0;
		trg.first = INT64_MIN / 2;
		k_mutex_unlock(&trg_lock);
		return;
	}
	memcpy(&trg_buf[trg.fill][2U * trg.got], p, 2U * n);
	trg.got += n;
	if (trg.got == ESDR_TRIG_SAMPLES) {
		trg.done = (int8_t)trg.fill;
		trg.seq++;
		trg.trig = (uint32_t)(-trg.first);
		trg.st.snapshots++;
		trg.got = 0;
		trg.fill ^= 1U;
	}
	k_mutex_unlock(&trg_lock);
}

int esdr_remote_trig_start(uint16_t ratio_x16, uint32_t post)
{
	uint8_t le[4];
	int ret;

	esdr_remote_rx_stop();
	sys_put_le32(post, le);
	ret = esdr_remote_request(ESDR_REQ_SET_TRIGGER, ratio_x16, le, sizeof(le), NULL, 0);
	if (ret < 0) {
		return ret;
	}
	k_mutex_lock(&trg_lock, K_FOREVER);
	trg.got = 0;
	trg.done = -1;
	trg.seq = 0;
	memset(&trg.st, 0, sizeof(trg.st));
	trg.active = true;
	k_mutex_unlock(&trg_lock);
	ret = esdr_remote_request(ESDR_REQ_SET_MODE, ESDR_MODE_TRIGGER, NULL, 0, NULL, 0);
	if (ret < 0) {
		trg.active = false;
	}
	return ret < 0 ? ret : 0;
}

void esdr_remote_reset(void)
{
	esdr_remote_rx_stop();
	esdr_remote_tx_stop();
	trg.active = false;
	gl.active = false;
	k_mutex_lock(&trg_lock, K_FOREVER);
	trg.got = 0;
	trg.done = -1;
	trg.seq = 0;
	memset(&trg.st, 0, sizeof(trg.st));
	k_mutex_unlock(&trg_lock);
	/* A radio without the request keeps its settings; the streams are off all the same. */
	(void)esdr_remote_request(ESDR_REQ_RESET, 0, NULL, 0, NULL, 0);
}

void esdr_remote_trig_stop(void)
{
	trg.active = false;
	(void)esdr_remote_request(ESDR_REQ_SET_MODE, ESDR_MODE_OFF, NULL, 0, NULL, 0);
}

bool esdr_remote_trig_get(int8_t *dst, uint32_t after, uint32_t *seq, uint32_t *trig)
{
	bool fresh;

	k_mutex_lock(&trg_lock, K_FOREVER);
	fresh = trg.done >= 0 && trg.seq != after;
	if (fresh) {
		memcpy(dst, trg_buf[trg.done], sizeof(trg_buf[0]));
		*seq = trg.seq;
		*trig = trg.trig;
	}
	k_mutex_unlock(&trg_lock);
	return fresh;
}

void esdr_remote_trig_stats(struct esdr_remote_trig_stats *st)
{
	k_mutex_lock(&trg_lock, K_FOREVER);
	*st = trg.st;
	k_mutex_unlock(&trg_lock);
}

/* ---- gapless receive (ESDR_MODE_GAPLESS) ---- */

/*
 * A segment's pairs into the monitor (its newest ESDR_MON_SAMPLES), as 32 times the raw units:
 * (item << shift) + (2^shift - 1) / 2 - dc (esdr_seg_hdr), full scale 16384 as bursts.
 */
static int gl_seg_cb(const struct esdr_seg *seg, void *user)
{
	const unsigned int sh = MIN(seg->shift, 9U) + 5U;
	const int32_t half = 16 * (int32_t)((1U << seg->shift) - 1U);
	const int32_t oi = half - 32 * (int32_t)seg->dc_i, oq = half - 32 * (int32_t)seg->dc_q;
	const uint32_t n = seg->pairs, skip = n > ESDR_MON_SAMPLES ? n - ESDR_MON_SAMPLES : 0U;
	uint64_t gap = esdr_seg_track_gap(&gl.track, seg);

	ARG_UNUSED(user);
	gl.st.segments++;
	gl.st.pairs += n;
	if (gap != 0U && gap != ESDR_SEG_GAP_START) {
		gl.st.gaps++;
	}
	if (!gl.monitor) {
		return 0;
	}
	for (uint32_t k = skip; k < n; k++) {
		int32_t i = CLAMP(((int32_t)seg->iq[2U * k] << sh) + oi, INT16_MIN, INT16_MAX);
		int32_t q = CLAMP(((int32_t)seg->iq[2U * k + 1U] << sh) + oq, INT16_MIN, INT16_MAX);

		*esdr_mon_slot(k) = (uint16_t)i | ((uint32_t)(uint16_t)q << 16);
	}
	esdr_mon_advance(n);
	return 0;
}

static void on_seg_block(const uint8_t *blk)
{
	void (*hook)(const uint8_t *blk) = gl.hook;

	if (!gl.active) {
		return;
	}
	gl.st.blocks++;
	if (esdr_seg_parse(blk, ESDR_BLOCK_BYTES, gl_seg_cb, NULL) < 0) {
		gl.st.bad++;
		return;
	}
	if (hook != NULL) {
		hook(blk);
	}
}

int esdr_remote_gl_start(const struct esdr_gapless_cfg *cfg, bool monitor)
{
	int ret;

	esdr_remote_rx_stop();
	trg.active = false;
	/* A radio without the mode refuses its settings. */
	ret = esdr_remote_request(ESDR_REQ_SET_GAPLESS, 0, cfg, sizeof(*cfg), NULL, 0);
	if (ret < 0) {
		return ret;
	}
	k_mutex_lock(&rx_lock, K_FOREVER);
	memset(&gl.st, 0, sizeof(gl.st));
	esdr_seg_track_reset(&gl.track);
	gl.monitor = monitor;
	gl.active = true;
	k_mutex_unlock(&rx_lock);
	ret = esdr_remote_request(ESDR_REQ_SET_MODE, ESDR_MODE_GAPLESS, NULL, 0, NULL, 0);
	if (ret < 0) {
		gl.active = false;
	}
	return ret < 0 ? ret : 0;
}

void esdr_remote_gl_stop(void)
{
	if (!gl.active) {
		return;
	}
	gl.active = false;
	(void)esdr_remote_request(ESDR_REQ_SET_MODE, ESDR_MODE_OFF, NULL, 0, NULL, 0);
}

bool esdr_remote_gl_busy(void)
{
	return gl.active;
}

void esdr_remote_gl_stats(struct esdr_remote_gl_stats *st)
{
	k_mutex_lock(&rx_lock, K_FOREVER);
	*st = gl.st;
	k_mutex_unlock(&rx_lock);
}

int esdr_remote_gl_radio_stats(struct esdr_gapless_stats *st)
{
	int ret;

	/* An older radio sends fewer fields: the rest read 0. */
	memset(st, 0, sizeof(*st));
	ret = esdr_remote_request(ESDR_REQ_GET_GAPLESS_STATS, 0, NULL, 0, st, sizeof(*st));
	if (ret < 0) {
		return ret;
	}
	return ret > 0 ? 0 : -EPROTO;
}

void esdr_remote_gl_hook(void (*hook)(const uint8_t *blk))
{
	gl.hook = hook;
}

static void on_block(const uint8_t *blk)
{
	struct esdr_block_hdr h;
	const int8_t *iq = (const int8_t *)(blk + sizeof(h));
	uint32_t n;

	memcpy(&h, blk, sizeof(h));
	n = sys_le16_to_cpu(h.samples);
	h.index = sys_le64_to_cpu(h.index);
	if ((sys_le16_to_cpu(h.flags) & ESDR_BLK_SEG) != 0U) {
		on_seg_block(blk);
		return;
	}
	if ((sys_le16_to_cpu(h.flags) & ESDR_BLK_TRIGGER) != 0U) {
		on_trig_block(&h, blk + sizeof(h), n);
		return;
	}
	if ((sys_le16_to_cpu(h.flags) & ESDR_BLK_BURST) != 0U) {
		uint32_t item = (sys_le16_to_cpu(h.flags) & ESDR_BLK_16BIT) != 0U ? 4U : 2U;

		if (rx.active && rx.burst_mhz != 0U && n <= ESDR_BLOCK_PAYLOAD / item) {
			on_burst_block(&h, blk + sizeof(h), n);
			if ((rx.count != 0U && rx.st.samples >= rx.count) ||
			    (rx.buf != NULL && !rx.wrap && rx.st.samples >= rx.cap)) {
				rx.active = false;
			}
		}
		return;
	}
	if (!rx.active || rx.burst_mhz != 0U || (sys_le16_to_cpu(h.flags) & ESDR_BLK_16BIT) != 0U ||
	    n > ESDR_BLOCK_PAYLOAD / 2U) {
		return;
	}
	if (rx.have_index && h.index != rx.next_index) {
		rx.st.gaps++;
		if (h.index > rx.next_index) {
			rx.st.gap_samples += h.index - rx.next_index;
		}
	}
	rx.have_index = true;
	rx.next_index = h.index + n;
	if ((sys_le16_to_cpu(h.flags) & (ESDR_BLK_GAP | ESDR_BLK_OVERFLOW)) != 0U) {
		rx.st.overflows++;
	}
	rx_store_block(iq, n);
	if ((rx.count != 0U && rx.st.samples >= rx.count) ||
	    (rx.buf != NULL && !rx.wrap && rx.st.samples >= rx.cap)) {
		rx.active = false;
		k_work_submit(&rx_done_work);
	}
}

/* ---- the parser ---- */

/* Word aligned: a block's hook copies it by words (esdr_gl.c). */
static uint8_t frame[ESDR_BLOCK_BYTES] __aligned(4);

static uint8_t *get_frame(void *user)
{
	ARG_UNUSED(user);
	return frame;
}

static void frame_done(uint8_t *blk, void *user)
{
	ARG_UNUSED(user);
	k_mutex_lock(&rx_lock, K_FOREVER);
	on_block(blk);
	k_mutex_unlock(&rx_lock);
}

static void parser_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		if (k_sem_take(&ring_sem, K_MSEC(20)) != 0 && esdr_link_busy(&link)) {
			/* 20 ms without a byte inside a frame: it lost bytes (the radio sends a
			 * frame or block in one go). Drop it, or it swallows the next replies. */
			(void)esdr_link_abort(&link);
		}
		/* What the ring holds, a block's worth at a time (up to its wrap): the room goes
		 * back to the reader as it is parsed. */
		while (ring_tail != ring_head) {
			uint32_t at = ring_tail % RING_BYTES;
			uint32_t k = MIN(MIN(ring_head - ring_tail, RING_BYTES - at),
					 (uint32_t)ESDR_BLOCK_BYTES);

			esdr_link_feed(&link, &ring[at], k);
			ring_tail += k;
		}
	}
}
K_THREAD_STACK_DEFINE(parser_stack, PARSER_STACK);
static struct k_thread parser_thread;

/* ---- transmit: the feeder ---- */

static struct {
	const int16_t *buf;
	size_t n;
	bool repeat;
	/* The generator when there is no buffer; it and the scale may change while it plays. */
	struct esdr_gen gen;
	volatile float scale;
	uint64_t count;
	uint32_t rate;
	volatile bool active, stop, use_gen;
	volatile uint64_t played;
} tx;
static K_SEM_DEFINE(tx_go, 0, 1);
static struct k_spinlock gen_lock;

static int8_t to_s8(float v)
{
	float f = v / 256.0f;

	return (int8_t)(f > 127.0f ? 127 : (f < -127.0f ? -127 : (int)f));
}

/* The waveform at phase @p ph (turns, 0 to 1), full scale +-32767, as esdr_tx.c on the S3. */
static void wave_at(enum esdr_wave wave, float ph, float *i, float *q)
{
	const float tau = 6.2831853f;

	*q = 0.0f;
	switch (wave) {
	case ESDR_WAVE_SIN:
		*i = 32767.0f * sinf(tau * ph);
		*q = -32767.0f * cosf(tau * ph);
		break;
	case ESDR_WAVE_COS:
		*i = 32767.0f * cosf(tau * ph);
		*q = 32767.0f * sinf(tau * ph);
		break;
	case ESDR_WAVE_SQR:
		*i = ph < 0.5f ? 32767.0f : -32767.0f;
		break;
	case ESDR_WAVE_TRI:
		*i = (ph < 0.5f ? ph : 1.0f - ph) * 4.0f * 32767.0f - 32767.0f;
		break;
	case ESDR_WAVE_SAW:
		*i = (ph * 2.0f - 1.0f) * 32767.0f;
		break;
	default:
		*i = 32767.0f;
		break;
	}
}

/* The next @p k pairs into @p p (8 bit), from the buffer at *@p pos or the generator. */
static void tx_fill(int8_t *p, size_t k, size_t *pos, float *phase)
{
	float scale = tx.scale;

	if (!tx.use_gen) {
		for (size_t j = 0; j < k; j++) {
			p[2U * j] = to_s8((float)tx.buf[2U * *pos] * scale);
			p[2U * j + 1U] = to_s8((float)tx.buf[2U * *pos + 1U] * scale);
			if (++*pos == tx.n && tx.repeat) {
				*pos = 0;
			}
		}
		return;
	}

	k_spinlock_key_t key = k_spin_lock(&gen_lock);
	struct esdr_gen g = tx.gen;

	k_spin_unlock(&gen_lock, key);
	for (size_t j = 0; j < k; j++) {
		float i, q;

		wave_at(g.wave, *phase, &i, &q);
		p[2U * j] = to_s8((i * g.amp + g.dc_i * 32767.0f) * scale);
		p[2U * j + 1U] = to_s8((q * g.amp + g.dc_q * 32767.0f) * scale);
		*phase += g.cycles;
		*phase -= floorf(*phase);
	}
}

static void feeder_fn(void *a, void *b, void *c)
{
	static int8_t out[ESDR_BLOCK_BYTES];

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		size_t pos = 0;
		float phase = 0.0f;

		(void)k_sem_take(&tx_go, K_FOREVER);
		while (!tx.stop && (tx.count == 0U || tx.played < tx.count) &&
		       (tx.use_gen || tx.repeat || pos < tx.n)) {
			size_t k = TX_FRAME_PAIRS;

			if (!tx.use_gen && !tx.repeat) {
				k = MIN(k, tx.n - pos);
			}
			if (tx.count != 0U) {
				k = (size_t)MIN((uint64_t)k, tx.count - tx.played);
			}
			tx_fill(out, k, &pos, &phase);
			(void)esdr_link_send_tx(&link, out, 2U * k);
			tx.played += k;
		}
		/* A buffer that ended plays out first (the radio replies once it has): the data
		 * link's peer answers a burst soon after it (its 40 ms guard), and the radio is
		 * deaf until this returns. A stop cuts it short. */
		if (tx.stop || esdr_remote_request(ESDR_REQ_SET_MODE,
						   ESDR_MODE_OFF | ESDR_MODE_F_DRAIN, NULL, 0,
						   NULL, 0) < 0) {
			(void)esdr_remote_request(ESDR_REQ_SET_MODE, ESDR_MODE_OFF, NULL, 0, NULL,
						  0);
		}
		tx.active = false;
	}
}

K_THREAD_STACK_DEFINE(feeder_stack, FEEDER_STACK);
static struct k_thread feeder_thread;

/* ---- receive: bursts, one request each, back to back while the run lasts ---- */

static K_SEM_DEFINE(burst_go, 0, 1);
static volatile bool burst_running;

static void burst_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		(void)k_sem_take(&burst_go, K_FOREVER);
		while (rx.active && rx.burst_mhz != 0U) {
			uint8_t le[4];

			sys_put_le32(ESDR_BURST_MAX, le);
			if (esdr_remote_request(ESDR_REQ_CAPTURE_BURST, rx.burst_mhz, le, sizeof(le),
						NULL, 0) < 0) {
				rx.st.overflows++;
				k_msleep(10);
			}
		}
		burst_running = false;
	}
}

K_THREAD_STACK_DEFINE(burst_stack, BURST_STACK);
static struct k_thread burst_thread;

/* ---- setup ---- */

int esdr_remote_init(const struct esdr_info **out)
{
	int ret;

	if (!opened) {
		ret = link_open();
		if (ret < 0) {
			return ret;
		}
		k_thread_create(&parser_thread, parser_stack, K_THREAD_STACK_SIZEOF(parser_stack),
				parser_fn, NULL, NULL, NULL, PARSER_PRIO, 0, K_NO_WAIT);
		k_thread_name_set(&parser_thread, "esdr_link");
		k_thread_create(&feeder_thread, feeder_stack, K_THREAD_STACK_SIZEOF(feeder_stack),
				feeder_fn, NULL, NULL, NULL, FEEDER_PRIO, 0, K_NO_WAIT);
		k_thread_name_set(&feeder_thread, "esdr_tx");
		k_thread_create(&burst_thread, burst_stack, K_THREAD_STACK_SIZEOF(burst_stack),
				burst_fn, NULL, NULL, NULL, BURST_PRIO, 0, K_NO_WAIT);
		k_thread_name_set(&burst_thread, "esdr_burst");
		opened = true;
	}
	if (info.magic != ESDR_INFO_MAGIC) {
		ret = esdr_remote_request(ESDR_REQ_GET_INFO, 0, NULL, 0, &info, sizeof(info));
		if (ret < 0) {
			return ret;
		}
		if (sys_le32_to_cpu(info.magic) != ESDR_INFO_MAGIC) {
			memset(&info, 0, sizeof(info));
			return -EPROTO;
		}
	}
	if (out != NULL) {
		*out = &info;
	}
	return 0;
}

int esdr_remote_rx_start(uint32_t rate, int16_t *buf, size_t cap, uint64_t count, bool wrap)
{
	uint8_t le[4];
	int ret;

	esdr_remote_rx_stop();
	ret = esdr_remote_request(ESDR_REQ_SET_FORMAT, 8, NULL, 0, NULL, 0);
	if (ret < 0) {
		return ret;
	}
	sys_put_le32(rate, le);
	ret = esdr_remote_request(ESDR_REQ_SET_SAMPLE_RATE, 0, le, sizeof(le), NULL, 0);
	if (ret < 0) {
		return ret;
	}
	k_mutex_lock(&rx_lock, K_FOREVER);
	rx.buf = buf;
	rx.cap = cap;
	rx.count = count;
	rx.wrap = wrap;
	rx.have_index = false;
	rx.burst_mhz = 0;
	memset(&rx.st, 0, sizeof(rx.st));
	rx.resyncs0 = link.resyncs;
	rx.active = true;
	k_mutex_unlock(&rx_lock);
	ret = esdr_remote_request(ESDR_REQ_SET_MODE, ESDR_MODE_RX, NULL, 0, NULL, 0);
	if (ret < 0) {
		esdr_remote_rx_stop();
	}
	return ret < 0 ? ret : 0;
}

int esdr_remote_rx_start_burst(uint16_t mhz, unsigned int decim, int16_t *buf, size_t cap,
			       uint64_t count)
{
	esdr_remote_rx_stop();
	k_mutex_lock(&rx_lock, K_FOREVER);
	rx.buf = buf;
	rx.cap = cap;
	rx.count = count;
	rx.wrap = false;
	rx.have_index = false;
	rx.burst_mhz = mhz;
	rx.decim = (uint8_t)decim;
	memset(&rx.st, 0, sizeof(rx.st));
	rx.resyncs0 = link.resyncs;
	rx.active = true;
	burst_running = true;
	k_mutex_unlock(&rx_lock);
	k_sem_give(&burst_go);
	return 0;
}

void esdr_remote_rx_stop(void)
{
	/* Once the parser is out of the block it was filling in, nothing writes into the buffer
	 * (the caller may free it); blocks still in the ring belong to the stopped run. */
	k_mutex_lock(&rx_lock, K_FOREVER);
	rx.active = false;
	rx.buf = NULL;
	k_mutex_unlock(&rx_lock);
	/* A burst on its way finishes first (its blocks find no run). */
	while (burst_running) {
		k_msleep(2);
	}
	(void)esdr_remote_request(ESDR_REQ_SET_MODE, ESDR_MODE_OFF, NULL, 0, NULL, 0);
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
	st->resyncs = link.resyncs - rx.resyncs0;
}

int esdr_remote_tx_start(uint32_t rate, const int16_t *buf, size_t n, bool repeat,
			 const struct esdr_gen *gen, uint64_t count, float scale)
{
	uint8_t le[4];
	int ret;

	esdr_remote_tx_stop();
	esdr_remote_rx_stop();
	ret = esdr_remote_request(ESDR_REQ_SET_FORMAT, 8, NULL, 0, NULL, 0);
	if (ret < 0) {
		return ret;
	}
	sys_put_le32(rate, le);
	ret = esdr_remote_request(ESDR_REQ_SET_SAMPLE_RATE, 0, le, sizeof(le), NULL, 0);
	if (ret < 0) {
		return ret;
	}
	ret = esdr_remote_request(ESDR_REQ_SET_MODE, ESDR_MODE_TX, NULL, 0, NULL, 0);
	if (ret < 0) {
		return ret;
	}
	tx.buf = buf;
	tx.n = n;
	tx.repeat = repeat;
	tx.use_gen = buf == NULL;
	if (gen != NULL) {
		tx.gen = *gen;
	}
	tx.count = count;
	tx.scale = scale;
	tx.rate = rate;
	tx.played = 0;
	tx.stop = false;
	tx.active = true;
	k_sem_give(&tx_go);
	return 0;
}

void esdr_remote_tx_stop(void)
{
	if (!tx.active) {
		return;
	}
	tx.stop = true;
	while (tx.active) {
		k_msleep(5);
	}
}

bool esdr_remote_tx_busy(void)
{
	return tx.active;
}

/*
 * What the radio has played, not what went to it: over SDIO a burst goes to the radio in a
 * millisecond and plays for many, and the data link stops the transmitter once its samples
 * have played (over the UART, feeding took as long as playing). One short of what went to the
 * radio while the feeder runs, which it does until the radio has played it all
 * (ESDR_MODE_F_DRAIN); all of it once the radio is off.
 */
uint64_t esdr_remote_tx_played(void)
{
	uint64_t fed = tx.played;

	return tx.active && fed > 0U ? fed - 1U : fed;
}

void esdr_remote_tx_set_gen(const struct esdr_gen *gen)
{
	k_spinlock_key_t key = k_spin_lock(&gen_lock);

	tx.gen = *gen;
	k_spin_unlock(&gen_lock, key);
}

void esdr_remote_tx_set_scale(float scale)
{
	tx.scale = scale;
}
