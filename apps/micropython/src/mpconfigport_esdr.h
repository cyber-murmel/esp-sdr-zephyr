/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The port's configuration (modules/micropython/ports/zephyr/mpconfigport.h) plus what
 * the ESP32-S3 and this app need.
 */

#include "mpconfigport.h"

/*
 * The Xtensa windowed ABI: the port has no assembler non-local return for it, but this
 * setjmp spills the register windows to the stack (setjmp.S; as ports/esp32 relies on), which is
 * also what the GC needs to see the live registers on the stack.
 */
#define MICROPY_NLR_SETJMP                  (1)
#define MICROPY_GCREGS_SETJMP               (1)

#define MICROPY_PY_BUILTINS_INPUT           (1)

/* The port's 512 bytes left too little C stack below the limit for printing the traceback of a
 * deep recursion (it overflowed the ESP32-C5's main stack). */
#undef MICROPY_STACK_CHECK_MARGIN
#define MICROPY_STACK_CHECK_MARGIN          (1024)

/* GRC exports carry complex constants (offsets, multiply_const_cc, vector_source_c data). */
#undef MICROPY_PY_BUILTINS_COMPLEX
#define MICROPY_PY_BUILTINS_COMPLEX         (1)

/*
 * The Python heap: CONFIG_MICROPY_HEAP_SIZE of internal RAM, which the receive ring leaves
 * little of, plus CONFIG_APP_MP_PSRAM_HEAP_KB of PSRAM that esdr_port_init() adds at every
 * start (cmod/modesdr.c). esdr_port_deinit() stops the radio before a soft reset frees it.
 */
#define MICROPY_GC_SPLIT_HEAP               (1)
void esdr_port_init(void);
void esdr_port_deinit(void);
#define MICROPY_PORT_INIT_FUNC              esdr_port_init()
#define MICROPY_PORT_DEINIT_FUNC            esdr_port_deinit()

/* The network stack is only there for the Wi-Fi driver, which brings the radio up (the port
 * tests the socket module with #ifdef). */
#undef MICROPY_PY_SOCKET

/*
 * The Wi-Fi driver selects THREAD_CUSTOM_DATA, which turns the port's _thread on: 20 KiB of
 * stacks the DRAM image has no room for. hashlib's sha256 collides with the Wi-Fi
 * supplicant's (sha256_init).
 */
#undef MICROPY_PY_THREAD
#undef MICROPY_PY_THREAD_GIL
#undef MICROPY_PY_THREAD_GIL_VM_DIVISOR
#undef MICROPY_PY_HASHLIB
#define MICROPY_PY_HASHLIB                  (0)
