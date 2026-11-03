/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * WS2812 on the SAO port and the IR LED's NEC frames: the stock firmware's PIO programs, assembled
 * (the WS2812 program is Raspberry Pi's, BSD-3-Clause; the NEC ones from pico-examples, the same).
 */

#include <zephyr/device.h>
#include <zephyr/drivers/misc/pio_rpi_pico/pio_rpi_pico.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <hardware/clocks.h>
#include <hardware/pio.h>

#include "coproc.h"

LOG_MODULE_REGISTER(pio_out, LOG_LEVEL_INF);

#define WS2812_PIO pio1
#define WS2812_SM  0U
#define WS2812_PIN 18U
#define WS2812_HZ  800000U
/* Instructions a bit. */
#define WS2812_CYCLES 10U

#define IR_PIO     pio0
#define IR_BURST_SM 0U
#define IR_CTRL_SM  1U
#define IR_PIN      27U

RPI_PICO_PIO_DEFINE_PROGRAM(ws2812, 0, 3,
	/* out x, 1       side 0 [2] */
	0x6221,
	/* jmp !x do_zero side 1 [1] */
	0x1123,
	/* jmp bitloop    side 1 [4] */
	0x1400,
	/* do_zero: nop   side 0 [4] */
	0xa442);

RPI_PICO_PIO_DEFINE_PROGRAM(nec_burst, 0, 4,
	/* set x, 20 */
	0xe034,
	/* wait 1 irq 7 */
	0x20c7,
	/* cycle_loop: set pins, 1 */
	0xe001,
	/* set pins, 0 [1] */
	0xe100,
	/* jmp x--, cycle_loop */
	0x0042);

RPI_PICO_PIO_DEFINE_PROGRAM(nec_control, 0, 10,
	/* pull */
	0x80a0,
	/* set x, 15 */
	0xe02f,
	/* long_burst: irq 7 */
	0xc007,
	/* jmp x--, long_burst */
	0x0042,
	/* nop [15] */
	0xaf42,
	/* irq 7 [1] */
	0xc107,
	/* data_bit: out x, 1 */
	0x6021,
	/* jmp !x, burst */
	0x0029,
	/* nop [3] */
	0xa342,
	/* burst: irq 7 */
	0xc007,
	/* jmp !osre, data_bit */
	0x00e6);

static uint ws2812_offset;
static bool ws2812_on;

void ws2812_mode(uint8_t mode)
{
	pio_sm_config c;

	pio_sm_set_enabled(WS2812_PIO, WS2812_SM, false);
	ws2812_on = mode == 1U || mode == 2U;
	if (!ws2812_on) {
		return;
	}
	c = pio_get_default_sm_config();
	sm_config_set_wrap(&c, ws2812_offset + RPI_PICO_PIO_GET_WRAP_TARGET(ws2812),
			   ws2812_offset + RPI_PICO_PIO_GET_WRAP(ws2812));
	sm_config_set_sideset(&c, 1, false, false);
	sm_config_set_sideset_pins(&c, WS2812_PIN);
	sm_config_set_out_shift(&c, false, true, mode == 2U ? 32U : 24U);
	sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
	sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / (WS2812_HZ * WS2812_CYCLES));
	pio_gpio_init(WS2812_PIO, WS2812_PIN);
	pio_sm_set_consecutive_pindirs(WS2812_PIO, WS2812_SM, WS2812_PIN, 1, true);
	pio_sm_init(WS2812_PIO, WS2812_SM, ws2812_offset, &c);
	pio_sm_set_enabled(WS2812_PIO, WS2812_SM, true);
}

void ws2812_put(uint32_t data)
{
	/* Off, the FIFO would fill and stay full (the stock firmware hangs there). */
	if (ws2812_on) {
		pio_sm_put_blocking(WS2812_PIO, WS2812_SM, data);
	}
}

void ir_send(uint16_t address, uint16_t data)
{
	/* A frame takes some 70 ms: one at a time, the rest wait for a free FIFO place. */
	if (!pio_sm_is_tx_fifo_full(IR_PIO, IR_CTRL_SM)) {
		pio_sm_put(IR_PIO, IR_CTRL_SM, address | ((uint32_t)data << 16));
	}
}

int pio_out_init(void)
{
	uint burst = pio_add_program(IR_PIO, RPI_PICO_PIO_GET_PROGRAM(nec_burst));
	uint ctrl = pio_add_program(IR_PIO, RPI_PICO_PIO_GET_PROGRAM(nec_control));
	const float hz = (float)clock_get_hz(clk_sys);
	pio_sm_config c;

	ws2812_offset = pio_add_program(WS2812_PIO, RPI_PICO_PIO_GET_PROGRAM(ws2812));

	/* The 38.222 kHz carrier, 4 instructions a cycle, 21 cycles to a burst. */
	c = pio_get_default_sm_config();
	sm_config_set_wrap(&c, burst + RPI_PICO_PIO_GET_WRAP_TARGET(nec_burst),
			   burst + RPI_PICO_PIO_GET_WRAP(nec_burst));
	sm_config_set_set_pins(&c, IR_PIN, 1);
	sm_config_set_clkdiv(&c, hz / (38222.0f * 4.0f));
	pio_gpio_init(IR_PIO, IR_PIN);
	pio_sm_set_consecutive_pindirs(IR_PIO, IR_BURST_SM, IR_PIN, 1, true);
	pio_sm_init(IR_PIO, IR_BURST_SM, burst, &c);
	pio_sm_set_enabled(IR_PIO, IR_BURST_SM, true);

	/* Two ticks per burst of 562.5 us. */
	c = pio_get_default_sm_config();
	sm_config_set_wrap(&c, ctrl + RPI_PICO_PIO_GET_WRAP_TARGET(nec_control),
			   ctrl + RPI_PICO_PIO_GET_WRAP(nec_control));
	sm_config_set_out_shift(&c, true, false, 32);
	sm_config_set_clkdiv(&c, hz / (2.0f / 562.5e-6f));
	pio_sm_init(IR_PIO, IR_CTRL_SM, ctrl, &c);
	pio_sm_set_enabled(IR_PIO, IR_CTRL_SM, true);
	return 0;
}
