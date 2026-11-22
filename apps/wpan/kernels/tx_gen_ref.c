/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A hot-swap kernel for the stage tx_gen of the 802.15.4 driver (lib/ieee802154/ieee154_esp_sdr.c, the
 * ieee154 module of apps/multiapp): the ACK and frame waveform generator, the same algorithm as the
 * built-in one (ieee154_dsp_tx_gen(): the shift register of chips and the 128-bit slot copies where the
 * vector unit is on), so that it must produce the same DAC words: the oracle is
 * `wpan bench ack` ("words differ from the built-in", 0) in a multiapp build, and the unit test
 * tests/unit/ieee154_dsp, which links this file. Built INTERNAL: its function runs from internal RAM in
 * the ring's DAC filler and burst builder, with interrupts masked (multiapp_kernel() in
 * apps/multiapp/CMakeLists.txt).
 *
 *   multiapp stage put tx_gen <size> <crc>    (apps/multiapp/scripts/kswap.py --stage tx_gen tx_gen_ref.llext)
 *
 * It imports nothing: the state is the driver's struct ieee154_dsp_tx, which it gets through the user
 * argument (so it needs no global of the ieee154 module), and the slot templates the state points at.
 */

#include <esp_sdr/esp_sdr_place.h>

#include <multiapp/stage.h>

#include "ieee154_dsp.h"

/* The attributes of the built-in one: no library calls from a function that runs with interrupts off. */
ESP_SDR_CODE(PACED_SAMPLE, tx_gen_ref)
static __attribute__((noclone, optimize("O3", "no-tree-loop-distribute-patterns",
					"no-unswitch-loops"))) void
tx_gen_ref(void *user, uint32_t *dst, uint64_t index, uint32_t n)
{
	ieee154_dsp_tx_gen(user, dst, index, n);
}

MULTIAPP_KERNEL_DEFINE(tx_gen, ieee154_dsp_tx_gen_fn_t, tx_gen_ref);
