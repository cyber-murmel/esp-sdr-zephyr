/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file
 * @brief ESP-SDR gapless receive on the ESP32-C5 through the modem diagnostic bus.
 *
 * The dump engine keeps running with its SRAM bank handed back to the CPU, and
 * the modem diagnostic bus carries the dump word (I and Q, 10 bits each). The
 * GPIO matrix loops 8 of its lines back into the PARLIO receiver, which
 * samples them at 40 MHz, every second sample of the 80 MS/s baseband, as 4
 * bits of I and 4 bits of Q per byte. A GDMA ring in SRAM bank 1 collects the
 * bytes without the CPU. The CPU only has to keep up with the ring:
 * 40 MB/s is 6 cycles per byte at 240 MHz.
 *
 * Method after the ESPsoup firmware (github.com/pit711/espsoup-firmware),
 * which follows ESP-SDR's ESP32-S31 streaming and credits Twotoz/C5VRX.
 *
 * Byte layout: Q[lsb+3:lsb] in bits 3:0 and I[lsb+3:lsb] in bits 7:4, signed
 * nibbles; the sample is the nibble times 2^lsb. Q and I are the library's
 * receive word fields (esp_sdr_rx_burst): Q in bits 9:0, I in bits 19:10; a
 * tone above the LO comes out as +f with I + jQ (measured against the 10-bit
 * capture). A window that is too low wraps: esp_sdr_diag_level() picks one that
 * does not.
 *
 * Sampling every second sample of the bus without a filter aliases the band
 * above 20 MHz offset: narrow the analog low-pass first.
 *
 * The 8 pads are driven as outputs that toggle at up to 40 MHz: nothing may
 * be connected to them.
 */

#ifndef ESP_SDR_ESP_SDR_DIAG_H_
#define ESP_SDR_ESP_SDR_DIAG_H_

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup esp_sdr_diag
 * @{
 */

/** Data lines (PARLIO RX width on the C5). */
#define ESP_SDR_DIAG_PINS 8U

/** Bytes per sink call: the unit of the ring. */
#define ESP_SDR_DIAG_BLOCK 8192U

/** Ring size in bytes: 7 blocks, 1.43 ms at 40 MB/s. */
#define ESP_SDR_DIAG_RING 57344U

/** cfg.lsb: pick the nibble window from the signal level at the start. */
#define ESP_SDR_DIAG_LSB_AUTO 0xffU

/** Highest nibble window start: I[9:6]. */
#define ESP_SDR_DIAG_LSB_MAX 6U

/**
 * Receives one block.
 *
 * @param user cfg.user.
 * @param blk ESP_SDR_DIAG_BLOCK bytes, valid only during the call.
 * @param n Block size in bytes.
 * @param index Byte index of the block since the start; a jump from the
 *              previous index plus size means the consumer fell behind and
 *              blocks were skipped.
 */
typedef void (*esp_sdr_diag_sink_t)(void *user, const uint8_t *blk, size_t n, uint64_t index);

/**
 * Receives decimated I/Q, called four times per ring block (every 51 us at 40 MB/s).
 *
 * @param user cfg.iq_user.
 * @param iq n interleaved I, Q pairs, valid only during the call, in raw 10-bit counts times 16
 *           (full scale +-8192), a boxcar of 16 and a third-order CIC of cfg.decim down from 40 MS/s.
 * @param n Complex samples in this call (0 to 17).
 * @param index Index of the first sample since the start; a jump means blocks were skipped.
 */
typedef void (*esp_sdr_diag_iq_sink_t)(void *user, const int32_t *iq, size_t n, uint64_t index);

/** Limits of cfg.decim. */
#define ESP_SDR_DIAG_DECIM_MIN 8U
#define ESP_SDR_DIAG_DECIM_MAX 255U

struct esp_sdr_diag_cfg {
	/** Eight GPIO numbers, or NULL for the defaults {0, 1, 3, 4, 5, 7, 10, 23}. */
	const int8_t *pins;
	/** Nibble window start 0 to ESP_SDR_DIAG_LSB_MAX, or ESP_SDR_DIAG_LSB_AUTO. */
	uint8_t lsb;
	/** PARLIO divider of 240 MHz (6 = 40 MS/s, 0 = 6), 2 to 255; also divides the sample rate. */
	uint8_t clk_div;
	/** Sample on the falling PARLIO clock edge instead of the rising one. */
	bool falling_edge;
	/** Run time in seconds, 0 to run until esp_sdr_diag_stop(). */
	uint32_t seconds;
	/** Called from the thread that called esp_sdr_diag_run(); may be NULL. */
	esp_sdr_diag_sink_t sink;
	void *user;
	/**
	 * Decimate in the library: a boxcar of 16 and a third-order CIC by this factor, ESP_SDR_DIAG_DECIM_MIN
	 * to MAX, or 0 for none. The output rate is byte_rate / 16 / decim (40 MS/s: 2.5 MS/s / decim).
	 */
	uint8_t decim;
	/** Called with the decimated samples; may be NULL. */
	esp_sdr_diag_iq_sink_t iq_sink;
	void *iq_user;
};

struct esp_sdr_diag_stats {
	/** Bytes the ring received. */
	uint64_t bytes;
	/** Bytes handed to the sink. */
	uint64_t consumed;
	/** Times the consumer fell more than a ring behind and skipped ahead. */
	uint32_t skips;
	/** Blocks skipped by those. */
	uint32_t skipped_blocks;
	/** The interrupt fell a whole ring behind the hardware (data lost, run aborted). */
	bool overrun;
	/** Ring byte count against the CPU cycle counter, in 0.01 ppm; 0 until a second has run. */
	int32_t ppm_x100;
	/** Nibble window used. */
	uint8_t lsb;
	/** CPU cycles per received byte spent in the consumer (decimation and sinks), in 0.01 cycles. */
	uint32_t sink_cycles_x100;
	/** Bytes per second the PARLIO clock delivers. */
	uint32_t byte_rate;
	/** Output sample rate with cfg.decim, in Hz. */
	uint32_t iq_rate;
	/** Debugging: ring interrupts, the raw DMA status, the first descriptors' first words. */
	uint32_t isrs, dma_raw, dw0[4];
};

/**
 * @brief Run the ring until the time is up, esp_sdr_diag_stop() or an overrun.
 *
 * Takes the radio, and SRAM bank 1, for the whole run. The receiver keeps its
 * frequency, gain and filter. Needs the bank usage of the capture engine, so it
 * cannot run with esp_sdr_rx_capture() on another thread.
 *
 * @return 0, -EINVAL for a bad configuration, -EAGAIN before esp_sdr_init(),
 *         -EBUSY when already running, -EOVERFLOW when the interrupt could not
 *         keep up with the ring (stats->overrun).
 */
int esp_sdr_diag_run(const struct esp_sdr_diag_cfg *cfg, struct esp_sdr_diag_stats *stats);

/** @brief Ask a running esp_sdr_diag_run() to return. */
void esp_sdr_diag_stop(void);

/** @return true while esp_sdr_diag_run() is in progress. */
bool esp_sdr_diag_active(void);

/**
 * @brief The nibble window start that fits the current signal.
 *
 * One 10-bit snapshot; the lowest start that does not wrap at the 99.9 %
 * point of the I and Q magnitudes.
 *
 * @param lsb Set to 0 to ESP_SDR_DIAG_LSB_MAX.
 * @return 0, or the error of esp_sdr_rx_capture().
 */
int esp_sdr_diag_level(uint8_t *lsb);

/**
 * @brief Bench hook: called by esp_sdr_diag_run() once the dump engine runs,
 * before the pads and the ring start. Weak and empty; a bench application can
 * override it to change or record modem registers.
 */
void esp_sdr_diag_armed_hook(void);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ESP_SDR_ESP_SDR_DIAG_H_ */
