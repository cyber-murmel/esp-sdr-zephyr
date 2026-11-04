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
#include <zephyr/drivers/fpga.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <esp_sdr/esdr_fpga.h>
#include <esp_sdr/esdr_fpga_regs.h>
#include <esp_sdr/esp_sdr_dsp.h>
#ifdef CONFIG_ESP_SDR
#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_rx.h>
#endif

static const struct device *const fpga = DEVICE_DT_GET(DT_NODELABEL(fpga));
static const struct device *const ice40 = DEVICE_DT_GET(DT_NODELABEL(ice40));

static int cmd_off(const struct shell *sh, size_t argc, char **argv)
{
	int ret = fpga_off(ice40);

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
	const struct esdr_fpga_trace trace = {.flags = ESDR_FPGA_SP_CENTER};
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

#define DSP_CHUNK 1024

static int8_t iq[2 * DSP_CHUNK] __aligned(4);
static int16_t levels[GFX_W];

/*
 * The DSP chain on samples made here: a full-scale tone sweeping in a sine, a fixed one 30 dB
 * down at +fs/4, noise. fpga dsp [seconds] [log2n] [frames per line] [repeat]: each chunk goes
 * out `repeat` times (to measure the gateware rather than the synthesis here).
 */
static int cmd_dsp(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	uint32_t secs = argc > 1 ? strtoul(argv[1], NULL, 0) : 10;
	uint8_t log2n = argc > 2 ? strtoul(argv[2], NULL, 0) : 9;
	uint8_t fpl = argc > 3 ? strtoul(argv[3], NULL, 0) : 4;
	uint32_t repeat = argc > 4 ? MAX(strtoul(argv[4], NULL, 0), 1) : 1;
	const struct esdr_fpga_trace trace = {.flags = ESDR_FPGA_SP_CENTER};
	const struct esdr_fpga_dsp dsp = {
		.log2n = log2n, .window = ESP_SDR_WINDOW_BLACKMAN_HARRIS, .alpha = 0.5f,
		.frames_per_line = fpl, .cols = GFX_W,
		.wf_min_db = -90.0f, .wf_max_db = -10.0f,
		.tr_top_db = 0.0f, .tr_bottom_db = -100.0f, .tr_rows = SP_H,
	};
	uint16_t f0, f1, l0, l1, ovr;
	uint32_t seed = 1, sent = 0;
	float ph = 0.0f, ph2 = 0.0f, sweep = 0.0f;
	uint64_t cpu = 0;
	int64_t t0, end;
	int ret;

	display_blanking_off(disp);
	palette(pal);
	ret = esdr_fpga_palette(fpga, 0, pal, 256);
	ret = ret ?: esdr_fpga_spectrum(fpga, 0, 0, GFX_W, SP_H, &trace);
	ret = ret ?: esdr_fpga_waterfall(fpga, 0, SP_H, GFX_W, WF_H);
	ret = ret ?: esdr_fpga_dsp_config(fpga, &dsp);
	ret = ret ?: esdr_fpga_render(fpga, true, true, true);
	ret = ret ?: esdr_fpga_dsp_run(fpga, true, &dsp);
	if (ret) {
		shell_error(sh, "setup: %d", ret);
		return ret;
	}
	esdr_fpga_reg_read(fpga, ESDR_FPGA_REG_FRAMES, &f0, 1);
	esdr_fpga_reg_read(fpga, ESDR_FPGA_REG_LINES, &l0, 1);
	t0 = k_uptime_get();
	end = t0 + secs * 1000;
	while (k_uptime_get() < end) {
		uint32_t c0 = k_cycle_get_32();
		float step = ESP_SDR_TAU_F * 0.35f * sinf(sweep);

		for (int k = 0; k < DSP_CHUNK; k++) {
			float i = 120.0f * cosf(ph) + 3.8f * cosf(ph2);
			float q = 120.0f * sinf(ph) + 3.8f * sinf(ph2);

			seed = seed * 1664525U + 1013904223U;
			i += (int8_t)(seed >> 24) / 64.0f;
			q += (int8_t)(seed >> 16) / 64.0f;
			iq[2 * k] = (int8_t)CLAMP(lroundf(i), -128, 127);
			iq[2 * k + 1] = (int8_t)CLAMP(lroundf(q), -128, 127);
			ph += step;
			ph2 += ESP_SDR_TAU_F / 4.0f;
		}
		ph = fmodf(ph, ESP_SDR_TAU_F);
		ph2 = fmodf(ph2, ESP_SDR_TAU_F);
		sweep += 0.01f;
		for (uint32_t r = 0; r < repeat; r++) {
			esdr_fpga_samples(fpga, ESDR_FPGA_FMT_IQ8, false, iq, sizeof(iq));
		}
		cpu += k_cycle_get_32() - c0;
		sent += DSP_CHUNK * repeat;
	}
	esdr_fpga_reg_read(fpga, ESDR_FPGA_REG_FRAMES, &f1, 1);
	esdr_fpga_reg_read(fpga, ESDR_FPGA_REG_LINES, &l1, 1);
	esdr_fpga_reg_read(fpga, ESDR_FPGA_REG_OVERRUNS, &ovr, 1);
	shell_print(sh, "dsp: N %u, %u samples, %u frames, %u lines in %u s, %u overruns, "
		    "%llu us CPU per %u samples", 1U << log2n, sent, (uint16_t)(f1 - f0),
		    (uint16_t)(l1 - l0), secs, ovr, k_cyc_to_us_floor64(cpu * DSP_CHUNK / sent),
		    DSP_CHUNK);

	/* A fixed tone now: where is it, how strong? */
	for (int k = 0; k < DSP_CHUNK; k++) {
		iq[2 * k] = (int8_t)lroundf(127.0f * cosf(ESP_SDR_TAU_F * k / 8.0f));
		iq[2 * k + 1] = (int8_t)lroundf(127.0f * sinf(ESP_SDR_TAU_F * k / 8.0f));
	}
	for (int r = 0; r < 4 * fpl; r++) {
		esdr_fpga_samples(fpga, ESDR_FPGA_FMT_IQ8, r == 0, iq, sizeof(iq));
	}
	k_msleep(50);
	esdr_fpga_dsp_columns(fpga, levels, GFX_W);
	int peak = 0;

	for (int c = 1; c < GFX_W; c++) {
		if (levels[c] > levels[peak]) {
			peak = c;
		}
	}
	shell_print(sh, "dsp: tone at +fs/8: column %d of %d (want %d), %d.%01d dB", peak, GFX_W,
		    GFX_W / 2 + GFX_W / 8, (int)esdr_fpga_l_to_db(levels[peak]),
		    abs((int)(10.0f * esdr_fpga_l_to_db(levels[peak]))) % 10);
	return 0;
}

#ifdef CONFIG_ESP_SDR
#define BURST 16380

static bool radio_up;

static int radio_init(const struct shell *sh, uint32_t mhz)
{
	int ret = 0;

	if (!radio_up) {
		ret = esp_sdr_init();
		radio_up = ret == 0;
	}
	ret = ret ?: esp_sdr_set_freq(mhz);
	if (ret) {
		shell_error(sh, "radio: %d", ret);
	}
	return ret;
}

/* ESDR_FPGA_FMT_IQ8 in place: each word's int8 pair, I in the low byte. */
static size_t pack_iq8(const uint32_t *words, size_t count, uint8_t *out)
{
	for (size_t j = 0; j < count; j++) {
		uint32_t iq = esp_sdr_rx_iq8(words[j]);

		out[2 * j] = (uint8_t)iq;
		out[2 * j + 1] = (uint8_t)(iq >> 8);
	}
	return 2 * count;
}

/*
 * ESDR_FPGA_FMT_IQ10P in place: the words' 20-bit I/Q fields little endian, two samples in
 * five bytes (a pair is read before its bytes are written).
 */
static size_t pack_iq10(const uint32_t *words, size_t count, uint8_t *out)
{
	uint8_t *p = out;

	for (size_t j = 0; j < count; j += 2, p += 5) {
		uint32_t a = words[j] & GENMASK(19, 0);
		uint32_t b = j + 1 < count ? words[j + 1] & GENMASK(19, 0) : 0U;

		p[0] = (uint8_t)a;
		p[1] = (uint8_t)(a >> 8);
		p[2] = (uint8_t)((a >> 16) | (b << 4));
		if (j + 1 < count) {
			p[3] = (uint8_t)(b >> 4);
			p[4] = (uint8_t)(b >> 12);
		}
	}
	return (count * 20U + 7U) / 8U;
}

/*
 * The badge's radio into the FPGA: fpga radio <MHz> [seconds] [rate: 0 80, 1 40, 6 16 MS/s]
 * [format: 0 dump words, 1 packed 10 bit, 2 8 bit]. Each burst starts new frames.
 */
static int cmd_radio(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	uint32_t mhz = strtoul(argv[1], NULL, 0);
	uint32_t secs = argc > 2 ? strtoul(argv[2], NULL, 0) : 10;
	enum esp_sdr_rate rate = argc > 3 ? strtoul(argv[3], NULL, 0) : ESP_SDR_RATE_40MSPS;
	uint8_t fmt = argc > 4 ? strtoul(argv[4], NULL, 0) : ESDR_FPGA_FMT_WORD32;
	const struct esdr_fpga_trace trace = {.flags = ESDR_FPGA_SP_CENTER};
	const struct esdr_fpga_dsp dsp = {
		.log2n = 9, .window = ESP_SDR_WINDOW_BLACKMAN_HARRIS, .alpha = 0.25f,
		.frames_per_line = 8, .cols = GFX_W,
		.wf_min_db = -95.0f, .wf_max_db = -35.0f,
		.tr_top_db = -10.0f, .tr_bottom_db = -110.0f, .tr_rows = SP_H,
	};
	struct esp_sdr_rx_burst b;
	uint64_t t_cap = 0, t_pack = 0, t_send = 0;
	uint16_t f0, f1, l0, l1;
	uint32_t bursts = 0;
	int64_t t0, end;
	int ret;

	ret = radio_init(sh, mhz);
	if (ret) {
		return ret;
	}
	display_blanking_off(disp);
	palette(pal);
	ret = esdr_fpga_palette(fpga, 0, pal, 256);
	ret = ret ?: esdr_fpga_spectrum(fpga, 0, 0, GFX_W, SP_H, &trace);
	ret = ret ?: esdr_fpga_waterfall(fpga, 0, SP_H, GFX_W, WF_H);
	ret = ret ?: esdr_fpga_dsp_config(fpga, &dsp);
	ret = ret ?: esdr_fpga_render(fpga, true, true, true);
	ret = ret ?: esdr_fpga_dsp_run(fpga, true, &dsp);
	if (ret) {
		shell_error(sh, "setup: %d", ret);
		return ret;
	}
	esdr_fpga_reg_read(fpga, ESDR_FPGA_REG_FRAMES, &f0, 1);
	esdr_fpga_reg_read(fpga, ESDR_FPGA_REG_LINES, &l0, 1);
	t0 = k_uptime_get();
	end = t0 + secs * 1000;
	while (k_uptime_get() < end) {
		uint32_t c0 = k_cycle_get_32(), c1, c2;
		size_t len;

		ret = esp_sdr_rx_capture(rate, BURST, &b);
		if (ret) {
			shell_error(sh, "capture: %d", ret);
			break;
		}
		c1 = k_cycle_get_32();
		/* Packed in place: the bank is DMA-capable memory. */
		if (fmt == ESDR_FPGA_FMT_IQ10P) {
			len = pack_iq10(b.words, b.count, (uint8_t *)b.words);
		} else if (fmt == ESDR_FPGA_FMT_IQ8) {
			len = pack_iq8(b.words, b.count, (uint8_t *)b.words);
		} else {
			len = b.count * 4;
		}
		c2 = k_cycle_get_32();
		ret = esdr_fpga_samples(fpga, fmt, true, b.words, len);
		if (ret) {
			shell_error(sh, "samples: %d", ret);
			break;
		}
		t_cap += c1 - c0;
		t_pack += c2 - c1;
		t_send += k_cycle_get_32() - c2;
		bursts++;
	}
	int64_t ms = k_uptime_get() - t0;

	esdr_fpga_reg_read(fpga, ESDR_FPGA_REG_FRAMES, &f1, 1);
	esdr_fpga_reg_read(fpga, ESDR_FPGA_REG_LINES, &l1, 1);
	shell_print(sh, "radio: %u MHz, %u bursts of %u in %lld ms, %u frames, %u lines; per "
		    "burst: capture %llu us, pack %llu us, send %llu us", mhz, bursts, BURST, ms,
		    (uint16_t)(f1 - f0), (uint16_t)(l1 - l0),
		    k_cyc_to_us_floor64(t_cap / MAX(bursts, 1)),
		    k_cyc_to_us_floor64(t_pack / MAX(bursts, 1)),
		    k_cyc_to_us_floor64(t_send / MAX(bursts, 1)));
	return ret;
}

/* One burst as hex, 5 digits a sample (the dump word's 20 bits): fpga dump <MHz> [rate]. */
static int cmd_dump(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t mhz = strtoul(argv[1], NULL, 0);
	enum esp_sdr_rate rate = argc > 2 ? strtoul(argv[2], NULL, 0) : ESP_SDR_RATE_40MSPS;
	struct esp_sdr_rx_burst b;
	char line[16 * 5 + 1];
	int ret;

	ret = radio_init(sh, mhz);
	ret = ret ?: esp_sdr_rx_capture(rate, BURST, &b);
	if (ret) {
		shell_error(sh, "capture: %d", ret);
		return ret;
	}
	shell_print(sh, "dump: %u MHz rate %d count %u", mhz, rate, (unsigned int)b.count);
	for (size_t j = 0; j < b.count; j += 16) {
		size_t n = MIN(16, b.count - j);

		for (size_t k = 0; k < n; k++) {
			snprintk(&line[5 * k], 6, "%05x", b.words[j + k] & 0xfffffU);
		}
		shell_print(sh, "%s", line);
	}
	shell_print(sh, "dump: end");
	return 0;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(fpga_cmds,
	SHELL_CMD_ARG(reg, NULL, "reg <addr> [value]", cmd_reg, 2, 1),
	SHELL_CMD_ARG(gfx, NULL, "gfx [seconds] [te 0/1]: spectrum and waterfall from software",
		      cmd_gfx, 1, 2),
	SHELL_CMD_ARG(dsp, NULL, "dsp [seconds] [log2n] [frames per line] [repeat]: the FPGA's "
		      "FFT on samples made here", cmd_dsp, 1, 4),
#ifdef CONFIG_ESP_SDR
	SHELL_CMD_ARG(radio, NULL, "radio <MHz> [seconds] [rate] [format]: bursts into the FPGA",
		      cmd_radio, 2, 3),
	SHELL_CMD_ARG(dump, NULL, "dump <MHz> [rate]: one burst as hex", cmd_dump, 2, 1),
#endif
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
