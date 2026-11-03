/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <esp_sdr/mch2022_coproc.h>

#include "reset_detect.h"

void reset_detect_init(struct reset_detect *d, uint8_t mode)
{
	*d = (struct reset_detect){.mode = mode};
}

void reset_detect_clear(struct reset_detect *d)
{
	d->download_state = 0;
	d->app_state = 0;
}

enum reset_action reset_detect_stock(struct reset_detect *d, bool dtr, bool rts, bool reset_active)
{
	const bool dtr2 = dtr || d->prev_dtr;
	const bool rts2 = rts || d->prev_rts;
	enum reset_action action = RESET_ACTION_NONE;

	if (!reset_active) {
		if (d->download_state == 0U && dtr2 && rts2) {
			d->download_state = 1;
		}
		if (d->download_state == 1U && dtr2 && !rts2) {
			d->download_state = 2;
		}
		if (d->download_state == 2U && !dtr2 && rts2) {
			d->download_state = 3;
		}
		if (d->download_state == 3U && dtr2 && rts2) {
			action = RESET_ACTION_DOWNLOAD;
		}
		if (d->app_state == 0U && !dtr2 && rts2) {
			d->app_state = 1;
		}
		if (d->app_state == 1U && !dtr2 && !rts2) {
			action = RESET_ACTION_APP;
		}
	}
	d->prev_dtr = dtr;
	d->prev_rts = rts;
	if (action != RESET_ACTION_NONE) {
		reset_detect_clear(d);
	}
	return action;
}

struct reset_lines reset_detect_circuit(bool dtr, bool rts)
{
	return (struct reset_lines){.en_low = rts && !dtr, .io0_low = dtr && !rts};
}
