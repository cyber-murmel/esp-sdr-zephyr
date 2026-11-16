/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Capture bank of the S31 dump engine: the upper 128 KiB group of the TCM (0x2F060000), which the
 * engine owns while it writes (HP_SYSTEM_TCM_DATA_DUMP_CTRL_REG), ROM data at its top included.
 * The bank proper ends well below that data (esp_sdr_s31.c). Nothing of the image may live in the
 * group, and lowering _heap_sentry keeps the libc malloc arena out of it.
 */

__esp_sdr_bank_org = 0x2F060000;
__esp_sdr_bank_len = 0x1A010;

.esp_sdr_bank __esp_sdr_bank_org (NOLOAD) :
{
	__esp_sdr_bank_start = .;
	. += __esp_sdr_bank_len;
	__esp_sdr_bank_end = .;
}

#if defined(CONFIG_ESP_SDR_DIAG16)
/* The gapless ring (esp_sdr_diag16.c) below the bank, not zeroed at boot. */
.esp_sdr_ring (__esp_sdr_bank_org - CONFIG_ESP_SDR_DIAG16_RING_KB * 1024) (NOLOAD) :
{
	__esp_sdr_ring_start = .;
	. += CONFIG_ESP_SDR_DIAG16_RING_KB * 1024;
}
__esp_sdr_image_end_max = __esp_sdr_ring_start;
#else
__esp_sdr_image_end_max = __esp_sdr_bank_org;
#endif

_heap_sentry = __esp_sdr_image_end_max;
_libc_heap_size = _heap_sentry - _end;

ASSERT(_end <= __esp_sdr_image_end_max, "esp-sdr: DRAM image overlaps the ring or capture bank")
ASSERT(__esp_sdr_bank_end <= 0x2F07AFB0, "esp-sdr: capture bank reaches the ROM boot stack")
