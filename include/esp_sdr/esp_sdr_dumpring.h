/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file
 * @brief ESP-SDR gapless receive on the ESP32-S31 through the dump engine alone.
 *
 * The engine that captures bursts also runs without a stop: in its circular mode it writes the
 * first words of whichever TCM group the ownership register gives it, and the CPU can read only a
 * group the engine does not own. The run moves it between group 2 (0x2F040000, which holds nothing
 * of the image) and the capture bank's group 3 and reads each while the other is written, so every
 * word arrives, once and in order. The method is ESPARGOS esp-sdr's S31 continuous spectra
 * (main/targets/esp32s31/burst/s31_spectrum.h), which turn the groups in equal units; here group 2
 * takes the long units and group 3 only as many words as it takes to copy one out of group 2: group
 * 3 holds the ROM's data, which the CPUs must not read while the engine owns it, so meanwhile this
 * core runs with interrupts off and the other core waits in its stall interrupt. On CPU 1, the
 * S31's only core with PIE, the turn copies and marks 4 words a 128-bit load or store: some 2.4k
 * CPU cycles a turn at 20 MS/s instead of 5.9k.
 *
 * Sentinel words mark where each unit may start and end; a unit's first word is the last unit's
 * end, or the run ends with -EIO. Needs no pads and no GPIO matrix, so it runs beside the panel of
 * the VIEWE board, where the diagnostic bus's ring (esp_sdr_diag16.h) cannot.
 *
 * Words: as a burst's, esp_sdr_rx_i() and esp_sdr_rx_q() (esp_sdr_rx.h) take out I and Q, 10
 * bits each; the bits above are the receiver's state. Only the analog low-pass stands before the
 * capture rate: at 20 MS/s it passes some 13 MHz, so a lower rate aliases.
 */

#ifndef ESP_SDR_ESP_SDR_DUMPRING_H_
#define ESP_SDR_ESP_SDR_DUMPRING_H_

#include <stddef.h>
#include <stdint.h>

#include <esp_sdr/esp_sdr.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup esp_sdr_dumpring
 * @{
 */

/**
 * Receives the next words of the run, in order, on the run's thread with interrupts on. The
 * pointer is good for the call only. The engine keeps writing meanwhile: a sink slower than the
 * capture rate ends the run with -ENOBUFS.
 */
typedef void (*esp_sdr_dumpring_sink_t)(void *user, const uint32_t *words, size_t n);

/** Run settings. */
struct esp_sdr_dumpring_cfg {
	/** ESP_SDR_RATE_20MSPS, 10, 8 or 4. */
	enum esp_sdr_rate rate;
	esp_sdr_dumpring_sink_t sink;
	void *user;
};

/** What a run did. */
struct esp_sdr_dumpring_stats {
	/** Words given to the sink. */
	uint64_t words;
	/** Turns to group 3 and back. */
	uint32_t turns;
	/** Words written past a unit's end before the turn (the sink's lateness), at most. */
	uint32_t late_max;
	/** CPU cycles of a turn (the other core waits as long), longest and in all. */
	uint32_t turn_cycles_max;
	uint64_t turn_cycles;
	/** CPU cycles of the run. */
	uint64_t cycles;
};

/**
 * @brief Receive until esp_sdr_dumpring_stop() or an error, on the calling thread.
 *
 * Holds the radio meanwhile, at the frequency and gain set before. With two cores, call it from a
 * thread pinned to CPU 1: the turns park the other one.
 *
 * @param cfg Settings.
 * @param stats Filled in at the end; may be NULL.
 * @return 0 after a stop, -EINVAL for a bad setting, -EAGAIN before esp_sdr_init() and a tuning,
 *	   -EBUSY while another run, a replay or a capture holds the engine, -ENOBUFS when the sink
 *	   fell behind, -EIO when a unit did not start where the last one ended, -ETIMEDOUT when the
 *	   engine stopped writing.
 */
int esp_sdr_dumpring_run(const struct esp_sdr_dumpring_cfg *cfg,
			 struct esp_sdr_dumpring_stats *stats);

/** @brief End a run at its next unit; from any thread or the sink. */
void esp_sdr_dumpring_stop(void);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ESP_SDR_ESP_SDR_DUMPRING_H_ */
