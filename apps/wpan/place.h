/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * wpan's placement rules (esp_sdr_place.h), ahead of the SoC's: the software PHY's per-sample code
 * (lib/ieee802154: the hunting loop, the block loop, the symbol decisions) runs from IRAM, 3.5 KB
 * of the room the build without the Wi-Fi driver left. Against flash (2026-11-15, the C6 asking for
 * ACKs, two rounds of 400 frames): the PHY's verdict after a frame's end came at most 173 to 188 us
 * after it instead of 220 to 322 (none of them too late for the ACK, against 3 and 1), the ACK
 * started at most 231 to 284 us after it instead of 313 to 413, and a busy console made the PHY
 * skip no samples. The thread loop and the ACK path in IRAM as well (CHUNK, EVENT) gained nothing
 * clear for another 3.5 KB.
 */

#define ESP_SDR_PLACE_SAMPLE ESP_SDR_IRAM
