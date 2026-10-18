/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The "chancap" shell command: the grid and detector settings, start and stop, status,
 * the plan and the bench. Settings apply at the next start; gates and thresholds also to
 * the running capture.
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/sys_heap.h>

#include <esp_sdr/esp_sdr.h>
#include <esp_sdr/esp_sdr_rx.h>
#include <esp_sdr/esp_sdr_chancap.h>

#include "app.h"
#include "app_shell.h"

extern struct k_heap _system_heap;

static int parse_onoff(const struct shell *sh, const char *s, bool *out)
{
	if (strcmp(s, "on") == 0 || strcmp(s, "1") == 0) {
		*out = true;
		return 0;
	}
	if (strcmp(s, "off") == 0 || strcmp(s, "0") == 0) {
		*out = false;
		return 0;
	}
	shell_error(sh, "%s: on or off", s);
	return -EINVAL;
}

static int cmd_start(const struct shell *sh, size_t argc, char **argv)
{
	int ret = cc_start();

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (ret != 0) {
		shell_error(sh, "start: %d", ret);
		return ret;
	}
	shell_print(sh, "started");
	return 0;
}

static int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	cc_stop();
	shell_print(sh, "stopped");
	return 0;
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	struct cc_settings *s = cc_settings();
	struct esp_sdr_chancap_stats st;
	struct esp_sdr_chancap_plan p;
	struct vrt_cc_stats vs;
	struct sys_memory_stats hs;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	esp_sdr_chancap_get_stats(&st);
	esp_sdr_chancap_get_plan(&p);
	vrt_cc_get_stats(&vs);
	shell_print(sh, "%s: %u channels from %u Hz every %u Hz at %u S/s; gates wb %s ch %s, "
		    "thresholds %d / %d dB, lookback %u hang %u max %u us; rxgain %d, bw %d MHz",
		    esp_sdr_chancap_active() ? "running" : "stopped", s->cfg.n_channels,
		    s->cfg.first_freq_hz, s->cfg.spacing_hz, s->cfg.channel_rate_hz,
		    s->cfg.wb_gate ? "on" : "off", s->cfg.ch_gate ? "on" : "off",
		    s->cfg.wb_threshold_db, s->cfg.ch_threshold_db, s->cfg.lookback_us,
		    s->cfg.hang_us, s->cfg.max_capture_us, s->rx_gain, s->rx_bw_mhz);
	shell_print(sh, "plan: %s m %u d %u, %u taps, LO %llu Hz", p.pfb ? "pfb" : "ddc", p.m, p.d,
		    p.taps, (unsigned long long)p.lo_hz);
	shell_print(sh, "pairs %llu, channelized %llu, captures %u (%u truncated, %u dropped), "
		    "overruns %u, gaps %u, %u.%02u cycles per pair, block max %u, lag max %u, "
		    "buffers %u (peak %u)",
		    (unsigned long long)st.pairs, (unsigned long long)st.pairs_run, st.captures,
		    st.truncated, st.dropped, st.overruns, st.gaps, st.cycles_x100 / 100U,
		    st.cycles_x100 % 100U, st.block_max, st.lag_max, st.bufs_used, st.bufs_peak);
	shell_print(sh, "ring: status %d detail 0x%08x, %u units, %u abandoned, %u lost pairs, "
		    "late max %u, %u.%02u cycles per pair",
		    st.ring.status, st.ring.detail, st.ring.units, st.ring.abandoned,
		    st.ring.lost_pairs, st.ring.late_max, st.ring.cycles_x100 / 100U,
		    st.ring.cycles_x100 % 100U);
	{
		struct esp_sdr_stats rx;

		esp_sdr_get_stats(&rx);
		shell_print(sh, "ring detail: %llu pairs, %u irq windows (lines 0x%08x), %u stalls, "
			    "slice max %u; gain re-forced %u in the ring, %u in all (last %u us)",
			    (unsigned long long)st.ring.pairs, st.ring.irq_windows,
			    st.ring.irq_lines, st.ring.stalls, st.ring.slice_max,
			    st.ring.gain_refreshed, rx.gain_refreshed, rx.gain_apply_us);
	}
	shell_print(sh, "vrt: %u captures, %u data, %u context packets, %u blocks, %u kB, "
		    "%u dropped, %u errors, %u blocks free",
		    vs.captures, vs.packets, vs.context, vs.blocks, vs.bytes / 1024U, vs.dropped,
		    vs.errors, vs.blocks_free);
	if (sys_heap_runtime_stats_get(&_system_heap.heap, &hs) == 0) {
		shell_print(sh, "system heap: %u allocated, %u free, %u peak",
			    (unsigned int)hs.allocated_bytes, (unsigned int)hs.free_bytes,
			    (unsigned int)hs.max_allocated_bytes);
	}
	return 0;
}

static int cmd_bench(const struct shell *sh, size_t argc, char **argv)
{
	struct esp_sdr_chancap_bench_result r;
	long pairs = 8192;
	int ret;

	if (argc > 1 && app_shell_long(sh, argv[1], 512, 1 << 20, &pairs) != 0) {
		return -EINVAL;
	}
	ret = esp_sdr_chancap_bench(&cc_settings()->cfg, (uint32_t)(pairs & ~511L), &r);
	if (ret != 0) {
		shell_error(sh, "bench: %d", ret);
		return ret;
	}
	shell_print(sh, "bench%s: ring CPU %u.%02u, channelizer CPU %u.%02u cycles per input pair "
		    "(15 available each); C engines alone: pfb frame %u.%02u, ddc channel %u.%02u",
		    r.fast ? " (both CPUs)" : "", r.ring_x100 / 100U, r.ring_x100 % 100U,
		    r.block_x100 / 100U, r.block_x100 % 100U, r.pfb_x100 / 100U, r.pfb_x100 % 100U,
		    r.ddc_x100 / 100U, r.ddc_x100 % 100U);
	if (r.fast) {
		shell_print(sh, "kernels per call: sums %u (a frame), post %u (8 frames), power %u; "
			    "%s (%u mismatches)",
			    r.sums_cyc, r.post_cyc, r.power_cyc,
			    r.kernels_match ? "match the C reference" : "MISMATCH", r.mismatches);
	}
	return 0;
}

static int set_first(const struct shell *sh, const char *v)
{
	char *end;
	/* Beyond a 32-bit long; the field is 32 bits, so 4 GHz at most. */
	unsigned long long x = strtoull(v, &end, 0);

	if (*v == '\0' || *end != '\0' || x < 100000000ULL || x > 4000000000ULL) {
		shell_error(sh, "bad value: %s (100 MHz to 4 GHz)", v);
		return -EINVAL;
	}
	cc_settings()->cfg.first_freq_hz = (uint32_t)x;
	return 0;
}

static int set_spacing(const struct shell *sh, const char *v)
{
	long x;

	if (app_shell_long(sh, v, 1000, 16000000, &x) != 0) {
		return -EINVAL;
	}
	cc_settings()->cfg.spacing_hz = (uint32_t)x;
	return 0;
}

static int set_channels(const struct shell *sh, const char *v)
{
	long x;

	if (app_shell_long(sh, v, 1, ESP_SDR_CHANCAP_CHANNELS_MAX, &x) != 0) {
		return -EINVAL;
	}
	cc_settings()->cfg.n_channels = (uint16_t)x;
	return 0;
}

static int set_rate(const struct shell *sh, const char *v)
{
	long x;

	if (app_shell_long(sh, v, 1000, 16000000, &x) != 0) {
		return -EINVAL;
	}
	cc_settings()->cfg.channel_rate_hz = (uint32_t)x;
	return 0;
}

static int set_db(const struct shell *sh, const char *v, int16_t *out)
{
	long x;

	if (app_shell_long(sh, v, 0, 60, &x) != 0) {
		return -EINVAL;
	}
	*out = (int16_t)x;
	return 0;
}

static int set_wb(const struct shell *sh, const char *v)
{
	struct cc_settings *s = cc_settings();
	int ret = set_db(sh, v, &s->cfg.wb_threshold_db);

	if (ret == 0 && esp_sdr_chancap_active()) {
		ret = esp_sdr_chancap_set_thresholds(s->cfg.wb_threshold_db, s->cfg.ch_threshold_db);
	}
	return ret;
}

static int set_ch(const struct shell *sh, const char *v)
{
	struct cc_settings *s = cc_settings();
	int ret = set_db(sh, v, &s->cfg.ch_threshold_db);

	if (ret == 0 && esp_sdr_chancap_active()) {
		ret = esp_sdr_chancap_set_thresholds(s->cfg.wb_threshold_db, s->cfg.ch_threshold_db);
	}
	return ret;
}

static int set_us(const struct shell *sh, const char *v, uint32_t *out, long max)
{
	long x;

	if (app_shell_long(sh, v, 0, max, &x) != 0) {
		return -EINVAL;
	}
	*out = (uint32_t)x;
	return 0;
}

static int set_lookback(const struct shell *sh, const char *v)
{
	return set_us(sh, v, &cc_settings()->cfg.lookback_us, 200);
}

static int set_hang(const struct shell *sh, const char *v)
{
	return set_us(sh, v, &cc_settings()->cfg.hang_us, 10000);
}

static int set_max(const struct shell *sh, const char *v)
{
	return set_us(sh, v, &cc_settings()->cfg.max_capture_us, 20000);
}

static int set_gate(const struct shell *sh, const char *v, bool *out)
{
	struct cc_settings *s = cc_settings();
	int ret = parse_onoff(sh, v, out);

	if (ret == 0 && esp_sdr_chancap_active()) {
		ret = esp_sdr_chancap_set_gates(s->cfg.wb_gate, s->cfg.ch_gate);
	}
	return ret;
}

static int set_wbgate(const struct shell *sh, const char *v)
{
	return set_gate(sh, v, &cc_settings()->cfg.wb_gate);
}

static int set_chgate(const struct shell *sh, const char *v)
{
	return set_gate(sh, v, &cc_settings()->cfg.ch_gate);
}

static int set_rxgain(const struct shell *sh, const char *v)
{
	long g = -1;

	if (strcmp(v, "auto") != 0 && app_shell_long(sh, v, 0, esp_sdr_rx_gain_max(), &g) != 0) {
		return -EINVAL;
	}
	cc_settings()->rx_gain = (int)g;
	return 0;
}

static int set_bw(const struct shell *sh, const char *v)
{
	long x;

	if (app_shell_long(sh, v, -1, 69, &x) != 0) {
		return -EINVAL;
	}
	cc_settings()->rx_bw_mhz = (int)x;
	return 0;
}

static const struct setting {
	const char *key, *help;
	int (*set)(const struct shell *sh, const char *value);
} settings[] = {
	{"first", "<Hz>: centre of channel 0", set_first},
	{"spacing", "<Hz>: channel spacing", set_spacing},
	{"channels", "<n>: channels", set_channels},
	{"rate", "<Hz>: per-channel sample rate", set_rate},
	{"wb", "<dB>: wideband gate threshold above the noise floor (live)", set_wb},
	{"ch", "<dB>: per-channel threshold above the noise floor (live)", set_ch},
	{"lookback", "<us>: samples before a trigger kept", set_lookback},
	{"hang", "<us>: hold after the power drops", set_hang},
	{"max", "<us>: longest capture / chunk length", set_max},
	{"wbgate", "on|off: wideband gate (live)", set_wbgate},
	{"chgate", "on|off: per-channel gate, off streams chunks (live)", set_chgate},
	{"rxgain", "auto|<index>: hardware AGC or a fixed gain index", set_rxgain},
	{"bw", "<MHz>: receive filter bandwidth, 0 widest, -1 the PHY's", set_bw},
};

/* set [<key> <value> ...]: settings, applied in order; without arguments the keys. */
static int cmd_set(const struct shell *sh, size_t argc, char **argv)
{
	if (argc == 1) {
		ARRAY_FOR_EACH(settings, k) {
			shell_print(sh, "  %-9s %s", settings[k].key, settings[k].help);
		}
		return 0;
	}
	if ((argc - 1U) % 2U != 0U) {
		shell_error(sh, "usage: set <key> <value> [<key> <value> ...]");
		return -EINVAL;
	}
	for (size_t a = 1; a + 1U < argc; a += 2U) {
		const struct setting *st = NULL;
		int ret;

		ARRAY_FOR_EACH(settings, k) {
			if (strcmp(argv[a], settings[k].key) == 0) {
				st = &settings[k];
			}
		}
		if (st == NULL) {
			shell_error(sh, "unknown key %s (chancap set lists them)", argv[a]);
			return -EINVAL;
		}
		ret = st->set(sh, argv[a + 1U]);
		shell_print(sh, "%s %s: %d", argv[a], argv[a + 1U], ret);
		if (ret != 0) {
			return ret;
		}
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(chancap_cmds,
			       SHELL_CMD(start, NULL, "tune to the grid and capture", cmd_start),
			       SHELL_CMD(stop, NULL, "stop capturing", cmd_stop),
			       SHELL_CMD(status, NULL, "settings, plan and counters", cmd_status),
			       SHELL_CMD_ARG(bench, NULL, "[pairs]: time the channelizer (stopped)",
					     cmd_bench, 1, 1),
			       SHELL_CMD_ARG(set, NULL,
					     "[<key> <value> ...]: settings (without arguments: the keys)",
					     cmd_set, 1, 16),
			       SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(chancap, &chancap_cmds, "channelized burst capture", NULL);
