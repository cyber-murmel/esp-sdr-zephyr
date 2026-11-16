/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The pcapng stream (doc/pcap.md, "Output"): one section per capture (SHB, the three interface
 * blocks of pcap_proto.h's PCAP_IF_*, timestamps in nanoseconds), a packet block per decoded
 * packet, statistics blocks every second, written into transfer buffers of internal RAM that go
 * out on the vendor interface's bulk IN as they fill (or after OUTPUT_FLUSH_MS). A block that
 * does not fit is dropped and counted against its interface, never waited for.
 */

#ifndef OUTPUT_H_
#define OUTPUT_H_

#include <stddef.h>
#include <stdint.h>

#include "pcap_proto.h"

/* A transfer buffer that holds something goes out after this long. */
#define OUTPUT_FLUSH_MS 50

/* A new section (SHB and the interface blocks); the counters start again. */
void output_start(void);
/* The section's header again, for a host that came during a capture. */
void output_section(void);
/* A packet on interface @p iface at host time @p t_ns, with an optional comment. */
int output_packet(uint32_t iface, uint64_t t_ns, const void *data, uint32_t len,
		  const char *comment);
/* Partly filled buffers that waited OUTPUT_FLUSH_MS, statistics blocks every second. */
void output_poll(void);
/* The last statistics blocks, everything out. */
void output_stop(void);
/* A transfer is done (usb_pcap.h). */
void output_in_done(uint8_t *data, size_t len, int err);

struct output_stats {
	uint32_t packets[PCAP_IF_COUNT], dropped[PCAP_IF_COUNT];
	uint64_t usb_bytes;
	uint32_t peak;
};

void output_get_stats(struct output_stats *st);

#endif /* OUTPUT_H_ */
