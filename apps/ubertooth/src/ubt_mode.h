/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The mode thread: upstream's main() loop. It waits for requested_mode to
 * differ from mode and runs the mode's function, which returns when another
 * mode is requested (the way upstream's mode loops watch requested_mode).
 */

#ifndef UBT_MODE_H_
#define UBT_MODE_H_

#include <stdint.h>

#include "ubt_dev.h"

struct ubt_mode_ops {
	uint8_t mode;
	/* Runs until d->requested_mode != d->mode; leaves the radio idle. */
	void (*run)(struct ubt_dev *d);
};

/* Starts the thread; the modes of the table in ubt_mode.c become supported_modes. */
void ubt_mode_init(struct ubt_dev *d);

/* A request changed the device state. Any thread. */
void ubt_mode_kick(void);

#endif /* UBT_MODE_H_ */
