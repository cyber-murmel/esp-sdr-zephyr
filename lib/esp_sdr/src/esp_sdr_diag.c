/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Gapless receive on the ESP32-C5 through the modem diagnostic bus (esp_sdr_diag.h): the dump
 * engine runs free, the GPIO matrix loops the bus back into the PARLIO receiver, a GDMA ring
 * collects the bytes. The method is ESPsoup's (github.com/pit711/espsoup-firmware), an
 * independent implementation: PARLIO and GDMA are driven through the HAL's low-level layer,
 * since Zephyr has no PARLIO driver and its GDMA driver builds only linear chains.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/interrupt_controller/intc_esp32.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <esp_rom_gpio.h>
#include <hal/ahb_dma_ll.h>
#include <hal/dma_types.h>
#include <hal/gdma_ll.h>
#include <hal/gpio_ll.h>
#include <hal/hal_utils.h>
#include <hal/parlio_ll.h>
#include <soc/gpio_sig_map.h>
#include <soc/interrupts.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_diag.h>
#include <esp_sdr/esp_sdr_paced.h>

#include "esp_sdr_diag_dsp.h"
#include "esp_sdr_priv.h"

LOG_MODULE_DECLARE(esp_sdr, CONFIG_ESP_SDR_LOG_LEVEL);

#define PLL_HZ 240000000U

/* One GDMA RX channel of the AHB group, fixed: Zephyr's GDMA driver stays disabled. */
#define DMA_CH      0U
/* GDMA trigger id of PARLIO0 (soc/gdma_channel.h). */
#define DMA_TRIG_PARLIO 9
/* Descriptor buffers hold at most 4095 bytes, 4-byte aligned: 16 nodes of 3584 bytes fill the ring. */
#define NODE_BYTES  3584U
#define NODES       (ESP_SDR_DIAG_RING / NODE_BYTES)
/* The dump engine counts 17 bits; counts above 65535 never reach DONE, so it runs for good. */
#define DUMP_ENDLESS 0x1ffffU

BUILD_ASSERT(NODES * NODE_BYTES == ESP_SDR_DIAG_RING, "ring is a whole number of nodes");
BUILD_ASSERT(ESP_SDR_DIAG_RING % ESP_SDR_DIAG_BLOCK == 0U, "ring is a whole number of blocks");

static const int8_t default_pins[ESP_SDR_DIAG_PINS] = {0, 1, 3, 4, 5, 7, 10, 23};

/*
 * The first 64 KiB of the bank are ours: the ring, then 8 KiB for the descriptors, the level
 * histogram and the output samples. The osmosdr_serial image has no RAM to spare.
 */
extern uint8_t __esp_sdr_bank_org[];
#define BANK_LOW_BYTES 0x10000U
static uint8_t *const ring = __esp_sdr_bank_org;
static uint8_t *const spare = __esp_sdr_bank_org + ESP_SDR_DIAG_RING;
static dma_descriptor_t *const desc = (dma_descriptor_t *)(__esp_sdr_bank_org + ESP_SDR_DIAG_RING);
#define SPARE_HIST 1024U
#define SPARE_IQ   2048U
/* Decimated samples reach the sink in chunks of 2 KiB of ring bytes (51 us at 40 MB/s). */
#define IQ_CHUNK   2048U
#define IQ_PAIRS   (IQ_CHUNK / DIAG_BOX / DIAG_CIC_R_MIN + 2U)

BUILD_ASSERT(NODES * sizeof(dma_descriptor_t) <= SPARE_HIST, "descriptors fit");
BUILD_ASSERT(SPARE_IQ + 2U * IQ_PAIRS * sizeof(int32_t) <= BANK_LOW_BYTES - ESP_SDR_DIAG_RING,
	     "output samples fit");

static struct {
	/* Written by the interrupt. */
	volatile uint32_t prod_lo, prod_hi;
	volatile uint32_t head;
	volatile uint64_t tick_first, tick_last;
	volatile uint64_t prod_first, prod_last;
	volatile uint32_t isrs;
	volatile bool overrun;
	struct k_sem sem;
	atomic_t stop;
	bool running;
} st;

static inline uint64_t prod_get(void)
{
	uint32_t hi, lo;

	do {
		hi = st.prod_hi;
		lo = st.prod_lo;
	} while (hi != st.prod_hi);
	return ((uint64_t)hi << 32) | lo;
}

static void diag_isr(void *arg)
{
	ahb_dma_dev_t *dma = AHB_DMA_LL_GET_HW(0);
	uint32_t status = ahb_dma_ll_rx_get_interrupt_status(dma, DMA_CH, false);
	uint64_t p = prod_get(), before = p;
	uint32_t head = st.head, walked = 0U;

	ARG_UNUSED(arg);
	ahb_dma_ll_rx_clear_interrupt_status(dma, DMA_CH, status);
	st.isrs++;
	/*
	 * The hardware writes the received length back into every descriptor it finishes. Walk
	 * the ring from the last one seen, so that interrupts merging into one lose nothing.
	 */
	while (walked < NODES) {
		volatile dma_descriptor_t *d = &desc[head];
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
		/* Every node finished since the last visit: the ring may have lapped us. */
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
		st.prod_hi = (uint32_t)(p >> 32);
		st.prod_lo = (uint32_t)p;
		if ((p / ESP_SDR_DIAG_BLOCK) != (before / ESP_SDR_DIAG_BLOCK) || st.overrun) {
			k_sem_give(&st.sem);
		}
	}
}

/* The diagnostic lane that carries data line k of the nibble window at lsb. */
static unsigned int lane_of(unsigned int k, unsigned int lsb)
{
	return k < 4U ? lsb + k : 10U + lsb + (k - 4U);
}

static void pads_route(const int8_t *pins, unsigned int lsb)
{
	for (unsigned int k = 0; k < ESP_SDR_DIAG_PINS; k++) {
		uint32_t p = (uint32_t)pins[k];

		gpio_ll_func_sel(&GPIO, p, PIN_FUNC_GPIO);
		gpio_ll_pullup_dis(&GPIO, p);
		gpio_ll_pulldown_dis(&GPIO, p);
		gpio_ll_set_drive_capability(&GPIO, p, GPIO_DRIVE_CAP_0);
		gpio_ll_input_enable(&GPIO, p);
		/* The connect call also enables the pad's output. */
		esp_rom_gpio_connect_out_signal(p, MODEM_DIAG0_IDX + lane_of(k, lsb), false, false);
		esp_rom_gpio_connect_in_signal(p, PARL_RX_DATA0_IDX + k, false);
	}
}

static void pads_release(const int8_t *pins)
{
	for (unsigned int k = 0; k < ESP_SDR_DIAG_PINS; k++) {
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
		desc[i].buffer = ring + i * NODE_BYTES;
		desc[i].dw0.size = NODE_BYTES;
		desc[i].dw0.length = 0;
		desc[i].dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
		desc[i].next = &desc[(i + 1U) % NODES];
	}
}

static void parlio_start(unsigned int div, bool falling)
{
	parl_io_dev_t *dev = &PARL_IO;
	hal_utils_clk_div_t d = {.integer = div};

	parlio_ll_enable_bus_clock(0, true);
	parlio_ll_reset_register(0);
	parlio_ll_rx_reset_clock(dev);
	parlio_ll_rx_reset_fifo(dev);
	parlio_ll_rx_enable_clock(dev, false);
	parlio_ll_rx_start(dev, false);
	parlio_ll_rx_set_clock_source(dev, PARLIO_CLK_SRC_PLL_F240M);
	parlio_ll_rx_set_clock_div(dev, &d);
	parlio_ll_rx_set_bus_width(dev, 8);
	parlio_ll_rx_enable_clock_gating(dev, false);

	/* A soft delimiter: reception runs from parlio_ll_rx_start_soft_recv() on, EOF at the ring length. */
	parlio_ll_rx_set_sample_clock_edge(dev, falling ? PARLIO_SAMPLE_EDGE_NEG : PARLIO_SAMPLE_EDGE_POS);
	parlio_ll_rx_set_bit_pack_order(dev, PARLIO_BIT_PACK_ORDER_LSB);
	parlio_ll_rx_set_soft_recv_mode(dev);
	parlio_ll_rx_set_recv_bit_len(dev, ESP_SDR_DIAG_RING * 8U);
	parlio_ll_rx_set_eof_condition(dev, PARLIO_LL_RX_EOF_COND_RX_FULL);
	parlio_ll_rx_enable_timeout(dev, false);
	parlio_ll_rx_update_config(dev);
}

static void dma_start(void)
{
	ahb_dma_dev_t *dma = AHB_DMA_LL_GET_HW(0);

	gdma_ll_enable_bus_clock(0, true);
	gdma_ll_reset_register(0);
	ahb_dma_ll_force_enable_reg_clock(dma, true);
	ahb_dma_ll_set_default_memory_range(dma);
	ahb_dma_ll_rx_reset_channel(dma, DMA_CH);
	ahb_dma_ll_rx_enable_owner_check(dma, DMA_CH, false);
	ahb_dma_ll_rx_disconnect_all(dma, DMA_CH);
	ahb_dma_ll_rx_connect_to_periph(dma, DMA_CH, DMA_TRIG_PARLIO);
	ahb_dma_ll_rx_clear_interrupt_status(dma, DMA_CH, AHB_DMA_LL_RX_EVENT_MASK);
	ahb_dma_ll_rx_enable_interrupt(dma, DMA_CH, GDMA_LL_EVENT_RX_DONE, true);
	ahb_dma_ll_rx_set_desc_addr(dma, DMA_CH, (uint32_t)&desc[0]);
	ahb_dma_ll_rx_start(dma, DMA_CH);
}

static void dma_stop(void)
{
	ahb_dma_dev_t *dma = AHB_DMA_LL_GET_HW(0);

	ahb_dma_ll_rx_enable_interrupt(dma, DMA_CH, AHB_DMA_LL_RX_EVENT_MASK, false);
	ahb_dma_ll_rx_stop(dma, DMA_CH);
	ahb_dma_ll_rx_disconnect_all(dma, DMA_CH);
}

__weak void esp_sdr_diag_armed_hook(void)
{
}

int esp_sdr_diag_level(uint8_t *lsb)
{
	/* Histogram of the magnitudes in the bank's spare bytes: the caller's stack is small. */
	uint16_t *hist = (uint16_t *)(spare + SPARE_HIST);
	struct esp_sdr_rx_burst b = {0};
	uint32_t total = 0U, acc = 0U, p = 0U;
	int ret = esp_sdr_rx_capture(ESP_SDR_RATE_80MSPS, 4096, &b);

	if (ret != 0) {
		return ret;
	}
	memset(hist, 0, 513U * sizeof(hist[0]));
	for (size_t n = 0; n < b.count; n++) {
		uint32_t w = b.words[n];
		int32_t a = ((int32_t)(w << 22)) >> 22;
		int32_t c = ((int32_t)(w << 12)) >> 22;

		hist[MIN(512U, (uint32_t)(a < 0 ? -a : a))]++;
		hist[MIN(512U, (uint32_t)(c < 0 ? -c : c))]++;
		total += 2U;
	}
	/* The 99.9 % point of the magnitudes. */
	for (p = 0; p < 512U; p++) {
		acc += hist[p];
		if (acc * 1000U >= total * 999U) {
			break;
		}
	}
	/* A nibble window at L holds magnitudes below 8 << L. */
	*lsb = 0;
	while (*lsb < ESP_SDR_DIAG_LSB_MAX && p >= (8U << *lsb)) {
		(*lsb)++;
	}
	return 0;
}

int esp_sdr_diag_run(const struct esp_sdr_diag_cfg *cfg, struct esp_sdr_diag_stats *stats)
{
	struct esp_sdr_diag_stats s = {0};
	const int8_t *pins;
	uint32_t usage, saved = 0, ctrl, div;
	uint64_t cons = 0, sink_cycles = 0, out_idx = 0;
	struct diag_cic cic = {0};
	uint32_t inv = 0;
	/* A chunk holds 128 box16 outputs; a CIC of R emits at most ceil(128 / R) + 1 per chunk. */
	int32_t *iq = (int32_t *)(spare + SPARE_IQ);
	int64_t t_end = 0;
	intr_handle_t ih = NULL;
	uint8_t lsb;
	int ret = 0;

	if (cfg == NULL) {
		return -EINVAL;
	}
	div = cfg->clk_div == 0U ? 6U : cfg->clk_div;
	if (div < 2U || (cfg->lsb != ESP_SDR_DIAG_LSB_AUTO && cfg->lsb > ESP_SDR_DIAG_LSB_MAX)) {
		return -EINVAL;
	}
	if (cfg->decim != 0U && (cfg->decim < ESP_SDR_DIAG_DECIM_MIN || cfg->decim > ESP_SDR_DIAG_DECIM_MAX)) {
		return -EINVAL;
	}
	pins = cfg->pins != NULL ? cfg->pins : default_pins;
	for (unsigned int k = 0; k < ESP_SDR_DIAG_PINS; k++) {
		if (pins[k] < 0 || pins[k] > 28) {
			return -EINVAL;
		}
	}
	if (!esp_sdr_ready) {
		return -EAGAIN;
	}
	if (esp_sdr_bank_usage(&usage) != 0) {
		return -ENOMEM;
	}
	k_mutex_lock(&esp_sdr_lock, K_FOREVER);
	if (st.running) {
		k_mutex_unlock(&esp_sdr_lock);
		return -EBUSY;
	}
	st.running = true;
	esp_sdr_rx_prepare();

	lsb = cfg->lsb;
	if (lsb == ESP_SDR_DIAG_LSB_AUTO) {
		ret = esp_sdr_diag_level(&lsb);
		if (ret != 0) {
			goto out;
		}
	}
	s.lsb = lsb;
	s.byte_rate = PLL_HZ / div;
	if (cfg->decim != 0U) {
		s.iq_rate = s.byte_rate / DIAG_BOX / cfg->decim;
		inv = diag_cic_inv(cfg->decim, lsb);
		diag_cic_init(&cic, cfg->decim);
	}

	st.prod_lo = 0;
	st.prod_hi = 0;
	st.head = 0;
	st.tick_first = st.tick_last = 0;
	st.prod_first = st.prod_last = 0;
	st.isrs = 0;
	st.overrun = false;
	k_sem_init(&st.sem, 0, 1);
	atomic_set(&st.stop, 0);
	ring_init();

	/* Free-running dump: RUN with an endless count, then the bank goes back to the CPU. */
	saved = esp_sdr_dump_arm(usage);
	REG_WRITE(DUMP_MODE_REG, REG_READ(DUMP_MODE_REG) & ~DUMP_MODE_RATE);
	ctrl = DUMP_CTRL_RUN | FIELD_PREP(DUMP_CTRL_COUNT, DUMP_ENDLESS);
	REG_WRITE(DUMP_CTRL_REG, FIELD_PREP(DUMP_CTRL_COUNT, DUMP_ENDLESS) | DUMP_CTRL_DONE);
	REG_WRITE(DUMP_CTRL_REG, ctrl);
	REG_WRITE(DUMP_CTRL_REG, ctrl | DUMP_CTRL_TRIGGER);
	REG_WRITE(DUMP_CTRL_REG, ctrl);
	REG_WRITE(DUMP_USAGE_REG, saved);
	__asm__ volatile("fence rw, rw" ::: "memory");
	(void)REG_READ(DUMP_USAGE_REG);
	esp_sdr_diag_armed_hook();

	pads_route(pins, lsb);
	ret = esp_intr_alloc(ETS_DMA_IN_CH0_INTR_SOURCE + DMA_CH, ESP_PRIO_TO_FLAGS(1), diag_isr, NULL,
			     &ih);
	if (ret != 0) {
		goto stop_dump;
	}
	parlio_start(div, cfg->falling_edge);
	dma_start();
	/* Order as IDF's PARLIO RX: DMA armed, then the unit and its clock, then the soft delimiter. */
	parlio_ll_rx_start(&PARL_IO, true);
	parlio_ll_rx_enable_clock(&PARL_IO, true);
	parlio_ll_rx_start_soft_recv(&PARL_IO, true);

	if (cfg->seconds != 0U) {
		t_end = k_uptime_get() + (int64_t)cfg->seconds * MSEC_PER_SEC;
	}
	int64_t t_start = k_uptime_get();

	while (atomic_get(&st.stop) == 0) {
		uint64_t p;

		if (k_sem_take(&st.sem, K_MSEC(100)) != 0 && prod_get() == 0U) {
			if (k_uptime_get() - t_start > 500) {
				LOG_ERR("diag: the ring got no data");
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
		while (cons + ESP_SDR_DIAG_BLOCK <= p) {
			uint32_t t0;

			if (p - cons > ESP_SDR_DIAG_RING - ESP_SDR_DIAG_BLOCK) {
				/* The producer is about to overwrite the oldest block: jump to the newest. */
				uint64_t to = (p / ESP_SDR_DIAG_BLOCK - 1U) * ESP_SDR_DIAG_BLOCK;

				uint32_t gone = (uint32_t)((to - cons) / ESP_SDR_DIAG_BLOCK);

				s.skips++;
				s.skipped_blocks += gone;
				cons = to;
				if (cfg->decim != 0U) {
					/* The filter restarts; the output index jumps over what was lost. */
					diag_cic_init(&cic, cfg->decim);
					out_idx += (uint64_t)gone * (ESP_SDR_DIAG_BLOCK / DIAG_BOX) / cfg->decim + 3U;
				}
			}
			t0 = esp_sdr_paced_cycles();
			if (cfg->decim != 0U) {
				/* In chunks, so that the sink can serve a slow consumer such as a USB FIFO often. */
				for (size_t off = 0; off < ESP_SDR_DIAG_BLOCK; off += IQ_CHUNK) {
					size_t n = diag_cic_run(
						&cic, (const uint32_t *)(ring + (cons % ESP_SDR_DIAG_RING) + off),
						IQ_CHUNK, iq);

					for (size_t k = 0; k < 2U * n; k++) {
						iq[k] = diag_norm(iq[k], inv);
					}
					if (cfg->iq_sink != NULL) {
						cfg->iq_sink(cfg->iq_user, iq, n, out_idx);
					}
					out_idx += n;
				}
			}
			if (cfg->sink != NULL) {
				cfg->sink(cfg->user, ring + (cons % ESP_SDR_DIAG_RING), ESP_SDR_DIAG_BLOCK,
					  cons);
			}
			sink_cycles += (uint32_t)(esp_sdr_paced_cycles() - t0);
			cons += ESP_SDR_DIAG_BLOCK;
			s.consumed += ESP_SDR_DIAG_BLOCK;
			p = prod_get();
		}
		if (t_end != 0 && k_uptime_get() >= t_end) {
			break;
		}
	}

	s.isrs = st.isrs;
	s.dma_raw = ahb_dma_ll_rx_get_interrupt_status(AHB_DMA_LL_GET_HW(0), DMA_CH, true);
	for (unsigned int k = 0; k < ARRAY_SIZE(s.dw0); k++) {
		s.dw0[k] = ((volatile uint32_t *)&desc[k])[0];
	}
	parlio_ll_rx_start_soft_recv(&PARL_IO, false);
	parlio_ll_rx_start(&PARL_IO, false);
	parlio_ll_rx_enable_clock(&PARL_IO, false);
	dma_stop();
	esp_intr_free(ih);
	s.bytes = prod_get();
	if (s.consumed != 0U) {
		s.sink_cycles_x100 = (uint32_t)(sink_cycles * 100U / s.consumed);
	}
	if (st.prod_last > st.prod_first) {
		/* Bytes between the first and the last interrupt against the system timer. */
		uint64_t ticks = st.tick_last - st.tick_first;
		double exp = (double)ticks / sys_clock_hw_cycles_per_sec() * s.byte_rate;
		double got = (double)(st.prod_last - st.prod_first);

		if (exp > 4.0e6) {
			s.ppm_x100 = (int32_t)((got / exp - 1.0) * 1.0e8);
		}
	}
stop_dump:
	pads_release(pins);
	esp_sdr_dump_disarm(saved);
out:
	st.running = false;
	k_mutex_unlock(&esp_sdr_lock);
	if (stats != NULL) {
		*stats = s;
	}
	return ret;
}

void esp_sdr_diag_stop(void)
{
	atomic_set(&st.stop, 1);
	k_sem_give(&st.sem);
}

bool esp_sdr_diag_active(void)
{
	return st.running;
}
