# ESP-SDR on Zephyr

A software defined radio on the ESP32-S3 Wi-Fi radio, for Zephyr: raw I/Q
capture from the receiver and I/Q playback through the transmit DAC, ported
from [ESPARGOS esp-sdr](https://github.com/ESPARGOS/esp-sdr) and extended.
This repository is a west manifest repository and a Zephyr module, laid out
like [example-application](https://github.com/zephyrproject-rtos/example-application).

## Contents

| Path | What |
|------|------|
| `lib/esp_sdr/` | The `esp_sdr` library (`CONFIG_ESP_SDR`): capture, tuning, gain, TX backends |
| `include/esp_sdr/` | Its API: [esp_sdr.h](include/esp_sdr/esp_sdr.h) (shared), [esp_sdr_rx.h](include/esp_sdr/esp_sdr_rx.h), [esp_sdr_tx.h](include/esp_sdr/esp_sdr_tx.h) |
| `apps/capture/` | Minimal capture survey on the console |
| `apps/sdr_stream/` | VITA 49.2 RX and TX over USB (CDC-NCM, UDP/IPv6), host tools |
| `apps/osmosdr/` | HackRF style USB SDR for osmosdr / GNU Radio: gapless decimated RX, TX, vendor bulk protocol |
| `apps/link/` | QAM packet link at 80 MS/s (RS/Hamming, CSMA/CA, iperf style test) |
| `apps/osmosdr/` | HackRF style USB SDR for osmosdr / GNU Radio: gapless decimated RX, TX, vendor bulk protocol |
| `apps/common/` | Shared by the apps: USB with DFU, watchdogs, crash records, thread pinning |
| `scripts/esp-sdr-update.sh` | DFU update and confirm of a running sdr_stream board |
| `zephyr/module.yml` | Module definition and the `librftest.a` blob |
| `west.yml` | The workspace: Zephyr, hal_espressif, libvrt, upstream esp-sdr |

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
[doc/hardware-quirks.md](doc/hardware-quirks.md) for the radio, engine and
toolchain peculiarities and how the code handles them.

## The library

- ESP32-S3 only (tested on the Seeed XIAO ESP32S3), SMP or single core.
- Capture: bursts of 256 to 16380 complex samples at 80, 40 or 16 MS/s from
  100 to 6000 MHz (5/6 LO mode at 1842 to 2209 MHz), into either dump bank
  (`CONFIG_ESP_SDR_BANK1`), analog low-pass filter by bandwidth or raw code,
  and optionally (`CONFIG_ESP_SDR_RX_DECIM`) a CIC decimated capture
  (16 MS/s / m).
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

### Memory

The capture engine reads and writes only fixed SRAM banks, and while it owns a
bank the CPUs cannot reach any of it. The library reserves bank 2
(0x3fcd0000) for capture, and with `CONFIG_ESP_SDR_TX_DAC` bank 1
(0x3fcc0000) as the second DAC bank, through linker snippets that also lower
`_heap_sentry`. The link fails if the image grows into a bank. The SRAM
between bank 2 and the bootloader's loader segment is available for buffers
with `ESP_SDR_HIGH_RAM` (not zeroed at boot).

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
