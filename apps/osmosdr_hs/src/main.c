/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * An ESP32-S31 as a USB high-speed osmosdr device (esdr_proto.h), the radio on the same chip. The
 * ring's nodes (esp_sdr_diag16.h) are blocks of the wire format with their header left out: the
 * header goes into the hole, and eight blocks at a time go to USB in memory of their own. See
 * ../README.rst.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/usbd.h>

#include <zephyr/app_version.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_diag16.h>
#include <esp_sdr/esp_sdr_rx.h>

#include <sample_usbd.h>

#include "esdr_cal.h"
#include "esdr_dev.h"
#include "esdr_proto.h"
#include "usb_sdr.h"

LOG_MODULE_REGISTER(osmosdr_hs, LOG_LEVEL_INF);

BUILD_ASSERT(ESP_SDR_DIAG16_NODE == ESDR_BLOCK_BYTES, "a node is a block");
BUILD_ASSERT(ESP_SDR_DIAG16_HOLE == sizeof(struct esdr_block_hdr), "the hole holds the header");

/*
 * 20 MS/s fills high-speed bulk's share. The ring takes 160 MHz over an even divider; down to 2
 * MS/s that is these (ESDR_REQ_GET_RATES). Hosts that know only max >> k get 20, 10 and 5.
 */
#define RATE_MAX_HZ 20000000U
#define RATE_COUNT  3U
#define RATE_START  RATE_MAX_HZ
static const uint32_t rates_hz[] = {20000000U, 16000000U, 10000000U, 8000000U, 5000000U,
				    4000000U, 3200000U, 2500000U, 2000000U};
BUILD_ASSERT(ARRAY_SIZE(rates_hz) <= ESDR_RATES_MAX, "more rates than a reply holds");

/* The S31's PLL, with no divided LO; the top end differs a little between chips. */
#define FREQ_MIN_HZ 2200000000ULL
#define FREQ_MAX_HZ 2900000000ULL

/* Eight blocks a transfer, a quarter of the ring: 1.2 kHz at 20 MS/s. */
#define BATCH_NODES   8U
#define BATCH_BYTES   (BATCH_NODES * ESDR_BLOCK_BYTES)
BUILD_ASSERT(CONFIG_ESP_SDR_DIAG16_RING_KB * 1024U % BATCH_BYTES == 0U, "whole batches in the ring");
/*
 * Transfers waiting for the host. The ring never waits, so they have memory of their own: a host
 * that falls behind gets whole blocks or none, never what the DMA has written over them. Copying,
 * that is in_buf; with CONFIG_APP_HS_ZEROCOPY it is the ring's slabs themselves, as many spare
 * ones as the library has (CONFIG_ESP_SDR_DIAG16_SPARE_SLABS, this many).
 */
#define IN_FLIGHT_MAX 3
BUILD_ASSERT(!IS_ENABLED(CONFIG_APP_HS_ZEROCOPY) || BATCH_NODES == ESP_SDR_DIAG16_SLAB_NODES,
	     "a batch is a slab");
#if defined(CONFIG_APP_HS_ZEROCOPY)
BUILD_ASSERT(CONFIG_ESP_SDR_DIAG16_SPARE_SLABS >= IN_FLIGHT_MAX, "a spare for every transfer");
#endif

/* Three transfers take some 3 ms; longer, and the host has stopped reading. */
#define DRAIN_MS  20
#define CANCEL_MS 200

#define OUT_BYTES ESDR_BLOCK_BYTES
#define BW_MIN_MHZ 13U
#define POLL_MS    100

struct settings {
	uint16_t mode;
	uint16_t rx_gain;
	uint64_t freq_hz;
	uint32_t rate_hz;
	uint32_t bandwidth_hz;
	int32_t corr_ppb;
};

/* Written by the USB thread, read by the stream thread: guarded by the lock. */
static struct k_spinlock want_lock;
static struct settings want = {
	.mode = ESDR_MODE_OFF,
	.rx_gain = ESDR_GAIN_AUTO,
	.freq_hz = 2412000000ULL,
	.rate_hz = RATE_START,
};
/*
 * Where the radio is, for GET_STATE: written by the stream thread, also under the lock. plan_hz is 0
 * after a failed tune; what the PLL's whole kHz leave moves with the calibration, so it is added when
 * read.
 */
static struct {
	uint64_t plan_hz;
	uint32_t mhz;
	int32_t khz;
} tuned;
/* What the radio has; the stream thread's. */
static struct settings have;
static bool have_valid;
static int tune_ret;
static K_SEM_DEFINE(kick, 0, 1);
/* Read once: the register goes through a rebuild while the stream thread retunes. */
static uint16_t gain_max;
/* Counts requests that change the stream; run_gen is what the running stream was started from. */
static atomic_t want_gen;
static atomic_val_t run_gen;

static struct esdr_stats stats;
static atomic_t in_flight;
#if !defined(CONFIG_APP_HS_ZEROCOPY)
static uint8_t in_buf[IN_FLIGHT_MAX][BATCH_BYTES] __aligned(64);
static uint8_t in_next;
#endif

/* The batch being gathered; the stream thread's. */
static uint8_t *batch_first;
static uint64_t batch_index;
static uint32_t batch_n;
static uint16_t next_flags;
static uint64_t next_index;
static uint64_t index_base;

/* The host said stop: what comes next is a new stream, however soon. */
static atomic_t stream_ended;
/* When the last run that delivered anything stopped; 0 starts a new stream's time axis. */
static uint64_t run_end_cycles;
static uint32_t run_rate_hz;
static bool run_first;

static uint8_t out_buf[2][OUT_BYTES] __aligned(64);

/* A request that changed the stream: the ring lets go, the stream thread takes it from there. */
static void stream_replan(void)
{
	atomic_inc(&want_gen);
	esp_sdr_diag16_stop();
	k_sem_give(&kick);
}

#if defined(MULTIAPP_NAME)
/* The host wants main() to end (multiapp_app_stop()). */
static atomic_t quit;
#define HS_QUIT() (atomic_get(&quit) != 0)
#else
#define HS_QUIT() false
#endif

static void batch_drop(uint32_t n)
{
	/* The host is behind: these go, the next block says so. */
	stats.rx_overflows++;
	stats.rx_overflow_samples += (uint64_t)n * ESP_SDR_DIAG16_PAYLOAD / 2U;
	next_flags |= ESDR_BLK_GAP | ESDR_BLK_OVERFLOW;
}

#if defined(CONFIG_APP_HS_ZEROCOPY)
/*
 * The batch's nodes are the tail of one slab of the ring (it ends where the slab does). The library
 * swaps the slab for a spare, looking at how far the DMA is before and after, and the transfer reads
 * the old memory as it stands: nothing is copied. Whatever the library refuses is dropped as
 * before, and the ring is untouched by it.
 */
static void batch_send(void)
{
	size_t bytes = batch_n * ESDR_BLOCK_BYTES;
	uint8_t *first = batch_first;
	uint32_t n = batch_n;
	uint8_t *slab;

	batch_first = NULL;
	batch_n = 0U;
	if (n == 0U) {
		return;
	}
	if (!IS_ENABLED(CONFIG_APP_HS_SELFTEST) && !usb_sdr_enabled()) {
		batch_drop(n);
		return;
	}
	if (esp_sdr_diag16_slab_detach(batch_index, &slab) != 0) {
		batch_drop(n);
		return;
	}
	if (IS_ENABLED(CONFIG_APP_HS_SELFTEST)) {
		/* The swap's cost is the point: the memory goes straight back. */
		stats.rx_blocks += n;
		(void)esp_sdr_diag16_slab_release(slab);
		return;
	}
	atomic_inc(&in_flight);
	if (usb_sdr_submit_in(first, bytes) != 0) {
		atomic_dec(&in_flight);
		(void)esp_sdr_diag16_slab_release(slab);
		batch_drop(n);
	}
}
#else
static void batch_send(void)
{
	size_t bytes = batch_n * ESDR_BLOCK_BYTES;
	const uint8_t *first = batch_first;
	uint8_t *copy = in_buf[in_next];
	uint32_t n = batch_n;

	batch_first = NULL;
	batch_n = 0U;
	if (n == 0U) {
		return;
	}
	if (!IS_ENABLED(CONFIG_APP_HS_SELFTEST) &&
	    (atomic_get(&in_flight) >= IN_FLIGHT_MAX || !usb_sdr_enabled())) {
		batch_drop(n);
		return;
	}
	memcpy(copy, first, bytes);
	/* A thread held off long enough has copied what the DMA was writing. */
	if (!esp_sdr_diag16_node_valid(batch_index)) {
		batch_drop(n);
		return;
	}
	if (IS_ENABLED(CONFIG_APP_HS_SELFTEST)) {
		stats.rx_blocks += n;
		return;
	}
	atomic_inc(&in_flight);
	if (usb_sdr_submit_in(copy, bytes) != 0) {
		atomic_dec(&in_flight);
		batch_drop(n);
		return;
	}
	/* Transfers finish in order and fewer than all are out: the next one is free. */
	in_next = (in_next + 1U) % IN_FLIGHT_MAX;
}
#endif

static void ring_sink(void *user, uint8_t *node, uint64_t index)
{
	struct esdr_block_hdr *h = (struct esdr_block_hdr *)node;
	/* From the index, never the address: after a swap the nodes are not one run of memory. */
	uint32_t nr = esp_sdr_diag16_node_nr(index);
	uint64_t sample;

	ARG_UNUSED(user);
	/*
	 * esp_sdr_diag16_run() clears a stop requested just before it starts: this ends that run.
	 * Quitting counts too: a stop that came after main() read its generation is not a new one.
	 */
	if (atomic_get(&want_gen) != run_gen || HS_QUIT()) {
		esp_sdr_diag16_stop();
	}
	if (run_first) {
		run_first = false;
		if (run_end_cycles != 0U) {
			/* The index is the time axis: the restart, phase check and servo count. */
			uint64_t gap = (k_cycle_get_64() - run_end_cycles) * run_rate_hz /
				       sys_clock_hw_cycles_per_sec();

			index_base += gap;
			next_index = index_base;
			stats.rx_restart_samples += gap;
		}
	}
	sample = index_base + index / 2U;
	if (sample != next_index) {
		/* The consumer fell a ring behind: the samples between are gone, and so is what
		 * was gathered, which the DMA has been over.
		 */
		next_flags |= ESDR_BLK_GAP;
		stats.rx_ring_lost_pairs += sample - next_index +
					    (uint64_t)batch_n * ESP_SDR_DIAG16_PAYLOAD / 2U;
		batch_first = NULL;
		batch_n = 0U;
	}
	next_index = sample + ESP_SDR_DIAG16_PAYLOAD / 2U;
	h->magic = ESDR_BLOCK_MAGIC;
	h->flags = next_flags;
	h->samples = ESP_SDR_DIAG16_PAYLOAD / 2U;
	h->index = sample;
	next_flags = 0U;
	if (batch_first == NULL) {
		batch_first = node;
		batch_index = index;
	}
	batch_n++;
	if (nr % BATCH_NODES == BATCH_NODES - 1U) {
		batch_send();
	}
}

static void radio_bandwidth(const struct settings *s)
{
	uint32_t lo, hi, mhz;

	esp_sdr_rx_bandwidth_range(&lo, &hi);
	/* Open to what the rate holds, as far as the filter goes. */
	mhz = s->bandwidth_hz != 0U ? s->bandwidth_hz / 1000000U :
				      MAX(s->rate_hz / 1000000U, BW_MIN_MHZ);
	(void)esp_sdr_rx_set_bandwidth(CLAMP(mhz, lo, hi));
}

/*
 * What changed since the last time: each of them retunes, and rebuilds the gain tables. The host's
 * correction goes into the LO plan, on top of the library's, which stays the board's calibration.
 *
 * @return 0, or why the frequency cannot be tuned: the same until a request changes it.
 */
static int radio_apply(const struct settings *s)
{
	if (!have_valid || s->freq_hz != have.freq_hz || s->corr_ppb != have.corr_ppb) {
		/* A fast crystal puts the LO high: program the opposite offset. */
		int64_t corr_hz = (int64_t)s->freq_hz * s->corr_ppb / 1000000000LL;
		int64_t hz = (int64_t)s->freq_hz - corr_hz;
		uint32_t mhz = (uint32_t)((hz + 500000) / 1000000);
		int32_t rest = (int32_t)(hz - (int64_t)mhz * 1000000);
		int32_t khz = (rest + (rest < 0 ? -500 : 500)) / 1000;
		k_spinlock_key_t key;

		(void)esp_sdr_set_freq_offset(khz);
		tune_ret = esp_sdr_set_freq(mhz);
		if (tune_ret != 0) {
			LOG_ERR("%u MHz: %d, no stream", mhz, tune_ret);
			stats.ring_errors++;
			stats.ring_last_status = (uint32_t)-tune_ret;
		}
		key = k_spin_lock(&want_lock);
		/* Back in the host's scale. */
		tuned.plan_hz = tune_ret != 0 ? 0U :
			(uint64_t)((int64_t)mhz * 1000000 + (int64_t)khz * 1000 + corr_hz);
		tuned.mhz = mhz;
		tuned.khz = khz;
		k_spin_unlock(&want_lock, key);
	}
	if (!have_valid || s->rx_gain != have.rx_gain) {
		(void)esp_sdr_rx_set_gain(s->rx_gain == ESDR_GAIN_AUTO ? ESP_SDR_RX_GAIN_AUTO :
									 (int)s->rx_gain);
	}
	if (!have_valid || s->bandwidth_hz != have.bandwidth_hz || s->rate_hz != have.rate_hz) {
		radio_bandwidth(s);
	}
	have = *s;
	have_valid = true;
	return tune_ret;
}

static void stream_run(const struct settings *s, uint32_t seconds)
{
	struct esp_sdr_diag16_cfg cfg = {
		.rate_hz = s->rate_hz,
		.seconds = seconds,
		.wake_nodes = BATCH_NODES,
		/* Only a fixed gain has the one setting of the DC DACs to find. */
		.dc_correct = s->rx_gain != ESDR_GAIN_AUTO,
		/* A host has no way to tell from the blocks: better none than those. */
		.phase_required = true,
		.sink = ring_sink,
	};
	struct esp_sdr_diag16_stats rs = {0};
	int ret;

	batch_first = NULL;
	batch_n = 0U;
	next_index = index_base;
	next_flags = ESDR_BLK_GAP;
	run_first = true;
	run_rate_hz = s->rate_hz;
	stats.ring_runs++;
	ret = esp_sdr_diag16_run(&cfg, &rs);
	/* A run stopped before its first block stays part of the same gap. */
	if (rs.consumed != 0U) {
		run_end_cycles = k_cycle_get_64();
	}
	if (ret != 0) {
		LOG_ERR("ring: %d", ret);
		stats.ring_errors++;
		stats.ring_last_status = (uint32_t)-ret;
	}
	/* Only what kept the stream on the PHY's own DC: an end before the servo finished is none. */
	if (cfg.dc_correct && rs.dc_ret != 0) {
		LOG_WRN("ring: DC servo: %d", rs.dc_ret);
		stats.ring_errors++;
	}
	stats.ring_errors += rs.skips;
	index_base = next_index;
	/* A short last batch goes with the rest: the next start says there is a gap. */
	batch_first = NULL;
	batch_n = 0U;
	if (!IS_ENABLED(CONFIG_APP_HS_SELFTEST)) {
		/*
		 * After a retune, whole transfers keep the host on block boundaries, and what waits
		 * is in memory of its own: it goes out first, however long a host takes. After a
		 * stop it must not reach the next session.
		 */
		int64_t end = k_uptime_get() + DRAIN_MS;

		while (atomic_get(&stream_ended) == 0 && atomic_get(&in_flight) != 0 &&
		       k_uptime_get() < end) {
			k_msleep(1);
		}
		if (atomic_get(&stream_ended) != 0 && atomic_get(&in_flight) != 0) {
			end = k_uptime_get() + CANCEL_MS;
			usb_sdr_cancel_in();
			while (atomic_get(&in_flight) != 0 && k_uptime_get() < end) {
				k_msleep(1);
			}
		}
	}
	LOG_INF("ring stopped: %u S/s, %llu bytes, phase slips %u, skips %u, %d ppm/100",
		s->rate_hz, rs.bytes, rs.phase_tries, rs.skips, rs.ppm_x100);
	if (IS_ENABLED(CONFIG_APP_HS_ZEROCOPY)) {
		LOG_INF("slabs: %u swapped, %u no spare, %u too close, %u stale, %u spares free",
			rs.slab_swaps, rs.slab_nospare, rs.slab_close, rs.slab_stale,
			esp_sdr_diag16_slabs_free());
	}
}

int usb_sdr_control_out(uint8_t req, uint16_t value, const uint8_t *data, size_t len)
{
	k_spinlock_key_t key;
	/* The value in force: restarting the ring for it would only cost samples. */
	bool same = false;
	int ret = 0;

	switch (req) {
	case ESDR_REQ_SET_MODE:
		if (value == (ESDR_MODE_OFF | ESDR_MODE_F_DRAIN)) {
			value = ESDR_MODE_OFF;
		}
		if (value != ESDR_MODE_OFF && value != ESDR_MODE_RX) {
			return -ENOTSUP;
		}
		/* Full speed carries a twentieth of the lowest rate. */
		if (value == ESDR_MODE_RX && !usb_sdr_high_speed()) {
			LOG_WRN("receiving needs a high-speed link");
			return -ENOTSUP;
		}
		key = k_spin_lock(&want_lock);
		same = value == ESDR_MODE_RX && want.mode == ESDR_MODE_RX;
		want.mode = value;
		k_spin_unlock(&want_lock, key);
		if (value == ESDR_MODE_OFF) {
			atomic_set(&stream_ended, 1);
		}
		break;
	case ESDR_REQ_SET_FREQ:
		if (len != 8U || !esdr_freq_ok(sys_get_le64(data))) {
			return -EINVAL;
		}
		key = k_spin_lock(&want_lock);
		same = want.freq_hz == sys_get_le64(data);
		want.freq_hz = sys_get_le64(data);
		k_spin_unlock(&want_lock, key);
		break;
	case ESDR_REQ_SET_SAMPLE_RATE:
		if (len != 4U) {
			return -EINVAL;
		}
		key = k_spin_lock(&want_lock);
		same = want.rate_hz ==
		       esdr_rate_round_list(rates_hz, ARRAY_SIZE(rates_hz), sys_get_le32(data));
		want.rate_hz = esdr_rate_round_list(rates_hz, ARRAY_SIZE(rates_hz), sys_get_le32(data));
		k_spin_unlock(&want_lock, key);
		break;
	case ESDR_REQ_SET_RX_GAIN:
		if (value != ESDR_GAIN_AUTO && value > gain_max) {
			return -EINVAL;
		}
		key = k_spin_lock(&want_lock);
		same = want.rx_gain == value;
		want.rx_gain = value;
		k_spin_unlock(&want_lock, key);
		break;
	case ESDR_REQ_SET_BANDWIDTH:
		if (len != 4U) {
			return -EINVAL;
		}
		key = k_spin_lock(&want_lock);
		same = want.bandwidth_hz == sys_get_le32(data);
		want.bandwidth_hz = sys_get_le32(data);
		k_spin_unlock(&want_lock, key);
		break;
	case ESDR_REQ_SET_FREQ_CORR:
		if (len != 4U || !IN_RANGE((int32_t)sys_get_le32(data), -ESP_SDR_FREQ_CORR_MAX_PPB,
					   ESP_SDR_FREQ_CORR_MAX_PPB)) {
			return -EINVAL;
		}
		key = k_spin_lock(&want_lock);
		same = want.corr_ppb == (int32_t)sys_get_le32(data);
		want.corr_ppb = (int32_t)sys_get_le32(data);
		k_spin_unlock(&want_lock, key);
		break;
	case ESDR_REQ_SET_CAL:
		/* The library's lock: the ring lets go first, also one that is only just starting. */
		stream_replan();
		ret = esdr_cal_set(value, data, len);
		break;
	case ESDR_REQ_SET_FORMAT:
		/* The ring's bytes are what there is. */
		return value == 8U ? 0 : -ENOTSUP;
	case ESDR_REQ_SET_DIGITAL_GAIN:
		/* Nothing touches the samples. */
		return value == ESDR_DGAIN_DEFAULT || value == 0U ? 0 : -ENOTSUP;
	case ESDR_REQ_SET_TX_GAIN:
		return value == 0U ? 0 : -ENOTSUP;
	case ESDR_REQ_SET_OPTIONS:
		return 0;
	case ESDR_REQ_RESET:
		key = k_spin_lock(&want_lock);
		want.mode = ESDR_MODE_OFF;
		want.rx_gain = ESDR_GAIN_AUTO;
		want.freq_hz = 2412000000ULL;
		want.rate_hz = RATE_START;
		want.bandwidth_hz = 0U;
		want.corr_ppb = 0;
		k_spin_unlock(&want_lock, key);
		atomic_set(&stream_ended, 1);
		break;
	default:
		return -ENOTSUP;
	}
	if (!same) {
		stream_replan();
	}
	return ret;
}

int usb_sdr_control_in(uint8_t req, uint16_t value, uint8_t *buf, size_t max)
{
	ARG_UNUSED(value);
	switch (req) {
	case ESDR_REQ_GET_INFO: {
		struct esdr_info info;

		esdr_info_init(&info, RATE_MAX_HZ, RATE_COUNT, gain_max, 0U,
			       "osmosdr_hs " APP_VERSION_STRING);
		info.freq_min_hz = FREQ_MIN_HZ;
		info.freq_max_hz = FREQ_MAX_HZ;
		info.caps = ESDR_CAP_RATE_LIST;
		memcpy(buf, &info, MIN(max, sizeof(info)));
		return (int)MIN(max, sizeof(info));
	}
	case ESDR_REQ_GET_RATES: {
		size_t n = MIN(max / 4U, ARRAY_SIZE(rates_hz));

		for (size_t k = 0; k < n; k++) {
			sys_put_le32(rates_hz[k], buf + 4U * k);
		}
		return (int)(4U * n);
	}
	case ESDR_REQ_GET_STATE: {
		k_spinlock_key_t key = k_spin_lock(&want_lock);
		struct esdr_state st = {
			.mode = want.mode,
			.bits = 8U,
			.rx_gain = want.rx_gain,
			.freq_hz = want.freq_hz,
			.center_hz = tuned.plan_hz != 0U ?
				tuned.plan_hz + (int64_t)esp_sdr_get_lo_residual_hz() : 0U,
			.sample_rate_hz = want.rate_hz,
			.bandwidth_hz = want.bandwidth_hz,
			.freq_corr_ppb = want.corr_ppb,
			.lo_mhz = tuned.mhz,
			.lo_offset_khz = tuned.khz,
		};

		k_spin_unlock(&want_lock, key);
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
	case ESDR_REQ_GET_CAL:
		return esdr_cal_get(buf, max);
	default:
		return -ENOTSUP;
	}
}

void usb_sdr_in_done(uint8_t *data, size_t len, int err)
{
	/* Whatever the outcome: the host is done with the memory, and the ring may have it. */
	if (IS_ENABLED(CONFIG_APP_HS_ZEROCOPY)) {
		(void)esp_sdr_diag16_slab_release(data);
	}
	if (err == 0) {
		stats.usb_in_bytes += len;
		stats.rx_blocks += (uint32_t)(len / ESDR_BLOCK_BYTES);
		stats.rx_samples += (uint64_t)(len / ESDR_BLOCK_BYTES) * ESP_SDR_DIAG16_PAYLOAD / 2U;
	}
	atomic_dec(&in_flight);
}

void usb_sdr_out_done(uint8_t *data, size_t len, int err)
{
	/* Nothing transmits: a host's samples go nowhere. */
	ARG_UNUSED(len);
	if (err != -ECONNABORTED && usb_sdr_enabled() && !HS_QUIT()) {
		(void)usb_sdr_submit_out(data, OUT_BYTES);
	}
}

/* The host went: the stream and the pads stop, and it starts over if a new one asks. */
static void stream_off(void)
{
	k_spinlock_key_t key = k_spin_lock(&want_lock);

	want.mode = ESDR_MODE_OFF;
	k_spin_unlock(&want_lock, key);
	atomic_set(&stream_ended, 1);
	stream_replan();
}

static void link_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	if (HS_QUIT()) {
		return;
	}
	if (usb_sdr_enabled()) {
		/* Read requests pending on bulk OUT, as a host's tool expects of the endpoint. */
		for (size_t k = 0; k < ARRAY_SIZE(out_buf); k++) {
			(void)usb_sdr_submit_out(out_buf[k], OUT_BYTES);
		}
	} else {
		stream_off();
	}
}
static K_WORK_DEFINE(link_work, link_work_fn);

void usb_sdr_link(bool up)
{
	ARG_UNUSED(up);
	k_work_submit(&link_work);
}

/*
 * A suspend is a state of the link, not the host's wish: the stream thread holds the ring off while it
 * lasts, and a host that resumes with no bus reset gets its stream back, from index 0.
 */
static void suspend_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	atomic_set(&stream_ended, 1);
	stream_replan();
}
static K_WORK_DEFINE(suspend_work, suspend_work_fn);

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
	/* This port's VBUS is the board's own: a pulled cable or a sleeping host is only a suspend. */
	if (msg->type == USBD_MSG_SUSPEND) {
		k_work_submit(&suspend_work);
	}
}

#if defined(CONFIG_APP_HS_SELFTEST) || defined(CONFIG_APP_HS_LOAD_LOG)
/* What a thread below everything gets: the CPU the stream leaves. */
#define SPIN_STACK 1024
static K_THREAD_STACK_DEFINE(spin_stack, SPIN_STACK);
static struct k_thread spin_thread;
#if defined(MULTIAPP_NAME)
/* The extension's unload ends the thread (hs_release()); it may never have started. */
static bool spin_started;
#endif
static volatile uint32_t spins;

static void spin_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		spins++;
	}
}

static void spin_start(void)
{
	k_thread_create(&spin_thread, spin_stack, K_THREAD_STACK_SIZEOF(spin_stack), spin_fn, NULL,
			NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, K_NO_WAIT);
#if defined(MULTIAPP_NAME)
	spin_started = true;
#endif
}

static uint32_t spins_in(uint32_t ms)
{
	uint32_t before = spins;

	k_msleep(ms);
	return spins - before;
}
#endif

#if defined(CONFIG_APP_HS_LOAD_LOG)
#define LOAD_LOG_S 10U
static uint32_t load_idle, load_last;

static void load_log_fn(struct k_work *work)
{
	uint32_t now = spins;

	LOG_INF("free CPU %u %%, %llu samples sent, %u overflows",
		(uint32_t)((uint64_t)(now - load_last) * 100U / (load_idle * LOAD_LOG_S)),
		stats.rx_samples, stats.rx_overflows);
	load_last = now;
	k_work_schedule(k_work_delayable_from_work(work), K_SECONDS(LOAD_LOG_S));
}
static K_WORK_DELAYABLE_DEFINE(load_log, load_log_fn);

static void load_log_start(void)
{
	spin_start();
	load_idle = spins_in(2000) / 2U;
	load_last = spins;
	LOG_INF("free CPU idle: %u spins/s", load_idle);
	k_work_schedule(&load_log, K_SECONDS(LOAD_LOG_S));
}
#endif

#if defined(CONFIG_APP_HS_SELFTEST)
static void selftest(void)
{
	static const uint32_t rates[] = {5000000U, 10000000U, 20000000U, 40000000U};
	struct settings s = want;
	uint32_t base;

	spin_start();
	base = spins_in(2000);
	LOG_INF("free CPU idle: %u spins/s", base / 2U);
	(void)radio_apply(&s);
	for (size_t k = 0; k < ARRAY_SIZE(rates); k++) {
		s.rate_hz = rates[k];
		stats.rx_blocks = 0U;
		uint32_t before = spins;
		int64_t t0 = k_uptime_get();

		stream_run(&s, 5U);
		uint32_t done = spins - before;
		int64_t ms = k_uptime_get() - t0;

		/* Spins per second during the run against those of an idle CPU. */
		LOG_INF("%u S/s: free CPU %u %%, %u blocks", rates[k],
			(uint32_t)((uint64_t)done * 1000U * 200U / ((uint64_t)ms * base)),
			stats.rx_blocks);
	}
	LOG_INF("selftest done");
}
#endif

#if defined(MULTIAPP_NAME)
/*
 * Built as an extension of apps/multiapp (include/multiapp/multiapp.h): the host calls this before
 * it unloads the app and then joins main()'s thread. main() is the stream thread: a ring run ends
 * at its next block, a wait for a request at once, and the loop below returns. Nothing else is
 * started here: the ring's interrupt is the library's, taken and freed per run.
 */
int multiapp_app_stop(void)
{
	atomic_set(&quit, 1);
	stream_replan();
	return 0;
}

/*
 * The last of the app, run by the host's unload once nothing calls into the app any more
 * (apps.c, app_unload()): what the app left queued with the system work queue and the USB class
 * must not outlive its memory. The work items first (one may be submitting), then every transfer
 * the class holds, IN batches and OUT reads alike; the host's class stays, as the PC has it open.
 */
static void __attribute__((destructor)) hs_release(void)
{
	struct k_work_sync sync;

	(void)k_work_cancel_sync(&link_work, &sync);
	(void)k_work_cancel_sync(&suspend_work, &sync);
#if defined(CONFIG_APP_HS_LOAD_LOG)
	(void)k_work_cancel_delayable_sync(&load_log, &sync);
#endif
#if defined(CONFIG_APP_HS_SELFTEST) || defined(CONFIG_APP_HS_LOAD_LOG)
	if (spin_started) {
		k_thread_abort(&spin_thread);
		spin_started = false;
	}
#endif
	usb_sdr_cancel();
}
#endif /* MULTIAPP_NAME */

int main(void)
{
	struct usbd_context *ctx;
	struct settings s;
	int ret;

	ret = esp_sdr_init();
	if (ret != 0) {
		LOG_ERR("radio: %d", ret);
		return 0;
	}
	gain_max = (uint16_t)esp_sdr_rx_gain_max();
	LOG_INF("radio up, gain 0 to %u", gain_max);

#if defined(CONFIG_APP_HS_SELFTEST)
	selftest();
	return 0;
#endif
#if defined(CONFIG_APP_HS_LOAD_LOG)
	load_log_start();
#endif
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
#if defined(MULTIAPP_NAME)
	/* A host that kept the interface enabled while the app reloaded sends no link event. */
	if (usb_sdr_enabled()) {
		k_work_submit(&link_work);
	}
#endif

	for (;;) {
		k_spinlock_key_t key;
		atomic_val_t gen;

		/* Before the snapshot: a request after it shows in both. */
		k_sem_reset(&kick);
		gen = atomic_get(&want_gen);
		/*
		 * After the generation is read: the multiapp stop hook sets the flag before it moves
		 * the generation, so a stop that this read did not see is seen here, and one after
		 * this read changes the generation, which the run's sink checks.
		 */
		if (HS_QUIT()) {
			break;
		}
		key = k_spin_lock(&want_lock);
		s = want;
		k_spin_unlock(&want_lock, key);
		if (atomic_clear(&stream_ended) != 0) {
			/* Also for a stop with no run left to end: a stalled host's last transfers. */
			usb_sdr_cancel_in();
			index_base = 0U;
			run_end_cycles = 0U;
		}
		if (s.mode != ESDR_MODE_RX || !usb_sdr_enabled() || usbd_is_suspended(ctx)) {
			(void)k_sem_take(&kick, K_MSEC(POLL_MS));
			continue;
		}
		ret = radio_apply(&s);
		/* A request since the snapshot: no run on old settings. */
		if (atomic_get(&want_gen) != gen) {
			continue;
		}
		if (ret != 0) {
			(void)k_sem_take(&kick, K_MSEC(POLL_MS));
			continue;
		}
		run_gen = gen;
		stream_run(&s, 0U);
	}
#if defined(MULTIAPP_NAME)
	/*
	 * Batches may still be queued: their cancellations would reach the next app's handler (and
	 * its count) if they came after the unload. Cancel and wait for them while this app is loaded.
	 */
	usb_sdr_cancel_in();
	for (int64_t end = k_uptime_get() + CANCEL_MS;
	     atomic_get(&in_flight) != 0 && k_uptime_get() < end;) {
		k_msleep(1);
	}
	if (atomic_get(&in_flight) != 0) {
		LOG_WRN("%ld IN transfers did not end", (long)atomic_get(&in_flight));
	}
#endif
	return 0;
}
