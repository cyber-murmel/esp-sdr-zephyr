/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ESP32-S31 multiapp: the network stack's thread stacks in PSRAM, so that the internal RAM
 * stays for the radio's ring (image end below 0x2F040000). Data buffers of the MAC (TX and RX) stay
 * internal: its DMA does not reach PSRAM (seen on the board, doc/dev/network-services.md). A RAM_SECTIONS
 * snippet: it comes ahead of the SoC script's .dram0.noinit, whose catch-all would take these
 * inputs first, and ahead of its .ext_ram.data. Same technique as lib/esp_sdr/esp_sdr_txmem_s31.ld.
 *
 * Never here: k_heap and k_mem_slab backing stores (their init hooks run before PRE_KERNEL_1
 * priority 40, when PSRAM becomes accessible), the receive buffers and the MAC's descriptors
 * (DMA), the interrupt and idle stacks, the stacks of threads that touch the flash. The names are the defining file and a counter: check with
 * scripts/tcm_groups.py or nm after a change of the configuration.
 */

#if defined(CONFIG_NETWORKING) && defined(CONFIG_ESP_SPIRAM)
.multiapp_net_psram (NOLOAD) :
{
	/* the network threads' stacks (mgmt_stack, its event msgq buffer, rx_stack) */
	*(.noinit.*subsys/net/ip/net_mgmt.c.*)
	*(.noinit.*subsys/net/ip/net_tc.c.*)
	/* the log thread and the esp_timer task. NOT the stacks of threads that touch the flash (main,
	 * the shell, the system work queue, USB, the drive, the app): the flash driver turns the cache
	 * off while it works, and a stack in PSRAM then stalls the thread (seen on the board: a hang
	 * after the banner, main scans the FAT volume) */
	*(.noinit.*subsys/logging/log_core.c.*)
	*(.bss.timer_task_stack)
	/* the stacks of the TCP work queue (work_q_stack) and the sockets service
	 * (service_thread_stack.1), and the mDNS and DNS message data pools (net_buf data, no DMA, no
	 * atomics: net_buf_data_mdns_msg_pool, net_buf_data_dns_msg_pool; checked against the map) */
	*(.noinit.*subsys/net/ip/tcp.c.4)
	*(.noinit.*subsys/net/lib/sockets/sockets_service.c.*)
	*(.noinit.*subsys/net/lib/dns/mdns_responder.c.5)
	*(.noinit.*subsys/net/lib/dns/dispatcher.c.1)
	/* the mbedTLS heap of the SSH server (ssh.conf: MBEDTLS_HEAP_CUSTOM_SECTION): a plain byte buffer that
	 * mbedTLS formats at POST_KERNEL, after PSRAM works. Never k_heap: this one is mbedTLS's own allocator. */
	*(.mbedtls_heap)
	. = ALIGN(64);
} > ext_dram_seg
#endif
