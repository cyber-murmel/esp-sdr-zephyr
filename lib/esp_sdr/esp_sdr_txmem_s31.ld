/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * S31 transmit layout (CONFIG_ESP_SDR_S31_TXMEM): TCM groups 1 and 2 (0x2F020000 to 0x2F060000)
 * belong to the DAC banks, the image (text, data, bss, heap) is in group 0 and everything
 * uninitialized (net_buf pools, thread and interrupt stacks, kernel heaps, app buffers) in group 3
 * up to the ROM boot stack.
 *
 * A RAM_SECTIONS snippet: it comes ahead of the SoC script's .dram0.noinit, whose catch-all
 * *(.noinit.*) would take these inputs first (ld gives an input section to the first statement
 * that matches). That one stays empty. Kconfig keeps out what the SoC script puts elsewhere or
 * ahead of this: USERSPACE (.user_stacks, app_smem), LLEXT heaps, and the network .noinit that
 * ESP32_WIFI_NET_ALLOC_SPIRAM sends to PSRAM.
 *
 * With CONFIG_ESP_SDR_S31_TXMEM_PSRAM the stacks of the threads that have slack (below) are taken
 * out ahead of that and put into PSRAM, in an output section of their own at the start of the
 * external RAM, ahead of the SoC script's .ext_ram.data (heap, bss and the app's
 * ESP_SDR_PSRAM_NOINIT data). Each is a section of its own, named by the file that defines it and
 * a counter, so the counter is part of the pattern: scripts/tcm_groups.py --tx checks the result.
 */

#if defined(CONFIG_ESP_SDR_S31_TXMEM_PSRAM)
.esp_sdr_psram (NOLOAD) :
{
	__esp_sdr_psram_start = .;
	/* the main thread: the stream's control thread (chain on and off, the report) */
	*(.noinit.*kernel/init.c.1)
	*(.noinit.*kernel/system_work_q.c.*)
	*(.noinit.*subsys/logging/log_core.c.*)
	/* the shell's stack only: its history heap is a k_heap, set up at PRE_KERNEL_1 priority 30, before
	 * esp_ldo_psram_init (priority 40) makes PSRAM accessible (a store fault at boot) */
	*(.noinit.*subsys/shell/backends/shell_uart.c.1)
	*(.noinit.*subsys/net/ip/net_mgmt.c.*)
	*(.noinit.*subsys/net/ip/net_tc.c.*)
#if defined(CONFIG_ESP_SDR_S31_TXMEM_NETTX_PSRAM)
	/* the transmit data buffers and their net_buf records (net_pkt.c.7, .6). Not the net_pkt slab: the free
	 * lists of slabs are built by a kernel init hook before PRE_KERNEL_1, when PSRAM is not accessible yet */
	*(.noinit.*subsys/net/ip/net_pkt.c.7)
	*(.noinit.*subsys/net/ip/net_pkt.c.6)
#endif
	. = ALIGN(64);
	__esp_sdr_psram_end = .;
} > ext_dram_seg

#endif

__esp_sdr_grp3_org = 0x2F060000;
__esp_sdr_grp3_max = 0x2F07AFB0;
__esp_sdr_grp0_max = 0x2F020000;

.esp_sdr_grp3 __esp_sdr_grp3_org (NOLOAD) :
{
	__esp_sdr_grp3_start = .;
	*(.noinit)
	*(.noinit.*)
	. = ALIGN(16);
	__esp_sdr_grp3_end = .;
}

/* The libc arena gets what group 0 has left. */
_heap_sentry = __esp_sdr_grp0_max;
_libc_heap_size = _heap_sentry - _end;

ASSERT(_end <= __esp_sdr_grp0_max, "esp-sdr txmem: image end is not below TCM group 1")
ASSERT(__esp_sdr_grp3_end <= __esp_sdr_grp3_max, "esp-sdr txmem: group 3 reaches the ROM boot stack")
ASSERT(__esp_sdr_grp3_start == __esp_sdr_grp3_org, "esp-sdr txmem: group 3 section moved")
/* What must never reach PSRAM or LP RAM (global symbols only; scripts/tcm_groups.py --tx checks the static ones,
 * kheap_*, _k_mem_slab_buf_*, the idle stacks, after the link): the interrupt stacks and the system heap */
ASSERT(z_interrupt_stacks >= 0x2F000000 && z_interrupt_stacks < 0x2F080000, "esp-sdr txmem: interrupt stacks outside internal RAM")
ASSERT(kheap__system_heap >= 0x2F000000 && kheap__system_heap < 0x2F080000, "esp-sdr txmem: system heap outside internal RAM")
