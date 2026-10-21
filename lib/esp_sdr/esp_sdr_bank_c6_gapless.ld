/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Capture banks of the C6 dump engine for the gapless ring (esp_sdr_gapless.h):
 * SRAM blocks 1 and 2 (0x40820000, usage bits 9 and 10, as upstream's ring).
 * Block 1 is the bank of the one-shot captures and the trigger, block 2 the
 * ring's second; each lap of the ring uses the lower 64 KiB of its block. The
 * image must fit block 0: the engine takes a whole block while it writes it.
 */

__esp_sdr_bank_org = 0x40820000;
__esp_sdr_bank_len = 0x20000;

.esp_sdr_bank __esp_sdr_bank_org (NOLOAD) :
{
	__esp_sdr_bank_start = .;
	. += __esp_sdr_bank_len;
	__esp_sdr_bank_end = .;
	__esp_sdr_bank2_start = .;
	. += __esp_sdr_bank_len;
	__esp_sdr_bank2_end = .;
}

/* Block 3 up to the end of user DRAM: buffers of the ring's consumers. Not zeroed at boot. */
.esp_sdr_high __esp_sdr_bank2_end (NOLOAD) :
{
	__esp_sdr_high_start = .;
	*(.esp_sdr_high .esp_sdr_high.*)
	__esp_sdr_high_end = .;
}

_heap_sentry = __esp_sdr_bank_org;
_libc_heap_size = _heap_sentry - _end;

ASSERT(_end <= __esp_sdr_bank_org, "esp-sdr: DRAM image overlaps the ring banks (block 1)")
ASSERT(__esp_sdr_high_end <= DRAM_USER_END, "esp-sdr: high buffers beyond user DRAM")
