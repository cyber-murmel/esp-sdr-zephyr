/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Does the ESP32-S3's I2S0 TX run with bck_div_num = 1 (the hal writes the field as val - 1, so
 * the register holds 0), at what bit rate, and is a GPIO-matrix inverted copy of the data pad
 * complementary? No wires: both pads are read back through the GPIO matrix, one to the CPU
 * (GPIO.in) and into PCNT units. Straight hal LL, not Zephyr's I2S driver.
 *
 * Bit rate = source clock / (mclk divider * bck_div). The mclk divider is fixed at 2 (the
 * smallest the hal programs). The data comes from a GDMA ring of one pattern word.
 *
 * PCNT: 16-bit units on the 80 MHz APB clock, inputs synchronised to it; the TRM wants a pulse of
 * at least one APB cycle, so about 40 MHz of rising edges at best. Counts above the high limit
 * wrap to 0 with an event, which the gate loop polls (no interrupt is routed).
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/sys/barrier.h>

#include <esp_private/periph_ctrl.h>
#include <hal/dma_types.h>
#include <hal/gdma_channel.h>
#include <hal/gdma_ll.h>
#include <hal/gpio_ll.h>
#include <hal/hal_utils.h>
#include <hal/i2s_ll.h>
#include <hal/pcnt_ll.h>
#include <soc/gpio_pins.h>
#include <soc/gpio_sig_map.h>
#include <soc/io_mux_reg.h>

#include "i2sbench.h"

/* Two adjacent pads with nothing on them on the DevKitC: no strapping, flash, PSRAM, USB or
 * UART pin, and no enabled node of the board's devicetree uses them (I2C1 and TWAI would). */
#define PAD_D   4 /* I2S0O_SD_OUT */
#define PAD_DN  5 /* its inverse */

#define GDMA_CH         0
#define MCLK_DIV        2
#define RING_WORDS      16
#define GATE_US         10000
#define SETTLE_US       2000
#define SAMPLES         4000
#define PCNT_LIMIT      PCNT_LL_MAX_LIM
#define SRC_IDX_CH0(u)  (PCNT_SIG_CH0_IN0_IDX + 4 * (u))
#define CTRL_IDX_CH0(u) (PCNT_CTRL_CH0_IN0_IDX + 4 * (u))

/* PCNT units: 0 pad D edges, 1 D edges while DN is high, 2 DN edges while D is high, 3 DN edges. */
enum { U_D, U_D_OVER, U_DN_OVER, U_DN, U_N };

BUILD_ASSERT(PCNT_SIG_CH0_IN1_IDX - PCNT_SIG_CH0_IN0_IDX == 4, "PCNT signal spacing");
BUILD_ASSERT(PCNT_CTRL_CH0_IN1_IDX - PCNT_CTRL_CH0_IN0_IDX == 4, "PCNT control spacing");

static dma_descriptor_t ring_desc __aligned(4);
static uint32_t ring_buf[RING_WORDS] __aligned(4);

static gdma_dev_t *gdma(void)
{
	return GDMA_LL_GET_HW(0);
}

static void gdma_up(void)
{
	/* The controller is gated and held in reset after boot; Zephyr's driver is not built. */
	const bool was_on = SYSTEM.perip_clk_en1.dma_clk_en && !SYSTEM.perip_rst_en1.dma_rst;

	PERIPH_RCC_ATOMIC() {
		gdma_ll_enable_bus_clock(0, true);
		if (!was_on) {
			gdma_ll_reset_register(0);
		}
	}
	gdma_ll_force_enable_reg_clock(gdma(), true);
}

static void pad_out(uint32_t pad, bool inv)
{
	gpio_ll_func_sel(&GPIO, pad, PIN_FUNC_GPIO);
	gpio_ll_input_enable(&GPIO, pad);
	gpio_ll_set_output_enable_ctrl(&GPIO, pad, false, false);
	gpio_ll_output_enable(&GPIO, pad);
	gpio_ll_set_output_signal_matrix_source(&GPIO, pad, I2S0O_SD_OUT_IDX, inv);
}

static void pad_free(uint32_t pad)
{
	gpio_ll_set_output_signal_matrix_source(&GPIO, pad, SIG_GPIO_OUT_IDX, false);
	gpio_ll_output_disable(&GPIO, pad);
}

static void pcnt_unit(uint32_t u, uint32_t sig_pad, uint32_t ctrl_pad, bool gate_high)
{
	pcnt_dev_t *hw = PCNT_LL_GET_HW(0);

	gpio_ll_set_input_signal_matrix_source(&GPIO, SRC_IDX_CH0(u), sig_pad, false);
	gpio_ll_set_input_signal_matrix_source(&GPIO, CTRL_IDX_CH0(u),
					       ctrl_pad ? ctrl_pad : GPIO_MATRIX_CONST_ZERO_INPUT, false);
	pcnt_ll_stop_count(hw, u);
	pcnt_ll_enable_glitch_filter(hw, u, false);
	pcnt_ll_disable_all_events(hw, u);
	pcnt_ll_set_high_limit_value(hw, u, PCNT_LIMIT);
	pcnt_ll_set_low_limit_value(hw, u, PCNT_LL_MIN_LIM);
	pcnt_ll_enable_high_limit_event(hw, u, true);
	pcnt_ll_set_edge_action(hw, u, 0, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
				PCNT_CHANNEL_EDGE_ACTION_HOLD);
	/* Counting only while the control pad is high: hold on low. */
	pcnt_ll_set_level_action(hw, u, 0, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
				 gate_high ? PCNT_CHANNEL_LEVEL_ACTION_HOLD :
					     PCNT_CHANNEL_LEVEL_ACTION_KEEP);
	pcnt_ll_clear_count(hw, u);
}

/* Counts for the gate, edges per unit (limit events folded in); returns the gate in cycles. */
static uint32_t pcnt_gate(uint64_t edges[U_N])
{
	pcnt_dev_t *hw = PCNT_LL_GET_HW(0);
	const uint32_t all = BIT_MASK(U_N);
	uint32_t wraps[U_N] = {0};
	const uint32_t gate = (uint32_t)((uint64_t)GATE_US * sys_clock_hw_cycles_per_sec() / 1000000U);
	unsigned int key = irq_lock();

	for (uint32_t u = 0; u < U_N; u++) {
		pcnt_ll_clear_count(hw, u);
	}
	pcnt_ll_enable_intr(hw, all, true);
	pcnt_ll_clear_intr_status(hw, all);
	const uint32_t t0 = k_cycle_get_32();

	for (uint32_t u = 0; u < U_N; u++) {
		pcnt_ll_start_count(hw, u);
	}
	while (k_cycle_get_32() - t0 < gate) {
		uint32_t st = pcnt_ll_get_intr_status(hw);

		if (st != 0U) {
			pcnt_ll_clear_intr_status(hw, st);
			for (uint32_t u = 0; u < U_N; u++) {
				wraps[u] += (st >> u) & 1U;
			}
		}
	}
	for (uint32_t u = 0; u < U_N; u++) {
		pcnt_ll_stop_count(hw, u);
	}
	const uint32_t t1 = k_cycle_get_32();
	uint32_t st = pcnt_ll_get_intr_status(hw);

	for (uint32_t u = 0; u < U_N; u++) {
		wraps[u] += (st >> u) & 1U;
		edges[u] = (uint64_t)wraps[u] * (uint32_t)PCNT_LIMIT +
			   (uint32_t)pcnt_ll_get_count(hw, u);
	}
	pcnt_ll_clear_intr_status(hw, all);
	pcnt_ll_enable_intr(hw, all, false);
	irq_unlock(key);
	return t1 - t0;
}

/* Both pads in one register read: how many of n snapshots had D == DN, and D high. */
static void pad_samples(uint32_t n, uint32_t *same, uint32_t *d_high)
{
	*same = 0;
	*d_high = 0;
	for (uint32_t i = 0; i < n; i++) {
		const uint32_t in = GPIO.in;
		const uint32_t d = (in >> PAD_D) & 1U, dn = (in >> PAD_DN) & 1U;

		*same += d == dn;
		*d_high += d;
		k_busy_wait(1);
	}
}

static void i2s_setup(uint32_t bck_div, uint32_t slot_bits, uint32_t clk_mhz)
{
	i2s_dev_t *hw = I2S_LL_GET_HW(0);
	hal_utils_clk_div_t mclk = {.integer = MCLK_DIV};
	i2s_clock_src_t src = clk_mhz == 240 ? I2S_CLK_SRC_PLL_240M :
			      clk_mhz == 40 ? I2S_CLK_SRC_XTAL : I2S_CLK_SRC_PLL_160M;

	PERIPH_RCC_ATOMIC() {
		i2s_ll_enable_bus_clock(0, true);
		i2s_ll_reset_register(0);
		i2s_ll_enable_core_clock(hw, true);
		pcnt_ll_enable_bus_clock(0, true);
		pcnt_ll_reset_register(0);
	}
	i2s_ll_tx_enable_clock(hw);
	i2s_ll_mclk_bind_to_tx_clk(hw);
	i2s_ll_tx_clk_set_src(hw, src);
	i2s_ll_tx_set_mclk(hw, &mclk);
	i2s_ll_tx_set_bck_div_num(hw, bck_div);

	i2s_ll_tx_reset(hw);
	i2s_ll_tx_set_slave_mod(hw, false);
	i2s_ll_tx_enable_std(hw);
	i2s_ll_tx_set_sample_bit(hw, slot_bits, slot_bits);
	i2s_ll_tx_enable_msb_shift(hw, false);
	i2s_ll_tx_set_ws_width(hw, slot_bits);
	i2s_ll_tx_set_half_sample_bit(hw, slot_bits);
	i2s_ll_tx_set_ws_idle_pol(hw, false);
	i2s_ll_tx_set_bit_order(hw, false);
	i2s_ll_tx_enable_left_align(hw, true);
	i2s_ll_tx_enable_big_endian(hw, false);
	i2s_ll_tx_enable_mono_mode(hw, false);
	i2s_ll_tx_select_std_slot(hw, I2S_STD_SLOT_BOTH);
	i2s_ll_tx_set_skip_mask(hw, false);
	i2s_ll_tx_reset_fifo(hw);
}

static void ring_start(uint32_t pattern)
{
	gdma_dev_t *g = gdma();

	for (uint32_t i = 0; i < RING_WORDS; i++) {
		ring_buf[i] = pattern;
	}
	ring_desc.dw0.size = sizeof(ring_buf);
	ring_desc.dw0.length = sizeof(ring_buf);
	ring_desc.dw0.err_eof = 0;
	ring_desc.dw0.suc_eof = 0;
	ring_desc.dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
	ring_desc.buffer = ring_buf;
	ring_desc.next = &ring_desc;
	barrier_dmem_fence_full();

	gdma_ll_tx_reset_channel(g, GDMA_CH);
	gdma_ll_tx_enable_owner_check(g, GDMA_CH, false);
	gdma_ll_tx_enable_auto_write_back(g, GDMA_CH, false);
	gdma_ll_tx_enable_data_burst(g, GDMA_CH, false);
	gdma_ll_tx_enable_descriptor_burst(g, GDMA_CH, false);
	gdma_ll_tx_connect_to_periph(g, GDMA_CH, SOC_GDMA_TRIG_PERIPH_I2S0);
	gdma_ll_tx_set_desc_addr(g, GDMA_CH, (uint32_t)&ring_desc);
	gdma_ll_tx_start(g, GDMA_CH);
	i2s_ll_tx_start(I2S_LL_GET_HW(0));
}

static void ring_stop(void)
{
	i2s_ll_tx_stop(I2S_LL_GET_HW(0));
	gdma_ll_tx_stop(gdma(), GDMA_CH);
	gdma_ll_tx_disconnect_all(gdma(), GDMA_CH);
}

/* Rising edges in the cyclic 32-bit word. */
static uint32_t rising(uint32_t w)
{
	return POPCOUNT(~w & ((w << 1) | (w >> 31)));
}

static uint32_t per_s(uint64_t edges, uint32_t cycles)
{
	return (uint32_t)(edges * sys_clock_hw_cycles_per_sec() / cycles);
}

int i2sbench_run(const struct shell *sh, uint32_t bck_div, uint32_t pattern, uint32_t slot_bits,
		 uint32_t clk_mhz)
{
	i2s_dev_t *hw = I2S_LL_GET_HW(0);
	uint64_t e[U_N];
	uint32_t same, high, same1, high1, same0, high0, cyc;

	if (bck_div < 1U || bck_div > 64U || slot_bits < 1U || slot_bits > 32U ||
	    (clk_mhz != 160U && clk_mhz != 240U && clk_mhz != 40U)) {
		return -EINVAL;
	}
	const uint32_t src_hz = clk_mhz * 1000000U;
	const uint32_t bit_hz = src_hz / (MCLK_DIV * bck_div);

	gdma_up();
	i2s_setup(bck_div, slot_bits, clk_mhz);
	pad_out(PAD_D, false);
	pad_out(PAD_DN, true);
	pcnt_unit(U_D, PAD_D, 0, false);
	pcnt_unit(U_D_OVER, PAD_D, PAD_DN, true);
	pcnt_unit(U_DN_OVER, PAD_DN, PAD_D, true);
	pcnt_unit(U_DN, PAD_DN, 0, false);

	/* Held levels: all ones then all zeros, D and DN must swap. */
	ring_start(0xFFFFFFFFU);
	k_busy_wait(SETTLE_US);
	pad_samples(200, &same1, &high1);
	ring_stop();
	i2s_ll_tx_reset(hw);
	i2s_ll_tx_reset_fifo(hw);
	ring_start(0U);
	k_busy_wait(SETTLE_US);
	pad_samples(200, &same0, &high0);
	ring_stop();
	i2s_ll_tx_reset(hw);
	i2s_ll_tx_reset_fifo(hw);
	const bool held_ok = same1 == 0U && high1 == 200U && same0 == 0U && high0 == 0U;

	/* The pattern: edges on both pads, the overlap counts, then levels. */
	ring_start(pattern);
	k_busy_wait(SETTLE_US);
	cyc = pcnt_gate(e);
	pad_samples(SAMPLES, &same, &high);
	const uint32_t cdiv = hw->tx_conf1.tx_bck_div_num;
	const uint32_t regs[] = {hw->tx_clkm_conf.val, hw->tx_clkm_div_conf.val, hw->tx_conf1.val,
				 hw->tx_conf.val};
	ring_stop();

	const uint32_t rd = per_s(e[U_D], cyc), rdn = per_s(e[U_DN], cyc);
	const uint32_t rise = rising(pattern);
	const uint32_t exp = (uint32_t)((uint64_t)bit_hz * rise / 32U);
	const uint32_t meas_bit = rise != 0U ? (uint32_t)((uint64_t)rd * 32U / rise) : 0U;
	const uint32_t over_pm = e[U_D] != 0U ? (uint32_t)(e[U_D_OVER] * 1000U / e[U_D]) : 0U;
	const uint32_t over2_pm = e[U_DN] != 0U ? (uint32_t)(e[U_DN_OVER] * 1000U / e[U_DN]) : 0U;
	const uint32_t same_pm = same * 1000U / SAMPLES;
	/*
	 * PCNT samples the other pad as an edge arrives: for a complementary pair it is still at its
	 * old level then (the inverse falls as D rises), so nearly every edge counts.
	 */
	const bool comp = held_ok && same_pm < 10U && over_pm > 990U && over2_pm > 990U;

	shell_print(sh, "i2s0 tx src %u MHz mclk_div %u bck_div %u slot %u bits pattern 0x%08x",
		    clk_mhz, MCLK_DIV, bck_div, slot_bits, pattern);
	shell_print(sh, "configured bit rate %u Hz, expected rising edges/s %u (%u per word)",
		    bit_hz, exp, rise);
	shell_print(sh, "measured rising edges/s D(GPIO%d) %u DN(GPIO%d) %u, gate %u cycles",
		    PAD_D, rd, PAD_DN, rdn, cyc);
	shell_print(sh, "measured bit rate %u Hz (rising edges/s * 32 / %u)", meas_bit, rise);
	shell_print(sh, "held 1: D==DN in %u of 200, D high %u; held 0: D==DN in %u of 200, D high %u",
		    same1, high1, same0, high0);
	shell_print(sh, "pattern: D==DN in %u of %u samples, D high %u; overlap per mille D-edges "
		    "with DN high %u, DN-edges with D high %u (complementary: near 1000)",
		    same, SAMPLES, high, over_pm, over2_pm);
	shell_print(sh, "complementary: %s", comp ? "yes" : "no");
	shell_print(sh, "regs tx_bck_div_num read back %u (bck_div %u written as %u), "
		    "tx_clkm_conf 0x%08x clkm_div_conf 0x%08x tx_conf1 0x%08x tx_conf 0x%08x",
		    cdiv, bck_div, bck_div - 1U, regs[0], regs[1], regs[2], regs[3]);

	pad_free(PAD_D);
	pad_free(PAD_DN);
	for (uint32_t u = 0; u < U_N; u++) {
		gpio_ll_set_input_signal_matrix_source(&GPIO, SRC_IDX_CH0(u),
						       GPIO_MATRIX_CONST_ZERO_INPUT, false);
		gpio_ll_set_input_signal_matrix_source(&GPIO, CTRL_IDX_CH0(u),
						       GPIO_MATRIX_CONST_ZERO_INPUT, false);
	}
	return 0;
}
