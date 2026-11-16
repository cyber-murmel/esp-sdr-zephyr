/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * In front of every source of the micropython app, after include/multiapp/ext.h.
 */

#ifndef MULTIAPP_MP_PRELUDE_H_
#define MULTIAPP_MP_PRELUDE_H_

#include <esp_intr_alloc.h>
#include <zephyr/usb/class/usbd_msc.h>

/* A LUN is in a host section, so the host has them (src/drive.c); src/usb_disks.c keeps its disks. */
#undef USBD_DEFINE_MSC_LUN
#define USBD_DEFINE_MSC_LUN(...)

/*
 * The drivers' interrupt handlers run from PSRAM: an interrupt line asking to work with the cache off
 * would be refused (intc_esp32.c). Without the flag, it stays off during flash operations.
 */
#undef ESP_INTR_FLAG_IRAM
#define ESP_INTR_FLAG_IRAM 0

#endif /* MULTIAPP_MP_PRELUDE_H_ */
