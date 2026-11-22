/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The phy module leaves with its last client (src/apps.c unloads it only when no module of the
 * next boot needs it): nothing of it may hold the radio then.
 */

#include <esp_sdr/esp_sdr_arb.h>
#include <multiapp/multiapp.h>

/* -EBUSY keeps the module loaded: a client of the arbiter is left. */
int multiapp_module_down(void)
{
	return esp_sdr_arb_shutdown();
}
