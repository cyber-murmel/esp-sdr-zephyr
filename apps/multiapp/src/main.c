/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * multiapp host (doc/apps/multiapp-internals.md): the USB shell and DFU, the radio, and the
 * apps built as extensions (apps.c).
 */

#include <zephyr/app_version.h>
#include <zephyr/kernel.h>

#include "app_cpu.h"
#include "app_usb.h"
#include "apps.h"

int main(void)
{
	printk("multiapp %s\n", APP_VERSION_STRING);
	/*
	 * Before USB and the watchdog, whose interrupts land on the CPU that allocates them: CPU 1 is
	 * the radio's, whose ring masks interrupts. This thread could otherwise come back on it from
	 * anything that makes it wait, such as stopping the shell (src/apps.c).
	 */
	app_cpu_pin_system_threads(0);
	app_cpu_pin_self(0);
	/*
	 * The files while bank 0 and the flash are the host's alone, then the app and the
	 * modules it needs, the radio among them, before USB, whose classes call its
	 * handlers (osmosdr's).
	 */
	(void)multiapp_drive_scan();
	if (!multiapp_boot()) {
		/* No app: the volume goes to the PC as a USB drive. */
		(void)multiapp_drive_export();
	}
	/* The USB shell and DFU, and the trial watchdog that reverts an image that hangs. */
	if (app_usb_init() != 0) {
		printk("multiapp: usb init failed\n");
	}
	multiapp_usb_up();
	multiapp_note_main_stack();
	return 0;
}
