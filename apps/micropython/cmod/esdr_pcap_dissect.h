/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The Pcap app's table fields of a packet (eggs/pcap/pcap_view.py's dissector, Pkt._dissect()),
 * in C: the same text for the same record, some 30 times faster on the board. Plain C, built
 * with the host compiler by tests/host/test_pcap_egg.py, which holds the two to each other.
 */

#ifndef ESDR_PCAP_DISSECT_H_
#define ESDR_PCAP_DISSECT_H_

#include <stddef.h>
#include <stdint.h>

/* A field's text: len bytes (UTF-8, NUL bytes possible in a device's name), not terminated. */
#define ESDR_PCAP_FIELD_MAX 320U

struct esdr_pcap_text {
	uint16_t len;
	char s[ESDR_PCAP_FIELD_MAX];
};

struct esdr_pcap_fields {
	struct esdr_pcap_text channel, phy, kind, src, dst, info;
	uint32_t length;
	/* The record's own CRC verdict (a Bluetooth LE header that has one): 0 or 1; -1 none. */
	int8_t ok;
	uint8_t random;
};

/* The fields of a record of interface @p iface (PCAP_IF_*), as the Python dissector sets them:
 * a record too short for what it says comes out with kind "malformed" and what was read before. */
void esdr_pcap_dissect(uint32_t iface, const uint8_t *rec, size_t len,
		       struct esdr_pcap_fields *f);

#endif /* ESDR_PCAP_DISSECT_H_ */
