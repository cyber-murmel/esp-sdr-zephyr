.. _esp_sdr_link:

link
####

Overview
********

A packet radio link between two ESP32-S3 boards on the raw Wi-Fi I/Q paths
of the ``esp-sdr`` module, at the native 80 MS/s, with an iperf style
throughput test on the USB shell.

- **PHY** (``src/qam.c``): single carrier QPSK to 4096-QAM, Gray coded per
  axis, raised cosine pulses (roll-off 0.35) at 4 samples per symbol
  (20 Mbaud, ``CONFIG_APP_SPS``). A frame is a preamble (twice the same 64
  QPSK symbols), a 32 symbol QPSK header with a CRC-16, and a payload of
  error correction units ending in a CRC-32. Frames are built cyclic and
  played in a DAC loop, so any capture window holds one whole copy. The
  receiver finds the preamble (Schmidl-Cox), times it against the known
  symbols, estimates the carrier offset (and from it the sample clock
  offset), trains an 11 tap equalizer by least squares and tracks phase and
  taps decision directed.
- **Error correction** per frame: RS(255,223), extended Hamming (128,120),
  or none. Full frames fill the whole capture window with a short last unit
  (a shortened RS codeword, fewer Hamming blocks).
- **Pairs**: two half length frames looped together. The receiver decodes
  them on both CPUs and answers with one ACK.
- **MAC** (``src/mac.c``): CSMA/CA. Carrier sense on short captures against
  the measured noise floor, DIFS and binary exponential backoff in sense
  slots, stop and wait ARQ with immediate ACKs, retries and duplicate
  detection. Frames for the next transmission are built on the other CPU
  while the sender waits for the ACK.

Measured
********

Two XIAO ESP32S3 boards cabled through a 30 dB attenuator, TX step 4, RX
gain 32, widest receive filter (``bw 0``), PHY in its 40 MHz mode
(``cbw 2``), 50 us TX/RX turnaround, 5 s per run. MER is about 31.5 dB.

=======  =======  ================  =================
Mod      FEC      pairs (``-w 2``)  single (``-w 1``)
=======  =======  ================  =================
64-QAM   RS       1.29 Mbit/s       1.19 Mbit/s
64-QAM   Hamming  1.41 Mbit/s       1.28 Mbit/s
64-QAM   none     1.54 Mbit/s       1.40 Mbit/s
256-QAM  RS       1.69 Mbit/s       1.47 Mbit/s
256-QAM  Hamming  1.44 Mbit/s       1.35 Mbit/s
256-QAM  none     0.89 Mbit/s       0.60 Mbit/s
=======  =======  ================  =================

The link is bound by the receiver's decode time (about 2.3 us per symbol
per CPU), not by airtime. The MER ceiling of about 31 dB, the same with
or without attenuation, rules out 1024-QAM and above on these boards.

OFDM
****

A second PHY (``src/ofdm.c``) with its link frames (``src/ofdm_frame.c``),
used by the MAC instead of QAM with ``link set phy 1`` on both boards (and
the same ``ofdm set`` settings on both).

- Configured by occupied bandwidth, subcarrier count and cyclic prefix; the
  FFT size follows (the power of two, 16 to 512, that spaces that many
  subcarriers over that bandwidth at the sample rate). Subcarriers on both
  sides of DC, DC unused. BPSK to 64-QAM (square, Gray coded per axis),
  chosen per frame (the client's ``-m``).
- The whole PHY runs at 40 MS/s by default (``fs 40``; the DAC at 40 MS/s
  costs about 4 dB MER against 80 MS/s, but halves the work on both ends),
  or at 80 MS/s with the receiver at 40 MS/s (``fs 80 rxdiv 2``) or 80.
- A frame: a Schmidl-Cox preamble (known BPSK on the even subcarriers), two
  pilot symbols, a BPSK header (type, modulation, addresses, sequence
  number, CRC-16; its bits repeated over the subcarriers and combined soft),
  the payload with another pilot in its middle, each symbol with a cyclic
  prefix. The payload carries the user data and a CRC-32 under the code the
  header names (``-f``, see "Light error correction"): RS(255,223) in
  equally shortened units byte interleaved over the frame, BCH blocks, or
  none. The frame loops in the DAC and fills the capture window (read
  around the period, see "Frames of a whole window").
- Receiver, on the raw capture words: an integer sliding Schmidl-Cox search
  (coarse at twice the stride when its peak is clear, then at full
  resolution), the carrier offset from its phase, the channel from the two
  pilots (averaged, smoothed over neighbouring subcarriers, their phase step
  giving the residual offset), fine timing from the pilots' phase slope (the
  FFT window moved to the middle of the prefix), then per symbol a
  derotation table, the FFT and one complex multiply per subcarrier, with a
  second order phase tracker. The mid pilot measures the phase again, so the
  payload is demodulated on both CPUs (each half on one), and RS units are
  decoded on both.
- ACKs are header-only OFDM frames (18 us), decoded in 0.8 ms.
- The FFT is the esp-dsp ESP32-S3 kernel (``CONFIG_APP_OFDM_ESP_DSP``,
  west project ``esp-dsp``), 1.8 times faster than the same algorithm in C
  from IRAM. The receive context (tables and buffers, 16 KB) sits in internal
  RAM above the capture bank.

Measured over the MAC (unique bytes ACKed per second; same setup as above,
TX step 4, RX gain 32, ``cbw 2``, ``ackair 200``, 20 s each, no frame lost
in any run):

=========  ===========  ======  ===========  ===========  =======
Bandwidth  Subcarriers  Mod     Bytes/frame  Effective    MER
=========  ===========  ======  ===========  ===========  =======
16 MHz     52           QPSK    548          0.60 Mbit/s  27 dB
16 MHz     52           16-QAM  1100         1.14 Mbit/s  27 dB
16 MHz     52           64-QAM  1684         1.63 Mbit/s  27 dB
26 MHz     84           QPSK    906          0.88 Mbit/s  24 dB
26 MHz     84           16-QAM  1850         1.67 Mbit/s  24 dB
26 MHz     84           64-QAM  2791         2.19 Mbit/s  24 dB
30 MHz     96           QPSK    1056         0.99 Mbit/s  23 dB
30 MHz     96           16-QAM  2116         1.84 Mbit/s  23 dB
30 MHz     96           64-QAM  3176         2.24 Mbit/s  23 dB
=========  ===========  ======  ===========  ===========  =======

The QAM link measured 1.62 Mbit/s in the same session. Each exchange is
bound by the receiver's decode (about 9 ms at 30 MHz: search 1.1, channel
and header 0.7, payload 3.7, RS 3.1 with its many corrections) and, just as
long, the sender's build of the next frame on the other CPU. 64-QAM at
30 MHz runs close to the code's limit (RS corrects 80 bytes per frame);
26 MHz is the default. 256-QAM would need about 33 dB MER.

Wide frames need the 40 MHz mode (``cbw 2``, the default) and ``amp`` 90 or
less (the default); the forced receive gain is lost now and then and the
library re-forces it (``link status``, engine line). Details in
``doc/hardware-quirks.md``.

Where the time goes
===================

The rate is set by the CPUs, not the air (a frame is on air for 0.2 ms of each
10 ms exchange). Per frame at 26 MHz, 64-QAM, RS (both CPUs; the receiver in ms): search
0.8, channel and header 0.85, payload 0.8, Reed-Solomon and CRC 1.3 to 1.9 (11 units,
more when the MER drops); the sender
builds the next frame in 4.6 on the other CPU, ACKs 0.3 each way.

- **16-bit FFT.** The receiver's payload FFT and the transmitter's inverse FFT run on
  esp-dsp's S3 vector kernel (``dsps_fft2r_sc16_aes3_``, 7 us for 128 points against 38
  for the float one). The input is 10 bit anyway: scaled by 32 (clipping rather than
  wrapping on a capture with a large offset), the output is X / N and goes back to float
  through ``ctx->fscale``; the transmitter picks the largest power of two (8, 4, 2, 1)
  that keeps the preamble in 16 bit and rounds on the way to the DAC. The pilots and the
  header stay on the float FFT, where two symbols decide the channel estimate (the 16-bit
  one cost 0.3 dB of MER and ACKs there). ``OFDM_FFT16`` in ``ofdm.c`` turns it off.
- **Phase tracking** uses the middle half of the band, every second subcarrier (21 of 84),
  the strongest ones; the payload loop is specialised for 16-QAM and 64-QAM.
- **Vector payload decoder** (16-QAM and 64-QAM, ``veq_kernel()`` in ``ofdm.c``). The
  float decisions (46 us per symbol) were bound by the FPU's latencies and 16 registers.
  Now, after the common phase is tracked (float, 21 subcarriers), four subcarriers per step
  on the S3's vector unit: the FFT output gathered by bin, one complex multiply by the
  equalizer and one by the phase (``ee.cmul.s16``, shifted by SAR), then slicing as an add,
  a clamp and a shift, the squared error summed in the accumulator (MER), the levels packed
  to bytes through a table. The equalizer (``ctx->veq``, from the pilots' channel estimate
  after every ``channel()``) is scaled so that the spacing of the constellation is exactly
  256, which turns the slicer into integer arithmetic; it needs about 10 bits of headroom
  and falls back to the float decoder for a frame whose weakest subcarrier does not fit.
  ``ofdm vec 0|1`` switches it; ``ofdm rx -c`` decodes every capture twice, vector and
  float: 10 635 against 10 632 bit errors in 300 frames, the same for 16-QAM and 30 MHz.
- **Vector derotation** (``derot_kernel()``): the payload symbol's samples go from the
  capture words (any alignment: ``ee.ld.128.usar`` and ``ee.src.q.qup``) to the 16-bit FFT
  input in one pass of the vector unit, four samples per step: I and Q unpacked by two shifts
  and a mask, the DC subtracted, clipped so that the rotation fits, and one complex multiply
  by the rotation in Q15 (``ctx->rot16``). 17 us per symbol became 3. Measured on chip
  first: ``ee.vmul.s16`` truncates, SAR takes a new value for the next vector instruction
  without a sync, ``ee.src.q.qup`` writes the shifted chunk and moves the second one up.
  ``ofdm derot 0|1`` switches it; ``ofdm rx -c`` also decodes each capture with the scalar
  one: 21 811 against 21 843 bit errors in 200 frames (rounding goes the other way).
- Per payload symbol (26 MHz, one CPU): derotate 3 us, FFT 11, tracking 10, equalize and
  decide 10; before: 12 + 41 + 26 + 34. A frame's payload takes 1.6 ms on one CPU instead
  of 5.7 (0.8 ms with both).
- **Preamble search on both CPUs.** The coarse Schmidl-Cox scan (every 4th sample, about 2000
  steps of 70 to 150 cycles, mostly the float metric) is cut in half at the middle of the
  window; the second half runs on the other CPU's ``link_rx2`` thread (``ofdm_scan_async()``,
  a semaphore each way, 12 us to hand over) while this one scans the first. Search 1.12 ->
  0.82 ms (not 0.6: the fine scan, DC estimate and the halves' own cost stay). Stream at
  6 ms: 3.61 against 3.43 Mbit/s and 0.5 against 5.3 % lost in one pair, 3.46 against 3.33
  in the next; MAC 2.81 against 2.66 to 2.74. ``ofdm split 0|1`` switches it.
- **Frames of a whole window.** The transmitter loops a frame without a gap (measured: the
  received period equals the frame, correlation 0.98 between copies), so one period
  anywhere holds every sample once, rotated. The receiver captures one frame plus a margin
  (``ofdm_rx_window()``: an FFT, a prefix, the search's step) instead of two frames and
  reads a slot past the window's end one period earlier, turned by the carrier's phase over
  one period, which the window's overlap with itself measures (``rx.cw``). Frames grow from
  8064 to 15984 samples (106 data symbols, 53 424 bits at 64-QAM, 26 MHz).
- **Sample clock drift.** Each chip's crystal drives its carrier and its sample clock, so the
  window drifts against the frame by the carrier offset over the carrier (9 ppm here):
  subcarrier k of a symbol t slots from the pilots turns by k t times 6.6e-5 rad. The 400 us
  frame lost 4 dB of MER to it (26 dB next to the pilots, 20.7 in the middle; a wrapped slot
  lies before the pilots in time). Each symbol is turned back by its capture time (the vector
  decoder's equalizer rebuilt per worker when the band edge moved 0.02 rad): the MER is
  flat at 26.3 dB over the frame, 64-QAM BER 1.7e-3 -> 3e-6. ``ofdm sfo -1|0|1`` (the sign;
  0 off).
- Blind stream with both (64-QAM, RS, 26 MHz): 4.35 Mbit/s with 0.1 % lost at a 10 ms
  period, 3.64 loss-free at 12 ms; RS fixes about 1.5 bytes per frame.
- **The sender.** The long frame took 10.1 ms to build on one CPU (RS coding 2.4 ms, then
  per symbol 30 us of mapping, 7 of FFT, 27 of output) while the MAC thread spun until the
  next bank switch: the sender was the limit. Now:

  - the MAC thread helps (``ofdm_tx_help()``) until 100 us before the deadline: the frame's
    symbols are claimed one by one from a shared counter (generation and next slot in one
    word, compare and swap; the build closes the word before it reuses the fields, a race a
    host stress test found) and built in either CPU's FFT buffer (``ofdm txhelp 0|1``);
  - a subcarrier's bits map through a table straight to its packed 16-bit point, one store
    per subcarrier (30 -> 8 us per symbol);
  - the output loop runs per shift with the shift a constant, without index arithmetic or
    reloads (27 -> 19 us);
  - the BCH codes' coding takes 0.43 ms instead of RS's 2.4.

  The build: 10.1 -> 2.85 ms (``link status``: build stages, coding and symbols), built
  frames checked bit for bit against the same frame built alone. Blind stream, 64-QAM,
  ``bch2-256``, 26 MHz: 7.5 to 7.7 Mbit/s at a 6 ms period (6 to 8 % lost), 6.8 at 7 ms
  (4 %); over the MAC with ACKs 5.5 Mbit/s. The receiver is the limit again: per frame
  capture 0.4, search 1.5, channel and header 1.05, payload 2.2 (both CPUs), FEC 0.55 ms.
- **Channel stage.** The search lands on the preamble plateau's early edge and the window
  is then moved to the middle of the prefix, almost always by some samples: the channel was
  measured a second time. A whole-sample move only turns subcarrier f by 2 pi f shift / N
  (and all of them by the carrier offset's turn), so the estimate is turned instead
  (``equalizer()``); and the vector equalizer is rounded inline instead of by ``lrintf()``.
  Channel and header 0.95 -> 0.55 ms. Averaging the channel over frames (``ofdm chavg 1``:
  a complex gain and a phase slope fitted per frame) gains 2 dB in the simulator but
  nothing on the bench, where the 27 to 29 dB are not set by the pilots' noise: off by
  default.
- **Search.** The float metric is about 40 % of a coarse step; an integer pre-test
  (``|pr| + |pi|`` against ``sqrt(best) min(Ra, Rb)``, rounded to never skip a better step) skips
  most of them: 116 -> 101 cycles per step, the same peaks. Tried and dropped: a metric
  every 8 samples with 8 correlation terms (frames missed at 12 dB, 15 times the errors at
  27 dB), the DC offset from every 16th sample (aliases with the 144-sample slot), rings of
  products and energies (slower than the multiplies they save).
- **Drift reference.** The sample clock drift is now timed against when the pilots were
  captured (a pilot read one period earlier counts as such), and the phase tracker turns its
  subcarriers for it too: frames that start in the last 270 samples of a period lost their
  payload (1.8 % of random captures). Minimum MER over 200 captures 26.1 dB (was 13 to 18).
- **Frame assembly** (``ofdm_frame.c``) used two integer divisions per byte with divisors
  that vary (units, bytes per unit): about 0.4 ms at each end. Nested loops now, and a byte
  table for the CRC-32 (the nibble table cost 90 us per frame).
- ``OFDM_NFFT_MAX`` is 256 (512 cost 8 KB of internal RAM that the 16-bit tables need).

Streaming test and SNR
======================

``ofdm stx`` plays frames one after the other from both DAC banks without leaving TX (the
next is built while the DAC loops the current one, a bank switch is one retrigger): frame
n carries n in its header and bits that depend on n. ``ofdm srx`` regenerates them and
counts frames seen, missed, clean and bit errors, and the clean rate. ``ofdm snr`` prints
the SNR per subcarrier of a looped frame.

.. code-block:: console

   ofdm set bw 26 ch 84 mod 16qam    # on both
   ofdm stx -t 5 -p 6000             # sending board: a frame every 6000 us
   ofdm srx -t 6                     # receiving board

Results (two boards, 30 dB, 26 MHz, ``txgain 4``, ``rxgain 32``):

- Uncoded 16-QAM is clean (BER 0 apart from captures that straddle a bank switch, 2 to 3 %
  of them): 2.0 Mbit/s of 17 kbit frames with the receiver on one CPU.
  Uncoded 64-QAM is not (BER 5e-3 in every frame): the MER is 24 dB and the band edges are
  at 19 to 20 dB; 64-QAM needs about 26 dB. Reed-Solomon (about 4.7 corrections per 255-byte
  unit on average, with a tail) is what makes it work.
- ``ofdm snr``: 28 dB in the middle of the band falling smoothly to 19 dB at both edges (the
  analog filters), so the MER is a property of the band, not of the frame: 27 dB at 16 MHz,
  24 at 26, 23 at 30.
- A broadcast (``link_perf.py run --dst 0xff``: no ACK, no retry) gains little: the ACK
  costs 0.34 ms of a 10 ms exchange.

Blind stream
============

``link client 0xff -s <us>`` (``link_perf.py run --dst 0xff --stream <us>``, OFDM only)
sends a stream without carrier sense or ACKs: the radio stays in TX and plays one frame
after the other, each from its own DAC bank for ``<us>``, the next built on the other CPU
meanwhile (a late build keeps the old frame on air and counts as dropped). The server
decodes what it catches, skips a frame it already has after the header (``dup``), and
reports as usual; the client's "acked" counts frames sent. Frame n carries bytes that depend
on n, and the RS and CRC-32 check them.

64-QAM, RS, 26 MHz, 30 dB (received by the server, 8 s; MER 23.3 to 23.5 dB):

======== ========== =========== ====== ======
Period   Offered    Received    Lost   Dups
======== ========== =========== ====== ======
7000 us  3.19       3.00        0      658
6000 us  3.72       3.50        0      96
5500 us  4.05       3.50        7.8 %  3
5000 us  4.46       3.38        18.7 % 0
======== ========== =========== ====== ======

(Mbit/s; before the search split and the vector derotation: 3.04 with 12.5 % lost at 6000
us.) The receiver's cycle is about 5.5 ms (capture 0.4, the stages above, and the captures
that straddle a bank switch): a shorter period only loses frames, a longer one repeats them
(the repeats are skipped after the header). With the float decoder
the same boards reached 2.66 Mbit/s with 10 % lost at 7 ms, over the MAC (stop and wait,
CSMA) 2.3 to 2.4. The MER of the link drifts by a dB or more during a session (22.3 to 23.7
dB); more errors per RS unit make the FEC stage slower (1.8 to 2.8 ms).

Parity (``CONFIG_APP_RS_PARITY``, 32 bytes per unit by default, RS(255,223), 16
corrections): the unit needs about 4.7 corrections at an MER of 23.5 dB and twice that at
22.3, and the MER drifts that much within a session (it fell from 23.6 to 22.2 dB over four
hours of bench runs, the carrier offset from -22 to -28 kHz). 24 parity bytes carry 3.6 %
more payload and cut the FEC stage to 1.3 to 1.9 ms, which at 23.5 dB is a better link, and
at 22.3 dB loses half the frames (blind stream, 64-QAM, 26 MHz, received Mbit/s and lost):

======== ============= ============= ============= =============
Period   32, 23.5 dB   24, 23.5 dB   32, 22.4 dB   24, 22.3 dB
======== ============= ============= ============= =============
7000 us  3.00, 0       3.11, 0       2.98, 0.4 %   1.57, 47 %
6500 us  3.10, 3.7 %   3.34, 0.2 %
6000 us  3.04, 12.5 %  3.46, 4.1 %   3.10, 10.7 %  1.19, 66 %
======== ============= ============= ============= =============

16 parity bytes lose a fifth of the frames at 23.5 dB. The edge carriers (19 to 20 dB)
produce most of the errors; a model on the measured SNR gives a static 16-QAM zone on the
outer carriers, or bit loading in general, only 4 % over flat 64-QAM at equal error rate,
so neither is built. What the lower MER needs is 16-QAM (2.3 Mbit/s loss-free in the same
state): a rate adaptation, not more parity.

Light error correction
======================

With the sample clock drift corrected the 64-QAM payload arrives with a raw bit error rate
around 3e-6 (MER 26 to 27 dB): about one bit error in five long frames, so RS(255,223)
(12.5 % parity, 16 byte errors per 255) carries far more protection than the channel needs.
The frames name their code in the header (4 bits, ``link client ... -f <code>``):

- ``rs``: RS(255,223) as before, for raw error rates up to a few 1e-3.
- ``none``: the CRC-32 alone.
- ``bch<t>-<bytes>``: binary BCH codes (``src/bch.c``) on contiguous blocks of up to 64, 128
  or 256 bytes (GF(2^9), 2^10, 2^11), correcting t = 1 to 4 bit errors per block with m t
  parity bits: ``bch2-128`` is 20 parity bits per 1024, 2.3 %. ``bch1-8`` is a Hamming
  code of 56 data bits per 64.

The receiver checks the CRC-32 first (four bytes per lookup step, slicing by 4); only a
frame that fails it is decoded, block by block: the remainder register (a reflected CRC
with the code's generator, also four bytes per step while r <= 32), and for a block whose
remainder is not zero the syndromes, Berlekamp-Massey and a Chien search. The sender's
parity is the same register. The blocks are not interleaved: a 1024-bit block already spans
every subcarrier twice, and a symbol that goes wrong as a whole would sink an interleaved
frame of t-error blocks just the same.

Simulated (``tools/ofdm_sim.c``, ``FRAME=1 FEC=<code>``, 64-QAM, 26 MHz, the 80 MS/s frame
of 3213 payload bytes; frames decoded of 20):

=========== ========== ======= ======= =======
Code        User bytes 24 dB   27 dB   30 dB
=========== ========== ======= ======= =======
rs          2791       20      20      20
bch1-8      2803       5       20      20
bch2-128    3121       0       20      20
bch2-256    3168       0       20      20
bch4-64     2954       20      20      20
none        3209       0       8       20
=========== ========== ======= ======= =======

``bch2-256`` carries 13.5 % more than ``rs`` where the link runs (the board's MER, about the
simulator's 27 to 30 dB); ``bch4-64`` keeps RS's margin at 24 dB with 6 % more. A Hamming
code of 64-bit blocks costs what RS costs and corrects far less.

Building and running
********************

Build with sysbuild (MCUboot, USB DFU) and flash each board once:

.. code-block:: console

   west build --sysbuild -b xiao_esp32s3/esp32s3/procpu esp-sdr-zephyr/apps/link
   west flash

Later updates go over USB DFU with ``west dfu -d <build dir> [-s <usb serial>]``
(see ``apps/osmosdr/README.rst``).

On the shell (``/dev/serial/by-id/usb-Zephyr_Project_ESP-SDR_Link_*``):

.. code-block:: console

   link status                       # address, settings, counters, timing
   link set txgain 4 rxgain 32       # several at once; also bw, cbw, amp, air, turn, ...
   link server                       # on the receiving board
   link client 0x00 -t 10 -m 256qam -f rs -w 2

or from the host, for both boards at once:

.. code-block:: console

   tools/link_perf.py run --server <serial> --client <serial> -t 10 -m 256qam \
           -f rs -w 2 --set txgain=4 --set rxgain=32     # --dst 0xff: broadcast, no ACKs

``tools/link_perf.py dump`` saves a capture for ``tools/qam_sim.c``, the
host simulator of the PHY (see its header for the build line).

OFDM over the MAC, both boards set alike:

.. code-block:: console

   tools/link_perf.py run --server <serial> --client <serial> -t 20 -m 64qam \
           --set phy=1 --set txgain=4 --set rxgain=32 --set ackair=200

``ofdm set`` changes the layout (several keys at once: bw, ch, cp, fs,
rxdiv, pilots, smooth, amp, syms, mod for the raw test). The raw PHY test
outside the MAC, with known test bits:

.. code-block:: console

   ofdm set bw 26 ch 84 mod 64qam
   ofdm tx -t 10                     # on the sending board
   ofdm rx -n 100 -v                 # on the receiving one; -v: channel per subcarrier,
                                     # -d: dump the first capture with errors
   ofdm bench                        # FFT and RS decode times

   tools/link_perf.py ofdm --rx <serial> --tx <serial> -n 100 \
           --set txgain=4 --set rxgain=32 --set bw=30 --set ch=96

``tools/ofdm_sim.c`` is its host simulator (PHY, link frames with
``FRAME=1``, ACKs with ``ACK=1``, the two-CPU split with ``SPLIT=1``); it
also decodes a capture saved with ``dump``.
