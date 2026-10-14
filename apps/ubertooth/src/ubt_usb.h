/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The USB function of the Ubertooth One: a vendor interface (number 0, as
 * libubertooth claims it) with bulk IN 0x82 for the packet stream, the
 * vendor requests of ubt_ctrl.h on the control endpoint, and the CDC-ACM
 * shell and DFU behind it. Packets reach the host from the device queue:
 * the radio threads commit to it and call ubt_usb_kick().
 */

#ifndef UBT_USB_H_
#define UBT_USB_H_

#include <stdbool.h>

#include "ubt_dev.h"

/* Before app_usb_init(): the class needs the device state to answer requests. */
void ubt_usb_init(struct ubt_dev *dev);

/* Whether the host has configured the interface. */
bool ubt_usb_enabled(void);

/* The queue has new packets: send them. Any thread. */
void ubt_usb_kick(void);

#endif /* UBT_USB_H_ */
