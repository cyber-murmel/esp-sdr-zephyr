/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Shell: chain start|stop|peer|stat|dump|info|servo|pps.
 */
#include <ctype.h>
#include <stdlib.h>

#include <zephyr/drivers/ethernet/eth_dwmac_fast.h>
#include <zephyr/device.h>
#include <zephyr/drivers/ptp_clock.h>
#include <zephyr/net/phy.h>
#include <zephyr/net/ptp_time.h>
#include <zephyr/shell/shell.h>

#include "chain.h"

static const char *const role_name[] = {"idle", "seme", "uke"};

static int cmd_start(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t us = argc > 1 ? strtoul(argv[1], NULL, 0) : 0;
	uint32_t rounds = argc > 2 ? strtoul(argv[2], NULL, 0) : 0;

	if (us != 0 && us < 100) {
		shell_error(sh, "interval_us at least 100");
		return -EINVAL;
	}
	if (rounds > 100) {
		shell_error(sh, "rounds_per_burst at most 100");
		return -EINVAL;
	}
	if (chain_start(us, rounds) != 0) {
		shell_error(sh, "no peer: chain peer <mac> first (the other board's MAC from chain info)");
		return -ENOTCONN;
	}
	shell_print(sh, "seme: interval %u us, %u rounds per burst", chain_interval_us(),
		    chain_rounds());
	return 0;
}

/* Parse aa:bb:cc:dd:ee:ff, exactly 6 bytes of 2 hex digits. */
static bool parse_mac(const char *s, uint8_t mac[6])
{
	for (int i = 0; i < 6; i++) {
		int hi, lo;

		if (!isxdigit((unsigned char)s[0]) || !isxdigit((unsigned char)s[1])) {
			return false;
		}
		hi = isdigit((unsigned char)s[0]) ? s[0] - '0' : (tolower((unsigned char)s[0]) - 'a' + 10);
		lo = isdigit((unsigned char)s[1]) ? s[1] - '0' : (tolower((unsigned char)s[1]) - 'a' + 10);
		mac[i] = (uint8_t)(hi << 4 | lo);
		s += 2;
		if (i < 5 && *s++ != ':') {
			return false;
		}
	}
	return *s == '\0';
}

/* peer [<mac> | clear] */
static int cmd_peer(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t m[6];
	int ret;

	if (argc == 1) {
		if (chain_peer_get(m)) {
			shell_print(sh, "peer %02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3],
				    m[4], m[5]);
		} else {
			shell_print(sh, "peer none");
		}
		return 0;
	}
	if (strcmp(argv[1], "clear") == 0) {
		ret = chain_peer_clear();
	} else if (parse_mac(argv[1], m)) {
		ret = chain_peer_set(m);
	} else {
		shell_error(sh, "peer <aa:bb:cc:dd:ee:ff> | clear");
		return -EINVAL;
	}
	if (ret != 0) {
		shell_error(sh, "chain is running: chain stop first");
		return ret;
	}
	return 0;
}

static int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
	chain_stop();
	shell_print(sh, "stopped (still answers)");
	return 0;
}

static int onoff(const struct shell *sh, const char *s, atomic_t *flag, const char *name)
{
	if (strcmp(s, "on") == 0) {
		atomic_set(flag, 1);
	} else if (strcmp(s, "off") == 0) {
		atomic_set(flag, 0);
	} else {
		shell_error(sh, "on or off");
		return -EINVAL;
	}
	shell_print(sh, "%s %s", name, s);
	return 0;
}

static int cmd_stat(const struct shell *sh, size_t argc, char **argv)
{
	return onoff(sh, argv[1], &chain_stat_on, "stat");
}

/* servo on [period_ms] [bw_hz] [clamp_ns] | off */
static int cmd_servo(const struct shell *sh, size_t argc, char **argv)
{
	if (strcmp(argv[1], "on") == 0) {
		uint32_t period_ms = argc > 2 ? strtoul(argv[2], NULL, 0) : 50;
		double bw_hz = argc > 3 ? strtod(argv[3], NULL) : 2.0;
		uint32_t clamp_ns = argc > 4 ? strtoul(argv[4], NULL, 0) : 500;

		if (period_ms < 10 || period_ms > 1000 || bw_hz < 0.01 || bw_hz > 10.0 ||
		    clamp_ns == 0) {
			shell_error(sh, "period_ms 10..1000, bw_hz 0.01..10, clamp_ns > 0");
			return -EINVAL;
		}
		chain_servo_config(period_ms, (uint32_t)(bw_hz * 1000.0 + 0.5), clamp_ns);
		atomic_set(&chain_servo_on, 1);
		shell_print(sh, "servo on (uke only): period %u ms, bandwidth %.3f Hz, clamp %u ns",
			    period_ms, bw_hz, clamp_ns);
		return 0;
	}
	return onoff(sh, argv[1], &chain_servo_on, "servo");
}

/* seq,base_sec,t1_ps,t2_ps,t3_ps,t4_ps; a servo step as a # line */
static int cmd_dump(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t end = chain_log_count();
	uint32_t n = argc > 1 ? strtoul(argv[1], NULL, 0) : CONFIG_APP_CHAIN_LOG;
	uint32_t i;

	if (n > CONFIG_APP_CHAIN_LOG) {
		n = CONFIG_APP_CHAIN_LOG;
	}
	if (n > end) {
		n = end;
	}
	shell_print(sh, "# seq,base_sec,t1_ps,t2_ps,t3_ps,t4_ps (t1,t4 local, t2,t3 peer; offset = peer - local)");
	for (i = end - n; i != end; i++) {
		struct chain_xchg x;

		if (!chain_log_get(i, &x)) {
			shell_warn(sh, "# log overwritten while printing");
			break;
		}
		if (x.seq == CHAIN_LOG_STEP) {
			shell_print(sh, "# servo step %lld ps", (long long)x.t_ps[0]);
			continue;
		}
		shell_print(sh, "%u,%u,%lld,%lld,%lld,%lld", x.seq, x.base_sec,
			    (long long)x.t_ps[0], (long long)x.t_ps[1], (long long)x.t_ps[2],
			    (long long)x.t_ps[3]);
	}
	return 0;
}

static int cmd_info(const struct shell *sh, size_t argc, char **argv)
{
	const struct device *clk = DEVICE_DT_GET(DT_NODELABEL(ptp_clock));
	const struct device *phy = DEVICE_DT_GET(DT_NODELABEL(phy));
	const uint8_t *m = chain_mac();
	uint8_t pm[6];
	struct net_ptp_time t;
	struct phy_link_state ls;
	struct dwmac_rx_stats rs;
	int ret;

	shell_print(sh, "mac %02x:%02x:%02x:%02x:%02x:%02x role %s interval %u us rounds %u", m[0],
		    m[1], m[2], m[3], m[4], m[5], role_name[chain_role_get()], chain_interval_us(),
		    chain_rounds());
	if (chain_peer_get(pm)) {
		shell_print(sh, "peer %02x:%02x:%02x:%02x:%02x:%02x", pm[0], pm[1], pm[2], pm[3],
			    pm[4], pm[5]);
	} else {
		shell_print(sh, "peer none");
	}
	shell_print(sh, "ptp subsec: %s, fine ssinc %s", CHAIN_DIGITAL ? "digital (ns)" : "binary (2^-31 s)",
		    IS_ENABLED(CONFIG_PTP_CLOCK_DWC_MAC_FINE_SSINC) ? "yes" : "no");
	ret = ptp_clock_get(clk, &t);
	if (ret == 0) {
		shell_print(sh, "ptp time %llu.%09u s", (unsigned long long)t.second, t.nanosecond);
	}
	ret = phy_get_link_state(phy, &ls);
	if (ret == 0) {
		shell_print(sh, "link %s speed %s", ls.is_up ? "up" : "down",
			    PHY_LINK_IS_SPEED_1000M(ls.speed) ? "1000M" :
			    PHY_LINK_IS_SPEED_100M(ls.speed) ? "100M" :
			    PHY_LINK_IS_SPEED_10M(ls.speed) ? "10M" : "?");
	}
	dwmac_rx_stats_get(chain_eth, &rs);
	shell_print(sh, "mac rx: offered %u claimed %u missed %u fifo_ovf %u starved %u", rs.offered,
		    rs.claimed, rs.missed, rs.fifo_overflow, rs.starved);
	shell_print(sh, "tx sent %u busy %u no_peer %u fail %u stamp_missing %u", chain_cnt.tx_sent,
		    chain_cnt.tx_busy, chain_cnt.tx_no_peer, chain_cnt.tx_fail,
		    chain_cnt.tx_stamp_missing);
	shell_print(sh, "rx chain %u nostamp %u stale %u lost %u", chain_cnt.rx_chain,
		    chain_cnt.rx_nostamp, chain_cnt.rx_stale, chain_cnt.rx_lost);
	shell_print(sh, "exchanges %u rejected %u ring_overflow %u reentry %u bursts %u skipped %u chain_end %u",
		    chain_cnt.exchanges, chain_cnt.rejected, chain_cnt.ring_overflow,
		    chain_cnt.reentry, chain_cnt.bursts, chain_cnt.burst_skipped, chain_cnt.chain_end);
	shell_print(sh, "stat %s servo %s (steps %u) log %u", atomic_get(&chain_stat_on) ? "on" : "off",
		    atomic_get(&chain_servo_on) ? "on" : "off", chain_servo_steps(),
		    chain_log_count());
	return 0;
}

/* pps <n> | pps flex [width_us] [clk] | pps stop */
static int cmd_pps(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	if (strcmp(argv[1], "flex") == 0) {
		struct chain_pps_info i;
		uint32_t width_us = argc > 2 ? strtoul(argv[2], NULL, 0) : 1000;
		bool ref_clk = argc > 3 && strcmp(argv[3], "clk") == 0;

		ret = chain_pps_flex(width_us, ref_clk, &i);
		if (ret != 0) {
			shell_error(sh, "flexible PPS failed: %d", ret);
			return ret;
		}
		shell_print(sh, "pulse train from PTP second %llu: tick %u ps, interval %u ticks, "
			    "width %u ticks", (unsigned long long)i.start_sec, i.tick_ps,
			    i.interval_ticks, i.width_ticks);
		return 0;
	}
	if (strcmp(argv[1], "stop") == 0) {
		return chain_pps_flex_stop();
	}
	ret = chain_pps_set(strtoul(argv[1], NULL, 0));
	if (ret != 0) {
		shell_error(sh, "n is 0 to 15, flex [width_us] [clk], or stop");
	}
	return ret;
}

SHELL_STATIC_SUBCMD_SET_CREATE(chain_cmds,
	SHELL_CMD_ARG(start, NULL, "[interval_us] [rounds_per_burst]", cmd_start, 1, 2),
	SHELL_CMD_ARG(peer, NULL, "[<aa:bb:cc:dd:ee:ff> | clear]: unicast destination", cmd_peer, 1, 1),
	SHELL_CMD_ARG(stop, NULL, "stop the bursts", cmd_stop, 1, 0),
	SHELL_CMD_ARG(stat, NULL, "on|off: print the 1 s window", cmd_stat, 2, 0),
	SHELL_CMD_ARG(dump, NULL, "[n]: last exchanges as CSV", cmd_dump, 1, 1),
	SHELL_CMD_ARG(info, NULL, "state and counters", cmd_info, 1, 0),
	SHELL_CMD_ARG(servo, NULL, "on [period_ms] [bw_hz] [clamp_ns] | off: the uke follows the seme", cmd_servo, 2, 3),
	SHELL_CMD_ARG(pps, NULL, "<n> | flex [width_us] [clk] | stop", cmd_pps, 2, 2),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(chain, &chain_cmds, "PTP chain", NULL);
