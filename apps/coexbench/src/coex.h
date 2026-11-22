/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef COEXBENCH_COEX_H_
#define COEXBENCH_COEX_H_

#include <stdint.h>

#include <zephyr/shell/shell.h>

/* The Wi-Fi primary channel the log stamps its entries with (0: unknown). */
void phylog_set_channel(uint8_t chan);
uint8_t phylog_channel(void);

/* "coex log [clear]": the entries and the statistics. */
int phylog_shell(const struct shell *sh, bool clear);

/* Refresh the cached channel from the driver (thread context only). */
void coex_channel_refresh(void);

/* Centre frequency of a 2.4 GHz Wi-Fi channel, MHz. */
uint32_t coex_chan_mhz(uint8_t chan);

/* "coex sdr <seconds> [ring|burst] [freq_mhz]" and "coex arb [clear]" (sdrwin.c). */
int coex_cmd_sdr(const struct shell *sh, size_t argc, char **argv);
int coex_cmd_arb(const struct shell *sh, size_t argc, char **argv);

/* ESP-NOW shell commands are registered by espnow.c. */

#endif
