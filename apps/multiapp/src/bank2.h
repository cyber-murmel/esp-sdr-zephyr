/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The capture bank as a second internal heap for the apps that do not capture (BANK2, apps.c):
 * ESP32-S3 dump bank 2, and on the ESP32-S31 TCM group 3 (0x2F060000), which the dump engine owns
 * only while it captures or starts the diagnostic bus (doc/dev/network-services.md, "Group 3").
 */

#ifndef MULTIAPP_BANK2_H_
#define MULTIAPP_BANK2_H_

#include <stdint.h>

#include <zephyr/sys/util.h>

extern uint8_t __esp_sdr_bank_start[], __esp_sdr_bank_end[];

#if defined(CONFIG_SOC_SERIES_ESP32S3)
#define MULTIAPP_BANK2 1
#define BANK2_START    __esp_sdr_bank_start
#define BANK2_SIZE     ((size_t)(__esp_sdr_bank_end - __esp_sdr_bank_start))
#elif defined(CONFIG_SOC_SERIES_ESP32S31)
#define MULTIAPP_BANK2 1
/*
 * The whole group up to the ROM boot stack (0x2F07AFB0, the ROM data is above it), not just the
 * engine's aperture (the bank proper, 0x1A010 bytes): the engine writes only the aperture, the rest
 * is memory like any, readable while the CPU is not parked (esp_sdr_s31.c group_take()).
 */
#define BANK2_GUARD 1024U
#define BANK2_START (__esp_sdr_bank_start + BANK2_GUARD)
#define BANK2_SIZE  ((size_t)(0x2F07AFB0U - (uintptr_t)BANK2_START))
#endif

#endif /* MULTIAPP_BANK2_H_ */
