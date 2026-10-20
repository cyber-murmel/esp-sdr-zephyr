/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The esp-sdr gateware on the MCH2022 badge's iCE40UP5K: configuration through the RP2040
 * (CRESET_B, CDONE) and the configuration SPI port, then command frames on the same port.
 *
 * Not lattice,ice40-fpga: that driver releases CRESET at init while SS is high, so the iCE40
 * would boot as SPI master and drive SCK and SS against the ESP32; it also caps the clock at
 * 25 MHz and loads from the system workqueue.
 */

#define DT_DRV_COMPAT esdr_mch2022_fpga

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/mfd/mch2022_rp2040.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <esp_memory_utils.h>

#include <esp_sdr/esdr_fpga.h>
#include <esp_sdr/esdr_fpga_regs.h>

LOG_MODULE_REGISTER(esdr_fpga, CONFIG_ESDR_FPGA_LOG_LEVEL);

/* The ESP32 SPI DMA moves at most 4092 bytes per descriptor. */
#define CHUNK 4092U

/* RP2040 register 0x05: bit 0 releases CRESET_B, bit 1 loops the FPGA UART back. */
#define FPGA_RUN BIT(0)

struct esdr_fpga_config {
	struct spi_dt_spec bus;
	uint32_t read_frequency;
	const struct device *mfd;
	struct gpio_dt_spec irq;
};

struct esdr_fpga_data {
	struct k_mutex lock;
	/* The bus with the chip select left to us: frames span several SPI writes. */
	struct spi_dt_spec bus_nocs;
	/* The bus at the read clock (a second spi_config, same CS). */
	struct spi_dt_spec bus_read;
	bool loaded;
	/* DMA-capable bounce buffer for images in flash or PSRAM. */
	uint8_t bounce[CHUNK] __aligned(4);
	/* Spectrum columns as bus words. */
	uint16_t cols[512];
};

static int creset(const struct device *dev, bool run)
{
	const struct esdr_fpga_config *cfg = dev->config;
	uint8_t reg;
	int ret;

	ret = mfd_mch2022_rp2040_read(cfg->mfd, MCH2022_RP2040_FPGA, &reg, 1);
	if (ret < 0) {
		return ret;
	}
	reg = run ? (reg | FPGA_RUN) : (reg & ~FPGA_RUN);
	return mfd_mch2022_rp2040_write(cfg->mfd, MCH2022_RP2040_FPGA, &reg, 1);
}

static int cdone(const struct device *dev)
{
	const struct esdr_fpga_config *cfg = dev->config;
	uint8_t reg;
	int ret;

	ret = mfd_mch2022_rp2040_read(cfg->mfd, MCH2022_RP2040_INPUT1, &reg, 1);
	if (ret < 0) {
		return ret;
	}
	return !(reg & BIT(MCH2022_RP2040_FPGA_NOT_DONE));
}

/* SPI writes with the CS GPIO left alone: the configuration sequence drives it by hand. */
static int raw_write(const struct spi_dt_spec *bus, const void *buf, size_t len)
{
	const struct spi_buf b = {.buf = (void *)buf, .len = len};
	const struct spi_buf_set set = {.buffers = &b, .count = 1};

	return spi_write_dt(bus, &set);
}

int esdr_fpga_load(const struct device *dev, const uint8_t *image, size_t size)
{
	const struct esdr_fpga_config *cfg = dev->config;
	struct esdr_fpga_data *data = dev->data;
	const struct gpio_dt_spec *cs = &cfg->bus.config.cs.gpio;
	const struct spi_dt_spec *bus = &data->bus_nocs;
	int64_t start = k_uptime_get();
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	data->loaded = false;

	/* Reset with SS low: the iCE40 samples SS on release and becomes an SPI slave. */
	ret = gpio_pin_configure_dt(cs, GPIO_OUTPUT_ACTIVE);
	if (ret == 0) {
		ret = creset(dev, false);
	}
	if (ret < 0) {
		goto out;
	}
	ret = cdone(dev);
	if (ret != 0) {
		LOG_ERR("CDONE still high in reset (%d)", ret);
		ret = ret < 0 ? ret : -EIO;
		goto out;
	}
	ret = creset(dev, true);
	if (ret < 0) {
		goto out;
	}
	/* At least 1200 us for the iCE40 to clear its configuration memory. */
	k_msleep(2);
	gpio_pin_set_dt(cs, 0);

	/* 8 clocks with SS high, then the image with SS low. */
	memset(data->bounce, 0, 20);
	ret = raw_write(bus, data->bounce, 1);
	if (ret < 0) {
		goto out;
	}
	gpio_pin_set_dt(cs, 1);
	for (size_t off = 0; off < size; off += CHUNK) {
		size_t n = MIN(CHUNK, size - off);

		memcpy(data->bounce, image + off, n);
		ret = raw_write(bus, data->bounce, n);
		if (ret < 0) {
			gpio_pin_set_dt(cs, 0);
			goto out;
		}
	}
	gpio_pin_set_dt(cs, 0);

	/* At least 49 more clocks for the user I/Os to wake up; the stock firmware sends 160. */
	memset(data->bounce, 0, 20);
	ret = raw_write(bus, data->bounce, 20);
	if (ret < 0) {
		goto out;
	}

	ret = cdone(dev);
	if (ret != 1) {
		LOG_ERR("CDONE low after %zu bytes (%d)", size, ret);
		ret = ret < 0 ? ret : -EIO;
		goto out;
	}
	data->loaded = true;
	ret = 0;
	LOG_INF("loaded %zu bytes in %lld ms", size, k_uptime_get() - start);
out:
	gpio_pin_configure_dt(cs, GPIO_OUTPUT_INACTIVE);
	k_mutex_unlock(&data->lock);
	return ret;
}

int esdr_fpga_off(const struct device *dev)
{
	struct esdr_fpga_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = creset(dev, false);
	data->loaded = false;
	k_mutex_unlock(&data->lock);
	return ret;
}

bool esdr_fpga_loaded(const struct device *dev)
{
	struct esdr_fpga_data *data = dev->data;

	return data->loaded;
}

/* Payload in DMA-sized pieces; buffers the DMA cannot read (flash, PSRAM) via the bounce. */
static int write_payload(const struct device *dev, const uint8_t *buf, size_t len)
{
	struct esdr_fpga_data *data = dev->data;
	int ret = 0;

	if (len == 0) {
		return 0;
	}
	if (esp_ptr_dma_capable(buf)) {
		return raw_write(&data->bus_nocs, buf, len);
	}
	for (size_t off = 0; off < len && ret == 0; off += CHUNK) {
		size_t n = MIN(CHUNK, len - off);

		memcpy(data->bounce, buf + off, n);
		ret = raw_write(&data->bus_nocs, data->bounce, n);
	}
	return ret;
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
	ret = write_payload(dev, head, head_len);
	if (ret == 0) {
		ret = write_payload(dev, payload, len);
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

	if (len > CHUNK || len % 4 != 0 || (uintptr_t)rx % 4 != 0) {
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

int esdr_fpga_reg_read(const struct device *dev, uint16_t addr, uint16_t *words, size_t n)
{
	struct esdr_fpga_data *data = dev->data;
	/* Whole 32-bit words for the DMA: read an even count into the bounce buffer. */
	size_t even = ROUND_UP(n, 2);
	size_t len = even * 2;
	/* Two pad bytes give the gateware time to fetch the first word. */
	const uint8_t head[7] = {
		ESDR_FPGA_OP_READ, addr & 0xff, addr >> 8, even & 0xff, even >> 8, 0, 0,
	};
	int ret;

	if (len > CHUNK) {
		return -EINVAL;
	}
	k_mutex_lock(&data->lock, K_FOREVER);
	ret = read_frame(dev, head, sizeof(head), data->bounce, len);
	if (ret == 0) {
		memcpy(words, data->bounce, n * 2);
	}
	k_mutex_unlock(&data->lock);
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
static const uint8_t image[] = {
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
	const uint16_t regs[] = {
		x, y, w, h, w && h ? t->flags | ESDR_FPGA_SP_ON : 0,
		t->grid_y0, MAX(t->grid_dy, 1), MAX(t->grid_dx, 1),
	};

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

static int esdr_fpga_init(const struct device *dev)
{
	const struct esdr_fpga_config *cfg = dev->config;
	struct esdr_fpga_data *data = dev->data;

	k_mutex_init(&data->lock);
	data->bus_nocs = cfg->bus;
	data->bus_nocs.config.cs = (struct spi_cs_control){.cs_is_gpio = false};
	data->bus_read = cfg->bus;
	data->bus_read.config.frequency = cfg->read_frequency;
	if (!spi_is_ready_dt(&cfg->bus) || !device_is_ready(cfg->mfd)) {
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
	(void)esdr_fpga_load(dev, image, sizeof(image));
#endif
	return 0;
}

#define SPI_OP (SPI_OP_MODE_CONTROLLER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB)

#define ESDR_FPGA_DEFINE(inst)                                                                     \
	static struct esdr_fpga_data esdr_fpga_data_##inst;                                        \
	static const struct esdr_fpga_config esdr_fpga_config_##inst = {                           \
		.bus = SPI_DT_SPEC_INST_GET(inst, SPI_OP),                                         \
		.read_frequency = DT_INST_PROP(inst, read_frequency),                            \
		.mfd = DEVICE_DT_GET(DT_INST_PHANDLE(inst, mfd)),                                  \
		.irq = GPIO_DT_SPEC_INST_GET_OR(inst, irq_gpios, {0}),                             \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, esdr_fpga_init, NULL, &esdr_fpga_data_##inst,                  \
			      &esdr_fpga_config_##inst, POST_KERNEL,                               \
			      CONFIG_ESDR_FPGA_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(ESDR_FPGA_DEFINE)
