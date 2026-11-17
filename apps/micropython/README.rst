.. _esp_sdr_micropython:

micropython
###########

Overview
********

MicroPython (its Zephyr port, built by MicroPython's Zephyr module) as the main program of the
XIAO ESP32S3, with a ``gnuradio`` stand-in that runs flowgraphs exported from GNU Radio Companion
unmodified: copy the ``.py`` file GRC writes to the board and run it, or start it from the
launcher's menu as an app (`Launcher and apps`_). The same app builds headless for the ESP32-C5-Zero, the bench's
coordinator, which also drives the bench's RF switch (`The ESP32-C5 image`_).

What runs:

.. list-table::
   :header-rows: 1

   * - In the export
     - On the board
   * - ``soapy.source`` (any ``driver=``)
     - the board's receiver: continuously at ``esdr.RATES`` (the S3: 500 kS/s and its halvings
       to 15.625 kS/s; offset tuning), or at the native 80, 40 or 16 MS/s in bursts of 16380
       samples, contiguous within
       a burst (wideband displays, short captures; 40 and 80 MS/s open the analog filters to
       +-18 MHz; displays there stream without a gap where the radio can, see
       `The WHY2025 badge`_); other rates are rounded to the nearest; one RF gain index (any gain name sets
       it, as the host plugin ``driver=esdr`` does, except that GRC's HackRF block's ``AMP``,
       ``LNA`` and ``VGA`` add up, the amp counting 11)
   * - ``soapy.sink`` (any ``driver=``)
     - the board's transmitter. A periodic signal (a ``sig_source_c`` tone that fits a loop
       within 1 Hz, a repeating vector or file of up to 16380 samples at the DAC rate) loops
       at the DAC's own 40 MS/s (80 MS/s for a rate of 80 MS/s) with the analog filters at
       +-18 MHz: any tone in the band, no CPU, changes applied at once. Anything else goes
       through the interpolating DAC stream, gapless up to 500 kS/s (a generated signal
       streams at up to 500 kS/s whatever the flowgraph's rate). Rates divide 40 MS/s or are
       80 MS/s (others are rounded); one transmit gain step from 0 (weakest, the default) to
       17, which any gain name sets (clamped, as with ``driver=esdr``); a centre within 500 Hz
       of the frequency asked for. Transmitting holds the radio: no receive flowgraph meanwhile
   * - ``analog.sig_source_c``
     - generated in C (sine table, 16 bits); frequency, amplitude, offset and waveform change
       at once while transmitting; square, triangle and sawtooth are real (Q is zero)
   * - ``blocks.file_source`` (``gr_complex`` or ``short*2`` items), ``vector_source_c``
     - read into memory before the transmission starts (the PSRAM heap: about a million
       samples); host paths are looked up under ``/SD:``, then ``/FLASH:``
   * - ``blocks.throttle``, ``blocks.multiply_const_cc`` (a real constant)
     - in a transmit chain: throttle passes samples through (the radio sets the pace),
       multiply_const scales them, also while transmitting
   * - ``blocks.head``, ``blocks.null_sink``
     - as in GNU Radio
   * - ``blocks.file_sink`` (``gr_complex`` or ``short*2`` items)
     - buffered in PSRAM during the capture and written to the SD card when the flowgraph
       stops; host paths are placed under ``/SD:`` (``/tmp/rx.fc32`` ->
       ``/SD:/tmp/rx.fc32``), readable with ``numpy.fromfile()``
   * - ``qtgui.freq_sink_c``, ``waterfall_sink_c``, ``time_sink_c``, ``const_sink_c``
     - drawn on the board's display (LVGL canvases filled by C helpers), placed as the
       export's layouts place them, in the board's colours (``modules/dusk.py``, derived in
       OkLab by ``tools/dusk.py``): a waterfall uses its heat scale, dark purple through
       orange to near white, whatever colour map the export sets. GRC's Grid switch
       (``enable_grid()``) gives a waterfall fine and coarse lines for frequency and time,
       spaced to suit the radio's real rate and the plot, and named on its title row;
       frequency lines sit at multiples of their spacing, time lines scroll with their rows.
       ``waterfall_sink_c.set_grid_spacing(freq_fine, freq_coarse, time_fine, time_coarse)``
       (Hz and seconds, not in GNU Radio; ``None``: automatic, 0: off) sets them. A frequency
       plot follows the same switch: the same frequency lines (``freq_sink_c.set_grid_spacing(
       freq_fine, freq_coarse)``) and lines every 10 and 20 dB where they fit, none when it is
       off. The MCH2022 badge's FPGA draws its plots without these. ``set_fft_size()`` changes
       a sink's FFT while it runs: GRC has no callback for it, so a flowgraph's FFT chooser
       needs a Python Snippet to connect the two (``eggs/waterfall/waterfall.grc``). Where the
       channelizer draws the rows (40 and 80 MS/s, gapless) its 125 kHz bins set the
       resolution instead
   * - QT GUI Range, Chooser (combo box and radio buttons), Check Box, Push Button, Label,
       Entry (shown, not editable)
     - LVGL widgets; the rotary encoder moves between them (turn), its switch edits or
       presses the selected one, and the export's setters run (a frequency variable retunes:
       a waterfall's picture moves with it, showing 0 power where it has no history, and comes
       back when tuned back; on the FPGA it stays put; a new sample rate clears it). On a Range
       of more than 100 steps a fast turn takes
       bigger steps (n detents between two display updates move n * n steps). A value the
       flowgraph refuses, such as a frequency outside the range the PLL locks over (about 1.8
       to 2.8 GHz, it varies from chip to chip), stops the slider at the last value that works
   * - ``QVBoxLayout``, ``QHBoxLayout``, ``QGridLayout`` (GUI hints ``row, col, rowspan,
       colspan``), ``QTabWidget`` (GUI hint ``tab@n``)
     - placed on the board's screen; a tab widget shows one page at a time; a scroll area
       does not scroll
   * - The rest of a Qt GUI export (``PyQt5``, ``sip``, settings, styling)
     - accepted, does nothing; ``QApplication.exec_()`` runs the display until Ctrl-C (or the
       knob held for 1.5 s), which calls the export's own SIGINT handler; when the loop ends,
       the window closes as on a desktop (the export's ``closeEvent()`` stops the flowgraph)
   * - ``signal``, ``threading.Event``, ``argparse``, ``eng_notation``, ``firdes``,
       ``fft.window``, ``pmt``
     - stand-ins for what every export imports
   * - ``digital.gfsk_mod``
     - as GNU Radio's (Gaussian filter, frequency modulator), in C, a packet at a time
   * - ``digital.gfsk_demod`` with ``digital.correlate_access_code_bb_ts``
     - a burst receiver in C (`Packet flowgraphs`_) that delivers what the correlator does:
       the payload after the access code and the two lengths of
       ``header_format_default``, up to 255 bytes
   * - ``digital.protocol_formatter_bb`` (``header_format_default``),
       ``blocks.tagged_stream_mux``, ``blocks.repack_bits_bb``
     - on each packet of a packet flowgraph
   * - ``pdu.pdu_to_tagged_stream``, ``pdu.tagged_stream_to_pdu`` (byte items)
     - a PDU becomes a burst on the air; a received frame a PDU, with ``snr`` (dB),
       ``rssi`` (dBFS), ``freq_offset`` (Hz) and ``errors`` (access code bit errors) in its
       metadata
   * - Message ports and connections, ``blocks.message_strobe``, ``message_debug``,
       ``msg_pair_to_var``, QT GUI Msg Push Button, embedded Python blocks with message
       ports only
     - messages pass as in GNU Radio, queued and handed over in order by the Qt GUI loop;
       a strobe fires from that loop; an embedded Python block runs as it is

A flowgraph either receives (one ``soapy.source``, an optional ``head``, sinks), transmits
(one chain from a signal source through ``throttle``, ``multiply_const_cc`` or ``head`` to
``soapy.sink``), or is a packet flowgraph.

Not yet: other signal processing blocks (filters, demodulators, ...), the other qtgui sinks
(accepted, not drawn).

Packet flowgraphs
=================

A flowgraph with the packet blocks shares the radio between a receive chain, ``soapy.source``
-> ``gfsk_demod`` -> ``correlate_access_code_bb_ts`` -> (``repack_bits_bb`` ...) ->
``tagged_stream_to_pdu``, and a transmit chain, ``pdu_to_tagged_stream`` -> (header and
payload through ``protocol_formatter_bb`` and ``tagged_stream_mux``) -> ``gfsk_mod`` ->
(``multiply_const_cc``) -> ``soapy.sink``; either may be missing, the rest are message blocks
(``modules/gnuradio/_link.py``). It runs in the Qt GUI loop (a Qt GUI export).

- The radio receives or transmits, never both (half duplex, as the host plugin). It receives
  into a ring in memory, which the receiver reads at every pass of the loop (on the C5 the
  radio engine runs the receiver itself). A PDU into ``pdu_to_tagged_stream`` becomes a burst:
  the receiver stops, the packet is modulated, the burst plays at ``soapy.sink``'s frequency
  and gain, the receiver starts again, about 20 ms plus the burst later. A burst waits while a
  frame comes in (up to 0.3 s) and for 40 ms after one (the peer turns round: the C5 listens
  again some 25 ms after its burst), and waiting bursts go out newest first (a reply before
  what waited).
- The receiver follows the peer's carrier: a frame's carrier offset moves the receive tuning
  (not the transmitter's, so that two boards do not chase each other), at the receiver's next
  start. Two boards' crystals differ by up to some 10 ppm (25 kHz between the bench's S3 and
  C5), which leaves one tone at the edge of the C5's receiver and costs it bits. The
  ``freq_offset`` a PDU carries is the peer's carrier against the frequency set.
- A receive run that keeps losing samples starts again when idle (``rx_stats()``: ``gaps``).
- Continuous receive rates only (250 kS/s down), the same rate both ways, up to 500 kS/s.
- The receiver (``digital/_fsk.py``, in C in ``cmod/esdr_fsk.c``): a discriminator, its sum
  over a symbol, and per sampling phase a search for the access code's last 32 bits against
  the mean of that phase's last 32 symbols. Around a hit, the phase that correlates best with
  the whole code wins; the code gives the carrier offset and the deviation and is sliced again
  against that offset (at most ``threshold`` errors). Carrier offsets of the order of the
  deviation are fine (tens of kHz between two boards' crystals); no lock-in time beyond the
  code, no timing tracking (frames up to 255 bytes). Real GNU Radio's ``gfsk_demod`` wants the
  offset well inside the deviation.
- Put a preamble in the access code (``0101...``, 32 bits) and keep the code balanced (as
  many ones as zeros): the receiver measures the offset over the code.
- GNU Radio's ``gfsk_mod`` holds a packet's last few bits in its filter until the next packet
  comes: end packets with a couple of padding bytes when the flowgraph is to run in GNU Radio
  as well (the board flushes the filter).

Hardware
********

On the XIAO ESP32S3, as in `mp-lvgl-example <https://github.com/cyber-murmel/mp-lvgl-example>`__:

- ST7789V 320x240 display and SD card on SPI2 (SCK D8, MISO D9, MOSI D10); display CS D2,
  D/C D1, reset D0; SD card CS D7. The SD card runs at 12 MHz: the pins reach SPI2 through
  the GPIO matrix.
- Rotary encoder on D3/D4, its switch on D5.

On the Hackaday Communicator badge (``communicator_badge/esp32s3/procpu``), the board's own
hardware (Zephyr's board documentation):

- The 428x142 NV3007 display; the launcher and the GUI lay out for its size
  (``esdr.display_size()``). What does not fit below the header scrolls into view when it takes
  the focus.
- The keyboard in place of the knob: up and down move the focus, left and right change values,
  Enter selects, Esc goes back, Esc held 1.5 s returns to the launcher.
- No SD card: only ``/FLASH:`` (``esdr.sd_info()`` raises ENODEV). The display SPI runs without
  GDMA, and UART0, the SAO bus and the LoRa module are off, for the image to end below the ring
  banks.

Storage and USB
***************

- ``/FLASH:``: a 4 MB FAT volume in the upper half of the flash, formatted on first use. It
  holds the scripts and apps (``apps/``) and is the working directory (``boot.py``,
  ``main.py`` run from it).
- ``/SD:``: the SD card, if one mounts (FAT or exFAT; it is never formatted).
- Both are USB drives (mass storage, LUN 0 FLASH, LUN 1 SD card). While the board writes a
  volume (a ``file_sink`` at the end of a capture) the host sees the medium removed, for at
  least 3 s, and reads it afresh when it comes back. Copy scripts while no flowgraph writes.
- The REPL is on the CDC-ACM port,
  ``/dev/serial/by-id/usb-Zephyr_Project_ESP-SDR_MicroPython_<serial>-if00``.

Running an export::

   >>> exec(open("rx_gui.py").read())

or name it ``main.py`` to run it at boot instead of the launcher. ``mpremote`` and
``pyboard.py`` work too, but they soft-reset the board when they connect (into the raw REPL,
which does not start the launcher).

Launcher and apps
*****************

The board starts into a menu of apps; the knob selects one and starts it. An app is laid out
as in the `badge.team hatchery <https://badge.team/docs/hatchery/>`__: a folder whose
``__init__.py`` runs the app when imported, and whose ``metadata.json`` describes it:

.. code-block:: json

   {
     "name": "Waterfall",
     "description": "A gapless waterfall of 250 kHz.",
     "category": "receive",
     "author": "esp-sdr",
     "revision": 1
   }

``category`` is the menu's section (``receive`` and ``transmit`` come first), ``"hidden": true``
keeps an app out of the menu, and so does ``"needs"`` naming an ``esdr`` function the board's module
lacks (Bursts needs ``trig_start``). ``tools/mkegg.py flowgraph.grc --category receive`` makes the
folder of a GRC flowgraph: the flowgraph, its export (``grcc``, unmodified), an ``__init__.py`` that
runs the export's ``main()``, and the metadata from the flowgraph's options (title, description,
author). Run it again after changing the flowgraph; the revision goes up when the export changes.

The menu lists:

- the apps bundled with the firmware, ``eggs/``: Spectrum and Waterfall (125 kS/s to 5 MS/s in
  13 steps, the nearest the radio has where it lacks one, gapless where the radio streams them,
  else from bursts, or 20, 40 and 80 MS/s: the bursts in the
  gapless stream where the radio has it, else bursts),
  Constellation,
  Receiver (tabs, radio buttons, a check box), Tone (transmits), Data link (two-way messages,
  `Data link`_), Pcap (Bluetooth LE and 802.15.4 packets in a table, `Pcap`_), Scan (a
  panorama of everything the radio tunes, `Scan`_) and, on the
  WHY2025 and MCH2022 badges, Bursts (80 MHz in channels, not a GRC
  export: the gapless stream in 40 channels with a burst detector each, ``esdr.chan_power()``
  and ``esdr.chan_bursts()``, where the radio streams it, else, or by its chooser, 64 channels
  of triggered snapshots, ``esdr.trig_get()`` and ``esdr.channelize()``). They are frozen;
  ``tools/eggs_index.py`` turns their ``metadata.json`` files into the index the menu reads;
- the folders in ``apps/`` on the FLASH and SD drives: copy an app's folder there over USB.

Each app runs in a fresh interpreter: the launcher keeps the choice across a soft reset
(``esdr.next_app()``), ``main.py`` runs the app, and when the app ends another soft reset
brings the menu back with the app selected. That soft reset also resets the radio: every stream stops
and its settings (frequency, gain, filter, channel bandwidth) go back to those ``esdr.init()``
found, so no app inherits another's (``esp_sdr_reset_settings()``; on the WHY2025 badge the
C6 gets ``ESDR_REQ_RESET``, and ``esdr.radio_state()`` shows what it holds). An app that fails shows its error until the knob
is pressed; the traceback is on the console.

Holding the knob for 1.5 s ends an app and returns to the menu, as Ctrl-C does (a GRC export's
SIGINT handler stops the flowgraph). Ctrl-C on the console ends an app or the menu into the
REPL. Holding the knob while nothing runs (the REPL waits), or again within 5 s when an app
ignored the first hold, restarts the board.

Without a display
=================

On a board without a display (no LVGL in the firmware, the C5), or with a file
``/FLASH:/headless`` on the S3, a Qt GUI export runs on the console
(``modules/gnuradio/_console.py``): ``QApplication.exec_()`` lists the controls of every tab
page with numbers, prints QT GUI Label values as they change, and runs the display loop's work
(timers, packet radios, messages) from a timer while lines typed at the console work the
controls::

   N           press push button N, toggle check box N, choose radio button N
   N VALUE     set range N, choose N's option (label or index), set entry N, check box N
               to 1 or 0; for a QT GUI Msg Push Button, send VALUE instead of its own value
   ?           the controls again
   !STATEMENT  run a Python statement

Ctrl-C ends the flowgraph as the knob does (the export's SIGINT handler). Sinks are not shown.

Data link
=========

``eggs/datalink``: text messages between two boards running it, or a board and a GNU Radio
peer running the same flowgraph. The flowgraph is GNU Radio's packet chain (`Packet
flowgraphs`_) around an embedded Python block, the link layer, which runs as it is on the
board and in GNU Radio:

- 2-GFSK at 250 kS/s, 25 kbaud (10 samples a symbol), +-25 kHz deviation, BT 0.5.
- A frame: ``header_format_default``'s header (a 64-bit access code, a 32-bit ``0101``
  preamble and a balanced sync word, then the length twice), then kind, the sender's id (2
  bytes), a sequence number, the text (up to 40 bytes) or, in an acknowledgement, the SNR the
  receiver measured; CRC-32 (little-endian) and two padding bytes. A short message takes about
  12 ms on the air.
- Stop and wait: a message goes again after 0.4 to 0.6 s without an acknowledgement, five
  times at most; a repeated message is acknowledged again and shown once. Auto sends a
  numbered message every 2 s (+-20 %).
- The Link tab: the text to send (turn to it, press, turn, press), Send, Auto, and four lines:
  RX the last message heard (sender id, number, text), TX messages sent, acknowledged, sent
  again and lost, Link the SNR, level and carrier offset of the last frame and frames whose
  CRC failed, Peer the SNR the peer measured, the round trip time and this board's id (random
  at every start). The Radio tab: frequency (2.401 to 2.482 GHz), transmit gain step (8 by
  default: through the bench's RF switch the S3's weakest step does not reach the C5) and
  receive gain.
- On the bench (an S3 and the C5 through the RF switch, gain step 8 both ways): every message
  acknowledged one way and the other, an occasional repeat; both in Auto with texts, about one
  message in 30 lost to collisions. Round trip about 0.3 s.
- ``tests/host/test_datalink.py`` runs the export with the stand-ins against a simulated
  peer, ``tests/host/test_fsk.py`` the modem.

Pcap
====

``eggs/pcap``: apps/pcap's packet capture (``doc/pcap.md``) on the board's own radio, its
packets in a table. Not a GRC export: a packet list is no flowgraph.

- One channel at 2 MS/s: Bluetooth LE advertising 37, 38, 39 or 802.15.4 11 to 26, cut into
  bursts by the detector of apps/pcap's channel mode; apps/pcap's decoders run over each burst
  (``cmod/esdr_pcap.c``, ``cmod/esdr_pcap_core.c``): Bluetooth LE packets (LE 1M), 802.15.4
  frames, and for the rest the generic analysis's record of modulation, symbol rate and bits
  (two bursts a pass, none while the capture is behind; the others are recorded with the
  detector's numbers). The packets are apps/pcap's records byte for byte, with their metadata
  comment.
- On the S3 in multiapp (the XIAO, the Communicator), the S31 (the VIEWE board) and the P4 (the
  WHY2025 badge) the engine: a thread above MicroPython runs the detector where the radio's
  samples land (``esdr.pcap_start()``) and closes the bursts into a pool of 5 ms buffers in
  PSRAM (48, more where the heap is large, fewer where it is short), a second one at
  MicroPython's priority decodes them into a ring of packets, and each pass of the display loop
  collects those as finished ``Pkt`` objects (``esdr.pcap_take_pkts()``). On the S3 the ring
  averages every fourth pair of its 16 MS/s in pairs (``esdr.PACKET_RATES``; the S3 has no
  filtered 2 MS/s) into SRAM bank 2, on the S31 the dump ring's gapless 2 MS/s goes into the
  capture bank, both from IRAM, so that the radio's CPU touches no flash or PSRAM
  (``lib/esp_sdr``'s ring stage, ``esp_sdr_ring_stage.h``); on the P4 the link fills a ring in
  the heap (``esdr.PCAP_RING_MIN``). A burst no decoder takes (one that fills the channel, or
  one for the generic analysis while half the pool is in use) goes over as its numbers alone and
  leaves its buffer free. Elsewhere the radio receives into a ring in the heap
  (``esdr.rx_start(wrap=True)``) and each pass detects and decodes what came
  (``esdr.pcap_feed()``). The standalone S3 image has no room for the ring's decimation by 4,
  and no Pcap app.
- The table (``eggs/pcap/pcap_view.py``; its dissector runs as C on the board,
  ``esdr.pcap_dissect()``, which a host test holds to the Python one), newest first: time since the start, level (dBFS),
  protocol, source, type and, where the display is wide enough, destination, length, channel
  and information (the device's name and manufacturer, a sequence number, a symbol rate). A
  type ends in ``!`` when the CRC failed. The bar at the table's right edge selects a row (the
  knob: turn to it, press, turn; a finger: drag); the lines below the table show it whole.
- Show filters: decoded packets with a good CRC (the default), all, Bluetooth LE, 802.15.4, the
  burst records, those with a bad CRC, or the packets of the source of the row selected when
  Source is chosen.
- Group makes a row per source, type, protocol or channel: its count, last time, loudest
  packet, bytes and the newest packet's information; the selected group's line shows its types
  and how many of each.
- Sort orders packets or groups: newest, oldest, loudest, longest on the air, most packets (the
  sources with the most first, when not grouped), source, type.
- Gain: automatic or a fixed index. Save writes the packets kept (1500 at most) to the SD card as
  ``/SD:/pcap/esdr-NNN.pcapng``: a section header, apps/pcap's three interfaces (Bluetooth LE
  256, 802.15.4 TAP 283, bursts 147 for ``apps/pcap/tools/esdr_burst.lua``) and a packet block
  each; Wireshark reads it as it reads apps/pcap's stream. Hold stops the table from changing
  while the capture goes on.
- Without a display the packets print on the console as lines of the table.
- A short display (the Communicator's 428 x 142) has two tabs: Packets, the table under the tab
  bar, and Setup, the choosers and buttons. A packet's fields are read from its record when
  something first needs them (a shown row, a filter, a group, a sort by source or type): some
  2 ms a packet on the S3, and most records of a busy band are never looked at.
- On the XIAO ESP32S3 (2026-11-09, an office full of Wi-Fi, automatic gain, 15 s on channel
  37): 1207 Bluetooth LE packets (advertising, scan requests and responses, extended
  advertising, named devices), the ring losing no input, the detector 37 % of CPU 0 and the
  decoders 18 %; with the display drawing, 1551 packets in 20 s, the table 3 frames a second
  (before the engine: 347 in 15 s, 222 in 20 s, the ring losing 8 to 20 % of its input whenever
  the other core ran, both cores sharing the cache in front of flash and PSRAM). 802.15.4
  acknowledgements, data frames and commands of the networks in range on 11, 15 and 25. CPU 0 is
  the limit: in the busiest moments some 5000 bursts in 15 s find no free buffer (mostly the
  band's Wi-Fi edges, among them some packets). The Communicator: 584 packets in 20 s with its
  display, more noise bursts at its receiver.
- ``tests/host/test_pcap_egg.py``: the capture's C (built with the host compiler) on synthetic
  Bluetooth LE and 802.15.4 streams, the detector and the decoders apart with the pool and the
  triage, the dissector, the filters, groups and sorts, and the app on a fake radio (with and
  without the engine, the short display's tabs), Save's pcapng read back;
  ``tests/host/test_ring_stage.py`` the ring stage.

Scan
====

``eggs/scan``: a survey of everything the board's radio tunes, as a panorama. Not a GRC export:
a sweep retunes in a loop. The radio hops its LO over the whole range its PLL locks in, an
80 MS/s burst of 8192 samples at each LO, and ``esdr.scan_hop()`` (``cmod/esdr_scan.c``) makes its
spectrum: 32 frames of 256 points, each frame's own centre (the LO's DC) taken out,
Blackman-Harris, a fixed-point FFT, so 312.5 kHz bins (16 to 5 MHz), and folds the hop's bins
into the panorama. On the S3 the FFT is esp-dsp's 16-bit kernel on the vector unit
(``cmod/esdr_scan_pie.c``), each frame scaled up by a power of two to fill 16 bits and the
kernel's rounding bias taken off, and the frame's range and window run there too
(``cmod/esdr_scan_vec.S``; its centre is then the middle of its range, which differs from the
mean in the bins within 3 of DC only); the P4 and the S31 run an int32 FFT in C. With two CPUs
the spectrum is made on the second (``esdr.scan_start()``) while the first tunes the next hop
(on the S3 and the S31 the capture runs on the second CPU, ahead of it). ``scan_view.py`` (pure
Python, tested on the host) plans the hops:

- LOs on a 5 MHz grid. Each panorama bin comes from the nearest LO, but within a guard around
  it (its DC, its 1/f skirt, the window's main lobe; 2.8 to 3.75 MHz) from the neighbour on its
  side, never farther than the step between LOs: 15 MHz on the S3 (its ``channel_bw(2)`` filter
  is within 1 dB to 15.6 MHz, 3 dB at 20), 20 on the S31 and the C6. Two plans whose LOs
  differ take turns, so what moves with the LO (DC residue, images, LO spurs) lands at other
  frequencies on the next sweep. The outer flanks beyond the end LOs come from one hop, shaded.
- The range: where ``tune()`` refuses an LO out of lock, bisected at start on the grid, an LO
  refused twice counting as refused (the S31 refuses one in a hundred at random), each edge a
  grid step in. The S3 and the S31 refuse by themselves; the C6 behind the P4 tunes as asked and
  reports where its PLL's VCO ended (``ESDR_STATUS_LO_*`` in ``struct esdr_state``), and the
  P4's ``tune()`` then puts it back and raises. A C6 image from before that report
  (``radio_state()["locked"]`` is None) takes a table instead: 1795 to 2845 MHz, a measured
  chip's range pulled in. The title says which.

The panorama shows the average power (orange), the strongest 3.2 us frame held (purple) and the
floor (gray: the quietest readings, rising 1 dB a sweep at most, so bursts and what follows the
LO stay out), in dB of a full-scale tone in a bin, at levels from the floor and the held peaks;
the bar under it is the marker, a column of the plot a step of the knob (in a zoomed span a step
past either end pans the view a quarter span that way, the view otherwise staying put), its label
the frequency and the three levels there (``x40`` at a crystal harmonic, the board's own known
lines named). Below, the history: a row a second at
most, the strongest of its sweeps. Gain (fixed: the AGC would move each hop's level; a change
starts the holds anew), Span (the whole range, 100 or 25 MHz around the marker; the sweep
always covers the whole range), Peak, Reset, Pause, and with an SD card Save: the history as
rtl_power CSV (``/SD:/scan/scan-NNN.csv``, uptime as 1970 dates, no RTC) and the panorama per
bin (``scan-NNN-sum.csv``, the settings in its first line). A hop that clips (more than 16
samples at full scale) goes to the history only; one the radio refuses twice, or that does not
finish in time, is marked over the plot. On the Communicator's short panel the panorama, the
history and the controls have a tab each; without a display a summary line every 10 s. Not on
the C5 (no bursts) nor the classic ESP32 (no lock flag, its fixed gain unverified): the launcher
hides it there (``esdr.scan_hop``).

Measured (2026-11-10, gain 48, the office): the XIAO 1840 to 2780 MHz of LO (panorama 1822 to
2797 MHz), 64 hops a plan, 0.54 s a sweep, 0.65 s with the display (a hop: tune 4.1 ms, the
burst 1.5 ms, the spectrum 2.1 ms on the second CPU meanwhile; inline 0.66 s; with the int32 FFT
9 ms a hop, 1.2 and 1.37 s a sweep); the VIEWE 2205 to 2870 MHz, 0.74 s, 0.79 s with the display
(0.90 and about 1 s before: its 5.6 ms spectrum on the second CPU, and rx_wait() looking every
1 ms instead of 10); the WHY2025 badge, bisected since its C6 reports the lock, 1800 to 2890 MHz,
56 hops, 1.41 s, 2.27 s with the display (1.52 and 2.43 s before: its 7 ms spectrum, 4.9 on the
second CPU, mostly within its 17 ms capture already; on the table, 1795 to 2845 MHz, 54 hops,
1.5 s). ``tune()`` now retunes once where the kHz offset stays (twice before: 7.7 to 4.1 ms on
the S3, 14 to 10.7 ms on the S31), and the P4 sends the C6 exact Hz (a float32 2405e6 was
2404 MHz plus 1000 kHz there, a second retune). The C6 (``apps/osmosdr_serial``) now rounds to the
nearest MHz and sets its kHz offset whenever it differs from the one the library keeps, also
back to 0: before, a whole MHz after a fractional one (the Tone app's float32 frequency) kept
the old offset, the LO off by it while the reported centre said otherwise.

Tests: ``tests/host/test_scan_core.py`` holds ``esdr_scan.c`` (built with the host compiler,
both FFTs: the S3's kernel and frame passes in C, which matched the XIAO's bit for bit) to
numpy; ``tests/host/test_scan_egg.py`` the plans (every bin owned once a plan, within the guard
and the step), the bisection, and the app on a fake radio of each flavour (the S3's float
arguments, the link's ints and keyword ``offset``, refusals before or after the LO moves, a C6
that reports its lock and one from before, each spectrum from the worker and no capture into the
buffer it reads, a stuck capture, a clipped hop, a gain change, tabs, headless, Save's lines).

The ``esdr`` module
*******************

The C side, used by the stand-ins and useful on its own: ``init()``, ``tune()``, ``rx_gain()``,
``rx_bandwidth()``, ``rx_start()`` / ``rx_wait()`` / ``rx_stop()`` / ``rx_stats()`` (capture
into a buffer, int16 I/Q), ``rx_latest()`` (the newest samples, for displays),
``sc16_to_fc32()``, ``psd()``, the drawing helpers (``stretch()`` on every port), ``scan_hop()``
/ ``scan_start()`` / ``scan_wait()`` / ``scan_q8()`` and ``RADIO`` (the Scan app's),
``tx_start()`` / ``tx_gen()`` / ``tx_scale()`` / ``tx_wait()`` / ``tx_stop()`` / ``tx_stats()``
(transmit a generated signal or int16 I/Q from a buffer), ``tx_loop()`` / ``tx_period()`` (loop
one period at the DAC rate), ``tx_gain_max()``, ``fc32_to_sc16()``, ``channel_bw()`` and
``tx_lpf()`` (the analog filters), ``BURST_RATES``, ``rx_start(wrap=True)`` and ``rx_count()``
(capture into a ring), ``fsk_rx_init()`` / ``fsk_rx()`` / ``fsk_rx_reset()`` /
``fsk_rx_state()`` and ``fsk_mod()`` (the packet modem, ``cmod/esdr_link.c``), ``pcap_new()`` /
``pcap_tune()`` / ``pcap_feed()`` / ``pcap_flush()`` / ``pcap_stats()``, ``pcap_dissect()`` (the
table's fields of a packet, ``cmod/esdr_pcap_dissect.c``: ``pcap_view.py``'s dissector in C) and
``pcapng_head()`` / ``pcapng_epb()`` (the Pcap app's capture and file, ``cmod/esdr_pcap.c``; on
the S3 in multiapp, the S31 and the P4 with ``pcap_start()`` / ``pcap_take()`` /
``pcap_take_pkts()`` / ``pcap_pending()`` / ``pcap_stop()``, the engine, and on the P4
``PCAP_RING_MIN``), on the S3 ``PACKET_RATES`` (4 and 2 MS/s from the ring's every fourth pair,
no filter, for packet captures; empty without ``CONFIG_ESP_SDR_RING_BOX4``), ``claim()`` /
``release()`` (a volume away from USB while writing), ``shell()`` (Zephyr shell commands, such
as ``usj`` and ``mcuboot confirm``), ``crash()`` and ``reset_reason()`` (why the board last
restarted), ``temp()`` (the radio chip's die temperature), ``lte_detect()`` and ``iq_power()``
(the LTE crystal calibration's estimator on a capture, ``cmod/esdr_lte.c``: the frozen
``ltecal`` module measures the radio's crystal with it, keeps the record in
``/FLASH:/esdr_cal.bin`` and sends it to the radio again whenever an app starts it,
``doc/lte-calibration.md``), ``sd_info()``, ``next_app()`` and ``home()`` (the launcher: the app
the next soft reset starts, whether the knob ended the app). The C5's ``esdr``
(``cmod/esdr_c5.c``) has the radio part for packet flowgraphs, ``rx_lsb()``, ``fsk_frames()``
and ``first_start()``; its docstring lists it. The WHY2025 and MCH2022 badges' ``esdr``
(``cmod/esdr_radio.c``, whose docstring lists it) has the same calls for a radio behind a link
or capturing bursts, ``radio_state()``, the triggered snapshots (``trig_start()``,
``trig_get()``, ``channelize()``, ``fb_image()``) and, on the P4, the gapless stream and its
channelizer (``gapless_start()``, ``chan_power()``, ``chan_bursts()``, ``chan_spectrum()``) and
``usb_to_c6()``.

Building and flashing
*********************

.. code-block:: console

   west build --sysbuild -b xiao_esp32s3/esp32s3/procpu esp-sdr-zephyr/apps/micropython
   west dfu

``west dfu`` finds the REPL on the CDC-ACM port and runs the MCUboot shell's commands through
``esdr.shell()``: it interrupts the running app (Ctrl-C), checks the new image and confirms it,
then restarts the launcher (Ctrl-D). By hand, or after ``west dfu --no-confirm``::

   >>> import esdr; esdr.shell("mcuboot confirm")

Without DFU access, hand the USB port to the USB-Serial-JTAG from the REPL
(``esdr.shell("usj")``) and write MCUboot at 0x0 and ``zephyr.signed.confirmed.bin`` at 0x20000
with ``esptool`` (``west dfu --esptool`` does all of it). That image is padded to its slot and
ends in a confirmed trailer; ``zephyr.signed.bin`` written plain leaves an earlier image's
trailer at the slot's end, which MCUboot reads as bad magic, and ``mcuboot confirm`` then fails
with -5. ``mcuboot`` reports -5 for the secondary slot's header too while that slot holds no
image, which is harmless.

``tools/check_capture.py`` checks a recording on the host (length, level, DC, strongest line).

Bench scripts
=============

``tools/bench/`` holds scripts for boards on the desk, run by ``tools/bench/bench.py PORT
SCRIPT [NAME=VALUE ...]`` (the settings go in front of the script; each run starts with a soft
reset):

- ``egg_sliders.py``: a bundled app (``EGG='"spectrum"'``) for a few seconds, imported as the
  launcher does, with every slider moved part way through its range (floats, as the knob gives
  them; ``SLIDERS=0``: none, a smoke run), and the frames its bands drew;
- ``waterfall_grid.py``: the Waterfall egg's picture through a retune and back, every rate of
  its chooser, the grid's spacings and what a redraw costs (``HOLD=1``: also that what is on the
  air stays on its frequency);
- ``datalink_auto.py``: the Data link egg in auto mode on two boards at once (start both),
  then the node's counters: ``acked`` against ``msgs``;
- ``trig_snapshots.py``: triggered snapshots (the WHY2025 and MCH2022 badges), channelized: how
  many came, how many broke, what ``channelize()`` costs; with ``F2`` also how many come per
  half second after retuning there and back;
- ``burst_capture.py`` on one board while another sends bursts, then
  ``burst_analyse.py`` on its output on the host: the envelope, where the burst lies, its
  frequency and what the Data link's GFSK receiver decodes (it found truncated bursts and a DC
  tracker that followed the signal).

.. code-block:: console

   tools/bench/bench.py /dev/ttyUSB0 egg_sliders.py EGG='"bursts"' SECS=10 SLIDERS=0
   tools/bench/bench.py /dev/ttyACM0 burst_capture.py > burst.txt
   tools/bench/burst_analyse.py burst.txt

The ESP32-C5 image
******************

The same app builds for the Waveshare ESP32-C5-Zero (``waveshare_esp32c5_zero/esp32c5/hpcore``),
the bench's coordinator: MicroPython without a display on the USB-Serial-JTAG console, the radio
for packet flowgraphs, the bench's RF switch from Python, and the bundled apps that run headless
(``CONFIG_APP_EGGS``: Data link). It is the C5's normal firmware.

.. code-block:: console

   west build -d build/micropython-c5 -b waveshare_esp32c5_zero/esp32c5/hpcore esp-sdr-zephyr/apps/micropython
   west flash -d build/micropython-c5 --esp-device /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<MAC>-if00

Simple boot, no MCUboot and no DFU (as for the C5's other apps). Every open of a
USB-Serial-JTAG port that changes DTR or RTS resets the chip: open it with both lines asserted
(pyserial: set ``dtr`` and ``rts`` before ``open()``), as ``mpremote`` does not.

- **Radio** (``cmod/esdr_c5.c``, the C5's ``esdr`` module): receive through the
  diagnostic-bus ring (``esp_sdr_diag.h``, 40 MB/s, box16 and CIC to the continuous rates 2.5
  MHz over a whole ratio, 250 to 15.625 kS/s in 10 steps), transmit through the CPU-paced TX DC path (``esp_sdr_paced.h``, 4 times
  interpolated); half duplex. The packet receiver runs inside the ring's consumer, so the
  samples need no buffer in RAM: ``rx_start(rate, fsk=state)`` and ``fsk_frames()``. A burst
  plays inside ``tx_start()`` with interrupts locked. The C5's level follows the samples only:
  the transmit gain steps 0 to 17 scale them, 1 dB a step. ``rx_lsb()`` picks the bus's 4-bit
  window (a signal above 8 << n of the ADC's 512 counts wraps). The ring's pads are GPIO 3 to
  7 and the test points 23 to 25: GPIO 0 to 2 and 8 to 10 drive the RF switch.
- **Memory**: no PSRAM. The Python heap is 20 KiB static, the upper half of SRAM block 1 less
  an 8 KiB guard (56 KiB), which the dump engine owns only while it captures (this image never
  captures: no ``rx_capture``, no automatic window), and what the libc heap has left less 4
  KiB: about 89 KiB free at a fresh prompt. A packet flowgraph's burst buffer (21 KiB, a 64-byte
  packet) is taken when ``gnuradio`` is first imported, while the heap is still in one piece.
  The Wi-Fi driver gets 40 KiB of kernel heap.
- **Headless**: the launcher lists the apps and stays in the REPL; ``launcher.run("datalink")``
  starts one, Ctrl-C ends it. A Qt GUI flowgraph's controls are on the console (`Without a
  display`_).

The RF switch
=============

``rfswitch`` (``modules_c5/rfswitch.py``) drives the bench's 8x8 RF switch on ``machine.Pin``:
side A on GPIO 0, 1, 2 and side B on GPIO 8, 9, 10, bits A, B, C of the port number minus one.

.. code-block:: pycon

   >>> import rfswitch
   >>> rfswitch.select(a=4, b=1)   # the C5 (A4) to the S3 (B1)
   (4, 1)
   >>> rfswitch.select(b=5)        # side B to the 50 ohm load, side A stays
   (4, 5)
   >>> rfswitch.path()
   (4, 5)

At the board's first start ``_boot.py`` selects A4-B1, or the path a file ``/FLASH:/rfswitch``
holds ("4 1"); soft resets keep the pins as they are.

The WHY2025 badge
*****************

On the WHY2025 badge the app runs on its ESP32-P4 (``why2025_badge/esp32p4/hpcore``): the
720x720 display, the keyboard as on the Communicator, the SD card at ``/SD:``, a 16 MiB Python
heap in PSRAM. Its radio is the badge's ESP32-C6 running ``apps/osmosdr_serial`` over SDIO
(``cmod/esdr_remote.c``). The REPL and DFU are on USB-C port J8 (CDC-ACM, OTG high speed);
the CH340 shows the boot messages only.

.. code-block:: console

   west build --sysbuild -b why2025_badge/esp32p4/hpcore esp-sdr-zephyr/apps/micropython
   west flash --esp-device /dev/ttyUSB0      # once, through the CH340: MCUboot and the app
   west dfu                                  # from then on, over J8

J8 reaches either the P4's USB or the C6's USB-Serial-JTAG (P4 GPIO2). ``esdr.usb_to_c6(s)``
hands it to the C6 for ``s`` seconds and then takes it back, which is how the C6 is updated
without the CH340 (a ``build`` directory of ``apps/osmosdr_serial`` for
``why2025_badge/esp32c6/hpcore``)::

   west dfu --c6 -d build-c6

``--c6-time`` sets how long the C6 keeps the port (30 s). The P4 reopens its SDIO link to a C6
that restarted by itself. The badge's P4 runs both CPUs (SMP, pin-only): CPU 1 the Pcap engine's
detector and decoders, the gapless channelizer and the Scan app's spectra, CPU 0 everything
else; a thread nobody pins stays there. On CPU 0 the USB threads are preemptible, above
MicroPython's (main, priority 10) and below the system work queue, which feeds the liveness
watchdog; a cooperative USB thread held it off through a DFU download until the board reset. The
EV board's P4 stays on one CPU.

With the C6's gapless stream (``apps/osmosdr_serial``: the badge's C6 builds it by default),
``esdr.gapless_start()`` streams the whole 80 MHz without a gap (with ``half=True`` 40 MHz at
40 MS/s: every other sample, behind an analog filter, so what lies around +-25 MHz folds in 15 to
25 dB down): the C6 keeps the 10 us windows that stand out, as many as the budget it is given
(2 MB/s by default) holds, and their samples go to the monitor (full scale 16384, as bursts), so the
spectrum and waterfall sinks show them; ``soapy.source`` at 80 and 40 MS/s takes this for displays. With ``channels`` the P4 also runs the
fixed-point polyphase filterbank and burst detector of ``apps/common`` (``esdr_pfbq.h``) on its PIE
SIMD in a thread of its own (``cmod/esdr_gl.c``, some 80 to 130 cycles a sample with more on the
air, its memory in PSRAM): per channel the power, floor, peak and share of time open
(``esdr.chan_power()``), and the bursts that ended (``esdr.chan_bursts()``: frequency, start, length
from the opening block to the last loud one, level). A hole in the stream longer than 0.5 ms ends a
burst; the radio's budget can cut a long burst short or into pieces. The Bursts app shows them.
``esdr.chan_spectrum()`` makes spectrum rows of the bursts alone: a loud block of a channel in a
burst is transformed over its 16 frames, the passband's 8 bins averaged over the burst and placed at
the channel's frequency (peak hold over the row), and the rest of the row reads the channels' floors
with a little noise, so the time the radio kept nothing and the quiet channels look quiet.
``soapy.source`` runs the channelizer on channels of 1 MHz for displays (125 kHz bins), and the spectrum
and waterfall sinks draw these rows (some 10 to 20 cycles a sample more).

The MCH2022 badge
*****************

On the MCH2022 badge (``mch2022/esp32/procpu``) the app runs on its classic ESP32: the
320x240 display, the buttons as an LVGL keypad (the joystick moves and changes, A and the
joystick's press select, B goes back, HOME held 1.5 s goes to the launcher), the SD card at
``/SD:``, a 3 MiB Python heap in PSRAM. The radio is the chip's own, which captures bursts only
(``cmod/esdr_local.c`` behind the interface ``cmod/esdr_remote.c`` has on the P4): the burst
rates come as captured, the stream rates (``esdr.RATES``: every rate 16 MHz / m reaches exactly
from 2 MS/s to 125 kS/s, ``cmod/esdr_rates.h``) are 16 MS/s bursts decimated by
the library, about 1 ms each with gaps between them (``esdr.rx_stats()``), good for displays,
not for demodulators. A display-only run captures a burst whenever the display has taken the
last one. No transmit: the launcher hides the apps that need it. The REPL is on the console
UART, which the badge's RP2040 bridges to its first USB serial port; flashing goes through it
too (no MCUboot, no DFU).

.. code-block:: console

   west build -b mch2022/esp32/procpu esp-sdr-zephyr/apps/micropython
   west flash --esp-device /dev/serial/by-id/usb-Badge.team_MCH2022_badge_*-if00

The Wi-Fi driver takes its heap from PSRAM and runs with a few buffers: the DRAM the image
leaves is the libc heap, and the radio only listens.

With a bitstream of the badge's FPGA (``gateware/``, see ``doc/fpga-accelerator.md``) built in,
the FPGA owns the display, LVGL's drawing passes through it, and the first waterfall and
frequency sinks of an app hand their plot areas to it (``modules/gnuradio/_fpga.py``, the
``fpga`` module of ``cmod/esdr_fpga.c``): it computes the spectrum of every monitored sample and
draws a line per redraw; the sinks keep only their label rows. Other sinks, and a firmware
without a bitstream, draw in software.

.. code-block:: console

   west build -b mch2022/esp32/procpu esp-sdr-zephyr/apps/micropython -- \
     -DCONFIG_ESDR_FPGA_IMAGE=\"<gateware build>/top/top.bin\"

``tools/bench/egg_fpga.py`` runs an app with the gateware's counters.

The VIEWE ESP32-S31 board
*************************

On the VIEWE PLCM-UES31S043H800V480C-U (``ues31s043h800v480c_u/esp32s31/hpcore``) the app has the
800x480 panel and its touch screen, a 8 MiB Python heap in PSRAM, ``/FLASH:`` in the upper 8 MB of the
flash, and the REPL on the console UART, the board's USB-C socket (a CH340, which also flashes it). The
launcher's list and the GUI's rows are tall enough for a finger, a slider follows it, a frequency's slider
covers what the radio tunes at every rate (2205 to 2900 MHz) with buttons beside it for one step, and holding the screen's top left corner or the BOOT button for 1.5 s goes home.

The radio is the chip's own, as on the MCH2022 badge, and transmits too (Tone and Data link are in its list):
the native rates 80, 40 and 20 MS/s come as bursts, and a display's stream (``esdr.RATES``: every rate 10 MHz / m
reaches exactly from 5 MS/s to 125 kS/s, ``cmod/esdr_rates.h``) is 20 MS/s bursts, 0.8 ms each, taken back to back
with gaps between them, each finished while the next is captured (at 2 MS/s 42 % of the stream, from 1.25 MS/s
down some 49 %). A stream into a buffer at 312.5 kS/s and below has no
gaps: the dump ring (``esp_sdr_dumpring.h``) runs on CPU 1, the image being built for both cores with only the radio
worker there, and its decimator in ``cmod/esdr_local.c`` carries its state from one call to the next. It captures
at 20 MS/s as the bursts do, with PIE copies in the ring's turns: the decimator takes 12 of the 14.6 CPU cycles a
word it has at 250 kS/s. 250 kS/s and below ran 30 to 60 s without a gap; 312.5 kS/s restarts some 1.3 times a
second, a gap each. At 2 MS/s the ring captures at 10 MS/s and the decimator takes every word into a third
order CIC of ratio 5 straight to the rate, no half-band (2.6 dB down at 500 kHz, 30 dB for what lies 1.5 MHz
off; the analog filter alone keeps out what lies 10 MHz away, some 12 dB on the LO's side, 28 on the other, so
the LO goes to the side of the 2.4 GHz band's nearer edge unless a crystal harmonic would fold in there):
10 s without a gap into a buffer while CPU 0
idles, but CPU 0's PSRAM traffic stalls its stores into a PSRAM buffer (65 restarts in 5 s under the Pcap
detector, the run collapsing under copies), so the Pcap app's engine has it write the ring stage in the
capture bank instead. Meanwhile CPU 0 waits about a tenth of the time (the ring's turns),
the panel keeps 59 frames a second without an underrun, and a request that needs the radio (a gain, a tuning)
pauses the run for its few milliseconds, a gap in ``rx_stats()``. With a XIAO ESP32S3 running Data link too,
both in Auto for a minute: 28 and 27 messages, each acknowledged, none lost, none bad. For the bursts the LO sits 5 MHz below the centre, out of the band with
the receiver's DC and its noise, the analog filter stands at 13 MHz (a burst's as wide as its rate) unless the
app sets a bandwidth, and a decimator of the app's own (``stream_decimate()`` in ``cmod/esdr_local.c``) leaves
the noise floor flat within 2 dB over the band's middle 3/4 and 3 to 5 dB down at its edges. A signal exactly
at the centre goes with the burst's mean. The filter is what keeps out the band's image, 20 MHz off: a
bandwidth set wider lets it in. The running panel is in the picture: its pixel clock comes from the APLL at
25.1 MHz, whose fraction leaves no line at its multiples, and the floor sits some 2 to 4 dB up under the AGC.
Lines at the multiples of the PSRAM's clock (2250, 2500 and 2750 MHz at the board's 250 MHz) grow with the
panel's reads from it: a burst near one waits for the panel's vertical blanking, one a frame, unless
``esdr.panel_quiet(False)`` (``doc/hardware-quirks.md``). Two framebuffers keep LVGL's frames whole, so a
waterfall scrolls without tearing. The diagnostic bus's gapless ring would need the pads and the LCD signals the
panel uses; ``apps/osmosdr_hs`` has it, without the display. ``esdr.tx_start()`` transmits a carrier from the PHY's tone
generator, at the frequency or a tone's offset from it (``gen``), in 16 power steps of about 1.5 dB, or plays a
buffer (``buf``, int16 I, Q, full scale 32767) once through the CPU-paced TX DC path (``esp_sdr_paced.h``) as
the C5 does: inside the call, ``tx_busy()`` False on return, at most 26628 pairs (106 ms at 250 kS/s). With
``repeat=True`` the DAC engine loops the buffer instead (``esp_sdr_replay.h``), out of the TCM group below the
capture bank while the CPU runs on, until ``tx_stop()`` or ``count`` samples: interpolated around the loop by a
windowed sinc to the highest of 80, 40 and 20 MS/s that is a whole multiple of the rate and holds it in 32768
words, so at most 409 pairs at 250 kS/s; ``tx_scale()`` refills it, a pause of some milliseconds. Its one LVGL
draw buffer keeps the image below that group. The rate times the interpolation factor (8, 4, 2 or 1, the largest that fits) stays within 1 MHz,
and must divide 320 MHz; the power steps scale the samples, 1.4 dB a step. A burst locks interrupts for its
length: the panel keeps its picture (no underrun; its frame counter misses the frames meanwhile), LVGL, touch
and the console wait. ``tx_stats()`` adds the last burst's ``late``, ``late_max`` (CPU cycles) and
``missing``. A call costs some 36 ms beyond its samples, three retunes and the chain's start and stop
(``doc/hardware-quirks.md``).

.. code-block:: console

   west build -b ues31s043h800v480c_u/esp32s31/hpcore esp-sdr-zephyr/apps/micropython
   west flash --esp-device /dev/serial/by-id/usb-1a86_USB_Serial-if00-port0

``CONFIG_APP_CONSOLE_REMOTE`` (on here) lets a script see the screen and touch it over that UART while
the REPL or an app runs, from ``tools/screen.py``: ``shot out.png`` saves the screen in a second or
several (a waterfall has little to compress), ``tap``, ``hold`` and ``drag`` play a finger, ``key`` a key,
``count`` tells the panel's frames and underruns. The port must open with both reset lines asserted, as the
tool does, or the board restarts.

How it is built
***************

- MicroPython is a Zephyr module, as in `mp-lvgl-example
  <https://github.com/cyber-murmel/mp-lvgl-example>`__: ``CONFIG_MICROPYTHON`` builds its
  Zephyr port as the ``micropython`` library, which provides ``main()``, with this app's
  configuration header, manifest and C modules (``cmod/``). ``modules/micropython`` is the
  ``zsdr`` branch of cyber-murmel's fork: the ``mp-lvgl`` branch the example pins, its port
  change split into single commits, and this app's own. ``mpy_module.cmake``
  changes the module's build where this app needs it: the bundled apps' index for the
  manifest, the USB device left to the app, a copy of the port's ``zephyr_filesystem.c`` (a
  volume stays mounted across soft resets instead of being powered off and on) and LVGL's
  allocator replaced by ``src/lv_mem_mp.c``; on the S3s the port's ``main()`` is compiled as
  ``real_main()`` for ``src/boot.c``.
- MicroPython runs in its own thread with its stack in the SRAM above the receive ring's banks;
  the ring needs the DRAM image to end below them. Most of the Python heap (5 MiB) is PSRAM.
- ``src/setjmp.S`` replaces the ROM's ``setjmp``, whose window spill traps into Zephyr on every
  ``try`` and garbage collection.
- USB is the shared device of ``apps/common``: CDC-ACM console, mass storage, DFU.
- ``prj.conf`` has what all boards share, ``socs/esp32s3_procpu.conf`` what both S3 boards
  share, ``boards/<board>.conf`` the rest; the C5 builds
  ``cmod/esdr_c5.c`` instead of the S3's engines and helpers, freezes ``manifest_c5.py`` and
  runs MicroPython from the port's own ``main()``, as the P4 and the classic ESP32 do.
