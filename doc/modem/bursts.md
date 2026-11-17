# Burst detection

This page describes how a sample stream becomes classified bursts: detected, cut out, decoded
when the protocol is known, and analysed when it is not. The detector and decoders are those of
[pcap](../../apps/pcap/README.rst), which runs them on the WHY2025 badge's ESP32-P4 for what its
ESP32-C6 radio receives:

- the **survey**: the C6's gapless stream through the P4's filterbank
  ([Polyphase filterbank on the ESP32-P4](channelizer.md#polyphase-filterbank-on-the-esp32-p4))
  and a burst detector per channel;
- the **channel mode**: one channel at 2 MS/s from the C6's paced receive
  ([Paced streams](../samples/paced.md)).

The same channel detector and decoders run in MicroPython's Pcap app
(`apps/micropython/cmod/esdr_pcap_core.c`) and on the host in
`apps/pcap/tools/esdr_gr_pcap.py`. The P4's threads, its burst pool and the follow mode are in
[pcap internals](../apps/pcap-internals.md).

This page builds on [Channelizer](channelizer.md), on the PHY of [IEEE 802.15.4](ieee802154.md)
and on [GFSK and Bluetooth LE](gfsk.md). It comes before the wideband link of
[QAM and OFDM](qam-ofdm.md).

## Wideband signals at the radio

### Width sorting on the C6

The C6 can sort what it keeps by width before it reaches the link (the user's choice for the C6,
`lib/esp_sdr/src/esp_sdr_gapless_sort.h`). It is opt-in, `ESDR_GAPLESS_F_WIDE`, off by default.
The C6's gapless receive and its pruning by the IQ estimator are in
[Gapless receive](../samples/rings.md#esp32-c6-gapless-80-mss).

#### The rule

- The C6 judges each chunk of kept samples (a window's length) by how fast it decorrelates: its
  correlations 4, 6 and 10 samples apart (50 to 125 ns) against its power above the noise, at 50
  points.
- All three below a half means a band wider than some 10 MHz (Wi-Fi).
- A flat 17 MHz band gives 0.17, 0.19, 0.06; a 2 MHz one 0.98, 0.96, 0.90; two equal narrow tones
  keep one of the three above 0.5 at any spacing.

#### The record

- A wide chunk goes to the link as a 4-pair record (`ESDR_SEG_WIDE`, `struct esdr_seg_wide`) in
  place of its samples: its span, the lag-one correlation whose angle is the signal's mean
  frequency, and the three correlations.
- The threshold servo counts the bytes saved.
- `ESDR_REQ_GET_GAPLESS_STATS` counts the chunks judged, those sent as records and the cycles by
  activity.
- On the P4 the survey assembler joins these records into wideband runs
  ([Survey assembler](#survey-assembler)).

#### Cost and limits

- A judged point costs 103 cycles, the sorting 5 to 12 % of the C6's CPU on the badge.
- At the desk it found few wide windows, and since the link was not the limit it could not lower
  the threshold the CPU holds.
- Its arithmetic is 32-bit throughout: 64-bit divisions went to libgcc in ROM and made the ring's
  switch late.
- Width sorting is off at 40 MS/s (`ESDR_GAPLESS_F_HALF`).

## Detection

### Survey assembler

In the survey each channel of the P4's bank (`esdr_pfbq`) has a detector (`esdr_detq`); the bank's
channels and rates per mode are in [pcap](../../apps/pcap/README.rst). The survey assembler
(`src/survey.c`) turns the detectors' openings into bursts.

- **Bursts.** Each channel's opening becomes a burst (`burst.h`): the detector's frames
  (`esdr_detq_take()`), a short gap zero filled so that every sample keeps its time.
- **Holes.** A hole in the C6's stream of two windows or more ends every open burst there
  (`BURST_F_HOLE`). A channel held open over it would otherwise fill its burst with zeros (it did:
  1.5 M cycles a burst and thousands lost, 44 000 after).
- **Wideband bursts.** A signal that opens six or more adjacent channels at once is kept as one
  wideband burst without samples (its span, duration, power); the C6's width records join into
  wideband runs.
- **Discards.** Bursts that never got loud after a hole go back unseen.

### Channel detector

The channel mode's detector (`src/narrow.c`):

- **Rule.** The power of 16-sample blocks against a floor learnt from the quietest of the first
  blocks and following the quiet ones; 64 samples of pre-roll; closing after 6 quiet blocks.
- **Minimum.** A burst with fewer than 3 loud blocks goes back unseen. The receiver's noise made
  thousands of one-block blips a second (2026-10-27: 59 000 bursts in 15 s, 99.7 % shorter than
  64 us), and nothing of interest is that short.
- **Cost.** Whole blocks go at once from the 8-bit items (power summed in integers), at -O2.
  Sample by sample it took 54 % of the CPU at 2 MS/s, the decoders the rest, and the pool ran dry;
  now 25 %.

### 802.15.4 preamble sightings

The survey cannot decode an 802.15.4 frame: it keeps some 30 to 50 us of one at 80 MS/s, three
times that at 40. But a frame begins with 8 equal symbols of 16 us, and the follow mode
(`PCAP_MODE_F_FOLLOW`, `src/follow.c`; the survey's decoder in `src/decoder.c`) uses them.

- The PHY counts those symbols in a row (`ieee154_phy_rx_preamble_run_i()`) in any burst of the
  survey on the channel grid, also in bursts too short to hold a frame.
- Three or more are a sighting; two are half a one.
- One symbol matches by chance some 4e-4 of the time. At the desk, in 25 s, two came once on an
  otherwise empty channel, three once on another.
- It takes a whole sighting on a channel to go there.

How the follow mode picks a channel and how long it stays, with its measurements, is in
[pcap internals](../apps/pcap-internals.md#following-802154).

## Decoders

The decoder thread (`src/decoder.c`) takes each burst through these, in order:

1. Bluetooth LE ([Bluetooth LE gates](#bluetooth-le-gates));
2. IEEE 802.15.4 ([IEEE 802.15.4 from the bank and at 2 MS/s](#ieee-802154-from-the-bank-and-at-2-mss));
3. wideband bursts and runs, which go out as burst records;
4. what no decoder claimed goes to the [generic analysis](#generic-analysis).

### Bluetooth LE gates

`src/dec_ble.c` takes a burst only when it passes these gates:

- **Frequency.** The burst's frequency is a channel of the grid (within 500 kHz) at 4 MS/s (LE 1M
  and 2M) or 2 MS/s (LE 1M).
- **Length.** Its loud part is at least 64 us long and at most 2400 us (unless the burst was
  cut).
- **Correlation.** Its correlation one sample apart over its power above the floor is high
  enough:

| Where | Threshold | Why |
| --- | --- | --- |
| Channel mode | above 0.45 | a Wi-Fi edge in the channel is near 0 |
| A bank's bin | above 0.75 | the bin's own filter correlates a band filling it to some 0.7 |
| LE 1M pass at 4 MS/s | above 0.85 | LE 1M keeps 0.95 |
| LE 2M pass at 4 MS/s | below 0.92 | |

The figures behind the bank's threshold:

- A band filling the bin, simulated through the prototype: 0.69 to 0.72.
- Wi-Fi, measured: 0.6 to 0.9.
- Bluetooth LE at 2 samples a symbol (LE 2M at 4 MS/s, LE 1M at 2 MS/s) keeps 0.78 to 0.88.

The figures behind the LE 1M pass, from a 40 MS/s survey:

- Every one of 437 packets was above 0.9.
- 58 % of the other bursts, which held most of the decoder's time, were below 0.85.

Without these gates the decoder spent 98 000 cycles a burst on noise and lost a tenth of them
(the survey's 32 us scraps, marked as cut by the C6's drops, ran it 13 000 times in 15 s), and
the 40 MS/s survey lost a third of its bursts.

### IEEE 802.15.4 from the bank and at 2 MS/s

`src/dec_wpan.c` decodes with the PHY of [IEEE 802.15.4](ieee802154.md)
(`lib/ieee802154/ieee154_phy.c`, with receiver instances), on a channel of the grid
(2405 + 5 n MHz), for bursts of 300 to 4500 us.

#### From the bank

- At 4 MS/s it decodes from a bin.
- The channels between two bins (odd MHz with an even centre) come from the two neighbours
  combined, `z[m] = y_k[m] (-j)^m + y_(k+1)[m] j^m`, with m the absolute frame index (the bank
  keeps absolute phase, `esdr_pfb.h`).
- Checked with a tone between two bins and with frames through the bank: 6 of 6 combined, 0 of 6
  from either bin alone.

#### At 2 MS/s

- The channel mode's 2 MS/s are read at 4 MS/s with every other sample a cubic midpoint:
  `(9 (x[m] + x[m+1]) - x[m-1] - x[m+2]) / 16`.
- Simulated from the C6's 40-input boxcar at any offset to the chips, the PHY decodes that down to
  6 dB as from 4 MS/s.
- The carrier offset there comes from the phase over the frame less what its decoded chips turn
  (O-QPSK as MSK, chip j by (-1)^(j+1) c[j] c[j-1] times 90 degrees): within 1 kHz, where the usual
  estimate (the squared phase step a chip) misses by tens of kHz on boxcar samples.

### The angle step

The decoders' angle step (`dec_util.h`'s `dec_angle()`, a sample at a time):

- normalises with one count of leading zeros (on RISC-V without Zbb from the float's exponent:
  `__builtin_clz()` is a library call there);
- takes the quotient where it is fastest, bit for bit the old angle:

| Chip | Quotient | Decoder time on a fixed stream |
| --- | --- | --- |
| RISC-V (ESP32-P4) | a reciprocal in a table with one correction, which beat `divu` | the P4's Bluetooth LE decoder 16.5 against 20.0 ms on 108016 samples |
| ESP32-S3 | `quou`, where the division won | 21.1 against 23.3 ms (23.9 before) |

## Generic analysis

### Hand-off

What no decoder claimed goes to the generic analysis, a thread of its own at the lowest priority
(time-sliced with the shell) with a queue of an eighth of the pool. It takes the time left over
and never holds the decoders up.

- **Bursts that fill the channel.** A burst whose correlation one sample apart is below 0.5 of its
  power above the floor, that power at least the floor's, fills the channel. The analysis found
  such bursts unknown or wideband (median bandwidth 1.8 MHz of 2), so it goes out as a wideband
  record unanalysed.
- **No room.** A burst the queue has no room for goes out with the detector's metadata only
  (`PCAP_BF_SKIPPED`).

### Spectrum, modulation and symbol rate

The analysis (`src/dec_generic.c`, `src/refine.c`):

1. centre and occupied bandwidth from an averaged spectrum;
2. [refinement](#recursive-refinement);
3. then the modulation, symbol rate and bits, from at most 8192 samples of the loud part (the
   record's duration and power cover all of it).

The modulation classes and the fields of the burst record are in the
[pcap](../../apps/pcap/README.rst) README.

### Recursive refinement

The user's rule: "only channelize further if the bandwidth of the signal is sufficiently small
after the previous channelization, recursively".

- While the signal fills less than a quarter of the rate, mix to its centre and decimate by two
  through a half-band stage, then measure again.
- Down to 62.5 kS/s.

### Cost on the P4

- 0.4 M cycles for a survey burst, 1.4 M for a channel mode burst of some 350 us.
- A 2048-sample 2FSK burst 1.9 M, of which the symbol grid's search (`try_fsk()`) 1.1 M, the phase
  steps 0.27 M, the spectrum 0.18 M.
- The decoders and the analysis inline took 2.6 M cycles a burst and lost nine bursts in ten in
  the channel mode.

How the host tool merges the records of several channels of one stream is in
[pcap](../../apps/pcap/README.rst).
