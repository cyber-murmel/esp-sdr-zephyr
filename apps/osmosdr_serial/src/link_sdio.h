/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The SDIO slave of the ESP32-C6 as the link to the host (link_sdio.c): the same byte stream
 * as on a UART (esdr_proto.h, serial transport), the host's writes in, the replies and
 * receive blocks out by DMA from the caller's buffers.
 */

#ifndef LINK_SDIO_H_
#define LINK_SDIO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pins, clock and DMA; the host may enumerate once it returns. */
void sdio_link_init(void);

/* A byte the host wrote is waiting; the next one. */
bool sdio_link_readable(void);
uint8_t sdio_link_getc(void);

/*
 * Queue @p n bytes at @p p (4-byte aligned, at most 16380) to send; @p block counts it in
 * sdio_link_blocks_done(). The buffer stays the DMA's until it is done. False when the queue
 * is full.
 */
bool sdio_link_queue(const uint8_t *p, size_t n, bool block);

/* Retire what the host has read and offer it what is queued. Cheap: call between samples. */
void sdio_link_pump(void);

/* Buffers queued and not yet read by the host. */
uint32_t sdio_link_pending(void);

/* Blocks (sdio_link_queue(..., true)) the host has read, a running count. */
uint32_t sdio_link_blocks_done(void);

#endif /* LINK_SDIO_H_ */
