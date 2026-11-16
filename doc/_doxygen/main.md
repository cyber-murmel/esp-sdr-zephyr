# esp-sdr-zephyr API

API reference of the esp_sdr library: raw I/Q receive and transmit on the
ESP32-S3 Wi-Fi radio (bursts, triggered snapshots, paced streaming and gapless
receive on the ESP32-C6, bursts, paced streaming, the diagnostic-bus ring and
replay on the ESP32-C5), and the IEEE 802.15.4 software PHY and radio driver
built on it. See the [topics](topics.html) for the parts:

- @ref esp_sdr : @ref esp_sdr_radio, @ref esp_sdr_rx, @ref esp_sdr_tx,
  @ref esp_sdr_ring, @ref esp_sdr_chancap, @ref esp_sdr_paced, @ref esp_sdr_trig,
  @ref esp_sdr_gapless, @ref esp_sdr_diag, @ref esp_sdr_replay and @ref esp_sdr_dsp.
- @ref ieee154 : @ref ieee154_phy and @ref ieee154_esp_sdr.

The Zigbee stack of `lib/zigbee` (`include/zigbee/`, portable C behind the
Z-Stack ZNP protocol of `apps/zstack`) documents its headers in plain comments
and is not part of this reference.

Declarations that depend on the SoC or on Kconfig are all listed here; each
says what it needs. The guides (hardware quirks, the 802.15.4 design,
testing) are in the project documentation built from `doc/` with Sphinx.

Source: [esp-sdr-zephyr](https://github.com/cyber-murmel/esp-sdr-zephyr).
