.. _esp_sdr_micropython:

micropython
###########

Overview
********

MicroPython (upstream v1.29.0, its Zephyr port) as the main program of the XIAO ESP32S3, with a
``gnuradio`` stand-in that runs flowgraphs exported from GNU Radio Companion unmodified: copy the
``.py`` file GRC writes to the board and run it.

What runs:

.. list-table::
   :header-rows: 1

   * - In the export
     - On the board
   * - ``soapy.source`` (any ``driver=``)
     - the board's receiver: 250, 125, 62.5, 31.25 or 15.625 kS/s (other rates are rounded to
       the nearest), one RF gain index (any gain name sets it, as the host plugin
       ``driver=esdr`` does), offset tuning
   * - ``soapy.sink`` (any ``driver=``)
     - the board's transmitter (the DAC, interpolated to 40 MS/s): input rates that divide
       40 MS/s (others are rounded to the nearest such rate), gapless up to 500 kS/s; one
       transmit gain step from 0 (weakest, the default) to 17, which any gain name sets
       (clamped, as with ``driver=esdr``); a centre within 500 Hz of the frequency asked for.
       Transmitting holds the radio: no receive flowgraph meanwhile
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
       presses the selected one, and the export's setters run (a frequency variable retunes)
   * - ``QVBoxLayout``, ``QHBoxLayout``, ``QGridLayout`` (GUI hints ``row, col, rowspan,
       colspan``), ``QTabWidget`` (GUI hint ``tab@n``)
     - placed on the 320x240 screen; a tab widget shows one page at a time; a scroll area
       does not scroll
   * - The rest of a Qt GUI export (``PyQt5``, ``sip``, settings, styling)
     - accepted, does nothing; ``QApplication.exec_()`` runs the display until Ctrl-C, which
       calls the export's own SIGINT handler; when the loop ends, the window closes as on a
       desktop (the export's ``closeEvent()`` stops the flowgraph)
   * - ``signal``, ``threading.Event``, ``argparse``, ``eng_notation``, ``firdes``,
       ``fft.window``, ``pmt``
     - stand-ins for what every export imports

A flowgraph either receives (one ``soapy.source``, an optional ``head``, sinks) or transmits
(one chain from a signal source through ``throttle``, ``multiply_const_cc`` or ``head`` to
``soapy.sink``).

Not yet: signal processing blocks (filters, demodulators, ...), the other qtgui sinks
(accepted, not drawn).

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
  holds the scripts and is the working directory (``boot.py``, ``main.py`` run from it).
- ``/SD:``: the SD card, if one mounts (FAT or exFAT; it is never formatted).
- Both are USB drives (mass storage, LUN 0 FLASH, LUN 1 SD card). While the board writes a
  volume (a ``file_sink`` at the end of a capture) the host sees the medium removed, for at
  least 3 s, and reads it afresh when it comes back. Copy scripts while no flowgraph writes.
- The REPL is on the CDC-ACM port,
  ``/dev/serial/by-id/usb-Zephyr_Project_ESP-SDR_MicroPython_<serial>-if00``.

Running an export::

   >>> exec(open("rx_gui.py").read())

or name it ``main.py`` to run it at boot. ``mpremote`` and ``pyboard.py`` work too, but they
soft-reset the board when they connect.

The ``esdr`` module
*******************

The C side, used by the stand-ins and useful on its own: ``init()``, ``tune()``, ``rx_gain()``,
``rx_bandwidth()``, ``rx_start()`` / ``rx_wait()`` / ``rx_stop()`` / ``rx_stats()`` (capture
into a buffer, int16 I/Q), ``rx_latest()`` (the newest samples, for displays),
``sc16_to_fc32()``, ``psd()``, the drawing helpers, ``tx_start()`` / ``tx_gen()`` /
``tx_scale()`` / ``tx_wait()`` / ``tx_stop()`` / ``tx_stats()`` (transmit a generated signal
or int16 I/Q from a buffer), ``tx_gain_max()``, ``fc32_to_sc16()``,
``claim()`` / ``release()`` (a volume away from USB while
writing), ``shell()`` (Zephyr shell commands, such as ``usj`` and ``mcuboot confirm``),
``crash()`` and ``reset_reason()`` (why the board last restarted), ``sd_info()``.

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
