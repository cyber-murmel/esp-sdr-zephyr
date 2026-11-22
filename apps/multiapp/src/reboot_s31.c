/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ESP32-S31: stop the receive ring's writers before any software reset. osmosdr_hs's ring
 * (esp_sdr_diag16.c: PARLIO into an AXI GDMA channel) is TCM group 2, 0x2F040000 to 0x2F060000, and a
 * CPU reset resets neither: the DMA keeps writing while the ROM loads MCUboot to 0x2F05AB00 (seen on
 * the board: "SHA-256 comparison failed", then an illegal instruction). Every sys_reboot() path
 * (the shell, the fatal error handler, DFU, the app's own) ends in sys_arch_reboot(), which the
 * link wraps (CMakeLists.txt). A hardware or watchdog reset runs no software and can still hit it.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>

#include <hal/axi_dma_ll.h>
#include <hal/parlio_ll.h>

extern void __real_sys_arch_reboot(int type);

/*
 * The same channel as esp_sdr_diag16.c (DMA_CH 0 of the AXI group). Only if a run is going: it
 * enables the PARLIO receive clock and clears it again at its end, so a clear clock bit (or a gated
 * bus clock, whose registers must not be read) means nothing to stop.
 */
void __wrap_sys_arch_reboot(int type)
{
	if (HP_SYS_CLKRST.parlio_ctrl0.reg_parlio_sys_clk_en &&
	    HP_SYS_CLKRST.parlio_rx_ctrl0.reg_parlio_rx_clk_en) {
		parlio_ll_rx_start_soft_recv(&PARL_IO, false);
		parlio_ll_rx_start(&PARL_IO, false);
		if (HP_SYS_CLKRST.axi_pdma_ctrl0.reg_axi_pdma_sys_clk_en) {
			axi_dma_ll_rx_stop(AXI_DMA_LL_GET_HW(0), 0U);
			axi_dma_ll_rx_disconnect_all(AXI_DMA_LL_GET_HW(0), 0U);
		}
	}
	__real_sys_arch_reboot(type);
}
