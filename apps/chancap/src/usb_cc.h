/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The capture stream's USB function: one vendor interface with a bulk IN endpoint carrying
 * VITA 49.2 packets in blocks (whole packets per block, so a host reads block by block).
 */

#ifndef USB_CC_H_
#define USB_CC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Whether the host has configured the interface (bulk transfers possible). */
bool usb_cc_enabled(void);

/*
 * Queue @p len bytes at @p data (internal RAM, untouched until done) on bulk IN. Completion
 * calls usb_cc_in_done() from the USB stack thread, in order.
 */
int usb_cc_submit_in(uint8_t *data, size_t len);
/* Cancel everything queued (completions report -ECONNABORTED). */
void usb_cc_cancel(void);

/* Provided by the app. */
void usb_cc_in_done(uint8_t *data, size_t len, int err);
/* Interface enabled (alternate setting 0 selected) or gone. */
void usb_cc_link(bool up);

#endif /* USB_CC_H_ */
