.. SPDX-License-Identifier: GPL-3.0-or-later

.. _esp_sdr_link:

####
link
####

A packet radio link between two ESP32-S3 boards on the raw Wi-Fi I/Q paths of the ``esp-sdr``
module, at the native 80 MS/s, with an iperf style throughput test on the USB shell.

Overview
********

How the PHYs work is in `QAM and OFDM <../../doc/modem/qam-ofdm.md>`__, the codes in
`Error correction <../../doc/modem/fec.md>`__.

- **PHY** (``src/qam.c``): single carrier QPSK to 4096-QAM; frames are built cyclic and played in
  a DAC loop.
- **Error correction** per frame: RS(255,223), extended Hamming (128,120), binary BCH codes
  (``src/bch.c``) or none.
- **Pairs**: two half length frames looped together, decoded on both CPUs and answered with one
  ACK.
- **MAC** (``src/mac.c``): CSMA/CA with stop and wait ARQ.
- **OFDM**: a second PHY (``src/ofdm.c``) with its link frames (``src/ofdm_frame.c``), which the
  MAC uses instead of QAM.

Hardware
********

Two XIAO ESP32S3 boards, one sending and one receiving.

Building and flashing
*********************

Build with sysbuild (MCUboot, USB DFU) and flash each board once:

.. code-block:: console

   west build --sysbuild -b xiao_esp32s3/esp32s3/procpu esp-sdr-zephyr/apps/link
   west flash

Later updates go over USB DFU with ``west dfu -d <build dir> [-s <usb serial>]``
(`Applications <../../doc/apps/index.md>`__, "DFU updates").

Usage
*****

Host tools and simulators
=========================

QAM over the MAC
----------------

``tools/link_perf.py`` drives both boards at once:

.. code-block:: console

   tools/link_perf.py run --server <serial> --client <serial> -t 10 -m 256qam \
           -f rs -w 2 --set txgain=4 --set rxgain=32     # --dst 0xff: broadcast, no ACKs

``tools/link_perf.py dump`` saves a capture for ``tools/qam_sim.c``, the host simulator of the PHY
(see its header for the build line).

OFDM over the MAC
-----------------

Both boards set alike:

.. code-block:: console

   tools/link_perf.py run --server <serial> --client <serial> -t 20 -m 64qam \
           --set phy=1 --set txgain=4 --set rxgain=32 --set ackair=200

The raw OFDM PHY test from the host:

.. code-block:: console

   tools/link_perf.py ofdm --rx <serial> --tx <serial> -n 100 \
           --set txgain=4 --set rxgain=32 --set bw=30 --set ch=96

``tools/ofdm_sim.c`` is the OFDM host simulator: the PHY, link frames with ``FRAME=1``, ACKs with
``ACK=1``, the two-CPU split with ``SPLIT=1``. It also decodes a capture saved with ``dump``.

Shell
=====

The shell is on ``/dev/serial/by-id/usb-Zephyr_Project_ESP-SDR_Link_*``.

Link commands
-------------

.. code-block:: console

   link status                       # address, settings, counters, timing
   link set txgain 4 rxgain 32       # several at once; also bw, cbw, amp, air, turn, ...
   link server                       # on the receiving board
   link client 0x00 -t 10 -m 256qam -f rs -w 2

- ``-m`` picks the modulation, ``-w 2`` pairs and ``-w 1`` single frames.
- ``-f <code>`` picks the error correction: ``rs``, ``none`` or ``bch<t>-<bytes>``
  (`Error correction <../../doc/modem/fec.md>`__).
- ``link set phy 1`` on both boards (and the same ``ofdm set`` settings on both) makes the MAC use
  the OFDM PHY.

OFDM commands
-------------

``ofdm set`` changes the layout, several keys at once: ``bw``, ``ch``, ``cp``, ``fs``, ``rxdiv``,
``pilots``, ``smooth``, ``amp``, ``syms``, and ``mod`` for the raw test.

The raw PHY test outside the MAC, with known test bits:

.. code-block:: console

   ofdm set bw 26 ch 84 mod 64qam
   ofdm tx -t 10                     # on the sending board
   ofdm rx -n 100 -v                 # on the receiving one; -v: channel per subcarrier,
                                     # -d: dump the first capture with errors
   ofdm bench                        # FFT and RS decode times

The streaming test, frames played back to back without leaving TX:

.. code-block:: console

   ofdm set bw 26 ch 84 mod 16qam    # on both
   ofdm stx -t 5 -p 6000             # sending board: a frame every 6000 us
   ofdm srx -t 6                     # receiving board

What ``ofdm snr`` and these tests measure is in `QAM and OFDM <../../doc/modem/qam-ofdm.md>`__.

Measurements
************

Two XIAO ESP32S3 boards cabled through a 30 dB attenuator; the tables and setups are in
`QAM and OFDM <../../doc/modem/qam-ofdm.md>`__.

- **QAM**: up to 1.69 Mbit/s (256-QAM, RS, pairs), bound by the receiver's decode time.
- **OFDM** over the MAC: up to 2.24 Mbit/s (30 MHz, 96 subcarriers, 64-QAM); 26 MHz is the
  default.

Limitations
***********

- Wide frames need the 40 MHz mode (``cbw 2``, the default) and ``amp`` 90 or less (the default):
  see `Filters <../../doc/radio/filters.md>`__.
- The forced receive gain is lost now and then and the library re-forces it (``link status``,
  engine line): see `Gain <../../doc/radio/gain.md>`__.

See also
********

- `QAM and OFDM <../../doc/modem/qam-ofdm.md>`__: the PHYs, the MAC, the measurements.
- `Error correction <../../doc/modem/fec.md>`__: RS, Hamming and BCH codes.
- `Applications <../../doc/apps/index.md>`__: DFU updates and USB access.
