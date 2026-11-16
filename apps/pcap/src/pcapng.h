/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A pcapng writer (https://ietf-opsawg-wg.github.io/draft-ietf-opsawg-pcap/draft-ietf-opsawg-pcapng.html):
 * one block at a time into a buffer the caller owns, little endian, 32-bit aligned, every
 * option list ended with opt_endofopt. Plain C, no Zephyr: unit tested on native_sim and built
 * by the host tests with the host compiler.
 */

#ifndef PCAPNG_H_
#define PCAPNG_H_

#include <stddef.h>
#include <stdint.h>

/* Block types. */
#define PCAPNG_BT_SHB 0x0A0D0D0AU
#define PCAPNG_BT_IDB 0x00000001U
#define PCAPNG_BT_EPB 0x00000006U
#define PCAPNG_BT_ISB 0x00000005U
#define PCAPNG_BYTE_ORDER_MAGIC 0x1A2B3C4DU

/* Option codes. */
#define PCAPNG_OPT_ENDOFOPT 0U
#define PCAPNG_OPT_COMMENT  1U
#define PCAPNG_SHB_HARDWARE 2U
#define PCAPNG_SHB_OS       3U
#define PCAPNG_SHB_USERAPPL 4U
#define PCAPNG_IF_NAME        2U
#define PCAPNG_IF_DESCRIPTION 3U
#define PCAPNG_IF_SPEED       8U
#define PCAPNG_IF_TSRESOL     9U
#define PCAPNG_IF_HARDWARE    15U
#define PCAPNG_EPB_FLAGS      2U
#define PCAPNG_EPB_DROPCOUNT  4U
#define PCAPNG_ISB_STARTTIME  2U
#define PCAPNG_ISB_ENDTIME    3U
#define PCAPNG_ISB_IFRECV     4U
#define PCAPNG_ISB_IFDROP     5U

/* epb_flags: inbound. */
#define PCAPNG_EPB_FLAG_INBOUND 0x00000001U

struct pcapng_idb {
	uint16_t linktype;
	uint32_t snaplen;
	/* NULL for none. */
	const char *name, *description, *hardware;
	/* if_tsresol: 9 for nanoseconds, 6 microseconds (0: none, the reader assumes 6). */
	uint8_t tsresol;
	/* Bits a second, 0 for none. */
	uint64_t speed_bps;
};

struct pcapng_epb {
	uint32_t interface;
	/* In the interface's resolution. */
	uint64_t timestamp;
	const void *data;
	uint32_t len;
	/* NULL for none. */
	const char *comment;
	/* 0 for no epb_flags option. */
	uint32_t flags;
	/* Packets lost since the previous one of the interface; UINT64_MAX for no option. */
	uint64_t dropcount;
};

struct pcapng_isb {
	uint32_t interface;
	uint64_t timestamp;
	/* UINT64_MAX leaves the option out. */
	uint64_t starttime, ifrecv, ifdrop;
};

/*
 * Each writes one block into @p buf (@p cap bytes) and returns its length, or -ENOSPC when it
 * does not fit (nothing written then), -EINVAL for a bad argument.
 */
int pcapng_shb(uint8_t *buf, size_t cap, const char *hardware, const char *os,
	       const char *userappl);
int pcapng_idb(uint8_t *buf, size_t cap, const struct pcapng_idb *idb);
int pcapng_epb(uint8_t *buf, size_t cap, const struct pcapng_epb *epb);
int pcapng_isb(uint8_t *buf, size_t cap, const struct pcapng_isb *isb);

/* The length an EPB of @p len data bytes and a comment of @p comment_len bytes will take. */
size_t pcapng_epb_size(uint32_t len, size_t comment_len, int with_flags, int with_dropcount);

#endif /* PCAPNG_H_ */
