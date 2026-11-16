/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The protocol's crystal calibration requests (ESDR_REQ_GET_CAL, ESDR_REQ_SET_CAL) on the
 * library's correction (esp_sdr_cal.h), for an app with the radio on its chip.
 */

#ifndef APP_ESDR_CAL_H_
#define APP_ESDR_CAL_H_

#include <stddef.h>
#include <stdint.h>

/* ESDR_REQ_GET_CAL: struct esdr_cal into buf; its size (at most max). */
int esdr_cal_get(uint8_t *buf, size_t max);

/*
 * ESDR_REQ_SET_CAL with wValue op and its data: 0 once applied (what storing it gave is
 * struct esdr_cal last_store), -EINVAL for a bad request or a correction out of range.
 * It retunes: call it while the radio is idle.
 */
int esdr_cal_set(uint16_t op, const uint8_t *data, size_t len);

#endif /* APP_ESDR_CAL_H_ */
