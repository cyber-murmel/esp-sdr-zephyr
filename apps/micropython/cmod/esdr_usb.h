/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * USB on the WHY2025 badge's ESP32-P4 (esdr_usb.c): its USB-C port J8 carries the REPL and DFU
 * (apps/common app_usb.h), or, handed over for a while, the ESP32-C6's USB-Serial-JTAG.
 */

#ifndef ESDR_USB_H_
#define ESDR_USB_H_

#include <stdint.h>

/*
 * USB-C port J8 to the ESP32-C6's USB-Serial-JTAG for @p seconds (esptool, its console), then
 * back to this chip's USB device. 0, or -EBUSY while handed over already.
 */
int esdr_usb_to_c6(uint32_t seconds);

#endif /* ESDR_USB_H_ */
