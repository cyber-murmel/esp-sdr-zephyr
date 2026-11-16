/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * apps/micropython's configuration, and the PSRAM a boot leaves free as the bulk of the heap
 * (mp_glue.c): CONFIG_APP_MP_PSRAM_HEAP_KB is a token here.
 */

#include "mpconfigport_esdr.h"

/* After the port's own config, which includes autoconf.h again. */
/* The host's serial shell is gone by the time this runs: no zephyr.shell_exec() on it. */
#undef CONFIG_SHELL_BACKEND_SERIAL
/* The host has these for itself, without the array zephyr.sys_memory_stats() reads. */
#undef CONFIG_SYS_HEAP_RUNTIME_STATS

void multiapp_mp_heap_add(void);
#undef MICROPY_PORT_INIT_FUNC
#define MICROPY_PORT_INIT_FUNC                                                                    \
	do {                                                                                       \
		esdr_port_init();                                                                  \
		multiapp_mp_heap_add();                                                            \
		MP_STATE_MEM(gc_alloc_threshold) = (size_t)-1;                                     \
	} while (0)
