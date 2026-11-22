/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A kernel for hello's stage hello_op (apps/multiapp/ext/hello.c), INTERNAL: its function runs
 * from internal RAM (the module region heap), four times the value, self-contained.
 */

#include <esp_sdr/esp_sdr_place.h>

#include <multiapp/stage.h>

#include "hello_stage.h"

ESP_SDR_CODE(PACED_SAMPLE, hello_x4_op)
static int hello_x4_op(int v)
{
	return 4 * v;
}

MULTIAPP_KERNEL_DEFINE(hello_op, hello_op_t, hello_x4_op);
