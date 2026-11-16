/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Gapless receive on the ESP32-S31 through the modem diagnostic bus (esp_sdr_diag16.h), after
 * upstream's targets/esp32s31/streaming/receiver.c. PARLIO and GDMA are driven through the HAL's
 * low-level layer, as on the C5: Zephyr has no PARLIO driver and its GDMA driver builds only linear
 * chains. Unlike the C5's, the nodes are ring elements with a hole for a header.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/drivers/interrupt_controller/intc_esp32.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/util.h>

#include <esp_attr.h>
#include <esp_cpu.h>
#include <esp_private/esp_clk.h>
#include <esp_private/periph_ctrl.h>
#include <esp_rom_gpio.h>
#include <hal/axi_dma_ll.h>
#include <hal/dma_types.h>
#include <hal/gdma_channel.h>
#include <hal/gdma_ll.h>
#include <hal/gpio_ll.h>
#include <hal/hal_utils.h>
#include <hal/parlio_ll.h>
#include <soc/gpio_sig_map.h>
#include <soc/interrupts.h>

#include <esp_sdr/esp_sdr_place.h>
#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_diag16.h>

#include "esp_sdr_priv.h"

BUILD_ASSERT(!IS_ENABLED(CONFIG_DISPLAY_ESP32_LCD_RGB),
	     "every run resets the AXI GDMA group and leaves its pads undriven: the panel is gone "
	     "until a reboot");

LOG_MODULE_DECLARE(esp_sdr, CONFIG_ESP_SDR_LOG_LEVEL);

#define PLL_HZ       160000000U
#define MAX_RATE_HZ  40000000U
#define STALL_MS     500
#define NODES        (CONFIG_ESP_SDR_DIAG16_RING_KB * 1024U / ESP_SDR_DIAG16_NODE)

/* One RX channel of the AXI group, fixed: Zephyr's GDMA driver has no use for it. */
#define DMA_GROUP    GDMA_LL_AXI_GROUP_START_ID
#define DMA_CH       0U
#define DMA_BURST    16U

/* A frame's length field holds 65535 bytes, less than the ring: EOF lands on a node's end. */
#define EOF_NODES    16U
#define EOF_BYTES    (EOF_NODES * ESP_SDR_DIAG16_PAYLOAD)

BUILD_ASSERT(NODES % EOF_NODES == 0U, "the frames fill the ring");
BUILD_ASSERT(EOF_BYTES * 8U <= PARL_IO_RX_BITLEN_V, "a frame fits the length field");
BUILD_ASSERT(ESP_SDR_DIAG16_HOLE % DMA_BURST == 0U, "the samples are on a burst boundary");
BUILD_ASSERT(ESP_SDR_DIAG16_PAYLOAD % DMA_BURST == 0U, "a node is whole bursts");

#if DT_NODE_HAS_PROP(DT_PATH(zephyr_user), esp_sdr_diag_pads)
static const int8_t default_pins[ESP_SDR_DIAG16_PINS] =
	DT_PROP(DT_PATH(zephyr_user), esp_sdr_diag_pads);
#else
/*
 * All on J2 of the ESP32-S31-Function-CoreBoard-1. Upstream's 33 and 34 are USB-Serial-JTAG's: 47
 * and 48 keep it. Not 35 or 45: lines on them come out unbalanced and mixed with others (measured).
 */
static const int8_t default_pins[ESP_SDR_DIAG16_PINS] = {
	20, 21, 22, 23, 24, 25, 47, 48, 36, 37, 38, 39, 40, 42, 43, 44,
};
#endif

extern uint8_t __esp_sdr_ring_start[];
static dma_descriptor_align8_t desc[NODES];

static struct {
	/*
	 * Written by the interrupt. The byte count is two words: prod_seq is odd while they change,
	 * so that a reader on the other core never pairs a new high word with an old low one.
	 */
	volatile uint32_t prod_seq, prod_lo, prod_hi;
	volatile uint32_t head;
	volatile uint64_t tick_first, tick_last;
	volatile uint64_t prod_first, prod_last;
	volatile uint32_t isrs;
	volatile bool overrun;
	uint32_t wake_bytes;
	/* Where the sink's index starts. */
	uint64_t first;
	atomic_t stop;
	bool running;
} st;

/* Not set up by a run: a stop may come before the first one. */
static K_SEM_DEFINE(ring_sem, 0, 1);

static inline uint64_t prod_get(void)
{
	uint32_t seq, hi, lo;

	do {
		seq = st.prod_seq;
		barrier_dmem_fence_full();
		hi = st.prod_hi;
		lo = st.prod_lo;
		barrier_dmem_fence_full();
	} while ((seq & 1U) != 0U || seq != st.prod_seq);
	return ((uint64_t)hi << 32) | lo;
}

static inline void prod_set(uint64_t p)
{
	st.prod_seq = st.prod_seq + 1U;
	barrier_dmem_fence_full();
	st.prod_hi = (uint32_t)(p >> 32);
	st.prod_lo = (uint32_t)p;
	barrier_dmem_fence_full();
	st.prod_seq = st.prod_seq + 1U;
}

uint64_t esp_sdr_diag16_produced(void)
{
	uint64_t p = prod_get();

	return p > st.first ? p - st.first : 0U;
}

bool esp_sdr_diag16_node_valid(uint64_t index)
{
	/* A node's margin, as for skipping: the count moves with the interrupt, after the DMA. */
	return esp_sdr_diag16_produced() - index < (NODES - 1U) * ESP_SDR_DIAG16_PAYLOAD;
}

ESP_SDR_CODE(PACED_CHUNK, ring_isr)
static void ring_isr(void *arg)
{
	axi_dma_dev_t *dma = AXI_DMA_LL_GET_HW(0);
	uint32_t status = axi_dma_ll_rx_get_interrupt_status(dma, DMA_CH, false);
	uint64_t p = prod_get(), before = p;
	uint32_t head = st.head, walked = 0U;

	ARG_UNUSED(arg);
	axi_dma_ll_rx_clear_interrupt_status(dma, DMA_CH, status);
	st.isrs++;
	/* The DMA writes the received length back into each descriptor: interrupts merging lose nothing. */
	while (walked < NODES) {
		volatile dma_descriptor_align8_t *d = &desc[head];
		uint32_t len = d->dw0.length;

		if (len == 0U) {
			break;
		}
		d->dw0.length = 0;
		d->dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
		p += len;
		head = (head + 1U) % NODES;
		walked++;
	}
	st.head = head;
	if (walked == NODES) {
		st.overrun = true;
	}
	if (walked != 0U) {
		uint64_t now = k_cycle_get_64();

		if (st.prod_first == 0U) {
			st.prod_first = p;
			st.tick_first = now;
		}
		st.prod_last = p;
		st.tick_last = now;
		prod_set(p);
		if ((p / st.wake_bytes) != (before / st.wake_bytes) || st.overrun) {
			k_sem_give(&ring_sem);
		}
	}
}

/*
 * Is the bus sampled on its edge? Some of Q's lines then take the next sample's bit: when Q crosses
 * zero with the receiver off, about 85 % of the time (617 to 1645 of a look, at any gain; on air 31 to
 * 82 %, by gain), it comes out around +-34 or +-42. So what counts is Q's crossings: this many with
 * next to no large value is no bad phase, the odds against being some 1e-9 with the receiver off. That
 * gives the ADC's noise alone, under a step rms, whatever is on the air, and any large value there is
 * this. Its DC depends on the LO, though, and can keep Q off zero. Then the look has to be with the
 * receiver on, at a gain that crosses, in a moment with nothing on the air: a frame would hide the
 * wrong values.
 */
#define PHASE_LARGE       32
#define PHASE_SMALL       16
#define PHASE_MAX_BAD     2U
#define PHASE_MIN_CROSS   16U
#define PHASE_NODES       2U
#define PHASE_SKIP_NODES  1U
#define PHASE_SETTLE_US   2000U
/* Some 500000 pairs: tens of crossings at the DC this often sits at, or none at all. */
#define PHASE_MUTED_LOOKS 128U
/* Beyond 3 steps rms (9 in squares) there is more than noise on the air. */
#define PHASE_NOISE_MAX   9U
/* Wrong values are some of the samples; most of them large is a signal. */
#define PHASE_MAX_SHARE   4U
#define PHASE_ON_LOOKS    256U
/* A long frame takes 5 ms: a gain gets some 25 ms before it counts as too much for the band. */
#define PHASE_BUSY_MAX    24U
#define PHASE_BUSY_MS     1
#define PHASE_RX_BACK_MS  2
#define WAIT_POLL_US      20U
#define WAIT_MAX_US       100000U

struct phase_count {
	uint32_t large;
	uint32_t small_sq, small_n;
	uint32_t cross_q;
};

static void node_count(const uint8_t *node, struct phase_count *c)
{
	const int8_t *s = (const int8_t *)(node + ESP_SDR_DIAG16_HOLE);

	for (size_t j = 0U; j < ESP_SDR_DIAG16_PAYLOAD; j += 2U) {
		for (size_t k = j; k < j + 2U; k++) {
			if (abs(s[k]) < PHASE_SMALL) {
				c->small_sq += (uint32_t)(s[k] * s[k]);
				c->small_n++;
			}
		}
		if (abs(s[j]) >= PHASE_LARGE || abs(s[j + 1U]) >= PHASE_LARGE) {
			c->large++;
		}
		if (j != 0U) {
			c->cross_q += (uint32_t)((s[j + 1U] < 0) != (s[j - 1U] < 0));
		}
	}
}

static bool wait_produced(uint64_t bytes)
{
	for (uint32_t us = 0; us < WAIT_MAX_US; us += WAIT_POLL_US) {
		if (prod_get() >= bytes) {
			return true;
		}
		k_busy_wait(WAIT_POLL_US);
	}
	return false;
}

static inline uint8_t *node_at(uint64_t byte)
{
	return __esp_sdr_ring_start +
	       (byte / ESP_SDR_DIAG16_PAYLOAD % NODES) * ESP_SDR_DIAG16_NODE;
}

/*
 * Restarting the divider lands on the same phase nearly every time. A period of the divider plus
 * one comes 6.25 ns late, half a bus period: an odd number of them moves to the other phase.
 */
static void parlio_clock_slip(unsigned int div)
{
	hal_utils_clk_div_t longer = {.integer = div + 1U};
	hal_utils_clk_div_t normal = {.integer = div};
	unsigned int key = irq_lock();

	PERIPH_RCC_ATOMIC() {
		parlio_ll_rx_set_clock_div(&PARL_IO, &longer);
		parlio_ll_rx_set_clock_div(&PARL_IO, &normal);
	}
	irq_unlock(key);
}

/* The next nodes the DMA finishes, counted. False if it stopped. */
static bool phase_look(struct phase_count *c)
{
	uint64_t from = (prod_get() / ESP_SDR_DIAG16_PAYLOAD + PHASE_SKIP_NODES) *
			ESP_SDR_DIAG16_PAYLOAD;

	*c = (struct phase_count){0};
	if (!wait_produced(from + PHASE_NODES * ESP_SDR_DIAG16_PAYLOAD)) {
		return false;
	}
	for (unsigned int n = 0; n < PHASE_NODES; n++) {
		node_count(node_at(from + n * ESP_SDR_DIAG16_PAYLOAD), c);
	}
	return true;
}

/*
 * Receiver off. In this order: with the receiver running, the bit alone freezes the bus on its last
 * value. Returns the slips, or a negative error; *done when the crossings were enough.
 */
static int phase_look_muted(unsigned int div, bool *done)
{
	uint32_t large = 0U, cross = 0U;
	int slips = 0;

	esp_sdr_s31_rx_mute(true);
	/* The gain does nothing to the ADC's noise; this restarts the front end without the antenna. */
	esp_sdr_s31_gain(0);
	k_busy_wait(PHASE_SETTLE_US);
	for (unsigned int looks = 0; looks < PHASE_MUTED_LOOKS; looks++) {
		struct phase_count c;

		if (!phase_look(&c)) {
			slips = -ETIMEDOUT;
			break;
		}
		large += c.large;
		cross += c.cross_q;
		if (large > PHASE_MAX_BAD) {
			parlio_clock_slip(div);
			slips++;
			large = 0U;
			cross = 0U;
		} else if (cross >= PHASE_MIN_CROSS) {
			*done = true;
			break;
		}
	}
	esp_sdr_s31_rx_mute(false);
	return slips;
}

/* Less gain for a lasting signal, then more or less for a DC that keeps Q off zero. */
static const uint8_t phase_gains[] = {54U, 48U, 60U, 66U, 30U, 0U};

/* Receiver on. Returns the slips, or a negative error; *done as above. */
static int phase_look_on_air(unsigned int div, bool *done)
{
	unsigned int gain = 0U, busy = 0U;
	int slips = 0;

	esp_sdr_s31_gain(phase_gains[gain]);
	k_busy_wait(PHASE_SETTLE_US);
	for (unsigned int looks = 0; looks < PHASE_ON_LOOKS; looks++) {
		struct phase_count c;
		bool next_gain = false;

		if (!phase_look(&c)) {
			return -ETIMEDOUT;
		}
		if (c.small_n == 0U || c.small_sq / c.small_n > PHASE_NOISE_MAX ||
		    c.large > PHASE_NODES * ESP_SDR_DIAG16_PAYLOAD / 2U / PHASE_MAX_SHARE) {
			/* A signal: no evidence either way. */
			next_gain = ++busy >= PHASE_BUSY_MAX;
			k_msleep(PHASE_BUSY_MS);
		} else if (c.large > PHASE_MAX_BAD) {
			parlio_clock_slip(div);
			slips++;
		} else if (c.cross_q >= PHASE_MIN_CROSS) {
			*done = true;
			break;
		} else {
			next_gain = true;
		}
		if (next_gain) {
			busy = 0U;
			if (++gain == ARRAY_SIZE(phase_gains)) {
				break;
			}
			esp_sdr_s31_gain(phase_gains[gain]);
			k_busy_wait(PHASE_SETTLE_US);
		}
	}
	return slips;
}

/* Returns the slips it took, or a negative error. */
static int phase_lock(unsigned int div, bool *bad)
{
	int saved = esp_sdr_s31_gain_get();
	bool done = false;
	int slips = phase_look_muted(div, &done);

	if (slips >= 0 && !done) {
		int more = phase_look_on_air(div, &done);

		slips = more < 0 ? more : slips + more;
	}
	*bad = !done;
	esp_sdr_s31_gain(saved);
	/* The front end's way back stays out of what the sink gets. */
	k_msleep(PHASE_RX_BACK_MS);
	return slips;
}

/* Means over a settled stretch of the ring, in 1/256 of a step: the median of three, for Wi-Fi frames. */
#define DC_SETTLE_MS  50
#define DC_WINDOWS    3U
#define DC_NODES      9U /* Some 16384 samples, as upstream. */

static bool ring_dc_measure(int32_t mean[2], void *user)
{
	int32_t seen[2][DC_WINDOWS];

	ARG_UNUSED(user);
	k_msleep(DC_SETTLE_MS);
	for (unsigned int w = 0; w < DC_WINDOWS; w++) {
		int64_t si = 0, sq = 0;
		uint64_t at = (prod_get() / ESP_SDR_DIAG16_PAYLOAD + 1U) * ESP_SDR_DIAG16_PAYLOAD;

		if (!wait_produced(at + DC_NODES * ESP_SDR_DIAG16_PAYLOAD)) {
			return false;
		}
		for (unsigned int n = 0; n < DC_NODES; n++) {
			const int8_t *s = (const int8_t *)(node_at(at + n * ESP_SDR_DIAG16_PAYLOAD) +
							   ESP_SDR_DIAG16_HOLE);

			for (size_t j = 0; j < ESP_SDR_DIAG16_PAYLOAD; j += 2U) {
				si += s[j];
				sq += s[j + 1U];
			}
		}
		seen[0][w] = (int32_t)(si * 256 / (int64_t)(DC_NODES * ESP_SDR_DIAG16_PAYLOAD / 2U));
		seen[1][w] = (int32_t)(sq * 256 / (int64_t)(DC_NODES * ESP_SDR_DIAG16_PAYLOAD / 2U));
	}
	for (unsigned int a = 0; a < 2U; a++) {
		for (unsigned int i = 1; i < DC_WINDOWS; i++) {
			for (unsigned int j = i; j > 0U && seen[a][j - 1U] > seen[a][j]; j--) {
				int32_t t = seen[a][j];

				seen[a][j] = seen[a][j - 1U];
				seen[a][j - 1U] = t;
			}
		}
		mean[a] = seen[a][DC_WINDOWS / 2U];
	}
	return true;
}

/* What pad k carries: the probe's lines that hold I[9:2] and Q[9:2] under the diagnostic select. */
static unsigned int signal_of(unsigned int k)
{
	if (k < 6U) {
		return HP_PROBE_TOP_OUT0_IDX + k + 2U;
	}
	if (k < 8U) {
		return HP_PROBE_TOP_OUT8_IDX + k - 6U;
	}
	if (k < 12U) {
		return HP_PROBE_TOP_OUT8_IDX + k - 4U;
	}
	return LCD_DATA_OUT_PAD_OUT0_IDX + k - 12U;
}

static void pads_route(const int8_t *pins)
{
	for (unsigned int k = 0; k < ESP_SDR_DIAG16_PINS; k++) {
		uint32_t p = (uint32_t)pins[k];

		gpio_ll_func_sel(&GPIO, p, PIN_FUNC_GPIO);
		gpio_ll_pullup_dis(&GPIO, p);
		gpio_ll_pulldown_dis(&GPIO, p);
		gpio_ll_set_drive_capability(&GPIO, p, GPIO_DRIVE_CAP_0);
		gpio_ll_input_enable(&GPIO, p);
		/* The connect call also enables the pad's output. */
		esp_rom_gpio_connect_out_signal(p, signal_of(k), false, false);
	}
	/* Bytes swapped on the way in: line k reads pad k ^ 8. */
	for (unsigned int k = 0; k < ESP_SDR_DIAG16_PINS; k++) {
		esp_rom_gpio_connect_in_signal((uint32_t)pins[k ^ 8U],
					       PARLIO_RX_DATA0_PAD_IN_IDX + k, false);
	}
}

static void pads_release(const int8_t *pins)
{
	for (unsigned int k = 0; k < ESP_SDR_DIAG16_PINS; k++) {
		uint32_t p = (uint32_t)pins[k];

		gpio_ll_output_disable(&GPIO, p);
		gpio_ll_input_disable(&GPIO, p);
		esp_rom_gpio_connect_out_signal(p, SIG_GPIO_OUT_IDX, false, false);
		gpio_ll_output_disable(&GPIO, p);
	}
}

static void ring_init(void)
{
	for (unsigned int i = 0; i < NODES; i++) {
		memset(&desc[i], 0, sizeof(desc[i]));
		desc[i].buffer = __esp_sdr_ring_start + i * ESP_SDR_DIAG16_NODE + ESP_SDR_DIAG16_HOLE;
		desc[i].dw0.size = ESP_SDR_DIAG16_PAYLOAD;
		desc[i].dw0.length = 0;
		desc[i].dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
		desc[i].next = &desc[(i + 1U) % NODES];
	}
}

static void parlio_start(unsigned int div, bool falling)
{
	parl_io_dev_t *dev = &PARL_IO;
	hal_utils_clk_div_t d = {.integer = div};

	PERIPH_RCC_ATOMIC() {
		parlio_ll_enable_bus_clock(0, true);
		parlio_ll_reset_register(0);
		parlio_ll_rx_reset_clock(dev);
		parlio_ll_rx_enable_clock(dev, false);
		parlio_ll_rx_set_clock_source(dev, PARLIO_CLK_SRC_PLL_F160M);
		parlio_ll_rx_set_clock_div(dev, &d);
	}
	parlio_ll_rx_reset_fifo(dev);
	parlio_ll_rx_start(dev, false);
	parlio_ll_rx_set_bus_width(dev, ESP_SDR_DIAG16_PINS);
	parlio_ll_rx_enable_clock_gating(dev, false);

	/* A soft delimiter: reception runs from parlio_ll_rx_start_soft_recv() on. */
	parlio_ll_rx_set_sample_clock_edge(dev,
					   falling ? PARLIO_SAMPLE_EDGE_NEG : PARLIO_SAMPLE_EDGE_POS);
	parlio_ll_rx_set_bit_pack_order(dev, PARLIO_BIT_PACK_ORDER_LSB);
	parlio_ll_rx_set_soft_recv_mode(dev);
	parlio_ll_rx_set_recv_bit_len(dev, EOF_BYTES * 8U);
	parlio_ll_rx_set_eof_condition(dev, PARLIO_LL_RX_EOF_COND_RX_FULL);
	parlio_ll_rx_enable_timeout(dev, false);
	parlio_ll_rx_update_config(dev);
}

static void dma_start(void)
{
	axi_dma_dev_t *dma = AXI_DMA_LL_GET_HW(0);

	PERIPH_RCC_ATOMIC() {
		gdma_ll_enable_bus_clock(DMA_GROUP, true);
		gdma_ll_reset_register(DMA_GROUP);
	}
	axi_dma_ll_force_enable_reg_clock(dma, true);
	axi_dma_ll_set_default_memory_range(dma);
	axi_dma_ll_rx_reset_channel(dma, DMA_CH);
	axi_dma_ll_rx_enable_owner_check(dma, DMA_CH, false);
	axi_dma_ll_rx_enable_data_burst(dma, DMA_CH, true);
	axi_dma_ll_rx_enable_descriptor_burst(dma, DMA_CH, true);
	axi_dma_ll_rx_set_burst_size(dma, DMA_CH, DMA_BURST);
	axi_dma_ll_rx_disconnect_all(dma, DMA_CH);
	axi_dma_ll_rx_connect_to_periph(dma, DMA_CH, SOC_GDMA_TRIG_PERIPH_PARLIO0);
	axi_dma_ll_rx_clear_interrupt_status(dma, DMA_CH, AXI_DMA_LL_RX_EVENT_MASK);
	axi_dma_ll_rx_enable_interrupt(dma, DMA_CH, GDMA_LL_EVENT_RX_DONE, true);
	axi_dma_ll_rx_set_desc_addr(dma, DMA_CH, (uint32_t)&desc[0]);
	axi_dma_ll_rx_start(dma, DMA_CH);
}

static void dma_stop(void)
{
	axi_dma_dev_t *dma = AXI_DMA_LL_GET_HW(0);

	axi_dma_ll_rx_enable_interrupt(dma, DMA_CH, AXI_DMA_LL_RX_EVENT_MASK, false);
	axi_dma_ll_rx_stop(dma, DMA_CH);
	axi_dma_ll_rx_disconnect_all(dma, DMA_CH);
}

int esp_sdr_diag16_run(const struct esp_sdr_diag16_cfg *cfg, struct esp_sdr_diag16_stats *stats)
{
	struct esp_sdr_diag16_stats s = {0};
	const int8_t *pins;
	uint64_t cons = 0, first = 0, sink_cycles = 0, seen = 0;
	int64_t t_end = 0, t_start, t_seen;
	intr_handle_t ih = NULL;
	unsigned int div;
	int ret = 0;

	if (cfg == NULL || cfg->sink == NULL || cfg->rate_hz == 0U || cfg->rate_hz > MAX_RATE_HZ ||
	    PLL_HZ % cfg->rate_hz != 0U || cfg->wake_nodes > NODES / 2U) {
		return -EINVAL;
	}
	div = PLL_HZ / cfg->rate_hz;
	/*
	 * An odd divider puts every second sample on the bus's edge, which no slip mends. A slip takes
	 * one more than the divider, which must fit too.
	 */
	if (div % 2U != 0U || div + 1U > PARLIO_LL_RX_MAX_CLK_INT_DIV) {
		return -EINVAL;
	}
	pins = cfg->pins != NULL ? cfg->pins : default_pins;
	for (unsigned int k = 0; k < ESP_SDR_DIAG16_PINS; k++) {
		if (pins[k] < 0 || !GPIO_IS_VALID_OUTPUT_GPIO(pins[k])) {
			return -EINVAL;
		}
	}
	if (!esp_sdr_ready) {
		return -EAGAIN;
	}
	/* Ahead of the lock too: a run holds it to its end. */
	if (st.running) {
		return -EBUSY;
	}
	k_mutex_lock(&esp_sdr_lock, K_FOREVER);
	/* A replay or the dump ring from this thread (the lock is recursive) has the group. */
	if (st.running || esp_sdr_s31_dac_busy() || esp_sdr_s31_ring_busy()) {
		k_mutex_unlock(&esp_sdr_lock);
		return -EBUSY;
	}
	st.running = true;
	prod_set(0U);
	st.head = 0;
	st.tick_first = st.tick_last = 0;
	st.prod_first = st.prod_last = 0;
	st.isrs = 0;
	st.overrun = false;
	st.wake_bytes = MAX(cfg->wake_nodes, 1U) * ESP_SDR_DIAG16_PAYLOAD;
	st.first = 0U;
	k_sem_reset(&ring_sem);
	atomic_set(&st.stop, 0);
	s.byte_rate = cfg->rate_hz * 2U;
	ring_init();

	esp_sdr_s31_rx_on();
	esp_sdr_s31_diag_start();
	pads_route(pins);
	ret = esp_intr_alloc(ETS_AXI_PDMA_IN_CH0_INTR_SOURCE + DMA_CH, ESP_PRIO_TO_FLAGS(1), ring_isr,
			     NULL, &ih);
	if (ret != 0) {
		goto stop_dump;
	}
	parlio_start(div, cfg->falling_edge);
	dma_start();
	/* Order as IDF's PARLIO RX: DMA armed, then the unit and its clock, then the soft delimiter. */
	parlio_ll_rx_start(&PARL_IO, true);
	PERIPH_RCC_ATOMIC() {
		parlio_ll_rx_enable_clock(&PARL_IO, true);
	}
	parlio_ll_rx_start_soft_recv(&PARL_IO, true);

	t_start = k_uptime_get();
	ret = phase_lock(div, &s.phase_bad);
	if (ret < 0) {
		LOG_ERR("diag16: the ring got no data");
		goto stop_ring;
	}
	s.phase_tries = (uint8_t)MIN(ret, UINT8_MAX);
	ret = 0;
	if (s.phase_bad) {
		LOG_WRN("diag16: no clean sampling phase");
		if (cfg->phase_required) {
			ret = -EIO;
			goto stop_ring;
		}
	}
	if (cfg->dc_correct && esp_sdr_s31_gain_get() >= 0) {
		struct esp_sdr_rx_dc dc;

		s.dc_ret = (int16_t)esp_sdr_s31_dc_calibrate(ring_dc_measure, NULL, &dc);
		s.dc_before[0] = dc.before[0];
		s.dc_before[1] = dc.before[1];
		s.dc_after[0] = dc.after[0];
		s.dc_after[1] = dc.after[1];
	}
	/*
	 * What the sink sees starts here, at the next node: at the lowest rates the one in progress
	 * can have begun before the gain came back.
	 */
	first = (prod_get() / ESP_SDR_DIAG16_PAYLOAD + 1U) * ESP_SDR_DIAG16_PAYLOAD;
	cons = first;
	st.first = first;
	t_start = k_uptime_get();
	t_seen = t_start;
	if (cfg->seconds != 0U) {
		t_end = t_start + (int64_t)cfg->seconds * MSEC_PER_SEC;
	}

	while (atomic_get(&st.stop) == 0) {
		uint64_t p;

		if (k_sem_take(&ring_sem, K_MSEC(100)) != 0 && prod_get() == seen) {
			/* Even the slowest rate wakes well within this. */
			if (k_uptime_get() - t_seen > STALL_MS) {
				LOG_ERR("diag16: the ring stopped");
				ret = -ETIMEDOUT;
				break;
			}
			continue;
		}
		if (st.overrun) {
			s.overrun = true;
			ret = -EOVERFLOW;
			break;
		}
		p = prod_get();
		if (p != seen) {
			seen = p;
			t_seen = k_uptime_get();
		}
		/* A ring's worth at a time: a sink slower than the DMA still sees stop and time. */
		for (unsigned int n = 0U; n < NODES && cons + ESP_SDR_DIAG16_PAYLOAD <= p; n++) {
			uint32_t t0;
			int core;

			if (p - cons >= (NODES - 1U) * ESP_SDR_DIAG16_PAYLOAD) {
				/* The DMA is about to overwrite the oldest node: on to the newest. */
				uint64_t to = (p / ESP_SDR_DIAG16_PAYLOAD - 1U) * ESP_SDR_DIAG16_PAYLOAD;

				s.skips++;
				s.skipped_nodes += (uint32_t)((to - cons) / ESP_SDR_DIAG16_PAYLOAD);
				cons = to;
			}
			/* The cycle counter is per core: a sink that moved to the other goes uncounted. */
			core = esp_cpu_get_core_id();
			t0 = (uint32_t)esp_cpu_get_cycle_count();
			cfg->sink(cfg->user, node_at(cons), cons - first);
			if (esp_cpu_get_core_id() == core) {
				sink_cycles += (uint32_t)((uint32_t)esp_cpu_get_cycle_count() - t0);
			}
			cons += ESP_SDR_DIAG16_PAYLOAD;
			s.consumed += ESP_SDR_DIAG16_PAYLOAD;
			p = prod_get();
		}
		if (t_end != 0 && k_uptime_get() >= t_end) {
			break;
		}
	}

stop_ring:
	s.isrs = st.isrs;
	s.dma_raw = axi_dma_ll_rx_get_interrupt_status(AXI_DMA_LL_GET_HW(0), DMA_CH, true);
	parlio_ll_rx_start_soft_recv(&PARL_IO, false);
	parlio_ll_rx_start(&PARL_IO, false);
	PERIPH_RCC_ATOMIC() {
		parlio_ll_rx_enable_clock(&PARL_IO, false);
	}
	dma_stop();
	esp_intr_free(ih);
	s.bytes = prod_get();
	if (s.consumed != 0U) {
		s.sink_cycles_x100 = (uint32_t)(sink_cycles * 100U / s.consumed);
	}
	if (st.prod_last > st.prod_first) {
		/* Bytes between the first and the last interrupt against the system timer. */
		int64_t ticks = (int64_t)(st.tick_last - st.tick_first);
		int64_t got = (int64_t)(st.prod_last - st.prod_first);
		int64_t hz = sys_clock_hw_cycles_per_sec();
		/* In two parts: ticks times the rate alone overflows after some hours. */
		int64_t want = ticks / hz * s.byte_rate + ticks % hz * s.byte_rate / hz;

		if (want > 4000000) {
			s.ppm_x100 = (int32_t)((got - want) * 100000000 / want);
		}
	}
stop_dump:
	pads_release(pins);
	esp_sdr_s31_diag_stop();
	esp_sdr_s31_rx_on();
	st.running = false;
	k_mutex_unlock(&esp_sdr_lock);
	if (stats != NULL) {
		*stats = s;
	}
	return ret;
}

void esp_sdr_diag16_stop(void)
{
	atomic_set(&st.stop, 1);
	k_sem_give(&ring_sem);
}

bool esp_sdr_diag16_active(void)
{
	return st.running;
}
