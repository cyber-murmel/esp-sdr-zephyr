# Transmit

This page covers the engine paths into the radio: playback and gapless streaming on the ESP32-S3,
replay through the DAC engine on the C5 and S31, and the carrier. It ends with the turnaround
between receive and transmit. CPU-paced transmit follows in [Paced streams](paced.md); the word
format is in [Samples in and out](index.md#sample-word-formats).

## Turning the chain on

Before samples reach the antenna, the transmit chain must be on.

- **ESP32-S3 and ESP32-C6.** The front-end force-on and the vendor calls that switch the chain
  are in [Transceiver](../radio/transceiver.md), [ESP32-S3 registers](../dev/registers-esp32s3.md)
  and [ESP32-C6 registers](../dev/registers-esp32c6.md).
- **Switch cost.** What these calls cost is under [Front-end switch cost](#front-end-switch-cost).
- **ESP32-C5.** Both transmit paths, paced and replay, work after the vendor CW turned the chain on
  and its tone stopped.
- **ESP32-S31.** See below.

### ESP32-S31

- Chain on is the carrier's start at the LO as tuned ([Carrier](#carrier)), then libphy's
  `phy_stop_tx_tone_new()`.
- That turns both tones' enables off and the DAC's source back to data, which hands the DAC to the
  TX DC value, the chain left on. There is no retune.
- The ROM's `phy_stop_tx_tone()` tail-calls `phy_dac_scale_set(1)` and is not used.
- Chain off is the carrier's stop, then receive as at startup. At a fixed gain the floor afterwards
  is within 1.5 dB of before.

## Carrier

On the ESP32-S31, `esp_sdr_tx_tone_start()` runs the vendor's carrier test (librftest's
`wifiscwout()`) without its Wi-Fi channel:

- First the ROM's `phy_txcal_debuge_mode_()`: the PHY bus in debug mode, the TX front end on, a TX
  gain.
- Then libphy's `phy_start_tx_tone_step_new(1, kHz, att, 0, 0, 0)` at the LO the library tunes.
- `phy_txcal_work_mode()` ends it, and the next capture receives as before (the floor within
  1.5 dB).
- librftest's `fedump_wr_txmem()`, `fedump_rd_txmem()` and `fedump_rd_rxmem()` are empty here (a
  lone `ret`): replay drives the engine itself
  ([Replay through the DAC engine](#replay-through-the-dac-engine)).
- The attenuation and the levels it gives are in [Gain](../radio/gain.md#level-per-chip), the
  carriers' frequency error in [Impairments](../radio/impairments.md#clock-offset-between-boards).

## Playback

The ESP32-S3 plays words out of its banks ([SRAM bank ownership](index.md#sram-bank-ownership)).

### One-shot and loop

- One-shot calls (`esp_sdr_tx_play()`, `esp_sdr_tx_sweep()`) play prepared words from the dump bank.
- Loop playback (`esp_sdr_tx_loop_*()`) holds the trigger bit and wraps only while it is held;
  pulsing the bit plays one pass.
- Retriggered bursts (`esp_sdr_tx_play_for()`) leave a hole of about 1.35 us each.

### Rates

- Playback runs at 80 or 40 MS/s.
- How much modulation error ratio (MER) the DAC loses at 40 MS/s is in
  [QAM and OFDM](../modem/qam-ofdm.md).
- The 18 power steps are in [Gain](../radio/gain.md#tx-gain-ladder).

## Gapless streaming

`CONFIG_ESP_SDR_TX` (which needs `librftest.a` and PSRAM) streams samples to the DAC without a gap,
at 40 MS/s / n. [stream](../../apps/stream/README.rst) and [osmosdr](../../apps/osmosdr/README.rst)
feed it from a host; their READMEs have the measured rates.

### PSRAM ring and bank fill

- The network thread writes samples into a ring in PSRAM (`esp_sdr_tx_dac.c`) of 256k samples.
- The leader's filler stages each block's input in internal RAM and interpolates it into the idle
  one of two 64 KiB dump banks.
- It flips the bank select at the block's sample time, on a precomputed cycle of the leader's CPU.
- The DAC loops over the selected bank (`DAC_TRIG` bit 19 held; `LOOP_LEAD`, `LOOP_STEP`,
  `LOOP_TAIL`), so the output is gapless and locked to real time.

### Interpolation

- The interpolation runs on the S3's vector unit, or in the C fallback split across both cores.
- It is linear, to the DAC's 40 MS/s. What that leaves of the images is under Limitations in
  [osmosdr](../../apps/osmosdr/README.rst).

### Pacing from the host

- Input that arrives late plays as zeros.
- In osmosdr, USB flow control paces the host about 50 ms ahead of the air.

### Generator mode

`esp_sdr_tx_dac_play_gen()` is a generator mode of the DAC stream. The 802.15.4 driver transmits
its frames with it ([IEEE 802.15.4](../modem/ieee802154.md)).

## Replay through the DAC engine

`esp_sdr_replay_*()` (`esp_sdr_replay.h`, `CONFIG_ESP_SDR_REPLAY`) has the DAC engine play words
out of memory once, a number of times or endlessly, while the CPU runs on. The C6 has no replay in
the library. The engine owns its memory while it plays
([SRAM bank ownership](index.md#sram-bank-ownership)).

### ESP32-C5

- The DAC engine (`0x600a900c`) plays up to 16383 words (I 9:0, Q 19:10) out of the dump bank, at
  80 MS/s, or at 40 MS/s with bit 17 (upstream's layout; the C6 uses bit 15 instead).
- Bits 14 to 16 are no rate field on the C5: with them the pass time is not proportional to the
  count, and some codes never set DONE.
- A repeat count (bits 27:20, with LOOP) plays the passes back to back: 255 passes of 16000 words
  took 12240047 cycles against 12240000 for the samples.
- Repeat count 0 with LOOP plays until RUN clears, and its seam is invisible: a C6 capture folded at
  the loop period shows no step above 1 degree, where one lost sample would make 31.5.
- Restarting a finished pass from the CPU leaves a hole of 35 to 37 samples at 80 MS/s, about
  0.45 us: the poll, and about 0.25 us until the engine plays. So periodic content uses the repeat
  count.
- The level follows the samples times the DAC scale register; the register and the compression
  are in [Gain](../radio/gain.md#level-per-chip).

### ESP32-S31

The S31 has a DAC reader at `0x2010900c`, the dump block's third word as the C5's trigger. No
vendor code, ROM or upstream program writes it (upstream only clears bit 31 before receive), so all
of this was measured on the VIEWE board.

The tools were a scratch probe, rfprobe's `probe tx replay` and
`tests/integration/esp_sdr_replay_api`, heard on the XIAO ESP32-S3 (`tmp/s31/tx/replay_s*.py` and
`replay_lib*.py` in the workspace).

#### Fields

The fields are as on the C5 but for the rate:

| Field | Bits | Behaviour |
|---|---|---|
| RUN | 31 | plays while set |
| REPEAT | 27:20 | with LOOP, n passes back to back; 0 plays until RUN clears |
| LOOP | 19 | cleared while it plays, it ends the pass and sets DONE |
| DONE | 18 | read only |
| COUNT | 16:0 | counts up to `0x10000` finish; `0x10001` never sets DONE |

- Bit 17 does not latch: there is no rate bit.
- A finite pass does not hold its last word.

#### Rates

The rate is the capture's field, `0x20109008` bits 24:21:

| Field value | CPU cycles a word | Rate at 320 MHz |
|---|---|---|
| 0 | 4 | 80 MS/s |
| 1 | 8 | 40 MS/s |
| 3 | 16 | 20 MS/s |
| 4 | 24 | |

- The other values are unmeasured.
- 255 passes of 16000 words at 80 MS/s take the samples' cycles and 89 more.

#### Memory

- It plays the group that `HP_SYSTEM_TCM_DATA_DUMP_CTRL_REG` hands the engine, from its first word:
  `0x00ff0000` is the group at `0x2F040000`.
- With nothing owned it plays nothing; the upper group (`0xff000000`) never.
- The index wraps at the group's 32768 words.
- It writes nothing: the group, the vector table and the IRAM code read the same afterwards.
- The CPU runs on while it plays: sleeps, a 1 ms timer and a busy wait keep time, and the IRAM code
  is unchanged (the test).
- What the library reserves for it is in [SRAM bank ownership](index.md#sram-bank-ownership).

#### Words and level

- The words are the TX DC value's (I 9:0, Q 19:10), with the chain on as for the paced path.
- `0x2010040c`'s source bits do not matter.
- The DAC scale bytes and the levels they give are in [Gain](../radio/gain.md#level-per-chip);
  the image, the LO leakage and the level of a tone are in
  [Impairments](../radio/impairments.md#transmit-per-chip).

#### Interpolation around the loop

MicroPython's `esdr.tx_start(..., repeat=True)` loops a buffer through this engine
([micropython](../../apps/micropython/README.rst)):

- It plays out of the TCM group below the capture bank while the CPU runs on, until `tx_stop()` or
  `count` samples.
- The buffer is interpolated around the loop by a windowed sinc, to the highest of 80, 40 and
  20 MS/s that is a whole multiple of the rate and holds it in 32768 words: at most 409 pairs at
  250 kS/s.
- `tx_scale()` refills it, a pause of some milliseconds.
- The app's one LVGL draw buffer keeps the image below that group.

## Turnaround

Receive and transmit share one physical RF front end and antenna pin.

- `esp_sdr_lock` makes them mutually exclusive in time.
- `esp_sdr_set_turnaround()` sets the settle delay, and an optional retune, when the direction
  switches.
- The default switch with a retune takes about 3 ms, most of it the retune's settle
  ([Tuning](../radio/tuning.md#retune-time)).
- Without a retune, 50 us of settle is enough for packet links: `esp_sdr_set_turnaround()` with
  50 us and no retune.

### Front-end switch cost

Measured on 2026-10-12 on the XIAO S3 against the C6, with `esp_sdr_tx_loop_begin()` and `_end()`
and the 50 us settle: RX to TX 143 to 156 us, TX to RX 140 to 158 us. Per call, in cycles at
240 MHz:

| RX to TX | Cycles | TX to RX | Cycles |
|---|---|---|---|
| `rom_pbus_workmode()` | 1811 | `phy_stop_tx_tone(1)` | 4512 |
| `rom_pbus_xpd_rx_on(0)` | 753 | `force_txon_mode(0, ...)` | 365 |
| `rom_pbus_xpd_tx_on(1)` | 1092 | workmode, xpd tx off, rx on, rxclk | 2533 |
| `force_tx_gain()` | 9013 to 9511 | `force_rx_gain()` | 9767 to 11822 |
| `tx_lo_select()` (I2C) | 2766 | `rx_lo_select()` (I2C) | 1720 |
| `force_txon_mode(1, ...)` | 2227 | filter code (I2C) | 1412 |

- The two gain calls are half of it.
- The 802.15.4 ACK's switch to TX leaves out the settle wait
  ([IEEE 802.15.4](../modem/ieee802154.md)).

### Bursts from a ring run

A ring run can send one burst and go on receiving, with a spare bank for the waveform:
[Gapless receive](rings.md#spare-bank-and-bursts-from-a-run).

### ESP32-C5 half duplex

- The C5 is half duplex only: while the chain transmits, the receive output freezes at a constant.
- Turning the receive power and clock back on (`phy_pbus_xpd_rx_on()`, `phy_set_rxclk_en()`)
  revives the receiver and stops the transmitter.
