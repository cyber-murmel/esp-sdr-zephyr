/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Captures as VITA 49.2: one stream per channel (stream ID base + channel), a context packet
 * per stream when it first appears and once a second, then signal data packets of 16-bit
 * link-efficient items. Timestamps come from the ring pair index (TSI "other": seconds since
 * the run started, TSF real time in picoseconds). The trailer marks the capture's first packet
 * (sample loss: the stream is not contiguous), its last (user bit 8), a cut at max_capture
 * (user bit 9) and a chunk of a continuous stream (user bit 10). A context packet before each
 * capture carries the channel's noise floor as the reference level (dBFS) and the burst's
 * peak above it as gain stage 1 (dB).
 *
 * Packets go to USB in 4 KiB blocks (whole packets per block): the encoder fills the current
 * block on the callback thread and submits it when the next packet does not fit, or 50 ms
 * after the first packet in it.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_chancap.h>
#include <esp_sdr/esp_sdr_ring.h>

#include <vrt/vrt_init.h>
#include <vrt/vrt_string.h>
#include <vrt/vrt_types.h>
#include <vrt/vrt_util.h>
#include <vrt/vrt_words.h>
#include <vrt/vrt_write.h>

#include "app.h"
#include "usb_cc.h"

LOG_MODULE_DECLARE(chancap, LOG_LEVEL_INF);

#define SAMPLES_PER_PACKET CONFIG_APP_SAMPLES_PER_PACKET
/* Header, stream ID, TSI, two TSF words, trailer. */
#define DATA_OVERHEAD      6
#define PACKET_WORDS       (SAMPLES_PER_PACKET + DATA_OVERHEAD)
#define BLOCK_BYTES        4096U
#define CONTEXT_PERIOD_MS  1000
#define FLUSH_MS           50
#define CHANNELS           ESP_SDR_CHANCAP_CHANNELS_MAX
#define PS_PER_PAIR        62500U /* 1 / 16 MHz */

BUILD_ASSERT(PACKET_WORDS * 4U <= BLOCK_BYTES, "a packet must fit a block");

struct block {
	uint8_t data[BLOCK_BYTES];
	size_t len;
};

/* USB DMA needs internal RAM. */
ESP_SDR_HIGH_BUF_DEFINE(struct block, blocks, [CONFIG_APP_USB_BLOCKS], 16);
K_MSGQ_DEFINE(free_q, sizeof(struct block *), CONFIG_APP_USB_BLOCKS, 4);
static struct block *cur;
static K_MUTEX_DEFINE(cur_lock);
static struct k_work_delayable flush_work;

static struct {
	uint8_t data_count, context_count;
	int64_t context_ms;
	bool seen;
} stream[CHANNELS];
static struct vrt_cc_stats stats;
/* Above the capture bank: the DRAM image below the ring banks has no room. */
static struct vrt_packet pkt ESP_SDR_HIGH_RAM;
static uint32_t body[SAMPLES_PER_PACKET] ESP_SDR_HIGH_RAM;

static void pair_time(uint64_t pair, struct vrt_fields *f)
{
	f->integer_seconds_timestamp = (uint32_t)(pair / ESP_SDR_RING_RATE_HZ);
	f->fractional_seconds_timestamp = (pair % ESP_SDR_RING_RATE_HZ) * PS_PER_PAIR;
}

static void init_header(struct vrt_packet *p, enum vrt_packet_type type, unsigned int ch,
			uint8_t count)
{
	vrt_init_packet(p);
	p->header.packet_type = type;
	p->header.packet_count = count & 0xfU;
	p->header.tsi = VRT_TSI_OTHER;
	p->header.tsf = VRT_TSF_REAL_TIME;
	p->fields.stream_id = CONFIG_APP_STREAM_ID_BASE + ch;
}

/* ---- blocks ---- */

static void submit_cur_locked(void)
{
	struct block *b = cur;

	if (b == NULL || b->len == 0U) {
		return;
	}
	cur = NULL;
	if (usb_cc_submit_in(b->data, b->len) != 0) {
		stats.errors++;
		b->len = 0;
		(void)k_msgq_put(&free_q, &b, K_NO_WAIT);
		return;
	}
	stats.blocks++;
	stats.bytes += b->len;
}

static void flush_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	k_mutex_lock(&cur_lock, K_FOREVER);
	submit_cur_locked();
	k_mutex_unlock(&cur_lock);
}

/* Room for @p bytes in the current block, submitting a full one; NULL if no block is free. */
static uint8_t *block_room(size_t bytes)
{
	if (cur != NULL && cur->len + bytes > BLOCK_BYTES) {
		submit_cur_locked();
	}
	if (cur == NULL) {
		if (k_msgq_get(&free_q, &cur, K_NO_WAIT) != 0) {
			return NULL;
		}
		cur->len = 0;
		k_work_schedule(&flush_work, K_MSEC(FLUSH_MS));
	}
	return cur->data + cur->len;
}

void usb_cc_in_done(uint8_t *data, size_t len, int err)
{
	struct block *b = CONTAINER_OF(data, struct block, data[0]);

	ARG_UNUSED(len);
	if (err != 0 && err != -ECONNABORTED) {
		stats.errors++;
	}
	b->len = 0;
	(void)k_msgq_put(&free_q, &b, K_NO_WAIT);
}

void usb_cc_link(bool up)
{
	if (!up) {
		usb_cc_cancel();
	}
	for (unsigned int c = 0; c < CHANNELS; c++) {
		stream[c].seen = false;
	}
}

/* Write the packet into the block; false if no block was free. */
static bool emit(struct vrt_packet *p)
{
	int32_t words = vrt_words_packet(p);
	uint8_t *at;
	int32_t rv;

	if (words < 0) {
		stats.errors++;
		return true;
	}
	p->header.packet_size = (uint16_t)words;
	at = block_room((size_t)words * 4U);
	if (at == NULL) {
		return false;
	}
	rv = vrt_write_packet(p, at, words, true);
	if (rv < 0) {
		LOG_ERR("vrt write: %s", vrt_string_error(rv));
		stats.errors++;
		return true;
	}
	vrt_to_big_endian((uint32_t *)at, rv);
	cur->len += (size_t)rv * 4U;
	return true;
}

static bool send_context(const struct esp_sdr_chancap *cap, bool changed)
{
	struct esp_sdr_chancap_cfg *cfg = &cc_settings()->cfg;
	unsigned int ch = cap->channel;
	struct vrt_data_packet_payload_format *fmt = &pkt.if_context.data_packet_payload_format;

	init_header(&pkt, VRT_PT_IF_CONTEXT, ch, stream[ch].context_count++);
	pkt.header.nd0 = true;
	pkt.if_context.has.v49_spec_compliance = true;
	pkt.if_context.v49_spec_compliance = VRT_V49_2;
	pair_time(cap->index, &pkt.fields);
	pkt.if_context.context_field_change_indicator = changed;
	pkt.if_context.has.rf_reference_frequency = true;
	pkt.if_context.rf_reference_frequency =
		(double)cfg->first_freq_hz + (double)ch * (double)cfg->spacing_hz;
	pkt.if_context.has.sample_rate = true;
	pkt.if_context.sample_rate = (double)cap->sample_rate;
	pkt.if_context.has.bandwidth = true;
	pkt.if_context.bandwidth = (double)cap->sample_rate;
	pkt.if_context.has.reference_level = true;
	pkt.if_context.reference_level = (float)cap->noise_db;
	pkt.if_context.has.gain = true;
	pkt.if_context.gain.stage1 = (float)(cap->peak_db - cap->noise_db);
	pkt.if_context.gain.stage2 = 0.0f;
	pkt.if_context.has.data_packet_payload_format = true;
	fmt->packing_method = VRT_PM_LINK_EFFICIENT;
	fmt->real_or_complex = VRT_ROC_COMPLEX_CARTESIAN;
	fmt->data_item_format = VRT_DIF_SIGNED_FIXED_POINT;
	fmt->item_packing_field_size = 15;
	fmt->data_item_size = 15;
	if (!emit(&pkt)) {
		return false;
	}
	stats.context++;
	stream[ch].context_ms = k_uptime_get();
	stream[ch].seen = true;
	return true;
}

void vrt_cc_capture(void *user, const struct esp_sdr_chancap *cap)
{
	unsigned int ch = cap->channel;
	uint64_t pairs_per_sample = ESP_SDR_RING_RATE_HZ / cap->sample_rate;
	bool ok;

	ARG_UNUSED(user);
	if (ch >= CHANNELS || !usb_cc_enabled()) {
		esp_sdr_chancap_release(cap);
		return;
	}
	k_mutex_lock(&cur_lock, K_FOREVER);
	stats.captures++;
	ok = send_context(cap, !stream[ch].seen ||
				     k_uptime_get() - stream[ch].context_ms >= CONTEXT_PERIOD_MS);
	for (size_t off = 0; ok && off < cap->n; off += SAMPLES_PER_PACKET) {
		size_t n = MIN((size_t)SAMPLES_PER_PACKET, cap->n - off);
		bool last = off + n >= cap->n, clipped = false;

		for (size_t j = 0; j < n; j++) {
			int16_t i = cap->iq[2U * (off + j)], q = cap->iq[2U * (off + j) + 1U];

			clipped |= i == INT16_MAX || i == INT16_MIN || q == INT16_MAX ||
				   q == INT16_MIN;
			/* Link efficient 16 bit: I in the upper half word. */
			body[j] = ((uint32_t)(uint16_t)i << 16) | (uint16_t)q;
		}
		init_header(&pkt, VRT_PT_IF_DATA_WITH_STREAM_ID, ch, stream[ch].data_count++);
		pair_time(cap->index + off * pairs_per_sample, &pkt.fields);
		pkt.header.has.trailer = true;
		pkt.body = body;
		pkt.words_body = (int32_t)n;
		pkt.trailer.has.valid_data = true;
		pkt.trailer.valid_data = true;
		pkt.trailer.has.over_range = true;
		pkt.trailer.over_range = clipped;
		pkt.trailer.has.detected_signal = true;
		pkt.trailer.detected_signal = !cap->chunk;
		pkt.trailer.has.sample_loss = true;
		pkt.trailer.sample_loss = off == 0U;
		pkt.trailer.has.user_defined8 = true;
		pkt.trailer.user_defined8 = last;
		pkt.trailer.has.user_defined9 = true;
		pkt.trailer.user_defined9 = cap->truncated;
		pkt.trailer.has.user_defined10 = true;
		pkt.trailer.user_defined10 = cap->chunk;
		ok = emit(&pkt);
		if (ok) {
			stats.packets++;
		}
	}
	if (!ok) {
		stats.dropped++;
	}
	/*
	 * The block goes out when full or FLUSH_MS after its first packet: short captures share
	 * blocks, since USB full speed moves a limited number of transfers per second.
	 */
	k_mutex_unlock(&cur_lock);
	esp_sdr_chancap_release(cap);
}

int vrt_cc_init(void)
{
	k_work_init_delayable(&flush_work, flush_fn);
	for (unsigned int k = 0; k < CONFIG_APP_USB_BLOCKS; k++) {
		struct block *b = &blocks[k];

		b->len = 0;
		(void)k_msgq_put(&free_q, &b, K_NO_WAIT);
	}
	return 0;
}

void vrt_cc_get_stats(struct vrt_cc_stats *st)
{
	*st = stats;
	st->blocks_free = k_msgq_num_used_get(&free_q);
}
