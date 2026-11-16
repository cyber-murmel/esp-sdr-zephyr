/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Firmware update over the link (fw_update.c): ESDR_REQ_FW_BEGIN, _WRITE and _END.
 */

#ifndef FW_UPDATE_H_
#define FW_UPDATE_H_

#include <stddef.h>
#include <stdint.h>

/* Start an image of size bytes for the OTA partition the C6 does not run from. */
int fw_update_begin(uint32_t size);
/* The next len bytes (a multiple of 4) at offset, in order. */
int fw_update_write(uint32_t offset, const uint8_t *data, size_t len);
/* Check the image against crc (CRC-32/IEEE), select it and restart into it. */
int fw_update_end(uint32_t crc);

#endif /* FW_UPDATE_H_ */
