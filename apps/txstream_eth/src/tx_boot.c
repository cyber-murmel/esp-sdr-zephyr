/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * "tx boot": where the image, the heaps and the stacks ended up (the checks of scripts/tcm_groups.py
 * --tx, at run time), and the canary of the memory the DAC banks and the engine must not touch.
 */

#include <stdlib.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/kernel_structs.h>
#include <zephyr/logging/log.h>
#include <soc/hp_system_reg.h>
#if defined(CONFIG_ESP_SPIRAM)
#include <esp_clk_tree.h>
#include <esp_psram.h>
#endif

#include <esp_sdr/esp_sdr.h>

#include "tx_boot.h"
#include "tx_stream.h"

LOG_MODULE_DECLARE(txstream, LOG_LEVEL_INF);

extern char _end[];
extern char _heap_sentry[];
extern char __esp_sdr_grp3_start[];
extern char __esp_sdr_grp3_end[];
#if defined(CONFIG_ESP_SDR_S31_TXMEM_PSRAM)
extern char __esp_sdr_psram_start[];
extern char __esp_sdr_psram_end[];
#endif
bool esp_sdr_s31_dump_ready(void);

#define GRP3_ORG 0x2F060000UL
#define GRP3_MAX 0x2F07AFB0UL
#define PSRAM_BASE 0x50000000UL
#define PSRAM_END  0x58000000UL
#define LPRAM_BASE 0x2E000000UL
#define LPRAM_END  0x2E008000UL
#define MAX_SHOWN 4U
#define MAX_THREADS 24U

static uint32_t seed;
static bool filled, with_stk;

static bool in_psram(uintptr_t a)
{
	return a >= PSRAM_BASE && a < PSRAM_END;
}

/* "group N", "psram" or "lp ram": where an address lies, for the reports */
static const char *where(uintptr_t a, char *buf, size_t n)
{
	if (in_psram(a)) {
		return "psram";
	}
	if (a >= LPRAM_BASE && a < LPRAM_END) {
		return "lp ram";
	}
	snprintk(buf, n, "group %u", GROUP_OF(a));
	return buf;
}

/* ---- PSRAM and the PLL-derived clocks ---- */

#if defined(CONFIG_ESP_SPIRAM)
/* The PSRAM's own init is a SYS_INIT at PRE_KERNEL_1 / CONFIG_KERNEL_INIT_PRIORITY_DEFAULT (soc/espressif/common/
 * esp_psram.c), the MSPI timing tuning and the chip's init included. These two bracket it by the CPU's cycle counter
 * (a 32 bit counter: 13 s at 320 MHz, enough; the kernel's clock is not up yet at PRE_KERNEL_1). */
static uint32_t psram_t0, psram_t1;

static uint32_t cycles(void)
{
	uint32_t v;

	__asm__ volatile("csrr %0, cycle" : "=r"(v));
	return v;
}

static uint32_t psram_us(void)
{
	return (psram_t1 - psram_t0) / (ESP_SDR_CPU_HZ / 1000000U);
}

static int psram_mark0(void)
{
	psram_t0 = cycles();
	return 0;
}

static int psram_mark1(void)
{
	psram_t1 = cycles();
	return 0;
}

/* literal priorities (the macro's section names do not take an expression) */
BUILD_ASSERT(CONFIG_KERNEL_INIT_PRIORITY_DEFAULT == 40, "tx_boot.c brackets the PSRAM init at priority 40");
SYS_INIT(psram_mark0, PRE_KERNEL_1, 39);
SYS_INIT(psram_mark1, PRE_KERNEL_1, 41);

static uint32_t clk_mhz(soc_module_clk_t c, uint32_t *hz_out)
{
	uint32_t hz = 0U;

	if (esp_clk_tree_src_get_freq_hz(c, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &hz) != ESP_OK) {
		hz = 0U;
	}
	*hz_out = hz;
	return hz / 1000000U;
}

void tx_mem_report(void)
{
	uint32_t mpll, apll, f80, cpll;
	const uint32_t us = psram_us();

	(void)clk_mhz(SOC_MOD_CLK_MPLL, &mpll);
	(void)clk_mhz(SOC_MOD_CLK_APLL, &apll);
	(void)clk_mhz(SOC_MOD_CLK_PLL_F80M, &f80);
	(void)clk_mhz(SOC_MOD_CLK_CPLL, &cpll);
	/* short messages: the log buffer is 512 B, a long one is dropped whole */
	LOG_INF("PSRAM %s %uKB %dMHz init %uus", esp_psram_is_initialized() ? "up" : "NOT UP",
		(unsigned int)(esp_psram_get_size() / 1024U), CONFIG_SPIRAM_SPEED, us);
	LOG_INF("MPLL %u APLL(EMAC) %u", mpll, apll);
	LOG_INF("PLL_F80M(PTP) %u CPLL %u", f80, cpll);
	if (mpll != 500000000U || apll != 125000000U) {
		LOG_WRN("MPLL not 500 MHz or APLL not 125 MHz");
	}
#if defined(CONFIG_ESP_SDR_S31_TXMEM_PSRAM)
	LOG_INF("PSRAM %uB: stacks, net mgmt queue", (unsigned int)(__esp_sdr_psram_end - __esp_sdr_psram_start));
#endif
}

static void psram_print(const struct shell *sh)
{
	uint32_t mpll, apll, f80, cpll;

	(void)clk_mhz(SOC_MOD_CLK_MPLL, &mpll);
	(void)clk_mhz(SOC_MOD_CLK_APLL, &apll);
	(void)clk_mhz(SOC_MOD_CLK_PLL_F80M, &f80);
	(void)clk_mhz(SOC_MOD_CLK_CPLL, &cpll);
	shell_print(sh, "  PSRAM %s, %u KB at %d MHz, init %u us; MPLL %u Hz, EMAC ref (APLL) %u Hz, PLL_F80M %u Hz, "
		    "CPLL %u Hz", esp_psram_is_initialized() ? "up" : "NOT INITIALIZED",
		    (unsigned int)(esp_psram_get_size() / 1024U), CONFIG_SPIRAM_SPEED,
		    psram_us(), mpll, apll, f80, cpll);
}
#else
void tx_mem_report(void)
{
	LOG_INF("PSRAM: off");
}

static void psram_print(const struct shell *sh)
{
	shell_print(sh, "  PSRAM off");
}
#endif


static uint32_t pat(uintptr_t a)
{
	return (uint32_t)a * 0x9E3779B1U ^ seed;
}

static uintptr_t tail_start(void)
{
	return ROUND_UP((uintptr_t)__esp_sdr_grp3_end, 16U);
}

static void stack_range(uintptr_t *a, uintptr_t *e)
{
	size_t n;

	*a = (uintptr_t)tx_stream_stack(&n);
	*e = *a + n;
}

static void fill_range(uintptr_t a, uintptr_t e)
{
	for (; a < e; a += 4U) {
		*(volatile uint32_t *)a = pat(a);
	}
}

void tx_canary_fill(bool with_stack)
{
	uintptr_t sa, se;

	seed = k_cycle_get_32() | 1U;
	fill_range(TX_GROUP1, TX_GROUP1 + 2U * TCM_GROUP_SIZE);
	fill_range(tail_start(), GRP3_MAX);
	stack_range(&sa, &se);
	if (with_stack) {
		fill_range(sa, se);
	}
	with_stk = with_stack;
	filled = true;
}

static unsigned int check_range(const struct shell *sh, const char *name, uintptr_t a, uintptr_t e)
{
	unsigned int bad = 0U;

	for (uintptr_t p = a; p < e; p += 4U) {
		const uint32_t got = *(volatile uint32_t *)p;

		if (got != pat(p)) {
			if (bad++ < MAX_SHOWN) {
				shell_print(sh, "  %s 0x%08lx: 0x%08x, filled 0x%08x", name, (unsigned long)p,
					    got, pat(p));
			}
		}
	}
	return bad;
}

unsigned int tx_canary_check(const struct shell *sh)
{
	uintptr_t sa, se;
	unsigned int g1, g2, t3, stk = 0U;

	if (!filled) {
		shell_error(sh, "canary: not filled");
		return 1U;
	}
	g1 = check_range(sh, "group 1", TX_GROUP1, TX_GROUP2);
	g2 = check_range(sh, "group 2", TX_GROUP2, TX_GROUP2 + TCM_GROUP_SIZE);
	t3 = check_range(sh, "group 3 tail", tail_start(), GRP3_MAX);
	stack_range(&sa, &se);
	if (with_stk) {
		stk = check_range(sh, "stream stack", sa, se);
	}
	shell_print(sh, "canary: group 1 %u, group 2 %u, group 3 free tail %u (%u B), stream stack %s%u "
		    "words changed: %s", g1, g2, t3, (unsigned int)(GRP3_MAX - tail_start()),
		    with_stk ? "" : "not checked ", with_stk ? stk : 0U,
		    g1 + g2 + t3 + stk == 0U ? "PASS" : "FAIL");
	return g1 + g2 + t3 + stk;
}

static unsigned int fails;

static void check(const struct shell *sh, const char *what, bool ok)
{
	shell_print(sh, "  %-4s %s", ok ? "ok" : "FAIL", what);
	fails += ok ? 0U : 1U;
}

/* k_thread_foreach() runs its callback with the scheduler lock held: a shell_print() there is lost.
 * The stacks are collected unlocked and printed afterwards.
 */
struct thr_info {
	const char *name;
	uintptr_t start;
	size_t size;
};

struct thr_ctx {
	struct thr_info t[MAX_THREADS];
	unsigned int n, total;
};

static void thr_cb(const struct k_thread *t, void *user)
{
	struct thr_ctx *c = user;
	const char *name = k_thread_name_get((k_tid_t)t);

	if (c->total++ < MAX_THREADS) {
		c->t[c->n].name = name != NULL ? name : "?";
		c->t[c->n].start = t->stack_info.start;
		c->t[c->n].size = t->stack_info.size;
		c->n++;
	}
}

int tx_boot_report(const struct shell *sh)
{
	void *km = k_malloc(32), *lm = malloc(32);
	const uint32_t own = sys_read32(HP_SYSTEM_TCM_DATA_DUMP_CTRL_REG);
	const uintptr_t rx = tx_last_rx_buf();
	static struct thr_ctx ctx;

	fails = 0U;
	shell_print(sh, "tx boot: ownership register 0x%08x (7:0 group 0, 15:8 group 1, 23:16 group 2, "
		    "31:24 group 3), dump_ready %d", (unsigned int)own, esp_sdr_s31_dump_ready());
	shell_print(sh, "  image end %p (group %u), libc arena up to %p, group 3 noinit %p..%p (%u of %u B)",
		    _end, GROUP_OF(_end), _heap_sentry, __esp_sdr_grp3_start, __esp_sdr_grp3_end,
		    (unsigned int)(__esp_sdr_grp3_end - __esp_sdr_grp3_start),
		    (unsigned int)(GRP3_MAX - GRP3_ORG));
	shell_print(sh, "  k_malloc block %p (group %u), malloc block %p (group %u), last receive buffer "
		    "0x%08lx (group %u)", km, GROUP_OF(km), lm, GROUP_OF(lm), (unsigned long)rx,
		    GROUP_OF(rx));
	check(sh, "image ends below group 1", (uintptr_t)_end <= TX_GROUP1);
	check(sh, "libc arena ends below group 1", (uintptr_t)_heap_sentry <= TX_GROUP1);
	check(sh, "group 3 section starts at 0x2f060000", (uintptr_t)__esp_sdr_grp3_start == GRP3_ORG);
	check(sh, "group 3 section ends below the ROM boot stack", (uintptr_t)__esp_sdr_grp3_end <= GRP3_MAX);
	check(sh, "kernel heap block is in group 3", km != NULL && GROUP_OF(km) == 3U);
	check(sh, "libc heap block is in group 0", lm != NULL && GROUP_OF(lm) == 0U);
	if (rx != 0U) {
		check(sh, "receive buffers are in group 3", GROUP_OF(rx) == 3U);
	} else {
		shell_print(sh, "  --   no frame seen yet (send a ping, then tx boot again): receive buffer group unchecked");
	}
	k_free(km);
	free(lm);
	for (unsigned int i = 0; i < arch_num_cpus(); i++) {
		const uintptr_t is = (uintptr_t)_kernel.cpus[i].irq_stack;

		shell_print(sh, "  interrupt stack top cpu %u: %08lx (group %u)", i, (unsigned long)is,
			    GROUP_OF(is - 1U));
		check(sh, "interrupt stack is in group 3", GROUP_OF(is - 1U) == 3U);
	}
	ctx.n = ctx.total = 0U;
	k_thread_foreach_unlocked(thr_cb, &ctx);
	for (unsigned int i = 0; i < ctx.n; i++) {
		const uintptr_t a = ctx.t[i].start;

		char w[16];
		const char *n = ctx.t[i].name;
		/* what CPU 1 or the stream's timing runs on stays internal, and in no DAC bank */
		const bool internal = strcmp(n, "tx_flip") == 0 || strcmp(n, "tx_stat") == 0 ||
				      strncmp(n, "idle", 4) == 0;

		shell_print(sh, "  stack %-14s %08lx size %5u %s", n, (unsigned long)a, (unsigned int)ctx.t[i].size,
			    where(a, w, sizeof(w)));
		check(sh, "  thread stack is not in a DAC bank", in_psram(a) || (GROUP_OF(a) != 1U && GROUP_OF(a) != 2U));
		if (internal) {
			check(sh, "  the flip, status and idle threads' stacks are internal", !in_psram(a));
		}
	}
	check(sh, "all threads listed", ctx.total == ctx.n);
	/* the banks: group 1 and 2 hold nothing of the image (the engine owns them, or nothing does) */
	psram_print(sh);
	check(sh, "ownership register has no group 0 or group 3 bit (nothing there goes to the engine)",
	      (own & 0xff0000ffU) == 0U);
	tx_canary_fill(false);
	k_msleep(200);
	check(sh, "canary of groups 1 and 2 and the group 3 tail held for 200 ms", tx_canary_check(sh) == 0U);
	shell_print(sh, "tx boot: %s", fails == 0U ? "PASS" : "FAIL");
	return fails == 0U ? 0 : -EIO;
}
