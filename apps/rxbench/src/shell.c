/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The "rxb" shell command:
 *   rxb start [full]   zero the counters and take frames (full: check every payload byte)
 *   rxb stop
 *   rxb stat           rates since the last stat, totals since start, driver and MAC counters
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/ethernet/eth_dwmac_fast.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/net/net_if.h>
#include <zephyr/shell/shell.h>

#include "rxb.h"
#include "rxb_proto.h"

/* A thread below everything per CPU: the share of the CPU the receive leaves. */
#define MAX_CPUS 2
#define SPIN_STACK 1024
/* The MMC counters kept as the start's baseline (the board's list has 20) */
#define MAX_MMC 32
#define MMC_RX_PREFIX "RX_"
#define MS_PER_S 1000

static K_THREAD_STACK_ARRAY_DEFINE(spin_stack, MAX_CPUS, SPIN_STACK);
static struct k_thread spin_thread[MAX_CPUS];
/* 64 bit: the low word alone wraps within a minute or two. Written by the spinner only. */
static volatile uint32_t spins_lo[MAX_CPUS], spins_hi[MAX_CPUS];
static uint64_t idle_per_s[MAX_CPUS], last_spins[MAX_CPUS];
static int64_t last_cpu_ms;
static unsigned int ncpu;

static const struct device *volatile eth_dev;
static struct net_if *eth_iface;

/* Baselines of the last start, and the last stat */
static struct dwmac_rx_stats base_ds, last_ds;
static uint32_t base_mmc[MAX_MMC];
static struct rxb_snap last_snap;
static int64_t last_ms;

static void spin_fn(void *a, void *b, void *c)
{
	unsigned int n = (unsigned int)(uintptr_t)a;

	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		if (++spins_lo[n] == 0U) {
			spins_hi[n]++;
		}
	}
}

static uint64_t spins_read(unsigned int n)
{
	uint32_t hi, lo;

	do {
		hi = spins_hi[n];
		lo = spins_lo[n];
	} while (hi != spins_hi[n]);
	return ((uint64_t)hi << 32) | lo;
}

void rxb_shell_init(const struct device *eth, struct net_if *iface)
{
	uint64_t before[MAX_CPUS];

	ncpu = MIN(arch_num_cpus(), (unsigned int)MAX_CPUS);
	for (unsigned int k = 0; k < ncpu; k++) {
		k_thread_create(&spin_thread[k], spin_stack[k], SPIN_STACK, spin_fn,
				(void *)(uintptr_t)k, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0,
				K_FOREVER);
#if defined(CONFIG_SMP) && defined(CONFIG_SCHED_CPU_MASK)
		if (ncpu > 1 && k_thread_cpu_pin(&spin_thread[k], (int)k) != 0) {
			printk("rxbench: cannot pin the load thread %u\n", k);
		}
#endif
		k_thread_start(&spin_thread[k]);
	}
	/* idle: what the spinners get per second, measured here at boot: the board is quiet then,
	 * so "CPU free" is relative to an idle CPU (a start under load would read too low)
	 */
	for (unsigned int k = 0; k < ncpu; k++) {
		before[k] = spins_read(k);
	}
	k_msleep(MS_PER_S);
	for (unsigned int k = 0; k < ncpu; k++) {
		last_spins[k] = spins_read(k);
		idle_per_s[k] = last_spins[k] - before[k];
	}
	last_cpu_ms = k_uptime_get();
	/* the commands work from here on */
	eth_iface = iface;
	barrier_dmem_fence_full();
	eth_dev = eth;
}

/* Free CPU of each core since the last call, in percent. */
static void cpu_free(uint32_t *per_cpu)
{
	int64_t now = k_uptime_get();

	for (unsigned int k = 0; k < ncpu; k++) {
		uint64_t s = spins_read(k), d = s - last_spins[k];
		uint64_t want = idle_per_s[k] * (uint64_t)(now - last_cpu_ms) / MS_PER_S;

		last_spins[k] = s;
		per_cpu[k] = want == 0U ? 0U : (uint32_t)MIN(d * 100U / want, 100ULL);
	}
	last_cpu_ms = now;
}

/* The vendor (MMC) counters of the MAC: the RX ones, each as it counted since the baseline. */
static void mmc_read(uint32_t *out, const char **keys, unsigned int max, unsigned int *n)
{
	const struct ethernet_api *api = eth_dev->api;
	struct net_stats_eth *st;

	*n = 0U;
	if (api->get_stats_type == NULL) {
		return;
	}
	st = api->get_stats_type(eth_dev, eth_iface, ETHERNET_STATS_TYPE_VENDOR);
	if (st == NULL || st->vendor == NULL) {
		return;
	}
	for (unsigned int i = 0; i < max && st->vendor[i].key != NULL; i++) {
		out[i] = st->vendor[i].value;
		if (keys != NULL) {
			keys[i] = st->vendor[i].key;
		}
		*n = i + 1U;
	}
}

static void print_addrs(const struct shell *sh)
{
	char s[NET_INET6_ADDRSTRLEN];

	if (eth_iface->config.ip.ipv6 != NULL) {
		for (int k = 0; k < NET_IF_MAX_IPV6_ADDR; k++) {
			struct net_if_addr *a = &eth_iface->config.ip.ipv6->unicast[k];

			if (a->is_used && net_addr_ntop(NET_AF_INET6, &a->address.in6_addr, s,
							sizeof(s)) != NULL) {
				shell_print(sh, "  IPv6 %s", s);
			}
		}
	}
	if (eth_iface->config.ip.ipv4 != NULL) {
		for (int k = 0; k < NET_IF_MAX_IPV4_ADDR; k++) {
			struct net_if_addr *a = &eth_iface->config.ip.ipv4->unicast[k].ipv4;

			if (a->is_used && net_addr_ntop(NET_AF_INET, &a->address.in_addr, s,
							sizeof(s)) != NULL) {
				shell_print(sh, "  IPv4 %s", s);
			}
		}
	}
}

/* The shell is there before main() has set the bench up (the network config holds it up). */
static bool not_started(const struct shell *sh)
{
	if (eth_dev != NULL) {
		return false;
	}
	shell_error(sh, "not started");
	return true;
}

static int cmd_start(const struct shell *sh, size_t argc, char **argv)
{
	bool full = argc > 1 && strcmp(argv[1], "full") == 0;
	unsigned int n;
	uint32_t cpu[MAX_CPUS];

	if (not_started(sh)) {
		return -EAGAIN;
	}
	if (argc > 1 && !full) {
		shell_error(sh, "usage: rxb start [full]");
		return -EINVAL;
	}
	rxb_start(full);
	dwmac_rx_stats_get(eth_dev, &base_ds);
	last_ds = base_ds;
	mmc_read(base_mmc, NULL, MAX_MMC, &n);
	cpu_free(cpu);
	rxb_snapshot(&last_snap);
	last_ms = k_uptime_get();
	shell_print(sh, "taking UDP to port %u (%s check); send to:", RXB_PORT,
		    full ? "full" : "sparse");
	print_addrs(sh);
	return 0;
}

static int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (not_started(sh)) {
		return -EAGAIN;
	}
	rxb_stop();
	shell_print(sh, "stopped: port %u frames are dropped, not passed to the stack", RXB_PORT);
	return 0;
}

static uint32_t rate_x10(uint64_t bytes, int64_t ms)
{
	return ms <= 0 ? 0U : (uint32_t)(bytes * 8U * 10U / ((uint64_t)ms * 1000U));
}

static int cmd_stat(const struct shell *sh, size_t argc, char **argv)
{
	struct rxb_snap s;
	struct dwmac_rx_stats ds;
	uint32_t cpu[MAX_CPUS] = {0};
	uint32_t mmc[MAX_MMC];
	const char *keys[MAX_MMC];
	unsigned int nmmc;
	int64_t now = k_uptime_get(), dt = now - last_ms;
	uint64_t dp, df, dpl;
	uint32_t pps, mp, mf, d_irq, d_off;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (not_started(sh)) {
		return -EAGAIN;
	}
	cpu_free(cpu);
	rxb_snapshot(&s);
	dwmac_rx_stats_get(eth_dev, &ds);
	mmc_read(mmc, keys, MAX_MMC, &nmmc);

	dp = s.t.pkts - last_snap.t.pkts;
	df = s.t.frame_bytes - last_snap.t.frame_bytes;
	dpl = s.t.payload_bytes - last_snap.t.payload_bytes;
	pps = dt <= 0 ? 0U : (uint32_t)(dp * MS_PER_S / (uint64_t)dt);
	mp = rate_x10(dpl, dt);
	mf = rate_x10(df, dt);
	d_irq = ds.irqs - last_ds.irqs;
	d_off = ds.offered - last_ds.offered;

	shell_print(sh, "rxb: %s, %s check, %lld s since start", s.running ? "running" : "stopped",
		    s.full_check ? "full" : "sparse", (now - s.start_ms) / MS_PER_S);
	shell_print(sh, "  last %lld ms: %u.%u Mb/s UDP payload, %u.%u Mb/s frames, %u packets/s",
		    dt, mp / 10U, mp % 10U, mf / 10U, mf % 10U, pps);
	shell_print(sh, "  total: %llu packets, %llu payload bytes, bad %u",
		    s.t.pkts, s.t.payload_bytes, s.t.bad);
	shell_print(sh, "  sequence: lost %u in %u gaps, duplicates %u, out of order %u "
			"(net loss %d), sender restarts %u", s.t.seq.lost, s.t.seq.gaps,
		    s.t.seq.dups, s.t.seq.ooo, (int)s.t.seq.lost - (int)s.t.seq.ooo,
		    s.t.seq.restarts);
	shell_print(sh, "  numbers: first %u, latest %u, next expected %u", s.pos.first,
		    s.pos.last, s.pos.expected);
	shell_print(sh, "  hook: %u in the ring (IPv4 %u, IPv6 %u), ring full drops %u, "
			"dropped while stopped %u", s.h.taken, s.h.ip4, s.h.ip6, s.h.ring_full,
		    s.h.idle);
	shell_print(sh, "  ring: at most %u of %u waiting, consumer %u.%u us a packet",
		    s.ring_high, s.ring_slots, s.consumer_us_per_pkt_x10 / 10U,
		    s.consumer_us_per_pkt_x10 % 10U);
	shell_print(sh, "  interrupts: %u in the interval, %u.%u frames each; since start "
			"%u interrupts, %u frames",
		    d_irq, d_irq == 0U ? 0U : d_off * 10U / d_irq / 10U,
		    d_irq == 0U ? 0U : d_off * 10U / d_irq % 10U, ds.irqs - base_ds.irqs,
		    ds.offered - base_ds.offered);
	shell_print(sh, "  pool: refills found it empty %u, MAC missed frames %u, "
			"MAC FIFO overflows %u (since start)", ds.starved - base_ds.starved,
		    ds.missed - base_ds.missed, ds.fifo_overflow - base_ds.fifo_overflow);
	shell_print(sh, "  CPU free %u %% on CPU 0, %u %% on CPU 1 (since the last stat, so a mean "
			"over the interval, idle part included)", cpu[0],
		    ncpu > 1 ? cpu[1] : 0U);
	for (unsigned int i = 0; i < nmmc; i++) {
		if (strncmp(keys[i], MMC_RX_PREFIX, strlen(MMC_RX_PREFIX)) == 0) {
			shell_print(sh, "  MMC %s %u", keys[i], mmc[i] - base_mmc[i]);
		}
	}
	last_snap = s;
	last_ds = ds;
	last_ms = now;
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(rxb_cmds,
	SHELL_CMD_ARG(start, NULL, "[full]: zero the counters and take frames", cmd_start, 1, 1),
	SHELL_CMD(stop, NULL, "stop counting", cmd_stop),
	SHELL_CMD(stat, NULL, "rates, counts and counters", cmd_stat),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(rxb, &rxb_cmds, "receive bench: start [full] | stop | stat", NULL);
