/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See ubt_dev.h.
 */

#include "ubt_dev.h"

void ubt_dev_reset(struct ubt_dev *d)
{
	d->hop_mode = HOP_NONE;
	d->do_hop = 0;
	d->channel = 2441;
	d->requested_channel = 0;
	d->le_adv_channel = 2402;
	d->mode = MODE_IDLE;
	d->requested_mode = MODE_IDLE;
	d->jam_mode = JAM_NONE;
	d->ego_mode = 0;
	d->modulation = MOD_BT_BASIC_RATE;
	d->low_freq = 2400;
	d->high_freq = 2483;
	d->rssi_threshold = -30;
	d->cs_threshold_req = -120;
	d->target.address = 0;
	d->target.syncword = 0;
	d->cfg_gen++;
}
