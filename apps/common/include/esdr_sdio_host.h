/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The WHY2025 badge's ESP32-C6 radio (apps/osmosdr_serial with CONFIG_APP_LINK_SDIO) seen from
 * its SDIO host, the ESP32-P4: the C6's SLC protocol on function 1, as esp_hosted_mcu_sdio.c
 * uses it. The radio offers a byte stream (reply frames and receive blocks, esdr_proto.h) and
 * takes frames into its receive buffers, counted by tokens. One radio per host; the calls are
 * thread safe. A bus error in esdr_sdio_read() or esdr_sdio_send() enumerates the radio again
 * (at most every 100 ms) and starts a new session: a radio that restarted comes back, its byte
 * stream from the start, what was in flight lost.
 */

#ifndef ESDR_SDIO_HOST_H_
#define ESDR_SDIO_HOST_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>

/*
 * Enumerates the radio on @p sdhc (retrying for @p ready_ms while its image arms the slave,
 * with an I/O reset for one this host enumerated before it restarted) and starts a session:
 * the radio drops anything queued for an earlier one, both byte counts start from zero.
 * 0 or a negative errno.
 */
int esdr_sdio_open(const struct device *sdhc, uint32_t ready_ms);

/*
 * Up to @p max bytes (a multiple of 4) of what the radio offers into @p buf (DMA-capable,
 * internal RAM). Returns the count, 0 when it offers nothing, or a negative errno.
 */
int esdr_sdio_read(uint8_t *buf, uint32_t max);

/*
 * One frame, @p a then @p b (@p nb may be 0), zero padded to whole words, as one write once
 * the radio has buffers for it; at most a request frame with a block of data. 0, -EMSGSIZE,
 * -ETIMEDOUT after @p timeout_ms, or the bus's errno.
 */
int esdr_sdio_send(const void *a, size_t na, const void *b, size_t nb, uint32_t timeout_ms);

#endif /* ESDR_SDIO_HOST_H_ */
