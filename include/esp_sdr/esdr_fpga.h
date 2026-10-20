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

/*
 * Graphics. Regions are in screen pixels, in the display driver's orientation; every pixel is
 * a palette index (entries ESDR_FPGA_PAL_BG.. are the trace's colours, the waterfall uses
 * 0..247).
 */

/** @brief Load @p n RGB565 palette entries from @p first on. */
int esdr_fpga_palette(const struct device *dev, uint8_t first, const uint16_t *rgb565, size_t n);

/** @brief Place the waterfall: @p w columns (at most 320) of @p h rows; w = 0 turns it off. */
int esdr_fpga_waterfall(const struct device *dev, uint16_t x, uint16_t y, uint16_t w,
			uint16_t h);

/** @brief A new waterfall line on top: @p n palette indices, one per column. */
int esdr_fpga_wf_line(const struct device *dev, const uint8_t *idx, size_t n);

/** @brief The spectrum trace's look. */
struct esdr_fpga_trace {
	uint8_t flags;		/**< ESDR_FPGA_SP_FILL, _HGRID, _VGRID, _CENTER */
	uint16_t grid_y0;	/**< rows above the first horizontal grid line */
	uint16_t grid_dy;	/**< rows between horizontal grid lines */
	uint16_t grid_dx;	/**< columns between vertical grid lines */
};

/** @brief Place the spectrum trace; w = 0 turns it off. */
int esdr_fpga_spectrum(const struct device *dev, uint16_t x, uint16_t y, uint16_t w,
		       uint16_t h, const struct esdr_fpga_trace *t);

/** @brief New trace: the row (0 at the top) of each of @p n columns; redraws the trace. */
int esdr_fpga_columns(const struct device *dev, const uint8_t *rows, size_t n);

/**
 * @brief Start or stop the renderer.
 *
 * @param te_sync start renders on the panel's tearing-effect edge
 * @param line_dirty redraw the waterfall after every new line
 */
int esdr_fpga_render(const struct device *dev, bool on, bool te_sync, bool line_dirty);

/** @brief Redraw regions now (or at the next TE). */
int esdr_fpga_redraw(const struct device *dev, bool waterfall, bool spectrum);

#ifdef __cplusplus
}
#endif

#endif /* ESDR_FPGA_H_ */
