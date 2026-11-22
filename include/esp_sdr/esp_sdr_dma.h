/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Memory-to-memory GDMA for I/Q: a gather of scattered pieces into one contiguous destination,
 * or a chopped copy, as descriptor lists the engine walks while the CPU does other work. Only
 * the ESP32-S3 backend (hal/gdma_ll.h) exists; the AHB (C5, C6) and AXI (S31, P4) GDMAs differ
 * in the descriptor layout and the LL calls only, which the .c file hides. See
 * doc/dev/dma-chains.md.
 *
 * Channels: the library owns the controller (CONFIG_DMA_ESP32, Zephyr's driver, is excluded)
 * and claims one pair of its five, from the top down, so a user of channel 0 is not hit first.
 *
 * Banks: the dump engine takes a bank from the CPUs and the GDMA with them. A source or a
 * destination inside a bank the engine may own is refused unless the caller asserts, with
 * ESP_SDR_DMA_CPU_OWNS_BANKS, that nothing is running that gives a bank away. Descriptors are
 * refused there always: the DRAM image ends below the banks (esp_sdr_bank_*.ld asserts it).
 *
 * A chain into a dump bank must be waited for (or its channel released, which resets it) before
 * anything gives that bank to the engine; start refuses a second chain while one runs.
 *
 * Registers: the GDMA cannot write one (its address check refuses, IN_DSCR_ERR). The other DMA of
 * the chip, the peripheral backup DMA, can: esp_sdr_bkdma_* below prepares a list of register
 * writes (or reads) once and fires it with one store. See doc/dev/dma-chains.md.
 */

#ifndef ESP_SDR_ESP_SDR_DMA_H_
#define ESP_SDR_ESP_SDR_DMA_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

/* The caller asserts the CPUs own the dump banks (no capture, ring or playback runs). */
#define ESP_SDR_DMA_CPU_OWNS_BANKS BIT(0)

/* Longest piece one descriptor moves (a word multiple below the field's 4095). */
#define ESP_SDR_DMA_PIECE_MAX 4092U

/* One descriptor: control word, buffer, next. The S3's (and the AHB GDMAs') layout, 12 bytes. */
#define ESP_SDR_DMA_DESC_WORDS 3U
struct esp_sdr_dma_desc {
	uint32_t w[ESP_SDR_DMA_DESC_WORDS];
} __aligned(4);

/* Repoints a built source descriptor at another piece of the same length. */
static inline void esp_sdr_dma_desc_set_buf(struct esp_sdr_dma_desc *d, const void *buf)
{
	d->w[1] = (uint32_t)(uintptr_t)buf;
}

struct esp_sdr_dma_seg {
	const void *src;
	uint32_t len;
};

/* The two lists of one transfer. The caller sets tx, tx_max, rx and rx_max; a build sets the rest. */
struct esp_sdr_dma_job {
	struct esp_sdr_dma_desc *tx; /* reads the pieces */
	struct esp_sdr_dma_desc *rx; /* writes the destination */
	uint32_t tx_max, rx_max;
	uint32_t ntx, nrx;
	uint32_t bytes;
};

struct esp_sdr_dma {
	uint8_t ch;
	/* A chain started and not yet waited for: start refuses, release resets the channel. */
	bool running;
	/* Channel state when the last wait failed (esp_sdr_dma_debug()). */
	uint32_t dbg[12];
	const struct esp_sdr_dma_job *job;
};

/* Descriptors needed: one per piece for tx, one per ESP_SDR_DMA_PIECE_MAX of the total for rx. */
static inline uint32_t esp_sdr_dma_rx_count(uint32_t bytes)
{
	return DIV_ROUND_UP(bytes, ESP_SDR_DMA_PIECE_MAX);
}

/* Claim a channel pair. 0, or -EBUSY when all are claimed. */
int esp_sdr_dma_claim(struct esp_sdr_dma *h);
void esp_sdr_dma_release(struct esp_sdr_dma *h);

/* INCR bursts of 16 bytes on both channels: pieces, source and destination multiples of 16. */
void esp_sdr_dma_burst(struct esp_sdr_dma *h, bool on);

/*
 * Lists for a gather of n pieces into dst (word aligned, the total a word multiple). Pieces
 * of up to ESP_SDR_DMA_PIECE_MAX; a longer one is -EINVAL. -ENOSPC: lists too short;
 * -EACCES: a bank the caller did not release.
 */
int esp_sdr_dma_gather_build(struct esp_sdr_dma_job *job, void *dst,
			     const struct esp_sdr_dma_seg *seg, uint32_t n, uint32_t flags);

/* The same for one source of len bytes cut into pieces of at most piece bytes. */
int esp_sdr_dma_copy_build(struct esp_sdr_dma_job *job, void *dst, const void *src,
			   uint32_t len, uint32_t piece, uint32_t flags);

/* Reset the channel pair, rearm the destination list and start. 0 or -EINVAL. */
int esp_sdr_dma_start(struct esp_sdr_dma *h, struct esp_sdr_dma_job *job);

/* The two stores that start a chain (IN_LINK then OUT_LINK, address and START in one word). */
struct esp_sdr_dma_kick {
	uint32_t reg[2];
	uint32_t val[2];
};

/*
 * Everything start does but the two starts, which are left to the caller: the backup DMA can
 * store them (esp_sdr_bkdma_prepare()), reg and val say where. The chain counts as running.
 * kick may be NULL (then the caller starts it with esp_sdr_dma_kick_cpu()).
 */
int esp_sdr_dma_arm(struct esp_sdr_dma *h, struct esp_sdr_dma_job *job,
		    struct esp_sdr_dma_kick *kick);
void esp_sdr_dma_kick_cpu(const struct esp_sdr_dma_kick *kick);

/* Whether the last piece has landed (the destination's success EOF). */
bool esp_sdr_dma_done(struct esp_sdr_dma *h);

/* Poll for the end: 0, -ETIMEDOUT (channel reset), or -EIO on a descriptor error (reset). */
int esp_sdr_dma_wait(struct esp_sdr_dma *h, uint32_t timeout_us);

/* The state captured at the last failed wait, as text (the buffer is returned). */
const char *esp_sdr_dma_debug(const struct esp_sdr_dma *h, char *buf, size_t len);

/* Whether [p, p + len) touches a dump bank the engine may own (the guard's test). */
bool esp_sdr_dma_in_bank(const void *p, size_t len);

/*
 * Peripheral backup DMA ("APB backup DMA", hal peri_backup_reg.h): copies a list of words between
 * SRAM and registers. One user at a time, the library's: the ROM entry ets_backup_dma_copy() and
 * its lock hook are not used (see esp_sdr_dma.c).
 *
 * A run is the CONFIG word and the addresses, prepared once; start is then one register store
 * after the first one. Linear: words consecutive registers from apb. Map: the four MAP words
 * pick which of the 128 words from apb are transferred (the memory side packed); the meaning is
 * measured by "probe dma map", not documented by Espressif.
 */
#define ESP_SDR_BKDMA_TO_MEM BIT(1) /* registers to SRAM, not SRAM to registers */
#define ESP_SDR_BKDMA_MAP    BIT(2) /* map mode */

/* Longest run: the SIZE field is 10 bits. */
#define ESP_SDR_BKDMA_WORDS_MAX 1023U

struct esp_sdr_bkdma_run {
	uint32_t cfg; /* CONFIG with ENA, without START */
	uint32_t apb, mem;
	uint32_t map[4];
	bool map_mode;
};

/* 0, or -EBUSY when someone holds it. Clock on, reset released. */
int esp_sdr_bkdma_claim(void);
/* Disables the engine and puts the clock and reset back as found. */
void esp_sdr_bkdma_release(void);
/* Pulse the engine's reset (a wait that fails does it): a stuck run ends, CONFIG is the default. */
void esp_sdr_bkdma_reset(void);

/*
 * apb: first register (word aligned), mem: word aligned SRAM, words 1..ESP_SDR_BKDMA_WORDS_MAX,
 * map: the 4 words of a map run (NULL otherwise). flags: ESP_SDR_BKDMA_*, and
 * ESP_SDR_DMA_CPU_OWNS_BANKS for a mem in a dump bank. -EINVAL, -EACCES (bank).
 */
int esp_sdr_bkdma_prepare(struct esp_sdr_bkdma_run *r, uint32_t apb, void *mem, uint32_t words,
			  uint32_t flags, const uint32_t *map);

/* Store the run: the address, map and CONFIG stores only where they differ from the last run. */
void esp_sdr_bkdma_start(const struct esp_sdr_bkdma_run *r);

/* Forget what the registers hold, so the next start stores everything again. */
void esp_sdr_bkdma_forget(void);

/* Whether DONE or ERR is up. */
bool esp_sdr_bkdma_done(void);

/* Poll for the end and clear it: 0, -EIO (ERR or FLOW_ERR), -ETIMEDOUT after timeout_cyc cycles. */
int esp_sdr_bkdma_wait(uint32_t timeout_cyc);

#endif
