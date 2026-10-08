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

#include "app_crash.h"
#include "app_usb.h"

LOG_MODULE_REGISTER(app_usb, LOG_LEVEL_INF);

#define REBOOT_DELAY K_MSEC(1000)

static struct k_work_delayable start_work;
static struct k_work_delayable reboot_work;

#if defined(CONFIG_APP_TRIAL_WATCHDOG) || defined(CONFIG_APP_LIVENESS_WATCHDOG)
static const struct device *const app_wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static struct k_work_delayable wdt_work;
static bool trial;

#define FEED_PERIOD K_MSEC(500)
#if defined(CONFIG_APP_TRIAL_TIMEOUT_S)
#define TRIAL_S CONFIG_APP_TRIAL_TIMEOUT_S
#else
#define TRIAL_S 0
#endif

/*
 * Liveness: fed from the system work queue, so a wedged system resets. Trial:
 * an unconfirmed image is no longer fed after CONFIG_APP_TRIAL_TIMEOUT_S, or
 * (without liveness) the watchdog is only disarmed once confirmed.
 */
static void wdt_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (trial && boot_is_img_confirmed()) {
		trial = false;
		LOG_INF("image confirmed");
		if (!IS_ENABLED(CONFIG_APP_LIVENESS_WATCHDOG)) {
			(void)wdt_disable(app_wdt);
			return;
		}
	}
	if (IS_ENABLED(CONFIG_APP_LIVENESS_WATCHDOG)) {
		/* The driver's tick scale halves the trial timeout; keep that. */
		if (!trial || k_uptime_get() < (int64_t)TRIAL_S * 1000 / 2) {
			(void)wdt_feed(app_wdt, 0);
		}
		k_work_schedule(&wdt_work, FEED_PERIOD);
	} else {
		k_work_schedule(&wdt_work, K_SECONDS(1));
	}
}

/*
 * Stage 0 of the ESP32 watchdog: the driver feeds it after this callback, so a
 * live system only resets if the callback does it. A hung one, which cannot
 * run the interrupt, gets the stage 1 hardware reset.
 */
static void wdt_expired(const struct device *dev, int channel_id)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(channel_id);
	if (!trial) {
		app_crash_note_watchdog();
	}
	sys_reboot(SYS_REBOOT_COLD);
}

/*
 * A DFU image boots unconfirmed. Unless the host confirms it in time, the
 * chip resets and MCUboot reverts to the previous image, so an update that
 * breaks USB never needs a replug. With the liveness watchdog a hang resets
 * any image.
 */
static void app_watchdog_arm(void)
{
	struct wdt_timeout_cfg cfg = {
		.callback = wdt_expired,
		.flags = WDT_FLAG_RESET_SOC,
	};

	trial = IS_ENABLED(CONFIG_APP_TRIAL_WATCHDOG) && IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT) &&
		!boot_is_img_confirmed();
	if (!trial && !IS_ENABLED(CONFIG_APP_LIVENESS_WATCHDOG)) {
		return;
	}
#if defined(CONFIG_APP_LIVENESS_WATCHDOG)
	cfg.window.max = CONFIG_APP_LIVENESS_TIMEOUT_MS;
#else
	cfg.window.max = TRIAL_S * MSEC_PER_SEC;
#endif
	if (!device_is_ready(app_wdt) || wdt_install_timeout(app_wdt, &cfg) < 0 ||
	    wdt_setup(app_wdt, 0) < 0) {
		LOG_ERR("watchdog unavailable");
		return;
	}
	if (trial) {
		LOG_WRN("trial boot: reverting in %d s unless confirmed (mcuboot confirm)",
			TRIAL_S);
	}
	k_work_init_delayable(&wdt_work, wdt_handler);
	k_work_schedule(&wdt_work, K_NO_WAIT);
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

__weak void app_usb_dfu_prepare(void)
{
}

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
		app_usb_dfu_prepare();
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

int app_usb_init(void)
{
#if defined(CONFIG_APP_TRIAL_WATCHDOG) || defined(CONFIG_APP_LIVENESS_WATCHDOG)
	app_watchdog_arm();
#endif
	k_work_init_delayable(&start_work, start_handler);
#if defined(CONFIG_APP_USB_DFU)
	k_work_init_delayable(&reboot_work, reboot_handler);
#endif
	LOG_INF("USB OTG starts in %d ms", CONFIG_APP_USB_START_DELAY_MS);
	k_work_schedule(&start_work, K_MSEC(CONFIG_APP_USB_START_DELAY_MS));
	return 0;
}
