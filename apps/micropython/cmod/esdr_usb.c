/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * USB on the WHY2025 badge's ESP32-P4 (esdr_usb.h). GPIO2 selects what USB-C port J8 reaches:
 * this chip's OTG controller (low) or the ESP32-C6's USB-Serial-JTAG (high, also where the
 * multiplexer rests while nothing drives it). Plain Zephyr code, built as an app source.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include "app_usb.h"
#include "esdr_usb.h"

#define USB_MUX_PIN 2
/* The host sees the device leave before the port switches. */
#define LEAVE_MS 200

static const struct device *const gpio = DEVICE_DT_GET(DT_NODELABEL(gpio0));
static bool started;
static atomic_t handed_over;

static void back_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	(void)gpio_pin_set_raw(gpio, USB_MUX_PIN, 0);
	app_usb_attach();
	atomic_clear(&handed_over);
}
static K_WORK_DELAYABLE_DEFINE(back_work, back_handler);

static void away_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(away_work, away_handler);
static uint32_t away_s;

static void away_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	(void)gpio_pin_set_raw(gpio, USB_MUX_PIN, 1);
	k_work_schedule(&back_work, K_SECONDS(away_s));
}

int esdr_usb_to_c6(uint32_t seconds)
{
	if (!atomic_cas(&handed_over, 0, 1)) {
		return -EBUSY;
	}
	away_s = seconds;
	app_usb_detach();
	k_work_schedule(&away_work, K_MSEC(LEAVE_MS));
	return 0;
}

/* The port's main() calls this at every soft reset; the USB device starts once. */
int mp_usbd_init(void)
{
	if (started) {
		return 0;
	}
	started = true;
	return app_usb_init();
}

/* J8 to this chip before USB starts. */
static int esdr_usb_mux_init(void)
{
	if (!device_is_ready(gpio)) {
		return -ENODEV;
	}
	return gpio_pin_configure(gpio, USB_MUX_PIN, GPIO_OUTPUT_LOW);
}
SYS_INIT(esdr_usb_mux_init, APPLICATION, 0);

