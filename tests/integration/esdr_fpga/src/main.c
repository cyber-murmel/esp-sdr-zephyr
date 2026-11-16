/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The gateware on the MCH2022 badge: the bitstream loaded at boot, the SPI link, the panel
 * behind the FPGA, and the DSP chain against a tone made here.
 */

#include <math.h>
#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>
#include <zephyr/ztest.h>

#include <esp_sdr/esdr_fpga.h>
#include <esp_sdr/esdr_fpga_regs.h>
#include <esp_sdr/esp_sdr_dsp.h>

static const struct device *const fpga = DEVICE_DT_GET(DT_NODELABEL(fpga));
static const struct device *const disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

static uint16_t reg(uint16_t addr)
{
	uint16_t v = 0;

	zassert_ok(esdr_fpga_reg_read(fpga, addr, &v, 1));
	return v;
}

ZTEST(esdr_fpga, test_loaded)
{
	zassert_true(device_is_ready(fpga));
	zassert_true(esdr_fpga_loaded(fpga), "no bitstream (ESDR_FPGA_IMAGE)");
	zassert_equal(reg(ESDR_FPGA_REG_ID), ESDR_FPGA_ID);
	/* The FPGA has the panel: LCD_MODE high. */
	zassert_true(reg(ESDR_FPGA_REG_STATUS) & BIT(0));
}

/* The load ran on the system work queue, through both I2C and SPI. */
ZTEST(esdr_fpga, test_load_stack)
{
	size_t unused = 0;

	zassert_ok(k_thread_stack_space_get(k_work_queue_thread_get(&k_sys_work_q), &unused));
	printk("system work queue: %zu of %d bytes unused\n", unused,
	       CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE);
	zassert_true(unused >= CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE / 5);
}

static uint16_t tx[256], rx[256];

ZTEST(esdr_fpga, test_link)
{
	for (int r = 0; r < 500; r++) {
		size_t n = 1 + sys_rand32_get() % 256;

		sys_rand_get(tx, sizeof(tx));
		zassert_ok(esdr_fpga_reg_write(fpga, ESDR_FPGA_SCRATCH_RAM, tx, n));
		zassert_ok(esdr_fpga_reg_read(fpga, ESDR_FPGA_SCRATCH_RAM, rx, n));
		zassert_mem_equal(tx, rx, n * 2, "round %d, %u words", r, (unsigned int)n);
	}
}

ZTEST(esdr_fpga, test_panel)
{
	static uint16_t px[32 * 32];
	struct display_buffer_descriptor desc = {
		.buf_size = sizeof(px), .width = 32, .height = 32, .pitch = 32,
	};
	uint16_t b0, t0;

	zassert_ok(display_blanking_off(disp));
	k_msleep(2);
	b0 = reg(ESDR_FPGA_REG_DBI_BYTES);
	t0 = reg(ESDR_FPGA_REG_TE_CNT);
	zassert_ok(display_write(disp, 10, 10, &desc, px));
	/* The FIFO drains at 15 MB/s. CASET, PASET (5 bytes each), RAMWR, the pixels. */
	k_msleep(2);
	uint16_t bytes = reg(ESDR_FPGA_REG_DBI_BYTES) - b0;

	zassert_equal(bytes, 11 + sizeof(px), "%u bytes on the bus", bytes);
	k_msleep(1000);
	uint16_t te = reg(ESDR_FPGA_REG_TE_CNT) - t0;

	zassert_true(te > 60 && te < 120, "TE %u per second", te);
}

static int8_t iq[2 * 2048] __aligned(4);
static int16_t levels[320];

ZTEST(esdr_fpga, test_dsp_tone)
{
	const struct esdr_fpga_dsp dsp = {
		.log2n = 9, .window = ESP_SDR_WINDOW_BLACKMAN_HARRIS, .alpha = 1.0f,
		.frames_per_line = 2, .cols = 320,
		.wf_min_db = -90.0f, .wf_max_db = 0.0f,
		.tr_top_db = 0.0f, .tr_bottom_db = -100.0f, .tr_rows = 80,
	};
	uint16_t l0;
	int peak = 0;

	/* A full-scale tone at +fs/8 (IQ8 full scale is 127.5 of 128). */
	for (int k = 0; k < 2048; k++) {
		iq[2 * k] = (int8_t)lroundf(127.0f * cosf(ESP_SDR_TAU_F * k / 8.0f));
		iq[2 * k + 1] = (int8_t)lroundf(127.0f * sinf(ESP_SDR_TAU_F * k / 8.0f));
	}
	zassert_ok(esdr_fpga_dsp_config(fpga, &dsp));
	zassert_ok(esdr_fpga_dsp_run(fpga, true, &dsp));
	l0 = reg(ESDR_FPGA_REG_LINES);
	zassert_ok(esdr_fpga_samples(fpga, ESDR_FPGA_FMT_IQ8, true, iq, sizeof(iq)));
	k_msleep(20);
	zassert_true((uint16_t)(reg(ESDR_FPGA_REG_LINES) - l0) >= 1);
	zassert_ok(esdr_fpga_dsp_columns(fpga, levels, 320));
	for (int c = 1; c < 320; c++) {
		if (levels[c] > levels[peak]) {
			peak = c;
		}
	}
	zassert_equal(peak, 160 + 40, "peak in column %d", peak);
	float db = esdr_fpga_l_to_db(levels[peak]);

	zassert_true(fabsf(db) < 0.5f, "peak %d.%d dB", (int)db, abs((int)(db * 10)) % 10);
	/* Far from the tone only the 8-bit quantisation is left. */
	float far = esdr_fpga_l_to_db(levels[20]);

	TC_PRINT("tone %d.%d dB, column 20 at %d dB\n", (int)db, abs((int)(db * 10)) % 10,
		 (int)far);
	zassert_true(far < -60.0f, "column 20 at %d dB", (int)far);
	zassert_ok(esdr_fpga_dsp_run(fpga, false, &dsp));
}

ZTEST_SUITE(esdr_fpga, NULL, NULL, NULL, NULL, NULL);
