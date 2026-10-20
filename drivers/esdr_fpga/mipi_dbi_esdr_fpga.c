/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The MCH2022 badge's ILI9341 behind the iCE40 (gateware/esdr_fpga/dbi.py): the display
 * driver's commands and pixels become DBI frames on the FPGA's SPI port, which plays them on
 * the panel's 8080 bus. Without gateware the panel stays on the ESP32's SPI bus.
 */

#define DT_DRV_COMPAT esdr_mch2022_fpga_dbi

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/mipi_dbi.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <esp_sdr/esdr_fpga.h>
#include <esp_sdr/esdr_fpga_regs.h>

LOG_MODULE_REGISTER(mipi_dbi_esdr_fpga, CONFIG_ESDR_FPGA_LOG_LEVEL);

#define CASET 0x2A
#define PASET 0x2B
#define RAMWR 0x2C

struct dbi_fpga_config {
	const struct device *fpga;
	const struct device *spi;
	struct gpio_dt_spec dc;
	struct gpio_dt_spec reset;
	struct gpio_dt_spec mode;
	/* The panel's CS on the ESP32 side (spi-dev's cs-gpios for reg 0). */
	struct gpio_dt_spec cs;
};

struct dbi_fpga_data {
	struct k_mutex lock;
	bool fpga;
	/*
	 * The open memory write: the gateware's renderer must not take the bus between the
	 * window, RAMWR and the pixels that fill it, which come in separate frames.
	 */
	uint16_t window[4];		/* CASET start, end; PASET start, end */
	uint32_t remaining;		/* bytes until the window is full */
};

/* SPI fallback, as zephyr,mipi-dbi-spi drives a 4-wire panel. */
static int spi_write_dc(const struct device *dev, const struct mipi_dbi_config *dbi, bool data,
			const uint8_t *buf, size_t len)
{
	const struct dbi_fpga_config *cfg = dev->config;
	struct spi_buf b = {.buf = (void *)buf, .len = len};
	struct spi_buf_set set = {.buffers = &b, .count = 1};

	gpio_pin_set_dt(&cfg->dc, data);
	return spi_write(cfg->spi, &dbi->config, &set);
}

static int dbi_fpga_command_write(const struct device *dev, const struct mipi_dbi_config *dbi,
				  uint8_t cmd, const uint8_t *data_buf, size_t len)
{
	const struct dbi_fpga_config *cfg = dev->config;
	struct dbi_fpga_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	if (data->fpga) {
		uint8_t head[3] = {ESDR_FPGA_OP_DBI, 0, cmd};

		data->remaining = 0;
		if ((cmd == CASET || cmd == PASET) && len >= 4) {
			uint16_t *w = &data->window[cmd == CASET ? 0 : 2];

			w[0] = sys_get_be16(&data_buf[0]);
			w[1] = sys_get_be16(&data_buf[2]);
			head[1] = ESDR_FPGA_DBI_HOLD;
		} else if (cmd == RAMWR) {
			data->remaining = 2U * (data->window[1] - data->window[0] + 1U) *
					  (data->window[3] - data->window[2] + 1U);
			data->remaining -= MIN(data->remaining, len);
			head[1] = data->remaining ? ESDR_FPGA_DBI_HOLD : 0;
		}
		ret = esdr_fpga_write(cfg->fpga, head, sizeof(head), data_buf, len);
	} else {
		ret = spi_write_dc(dev, dbi, false, &cmd, 1);
		if (ret == 0 && len > 0) {
			ret = spi_write_dc(dev, dbi, true, data_buf, len);
		}
		spi_release(cfg->spi, &dbi->config);
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

static int dbi_fpga_write_display(const struct device *dev, const struct mipi_dbi_config *dbi,
				  const uint8_t *framebuf, struct display_buffer_descriptor *desc,
				  enum display_pixel_format pixfmt)
{
	const struct dbi_fpga_config *cfg = dev->config;
	struct dbi_fpga_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	if (data->fpga) {
		uint8_t head[2] = {ESDR_FPGA_OP_DBI, ESDR_FPGA_DBI_NOCMD};

		data->remaining -= MIN(data->remaining, desc->buf_size);
		if (data->remaining) {
			head[1] |= ESDR_FPGA_DBI_HOLD;
		}
		ret = esdr_fpga_write(cfg->fpga, head, sizeof(head), framebuf, desc->buf_size);
	} else {
		ret = spi_write_dc(dev, dbi, true, framebuf, desc->buf_size);
		spi_release(cfg->spi, &dbi->config);
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

static int dbi_fpga_reset(const struct device *dev, k_timeout_t delay)
{
	const struct dbi_fpga_config *cfg = dev->config;
	int ret;

	ret = gpio_pin_set_dt(&cfg->reset, 1);
	if (ret < 0) {
		return ret;
	}
	k_sleep(delay);
	return gpio_pin_set_dt(&cfg->reset, 0);
}

static int dbi_fpga_configure_te(const struct device *dev, uint8_t edge, k_timeout_t delay)
{
	/* The gateware watches FMARK itself; the display driver may send TEON. */
	return 0;
}

static DEVICE_API(mipi_dbi, dbi_fpga_api) = {
	.command_write = dbi_fpga_command_write,
	.write_display = dbi_fpga_write_display,
	.reset = dbi_fpga_reset,
	.configure_te = dbi_fpga_configure_te,
};

static int dbi_fpga_init(const struct device *dev)
{
	const struct dbi_fpga_config *cfg = dev->config;
	struct dbi_fpga_data *data = dev->data;
	uint16_t id = 0;
	int ret;

	k_mutex_init(&data->lock);
	if (!gpio_is_ready_dt(&cfg->dc) || !gpio_is_ready_dt(&cfg->reset) ||
	    !gpio_is_ready_dt(&cfg->mode) || !device_is_ready(cfg->spi)) {
		return -ENODEV;
	}
	ret = gpio_pin_configure_dt(&cfg->reset, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		return ret;
	}

	if (device_is_ready(cfg->fpga) && esdr_fpga_loaded(cfg->fpga) &&
	    esdr_fpga_reg_read(cfg->fpga, ESDR_FPGA_REG_ID, &id, 1) == 0 && id == ESDR_FPGA_ID) {
		/* The FPGA's WR and CS meet D/C and CS through 47 ohm: float ours first. */
		gpio_pin_configure_dt(&cfg->dc, GPIO_INPUT);
		gpio_pin_configure_dt(&cfg->cs, GPIO_INPUT);
		data->fpga = true;
		ret = gpio_pin_configure_dt(&cfg->mode, GPIO_OUTPUT_ACTIVE);
		LOG_INF("panel on the FPGA's bus");
	} else {
		data->fpga = false;
		gpio_pin_configure_dt(&cfg->mode, GPIO_OUTPUT_INACTIVE);
		ret = gpio_pin_configure_dt(&cfg->dc, GPIO_OUTPUT_INACTIVE);
		LOG_INF("panel on SPI (no gateware, ID 0x%04x)", id);
	}
	return ret;
}

#define DBI_FPGA_DEFINE(inst)                                                                      \
	static const struct dbi_fpga_config dbi_fpga_config_##inst = {                             \
		.fpga = DEVICE_DT_GET(DT_INST_PHANDLE(inst, fpga)),                                \
		.spi = DEVICE_DT_GET(DT_INST_PHANDLE(inst, spi_dev)),                              \
		.dc = GPIO_DT_SPEC_INST_GET(inst, dc_gpios),                                       \
		.reset = GPIO_DT_SPEC_INST_GET(inst, reset_gpios),                                 \
		.mode = GPIO_DT_SPEC_INST_GET(inst, lcd_mode_gpios),                               \
		.cs = GPIO_DT_SPEC_GET_BY_IDX(DT_INST_PHANDLE(inst, spi_dev), cs_gpios, 0),        \
	};                                                                                         \
	static struct dbi_fpga_data dbi_fpga_data_##inst;                                          \
	DEVICE_DT_INST_DEFINE(inst, dbi_fpga_init, NULL, &dbi_fpga_data_##inst,                    \
			      &dbi_fpga_config_##inst, POST_KERNEL,                                \
			      CONFIG_MIPI_DBI_ESDR_FPGA_INIT_PRIORITY, &dbi_fpga_api);

DT_INST_FOREACH_STATUS_OKAY(DBI_FPGA_DEFINE)
