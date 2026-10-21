/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Capture bank of the C6 dump engine: all of SRAM block 2 (128 KiB at 0x40840000, usage bit
 * 10, as upstream); a capture uses up to 64 KiB of it. For the gapless ring
 * (CONFIG_ESP_SDR_GAPLESS, esp_sdr_gapless.h) SRAM blocks 1 and 2 (0x40820000, usage bits 9
 * and 10, as upstream's ring): block 1 is then the bank of the one-shot captures and the
 * trigger, block 2 the ring's second, and each lap of the ring uses the lower 64 KiB of its
 * block. The image must then fit block 0: the engine takes a whole block while it writes it.
 * Lowering _heap_sentry keeps the libc malloc arena below the banks.
 */

#if defined(CONFIG_ESP_SDR_GAPLESS)
__esp_sdr_bank_org = 0x40820000;
#else
__esp_sdr_bank_org = 0x40840000;
#endif
__esp_sdr_bank_len = 0x20000;

.esp_sdr_bank __esp_sdr_bank_org (NOLOAD) :
{
	__esp_sdr_bank_start = .;
	. += __esp_sdr_bank_len;
	__esp_sdr_bank_end = .;
#if defined(CONFIG_ESP_SDR_GAPLESS)
	__esp_sdr_bank2_start = .;
	. += __esp_sdr_bank_len;
	__esp_sdr_bank2_end = .;
#endif
}

/*
 * Block 3 up to the end of user DRAM: the engine takes a whole block during a capture, so
 * nothing else may live in the banks. Not zeroed at boot.
 */
#if defined(CONFIG_ESP_SDR_GAPLESS)
.esp_sdr_high __esp_sdr_bank2_end (NOLOAD) :
#else
.esp_sdr_high __esp_sdr_bank_end (NOLOAD) :
#endif
{
	__esp_sdr_high_start = .;
	*(.esp_sdr_high .esp_sdr_high.*)
	__esp_sdr_high_end = .;
}

_heap_sentry = __esp_sdr_bank_org;
_libc_heap_size = _heap_sentry - _end;

ASSERT(_end <= __esp_sdr_bank_org, "esp-sdr: DRAM image overlaps the capture bank")
ASSERT(__esp_sdr_high_end <= DRAM_USER_END, "esp-sdr: high buffers beyond user DRAM")
