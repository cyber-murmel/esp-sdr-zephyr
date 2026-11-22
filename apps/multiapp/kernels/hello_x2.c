/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A kernel for hello's stage hello_op (apps/multiapp/ext/hello.c), in PSRAM: twice the value and
 * hello's bias, which it imports from the app (an app global is allowed, exports.py checks).
 * "multiapp stage load hello_op hello_x2", or kswap.py.
 */

#include <esp_sdr/esp_sdr_place.h>

#include <multiapp/stage.h>

#include "hello_stage.h"

ESP_SDR_CODE(PACED_SAMPLE, hello_x2_op)
static int hello_x2_op(int v)
{
	return 2 * v + hello_stage_bias;
}

MULTIAPP_KERNEL_DEFINE(hello_op, hello_op_t, hello_x2_op);
