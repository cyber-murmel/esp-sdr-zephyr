.. _esp_sdr_link:

ESP-SDR link
############

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

========  =========  ==============  ==============
Mod       FEC        pairs (``-w 2``)  single (``-w 1``)
========  =========  ==============  ==============
64-QAM    RS         1.29 Mbit/s     1.19 Mbit/s
64-QAM    Hamming    1.41 Mbit/s     1.28 Mbit/s
64-QAM    none       1.54 Mbit/s     1.40 Mbit/s
256-QAM   RS         1.69 Mbit/s     1.47 Mbit/s
256-QAM   Hamming    1.44 Mbit/s     1.35 Mbit/s
256-QAM   none       0.89 Mbit/s     0.60 Mbit/s
========  =========  ==============  ==============

The link is bound by the receiver's decode time (about 2.3 us per symbol
per CPU), not by airtime. The MER ceiling of about 31 dB, the same with
or without attenuation, rules out 1024-QAM and above on these boards.

OFDM
****

A second PHY (``src/ofdm.c``), so far as a raw test outside the MAC
(``src/ofdm_test.c``, the ``ofdm`` shell command): one board loops a frame
of known test bits on the DAC, the other decodes captures of it and counts
bit errors.

- Configured by occupied bandwidth, subcarrier count and modulation (BPSK,
  QPSK). The FFT size follows: the power of two (16 to 512) that spaces that
  many subcarriers over that bandwidth at 80 MS/s. The subcarriers sit on
  both sides of DC, DC unused.
- A frame is a Schmidl-Cox preamble (known BPSK on the even subcarriers), a
  pilot symbol for a one tap channel estimate per subcarrier, and the data
  symbols, each with a cyclic prefix (FFT size / ``cp``). The frame fills
  half the capture window by default, so a whole copy always fits.
- The receiver works on the raw capture words: an integer sliding Schmidl-Cox
  search (coarse, then at full resolution over the prefix), the carrier
  offset from its phase (to +-1 subcarrier spacing), a derotation table per
  frame, the FFT, and a common phase per data symbol measured against its own
  decisions.
- It can receive at 40 MS/s (``rxdiv 2``, the default): the radio
  decimates, the receiver runs at half the FFT size and half the work.
- The FFT is the esp-dsp ESP32-S3 kernel (``CONFIG_APP_OFDM_ESP_DSP``,
  west project ``esp-dsp``), 1.8 times faster than the same algorithm in C
  from IRAM. Its context (tables and buffers, 16 KB) sits in internal RAM
  above the capture bank.

Measured with the same setup as above, 100 captures each, no bit errors in
any of these:

=============  ===========  =====  ==========  =======  ============
Bandwidth      Subcarriers  FFT    Air rate    MER      Decode
=============  ===========  =====  ==========  =======  ============
16 MHz, QPSK   52           256    26.8 Mb/s   30 dB    3.0 ms
16 MHz, BPSK   52           256    13.4 Mb/s   30 dB    3.0 ms
18 MHz, QPSK   116          512    27.6 Mb/s   29 dB    2.7 ms
36 MHz, QPSK   116          256    59.8 Mb/s   26 dB    5.4 ms
=============  ===========  =====  ==========  =======  ============

The decode time is per frame (about 100 us on air) on one CPU; the 36 MHz
frame needs the 80 MS/s receiver. In its 40 MHz mode (``cbw 2``) the radio
passes the whole +-18 MHz within +-3 dB end to end; in the 20 MHz mode
(``cbw 0``) the 36 MHz frame fails. The transmitter compresses before the
DAC words clip: wide frames need a lower ``amp`` (90 is the default; at
120, 116 subcarriers drop to 16 dB MER).

Building and running
********************

Build with sysbuild (MCUboot, USB DFU) and flash each board once:

.. code-block:: console

   west build --sysbuild -b xiao_esp32s3/esp32s3/procpu esp-sdr-zephyr/apps/link
   west flash

Later updates go over USB DFU with ``scripts/esp-sdr-update.sh <serial>
<zephyr.signed.bin>``.

On the shell (``/dev/serial/by-id/usb-Zephyr_Project_ESP-SDR_Link_*``):

.. code-block:: console

   link status                       # address, settings, counters, timing
   link set txgain 4                 # also rxgain, bw, cbw, amp, air, turn, ...
   link server                       # on the receiving board
   link client 0x00 -t 10 -m 256qam -f rs -w 2

or from the host, for both boards at once:

.. code-block:: console

   tools/link_perf.py run --server <serial> --client <serial> -t 10 -m 256qam \
           -f rs -w 2 --set txgain=4 --set rxgain=32

``tools/link_perf.py dump`` saves a capture for ``tools/qam_sim.c``, the
host simulator of the PHY (see its header for the build line).

The OFDM test, on the shell or from the host (both boards set alike):

.. code-block:: console

   ofdm set bw 16 ch 52 mod qpsk     # also cp, amp, syms, rxdiv
   ofdm tx -t 10                     # on the sending board
   ofdm rx -n 100 -v                 # on the receiving one; -v: channel per subcarrier

   tools/link_perf.py ofdm --rx <serial> --tx <serial> -n 100 \
           --set txgain=4 --set rxgain=32 --set bw=36 --set ch=116 --set rxdiv=1

``tools/ofdm_sim.c`` is its host simulator; it also decodes a capture
saved with ``dump``.
