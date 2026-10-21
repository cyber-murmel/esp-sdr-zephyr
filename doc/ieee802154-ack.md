# Sending ACKs from the software radio

Built (2026-10-12): the S3 acknowledges frames to its address from inside the ring run, 192 us after their end in more than 99 % of the cases. Zigbee and Thread need an ACK for every unicast frame to the node that has the AR bit set: without it the sender retries three times and reports the frame as lost, so a node that cannot send one in time cannot even join a network as an end device (the association response from the parent asks for an ACK). Before this, the S3 software radio received 802.15.4 frames and the ACKs to its own frames ([ieee802154.md](ieee802154.md)), but its ACK left milliseconds late: the ring handed samples over in units of 768 us, the PHY lagged most of a unit behind, and stopping the ring and starting a DAC session took a millisecond each. This page describes the ACK path that was built, its results, and the measurements it was designed from; what is still open is in [ieee802154.md](ieee802154.md).

## The deadline

- An Imm-Ack starts aTurnaroundTime after the last symbol of the frame it acknowledges: 12 symbols, 192 us.
- The sender waits macAckWaitDuration = 54 symbols (864 us) from the end of its frame for the whole ACK: 20 (backoff period) + 12 (turnaround) + 10 (SHR) + 12 (PHR and the 5-byte frame). The ACK lasts 22 symbols (352 us), so a sender that follows the formula accepts an ACK that starts up to 32 symbols, 512 us, after its frame.
- The binding limit is the peer's. Neither the Espressif HAL nor Zephyr's ESP32 driver sets the ESP32-C6's ACK timeout (`IEEE802154_ACK_TIMEOUT_REG`, 16 bits in units of 16 us), so the C6 runs on the hardware default; `esp_ieee802154_get_ack_timeout()` reads it.
- Target: start 192 us after the frame. Earlier is not better (the sender may still be turning its receiver on); later is accepted up to the peer's limit and counted; an ACK that would start beyond the limit is skipped.

## Built

With `CONFIG_ESP_SDR_IEEE802154_AUTO_ACK` (default y) the driver reports `IEEE802154_HW_RX_TX_ACK` and `IEEE802154_HW_FILTER`:

- **Ring** (`esp_sdr_ring_cfg::spare_bank`, `esp_sdr_ring_burst_*()`): units of 1024 pairs, rotation through banks 0 and 1 only, so bank 2 is the CPUs' for the ACK waveform for the whole run. Armed with the frame's end pair, the run builds the rest of the waveform between its polls, stops at the first unit boundary at or after the end, hands the last unit to the PHY, switches the front end to TX without the settle wait, plays bank 2 once when CPU 0 fires (or drops it on a cancel or at the limit), switches back to RX and restarts the capture. The pair index keeps counting real time over the gap, so the PHY resets there as at any gap.
- **PHY** (`ieee154_phy_set_hdr_cb()`): reports the length and the predicted end at the PHR and each further byte.
- **Driver:** decides from the frame control, PAN ID and destination address (`filter()`, not in promiscuous mode) whether an ACK is due; arms with the sequence number; fires `ack_delay` (192 us) after the end when the FCS passes, cancels when the PHY is past the end without a verdict. The first 416 slots of every ACK (preamble, SFD, PHR, frame control low nibble) go to bank 2 before each run. While armed the PHY thread does not sleep and runs at `CONFIG_ESP_SDR_IEEE802154_ACK_PRIORITY`. Not done: Enh-Ack (frame version 2), frames without a destination address to a PAN coordinator, the frame pending bit (always 0).
- **wpan:** `wpan set ack <delay>[,<limit>]`, `wpan bench acktx [seq]` (one ACK on the air now), counters in `wpan status`.

Measured with the C6 sending to the S3 (`wpan tx 200 10 <payload> 0x0001 1`, the C6 reports ok when its hardware got the ACK), coax with 30 dB:

| Case | Result |
| --- | --- |
| Channel 15, payloads 8, 16, 50, 100 bytes | 200 of 200 ACKs each |
| Channels 11, 20, 25, payloads 8 and 100 | 197 to 200 of 200; each miss a frame the S3 did not receive (the PHY's sample skips, see below) |
| Promiscuous, broadcast, another address | no ACK sent |
| Start after the frame's end | 192 us for 397 of 400; the rest up to about 350 us |
| Verdict (FCS checked) after the end | average 115 to 125 us, at most 150 to 226 us |
| Front end in TX after the end | 86 us typically, at most 208 us (waveform still being built after the stop for short frames) |
| Capture running again after the end | about 670 us (the ACK ends at 544 us) |

Steps that got there: the front end's 50 us settle no longer delays an ACK (late ACKs 35 of 400 went to 0); the PHY thread's raised priority while armed (34 to 17 of 400 before that). The interop test (frames both ways, the S3's own ACK requests) passes as before on the two-bank ring.

## Measurements before the build (2026-10-12)

Measured on the XIAO S3 against the C6, coax with 30 dB between them, channel 15 unless noted. The tools for them stay in the tree: the ring's unit length as a run parameter (`esp_sdr_ring_cfg::unit_pairs`), `esp_sdr_ring_now()`, `ieee154_phy_rx_position()`, the driver's verdict counters, `wpan set unit` and `wpan bench ack`.

- **The C6's ACK timeout is 1728 us** (`wpan status` on the C6: 108 units of 16 us, the hardware default). Far more lenient than the 864 us of the standard; other peers will not be.
- **Verdict latency** (frame's last sample to the PHY's frame callback, from the ring's live pair index), 200 frames of 16 bytes:

  | Unit (pairs) | min | avg | max (us) |
  | --- | --- | --- | --- |
  | 12288 | 751 | 917 | 1067 |
  | 4096 | 272 | 522 | 865 |
  | 2048 | 146 | 273 | 829 |
  | 1024 | 84 | 104 | 177 |

  With 100-byte frames on channels 11 to 25 at 1024: average about 118 us, maxima 135 to 414 us (the tail is CPU 0 contention, see below).
- **The ring at 1024-pair units** keeps up: 8.5 cycles per pair (subsampling sink included, budget 15), worst switch 60 to 137 pairs late (limit 896), no unit abandoned over minutes. This needed a faster subsampling path: 9.3 cycles per pair with an empty sink before (every store reloaded the run state, the build does not assume strict aliasing), 3.05 after. The driver now runs 1024-pair units always: with the faster path, 12288-pair units arrive as one burst larger than the PHY's "fell behind" margin, and 73 of 200 frames were lost that way.
- **CPU 0 contention:** the PHY (87 % of CPU 0, lowest priority) still skips about 0.2 % of the samples while idle and a burst at each ring restart. With 1024 units interop is 197 to 200 of 200 per direction and 48 to 49 of 50 ACKs received; a skip at a frame end is also a lost ACK.
- **ACK waveform**, scalar slot copies into the capture bank: chips 3.2 to 3.8k cycles (16 us); all 14100 words 60 to 63k cycles (**251 us**); from slot 416 on, the part that depends on the sequence number, frame pending bit and FCS, 25k cycles (105 us). About 85 cycles per 20-word slot.
- **Front end**, `esp_sdr_tx_loop_begin()` / `_end()` with the 50 us settle: RX to TX 143 to 156 us, TX to RX 140 to 158 us. Per call, in cycles at 240 MHz:

  | RX to TX | cycles | TX to RX | cycles |
  | --- | --- | --- | --- |
  | `rom_pbus_workmode()` | 1811 | `phy_stop_tx_tone(1)` | 4512 |
  | `rom_pbus_xpd_rx_on(0)` | 753 | `force_txon_mode(0, ...)` | 365 |
  | `rom_pbus_xpd_tx_on(1)` | 1092 | workmode, xpd tx off, rx on, rxclk | 2533 |
  | `force_tx_gain()` | 9013 to 9511 | `force_rx_gain()` | 9767 to 11822 |
  | `tx_lo_select()` (I2C) | 2766 | `rx_lo_select()` (I2C) | 1720 |
  | `force_txon_mode(1, ...)` | 2227 | filter code (I2C) | 1412 |

  The two gain calls are half of it. The ACK's switch to TX leaves out the settle wait (see "Built").
