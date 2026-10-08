/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef APP_USB_H_
#define APP_USB_H_

/*
 * Schedule the USB device start (CDC-ACM shell, the app's classes, DFU) and
 * arm the trial watchdog on an unconfirmed image; returns at once.
 */
int app_usb_init(void);

/*
 * Called in the USB thread when the host detaches into DFU, before the
 * download: stop whatever keeps a CPU from taking flash stall requests
 * (an esp_sdr ring run). Weak, empty by default.
 */
void app_usb_dfu_prepare(void);

#endif /* APP_USB_H_ */
