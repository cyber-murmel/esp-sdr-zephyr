# Tuning

<!-- nav-start -->
Previous: [Transceiver](transceiver.md) · Up: [The radio](index.md) · Next: [Gain](gain.md)
<!-- nav-end -->

This page explains how the radio makes its LO, where the LO can go on each chip, what a retune
costs and where the apps put the LO relative to the channel they want. It builds on the LO section
of [Transceiver](transceiver.md), which lists the registers involved.

## Frequency plan

The LO comes from a fractional-N RF PLL: a VCO with a capacitor bank, locked to the crystal through
a sigma-delta divider (SDM) in its feedback. Below 2210 MHz a 5/6 divider follows the PLL. The
library tunes through the vendor's PHY calls, which calibrate the VCO and write the divider word;
the library itself sets the 5/6 divider and reads the VCO's window flag.

### RF PLL and the sigma-delta divider

The SDM word sits in analog block 0x63, registers 3 to 5 on the S3 and 3 to 6 on the C6:

| Chip | PLL frequency | With the 40 MHz crystal |
|---|---|---|
| ESP32-S3 | `f_PLL = 0.75 * f_xtal * (32 + r3 + r4 / 2^8 + r5 / 2^16)` | 30 MHz per integer step, about 458 Hz per LSB |
| ESP32-C6 | `f_PLL = 1.5 * f_xtal * (32 + r3 + (r4 * 2^11 + r5 * 2^3 + r6[2:0]) / 2^19)` | 60 MHz per integer step |

- The vendor's calibration picks the VCO's capacitor code, 9 bits (`0x62:5` plus 256 times
  `0x62:7[2]`). A higher code means a lower VCO frequency.
- The fields, the vendor's words and the bench values are on the register pages
  ([ESP32-S3 registers](../dev/registers-esp32s3.md#register-rfpll_sdm_r3-0x633),
  [ESP32-C6 registers](../dev/registers-esp32c6.md#register-rfpll_sdm_r3-0x633)).

### Whole MHz and the kHz offset

The library hands the PLL a whole MHz and a kHz offset
([Transceiver](transceiver.md#vendor-phy-functions)):

- on the S3 `set_rf_freq_offset(0, PLL MHz, kHz + offset)`;
- on the C6 `phy_set_freq(PLL MHz, kHz + offset)`, since the C6 channel call
  `chip_v7_set_chan()` loses off-grid requests: hence its calibration channel plus
  `phy_set_freq()`.

The crystal correction reaches the PLL in whole kHz too, so the LO can stay a few hundred Hz from
the frequency asked for. [Crystal calibration](crystal-calibration.md) gives that residual and
`esp_sdr_get_lo_residual_hz()`.

### The 5/6 divider

Below 2210 MHz the LO is the PLL through a 5/6 divider: the PLL runs at 6/5 of the LO, and
LO = PLL / 1.2. One selector feeds both the receive and the transmit mixer (CKGEN_LO_DIV on the S3,
RFPLL_LO_DIV on the C6); the register pages give the order of its writes.

- The VCO does not reach below about 2130 MHz (ESP32-C6) to 2180 MHz (ESP32-S3). So below
  2210 MHz the divider is always the better choice: it reaches down to the VCO's lower limit / 1.2
  (`lib/esp_sdr/src/esp_sdr.c`, `LO_DIVIDER_BELOW_MHZ`).
- Upstream's `rx_lo_plan()` uses the divider from 1842 MHz only and tries the PLL directly below.
- The C5 and the S31 have no qualified divided LO path (`esp_sdr_lo_divided()`). Upstream warns
  that the C5, C61, H2 and S31 lay out their analog blocks differently.
- `tests/regression/lo_tuning` checks the divider band, the switch at 2209/2210 MHz and that a
  transmission in the divider band goes out where it should ([Testing](../dev/testing.md)).

How far down the divider reaches, by source:

| Chip | Bench 2026-10-09 (below) | LTE calibration run ([Crystal calibration](crystal-calibration.md)) |
|---|---|---|
| ESP32-S3 | LO 1828 MHz without `-ERANGE`, 1817 MHz on air | about 1865 MHz |
| ESP32-C6 | LO 1784 MHz without `-ERANGE`, 1774 MHz on air | about 1775 MHz |

The S3 figures disagree by some 40 MHz; the sources do not say whether the same board was used.

## Tuning range

`esp_sdr_set_freq()` takes 100 to 6000 MHz (`ESP_SDR_FREQ_MIN_MHZ`, `ESP_SDR_FREQ_MAX_MHZ`) and
returns `-EINVAL` outside. The PLL locks over a much narrower range that differs between chips:

| Chip | PLL locks on air | LO without `-ERANGE` | LO on air | Below 2210 MHz |
|---|---|---|---|---|
| ESP32-S3 | 2180 to 2793 MHz | 1828 to 2781 MHz | 1817 to 2793 MHz | through the divider |
| ESP32-C6 | 2128 to at least 2856 MHz | 1784 to 2884 MHz | 1774 to at least 2856 MHz | through the divider |
| ESP32-C5 | no lock flag known | never `-ERANGE` | the PHY takes any MHz above 14, but nothing tunes below about 1100 MHz; edges not measured | no divider |
| ESP32-S31 | not measured apart | `-ERANGE` below 2200 MHz | tunes 2200 to some 2900 MHz | no divider |
| ESP32 | not measured | never `-ERANGE` (PLL status not mapped) | not measured | through the divider |

- The S3 and C6 figures are one chip each, bench 2026-10-09 ([VCO window and -ERANGE](#vco-window-and--erange)).
- The C6 page gives its upper lock limit as at least 2856 MHz, "the limit of the S3 transmitter";
  the S3 page measured its own PLL locking on air only up to 2793 MHz. The sources do not say
  whether the two measurements used the same S3 board.
- The MicroPython Scan app bisects the range each board tunes at start; its measured LO ranges
  (2026-11-10) are in [micropython](../../apps/micropython/README.rst) ("Scan"). Its S31 edge lies
  below the "some 2900 MHz" above.
- The paced transmit path tunes a narrower range per chip ([Paced streams](../samples/paced.md)).

### ESP32-C5

- `phy_set_chanfreq()` takes any MHz above 14 and runs a full channel setup, including 5 GHz.
- Wi-Fi bursts received with the LO off the channel grid (2433, 2444, 5190, 5200 MHz) centre on
  2437, 5180 and 5220 MHz within one 0.31 MHz bin.
- The PHY takes its 5 GHz path from 4500 MHz up.
- Below about 1100 MHz the PLL's SDM word goes negative, so those requests tune nothing.
- No PLL lock or VCO window flag is known on the C5, so `esp_sdr_set_freq()` never returns
  `-ERANGE` there.

### ESP32-S31

- The S31 tunes 2200 to some 2900 MHz. It has no divided LO, so `-ERANGE` below.
- A retune rebuilds the gain tables at the LO, keeping the IQ correction from startup, whose
  loopback would transmit.
- Below 2400 MHz the vendor calibration can stop short of the required code. The library then finds
  both edges of the lock window with the PLL's own comparator and takes the middle, for drift
  margin (`pll_recover()` in `lib/esp_sdr/src/esp_sdr_s31.c`).
- Its transmit calls return `-ERANGE` outside 2200 to 2900 MHz (`TX_MIN_MHZ`, `TX_MAX_MHZ` in the
  same file).

## VCO window and -ERANGE

After the vendor's calibration, `0x62:12[3:2]` (RFPLL_VCO_WINDOW) compares the requested frequency
with the VCO's tuning window: 0 inside it, 1 below it (capacitor code at its maximum), 2 above it
(code 0).

- `esp_sdr_set_freq()` and `esp_sdr_set_freq_offset()` read it after every tune and return
  `-ERANGE` when it is not 0: tuned, but the PLL may not lock there.
- `esp_sdr_freq_window_checked()` tells whether a chip checks it: false on the C5 and the classic
  ESP32, which return 0 wherever the PLL ends up.

The tables below are bench 2026-10-09. Frequencies are the PLL's (the LO is PLL / 1.2 behind the
divider). Code is the capacitor code (`0x62:5` plus 256 times `0x62:7[2]`), reg 12 the raw
`0x62:12`.

### ESP32-S3

| PLL MHz | Code | reg 12 |
|---|---|---|
| 2170 to 2192 | 383 | 0x04 (below the window) |
| 2193 | 383 | 0x00 |
| 2210 | 371 | 0x00 |
| 2300 | 293 | 0x00 |
| 2412 | 208 | 0x00 |
| 2500 | 148 | 0x00 |
| 2600 | 88 | 0x00 |
| 2700 | 34 | 0x02 |
| 2750 | 9 | 0x02 |
| 2780, 2781 | 0 | 0x02 |
| 2782 and up | 0 | 0x0a (above the window) |

| Limit | MHz |
|---|---|
| VCO window, `0x62:12[3:2]` = 0 (PLL) | 2193 to 2781 (2780 in another run) |
| PLL locks on air | 2180 to 2793 |
| LO without `-ERANGE` | 1828 to 2781 |
| LO on air | 1817 to 2793 |

Nothing locks directly below 2180 or above 2793 MHz (searched 200 to 6000 MHz).

### ESP32-C6

| PLL MHz | Code | reg 12 |
|---|---|---|
| 2110 to 2138 | 351 | 0x05 (below the window) |
| 2140 | 351 | 0x01 |
| 2412 | 189 (187 in another run) | 0x00 |
| 2413 | 187 | |
| 2437 | 177 | |
| 2600 | 102 | 0x00 |
| 2800 | 29 | 0x02 |
| 2884 | 3 | 0x02 |
| 2888 to 2900 | erratic: 324, 430, 337, 321 | 0x09 |
| 2904 | 0 | 0x02 |
| 2908 and up | 0 | 0x0a (above the window) |

| Limit | MHz |
|---|---|
| VCO window, `0x62:12[3:2]` = 0 (PLL) | 2140 to 2884 |
| PLL locks on air | 2128 to at least 2856 (limit of the S3 transmitter) |
| LO without `-ERANGE` | 1784 to 2884 |
| LO on air | 1774 to at least 2856 |

430, seen in the erratic band 2888 to 2900 MHz, shows that all 9 bits of the code exist.

### What the window tells

- Below the window the calibration pins the code at its maximum (383 on the S3, 351 on the C6),
  above it at 0. Repeated calibrations at one frequency land a few codes apart.
- The window sits about 12 MHz inside the lock limits, so `-ERANGE` errs on the safe side (the
  file comment and the `esp_sdr_set_freq()` docs in `include/esp_sdr/esp_sdr.h`).
- The limits are the same with and without the divider, so they are VCO limits, per chip and
  untested over temperature.
- `0x62:12[1]` is set from about 2700 MHz up on both chips, `0x62:12[0]` below about 2412 MHz on
  the C6; their meaning is unknown.
- The fields are RFPLL_R12 on the register pages
  ([ESP32-S3 registers](../dev/registers-esp32s3.md#register-rfpll_r12-0x6212),
  [ESP32-C6 registers](../dev/registers-esp32c6.md#register-rfpll_r12-0x6212)).

## Hop engine

The S3's PHY can tune a Wi-Fi channel with a hardware I2C sequencer in the analog I2C master
instead of a software VCO calibration: `set_chanfreq()` on a channel frequency probably loads an
entry of an 85-entry table (2400 to 2484 MHz).

The field meanings are *speculative*, the flow *decompile*; the library does not use the engine.
Its registers start at
[I2C_ANA_MST_HOP_CTRL_REG](../dev/registers-esp32s3.md#register-i2c_ana_mst_hop_ctrl_reg-0x6000e0c4).

## Retune time

### What a retune costs

`esp_sdr_set_freq()` timed with `ubt tune <a> <b> 20` (the Ubertooth app on a XIAO ESP32-S3,
A4CB8FD1C700, 2026-10-14):

| Frequencies | min | mean | max |
|---|---|---|---|
| 2412 / 2417 MHz (Wi-Fi channel grid) | 3143 us | 3161 us | 3511 us |
| 2441 / 2442 MHz | 3143 us | 3473 us | 4175 us |
| 2402 / 2403, 2405 / 2408, 2426 / 2480 MHz (off grid) | 3766 us | 3784 to 3804 us | 4129 to 4240 us |

- Every retune waits 3 ms for the analog chain to settle: a fixed busy wait in the library
  (`SDR_RETUNE_SETTLE_US`).
- An off-grid MHz adds a VCO calibration of about 0.6 ms.
- A turnaround between receive and transmit can skip the retune and its settle
  ([Transmit](../samples/transmit.md), "Turnaround").
- The Ubertooth app's own retunes (tune request to the first sample) are measured in
  [ubertooth internals](../apps/ubertooth-internals.md).

### Ring start

`ubt rx start` on the same board: the retune took about 4.2 ms, the first samples came about 4 ms
after that. The stream at 4 MS/s then ran for minutes without a gap or lost pair. The ring itself
is in [Gapless receive](../samples/rings.md).

### Keeping the kHz offset

The MicroPython app's Scan measurements (2026-11-10) found retunes beyond the one a new frequency
needs, around the kHz offset:

- `tune()` now retunes once where the kHz offset stays (twice before: 7.7 to 4.1 ms on the S3, 14
  to 10.7 ms on the S31).
- The P4 sends the C6 exact Hz: a float32 2405e6 was 2404 MHz plus 1000 kHz there, a second retune.
- The C6 (`apps/osmosdr_serial`) now rounds to the nearest MHz and sets its kHz offset whenever it
  differs from the one the library keeps, also back to 0. Before, a whole MHz after a fractional
  one (the Tone app's float32 frequency) kept the old offset: the LO was off by it while the
  reported centre said otherwise.

## LO placement

### Why the LO sits off the channel

The DC offset, the LO leakage and the 1/f noise sit at the LO
([The radio](index.md#direct-conversion)). Apps that need a clean channel tune the LO beside it and
move the channel where they want it in software.

### Offset tuning

The common placement puts the LO 4 MHz below the channel:

- The 16 MS/s ring mixes the stream by -4 MHz, a quarter of its rate, which brings the channel to
  DC ([Gapless receive](../samples/rings.md)).
- The LO leakage, the DC offset and the 1/f noise then stay out of the channel.
- The LO is whole MHz plus a kHz PLL offset. A rotation of the output removes the rest, so the
  centre is exact to the Hz, after the crystal correction.

### Placement per app

| App or driver | LO | Effect | Details |
|---|---|---|---|
| IEEE 802.15.4 driver | 4 MHz below the channel | its leakage and the DC offset stay out of the band | [IEEE 802.15.4](../modem/ieee802154.md) |
| osmosdr, offset tuning (default, `SET_OPTIONS` bit 0) | 4 MHz below the centre, the ring mixes the stream down | the LO leakage and the 1/f noise stay out of the band; the centre exact to the Hz | [osmosdr](../../apps/osmosdr/README.rst) |
| ubertooth | 4 MHz below the wanted channel | the channel at DC in the stream | [ubertooth internals](../apps/ubertooth-internals.md) |
| micropython bursts on the VIEWE S31 board | 5 MHz below the centre | out of the band with the receiver's DC and its noise | [micropython](../../apps/micropython/README.rst) |
| osmosdr_serial (paced receive) | on the centre: no offset tuning | the LO leakage stays at the centre | [osmosdr_serial](../../apps/osmosdr_serial/README.rst) |
| chancap | between two channels (filter bank) or off the grid's centre (decimator bank) | the leakage falls on a channel edge | [Channelizer](../modem/channelizer.md) |

<!-- nav-start -->
Previous: [Transceiver](transceiver.md) · Up: [The radio](index.md) · Next: [Gain](gain.md)
<!-- nav-end -->
