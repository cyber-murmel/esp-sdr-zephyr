/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Memory-to-memory GDMA, the ESP32-S3 backend (esp_sdr_dma.h). The TX channel reads the source
 * list, the RX channel of the same pair writes the destination list; nothing is interrupt driven,
 * the caller polls the RX success EOF. The controller is shared only with ourselves: Zephyr's
 * driver (CONFIG_DMA_ESP32) is excluded in Kconfig, so a software claim mask is enough.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/barrier.h>

#include <esp_private/periph_ctrl.h>
#include <hal/dma_types.h>
#include <hal/gdma_ll.h>
#include <soc/peri_backup_reg.h>
#include <soc/system_reg.h>

#include <esp_sdr/esp_sdr_dma.h>

/* Trigger id of the memory side (any free one, as Zephyr's driver takes the first). */
#define M2M_ID 0

#define RX_ERRORS (GDMA_LL_EVENT_RX_DESC_ERROR | GDMA_LL_EVENT_RX_ERR_EOF | GDMA_LL_EVENT_RX_DESC_EMPTY)
#define TX_ERRORS GDMA_LL_EVENT_TX_DESC_ERROR

/* The dump banks are 64 KiB each (esp_sdr_bank_s3.ld). */
#define BANK_SIZE 0x10000U

BUILD_ASSERT(sizeof(struct esp_sdr_dma_desc) == sizeof(dma_descriptor_t), "descriptor layout");
BUILD_ASSERT(ESP_SDR_DMA_PIECE_MAX <= DMA_DESCRIPTOR_BUFFER_MAX_SIZE_4B_ALIGNED, "piece size");

extern uint32_t __esp_sdr_bank_start[];
#if defined(CONFIG_ESP_SDR_BANK1)
extern uint32_t __esp_sdr_bank1_start[];
#endif
#if defined(CONFIG_ESP_SDR_RING)
extern uint32_t __esp_sdr_bank0_start[];
#endif

static atomic_t claimed;

static gdma_dev_t *dev(void)
{
	return GDMA_LL_GET_HW(0);
}

bool esp_sdr_dma_in_bank(const void *p, size_t len)
{
	const uintptr_t lo = (uintptr_t)p, hi = lo + MAX(len, 1U);
	const uintptr_t banks[] = {
		(uintptr_t)__esp_sdr_bank_start,
#if defined(CONFIG_ESP_SDR_BANK1)
		(uintptr_t)__esp_sdr_bank1_start,
#endif
#if defined(CONFIG_ESP_SDR_RING)
		(uintptr_t)__esp_sdr_bank0_start,
#endif
	};

	for (size_t i = 0; i < ARRAY_SIZE(banks); i++) {
		if (lo < banks[i] + BANK_SIZE && hi > banks[i]) {
			return true;
		}
	}
	return false;
}

int esp_sdr_dma_claim(struct esp_sdr_dma *h)
{
	for (int ch = GDMA_LL_PAIRS_PER_INST - 1; ch >= 0; ch--) {
		if (atomic_test_and_set_bit(&claimed, ch)) {
			continue;
		}
		/*
		 * The controller's clock is gated and its reset held after boot (Zephyr's driver, off
		 * here, runs enable and reset in its init); a reset also drops the other channels'
		 * state, so only when it was not running.
		 */
		const bool was_on = SYSTEM.perip_clk_en1.dma_clk_en && !SYSTEM.perip_rst_en1.dma_rst;

		PERIPH_RCC_ATOMIC() {
			gdma_ll_enable_bus_clock(0, true);
			if (!was_on) {
				gdma_ll_reset_register(0);
			}
		}
		gdma_ll_force_enable_reg_clock(dev(), true);
		h->ch = (uint8_t)ch;
		h->running = false;
		gdma_ll_tx_enable_owner_check(dev(), ch, false);
		gdma_ll_rx_enable_owner_check(dev(), ch, false);
		gdma_ll_tx_enable_auto_write_back(dev(), ch, false);
		gdma_ll_tx_set_eof_mode(dev(), ch, 1);
		esp_sdr_dma_burst(h, false);
		return 0;
	}
	return -EBUSY;
}

void esp_sdr_dma_release(struct esp_sdr_dma *h)
{
	/* A chain still running would write on into a bank the radio hands to the dump engine. */
	gdma_ll_tx_stop(dev(), h->ch);
	gdma_ll_rx_stop(dev(), h->ch);
	gdma_ll_tx_reset_channel(dev(), h->ch);
	gdma_ll_rx_reset_channel(dev(), h->ch);
	h->running = false;
	gdma_ll_tx_disconnect_all(dev(), h->ch);
	gdma_ll_rx_disconnect_all(dev(), h->ch);
	atomic_clear_bit(&claimed, h->ch);
}

void esp_sdr_dma_burst(struct esp_sdr_dma *h, bool on)
{
	gdma_ll_tx_enable_data_burst(dev(), h->ch, on);
	gdma_ll_rx_enable_data_burst(dev(), h->ch, on);
	gdma_ll_tx_enable_descriptor_burst(dev(), h->ch, on);
	gdma_ll_rx_enable_descriptor_burst(dev(), h->ch, on);
	gdma_ll_tx_set_burst_size(dev(), h->ch, 16);
	gdma_ll_rx_set_burst_size(dev(), h->ch, 16);
}

static void desc_fill(struct esp_sdr_dma_desc *d, const void *buf, uint32_t size, uint32_t length,
		      struct esp_sdr_dma_desc *next, bool eof)
{
	dma_descriptor_t *dd = (dma_descriptor_t *)d;

	dd->dw0.size = size;
	dd->dw0.length = length;
	dd->dw0.err_eof = 0;
	dd->dw0.suc_eof = eof;
	dd->dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
	dd->buffer = (void *)buf;
	dd->next = (dma_descriptor_t *)next;
}

static int guard(const struct esp_sdr_dma_job *j, const void *dst, uint32_t flags)
{
	if (esp_sdr_dma_in_bank(j->tx, j->tx_max * sizeof(*j->tx)) ||
	    esp_sdr_dma_in_bank(j->rx, j->rx_max * sizeof(*j->rx))) {
		return -EACCES;
	}
	if ((flags & ESP_SDR_DMA_CPU_OWNS_BANKS) == 0U &&
	    esp_sdr_dma_in_bank(dst, j->bytes)) {
		return -EACCES;
	}
	return 0;
}

/* The destination list over bytes at dst, 4092 a descriptor; the last takes the rest. */
static int rx_build(struct esp_sdr_dma_job *j, void *dst)
{
	uint32_t left = j->bytes, n = esp_sdr_dma_rx_count(left);
	uint8_t *p = dst;

	if (n > j->rx_max) {
		return -ENOSPC;
	}
	for (uint32_t i = 0; i < n; i++) {
		uint32_t c = MIN(left, ESP_SDR_DMA_PIECE_MAX);

		desc_fill(&j->rx[i], p, c, 0, i + 1U < n ? &j->rx[i + 1U] : NULL, false);
		p += c;
		left -= c;
	}
	j->nrx = n;
	return 0;
}

int esp_sdr_dma_gather_build(struct esp_sdr_dma_job *j, void *dst,
			     const struct esp_sdr_dma_seg *seg, uint32_t n, uint32_t flags)
{
	uint32_t total = 0;
	int ret;

	if (n == 0U || n > j->tx_max) {
		return -ENOSPC;
	}
	for (uint32_t i = 0; i < n; i++) {
		if (seg[i].len == 0U || seg[i].len > ESP_SDR_DMA_PIECE_MAX) {
			return -EINVAL;
		}
		total += seg[i].len;
	}
	if (((uintptr_t)dst & 3U) != 0U || (total & 3U) != 0U) {
		return -EINVAL;
	}
	j->bytes = total;
	ret = guard(j, dst, flags);
	for (uint32_t i = 0; ret == 0 && i < n; i++) {
		if ((flags & ESP_SDR_DMA_CPU_OWNS_BANKS) == 0U &&
		    esp_sdr_dma_in_bank(seg[i].src, seg[i].len)) {
			ret = -EACCES;
		}
	}
	if (ret != 0) {
		return ret;
	}
	for (uint32_t i = 0; i < n; i++) {
		desc_fill(&j->tx[i], seg[i].src, seg[i].len, seg[i].len,
			  i + 1U < n ? &j->tx[i + 1U] : NULL, i + 1U == n);
	}
	j->ntx = n;
	return rx_build(j, dst);
}

int esp_sdr_dma_copy_build(struct esp_sdr_dma_job *j, void *dst, const void *src, uint32_t len,
			   uint32_t piece, uint32_t flags)
{
	uint32_t n, left = len;
	const uint8_t *p = src;
	int ret;

	if (len == 0U || piece == 0U || piece > ESP_SDR_DMA_PIECE_MAX) {
		return -EINVAL;
	}
	n = DIV_ROUND_UP(len, piece);
	if (n > j->tx_max) {
		return -ENOSPC;
	}
	if (((uintptr_t)dst & 3U) != 0U || (len & 3U) != 0U) {
		return -EINVAL;
	}
	j->bytes = len;
	ret = guard(j, dst, flags);
	if (ret == 0 && (flags & ESP_SDR_DMA_CPU_OWNS_BANKS) == 0U && esp_sdr_dma_in_bank(src, len)) {
		ret = -EACCES;
	}
	if (ret != 0) {
		return ret;
	}
	for (uint32_t i = 0; i < n; i++) {
		uint32_t c = MIN(left, piece);

		desc_fill(&j->tx[i], p, c, c, i + 1U < n ? &j->tx[i + 1U] : NULL, i + 1U == n);
		p += c;
		left -= c;
	}
	j->ntx = n;
	return rx_build(j, dst);
}

int esp_sdr_dma_arm(struct esp_sdr_dma *h, struct esp_sdr_dma_job *j, struct esp_sdr_dma_kick *k)
{
	const uint32_t ch = h->ch;

	if (j->ntx == 0U || j->nrx == 0U) {
		return -EINVAL;
	}
	if (h->running) {
		return -EBUSY;
	}
	/* The RX descriptors hold the previous run's lengths and owner. */
	for (uint32_t i = 0; i < j->nrx; i++) {
		dma_descriptor_t *dd = (dma_descriptor_t *)&j->rx[i];

		dd->dw0.length = 0;
		dd->dw0.suc_eof = 0;
		dd->dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
	}
	gdma_ll_rx_reset_channel(dev(), ch);
	gdma_ll_tx_reset_channel(dev(), ch);
	gdma_ll_rx_connect_to_mem(dev(), ch, M2M_ID);
	gdma_ll_tx_connect_to_mem(dev(), ch, M2M_ID);
	gdma_ll_rx_clear_interrupt_status(dev(), ch, GDMA_LL_RX_EVENT_MASK);
	gdma_ll_tx_clear_interrupt_status(dev(), ch, GDMA_LL_TX_EVENT_MASK);
	gdma_ll_rx_set_desc_addr(dev(), ch, (uint32_t)(uintptr_t)j->rx);
	gdma_ll_tx_set_desc_addr(dev(), ch, (uint32_t)(uintptr_t)j->tx);
	barrier_dmem_fence_full();
	h->job = j;
	h->running = true;
	if (k != NULL) {
		/* The link words as the starts write them: the list's low 20 bits and START. */
		const typeof(dev()->channel[ch].in.link) in = {
			.addr = (uint32_t)(uintptr_t)j->rx, .start = 1};
		const typeof(dev()->channel[ch].out.link) out = {
			.addr = (uint32_t)(uintptr_t)j->tx, .start = 1};

		k->reg[0] = (uint32_t)(uintptr_t)&dev()->channel[ch].in.link;
		k->reg[1] = (uint32_t)(uintptr_t)&dev()->channel[ch].out.link;
		k->val[0] = in.val;
		k->val[1] = out.val;
	}
	return 0;
}

void esp_sdr_dma_kick_cpu(const struct esp_sdr_dma_kick *k)
{
	*(volatile uint32_t *)k->reg[0] = k->val[0];
	*(volatile uint32_t *)k->reg[1] = k->val[1];
}

int esp_sdr_dma_start(struct esp_sdr_dma *h, struct esp_sdr_dma_job *j)
{
	int ret = esp_sdr_dma_arm(h, j, NULL);

	if (ret == 0) {
		gdma_ll_rx_start(dev(), h->ch);
		gdma_ll_tx_start(dev(), h->ch);
	}
	return ret;
}

bool esp_sdr_dma_done(struct esp_sdr_dma *h)
{
	return !h->running ||
	       (gdma_ll_rx_get_interrupt_status(dev(), h->ch, true) & GDMA_LL_EVENT_RX_SUC_EOF) != 0U;
}

/* Registers and first descriptors, before a reset wipes them. */
static void snapshot(struct esp_sdr_dma *h)
{
	const uint32_t ch = h->ch;
	const uint32_t *rx = (const uint32_t *)h->job->rx, *tx = (const uint32_t *)h->job->tx;

	h->dbg[0] = gdma_ll_rx_get_interrupt_status(dev(), ch, true);
	h->dbg[1] = gdma_ll_tx_get_interrupt_status(dev(), ch, true);
	h->dbg[2] = gdma_ll_rx_is_desc_fsm_idle(dev(), ch) | (gdma_ll_tx_is_desc_fsm_idle(dev(), ch) << 1) |
		    (SYSTEM.perip_clk_en1.dma_clk_en << 2) | (SYSTEM.perip_rst_en1.dma_rst << 3);
	h->dbg[3] = gdma_ll_rx_get_prefetched_desc_addr(dev(), ch);
	h->dbg[4] = gdma_ll_tx_get_prefetched_desc_addr(dev(), ch);
	h->dbg[5] = gdma_ll_rx_get_success_eof_desc_addr(dev(), ch);
	h->dbg[6] = gdma_ll_tx_get_eof_desc_addr(dev(), ch);
	h->dbg[7] = dev()->channel[ch].in.peri_sel.val | (dev()->channel[ch].out.peri_sel.val << 8) |
		    (dev()->channel[ch].in.conf0.mem_trans_en << 16);
	h->dbg[8] = rx[0];
	h->dbg[9] = tx[0];
	h->dbg[10] = (uint32_t)(uintptr_t)h->job->rx;
	h->dbg[11] = (uint32_t)(uintptr_t)h->job->tx;
}

const char *esp_sdr_dma_debug(const struct esp_sdr_dma *h, char *buf, size_t len)
{
	snprintf(buf, len,
		 "ch %u: raw rx 0x%x tx 0x%x, idle rx %u tx %u, clk %u rst %u, desc rx 0x%08x tx 0x%08x,"
		 " eof rx 0x%08x tx 0x%08x, sel in 0x%x out 0x%x m2m %u, w0 rx 0x%08x tx 0x%08x,"
		 " lists rx 0x%08x tx 0x%08x",
		 h->ch, h->dbg[0], h->dbg[1], h->dbg[2] & 1U, (h->dbg[2] >> 1) & 1U,
		 (h->dbg[2] >> 2) & 1U, (h->dbg[2] >> 3) & 1U, h->dbg[3], h->dbg[4], h->dbg[5],
		 h->dbg[6], h->dbg[7] & 0xffU, (h->dbg[7] >> 8) & 0xffU, h->dbg[7] >> 16, h->dbg[8],
		 h->dbg[9], h->dbg[10], h->dbg[11]);
	return buf;
}

int esp_sdr_dma_wait(struct esp_sdr_dma *h, uint32_t timeout_us)
{
	const uint32_t t0 = k_cycle_get_32(), lim = k_us_to_cyc_ceil32(timeout_us);

	for (;;) {
		uint32_t rx = gdma_ll_rx_get_interrupt_status(dev(), h->ch, true);

		if ((rx & GDMA_LL_EVENT_RX_SUC_EOF) != 0U) {
			/* The last words may still be on their way into memory: wait for the FSM. */
			while (!gdma_ll_rx_is_desc_fsm_idle(dev(), h->ch) &&
			       k_cycle_get_32() - t0 <= lim) {
			}
			h->running = false;
			return 0;
		}
		if ((rx & RX_ERRORS) != 0U ||
		    (gdma_ll_tx_get_interrupt_status(dev(), h->ch, true) & TX_ERRORS) != 0U) {
			snapshot(h);
			break;
		}
		if (k_cycle_get_32() - t0 > lim) {
			snapshot(h);
			gdma_ll_tx_reset_channel(dev(), h->ch);
			gdma_ll_rx_reset_channel(dev(), h->ch);
			h->running = false;
			return -ETIMEDOUT;
		}
	}
	gdma_ll_tx_reset_channel(dev(), h->ch);
	gdma_ll_rx_reset_channel(dev(), h->ch);
	h->running = false;
	return -EIO;
}

/*
 * Peripheral backup DMA. Base 0x6001A000: the hal's DR_REG_PERI_BACKUP_BASE was 0x6002A000 (wrong,
 * found from the ROM's ets_backup_dma_copy) until fixed in reg_base.h. Not used by anything else
 * in our build: the IDF's apb_backup_dma.c (the lock hook for the ROM entry) is not compiled, so
 * ets_apb_backup_init_lock_func() is never called; the only IDF user, the BT controller, is the
 * C3's. The ROM entry is not called here at all (it takes that hook's lock and rewrites the
 * registers), so the software claim below is the whole exclusion. Nothing starts it from
 * hardware: the S3 has no ETM and the GDMA cannot write START (esp_sdr_dma.h).
 */
#define BK_INT_ALL      (PERI_BACKUP_DONE_INT_CLR | PERI_BACKUP_ERR_INT_CLR)
#define BK_INT_RAW      (PERI_BACKUP_DONE_INT_RAW | PERI_BACKUP_ERR_INT_RAW)
/* CONFIG's reset value without ENA and size: timeout threshold 50, burst limit 8. */
#define BK_CFG_DEFAULT  ((50U << PERI_BACKUP_TOUT_THRES_S) | (8U << PERI_BACKUP_BURST_LIMIT_S))
#define BK_CLK          SYSTEM_PERI_BACKUP_CLK_EN
#define BK_RST          SYSTEM_PERI_BACKUP_RST

static atomic_t bk_claimed;
static uint32_t bk_saved_clk, bk_saved_rst;
/* What the registers hold after the last start; valid only with bk_known. */
static struct {
	uint32_t cfg, apb, mem, map[4];
	bool known;
} bk;

int esp_sdr_bkdma_claim(void)
{
	if (atomic_test_and_set_bit(&bk_claimed, 0)) {
		return -EBUSY;
	}
	PERIPH_RCC_ATOMIC() {
		bk_saved_clk = REG_READ(SYSTEM_PERIP_CLK_EN1_REG) & BK_CLK;
		bk_saved_rst = REG_READ(SYSTEM_PERIP_RST_EN1_REG) & BK_RST;
		REG_SET_BIT(SYSTEM_PERIP_CLK_EN1_REG, BK_CLK);
		REG_CLR_BIT(SYSTEM_PERIP_RST_EN1_REG, BK_RST);
	}
	REG_WRITE(PERI_BACKUP_INT_CLR_REG, BK_INT_ALL);
	REG_WRITE(PERI_BACKUP_CONFIG_REG, BK_CFG_DEFAULT);
	bk.known = false;
	return 0;
}

void esp_sdr_bkdma_reset(void)
{
	PERIPH_RCC_ATOMIC() {
		REG_SET_BIT(SYSTEM_PERIP_RST_EN1_REG, BK_RST);
		REG_CLR_BIT(SYSTEM_PERIP_RST_EN1_REG, BK_RST);
	}
	REG_WRITE(PERI_BACKUP_INT_CLR_REG, BK_INT_ALL);
	REG_WRITE(PERI_BACKUP_CONFIG_REG, BK_CFG_DEFAULT);
	bk.known = false;
}

void esp_sdr_bkdma_release(void)
{
	REG_WRITE(PERI_BACKUP_CONFIG_REG, BK_CFG_DEFAULT);
	REG_WRITE(PERI_BACKUP_INT_CLR_REG, BK_INT_ALL);
	PERIPH_RCC_ATOMIC() {
		REG_WRITE(SYSTEM_PERIP_CLK_EN1_REG,
			  (REG_READ(SYSTEM_PERIP_CLK_EN1_REG) & ~BK_CLK) | bk_saved_clk);
		REG_WRITE(SYSTEM_PERIP_RST_EN1_REG,
			  (REG_READ(SYSTEM_PERIP_RST_EN1_REG) & ~BK_RST) | bk_saved_rst);
	}
	atomic_clear_bit(&bk_claimed, 0);
}

int esp_sdr_bkdma_prepare(struct esp_sdr_bkdma_run *r, uint32_t apb, void *mem, uint32_t words,
			  uint32_t flags, const uint32_t *map)
{
	const bool to_mem = (flags & ESP_SDR_BKDMA_TO_MEM) != 0U;
	const bool map_mode = (flags & ESP_SDR_BKDMA_MAP) != 0U;

	if (words == 0U || words > ESP_SDR_BKDMA_WORDS_MAX || (apb & 3U) != 0U ||
	    ((uintptr_t)mem & 3U) != 0U || (map_mode && map == NULL)) {
		return -EINVAL;
	}
	if ((flags & ESP_SDR_DMA_CPU_OWNS_BANKS) == 0U && esp_sdr_dma_in_bank(mem, words * 4U)) {
		return -EACCES;
	}
	r->cfg = PERI_BACKUP_ENA | (to_mem ? PERI_BACKUP_TO_MEM : 0U) | BK_CFG_DEFAULT |
		 (words << PERI_BACKUP_SIZE_S) | (map_mode ? PERI_BACKUP_ADDR_MAP_MODE : 0U);
	r->apb = apb;
	r->mem = (uint32_t)(uintptr_t)mem;
	r->map_mode = map_mode;
	for (int i = 0; i < 4; i++) {
		r->map[i] = map_mode ? map[i] : 0U;
	}
	return 0;
}

void esp_sdr_bkdma_start(const struct esp_sdr_bkdma_run *r)
{
	if (!bk.known || bk.apb != r->apb) {
		REG_WRITE(PERI_BACKUP_APB_ADDR_REG, r->apb);
		bk.apb = r->apb;
	}
	if (!bk.known || bk.mem != r->mem) {
		REG_WRITE(PERI_BACKUP_MEM_ADDR_REG, r->mem);
		bk.mem = r->mem;
	}
	if (r->map_mode) {
		for (int i = 0; i < 4; i++) {
			if (!bk.known || bk.map[i] != r->map[i]) {
				REG_WRITE(PERI_BACKUP_REG_MAP0_REG + 4U * i, r->map[i]);
				bk.map[i] = r->map[i];
			}
		}
	}
	/* ENA (with the rest) before START unless CONFIG already holds this run. */
	if (!bk.known || bk.cfg != r->cfg) {
		REG_WRITE(PERI_BACKUP_CONFIG_REG, r->cfg);
		bk.cfg = r->cfg;
	}
	bk.known = true;
	barrier_dmem_fence_full();
	REG_WRITE(PERI_BACKUP_CONFIG_REG, r->cfg | PERI_BACKUP_START);
}

void esp_sdr_bkdma_forget(void)
{
	bk.known = false;
}

bool esp_sdr_bkdma_done(void)
{
	return (REG_READ(PERI_BACKUP_INT_RAW_REG) & BK_INT_RAW) != 0U;
}

int esp_sdr_bkdma_wait(uint32_t timeout_cyc)
{
	const uint32_t t0 = k_cycle_get_32();
	uint32_t raw;
	int ret = 0;

	while (((raw = REG_READ(PERI_BACKUP_INT_RAW_REG)) & BK_INT_RAW) == 0U) {
		if (k_cycle_get_32() - t0 > timeout_cyc) {
			ret = -ETIMEDOUT;
			break;
		}
	}
	if ((raw & PERI_BACKUP_ERR_INT_RAW) != 0U ||
	    (REG_READ(PERI_BACKUP_CONFIG_REG) & PERI_BACKUP_FLOW_ERR) != 0U) {
		ret = -EIO;
	}
	REG_WRITE(PERI_BACKUP_INT_CLR_REG, BK_INT_ALL);
	if (ret != 0) {
		esp_sdr_bkdma_reset();
	}
	return ret;
}
