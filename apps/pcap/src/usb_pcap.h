/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The pcap stream's USB function: one vendor interface with a bulk IN endpoint carrying the
 * pcapng stream, and the vendor control requests (esdr_proto.h's, forwarded to the radio, and
 * pcap_proto.h's).
 */

#ifndef USB_PCAP_H_
#define USB_PCAP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Whether the host has configured the interface (bulk transfers possible). */
bool usb_pcap_enabled(void);

/*
 * Queue @p len bytes at @p data (internal RAM, untouched until done) on bulk IN. Completion
 * calls usb_pcap_in_done() from the USB stack thread, in order. 0 or a negative errno.
 */
int usb_pcap_submit(uint8_t *data, size_t len);

/* Provided by the app. */
void usb_pcap_in_done(uint8_t *data, size_t len, int err);
/* Interface enabled or gone (the host claimed it, or left). */
void usb_pcap_link(bool up);
/*
 * Vendor requests: to the device with @p len bytes of data (0 for none), or to the host
 * filling at most @p max bytes (return the length). Called from the USB stack thread; a
 * negative errno stalls.
 */
int usb_pcap_control_out(uint8_t req, uint16_t value, const uint8_t *data, size_t len);
int usb_pcap_control_in(uint8_t req, uint16_t value, uint8_t *buf, size_t max);

#endif /* USB_PCAP_H_ */
