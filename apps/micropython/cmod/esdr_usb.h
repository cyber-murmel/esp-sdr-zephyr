/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * USB on a P4 (esdr_usb.c): its OTG port carries the REPL and DFU (apps/common app_usb.h). On
 * the WHY2025 badge, USB-C port J8 can be handed to the ESP32-C6's USB-Serial-JTAG for a while.
 */

#ifndef ESDR_USB_H_
#define ESDR_USB_H_

#include <stdint.h>

#include <zephyr/devicetree.h>

/* A multiplexer hands the P4's USB port to the C6 (the WHY2025 badge's J8). */
#define ESDR_USB_TO_C6 DT_NODE_HAS_PROP(DT_PATH(zephyr_user), usb_mux_gpios)

/*
 * USB-C port J8 to the ESP32-C6's USB-Serial-JTAG for @p seconds (esptool, its console), then
 * back to this chip's USB device. 0, or -EBUSY while handed over already.
 */
int esdr_usb_to_c6(uint32_t seconds);

#endif /* ESDR_USB_H_ */
