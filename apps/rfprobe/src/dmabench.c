/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * "probe dma" (ESP32-S3): what the two DMAs of esp_sdr_dma.h can do for a radio that wants to be
 * coordinated by DMA, with as little CPU as possible. Results and reasoning: doc/dev/dma-chains.md.
 *
 *   sram            GDMA SRAM to SRAM, descriptor chains against memcpy
 *   reg [mac]       GDMA into a register: refused (IN_DSCR_ERR), kept to document it
 *   regs [n|mac]    peripheral backup DMA, SRAM to registers and back, n words
 *   map             the backup DMA's map mode, by experiment
 *   kick            the backup DMA writes a GDMA chain's link registers (starts it)
 *
 * The register block of the "regs" and "map" rows is RMT RAM: 384 words of plain read/write
 * memory on the APB (0x60016800), nothing reads it unless an RMT channel runs (none does: no
 * driver of this image enables the RMT), no side effects when written, reachable by the backup
 * DMA (SENSITIVE_BACKUP_BUS_PMS_CONSTRAIN_RMT, open at reset). Its clock is on only while a row
 * runs, the words (and SYS_CONF, clock and reset) are saved first and restored after.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/barrier.h>

#include <esp_private/periph_ctrl.h>
#include <hal/clk_gate_ll.h>
#include <hal/gdma_ll.h>
#include <hal/rmt_ll.h>
#include <soc/peri_backup_reg.h>
#include <soc/reg_base.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/sensitive_reg.h>
#include <soc/system_reg.h>

#include <esp_sdr/esp_sdr_dma.h>
#include <esp_sdr/esp_sdr_xtensa.h>
#if defined(CONFIG_ESP_SDR_RING)
#include <esp_sdr/esp_sdr_ring.h>
#endif
#if defined(CONFIG_ESP_SDR_ARB)
#include <esp_sdr/esp_sdr_arb.h>
#endif

#include "dmabench.h"

#define U(x) ((unsigned int)(x))
#define FLD(v, n) (((v) >> n##_S) & n##_V)

#define WAIT_US 5000U
#define BK_TIMEOUT_US 1000U

/* The Wi-Fi MAC's dump engine (esp_sdr_priv.h): DUMP_CONFIG_REG is rewritten whole by every arm. */
#define MAC_DUMP_CTRL_REG   0x60033d5cU
#define MAC_DUMP_CTRL_RUN   BIT(31)
#define MAC_DUMP_CONFIG_REG 0x60033d90U

extern bool esp_sdr_ready;

/* 8 channels of 48 words (SOC_RMT_MEM_WORDS_PER_CHANNEL). */
#define RMT_WORDS 384U
#define RMT_RAM   (DR_REG_RMT_BASE + 0x800U)


static uint32_t cyc_us(void)
{
	return sys_clock_hw_cycles_per_sec() / 1000000U;
}

static uint32_t mbps_x100(uint32_t bytes, uint32_t cyc)
{
	return (uint32_t)((uint64_t)bytes * sys_clock_hw_cycles_per_sec() / 10000U / MAX(cyc, 1U));
}

/* ---- GDMA SRAM to SRAM ---- */

#define DMA_TX_DESCS 210U /* 80 B pieces of 16 KiB */
#define DMA_RX_DESCS 8U
#define DMA_BYTES    16384U
#define CANARY       0xc0ffee11U
#define CANARY_WORDS 16U

static struct esp_sdr_dma_desc dma_tx[DMA_TX_DESCS], dma_rx[DMA_RX_DESCS];
/*
 * Buffers of a chain and the canaries behind them: the first two quarters of the capture bank
 * (the image has no room for 32 KiB; the CPUs own the bank while nothing captures or loops).
 */
extern uint32_t __esp_sdr_bank_start[];
#define buf_src (__esp_sdr_bank_start)
#define buf_dst (__esp_sdr_bank_start + (DMA_BYTES / 4U + CANARY_WORDS))
/* The register rows' word lists follow: pattern, read back, the RMT RAM's saved words. */
#define LIST_BASE (__esp_sdr_bank_start + 2U * (DMA_BYTES / 4U + CANARY_WORDS))
#define pat (LIST_BASE)
#define back (LIST_BASE + RMT_WORDS)
#define rmt_saved (LIST_BASE + 2U * RMT_WORDS)
#define BANK_OK ESP_SDR_DMA_CPU_OWNS_BANKS
static char dma_dbg[300];

static int gdma_open(const struct shell *sh, struct esp_sdr_dma *h)
{
	int ret = esp_sdr_dma_claim(h);

	if (ret != 0) {
		shell_error(sh, "dma: no gdma channel (%d)", ret);
		return ret;
	}
	shell_print(sh, "gdma channel pair %u, descriptors %u B each, cpu %u MHz", h->ch,
		    U(sizeof(struct esp_sdr_dma_desc)), U(cyc_us()));
	return 0;
}

/* One chain: best of 4 runs; setup is the CPU time to write the lists. */
static void dma_row(const struct shell *sh, struct esp_sdr_dma *h, uint32_t bytes, uint32_t piece,
		    bool burst)
{
	struct esp_sdr_dma_job job = {.tx = dma_tx, .tx_max = DMA_TX_DESCS, .rx = dma_rx,
				      .rx_max = DMA_RX_DESCS};
	uint32_t setup = 0, best = UINT32_MAX, t0, bad = 0;
	int ret = 0;

	esp_sdr_dma_burst(h, burst);
	for (uint32_t k = 0; k < CANARY_WORDS; k++) {
		buf_dst[bytes / 4U + k] = CANARY;
		buf_src[bytes / 4U + k] = CANARY;
	}
	for (int i = 0; i < 4; i++) {
		unsigned int key = arch_irq_lock();

		memset(buf_dst, 0, bytes);
		t0 = esp_sdr_ccount();
		ret = esp_sdr_dma_copy_build(&job, buf_dst, buf_src, bytes, piece, BANK_OK);
		setup = esp_sdr_ccount() - t0;
		if (ret == 0) {
			t0 = esp_sdr_ccount();
			ret = esp_sdr_dma_start(h, &job);
			if (ret == 0) {
				ret = esp_sdr_dma_wait(h, WAIT_US);
			}
			best = MIN(best, esp_sdr_ccount() - t0);
		}
		arch_irq_unlock(key);
		if (ret != 0) {
			break;
		}
	}
	if (ret != 0) {
		shell_print(sh, "dma sram %5u B in %3u x %4u B%s: error %d", U(bytes), U(job.ntx),
			    U(piece), burst ? " burst" : "", ret);
		shell_print(sh, "  %s", esp_sdr_dma_debug(h, dma_dbg, sizeof(dma_dbg)));
		return;
	}
	for (uint32_t k = 0; k < CANARY_WORDS; k++) {
		bad += (buf_dst[bytes / 4U + k] != CANARY) + (buf_src[bytes / 4U + k] != CANARY);
	}
	shell_print(sh, "dma sram %5u B in %3u x %4u B%s: setup %u cyc, dma %u cyc (%u per desc),"
		    " %u.%02u MB/s, %s, canaries %s", U(bytes), U(job.ntx), U(piece),
		    burst ? " burst" : "", U(setup), U(best), U(best / job.ntx),
		    U(mbps_x100(bytes, best) / 100U), U(mbps_x100(bytes, best) % 100U),
		    memcmp(buf_dst, buf_src, bytes) == 0 ? "same as source" : "DIFFERS",
		    bad == 0U ? "intact" : "OVERWRITTEN");
}

/* A loop of reads, alone and while a 1600 B chain runs: whether it slowed down. */
static void dma_overlap(const struct shell *sh, struct esp_sdr_dma *h)
{
	struct esp_sdr_dma_job job = {.tx = dma_tx, .tx_max = DMA_TX_DESCS, .rx = dma_rx,
				      .rx_max = DMA_RX_DESCS};
	/* Image data (the descriptors') and the destination buffer. */
	const volatile uint32_t *where[2] = {(const volatile uint32_t *)dma_tx, buf_dst + 2048U};
	static const char *const name[2] = {"image", "destination"};
	enum { READS = 1024 };

	if (esp_sdr_dma_copy_build(&job, buf_dst, buf_src, DMA_BYTES, 1600U, BANK_OK) != 0) {
		return;
	}
	for (int w = 0; w < 2; w++) {
		uint32_t alone, during, t0, sum = 0;
		unsigned int key = arch_irq_lock();
		bool done;

		t0 = esp_sdr_ccount();
		for (int k = 0; k < READS; k++) {
			sum += where[w][k];
		}
		alone = esp_sdr_ccount() - t0;
		(void)esp_sdr_dma_start(h, &job);
		t0 = esp_sdr_ccount();
		for (int k = 0; k < READS; k++) {
			sum += where[w][k];
		}
		during = esp_sdr_ccount() - t0;
		done = esp_sdr_dma_done(h);
		(void)esp_sdr_dma_wait(h, WAIT_US);
		arch_irq_unlock(key);
		shell_print(sh, "dma overlap, cpu reads %u words of %s: alone %u cyc, with the"
			    " chain %u cyc (chain %s)%s", U(READS), name[w], U(alone), U(during),
			    done ? "done before the loop ended" : "still running at its end",
			    sum == 0xdeadbeefU ? "?" : "");
	}
}

static int bench_sram(const struct shell *sh)
{
	static const uint16_t pieces[] = {4092U, 1600U, 400U, 160U, 80U};
	struct esp_sdr_dma h;
	uint32_t x = 1U, t0, cpu = UINT32_MAX;
	int ret = gdma_open(sh, &h);

	if (ret != 0) {
		return ret;
	}
	shell_print(sh, "dma sram: src 0x%08x dst 0x%08x, lists tx 0x%08x rx 0x%08x",
		    U(buf_src), U(buf_dst), U(dma_tx), U(dma_rx));
	for (uint32_t k = 0; k < DMA_BYTES / 4U; k++) {
		x = x * 1664525U + 1013904223U;
		buf_src[k] = x;
	}
	/* A single descriptor, then chains over 16 KiB. */
	dma_row(sh, &h, 4092U, 4092U, false);
	for (size_t i = 0; i < ARRAY_SIZE(pieces); i++) {
		dma_row(sh, &h, DMA_BYTES, pieces[i], false);
		if (pieces[i] % 16U == 0U) {
			dma_row(sh, &h, DMA_BYTES, pieces[i], true);
		}
	}
	for (int i = 0; i < 4; i++) {
		unsigned int key = arch_irq_lock();

		t0 = esp_sdr_ccount();
		memcpy(buf_dst, buf_src, DMA_BYTES);
		cpu = MIN(cpu, esp_sdr_ccount() - t0);
		arch_irq_unlock(key);
	}
	shell_print(sh, "cpu memcpy %u B: %u cyc, %u.%02u MB/s", U(DMA_BYTES), U(cpu),
		    U(mbps_x100(DMA_BYTES, cpu) / 100U), U(mbps_x100(DMA_BYTES, cpu) % 100U));
	esp_sdr_dma_burst(&h, false);
	dma_overlap(sh, &h);
	esp_sdr_dma_release(&h);
	return 0;
}

/* ---- GDMA into a register: refused ---- */

/* The radio's clock on and the dump engine idle, as the earlier wpan bench required. */
static int mac_prep(const struct shell *sh)
{
#if defined(CONFIG_ESP_SDR_RING)
	if (esp_sdr_ring_active() || esp_sdr_ring_burst_busy()) {
		shell_error(sh, "dma mac: ring or burst running");
		return -EBUSY;
	}
#endif
	if (!esp_sdr_ready) {
		shell_error(sh, "dma mac: radio never initialised");
		return -EBUSY;
	}
#if defined(CONFIG_ESP_SDR_ARB)
	if (!esp_sdr_arb_wifi_attached())
#endif
	{
		PERIPH_RCC_ATOMIC() {
			periph_ll_wifi_module_enable_clk_clear_rst();
		}
	}
	if ((REG_READ(MAC_DUMP_CTRL_REG) & MAC_DUMP_CTRL_RUN) != 0U) {
		shell_error(sh, "dma mac: dump engine running (DUMP_CTRL 0x%08x)",
			    U(REG_READ(MAC_DUMP_CTRL_REG)));
		return -EBUSY;
	}
	return 0;
}

/* Violation monitors: save the control register, clear the latch, enable. */
static uint32_t pms_arm(uint32_t ctl_reg, uint32_t en, uint32_t clr)
{
	uint32_t saved = REG_READ(ctl_reg);

	REG_WRITE(ctl_reg, saved | en | clr);
	REG_WRITE(ctl_reg, (saved | en) & ~clr);
	return saved;
}

static void gdma_mon_print(const struct shell *sh, uint32_t saved)
{
	uint32_t m2 = REG_READ(SENSITIVE_DMA_APBPERI_PMS_MONITOR_2_REG);
	uint32_t m3 = REG_READ(SENSITIVE_DMA_APBPERI_PMS_MONITOR_3_REG);

	shell_print(sh, "  gdma pms monitor (was ctl 0x%x, lock %u): violation %u addr field 0x%06x"
		    " wr %u byteen 0x%04x world %u", U(saved),
		    U(REG_READ(SENSITIVE_DMA_APBPERI_PMS_MONITOR_0_REG) & 1U),
		    U(FLD(m2, SENSITIVE_DMA_APBPERI_PMS_MONITOR_VIOLATE_INTR)),
		    U(FLD(m2, SENSITIVE_DMA_APBPERI_PMS_MONITOR_VIOLATE_STATUS_ADDR)),
		    U(FLD(m3, SENSITIVE_DMA_APBPERI_PMS_MONITOR_VIOLATE_STATUS_WR)),
		    U(FLD(m3, SENSITIVE_DMA_APBPERI_PMS_MONITOR_VIOLATE_STATUS_BYTEEN)),
		    U(FLD(m2, SENSITIVE_DMA_APBPERI_PMS_MONITOR_VIOLATE_STATUS_WORLD)));
}

static void backup_mon_print(const struct shell *sh, uint32_t saved)
{
	uint32_t m2 = REG_READ(SENSITIVE_BACKUP_BUS_PMS_MONITOR_2_REG);

	shell_print(sh, "  backup bus pms monitor (was ctl 0x%x, lock %u): violation %u haddr 0x%08x"
		    " hwrite %u hsize %u htrans %u", U(saved),
		    U(REG_READ(SENSITIVE_BACKUP_BUS_PMS_MONITOR_0_REG) & 1U),
		    U(FLD(m2, SENSITIVE_BACKUP_BUS_PMS_MONITOR_VIOLATE_INTR)),
		    U(REG_READ(SENSITIVE_BACKUP_BUS_PMS_MONITOR_3_REG)),
		    U(FLD(m2, SENSITIVE_BACKUP_BUS_PMS_MONITOR_VIOLATE_STATUS_HWRITE)),
		    U(FLD(m2, SENSITIVE_BACKUP_BUS_PMS_MONITOR_VIOLATE_STATUS_HSIZE)),
		    U(FLD(m2, SENSITIVE_BACKUP_BUS_PMS_MONITOR_VIOLATE_STATUS_HTRANS)));
}

static uint32_t backup_mon_arm(void)
{
	return pms_arm(SENSITIVE_BACKUP_BUS_PMS_MONITOR_1_REG,
		       SENSITIVE_BACKUP_BUS_PMS_MONITOR_VIOLATE_EN,
		       SENSITIVE_BACKUP_BUS_PMS_MONITOR_VIOLATE_CLR);
}

/* Value a register keeps after a write of val (not every bit of DUMP_CONFIG may stick). */
static uint32_t reg_stick(volatile uint32_t *reg, uint32_t val, uint32_t *zero)
{
	uint32_t e;

	*reg = 0U;
	*zero = *reg;
	*reg = val;
	e = *reg;
	*reg = 0U;
	return e;
}

/*
 * One word through the GDMA into RTC_CNTL_STORE7 (a general purpose register of the always-powered
 * RTC block, used only by the deep sleep wake stub's checksum) or DUMP_CONFIG_REG, with both PMS
 * monitors armed. The GDMA refuses itself (RX IN_DSCR_ERR); neither monitor records anything.
 */
static int bench_reg(const struct shell *sh, bool mac)
{
	const char *name = mac ? "DUMP_CONFIG_REG" : "RTC_CNTL_STORE7";
	volatile uint32_t *reg = (volatile uint32_t *)(mac ? MAC_DUMP_CONFIG_REG : RTC_CNTL_STORE7_REG);
	static uint32_t val;
	struct esp_sdr_dma_job job = {.tx = dma_tx, .tx_max = DMA_TX_DESCS, .rx = dma_rx,
				      .rx_max = DMA_RX_DESCS};
	struct esp_sdr_dma h;
	uint32_t old, after, expect, zero, zero2, mon_dma, mon_bk;
	int ret = mac ? mac_prep(sh) : 0;

	if (ret == 0) {
		ret = gdma_open(sh, &h);
	}
	if (ret != 0) {
		return ret;
	}
	old = *reg;
	val = mac ? 0x000d2141U : 0xa5c3e1f7U;
	expect = reg_stick(reg, val, &zero);
	zero2 = *reg;
	mon_dma = pms_arm(SENSITIVE_DMA_APBPERI_PMS_MONITOR_1_REG,
			  SENSITIVE_DMA_APBPERI_PMS_MONITOR_VIOLATE_EN,
			  SENSITIVE_DMA_APBPERI_PMS_MONITOR_VIOLATE_CLR);
	mon_bk = backup_mon_arm();
	ret = esp_sdr_dma_copy_build(&job, (void *)reg, &val, sizeof(val), sizeof(val), 0U);
	if (ret == 0) {
		ret = esp_sdr_dma_start(&h, &job);
	}
	if (ret == 0) {
		ret = esp_sdr_dma_wait(&h, WAIT_US);
	}
	after = *reg;
	*reg = old;
	shell_print(sh, "  gdma raw int rx 0x%x tx 0x%x (after a failed wait the channel was reset;"
		    " the snapshot below is from before)",
		    U(gdma_ll_rx_get_interrupt_status(GDMA_LL_GET_HW(0), h.ch, true)),
		    U(gdma_ll_tx_get_interrupt_status(GDMA_LL_GET_HW(0), h.ch, true)));
	gdma_mon_print(sh, mon_dma);
	backup_mon_print(sh, mon_bk);
	REG_WRITE(SENSITIVE_DMA_APBPERI_PMS_MONITOR_1_REG, mon_dma);
	REG_WRITE(SENSITIVE_BACKUP_BUS_PMS_MONITOR_1_REG, mon_bk);
	shell_print(sh, "  %s", esp_sdr_dma_debug(&h, dma_dbg, sizeof(dma_dbg)));
	shell_print(sh, "  rx desc 0x%08x 0x%08x 0x%08x, tx desc 0x%08x 0x%08x 0x%08x",
		    U(dma_rx[0].w[0]), U(dma_rx[0].w[1]), U(dma_rx[0].w[2]), U(dma_tx[0].w[0]),
		    U(dma_tx[0].w[1]), U(dma_tx[0].w[2]));
	shell_print(sh, "dma reg: cpu write sticks %s (0 reads 0x%08x, 0x%08x reads 0x%08x);"
		    " gdma write to %s: ret %d, reads 0x%08x (before it 0x%08x, wanted 0x%08x): %s",
		    expect != zero ? "yes" : "NO", U(zero), U(val), U(expect), name, ret, U(after),
		    U(zero2), U(expect), after == expect && after != zero2 ? "landed" : "did not land");
	esp_sdr_dma_release(&h);
	return ret;
}

/* ---- the backup DMA on RMT RAM ---- */

struct rmt_blk {
	uint32_t clk, rst, conf;
};


static volatile uint32_t *rmt_ram(void)
{
	return (volatile uint32_t *)RMT_RAM;
}

/* Clock on, reset released, APB access to the RAM as plain memory; the words saved. */
static void rmt_open(struct rmt_blk *b)
{
	PERIPH_RCC_ATOMIC() {
		b->clk = SYSTEM.perip_clk_en0.rmt_clk_en;
		b->rst = SYSTEM.perip_rst_en0.rmt_rst;
		rmt_ll_enable_bus_clock(0, true);
		SYSTEM.perip_rst_en0.rmt_rst = 0;
	}
	b->conf = RMT.sys_conf.val;
	rmt_ll_enable_mem_access_nonfifo(&RMT, true);
	RMT.sys_conf.mem_clk_force_on = 1;
	rmt_ll_mem_force_power_on(&RMT);
	for (uint32_t i = 0; i < RMT_WORDS; i++) {
		rmt_saved[i] = rmt_ram()[i];
	}
}

static void rmt_close(const struct rmt_blk *b)
{
	for (uint32_t i = 0; i < RMT_WORDS; i++) {
		rmt_ram()[i] = rmt_saved[i];
	}
	RMT.sys_conf.val = b->conf;
	PERIPH_RCC_ATOMIC() {
		SYSTEM.perip_rst_en0.rmt_rst = b->rst;
		rmt_ll_enable_bus_clock(0, b->clk != 0U);
	}
}

static void rmt_fill(uint32_t n, uint32_t v)
{
	for (uint32_t i = 0; i < n; i++) {
		rmt_ram()[i] = v;
	}
}

static void pat_fill(uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		pat[i] = 0xa5000000U | (i << 8) | ((n * 7U + i) & 0xffU);
	}
}

/* Poll for done like the library does, timestamped; the run was started at t0. */
static uint32_t poll_done(uint32_t t0)
{
	uint32_t t = 0;

	do {
		t = esp_sdr_ccount() - t0;
	} while (!esp_sdr_bkdma_done() && t < k_us_to_cyc_ceil32(BK_TIMEOUT_US));
	return t;
}

/* One pass: the run started (cold: every register stored again), start call and to-done cycles. */
static int bk_pass(const struct esp_sdr_bkdma_run *r, bool cold, uint32_t *start, uint32_t *total)
{
	unsigned int key = arch_irq_lock();
	uint32_t t0, t1;
	int ret;

	if (cold) {
		esp_sdr_bkdma_forget();
	}
	/* Drain the CPU's own peripheral stores first, so they are not timed. */
	barrier_dmem_fence_full();
	t0 = esp_sdr_ccount();
	esp_sdr_bkdma_start(r);
	t1 = esp_sdr_ccount();
	*total = poll_done(t0);
	*start = t1 - t0;
	arch_irq_unlock(key);
	ret = esp_sdr_bkdma_wait(k_us_to_cyc_ceil32(BK_TIMEOUT_US));
	return ret;
}

static uint32_t slope_w[2], base1[2];

/* n words to the block and back, each cold (first run) and warm (the same run again). */
static void regs_row(const struct shell *sh, uint32_t n)
{
	struct esp_sdr_bkdma_run wr, rd;
	uint32_t start, total, landed;
	int ret;

	pat_fill(n);
	if (esp_sdr_bkdma_prepare(&wr, RMT_RAM, pat, n, BANK_OK, NULL) != 0 ||
	    esp_sdr_bkdma_prepare(&rd, RMT_RAM, back, n, ESP_SDR_BKDMA_TO_MEM | BANK_OK, NULL) != 0) {
		shell_error(sh, "dma regs: bad size %u", U(n));
		return;
	}
	for (int pass = 0; pass < 2; pass++) {
		rmt_fill(n, 0U);
		ret = bk_pass(&wr, pass == 0, &start, &total);
		landed = 0U;
		for (uint32_t i = 0; i < n; i++) {
			landed += rmt_ram()[i] == pat[i];
		}
		shell_print(sh, "dma regs to regs %3u words %s: start call %u cyc, START to DONE %u cyc"
			    " (%u.%02u per word), ret %d, landed %u of %u", U(n),
			    pass == 0 ? "cold" : "warm", U(start), U(total), U(total / n),
			    U((total * 100U / n) % 100U), ret, U(landed), U(n));
		if (pass == 1) {
			if (n == 1U) {
				base1[0] = total;
			} else if (total > base1[0]) {
				slope_w[0] = (total - base1[0]) * 100U / (n - 1U);
				shell_print(sh, "  marginal %u.%02u cyc per word over 1 word",
					    U(slope_w[0] / 100U), U(slope_w[0] % 100U));
			}
		}
	}
	for (int pass = 0; pass < 2; pass++) {
		for (uint32_t i = 0; i < n; i++) {
			rmt_ram()[i] = pat[i];
			back[i] = 0U;
		}
		ret = bk_pass(&rd, pass == 0, &start, &total);
		landed = 0U;
		for (uint32_t i = 0; i < n; i++) {
			landed += back[i] == pat[i];
		}
		shell_print(sh, "dma regs to sram %3u words %s: start call %u cyc, START to DONE %u cyc"
			    " (%u.%02u per word), ret %d, landed %u of %u", U(n),
			    pass == 0 ? "cold" : "warm", U(start), U(total), U(total / n),
			    U((total * 100U / n) % 100U), ret, U(landed), U(n));
		if (pass == 1) {
			if (n == 1U) {
				base1[1] = total;
			} else if (total > base1[1]) {
				slope_w[1] = (total - base1[1]) * 100U / (n - 1U);
				shell_print(sh, "  marginal %u.%02u cyc per word over 1 word",
					    U(slope_w[1] / 100U), U(slope_w[1] % 100U));
			}
		}
	}
}

/* One word of the radio's dump engine configuration, through the API; value restored. */
static int regs_mac(const struct shell *sh)
{
	static uint32_t val = 0x000d2141U;
	volatile uint32_t *reg = (volatile uint32_t *)MAC_DUMP_CONFIG_REG;
	struct esp_sdr_bkdma_run r;
	uint32_t old, zero, expect, start, total, after, mon;
	int ret = mac_prep(sh);

	if (ret != 0) {
		return ret;
	}
	ret = esp_sdr_bkdma_prepare(&r, MAC_DUMP_CONFIG_REG, &val, 1U, 0U, NULL);
	if (ret != 0) {
		return ret;
	}
	old = *reg;
	expect = reg_stick(reg, val, &zero);
	mon = backup_mon_arm();
	for (int pass = 0; pass < 2; pass++) {
		*reg = 0U;
		ret = bk_pass(&r, pass == 0, &start, &total);
		after = *reg;
		shell_print(sh, "dma regs mac %s: cpu write sticks %s; DUMP_CONFIG_REG start call %u cyc,"
			    " START to DONE %u cyc, ret %d, reads 0x%08x (wanted 0x%08x): %s",
			    pass == 0 ? "cold" : "warm", expect != zero ? "yes" : "NO", U(start),
			    U(total), ret, U(after), U(expect),
			    after == expect && after != zero ? "landed" : "did not land");
	}
	*reg = old;
	backup_mon_print(sh, mon);
	REG_WRITE(SENSITIVE_BACKUP_BUS_PMS_MONITOR_1_REG, mon);
	return 0;
}

static int bench_regs(const struct shell *sh, size_t argc, char **argv)
{
	static const uint16_t sweep[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 384};
	struct rmt_blk blk;
	uint32_t mon, probe;
	int ret = esp_sdr_bkdma_claim();

	if (ret != 0) {
		shell_error(sh, "dma regs: backup dma busy (%d)", ret);
		return ret;
	}
	shell_print(sh, "backup dma: base 0x%08x, CONFIG 0x%08x after claim, DATE 0x%08x",
		    U(DR_REG_PERI_BACKUP_BASE), U(REG_READ(PERI_BACKUP_CONFIG_REG)),
		    U(REG_READ(PERI_BACKUP_DATE_REG)));
	if (argc > 2 && strcmp(argv[2], "mac") == 0) {
		ret = regs_mac(sh);
		esp_sdr_bkdma_release();
		return ret;
	}
	rmt_open(&blk);
	/* The block must be plain read/write for the CPU first. */
	rmt_ram()[0] = 0x12345678U;
	rmt_ram()[RMT_WORDS - 1U] = 0x9abcdef0U;
	probe = (rmt_ram()[0] == 0x12345678U) + 2U * (rmt_ram()[RMT_WORDS - 1U] == 0x9abcdef0U);
	shell_print(sh, "rmt ram 0x%08x: %u words, cpu read back first/last %s, sys_conf was 0x%08x,"
		    " clock was %s", U(RMT_RAM), U(RMT_WORDS), probe == 3U ? "ok" : "BAD",
		    U(blk.conf), blk.clk != 0U ? "on" : "off");
	if (probe == 3U) {
		mon = backup_mon_arm();
		if (argc > 2) {
			regs_row(sh, MIN((uint32_t)strtoul(argv[2], NULL, 0), RMT_WORDS));
		} else {
			for (size_t i = 0; i < ARRAY_SIZE(sweep); i++) {
				regs_row(sh, sweep[i]);
			}
		}
		backup_mon_print(sh, mon);
		REG_WRITE(SENSITIVE_BACKUP_BUS_PMS_MONITOR_1_REG, mon);
	}
	rmt_close(&blk);
	esp_sdr_bkdma_release();
	return 0;
}

/* ---- map mode ---- */

struct map_try {
	uint32_t m[4];
	uint16_t size;
	uint16_t apb_words;
	bool to_mem;
};

/* SIZE both as the number of set bits and as the span; one row with the APB start moved. */
static const struct map_try map_tries[] = {
	{{0x00000005U, 0U, 0U, 0U}, 2, 0, false},
	{{0x00000005U, 0U, 0U, 0U}, 3, 0, false},
	{{0x00000005U, 0U, 0U, 0U}, 1, 0, false},
	{{0x80000000U, 0U, 0U, 1U}, 2, 0, false},
	{{0xffffffffU, 0U, 0U, 0U}, 16, 0, false},
	{{0xffffffffU, 0U, 0U, 0U}, 32, 0, false},
	{{0U, 0U, 0U, 0U}, 2, 0, false},
	{{0x00000005U, 0U, 0U, 0U}, 2, 16, false},
	{{0x00000005U, 0U, 0U, 0U}, 2, 0, true},
	{{0x0000000aU, 0U, 0U, 0U}, 2, 0, true},
};

#define MAP_SRAM 40U
#define MAP_SPAN 160U

static void map_row(const struct shell *sh, const struct map_try *t)
{
	struct esp_sdr_bkdma_run r;
	uint32_t start, total, shown = 0U, hits = 0U;
	int ret;

	/* A stuck engine from the row before. */
	esp_sdr_bkdma_reset();
	for (uint32_t k = 0; k < MAP_SRAM; k++) {
		pat[k] = 0xb0000000U | k;
		back[k] = 0U;
	}
	rmt_fill(MAP_SPAN, 0U);
	if (t->to_mem) {
		for (uint32_t i = 0; i < MAP_SPAN; i++) {
			rmt_ram()[i] = 0xc0000000U | i;
		}
	}
	ret = esp_sdr_bkdma_prepare(&r, RMT_RAM + 4U * t->apb_words, t->to_mem ? back : pat, t->size,
				    ESP_SDR_BKDMA_MAP | BANK_OK | (t->to_mem ? ESP_SDR_BKDMA_TO_MEM : 0U),
				    t->m);
	if (ret == 0) {
		ret = bk_pass(&r, true, &start, &total);
	}
	shell_print(sh, "dma map %08x %08x %08x %08x size %u apb+%u words %s: ret %d, %u cyc, raw 0x%x,"
		    " CONFIG 0x%08x", U(t->m[0]), U(t->m[1]), U(t->m[2]), U(t->m[3]), U(t->size),
		    U(t->apb_words), t->to_mem ? "to sram" : "to regs", ret, U(total),
		    U(REG_READ(PERI_BACKUP_INT_RAW_REG)), U(REG_READ(PERI_BACKUP_CONFIG_REG)));
	if (!t->to_mem) {
		/* A register word holds the index of the SRAM word it got. */
		for (uint32_t i = 0; i < MAP_SPAN; i++) {
			uint32_t v = rmt_ram()[i];

			if (v != 0U) {
				hits++;
				if (shown++ < 12U) {
					shell_print(sh, "    reg[apb+%u] = 0x%08x (sram word %u)", U(i), U(v),
						    U(v & 0xffffU));
				}
			}
		}
	} else {
		for (uint32_t k = 0; k < MAP_SRAM; k++) {
			if (back[k] != 0U) {
				hits++;
				if (shown++ < 12U) {
					shell_print(sh, "    sram[%u] = 0x%08x (reg word apb+%u)", U(k),
						    U(back[k]), U(back[k] & 0xffffU));
				}
			}
		}
	}
	shell_print(sh, "    %u words changed", U(hits));
}

static int bench_map(const struct shell *sh)
{
	struct rmt_blk blk;
	uint32_t mon;
	int ret = esp_sdr_bkdma_claim();

	if (ret != 0) {
		shell_error(sh, "dma map: backup dma busy (%d)", ret);
		return ret;
	}
	rmt_open(&blk);
	mon = backup_mon_arm();
	for (size_t i = 0; i < ARRAY_SIZE(map_tries); i++) {
		map_row(sh, &map_tries[i]);
	}
	backup_mon_print(sh, mon);
	REG_WRITE(SENSITIVE_BACKUP_BUS_PMS_MONITOR_1_REG, mon);
	rmt_close(&blk);
	esp_sdr_bkdma_release();
	return 0;
}

/* ---- the backup DMA starts a GDMA chain ---- */

#define KICK_BYTES ESP_SDR_DMA_PIECE_MAX

static struct esp_sdr_dma_desc kick_tx[1], kick_rx[DIV_ROUND_UP(KICK_BYTES, ESP_SDR_DMA_PIECE_MAX)];
static struct esp_sdr_dma_kick kk;

enum { K_CPU_START, K_CPU_STORES, K_TWO_IN_OUT, K_TWO_OUT_IN, K_MAP, K_N };

static const char *const kick_name[K_N] = {
	"cpu esp_sdr_dma_start", "cpu two stores", "backup, two runs in then out",
	"backup, two runs out then in", "backup, one map run (in, out)",
};

/* Best-of cycle counts of one way of starting the chain. */
struct kick_res {
	uint32_t call, bk_done, eof;
	int bad;
};

static void kick_stamp(struct kick_res *r, uint32_t call, uint32_t bk, uint32_t eof)
{
	r->call = MIN(r->call, call);
	r->bk_done = MIN(r->bk_done, bk);
	r->eof = MIN(r->eof, eof);
}

/* Poll the GDMA's success EOF, cycles since t0. */
static uint32_t poll_eof(struct esp_sdr_dma *h, uint32_t t0)
{
	uint32_t t;

	do {
		t = esp_sdr_ccount() - t0;
	} while (!esp_sdr_dma_done(h) && t < k_us_to_cyc_ceil32(WAIT_US));
	return t;
}

static void kick_one(struct esp_sdr_dma *h, struct esp_sdr_dma_job *job, int mode,
		     struct kick_res *res)
{
	struct esp_sdr_bkdma_run run[2] = {0};
	const uint32_t lim = k_us_to_cyc_ceil32(BK_TIMEOUT_US);
	uint32_t t0, t1, tb, te, call;
	int ret = 0;

	memset(buf_dst, 0, KICK_BYTES);
	if (mode == K_CPU_START) {
		unsigned int key = arch_irq_lock();

		/* Drain the CPU's own peripheral stores first, so they are not timed. */
		barrier_dmem_fence_full();
		t0 = esp_sdr_ccount();
		ret = esp_sdr_dma_start(h, job);
		call = esp_sdr_ccount() - t0;
		te = poll_eof(h, t0);
		arch_irq_unlock(key);
		kick_stamp(res, call, 0U, te);
		goto out;
	}
	ret = esp_sdr_dma_arm(h, job, &kk);
	if (ret != 0) {
		res->bad = ret;
		return;
	}
	if (mode == K_CPU_STORES) {
		unsigned int key = arch_irq_lock();

		/* Drain the CPU's own peripheral stores first, so they are not timed. */
		barrier_dmem_fence_full();
		t0 = esp_sdr_ccount();
		esp_sdr_dma_kick_cpu(&kk);
		call = esp_sdr_ccount() - t0;
		te = poll_eof(h, t0);
		arch_irq_unlock(key);
		kick_stamp(res, call, 0U, te);
		goto out;
	}
	if (mode == K_MAP) {
		const uint32_t d = (kk.reg[1] - kk.reg[0]) / 4U;
		uint32_t map[4] = {0};

		if (kk.reg[1] <= kk.reg[0] || d >= 128U) {
			res->bad = -ERANGE;
			return;
		}
		map[0] = 1U;
		map[d / 32U] |= BIT(d % 32U);
		ret = esp_sdr_bkdma_prepare(&run[0], kk.reg[0], kk.val, 2U, ESP_SDR_BKDMA_MAP, map);
	} else {
		const int a = mode == K_TWO_IN_OUT ? 0 : 1;

		ret = esp_sdr_bkdma_prepare(&run[0], kk.reg[a], &kk.val[a], 1U, 0U, NULL);
		if (ret == 0) {
			ret = esp_sdr_bkdma_prepare(&run[1], kk.reg[1 - a], &kk.val[1 - a], 1U, 0U,
						    NULL);
		}
	}
	if (ret != 0) {
		res->bad = ret;
		return;
	}
	{
		unsigned int key = arch_irq_lock();

		/* Drain the CPU's own peripheral stores first, so they are not timed. */
		barrier_dmem_fence_full();
		t0 = esp_sdr_ccount();
		esp_sdr_bkdma_start(&run[0]);
		call = esp_sdr_ccount() - t0;
		tb = poll_done(t0);
		if (mode != K_MAP) {
			ret = esp_sdr_bkdma_wait(lim);
			t1 = esp_sdr_ccount();
			esp_sdr_bkdma_start(&run[1]);
			call += esp_sdr_ccount() - t1;
			tb = poll_done(t0);
		}
		te = poll_eof(h, t0);
		arch_irq_unlock(key);
	}
	ret = esp_sdr_bkdma_wait(lim) != 0 ? -EIO : ret;
	kick_stamp(res, call, tb, te);
out:
	if (ret == 0) {
		ret = esp_sdr_dma_wait(h, WAIT_US);
	}
	if (ret == 0 && memcmp(buf_dst, buf_src, KICK_BYTES) != 0) {
		ret = -EBADMSG;
	}
	if (ret != 0) {
		res->bad = ret;
		(void)esp_sdr_dma_wait(h, 1U);
	}
}

static int bench_kick(const struct shell *sh)
{
	struct esp_sdr_dma_job job = {.tx = kick_tx, .tx_max = ARRAY_SIZE(kick_tx), .rx = kick_rx,
				      .rx_max = ARRAY_SIZE(kick_rx)};
	struct esp_sdr_dma h;
	uint32_t x = 7U;
	int ret = gdma_open(sh, &h);

	if (ret != 0) {
		return ret;
	}
	ret = esp_sdr_bkdma_claim();
	if (ret != 0) {
		shell_error(sh, "dma kick: backup dma busy (%d)", ret);
		esp_sdr_dma_release(&h);
		return ret;
	}
	for (uint32_t k = 0; k < KICK_BYTES / 4U; k++) {
		x = x * 1664525U + 1013904223U;
		buf_src[k] = x;
	}
	ret = esp_sdr_dma_copy_build(&job, buf_dst, buf_src, KICK_BYTES, KICK_BYTES, BANK_OK);
	if (ret != 0) {
		shell_error(sh, "dma kick: build %d", ret);
		goto out;
	}
	shell_print(sh, "dma kick: %u B in %u + %u descriptors, IN_LINK 0x%08x OUT_LINK 0x%08x",
		    U(KICK_BYTES), U(job.ntx), U(job.nrx),
		    U((uintptr_t)&GDMA_LL_GET_HW(0)->channel[h.ch].in.link),
		    U((uintptr_t)&GDMA_LL_GET_HW(0)->channel[h.ch].out.link));
	for (int mode = 0; mode < K_N; mode++) {
		struct kick_res res = {.call = UINT32_MAX, .bk_done = UINT32_MAX, .eof = UINT32_MAX};

		for (int i = 0; i < 4 && res.bad == 0; i++) {
			kick_one(&h, &job, mode, &res);
		}
		if (res.bad != 0) {
			shell_print(sh, "dma kick %s: FAILED %d (errno: %d timeout, %d bad data), gdma:",
				    kick_name[mode], res.bad, ETIMEDOUT, EBADMSG);
			shell_print(sh, "  %s", esp_sdr_dma_debug(&h, dma_dbg, sizeof(dma_dbg)));
			continue;
		}
		shell_print(sh, "dma kick %s: cpu in start call(s) %u cyc, START to backup DONE %u cyc,"
			    " START to gdma EOF %u cyc (%u us), data arrived",
			    kick_name[mode], U(res.call), mode >= K_TWO_IN_OUT ? U(res.bk_done) : 0U,
			    U(res.eof), U(res.eof / cyc_us()));
	}
out:
	esp_sdr_bkdma_release();
	esp_sdr_dma_release(&h);
	return ret;
}

int dmabench_run(const struct shell *sh, size_t argc, char **argv)
{
	const char *what = argc > 1 ? argv[1] : "";

	if (strcmp(what, "sram") == 0) {
		return bench_sram(sh);
	} else if (strcmp(what, "reg") == 0) {
		return bench_reg(sh, argc > 2 && strcmp(argv[2], "mac") == 0);
	} else if (strcmp(what, "regs") == 0) {
		return bench_regs(sh, argc, argv);
	} else if (strcmp(what, "map") == 0) {
		return bench_map(sh);
	} else if (strcmp(what, "kick") == 0) {
		return bench_kick(sh);
	}
	shell_error(sh, "dma: sram | reg [mac] | regs [n|mac] | map | kick");
	return -EINVAL;
}
