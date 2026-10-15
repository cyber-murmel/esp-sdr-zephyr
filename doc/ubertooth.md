# Ubertooth One on the ESP32-S3 SDR

`apps/ubertooth` runs the device side of an [Ubertooth One](https://github.com/greatscottgadgets/ubertooth)
on the software radio, so the unmodified upstream host tools work. This page is the design and the
measurement log; the app's README is the user's view.

## What the original does, and what replaces it

| Ubertooth One | Here |
| --- | --- |
| LPC1758 firmware, USB bulk IN 0x82 with 64 byte packets, vendor requests on EP0 | `src/ubt_usb.c`, `src/ubt_ctrl.c`, `src/ubt_queue.c` |
| CC2400 radio: GFSK demodulator with bit clock, carrier sense, RSSI, sync word detector | software on the 4 MS/s ring stream: `gfsk_demod`, `ubt_rssi`, `ubt_le` |
| CC2400 transmit (GFSK modulator, packet engine) | `gfsk_mod`, `ubt_tx` on the DAC |
| Spectrum analyser: retune per MHz, read RSSI | wideband burst captures and an FFT: `ubt_specan` |
| Timer0: CLKN 3.2 kHz, CLK100NS | one timeline from the CPU cycle counter: `ubt_clock` |
| LPC IAP serial number, part number | the chip's MAC in the serial, a fixed part id |

The USB protocol is `src/upstream/ubertooth_interface.h` (unchanged, GPL-2.0-or-later). The host tools
check `bcdDevice` (the API version, 0x0107), open interface 0 and use bulk IN 0x82; they accept only the
USB IDs 1d50:6000, 1d50:6002 and ffff:0004.

## Signal path

The receive ring runs at 16 MS/s and delivers 4 MS/s through `esp_sdr_ring_run()` (boxcar-4 setup:
every 4th pair, `shift_fs4`). The LO sits 3 MHz above the wanted channel, so the channel appears at +1
MHz in the stream and the LO leakage at 0 Hz, where a constant offset is easy to tell from a signal
(measurements below). A 4-tap boxcar after a derotation by 1 MHz puts nulls on every multiple of 1 MHz from the
channel: the leakage and, for BR, the neighbours at +-1 MHz fall into them.

## Hardware limits that shape the app

* A retune is `esp_sdr_set_freq()`: 3 ms of settling plus a calibration, and the ring has to be stopped.
  Bluetooth BR hopping (1600 hops/s) is out of reach, as is jamming (a reaction inside a packet);
  BLE connection following works for intervals that leave time for a retune and a ring start.
* One radio user at a time, half duplex.
* Internal RAM below the three ring banks is full: the app's stacks and USB buffers live in the high
  region (`ESP_SDR_HIGH_RAM`), the packet queue in PSRAM.

## Measurements (XIAO ESP32-S3, A4CB8FD1C700, 2026-10-14)

Retune, `ubt tune <a> <b> 20` (time of `esp_sdr_set_freq()`):

| Frequencies | min | mean | max |
| --- | --- | --- | --- |
| 2412 / 2417 MHz (Wi-Fi channel grid) | 3143 us | 3161 us | 3511 us |
| 2441 / 2442 MHz | 3143 us | 3473 us | 4175 us |
| 2402 / 2403, 2405 / 2408, 2426 / 2480 MHz (off grid) | 3766 us | 3784 to 3804 us | 4129 to 4240 us |

The 3 ms are a fixed busy wait in the library (`SDR_RETUNE_SETTLE_US`); an off-grid MHz adds a VCO
calibration of about 0.6 ms.

Ring start, `ubt rx start`: retune about 4.2 ms, the first samples about 4 ms after that. The stream
at 4 MS/s ran for minutes without a gap or lost pair.

Noise floor, DC spur and offset of the stream on 2430 MHz (`ubt rx floor`, Hann window, 256 points, dBFS
per bin of a full scale tone; ambient signals present):

| Gain index | Floor | DC spur | In-channel (+0.5 to +1.5 MHz) | Mean I / Q (LSB, full scale 8192) |
| --- | --- | --- | --- | --- |
| AGC | -46.7 | -37.5 | -43.8 | -127 / -256 |
| 0 | -72.8 | -60.9 | -73.0 | -3 / -18 |
| 20 | -70.5 | -59.6 | -70.8 | 21 / 3 |
| 40 | -62.3 | -44.2 | -61.5 | -39 / 30 |
| 60 | -57.3 | -57.8 | -57.6 | 6 / 1 |
| 82 | -40.4 | -34.0 | -39.6 | -235 / -360 |

On a busy channel (2426 MHz, BLE advertising channel 38) the in-channel band stood 7 dB above the floor.

Spectrum analyser, `ubertooth-specan -l 2402 -u 2480` against the board (serial version: retune,
capture, 32 frames, FFT per placement; two placements 2428 / 2455 MHz): 504 complete sweeps in 6 s,
ascending 2402 to 2480 MHz, about 84 sweeps/s (about 11.9 ms per sweep: two retunes of 3.8 ms, two
captures of 0.2 ms, two times 32 frames of FFT and the rest). Overlapping the FFT with the next retune
would reach about 135 sweeps/s. The levels are relative: the full scale reference of the byte is a
guess (`ubt specanfs`) until a source of known power is available.

Transmit, `ubertooth-btle -s22:44:66:88:aa:cc -A38` (ADV_IND, flags 05, every 100 ms on 2426 MHz,
LO at 2422 MHz, TX gain step 6): the PC's Bluetooth adapter (`bluetoothctl scan on`) lists
"Device 22:44:66:88:AA:CC (public)" with AdvertisingFlags 05, i.e. the whitening, CRC-24, GFSK
modulator, DAC and RF all work as a BLE advertiser. TX_TEST (PRBS9 loop on 2441 MHz) starts and
stops; its output was not observed.

## Status

See the table in the README. Hardware results so far: USB function and the control requests (checked
with the nixpkgs `ubertooth-util` against the board, IDs patched to the project ID for that bench),
the receive ring, the numbers above and the spectrum analyser.

BLE sniffing (MODE_BT_FOLLOW_LE on a fixed channel, `src/ubt_rx_run.c`): `ubertooth-btle -n -A38`
for 20 s decoded 974 advertisements from 98 addresses, 22 of them also seen by the PC's adapter.
CRC rate, RSSI calibration (it prints -98 to -99 dBm, a placeholder constant), timing and missed
packets are not measured yet.

The GFSK demodulator (`src/gfsk_demod.c`) against `tools/gfsk_sim.c` (BLE, -24 dBFS, the S3's
receive front end modelled): bit error rate 1e-3 at 18.3 dB SNR (carrier over noise in 1 MHz), no
errors from 25 dB up, power reading exact (pwr 152 for -24 dBFS). A floating point discriminator
with ideal timing at 16 MS/s gets 6e-5 at 15 dB: about 5 dB are left on the table.

Written and compiled but not wired into a mode, and without unit tests: `src/ubt_rssi.c` (carrier
sense, squelch and per channel RSSI average of upstream's ubertooth_cs.c and ubertooth_rssi.c, with
fixes), `src/ubt_br.c` (the RX_SYMBOLS block builder for BR). `src/le_conn.c` (connection
following) is covered by `tests/unit/ubertooth_le` but not used by a mode either.

Next steps: RX_SYMBOLS on top of `ubt_br` and `ubt_rssi`, with a `tests/unit/ubertooth_demod`
suite (demodulator against a GFSK generator independent of `gfsk_mod.c`, RSSI conversions, block
building); BLE promiscuous and connection following glue; RSSI and specan level calibration with a
source of known power; LED_SPECAN; closing the demodulator's 5 dB.
