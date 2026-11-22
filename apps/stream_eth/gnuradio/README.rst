GNU Radio examples for the S31 VITA 49.2 stream
###############################################

Receive the stream of the ESP32-S31 (``apps/stream_eth``: cs8, signed 8 bit complex items in network
order, one unicast UDP stream subscribed by the host, up to 40 MS/s or 80 MB/s) in GNU Radio on the PC.
Run everything inside ``nix-shell -p gnuradio.pythonEnv`` (its ``python3`` has gnuradio, PyQt5 and
numpy; GNU Radio 3.10.12 here). The flowgraphs are hand-written Python, not ``.grc``: they need a
function-only block (pipe handling, frame skipping) that GRC would hide in an embedded Python block
anyway, and a ``.grc`` could not be generated and checked here without a display.

.. list-table::
   :header-rows: 1

   * - File
     - Purpose
   * - ``vrt_pipe.py``
     - high rate: ``vrt_udp -o -`` into a pipe, spectrum and waterfall (up to 40 MS/s)
   * - ``wideband_peaks.py``
     - averaged and max-held wideband spectrum with a peak list (the 2.4 GHz band)
   * - ``vrt_source.py``
     - pure Python source block, the packet format as code, tags like UHD (low and medium rates)
   * - ``fake_vrt.py``
     - fake board and a stand-in for ``vrt_udp``, for tests without hardware
   * - ``golden_gen.c``
     - writes the board's context packet, the command packets and the acknowledges with libvrt (the golden packets of the self test)
   * - ``selftest.py``, ``bench.py``
     - headless checks and throughput measurements

Pipe receiver with spectrum and waterfall
=========================================

.. code-block:: shell

   nix-shell -p gnuradio.pythonEnv --run 'python3 vrt_pipe.py fe80::1%eth0 -r 40e6 -f 2.44e9 -g auto'

``vrt_pipe.py`` starts ``vrt_udp <board> -r .. -f .. -g .. -o -`` (``--vrt-udp PATH`` if it is not
in ``PATH``) with a pipe raised by ``F_SETPIPE_SZ`` to ``/proc/sys/fs/pipe-max-size`` (1 MiB by default,
13 ms at 40 MS/s), and reads it with ``file_descriptor_source``. ``vrt_udp`` (IPv6 preferred, it prints the family it uses; exit status 3 when another host has the
stream, the launcher passes that message through; ``-T`` there takes the stream over) zero fills lost packets, so
the stream is continuous, and it prints its status on stderr, which the launcher passes through. The
displays get one frame of 4096 samples in K, chosen from the rate for about 60 frames per second and
taken from the raw bytes before the conversion to complex, so a slow GUI cannot hold back the reader.
When ``vrt_udp`` exits (``-t``, a crash, the board going away) the pipe closes, the flowgraph runs dry
and the window says so and keeps the last frames. ``vrt_pipe.py`` and ``wideband_peaks.py`` exit with
``vrt_udp``'s status and name it on stderr (3: another host has the stream, 4: the board refused a setting, 5: no
stream). ``--no-gui --seconds N`` runs without a window.
The flowgraph is ``file_descriptor_source`` to ``stream_to_vector``, ``keep_one_in_n``,
``vector_to_stream``, ``interleaved_char_to_complex`` (scale 1/128) and the Qt frequency and waterfall
sinks. For a full rate consumer (a demodulator, a file) connect after ``interleaved_char_to_complex``
on the whole stream instead; the scaling chain alone drains the pipe 24 times faster than 40 MS/s.

Wideband peaks
==============

.. code-block:: shell

   nix-shell -p gnuradio.pythonEnv --run 'python3 wideband_peaks.py fe80::1%eth0 -r 40e6 -f 2.44e9'

Shows what 40 MS/s buys: the 2.42 to 2.46 GHz band at 9.8 kHz per bin (4096 points), so a Wi-Fi
channel is 2000 bins wide and a BLE channel 100. About 200 frames per second are FFTed with a
Blackman window and averaged (alpha 0.1) and max-held; the five strongest local maxima of the average
are listed under the plot. ``--no-gui`` prints them once a second. To move the centre frequency restart
with another ``-f``.

Python source block
===================

``vrt_source.py`` is a ``gr.sync_block`` that subscribes to the board, parses each datagram with
``struct`` (header word, stream ID, TSI and TSF, payload, trailer; context packets with CIF0 fields
rf frequency and sample rate in Q44.20) and outputs ``complex64``. A gap is filled with zeros and tagged.
It is found from the jump of the packet time (index = seconds times rate plus the fraction), which shows lost
packets in any number and also samples the board dropped without a counter gap; before the first context
packet gives the rate it falls back to the 4 bit counter. Packets are held back four deep, so two swapped
packets are put in order and make no gap; one too late for that is dropped and counted in ``.late``. A jump of
the time index of more than a second, either way, is a restart of the board's index: no fill, a ``restart``
tag. Tags (UHD style): ``rx_time`` (tuple of seconds and fraction) at the first sample and after every
discontinuity, ``rx_freq`` and ``rx_rate`` (double) from the context, ``overflow`` (lost samples) on the first
filled sample, ``restart`` (count) on the first sample of a new index. Use it in Python or wrap it in a GRC
embedded block:

.. code-block:: python

   from vrt_source import vrt_source
   src = vrt_source("fe80::1%eth0")            # IPv6 link-local needs the scope; IPv4 works too
   src = vrt_source("fe80::1%eth0", takeover=True)   # take the stream from another host
   src = vrt_source("fe80::1%eth0", rate=8e6, freq=2.44e9, gain="auto")   # also set the radio

The board streams from UDP port 4992 to the source port of the subscriber (the socket is connected, so
only the board's packets arrive). The handshake is the board's: ``VRTS`` is answered by ``VRTC`` plus an
8 byte cookie, ``VRTS`` plus cookie subscribes (``VRTK``) and is repeated every 0.25 s as the renewal (the
board wants one in 5 s; a fresh ``VRTC`` replaces the cookie), ``VRTX`` plus cookie stops when the flowgraph
stops. ``VRTB`` (another host has the stream, at the start or on a renewal because one took it from us) ends the flowgraph (``top_block.wait()`` returns; a block that raised in its thread would leave it
hanging) with the ``VrtBusy`` message on stderr and in ``src.error``; ``takeover=True`` sends ``VRTT`` plus cookie
for the first subscription instead. ``rate``, ``freq`` and ``gain`` (``"auto"`` or the gain index) are sent as a VITA 49.2 command packet
(``vrt_source.command()``, byte for byte what ``vrt_udp.c`` writes with libvrt: EXECUTE with the error words asked
for, CAM bit 16) after ``VRTK`` and with the renewals (``VRTS`` plus cookie) until the board answers, five tries at most.
The board marks a refusal by leaving *executed* (CAM bit 10) out of its acknowledge, and names the refused settings in
the error words; the block takes an answer as accepted only when it is executed and has no error words. A refusal, or
no answer, ends the flowgraph like ``VrtBusy`` (``src.error`` is a ``RuntimeError`` naming the settings, for example
"the board refused the settings: rf frequency (out of range), sample rate (out of range)"). The context packet is parsed field by field in
CIF0 order, skipping the CIF1 word of VITA 49.2. Packets are the board's: 20 byte header (header word,
stream ID 0x5d5d0001, seconds, picoseconds in two words), 1440 byte payload (720 samples at the LAN
MTU of 1492) and a 4 byte trailer; ``fake_vrt.py`` plays the board side of the handshake (and ``--other-host`` for ``VRTB``) with that
layout. The tests use IPv4 on 127.0.0.1 only; the IPv6 path is the same code with another address family.

Self test and benchmark
=======================

.. code-block:: shell

   nix-shell -p gnuradio.pythonEnv --run 'QT_QPA_PLATFORM=offscreen python3 selftest.py'
   nix-shell -p gnuradio.pythonEnv --run 'python3 bench.py'

``selftest.py`` needs no board and no display. Its first case uses golden packets encoded by libvrt
(``golden_gen.c``): the parser must read rate 8e6 and rf 2.44e9 from the board's context packet, the fake must
build the same bytes, ``command()`` must equal libvrt's command packets, and ``parse_ack()`` and the fake's
acknowledges must match libvrt's executed and refused ones (and a CAM without *executed* is a refusal), so the fake cannot drift from the
board unnoticed. ``fake_vrt.py`` sends a 125 kHz tone at 1 MS/s,
amplitude 0.5, in 720 sample packets (1440 byte payload), and drops packets 100 to 102, 300 and 450 to 452. Checked
through ``vrt_source`` (also: ``VrtBusy`` when another host holds the stream, samples with
``takeover=True``) and through the pipe path (``fake_vrt.py vrt_udp`` as the stand-in): the tone
frequency and level, the sample count, the zero runs at the right places with the right length, and
for the block the overflow, rx_freq, rx_rate and rx_time tags, and a loss free run for phase
continuity. More cases for the block: 20 lost packets in a row, samples the board dropped without a counter
gap, a restart of the time index (+50 s), two swapped packets (no gap, tone intact), and another host taking
the stream while the flowgraph runs (the flowgraph ends by itself, with ``VrtBusy`` in ``src.error``). Output on the development machine (i7-12700H)::

   continuity: 216000 samples, instantaneous frequency 125.00 kHz, std 0.00 kHz
   vrt_source: 432000 samples (expected 432000), tone 125.00 kHz (expected 125), rms 0.499 (expected 0.500), ...
   pipe: 432000 samples (expected 432000), tone 125.00 kHz (expected 125), rms 0.499 (expected 0.500), ...
   golden context: rf 2.44e+09 Hz, rate 8e+06, fake builds the same bytes: True; commands equal: True; ...
   acks: executed (True, []), refused (False, ['rf frequency (out of range)', 'sample rate (out of range)']), ...
   refusal: flowgraph ended: True; error: the board refused the settings: rf frequency (out of range), sample rate (out of range)
   16+ lost packets: 216000 samples, zero runs (packet, length) [(100, 20)], tags ['overflow', 'rx_time'], ...
   board drop, counter intact: 218160 samples, zero runs (packet, length) [(100, 3)], ...
   restart (+50 s): 216000 samples, zero runs (packet, length) [], tags ['restart', 'rx_time'], ... restarts 1
   swapped packets: 216000 samples, zero runs (packet, length) [], tags ['rx_time'], ...
   evicted: flowgraph ended by itself after 2.3 s: True; error: the board streams to another host ...
   busy: the board streams to another host: pass takeover=True (VRTT) to take it
   takeover: 72000 samples (expected 72000)
   settings: acknowledged True, rx_freq tags [2440000000.0, 2412000000.0]
   PASS

Measured with ``bench.py`` (``head -c 4G /dev/zero`` into the pipe, unthrottled, 4 GiB):

.. list-table::
   :header-rows: 1

   * - Path
     - Sustained
   * - pipe to null sink
     - 2860 MB/s
   * - pipe, cs8 to complex +-1, null sink
     - 1960 MB/s (980 MS/s)
   * - pipe, display branch of ``vrt_pipe.py``
     - 2350 MB/s
   * - ``vrt_source``, loopback, 720 sample packets
     - no loss to 24 MS/s (33k packets/s); at 32 MS/s 0 to 12 % of the samples lost between runs, 33 % at 40 MS/s

The needed 80 MB/s is 40 times below what GNU Radio takes from the pipe. The limit at 40 MS/s is the C
receiver and the network, not this side. ``vrt_source`` is for rates up to about 10 MS/s: live from the board it
ran 8 MS/s in real time with 0.01 % zeros, and lost 4 to 6 % at 20 MS/s. The loopback figure above (no loss to 24 MS/s)
also includes the Python fake sender and says nothing about the board.

Needs the board
===============

Interface values (port 4992, the cookie handshake, header, trailer bits 24/12 for sample loss, context
fields) come from the board's definition; the self test checks the code against the fake sender built
to it. Verified live on the board (by the coordinator): ``vrt_source`` at 8 MS/s over IPv6 with the rate set. Not
verified: the refusal and busy paths against the real board, the real ``vrt_udp`` in the pipe path, and what the
Qt displays look like (they were only started under ``QT_QPA_PLATFORM=offscreen``).
