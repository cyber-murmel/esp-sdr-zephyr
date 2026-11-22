/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * txstream_eth's placement rules (esp_sdr_place.h), ahead of the SoC's: what runs on CPU 1 in the
 * stream's window, the deadman's interrupt and the receive hook stay out of the flash cache.
 */

#define ESP_SDR_PIN_CODE_tx_deadman ESP_SDR_IRAM
#define ESP_SDR_PIN_CODE_tx_hook ESP_SDR_IRAM
/* what the hook calls per packet, and the completion of a frame the MAC sent (interrupt); the rest of
 * tx_net.c (control thread, cookie protocol, status thread, shell) is flash */
#define ESP_SDR_PIN_CODE_tx_stream_put ESP_SDR_IRAM
#define ESP_SDR_PIN_CODE_tx_stream_inflight_add ESP_SDR_IRAM
#define ESP_SDR_PIN_CODE_slot_done ESP_SDR_IRAM

/* Tables and scratch the code fills itself: group 3, where the buffers are (group 0 is full) */
#define ESP_SDR_PIN_DATA_tbl_w ESP_SDR_NOINIT
#define ESP_SDR_PIN_DATA_tbl_8 ESP_SDR_NOINIT
#define ESP_SDR_PIN_DATA_cal_src ESP_SDR_NOINIT
#define ESP_SDR_PIN_DATA_fin ESP_SDR_NOINIT
#define ESP_SDR_PIN_DATA_pub_stats ESP_SDR_NOINIT
#define ESP_SDR_PIN_DATA_ctl_wake ESP_SDR_NOINIT
/* The flip thread's state (2 KB, zeroed by a SYS_INIT in tx_stream.c) and its thread object: group 0 holds
 * the image and the PSRAM driver's code, group 3 has the room since the stacks went to PSRAM */
#define ESP_SDR_PIN_DATA_st ESP_SDR_NOINIT
/* The library's gain table (1.9 KB), read only where its valid flags say it was written */
#define ESP_SDR_PIN_DATA_gain_words ESP_SDR_NOINIT
#define ESP_SDR_PIN_DATA_tx_thread ESP_SDR_NOINIT
#define ESP_SDR_PIN_DATA_ctl_ack ESP_SDR_NOINIT
/* The network side (tx_net.c): kernel objects, threads and tables the code initializes itself */
#define ESP_SDR_PIN_DATA_net ESP_SDR_NOINIT
#define ESP_SDR_PIN_DATA_plk ESP_SDR_NOINIT
#define ESP_SDR_PIN_DATA_st_wake ESP_SDR_NOINIT
#define ESP_SDR_PIN_DATA_ctl_q ESP_SDR_NOINIT
#define ESP_SDR_PIN_DATA_ctl_q_buf ESP_SDR_NOINIT
#define ESP_SDR_PIN_DATA_ctl_thread ESP_SDR_NOINIT
#define ESP_SDR_PIN_DATA_st_thread ESP_SDR_NOINIT
/* The injector's thread object (injector.conf): tx_inject_start() zeroes it before each k_thread_create() */
#define ESP_SDR_PIN_DATA_inj_thread ESP_SDR_NOINIT

/*
 * PSRAM and LP RAM (CONFIG_ESP_SPIRAM, the S31's 16 MB octal PSRAM at 250 MHz, and its LP RAM). Only what no
 * interrupt, no DMA and no CPU 1 loop touches, and what is not on a timing the stream depends on:
 * - the control thread's stack (tx_ctl, a message at a time);
 * - its state and counters (`nc`, the session) in LP RAM: small, cold, never atomic.
 * Stays in internal RAM: the status thread's stack (it wakes every 100 us and its delay budget at 40 MS/s is 0.42
 * ms; a stack line missing the cache costs about a microsecond, but the margin is not worth 1.3 KB), the flip
 * thread's stack and state, the receive buffers and descriptors, the interrupt and idle stacks, the status slots
 * (the MAC reads them), the system heap. The Zephyr threads' stacks are moved by esp_sdr_txmem_s31.ld.
 */
#if defined(CONFIG_ESP_SPIRAM)
#define ESP_SDR_PIN_DATA_ctl_stack ESP_SDR_PSRAM_NOINIT
#endif
#define ESP_SDR_PIN_DATA_nc ESP_SDR_LPRAM
