/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Tells the host where a module's init entries are (ext/module_devices.ld). The script's own
 * symbols have no type, and llext exports only functions and objects.
 */

#include <zephyr/init.h>

#include <multiapp/multiapp.h>

extern const struct init_entry multiapp_init_start[];
extern const struct init_entry multiapp_init_end[];

const struct multiapp_init_table multiapp_init_table = {
	.start = multiapp_init_start,
	.end = multiapp_init_end,
};
