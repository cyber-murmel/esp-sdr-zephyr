/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Ring bank 0 (esp_sdr_bank_s3.ld only reserves it) for buffers no ring
 * user needs: stream's CDC-NCM DMA buffers. Included before .dram0.noinit and
 * .dram0.bss, so these sections land here; nothing zeroes them. The FATFS object the host
 * reads files with (src/drive.c), the flash disk's cache and the USB drive's buffer are not
 * among them: the host reads the volume, and the PC uses the drive, with the ring running.
 */

.multiapp_bank0 0x3fcb0000 (NOLOAD) :
{
	__multiapp_bank0_start = .;
	*usbd_cdc_ncm.c.obj(.noinit .noinit.* .bss.net_buf_data_*)
	__multiapp_bank0_end = .;
}

ASSERT(__multiapp_bank0_end <= 0x3fcc0000, "multiapp: bank 0 buffers beyond bank 0")

/* The app's file is read to the top of PSRAM (src/apps.c). */
ASSERT(_ext_ram_end + CONFIG_MULTIAPP_FILE_MAX_KB * 1024 <= _ext_ram_start + CONFIG_ESP_SPIRAM_SIZE,
       "multiapp: PSRAM has no room left for the app file's buffer")
