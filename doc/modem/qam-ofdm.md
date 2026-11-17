# QAM and OFDM

<!-- nav-start -->
Previous: [Burst detection](bursts.md) · Up: [Modulation and coding](index.md) · Next: [Error correction](fec.md)
<!-- nav-end -->

[link](../../apps/link/README.rst) is a packet radio link between two ESP32-S3 boards on the raw
Wi-Fi I/Q paths of the `esp-sdr` module, at the native 80 MS/s. It has two PHYs under one MAC:

- single-carrier QAM (`src/qam.c`);
- OFDM (`src/ofdm.c`, with its link frames in `src/ofdm_frame.c`);
- a CSMA/CA MAC with ARQ (`src/mac.c`) over either.

The rate is set by the CPUs, not the air: a frame is on air for 0.2 ms of each 10 ms exchange.
Most of this page is therefore about receiver and sender work per frame.

The PHYs loop their frames in the DAC and capture them in bursts, the paths of
[Transmit](../samples/transmit.md) and [Burst capture](../samples/bursts.md). In
[Modulation and coding](index.md) this page comes after [Burst detection](bursts.md); the codes
the frames carry are on the next page, [Error correction](fec.md).

## Single-carrier QAM

### PHY

- **Modulation.** QPSK to 4096-QAM, Gray coded per axis.
- **Pulses.** Raised cosine (roll-off 0.35) at 4 samples per symbol (20 Mbaud, `CONFIG_APP_SPS`).
- **Frame.** A preamble (twice the same 64 QPSK symbols), a 32 symbol QPSK header with a CRC-16,
  and a payload of error correction units ending in a CRC-32.
- **Transmit.** Frames are built cyclic and played in a DAC loop, so any capture window holds one
  whole copy.
- **Receiver.** It finds the preamble (Schmidl-Cox), times it against the known symbols,
  estimates the carrier offset and derives the sample clock offset from it, trains an 11 tap
  equalizer by least squares and tracks phase and taps decision directed.

### Looped frames

Copies of a looped frame are not phase coherent: phase noise over one loop period makes them
differ in phase ([Impairments](../radio/impairments.md#phase-noise)). The two receivers handle
it differently:

- **QAM** measures the phase step at the wrap from the overlap of two copies (`j->r.wrap` in
  `qam.c`).
- **OFDM** decodes one contiguous copy (hardware notes). The whole-window reading of
  [Frames of a whole window](#frames-of-a-whole-window) instead reads a slot one period earlier,
  turned by the phase the window's overlap with itself measures.

### Coding units and frame pairs

- **Codes.** Error correction per frame: RS(255,223), extended Hamming (128,120), or none
  ([Error correction](fec.md)).
- **Short last unit.** Full frames fill the whole capture window with a short last unit (a
  shortened RS codeword, fewer Hamming blocks).
- **Pairs.** Two half length frames looped together. The receiver decodes them on both CPUs and
  answers with one ACK.

### Measured

Setup: two XIAO ESP32S3 boards cabled through a 30 dB attenuator, TX step 4, RX gain 32, widest
receive filter (`bw 0`), PHY in its 40 MHz mode (`cbw 2`), 50 us TX/RX turnaround, 5 s per run.
The MER is in [MER ceilings](#mer-ceilings).

| Mod | FEC | pairs (`-w 2`) | single (`-w 1`) |
| --- | --- | --- | --- |
| 64-QAM | RS | 1.29 Mbit/s | 1.19 Mbit/s |
| 64-QAM | Hamming | 1.41 Mbit/s | 1.28 Mbit/s |
| 64-QAM | none | 1.54 Mbit/s | 1.40 Mbit/s |
| 256-QAM | RS | 1.69 Mbit/s | 1.47 Mbit/s |
| 256-QAM | Hamming | 1.44 Mbit/s | 1.35 Mbit/s |
| 256-QAM | none | 0.89 Mbit/s | 0.60 Mbit/s |

The link is bound by the receiver's decode time (about 2.3 us per symbol per CPU), not by
airtime.

## Medium access

### CSMA/CA and ARQ

- **Carrier sense** on short captures against the measured noise floor.
- **DIFS and binary exponential backoff** in sense slots.
- **Stop and wait ARQ** with immediate ACKs, retries and duplicate detection.
- **Overlap.** Frames for the next transmission are built on the other CPU while the sender waits
  for the ACK.
- **PHY choice.** The MAC uses OFDM instead of QAM with `link set phy 1` on both boards (and the
  same `ofdm set` settings on both).

## OFDM

### PHY design

#### Configuration

- Configured by occupied bandwidth, subcarrier count and cyclic prefix.
- The FFT size follows: the power of two, 16 to 512, that spaces that many subcarriers over that
  bandwidth at the sample rate.
- Subcarriers on both sides of DC, DC unused.
- BPSK to 64-QAM (square, Gray coded per axis), chosen per frame (the client's `-m`).

#### Sample rates

- The whole PHY runs at 40 MS/s by default (`fs 40`), which halves the work on both ends. What the
  DAC at 40 MS/s costs in MER is in [MER ceilings](#mer-ceilings).
- Or at 80 MS/s with the receiver at 40 MS/s (`fs 80 rxdiv 2`), or 80.
- Receiving at 40 MS/s goes through the radio's decimation filter
  ([Filters](../radio/filters.md)).

#### Frame

- A Schmidl-Cox preamble (known BPSK on the even subcarriers) and two pilot symbols.
- A BPSK header: type, modulation, addresses, sequence number, CRC-16; its bits repeated over the
  subcarriers and combined soft.
- The payload with another pilot in its middle; each symbol with a cyclic prefix.
- The payload carries the user data and a CRC-32 under the code the header names (`-f`, see
  [Error correction](fec.md)): RS(255,223) in equally shortened units byte interleaved over the
  frame, BCH blocks, or none.
- The frame loops in the DAC and fills the capture window, read around the period
  ([Frames of a whole window](#frames-of-a-whole-window)).

#### Receiver

On the raw capture words:

1. An integer sliding Schmidl-Cox search: coarse at twice the stride when its peak is clear, then
   at full resolution.
2. The carrier offset from its phase.
3. The channel from the two pilots: averaged, smoothed over neighbouring subcarriers, their phase
   step giving the residual offset.
4. Fine timing from the pilots' phase slope: the FFT window moved to the middle of the prefix.
5. Per symbol a derotation table, the FFT and one complex multiply per subcarrier, with a second
   order phase tracker.
6. The mid pilot measures the phase again, so the payload is demodulated on both CPUs (each half
   on one), and RS units are decoded on both.

Fine timing matters at 40 MS/s. The decimation filter spreads each symbol
([Filters](../radio/filters.md)), so an FFT window that starts only a quarter of the prefix after
its beginning picks up the previous symbol.

`ofdm_rx_begin_period()` measures the window position from the pilots' phase slope across the
subcarriers and moves the window to the middle of the prefix.

#### ACKs and the FFT

- ACKs are header-only OFDM frames (18 us), decoded in 0.8 ms.
- The FFT is the esp-dsp ESP32-S3 kernel (`CONFIG_APP_OFDM_ESP_DSP`, west project `esp-dsp`),
  1.8 times faster than the same algorithm in C from IRAM ([Chip notes](../dev/chips.md)).
- The receive context (tables and buffers, 16 KB) sits in internal RAM above the capture bank.

### Carrier offset and sample clock drift

The two boards' crystals differ, and each crystal drives its chip's carrier and its sample clock
([Impairments](../radio/impairments.md#clock-offset-between-boards)).

#### Carrier offset within a frame

- Within a frame the estimate can be off by a few kHz, so phase tracking must follow a frequency
  error, not only a phase.
- `apps/link/src/ofdm.c` takes the residual offset from the phase step between its two pilot
  symbols and tracks it with a second order loop (`track()`, `RATE_GAIN`).

#### Sample clock drift

- The window drifts against the frame by the carrier offset over the carrier (9 ppm here):
  subcarrier k of a symbol t slots from the pilots turns by k t times 6.6e-5 rad.
- The 400 us frame lost 4 dB of MER to it: 26 dB next to the pilots, 20.7 in the middle (a wrapped
  slot lies before the pilots in time).
- Each symbol is turned back by its capture time; the vector decoder's equalizer is rebuilt per
  worker when the band edge moved 0.02 rad.
- The MER is then flat at 26.3 dB over the frame, and the 64-QAM BER went from 1.7e-3 to 3e-6.
- `ofdm sfo -1|0|1` sets the sign; 0 is off.

#### Drift reference

- The sample clock drift is now timed against when the pilots were captured (a pilot read one
  period earlier counts as such), and the phase tracker turns its subcarriers for it too.
- Before, frames that start in the last 270 samples of a period lost their payload (1.8 % of
  random captures).
- Minimum MER over 200 captures: 26.1 dB (was 13 to 18).

### Measured over the MAC

Unique bytes ACKed per second; the same setup as the QAM measurement, TX step 4, RX gain 32,
`cbw 2`, `ackair 200`, 20 s each, no frame lost in any run:

| Bandwidth | Subcarriers | Mod | Bytes/frame | Effective | MER |
| --- | --- | --- | --- | --- | --- |
| 16 MHz | 52 | QPSK | 548 | 0.60 Mbit/s | 27 dB |
| 16 MHz | 52 | 16-QAM | 1100 | 1.14 Mbit/s | 27 dB |
| 16 MHz | 52 | 64-QAM | 1684 | 1.63 Mbit/s | 27 dB |
| 26 MHz | 84 | QPSK | 906 | 0.88 Mbit/s | 24 dB |
| 26 MHz | 84 | 16-QAM | 1850 | 1.67 Mbit/s | 24 dB |
| 26 MHz | 84 | 64-QAM | 2791 | 2.19 Mbit/s | 24 dB |
| 30 MHz | 96 | QPSK | 1056 | 0.99 Mbit/s | 23 dB |
| 30 MHz | 96 | 16-QAM | 2116 | 1.84 Mbit/s | 23 dB |
| 30 MHz | 96 | 64-QAM | 3176 | 2.24 Mbit/s | 23 dB |

- The QAM link measured 1.62 Mbit/s in the same session.
- Each exchange is bound by the receiver's decode, about 9 ms at 30 MHz: search 1.1, channel and
  header 0.7, payload 3.7, RS 3.1 with its many corrections.
- Just as long is the sender's build of the next frame on the other CPU.
- 64-QAM at 30 MHz runs close to the code's limit (RS corrects 80 bytes per frame); 26 MHz is the
  default.
- The later work on the sender and receiver is in [Where the time goes](#where-the-time-goes).

### Streaming test and SNR

`ofdm stx` plays frames one after the other from both DAC banks without leaving TX: the next is
built while the DAC loops the current one, and a bank switch is one retrigger. Frame n carries n
in its header and bits that depend on n.

- `ofdm srx` regenerates them and counts frames seen, missed, clean and bit errors, and the clean
  rate.
- `ofdm snr` prints the SNR per subcarrier of a looped frame.
- The command lines are in [link](../../apps/link/README.rst).

Results, two boards, 30 dB, 26 MHz, `txgain 4`, `rxgain 32`:

- **Uncoded 16-QAM is clean:** BER 0 apart from captures that straddle a bank switch (2 to 3 % of
  them); 2.0 Mbit/s of 17 kbit frames with the receiver on one CPU.
- **Uncoded 64-QAM is not:** BER 5e-3 in every frame. The MER is 24 dB and the band edges are at
  19 to 20 dB; 64-QAM needs about 26 dB.
- **Reed-Solomon** (about 4.7 corrections per 255-byte unit on average, with a tail) is what makes
  64-QAM work.
- **SNR per subcarrier** (`ofdm snr`): 28 dB in the middle of the band falling smoothly to 19 dB at
  both edges (the analog filters). So the MER is a property of the band, not of the frame: 27 dB
  at 16 MHz, 24 at 26, 23 at 30.
- **Broadcast** (`link_perf.py run --dst 0xff`: no ACK, no retry) gains little: the ACK costs
  0.34 ms of a 10 ms exchange.

### Blind stream

`link client 0xff -s <us>` (`link_perf.py run --dst 0xff --stream <us>`, OFDM only) sends a stream
without carrier sense or ACKs.

- The radio stays in TX and plays one frame after the other, each from its own DAC bank for
  `<us>`, the next built on the other CPU meanwhile. A late build keeps the old frame on air and
  counts as dropped.
- The server decodes what it catches, skips a frame it already has after the header (`dup`), and
  reports as usual; the client's "acked" counts frames sent.
- Frame n carries bytes that depend on n, and the RS and CRC-32 check them.

#### Period against loss

64-QAM, RS, 26 MHz, 30 dB, received by the server in 8 s; MER 23.3 to 23.5 dB:

| Period | Offered | Received | Lost | Dups |
| --- | --- | --- | --- | --- |
| 7000 us | 3.19 | 3.00 | 0 | 658 |
| 6000 us | 3.72 | 3.50 | 0 | 96 |
| 5500 us | 4.05 | 3.50 | 7.8 % | 3 |
| 5000 us | 4.46 | 3.38 | 18.7 % | 0 |

- Rates in Mbit/s. Before the search split and the vector derotation: 3.04 with 12.5 % lost at
  6000 us.
- The receiver's cycle is about 5.5 ms: capture 0.4, the stages of
  [Where the time goes](#where-the-time-goes), and the captures that straddle a bank switch.
- A shorter period only loses frames, a longer one repeats them (the repeats are skipped after the
  header).

#### Other runs

- With the float decoder the same boards reached 2.66 Mbit/s with 10 % lost at 7 ms; over the MAC
  (stop and wait, CSMA) 2.3 to 2.4.
- With the whole-window frames and the sample clock drift correction (64-QAM, RS, 26 MHz):
  4.35 Mbit/s with 0.1 % lost at a 10 ms period, 3.64 loss-free at 12 ms; RS fixes about 1.5
  bytes per frame.
- The MER of the link drifts by a dB or more during a session (22.3 to 23.7 dB). More errors per
  RS unit make the FEC stage slower (1.8 to 2.8 ms).
- How much parity the drift needs is in [Error correction](fec.md#reed-solomon-parity); the
  fastest blind stream, after the sender's rework, is under [Sender](#sender).

## MER ceilings

The MER (modulation error ratio) a link reaches caps the constellation it can carry. Measured on
the ESP32-S3 boards above:

| Signal | MER |
|---|---|
| Single carrier QAM, 20 Mbaud, 80 MS/s | about 31.5 dB, with or without attenuation |
| OFDM 16 MHz, DAC at 80 MS/s | about 31 dB |
| OFDM, DAC at 40 MS/s, 16 / 22 / 26 / 30 MHz | 27 / 25 / 24 / 23 dB |

- The DAC also runs at 40 MS/s (`esp_sdr_tx_loop_begin(ESP_SDR_RATE_40MSPS)`), at about 4 dB less
  MER than 80 MS/s.
- The ceiling of about 31 dB, the same with or without attenuation, rules out 1024-QAM and above
  on a single carrier on these boards.
- It also rules out 256-QAM on OFDM, which would need about 33 dB.
- The transmitter's compression and the receiver's headroom, which set the working point, are in
  [Gain](../radio/gain.md).

## Where the time goes

### Per frame

At 26 MHz, 64-QAM, RS (both CPUs), in ms:

| Stage | Time |
| --- | --- |
| Receiver: search | 0.8 |
| Receiver: channel and header | 0.85 |
| Receiver: payload | 0.8 |
| Receiver: Reed-Solomon and CRC | 1.3 to 1.9 (11 units, more when the MER drops) |
| Sender: the next frame, on the other CPU | 4.6 |
| ACKs | 0.3 each way |

### Receiver stages

#### 16-bit FFT

- The receiver's payload FFT and the transmitter's inverse FFT run on esp-dsp's S3 vector kernel
  (`dsps_fft2r_sc16_aes3_`, 7 us for 128 points against 38 for the float one).
- The input is 10 bit anyway: scaled by 32 (clipping rather than wrapping on a capture with a
  large offset), the output is X / N and goes back to float through `ctx->fscale`.
- The transmitter picks the largest power of two (8, 4, 2, 1) that keeps the preamble in 16 bit
  and rounds on the way to the DAC.
- The pilots and the header stay on the float FFT, where two symbols decide the channel estimate
  (the 16-bit one cost 0.3 dB of MER and ACKs there).
- `OFDM_FFT16` in `ofdm.c` turns it off.

#### Phase tracking

- It uses the middle half of the band, every second subcarrier (21 of 84), the strongest ones.
- The payload loop is specialised for 16-QAM and 64-QAM.

#### Vector payload decoder

For 16-QAM and 64-QAM, `veq_kernel()` in `ofdm.c`. The float decisions (46 us per symbol) were
bound by the FPU's latencies and 16 registers.

- After the common phase is tracked (float, 21 subcarriers), four subcarriers per step go through
  the S3's vector unit.
- The FFT output is gathered by bin, then one complex multiply by the equalizer and one by the
  phase (`ee.cmul.s16`, shifted by SAR).
- Slicing is an add, a clamp and a shift; the squared error is summed in the accumulator (MER);
  the levels are packed to bytes through a table.
- The equalizer (`ctx->veq`, from the pilots' channel estimate after every `channel()`) is scaled
  so that the spacing of the constellation is exactly 256, which turns the slicer into integer
  arithmetic.
- It needs about 10 bits of headroom and falls back to the float decoder for a frame whose
  weakest subcarrier does not fit.
- `ofdm vec 0|1` switches it; `ofdm rx -c` decodes every capture twice, vector and float: 10 635
  against 10 632 bit errors in 300 frames, the same for 16-QAM and 30 MHz.

#### Vector derotation

`derot_kernel()`: the payload symbol's samples go from the capture words to the 16-bit FFT input
in one pass of the vector unit, four samples per step. 17 us per symbol became 3.

- Any alignment: `ee.ld.128.usar` and `ee.src.q.qup`.
- I and Q unpacked by two shifts and a mask, the DC subtracted, clipped so that the rotation fits,
  and one complex multiply by the rotation in Q15 (`ctx->rot16`).
- Measured on chip first: `ee.vmul.s16` truncates, SAR takes a new value for the next vector
  instruction without a sync, `ee.src.q.qup` writes the shifted chunk and moves the second one up.
- `ofdm derot 0|1` switches it; `ofdm rx -c` also decodes each capture with the scalar one: 21 811
  against 21 843 bit errors in 200 frames (rounding goes the other way).

#### Per-symbol costs

Per payload symbol, 26 MHz, one CPU, in us:

| | Derotate | FFT | Tracking | Equalize and decide |
| --- | --- | --- | --- | --- |
| Before | 12 | 41 | 26 | 34 |
| Now | 3 | 11 | 10 | 10 |

A frame's payload takes 1.6 ms on one CPU instead of 5.7 (0.8 ms with both).

#### Preamble search on both CPUs

- The coarse Schmidl-Cox scan (every 4th sample, about 2000 steps of 70 to 150 cycles, mostly the
  float metric) is cut in half at the middle of the window.
- The second half runs on the other CPU's `link_rx2` thread (`ofdm_scan_async()`, a semaphore each
  way, 12 us to hand over) while this one scans the first.
- Search 1.12 to 0.82 ms (not 0.6: the fine scan, DC estimate and the halves' own cost stay).
- Stream at 6 ms: 3.61 against 3.43 Mbit/s and 0.5 against 5.3 % lost in one pair, 3.46 against
  3.33 in the next; MAC 2.81 against 2.66 to 2.74.
- `ofdm split 0|1` switches it.

#### Frames of a whole window

- The transmitter loops a frame without a gap (measured: the received period equals the frame,
  correlation 0.98 between copies), so one period anywhere holds every sample once, rotated.
- The receiver captures one frame plus a margin (`ofdm_rx_window()`: an FFT, a prefix, the
  search's step) instead of two frames.
- It reads a slot past the window's end one period earlier, turned by the carrier's phase over one
  period, which the window's overlap with itself measures (`rx.cw`).
- Frames grow from 8064 to 15984 samples (106 data symbols, 53 424 bits at 64-QAM, 26 MHz).

#### Channel stage

- The search lands on the preamble plateau's early edge and the window is then moved to the middle
  of the prefix, almost always by some samples: the channel was measured a second time.
- A whole-sample move only turns subcarrier f by 2 pi f shift / N (and all of them by the carrier
  offset's turn), so the estimate is turned instead (`equalizer()`).
- The vector equalizer is rounded inline instead of by `lrintf()`.
- Channel and header 0.95 to 0.55 ms.
- Averaging the channel over frames (`ofdm chavg 1`: a complex gain and a phase slope fitted per
  frame) gains 2 dB in the simulator but nothing on the bench, where the 27 to 29 dB are not set
  by the pilots' noise: off by default.

#### Search pre-test

- The float metric is about 40 % of a coarse step. An integer pre-test (`|pr| + |pi|` against
  `sqrt(best) min(Ra, Rb)`, rounded to never skip a better step) skips most of them: 116 to 101
  cycles per step, the same peaks.
- Tried and dropped: a metric every 8 samples with 8 correlation terms (frames missed at 12 dB,
  15 times the errors at 27 dB); the DC offset from every 16th sample (aliases with the 144-sample
  slot); rings of products and energies (slower than the multiplies they save).

#### Frame assembly

- `ofdm_frame.c` used two integer divisions per byte with divisors that vary (units, bytes per
  unit): about 0.4 ms at each end.
- Nested loops now, and a byte table for the CRC-32 (the nibble table cost 90 us per frame).

### Sender

The long frame took 10.1 ms to build on one CPU (RS coding 2.4 ms, then per symbol 30 us of
mapping, 7 of FFT, 27 of output) while the MAC thread spun until the next bank switch: the sender
was the limit. Now:

- **Helper.** The MAC thread helps (`ofdm_tx_help()`) until 100 us before the deadline. The
  frame's symbols are claimed one by one from a shared counter (generation and next slot in one
  word, compare and swap; the build closes the word before it reuses the fields, a race a host
  stress test found) and built in either CPU's FFT buffer (`ofdm txhelp 0|1`).
- **Table mapping.** A subcarrier's bits map through a table straight to its packed 16-bit point,
  one store per subcarrier (30 to 8 us per symbol).
- **Output loop.** It runs per shift with the shift a constant, without index arithmetic or
  reloads (27 to 19 us).
- **BCH coding.** The BCH codes' coding takes 0.43 ms instead of RS's 2.4
  ([Error correction](fec.md)).

Results:

- The build went from 10.1 to 2.85 ms (`link status`: build stages, coding and symbols); built
  frames are checked bit for bit against the same frame built alone.
- Blind stream, 64-QAM, `bch2-256`, 26 MHz: 7.5 to 7.7 Mbit/s at a 6 ms period (6 to 8 % lost),
  6.8 at 7 ms (4 %); over the MAC with ACKs 5.5 Mbit/s.
- The receiver is the limit again. Per frame: capture 0.4, search 1.5, channel and header 1.05,
  payload 2.2 (both CPUs), FEC 0.55 ms.

### Memory

`OFDM_NFFT_MAX` is 256: 512 cost 8 KB of internal RAM that the 16-bit tables need.

<!-- nav-start -->
Previous: [Burst detection](bursts.md) · Up: [Modulation and coding](index.md) · Next: [Error correction](fec.md)
<!-- nav-end -->
