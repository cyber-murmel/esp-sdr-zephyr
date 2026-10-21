/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The WHY2025 badge's ESP32-P4 as an osmosdr USB device (../osmosdr/src/usb_sdr.c, high speed
 * on its OTG HS port), its ESP32-C6 running apps/osmosdr_serial over SDIO as the radio: the
 * host's vendor requests go to the C6 as request frames and its replies come back, the C6's
 * receive blocks (already in the USB block format) go out on bulk IN as they are, the host's
 * transmit samples go to the C6 as transmit frames.
 *
 * The SDIO side is ../../common/src/esdr_sdio_host.c (the C6's SLC protocol on function 1, as
 * the MicroPython app uses it too): a reader thread takes the C6's stream and splits it into
 * frames and blocks; frames go out as one write each once the C6 has buffer tokens for them.
 *
 * With a channelizer set (ESDR_REQ_SET_CHANNELIZER, answered here), the gapless stream's
 * segment blocks (ESDR_BLK_SEG) go to a thread instead that runs them through the fixed-point
 * polyphase filterbank and burst detector (../../common/include/esdr_pfbq.h, PIE SIMD where the
 * build has it) and sends the open channels' samples as ESDR_BLK_CHAN blocks.
 */

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/usbd.h>

#include <sample_usbd.h>

#include "app_clock.h"
#include "esdr_pfbq.h"
#include "esdr_proto.h"
#include "esdr_sdio_host.h"
#include "esdr_seg.h"
#include "usb_sdr.h"

LOG_MODULE_REGISTER(bridge, LOG_LEVEL_INF);

#define LINK_SDIO DT_CHOSEN(esdr_link_sdio)

#define SDIO_CHUNK      16384U
#define SDIO_READY_MS   5000

#define REPLY_MAX          512U
#define REQUEST_TIMEOUT_MS 1000
/* Bulk IN blocks in flight or waiting: 64 KB, 16 ms at 2 MS/s. */
#define POOL      16U
#define OUT_BUFS  2U
#define OUT_BYTES ESDR_BLOCK_BYTES

#define READER_STACK 2048
#define READER_PRIO  2

/* GPIO2: USB-C port J8 to the ESP32-C6 (high) or this chip (low). GPIO12: the C6's enable. */
#define USB_MUX_PIN 2
#define C6_EN_PIN   12

static const struct device *const sdhc = DEVICE_DT_GET(LINK_SDIO);

/* DMA buffers in internal RAM. */
static uint8_t chunk[SDIO_CHUNK] __aligned(64);
static uint8_t pool[POOL][ESDR_BLOCK_BYTES] __aligned(64);
static uint8_t outb[OUT_BUFS][OUT_BYTES] __aligned(64);
K_MSGQ_DEFINE(pool_free, sizeof(uint8_t), POOL, 1);

static struct {
	uint32_t blocks, dropped, resyncs, tx_frames;
	/* Channelizer: segment blocks taken and dropped (it fell behind), channel blocks sent. */
	uint32_t seg_blocks, seg_dropped, chan_blocks, chan_dropped;
	/* Input pairs channelized, CPU cycles (mcycle) that took; of them the detector's and the
	 * channel blocks' (packing, sending). */
	uint64_t chan_pairs, chan_cycles, det_cycles, send_cycles;
} st;

extern uint64_t esdr_pfbq_prof[5];

uint64_t esdr_pfbq_now(void)
{
	return esdr_pfbq_cycles();
}

/* ---- channelizer (ESDR_REQ_SET_CHANNELIZER) ---- */

#define CHAN_STACK 4096
#define CHAN_PRIO  5
/* Working memory of the filterbank and the detector, for up to 80 channels. */
#define CHAN_MEM   (96U * 1024U)
/* Output scale: a full-scale carrier (512 raw units) in its channel gives 16384. */
#define CHAN_GAIN  32.0f
/* A channel block that waited this long goes out part full. */
#define CHAN_FLUSH_MS 5
/* No segment block for this long: the stream stopped, its last quad and block go out. */
#define CHAN_IDLE_MS  20

K_MSGQ_DEFINE(chan_q, sizeof(uint8_t), POOL, 1);
static K_THREAD_STACK_DEFINE(chan_stack, CHAN_STACK);
static struct k_thread chan_thread;

static struct {
	/* Set by the request, taken by the thread at its next block. */
	struct esdr_chan_cfg want;
	atomic_t dirty;
	/* What runs. */
	struct esdr_chan_cfg cfg;
	struct esdr_pfbq pfb;
	struct esdr_detq det;
	struct esdr_seg_track track;
	/* The channel block being filled: its pool slot, write offset, index (set at its first
	 * run: based), samples.
	 */
	int out;
	uint32_t pos, samples;
	uint64_t index;
	bool based;
	int64_t opened;
} ch = {.out = -1};

static uint8_t chan_mem[CHAN_MEM] __aligned(16);

static bool chan_on(void)
{
	return ch.want.channels != 0U;
}

static int chan_setup(void)
{
	const struct esdr_detq_cfg dc = {
		.channels = ch.cfg.channels,
		.open_ratio = ch.cfg.open_db_x10 != 0U ? powf(10.0f, ch.cfg.open_db_x10 / 100.0f)
						       : 0.0f,
	};
	const struct esdr_pfbq_cfg pc = {
		.channels = ch.cfg.channels,
		.quads = ESDR_DETQ_QUADS(16U),
		.gain = CHAN_GAIN,
	};
	size_t pn = esdr_pfbq_mem_size(&pc), dn = esdr_detq_mem_size(&dc);

	if (pn == 0U || dn == 0U || ROUND_UP(pn, 16U) + dn > sizeof(chan_mem)) {
		return -ENOMEM;
	}
	if (esdr_pfbq_init(&ch.pfb, &pc, chan_mem, pn) != 0 ||
	    esdr_detq_init(&ch.det, &dc, chan_mem + ROUND_UP(pn, 16U), dn) != 0) {
		return -EINVAL;
	}
	esdr_seg_track_reset(&ch.track);
	return 0;
}

/* The channel block being filled goes out. */
static void chan_flush(void)
{
	uint8_t *b, i;
	struct esdr_block_hdr h = {
		.magic = sys_cpu_to_le32(ESDR_BLOCK_MAGIC),
		.flags = sys_cpu_to_le16(ESDR_BLK_CHAN | ESDR_BLK_16BIT),
	};

	if (ch.out < 0) {
		return;
	}
	i = (uint8_t)ch.out;
	b = pool[i];
	ch.out = -1;
	if (ch.samples == 0U) {
		(void)k_msgq_put(&pool_free, &i, K_NO_WAIT);
		return;
	}
	h.samples = sys_cpu_to_le16((uint16_t)ch.samples);
	h.index = sys_cpu_to_le64(ch.index);
	memcpy(b, &h, sizeof(h));
	/* A zero run header ends the payload if one fits. */
	if (ch.pos + sizeof(struct esdr_chan_hdr) <= ESDR_BLOCK_BYTES) {
		memset(b + ch.pos, 0, sizeof(struct esdr_chan_hdr));
	}
	if (!usb_sdr_enabled() || usb_sdr_submit_in(b, ESDR_BLOCK_BYTES) != 0) {
		st.chan_dropped++;
		(void)k_msgq_put(&pool_free, &i, K_NO_WAIT);
		return;
	}
	st.chan_blocks++;
}

/*
 * Room for a run in the channel block being filled, a fresh one if it has none left for a run
 * header and a quad's samples: where the samples go and how many fit, NULL when no pool buffer
 * is free.
 */
static int16_t *chan_room(unsigned int *max)
{
	if (ch.out >= 0 && ch.pos + sizeof(struct esdr_chan_hdr) + 16U > ESDR_BLOCK_BYTES) {
		chan_flush();
	}
	if (ch.out < 0) {
		uint8_t i;

		if (k_msgq_get(&pool_free, &i, K_NO_WAIT) != 0) {
			st.chan_dropped++;
			return NULL;
		}
		ch.out = i;
		ch.pos = sizeof(struct esdr_block_hdr);
		ch.samples = 0;
		ch.based = false;
		ch.opened = k_uptime_get();
	}
	*max = (ESDR_BLOCK_BYTES - ch.pos - sizeof(struct esdr_chan_hdr)) / 4U;
	return (int16_t *)(pool[ch.out] + ch.pos + sizeof(struct esdr_chan_hdr));
}

/*
 * The run of @p n samples from frame @p m0 that went to chan_room()'s place: its header (as
 * words: the build's memcpy() is slow). The block's index lies two detector blocks before its
 * first run, before any run it can get after (a pre-roll reaches one block back).
 */
static void chan_commit(const struct esdr_detq_ev *ev, unsigned int n, uint64_t m0, uint32_t fl,
			bool first, bool last)
{
	const uint32_t back = 2U * ch.det.b;
	const uint32_t flags =
		((fl & ESDR_PFB_COMPLETE) == 0U ? ESDR_CHAN_TRANSIENT : 0U) |
		(first && (ev->flags & ESDR_DET_OPEN) != 0U ? ESDR_CHAN_OPEN : 0U) |
		(last && (ev->flags & ESDR_DET_CLOSE) != 0U ? ESDR_CHAN_CLOSE : 0U);
	uint32_t *h = (uint32_t *)(pool[ch.out] + ch.pos);

	if (!ch.based) {
		ch.index = m0 > back ? m0 - back : 0U;
		ch.based = true;
	}
	if (m0 < ch.index || m0 - ch.index > UINT32_MAX) {
		st.chan_dropped++;
		return;
	}
	h[0] = sys_cpu_to_le32(ev->chan | ((uint32_t)ch.cfg.channels << 16));
	h[1] = sys_cpu_to_le32(ch.cfg.channels / 2U | flags << 16);
	h[2] = sys_cpu_to_le32((uint32_t)(m0 - ch.index));
	h[3] = sys_cpu_to_le32(n);
	ch.pos += sizeof(struct esdr_chan_hdr) + 4U * n;
	ch.samples += n;
}

/* A channel to send at a block's end: its frames as channel blocks, taken into them. */
static void det_cb(const struct esdr_detq *d, const struct esdr_detq_ev *ev, void *user)
{
	uint32_t pos = 0, fl;
	uint64_t m0;
	unsigned int n, max;
	bool first = true;

	ARG_UNUSED(user);
	if (ev->chan >= 96U || (ch.cfg.mask[ev->chan / 32U] & BIT(ev->chan % 32U)) == 0U) {
		return;
	}
	uint64_t t0 = esdr_pfbq_cycles();

	while (pos < 4U * ev->n) {
		int16_t *o = chan_room(&max);

		if (o == NULL) {
			break;
		}
		n = esdr_detq_take(d, ev, &pos, o, max, &m0, &fl);
		if (n == 0U) {
			break;
		}
		chan_commit(ev, n, m0, fl, first, pos >= 4U * ev->n);
		first = false;
	}
	st.send_cycles += esdr_pfbq_cycles() - t0;
}

static void quad_cb(uint64_t m0, const int16_t *iq, uint8_t valid, uint8_t complete, void *user)
{
	uint64_t t0 = esdr_pfbq_cycles();

	ARG_UNUSED(user);
	esdr_detq_quad(&ch.det, m0, iq, valid, complete, det_cb, NULL);
	st.det_cycles += esdr_pfbq_cycles() - t0;
}

static int seg_cb(const struct esdr_seg *seg, void *user)
{
	ARG_UNUSED(user);
	st.chan_pairs += seg->pairs;
	/* A segment that goes back (a restart of the radio) starts the filterbank anew. */
	if (esdr_seg_track_gap(&ch.track, seg) == ESDR_SEG_GAP_BACK) {
		esdr_pfbq_reset(&ch.pfb);
	}
	(void)esdr_pfbq_push(&ch.pfb, seg->index, seg->iq, seg->pairs, seg->shift, seg->dc_i,
			     seg->dc_q, quad_cb, NULL);
	return 0;
}

static void chan_fn(void *a, void *b, void *c)
{
	uint8_t i;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		if (k_msgq_get(&chan_q, &i, K_MSEC(CHAN_IDLE_MS)) != 0) {
			/* The stream stopped: what waits for more input goes out. */
			if (ch.cfg.channels != 0U) {
				(void)esdr_pfbq_flush(&ch.pfb, quad_cb, NULL);
				esdr_detq_flush(&ch.det, det_cb, NULL);
			}
			chan_flush();
			continue;
		}
		if (atomic_cas(&ch.dirty, 1, 0)) {
			ch.cfg = ch.want;
			if (ch.cfg.channels != 0U && chan_setup() != 0) {
				LOG_ERR("channelizer: %u channels not supported", ch.cfg.channels);
				ch.cfg.channels = 0U;
			}
		}
		if (ch.cfg.channels != 0U) {
			uint64_t t0 = esdr_pfbq_cycles();

			if (esdr_seg_parse(pool[i], ESDR_BLOCK_BYTES, seg_cb, NULL) < 0) {
				st.seg_dropped++;
			}
			/* A part full channel block waits a little for more runs. */
			if (ch.out >= 0 && k_uptime_get() - ch.opened >= CHAN_FLUSH_MS) {
				chan_flush();
			}
			st.chan_cycles += esdr_pfbq_cycles() - t0;
		} else {
			chan_flush();
		}
		(void)k_msgq_put(&pool_free, &i, K_NO_WAIT);
	}
}

static int set_channelizer(const uint8_t *data, size_t len)
{
	struct esdr_chan_cfg c;

	if (len != sizeof(c)) {
		return -EINVAL;
	}
	memcpy(&c, data, sizeof(c));
	c.channels = sys_le16_to_cpu(c.channels);
	c.open_db_x10 = sys_le16_to_cpu(c.open_db_x10);
	for (int k = 0; k < 3; k++) {
		c.mask[k] = sys_le32_to_cpu(c.mask[k]);
	}
	if (c.channels != 0U && c.channels != 16U && c.channels != 32U && c.channels != 40U &&
	    c.channels != 64U && c.channels != 80U) {
		return -EINVAL;
	}
	ch.want = c;
	atomic_set(&ch.dirty, 1);
	return 0;
}

/* ---- SDIO (../../common/src/esdr_sdio_host.c) ---- */

static int link_send(const void *a, size_t na, const void *b, size_t nb)
{
	return esdr_sdio_send(a, na, b, nb, REQUEST_TIMEOUT_MS);
}

static int link_open(void)
{
	return esdr_sdio_open(sdhc, SDIO_READY_MS);
}

/* ---- requests ---- */

static K_MUTEX_DEFINE(req_lock);
static K_SEM_DEFINE(reply_sem, 0, 1);
static struct k_spinlock pend_lock;
static struct {
	uint8_t req;
	bool waiting;
	void *buf;
	uint16_t max, len;
	int status;
} pend;

static int request(uint8_t req, uint16_t value, const void *data, uint16_t len, void *reply,
		   uint16_t reply_max)
{
	struct esdr_ser_hdr h = {
		.magic = sys_cpu_to_le32(ESDR_SER_MAGIC),
		.req = req,
		.flags = reply != NULL ? ESDR_SER_IN : 0U,
		.value = sys_cpu_to_le16(value),
		.len = sys_cpu_to_le16(reply != NULL ? reply_max : len),
	};
	int ret;

	k_mutex_lock(&req_lock, K_FOREVER);
	k_sem_reset(&reply_sem);
	pend.req = req;
	pend.buf = reply;
	pend.max = reply_max;
	pend.len = 0;
	pend.status = 0;
	pend.waiting = true;
	ret = link_send(&h, sizeof(h), data, reply == NULL ? len : 0U);
	if (ret == 0) {
		if (k_sem_take(&reply_sem, K_MSEC(REQUEST_TIMEOUT_MS)) != 0) {
			ret = -ETIMEDOUT;
		} else {
			ret = pend.status < 0 ? pend.status : (int)pend.len;
		}
	}
	/* A reply after this finds nobody: the buffer may be gone (a USB request's). */
	k_spinlock_key_t key = k_spin_lock(&pend_lock);

	pend.waiting = false;
	k_spin_unlock(&pend_lock, key);
	k_mutex_unlock(&req_lock);
	return ret;
}

/* ---- the C6's stream: frames and blocks ---- */

static struct {
	uint8_t hdr[sizeof(struct esdr_ser_hdr) + REPLY_MAX];
	/* Bytes of the frame or block in hand; the block's pool buffer (NULL: dropped). */
	uint32_t have, need;
	uint8_t *blk;
	bool is_blk;
} rx;

static void block_done(uint8_t *b)
{
	st.blocks++;
	if (b == NULL) {
		st.dropped++;
		return;
	}
	if (chan_on() && (sys_get_le16(b + offsetof(struct esdr_block_hdr, flags)) &
			  ESDR_BLK_SEG) != 0U) {
		uint8_t i = (uint8_t)((b - pool[0]) / ESDR_BLOCK_BYTES);

		st.seg_blocks++;
		if (k_msgq_put(&chan_q, &i, K_NO_WAIT) != 0) {
			st.seg_dropped++;
			(void)k_msgq_put(&pool_free, &i, K_NO_WAIT);
		}
		return;
	}
	if (!usb_sdr_enabled() || usb_sdr_submit_in(b, ESDR_BLOCK_BYTES) != 0) {
		uint8_t i = (uint8_t)((b - pool[0]) / ESDR_BLOCK_BYTES);

		st.dropped++;
		(void)k_msgq_put(&pool_free, &i, K_NO_WAIT);
	}
}

static void frame_done(void)
{
	struct esdr_ser_hdr h;

	memcpy(&h, rx.hdr, sizeof(h));
	k_spinlock_key_t key = k_spin_lock(&pend_lock);

	if (pend.waiting && h.req == pend.req) {
		pend.status = (int16_t)sys_le16_to_cpu(h.status);
		pend.len = MIN(sys_le16_to_cpu(h.len), pend.max);
		if (pend.buf != NULL) {
			memcpy(pend.buf, rx.hdr + sizeof(h), pend.len);
		}
		pend.waiting = false;
		k_sem_give(&reply_sem);
	}
	k_spin_unlock(&pend_lock, key);
}

static void parse(const uint8_t *p, uint32_t n)
{
	while (n > 0U) {
		if (rx.have < 4U) {
			uint8_t c = *p++;

			n--;
			if (rx.have == 0U && c == 0U) {
				/* Padding to whole words between frames. */
				continue;
			}
			rx.hdr[rx.have++] = c;
			if (rx.have == 4U) {
				uint32_t magic = sys_get_le32(rx.hdr);

				if (magic == ESDR_BLOCK_MAGIC) {
					uint8_t i;

					rx.is_blk = true;
					rx.need = ESDR_BLOCK_BYTES;
					rx.blk = k_msgq_get(&pool_free, &i, K_NO_WAIT) == 0 ? pool[i]
											    : NULL;
					if (rx.blk != NULL) {
						memcpy(rx.blk, rx.hdr, 4);
					}
				} else if (magic == ESDR_SER_MAGIC) {
					rx.is_blk = false;
					rx.need = sizeof(struct esdr_ser_hdr);
				} else {
					/* Not a magic: slide by a byte. */
					memmove(rx.hdr, rx.hdr + 1, 3);
					rx.have = 3;
					st.resyncs++;
				}
			}
			continue;
		}
		uint32_t k = MIN(n, rx.need - rx.have);

		if (rx.is_blk) {
			if (rx.blk != NULL) {
				memcpy(rx.blk + rx.have, p, k);
			}
		} else {
			memcpy(rx.hdr + rx.have, p, k);
		}
		rx.have += k;
		p += k;
		n -= k;
		if (rx.have < rx.need) {
			continue;
		}
		if (rx.is_blk) {
			block_done(rx.blk);
			rx.have = 0;
			continue;
		}
		if (rx.need == sizeof(struct esdr_ser_hdr)) {
			/* A reply's data, if any, follows its header. */
			uint32_t len = sys_le16_to_cpu(((struct esdr_ser_hdr *)rx.hdr)->len);

			if (len > 0U && len <= REPLY_MAX) {
				rx.need += len;
				continue;
			}
		}
		frame_done();
		rx.have = 0;
	}
}

static void reader_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		int n = esdr_sdio_read(chunk, SDIO_CHUNK);

		if (n <= 0) {
			k_sleep(K_TICKS(1));
			continue;
		}
		parse(chunk, (uint32_t)n);
	}
}
K_THREAD_STACK_DEFINE(reader_stack, READER_STACK);
static struct k_thread reader_thread;

/* ---- USB ---- */

/* ESDR_REQ_USB_TO_C6: J8 to the C6 once the reply is out, back to this chip later. */
static uint32_t usb_to_c6_s;

static void usb_mux(bool c6)
{
	(void)gpio_pin_set(DEVICE_DT_GET(DT_NODELABEL(gpio0)), USB_MUX_PIN, c6 ? 1 : 0);
}

static void usb_back_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	usb_mux(false);
	LOG_INF("J8 back from the C6");
}
static K_WORK_DELAYABLE_DEFINE(usb_back_work, usb_back_fn);

static void usb_to_c6_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	LOG_INF("J8 to the C6 for %u s", usb_to_c6_s);
	usb_mux(true);
	(void)k_work_reschedule(&usb_back_work, K_SECONDS(usb_to_c6_s));
}
static K_WORK_DELAYABLE_DEFINE(usb_to_c6_work, usb_to_c6_fn);

int usb_sdr_control_out(uint8_t req, uint16_t value, const uint8_t *data, size_t len)
{
	int ret;

	if (req == ESDR_REQ_USB_TO_C6) {
		usb_to_c6_s = value != 0U ? value : 30U;
		(void)k_work_reschedule(&usb_to_c6_work, K_MSEC(50));
		return 0;
	}
	if (req == ESDR_REQ_SET_CHANNELIZER) {
		return set_channelizer(data, len);
	}
	ret = request(req, value, data, (uint16_t)len, NULL, 0);
	return ret < 0 ? ret : 0;
}

int usb_sdr_control_in(uint8_t req, uint16_t value, uint8_t *buf, size_t max)
{
	if (req == ESDR_REQ_SET_CHANNELIZER) {
		struct esdr_chan_stats cs = {
			.seg_blocks = st.seg_blocks,
			.seg_dropped = st.seg_dropped,
			.chan_blocks = st.chan_blocks,
			.chan_dropped = st.chan_dropped,
			.pairs = st.chan_pairs,
			.cycles = st.chan_cycles,
			.cpu_mhz = (uint32_t)MAX(app_clock_cpu_max(), 0),
			.det_cycles = st.det_cycles,
			.send_cycles = st.send_cycles,
			.branch_cycles = esdr_pfbq_prof[0],
			.fft_cycles = esdr_pfbq_prof[1],
		};

		memcpy(buf, &cs, MIN(max, sizeof(cs)));
		return (int)MIN(max, sizeof(cs));
	}
	return request(req, value, NULL, 0, buf, (uint16_t)MIN(max, REPLY_MAX));
}

void usb_sdr_in_done(uint8_t *data, size_t len, int err)
{
	uint8_t i = (uint8_t)((data - pool[0]) / ESDR_BLOCK_BYTES);

	ARG_UNUSED(len);
	ARG_UNUSED(err);
	(void)k_msgq_put(&pool_free, &i, K_NO_WAIT);
}

void usb_sdr_out_done(uint8_t *data, size_t len, int err)
{
	if (err == 0 && len > 0U) {
		struct esdr_ser_hdr h = {
			.magic = sys_cpu_to_le32(ESDR_SER_MAGIC),
			.req = ESDR_SER_TX_DATA,
			.len = sys_cpu_to_le16((uint16_t)len),
		};

		if (link_send(&h, sizeof(h), data, len) == 0) {
			st.tx_frames++;
		}
	}
	if (err != -ECONNABORTED && usb_sdr_enabled()) {
		(void)usb_sdr_submit_out(data, OUT_BYTES);
	}
}

static void link_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	if (usb_sdr_enabled()) {
		for (uint32_t k = 0; k < OUT_BUFS; k++) {
			(void)usb_sdr_submit_out(outb[k], OUT_BYTES);
		}
	} else {
		/* The host went: the radio stops. */
		(void)request(ESDR_REQ_SET_MODE, ESDR_MODE_OFF, NULL, 0, NULL, 0);
	}
}
static K_WORK_DEFINE(link_work, link_work_fn);

void usb_sdr_link(bool up)
{
	ARG_UNUSED(up);
	k_work_submit(&link_work);
}

static void msg_cb(struct usbd_context *const ctx, const struct usbd_msg *const msg)
{
	if (usbd_can_detect_vbus(ctx)) {
		if (msg->type == USBD_MSG_VBUS_READY) {
			(void)usbd_enable(ctx);
		}
		if (msg->type == USBD_MSG_VBUS_REMOVED) {
			(void)usbd_disable(ctx);
		}
	}
}

int main(void)
{
	const struct device *gpio = DEVICE_DT_GET(DT_NODELABEL(gpio0));
	struct usbd_context *ctx;
	struct esdr_info info;
	int ret;

	LOG_INF("CPU %d MHz", app_clock_cpu_max());
	/* J8 to this chip's OTG HS port; the C6 out of reset. */
	(void)gpio_pin_configure(gpio, USB_MUX_PIN, GPIO_OUTPUT_LOW);
	(void)gpio_pin_configure(gpio, C6_EN_PIN, GPIO_OUTPUT_HIGH);

	for (uint8_t i = 0; i < POOL; i++) {
		(void)k_msgq_put(&pool_free, &i, K_NO_WAIT);
	}
	ret = link_open();
	/* The reader enumerates the C6 again whenever it answers nothing (a reset, a new image). */
	k_thread_create(&reader_thread, reader_stack, K_THREAD_STACK_SIZEOF(reader_stack),
			reader_fn, NULL, NULL, NULL, READER_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&reader_thread, "sdio_reader");
	k_thread_create(&chan_thread, chan_stack, K_THREAD_STACK_SIZEOF(chan_stack), chan_fn, NULL,
			NULL, NULL, CHAN_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&chan_thread, "channelizer");
	if (ret != 0) {
		/* USB still comes up: ESDR_REQ_USB_TO_C6 then lets esptool reach the C6. */
		LOG_ERR("SDIO to the C6: %d", ret);
	} else {
		ret = request(ESDR_REQ_GET_INFO, 0, NULL, 0, &info, sizeof(info));
		if (ret < 0 || sys_le32_to_cpu(info.magic) != ESDR_INFO_MAGIC) {
			LOG_ERR("C6 info: %d", ret);
		} else {
			LOG_INF("radio: %s, receive up to %u S/s", info.firmware,
				sys_le32_to_cpu(info.rx_rate_max_hz));
		}
	}

	ctx = sample_usbd_init_device(msg_cb);
	if (ctx == NULL) {
		LOG_ERR("USB device init failed");
		return 0;
	}
	if (!usbd_can_detect_vbus(ctx)) {
		ret = usbd_enable(ctx);
		if (ret != 0) {
			LOG_ERR("USB enable: %d", ret);
			return 0;
		}
	}
	LOG_INF("USB up");
	for (;;) {
		k_sleep(K_SECONDS(10));
		LOG_INF("blocks %u, dropped %u, resyncs %u, tx frames %u", st.blocks, st.dropped,
			st.resyncs, st.tx_frames);
		if (st.seg_blocks != 0U) {
			LOG_INF("channelizer: %u segment blocks (%u dropped), %u channel blocks (%u "
				"dropped)", st.seg_blocks, st.seg_dropped, st.chan_blocks,
				st.chan_dropped);
		}
	}
	return 0;
}
