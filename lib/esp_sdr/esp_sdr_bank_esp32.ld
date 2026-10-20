/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Capture bank of the ESP32 dump engine: SRAM1 0x3ffe8000..0x3fff7fff, which
 * MAC_DUMP_MODE 3 hands to the engine. Zephyr leaves it unused while the
 * .noinit spill to SRAM1 is off (ESP32_REGION_1_NOINIT) and the IRAM, which
 * fills SRAM1 from the other end through the reversed alias, ends below
 * 0x400a8000.
 */

__esp_sdr_bank_start = 0x3ffe8000;
__esp_sdr_bank_end = 0x3fff8000;

/* Buffers the CPU must reach during a capture: ordinary DRAM here. Not zeroed at boot. */
.esp_sdr_high (NOLOAD) :
{
	__esp_sdr_high_start = .;
	*(.esp_sdr_high .esp_sdr_high.*)
	__esp_sdr_high_end = .;
} GROUP_NOLOAD_LINK_IN(RAMABLE_REGION, RAMABLE_REGION)

ASSERT(_iram_end <= 0x400a8000, "esp-sdr: IRAM overlaps the capture bank")
