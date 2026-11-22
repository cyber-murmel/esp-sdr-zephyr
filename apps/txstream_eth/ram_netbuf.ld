/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * In IRAM, ahead of the flash text: what CPU 1 calls per packet when it gives a receive buffer back (tx_stream.c,
 * net_buf_unref()): the release, the pool's destroy path and the fixed allocator's unref, and the const tables it
 * reads (the allocator's callbacks), so that CPU 1 never fetches code or constants through the flash cache. About
 * 230 B of group 0. A RAMFUNC_SECTION snippet: it lands in .iram0.text before the SoC script's flash catch-alls.
 */
*liblib__net_buf.a:buf.c.obj(.text.net_buf_unref .text.net_buf_destroy .text.fixed_data_unref)
*(.rodata.net_buf_fixed_* .srodata.net_buf_fixed_*)
