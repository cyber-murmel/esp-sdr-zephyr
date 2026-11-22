/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TX_BOOT_H_
#define TX_BOOT_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/shell/shell.h>

#define TCM_BASE 0x2F000000UL
#define TCM_GROUP_SIZE 0x20000UL
#define TCM_GROUPS 4U
#define GROUP_OF(a) ((unsigned int)(((uintptr_t)(a) - TCM_BASE) / TCM_GROUP_SIZE))

/** The hook's view of a received frame, from tx_net.c: the buffer address of the last one. */
uintptr_t tx_last_rx_buf(void);

/** Canaries: groups 1 and 2 (the banks), the free tail of group 3 and the idle stream stack. */
void tx_canary_fill(bool with_stack);
/** Print what changed since tx_canary_fill(); returns the number of changed words. */
unsigned int tx_canary_check(const struct shell *sh);

/** Once after the init: PSRAM and the PLL-derived clocks (EMAC reference, PTP reference, MPLL) to the log. */
void tx_mem_report(void);

/** tx boot: addresses per group and run time layout checks; 0 when all pass. */
int tx_boot_report(const struct shell *sh);

#endif
