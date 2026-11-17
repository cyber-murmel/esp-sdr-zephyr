.. SPDX-License-Identifier: GPL-3.0-or-later

.. _esp_sdr_ubertooth:

#########
ubertooth
#########

.. nav-start

Previous: `zstack <../zstack/README.rst>`__ · Up: `Applications <../../doc/apps/index.md>`__ · Next: `ubertooth internals <../../doc/apps/ubertooth-internals.md>`__

.. nav-end

The device firmware of an `Ubertooth One <https://github.com/greatscottgadgets/ubertooth>`__ on
the ESP32-S3 software radio, for the unmodified upstream host tools.

Overview
********

- The unmodified host tools (``ubertooth-util``, ``-specan``, ``-btle``, ``-rx``, ``-scan``, the
  Wireshark extcap) talk to it over the Ubertooth USB protocol.
- The CC2400 radio of the original is replaced by software demodulation of the ring receive and a
  GFSK modulator on the DAC (`GFSK and Bluetooth LE <../../doc/modem/gfsk.md>`__).
- The USB function is the protocol of ``libubertooth``: vendor interface 0 with bulk IN 0x82 (and
  OUT 0x05), the vendor requests 0 to 73 on the control endpoint, the API version (``0x0107``) as
  ``bcdDevice``. The CDC-ACM shell and DFU sit behind it.

The design and its measurements are in
`ubertooth internals <../../doc/apps/ubertooth-internals.md>`__.

Hardware
********

A XIAO ESP32-S3.

Building and flashing
*********************

.. code-block:: console

   west build -d build-clanker/xiao_esp32s3/ubertooth --sysbuild -b xiao_esp32s3/esp32s3/procpu esp-sdr-zephyr/apps/ubertooth
   west flash

Later updates go over DFU like the other apps: ``west dfu -s <usb serial>`` (``dfu.py`` knows both
IDs; `Applications <../../doc/apps/index.md>`__, "DFU updates").

USB identity
============

The tools open only the IDs of the Ubertooth (``1d50:6000``, ``1d50:6002``, ``ffff:0004``).

.. list-table::
   :header-rows: 1

   * - Option
     - Presents
     - Use
   * - ``CONFIG_APP_UBERTOOTH_USB_ID_UBERTOOTH`` (default)
     - ``1d50:6002``, the ID of Great Scott Gadgets' Ubertooth One
     - to run the unmodified tools on a bench, not in a product
   * - ``CONFIG_APP_UBERTOOTH_USB_ID_PROJECT``
     - ``2fe3:0005``
     - the tools then need a patch to open the device

- The product string says "ESP-SDR Ubertooth One" and the manufacturer stays "Zephyr Project".
- DFU mode keeps the project ID ``2fe3:ffff`` (``CONFIG_APP_USB_DFU_VID``).
- The vendor interface must be interface 0 and own the endpoints 0x82 and 0x05.
- The tree's Zephyr gives a class fixed endpoint addresses with ``USBD_EP_ADDR_FIXED`` (a local
  patch, ``usbd.h``). Without it the interface carries an unused interrupt endpoint, which gives
  IN 0x82 and OUT 0x01: enough for the C tools, not for ``host/python/ubtbr``.

Usage
*****

Host tools
==========

The tools are in nixpkgs:

.. code-block:: console

   nix-shell -p ubertooth
   ubertooth-util -v -s -p

On Linux the tools need access to the USB device: the udev rule of the ``ubertooth`` package
(``40-ubertooth.rules``) covers ``1d50:6002``.

Shell
=====

.. list-table::
   :header-rows: 1

   * - Command
     - Effect
   * - ``ubt status``
     - mode, channel, USB state, queue fill, clock
   * - ``ubt crash``
     - what ended the previous boot, if it ended badly
   * - ``ubt tune <a> <b> [n]``
     - times retunes
   * - ``ubt rx start <mhz> [gain]``, ``stop``, ``stats``
     - runs the 4 MS/s ring stream
   * - ``ubt rx spec``, ``ubt rx floor``
     - the stream's spectrum and noise floor
   * - ``ubt rx bench <mhz>``
     - times the demodulator on 4 ms of the stream, checks its vector unit planes and its assembly
       (slicer, group logic) against C and compares it with the frozen reference symbol for symbol
       (``CONFIG_APP_UBERTOOTH_GFSK_REF``, on by default; ``CONFIG_APP_UBERTOOTH_GFSK_PROF`` adds
       the cycles of its parts)
   * - ``ubt rx ring [unit pairs] [LO MHz]``
     - overrides the ring's unit length and the LO's offset from the channel from the next start
   * - ``ubt rx gain [guard|<index>|auto]``
     - the gain of the demodulating modes from their next start (default 60; `Gain for strong
       signals`_)
   * - ``ubt specanfs <dbm>``
     - the spectrum analyser's level calibration
   * - ``ubt pollhold [ms]``
     - how long an empty ``UBERTOOTH_POLL`` waits for a packet in the BLE modes (default 16; 0
       answers at once, as the original does)
   * - ``prof start [period_us]|stop|dump [min]``
     - with ``CONFIG_ESP_SDR_CPU_PROF``: a profile of CPU 0 taken by the ring on CPU 1 while a
       stream runs (``esp_sdr_prof.h``, the shell command from ``apps/common``)

- The commands from ``ubt tune`` on are for bench work
  (`ubertooth internals <../../doc/apps/ubertooth-internals.md>`__).
- The modes log what they did on the shell's port when they end.

Gain for strong signals
=======================

The demodulating modes run at a fixed gain (index 60), which reaches full scale for a transmitter
within a metre or so (the host's own adapter, a phone on the desk). Its packets then come out with
bit errors or not at all.

.. list-table::
   :header-rows: 1

   * - Setting
     - Behaviour
   * - index 60
     - the default
   * - ``ubt rx gain 40``
     - a connection between two such devices is followed better; 8 to 10 dB less sensitive for
       distant devices
   * - ``ubt rx gain auto``
     - the hardware AGC; idles above 60 and loses packets
   * - ``ubt rx gain guard``
     - experimental: steps the gain down on packets at full scale and up again after 10 s
       without, holding twice as long each time it has to step down again within a minute, up to
       160 s

Status
******

Written and tested on the host; the table says what has also run on a board (a XIAO ESP32-S3).

.. list-table::
   :header-rows: 1

   * - Part
     - Host tool
     - State
   * - USB function, vendor requests, clock, queue, register file
     - ``ubertooth-util``
     - unit tests on ``native_sim`` (``tests/unit/ubertooth_core``); ``ubertooth-util`` runs on a
       board
   * - Spectrum analyser, LED spectrum analyser
     - ``ubertooth-specan``, ``ubertooth-util -q``
     - runs on a board
   * - BLE sniffing
     - ``ubertooth-btle -n``
     - runs on a board
   * - BLE connection following
     - ``ubertooth-btle -f``
     - runs on a board
   * - BLE promiscuous mode
     - ``ubertooth-btle -p``
     - runs on a board
   * - BR symbol stream and AFH
     - ``ubertooth-rx``, ``-scan``, ``-dump -b``, ``ubertooth-afh``
     - runs on a board
   * - Transmit (BLE advertising, TX test, generic packet)
     - ``ubertooth-btle -s``
     - advertising runs on a board; TX test starts and stops; generic packet not tried

Spectrum analysers
==================

- 79 channels per sweep in ascending order, about 84 sweeps/s.
- The LED one measures Wi-Fi channels 1, 6 and 11 and lights the board's one LED when any is above
  the threshold.
- The levels are not calibrated (`Limitations`_).

Bluetooth LE
============

- Sniffing: 130 to 150 advertisements a second on a busy advertising channel.
- Following catches the CONNECT_IND and follows a 1M connection through its channel map and PHY
  updates to its end (25 s, 45 ms interval, CSA #2: a packet in 60 to 80 % of its events).
- It stops at a switch to the 2M PHY; on a busy channel only some of the CONNECT_INDs are caught
  (`Measurements`_).
- Promiscuous mode finds the access address and CRCInit of a connection on the air, then follows
  it like ``-f``.
- It finds the hop interval only of a connection that uses all 37 channels with channel
  selection #1 (upstream's method).
- Advertising (``ubertooth-btle -s``) is seen by a PC Bluetooth adapter, with the address, flags
  and CRC intact.

Bluetooth BR and AFH
====================

- ``ubertooth-rx`` and the others work on a fixed channel, the 25 Hz sweep (``SET_CHANNEL 9999``)
  and the AFH mode's visits.
- ``ubertooth-rx -z`` lists the LAPs (and UAPs) on the air.
- ``ubertooth-afh`` builds a piconet's channel map.

Measurements
************

- The demodulator's full path costs about 50 cycles a sample at 4 MS/s (the vector unit and
  assembly), within the 60 a sample the stream leaves.
- The later status in `GFSK and Bluetooth LE <../../doc/modem/gfsk.md>`__ gives about 53 of 60
  for the same path.
- Behind its squelch it averages about 10.
- A 2 ms sample ring rides out the rest: sniffing a busy advertising channel for 5 minutes lost
  0.1 % of the stream.
- The modes report lost samples in their log line (``samples lost``); a packet in a lost stretch
  is gone.
- Following and sniffing catch about 90 % of the CONNECT_INDs sent to a peripheral that
  advertises on the channel (an office at night, gain 40).
- A follow misses about 1 % of a connection's events.

The dated runs are in `ubertooth internals <../../doc/apps/ubertooth-internals.md>`__, the
demodulator's budget in `GFSK and Bluetooth LE <../../doc/modem/gfsk.md>`__.

Limitations
***********

Retune time
===========

- A retune is ``esp_sdr_set_freq()``: 3 ms of settling plus a calibration, 3 to 4 ms in all, and
  the ring has to be stopped (`Tuning <../../doc/radio/tuning.md>`__).
- So Bluetooth BR hopping (1600 hops/s) is out of reach: ``START_HOPPING``, ``TX_SYMBOLS`` and the
  BR follow mode stall.
- Jamming (``JAM_ONCE``, ``JAM_CONTINUOUS``) needs a reaction inside the packet and stalls too.
- BLE connection following works for connection intervals that leave time for a retune and a ring
  start: events the radio cannot reach in time are passed over.

Requests without a counterpart
==============================

- ``SET_ISP``, range test and repeater have no counterpart here.
- ``EGO`` (a 250 kbit/s 2-FSK modem of its own) and ``RX_GENERIC`` (no host tool uses it) stall.

Radio and memory
================

- One radio user at a time, half duplex.
- Internal RAM below the three ring banks is full: the app's stacks and USB buffers live in the
  high region (``ESP_SDR_HIGH_RAM``), the packet queue in PSRAM.
- An Ubertooth follows the 1M PHY only, and most current centrals move a connection to 2M after a
  second or so: the follower then reports ``PHY change`` and returns to advertising.

Levels
======

The receive stream's RSSI and the spectrum analyser use placeholder calibrations
(``UBT_RSSI_K_DEFAULT``, ``ubt specanfs``) some 30 dB apart; neither has been checked against a
source of known power.

What is not done yet in the demodulator is in `GFSK and Bluetooth LE <../../doc/modem/gfsk.md>`__,
the app's open points in `ubertooth internals <../../doc/apps/ubertooth-internals.md>`__.

Source and licensing
********************

- ``src/upstream/`` holds files of the Ubertooth project (GPL-2.0-or-later, headers kept):
  ``ubertooth_interface.h`` (the protocol), ``types.h`` and ``bluetooth_le.[ch]`` (CRC-24,
  whitening, channel helpers).
- The rest is new, under GPL-3.0-or-later.
- The USB product, the protocol and the behaviour of the requests follow the upstream firmware
  (``firmware/bluetooth_rxtx/bluetooth_rxtx.c``); deviations are marked in the sources.

See also
********

- `ubertooth internals <../../doc/apps/ubertooth-internals.md>`__: design and measurement log.
- `GFSK and Bluetooth LE <../../doc/modem/gfsk.md>`__: the demodulator.
- `Applications <../../doc/apps/index.md>`__: DFU updates and USB access.

.. nav-start

Previous: `zstack <../zstack/README.rst>`__ · Up: `Applications <../../doc/apps/index.md>`__ · Next: `ubertooth internals <../../doc/apps/ubertooth-internals.md>`__

.. nav-end
