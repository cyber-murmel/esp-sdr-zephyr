/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The esp-sdr gateware on the MCH2022 badge's iCE40UP5K: command frames on the FPGA's SPI port.
 * The parent node, Zephyr's lattice,ice40-fpga, configures it; the ESP32's SPI driver splits long
 * transfers for its DMA and copies what the DMA cannot reach.
 */

#define DT_DRV_COMPAT esdr_mch2022_fpga

#include <zephyr/device.h>
#include <zephyr/drivers/fpga.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <esp_sdr/esdr_fpga.h>
#include <esp_sdr/esdr_fpga_regs.h>

LOG_MODULE_REGISTER(esdr_fpga, CONFIG_ESDR_FPGA_LOG_LEVEL);

struct esdr_fpga_config {
	const struct device *ice40;
	struct spi_dt_spec bus;
	uint32_t frequency;
	uint32_t read_frequency;
	struct gpio_dt_spec irq;
};

struct esdr_fpga_data {
	struct k_mutex lock;
	/* The bus with the chip select left to us: frames span several SPI writes. */
	struct spi_dt_spec bus_nocs;
	/* The bus at the read clock (a second spi_config, same CS). */
	struct spi_dt_spec bus_read;
	/* Spectrum columns as bus words. */
	uint16_t cols[512];
};

bool esdr_fpga_loaded(const struct device *dev)
{
	const struct esdr_fpga_config *cfg = dev->config;

	return fpga_get_status(cfg->ice40) == FPGA_STATUS_ACTIVE;
}

/* A frame's part. The DMA always sends at least a byte: an empty part must not reach it. */
static int write_part(const struct device *dev, const void *buf, size_t len)
{
	struct esdr_fpga_data *data = dev->data;
	const struct spi_buf b = {.buf = (void *)buf, .len = len};
	const struct spi_buf_set set = {.buffers = &b, .count = 1};

	return len == 0 ? 0 : spi_write_dt(&data->bus_nocs, &set);
}

int esdr_fpga_write(const struct device *dev, const void *head, size_t head_len,
		    const void *payload, size_t len)
{
	const struct esdr_fpga_config *cfg = dev->config;
	struct esdr_fpga_data *data = dev->data;
	const struct gpio_dt_spec *cs = &cfg->bus.config.cs.gpio;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	gpio_pin_set_dt(cs, 1);
	ret = write_part(dev, head, head_len);
	if (ret == 0) {
		ret = write_part(dev, payload, len);
	}
	gpio_pin_set_dt(cs, 0);
	k_mutex_unlock(&data->lock);
	return ret;
}

static int read_frame(const struct device *dev, const void *head, size_t head_len, void *rx,
		      size_t len)
{
	struct esdr_fpga_data *data = dev->data;
	/*
	 * Half duplex: the header goes out as a write-only chunk, then a read-only chunk clocks
	 * len bytes in (no dummy bit below 26.7 MHz). Needs the SPI driver to leave the write
	 * phase out of read-only chunks: the classic ESP32's DMA cannot do both in one.
	 */
	const struct spi_buf tx = {.buf = (void *)head, .len = head_len};
	const struct spi_buf rxb[2] = {
		{.buf = NULL, .len = head_len},
		{.buf = rx, .len = len},
	};
	const struct spi_buf_set tx_set = {.buffers = &tx, .count = 1};
	const struct spi_buf_set rx_set = {.buffers = rxb, .count = 2};

	/* The classic ESP32's DMA loses the end of a read short of a whole 32-bit word. */
	if (len % 4 != 0) {
		return -EINVAL;
	}
	return spi_transceive_dt(&data->bus_read, &tx_set, &rx_set);
}

int esdr_fpga_read(const struct device *dev, const void *head, size_t head_len, void *rx,
		   size_t len)
{
	struct esdr_fpga_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = read_frame(dev, head, head_len, rx, len);
	k_mutex_unlock(&data->lock);
	return ret;
}

int esdr_fpga_reg_write(const struct device *dev, uint16_t addr, const uint16_t *words,
			size_t n)
{
	const uint8_t head[3] = {ESDR_FPGA_OP_WRITE, addr & 0xff, addr >> 8};

	/* Words go out little-endian, as the ESP32 stores them. */
	return esdr_fpga_write(dev, head, sizeof(head), words, n * 2);
}

static int read_even(const struct device *dev, uint16_t addr, void *words, size_t n)
{
	/* Two pad bytes give the gateware time to fetch the first word. */
	const uint8_t head[7] = {
		ESDR_FPGA_OP_READ, addr & 0xff, addr >> 8, n & 0xff, n >> 8, 0, 0,
	};

	return esdr_fpga_read(dev, head, sizeof(head), words, n * 2);
}

int esdr_fpga_reg_read(const struct device *dev, uint16_t addr, uint16_t *words, size_t n)
{
	/* An odd count's last word comes with a spare one, for the DMA's whole 32-bit words. */
	uint16_t tail[2] __aligned(4);
	size_t even = n & ~(size_t)1;
	int ret = 0;

	if (even != 0) {
		ret = read_even(dev, addr, words, even);
	}
	if (ret == 0 && even != n) {
		ret = read_even(dev, addr + even, tail, ARRAY_SIZE(tail));
		words[even] = tail[0];
	}
	return ret;
}

void esdr_fpga_set_read_frequency(const struct device *dev, uint32_t hz)
{
	struct esdr_fpga_data *data = dev->data;

	k_mutex_lock(&data->lock, K_FOREVER);
	data->bus_read.config.frequency = hz;
	k_mutex_unlock(&data->lock);
}

#ifdef CONFIG_ESDR_FPGA_IMAGE_EMBEDDED
static const uint8_t image[] __aligned(4) = {
#include "esdr_fpga_image.inc"
};
#endif

int esdr_fpga_palette(const struct device *dev, uint8_t first, const uint16_t *rgb565, size_t n)
{
	if (first + n > 256) {
		return -EINVAL;
	}
	return esdr_fpga_reg_write(dev, ESDR_FPGA_PALETTE + first, rgb565, n);
}

int esdr_fpga_waterfall(const struct device *dev, uint16_t x, uint16_t y, uint16_t w,
			uint16_t h)
{
	/* The ring takes as many lines as fit in the 64 KiB. */
	uint32_t ring = w ? MIN(65536U / w, 0xffffU) : 1U;
	const uint16_t regs[] = {x, y, w, h, ring, w && h ? 1 : 0};

	if (w > 320 || (w && ring <= h)) {
		return -EINVAL;
	}
	/* WF_PAGE0 .. WF_FLAGS: native row x carries column x, native column y row y. */
	return esdr_fpga_reg_write(dev, ESDR_FPGA_REG_WF_PAGE0, regs, ARRAY_SIZE(regs));
}

int esdr_fpga_wf_line(const struct device *dev, const uint8_t *idx, size_t n)
{
	const uint8_t op = ESDR_FPGA_OP_WFLINE;

	return esdr_fpga_write(dev, &op, 1, idx, n);
}

int esdr_fpga_spectrum(const struct device *dev, uint16_t x, uint16_t y, uint16_t w,
		       uint16_t h, const struct esdr_fpga_trace *t)
{
	const uint16_t regs[] = {x, y, w, h, w && h ? t->flags | ESDR_FPGA_SP_ON : 0};

	if (w > 320) {
		return -EINVAL;
	}
	return esdr_fpga_reg_write(dev, ESDR_FPGA_REG_SP_PAGE0, regs, ARRAY_SIZE(regs));
}

int esdr_fpga_columns(const struct device *dev, const uint8_t *rows, size_t n)
{
	struct esdr_fpga_data *data = dev->data;
	const uint16_t commit = 1;
	int ret;

	if (n > ARRAY_SIZE(data->cols)) {
		return -EINVAL;
	}
	/* The mutex nests: esdr_fpga_write() takes it again. */
	k_mutex_lock(&data->lock, K_FOREVER);
	for (size_t i = 0; i < n; i++) {
		data->cols[i] = rows[i];
	}
	ret = esdr_fpga_reg_write(dev, ESDR_FPGA_COLUMNS, data->cols, n);
	if (ret == 0) {
		ret = esdr_fpga_reg_write(dev, ESDR_FPGA_REG_COL_COMMIT, &commit, 1);
	}
	k_mutex_unlock(&data->lock);
	return ret;
}

int esdr_fpga_render(const struct device *dev, bool on, bool te_sync, bool line_dirty)
{
	uint16_t madctl;
	uint16_t ctrl = (on ? BIT(0) : 0) | (te_sync ? BIT(1) : 0) | (line_dirty ? BIT(2) : 0);
	int ret;

	/* The host's MADCTL without MV: native rows run along the screen's columns. */
	ret = esdr_fpga_reg_read(dev, ESDR_FPGA_REG_MADCTL_HOST, &madctl, 1);
	if (ret == 0) {
		madctl &= ~BIT(5);
		ret = esdr_fpga_reg_write(dev, ESDR_FPGA_REG_MADCTL_FPGA, &madctl, 1);
	}
	if (ret == 0) {
		ret = esdr_fpga_reg_write(dev, ESDR_FPGA_REG_CTRL, &ctrl, 1);
	}
	return ret;
}

int esdr_fpga_redraw(const struct device *dev, bool waterfall, bool spectrum)
{
	const uint16_t dirty = (waterfall ? BIT(0) : 0) | (spectrum ? BIT(1) : 0);

	return esdr_fpga_reg_write(dev, ESDR_FPGA_REG_DIRTY, &dirty, 1);
}

/* The LED breathes: the block's setup, then the colour, which starts it. */
static int led_start(const struct device *dev)
{
	const uint16_t words[] = {
		ESDR_FPGA_LED_SETUP,
		ESDR_FPGA_LED_RUN | ESDR_FPGA_LED_PWRR | CONFIG_ESDR_FPGA_LED_RED,
		ESDR_FPGA_LED_RUN | ESDR_FPGA_LED_PWRG | CONFIG_ESDR_FPGA_LED_GREEN,
		ESDR_FPGA_LED_RUN | ESDR_FPGA_LED_PWRB | CONFIG_ESDR_FPGA_LED_BLUE,
	};
	int ret = 0;

	/* A frame each: a frame's words go to consecutive addresses. */
	for (size_t i = 0; i < ARRAY_SIZE(words) && ret == 0; i++) {
		ret = esdr_fpga_reg_write(dev, ESDR_FPGA_REG_LED, &words[i], 1);
	}
	return ret;
}

static int esdr_fpga_init(const struct device *dev)
{
	const struct esdr_fpga_config *cfg = dev->config;
	struct esdr_fpga_data *data = dev->data;

	k_mutex_init(&data->lock);
	data->bus_nocs = cfg->bus;
	data->bus_nocs.config.frequency = cfg->frequency;
	data->bus_nocs.config.cs = (struct spi_cs_control){.cs_is_gpio = false};
	data->bus_read = cfg->bus;
	data->bus_read.config.frequency = cfg->read_frequency;
	if (!spi_is_ready_dt(&cfg->bus) || !device_is_ready(cfg->ice40)) {
		return -ENODEV;
	}
	if (cfg->irq.port != NULL) {
		if (!gpio_is_ready_dt(&cfg->irq)) {
			return -ENODEV;
		}
		gpio_pin_configure_dt(&cfg->irq, GPIO_INPUT);
	}
#ifdef CONFIG_ESDR_FPGA_IMAGE_EMBEDDED
	/* A failed load leaves the FPGA in reset; the display falls back to the ESP32's SPI. */
	int64_t start = k_uptime_get();
	int ret = fpga_load(cfg->ice40, (uint32_t *)(uintptr_t)image, sizeof(image));

	if (ret == 0) {
		ret = fpga_on(cfg->ice40);
	}
	if (ret == 0) {
		ret = led_start(dev);
	}
	if (ret < 0) {
		LOG_ERR("load of %zu bytes failed: %d", sizeof(image), ret);
	} else {
		LOG_INF("loaded %zu bytes in %lld ms", sizeof(image), k_uptime_get() - start);
	}
#endif
	return 0;
}

#define SPI_OP (SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB)

#define ESDR_FPGA_DEFINE(inst)                                                                     \
	static struct esdr_fpga_data esdr_fpga_data_##inst;                                        \
	static const struct esdr_fpga_config esdr_fpga_config_##inst = {                           \
		.ice40 = DEVICE_DT_GET(DT_INST_PARENT(inst)),                                      \
		.bus = SPI_DT_SPEC_GET(DT_INST_PARENT(inst), SPI_OP),                              \
		.frequency = DT_INST_PROP(inst, frequency),                                        \
		.read_frequency = DT_INST_PROP(inst, read_frequency),                              \
		.irq = GPIO_DT_SPEC_INST_GET_OR(inst, irq_gpios, {0}),                             \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, esdr_fpga_init, NULL, &esdr_fpga_data_##inst,                  \
			      &esdr_fpga_config_##inst, POST_KERNEL,                               \
			      CONFIG_ESDR_FPGA_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(ESDR_FPGA_DEFINE)
