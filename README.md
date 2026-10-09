# ESP-SDR on Zephyr

A software defined radio on the ESP32-S3 Wi-Fi radio, for Zephyr: raw I/Q
capture from the receiver and I/Q playback through the transmit DAC, ported
from [ESPARGOS esp-sdr](https://github.com/ESPARGOS/esp-sdr) and extended.
This repository is a west manifest repository and a Zephyr module, laid out
like [example-application](https://github.com/zephyrproject-rtos/example-application).

## Contents

| Path | What |
|------|------|
| `lib/esp_sdr/` | The `esp_sdr` library (`CONFIG_ESP_SDR`): capture, tuning, gain, TX backends, continuous ring receive |
| `lib/ieee802154/` | IEEE 802.15.4 on the library (`CONFIG_ESP_SDR_IEEE802154`): software O-QPSK PHY and a Zephyr radio driver, see [doc/ieee802154.md](doc/ieee802154.md) |
| `include/esp_sdr/` | The API: [esp_sdr.h](include/esp_sdr/esp_sdr.h) (shared), [esp_sdr_rx.h](include/esp_sdr/esp_sdr_rx.h), [esp_sdr_tx.h](include/esp_sdr/esp_sdr_tx.h), [esp_sdr_ring.h](include/esp_sdr/esp_sdr_ring.h); 802.15.4: [ieee154_phy.h](include/esp_sdr/ieee154_phy.h), [ieee154_esp_sdr.h](include/esp_sdr/ieee154_esp_sdr.h) |
| `apps/capture/` | Minimal capture survey on the console |
| `apps/sdr_stream/` | VITA 49.2 RX and TX over USB (CDC-NCM, UDP/IPv6), host tools |
| `apps/osmosdr/` | HackRF style USB SDR for osmosdr / GNU Radio: gapless decimated RX, TX, vendor bulk protocol |
| `apps/link/` | Packet link at 80 or 40 MS/s: single carrier QAM or OFDM (RS/Hamming, CSMA/CA, iperf style test) |
| `apps/wpan/` | `wpan` shell: 802.15.4 test frames and counters on any Zephyr radio, the software radio (S3) or a native one (C6) |
| `apps/common/` | Shared by the apps: USB with DFU, watchdogs, crash records, thread pinning |
| `tests/unit/` | Unit tests on the host (ztest, `native_sim`): `scripts/run-unit-tests.sh`; the 802.15.4 PHY and driver error rate tables: `scripts/unit-tables.sh`; all test tiers and CI (`.github/workflows/build.yml`) in [doc/testing.md](doc/testing.md) |
| `tests/integration/` | ztest on one real board: the radio API's contract |
| `tests/regression/` | pytest on two real boards: 802.15.4 frames between the C6 and the S3 software radio |
| `doc/` | Guides (Markdown) and the documentation build: Doxygen API reference of `include/`, Sphinx site, see [Documentation](#documentation) |
| `scripts/west_commands/dfu.py` | `west dfu`: DFU update, health check and confirm of a running board |
| `zephyr/module.yml` | Module definition and the `librftest.a` blob |
| `west.yml` | The workspace: Zephyr, hal_espressif, libvrt, upstream esp-sdr, esp-dsp |

## Getting started

```
west init -m https://github.com/cyber-murmel/esp-sdr-zephyr esp-sdr-ws
cd esp-sdr-ws
west update
west blobs fetch hal_espressif
west blobs fetch esp-sdr-zephyr
west build --sysbuild -b xiao_esp32s3/esp32s3/procpu esp-sdr-zephyr/apps/osmosdr
west flash
```

See [apps/osmosdr/README.rst](apps/osmosdr/README.rst) for the host
setup, DFU updates and the tools, and
[doc/hardware-quirks.md](doc/hardware-quirks.md) for the radio, engine, memory
layout and toolchain peculiarities and how the code handles them.

## The library

- ESP32-S3 (tested on the Seeed XIAO ESP32S3), SMP or single core. The
  ESP32-C6 (Seeed XIAO ESP32C6) has receive capture only: 80 MS/s bursts
  into SRAM block 2 (0x40840000), analog filter and fixed gain; no transmit
  (its RF test library has no DAC playback), no ring and no 40 or 16 MS/s
  rates; the CIC decimated and folded captures work at 80 MS/s. Of the
  apps, `capture` and `wpan` (native radio) run on it; the others
  need the S3's USB OTG port, PSRAM or transmit.
- Tuning: the API takes 100 to 6000 MHz, but the PLL only locks over a range
  that differs from chip to chip: measured 2180 to 2793 MHz on an S3 and 2128
  to at least 2856 MHz on a C6. Requests from 1842 to 2209 MHz go through the
  5/6 LO divider (PLL at 1.2 times the frequency), for receive and transmit,
  so the usable range on those two boards starts at 1842 MHz and ends at 2793
  MHz (S3) and above 2856 MHz (C6). Outside it the radio does not lock and
  receives or sends nothing useful.
- Capture: bursts of 256 to 16380 complex samples at 80, 40 or 16 MS/s, into
  either dump bank (`CONFIG_ESP_SDR_BANK1`), analog low-pass filter by
  bandwidth or raw code, and optionally (`CONFIG_ESP_SDR_RX_DECIM`) a CIC
  decimated capture (16 MS/s / m).
- Gain: hardware AGC or a fixed gain index (`esp_sdr_rx_set_gain()`, 0 to
  `esp_sdr_rx_gain_max()`, not dB). The fixed gain uses `force_rx_gain()` from
  Espressif's `librftest.a` (Apache-2.0, the esp-phy-lib commit matching the
  hal_espressif `libphy`), a module blob: `west blobs fetch esp-sdr-zephyr`,
  or `CONFIG_ESP_SDR_RFTEST=n` for AGC only (that also drops the TX
  power steps).
- Transmit: `esp_sdr_tx_play()` and `esp_sdr_tx_sweep()` play samples through the
  Wi-Fi DAC at 40 or 80 MS/s, with an 18 step power ladder
  (`esp_sdr_tx_set_gain()`). `CONFIG_ESP_SDR_TX_DAC` (needs PSRAM) streams
  arbitrary I/Q at 40 MS/s / n: a PSRAM ring, linear interpolation with the
  S3 vector unit, and the DAC in loop mode over two SRAM banks, so playback
  is gapless and locked to real time. `esp_sdr_tx_loop_*()` plays prepared
  banks in a loop directly.
- Front end: `esp_sdr_set_turnaround()` shortens the TX/RX switch (3 ms by
  default, 50 us works for packets), `esp_sdr_set_channel_bw()` passes the
  PHY's channel bandwidth mode (2 is its 40 MHz mode, wider analog filters),
  `esp_sdr_tx_set_lpf()` sets the TX low-pass codes.

Enable it with:

```
CONFIG_NETWORKING=y
CONFIG_WIFI=y
CONFIG_ESP32_WIFI_STA_POWER_SAVE_NONE=y
CONFIG_ESP_SDR=y
```

The Wi-Fi driver brings the radio up at boot; `esp_sdr_init()` then switches
it to promiscuous mode on channel 1 and prepares the receiver. Do not scan,
connect or start an access point while capturing: those retune the radio.

## Documentation

`doc/` builds the documentation as Zephyr's
[example-application](https://github.com/zephyrproject-rtos/example-application)
does: Doxygen for the API reference of everything under `include/`, Sphinx for
the site (this README, the guides in `doc/` and the apps' READMEs), with the
API reference linked in. In the nix shell:

```
cd esp-sdr-zephyr/doc
doxygen
make html
```

The site is in `doc/_build_sphinx/html` (open `index.html`), the API reference
in its `doxygen/` folder. Both treat warnings as errors in CI
(`.github/workflows/docs.yml`, which also publishes to GitHub Pages);
`SPHINXOPTS=-W make html` does the same locally. Outside the shell,
`pip install -r doc/requirements.txt` and a Doxygen install give the same tools.

## Dependencies

`west.yml` pins forks where this work needs changes not yet upstream:

- [Zephyr `esp-sdr`](https://github.com/cyber-murmel/zephyr/tree/esp-sdr): ESP32-S3 SMP (zephyrproject-rtos/zephyr#120082,
  rebased) and CDC-NCM NTB aggregation.
- [hal_espressif `esp-sdr`](https://github.com/cyber-murmel/hal_espressif/tree/esp-sdr): the HAL side of the SMP port.
- [libvrt `esp-sdr`](https://github.com/cyber-murmel/libvrt/tree/esp-sdr): VITA 49.2 support and a Zephyr module.
- [esp-sdr (upstream, unmodified)](https://github.com/ESPARGOS/esp-sdr): the receiver helper headers.

## License

GPL-3.0-or-later, like upstream esp-sdr. Images that enable the library are
subject to the GPL.
