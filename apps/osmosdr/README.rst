.. SPDX-License-Identifier: GPL-3.0-or-later

.. _esp_sdr_osmosdr:

#######
osmosdr
#######

.. nav-start

Previous: `stream <../stream/README.rst>`__ · Up: `Applications <../../doc/apps/index.md>`__ · Next: `osmosdr_serial <../osmosdr_serial/README.rst>`__

.. nav-end

A USB SDR after HackRF's model, for osmosdr and GNU Radio hosts, on the XIAO ESP32S3.

Overview
********

Vendor control requests set the radio, bulk endpoints carry the samples, one direction at a
time. The protocol is in ``src/esdr_proto.h``, shared with the host tool ``tools/esdr_usb.c``;
`osmosdr_serial <../osmosdr_serial/README.rst>`__, `osmosdr_hs <../osmosdr_hs/README.rst>`__
and `osmosdr_bridge <../osmosdr_bridge/README.rst>`__ speak it too.

Receive
=======

- Gapless I/Q at 500, 250, 125, 62.5, 31.25 or 15.625 kS/s.
- 4, 8 or 16 bit items; 500 kS/s as 4 or 8-bit items only (USB full speed carries some
  1.16 MB/s).
- The ring owns CPU 1 with that CPU's interrupts masked; USB, Wi-Fi and the shell run on CPU 0.
- `Gapless receive <../../doc/samples/rings.md>`__ describes the ring and its decimating filter.

Offset tuning
=============

On by default (``SET_OPTIONS`` bit 0). The centre is exact to the Hz, after the crystal
correction. `Tuning <../../doc/radio/tuning.md>`__ explains where the LO sits and how the ring
removes the offset.

Transmit
========

The host's samples go to the DAC stream. USB flow control paces the host about 50 ms ahead of
the air. `Transmit <../../doc/samples/transmit.md>`__ describes the DAC stream.

Hardware
********

.. list-table::
   :header-rows: 1

   * - Board target
     - Chip
     - Console
     - USB functions
   * - ``xiao_esp32s3/esp32s3/procpu``
     - ESP32-S3
     - CDC-ACM shell
     - the SDR's vendor interface, the console, DFU (MCUboot)

Building and flashing
*********************

.. code-block:: console

   west build --sysbuild -b xiao_esp32s3/esp32s3/procpu esp-sdr-zephyr/apps/osmosdr
   west flash

- The app uses the same USB IDs as the other apps (2fe3:0005); hosts find it by its vendor
  interface and the ``GET_INFO`` magic. `Applications <../../doc/apps/index.md>`__ has the
  host's udev rules.
- The three ring banks leave little internal RAM: sample blocks, filter buffers, the DAC
  stream's buffers and the thread stacks live in ``ESP_SDR_HIGH_RAM``
  (`Code placement <../../doc/dev/code-placement.md>`__, `Chip notes <../../doc/dev/chips.md>`__).
- The first flash goes over USB-Serial-JTAG. Later updates: ``west dfu -s <usb serial>`` (see
  `Applications <../../doc/apps/index.md>`__).

Usage
*****

Host tool
=========

.. code-block:: console

   cc -O2 -Iesp-sdr-zephyr/include -Iesp-sdr-zephyr/apps/osmosdr/src \
      esp-sdr-zephyr/apps/osmosdr/tools/esdr_usb.c \
      -lusb-1.0 -lm -o esdr_usb
   ./esdr_usb -s <serial> info
   ./esdr_usb -s <serial> test-in        # USB throughput in; test-out for out
   ./esdr_usb -s <serial> rx -f 2450e6 -r 250e3 -g 32 -b 16 -t 10 -o rx.cs16
   ./esdr_usb -s <serial> tx -f 2450e6 -r 250e3 -G 4 -T 50e3 -a 0.3 -t 10
   ./esdr_usb -s <serial> trig -f 2440e6 -g 30 -R 8 -t 10 -o snaps.cs8   # a radio with
                                       # ESDR_MODE_TRIGGER: 80 MS/s around each burst

SoapySDR, GNU Radio, gqrx
=========================

``soapy/`` holds a SoapySDR module (``driver=esdr``) over the same protocol:

- receive and transmit, half duplex: activating one direction stops the other;
- CS8, CS16 and CF32;
- gains ``RF`` (receive index) and ``TX`` (power step);
- frequency correction in ppm.

gr-osmosdr reaches it through its ``soapy`` backend, which is how GNU Radio and gqrx use it.

.. code-block:: console

   cmake -S esp-sdr-zephyr/apps/osmosdr/soapy -B build-soapy && cmake --build build-soapy
   export SOAPY_SDR_PLUGIN_PATH=$PWD/build-soapy
   SoapySDRUtil --find="driver=esdr"
   gqrx        # device string: soapy=0,driver=esdr  (add ,serial=<serial> for one board)

``port=/dev/...`` in the device string selects the serial link instead, for the ESP32-C6's
`osmosdr_serial <../osmosdr_serial/README.rst>`__ app.

In GNU Radio: ``osmosdr.source(args="numchan=1 soapy=0,driver=esdr")`` and
``osmosdr.sink(...)`` with the same string, at one of the receive rates (500 kS/s down to
15.625 kS/s; other requests get the nearest one, which ``getSampleRate()`` reports). Changing a
setting while streaming restarts the device's stream: expect a few ms of zeros.

Stream arguments
================

``getStreamArgsInfo()`` lists the receive stream arguments, which gr-soapy checks:

.. list-table::
   :header-rows: 1

   * - Argument
     - Effect
   * - ``bits=4``
     - 4-bit items on the wire
   * - ``trigger=<ratio>`` (``post=<samples>``)
     - for a radio with the trigger mode, the WHY2025 badge's C6 directly or through its
       bridge: the stream carries 80 MS/s snapshots of 16320 samples around each burst, one
       after another, and the sample rate does not apply

With GNU Radio's ``soapy.source(..., "trigger=4", ...)`` and the Communicator sending bursts, 58
snapshots arrived in 8 s, none lost. `Burst capture <../../doc/samples/bursts.md>`__ explains
the triggered snapshots.

Spectrum viewer
===============

``tools/esdr_scope.py`` is a small standalone GNU Radio + PyQt5 app: one window, a frequency
spectrum above a waterfall, with on-screen frequency, sample rate, gain and ppm correction
controls. It talks to the board through the SoapySDR module above (build that first).

.. code-block:: console

   esp-sdr-zephyr/apps/osmosdr/tools/esdr_scope.sh --serial <serial> \
       --freq 2450e6 --rate 250e3 --gain 32 --corr -7.5

``esdr_scope.sh`` runs the script with GNU Radio's own Python (PyQt5 and ``gnuradio.qtgui``
live there) and the Qt platform plugin path, both found from the ``gnuradio-companion`` wrapper
nixpkgs builds, so it needs no changes to the project's ``shell.nix``.

Protocol
********

Vendor requests
===============

Vendor requests to the device (``bmRequestType`` 0x40 / 0xc0), all values little endian. This
table is the reference for every osmosdr app.

.. list-table::
   :header-rows: 1

   * - Request
     - Value / data
   * - ``GET_INFO`` (0x01, in)
     - ``struct esdr_info``: firmware, tuning range, rates, gain ranges
   * - ``SET_MODE`` (0x02)
     - off 0, receive 1, transmit 2; throughput tests 0x10 (in), 0x11 (out)
   * - ``SET_FREQ`` (0x03)
     - centre in Hz, 8 bytes
   * - ``SET_SAMPLE_RATE`` (0x04)
     - Hz, 4 bytes; the nearest supported rate is taken (on a log scale, of ``GET_RATES``'s
       where there is the list)
   * - ``SET_RX_GAIN`` (0x05)
     - PHY gain index 0 to 82 (not dB), or 0xffff for the hardware AGC
   * - ``SET_TX_GAIN`` (0x06)
     - power step 0 (weakest) to 17
   * - ``SET_BANDWIDTH`` (0x07)
     - analog low-pass in Hz (13 to 69 MHz two-sided), 0 for the PHY's
   * - ``SET_FORMAT`` (0x08)
     - 4, 8 or 16 bits per I and Q item (4: one byte a sample, I in bits 3:0, Q in 7:4,
       signed, receive only; Soapy stream argument ``bits=4``)
   * - ``SET_FREQ_CORR`` (0x09)
     - crystal correction in ppb, 4 bytes signed
   * - ``GET_STATE`` (0x0a, in) / ``GET_STATS`` (0x0b, in)
     - settings as applied (``struct esdr_state``, see `The state's status`_), counters
   * - ``SET_OPTIONS`` (0x0c)
     - bit 0: offset tuning (default on)
   * - ``SET_DIGITAL_GAIN`` (0x0d)
     - dB in steps of 6 up to 42, or 0xffff: 24 dB at 8 bit, 0 at 16
   * - ``GET_TEMP`` (0x18, in)
     - die temperature in 0.1 degC, 2 bytes signed (-ENOTSUP without the sensor)
   * - ``GET_CAL`` (0x19, in)
     - the device's own crystal correction (see `Calibration requests`_)
   * - ``SET_CAL`` (0x1a)
     - a crystal calibration for the device to keep (see `Calibration requests`_)
   * - ``GET_RATES`` (0x1c, in)
     - the receive rates in Hz, 4 bytes each, fastest first, the reply's length giving their
       number; only where ``esdr_info.caps`` has ``ESDR_CAP_RATE_LIST`` (osmosdr_hs). The
       ``rx_rate_max_hz >> k`` of ``GET_INFO`` are among them for hosts that know only those

The state's status
------------------

The state's last field, ``status``, came later: ``ESDR_STATE_MIN_SIZE`` is what every device
sends.

- So far ``apps/osmosdr_serial`` sets it: ``ESDR_STATUS_LO_CHECKED`` where its chip checks the
  PLL's VCO after a tune (not the C5 nor the classic ESP32).
- ``ESDR_STATUS_LO_UNLOCKED`` where the last tune left it outside its window or the radio
  refused the frequency.
- The request that tuned does not fail for it: a tune is applied as asked, after the reply.
- This app and ``apps/osmosdr_hs`` send 0.

Calibration requests
--------------------

``GET_CAL`` reads the device's own crystal correction, ``SET_CAL`` gives it a calibration to
keep. Their payloads (``struct esdr_cal`` and the ``SET_CAL`` kinds by ``wValue``) are in
`Crystal calibration <../../doc/radio/crystal-calibration.md>`__, which also measures the crystal
error.

This app answers ``GET_CAL`` only; the serial transport and the MicroPython radios take both.

Output format
*************

Sample blocks
=============

Receive data comes in 4096-byte blocks on bulk IN: a 16-byte header, then interleaved I/Q. The
header holds:

- the magic ``EBLK``;
- flags;
- the sample count;
- the index of the first sample on the stream's time axis.

A jump in the index is a gap: the host did not read in time, or a retune restarted the ring.
The host fills it with zeros to keep the time axis.

Transmit data is plain interleaved I/Q on bulk OUT.

Changing a setting while streaming restarts the ring (a gap of about 4 ms for a retune) or the
transmit session.

Measurements
************

Two XIAO ESP32S3 boards cabled through a 30 dB attenuator.
`Host transports <../../doc/samples/transports.md>`__ has the vendor bulk capacity of USB full
speed.

.. list-table::
   :header-rows: 1

   * - Test
     - Result
   * - Receive 250 kS/s, 16 bit (1.0 MB/s) and 8 bit, with fine tuning
     - 60 s, 15.0 M samples, no gaps. The filter takes 12.7 to 13.3 of the 15 cycles per input
       pair CPU 1 has at 16 MS/s (``sdr bench`` breaks it down)
   * - Transmit 250 kS/s from the host
     - 66 s, nothing dropped, no underruns
   * - A tone from one board, received on the other
     - lands where expected, after the 7.5 ppm crystal correction between the two boards, to
       within their drift of about 100 Hz. The other board's carrier leak is about 40 dB down,
       the image about 43 dB
   * - Receive to transmit and back on one board
     - no errors
   * - GNU Radio 3.10 through gr-osmosdr's soapy backend
     - 20 s at 250 and 125 kS/s with no gaps (the module logs its counters when a stream
       stops); transmit a ``sig_source`` tone for 7 s, nothing dropped
   * - ``SoapySDRUtil --rate``
     - 0.24 MS/s (start included)

Limitations
***********

- USB full speed caps receive at 500 kS/s, 8-bit items (1.0 MB/s, gapless in 5 s runs) or
  4-bit ones (0.5 MB/s, a tone 1 dB lower over the floor than at 8 bit); 32 times decimation
  leaves the filter 14.4 of its 15 cycles a pair.
- Transmit interpolates linearly: images at multiples of the sample rate are only attenuated by
  the interpolator's sinc squared response.
- Gains are uncalibrated PHY indices; the crystal error (several ppm) needs ``SET_FREQ_CORR``
  (`Crystal calibration <../../doc/radio/crystal-calibration.md>`__ measures it against an LTE
  cell).
- No flash writes while streaming: the ring CPU runs with interrupts masked. DFU stops the
  stream first.

See also
********

- `osmosdr_serial <../osmosdr_serial/README.rst>`__, `osmosdr_hs <../osmosdr_hs/README.rst>`__
  and `osmosdr_bridge <../osmosdr_bridge/README.rst>`__: the same protocol on other chips and
  links.
- `Gapless receive <../../doc/samples/rings.md>`__: the receive ring.
- `Transmit <../../doc/samples/transmit.md>`__: the DAC stream.
- `Host transports <../../doc/samples/transports.md>`__: the vendor bulk link among the others.
- `Applications <../../doc/apps/index.md>`__: USB IDs, udev rules and DFU updates.

.. nav-start

Previous: `stream <../stream/README.rst>`__ · Up: `Applications <../../doc/apps/index.md>`__ · Next: `osmosdr_serial <../osmosdr_serial/README.rst>`__

.. nav-end
