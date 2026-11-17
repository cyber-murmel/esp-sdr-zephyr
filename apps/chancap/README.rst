.. SPDX-License-Identifier: GPL-3.0-or-later

.. _esp_sdr_chancap:

#######
chancap
#######

.. nav-start

Previous: `osmosdr_hs <../osmosdr_hs/README.rst>`__ · Up: `Applications <../../doc/apps/index.md>`__ · Next: `link <../link/README.rst>`__

.. nav-end

Channelized burst capture on the XIAO ESP32S3: one I/Q capture per packet per channel of a
uniform grid, sent to the host as VITA 49.2 over a vendor bulk endpoint.

Overview
********

The receiver watches a grid of equally spaced channels: Bluetooth LE at 2 MHz, IEEE 802.15.4 at
5 MHz, or any uniform grid inside 16 MHz. The design is in
`Channelizer <../../doc/modem/channelizer.md>`__; the library module is ``esp_sdr_chancap.h``.

- **Receive**: the 16 MS/s ring runs on CPU 1. On CPU 0 a wideband gate, a filter bank or
  decimators and a detector per channel cut the packets out; captures live in a PSRAM slab.
- **Output**: one VITA 49.2 stream per channel, in 4 KiB blocks on bulk IN (`Output format`_).
- **USB**: the capture function, the CDC-ACM shell and DFU (MCUboot).

Hardware
********

A XIAO ESP32S3.

Building and flashing
*********************

.. code-block:: console

   west build --sysbuild -b xiao_esp32s3/esp32s3/procpu esp-sdr-zephyr/apps/chancap
   west flash

- Later updates: ``west dfu -s <usb serial>`` (`Applications <../../doc/apps/index.md>`__,
  "DFU updates").
- The app uses the USB IDs all apps share (Applications, "USB IDs and host access"); the host
  tool finds it by its product string.

Usage
*****

Configuration
=============

The grid and the detector come from Kconfig and start at boot:

.. list-table::
   :header-rows: 1

   * - Option
     - Sets
     - Default (Bluetooth LE)
   * - ``CONFIG_APP_FIRST_FREQ_HZ``
     - the first channel
     - 2402 MHz
   * - ``CONFIG_APP_SPACING_HZ``
     - the channel spacing
     - 2 MHz
   * - ``CONFIG_APP_CHANNELS``
     - the number of channels
     - 8
   * - ``CONFIG_APP_CHANNEL_RATE_HZ``
     - the rate per channel
     - 2 MS/s

The detector's thresholds are Kconfig options too. The shell (`Shell`_) changes them.

Host tools
==========

chancap_bridge.py
-----------------

``tools/chancap_bridge.py`` reads the blocks with pyusb, reassembles the captures and turns every
capture into a GNU Radio PDU (`GNU Radio PDUs`_):

.. list-table::
   :header-rows: 1

   * - Option
     - Output
   * - ``--print``
     - a line per capture
   * - ``--save DIR``
     - ``.npz`` files: ``iq`` as complex float, full scale 1.0, and the metadata as JSON
   * - ``--zmq tcp://*:5555``
     - the serialized PDUs, for a gr-zeromq "ZMQ SUB Message Source"

It reads the bulk data as one stream: a read need not end where the board's 4 KiB block did, and
a word that cannot start a packet is skipped and counted.

ble_pcap.py
-----------

``tools/ble_pcap.py`` turns the captures of the Bluetooth LE grid into pcap for Wireshark. How it
demodulates and checks the packets is in
`GFSK and Bluetooth LE <../../doc/modem/gfsk.md>`__.

.. code-block:: console

   ble_pcap.py -w ble.pcap --seconds 60 --print   # a file, a line per packet
   ble_pcap.py -w - --seconds 0 | wireshark -k -i -   # live
   ble_pcap.py --npz DIR -w ble.pcap              # chancap_bridge.py --save's files

- The link type is ``LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR``.
- The radio header carries the RF channel, the access address's bit errors and the CRC result.
- The power fields stay unset: the board knows dBFS, not dBm.

Shell
=====

On the CDC-ACM port:

.. code-block:: console

   chancap set first 2405000000 spacing 5000000 channels 3 rate 4000000
   chancap stop
   chancap start
   chancap status

- ``chancap set`` without arguments lists the keys; ``wb``, ``ch``, ``wbgate`` and ``chgate``
  also apply to the running capture.
- ``chancap status`` prints the plan (engine, LO, taps), the pairs handed over and channelized,
  captures, drops, hand-over overruns and the channelizer's cycles per input pair.
- ``chancap bench`` times the channelizer on synthetic data and checks the vector kernels against
  the C reference.

Gate modes
----------

``chancap set wbgate`` and ``chgate`` switch the two gates at runtime, for three ways of capturing:

- per-packet captures;
- every channel in full while the band is busy;
- every channel as a continuous stream in chunks.

The modes and the rules for switching them are in `Channelizer <../../doc/modem/channelizer.md>`__.

Output format
*************

Streams and packets
===================

- One VITA 49.2 stream per channel: stream ID = a base ID + the channel index.
- A context packet before every capture: centre frequency, rate, the channel's noise floor as the
  reference level in dBFS, and the burst's peak above it as gain stage 1.
- Then 16-bit signal data packets, whose trailer marks the capture's parts:

  .. list-table::
     :header-rows: 1

     * - Trailer bit
       - Marks
     * - sample loss
       - the capture's first packet
     * - user bit 8
       - its last packet
     * - user bit 9
       - a cut at the length limit
     * - user bit 10
       - a chunk of a continuous stream

- Timestamps are the ring's pair index as seconds and picoseconds.

Transport
=========

- The packets go in 4 KiB blocks on a vendor bulk IN endpoint.
- Vendor bulk, because no app with the CDC-NCM stack has room for the ring: ``stream``'s image
  ends 1.5 KiB below bank 1, and the ring needs it below bank 0.

GNU Radio PDUs
==============

VRT is a wire format; a PDU is GNU Radio's in-memory message (a metadata dictionary paired with a
sample vector). The two map onto each other one to one: each capture becomes one PDU, sent through
GNU Radio's ZMQ message blocks or a source block.

.. list-table::
   :header-rows: 1

   * - PDU metadata key
     - Type
     - From
   * - ``rx_time``
     - tuple (uint64 s, double frac s)
     - VRT TSI and TSF, the UHD tag convention
   * - ``rx_freq``
     - double, Hz
     - context packet RF reference frequency
   * - ``rx_rate``
     - double, Hz
     - context packet sample rate
   * - ``channel``
     - uint64
     - stream ID minus the base ID
   * - ``start_index``
     - uint64
     - raw input sample index, for exact pairing across channels
   * - ``power_db``, ``noise_db``
     - double
     - capture metadata
   * - ``truncated``
     - bool
     - capture hit ``max_capture_us``
   * - ``chunk``
     - bool
     - fixed-length piece of a continuous stream, per-channel gate off

- The sample vector is a ``c32vector`` (complex float), scaled from the int16 items, so stock GNU
  Radio PDU blocks work unchanged.
- Reusing the ``rx_`` keys from the UHD convention lets existing burst flowgraphs read the time
  and frequency without changes.

Design history
==============

The design note of 2026-10-15 chose the format; the app was built on 2026-10-16. The note's plan,
with its remarks on the code of that time:

Transport choice
----------------

- On the S3 the chip keeps sending VITA 49.2, the transport ``apps/stream`` already uses (UDP
  over IPv6 on a USB CDC-NCM link).
- The C5 and C6 stream the osmosdr protocol over USB-Serial-JTAG instead
  (``apps/osmosdr_serial``); VRT there would be new.

Streams
-------

- The chip side was to reuse ``vrt_rx.c``.
- ``apps/stream`` had one stream with a build-time ID (``CONFIG_APP_RX_STREAM_ID``).
- It routed commands for any other ID to the transmitter.
- It kept its packet counter and context state in single variables.
- So the per-channel streams needed a new ID range and per-stream state.

Context packets
---------------

The design put a context packet per stream on start and on change:

.. list-table::
   :header-rows: 1

   * - Field
     - Value
   * - RF reference frequency
     - that channel's centre
   * - sample rate and bandwidth
     - the per-channel rate
   * - payload format
     - 8, 12 or 16 bit

The existing code filled all of these, the RF reference at whole-MHz resolution (channel centres
need Hz).

Timestamps and items
--------------------

- TSI and TSF were to carry the time of the first sample, derived from the raw sample index.
- ``vrt_rx.c`` stamped the capture's arming time instead (TSI "other", seconds since boot; TSF real
  time in picoseconds).
- 16-bit items by default: after channelization the per-channel rate is low enough that the
  bandwidth saving of 8 bit is not needed.

Measurements
************

On the bench, 2026-10-18, with the ESP32-C5 advertising through the RF switch:

- Wireshark shows the C5's ``ADV_SCAN_IND`` with the CRC valid, along with the advertisers nearby.
- About a third of the C5's advertisements on channel 37 come through; the rest are lost to
  channelizer overruns (`Limitations`_).

Limitations
***********

- The grid must fit the ring's 16 MHz, with rules for the spacing, the offsets from the LO and
  where the LO goes so its leakage falls on a channel edge: see
  `Channelizer <../../doc/modem/channelizer.md>`__.
- The channelizer shares CPU 0 with USB and the shell. When the band is busy for longer than it
  keeps up with, the hand-over ring overruns, the capture restarts from the newest data and
  ``status`` counts it.
- USB full speed carries about 1 MB/s: about 250 kS/s of 16-bit captures in all, so the
  per-channel gate must stay on for a busy band.

See also
********

- `Channelizer <../../doc/modem/channelizer.md>`__: the pipeline, gating, sizing and the C API.
- `GFSK and Bluetooth LE <../../doc/modem/gfsk.md>`__: Bluetooth LE from the captures.
- `stream <../stream/README.rst>`__: VITA 49.2 over CDC-NCM.
- `Applications <../../doc/apps/index.md>`__: DFU updates and USB access.

.. nav-start

Previous: `osmosdr_hs <../osmosdr_hs/README.rst>`__ · Up: `Applications <../../doc/apps/index.md>`__ · Next: `link <../link/README.rst>`__

.. nav-end
