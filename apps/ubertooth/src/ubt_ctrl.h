/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The vendor requests of the Ubertooth One (upstream vendor_request_handler(),
 * firmware/bluetooth_rxtx/bluetooth_rxtx.c), without USB or radio: requests
 * change the device state in struct ubt_dev, ask for a mode through
 * requested_mode and answer from that state. The mode thread does the rest.
 */

#ifndef UBT_CTRL_H_
#define UBT_CTRL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ubt_dev.h"

/* The setup packet of a vendor request to the device. */
struct ubt_req {
	uint8_t request;
	/* Data stage from device to host. */
	bool in;
	uint16_t value;
	uint16_t index;
	uint16_t length;
};

/* Highest request number the table covers (UBERTOOTH_XMAS). */
#define UBT_REQ_MAX UBERTOOTH_XMAS

/*
 * Whether @p request is one the device answers (host direction as the tools use it). The class
 * claims exactly these in its vendor request table.
 */
bool ubt_ctrl_known(uint8_t request);

/*
 * Run one request. @p data is the data stage: @p len received bytes for a request to the device,
 * a buffer of @p len (= wLength) bytes to fill for a request to the host.
 *
 * @return the number of reply bytes (0 for requests to the device), or a negative errno: the
 * request stalls (unknown, unsupported here, or malformed).
 */
int ubt_ctrl(struct ubt_dev *d, const struct ubt_req *r, uint8_t *data, size_t len);

#endif /* UBT_CTRL_H_ */
