# API reference

API reference of the esp_sdr library, raw I/Q receive and transmit on the Wi-Fi radio of the
ESP32-S3, ESP32-C6, ESP32-C5, ESP32-S31 and the classic ESP32, and of the IEEE 802.15.4 software
PHY and radio driver built on it. A group limited to some chips names them in its brief; the
matrix of sample paths per chip is in [Samples in and out](../samples/index.html).

## Groups

The [topics](topics.html) list the library's groups in the order the guides teach them, then the
IEEE 802.15.4 groups. Each group below names its header and the guide that explains it.

The library's groups are subgroups of @ref esp_sdr; the IEEE 802.15.4 groups are subgroups of
@ref ieee154.

### The radio

| Group | Header | Guide |
| --- | --- | --- |
| @ref esp_sdr_radio | `esp_sdr.h` | [Transceiver](../radio/transceiver.html), [Tuning](../radio/tuning.html) |
| @ref esp_sdr_rx | `esp_sdr_rx.h` | [Gain](../radio/gain.html), [Filters](../radio/filters.html), [Burst capture](../samples/bursts.html) |
| @ref esp_sdr_gain_guard | `esp_sdr_gain_guard.h` | [Gain](../radio/gain.html) |
| @ref esp_sdr_tx, with @ref esp_sdr_tx_loop | `esp_sdr_tx.h` | [Transmit](../samples/transmit.html) |
| @ref esp_sdr_cal | `esp_sdr_cal.h` | [Crystal calibration](../radio/crystal-calibration.html) |
| @ref esp_sdr_lte | `esp_sdr_lte.h` | [Crystal calibration](../radio/crystal-calibration.html) |
| @ref esp_sdr_arb | `esp_sdr_arb.h` | [Coexistence](../radio/coexistence.html) |

### Samples in and out

| Group | Header | Guide |
| --- | --- | --- |
| @ref esp_sdr_trig | `esp_sdr_trig.h` | [Burst capture](../samples/bursts.html) |
| @ref esp_sdr_ring, with @ref esp_sdr_ring_burst | `esp_sdr_ring.h` | [Gapless receive](../samples/rings.html) |
| @ref esp_sdr_gapless | `esp_sdr_gapless.h` | [Gapless receive](../samples/rings.html) |
| @ref esp_sdr_diag | `esp_sdr_diag.h` | [Gapless receive](../samples/rings.html) |
| @ref esp_sdr_diag16 | `esp_sdr_diag16.h` | [Gapless receive](../samples/rings.html) |
| @ref esp_sdr_dumpring | `esp_sdr_dumpring.h` | [Gapless receive](../samples/rings.html) |
| @ref esp_sdr_replay | `esp_sdr_replay.h` | [Transmit](../samples/transmit.html) |
| @ref esp_sdr_paced | `esp_sdr_paced.h` | [Paced streams](../samples/paced.html) |

### Modulation and coding

| Group | Header | Guide |
| --- | --- | --- |
| @ref esp_sdr_chancap | `esp_sdr_chancap.h` | [Channelizer](../modem/channelizer.html) |
| @ref esp_sdr_dsp | `esp_sdr_dsp.h` | |
| @ref ieee154_phy, in @ref ieee154 | `ieee154_phy.h` | [IEEE 802.15.4](../modem/ieee802154.html) |
| @ref ieee154_esp_sdr, in @ref ieee154 | `ieee154_esp_sdr.h` | [IEEE 802.15.4](../modem/ieee802154.html) |

### Support

| Group | Header | Guide |
| --- | --- | --- |
| @ref esp_sdr_place | `esp_sdr_place.h` | [Code placement](../dev/code-placement.html) |
| @ref esp_sdr_prof | `esp_sdr_prof.h` | [Code placement](../dev/code-placement.html) |

### Headers outside the groups

These headers belong to no group; each has a file page only.

| Header | What |
| --- | --- |
| `esp_sdr_xtensa.h` | ESP32-S3 CPU helpers: the cycle counter and the vector unit (PIE) |
| `esp_sdr_ring_stage.h` | The receiver's output staged in internal RAM for a consumer on the other CPU |
| `esdr_dma_ring.h` | The bookkeeping of a DMA receive ring whose memory can be handed away in slabs |
| `esdr_link.h` | The framed serial link between a host and `apps/osmosdr_serial` |
| `esdr_fpga.h` | The esp-sdr gateware on the MCH2022 badge's iCE40UP5K |
| `mch2022_coproc.h` | What `apps/mch2022_coproc` adds to the stock firmware of the MCH2022 badge's RP2040 |

`esdr_fpga_regs.h`, generated from the gateware, has no `@file` block and shows as source only.

## SoC and Kconfig dependencies

Declarations that depend on the SoC or on Kconfig are all listed here; each says what it needs.

## Not in this reference

The Zigbee stack of `lib/zigbee` (`include/zigbee/`, portable C behind the Z-Stack ZNP protocol of
`apps/zstack`) documents its headers in plain comments and is not part of this reference.

## Guides

The guides explain what the API does and why. Sphinx builds them from `doc/` and publishes them
with this reference: the overview, [ESP-SDR on Zephyr](../overview.html), then five parts:

- [The radio](../radio/index.html)
- [Samples in and out](../samples/index.html)
- [Modulation and coding](../modem/index.html)
- [Applications](../apps/index.html)
- [Reference](../dev/index.html)

## Source

The code is on GitHub: [esp-sdr-zephyr](https://github.com/cyber-murmel/esp-sdr-zephyr).
