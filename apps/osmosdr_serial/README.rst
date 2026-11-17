.. SPDX-License-Identifier: GPL-3.0-or-later

.. _esp_sdr_osmosdr_serial:

##############
osmosdr_serial
##############

.. nav-start

Previous: `osmosdr <../osmosdr/README.rst>`__ · Up: `Applications <../../doc/apps/index.md>`__ · Next: `osmosdr_bridge <../osmosdr_bridge/README.rst>`__

.. nav-end

The osmosdr protocol over a serial link, for the ESP32-C6 and the ESP32-C5, whose only USB port
is USB-Serial-JTAG, and for the MCH2022 badge's ESP32.

Overview
********

The app speaks the `osmosdr <../osmosdr/README.rst>`__ protocol
(``../osmosdr/src/esdr_proto.h``) as frames on a byte stream, one direction at a time, as on
the S3. `Host transports <../../doc/samples/transports.md>`__ describes the serial transport.

The byte stream is USB-Serial-JTAG on the XIAO C6 and the C5, SDIO or UART on the WHY2025
badge's C6, and UART0 through the RP2040 on the MCH2022 badge.

.. list-table::
   :header-rows: 1

   * - Mode
     - Chips
     - What the host gets
   * - Receive
     - C6, C5
     - continuous I/Q at 250, 125, 62.5, 31.25 or 15.625 kS/s, 8 or 16 bit, from the modem's
       IQ estimator sums (`Paced streams <../../doc/samples/paced.md>`__); over SDIO also 2, 1
       and 0.5 MS/s
   * - Transmit
     - C6, C5
     - the host's samples through the TX DC value register
       (`Paced streams <../../doc/samples/paced.md>`__ has the DAC rate, the tuning range and
       the power steps of each chip)
   * - Triggered snapshots
     - C6
     - 80 MS/s snapshots around bursts (`Triggered snapshots`_)
   * - Gapless receive
     - C6 on SDIO
     - the whole 80 MHz without a gap, pruned to what the link carries (`Gapless 80 MS/s`_)
   * - Bursts at the native rates
     - all
     - ``ESDR_REQ_CAPTURE_BURST``
   * - Decimated bursts
     - the MCH2022 badge's ESP32
     - 16 MS/s bursts decimated to 2 MS/s down to 125 kS/s (`MCH2022 badge`_)

Hardware
********

.. list-table::
   :header-rows: 1

   * - Board target
     - Chip
     - Link to the host
     - Console
   * - ``xiao_esp32c6/esp32c6/hpcore``
     - ESP32-C6
     - USB-Serial-JTAG
     - UART0 (D6 TX, D7 RX)
   * - ``waveshare_esp32c5_zero/esp32c5/hpcore``
     - ESP32-C5
     - USB-Serial-JTAG
     - UART0
   * - ``why2025_badge/esp32c6/hpcore``
     - ESP32-C6
     - SDIO to the P4 (default), or UART (see `Links`_)
     - USB-Serial-JTAG on J8, while the P4 hands it over
   * - ``mch2022/esp32/procpu``
     - ESP32
     - UART0 through the badge's RP2040
     - none

- The board configs select the U.FL connector.
- Opening the port does not reset the board as long as the host leaves DTR and RTS alone (the
  host tool and the SoapySDR module open it raw); tools that toggle them (esptool, terminal
  programs) reset it.

Building and flashing
*********************

.. code-block:: console

   west build -b xiao_esp32c6/esp32c6/hpcore esp-sdr-zephyr/apps/osmosdr_serial
   west build -b waveshare_esp32c5_zero/esp32c5/hpcore esp-sdr-zephyr/apps/osmosdr_serial
   west build -b why2025_badge/esp32c6/hpcore esp-sdr-zephyr/apps/osmosdr_serial
   west flash --esp-device /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<mac>-if00

The WHY2025 badge's C6 reaches its USB port only while the P4 hands it over: ``west dfu --c6``
with `micropython <../micropython/README.rst>`__ on the P4, or as
`osmosdr_bridge <../osmosdr_bridge/README.rst>`__ describes.
`Applications <../../doc/apps/index.md>`__ has the steps all apps share.

Links
=====

The WHY2025 badge's C6 reaches its P4 over one of two links:

.. list-table::
   :header-rows: 1

   * - Link
     - Build
     - Rates
   * - UART
     - chosen ``esdr,link-uart``, ``-DEXTRA_CONF_FILE=uart.conf``; the badge's fallback
     - 5 Mbaud with RTS flow control
   * - SDIO
     - ``CONFIG_APP_LINK_SDIO``, the badge's default; the C6's SDIO slave
       (``src/link_sdio.c``)
     - receive **2 MS/s** (2, 1, 0.5 MS/s, then the paced rates), in 8-bit items above
       250 kS/s, gapless at the WHY badge's P4; transmit 250 kS/s

`Host transports <../../doc/samples/transports.md>`__ describes both links;
`Paced streams <../../doc/samples/paced.md>`__ how the receiver keeps up at 2 MS/s.

The C6's other apps
===================

The WHY2025 badge's C6 can hold its other apps next to this one, as complete images under
MCUboot:

- this app in the first slot, then ``capture``, ``rfprobe``, ``wpan`` and ``zstack``, up to
  1 MiB each (``c6apps/``);
- a power-on starts this app, so the P4 has its radio;
- the C6 has no PSRAM for extensions, so this replaces `multiapp <../multiapp/README.rst>`__ on
  it (`multiapp internals <../../doc/apps/multiapp-internals.md>`__).

.. code-block:: console

   west build --sysbuild -b why2025_badge/esp32c6/hpcore -d build-c6 \
       esp-sdr-zephyr/apps/osmosdr_serial -- -DSB_EXTRA_CONF_FILE=c6apps/sysbuild.conf
   west dfu --c6 -d build-c6 --c6-time 120    # with apps/micropython on the P4
   west dfu --c6-run wpan --c6-time 300       # its console on J8 for 300 s
   west dfu --c6-run osmosdr_serial           # back to the SDR

- An app chosen this way stays through resets, since a terminal opening its console resets the
  chip; a power-on or ``--c6-run osmosdr_serial`` ends it.
- MCUboot validates the image first (``lib/boot_select``): one that is missing or damaged boots
  this app instead.
- While another app runs, the P4's ``esdr.init()`` still returns, and the P4 has no radio.

Usage
*****

Host tool
=========

``tools/esdr_serial.py`` speaks the transport from Python:

.. code-block:: console

   tools/esdr_serial.py <port> info
   tools/esdr_serial.py <port> rx --rate 250000 --seconds 4 --out rx.npy
   tools/esdr_serial.py <port> tx --rate 125000 --seconds 4 --tone 30000

SoapySDR, GNU Radio, gqrx
=========================

The osmosdr app's SoapySDR module (``../osmosdr/soapy``) has a serial backend: name the port in
the device string.

- It does not probe serial ports by itself (that would write into other boards' consoles).
- Over the serial link the native format is CS8, and CF32 travels as 8-bit items.

.. code-block:: console

   SoapySDRUtil --probe="driver=esdr,port=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<mac>-if00"
   gqrx        # device string: soapy=0,driver=esdr,port=/dev/serial/by-id/...

Behind a USB bridge add the line rate, as for the MCH2022 badge:
``driver=esdr,port=/dev/serial/by-id/usb-Badge.team_MCH2022_badge_<serial>-if00,baud=2000000``.

Receive modes
=============

Triggered snapshots
-------------------

On the ESP32-C6 (``CONFIG_ESP_SDR_TRIG``, ``include/esp_sdr/esp_sdr_trig.h``),
``ESDR_MODE_TRIGGER`` keeps the whole 80 MS/s, but only around bursts.

- ``ESDR_REQ_SET_TRIGGER`` sets the threshold, a multiple of the quiet baseline, 8 by default.
- The newest ``ESDR_TRIG_SAMPLES`` (16320) samples go to the host as 8 int8 blocks flagged
  ``ESDR_BLK_TRIGGER``, the first also ``ESDR_BLK_GAP``.
- Their index counts from the trigger (negative before it).
- In the office's 2.4 GHz Wi-Fi the WHY badge's P4 received 52 a second with none broken.

On the host: ``esdr_usb trig`` and the Soapy stream argument ``trigger=`` (see
`osmosdr <../osmosdr/README.rst>`__). `Burst capture <../../doc/samples/bursts.md>`__ describes
the detection and the dead time.

Gapless 80 MS/s
---------------

``ESDR_MODE_GAPLESS`` (``include/esp_sdr/esp_sdr_gapless.h``, after upstream's ring and
h0m3us3r's eSpDR) receives the whole 80 MHz without a gap and keeps what stands out, as
``ESDR_BLK_SEG`` blocks of segments (``esdr_seg.h``).

- The WHY2025 badge's C6 (SDIO) is built for it by default: its board configuration holds what
  ``gapless.conf`` sets.
- Another C6 on SDIO takes it with ``-DEXTRA_CONF_FILE=gapless.conf``.

``../osmosdr/tools/esdr_usb gapless`` records the blocks through
`osmosdr_bridge <../osmosdr_bridge/README.rst>`__, and ``tools/gapless.py`` checks them: with a
carrier on air, its phase across the gaps proves every segment's index.

`Gapless receive <../../doc/samples/rings.md>`__ describes the ring, the window selection, the
servos and the measurements; `Burst detection <../../doc/modem/bursts.md>`__ the width sorting.

Gapless settings
----------------

.. list-table::
   :header-rows: 1

   * - Setting
     - Effect
   * - ``ESDR_REQ_SET_GAPLESS``
     - what is kept: two servos keep 5 to 10 % headroom, the threshold against the link's
       budget and, with automatic gain, the receive gain against full scale
   * - ``ESDR_GAPLESS_F_HALF``
     - 40 MS/s, every other sample without a filter; every index, count and window of the run
       on the 40 MS/s axis
   * - ``ESDR_GAPLESS_F_WIDE``
     - width sorting, off by default: a signal wider than some 10 MHz (Wi-Fi) goes to the link
       as a 4-pair ``ESDR_SEG_WIDE`` record in place of its samples
   * - ``CONFIG_ESP_SDR_GAPLESS_THR_LINK_PCT``
     - how full the link may get (95 %)
   * - ``CONFIG_ESP_SDR_GAPLESS_THR_LOST_PCT``
     - the share of the kept samples the CPU may lose in a period before the threshold rises
       (5 %)

Gapless on the C5
-----------------

On the C5, ``-DEXTRA_CONF_FILE=diag.conf`` (``CONFIG_APP_RX_DIAG``) receives through the modem's
diagnostic bus instead. `Gapless receive <../../doc/samples/rings.md>`__ describes the
diagnostic-bus ring and its measurements.

MCH2022 badge
=============

The badge's classic ESP32 (``mch2022/esp32/procpu``) has no paced receiver:

- It streams 16 MS/s bursts decimated by the library (``esp_sdr_rx_capture_decimated()``) at
  2 MS/s to 125 kS/s, each about 1 ms contiguous.
- The gap to the next burst moves the block index and flags the block ``ESDR_BLK_GAP``, as many
  bursts as the link carries.
- Bursts at the native rates (``ESDR_REQ_CAPTURE_BURST``) are as on the others.
- No transmit. No console.

The link is UART0, which the badge's RP2040 bridges to USB without flow control wires, in one of
two ways. The firmware understands both and follows the host: hello goes to frames, the zeros a
plain host opens with go back.

Framed link
-----------

With `mch2022_coproc <../mch2022_coproc/README.rst>`__ on the RP2040: the framed link
(``esp_sdr/esdr_link.h``) on its USB interface 6, through the FPGA if its link bitstream is
loaded, as this image's is, else up to 5 Mbaud on the UART.

.. list-table::
   :header-rows: 1

   * - Setting
     - SoapySDR
     - ``tools/esdr_serial.py``
   * - The badge
     - ``driver=esdr,badge=<serial>`` (found by itself)
     - ``usb`` or ``usb:<serial>`` as the port
   * - Stay on the UART
     - ``fast=0``
     - ``--no-fast``
   * - Lower the UART's rate
     - ``baud=``
     - ``--baud``

Plain transport
---------------

With any firmware on the RP2040: the plain transport on its first USB serial port at 2 Mbaud
(the stock firmware did not answer at 3 and 4): ``driver=esdr,port=<port>,baud=2000000``,
``--baud 2000000 <port>``.

Rates and reset
---------------

At 2 MS/s the framed link through the FPGA averages 449.6 kS/s;
`MCH2022 badge <../../doc/apps/mch2022.md>`__ has the rates of every link and baud rate and
describes the framed link.

The stock RP2040 firmware resets the ESP32 into its download mode for esptool and keeps IO0 low
until a reset into the application, which the RTS/DTR sequences do not always give. Its WebUSB
request 0x23 does (``wValue`` 0: application), e.g. with pyusb
``usb.core.find(idVendor=0x16D0, idProduct=0x0F9A).ctrl_transfer(0x21, 0x23, 0, 4, None)``.

Output format
*************

The blocks and requests are osmosdr's (see `osmosdr <../osmosdr/README.rst>`__).

Statistics
==========

``GET_STATS`` fills the osmosdr fields; the pacing reports in the S3 ring's:

- ``ring_runs`` counts streams;
- ``ring_errors`` polls a whole period late;
- ``ring_late_max`` the worst lateness in CPU cycles.

Limitations
***********

- USB-Serial-JTAG: 250 kS/s receive fits as 8-bit items only, 16 bit up to 125 kS/s.
- Transmit on the C6: at 250 kS/s about 10 % of the input arrives late and plays as zeros, so
  125 kS/s is its reliable rate. `Host transports <../../doc/samples/transports.md>`__ has the
  measured rates of each chip.
- Transmit on the C5 at 250 kS/s: the sources disagree whether it keeps up;
  `Host transports <../../doc/samples/transports.md>`__ has both.
- No offset tuning on the paced receiver: the LO leakage stays at the centre.

See also
********

- `osmosdr <../osmosdr/README.rst>`__: the protocol, ``esdr_usb`` and the Soapy module.
- `osmosdr_bridge <../osmosdr_bridge/README.rst>`__: the WHY2025 badge's P4 as the USB device in
  front of this app.
- `micropython <../micropython/README.rst>`__: the WHY2025 badge's P4 using this app as its
  radio.
- `Paced streams <../../doc/samples/paced.md>`__, `Gapless receive <../../doc/samples/rings.md>`__
  and `Burst capture <../../doc/samples/bursts.md>`__: the receive and transmit paths.
- `Host transports <../../doc/samples/transports.md>`__: the serial links and their throughput.
- `MCH2022 badge <../../doc/apps/mch2022.md>`__: the RP2040's links and the rates through them.

.. nav-start

Previous: `osmosdr <../osmosdr/README.rst>`__ · Up: `Applications <../../doc/apps/index.md>`__ · Next: `osmosdr_bridge <../osmosdr_bridge/README.rst>`__

.. nav-end
