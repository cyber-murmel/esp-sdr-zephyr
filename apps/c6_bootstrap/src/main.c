/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The ESP32-P4 installs an ESP32-C6 image through the C6's esp-hosted-mcu firmware, as the
 * EV board's C6 ships: the image (built in, C6_IMAGE) goes to the C6's next OTA partition by
 * the firmware's OTA requests over SDIO. Firmware before v2.6.0 selects the new partition and
 * restarts on its own after the end request, later firmware on the activate request. From then
 * on the C6 runs the image (apps/osmosdr_serial with CONFIG_APP_LINK_SDIO), and updates go
 * over its own SDIO link.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <esp_hosted_mcu.h>

LOG_MODULE_REGISTER(c6_bootstrap, LOG_LEVEL_INF);

#if defined(C6_IMAGE_BUILT_IN)
static const uint8_t image[] = {
#include "c6_image.inc"
};
#else
static const uint8_t image[1];
#endif

/* esp_image_header_t: magic at 0, chip id (13 = ESP32-C6) at 12; the descriptor at 0x20 */
#define IMAGE_MAGIC 0xe9U
#define IMAGE_CHIP_ID_OFFSET 12U
#define IMAGE_CHIP_ID_ESP32C6 13U
#define APP_DESC_OFFSET 0x20U
#define APP_DESC_MAGIC 0xabcd5432U

#define FW_VERSION_ACTIVATE ESP_HOSTED_MCU_FW_VERSION_VAL(2, 6, 0)

/*
 * The C6's console, where the board wires it to this chip (zephyr,user c6-console-uarts: a
 * receiver per candidate pin): logged a line at a time, to see the C6 restart into the image.
 */
#define USER_NODE DT_PATH(zephyr_user)

#if DT_NODE_HAS_PROP(USER_NODE, c6_console_uarts)
#define CONSOLE_UART(node, prop, idx) DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node, prop, idx)),
static const struct device *const console[] = {
	DT_FOREACH_PROP_ELEM(USER_NODE, c6_console_uarts, CONSOLE_UART)};

static void console_fn(void *p1, void *p2, void *p3)
{
	static char line[ARRAY_SIZE(console)][128];
	static size_t len[ARRAY_SIZE(console)];

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	for (;;) {
		bool idle = true;

		for (size_t i = 0; i < ARRAY_SIZE(console); i++) {
			unsigned char c;

			while (uart_poll_in(console[i], &c) == 0) {
				idle = false;
				if (c == '\n' || len[i] == sizeof(line[i]) - 1U) {
					line[i][len[i]] = '\0';
					LOG_INF("C6 (%s): %s", console[i]->name, line[i]);
					len[i] = 0;
				} else if (c != '\r') {
					line[i][len[i]++] = (char)c;
				}
			}
		}
		if (idle) {
			k_msleep(1);
		}
	}
}

K_THREAD_DEFINE(console_thread, 2048, console_fn, NULL, NULL, NULL, 5, 0, 0);
#endif

static int check_image(void)
{
	if (sizeof(image) < APP_DESC_OFFSET + sizeof(uint32_t) || image[0] != IMAGE_MAGIC) {
		LOG_ERR("not an ESP image");
		return -EINVAL;
	}
	if (sys_get_le16(&image[IMAGE_CHIP_ID_OFFSET]) != IMAGE_CHIP_ID_ESP32C6) {
		LOG_ERR("image is not for the ESP32-C6 (chip id %u)",
			sys_get_le16(&image[IMAGE_CHIP_ID_OFFSET]));
		return -EINVAL;
	}
	if (sys_get_le32(&image[APP_DESC_OFFSET]) != APP_DESC_MAGIC) {
		LOG_ERR("image has no application descriptor (CONFIG_ESP_IDF_BOOTLOADER)");
		return -EINVAL;
	}
	return 0;
}

int main(void)
{
	const struct device *hosted = DEVICE_DT_GET(DT_NODELABEL(esp_hosted_mcu));
	uint32_t version = esp_hosted_mcu_fw_version();
	size_t done = 0;
	int ret;

	if (!device_is_ready(hosted)) {
		LOG_ERR("esp-hosted-mcu link is down (is the C6 running esp-hosted-mcu?)");
		return 0;
	}

	LOG_INF("C6 firmware v%u.%u.%u", ESP_HOSTED_MCU_FW_VERSION_MAJOR(version),
		ESP_HOSTED_MCU_FW_VERSION_MINOR(version), ESP_HOSTED_MCU_FW_VERSION_PATCH(version));

	if (!IS_ENABLED(C6_IMAGE_BUILT_IN)) {
		LOG_INF("no image built in (C6_IMAGE)");
		return 0;
	}
	if (check_image() != 0) {
		return 0;
	}

	LOG_INF("installing %zu bytes", sizeof(image));

	ret = esp_hosted_mcu_ota_begin();
	if (ret != 0) {
		LOG_ERR("OTA begin: %d", ret);
		return 0;
	}

	while (done < sizeof(image)) {
		size_t len = MIN(sizeof(image) - done, ESP_HOSTED_MCU_OTA_CHUNK_SIZE);

		ret = esp_hosted_mcu_ota_write(&image[done], len);
		if (ret != 0) {
			LOG_ERR("OTA write at %zu: %d", done, ret);
			return 0;
		}
		done += len;
		if ((done / ESP_HOSTED_MCU_OTA_CHUNK_SIZE) % 64 == 0 || done == sizeof(image)) {
			LOG_INF("%zu/%zu", done, sizeof(image));
		}
	}

	ret = esp_hosted_mcu_ota_end();
	if (ret != 0) {
		LOG_ERR("OTA end: %d (image rejected by the C6)", ret);
		return 0;
	}

	if (version >= FW_VERSION_ACTIVATE) {
		ret = esp_hosted_mcu_ota_activate();
		if (ret != 0) {
			LOG_ERR("OTA activate: %d", ret);
			return 0;
		}
	}

	LOG_INF("installed; the C6 restarts into it within seconds");
	return 0;
}
