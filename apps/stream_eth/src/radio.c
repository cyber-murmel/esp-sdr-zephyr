/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The radio and the ring consumer, after apps/osmosdr_hs: the diag16 ring at the requested rate,
 * its nodes copied into the staging cells (vrt_cells.h) for the sender. The settings come from
 * the commands (ctl.c); a change replans the stream, which restarts the ring.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_diag16.h>
#include <esp_sdr/esp_sdr_rx.h>

#include "stream_eth.h"
#include "vrt_pkt.h"

LOG_MODULE_DECLARE(stream_eth, LOG_LEVEL_INF);

BUILD_ASSERT(ESP_SDR_DIAG16_PAYLOAD % 4U == 0U, "whole words in a node");

/* 160 MHz over an even whole number: what esp_sdr_diag16_run() takes. */
static const uint32_t rates_hz[] = {40000000U, 20000000U, 16000000U, 10000000U, 8000000U,
				    5000000U,  4000000U,  3200000U,  2500000U,  2000000U,
				    1600000U,  1250000U,  1000000U,  800000U,    640000U};
#define FREQ_MIN_HZ 2200000000ULL
#define FREQ_MAX_HZ 2900000000ULL
#define BW_MIN_MHZ  13U
#define POLL_MS     100
#define WAKE_NODES  4U

static struct k_spinlock lock;
static struct {
	uint64_t freq_hz;
	int gain;
	uint32_t rate_hz;
	bool on;
	uint8_t epoch;
} want = {
	.freq_hz = 2412000000ULL,
	.gain = ESP_SDR_RX_GAIN_AUTO,
	.rate_hz = CONFIG_APP_VRT_RATE_HZ,
};
static struct {
	uint64_t freq_hz;
	int gain;
	uint32_t rate_hz;
	bool valid;
} have;
/* what the radio is tuned to, and for which wanted frequency */
static struct {
	uint64_t plan_hz;
	uint64_t want_hz;
} tuned;
static int tune_ret;
static int last_tune_err, last_ring_err;
static uint32_t backoff_ms = POLL_MS;
static int gain_max;
static K_SEM_DEFINE(kick, 0, 1);
static atomic_t want_gen, change_count;
static atomic_val_t run_gen;

/* The consumer's. */
static uint64_t index_base;
static bool run_first;
static uint32_t run_rate, run_gain_idx;
static uint8_t run_epoch;
static uint64_t run_freq;
/* Bumped each time the stream is asked for after it was off; a run that follows the same epoch as
 * the last one is a continuation (retune) and says so with sample loss.
 */
static atomic_t sub_epoch;
static atomic_val_t last_run_epoch = -1;
static uint32_t nodes, ring_skips;
struct radio_prof prof;

#define CELL_STRIDE 1472U
BUILD_ASSERT(IS_POWER_OF_TWO(CONFIG_APP_VRT_CELLS), "the cell counters wrap at 2^32");
static uint8_t cell_mem[CONFIG_APP_VRT_CELLS][CELL_STRIDE] __aligned(64);
static struct vrt_cell_meta cell_meta[CONFIG_APP_VRT_CELLS];
struct vrt_cells cells;

static uint64_t boot_index(uint32_t rate)
{
	uint64_t c = k_cycle_get_64();
	uint32_t hz = sys_clock_hw_cycles_per_sec();

	/* seconds since boot, exactly, from the cycle counter, then the rate takes over */
	return (c / hz) * rate + (c % hz) * rate / hz;
}

bool radio_rate_ok(uint32_t hz)
{
	for (size_t k = 0; k < ARRAY_SIZE(rates_hz); k++) {
		if (rates_hz[k] == hz) {
			return true;
		}
	}
	return false;
}

uint8_t radio_epoch(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	uint8_t e = want.epoch;

	k_spin_unlock(&lock, key);
	return e;
}

bool radio_freq_ok(uint64_t hz)
{
	return hz >= FREQ_MIN_HZ && hz <= FREQ_MAX_HZ;
}

bool radio_agc(void)
{
	return want.gain == ESP_SDR_RX_GAIN_AUTO;
}

int radio_gain_max(void)
{
	return gain_max;
}

static void replan(void)
{
	atomic_inc(&want_gen);
	atomic_inc(&change_count);
	esp_sdr_diag16_stop();
	k_sem_give(&kick);
}

void radio_replan(void)
{
	replan();
}

int radio_set_freq(uint64_t hz)
{
	k_spinlock_key_t key;
	bool same;

	if (hz < FREQ_MIN_HZ || hz > FREQ_MAX_HZ) {
		return -EINVAL;
	}
	key = k_spin_lock(&lock);
	same = !vrt_set_u64(&want.freq_hz, hz, &want.epoch);
	k_spin_unlock(&lock, key);
	if (!same) {
		replan();
	}
	return 0;
}

int radio_set_gain(int gain)
{
	k_spinlock_key_t key;
	bool same;

	if (gain != ESP_SDR_RX_GAIN_AUTO && (gain < 0 || gain > gain_max)) {
		return -EINVAL;
	}
	key = k_spin_lock(&lock);
	same = !vrt_set_int(&want.gain, gain, &want.epoch);
	k_spin_unlock(&lock, key);
	if (!same) {
		replan();
	}
	return 0;
}

int radio_set_rate(uint32_t hz)
{
	k_spinlock_key_t key;
	bool same;

	if (!radio_rate_ok(hz)) {
		return -EINVAL;
	}
	key = k_spin_lock(&lock);
	same = !vrt_set_u32(&want.rate_hz, hz, &want.epoch);
	k_spin_unlock(&lock, key);
	if (!same) {
		replan();
	}
	return 0;
}

void radio_enable(bool on)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool same = want.on == on;

	want.on = on;
	k_spin_unlock(&lock, key);
	if (!same) {
		if (on) {
			atomic_inc(&sub_epoch);
		}
		replan();
	}
}

void radio_get(struct radio_state *s)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	s->freq_hz = want.freq_hz;
	s->gain = want.gain;
	s->rate_hz = want.rate_hz;
	s->on = want.on;
	s->epoch = want.epoch;
	k_spin_unlock(&lock, key);
}

/* The centre, 0 unless the radio is tuned for want_hz (a retune may still be under way). */
uint64_t radio_center_hz(uint64_t want_hz)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	uint64_t hz = tuned.want_hz == want_hz ? tuned.plan_hz : 0U;

	k_spin_unlock(&lock, key);
	return hz != 0U ? (uint64_t)((int64_t)hz + esp_sdr_get_lo_residual_hz()) : 0U;
}

uint32_t radio_ring_skips(void)
{
	return ring_skips;
}

void radio_errors(int *tune, int *ring)
{
	*tune = last_tune_err;
	*ring = last_ring_err;
}

void radio_prof_take(struct radio_prof *p)
{
	*p = prof;
	memset(&prof, 0, sizeof(prof));
}

uint32_t radio_nodes(void)
{
	return nodes;
}

uint32_t radio_change_count(void)
{
	return (uint32_t)atomic_get(&change_count);
}

static void radio_bandwidth(uint32_t rate_hz)
{
	uint32_t lo, hi;

	esp_sdr_rx_bandwidth_range(&lo, &hi);
	(void)esp_sdr_rx_set_bandwidth(CLAMP(MAX(rate_hz / 1000000U, BW_MIN_MHZ), lo, hi));
}

static int radio_apply(const struct radio_state *s)
{
	if (!have.valid || s->freq_hz != have.freq_hz) {
		uint32_t mhz = (uint32_t)((s->freq_hz + 500000U) / 1000000U);
		int32_t rest = (int32_t)((int64_t)s->freq_hz - (int64_t)mhz * 1000000);
		int32_t khz = (rest + (rest < 0 ? -500 : 500)) / 1000;
		k_spinlock_key_t key;

		(void)esp_sdr_set_freq_offset(khz);
		tune_ret = esp_sdr_set_freq(mhz);
		if (tune_ret != 0) {
			LOG_ERR("%u MHz: %d, no stream", mhz, tune_ret);
		}
		key = k_spin_lock(&lock);
		tuned.plan_hz = tune_ret != 0 ? 0U :
				(uint64_t)((int64_t)mhz * 1000000 + (int64_t)khz * 1000);
		tuned.want_hz = s->freq_hz;
		k_spin_unlock(&lock, key);
	}
	if (!have.valid || s->gain != have.gain) {
		(void)esp_sdr_rx_set_gain(s->gain);
	}
	if (!have.valid || s->rate_hz != have.rate_hz) {
		radio_bandwidth(s->rate_hz);
	}
	have.freq_hz = s->freq_hz;
	have.gain = s->gain;
	have.rate_hz = s->rate_hz;
	/* a failed tune is not recorded as done: the next try tunes again */
	have.valid = tune_ret == 0;
	last_tune_err = tune_ret;
	return tune_ret;
}

static void ring_sink(void *user, uint8_t *node, uint64_t index)
{
	ARG_UNUSED(user);
	/* esp_sdr_diag16_run() clears a stop requested just before it starts: this ends that run. */
	if (atomic_get(&want_gen) != run_gen) {
		esp_sdr_diag16_stop();
	}
	if (run_first) {
		run_first = false;
		/* the time of the first sample, now that the phase search is over */
		index_base = boot_index(run_rate) - index / 2U;
		vrt_cells_start(&cells, txpath_payload_bytes(), run_rate, run_freq, (int)run_gain_idx, run_epoch,
				txpath_v6() && !txpath_zero_csum(), atomic_get(&sub_epoch) == last_run_epoch);
		last_run_epoch = atomic_get(&sub_epoch);
	}
	uint32_t t0 = k_cycle_get_32();

	nodes++;
	/* index counts bytes, two to a sample; the time axis is the sample index */
	if (vrt_cells_push(&cells, node + ESP_SDR_DIAG16_HOLE, ESP_SDR_DIAG16_PAYLOAD,
			   index_base + index / 2U) != 0) {
		return;
	}
	/* copy first, ask after: a consumer held off long enough has copied what the DMA wrote */
	if (!esp_sdr_diag16_node_valid(index)) {
		vrt_cells_abort(&cells);
		return;
	}
	vrt_cells_commit(&cells);
	{
		uint32_t d = k_cycle_get_32() - t0;

		prof.node_total += d;
		prof.node_n++;
		prof.node_max = MAX(prof.node_max, d);
	}
	txpath_kick();
}

static int stream_run(const struct radio_state *s)
{
	struct esp_sdr_diag16_cfg cfg = {
		.rate_hz = s->rate_hz,
		.wake_nodes = WAKE_NODES,
		/* Only a fixed gain has the one setting of the DC DACs to find. */
		.dc_correct = s->gain != ESP_SDR_RX_GAIN_AUTO,
		.phase_required = true,
		.sink = ring_sink,
	};
	struct esp_sdr_diag16_stats rs = {0};
	int ret;

	run_rate = s->rate_hz;
	run_gain_idx = (uint32_t)s->gain;
	/* the epoch the settings of this run were snapshot with (radio_get(), one lock) */
	run_epoch = s->epoch;
	run_freq = radio_center_hz(s->freq_hz);
	if (run_freq == 0U) {
		run_freq = s->freq_hz;
	}
	index_base = boot_index(s->rate_hz);
	run_first = true;
	ret = esp_sdr_diag16_run(&cfg, &rs);
	ring_skips += rs.skips;
	last_ring_err = ret;
	if (ret != 0) {
		LOG_ERR("ring: %d", ret);
	}
	if (cfg.dc_correct && rs.dc_ret != 0) {
		LOG_WRN("ring: DC servo: %d", rs.dc_ret);
	}
	LOG_INF("ring stopped: %u S/s, %llu bytes, phase slips %u, skips %u, %d ppm/100",
		s->rate_hz, rs.bytes, rs.phase_tries, rs.skips, rs.ppm_x100);
	return ret;
}

int radio_init(void)
{
	int ret = esp_sdr_init();

	if (ret != 0) {
		LOG_ERR("radio: %d", ret);
		return ret;
	}
	gain_max = esp_sdr_rx_gain_max();
	vrt_cells_init(&cells, &cell_mem[0][0], cell_meta, CONFIG_APP_VRT_CELLS, CELL_STRIDE);
	LOG_INF("radio up, gain 0 to %d", gain_max);
	return 0;
}

void radio_run(void)
{
	for (;;) {
		struct radio_state s;
		atomic_val_t gen;

		k_sem_reset(&kick);
		gen = atomic_get(&want_gen);
		radio_get(&s);
		if (!s.on) {
			(void)k_sem_take(&kick, K_MSEC(POLL_MS));
			continue;
		}
		if (radio_apply(&s) != 0) {
			/* it cannot tune: try again, ever less often, until a request changes it */
			(void)k_sem_take(&kick, K_MSEC(backoff_ms));
			backoff_ms = MIN(backoff_ms * 2U, 2000U);
			continue;
		}
		backoff_ms = POLL_MS;
		if (atomic_get(&want_gen) != gen) {
			continue;
		}
		run_gen = gen;
		if (stream_run(&s) != 0) {
			/* no clean phase, no data: not a busy loop of attempts */
			(void)k_sem_take(&kick, K_MSEC(50));
		}
	}
}
