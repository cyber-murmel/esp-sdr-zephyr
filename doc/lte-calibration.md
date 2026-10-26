# Crystal calibration against LTE

The LO and the sample clock of every chip here come from one crystal, which is several ppm off
(two boards differ by 6 to 10 ppm, `hardware-quirks.md`). An LTE base station's carrier is
accurate to about 0.05 ppm and sits on the 100 kHz channel raster, so a received LTE downlink is
a frequency reference: where its carrier lands in the baseband tells how far the crystal is off.

`esp_sdr_lte_calibrate()` (`include/esp_sdr/esp_sdr_cal.h`, `CONFIG_ESP_SDR_LTE_CAL`) measures
it on a chip with the radio, the library applies it to every tuning, and with
`CONFIG_ESP_SDR_CALSTORE` it is kept in Zephyr settings and applied at every start. With the die
temperature sensor (`CONFIG_ESP_SDR_TEMP`) each measurement records the temperature, and
calibrations at several temperatures make a model the correction follows. A radio behind a link
(the WHY2025 badge's ESP32-C6) is measured by its host (the badge's ESP32-P4, `ltecal`) and told
the result over the protocol (`ESDR_REQ_SET_CAL`).

## The measurement

`include/esp_sdr/esp_sdr_lte.h` does the signal processing on a capture of about 2 MS/s
(header only, plain C, also used on the host, on the P4 and in `tests/unit/lte`):

1. **PSS search.** The three primary synchronization sequences (Zadoff-Chu roots 25, 29, 34 on
   62 subcarriers either side of DC) are correlated with the capture at carrier offsets in
   15 kHz steps, normalised by the energy of each symbol-long window (the downlink's power comes
   in bursts 12 dB over its floor; an unnormalised peak locks onto them). The templates are
   evaluated at the capture's own sample rate, so no 1.92 MS/s resampler is needed. The search
   looks at the first transform length less a symbol of the capture (8 ms at 16384 points,
   more than the 5 ms period of the synchronization signals, so it always holds a PSS); a
   longer capture feeds the refinement only, and the work memory stays 8 floats a point.
2. **Candidates.** A Zadoff-Chu sequence trades frequency error for timing error: a coarse step
   a few kHz off puts the peak tens of samples from the true start, and such ridge points can
   outdo the true one in peak height or in peak over mean alike. So the six strongest coarse
   points are refined in 2.5 kHz steps and ordered by how well their demodulated subcarriers
   match the sequence.
3. **SSS decode.** For each candidate the timing is refined to an eighth of a sample and the
   frequency to 250 Hz, then the secondary synchronization signal in front of every PSS in the
   capture (one symbol earlier, 71.35 us, frequency division duplex, normal cyclic prefix) is
   tested against all 168 cell groups and both half frames. `z = Y_sss conj(Y_pss)` per
   subcarrier removes the channel. The candidate whose SSS matches best wins; the PSS alone
   cannot tell the ridge points apart, and the true point is sometimes the sixth (a real
   capture with the cell's power 15 dB under the band's).
4. **Synchronization offset.** The phase between the SSS and PSS symbols gives the residual
   frequency error (range +-7 kHz, about 1 kHz at a single pair, `offset_sync_hz`).
5. **Reference signal offset.** The decoded cell's port 0 cell-specific reference signals
   (CRS, symbols 0 and 4 of every slot, the central six resource blocks, which every bandwidth
   has) give the channel per subcarrier, symbol by symbol. Its phase turns at the residual
   error: the two neighbouring symbols (0.21 and 0.29 ms apart, on interleaved subcarriers)
   agree only at the true offset within +-7 kHz, like a vernier; the same subcarriers 0.5, 2
   and 8 ms apart then refine it, each lag inside the range the last one leaves. That is a
   few Hz over a 20 ms capture, 0.002 ppm at 2.1 GHz. The coherence of the reference signals
   0.5 ms apart (`crs_metric`, about 0.92 on the badge, 0.1 for noise) gates it: below 0.5 the
   synchronization offset stands (`crs_symbols` is 0). The calibration runs take an estimate
   only from a cell whose reference signals cohere: the SSS alone, at the 0.55 and 1.25 to 1
   margins, now and then "decodes" a cell in a capture of noise (a cell 150 at a spot without
   any cell, SSS 0.68, no coherent reference symbol).
6. **Crystal error.** The carrier snaps to the 100 kHz raster (right while the receiver is off by
   less than 50 kHz); `esp_sdr_lte_ppb()` returns `1e9 (raster - carrier) / LO`, positive when
   the LO runs high.

Two details cost a day each: the subcarrier phase of the demodulation must be relative to the
symbol start (the carrier phase relative to the capture start), or the k-linear phase looks
like a timing shift and every ambiguity shows up; and a 4-bit capture loses the PSS, which sits
in the quiet parts of the signal at 1.7 LSB rms.

## The calibration run

`esp_sdr_lte_calibrate()` steps the LO across the scan range (default LTE band 1 downlink,
2110 to 2170 MHz) by 1 MHz and measures the received power at each step the LO reaches (below
2210 MHz the LO is the PLL through a 5/6 divider, which reaches down to about 1865 MHz on the
S3 and 1775 on the C6; steps beyond that are left out, `unreachable` counts them). A carrier
is 5 to 20 MHz wide and its synchronization signals sit at its centre, so it takes the runs of
steps 6 dB over the median power, strongest first, each from its centre outwards, and captures
there until a cell decodes (`candidates`, `tries`). Then it collects `estimates` from that
cell. Only FDD cells decode (the SSS one symbol before the PSS): bands 1, 3, 7, not the TDD
bands 38 to 41.

Each estimate is relative to where the LO is: the correction in force (held for the whole run)
reaches the PLL in whole kHz (the classic ESP32's ROM in 1/1024 MHz after that), so even a right
correction leaves the LO up to 500 Hz at the PLL (417 Hz at the RF below 2210 MHz, through the
5/6 divider) from the frequency asked for. `esp_sdr_get_lo_residual_hz()` tells that rest, and
the run measures against the LO it asked for plus it. The median of the estimates, on top of
the correction, goes to `esp_sdr_cal_update()`: a point of the temperature model, applied and
stored. Estimates spread by more than 500 ppb fail the run (`-ERANGE`). The caller's thread
needs about 9 KiB of stack (rfprobe's shell thread peaks at 11.6 KiB of its 16 KiB on the S3).

| Chip | Capture | Synchronization pair in a capture | Work memory |
|------|---------|-----------------------------------|-------------|
| ESP32-S3 | 16 MS/s, CIC by 8 | 0.95 ms: about 18 % of captures | `ESP_SDR_LTE_CAL_FFT` points, 2048: about 80 KiB heap |
| ESP32-C5 | 4 MS/s, CIC by 2 | as the S3 at 2048 points, longer at 4096 | as the S3; the C5-Zero's SRAM has no room, rfprobe leaves it out there |
| ESP32-C6 | 80 MS/s, CIC by 40 | 0.2 ms: about 1.3 % of captures | 1024 points static, above the capture bank |

The C6 captures 80 MS/s bursts only, so it needs many more captures; on the WHY2025 badge its
stream (2 MS/s through the P4) is the source, and the P4 does the measuring (below).

## Storage

`esp_sdr_cal_load()`, `esp_sdr_cal_save()` and `esp_sdr_cal_clear()` keep one record under the
settings key `esp_sdr/cal`: the correction, how it was measured (cell, carrier, estimates,
spread, temperature) and up to eight temperature points. The app picks the settings backend.
`apps/common/calstore.conf` sets the file backend at `/FLASH:/settings`, the FAT volume name
the MicroPython platforms use; `apps/common/calstore.overlay` puts that volume on the storage
partition for apps that have none (automounted, formatted on first mount). `esp_sdr_init()`
applies the stored record before it first tunes. `esp_sdr_get_freq_corr_model()` is the record
in force.

The WHY2025 badge's C6 has no room for a store (its image fills SRAM block 0 beside the
gapless ring), so the badge's record lives on the P4 (`/FLASH:/esdr_cal.bin`) and goes to the
C6 again whenever an app starts the radio (`ltecal.restore()` from the GNU Radio shim).

## Temperature

`apps/common/dietemp.overlay` enables the chip's temperature sensor (`coretemp`, alias
`die-temp0`; the S3, C6, C5 and P4 have one, the classic ESP32 not), and
`esp_sdr_get_temp_dc()` reads it (kept for a second). Each calibration records the temperature
and adds a point (temperature, ppb) to the record; a point within 2 degC of an earlier one
replaces it, and with eight points the oldest goes. `esp_sdr_cal_eval()` fits them: one point a
constant, two the line through them, three or more a least-squares quadratic once they span
10 degC (a least-squares line before), never extrapolated more than 10 degC past the points.

The library evaluates the model at every full retune: `esp_sdr_rx_prepare()` (set_freq, stream
starts), `esp_sdr_tx_prepare()` and the paced transmit start. The fast RX/TX turnarounds keep
the last value (the S3 ring's 802.15.4 ACK path turns around in microseconds), and a running
stream keeps the correction it started with. `esp_sdr_set_freq_corr_ppb()` sets a constant and
drops the model.

The die temperature leads the crystal's (self heating), so the model is per board and enclosure.

## Using it

rfprobe takes the calibration and the temperature (`apps/rfprobe/README.rst`):

```
probe cal lte [start MHz] [stop MHz] [gain]   measure, apply, store
probe cal show                                correction, LO residual, temperature, stored
                                              points, model now
probe cal set <ppb> | probe cal clear
probe temp
```

Over the protocol (`apps/osmosdr/src/esdr_proto.h`, `apps/common/src/esdr_cal.c` answers them
on a chip with the radio):

| Request | |
|---------|-|
| `GET_TEMP` (0x18) | the die temperature, 0.1 degC |
| `GET_CAL` (0x19) | `struct esdr_cal`: the correction in force, the LO residual, the temperature, the record behind it with its points, whether it is stored |
| `SET_CAL` (0x1a) | wValue 0: a measured error (`struct esdr_cal_meas`) as a point; 1: a constant; 2: none; 3: a whole record (`struct esdr_cal_rec`). Applied from the next tuning, stored where the device can (`last_store` tells) |

They are separate from `SET_FREQ_CORR`, the host's own correction on top. osmosdr_serial and
the MicroPython radios take all of them; the S3 osmosdr app answers `GET_CAL` only (a `SET_CAL`
would have to wait for its worker thread). On the serial link every request stops a running
stream for a moment, so read them between runs. Hosts: `esdr_usb temp | cal | cal-set -p ppb |
cal-clear`, `esdr_serial.py PORT cal [--set PPB | --clear]`, SoapySDR sensors `temperature`,
`corr_ppb` and `lo_residual_hz` (`SoapySDRUtil --probe` lists them).

### The WHY2025 badge

The P4 has the C6's 2 MS/s stream and the estimator (`esdr.lte_detect()`, `cmod/esdr_lte.c`,
about 512 KiB of work memory from the Python heap, which is PSRAM: the 16384-point transforms
stream from it, so one 20 ms capture takes about 20 s there against 0.2 s on the host; a search
in 2048-point windows of the capture, decoded over the whole of it, would cut that). The frozen `ltecal` module does what the
library's run does: `ltecal.calibrate()` surveys the band, captures 20 ms at the strongest
places until a cell decodes, collects estimates (each from `esdr.radio_state()['centre']`, the
LO with its residual, and `GET_CAL`'s correction in force), and hands the median with the
temperature to the C6 (`SET_CAL` 0), then keeps the record in `/FLASH:/esdr_cal.bin`.
`ltecal.show()`, `set_ppb()`, `clear()`, `measure(mhz)` and `survey()` are the parts. The
C6 keeps the calibration for its session; `ltecal.restore()` sends the file's record (`SET_CAL`
3) and runs whenever an app starts the radio.

## Bench results

- WHY2025 badge, ESP32-C6, a band 1 cell at 2134.5 MHz (cell id 69) received on the chip antenna
  indoors at 2 MS/s: from one 20 ms capture -4379 ppb; from 1 ms windows of it -4370 to -4585
  ppb (one window with a partial PSS flagged invalid, windows without a PSS rejected). Later,
  eight 20 ms captures over 7 minutes at 35.7 to 37.7 degC: -5482 to -5648 ppb by the
  synchronization offset alone, median about -5520 (the temperature of the first run was not
  recorded). Streaming warms the C6 by only about 2 degC; the sensor reads in steps of about
  2 degC there.
- The same eight captures through the reference signal stage: -5571.8 ppb for the first (at
  35.7 degC, warming), then -5530.5, -5522.9, -5521.7, -5520.5, -5521.6, -5521.4 and -5521.4
  at 37.7 degC: about 1 ppb between captures where the synchronization offset alone scattered
  by +-80. Coherence 0.90 to 0.96, all 80 reference symbols used. The first capture decodes
  only with all six candidates tried (its true PSS point ranked sixth).
- At two other spots the same badge received the band 1 carriers' power but no PSS stood out,
  or the band was flat (every MHz within 2 dB of the median): check reception first (`esdr`
  waterfall, `ltecal.survey()`, or a capture) when a run finds no cell.
