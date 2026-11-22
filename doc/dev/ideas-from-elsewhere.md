# Ideas from elsewhere

<!-- nav-start -->
Previous: [Hot loops](hot-loops.md) · Up: [Reference](index.md) · Next: [ESP32-S3 registers](registers-esp32s3.md)
<!-- nav-end -->

This page records what was taken or left from six outside projects, each reviewed against this
tree: cnlohr's `cnlohr_tricks`, `fix32_fft`, `esp32s2-cookbook` and `lolra`, bitluni's
`CH570Composite` and WCH's CH570 radio. Each section gives a verdict, which chip it applies to,
the concrete item and its status.

The reviews were desk studies. Nothing was run on a board unless a section says so. Figures from
a simulation or an emulator are marked **(estimate)**, and claims not yet checked are marked
**(check)**. Several of the source notes (the bench scripts, `sdm-aliasing-dac.md`,
`esb-shockburst-plan.md` and others) are not in the repo, so for those the summaries here are all
there is.

## Status at a glance

| Source | Item | Chip | Status |
| --- | --- | --- | --- |
| cnlohr_tricks | 1: renormalize the sweep oscillator | S3 (all chips with the sweep) | planned next (S3) |
| cnlohr_tricks | 2: UBSan on the unit tests | native_sim | planned next |
| cnlohr_tricks | 3: burst centre from three bins | pcap survey | deferred: low value, no consumer yet |
| cnlohr_tricks | 4: Goertzel | any | deferred: no feature needs it |
| fix32_fft | in-repo Q31 FFT for LTE calibration | C6, C5 | deferred: needs the C6 |
| esp32s2-cookbook | 1: I2S bit clock divider of 1, inverted pin pair | S3 first | planned next (S3) |
| esp32s2-cookbook | 2: APLL clock on a pin | P4, S31 | deferred: needs the P4 or S31 |
| esp32s2-cookbook | 3: hot reload of one kernel | S3, S31 | later work, by decision |
| lolra | hard-limited phase bitstream, chirp-table LoRa | S3, C5, C6 | deferred: waits for the divider result and a 1-bit DMA path; source scripts missing |
| CH570Composite | noise-shaped PWM bitstream | any with I2S | deferred: waits for the same path; simulation script missing |
| CH570 radio | companion node, test source | CH570 | deferred: no board |

## cnlohr_tricks

Source: https://github.com/cnlohr/cnlohr_tricks (README and `archive/CORDIC.md`).

**Verdict.** Most of the page is already in the repo, often in a stronger form. Two items are worth
doing.

Already in the repo, so skipped: binary angles (`apps/pcap/src/refine.c`, `NCO_SCALE`,
`esdr_fsk_angle()`), wrap-safe timestamps (`(int32_t)(now - target)`), shift-only IIRs
(`dc_track()`, `esp_sdr_paced.c:88`), squares instead of sqrt (`peak_within()`), Q14 and Q15 fixed
point, LFSR whitening and the profile-then-assembly ladder (`esp_sdr_prof`, per-function O3, PIE).

### 1. TX sweep oscillator drift

`sweep_fill()` (`lib/esp_sdr/src/esp_sdr_tx.c:299`) builds a chirp by rotating `z` by `w` each
sample and `w` by `c`, all in float32, for up to `ESP_SDR_SAMPLES_MAX` = 16380 samples
(`include/esp_sdr/esp_sdr.h:44`) a bank. Each bank restarts from the exact phase of absolute time.
A float simulation at 40 MS/s, -15 to +15 MHz in 10 ms, amplitude 511:

| Variant | Worst amplitude error at a bank's end | Worst phase error |
| --- | --- | --- |
| Today | 3.1 % (about 16 LSB of 511, 0.27 dB) | 0.11 rad (6.5 degrees) |
| Renormalize `z` and `w` by `g = 1.5 - 0.5 abs(z)^2` every 64 samples | 0.007 % (0 LSB) | 0.11 rad |
| Also start `w` at `f + k / (2 fs)` (half a chirp step) | 0 LSB | 0.024 rad |
| Also re-derive `w` from double every 64 samples | 0 LSB | 0.00017 rad |

Two causes:

- **Amplitude.** Float rounding makes the length of `w` walk away from 1, and the length of `z` is
  the product of all of them. One Newton step every 64 samples (4 multiplies) removes it.
- **Phase.** Mostly a modelling slip. The step from sample j to j+1 of a discrete chirp is
  `f + k (j + 0.5) / fs`, but `w` starts at `f`. That is half a chirp step short and adds up to
  `n pi k / fs^2`, 0.096 rad over a bank here.

The result is a phase jump and an amplitude step at every bank boundary (every 0.4 ms at 40 MS/s),
so there are spurs at the bank rate around the chirp. **Item:** the half-step offset plus the
64-sample resync, which costs well under one cycle a sample **(estimate)**. A unit test measures
amplitude and phase drift over a long sweep against a double reference; a capture of a sweep on a
second board shows the amplitude flat.

The integer form from the page (`a += b*d; b -= a*d`) stays bounded but traces an ellipse (radius
ripple about 4 %, an image near -28 dBc **(estimate)**): a test tone for cores without an FPU,
not a clean I/Q source.

**Applies to:** the sweep on the S3 first (the board with the capture partner on the desk); the
same function serves the other chips. **Status:** planned next (S3).

### 2. UBSan on the unit tests

Zephyr already builds with `-fno-strict-aliasing`, so the page's type-punning advice is covered.
The remaining optimizer trap is undefined behaviour in fixed-point code: left shifts of negative
values, `int32` products that overflow before a `>> 15`, shifts by 32. The repo forces
`optimize("O3")` on many hot functions. Zephyr has `CONFIG_UBSAN` for native_sim. Only `esdr_seg`
runs a sanitizer today (ASan, `tests/unit/esdr_seg/testcase.yaml`).

**Item:** a twister variant with `CONFIG_UBSAN=y` (and ASan where it works) for the plain-C DSP
suites (`gapless_sort`, `pcap_generic`, `ieee154_dsp`, `chancap_dsp`, `esdr_pfbq`,
`ubertooth_*`), run by `scripts/run-unit-tests.sh`. The cost is CI time. The repo's
`(int32_t)(a - b)` wrap compares use unsigned operands, so they should not fire.

**Status:** planned next. Host only; no board.

### 3 and 4. Burst centre, Goertzel

- **Burst centre between channels.** The survey reports a burst at its channel centre
  (`apps/pcap/src/survey.c`) and `refine.c` finds the exact frequency later. The two neighbour
  channels' powers give the offset almost free, with the Gaussian vertex
  `d = 0.5 ln(u/l) / (2 ln c - ln u - ln l)` or a table from the channelizer's response (not
  cnlohr's rough formula). It saves no CPU. **Status:** deferred: no consumer asks for it yet.
- **Goertzel.** 2 real multiplies a sample for complex I/Q against 4 for a direct correlation, with
  no table; fixed point loses precision near DC. **Status:** deferred: nothing needs an on-device
  tone or image measurement today.

## fix32_fft

Source: https://github.com/cnlohr/fix32_fft (MIT, optionally New BSD).

**Verdict.** Worth it in one place: the no-FPU chips (C6, C5), where the repo runs a soft-float
FFT: `esp_sdr_fft()` (`include/esp_sdr/esp_sdr_dsp.h:176`) as the LTE crystal calibration uses it
(`esp_sdr_lte.h:352`, `esp_sdr_lte_pss_search()` at `:446`, rfprobe on the XIAO C6 with
`CONFIG_ESP_SDR_LTE_CAL`). Elsewhere the repo has something as fast or faster: S3 PIE, P4 PIE with
FPU, the int32 and Q15 kernel in `apps/micropython/cmod/esdr_scan.c`, the PFB kernels.

**What it is.** A single header with a radix-2 in-place complex FFT on split `int32_t` arrays up
to 2^24 points, Q31 `mulh` multiplies (one instruction on RV32IM) and no saturation (the caller
pre-scales: int16 shifted up by 14 - ceil(M/2), 9 for 1024 points). Its "forward" transform uses
exp(+j...), so bin k comes out at N - k; pass `inverse = 1` for the usual sign.

**Cost on rv32imac** (GCC 13.2 `-O2`, emulated, whole transform with bit reversal; cycles are
instructions plus 2 per taken branch, a guess for the in-order core, not measured on a chip):

| Kernel | 256 pt | 1024 pt | 4096 pt | cycles per point at 1024 (estimate) |
| --- | --- | --- | --- | --- |
| fix32_fft, default rounding | 173 | 196 | 221 | 214 |
| fix32_fft, precise rounding | 287 | 336 | 389 | 354 |
| repo `esdr_scan.c` int32 and Q15 twiddles | 144 | 180 | 214 | 189 |
| repo `esp_sdr_fft()`, soft float | 3559 | 4471 | 5387 | 5386 |

About 1.3 ms for 1024 points on the C6 (160 MHz) and 0.9 ms on the C5 (240 MHz), against about
34 ms and 23 ms soft float.

**Accuracy** against a float64 DFT of int16 input, 1024 points: default rounding gives 107 dB SNR
at noise 3000 rms and 78 dB at noise 100 rms (floor -158 dBFS, worst spur -135 dBc); precise
rounding 118 and 89 dB; float32 140 dB; the repo's int32 and Q15 kernel 45 and 16 dB (floor -96
dBFS, spur -83 dBc). Default rounding is already 30 dB below the input's own quantization, so
precise rounding (+70 % instructions) buys nothing. The Q15 kernel is fine for a 256-point display
of 8 to 10 bit samples but loses weak signals in long transforms.

**Where it helps.** `esp_sdr_lte_pss_search()` runs 230 or more transforms a capture (3 sequences,
about 71 coarse offsets, fine refinements, each a pointwise product and an inverse FFT of `n_fft`,
1024 on the C6), and only 1.3 % of C6 captures hold a PSS pair. The FFT is about 85 % of the
soft-float cost. Swapping it alone is 4 to 5 times faster; doing the product and peak search in
int32 too is 15 to 20 times faster, about 1 s instead of 8 s per capture **(estimate)**. Memory
stays the same, so it does not fix the C5-Zero's missing SRAM.

**Licence and upstream.** MIT, compatible with the repo's GPL-3.0-or-later when the notice is
kept. The upstream README prohibits AI contributions and takes no regular PRs, so nothing is sent
upstream and the port is an in-repo function, not a vendored header.

**Item:** `esp_sdr_fft_q31()` in `esp_sdr_dsp.h`, used only without an FPU: exp(-j...) sign, a full
Q31 twiddle table (4 KB at 1024 points), the first two stages without multiplies as `esdr_scan.c`
does, about 130 to 150 instructions a point **(estimate)**. Keep the float path as the host
reference (`tests/unit/lte`) and measure on the C6 before claiming a speedup.

**Applies to:** C6 and C5 only. **Status:** deferred: needs the C6. The benchmark scripts are not
in the repo; the tables above are their summary.

## esp32s2-cookbook

Source: https://github.com/cnlohr/esp32s2-cookbook (`examples/`, `idf_sandbox`,
`ch32v003programmer/main/funprog_port.h`).

**Verdict.** ESP32-S2 clock and peripheral hacking with no radio or PHY register work; its only
analog (regi2c) writes go to the BBPLL and APLL. Three things are worth taking, one as a test.

### 1. I2S bit clock divider of 1, and the inverted pin

`idf_sandbox/tools/i2stest/sandbox.c` sets `I2S_TX_BCK_DIV_NUM = 1`, which the S2 TRM forbids, and
gets serial data at 80 Mb/s from the 160 MHz clock. It also routes the inverse of the TX pin to a
second pin through the GPIO matrix.

- This answers the open question of the 1-bit DMA bitstream DAC (does I2S accept a divider below
  the driver's limit?) for the S2 only. On the S3 and C6 the hal writes `val - 1` with no assert
  (`i2s_ll_tx_set_bck_div_num()`, `esp_hal_i2s/esp32s3/include/hal/i2s_ll.h:329`, C6 `:335`), so
  writing 1 is the test. Our 40 MHz pad limit still applies to what reaches the pad.
- An inverted pin pair makes a push-pull output: +6 dB of swing, with even-order products and
  common mode cancelled. Edge skew between the two pins becomes the error term, so use adjacent
  pins with the same drive and measure the image.
- **Item:** a bench command that programs I2S TX with divider 1, puts the data on a pad and its
  inverse on a second pad, and measures the real bit rate with PCNT reading the pad back through
  the GPIO matrix (no wire), and checks that the pair is complementary. The result decides
  whether the lolra and PWM bitstream ideas below get a spike.

**Applies to:** S3, C5, C6, P4, S31 (any I2S, PARL_IO or LCD_CAM TX with GPIO matrix inversion).
**Status:** planned next (S3).

### 2. APLL as a fractional clock on a pin

`EnableMCO()` in `funprog_port.h` routes the APLL through the I2S clock mux to a GPIO (about
125 MHz is safe, steps of about 150 Hz). Use: a tunable test tone or comb, and exact non-integer
I2S or PARL_IO rates for host-side test DACs. It is not a radio LO. Only chips with an APLL can do
it (the P4 has `clk_ll_apll_*` in the hal; the S31 per an earlier note not in the repo, **check**).
**Status:** deferred: needs the P4 or S31 board.

### 3. Hot reload without a reboot

`idf_sandbox` writes code into an IRAM scratch buffer over USB HID and calls it, about 200 ms from
save to run. `multiapp` already loads llext apps, but switching apps reboots because of the USB
classes and the shared RAM. What carries over: a shell or USB command that loads one small llext
into the running app and swaps a function pointer for one DSP stage (a FIR, the GFSK slicer, the
ACK slot builder). It works for kernels that touch neither USB nor the ring's RAM, on the S3 and
S31. It is a tuning-speed gain for the loops in [Hot loops](hot-loops.md), not a feature.
**Status:** later work, by decision of the maintainer.

### Small uses and the rest

| Idea | Verdict |
| --- | --- |
| Dedicated GPIO (one-cycle `EE.*GPIO_OUT` on the S3, CSR-mapped on C5, C6, P4), unused by the repo | scope markers around a hot loop or a unit boundary; deferred: needs a scope |
| SAR ADC with DMA from the APLL (1.3 MS/s on the S2) | auxiliary inputs only; the radio has its own ADC. deferred: needs the P4 or S31 |
| ULP, RMT, LEDC dithering, reboot to bootloader | slow, covered elsewhere or S2 specific |
| Register store cost: one store is buffered, a second right after stalls, a read stalls | same rule as "the timed loop only stores" in [Hot loops](hot-loops.md), item 6 |
| Overclock through the BBPLL | no gain: the PHY ADC clock follows the PLL, cycles per sample stay the same, the C6 HP root is fixed at PLL/3 and the S3 and C5 only divide 480 MHz by integers |

One idea the cookbook suggests: feed the crystal clock of one board into XTAL_P of a second for a
common reference in coherent capture (TDoA, direction finding). GPIO jitter is multiplied by about
60 up to 2.4 GHz and only the RF PLL's loop removes it. **(check)** whether these chips accept a
driven XTAL_P and what phase noise results. **Status:** deferred: needs two boards and the check.

## lolra

Source: https://github.com/cnlohr/lolra (a 1-bit DMA bitstream LoRa transmitter).

**Licence.** lolra's RF files carry cnlohr's own licence (MIT-like with an AI restriction). Take
the ideas only and copy none of that code. `LoRa-SDR-Code.h` and `lorawan_simple.h` are marked MIT,
and the first derives from myriadrf/LoRa-SDR (check that licence before importing).

**Verdict.** Two things, both on the TX side. Its receive side (ADC undersampling and Goertzel per
station) has nothing the repo lacks.

1. **A hard-limited phase bitstream beats the bandpass sigma-delta modulator** for constant
   envelope signals on a 1-bit DMA aliasing path. lolra's bit is `sin(phase of the target RF
   signal at the bit time) > 0`; sampling folds the image to where it belongs. In a simulation it
   puts about 11 dB more power in the channel than the SDM and keeps 36 to 44 dB SNR with pad
   rise and fall skew, where the SDM drops to 8 to 24 dB. It covers LoRa, FSK, GFSK, FM and CW.
   AM, OFDM and anything with an envelope still need the SDM or the R2R ladder.
2. **LoRa symbols as start offsets into one chirp table**, played in quarter-chirp pieces with a
   length trim for fractional samples. That maps onto the descriptor-list plan
   ([DMA chains](dma-chains.md)) and onto the S3 generator path for native 2.4 GHz LoRa TX.

**Simulation** (LoRa SF7, BW 125 kHz, 80 Mb/s 1-bit, target 868.1 MHz, every rising edge late by
the skew; the SDM is 4th-order bandpass): SNR of the hard limit is 44 dB with no skew, 36 dB at
0.31 ns and 40 dB at 0.94 ns on a single pin, and 44 dB with a pin pair. The SDM is 51 dB with no
skew but 12 dB at 0.31 ns and 17 dB at 0.94 ns on a single pin, 51 dB with a pin pair. At 433.8 MHz
and 0.94 ns the SDM falls to 8 dB against 32 dB.

A hard-limited sine has two edges per alias cycle, so skew changes only its duty cycle. The SDM has
a data-dependent edge count, so skew becomes noise in the channel. A pin pair cancels the skew for
both. The hard limiter's cost is odd harmonics of the alias that fold to known frequencies (859.5
and 876.7 MHz around 868.1 at 80 Mb/s); pick the bit rate so they miss the filter. A SAW or
helical filter is needed either way. At 868 MHz the image is about -20 dBm before pad edge-rate
loss, likely another 10 to 20 dB lower (not modelled).

**Real-time generator.** A 32-bit NCO at word rate: per output word `acc += step; word =
lut[acc >> 24]`, where `lut[p]` holds the 32 bits of `sin(p + 2 pi f_a n / fs) > 0`. Modulation
goes into `step`. A 1 KB table (256 phases, one frequency) gives 41.9 dB SNR against 44.2 dB
exact; 16 KB with 16 frequency bins gives 42.2 dB. At 80 Mb/s that is 2.5 M words a second at about
4 instructions each, real time on any of the chips with DMA doing the shifting. The SDM cannot be
done this way, since its state depends on every bit.

**Native 2.4 GHz LoRa TX.** One up-chirp table, a symbol as a start offset, 4 quarter-chirp copies
a symbol, the conjugate for down-chirps. At 40 MS/s and 812.5 kHz there are 49.23 samples a chip;
carry the fraction across symbols by shortening one piece. Table sizes at 4 B a word: SF7 at
812 kHz 25 KB, SF9 at 1625 kHz 50 KB, SF12 at 1625 kHz about 400 KB (PSRAM). On the C5 and C6 the
offsets could become GDMA descriptors into the table.

**Not worth taking:** APLL harmonic TX (S2 and classic ESP32 only), ADC fs/4 quadrature RX, and
lolra's decoder (there is none). Its MIT LoRa encoder fails at SF 6 and below, SF 11 and above and
with LDRO, so use gr-lora_sdr as the reference for SF 5 to 12.

**Open checks, all on hardware:**

1. A hard-limited stream at 868.1 MHz from C5 PARL_IO or S3 LCD_CAM at 80 Mb/s through a SAW
   filter, received by the badge's SX1262: RSSI and SNR against the SDM stream.
2. A pin pair with `OUT_INV_SEL`: inter-pad skew and whether the cancellation holds.
3. Pad edge-rate loss at 433 and 868 MHz on each chip.

**Applies to:** S3 (LCD_CAM), C5 (PARL_IO), C6 and P4. **Status:** deferred: first the divider
test (cookbook item 1) must show 80 Mb/s works, and the 1-bit DMA path (the DAC note) is not in
the repo. The simulation scripts are not in the repo either.

## bitluni CH570Composite

Source: https://github.com/bitluni/CH570Composite (one commit, a PAL text terminal on a CH570).

**Verdict.** Mostly no. It does not touch the CH570 radio. Its techniques (a timer PWM DAC fed by
DMA, a two-line ping-pong buffer, per-line sync templates) exist here already or are better here.
The `no-tree-loop-distribute-patterns` pragma is already used in the repo
(`lib/ieee802154/ieee154_esp_sdr.c`, `apps/ubertooth/CMakeLists.txt`,
`apps/micropython/cmod/esdr_gl.c`).

**One idea to keep.** The output is pulse-width coded, so every sample has exactly one pulse. Pad
rise and fall mismatch then becomes a constant DC offset, not data-dependent noise, which is what
limits a 1-bit sigma-delta stream. Quantization is coarser but can be noise-shaped across frames
(error feedback on the duty value, as in class D audio). Simulation at 80 Mb/s, a 149 kHz tone at
-7 dBFS, 0.1 ns resolution, each high run lengthened by the skew:

| Bitstream | SNR in 200 kHz, ideal | 0.3 ns skew | 1.0 ns skew | SNR in 1 MHz, ideal and 1.0 ns |
| --- | --- | --- | --- | --- |
| 2nd-order SDM | 96.5 dB | 55.1 dB | 44.7 dB | 59.5 and 34.1 dB |
| Noise-shaped PWM, 8-bit frames (10 MHz; plain PWM gets only 32.6 dB) | 64.9 dB | 64.9 dB | 64.9 dB | 33.6 and 33.6 dB |

- In a narrow band (200 kHz: beacons, test tones) noise-shaped PWM keeps 61 to 65 dB whatever the
  edge mismatch, 6 to 20 dB better than the SDM with real pads. In a wide band (1 MHz and up) the
  SDM still wins.
- It is a baseband or low-IF format only: strong lines at multiples of 5 or 10 MHz. Clamp the duty
  to 1..L-1 or the frame loses its pulse.
- It needs no new peripheral: the same DMA bitstream path with another encoder, cheaper than an SDM.
- Hardware check to add: SNR of both streams on a real pad, with and without a retiming flip-flop.

Smaller point: the board is a one-dollar PAL test source for 13 cm ATV work.

**Status:** deferred: waits for the same 1-bit DMA path as lolra. The simulation script is not in
the repo; the table is its summary.

## WCH CH570 radio

Sources: https://github.com/openwch/ch570 (datasheet CH572DS1 and the EVT SDK),
https://www.mikrocontroller.net/attachment/670693/CH572DS1_en.PDF,
https://github.com/cnlohr/ch32fun/blob/master/extralibs/iSLER.h,
https://github.com/ch32-rs/wchble, https://github.com/ArcaneNibble/project-nanjing,
https://docs.zephyrproject.org/latest/boards/muselab/nano_ch57x/doc/index.html,
https://www.cnx-software.com/2025/03/15/10-cents-wch-ch570-ch572-risc-v-mcu-features-2-4ghz-wireless-bluetooth-le-5-0-usb-2-0/.
The review used a disassembly of the vendor RF library. Nothing was tested on silicon.

**Verdict.** Not an SDR platform: a GFSK packet radio at 1 and 2 Mbit/s, with no known I/Q, raw ADC
or continuous RSSI path, 12 KB SRAM and full-speed USB. It falls short of the ESP32 parts on every
SDR axis. It is still interesting as a 10-cent companion node: a GFSK beacon or packet sniffer, a CW
test source at any frequency in the band (244 Hz steps), and a target for open register-level
drivers.

Both parts: QingKe V3C RV32IMBC up to 100 MHz, 12 KB SRAM, 256 KB flash, USB FS, UART, SPI, I2C,
AES-128. The CH572 adds BLE 5.0 broadcaster and peripheral roles.

- **Documentation.** The datasheet gives only base addresses: radio DMA 0x4000C000, BB 0x4000C100,
  LLE 0x4000C200, RFEND 0x4000D000 (not in the datasheet). The vendor library `libCH57xRF.a` is
  closed but not stripped, so the globals are named. Its licence restricts use to WCH parts, so an
  open driver avoids redistributing it.
- **Frequency.** The kHz path programs RFEND+0x44 directly: an integer N of a 64 MHz reference and
  an 18-bit fraction, so the step is 64 MHz / 2^18 = 244 Hz. In RX the library subtracts 1 or 2 MHz,
  so the receiver is low-IF. Whether the VCO locks outside 2400 to 2483.5 MHz is unknown.
- **Transmitter and raw data.** TX calibration measures a coarse VCO bank and a modulation gain: a
  directly modulated VCO, so FSK only (index 0.5, 0.375, 0.3125 or 0.25). There is an IF ADC but no
  known readout; RSSI is one byte a packet. A noise-packet scan could give a slow biased energy
  scan **(check)**.
- **Open work.** ch32fun `iSLER.h` is a blob-free driver and the best start. Zephyr has the
  `nano_ch57x` boards but no radio driver for any CH5xx part, and 12 KB is too small for a Zephyr
  BLE stack: the realistic shape is a small out-of-tree raw packet driver.
- **Where it fits:** a test signal source (CW in 244 Hz steps, GFSK beacons with known framing,
  whitening off for steady tones), ranging and TDoA beacons (five 2 MHz timers, 0.5 us), and a
  packet front end for nRF24-style and BLE advertising traffic. Not a wideband receiver.
- **Open questions for a bench:** PLL range outside the band, retune time, a live RSSI register,
  any ADC readout, on-air compatibility with nRF24L01+.
- **Update.** An early prototype (`cnlohr/ch570_inram_advertiser`, no licence file) adds three
  test-source points: reload the whole program into SRAM over the debug port, whitening off (BB
  CTRL_CFG bit 6) turns packets into known 1 Mb/s FSK patterns, and TX power is BB CTRL_TX[30:24].

**Status:** deferred: no CH570 board.

<!-- nav-start -->
Previous: [Hot loops](hot-loops.md) · Up: [Reference](index.md) · Next: [ESP32-S3 registers](registers-esp32s3.md)
<!-- nav-end -->
