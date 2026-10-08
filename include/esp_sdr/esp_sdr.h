/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * @file
 * @brief ESP-SDR: raw I/Q receive and transmit on the ESP32-S3 Wi-Fi radio.
 *
 * Zephyr port of ESPARGOS esp-sdr, extended with transmit. Both directions
 * share the radio, its LO and the dump engine's SRAM banks: one user at a
 * time, all calls may sleep. Receive: esp_sdr_rx.h; transmit: esp_sdr_tx.h
 * (both included here).
 */

#ifndef ESP_SDR_ESP_SDR_H_
#define ESP_SDR_ESP_SDR_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @addtogroup esp_sdr_radio
 * @{
 */

/** Software tuning limits in MHz; PLL lock is not guaranteed across the range. */
#define ESP_SDR_FREQ_MIN_MHZ 100U
#define ESP_SDR_FREQ_MAX_MHZ 6000U

/** Limits of one capture or playback burst in complex samples (one dump bank). */
#define ESP_SDR_SAMPLES_MIN 256U
#define ESP_SDR_SAMPLES_MAX 16380U

/** Dump banks available for capture and loop playback (CONFIG_ESP_SDR_BANK1 adds one). */
#if defined(CONFIG_ESP_SDR_BANK1)
#define ESP_SDR_BANKS 2
#else
#define ESP_SDR_BANKS 1
#endif

/**
 * Place a static buffer in the internal SRAM above the capture bank (about
 * 20 KiB, DMA capable). It is not zeroed at boot: buffers only.
 */
#define ESP_SDR_HIGH_RAM __attribute__((section(".esp_sdr_high")))

/**
 * Native dump engine rates; values are the upstream protocol rate indices.
 * The S3 receives at all three (esp_sdr_rx_rate_hz()), the C6 at 80 MS/s only
 * (the others return 0); the DAC runs at 80 and 40 MS/s (esp_sdr_tx_rate_hz(),
 * S3 only).
 */
enum esp_sdr_rate {
	ESP_SDR_RATE_80MSPS = 0,
	ESP_SDR_RATE_40MSPS = 1,
	ESP_SDR_RATE_16MSPS = 6,
};

/** Complex sample, I then Q, full scale +-32767. */
struct esp_sdr_iq16 {
	int16_t i;
	int16_t q;
};

/**
 * @brief Bring the radio up and tune it to the default frequency.
 *
 * Needs the Wi-Fi driver started; safe to call more than once. The radio
 * rests in the receive state between transmissions.
 *
 * @retval 0 on success.
 * @retval -ENODEV if the Wi-Fi driver is not ready.
 * @retval -EIO if a Wi-Fi call failed.
 */
int esp_sdr_init(void);

/**
 * @brief Tune the LO, shared by receive and transmit.
 *
 * @param mhz Frequency in MHz, ESP_SDR_FREQ_MIN_MHZ to ESP_SDR_FREQ_MAX_MHZ.
 * @retval 0 on success.
 * @retval -EINVAL if out of range.
 * @retval -EAGAIN if esp_sdr_init() has not run.
 */
int esp_sdr_set_freq(uint32_t mhz);

/** @return Current LO frequency in MHz. */
uint32_t esp_sdr_get_freq(void);

/**
 * @brief Set a PLL offset in kHz and retune.
 *
 * A non-zero offset forces direct PLL tuning, also on Wi-Fi channel frequencies.
 *
 * @retval 0 on success.
 * @retval -EAGAIN if esp_sdr_init() has not run.
 */
int esp_sdr_set_freq_offset(int32_t khz);

/**
 * @brief Set the channel bandwidth flag of the vendor tuning call (experimental).
 *
 * Passed as the bandwidth argument of the PHY's channel setup; 0 (default)
 * is the 20 MHz configuration the receiver was characterized with.
 *
 * @retval 0 on success.
 * @retval -EINVAL above 2.
 * @retval -EAGAIN if esp_sdr_init() has not run.
 */
int esp_sdr_set_channel_bw(unsigned int cbw);

/** Radio housekeeping counters, for diagnosis. */
struct esp_sdr_stats {
	/** Transmit sessions; forced RX gain re-applied at captures, and the last one's time. */
	uint32_t dac_sessions, gain_refreshed, gain_apply_us;
};

/** Copy the radio housekeeping counters into @p stats. */
void esp_sdr_get_stats(struct esp_sdr_stats *stats);

/**
 * @brief Read or write a register of the analog baseband (I2C block 0x67), for characterization.
 *
 * Unchecked: wrong values can detune or disable the radio until the next
 * retune or reset.
 */
int esp_sdr_bbtop_read(unsigned int reg);
void esp_sdr_bbtop_write(unsigned int reg, unsigned int val);

/**
 * @brief Set the front end turnaround between transmit and receive.
 *
 * Every switch to TX (playback, loop sessions) and back to RX waits
 * @p settle_us, and with @p retune also reprograms the LO first. Defaults:
 * 3000 us with retune, as after a frequency change (which always retunes and
 * waits the full 3000 us). Shorter turnarounds suit packet links; check the
 * signal quality at the chosen setting.
 *
 * @retval 0 on success.
 * @retval -EINVAL for more than 3000 us.
 */
int esp_sdr_set_turnaround(uint32_t settle_us, bool retune);

/** @} */

#ifdef __cplusplus
}
#endif

#include <esp_sdr/esp_sdr_rx.h>
#include <esp_sdr/esp_sdr_tx.h>

#endif /* ESP_SDR_ESP_SDR_H_ */
