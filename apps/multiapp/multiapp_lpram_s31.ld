/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ESP32-S31 multiapp: stacks and buffers in the LP RAM (0x2E000000, about 30 KB the image does not
 * use), so that the internal RAM below the receive ring holds the network's DMA buffers. LP RAM is
 * not behind the cache, so a thread whose stack is here runs through flash operations (a stack in
 * PSRAM stalls), and it is accessible from reset. It is slower than the TCM and no DMA reaches it.
 * Only COLD things: nothing the osmosdr_hs stream or its USB path runs on or touches per batch (the
 * app's stack, the USB device and UDC threads stay internal: the first board run with them here
 * dropped samples; and nothing the USB drive touches while a stream runs: the mass storage thread's
 * stack, the flash disk's cache and the FATFS object, which cost 567 gaps in 60 s at 20 MS/s on the
 * board, 0 gaps when internal), no interrupt or idle stacks, no DMA buffers, no atomic_t (the kernel objects,
 * the net_pkt and TCP slabs stay internal; AMO to this RAM is not checked).
 * A RAM_SECTIONS snippet, ahead of the SoC script's .dram0.noinit and .bss catch-alls. Not zeroed:
 * a stack needs no zero.
 */

#if defined(CONFIG_SOC_SERIES_ESP32S31)
.multiapp_lpram (NOLOAD) :
{
	. = ALIGN(16);
	__multiapp_lpram_start = .;
	/* main (the FAT scan, the radio start), the shell, the system work queue */
	*(.noinit.*kernel/init.c.1)
	*(.noinit.*subsys/shell/backends/shell_uart.c.1)
	*(.noinit.*kernel/system_work_q.c.*)
	/* the system heap's bytes (kheap__system_heap, with the allocator's chunk headers; the k_heap
	 * struct _system_heap stays internal); no atomic_t in it, and the stream path is not known to
	 * allocate from it (the board's peak is 2224 B) */
	*(.noinit.*kernel/mempool.c.kheap_buf__system_heap)
	/* the log's message buffer and the shell's history bytes: plain data */
	*(.bss.buf32)
	*(.noinit.*subsys/shell/backends/shell_uart.c.kheap_buf_shell_uart_history_heap)
	/* MCUmgr (apps/common/net/mcumgr.conf): the work queue's stack (its handlers write the flash, so
	 * never PSRAM) and the data of its two request/response buffers (the UDP thread copies into
	 * them: no DMA). The net_buf pool and work queue structs stay internal (kernel objects). */
	*(.noinit.*subsys/mgmt/mcumgr/transport/src/smp.c.*)
	/* SSH (ssh.conf): the stack and history bytes of the session's shell (src/console_mux.c; its commands touch the
	 * flash, so never PSRAM) */
	*(.noinit.*console_mux.c.*)
	/* and the stack of the thread that makes the host key and reads the key file (src/sshd.c) */
	*(.noinit.*sshd.c.*)
	. = ALIGN(16);
	__multiapp_lpram_end = .;
} > lp_ram_seg

/* The region's own overflow error is the size guard; these say what must be where. */
ASSERT(__multiapp_lpram_end <= ORIGIN(lp_ram_seg) + LENGTH(lp_ram_seg),
       "multiapp: LP RAM section beyond the LP RAM")
ASSERT(z_main_stack >= __multiapp_lpram_start && z_main_stack < __multiapp_lpram_end,
       "multiapp: the main stack is not in LP RAM")
ASSERT(kheap__system_heap >= __multiapp_lpram_start && kheap__system_heap < __multiapp_lpram_end,
       "multiapp: the system heap bytes are not in LP RAM")
/* The static stacks (app, USB) are checked after the link: scripts/tcm_groups.py --multiapp */
ASSERT(z_interrupt_stacks >= 0x2F000000 && z_interrupt_stacks < 0x2F080000,
       "multiapp: interrupt stacks outside internal RAM")
#endif
