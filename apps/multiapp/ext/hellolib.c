/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A module in PSRAM that needs another one, hellofast: a call and a data
 * reference across modules. It has a device, which only the host can initialize.
 */

#include <zephyr/device.h>

#include "hellomod.h"

static int dev_inits;

static int hellolib_dev_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	dev_inits++;
	return 0;
}

DEVICE_DEFINE(hellolib, "hellolib", hellolib_dev_init, NULL, NULL, NULL, POST_KERNEL,
	      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, NULL);

int hellolib_dev_inits(void)
{
	return dev_inits;
}

int hellolib_chain(int v)
{
	return hellofast_twice(v) + hellofast_value;
}

uint32_t hellolib_loop(uint32_t rounds)
{
	return hellomod_loop(rounds);
}
