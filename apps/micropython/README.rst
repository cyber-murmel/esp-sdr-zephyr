.. _esp_sdr_micropython:

micropython
###########

Overview
********

MicroPython (upstream v1.29.0, its Zephyr port) as the main program of the XIAO ESP32S3, with a
``gnuradio`` stand-in that runs flowgraphs exported from GNU Radio Companion unmodified: copy the
``.py`` file GRC writes to the board and run it, or start it from the launcher's menu as an app
(`Launcher and apps`_). The same app builds headless for the ESP32-C5-Zero, the bench's
coordinator, which also drives the bench's RF switch (`The ESP32-C5 image`_).

What runs:

.. list-table::
   :header-rows: 1

   * - In the export
     - On the board
   * - ``soapy.source`` (any ``driver=``)
     - the board's receiver: continuously at 250, 125, 62.5, 31.25 or 15.625 kS/s (offset
       tuning), or at the native 80, 40 or 16 MS/s in bursts of 16380 samples, contiguous within
       a burst (wideband displays, short captures; 40 and 80 MS/s open the analog filters to
       +-18 MHz); other rates are rounded to the nearest; one RF gain index (any gain name sets
       it, as the host plugin ``driver=esdr`` does)
   * - ``soapy.sink`` (any ``driver=``)
     - the board's transmitter. A periodic signal (a ``sig_source_c`` tone that fits a loop
       within 1 Hz, a repeating vector or file of up to 16380 samples at the DAC rate) loops
       at the DAC's own 40 MS/s (80 MS/s for a rate of 80 MS/s) with the analog filters at
       +-18 MHz: any tone in the band, no CPU, changes applied at once. Anything else streams
       through the interpolating DAC backend, gapless up to 500 kS/s (a generated signal
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
     - drawn on the ST7789V display (LVGL canvases filled by C helpers), placed as the
       export's layouts place them
   * - QT GUI Range, Chooser (combo box and radio buttons), Check Box, Push Button, Label,
       Entry (shown, not editable)
     - LVGL widgets; the rotary encoder moves between them (turn), its switch edits or
       presses the selected one, and the export's setters run (a frequency variable retunes;
       a waterfall keeps its history). On a Range of more than 100 steps a fast turn takes
       bigger steps (n detents between two display updates move n * n steps). A value the
       flowgraph refuses, such as a frequency outside the range the PLL locks over (about 1.8
       to 2.8 GHz, it varies from chip to chip), stops the slider at the last value that works
   * - ``QVBoxLayout``, ``QHBoxLayout``, ``QGridLayout`` (GUI hints ``row, col, rowspan,
       colspan``), ``QTabWidget`` (GUI hint ``tab@n``)
     - placed on the 320x240 screen; a tab widget shows one page at a time; a scroll area
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

As in `mp-lvgl-example <https://github.com/cyber-murmel/mp-lvgl-example>`__:

- ST7789V 320x240 display and SD card on SPI2 (SCK D8, MISO D9, MOSI D10); display CS D2,
  D/C D1, reset D0; SD card CS D7. The SD card runs at 12 MHz: the pins reach SPI2 through
  the GPIO matrix.
- Rotary encoder on D3/D4, its switch on D5.

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
keeps an app out of the menu. ``tools/mkegg.py flowgraph.grc --category receive`` makes the
folder of a GRC flowgraph: the flowgraph, its export (``grcc``, unmodified), an ``__init__.py``
that runs the export's ``main()``, and the metadata from the flowgraph's options (title,
description, author). Run it again after changing the flowgraph; the revision goes up when the
export changes.

The menu lists:

- the apps bundled with the firmware, ``eggs/``: Spectrum, Waterfall (250 kS/s, gapless),
  Wideband (40 MS/s bursts), Constellation, Receiver (tabs, radio buttons, a check box), Tone
  (transmits) and Data link (two-way messages, `Data link`_). They are frozen;
  ``tools/eggs_index.py`` turns their ``metadata.json`` files into the index the menu reads;
- the folders in ``apps/`` on the FLASH and SD drives: copy an app's folder there over USB.

Each app runs in a fresh interpreter: the launcher keeps the choice across a soft reset
(``esdr.next_app()``), ``main.py`` runs the app, and when the app ends another soft reset
brings the menu back with the app selected. An app that fails shows its error until the knob
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

The ``esdr`` module
*******************

The C side, used by the stand-ins and useful on its own: ``init()``, ``tune()``, ``rx_gain()``,
``rx_bandwidth()``, ``rx_start()`` / ``rx_wait()`` / ``rx_stop()`` / ``rx_stats()`` (capture
into a buffer, int16 I/Q), ``rx_latest()`` (the newest samples, for displays),
``sc16_to_fc32()``, ``psd()``, the drawing helpers, ``tx_start()`` / ``tx_gen()`` /
``tx_scale()`` / ``tx_wait()`` / ``tx_stop()`` / ``tx_stats()`` (transmit a generated signal
or int16 I/Q from a buffer), ``tx_loop()`` / ``tx_period()`` (loop one period at the DAC rate),
``tx_gain_max()``, ``fc32_to_sc16()``, ``channel_bw()`` and ``tx_lpf()`` (the analog filters),
``BURST_RATES``, ``rx_start(wrap=True)`` and ``rx_count()`` (capture into a ring),
``fsk_rx_init()`` / ``fsk_rx()`` / ``fsk_rx_reset()`` / ``fsk_rx_state()`` and ``fsk_mod()``
(the packet modem, ``cmod/esdr_link.c``),
``claim()`` / ``release()`` (a volume away from USB while
writing), ``shell()`` (Zephyr shell commands, such as ``usj`` and ``mcuboot confirm``),
``crash()`` and ``reset_reason()`` (why the board last restarted), ``sd_info()``,
``next_app()`` and ``home()`` (the launcher: the app the next soft reset starts, whether the
knob ended the app). The C5's ``esdr`` (``cmod/esdr_c5.c``) has the radio part for packet
flowgraphs, ``rx_lsb()``, ``fsk_frames()`` and ``first_start()``; its docstring lists it.

Building and flashing
*********************

.. code-block:: console

   west build --sysbuild -b xiao_esp32s3/esp32s3/procpu esp-sdr-zephyr/apps/micropython
   west dfu --no-confirm

``west dfu`` confirms a new image through the MCUboot shell on the CDC-ACM port, which the REPL
has taken, hence ``--no-confirm``; confirm from the REPL instead (an image written with esptool
is already confirmed)::

   >>> import esdr; esdr.shell("mcuboot confirm")

Without DFU access, hand the USB port to the USB-Serial-JTAG from the REPL
(``esdr.shell("usj")``) and write MCUboot at 0x0 and ``zephyr.signed.bin`` at 0x20000 with
``esptool``.

``tools/check_capture.py`` checks a recording on the host (length, level, DC, strongest line).

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
  diagnostic-bus ring (``esp_sdr_diag.h``, 40 MB/s, box16 and CIC to the continuous rates 250 to
  15.625 kS/s), transmit through the CPU-paced TX DC path (``esp_sdr_paced.h``, 4 times
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

How it is built
***************

- The port is built into this app: ``mpy_port.cmake`` is derived from the port's
  ``CMakeLists.txt``, which cannot be ``include()``\ d, and builds a corrected copy of the
  port's ``zephyr_filesystem.c`` (two fstab entries; a volume stays mounted across soft resets
  instead of being powered off and on). ``modules/micropython`` is unmodified upstream.
- MicroPython runs in its own thread with its stack in the SRAM above the receive ring's banks;
  the ring needs the DRAM image to end below them. Most of the Python heap (5 MiB) is PSRAM.
- ``src/setjmp.S`` replaces the ROM's ``setjmp``, whose window spill traps into Zephyr on every
  ``try`` and garbage collection.
- USB is the shared device of ``apps/common``: CDC-ACM console, mass storage, DFU.
- ``prj.conf`` has what both boards share, ``boards/<board>.conf`` the rest; the C5 builds
  ``cmod/esdr_c5.c`` instead of the S3's engines and helpers, freezes ``manifest_c5.py`` and
  runs MicroPython from the port's own ``main()``.
