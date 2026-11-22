/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The "stream" shell command on the UART console:
 *   stream status
 *   stream stop
 *   stream bench <pkt bytes> <seconds> [rate pkt/s]
 *   stream mtu [bytes]
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/shell/shell.h>

#include "stream_eth.h"

/* A thread below everything per CPU: the share of the CPU the stream leaves. */
#define MAX_CPUS 2
#define SPIN_STACK 1024
static K_THREAD_STACK_ARRAY_DEFINE(spin_stack, MAX_CPUS, SPIN_STACK);
static struct k_thread spin_thread[MAX_CPUS];
static volatile uint32_t spins[MAX_CPUS];
static uint32_t idle_per_s[MAX_CPUS], last_spins[MAX_CPUS];
static int64_t last_ms;
static unsigned int ncpu;

static void spin_fn(void *a, void *b, void *c)
{
	unsigned int n = (unsigned int)(uintptr_t)a;

	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		spins[n]++;
	}
}

void shell_spin_start(void)
{
	ncpu = MIN(arch_num_cpus(), (unsigned int)MAX_CPUS);
	for (unsigned int k = 0; k < ncpu; k++) {
		k_thread_create(&spin_thread[k], spin_stack[k], SPIN_STACK, spin_fn,
				(void *)(uintptr_t)k, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0,
				K_FOREVER);
#if defined(CONFIG_SMP) && defined(CONFIG_SCHED_CPU_MASK)
		if (ncpu > 1) {
			if (k_thread_cpu_pin(&spin_thread[k], (int)k) != 0) {
				printk("stream_eth: cannot pin the load thread %u\n", k);
			}
		}
#endif
		k_thread_start(&spin_thread[k]);
	}
	/* idle: what the spinners get on a quiet system, per second */
	uint32_t before[MAX_CPUS];

	for (unsigned int k = 0; k < ncpu; k++) {
		before[k] = spins[k];
	}
	k_msleep(1000);
	for (unsigned int k = 0; k < ncpu; k++) {
		idle_per_s[k] = spins[k] - before[k];
		last_spins[k] = spins[k];
	}
	last_ms = k_uptime_get();
}

/* Free CPU of each core since the last call, in percent; the lowest of them. */
static uint32_t cpu_free(uint32_t *per_cpu)
{
	int64_t now = k_uptime_get();
	uint32_t low = 100U;

	for (unsigned int k = 0; k < ncpu; k++) {
		uint32_t s = spins[k], d = s - last_spins[k];
		uint64_t want = (uint64_t)idle_per_s[k] * (uint64_t)(now - last_ms) / 1000U;
		uint32_t pct = want == 0U ? 0U : (uint32_t)MIN((uint64_t)d * 100U / want, 100ULL);

		last_spins[k] = s;
		per_cpu[k] = pct;
		low = MIN(low, pct);
	}
	last_ms = now;
	return low;
}

uint32_t txpath_cpu_free_pct(void)
{
	uint32_t p[MAX_CPUS];

	return cpu_free(p);
}

static void print_addr(const struct shell *sh, const struct peer *p)
{
	char s[NET_INET6_ADDRSTRLEN];

	(void)net_addr_ntop(p->v6 ? NET_AF_INET6 : NET_AF_INET, p->addr, s, sizeof(s));
	shell_print(sh, "  subscriber %s port %u, MAC %02x:%02x:%02x:%02x:%02x:%02x", s, p->port,
		    p->mac[0], p->mac[1], p->mac[2], p->mac[3], p->mac[4], p->mac[5]);
}

/* Set by main() when the radio, the sender and the control side are up: commands wait for it. */
static atomic_t ready;

void shell_app_ready(void)
{
	atomic_set(&ready, 1);
}

static bool starting(const struct shell *sh)
{
	if (atomic_get(&ready) != 0) {
		return false;
	}
	shell_error(sh, "not started");
	return true;
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	struct radio_state rs;
	struct tx_stats ts;
	struct peer p;
	uint32_t per[MAX_CPUS] = {0};
	uint32_t free_pct;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (starting(sh)) {
		return -EAGAIN;
	}
	free_pct = cpu_free(per);
	radio_get(&rs);
	txpath_get_stats(&ts);
	shell_print(sh, "stream: %s, %llu Hz, rate %u S/s, gain %d", rs.on ? "on" : "off",
		    rs.freq_hz, rs.rate_hz, rs.gain);
	if (txpath_peer(&p)) {
		print_addr(sh, &p);
	} else {
		shell_print(sh, "  no subscriber");
	}
	shell_print(sh, "  MTU %u, packet payload %u bytes", txpath_mtu(),
		    txpath_payload_bytes());
	shell_print(sh, "  packets %u, %llu bytes, context %u, desc waits %u, errors %u",
		    ts.packets, ts.bytes, ts.context, ts.desc_waits, ts.errors);
	shell_print(sh, "  nodes %u, dropped %u (staging full), gaps %u (ring skips %u at the last stop)%s",
		    radio_nodes(), cells.dropped_nodes, cells.gaps, radio_ring_skips(),
		    ctl_blocked() ? ", subscriptions blocked (stream start)" : "");
	{
		struct radio_prof rp;

		radio_prof_take(&rp);
		shell_print(sh, "  cycles since the last status: sink %u a node (max %u, %u nodes); build %u a frame; "
				"submit %u a frame",
			    rp.node_n ? rp.node_total / rp.node_n : 0U, rp.node_max, rp.node_n,
			    ts.build_n ? ts.build_cycles / ts.build_n : 0U,
			    ts.send_n ? ts.send_cycles / ts.send_n : 0U);
	}
	{
		struct ctl_stats cs;
		int te, re;

		ctl_get_stats(&cs);
		radio_errors(&te, &re);
		shell_print(sh, "  last tune error %d, last ring error %d; control: %u commands, %u subscribes, "
				"%u bad, %u errors, %u dropped, %u sources rejected",
			    te, re, cs.commands, cs.subscribes, cs.bad, cs.errors, cs.dropped, cs.rejected);
	}
	shell_print(sh, "  MAC tx underflows %u", txpath_underflows() - ts.underflow_base);
	shell_print(sh, "  CPU free %u %% (per CPU %u %%, %u %%) since the last status", free_pct,
		    per[0], ncpu > 1 ? per[1] : 0U);
	return 0;
}

static int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (starting(sh)) {
		return -EAGAIN;
	}
	txpath_bench_abort();
	ctl_block();
	shell_print(sh, "stopped, no new subscriber until \"stream start\"");
	return 0;
}

static int cmd_start(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (starting(sh)) {
		return -EAGAIN;
	}
	ctl_unblock();
	shell_print(sh, "subscriptions allowed");
	return 0;
}

static int cmd_mtu(const struct shell *sh, size_t argc, char **argv)
{
	if (starting(sh)) {
		return -EAGAIN;
	}
	if (argc > 1) {
		txpath_set_mtu((unsigned int)strtoul(argv[1], NULL, 0));
	}
	shell_print(sh, "MTU %u (interface %u, cap)", txpath_mtu(),
		    net_if_get_mtu(net_if_get_default()));
	return 0;
}

static int cmd_bench(const struct shell *sh, size_t argc, char **argv)
{
	struct bench_result r;
	uint32_t pkt = (uint32_t)strtoul(argv[1], NULL, 0);
	uint32_t secs = (uint32_t)strtoul(argv[2], NULL, 0);
	uint32_t pps = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 0) : 0U;
	int ret;

	if (starting(sh)) {
		return -EAGAIN;
	}
	if (secs == 0U || secs > 600U) {
		shell_error(sh, "seconds 1..600");
		return -EINVAL;
	}
	shell_print(sh, "bench: the radio stops, packets go to the subscriber...");
	ret = txpath_bench(pkt, secs, pps, &r);
	if (ret != 0) {
		shell_error(sh, "bench: %d (is there a subscriber? send VRTS first)", ret);
		return ret;
	}
	{
		uint64_t pk = r.packets;
		uint32_t pps_got = (uint32_t)(pk / r.seconds);
		/* the UDP datagram plus IP, UDP, Ethernet headers and the 24 B of preamble, FCS, gap */
		uint32_t wire = r.pkt_bytes + (txpath_v6() ? 40U : 20U) + 8U + 14U + 24U;
		uint64_t mbit = (uint64_t)pps_got * wire * 8U / 1000000U;
		uint64_t data = (uint64_t)pps_got * (r.pkt_bytes - 24U) * 8U / 1000000U;

		shell_print(sh, "bench: %u B packets, %u s: %u packets, %u packets/s",
			    r.pkt_bytes, r.seconds, r.packets, pps_got);
		shell_print(sh, "bench: %llu Mb/s on the wire, %llu Mb/s of samples (%llu MS/s)",
			    mbit, data, data / 16U);
		shell_print(sh, "bench: %llu cycles a packet (build and submit, %u Hz), "
				"descriptor waits %u, late %u, MAC tx underflows %u",
			    r.packets != 0U ? r.cycles / r.packets : 0ULL, r.cycles_per_sec,
			    r.desc_waits, r.late, r.underflows);
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(stream_cmds,
	SHELL_CMD(status, NULL, "Subscriber, rate, packets, drops, CPU free", cmd_status),
	SHELL_CMD(start, NULL, "Allow subscriptions again after stop", cmd_start),
	SHELL_CMD(stop, NULL, "Stop the stream and drop the subscriber", cmd_stop),
	SHELL_CMD_ARG(mtu, NULL, "[bytes]: the IP packet size the stream may use", cmd_mtu, 1, 1),
	SHELL_CMD_ARG(bench, NULL,
		      "<pkt bytes> <seconds> [rate pkt/s]: synthetic packets to the subscriber",
		      cmd_bench, 3, 1),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(stream, &stream_cmds, "VRT stream over Ethernet", NULL);
