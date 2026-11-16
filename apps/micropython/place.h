/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * MicroPython's placement rules (esp_sdr_place.h), for its sources (ESP_SDR_PLACE_SCOPE
 * micropython, here and as multiapp's extension), ahead of the SoC's. Pins marked "as before" keep
 * code and data where it was, each a candidate for a measured change (doc/code-placement.md).
 */

/*
 * PSRAM: the raw section on the P4 and the S31, whose EXT_RAM_BSS_ATTR places nothing there (the
 * HAL enables SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY for the ESP32, S2 and S3 only) and whose
 * linker scripts put .ext_ram.bss in PSRAM all the same: the SRAM stays free for the kernel, the
 * drivers and LVGL's draw buffers.
 */
#if defined(CONFIG_SOC_SERIES_ESP32P4) || defined(CONFIG_SOC_SERIES_ESP32S31)
#define Z_MP_PSRAM ESP_SDR_PSRAM_FILE
#else
#define Z_MP_PSRAM ESP_SDR_PSRAM
#endif

/* As before: the S3's receive sinks run per sample from flash. */
#define ESP_SDR_PIN_CODE_micropython_sink       ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_micropython_sink_half  ESP_SDR_FLASH
#define ESP_SDR_PIN_CODE_micropython_sink_words ESP_SDR_FLASH

/* As before: the first decimator stage in IRAM only beside the other core's dump ring. */
#if !(defined(CONFIG_ESP_SDR_DUMPRING) && CONFIG_MP_MAX_NUM_CPUS > 1)
#define ESP_SDR_PIN_CODE_micropython_dec_run1 ESP_SDR_FLASH
#endif

/* As before: the Scan, Pcap and decoder threads' stacks and the generator's table in PSRAM. */
#define ESP_SDR_PIN_DATA_micropython_scan_stack Z_MP_PSRAM
#define ESP_SDR_PIN_DATA_micropython_det_stack  Z_MP_PSRAM
#define ESP_SDR_PIN_DATA_micropython_dec_stack  Z_MP_PSRAM
#define ESP_SDR_PIN_DATA_micropython_sine_tab   Z_MP_PSRAM

/* As before: the Scan thread's kernel objects above the banks on the S3 only. */
#if !defined(CONFIG_SOC_SERIES_ESP32S3)
#define ESP_SDR_PIN_DATA_micropython_sk ESP_SDR_DEFAULT
#endif

/* As before: the remote console's rows in PSRAM on the P4 and the S31 only. */
#if !defined(CONFIG_SOC_SERIES_ESP32P4) && !defined(CONFIG_SOC_SERIES_ESP32S31)
#define ESP_SDR_PIN_DATA_micropython_row  ESP_SDR_DEFAULT
#define ESP_SDR_PIN_DATA_micropython_last ESP_SDR_DEFAULT
#endif

/* As before: the FPGA feed in the raw PSRAM section. */
#define ESP_SDR_PIN_DATA_micropython_feed_buf ESP_SDR_PSRAM_FILE
