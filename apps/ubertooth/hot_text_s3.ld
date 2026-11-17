/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * CPU 0's hot flash code in the BLE modes: the list of lib/esp_sdr's hot block
 * (CONFIG_ESP_SDR_HOT_TEXT, esp_sdr_hot_text() in CMakeLists.txt), one block at a fixed
 * instruction cache position ahead of the flash text's catch-all.
 *
 * The S3's instruction cache is 16 KB, 8 ways of 2 KB with 32 byte lines. Where its lines fall
 * decided how much of the stream CPU 0 lost: the same image padded back to the previous flash
 * layout lost a fifth to a fifteenth as much (doc/apps/ubertooth-internals.md). Every function, and
 * every literal before the text, moved the code behind it. Aligned to the way, the block's lines
 * keep their cache sets whatever comes before or after it. At about 12 KB it takes 6 of the 8 ways
 * of each set, leaving 2 to the rest: filled to 16 KB, it left the losses depending 9 times
 * as much on code outside it as at 12 KB (3.5 times).
 *
 * The functions: what CPU 0 ran from flash in three profiles of 131072 samples (the app's former
 * ubt prof, about 2 s each: following a connection, scanning and connecting, sniffing channel 37),
 * densest first (samples per byte) up to 11.5 KB, one statement per object in that order: 66
 * functions, 96 % of the flash samples, 11841 B with their literals, which the linker pools at the
 * block's start. The USB device stack (DWC2 driver, usbd control path, net_buf, sys_heap), the mode
 * loop (le_run()), acquire(), find_gap() and the CRC. scripts/place/hot_text.py makes the list
 * from "prof dump" captures (CONFIG_ESP_SDR_CPU_PROF). A function renamed falls back to the
 * catch-all.
 */

    *libapp.a:bluetooth_le.c.obj(
        .literal.btle_crcgen_lut .literal.btle_crcgen_lut$*
        .text.btle_crcgen_lut .text.btle_crcgen_lut$*)
    *libapp.a:ubt_rssi.c.obj(
        .literal.ubt_floor_hist_add .literal.ubt_floor_hist_add$*
        .text.ubt_floor_hist_add .text.ubt_floor_hist_add$*)
    *libzephyr.a:heap.c.obj(
        .literal.chunk_size .literal.chunk_size$*
        .text.chunk_size .text.chunk_size$*
        .literal.chunk_set .literal.chunk_set$*
        .text.chunk_set .text.chunk_set$*
        .literal.chunk_field .literal.chunk_field$*
        .text.chunk_field .text.chunk_field$*
        .literal.set_chunk_used .literal.set_chunk_used$*
        .text.set_chunk_used .text.set_chunk_used$*
        .literal.split_chunks .literal.split_chunks$*
        .text.split_chunks .text.split_chunks$*
        .literal.set_chunk_size .literal.set_chunk_size$*
        .text.set_chunk_size .text.set_chunk_size$*
        .literal.bucket_idx .literal.bucket_idx$*
        .text.bucket_idx .text.bucket_idx$*
        .literal.alloc_chunk .literal.alloc_chunk$*
        .text.alloc_chunk .text.alloc_chunk$*
        .literal.free_list_add .literal.free_list_add$*
        .text.free_list_add .text.free_list_add$*
        .literal.free_list_remove_bidx .literal.free_list_remove_bidx$*
        .text.free_list_remove_bidx .text.free_list_remove_bidx$*
        .literal.chunk_usable_bytes .literal.chunk_usable_bytes$*
        .text.chunk_usable_bytes .text.chunk_usable_bytes$*
        .literal.sys_heap_aligned_alloc .literal.sys_heap_aligned_alloc$*
        .text.sys_heap_aligned_alloc .text.sys_heap_aligned_alloc$*
        .literal.merge_chunks .literal.merge_chunks$*
        .text.merge_chunks .text.merge_chunks$*)
    *libsubsys__usb__device_next.a:usbd_class.c.obj(
        .literal.usbd_class_get_by_req .literal.usbd_class_get_by_req$*
        .text.usbd_class_get_by_req .text.usbd_class_get_by_req$*)
    *libapp.a:ubt_rx_run.c.obj(
        .literal.le_run .literal.le_run$*
        .text.le_run .text.le_run$*
        .literal.clk_of_sample .literal.clk_of_sample$*
        .text.clk_of_sample .text.clk_of_sample$*)
    *libdrivers__usb__udc.a:udc_common.c.obj(
        .literal.udc_ep_set_busy .literal.udc_ep_set_busy$*
        .text.udc_ep_set_busy .text.udc_ep_set_busy$*
        .literal.udc_ep_buf_alloc .literal.udc_ep_buf_alloc$*
        .text.udc_ep_buf_alloc .text.udc_ep_buf_alloc$*
        .literal.udc_buf_destroy .literal.udc_buf_destroy$*
        .text.udc_buf_destroy .text.udc_buf_destroy$*
        .literal.udc_ep_is_busy .literal.udc_ep_is_busy$*
        .text.udc_ep_is_busy .text.udc_ep_is_busy$*
        .literal.udc_ctrl_data_alloc .literal.udc_ctrl_data_alloc$*
        .text.udc_ctrl_data_alloc .text.udc_ctrl_data_alloc$*
        .literal.udc_ep_enqueue .literal.udc_ep_enqueue$*
        .text.udc_ep_enqueue .text.udc_ep_enqueue$*
        .literal.udc_get_ep_cfg .literal.udc_get_ep_cfg$*
        .text.udc_get_ep_cfg .text.udc_get_ep_cfg$*
        .literal.udc_buf_put .literal.udc_buf_put$*
        .text.udc_buf_put .text.udc_buf_put$*
        .literal.udc_ctrl_status_alloc .literal.udc_ctrl_status_alloc$*
        .text.udc_ctrl_status_alloc .text.udc_ctrl_status_alloc$*
        .literal.udc_ep_buf_has_zlp .literal.udc_ep_buf_has_zlp$*
        .text.udc_ep_buf_has_zlp .text.udc_ep_buf_has_zlp$*
        .literal.udc_submit_ep_event .literal.udc_submit_ep_event$*
        .text.udc_submit_ep_event .text.udc_submit_ep_event$*
        .literal.udc_buf_peek .literal.udc_buf_peek$*
        .text.udc_buf_peek .text.udc_buf_peek$*
        .literal.udc_setup_received .literal.udc_setup_received$*
        .text.udc_setup_received .text.udc_setup_received$*)
    *libapp.a:ubt_le.c.obj(
        .literal.find_gap .literal.find_gap$*
        .text.find_gap .text.find_gap$*)
    *liblib__net_buf.a:buf_simple.c.obj(
        .literal.net_buf_simple_pull .literal.net_buf_simple_pull$*
        .text.net_buf_simple_pull .text.net_buf_simple_pull$*)
    *libapp.a:gfsk_demod.c.obj(
        .literal.acquire .literal.acquire$*
        .text.acquire .text.acquire$*)
    *libsubsys__usb__device_next.a:usbd_ch9.c.obj(
        .literal.usbd_enqueue_setup .literal.usbd_enqueue_setup$*
        .text.usbd_enqueue_setup .text.usbd_enqueue_setup$*
        .literal.reqtype_is_to_device .literal.reqtype_is_to_device$*
        .text.reqtype_is_to_device .text.reqtype_is_to_device$*
        .literal.nonstd_request .literal.nonstd_request$*
        .text.nonstd_request .text.nonstd_request$*)
    *liblib__libc__common.a:errno.c.obj(
        .literal.z_impl_z_errno .literal.z_impl_z_errno$*
        .text.z_impl_z_errno .text.z_impl_z_errno$*)
    *libdrivers__usb__udc.a:udc_dwc2.c.obj(
        .literal.udc_dwc2_unlock .literal.udc_dwc2_unlock$*
        .text.udc_dwc2_unlock .text.udc_dwc2_unlock$*
        .literal.udc_dwc2_ep_enqueue .literal.udc_dwc2_ep_enqueue$*
        .text.udc_dwc2_ep_enqueue .text.udc_dwc2_ep_enqueue$*
        .literal.dwc2_clear_control_in_nak .literal.dwc2_clear_control_in_nak$*
        .text.dwc2_clear_control_in_nak .text.dwc2_clear_control_in_nak$*
        .literal.dwc2_handle_xfer_next .literal.dwc2_handle_xfer_next$*
        .text.dwc2_handle_xfer_next .text.dwc2_handle_xfer_next$*
        .literal.dwc2_prep_rx .literal.dwc2_prep_rx$*
        .text.dwc2_prep_rx .text.dwc2_prep_rx$*
        .literal.dwc2_tx_fifo_write .literal.dwc2_tx_fifo_write$*
        .text.dwc2_tx_fifo_write .text.dwc2_tx_fifo_write$*
        .literal.udc_dwc2_isr_handler .literal.udc_dwc2_isr_handler$*
        .text.udc_dwc2_isr_handler .text.udc_dwc2_isr_handler$*
        .literal.k_event_post .literal.k_event_post$*
        .text.k_event_post .text.k_event_post$*
        .literal.udc_dwc2_ep_disable .literal.udc_dwc2_ep_disable$*
        .text.udc_dwc2_ep_disable .text.udc_dwc2_ep_disable$*
        .literal.dwc2_in_completer_mode .literal.dwc2_in_completer_mode$*
        .text.dwc2_in_completer_mode .text.dwc2_in_completer_mode$*)
    *libapp.a:ubt_rx_sess.c.obj(
        .literal.ubt_rx_sess_floor .literal.ubt_rx_sess_floor$*
        .text.ubt_rx_sess_floor .text.ubt_rx_sess_floor$*
        .literal.ubt_rx_sess_split .literal.ubt_rx_sess_split$*
        .text.ubt_rx_sess_split .text.ubt_rx_sess_split$*
        .literal.ubt_rx_sess_emit .literal.ubt_rx_sess_emit$*
        .text.ubt_rx_sess_emit .text.ubt_rx_sess_emit$*)
    *libdrivers__usb__common__buf.a:usb_buf.c.obj(
        .literal.usb_pool_data_alloc .literal.usb_pool_data_alloc$*
        .text.usb_pool_data_alloc .text.usb_pool_data_alloc$*
        .literal.usb_pool_data_unref .literal.usb_pool_data_unref$*
        .text.usb_pool_data_unref .text.usb_pool_data_unref$*)
    *libapp.a:ubt_queue.c.obj(
        .literal.ubt_queue_take .literal.ubt_queue_take$*
        .text.ubt_queue_take .text.ubt_queue_take$*
        .literal.ubt_queue_count .literal.ubt_queue_count$*
        .text.ubt_queue_count .text.ubt_queue_count$*)
    *libdrivers__cache.a:cache_esp32.c.obj(
        .literal.cache_data_invd_range .literal.cache_data_invd_range$*
        .text.cache_data_invd_range .text.cache_data_invd_range$*)
    *libsubsys__usb__device_next.a:usbd_core.c.obj(
        .literal.usbd_event_carrier .literal.usbd_event_carrier$*
        .text.usbd_event_carrier .text.usbd_event_carrier$*)
    *liblib__net_buf.a:buf.c.obj(
        .literal.net_buf_unref .literal.net_buf_unref$*
        .text.net_buf_unref .text.net_buf_unref$*
        .literal.net_buf_reset .literal.net_buf_reset$*
        .text.net_buf_reset .text.net_buf_reset$*
        .literal.net_buf_alloc_len .literal.net_buf_alloc_len$*
        .text.net_buf_alloc_len .text.net_buf_alloc_len$*
        .literal.net_buf_pool_get .literal.net_buf_pool_get$*
        .text.net_buf_pool_get .text.net_buf_pool_get$*)
    *libsubsys__usb__device_next.a:usbd_endpoint.c.obj(
        .literal.usbd_ep_ctrl_enqueue .literal.usbd_ep_ctrl_enqueue$*
        .text.usbd_ep_ctrl_enqueue .text.usbd_ep_ctrl_enqueue$*
        .literal.usbd_ep_ctrl_data_in_alloc .literal.usbd_ep_ctrl_data_in_alloc$*
        .text.usbd_ep_ctrl_data_in_alloc .text.usbd_ep_ctrl_data_in_alloc$*)
    *libapp.a:ubt_usb.c.obj(
        .literal.ubt_control_to_host .literal.ubt_control_to_host$*
        .text.ubt_control_to_host .text.ubt_control_to_host$*)
    *libsubsys__usb__device_next.a:usbd_config.c.obj(
        .literal.usbd_config_get_current .literal.usbd_config_get_current$*
        .text.usbd_config_get_current .text.usbd_config_get_current$*)
    *liblib__libc__picolibc.a:errno_wrap.c.obj(
        .literal.z_errno_wrap .literal.z_errno_wrap$*
        .text.z_errno_wrap .text.z_errno_wrap$*)
