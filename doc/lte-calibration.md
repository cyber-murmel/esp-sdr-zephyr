# Crystal calibration against LTE

The LO and the sample clock of every chip here come from one crystal, which is several ppm off
(two boards differ by 6 to 10 ppm, `hardware-quirks.md`). An LTE base station's carrier is
accurate to about 0.05 ppm and sits on the 100 kHz channel raster, so a received LTE downlink is
a frequency reference: where its carrier lands in the baseband tells how far the crystal is off.

`esp_sdr_lte_calibrate()` (`include/esp_sdr/esp_sdr_cal.h`, `CONFIG_ESP_SDR_LTE_CAL`) measures
it, `esp_sdr_set_freq_corr_ppb()` applies it to every tuning, and with `CONFIG_ESP_SDR_CALSTORE`
it is kept in Zephyr settings and applied at every start. With the die temperature sensor
(`CONFIG_ESP_SDR_TEMP`) each measurement records the temperature, and calibrations at several
temperatures make a model the correction follows.

## The measurement

`include/esp_sdr/esp_sdr_lte.h` does the signal processing on a capture of about 2 MS/s
(header only, plain C, also used on the host and in `tests/unit/lte`):

1. **PSS search.** The three primary synchronization sequences (Zadoff-Chu roots 25, 29, 34 on
   62 subcarriers either side of DC) are correlated with the capture at carrier offsets in
   15 kHz steps, normalised by the energy of each symbol-long window (the downlink's power comes
   in bursts 12 dB over its floor; an unnormalised peak locks onto them). The templates are
   evaluated at the capture's own sample rate, so no 1.92 MS/s resampler is needed.
2. **Candidates.** A Zadoff-Chu sequence trades frequency error for timing error: a coarse step
   a few kHz off puts the peak tens of samples from the true start, and such ridge points can
   outdo the true one in peak height or in peak over mean alike. So the six strongest coarse
   points are refined in 2.5 kHz steps and ordered by how well their demodulated subcarriers
   match the sequence.
3. **SSS decode.** For the best four, the timing is refined to an eighth of a sample and the
   frequency to 250 Hz, then the secondary synchronization signal in front of every PSS in the
   capture (one symbol earlier, 71.35 us, frequency division duplex, normal cyclic prefix) is
   tested against all 168 cell groups and both half frames. `z = Y_sss conj(Y_pss)` per
   subcarrier removes the channel. The candidate whose SSS matches best wins; the PSS alone
   cannot tell the ridge points apart.
4. **Fine offset.** The phase between the SSS and PSS symbols gives the residual frequency
   error (range +-7 kHz, about 1 kHz at a single pair).
5. **Crystal error.** The carrier snaps to the 100 kHz raster (right while the receiver is off by
   less than 50 kHz); `esp_sdr_lte_ppb()` returns `1e9 (raster - carrier) / LO`, positive when
   the LO runs high.

Two details cost a day each: the subcarrier phase of the demodulation must be relative to the
symbol start (the carrier phase relative to the capture start), or the k-linear phase looks
like a timing shift and every ambiguity shows up; and a 4-bit capture loses the PSS, which sits
in the quiet parts of the signal at 1.7 LSB rms.

## The calibration run

`esp_sdr_lte_calibrate()` steps the LO across the scan range (default LTE band 1 downlink,
2110 to 2170 MHz) by 1 MHz and measures the received power at each step. A carrier is 5 to
20 MHz wide and its synchronization signals sit at its centre, so it takes the runs of steps
6 dB over the median power, strongest first, each from its centre outwards, and captures there
until a cell decodes (`candidates`, `tries`). Then it collects `estimates` from that cell; the
median, added to the correction in force at the start (held for the whole run), is the new
correction. Estimates spread by more than 500 ppb fail the run (`-ERANGE`).

| Chip | Capture | Synchronization pair in a capture | Work memory |
|------|---------|-----------------------------------|-------------|
| ESP32-S3 | 16 MS/s, CIC by 8 | 0.95 ms: about 18 % of captures | `ESP_SDR_LTE_CAL_FFT` points, 2048: about 80 KiB heap |
| ESP32-C5 | 4 MS/s, CIC by 2 | as the S3 at 2048 points, longer at 4096 | as the S3 |
| ESP32-C6 | 80 MS/s, CIC by 40 | 0.2 ms: about 1.3 % of captures | 1024 points static, above the capture bank |

The C6 captures 80 MS/s bursts only, so it needs many more captures; on the WHY2025 badge its
stream (2 MS/s through the P4) is the better source for bench measurements.

## Storage

`esp_sdr_cal_load()`, `esp_sdr_cal_save()` and `esp_sdr_cal_clear()` keep one record under the
settings key `esp_sdr/cal`: the correction, how it was measured (cell, carrier, estimates,
spread, temperature) and up to eight temperature points. The app picks the settings backend.
`apps/common/calstore.conf` sets the file backend at `/FLASH:/settings`, the FAT volume name
the MicroPython platforms use; `apps/common/calstore.overlay` puts that volume on the storage
partition for apps that have none (automounted, formatted on first mount). `esp_sdr_init()`
applies the stored record before it first tunes.

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
probe cal show                                correction, temperature, stored points, model now
probe cal set <ppb> | probe cal clear
probe temp
```

osmosdr_serial answers `ESDR_REQ_GET_TEMP` (0x18, the die temperature), the WHY2025 badge's
MicroPython has `esdr.temp()` and `esdr_usb temp` reads it over USB. On the serial link, like
any request, it stops a running stream for a moment, so read it between runs.

## Bench results

- WHY2025 badge, ESP32-C6, a band 1 cell at 2134.5 MHz (cell id 69) received on the chip antenna
  indoors at 2 MS/s: from one 20 ms capture -4379 ppb; from 1 ms windows of it -4370 to -4585
  ppb (one window with a partial PSS flagged invalid, windows without a PSS rejected). Later,
  eight 20 ms captures over 7 minutes at 35.7 to 37.7 degC: -5482 to -5646 ppb, median about
  -5520 (the temperature of the first run was not recorded). Streaming warms the C6 by only
  about 2 degC; the sensor reads in steps of about 2 degC there.
- At an earlier spot the same badge received the band 1 carriers' power but no PSS stood out:
  check reception first (`esdr` waterfall, or a capture) when a run finds no cell.
