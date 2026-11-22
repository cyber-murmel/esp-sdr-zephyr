/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The "tx" shell command:
 *   tx boot                      addresses per group, run time layout checks, canary
 *   tx init [MHz]                esp_sdr_init() and the first tune (2437) with the network up
 *   tx tone <20|40> <s> [rf] [amp 1..40] [k 1..175] [fmt iq8|w32] [verify 0|1]
 *                                stream a tone (k cycles per 351 samples) as packets from the
 *                                injector through the txs path; without rf the transmit chain
 *                                stays off
 *   tx kern ref|fast|pie | tx inject ... | tx cfg ... | tx stop [now] | tx hang [lock] | tx stat
 *   tx net                       the network side: subscriber, run, the hook's counts, statuses, pings
 *   tx arm <minutes>             a run from the network may ask for RF for that long (tx disarm: not)
 * (tx tone and tx inject need CONFIG_APP_TXS_INJECT.)
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/ethernet/eth_dwmac_fast.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_if.h>
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>
#include <soc/hp_system_reg.h>

#include <esp_sdr/esp_sdr.h>

#include "tx_boot.h"
#include "tx_inject.h"
#include "tx_net.h"
#include "tx_shell.h"
#include "tx_stream.h"
#include "tx_tone.h"
#include "txs_expand.h"
#include "txs_limits.h"

extern bool esp_sdr_ready;
bool esp_sdr_s31_dump_ready(void);

#define MAX_MMC 32
#define DEFAULT_MHZ 2437U
#define DEFAULT_AMP TXTG_AMP
#define MAX_AMP 40
#define DEFAULT_K TXTG_K
#define MAX_K 175
#define MAX_SECONDS 3600U

static const struct device *eth_dev;
static struct net_if *eth_iface;
/* tuned_mhz: what tx init tuned successfully; the RF gate asks the library itself (esp_sdr_get_freq()) */
static unsigned int tuned_mhz;
static bool inited;

struct mac_snap {
	uint32_t offered;
	struct dwmac_rx_stats ds;
	uint32_t mmc[MAX_MMC];
	const char *key[MAX_MMC];
	unsigned int nmmc;
	uint32_t net_rx, net_rx_err;
	int64_t ms;
};

static void snap(struct mac_snap *s)
{
	const struct ethernet_api *api = eth_dev->api;
	struct net_stats_eth *st = NULL;

	memset(s, 0, sizeof(*s));
	s->ms = k_uptime_get();
	s->offered = tx_hook_offered();
	dwmac_rx_stats_get(eth_dev, &s->ds);
	if (api->get_stats_type != NULL) {
		st = api->get_stats_type(eth_dev, eth_iface, ETHERNET_STATS_TYPE_VENDOR);
	}
	for (unsigned int i = 0; st != NULL && st->vendor != NULL && i < MAX_MMC &&
				 st->vendor[i].key != NULL; i++) {
		s->mmc[i] = st->vendor[i].value;
		s->key[i] = st->vendor[i].key;
		s->nmmc = i + 1U;
	}
	s->net_rx = eth_iface->stats.bytes.received;
	s->net_rx_err = eth_iface->stats.processing_error;
}

/* What the MAC and the network counted between two snapshots. */
static void snap_diff(const struct shell *sh, const char *tag, const struct mac_snap *a,
		      const struct mac_snap *b)
{
	unsigned int shown = 0U;

	shell_print(sh, "%s: %u ms, hook offered %u frames (the stack took what the hook did not claim), net rx bytes %u, net processing "
		    "errors %u, DMA ring: irqs %u, pool starved %u, missed %u, FIFO overflow %u",
		    tag, (unsigned int)(b->ms - a->ms), b->offered - a->offered, b->net_rx - a->net_rx,
		    b->net_rx_err - a->net_rx_err, b->ds.irqs - a->ds.irqs, b->ds.starved - a->ds.starved,
		    b->ds.missed - a->ds.missed, b->ds.fifo_overflow - a->ds.fifo_overflow);
	for (unsigned int i = 0; i < b->nmmc && i < a->nmmc; i++) {
		if (b->mmc[i] != a->mmc[i]) {
			if (shown++ == 0U) {
				shell_print(sh, "%s: MAC counters that changed (errors are the ones to read):", tag);
			}
			shell_print(sh, "    %-28s +%u", b->key[i], b->mmc[i] - a->mmc[i]);
		}
	}
}

static int cmd_boot(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (tx_stream_active()) {
		shell_error(sh, "tx boot: a stream runs");
		return -EBUSY;
	}
	return tx_boot_report(sh);
}

static int cmd_init(const struct shell *sh, size_t argc, char **argv)
{
	const unsigned int mhz = argc > 1 ? (unsigned int)strtoul(argv[1], NULL, 0) : DEFAULT_MHZ;
	struct mac_snap a, b;
	uint32_t own0, own1, own2, own3;
	bool dr0, dr1, dr2;
	int64_t t0;
	unsigned int bad;
	int ret, ret2 = 0;

	if (tx_stream_active()) {
		shell_error(sh, "tx init: a stream runs");
		return -EBUSY;
	}
	if (mhz < 2200U || mhz > 2900U) {
		shell_error(sh, "tx init: %u MHz outside 2200 to 2900", mhz);
		return -EINVAL;
	}
	tx_canary_fill(!inited);
	/* not tuned until the tune succeeds: a failing tune may still have moved the hardware */
	tuned_mhz = 0U;
	inited = false;
	snap(&a);
	own0 = sys_read32(HP_SYSTEM_TCM_DATA_DUMP_CTRL_REG);
	dr0 = esp_sdr_s31_dump_ready();
	t0 = k_uptime_get();
	shell_print(sh, "tx init: ownership 0x%08x, dump_ready %d, esp_sdr_init()...", own0, dr0);
	ret = esp_sdr_init();
	own1 = sys_read32(HP_SYSTEM_TCM_DATA_DUMP_CTRL_REG);
	dr1 = esp_sdr_s31_dump_ready();
	shell_print(sh, "tx init: esp_sdr_init() returned %d after %u ms, ownership 0x%08x, dump_ready %d",
		    ret, (unsigned int)(k_uptime_get() - t0), own1, dr1);
	if (ret == 0) {
		const int64_t t1 = k_uptime_get();

		shell_print(sh, "tx init: tune %u MHz (the first tune runs dump_setup() with group 2 as owner)...",
			    mhz);
		ret2 = esp_sdr_set_freq(mhz);
		own2 = sys_read32(HP_SYSTEM_TCM_DATA_DUMP_CTRL_REG);
		dr2 = esp_sdr_s31_dump_ready();
		shell_print(sh, "tx init: esp_sdr_set_freq() returned %d after %u ms, ownership 0x%08x, "
			    "dump_ready %d", ret2, (unsigned int)(k_uptime_get() - t1), own2, dr2);
		if (ret2 == 0) {
			tuned_mhz = mhz;
			inited = true;
		}
	}
	own3 = sys_read32(HP_SYSTEM_TCM_DATA_DUMP_CTRL_REG);
	k_msleep(100);
	snap(&b);
	snap_diff(sh, "tx init", &a, &b);
	bad = tx_canary_check(sh);
	shell_print(sh, "tx init: total %u ms, ownership now 0x%08x: %s", (unsigned int)(k_uptime_get() - t0),
		    own3, ret == 0 && ret2 == 0 && bad == 0U && own3 == 0U &&
		    b.ds.missed == a.ds.missed && b.ds.fifo_overflow == a.ds.fifo_overflow &&
		    b.net_rx_err == a.net_rx_err ? "PASS" : "CHECK (see above)");
	return ret != 0 ? ret : ret2;
}

static const struct txs_kern *kern = &txs_kern_fast;
/* `tx kern` was used: the net runs take that kernel, else the PIE one when it is built */
static bool kern_set;
static uint32_t cfg_lat = 0U, cfg_tol = 8U, cfg_ovh = 800U;

bool tx_shell_inited(void)
{
	return inited && esp_sdr_ready;
}

void tx_shell_cfg(struct tx_cfg *c)
{
	const struct txs_kern *pie = txs_kern_find("pie");

	/* the scalar kernel is slower than line rate bursts: a run from the network takes the PIE kernel unless
	 * `tx kern` chose one, and falls back to the fast one if PIE does not pass its golden check */
	c->kern = kern_set || pie == NULL ? kern : pie;
	/* a kernel chosen with `tx kern` is not replaced: if it fails its golden check the run ends with an error */
	c->fallback = !kern_set && c->kern != &txs_kern_fast ? &txs_kern_fast : NULL;
	c->write_lat = cfg_lat;
	c->tol = cfg_tol;
	c->overhead = cfg_ovh;
}

static const char *const err_names[TX_ERR_N] = {"ok", "short", "type", "time", "size", "stream",
						"port", "align", "big", "index"};
static const char *const reasons[] = {"none", "drain", "now", "hang", "drained", "missed"};
static const char *const kinds[] = {"nothing", "fill step", "zeroing", "verify", "idle", "pull", "flip"};

/* A line is one call into the shell (it redraws the prompt around it) or one printk, built whole. */
struct sink {
	const struct shell *sh;
	char *buf;
	size_t size;
};

static void line(const struct sink *k, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(k->buf, k->size, fmt, ap);
	va_end(ap);
	if (k->sh != NULL) {
		shell_print(k->sh, "%s", k->buf);
	} else {
		printk("%s\n", k->buf);
	}
}

static uint32_t x100(uint64_t num, uint64_t den)
{
	return den == 0U ? 0U : (uint32_t)(num * 100U / den);
}

#define LLU(x) ((unsigned long long)(x))

static void print_stats(const struct sink *k, const char *tag, const struct tx_stats *s)
{
	struct tx_inj_stats is;
	char errs[96] = "";
	uint64_t exp_sys = 0U;
	int64_t ppm = 0;
	const uint32_t cpu_hz = ESP_SDR_CPU_HZ, pk = MAX(s->pk_done, 1U);
	const struct tx_flip_ev *ev = &s->ev;

	if (s->sys_cyc != 0U && s->sys_hz != 0U) {
		/* sys_hz x cpu_cyc / cpu_hz in 64 bits without overflow: whole seconds, then the rest */
		exp_sys = (s->cpu_cyc / cpu_hz) * s->sys_hz + (s->cpu_cyc % cpu_hz) * s->sys_hz / cpu_hz;
		ppm = ((int64_t)exp_sys - (int64_t)s->sys_cyc) * 1000000 / (int64_t)s->sys_cyc;
	}
	for (unsigned int i = 1, n = 0; i < TX_ERR_N; i++) {
		if (s->pk_err[i] != 0U) {
			n += snprintf(errs + n, sizeof(errs) - n, " %s %u", err_names[i], s->pk_err[i]);
		}
	}
	tx_inject_stats(&is);
	line(k, "%s: %s %s %s %u MS/s%s, %u ms, thread %s, stop reason %s, start %d", tag,
	     s->active ? "running" : "ended", s->kern != NULL ? s->kern : "-",
	     s->fmt == TXS_FMT_IQ8 ? "iq8" : "w32", s->msps, s->rf ? " RF ON" : " chain off", s->elapsed_ms,
	     s->thread_ended ? "returned" : (s->active ? "running" : "aborted, it never returned"),
	     reasons[MIN((unsigned int)s->reason, 5U)], s->ret);
	if (s->ret == -EILSEQ) {
		line(k, "%s: THE %s KERNEL FAILED ITS GOLDEN CHECK against the reference: the run did not start", tag,
		     s->kern != NULL ? s->kern : "?");
	} else if (s->kern_fb) {
		line(k, "%s: the PIE kernel failed its golden check: this run used the fast one", tag);
	}
	line(k, "%s: flips %llu (%u cyc per group), late (> %u words) %llu, slack %llu, early %llu, missed %llu",
	     tag, LLU(s->flips), s->pass_cyc, s->tol, LLU(s->late_flips), LLU(s->slack_flips),
	     LLU(s->early_flips), LLU(s->missed_flips));
	line(k, "%s: worst late %llu / early %llu words, stale words %llu; ownership write %u/%u/%u cyc "
	     "(min/mean/max), before the start %u/%u/%u, aimed %u cyc ahead (%s)", tag, LLU(s->late_worst),
	     LLU(s->early_worst), LLU(s->stale_words), s->write_min, (uint32_t)(s->write_sum / MAX(s->write_n, 1U)), s->write_max,
	     s->cal_min, s->cal_mean, s->cal_max, s->write_lat, s->lat_auto ? "the mean, measured" : "set");
	line(k, "%s: spin exit worst %u cyc, %u flips over 64 cyc; the worst was flip %llu at %u ms: %u cyc past "
	     "the aim", tag, s->spin_worst, s->spin_over64, LLU(ev->flip), ev->ms, ev->late);
	line(k, "%s:   entered the flip %d cyc past the open spin's start, open spin ended %d past it, lock "
	     "taken %d past the aim; the turn before: %s%s, %u cyc, %u cyc earlier", tag, ev->entry, ev->open,
	     ev->lock, kinds[MIN(ev->prev_kind & 0xffU, 6U)], (ev->prev_kind & 0x100U) != 0U ? " + publish" : "",
	     ev->prev_cyc, ev->prev_ago);
	line(k, "%s: schedule cost %u zcost %u margin %u lock lead %u chunk %u overhead %u cyc, depth %u words",
	     tag, s->cost, s->zcost, s->margin, s->lock_lead, s->chunk, s->overhead, s->depth);
	line(k, "%s: worst step %u cyc (budget %u), steps %u, %u loops", tag, s->worst_step,
	     s->chunk * s->cost + s->overhead, s->steps, s->loops);
	line(k, "%s: window: placed %llu, lost %llu (after the %llu words before the first packet), "
	     "recovered %llu, dup %llu, late %llu, far %llu", tag, LLU(s->placed),
	     LLU(s->lost - MIN(s->lost, s->align_gap)), LLU(s->align_gap), LLU(s->recovered), LLU(s->dup),
	     LLU(s->late), LLU(s->far));
	line(k, "%s: zeroed %llu words, underruns %u (unfilled %llu words): at the close words of a packet "
	     "waiting %u (oldest %u cyc old), a packet in the ring %u, nothing %u", tag, LLU(s->zeroed),
	     s->underruns, LLU(s->unfilled), s->ur_pend, s->ur_age, s->ur_ring, s->ur_none);
	line(k, "%s: packets taken %llu, done %llu, flushed at the end %llu, in flight at the end %u, parse "
	     "errors%s", tag, LLU(s->pk_taken), LLU(s->pk_done), LLU(s->pk_flushed),
	     s->inflight_end, errs[0] != '\0' ? errs : " none");
	line(k, "%s: cycles: %u.%02u per packet (all fill steps), %u.%02u per word in the kernel, worst "
	     "kernel call %u, zeroed by steps %llu", tag, x100(s->fill_cyc, pk) / 100U,
	     x100(s->fill_cyc, pk) % 100U, x100(s->kern_cyc, MAX(s->kern_words, 1U)) / 100U,
	     x100(s->kern_cyc, MAX(s->kern_words, 1U)) % 100U, s->worst_kern_call, LLU(s->zero_words));
	line(k, "%s: cycles by part (all, per packet): poll %llu (%llu), parse+queue %llu (%llu), fit %llu (%llu)",
	     tag, LLU(s->c_poll), LLU(s->c_poll / pk), LLU(s->c_pull), LLU(s->c_pull / pk), LLU(s->c_fit),
	     LLU(s->c_fit / pk));
	line(k, "%s:   jitter %llu (%llu), zeroing %llu (%llu), status %llu (%llu)", tag, LLU(s->c_jit),
	     LLU(s->c_jit / pk), LLU(s->c_zero), LLU(s->c_zero / pk), LLU(s->c_pub), LLU(s->c_pub / pk));
	line(k, "%s: bank read back %llu words, %llu zero, %llu wrong; status play %llu contig %llu credit %llu "
	     "restart %llu", tag, LLU(s->ver_words), LLU(s->ver_zero), LLU(s->ver_bad), LLU(s->status.play),
	     LLU(s->status.contig), LLU(s->status.credit), LLU(s->status.restart));
	if (s->ver_bad != 0U) {
		line(k, "%s: wrong: %llu are the window's old content; per 512 words from its start %u %u %u %u %u %u %u "
		     "%u; first idx %u +%u 0x%08x not 0x%08x, idx %u +%u 0x%08x not 0x%08x", tag, LLU(s->ver_stale),
		     s->ver_pos[0], s->ver_pos[1], s->ver_pos[2], s->ver_pos[3], s->ver_pos[4], s->ver_pos[5],
		     s->ver_pos[6], s->ver_pos[7], s->ver_ex[0][0], s->ver_ex[0][1], s->ver_ex[0][2],
		     s->ver_ex[0][3], s->ver_ex[1][0], s->ver_ex[1][1], s->ver_ex[1][2], s->ver_ex[1][3]);
	}
	line(k, "%s: injector sent %llu packets (%llu words, %llu words/s), dropped %llu, reordered %llu, "
	     "stalls %llu, ended by abort %llu", tag, LLU(is.sent), LLU(is.words),
	     LLU(is.ms != 0U ? is.words * 1000U / is.ms : 0U), LLU(is.dropped), LLU(is.reordered),
	     LLU(is.stalls), LLU(is.aborted));
	line(k, "%s:   resyncs %llu (%llu words skipped to realign), waits for a free buffer %llu, ring full %llu, credit waits %llu",
	     tag, LLU(is.resyncs), LLU(is.align_words), LLU(is.no_buf), LLU(is.ring_full),
	     LLU(is.credit_waits));
	line(k, "%s: CPU counter against the systimer over %llu cyc: %d ppm (expected %llu, counted %llu "
	     "systimer cycles at %u Hz)%s", tag, LLU(s->cpu_cyc), (int)ppm, LLU(exp_sys), LLU(s->sys_cyc),
	     s->sys_hz, s->dead ? ", DEADMAN FIRED (engine off, chain off, flip thread aborted)" : "");
}

/* From the control thread (tx_ctl): static buffer, the UART shell if there is one. */
void tx_shell_done(const struct tx_stats *s)
{
	static char wbuf[256];
	const struct sink k = {
#if defined(CONFIG_SHELL_BACKEND_SERIAL)
		.sh = shell_backend_uart_get_ptr(),
#endif
		.buf = wbuf, .size = sizeof(wbuf),
	};

	print_stats(&k, "tx done", s);
	line(&k, "tx done: engine off%s, ownership 0x%08x", s->rf ? ", chain off" : "",
	     (unsigned int)sys_read32(HP_SYSTEM_TCM_DATA_DUMP_CTRL_REG));
}

static int cmd_tone(const struct shell *sh, size_t argc, char **argv)
{
	struct tx_cfg c = {.rf = false, .verify = false, .fmt = TXS_FMT_IQ8, .kern = kern,
			   .write_lat = cfg_lat, .tol = cfg_tol, .overhead = cfg_ovh};
	int amp = DEFAULT_AMP, k = DEFAULT_K, ret;

	if (!IS_ENABLED(CONFIG_APP_TXS_INJECT)) {
		shell_error(sh, "tx tone: this image has no injector (the network is the only source): build with "
			    "-DEXTRA_CONF_FILE=injector.conf, see apps/txstream_eth/README.rst");
		return -ENOTSUP;
	}
	/* before anything touches the tone's reference: a running stream is verified against it */
	if (tx_stream_active()) {
		shell_error(sh, "tx tone: a stream runs");
		return -EBUSY;
	}
	if (argc < 3) {
		shell_error(sh, "tx tone <20|40> <seconds> [rf] [amp 1..%d] [k 1..175] [fmt iq8|w32] [verify 0|1]",
			    MAX_AMP);
		return -EINVAL;
	}
	c.msps = (uint32_t)strtoul(argv[1], NULL, 0);
	c.seconds = (uint32_t)strtoul(argv[2], NULL, 0);
	for (size_t i = 3; i < argc; i++) {
		if (strcmp(argv[i], "rf") == 0) {
			c.rf = true;
		} else if (strcmp(argv[i], "amp") == 0 && i + 1 < argc) {
			amp = (int)strtol(argv[++i], NULL, 0);
		} else if (strcmp(argv[i], "k") == 0 && i + 1 < argc) {
			k = (int)strtol(argv[++i], NULL, 0);
		} else if (strcmp(argv[i], "fmt") == 0 && i + 1 < argc) {
			c.fmt = strcmp(argv[++i], "w32") == 0 ? TXS_FMT_WORD32 : TXS_FMT_IQ8;
		} else if (strcmp(argv[i], "verify") == 0 && i + 1 < argc) {
			c.verify = strtoul(argv[++i], NULL, 0) != 0U;
		} else {
			shell_error(sh, "tx tone: unknown argument %s", argv[i]);
			return -EINVAL;
		}
	}
	if ((c.msps != 20U && c.msps != 40U) || c.seconds == 0U || c.seconds > MAX_SECONDS || amp < 1 ||
	    amp > MAX_AMP || k < 1 || k > MAX_K) {
		shell_error(sh, "tx tone: rate 20 or 40, 1 to %u s, amp 1..%d, k 1..%d", MAX_SECONDS, MAX_AMP, MAX_K);
		return -EINVAL;
	}
	if (!inited || !esp_sdr_ready) {
		shell_error(sh, "tx tone: run tx init first");
		return -EAGAIN;
	}
	/* the library's setting, which a failed tune also changes, and the tone on both sides of it */
	if (c.rf && !txs_rf_ok(esp_sdr_get_freq(), esp_sdr_get_freq_offset(), (uint32_t)k, c.msps)) {
		shell_error(sh, "tx tone: rf only inside %u to %u MHz with %u kHz guard, carrier %u MHz (LO offset "
			    "%d kHz) and the tone +-%u kHz", TXS_RF_BAND_LO_KHZ / 1000U, TXS_RF_BAND_HI_KHZ / 1000U,
			    TXS_RF_GUARD_KHZ, (unsigned int)esp_sdr_get_freq(), (int)esp_sdr_get_freq_offset(),
			    txs_tone_khz((uint32_t)k, c.msps));
		return -ERANGE;
	}
	tx_tone_lock();
	if (tx_stream_active()) {
		tx_tone_unlock();
		shell_error(sh, "tx tone: a stream runs");
		return -EBUSY;
	}
	tx_tone_set(k, amp);
	ret = tx_stream_start(&c, tx_shell_done);
	tx_tone_unlock();
	if (ret != 0) {
		shell_error(sh, "tx tone: start failed: %d", ret);
		return ret;
	}
	shell_print(sh, "tx tone: %u MS/s for %u s, kernel %s, %s packets of 1420 B from the injector, tone "
		    "%d kHz (k %d), amp %d of 511 counts, %s; tx stat, tx stop [now]", c.msps, c.seconds,
		    c.kern->name, c.fmt == TXS_FMT_IQ8 ? "8+8 bit" : "ready word", k * (int)(c.msps * 1000U) / 351,
		    k, amp, c.rf ? "RF ON (chain on by the library, its power step is not used on the S31: "
		    "the level is the amp), engine restarted on CPU 1" :
		    "chain off, RF off");
	return 0;
}

static int cmd_kern(const struct shell *sh, size_t argc, char **argv)
{
	const struct txs_kern *nk;

	if (argc < 2) {
		shell_print(sh, "tx kern: %s", kern->name);
		return 0;
	}
	if (tx_stream_active()) {
		shell_error(sh, "tx kern: a stream runs");
		return -EBUSY;
	}
	nk = txs_kern_find(argv[1]);
	if (nk == NULL) {
		shell_error(sh, "tx kern ref|fast|pie: %s is unknown%s", argv[1],
			    strcmp(argv[1], "pie") == 0 ? " (CONFIG_ESP_SDR_TXS_PIE is off)" : "");
		return -EINVAL;
	}
	kern = nk;
	kern_set = true;
	shell_print(sh, "tx kern: %s (golden-checked against ref and timed at each start)", kern->name);
	return 0;
}

static int cmd_inject(const struct shell *sh, size_t argc, char **argv)
{
	struct tx_inj_cfg c;

	if (!IS_ENABLED(CONFIG_APP_TXS_INJECT)) {
		shell_error(sh, "tx inject: this image has no injector (the network is the only source): build with "
			    "-DEXTRA_CONF_FILE=injector.conf, see apps/txstream_eth/README.rst");
		return -ENOTSUP;
	}
	tx_inject_get(&c);
	if (argc > 1 && strcmp(argv[1], "bench") == 0) {
		uint64_t wps;
		uint32_t cpp;
		const enum txs_fmt fmt = argc > 2 && strcmp(argv[2], "w32") == 0 ? TXS_FMT_WORD32 : TXS_FMT_IQ8;
		int ret;

		if (tx_stream_active()) {
			shell_error(sh, "tx inject bench: a stream runs");
			return -EBUSY;
		}
		tx_tone_set(DEFAULT_K, DEFAULT_AMP);
		ret = tx_inject_bench(fmt, 500U, &wps, &cpp);
		if (ret != 0) {
			shell_error(sh, "tx inject bench: %d", ret);
			return ret;
		}
		shell_print(sh, "tx inject bench %s: %llu words/s alone (no stream, no credit), %u cycles a packet",
			    fmt == TXS_FMT_IQ8 ? "iq8" : "w32", (unsigned long long)wps, cpp);
		return 0;
	}
	for (size_t i = 1; i < argc; i++) {
		if (strcmp(argv[i], "off") == 0) {
			memset(&c, 0, sizeof(c));
		} else if (strcmp(argv[i], "loss") == 0 && i + 1 < argc) {
			c.loss_pm = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (strcmp(argv[i], "reorder") == 0 && i + 1 < argc) {
			c.reorder = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (strcmp(argv[i], "stall") == 0 && i + 1 < argc) {
			c.stall_ms = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else {
			shell_error(sh, "tx inject [off|bench [iq8|w32]] [loss <permille>] [reorder <n>] [stall <ms>]");
			return -EINVAL;
		}
	}
	if (c.loss_pm > 1000U || !txs_stall_ok(c.stall_ms)) {
		shell_error(sh, "tx inject: loss 0 to 1000 permille, stall 0 to %u ms", TXS_STALL_MAX);
		return -EINVAL;
	}
	tx_inject_set(&c);
	tx_inject_get(&c);
	shell_print(sh, "tx inject: loss %u permille, reorder batches of %u, stall %u ms a second (applies to "
		    "the running stream too)", c.loss_pm, c.reorder, c.stall_ms);
	return 0;
}

static int cmd_cfg(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t lat = cfg_lat, tol = cfg_tol, ovh = cfg_ovh;

	if ((argc & 1U) == 0U) {
		shell_error(sh, "tx cfg [lat <0|%u..%u cycles>] [tol <0..%u words>] [ovh <0..%u cycles>]",
			    TXS_LAT_MIN, TXS_LAT_MAX, TXS_TOL_MAX, TXS_OVH_MAX);
		return -EINVAL;
	}
	for (size_t i = 1; i + 1 < argc; i += 2) {
		const uint32_t v = (uint32_t)strtoul(argv[i + 1], NULL, 0);

		if (strcmp(argv[i], "lat") == 0 && txs_lat_ok(v)) {
			lat = v;
		} else if (strcmp(argv[i], "tol") == 0 && txs_tol_ok(v)) {
			tol = v;
		} else if (strcmp(argv[i], "ovh") == 0 && txs_ovh_ok(v)) {
			ovh = v;
		} else {
			shell_error(sh, "tx cfg [lat <0|%u..%u cycles>] [tol <0..%u words>] [ovh <0..%u cycles>]: "
				    "%s %s is not accepted", TXS_LAT_MIN, TXS_LAT_MAX, TXS_TOL_MAX, TXS_OVH_MAX,
				    argv[i], argv[i + 1]);
			return -EINVAL;
		}
	}
	cfg_lat = lat;
	cfg_tol = tol;
	cfg_ovh = ovh;
	shell_print(sh, "tx cfg: ownership write latency %u cyc (0: measured before each start), flip tolerance %u words, step overhead %u cyc "
		    "(from the next start)", cfg_lat, cfg_tol, cfg_ovh);
	return 0;
}

static int cmd_net(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	tx_net_print(sh);
	return 0;
}

static int cmd_arm(const struct shell *sh, size_t argc, char **argv)
{
	const uint32_t min = (uint32_t)strtoul(argv[1], NULL, 0);

	if (min < 1U || min > 120U) {
		shell_error(sh, "tx arm <minutes 1..120>");
		return -EINVAL;
	}
	return tx_net_arm(sh, min);
}

static int cmd_disarm(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	tx_net_disarm();
	shell_print(sh, "tx disarm: no RF from the network (a run on air drains)");
	return 0;
}

static int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
	const bool now = argc > 1 && strcmp(argv[1], "now") == 0;
	const int ret = tx_stream_stop(now);

	if (ret == 0) {
		shell_print(sh, now ? "tx stop: engine off at the next poll" :
			    "tx stop: draining (zeros for a window, then engine and chain off)");
	} else {
		shell_print(sh, "tx stop: %s", ret == -EAGAIN ? "still starting, again in a moment" :
			    "nothing runs");
	}
	return 0;
}

static int cmd_hang(const struct shell *sh, size_t argc, char **argv)
{
	const bool lock = argc > 1 && strcmp(argv[1], "lock") == 0;
	int ret;

	if (argc > 1 && !lock) {
		shell_error(sh, "tx hang [lock]");
		return -EINVAL;
	}
	ret = tx_stream_hang(lock);
	if (ret != 0) {
		shell_error(sh, "tx hang%s: %s", lock ? " lock" : "",
			    ret == -EPERM ? "only without rf" : "nothing runs");
		return ret;
	}
	shell_print(sh, "tx hang%s: the flip thread spins%s; the deadman must stop the engine in about 2.5 "
		    "passes%s", lock ? " lock" : "", lock ? " with interrupts locked" : "",
		    lock ? " and the abort of the thread may never return (the report comes first; a new start "
		    "is refused until a reset)" : "");
	return 0;
}

static int cmd_stat(const struct shell *sh, size_t argc, char **argv)
{
	struct tx_stats s;
	char buf[256];
	const struct sink k = {.sh = sh, .buf = buf, .size = sizeof(buf)};

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	size_t fu, cu;

	tx_stream_stats(&s);
	print_stats(&k, "tx stat", &s);
	tx_stream_stacks(&fu, &cu);
	shell_print(sh, "tx stat: stack bytes never used: flip thread %u of 3072, control thread (main) %u of %u",
		    (unsigned int)fu, (unsigned int)cu, (unsigned int)CONFIG_MAIN_STACK_SIZE);
	shell_print(sh, "tx stat: ownership 0x%08x, tuned %u MHz, esp_sdr_ready %d, dump_ready %d, kernel %s",
		    (unsigned int)sys_read32(HP_SYSTEM_TCM_DATA_DUMP_CTRL_REG), tuned_mhz, esp_sdr_ready,
		    esp_sdr_s31_dump_ready(), kern->name);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(tx_cmds,
	SHELL_CMD(boot, NULL, "addresses per group, layout checks, canary", cmd_boot),
	SHELL_CMD_ARG(init, NULL, "[MHz]: esp_sdr_init() and the first tune with the network up", cmd_init,
		      1, 1),
	SHELL_CMD_ARG(tone, NULL, "<20|40> <seconds> [rf] [amp 1..40] [k 1..175] [fmt iq8|w32] [verify 0|1]",
		      cmd_tone, 3, 9),
	SHELL_CMD_ARG(kern, NULL, "[ref|fast|pie]: the expansion kernels", cmd_kern, 1, 1),
	SHELL_CMD_ARG(inject, NULL, "[off|bench [iq8|w32]] [loss <permille>] [reorder <n>] [stall <ms>]", cmd_inject, 1, 8),
	SHELL_CMD_ARG(cfg, NULL, "[lat <0|20..200>] [tol <0..1024>] [ovh <0..2000>]", cmd_cfg, 1, 6),
	SHELL_CMD_ARG(stop, NULL, "[now]: drain to zeros, engine and chain off", cmd_stop, 1, 1),
	SHELL_CMD_ARG(hang, NULL, "[lock]: test: stop the flip thread (lock: with interrupts locked, no rf), the "
		      "deadman must fire", cmd_hang, 1, 1),
	SHELL_CMD(stat, NULL, "stream statistics", cmd_stat),
	SHELL_CMD(net, NULL, "the network side: subscriber, run, the hook's counts, statuses, pings", cmd_net),
	SHELL_CMD_ARG(arm, NULL, "<minutes 1..120>: the subscriber there is now (needed) may ask for RF that long",
		      cmd_arm, 2, 0),
	SHELL_CMD(disarm, NULL, "no RF from the network", cmd_disarm),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(tx, &tx_cmds, "DAC stream by group flips", NULL);

void tx_shell_init(const struct device *eth, struct net_if *iface)
{
	eth_iface = iface;
	eth_dev = eth;
}
