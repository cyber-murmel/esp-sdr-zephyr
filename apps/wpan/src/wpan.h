/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Radio control of the wpan app, shared by its shell commands (main.c) and the sniffer, so that
 * `wpan status` always shows the true state.
 */
#ifndef WPAN_H_
#define WPAN_H_

#include <stdbool.h>
#include <stdint.h>

/** Start receiving (api->start()); 0 or a negative errno. */
int wpan_radio_on(void);
/** Stop the radio. */
int wpan_radio_off(void);
bool wpan_radio_is_on(void);
/** Set the channel (the range is the caller's to check); restarts the radio if it is on. */
int wpan_set_channel(uint16_t ch);
uint16_t wpan_get_channel(void);
int wpan_set_promisc(bool on);

#endif /* WPAN_H_ */
