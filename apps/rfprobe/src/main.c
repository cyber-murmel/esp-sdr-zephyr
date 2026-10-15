/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Bench probe for the radio: tune (through the library's frequency plan or
 * the PLL directly), loop a tone (ESP32-S3), dump raw captures as base64 for
 * host-side analysis and read the analog I2C registers. tools/rfprobe.py
 * drives it; README.rst lists the commands.
 */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/app_version.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/base64.h>
#include <zephyr/sys/util.h>

#include <esp_rom_sys.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_rx.h>
#include <esp_sdr/esp_sdr_tx.h>
#if defined(CONFIG_ESP_SDR_DIAG)
#include <esp_sdr/esp_sdr_diag.h>
#endif
#if defined(CONFIG_ESP_SDR_REPLAY)
#include <esp_sdr/esp_sdr_paced.h>
#include <esp_sdr/esp_sdr_replay.h>
#endif

#if defined(CONFIG_USB_DEVICE_STACK_NEXT)
#include "app_usb.h"
#endif

#if defined(CONFIG_POWEROFF)
#include <esp_sleep.h>
#include <zephyr/sys/poweroff.h>
#endif

#define TWO_PI 6.28318530717958647692

/* Vendor and ROM calls, as lib/esp_sdr uses them (doc/undocumented-registers-*.md). */
extern void regi2c_enter_critical(void);
extern void regi2c_exit_critical(void);
#if defined(CONFIG_SOC_SERIES_ESP32C5)
/* The C5 PHY's I2C read and direct PLL setting; phy_param byte 49 is the crystal selector. */
extern unsigned int phy_chip_i2c_readReg(unsigned int block, unsigned int host, unsigned int reg);
#define rom_chip_i2c_readReg phy_chip_i2c_readReg
extern void phy_set_rf_freq_offset(unsigned int xtal, unsigned int mhz, int khz);
extern unsigned char phy_param[];
/* Receive side power and clock (libphy), for the duplex probe. */
extern void phy_pbus_workmode(void);
extern void phy_pbus_xpd_rx_on(unsigned int en);
extern void phy_set_rxclk_en(unsigned int en);
#else
extern unsigned int rom_chip_i2c_readReg(unsigned int block, unsigned int host, unsigned int reg);
#endif
#if defined(CONFIG_SOC_SERIES_ESP32S3)
extern void set_rf_freq_offset(unsigned int xtal, unsigned int mhz, int khz);
#elif !defined(CONFIG_SOC_SERIES_ESP32C5)
extern void phy_set_freq(unsigned int mhz, int khz);
#endif

/* Settle after a direct PLL write, as the library's retune. */
#define RAW_SETTLE_US 3000U

#if defined(CONFIG_SOC_SERIES_ESP32S3)
/* A tone's loop session holds the radio until "probe tone off". */
static bool tone_active;
#endif

static long arg(char **argv, size_t argc, size_t i, long def)
{
	return argc > i ? strtol(argv[i], NULL, 0) : def;
}

static bool busy(const struct shell *sh)
{
#if defined(CONFIG_SOC_SERIES_ESP32S3)
	if (tone_active) {
		shell_error(sh, "tone on: probe tone off first");
		return true;
	}
#elif defined(CONFIG_ESP_SDR_REPLAY)
	if (esp_sdr_replay_active()) {
		shell_error(sh, "replay on: probe tx stop first");
		return true;
	}
#else
	ARG_UNUSED(sh);
#endif
	return false;
}

/* f <MHz>: tune through the library's plan; prints esp_sdr_set_freq()'s return. */
static int cmd_f(const struct shell *sh, size_t argc, char **argv)
{
	long mhz = arg(argv, argc, 1, 2412);
	int ret;

	if (busy(sh)) {
		return -EBUSY;
	}
	(void)esp_sdr_set_turnaround(RAW_SETTLE_US, true);
	ret = mhz > 0 ? esp_sdr_set_freq((uint32_t)mhz) : -EINVAL;
	shell_print(sh, "f %ld ret %d", mhz, ret);
	return 0;
}

/*
 * raw <PLL MHz> [kHz]: program the PLL directly, bypassing the frequency plan,
 * and stop retuning around captures and transmissions. The LO divider stays as
 * the last "f" left it (on below 2210 MHz), so the LO is the PLL or PLL / 1.2.
 */
static int cmd_raw(const struct shell *sh, size_t argc, char **argv)
{
	long mhz = arg(argv, argc, 1, 2412);
	long khz = arg(argv, argc, 2, 0);

	if (busy(sh)) {
		return -EBUSY;
	}
	if (mhz <= 0) {
		shell_error(sh, "raw: MHz must be positive");
		return -EINVAL;
	}
	(void)esp_sdr_set_turnaround(RAW_SETTLE_US, false);
#if defined(CONFIG_SOC_SERIES_ESP32S3)
	set_rf_freq_offset(0, (unsigned int)mhz, (int)khz);
#elif defined(CONFIG_SOC_SERIES_ESP32C5)
	phy_set_rf_freq_offset(phy_param[49], (unsigned int)mhz, (int)khz);
#else
	phy_set_freq((unsigned int)mhz, (int)khz);
#endif
	k_busy_wait(RAW_SETTLE_US);
	shell_print(sh, "raw %ld %ld", mhz, khz);
	return 0;
}

/* lpf <rx> [txa txb]: low-pass codes, -1 the PHY's, 0 the widest. */
static int cmd_lpf(const struct shell *sh, size_t argc, char **argv)
{
	int ret = esp_sdr_rx_set_lpf((int)arg(argv, argc, 1, 0));
	int tret = -ENOTSUP;

	if (argc > 2) {
		tret = esp_sdr_tx_set_lpf((int)arg(argv, argc, 2, 0),
					  (int)arg(argv, argc, 3, arg(argv, argc, 2, 0)));
	}
	shell_print(sh, "lpf rx %d ret %d tx ret %d", esp_sdr_rx_get_lpf(), ret, tret);
	return 0;
}

/* gain <rx index|-1> [tx step]: fixed receive gain or AGC; transmit step (S3). */
static int cmd_gain(const struct shell *sh, size_t argc, char **argv)
{
	int ret = esp_sdr_rx_set_gain((int)arg(argv, argc, 1, ESP_SDR_RX_GAIN_AUTO));

#if defined(CONFIG_SOC_SERIES_ESP32S3)
	if (argc > 2) {
		int tret = esp_sdr_tx_set_gain((int)arg(argv, argc, 2, 0));

		shell_print(sh, "txgain %d ret %d max %d", esp_sdr_tx_get_gain(), tret,
			    esp_sdr_tx_gain_max());
	}
#endif
	shell_print(sh, "rxgain %d ret %d max %d", esp_sdr_rx_get_gain(), ret,
		    esp_sdr_rx_gain_max());
	return 0;
}

#if defined(CONFIG_SOC_SERIES_ESP32S3)
/* tone on <Hz> [amp] [rate 0|1]: a whole number of periods in the bank, looped. */
static int cmd_tone_on(const struct shell *sh, size_t argc, char **argv)
{
	long hz = arg(argv, argc, 1, 1000000);
	long amp = arg(argv, argc, 2, 400);
	enum esp_sdr_rate rate = (enum esp_sdr_rate)arg(argv, argc, 3, ESP_SDR_RATE_80MSPS);
	double fs = (double)esp_sdr_tx_rate_hz(rate);
	double best = INFINITY;
	size_t words, n = 0;
	long cycles = 0;
	uint32_t *bank;
	int ret;

	if (busy(sh)) {
		return -EBUSY;
	}
	if (fs == 0.0 || fabs((double)hz) >= fs / 2.0 || amp < 1 || amp > 511) {
		shell_error(sh, "tone: rate 0 or 1, |Hz| below rate / 2, amp 1..511");
		return -EINVAL;
	}
	ret = esp_sdr_tx_loop_begin(rate);
	if (ret != 0) {
		shell_error(sh, "tone: begin %d", ret);
		return ret;
	}
	bank = esp_sdr_tx_loop_buf(0, &words);
	words = MIN(words, ESP_SDR_SAMPLES_MAX);
	/* The period length whose whole number of cycles comes closest to hz. */
	for (size_t len = words; len >= ESP_SDR_SAMPLES_MIN; len--) {
		long c = lround((double)hz * (double)len / fs);
		double err = fabs((double)c * fs / (double)len - (double)hz);

		if (err < best) {
			best = err;
			n = len;
			cycles = c;
			if (err < 1.0) {
				break;
			}
		}
	}
	for (size_t k = 0; k < n; k++) {
		double cyc = (double)cycles * (double)k / (double)n;
		double ph = TWO_PI * (cyc - floor(cyc));

		bank[k] = esp_sdr_tx_word((int16_t)lround((double)amp * cos(ph)),
					  (int16_t)lround((double)amp * sin(ph)));
	}
	ret = esp_sdr_tx_loop_start(0, n);
	if (ret != 0) {
		esp_sdr_tx_loop_end();
		shell_error(sh, "tone: start %d", ret);
		return ret;
	}
	tone_active = true;
	shell_print(sh, "tone on %u MHz %+ld Hz period %u amp %ld txgain %d",
		    esp_sdr_get_freq(), lround((double)cycles * fs / (double)n), (unsigned int)n,
		    amp, esp_sdr_tx_get_gain());
	return 0;
}

static int cmd_tone_off(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (tone_active) {
		esp_sdr_tx_loop_end();
		tone_active = false;
	}
	shell_print(sh, "tone off");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(tone_cmds,
	SHELL_CMD_ARG(on, NULL, "<Hz> [amp] [rate 0|1]: loop a tone at the LO + Hz", cmd_tone_on,
		      1, 3),
	SHELL_CMD(off, NULL, "stop the tone", cmd_tone_off),
	SHELL_SUBCMD_SET_END);
#endif

/* rx [n] [rate]: one capture, dumped as base64 of the raw little-endian words. */
static int cmd_rx(const struct shell *sh, size_t argc, char **argv)
{
	size_t n = (size_t)arg(argv, argc, 1, ESP_SDR_SAMPLES_MAX);
	enum esp_sdr_rate rate = (enum esp_sdr_rate)arg(argv, argc, 2, ESP_SDR_RATE_80MSPS);
	struct esp_sdr_rx_burst b = {0};
	char line[80];
	int ret;

	if (busy(sh)) {
		return -EBUSY;
	}
	ret = esp_sdr_rx_capture(rate, n, &b);
	if (ret != 0) {
		shell_print(sh, "rx ret %d", ret);
		return 0;
	}
	shell_print(sh, "rx n %u f %u fs %u lpf %d gain %d", (unsigned int)b.count,
		    esp_sdr_get_freq(), esp_sdr_rx_rate_hz(rate), esp_sdr_rx_get_lpf(),
		    esp_sdr_rx_get_gain());
	for (size_t w = 0; w < b.count; w += 12) {
		size_t words = MIN(12U, b.count - w), olen;

		(void)base64_encode((uint8_t *)line, sizeof(line), &olen,
				    (const uint8_t *)&b.words[w], words * 4U);
		line[olen] = '\0';
		shell_print(sh, "d %s", line);
	}
	shell_print(sh, "end");
	return 0;
}

/* dump <block> [regs]: analog I2C registers 0..regs-1 of one block, read only. */
static int cmd_dump(const struct shell *sh, size_t argc, char **argv)
{
	unsigned int block = (unsigned int)arg(argv, argc, 1, 0x62);
	unsigned int regs = (unsigned int)CLAMP(arg(argv, argc, 2, 16), 1, 64);
	char line[3 * 64 + 1];
	int len = 0;

	for (unsigned int r = 0; r < regs; r++) {
		unsigned int v;

		/* The host argument is ignored by the ROM: the block picks the master. */
		regi2c_enter_critical();
		v = rom_chip_i2c_readReg(block, 1, r) & 0xffU;
		regi2c_exit_critical();
		len += snprintf(line + len, sizeof(line) - len, " %02x", v);
	}
	shell_print(sh, "dump %02x:%s", block, line);
	return 0;
}

#if defined(CONFIG_POWEROFF)
/* sleep <ms>: deep sleep with a timer wakeup; the chip boots again afterwards. */
static int cmd_sleep(const struct shell *sh, size_t argc, char **argv)
{
	long ms = CLAMP(arg(argv, argc, 1, 100), 1, 60000);

	if (busy(sh)) {
		return -EBUSY;
	}
	shell_print(sh, "sleep %ld ms", ms);
	k_msleep(50);
	esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000U);
	sys_poweroff();
	return 0;
}
#endif

#if defined(CONFIG_ESP_SDR_DIAG)
/* diag level: the nibble window the signal needs. */
static int cmd_diag_level(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t lsb = 0;
	int ret = esp_sdr_diag_level(&lsb);

	shell_print(sh, "diag level ret %d lsb %u", ret, lsb);
	return 0;
}

static struct {
	uint32_t x;
	uint8_t buf[8192];
	size_t want, got;
	bool snap;
} dg;

static void diag_sink(void *user, const uint8_t *blk, size_t n, uint64_t index)
{
	ARG_UNUSED(user);
	ARG_UNUSED(index);
	if (dg.snap) {
		dg.got = MIN(dg.want, MIN(n, sizeof(dg.buf)));
		memcpy(dg.buf, blk, dg.got);
		esp_sdr_diag_stop();
		return;
	}
	/* Touch every byte, as a consumer would. */
	for (size_t k = 0; k < n; k += 4) {
		dg.x ^= *(const uint32_t *)&blk[k];
	}
}

static void diag_print(const struct shell *sh, int ret, const struct esp_sdr_diag_stats *s)
{
	shell_print(sh, "diag ret %d bytes %llu consumed %llu skips %u skipped %u overrun %d "
			"ppm %d.%02d lsb %u sink %u.%02u cyc/B rate %u isrs %u raw %08x",
		    ret, s->bytes, s->consumed, s->skips, s->skipped_blocks, s->overrun,
		    s->ppm_x100 / 100, abs(s->ppm_x100) % 100, s->lsb, s->sink_cycles_x100 / 100U,
		    s->sink_cycles_x100 % 100U, s->byte_rate, s->isrs, s->dma_raw);
	shell_print(sh, "desc %08x %08x %08x %08x", s->dw0[0], s->dw0[1], s->dw0[2], s->dw0[3]);
}

/* diag run <s> [div] [lsb|-1] [edge] [R]: the ring for s seconds; with R the CIC decimates, else a consumer touches every byte. */
static int cmd_diag_run(const struct shell *sh, size_t argc, char **argv)
{
	struct esp_sdr_diag_stats s;
	struct esp_sdr_diag_cfg cfg = {
		.seconds = (uint32_t)arg(argv, argc, 1, 5),
		.clk_div = (uint8_t)arg(argv, argc, 2, 6),
		.lsb = (uint8_t)arg(argv, argc, 3, -1),
		.falling_edge = arg(argv, argc, 4, 0) != 0,
		.decim = (uint8_t)arg(argv, argc, 5, 0),
	};

	cfg.sink = cfg.decim != 0U ? NULL : diag_sink;
	dg.snap = false;
	diag_print(sh, esp_sdr_diag_run(&cfg, &s), &s);
	return 0;
}

static void diag_iq_sink(void *user, const int32_t *iq, size_t n, uint64_t index)
{
	ARG_UNUSED(user);
	ARG_UNUSED(index);
	int16_t *out = (int16_t *)dg.buf;

	for (size_t k = 0; k < n && dg.got < dg.want; k++, dg.got++) {
		out[2U * dg.got] = (int16_t)CLAMP(iq[2U * k], INT16_MIN, INT16_MAX);
		out[2U * dg.got + 1U] = (int16_t)CLAMP(iq[2U * k + 1U], INT16_MIN, INT16_MAX);
	}
	if (dg.got >= dg.want) {
		esp_sdr_diag_stop();
	}
}

/* diag iq <R> [pairs] [lsb|-1]: decimated I/Q (int16 pairs, raw counts times 16), base64. */
static int cmd_diag_iq(const struct shell *sh, size_t argc, char **argv)
{
	struct esp_sdr_diag_stats s;
	struct esp_sdr_diag_cfg cfg = {
		.seconds = 4,
		.decim = (uint8_t)arg(argv, argc, 1, 40),
		.lsb = (uint8_t)arg(argv, argc, 3, -1),
		.iq_sink = diag_iq_sink,
	};
	char line[80];
	int ret;

	dg.snap = false;
	dg.got = 0;
	dg.want = (size_t)CLAMP(arg(argv, argc, 2, 2048), 16, (long)(sizeof(dg.buf) / 4U));
	ret = esp_sdr_diag_run(&cfg, &s);
	diag_print(sh, ret, &s);
	shell_print(sh, "iq n %u rate %u lsb %u f %u", (unsigned int)dg.got, s.iq_rate, s.lsb,
		    esp_sdr_get_freq());
	for (size_t w = 0; w < dg.got * 4U; w += 48) {
		size_t bytes = MIN(48U, dg.got * 4U - w), olen;

		(void)base64_encode((uint8_t *)line, sizeof(line), &olen, &dg.buf[w], bytes);
		line[olen] = '\0';
		shell_print(sh, "d %s", line);
	}
	shell_print(sh, "end");
	return 0;
}

/* diag snap [lsb|-1] [bytes] [edge]: the first bytes of the ring, base64 (8 bytes per sample pair of nibbles). */
static int cmd_diag_snap(const struct shell *sh, size_t argc, char **argv)
{
	struct esp_sdr_diag_stats s;
	struct esp_sdr_diag_cfg cfg = {
		.seconds = 2,
		.lsb = (uint8_t)arg(argv, argc, 1, -1),
		.falling_edge = arg(argv, argc, 3, 0) != 0,
		.sink = diag_sink,
	};
	char line[80];
	int ret;

	dg.snap = true;
	dg.got = 0;
	dg.want = (size_t)CLAMP(arg(argv, argc, 2, 4096), 64, (long)sizeof(dg.buf));
	ret = esp_sdr_diag_run(&cfg, &s);
	diag_print(sh, ret, &s);
	shell_print(sh, "snap n %u lsb %u f %u", (unsigned int)dg.got, s.lsb, esp_sdr_get_freq());
	for (size_t w = 0; w < dg.got; w += 48) {
		size_t bytes = MIN(48U, dg.got - w), olen;

		(void)base64_encode((uint8_t *)line, sizeof(line), &olen, &dg.buf[w], bytes);
		line[olen] = '\0';
		shell_print(sh, "d %s", line);
	}
	shell_print(sh, "end");
	return 0;
}

/* Register changes the diagnostic-bus run applies once the dump engine runs, and what it read. */
#define POKES_MAX 8
static struct {
	uint32_t addr, clear, set;
} pokes[POKES_MAX];
static unsigned int npokes;
static const uint32_t page_base[] = {0x600a9000U, 0x600a9400U, 0x600a9c00U};
static uint32_t pages[ARRAY_SIZE(page_base)][64];

void esp_sdr_diag_armed_hook(void)
{
	for (unsigned int k = 0; k < npokes; k++) {
		volatile uint32_t *r = (volatile uint32_t *)pokes[k].addr;

		*r = (*r & ~pokes[k].clear) | pokes[k].set;
	}
	for (unsigned int p = 0; p < ARRAY_SIZE(page_base); p++) {
		for (unsigned int w = 0; w < 64U; w++) {
			pages[p][w] = *(volatile uint32_t *)(page_base[p] + 4U * w);
		}
	}
}

/* diag poke [<addr> <clear> <set>]: add a register change for the next runs; none clears the list. */
static int cmd_diag_poke(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 4) {
		npokes = 0;
		shell_print(sh, "poke cleared");
		return 0;
	}
	if (npokes == POKES_MAX) {
		shell_error(sh, "poke: list full");
		return -ENOMEM;
	}
	pokes[npokes].addr = (uint32_t)strtoul(argv[1], NULL, 0);
	pokes[npokes].clear = (uint32_t)strtoul(argv[2], NULL, 0);
	pokes[npokes].set = (uint32_t)strtoul(argv[3], NULL, 0);
	npokes++;
	shell_print(sh, "poke %u: %08x &~%08x |%08x", npokes, pokes[npokes - 1].addr,
		    pokes[npokes - 1].clear, pokes[npokes - 1].set);
	return 0;
}

/* diag regs: the register pages as the last run left them right after the pokes. */
static int cmd_diag_regs(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	for (unsigned int p = 0; p < ARRAY_SIZE(page_base); p++) {
		for (unsigned int w = 0; w < 64U; w += 8U) {
			shell_print(sh, "regs %08x: %08x %08x %08x %08x %08x %08x %08x %08x",
				    page_base[p] + 4U * w, pages[p][w], pages[p][w + 1], pages[p][w + 2],
				    pages[p][w + 3], pages[p][w + 4], pages[p][w + 5], pages[p][w + 6],
				    pages[p][w + 7]);
		}
	}
	shell_print(sh, "regs end");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(diag_cmds,
	SHELL_CMD(level, NULL, "the nibble window the signal needs", cmd_diag_level),
	SHELL_CMD_ARG(run, NULL, "<s> [div] [lsb|-1] [edge] [R]: gapless ring, stats", cmd_diag_run, 1, 5),
	SHELL_CMD_ARG(snap, NULL, "[lsb|-1] [bytes] [edge]: ring bytes, base64", cmd_diag_snap, 1, 3),
	SHELL_CMD_ARG(iq, NULL, "<R> [pairs] [lsb|-1]: decimated I/Q, base64", cmd_diag_iq, 1, 3),
	SHELL_CMD_ARG(poke, NULL, "[<addr> <clear> <set>]: register change once armed; none clears",
		      cmd_diag_poke, 1, 3),
	SHELL_CMD(regs, NULL, "register pages as the last run had them", cmd_diag_regs),
	SHELL_SUBCMD_SET_END);
#endif

#if defined(CONFIG_ESP_SDR_REPLAY)
/*
 * tx replay <Hz> [amp] [rate 0|1] [passes] [n] [power] [extra] [scale]: a tone of a
 * whole number of cycles in n words (at most n: the period closest to Hz when
 * n is left out), played by the DAC engine. passes 0 loops until "probe tx
 * stop"; otherwise the command waits and prints the cycles the passes took,
 * and the radio stays held for "tx rearm" until "tx stop".
 */
static int cmd_tx_replay(const struct shell *sh, size_t argc, char **argv)
{
	long hz = arg(argv, argc, 1, 1000000);
	long amp = arg(argv, argc, 2, 400);
	struct esp_sdr_replay_cfg cfg = {
		.rate = (enum esp_sdr_rate)arg(argv, argc, 3, ESP_SDR_RATE_80MSPS),
		.passes = (unsigned int)arg(argv, argc, 4, 0),
		.power = (unsigned int)arg(argv, argc, 6, 15),
		.ctrl_extra = (uint32_t)strtoul(argc > 7 ? argv[7] : "0", NULL, 0),
		.scale = (uint8_t)arg(argv, argc, 8, 0),
	};
	double fs = cfg.rate == ESP_SDR_RATE_40MSPS ? 40e6 : 80e6;
	long nmax = arg(argv, argc, 5, 16000);
	double best = INFINITY;
	size_t words, n = 0;
	long cycles = 0;
	uint32_t *buf, dt = 0;
	int ret;

	if (busy(sh)) {
		return -EBUSY;
	}
	buf = esp_sdr_replay_buf(&words);
	if (fabs((double)hz) >= fs / 2.0 || amp < 1 || amp > 511 || nmax < 1 ||
	    nmax > (long)words) {
		shell_error(sh, "replay: |Hz| below rate / 2, amp 1..511, n 1..%u",
			    (unsigned int)words);
		return -EINVAL;
	}
	if (argc > 5) {
		/* n given: exactly n words, the nearest whole number of cycles. */
		n = (size_t)nmax;
		cycles = lround((double)hz * (double)n / fs);
	} else {
		for (size_t len = (size_t)nmax; len >= ESP_SDR_SAMPLES_MIN; len--) {
			long c = lround((double)hz * (double)len / fs);
			double err = fabs((double)c * fs / (double)len - (double)hz);

			if (err < best) {
				best = err;
				n = len;
				cycles = c;
				if (err < 1.0) {
					break;
				}
			}
		}
	}
	for (size_t k = 0; k < n; k++) {
		double cyc = (double)cycles * (double)k / (double)n;
		double ph = TWO_PI * (cyc - floor(cyc));

		buf[k] = esp_sdr_replay_word(lround((double)amp * cos(ph)),
					     lround((double)amp * sin(ph)));
	}
	cfg.count = n;
	ret = esp_sdr_replay_start(&cfg);
	if (ret != 0) {
		shell_error(sh, "replay: start %d", ret);
		return ret;
	}
	if (cfg.passes != 0U) {
		ret = esp_sdr_replay_wait(1000000, &dt);
		shell_print(sh, "replay done %d cycles %u ctrl %08x", ret, dt,
			    esp_sdr_replay_ctrl());
	}
	shell_print(sh, "replay %u MHz %+ld Hz period %u cycles %ld rate %u passes %u ctrl %08x",
		    esp_sdr_get_freq(), lround((double)cycles * fs / (double)n), (unsigned int)n,
		    cycles, (unsigned int)fs, cfg.passes, esp_sdr_replay_ctrl());
	return 0;
}

/*
 * tx rearm <ms>: restarts the finite replay the last "tx replay" set up back to
 * back for ms, with interrupts locked, and prints the restart count.
 */
static int cmd_tx_rearm(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t ms = (uint32_t)arg(argv, argc, 1, 2000);
	int64_t end = k_uptime_get() + ms;
	unsigned int key, restarts = 0;
	uint32_t dt;
	int ret = 0;

	if (!esp_sdr_replay_active()) {
		shell_error(sh, "rearm: set up a finite replay (tx replay, passes > 0) first");
		return -EINVAL;
	}
	while (ret == 0 && k_uptime_get() < end) {
		key = irq_lock();
		for (int k = 0; k < 1000 && ret == 0; k++) {
			ret = esp_sdr_replay_wait(100000, &dt);
			if (ret == 0) {
				ret = esp_sdr_replay_restart();
				restarts++;
			}
		}
		irq_unlock(key);
	}
	shell_print(sh, "rearm ret %d restarts %u last pass cycles %u", ret, restarts, dt);
	return 0;
}

/* The TX DC table: I in the low half, Q in the high half, full scale 32767. */
static struct {
	const uint32_t *tab;
	size_t len, pos;
} dc;

static bool dc_input(void *user, int16_t *i, int16_t *q)
{
	uint32_t w = dc.tab[dc.pos];

	ARG_UNUSED(user);
	dc.pos = dc.pos + 1U == dc.len ? 0U : dc.pos + 1U;
	*i = (int16_t)(w & 0xffffU);
	*q = (int16_t)(w >> 16);
	return true;
}

/*
 * tx dc <rate Hz> <tone Hz> [ms] [amp] [interp] [power]: the CPU-paced TX DC
 * path fed from a whole-cycle tone table on the chip (in the idle replay
 * buffer), interrupts locked; prints lateness and the rate it kept.
 */
static int cmd_tx_dc(const struct shell *sh, size_t argc, char **argv)
{
	long rate = arg(argv, argc, 1, 1000000);
	long hz = arg(argv, argc, 2, 100000);
	uint32_t ms = (uint32_t)arg(argv, argc, 3, 2000);
	long amp = arg(argv, argc, 4, 8000);
	unsigned int interp = (unsigned int)arg(argv, argc, 5, 1);
	unsigned int power = (unsigned int)arg(argv, argc, 6, 2);
	struct esp_sdr_paced_stats st;
	uint32_t *tab, c0, dt;
	long cycles = 0;
	double best = INFINITY;
	size_t words;
	unsigned int key;
	int ret;

	if (busy(sh)) {
		return -EBUSY;
	}
	if (rate <= 0 || labs(hz) * 2 >= rate || amp < 1 || amp > 32767) {
		shell_error(sh, "dc: rate > 0, |Hz| below rate / 2, amp 1..32767");
		return -EINVAL;
	}
	tab = esp_sdr_replay_buf(&words);
	dc.len = 0;
	for (size_t len = MIN(words, 16000U); len >= 16U; len--) {
		long c = lround((double)hz * (double)len / (double)rate);
		double err = fabs((double)c * (double)rate / (double)len - (double)hz);

		if (err < best) {
			best = err;
			dc.len = len;
			cycles = c;
			if (err < 1.0) {
				break;
			}
		}
	}
	for (size_t k = 0; k < dc.len; k++) {
		double cyc = (double)cycles * (double)k / (double)dc.len;
		double ph = TWO_PI * (cyc - floor(cyc));

		tab[k] = ((uint32_t)(uint16_t)lround((double)amp * cos(ph))) |
			 ((uint32_t)(uint16_t)lround((double)amp * sin(ph)) << 16);
	}
	dc.tab = tab;
	dc.pos = 0;
	ret = esp_sdr_tx_paced_start((uint32_t)rate, interp, power, dc_input, NULL);
	if (ret != 0) {
		shell_error(sh, "dc: start %d", ret);
		return ret;
	}
	key = irq_lock();
	esp_sdr_paced_sync();
	c0 = esp_sdr_paced_cycles();
	dt = (uint32_t)((uint64_t)ms * (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000U));
	while (esp_sdr_paced_cycles() - c0 < dt) {
		(void)esp_sdr_tx_paced_poll();
	}
	irq_unlock(key);
	esp_sdr_paced_get_stats(&st);
	esp_sdr_tx_paced_stop();
	shell_print(sh, "dc rate %ld x%u tone %+ld Hz period %u: samples %llu (%u per s) late %u "
		    "late_max %u cycles",
		    rate, interp, lround((double)cycles * (double)rate / (double)dc.len),
		    (unsigned int)dc.len, (unsigned long long)st.samples,
		    (unsigned int)(st.samples * 1000U / ms), st.late, st.late_max);
	return 0;
}

/*
 * tx dcraw <rate Hz> <tone Hz> [ms] [amp]: the TX DC register written from a
 * tight loop (one prepacked table word per sample, paced on the cycle counter),
 * the transmit chain brought up by the paced backend. Bench only: the ceiling
 * of the TX DC path with content made on the chip.
 */
#define TXDC_REG_BENCH 0x600a0c08U
static int cmd_tx_dcraw(const struct shell *sh, size_t argc, char **argv)
{
	long rate = arg(argv, argc, 1, 4000000);
	long hz = arg(argv, argc, 2, 500000);
	uint32_t ms = (uint32_t)arg(argv, argc, 3, 2000);
	long amp = arg(argv, argc, 4, 128);
	uint32_t cpu = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000U, step, t, c0, dt, upper;
	uint32_t late = 0, late_max = 0, n = 0;
	volatile uint32_t *reg = (volatile uint32_t *)TXDC_REG_BENCH;
	uint32_t *tab;
	size_t words, len, pos = 0;
	long cycles;
	unsigned int key;
	int ret;

	if (busy(sh)) {
		return -EBUSY;
	}
	if (rate <= 0 || cpu % (uint32_t)rate != 0U || labs(hz) * 2 >= rate || amp < 1 ||
	    amp > 511) {
		shell_error(sh, "dcraw: rate divides the CPU clock, |Hz| below rate / 2, amp 1..511");
		return -EINVAL;
	}
	/* Whole cycles in at most 16000 words: the rate's divisor of the tone. */
	tab = esp_sdr_replay_buf(&words);
	len = MIN(words, 16000U);
	cycles = lround((double)hz * (double)len / (double)rate);
	dc.tab = tab;
	dc.len = 1;
	dc.pos = 0;
	tab[0] = 0;
	ret = esp_sdr_tx_paced_start((uint32_t)rate, 1, 2, dc_input, NULL);
	if (ret != 0) {
		shell_error(sh, "dcraw: start %d", ret);
		return ret;
	}
	/* One poll writes the register's upper bits with the injection on. */
	key = irq_lock();
	esp_sdr_paced_sync();
	while (!esp_sdr_tx_paced_poll()) {
	}
	upper = *reg & ~GENMASK(19, 0);
	irq_unlock(key);
	for (size_t k = 0; k < len; k++) {
		double cyc = (double)cycles * (double)k / (double)len;
		double ph = TWO_PI * (cyc - floor(cyc));

		tab[k] = upper | esp_sdr_replay_word(lround((double)amp * cos(ph)),
						     lround((double)amp * sin(ph)));
	}
	step = cpu / (uint32_t)rate;
	dt = (uint32_t)((uint64_t)ms * (cpu / 1000U));
	key = irq_lock();
	c0 = esp_sdr_paced_cycles();
	t = c0 + step;
	while (t - c0 < dt) {
		uint32_t now;

		while ((int32_t)((now = esp_sdr_paced_cycles()) - t) < 0) {
		}
		*reg = tab[pos];
		pos = pos + 1U == len ? 0U : pos + 1U;
		if (now - t > late_max) {
			late_max = now - t;
		}
		if (now - t >= step) {
			late++;
		}
		t += step;
		n++;
	}
	irq_unlock(key);
	esp_sdr_tx_paced_stop();
	shell_print(sh, "dcraw rate %ld tone %+ld Hz period %u: samples %u late %u late_max %u cycles",
		    rate, lround((double)cycles * (double)rate / (double)len), (unsigned int)len, n,
		    late, late_max);
	return 0;
}

/*
 * tx carrier <i> <q> [rx]: the transmit chain on with a constant TX DC value (a
 * carrier at the LO, no CPU), held until "tx stop"; receive commands still run.
 * rx bits turn receive parts back on afterwards: 1 RX power, 2 RX clock, 4 pbus
 * work mode (first).
 */
static bool carrier_on;

static int cmd_tx_carrier(const struct shell *sh, size_t argc, char **argv)
{
	long i = arg(argv, argc, 1, 100), q = arg(argv, argc, 2, 0);
	long rx = arg(argv, argc, 3, 0);
	volatile uint32_t *reg = (volatile uint32_t *)TXDC_REG_BENCH;
	uint32_t *tab;
	size_t words;
	unsigned int key;
	int ret;

	if (busy(sh) || carrier_on) {
		return -EBUSY;
	}
	tab = esp_sdr_replay_buf(&words);
	tab[0] = (uint32_t)(uint16_t)(i * 64) | ((uint32_t)(uint16_t)(q * 64) << 16);
	dc.tab = tab;
	dc.len = 1;
	dc.pos = 0;
	ret = esp_sdr_tx_paced_start(1000000, 1, 2, dc_input, NULL);
	if (ret != 0) {
		shell_error(sh, "carrier: start %d", ret);
		return ret;
	}
	/* Two polls: the second writes the value the first computed. */
	key = irq_lock();
	esp_sdr_paced_sync();
	for (int k = 0; k < 2; k++) {
		while (!esp_sdr_tx_paced_poll()) {
		}
	}
	irq_unlock(key);
	if (rx & 4) {
		phy_pbus_workmode();
	}
	if (rx & 1) {
		phy_pbus_xpd_rx_on(1);
	}
	if (rx & 2) {
		phy_set_rxclk_en(1);
	}
	carrier_on = true;
	shell_print(sh, "carrier %ld %ld: txdc %08x", i, q, *reg);
	return 0;
}

/*
 * tx cost <reg> [n]: CPU cycles per store and per load of a register (bench;
 * the TX DC value is 0x600a0c08), n each, interrupts locked. The store writes
 * the value the register holds.
 */
static int cmd_tx_cost(const struct shell *sh, size_t argc, char **argv)
{
	uintptr_t reg = (uintptr_t)strtoul(argc > 1 ? argv[1] : "0x600a0c08", NULL, 0);
	uint32_t n = (uint32_t)arg(argv, argc, 2, 10000);
	volatile uint32_t *r = (volatile uint32_t *)reg;
	uint32_t v = *r, c0, st, ld, acc = 0;
	unsigned int key = irq_lock();

	c0 = esp_sdr_paced_cycles();
	for (uint32_t k = 0; k < n; k++) {
		*r = v;
	}
	st = esp_sdr_paced_cycles() - c0;
	c0 = esp_sdr_paced_cycles();
	for (uint32_t k = 0; k < n; k++) {
		acc += *r;
	}
	ld = esp_sdr_paced_cycles() - c0;
	irq_unlock(key);
	shell_print(sh, "cost %08lx: store %u.%02u load %u.%02u cycles (%08x)", (unsigned long)reg,
		    st / n, st % n * 100U / n, ld / n, ld % n * 100U / n, acc);
	/* The paced loop's parts: the cycle counter, the input callback. */
	{
		int16_t i, q;
		uint32_t t[2];

		dc.tab = (const uint32_t *)esp_sdr_replay_buf(&dc.len);
		dc.pos = 0;
		key = irq_lock();
		c0 = esp_sdr_paced_cycles();
		for (uint32_t k = 0; k < n; k++) {
			acc += esp_sdr_paced_cycles();
		}
		t[0] = esp_sdr_paced_cycles() - c0;
		c0 = esp_sdr_paced_cycles();
		for (uint32_t k = 0; k < n; k++) {
			(void)dc_input(NULL, &i, &q);
			acc += i;
		}
		t[1] = esp_sdr_paced_cycles() - c0;
		irq_unlock(key);
		shell_print(sh, "cost counter %u.%02u input %u.%02u cycles (%08x)", t[0] / n,
			    t[0] % n * 100U / n, t[1] / n, t[1] % n * 100U / n, acc);
	}
	return 0;
}

static int cmd_tx_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	esp_sdr_replay_stop();
	if (carrier_on) {
		esp_sdr_tx_paced_stop();
		carrier_on = false;
	}
	shell_print(sh, "replay off");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(tx_cmds,
	SHELL_CMD_ARG(replay, NULL,
		      "<Hz> [amp] [rate 0|1] [passes] [n] [power] [extra] [scale]: DAC engine loop",
		      cmd_tx_replay, 1, 8),
	SHELL_CMD_ARG(dc, NULL, "<rate> <Hz> [ms] [amp] [interp] [power]: paced TX DC tone",
		      cmd_tx_dc, 3, 4),
	SHELL_CMD_ARG(dcraw, NULL, "<rate> <Hz> [ms] [amp]: TX DC from a tight loop", cmd_tx_dcraw,
		      3, 2),
	SHELL_CMD_ARG(carrier, NULL, "<i> <q> [rx]: constant TX DC value until tx stop", cmd_tx_carrier,
		      1, 3),
	SHELL_CMD_ARG(cost, NULL, "<reg> [n]: cycles per store and load", cmd_tx_cost, 1, 2),
	SHELL_CMD_ARG(rearm, NULL, "<ms>: restart a finite replay back to back", cmd_tx_rearm,
		      1, 1),
	SHELL_CMD(stop, NULL, "stop the replay", cmd_tx_stop),
	SHELL_SUBCMD_SET_END);
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(probe_cmds,
	SHELL_CMD_ARG(f, NULL, "<MHz>: tune through the library's plan", cmd_f, 2, 0),
	SHELL_CMD_ARG(raw, NULL, "<PLL MHz> [kHz]: program the PLL directly", cmd_raw, 2, 1),
	SHELL_CMD_ARG(lpf, NULL, "<rx> [txa [txb]]: low-pass codes, -1 PHY, 0 widest", cmd_lpf,
		      2, 2),
	SHELL_CMD_ARG(gain, NULL, "<rx index|-1> [tx step]", cmd_gain, 2, 1),
#if defined(CONFIG_SOC_SERIES_ESP32S3)
	SHELL_CMD(tone, &tone_cmds, "loop a tone", NULL),
#endif
	SHELL_CMD_ARG(rx, NULL, "[n] [rate 0..6]: capture, base64 dump", cmd_rx, 1, 2),
#if defined(CONFIG_ESP_SDR_DIAG)
	SHELL_CMD(diag, &diag_cmds, "gapless ring through the diagnostic bus (C5)", NULL),
#endif
#if defined(CONFIG_ESP_SDR_REPLAY)
	SHELL_CMD(tx, &tx_cmds, "DAC engine replay (C5)", NULL),
#endif
	SHELL_CMD_ARG(dump, NULL, "<block> [regs]: analog I2C registers", cmd_dump, 2, 1),
#if defined(CONFIG_POWEROFF)
	SHELL_CMD_ARG(sleep, NULL, "<ms>: deep sleep, timer wakeup", cmd_sleep, 2, 0),
#endif
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(probe, &probe_cmds, "Radio bench probe", NULL);

int main(void)
{
	int ret;

	printk("rfprobe %s\n", APP_VERSION_STRING);
#if defined(CONFIG_USB_DEVICE_STACK_NEXT)
	/* S3: the USB shell and DFU, and the trial watchdog that reverts an image that hangs. */
	if (app_usb_init() != 0) {
		printk("rfprobe: usb init failed\n");
	}
#endif
	printk("rfprobe: reset reason %d\n", (int)esp_rom_get_reset_reason(0));
	ret = esp_sdr_init();
	if (ret != 0) {
		printk("rfprobe: init failed (%d)\n", ret);
		return 0;
	}
	/* Every filter wide open. */
	(void)esp_sdr_rx_set_lpf(0);
#if defined(CONFIG_SOC_SERIES_ESP32S3)
	(void)esp_sdr_tx_set_lpf(0, 0);
#endif
	printk("rfprobe: ready\n");
	return 0;
}
