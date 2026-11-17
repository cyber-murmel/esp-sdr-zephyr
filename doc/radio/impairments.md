# Impairments

<!-- nav-start -->
Previous: [Filters](filters.md) · Up: [The radio](index.md) · Next: [Crystal calibration](crystal-calibration.md)
<!-- nav-end -->

This page lists what the direct conversion front end, the boards and the shared crystal add to the
samples, chip by chip, and the cures. It builds on [The radio](index.md) (direct conversion, one
crystal) and points back to [Tuning](tuning.md) and [Gain](gain.md) for the cures that live there.
[Crystal calibration](crystal-calibration.md) follows with the cure for the clock offset.

## DC offset

The receiver puts a DC offset at the LO ([The radio](index.md#direct-conversion)). It changes with
the gain index, with the LO and, under the AGC, from boot to boot.

### At the LO

Noise floor, DC spur and offset of the S3's stream on 2430 MHz, measured with the Ubertooth app on a
XIAO ESP32-S3 (A4CB8FD1C700, 2026-10-14): `ubt rx floor`, Hann window, 256 points, dBFS per bin of
a full scale tone, ambient signals present.

| Gain index | Floor | DC spur | In-channel (+0.5 to +1.5 MHz) | Mean I / Q (LSB, full scale 8192) |
|---|---|---|---|---|
| AGC | -46.7 | -37.5 | -43.8 | -127 / -256 |
| 0 | -72.8 | -60.9 | -73.0 | -3 / -18 |
| 20 | -70.5 | -59.6 | -70.8 | 21 / 3 |
| 40 | -62.3 | -44.2 | -61.5 | -39 / 30 |
| 60 | -57.3 | -57.8 | -57.6 | 6 / 1 |
| 82 | -40.4 | -34.0 | -39.6 | -235 / -360 |

On a busy channel (2426 MHz, BLE advertising channel 38) the in-channel band stood 7 dB above the
floor. The [capture](../../apps/capture/README.rst) app's sample output shows the DC offset at
1900 MHz, through the 5/6 divider.

### ESP32-C5

- With a forced index up to 60 the DC stays within about 10 of 512.
- At 65 to 77 it reaches 20 to 50, and at 5500 MHz with index 77 it reaches 78.
- Under the AGC (`ESP_SDR_RX_GAIN_AUTO`) it measured +8 to +230 on I at 2412 MHz, varying from
  boot to boot.
- So force a gain for measurements.

### ESP32-S31

- `esp_sdr_rx_dc_calibrate()` and the ring's `dc_correct` move the two baseband DC DACs, whose
  effect on I and Q is measured each time, down to about half an 8-bit step.
- Any gain change, retune or ring start undoes it: the ring's phase check passes through gain 0.

### Servos

The library and the apps take the DC off in several places:

- **Paced streams (C6, C5):** `CONFIG_ESP_SDR_RX_DC_REMOVE` (default y) makes
  `esp_sdr_rx_paced_start()` measure the receive chain's DC with the IQ estimator
  ([Paced streams](../samples/paced.md)) (32768 inputs, 2.5 ms with the settling) and take it off
  every sample. Without it the DC stands some 45 dB over
  the noise floor at the stream's centre (`lib/esp_sdr/Kconfig`).
- **S31 rings:** the DC servo of the rings is in [Gapless receive](../samples/rings.md);
  [osmosdr_hs](../../apps/osmosdr_hs/README.rst) runs it at each start at a fixed gain (not
  under automatic gain), and
  [rfprobe](../../apps/rfprobe/README.rst) has `probe dc`.
- **Offset tuning:** the apps that need a clean channel keep the LO off it
  ([Tuning](tuning.md#lo-placement)).

## LO leakage and image

### Receive

The LO leakage lands at 0 Hz with the DC ([The radio](index.md#direct-conversion)). The cure is to
tune the LO off the channel ([Tuning](tuning.md#lo-placement)). Without offset tuning, as in
[osmosdr_serial](../../apps/osmosdr_serial/README.rst), the leakage stays at the centre.

### Transmit per chip

A transmitter shows its LO leakage at the LO and an I/Q image at -f for a tone at +f.

| Chip | Path | I/Q image | LO leakage | Third order |
|---|---|---|---|---|
| ESP32-S31 | TX DC value, paced; a 31.25 kHz tone at 8000 of 32767 | -25 dBc at every rate, factor and level | -43 to -54 dBc | -46 to -57 dBc |
| ESP32-S31 | DAC engine, replay | -25 dBc, as on the paced path | -40 dBc | |
| ESP32-C5 | tone | [Gain](gain.md#transmitter-compression) | | [Gain](gain.md#transmitter-compression) |
| ESP32-S3 | `apps/osmosdr` transmit | see [osmosdr](../../apps/osmosdr/README.rst) ("Measured") | see [osmosdr](../../apps/osmosdr/README.rst) ("Measured") | |
| ESP32-C6 | DAC engine (not used by the library) | DUMP_DAC_TRIG_REG on [ESP32-C6 registers](../dev/registers-esp32c6.md#register-dump_dac_trig_reg-0x600a900c) | | |

- On the S31's paced path no TX IQ correction acts, so the image is the largest spur from x2
  interpolation on. The images of the interpolation itself are in
  [Paced streams](../samples/paced.md).
- The S31's replay spectrum is the same at 80, 40 and 20 MS/s, and a tone at -1 MHz lands at
  -1 MHz. Amplitude 100 is -19 dBFS on the XIAO at gain 10, and about 5.5 dB more at 200.
- The S31 figures were heard on a XIAO ESP32-S3 a few cm from the VIEWE board.

## Spurs

### Crystal harmonics

- The S31 crystal's harmonics land within a few kHz of where they belong from 2280 to 2800 MHz.
- On the VIEWE board, 2400 MHz is the crystal's 60th harmonic: -55 dBFS with the panel stopped.

### Display panel and PSRAM clock

The VIEWE board's panel is in the receiver's band. The board takes the pixel clock from the audio
PLL (APLL) at 25.1 MHz (`clock-source = "apll"`), which leaves the APLL's own setting a fraction.
Its sigma-delta noise takes the clock's multiples down to the floor; what it does past 250 kHz was
not measured. With the panel running, the floor sits some 2 to 4 dB up under the AGC.

#### Pixel clock source

dBFS over 250 kHz at a fixed gain of 48, the clocks interleaved, two to three rounds each
(`tmp/s31/viewe/f22.out` to `f24.out`):

| Pixel clock | Its multiples (2350, 2375 MHz) | Between them (2337 to 2512 MHz) |
|---|---|---|
| PLL_F160M, 25 MHz (160 / 3.2 / 2) | lines, -52 and -55 | -64 to -67 |
| APLL, exactly 25 MHz (no fraction) | humps of -59 and -61, some +-0.4 MHz wide | -65 to -66 |
| APLL with a fraction (25.1 MHz, and four others) | the floor, -62 to -65 | 0 to 3 dB higher |

- From PLL_F160M the fractional divider's pattern adds weaker lines every 5 MHz (2355 MHz: -68);
  from the APLL there are none.
- Under the AGC, PLL_F160M's 2350 MHz line read -37, the APLL's window -58.
- Moving the clock at each tuning would add nothing to the APLL's fraction: any move needs one.

#### Pad drive strength

The panel's 22 pads run at the weakest drive (`drive-strength = <5>` in the board's pinctrl), the
picture unchanged. Measured from PLL_F160M, three interleaved rounds each; lines at gain 48, under
the AGC in brackets:

| Panel's pads | 2350 MHz | 2375 MHz | 2355 MHz | Floor, gain 48 | Floor, AGC |
|---|---|---|---|---|---|
| Default drive (20 mA) | -47 (-28) | -48 (-29) | -60 | -58 to -66 | -46 to -47 |
| Weakest drive (5 mA) | -52 (-39) | -55 (-46) | -68 | -64 to -67 | -54 to -57 |
| Controller stopped | under -85 (-79) | under -85 (-80) | | -67 to -69 | -56 to -62 |

With the DMA stopped and the clocks running (PLL_F160M, default drive) the floor came back 0 to
5 dB and the pixel clock's lines stayed.

#### PSRAM clock lines

- With the PSRAM at 200 MHz, the lines at 2400 and 2600 MHz (-46 to -52 and -57) do not follow the
  pixel clock. They drop 10 to 12 dB when the panel's DMA stops: they are the PSRAM's bus clock
  under the panel's reads.
- With the PSRAM at 250 MHz, the board's clock, they move to its multiples, 2250, 2500 and
  2750 MHz, and grow with the panel's reads from it.
- 2400 MHz is also the crystal's 60th harmonic ([Crystal harmonics](#crystal-harmonics)).

#### Stopping the controller

Stopping the controller for every burst, at a frame's start, and restarting it after gave back up
to 3.5 dB of floor (0 to 3.5 between 2337 and 2687 MHz) and took 2400 and 2600 MHz down 6 to 7 dB.
But the panel glitched and faded in and out while it ran: not usable.

#### Bursts in the vertical blanking

What `apps/micropython` does instead: a burst with a multiple of the PSRAM's clock
(`CONFIG_SPIRAM_SPEED`) in or near its band, or in its image, waits for the panel's vertical
blanking (`display_esp32_lcd_rgb_wait_blanking()`, just before the capture in
`cmod/esdr_local.c`).

- The board's vertical back porch is 32 lines, the vendor's 8. With the VSYNC pulse that gives
  1.18 ms from the wait's return to the first line, room for a burst at 20 MS/s.
- The panel runs at 58.5 Hz.
- The panel's DMA is idle in the blanking; its clocks are not.

With the PSRAM at 200 MHz:

| dBFS, gain 48 | Bursts anywhere | In the blanking |
|---|---|---|
| 2400 MHz, the line's bin | -48 to -56 | -54 to -57, the chip's own line |
| 2600 MHz, the line's bin | -56 to -60 | -64 to -80, varying between runs |
| 2800 MHz, the line's bin | -56 to -66 | -69 to -71 |
| 2200 MHz, in a 20 MS/s burst at 2205 | -46 to -47 | -72 to -75 |
| Floor between lines, 250 kHz | -63 to -67 | the same, within 1 dB |

#### Blanking with the PSRAM at 250 MHz

Each line 5 MHz from the centre of a 20 MS/s burst, the floor 3 MHz from the line in brackets:

| dBFS, gain 48 | Bursts anywhere | In the blanking |
|---|---|---|
| 2250 MHz | -50.9 (-68) | -77.7 (-76) |
| 2500 MHz | -59.9 (-75) | -72.7 (-80) |
| 2750 MHz | -63.5 (-76) | -75.6 (-83) |
| 2200, 2600, 2800 MHz | -69, -65, -75 | -67, -68, -74: no longer the panel's |
| 2400 MHz | -53 | -54, the chip's own line |

- Near those lines the wait allows one burst a frame: at 250 kS/s 177 reads in 3 s against 1063
  (with the PSRAM at 250 MHz, 176 at 2500 MHz, 1075 to 1079 at 2337 and 2600 MHz).
- Elsewhere bursts run as before.
- `esdr.panel_quiet(False)` turns it off until the next app.
- `apps/osmosdr_hs` has no panel running.

## ADC clipping products

A signal past the ADC's full scale clips I and Q apart, each limited on its own. The clipped
signal then carries products of its phase: the phase times -3, 5, -7 and so on. At a quarter of
the ADC's rate these products land on the channel itself.

The Ubertooth demodulator shows what that costs, in its simulator (2026-11-12, night;
[GFSK and Bluetooth LE](../modem/gfsk.md) has the demodulator):

- With the channel at DC, where the sum of 4 takes the strongest product, the deviation comes out
  mirrored and tripled, only 10 dB down.
- With the channel at +1 MHz the products reached it through the ring's sums at 3 and 5 MHz, 10 to
  13 dB further down.
- A packet driving the ADC 6 dB past full scale comes through either way (30 dB SNR, behind the
  squelch).
- 8, 10 and 12 dB past it, the channel at DC loses 5, 19 and 27 % of them (right behind a weak
  packet 1, 52 and 92 %), +1 MHz none.
- On the board the PC's Bluetooth adapter reads 18 to 22 dB below full scale at gain 40, so near
  full scale at 60, 21 steps of about 1 dB up (the RSSI reads -65 to -68).

The clip guard steps the gain down for such sources ([Gain](gain.md#clip-guard)).

## Clock offset between boards

The LO and the sample clock share one crystal, so two boards differ in both by the same ratio:

- Two XIAO ESP32S3 boards differ by about 19 to 21 kHz at 2412 MHz (about 8 ppm), and the sample
  clocks by the same ratio.
- The S31's carriers, 2420 to 2700 MHz, arrived all 21 ppm low, as heard on a XIAO ESP32-S3 a few
  cm from the VIEWE board. The source says "against that board's crystal" without naming which
  of the two boards.
- The app READMEs give the offsets of their own bench pairs:
  [osmosdr](../../apps/osmosdr/README.rst), [link](../../apps/link/README.rst) and
  [micropython](../../apps/micropython/README.rst).

How the link receivers follow the offset within a frame is in
[QAM and OFDM](../modem/qam-ofdm.md). [Crystal calibration](crystal-calibration.md) measures a
board's crystal and corrects it.

## Phase noise

Phase noise over one loop period makes the copies of a looped frame differ in phase.
[QAM and OFDM](../modem/qam-ofdm.md) ("Looped frames") shows how its receivers decode such frames.

<!-- nav-start -->
Previous: [Filters](filters.md) · Up: [The radio](index.md) · Next: [Crystal calibration](crystal-calibration.md)
<!-- nav-end -->
