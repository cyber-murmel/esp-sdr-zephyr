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

#if defined(CONFIG_SOC_SERIES_ESP32S31)
/*
 * The threads of esdr: below the SSH server (10), the network and USB, and ahead of MicroPython's own
 * thread (13, ext/mp/mp_repl_s31.c). A standalone build runs them ahead of everything but the
 * radio (cooperative 4).
 */
#define ESDR_WORKER_PRIO 12
#define ESDR_SCAN_PRIO   12

/*
 * MicroPython's thread has its stack in PSRAM (ext/mp/mp_repl_s31.c), so nothing it calls may touch the flash
 * (the cache is off during a flash operation): the host's shell commands run on a worker of internal stack.
 */
int multiapp_mp_shell_exec(const char *cmd, char *buf, size_t cap, size_t *len);
void multiapp_mp_stacks(size_t v[6]);
int multiapp_mp_flash_call(int (*fn)(void *arg), void *arg);
#define ESDR_SHELL_RUN(cmd, buf, cap, lenp) multiapp_mp_shell_exec(cmd, buf, cap, lenp)
#define ESDR_STACKS    multiapp_mp_stacks
#define ESDR_FLASH_CALL multiapp_mp_flash_call
#endif

#endif /* MULTIAPP_MP_PRELUDE_H_ */
