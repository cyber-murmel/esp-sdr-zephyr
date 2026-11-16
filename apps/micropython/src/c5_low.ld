/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ESP32-C5: MicroPython's static heap (the port's main.c) in SRAM block 0, which the capture
 * bank's snippet (lib/esp_sdr/esp_sdr_bank_c5.ld, sorted after this one) leaves partly empty;
 * block 2 keeps the stacks and the libc heap, part of which esdr_port_init() adds as well.
 */

.app_mp_low (NOLOAD) :
{
	. = ALIGN(16);
	*libmicropython.a:main.c.obj(.noinit .noinit.*)
} GROUP_NOLOAD_LINK_IN(RAMABLE_REGION, RAMABLE_REGION)
