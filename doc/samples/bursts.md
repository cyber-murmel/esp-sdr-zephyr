# Burst capture

<!-- nav-start -->
Previous: [Samples in and out](index.md) · Up: [Samples in and out](index.md) · Next: [Gapless receive](rings.md)
<!-- nav-end -->

A burst is a one-shot capture: the dump engine fills a bank at its rate, and the CPU reads the bank
afterwards. It is the first and simplest path out of the radio. The engine and its banks are
introduced in [Samples in and out](index.md); [Gapless receive](rings.md) continues where a burst
ends.

## How a burst is captured

- The capture engine writes raw I/Q words directly into the selected DRAM bank.
- The CPU does not touch samples until `esp_sdr_rx_capture_iq()` reads that bank after the burst
  completes.

### Short captures

- The engine can stop early without an error.
- `capture_locked()` writes a sentinel into the first and last word and fails the capture if
  either survives.
- Checking every word instead cost 1.2 ms for a full bank.

## Lengths and rates

- A burst holds 256 to 16380 samples. The upper limit is `ESP_SDR_SAMPLES_MAX`, a 14-bit count.
- A looped frame must fit twice into one window, so that a whole copy is always present. That caps
  the bytes per received window: what matters is the payload bits per captured sample.
- The analog low-pass filter limits the band before the engine ([Filters](../radio/filters.md)).
  An optional CIC decimation follows in software (below).
- The gapless ring and the channelized capture built on it are in [Gapless receive](rings.md) and
  [Channelizer](../modem/channelizer.md).

| Chip | Burst rates (MS/s) | Rate field |
|---|---|---|
| ESP32-S3 | 80, 40, 16 | DUMP rate fields on [ESP32-S3 registers](../dev/registers-esp32s3.md); 40 MS/s passes the decimation filter ([Filters](../radio/filters.md)) |
| ESP32-C6 | 80 only | `DUMP_CTRL_REG` on [ESP32-C6 registers](../dev/registers-esp32c6.md) |
| ESP32-C5 | 80, 40, 20, 10, 8, 4 | `0x600a9008` bits 23:21 (below) |
| ESP32-S31 | 80, 40, 20, 10, 8, 4 | rate field 0, 1, 3, 5, 7, 9 (below) |
| ESP32 | 80, 40, 16 (`lib/esp_sdr/Kconfig`) | |

## Capture modes

`rx_mode` selects the mode per burst.

| Mode | Function | Kconfig | What the CPU does |
|---|---|---|---|
| Raw | `esp_sdr_rx_capture_iq()` | `CONFIG_ESP_SDR` | reads the bank after the burst |
| Decimated | `esp_sdr_rx_capture_decimated()` | `CONFIG_ESP_SDR_RX_DECIM` | filters the burst with a CIC and decimates it |
| Folded | `esp_sdr_rx_capture_folded()` | `CONFIG_ESP_SDR_RX_DECIM` | folds the burst instead of filtering it |
| Callback | `esp_sdr_rx_capture_with()` | `CONFIG_ESP_SDR` | hands the raw words to the caller's function, still under the lock (the S31 MicroPython's stream decimator) |

## Per chip

### ESP32-C6

The library captures C6 bursts at 80 MS/s only. The gapless ring uses the engine's rate bit for
40 MS/s ([Gapless receive](rings.md#half-rate)).

### ESP32-C5

The capture engine is the C6's with a rate field. Measured on one ESP32-C5-Zero
([The radio](../radio/index.md) lists the bench).

| Field | Register and bits |
|---|---|
| Raw source | 15, in `0x600a9008` bits 20:17 |
| Rate index | bits 23:21 (table below) |
| Count | 17 bits; the count field (`0x600a9008` bits 15:0) advances by 10 samples per 2.5 us at index 5 |
| Lanes | `0x600a9018`, as librftest's `adctrig()` |

- `ESP_SDR_RATE_20MSPS` to `_4MSPS` are C5 only.
- The C5 build skips the C6's dump source gate register, whose access stalls the bus
  ([Chip notes](../dev/chips.md)).

| Rate index | Rate | 16380 samples took |
|---|---|---|
| 0 | 80 MS/s | 208 us |
| 1 | 40 MS/s | 412 us |
| 2 | 20 MS/s | 822 us |
| 3 | 10 MS/s | 1640 us |
| 4 | 8 MS/s | 2050 us |
| 5 | 4 MS/s | 4098 us |

### ESP32-S31

The S31's finite mode writes at TCM offset 0, over the image, so a burst runs in the circular mode
and stops before it wraps. `esp_sdr_rx_api` checks the code's CRC around bursts.

- The engine owns the TCM's upper group while it writes
  ([SRAM bank ownership](index.md#sram-bank-ownership)): hence interrupts off, code and stack below
  the group, no ROM or PHY calls, for up to 4.2 ms at 4 MS/s.
- With `CONFIG_SMP` the other core waits meanwhile in its stall interrupt, which spins in IRAM with
  its interrupts off.
- `soc_mp_pause_others()` comes first, with this core's interrupts still open, since the other core
  may hold the pause for a flash write and have to park this one.
- There is no done flag: the write index counts.
- The first 256 samples are the engine settling.

| Rate field | Rate |
|---|---|
| 0 | 80 MS/s |
| 1 | 40 MS/s |
| 3 | 20 MS/s |
| 5 | 10 MS/s |
| 7 | 8 MS/s |
| 9 | 4 MS/s |

The burst durations match these rates to the microsecond.

### ESP32

The MCH2022 badge's classic ESP32 streams 16 MS/s bursts decimated by the library, as
[osmosdr_serial](../../apps/osmosdr_serial/README.rst) and the [MCH2022 badge](../apps/mch2022.md)
describe.

## Triggered snapshots

On the ESP32-C6 (`CONFIG_ESP_SDR_TRIG`, `include/esp_sdr/esp_sdr_trig.h`), `ESDR_MODE_TRIGGER`
keeps the whole 80 MS/s, but only around bursts. [osmosdr](../../apps/osmosdr/README.rst),
[osmosdr_serial](../../apps/osmosdr_serial/README.rst) and
[osmosdr_bridge](../../apps/osmosdr_bridge/README.rst) take the snapshots to a host.

### Trigger rule

- The dump engine writes a circular ring of 16384 samples (205 us) into SRAM block 2.
- Meanwhile the CPU differences the IQ estimator's sums of I^2 + Q^2, I and Q over the same 80 MHz
  input every 10 us: each window's power about its mean, so the receiver's DC does not count.
- Three windows in a row whose power passes the threshold after a quiet one let the engine run on
  for the samples asked for after the first of them, then stop it.
- The threshold (`ESDR_REQ_SET_TRIGGER`) is a multiple of the quiet baseline, 8 by default.
- Bursts shorter than 20 to 30 us do not trigger. That keeps out a 1.75 us glitch the Wi-Fi driver
  puts into the receiver every 139 ms in some of its states.

### Ring and dead time

- The newest `ESDR_TRIG_SAMPLES` (16320) samples make a snapshot.
  [osmosdr_serial](../../apps/osmosdr_serial/README.rst) sends it to the host: its blocks, flags
  and sample index are in that README.
- The engine never writes the block's upper 64 KiB: the blocks are packed there and sent by DMA
  from it, after which the ring runs again.
- That dead time is about 4 ms (packing, and 33 KB over SDIO at 11.5 MB/s), so a busy band yields
  some tens of snapshots a second.

### Measured

- The snapshot rate the WHY badge's P4 received in office Wi-Fi is in
  [osmosdr_serial](../../apps/osmosdr_serial/README.rst).
- A continuous 40 MS/s stream does not fit the SDIO link, hence the snapshots
  ([SDIO ceiling](transports.md#sdio-ceiling)).

<!-- nav-start -->
Previous: [Samples in and out](index.md) · Up: [Samples in and out](index.md) · Next: [Gapless receive](rings.md)
<!-- nav-end -->
