/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * USB device on the OTG port with DFU into the MCUboot secondary slot.
 * OTG takes the PHY from USB-Serial-JTAG, so it starts only after a delay
 * that leaves the RTS/DTR flashing path usable right after reset.
 */

#include <sample_usbd.h>

#include <zephyr/dfu/mcuboot.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/usb/usbd.h>

#include "usb.h"

LOG_MODULE_REGISTER(rx_usb, LOG_LEVEL_INF);

#define REBOOT_DELAY K_MSEC(1000)

static struct k_work_delayable start_work;
static struct k_work_delayable reboot_work;

#if defined(CONFIG_APP_TRIAL_WATCHDOG)
static const struct device *const trial_wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static struct k_work_delayable trial_work;

/* Disarm once the image is confirmed, for example with "mcuboot confirm". */
static void trial_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!boot_is_img_confirmed()) {
		k_work_schedule(&trial_work, K_SECONDS(1));
		return;
	}
	if (wdt_disable(trial_wdt) == 0) {
		LOG_INF("image confirmed, trial watchdog off");
	}
}

/*
 * Stage 0 of the ESP32 watchdog: the driver feeds it after this callback, so a
 * live system only resets if the callback does it. A hung one, which cannot
 * run the interrupt, gets the stage 1 hardware reset.
 */
static void trial_expired(const struct device *dev, int channel_id)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(channel_id);
	sys_reboot(SYS_REBOOT_COLD);
}

/*
 * A DFU image boots unconfirmed. Unless the host confirms it in time, the
 * chip resets and MCUboot reverts to the previous image, so an update that
 * breaks USB never needs a replug.
 */
static void trial_watchdog_arm(void)
{
	struct wdt_timeout_cfg cfg = {
		.window.max = CONFIG_APP_TRIAL_TIMEOUT_S * MSEC_PER_SEC,
		.callback = trial_expired,
		.flags = WDT_FLAG_RESET_SOC,
	};

	if (!IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT) || boot_is_img_confirmed()) {
		return;
	}
	if (!device_is_ready(trial_wdt) || wdt_install_timeout(trial_wdt, &cfg) < 0 ||
	    wdt_setup(trial_wdt, 0) < 0) {
		LOG_ERR("trial watchdog unavailable");
		return;
	}
	LOG_WRN("trial boot: reverting in %d s unless confirmed (mcuboot confirm)",
		CONFIG_APP_TRIAL_TIMEOUT_S);
	k_work_init_delayable(&trial_work, trial_handler);
	k_work_schedule(&trial_work, K_SECONDS(1));
}
#endif

#if defined(CONFIG_APP_USB_DFU)
/* DFU mode is a separate device with only the DFU class, as in samples/subsys/usb/dfu. */
USBD_DEVICE_DEFINE(dfu_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), CONFIG_SAMPLE_USBD_VID,
		   CONFIG_APP_USB_DFU_PID);
USBD_DESC_LANG_DEFINE(dfu_lang);
USBD_DESC_PRODUCT_DEFINE(dfu_product, CONFIG_SAMPLE_USBD_PRODUCT " DFU");
USBD_DESC_CONFIG_DEFINE(dfu_cfg_desc, "DFU FS Configuration");
USBD_CONFIGURATION_DEFINE(dfu_fs_config, 0, CONFIG_SAMPLE_USBD_MAX_POWER, &dfu_cfg_desc);

static void msg_cb(struct usbd_context *const ctx, const struct usbd_msg *const msg);

static void switch_to_dfu_mode(struct usbd_context *const ctx)
{
	int err;

	LOG_INF("switching to DFU mode");
	usbd_disable(ctx);
	usbd_shutdown(ctx);

	err = usbd_add_descriptor(&dfu_usbd, &dfu_lang);
	if (err == 0) {
		err = usbd_add_descriptor(&dfu_usbd, &dfu_product);
	}
	if (err == 0) {
		err = usbd_add_configuration(&dfu_usbd, USBD_SPEED_FS, &dfu_fs_config);
	}
	if (err == 0) {
		err = usbd_register_class(&dfu_usbd, "dfu_dfu", USBD_SPEED_FS, 1);
	}
	if (err == 0) {
		usbd_device_set_code_triple(&dfu_usbd, USBD_SPEED_FS, 0, 0, 0);
		err = usbd_init(&dfu_usbd);
	}
	if (err == 0) {
		err = usbd_msg_register_cb(&dfu_usbd, msg_cb);
	}
	if (err == 0) {
		err = usbd_enable(&dfu_usbd);
	}
	if (err != 0) {
		LOG_ERR("DFU mode failed (%d)", err);
	}
}
#endif /* CONFIG_APP_USB_DFU */

static void msg_cb(struct usbd_context *const ctx, const struct usbd_msg *const msg)
{
	LOG_DBG("USBD message: %s", usbd_msg_type_string(msg->type));

	if (usbd_can_detect_vbus(ctx)) {
		if (msg->type == USBD_MSG_VBUS_READY && usbd_enable(ctx) != 0) {
			LOG_ERR("enable failed");
		}
		if (msg->type == USBD_MSG_VBUS_REMOVED && usbd_disable(ctx) != 0) {
			LOG_ERR("disable failed");
		}
	}

#if defined(CONFIG_APP_USB_DFU)
	if (msg->type == USBD_MSG_DFU_APP_DETACH) {
		switch_to_dfu_mode(ctx);
	}
	if (msg->type == USBD_MSG_DFU_DOWNLOAD_COMPLETED) {
		/* MCUboot test-boots the new image; the host confirms it over the shell. */
		int err = boot_request_upgrade(BOOT_UPGRADE_TEST);

		LOG_INF("update stored (%d), rebooting", err);
		k_work_schedule(&reboot_work, REBOOT_DELAY);
	}
#endif
}

#if defined(CONFIG_APP_USB_DFU)
static void reboot_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	sys_reboot(SYS_REBOOT_COLD);
}
#endif

static void start_handler(struct k_work *work)
{
	struct usbd_context *ctx;
	int err;

	ARG_UNUSED(work);

	ctx = sample_usbd_init_device(msg_cb);
	if (ctx == NULL) {
		LOG_ERR("USB device init failed");
		return;
	}
	if (!usbd_can_detect_vbus(ctx)) {
		err = usbd_enable(ctx);
		if (err != 0) {
			LOG_ERR("USB enable failed (%d)", err);
			return;
		}
	}
	LOG_INF("USB device enabled");
}

int usb_init(void)
{
#if defined(CONFIG_APP_TRIAL_WATCHDOG)
	trial_watchdog_arm();
#endif
	k_work_init_delayable(&start_work, start_handler);
#if defined(CONFIG_APP_USB_DFU)
	k_work_init_delayable(&reboot_work, reboot_handler);
#endif
	LOG_INF("USB OTG starts in %d ms", CONFIG_APP_USB_START_DELAY_MS);
	k_work_schedule(&start_work, K_MSEC(CONFIG_APP_USB_START_DELAY_MS));
	return 0;
}
