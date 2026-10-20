/* SPDX-License-Identifier: GPL-3.0-or-later */

/**
 * @file
 * @brief The esp-sdr gateware on the MCH2022 badge's iCE40UP5K.
 *
 * One SPI frame (one CS assertion) carries one command: an opcode byte, then its payload.
 */

#ifndef ESDR_FPGA_H_
#define ESDR_FPGA_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure the FPGA with a bitstream.
 *
 * Holds the FPGA in reset through the RP2040, releases it with CS low (SPI slave mode), then
 * streams @p image in DMA-sized chunks. The image may sit in flash or PSRAM.
 *
 * @return 0, -EIO when CDONE does not follow, or a bus error.
 */
int esdr_fpga_load(const struct device *dev, const uint8_t *image, size_t size);

/** @brief Hold the FPGA in reset (its pins float). */
int esdr_fpga_off(const struct device *dev);

/** @brief Whether a bitstream is loaded and CDONE is high. */
bool esdr_fpga_loaded(const struct device *dev);

/**
 * @brief One write frame: @p head then @p data (either may be empty) under one CS assertion.
 */
int esdr_fpga_write(const struct device *dev, const void *head, size_t head_len,
		    const void *data, size_t len);

/**
 * @brief One read frame at the read clock: send @p head, then receive @p len bytes.
 *
 * @p rx must be 4-byte aligned and @p len a multiple of 4.
 */
int esdr_fpga_read(const struct device *dev, const void *head, size_t head_len, void *rx,
		   size_t len);

/** @brief Write @p n words to the gateware's registers from @p addr on. */
int esdr_fpga_reg_write(const struct device *dev, uint16_t addr, const uint16_t *words,
			size_t n);

/** @brief Read @p n words (at most 2046) from @p addr on. */
int esdr_fpga_reg_read(const struct device *dev, uint16_t addr, uint16_t *words, size_t n);

/** @brief Change the read clock (tests; the default comes from devicetree). */
void esdr_fpga_set_read_frequency(const struct device *dev, uint32_t hz);

#ifdef __cplusplus
}
#endif

#endif /* ESDR_FPGA_H_ */
