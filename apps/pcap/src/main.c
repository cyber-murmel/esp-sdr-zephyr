/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The WHY2025 badge's ESP32-P4 as a burst sniffer (doc/pcap.md): its ESP32-C6 runs
 * apps/osmosdr_serial over SDIO as the radio, this chip demodulates and decodes what it
 * receives and streams pcapng over USB (the shell on CDC-ACM, DFU, the stream on a vendor
 * bulk IN endpoint). Here: the settings and their requests (esdr_proto.h's to the radio,
 * pcap_proto.h's), the capture's start, stop and alternation (a control work queue), the time
 * base, the shell's commands, the counters.
 */

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "app.h"
#include "app_clock.h"
#include "app_usb.h"
#include "output.h"
#include "radio.h"
#include "usb_pcap.h"

LOG_MODULE_REGISTER(pcap, LOG_LEVEL_INF);

#define CTL_STACK 3072
#define CTL_PRIO  6
/* The channel mode's rate unless the host chooses: the C6's paced receive at its top. */
#define CHANNEL_RATE_HZ 2000000U
/*
 * The channel mode's gain when the host leaves it automatic: the PHY's own gain control and
 * gain 64 and above lost packets at the desk (2026-10-27: 74 and 100 in 10 s on channel 37,
 * gain 60 161 to 214). The C6's digital gain, whatever the analog one: 0 dB (24 below its 8-bit
 * default) leaves the floor some 37 dB under full scale, still a few steps of the 8-bit items,
 * and the nearest transmitters short of clipping (on channel 37, 1937 Bluetooth LE packets in
 * 15 s against 1691 at 18 dB; an 802.15.4 sender on the desk decoded only below 18 dB).
 */
#define CHANNEL_GAIN  60U
#define CHANNEL_DGAIN 0U

struct app_cfg app_cfg = {
	.mode = PCAP_MODE_SURVEY,
	.survey_hz = 2440000000ULL,
	/* Bluetooth LE advertising channel 37. */
	.channel_hz = 2402000000ULL,
	.channel_rate_hz = CHANNEL_RATE_HZ,
	.rx_gain = ESDR_GAIN_AUTO,
	.channels = 40,
	.open_ratio = 4.0f,
	.mask = {UINT32_MAX, UINT32_MAX, UINT32_MAX},
	/* Threshold 2 (3 dB) over the quiet baseline, a window before and after, the link's
	 * whole budget (the servo keeps 5 to 10 % headroom). */
	.gapless = {.ratio_x16 = 32, .pre = 1, .post = 1},
	.dec = {.flags = PCAP_DEC_DEFAULT, .min_burst_us = 20, .max_bits = PCAP_BURST_BITS_MAX},
};

struct app_stats app_stats;

/* ---- time ---- */

static struct k_spinlock tb_lock;
static struct {
	/* The host's epoch at an uptime (0, 0 until it says: time since boot). */
	uint64_t epoch_ns, set_ns;
	/* The axis time of a run's first block and the host time it came at. */
	uint64_t anchor_axis_ns, anchor_host_ns;
	bool anchored;
} tb;

static uint64_t uptime_ns(void)
{
	return k_ticks_to_ns_floor64(k_uptime_ticks());
}

void timebase_set(uint64_t epoch_ns)
{
	k_spinlock_key_t key = k_spin_lock(&tb_lock);

	tb.epoch_ns = epoch_ns;
	tb.set_ns = uptime_ns();
	k_spin_unlock(&tb_lock, key);
}

uint64_t timebase_now_ns(void)
{
	k_spinlock_key_t key = k_spin_lock(&tb_lock);
	uint64_t t = tb.epoch_ns + (uptime_ns() - tb.set_ns);

	k_spin_unlock(&tb_lock, key);
	return t;
}

void timebase_anchor(uint64_t axis_ns)
{
	uint64_t now = timebase_now_ns();
	k_spinlock_key_t key = k_spin_lock(&tb_lock);

	tb.anchor_axis_ns = axis_ns;
	tb.anchor_host_ns = now;
	tb.anchored = true;
	k_spin_unlock(&tb_lock, key);
}

uint64_t timebase_ns(uint64_t axis_ns)
{
	k_spinlock_key_t key = k_spin_lock(&tb_lock);
	uint64_t t = tb.anchored ? tb.anchor_host_ns + (axis_ns - tb.anchor_axis_ns) : 0U;
	bool anchored = tb.anchored;

	k_spin_unlock(&tb_lock, key);
	return anchored ? t : timebase_now_ns();
}

/* ---- the radio ---- */

static int radio_mode(uint16_t mode)
{
	return radio_request(ESDR_REQ_SET_MODE, mode, NULL, 0, NULL, 0);
}

static int radio_freq(uint64_t hz)
{
	uint8_t le[8];

	sys_put_le64(hz, le);
	return radio_request(ESDR_REQ_SET_FREQ, 0, le, sizeof(le), NULL, 0);
}

/* Where the radio is actually centred (its LO residual in), or @p dflt. */
static uint64_t radio_centre(uint64_t dflt)
{
	struct esdr_state st;
	int ret = radio_request(ESDR_REQ_GET_STATE, 0, NULL, 0, &st, sizeof(st));

	return ret >= (int)sizeof(st) ? sys_le64_to_cpu(st.center_hz) : dflt;
}

/* ---- the capture ---- */

static struct k_work_q ctl_q;
static K_THREAD_STACK_DEFINE(ctl_stack, CTL_STACK);
static bool running;
static uint16_t cur_mode;

static void stop_mode(void)
{
	(void)radio_mode(ESDR_MODE_OFF);
	survey_stop();
	radio_set_sink(NULL);
}

/* The C6's rate in the survey. */
static uint32_t survey_rate(void)
{
	return (app_cfg.mode_flags & PCAP_MODE_F_HALF) != 0U ? ESDR_GAPLESS_RATE_HZ / 2U
							     : ESDR_GAPLESS_RATE_HZ;
}

static int start_mode(uint16_t mode)
{
	uint64_t centre;
	int ret;

	(void)radio_mode(ESDR_MODE_OFF);
	if (mode == PCAP_MODE_CHANNEL) {
		(void)radio_request(ESDR_REQ_SET_RX_GAIN,
				    app_cfg.rx_gain == ESDR_GAIN_AUTO ? CHANNEL_GAIN : app_cfg.rx_gain,
				    NULL, 0, NULL, 0);
		(void)radio_request(ESDR_REQ_SET_DIGITAL_GAIN, CHANNEL_DGAIN, NULL, 0, NULL, 0);
	} else {
		/* Automatic in the survey: the C6's gapless gain servo keeps the peaks' headroom. */
		(void)radio_request(ESDR_REQ_SET_RX_GAIN, app_cfg.rx_gain, NULL, 0, NULL, 0);
	}
	if (mode == PCAP_MODE_SURVEY) {
		const bool half = (app_cfg.mode_flags & PCAP_MODE_F_HALF) != 0U;
		struct esdr_gapless_cfg g = app_cfg.gapless;
		uint16_t fl = sys_le16_to_cpu(g.flags) & ~(ESDR_GAPLESS_F_WIDE | ESDR_GAPLESS_F_HALF);

		/* The C6 sorts by width at 80 MS/s only. */
		if (half) {
			fl |= ESDR_GAPLESS_F_HALF;
		} else if ((app_cfg.mode_flags & PCAP_MODE_F_C6_WIDE) != 0U) {
			fl |= ESDR_GAPLESS_F_WIDE;
		}
		g.flags = sys_cpu_to_le16(fl);
		ret = radio_freq(app_cfg.survey_hz);
		if (ret == 0) {
			ret = radio_request(ESDR_REQ_SET_GAPLESS, 0, &g, sizeof(g), NULL, 0);
		}
		if (ret < 0) {
			return ret;
		}
		centre = radio_centre(app_cfg.survey_hz);
		ret = survey_start(centre, survey_rate());
		if (ret != 0) {
			return ret;
		}
		radio_set_sink(survey_block);
		ret = radio_mode(ESDR_MODE_GAPLESS);
	} else {
		uint64_t freq = app_cfg.channel_hz != 0U ? app_cfg.channel_hz : app_cfg.survey_hz;
		uint32_t rate = app_cfg.channel_rate_hz != 0U ? app_cfg.channel_rate_hz
							       : CHANNEL_RATE_HZ;
		uint8_t le[4];

		sys_put_le32(rate, le);
		ret = radio_freq(freq);
		if (ret == 0) {
			ret = radio_request(ESDR_REQ_SET_SAMPLE_RATE, 0, le, sizeof(le), NULL, 0);
		}
		if (ret == 0) {
			ret = radio_request(ESDR_REQ_SET_FORMAT, 8, NULL, 0, NULL, 0);
		}
		if (ret < 0) {
			return ret;
		}
		centre = radio_centre(freq);
		ret = stream_narrow(centre, rate);
		if (ret != 0) {
			return ret;
		}
		radio_set_sink(survey_block);
		ret = radio_mode(ESDR_MODE_RX);
	}
	if (ret < 0) {
		stop_mode();
		return ret;
	}
	cur_mode = mode;
	return 0;
}

static void alt_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(alt_work, alt_fn);

static bool alternating(void)
{
	return app_cfg.survey_ms != 0U && app_cfg.channel_ms != 0U;
}

/*
 * The settings can start: a capture that runs the survey needs a bank the filterbank has (16
 * channels at 40 MS/s would be M = 8). Checked before the start is queued, so that the host and
 * the shell hear of it.
 */
static int start_check(void)
{
	if (app_cfg.mode != PCAP_MODE_SURVEY && !alternating()) {
		return 0;
	}
	return survey_check(survey_rate());
}

static void alt_fn(struct k_work *work)
{
	uint16_t next = cur_mode == PCAP_MODE_SURVEY ? PCAP_MODE_CHANNEL : PCAP_MODE_SURVEY;

	ARG_UNUSED(work);
	if (!running || !alternating()) {
		return;
	}
	stop_mode();
	if (start_mode(next) != 0) {
		LOG_ERR("switching to mode %u failed", next);
	}
	(void)k_work_reschedule_for_queue(
		&ctl_q, &alt_work,
		K_MSEC(cur_mode == PCAP_MODE_SURVEY ? app_cfg.survey_ms : app_cfg.channel_ms));
}

static void capture_stop(void)
{
	(void)k_work_cancel_delayable(&alt_work);
	if (!running) {
		return;
	}
	stop_mode();
	output_stop();
	running = false;
	LOG_INF("capture stopped");
}

static void start_fn(struct k_work *work)
{
	int ret;

	ARG_UNUSED(work);
	capture_stop();
	memset(&app_stats, 0, sizeof(app_stats));
	output_start();
	running = true;
	ret = start_mode(app_cfg.mode);
	if (ret != 0) {
		LOG_ERR("capture start: %d", ret);
		output_stop();
		running = false;
		return;
	}
	LOG_INF("capture started, mode %u", cur_mode);
	if (alternating()) {
		(void)k_work_reschedule_for_queue(
			&ctl_q, &alt_work,
			K_MSEC(cur_mode == PCAP_MODE_SURVEY ? app_cfg.survey_ms
							    : app_cfg.channel_ms));
	}
}
static K_WORK_DEFINE(start_work, start_fn);

static void stop_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	capture_stop();
}
static K_WORK_DEFINE(stop_work, stop_fn);

static bool link_up;

static void link_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	if (!link_up) {
		/* The host went: the capture ends. */
		capture_stop();
	} else if (running) {
		/* A host that came during a capture gets the section's header first. */
		output_section();
	}
}
static K_WORK_DEFINE(link_work, link_fn);

/* ---- USB ---- */

void usb_pcap_link(bool up)
{
	link_up = up;
	(void)k_work_submit_to_queue(&ctl_q, &link_work);
}

void usb_pcap_in_done(uint8_t *data, size_t len, int err)
{
	output_in_done(data, len, err);
}

static int set_channelizer(const uint8_t *data, size_t len)
{
	struct esdr_chan_cfg c;
	uint16_t m;

	if (len != sizeof(c)) {
		return -EINVAL;
	}
	memcpy(&c, data, sizeof(c));
	m = sys_le16_to_cpu(c.channels);
	if (m != 16U && m != 32U && m != 40U && m != 64U && m != 80U) {
		return -EINVAL;
	}
	app_cfg.channels = m;
	app_cfg.open_ratio = c.open_db_x10 != 0U
				     ? powf(10.0f, sys_le16_to_cpu(c.open_db_x10) / 100.0f)
				     : 4.0f;
	for (int k = 0; k < 3; k++) {
		app_cfg.mask[k] = sys_le32_to_cpu(c.mask[k]);
	}
	return 0;
}

static int set_mode(const uint8_t *data, size_t len)
{
	struct pcap_mode_cfg c;

	if (len != sizeof(c)) {
		return -EINVAL;
	}
	memcpy(&c, data, sizeof(c));
	if (sys_le16_to_cpu(c.mode) > PCAP_MODE_CHANNEL) {
		return -EINVAL;
	}
	app_cfg.mode = sys_le16_to_cpu(c.mode);
	app_cfg.mode_flags = sys_le16_to_cpu(c.flags);
	app_cfg.channel_rate_hz = sys_le32_to_cpu(c.channel_rate_hz);
	app_cfg.channel_hz = sys_le64_to_cpu(c.channel_freq_hz);
	app_cfg.survey_ms = sys_le32_to_cpu(c.survey_ms);
	app_cfg.channel_ms = sys_le32_to_cpu(c.channel_ms);
	return 0;
}

int usb_pcap_control_out(uint8_t req, uint16_t value, const uint8_t *data, size_t len)
{
	int ret;

	switch (req) {
	case PCAP_REQ_START: {
		int ret = start_check();

		if (ret != 0) {
			return ret;
		}
		(void)k_work_submit_to_queue(&ctl_q, &start_work);
		return 0;
	}
	case PCAP_REQ_STOP:
		(void)k_work_submit_to_queue(&ctl_q, &stop_work);
		return 0;
	case PCAP_REQ_SET_TIME:
		if (len != 8U) {
			return -EINVAL;
		}
		timebase_set(sys_get_le64(data));
		return 0;
	case PCAP_REQ_SET_DECODERS: {
		struct pcap_decoders d;

		if (len != sizeof(d)) {
			return -EINVAL;
		}
		memcpy(&d, data, sizeof(d));
		app_cfg.dec.flags = sys_le32_to_cpu(d.flags);
		app_cfg.dec.min_burst_us = sys_le16_to_cpu(d.min_burst_us);
		app_cfg.dec.max_bits = MIN(sys_le16_to_cpu(d.max_bits), PCAP_BURST_BITS_MAX);
		return 0;
	}
	case PCAP_REQ_ADD_AA:
		if (value == 2U) {
			return decoder_aa(2U, 0U, 0U);
		}
		if (len != sizeof(struct pcap_aa)) {
			return -EINVAL;
		}
		return decoder_aa(value, sys_get_le32(data), sys_get_le32(data + 4));
	case PCAP_REQ_SET_MODE:
		return running ? -EBUSY : set_mode(data, len);
	case ESDR_REQ_SET_CHANNELIZER:
		return running ? -EBUSY : set_channelizer(data, len);
	case ESDR_REQ_SET_GAPLESS:
		/* Taken at the start, with the width sorting the mode asks for. */
		if (len != sizeof(app_cfg.gapless)) {
			return -EINVAL;
		}
		memcpy(&app_cfg.gapless, data, len);
		return 0;
	case ESDR_REQ_SET_FREQ:
		if (len != 8U) {
			return -EINVAL;
		}
		if (running) {
			return -EBUSY;
		}
		app_cfg.survey_hz = sys_get_le64(data);
		break;
	case ESDR_REQ_SET_RX_GAIN:
		app_cfg.rx_gain = value;
		break;
	case ESDR_REQ_USB_TO_C6:
		return radio_usb_to_c6(value);
	default:
		break;
	}
	ret = radio_request(req, value, data, (uint16_t)len, NULL, 0);
	return ret < 0 ? ret : 0;
}

static void fill_stats(struct pcap_stats *s)
{
	struct output_stats os;

	output_get_stats(&os);
	*s = (struct pcap_stats){
		.seg_blocks = app_stats.seg_blocks,
		.seg_dropped = app_stats.seg_dropped,
		.other_blocks = app_stats.other_blocks,
		.pairs = app_stats.pairs,
		.chan_cycles = app_stats.chan_cycles,
		.bursts = app_stats.bursts,
		.bursts_lost = app_stats.bursts_lost,
		.bursts_wide = app_stats.bursts_wide,
		.decoded = app_stats.decoded,
		.dec_cycles = app_stats.dec_cycles,
		.usb_bytes = os.usb_bytes,
		.ring_peak = os.peak,
		.cpu_mhz = (uint32_t)MAX(app_clock_cpu_max(), 0),
		.running = running ? 1U : 0U,
		.mode = cur_mode,
		.channel_samples = app_stats.channel_samples,
		.channel_gaps = app_stats.channel_gaps,
		.c6_wide = app_stats.c6_wide,
		.generic = app_stats.generic,
		.generic_skipped = app_stats.generic_skipped,
		.generic_wide = app_stats.generic_wide,
		.generic_cycles = app_stats.generic_cycles,
	};
	for (unsigned int k = 0; k < PCAP_IF_COUNT; k++) {
		s->packets[k] = os.packets[k];
		s->dropped[k] = os.dropped[k];
	}
}

int usb_pcap_control_in(uint8_t req, uint16_t value, uint8_t *buf, size_t max)
{
	if (req == PCAP_REQ_GET_STATS) {
		struct pcap_stats s;

		fill_stats(&s);
		memcpy(buf, &s, MIN(max, sizeof(s)));
		return (int)MIN(max, sizeof(s));
	}
	if (req == ESDR_REQ_SET_CHANNELIZER) {
		struct esdr_chan_stats cs = {
			.seg_blocks = app_stats.seg_blocks,
			.seg_dropped = app_stats.seg_dropped,
			.pairs = app_stats.pairs,
			.cycles = app_stats.chan_cycles,
			.cpu_mhz = (uint32_t)MAX(app_clock_cpu_max(), 0),
		};

		memcpy(buf, &cs, MIN(max, sizeof(cs)));
		return (int)MIN(max, sizeof(cs));
	}
	return radio_request(req, value, NULL, 0, buf, (uint16_t)MIN(max, ESDR_SER_MAX_DATA));
}

/* ---- shell ---- */

static void print_stats(const struct shell *sh)
{
	struct pcap_stats s;
	struct radio_stats rs;

	fill_stats(&s);
	radio_get_stats(&rs);
	shell_print(sh, "%s, mode %u; radio blocks %u (dropped %u, resyncs %u)",
		    s.running ? "running" : "stopped", s.mode, rs.blocks, rs.dropped, rs.resyncs);
	shell_print(sh, "segment blocks %u (dropped %u, other %u), pairs %llu, %u cycles a pair",
		    s.seg_blocks, s.seg_dropped, s.other_blocks, (unsigned long long)s.pairs,
		    s.pairs != 0U ? (unsigned int)(s.chan_cycles / s.pairs) : 0U);
	shell_print(sh, "channel samples %llu (gaps %u); bursts %u (lost %u, wideband %u, C6 "
		    "width records %u), %u free, %u waiting",
		    (unsigned long long)s.channel_samples, s.channel_gaps, s.bursts, s.bursts_lost,
		    s.bursts_wide, s.c6_wide, burst_free_count(), decoder_backlog());
	shell_print(sh, "decoded %u, %llu cycles each; packets BLE %u (dropped %u), 802.15.4 %u "
		    "(%u), bursts %u (%u); USB %llu bytes, %u buffers out at most",
		    s.decoded, s.decoded != 0U ? (unsigned long long)(s.dec_cycles / s.decoded) : 0ULL,
		    s.packets[PCAP_IF_BLE], s.dropped[PCAP_IF_BLE], s.packets[PCAP_IF_WPAN],
		    s.dropped[PCAP_IF_WPAN], s.packets[PCAP_IF_BURST], s.dropped[PCAP_IF_BURST],
		    (unsigned long long)s.usb_bytes, s.ring_peak);
	shell_print(sh, "generic analysis: %u bursts, %llu cycles each; skipped %u, filling the "
		    "channel %u", s.generic,
		    s.generic != 0U ? (unsigned long long)(s.generic_cycles / s.generic) : 0ULL,
		    s.generic_skipped, s.generic_wide);
}

static int cmd_start(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		if (strcmp(argv[1], "survey") == 0) {
			app_cfg.mode = PCAP_MODE_SURVEY;
			app_cfg.survey_ms = app_cfg.channel_ms = 0;
		} else if (strcmp(argv[1], "channel") == 0) {
			app_cfg.mode = PCAP_MODE_CHANNEL;
			app_cfg.survey_ms = app_cfg.channel_ms = 0;
		} else if (strcmp(argv[1], "alt") == 0) {
			app_cfg.mode = PCAP_MODE_SURVEY;
			app_cfg.survey_ms = argc > 2 ? (uint32_t)atoi(argv[2]) : 2000U;
			app_cfg.channel_ms = argc > 3 ? (uint32_t)atoi(argv[3]) : 2000U;
		} else {
			shell_error(sh, "survey, channel or alt [survey_ms channel_ms]");
			return -EINVAL;
		}
	}
	if (start_check() != 0) {
		shell_error(sh, "no survey of %u channels at %u MS/s (16 channels need 80 MS/s)",
			    app_cfg.channels, survey_rate() / 1000000U);
		return -EINVAL;
	}
	(void)k_work_submit_to_queue(&ctl_q, &start_work);
	shell_print(sh, "starting");
	return 0;
}

static int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	(void)k_work_submit_to_queue(&ctl_q, &stop_work);
	shell_print(sh, "stopping");
	return 0;
}

static int cmd_freq(const struct shell *sh, size_t argc, char **argv)
{
	double mhz;

	if (running) {
		shell_error(sh, "stop first");
		return -EBUSY;
	}
	if (argc > 1) {
		mhz = strtod(argv[1], NULL);
		if (mhz < 100.0 || mhz > 6000.0) {
			return -EINVAL;
		}
		if (strcmp(argv[0], "chfreq") == 0) {
			app_cfg.channel_hz = (uint64_t)llround(mhz * 1e6);
		} else {
			app_cfg.survey_hz = (uint64_t)llround(mhz * 1e6);
		}
	}
	shell_print(sh, "survey %llu Hz, channel %llu Hz",
		    (unsigned long long)app_cfg.survey_hz, (unsigned long long)app_cfg.channel_hz);
	return 0;
}

static int cmd_gain(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		app_cfg.rx_gain = strcmp(argv[1], "auto") == 0 ? ESDR_GAIN_AUTO
							       : (uint16_t)atoi(argv[1]);
	}
	shell_print(sh, "gain %u", app_cfg.rx_gain);
	return 0;
}

static int cmd_chan(const struct shell *sh, size_t argc, char **argv)
{
	if (running) {
		shell_error(sh, "stop first");
		return -EBUSY;
	}
	if (argc > 1) {
		int m = atoi(argv[1]);

		if (m != 16 && m != 32 && m != 40 && m != 64 && m != 80) {
			shell_error(sh, "16, 32, 40, 64 or 80 channels");
			return -EINVAL;
		}
		app_cfg.channels = (uint16_t)m;
	}
	if (argc > 2) {
		app_cfg.open_ratio = powf(10.0f, (float)strtod(argv[2], NULL) / 10.0f);
	}
	shell_print(sh, "%u channels, opening %.1f dB over the floor", app_cfg.channels,
		    (double)(10.0f * log10f(app_cfg.open_ratio)));
	return 0;
}

static int cmd_dec(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		app_cfg.dec.flags = (uint32_t)strtoul(argv[1], NULL, 16);
	}
	shell_print(sh, "decoders 0x%04x (ble 1, wpan 2, generic 4, wideband 8, bad CRC 10, ble data "
		    "20, comments 40)", app_cfg.dec.flags);
	return 0;
}

static int cmd_wide(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		if (atoi(argv[1]) != 0) {
			app_cfg.mode_flags |= PCAP_MODE_F_C6_WIDE;
		} else {
			app_cfg.mode_flags &= ~PCAP_MODE_F_C6_WIDE;
		}
	}
	shell_print(sh, "the C6's width sorting %s",
		    (app_cfg.mode_flags & PCAP_MODE_F_C6_WIDE) != 0U ? "on" : "off");
	return 0;
}

static int cmd_rate(const struct shell *sh, size_t argc, char **argv)
{
	/* An alternating capture reads it at every switch. */
	if (argc > 1 && running) {
		shell_error(sh, "stop first");
		return -EBUSY;
	}
	if (argc > 1) {
		int mhz = atoi(argv[1]);

		if (mhz == 40) {
			app_cfg.mode_flags |= PCAP_MODE_F_HALF;
		} else if (mhz == 80) {
			app_cfg.mode_flags &= ~PCAP_MODE_F_HALF;
		} else {
			return -EINVAL;
		}
	}
	shell_print(sh, "the survey at %u MS/s",
		    (app_cfg.mode_flags & PCAP_MODE_F_HALF) != 0U ? 40U : 80U);
	return 0;
}

static int cmd_ratio(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		double r = strtod(argv[1], NULL);

		if (r <= 1.0625 || r > 4000.0) {
			return -EINVAL;
		}
		app_cfg.gapless.ratio_x16 = sys_cpu_to_le16((uint16_t)lround(r * 16.0));
	}
	shell_print(sh, "gapless threshold floor %.2f over the baseline",
		    sys_le16_to_cpu(app_cfg.gapless.ratio_x16) / 16.0);
	return 0;
}

static int cmd_stats(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	print_stats(sh);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(pcap_cmds,
	SHELL_CMD_ARG(start, NULL, "Start a capture: [survey|channel|alt [survey_ms channel_ms]]",
		      cmd_start, 1, 3),
	SHELL_CMD(stop, NULL, "Stop the capture", cmd_stop),
	SHELL_CMD_ARG(freq, NULL, "The survey's centre: [MHz]", cmd_freq, 1, 1),
	SHELL_CMD_ARG(chfreq, NULL, "The channel mode's frequency: [MHz]", cmd_freq, 1, 1),
	SHELL_CMD_ARG(gain, NULL, "Receive gain: [index|auto]", cmd_gain, 1, 1),
	SHELL_CMD_ARG(chan, NULL, "Channelizer: [16|32|40|64|80 [open dB]]", cmd_chan, 1, 2),
	SHELL_CMD_ARG(dec, NULL, "Decoders: [hex flags]", cmd_dec, 1, 1),
	SHELL_CMD_ARG(wide, NULL, "The C6's width sorting: [0|1]", cmd_wide, 1, 1),
	SHELL_CMD_ARG(rate, NULL, "The survey's rate: [80|40] MS/s", cmd_rate, 1, 1),
	SHELL_CMD_ARG(ratio, NULL, "Gapless threshold floor over the baseline: [ratio]",
		      cmd_ratio, 1, 1),
	SHELL_CMD(stats, NULL, "Counters", cmd_stats),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(pcap, &pcap_cmds, "Burst capture to pcapng", NULL);

int main(void)
{
	const struct k_work_queue_config ctl_cfg = {.name = "pcap_ctl"};

	LOG_INF("CPU %d MHz", app_clock_cpu_max());
	k_work_queue_start(&ctl_q, ctl_stack, K_THREAD_STACK_SIZEOF(ctl_stack), CTL_PRIO, &ctl_cfg);
	(void)radio_init();
	decoder_init();
	(void)app_usb_init();
	for (;;) {
		k_sleep(K_SECONDS(10));
		if (running) {
			LOG_INF("bursts %u (lost %u, wideband %u), decoded %u, waiting %u",
				app_stats.bursts, app_stats.bursts_lost, app_stats.bursts_wide,
				app_stats.decoded, decoder_backlog());
		}
	}
	return 0;
}
