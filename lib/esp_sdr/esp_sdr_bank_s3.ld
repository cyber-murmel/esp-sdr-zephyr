/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Dump banks of the S3 engine. The capture bank is SRAM bank 2 (MAC_DUMP_USAGE bit 2);
 * CONFIG_ESP_SDR_BANK1 adds bank 1 (bit 1) for DAC ping-pong and captures, and
 * CONFIG_ESP_SDR_RING bank 0 (bit 0) for the continuous ring, banks 1 and 2 keeping their
 * roles. While the dump engine owns a bank the CPUs cannot reach any of it, so nothing else
 * may live in these. Lowering _heap_sentry keeps the libc malloc arena below them.
 */

__esp_sdr_bank_org = 0x3fcd0000;
__esp_sdr_bank_len = 0x10000;

#if defined(CONFIG_ESP_SDR_RING)
__esp_sdr_bank0_org = 0x3fcb0000;

/*
 * Bank 0 is only reserved, not a section: an image whose ring may stay off
 * (apps/multiapp) places buffers there that are used only while it is off.
 */
__esp_sdr_bank0_start = __esp_sdr_bank0_org;
__esp_sdr_bank0_end = __esp_sdr_bank0_org + __esp_sdr_bank_len;
#endif

#if defined(CONFIG_ESP_SDR_BANK1)
__esp_sdr_bank1_org = 0x3fcc0000;

.esp_sdr_bank1 __esp_sdr_bank1_org (NOLOAD) :
{
	__esp_sdr_bank1_start = .;
	. += __esp_sdr_bank_len;
	__esp_sdr_bank1_end = .;
}
#endif

.esp_sdr_bank __esp_sdr_bank_org (NOLOAD) :
{
	__esp_sdr_bank_start = .;
	. += __esp_sdr_bank_len;
	__esp_sdr_bank_end = .;
}

/*
 * Internal SRAM between the capture bank and the bootloader's loader segment
 * (bank 3 below its ROM data; the engine never owns it). Not zeroed at boot.
 */
.esp_sdr_high __esp_sdr_bank_end (NOLOAD) :
{
	__esp_sdr_high_start = .;
	*(.esp_sdr_high .esp_sdr_high.*)
	__esp_sdr_high_end = .;
}

#if defined(CONFIG_ESP_SDR_RING)
_heap_sentry = __esp_sdr_bank0_org;
ASSERT(_end <= __esp_sdr_bank0_org, "esp-sdr: DRAM image overlaps the ring banks")
ASSERT(__esp_sdr_bank0_end == __esp_sdr_bank1_org && __esp_sdr_bank1_end == __esp_sdr_bank_org,
       "esp-sdr: ring banks not contiguous")
#elif defined(CONFIG_ESP_SDR_BANK1)
_heap_sentry = __esp_sdr_bank1_org;
ASSERT(_end <= __esp_sdr_bank1_org, "esp-sdr: DRAM image overlaps the transmit bank")
ASSERT(__esp_sdr_bank1_end == __esp_sdr_bank_org,
       "esp-sdr: transmit bank not right below the capture bank")
#else
_heap_sentry = __esp_sdr_bank_org;
ASSERT(_end <= __esp_sdr_bank_org, "esp-sdr: DRAM image overlaps the capture bank")
#endif
_libc_heap_size = _heap_sentry - _end;

ASSERT(__esp_sdr_bank_end <= DRAM_USER_END, "esp-sdr: capture bank beyond user DRAM")
ASSERT(__esp_sdr_high_end <= DRAM_USER_END, "esp-sdr: high buffers beyond user DRAM")
