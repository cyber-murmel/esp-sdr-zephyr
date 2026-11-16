/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A module placed as a standalone image (multiapp_module(... INTERNAL)): IRAM_ATTR code at the
 * instruction bus alias of its data bus address, data and a constructor.
 */

#include <esp_attr.h>

#include "hellomod.h"

int hellofast_value = 20;
static int constructed;

static void __attribute__((constructor)) hellofast_init(void)
{
	constructed = 1;
	hellofast_value++;
}

IRAM_ATTR int hellofast_twice(int v)
{
	return constructed != 0 ? 2 * v : -1;
}

IRAM_ATTR uint32_t hellofast_loop(uint32_t rounds)
{
	return hellomod_loop(rounds);
}
