/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Shell for the MCH2022 badge's FPGA (bitstream from CONFIG_ESDR_FPGA_IMAGE): reset, registers,
 * an SPI round-trip test.
 */

#include <math.h>
#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <esp_sdr/esdr_fpga.h>
#include <esp_sdr/esdr_fpga_regs.h>

static const struct device *const fpga = DEVICE_DT_GET(DT_NODELABEL(fpga));

static int cmd_off(const struct shell *sh, size_t argc, char **argv)
{
	int ret = esdr_fpga_off(fpga);

	shell_print(sh, "off: %d", ret);
	return ret;
}

static int cmd_reg(const struct shell *sh, size_t argc, char **argv)
{
	uint16_t addr = strtoul(argv[1], NULL, 0);
	uint16_t w;
	int ret;

	if (argc > 2) {
		w = strtoul(argv[2], NULL, 0);
		ret = esdr_fpga_reg_write(fpga, addr, &w, 1);
	} else {
		ret = esdr_fpga_reg_read(fpga, addr, &w, 1);
		shell_print(sh, "0x%04x: 0x%04x", addr, w);
	}
	return ret;
}

static int cmd_rate(const struct shell *sh, size_t argc, char **argv)
{
	uint16_t addr = strtoul(argv[1], NULL, 0);
	uint32_t ms = argc > 2 ? strtoul(argv[2], NULL, 0) : 1000;
	uint16_t a, b;
	int64_t t0, t1;

	t0 = k_uptime_get();
	esdr_fpga_reg_read(fpga, addr, &a, 1);
	k_msleep(ms);
	t1 = k_uptime_get();
	esdr_fpga_reg_read(fpga, addr, &b, 1);
	shell_print(sh, "0x%04x: %u in %lld ms, %u/s", addr, (uint16_t)(b - a), t1 - t0,
		    (uint32_t)((uint16_t)(b - a) * 1000LL / (t1 - t0)));
	return 0;
}

/* The gateware's scratch RAM (gateware/esdr_fpga/core.py) and SCRATCH register. */
#define SCRATCH_RAM ESDR_FPGA_SCRATCH_RAM
#define SCRATCH_REG ESDR_FPGA_REG_SCRATCH

static uint16_t tx_words[256], rx_words[256];

static int cmd_test(const struct shell *sh, size_t argc, char **argv)
{
	unsigned int rounds = argc > 1 ? strtoul(argv[1], NULL, 0) : 1000;
	uint32_t hz = argc > 2 ? strtoul(argv[2], NULL, 0) : 0;
	unsigned int bad = 0, words = 0;
	uint32_t seed = k_cycle_get_32();
	int64_t start = k_uptime_get();

	if (hz) {
		esdr_fpga_set_read_frequency(fpga, hz);
	}
	for (unsigned int r = 0; r < rounds; r++) {
		size_t n = 1 + (seed % 256);

		for (size_t i = 0; i < 256; i++) {
			seed = seed * 1664525U + 1013904223U;
			tx_words[i] = seed >> 16;
		}
		if (esdr_fpga_reg_write(fpga, SCRATCH_RAM, tx_words, n) ||
		    esdr_fpga_reg_read(fpga, SCRATCH_RAM, rx_words, n) ||
		    esdr_fpga_reg_write(fpga, SCRATCH_REG, &tx_words[0], 1) ||
		    esdr_fpga_reg_read(fpga, SCRATCH_REG, &rx_words[n], 1)) {
			shell_error(sh, "bus error in round %u", r);
			return -EIO;
		}
		for (size_t i = 0; i < n; i++) {
			bad += rx_words[i] != tx_words[i];
		}
		bad += rx_words[n] != tx_words[0];
		words += n + 1;
	}
	shell_print(sh, "test: %u rounds, %u words, %u bad, read %u Hz, %lld ms", rounds, words,
		    bad, hz, k_uptime_get() - start);
	return bad ? -EIO : 0;
}

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
	return ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
}

/* The eggs' waterfall colours (apps/micropython _gui.py), spread over 248 entries. */
static void palette(uint16_t *pal)
{
	static const uint8_t stops[][3] = {
		{0, 0, 0}, {0, 0, 200}, {0, 200, 255}, {255, 255, 0}, {255, 0, 0}, {255, 255, 255},
	};
	const int n = ESDR_FPGA_PAL_BG;
	const int segs = ARRAY_SIZE(stops) - 1;

	for (int i = 0; i < n; i++) {
		int pos = i * segs * 256 / (n - 1);
		int s = MIN(pos / 256, segs - 1);
		int f = pos - s * 256;
		uint8_t c[3];

		for (int k = 0; k < 3; k++) {
			c[k] = (stops[s][k] * (256 - f) + stops[s + 1][k] * f) / 256;
		}
		pal[i] = rgb565(c[0], c[1], c[2]);
	}
	pal[ESDR_FPGA_PAL_BG] = rgb565(0, 0, 0);
	pal[ESDR_FPGA_PAL_TRACE] = rgb565(255, 255, 0);
	pal[ESDR_FPGA_PAL_FILL] = rgb565(0, 40, 90);
	pal[ESDR_FPGA_PAL_GRID] = rgb565(60, 60, 60);
	pal[ESDR_FPGA_PAL_CENTER] = rgb565(200, 0, 0);
	for (int i = ESDR_FPGA_PAL_CENTER + 1; i < 256; i++) {
		pal[i] = rgb565(255, 0, 255);
	}
}

#define GFX_W  320
#define SP_H   80
#define WF_H   160

static uint16_t pal[256];
static uint8_t line[GFX_W], rows[GFX_W];

static int cmd_gfx(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	uint32_t secs = argc > 1 ? strtoul(argv[1], NULL, 0) : 10;
	bool te = argc > 2 ? strtoul(argv[2], NULL, 0) : 1;
	const struct esdr_fpga_trace trace = {
		.flags = ESDR_FPGA_SP_FILL | ESDR_FPGA_SP_HGRID | ESDR_FPGA_SP_VGRID |
			 ESDR_FPGA_SP_CENTER,
		.grid_y0 = 0, .grid_dy = 20, .grid_dx = 40,
	};
	uint32_t seed = 1, n = 0;
	uint64_t cpu = 0;
	uint16_t r0, r1;
	int64_t t0, end;
	int ret;

	display_blanking_off(disp);
	palette(pal);
	ret = esdr_fpga_palette(fpga, 0, pal, 256);
	ret = ret ?: esdr_fpga_spectrum(fpga, 0, 0, GFX_W, SP_H, &trace);
	ret = ret ?: esdr_fpga_waterfall(fpga, 0, SP_H, GFX_W, WF_H);
	ret = ret ?: esdr_fpga_render(fpga, true, te, true);
	if (ret) {
		shell_error(sh, "setup: %d", ret);
		return ret;
	}
	esdr_fpga_reg_read(fpga, ESDR_FPGA_REG_RENDERS, &r0, 1);
	t0 = k_uptime_get();
	end = t0 + secs * 1000;
	while (k_uptime_get() < end) {
		uint32_t c0 = k_cycle_get_32();
		/* A floor of noise, a tone drifting across, a fixed carrier: levels 0..1. */
		float tone = GFX_W / 2 + (GFX_W / 2 - 20) * sinf(n * 0.02f);

		for (int x = 0; x < GFX_W; x++) {
			float v;

			seed = seed * 1664525U + 1013904223U;
			v = 0.15f + 0.1f * (seed >> 24) / 255.0f;
			v += 0.7f * expf(-(x - tone) * (x - tone) / 8.0f);
			v += 0.5f * expf(-(x - 80) * (x - 80) / 2.0f);
			v = MIN(v, 1.0f);
			line[x] = (uint8_t)(v * (ESDR_FPGA_PAL_BG - 1));
			rows[x] = (uint8_t)((1.0f - v) * (SP_H - 1));
		}
		esdr_fpga_columns(fpga, rows, GFX_W);
		esdr_fpga_wf_line(fpga, line, GFX_W);
		cpu += k_cycle_get_32() - c0;
		n++;
		k_msleep(5);
	}
	esdr_fpga_reg_read(fpga, ESDR_FPGA_REG_RENDERS, &r1, 1);
	shell_print(sh, "gfx: %u lines, %u renders in %u s, %llu us CPU per line", n,
		    (uint16_t)(r1 - r0), secs,
		    k_cyc_to_us_floor64(cpu / MAX(n, 1)));
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(fpga_cmds,
	SHELL_CMD_ARG(reg, NULL, "reg <addr> [value]", cmd_reg, 2, 1),
	SHELL_CMD_ARG(gfx, NULL, "gfx [seconds] [te 0/1]: spectrum and waterfall from software",
		      cmd_gfx, 1, 2),
	SHELL_CMD_ARG(rate, NULL, "rate <addr> [ms]: how fast a counter register runs", cmd_rate,
		      2, 1),
	SHELL_CMD_ARG(test, NULL, "test [rounds] [read Hz]: scratch RAM round trips", cmd_test,
		      1, 2),
	SHELL_CMD(off, NULL, "Hold the FPGA in reset", cmd_off),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(fpga, &fpga_cmds, "MCH2022 FPGA", NULL);

int main(void)
{
	printk("fpga: %s\n", !device_is_ready(fpga) ? "not ready"
			      : esdr_fpga_loaded(fpga) ? "loaded" : "not loaded");
	return 0;
}
