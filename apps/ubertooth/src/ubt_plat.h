/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The device instance and its platform hooks (LED, serial number, reset).
 */

#ifndef UBT_PLAT_H_
#define UBT_PLAT_H_

#include "ubt_dev.h"

/* The one device. */
extern struct ubt_dev ubt;

/* Sets up the state, the USB function and the mode thread. Before app_usb_init(). */
int ubt_init(void);

#endif /* UBT_PLAT_H_ */
