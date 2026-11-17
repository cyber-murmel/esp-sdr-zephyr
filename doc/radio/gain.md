# Gain

This page covers the receive gain, one index or the hardware AGC, and the transmit power, and where
each of them breaks. It builds on the baseband amplifier and transmit gain blocks of
[Transceiver](transceiver.md), which list the registers.

## Forced gain and the AGC

### Gain index

`esp_sdr_rx_set_gain()` takes either `ESP_SDR_RX_GAIN_AUTO`, which leaves the gain to the hardware
AGC, or a fixed index.

- `esp_sdr_rx_gain_max()` returns the largest calibrated index.
- The S3 register page lists the noise floor with a 50 ohm load per index
  ([AGC_FORCE_REG](../dev/registers-esp32s3.md#register-agc_force_reg-0x6001c02c)).

### librftest.a

A fixed receive gain and the transmit power steps use Espressif's `librftest.a`, fetched with
`west blobs fetch esp-sdr-zephyr`. `CONFIG_ESP_SDR_RFTEST` (default y) links it.

| Needs `librftest.a` | Through |
|---|---|
| a fixed receive gain on the S3, C6 and C5 | `force_rx_gain()`; `ESP_SDR_RFTEST` links the C5's librftest for it |
| the S3's 18 transmit power steps | `force_tx_gain()`, `force_txon_mode()` |
| S3 transmit streaming, `CONFIG_ESP_SDR_TX` (needs PSRAM too) | `lib/esp_sdr/Kconfig` |
| the paced streams of the C6, C5 and S31, `CONFIG_ESP_SDR_PACED` | `lib/esp_sdr/Kconfig` |
| the S31's whole radio, always | it comes up through `esp_phy_rftest_config()`, which lives in that library |

- [ESP-SDR on Zephyr](../../README.rst) says `CONFIG_ESP_SDR_RFTEST=n` leaves transmit out.
- The Kconfig help (`lib/esp_sdr/Kconfig`) says the receiver then runs on the hardware AGC only
  and transmit keeps the PHY's own power setting.
- The classic ESP32 has no such blob: it forces its gain by writing AGC_FORCE_REG directly.

### Level and noise floor per step

The Ubertooth app on a XIAO ESP32-S3 (2026-11-11, a busier office) measured what an index step
does. One transmitter at a fixed power, a XIAO ESP32-C6, read against the gain index:

| Gain index | 60 | 56 | 52 | 48 | 44 | 40 | 36 | 32 | 28 |
|---|---|---|---|---|---|---|---|---|---|
| Level, dB lower than at 60 | 0 | 6.1 | 8.4 | 11.4 | 17.0 | 21.1 | 25.2 | 28.6 | 32.7 |

- That is about 1 dB a step.
- At index 60 the PC's Bluetooth adapter (a metre away, some +10 dBm) and the C6 at its default
  power arrive at full scale (the RSSI pins at -64 to -65 dBm on the placeholder scale). Their
  packets come out with bit errors or not at all; no CONNECT_IND of the PC was caught in five
  tries.
- The stream's noise floor (`ubt rx floor`, 2480 MHz) falls only 4.5 dB from 60 to 50 and stays
  at -66 to -70 dBFS a bin below that. The receiver after the gain stage sets it, not the 8 bit
  ring: a ring four times finer moved it 1 to 2 dB.
- So below 50 every step down costs sensitivity. On ambient advertisers (channels 37 and 38,
  interleaved 15 s runs) gain 60 heard 38 to 44 devices, gain 50 29 to 36.
- The app's default stays 60; `ubt rx gain` sets another for a connection close by.

### Gain tables per chip

[Transceiver](transceiver.md#baseband-amplifier-and-agc) gives the register field that holds the
largest calibrated index. Where the tables differ:

- **ESP32-C5:** the largest calibrated gain index is 77 at 2412 MHz and 83 at 5500 MHz. Its DC
  grows with the index ([Impairments](impairments.md#esp32-c5)).
- **ESP32-S31:** a forced index only reads as itself when its low-table word is mirrored into the
  high table. Quiet-band noise rises monotonically with the index, 22 dB over the table (max 71
  here).

#### ESP32-C6 digital gain

In the `apps/pcap` channel mode on the WHY2025 badge ([pcap internals](../apps/pcap-internals.md)),
automatic gain means gain 60. The C6's digital gain is 0 dB whatever the analog gain (24 below its
8-bit default): the floor sits some 37 dB under full scale, still a few steps of the 8-bit items.

Packets decoded at the desk on channel 37 in 10 s, by analog gain:

| Gain | Packets | Note |
|---|---|---|
| 30 | 16 of one device | the noise floor at -20 dBFS |
| 40 | 48 of 7 devices | |
| 50 | 64 of 15 | |
| 60 | 161 to 214 of 23 to 30 | |
| 64 | 100 | the nearest transmitters clipped |
| the PHY's own gain control | 74 | |

With 18 dB of digital gain an 802.15.4 sender on the desk clipped and never decoded. On
channel 37, 0 dB of digital gain decoded 1937 Bluetooth LE packets in 15 s, 6 dB 1755, 18 dB 1691.

### AGC idles high

On a quiet band the hardware AGC winds the gain up. That raises the floor and costs packets:

- **ESP32-S3:** the AGC idled 24 dB above index 60 and halved the Ubertooth app's catch rate
  (2026-11-11).
- **ESP32-S31:** on a quiet band the AGC winds up to AGC_THRESH_REG's index, which the PHY sets to
  the table's top (71 here): noise at 10 8-bit steps rms, -22 dBFS, with the DC 5 to 9 steps off.
  The library caps it at index 60: 2.5 steps rms, -34 dBFS, DC some 2 steps. `AGC_GAIN_MAX` and
  `AGC_INIT_INDEX` change nothing there.
- **ESP32-C5:** the AGC idles high too; its DC under the AGC is in
  [Impairments](impairments.md#esp32-c5).

### Lost forced gain

A manual receive gain (`esp_sdr_rx_set_gain()`, forced through the vendor blob's
`force_rx_gain()`) is occasionally lost after minutes of traffic, on either board of the S3 link.

#### Effect

- The Wi-Fi AGC takes over.
- With nothing on air it winds the gain up, and the quiet noise floor rises by up to 30 dB.
- The cause is unknown.

#### Symptoms

- Carrier sense reads a busy channel all the time.
- A sender waiting for an ACK never transmits again.
- A receiver clips strong frames and decodes bursts of corrupted headers.
- It looks like hardware damage, but re-applying the gain cures it at once.
- Comparing the AGC force register (`0x6001c02c`) does not detect it: `force_rx_gain()` writes
  other registers too.

#### Handling

- The library re-forces the gain at a capture when the last force is more than 20 ms old
  (`esp_sdr_rx_gain_refresh()` in `lib/esp_sdr/src/esp_sdr_rx.c`, `GAIN_REFRESH_MS`).
- Each re-force costs 14 to 51 us. `esp_sdr_get_stats()` counts them, and `link status` prints
  them on its engine line.
- A turnaround back to receive also forces the gain.
- A ring run ([Gapless receive](../samples/rings.md)) re-forces between units: every 20 ms when
  it sends bursts, every second when it only receives. A re-force is longer than the slack of
  short units: `apps/chancap`'s 128 us units dropped a few pairs at each one.

#### Debugging

When debugging this kind of fault: any `link set` in `apps/link` re-applies the whole radio
configuration, which hides it.

## Clip guard

A receiver at a high gain clips a transmitter close by. The clip guard (`esp_sdr_gain_guard.h`,
`CONFIG_ESP_SDR_GAIN_GUARD`) takes the receive gain down a level when packets reach full scale and
up again after a quiet hold, a hold that grows when the source comes in bursts.

The header documents its thresholds; [ubertooth](../../apps/ubertooth/README.rst) shows its use
(`ubt rx gain guard`).

The Ubertooth app's guard holds twice as long each time it has to step down again within a minute
of stepping up, from 10 s up to 160 s, for sources that come in bursts. The board did not put the
doubling to the test (2026-11-12, night):

- Sniffing channel 39 under the guard while the PC scanned 3 s in every 40, advertisers near the S3
  took either version of the guard, with or without the doubling, down to the last level (gain 36)
  within seconds.
- Those advertisers had random addresses, printed -41 to -50 dBm, 16 to 25 dB past full scale at
  gain 60. They held the gain there while they did not pause for 10 s.
- The old guard, given two such pauses, climbed and came back down twice: 7 changes and 225 packets
  at full scale in 4 minutes, against 3 and 12.
- Such sources are the ones a channel at DC takes worst: at gain 60, without the guard, they reach
  the ADC that far past full scale ([Impairments](impairments.md#adc-clipping-products)).

## Receive headroom

Through the 30 dB attenuator of the S3 link, at TX step 4, RX gain 32 is the working point and 44
saturates the receiver (MER 4 dB). MER is the modulation error ratio of the decoded symbols;
[QAM and OFDM](../modem/qam-ofdm.md) gives the MER ceilings per signal.

## Transmit gain

### TX gain ladder

On the S3, `esp_sdr_tx_set_gain()` picks one of 18 power steps, 0 to 17 (`esp_sdr_tx_gain_max()`
the strongest). Each step goes through librftest's `force_tx_gain(gain, bb, dig)`:

- The ladder is the vendor's table in `phy_tx_gain.o` `.rodata`: gain at 0x04, bb at 0x16, power at
  0x3a in quarter dB.
- bb, the baseband gain code, is 0, 0x20, 0x80, 0xa0 or 0x100; dig, the digital gain, stays 0.
- The ladder spans 46.3 dB and is monotonic.
- `esp_sdr_tx_gain_power()` returns a step's vendor target power in quarter dB, relative, not dBm:
  4.24 units per dB measured over the ladder.
- Evidence: *decompile* S3 `phy_tx_gain.o`; *working* `esp_sdr_tx_gain_power()` in
  `include/esp_sdr/esp_sdr_tx.h`; *measured* bench 2026-10-05.

### Level per chip

The other chips have no gain ladder. What sets the transmit level:

| Chip | Path | What sets the level | Measured |
|---|---|---|---|
| ESP32-C6 | TX DC value, paced | the `power` argument: the vendor CW's backoff, 8 quarter dB a step ([Transceiver](transceiver.md#transmit-gain)) | |
| ESP32-C5 | TX DC value and DAC engine | the samples times the DAC scale (`0x600a0c04` bits 23:8, one byte per branch); the tone stop leaves full scale, the library sets 15 as upstream | on the C5-Zero bench the power step of the vendor tone does nothing on either path: steps 0 to 15 gave the same level; the [osmosdr_serial](../../apps/osmosdr_serial/README.rst) README gives 0 to 15 in 2 dB steps |
| ESP32-S31 | carrier from the tone generator | the attenuation, in quarter dB, from the vendor's least, 24, to the generator's most, 127 | 1.4 dB a step of 6, 21 dB from the strongest to the weakest, carriers from 2420 to 2700 MHz |
| ESP32-S31 | TX DC value, paced | the samples only: neither `0x20100c04` (the DAC scale bytes) nor the tone's attenuation changes it | sample I 100 is -21.6 dBFS on the XIAO at gain 10, the same on Q and for -100 |
| ESP32-S31 | DAC engine, replay | the samples; the DAC scale bytes (`0x20100c04`) act here, but not as a gain | against `0xff`: `0x40` is +10.7 dB, `0x10` +3.9 dB and `0x80` -30 dB; the library keeps `0xff` unless asked |

The S31 levels were heard on a XIAO ESP32-S3 a few cm from the VIEWE board.

### Transmitter compression

The analog transmit chain compresses on every chip measured, on the S3 long before the DAC words
clip:

| Chip | Signal | Where it compresses | Products |
|---|---|---|---|
| ESP32-S3 | OFDM, 116 subcarriers | at an RMS amplitude of 120 DAC units the subcarriers drop to 16 dB MER; at 60 to 90 they reach 26 to 29 dB | |
| ESP32-C5 | tone | above amplitude times DAC scale of about 1000 to 1500 | third harmonic at -16 dBc with 400 x 15; at 50 x 15 the image is 32 dB down and the third harmonic 46 dB |
| ESP32-S31 | tone, paced | from 24000 to 32767 the tone grows only 0.5 dB | the third-order product at -3f rises from -31 to -20 dBc (-41 at 16000) |

#### ESP32-S3

- Wide multicarrier signals distort in the analog TX path long before the 10-bit DAC words reach
  full scale.
- The digital clip counter stays at 0, and lowering the RX gain does not help, which places it in
  the transmitter.
- The OFDM default is `amp 90`.

#### ESP32-C5

- Once the chain came up 28 dB louder with the same settings, a clean tone, not reproduced since.

#### ESP32-S31

- The compression is the S31's: the knee has the same shape at two XIAO gains 10 dB apart.
- Full scale is 16384 (256 DAC counts); the library maps 32767 to 511.
