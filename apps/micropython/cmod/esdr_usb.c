/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * USB on a P4 (esdr_usb.h): the REPL and DFU on its OTG port. On the WHY2025 badge, a
 * multiplexer (zephyr,user usb-mux-gpios, GPIO2) selects what USB-C port J8 reaches: this
 * chip's OTG controller (inactive) or the ESP32-C6's USB-Serial-JTAG (active, also where the
 * multiplexer rests while nothing drives it). The ESP32-C6's enable (zephyr,user
 * c6-enable-gpios), where the board has one, is driven active at boot. Plain Zephyr code,
 * built as an app source.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include "app_usb.h"
#include "esdr_usb.h"

#define USER_NODE DT_PATH(zephyr_user)

static bool started;

#if ESDR_USB_TO_C6
/* The host sees the device leave before the port switches. */
#define LEAVE_MS 200

static const struct gpio_dt_spec mux = GPIO_DT_SPEC_GET(USER_NODE, usb_mux_gpios);
static atomic_t handed_over;

static void back_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	(void)gpio_pin_set_dt(&mux, 0);
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
	(void)gpio_pin_set_dt(&mux, 1);
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
#endif /* ESDR_USB_TO_C6 */

/* The port's main() calls this at every soft reset; the USB device starts once. */
int mp_usbd_init(void)
{
	if (started) {
		return 0;
	}
	started = true;
	return app_usb_init();
}

/* The port to this chip and the C6 running, before USB starts. */
static int esdr_usb_pins_init(void)
{
	int ret = 0;

#if ESDR_USB_TO_C6
	if (!gpio_is_ready_dt(&mux)) {
		return -ENODEV;
	}
	ret = gpio_pin_configure_dt(&mux, GPIO_OUTPUT_INACTIVE);
#endif
#if DT_NODE_HAS_PROP(USER_NODE, c6_enable_gpios)
	const struct gpio_dt_spec c6_en = GPIO_DT_SPEC_GET(USER_NODE, c6_enable_gpios);

	if (ret == 0) {
		ret = gpio_is_ready_dt(&c6_en) ? gpio_pin_configure_dt(&c6_en, GPIO_OUTPUT_ACTIVE)
					       : -ENODEV;
	}
#endif
	return ret;
}
SYS_INIT(esdr_usb_pins_init, APPLICATION, 0);
