/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * In IRAM, ahead of the flash text: what the receive hook calls from chain_math.c (pairing and the
 * tx records), and net_buf_unref() with what it calls, so that the interrupt never fetches code or
 * constants through the flash cache. A RAMFUNC_SECTION snippet.
 */
*libapp.a:chain_math.c.obj(.text.chain_rx .text.chain_tx_register .text.chain_tx_stamp .text.chain_tx_get .text.tx_find .text.emit)
*liblib__net_buf.a:buf.c.obj(.text.net_buf_unref .text.net_buf_pool_get .text.net_buf_destroy .text.fixed_data_unref)
*(.rodata.net_buf_fixed_* .srodata.net_buf_fixed_*)
