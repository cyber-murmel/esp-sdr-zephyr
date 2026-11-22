#################
ESP-SDR on Zephyr
#################

.. nav-start

Next: `The radio <doc/radio/index.md>`__

.. nav-end

A software defined radio on the Wi-Fi radio of the ESP32-S3, ESP32-C6, ESP32-C5, ESP32-S31 and
the classic ESP32, for Zephyr: I/Q capture from the receiver, and I/Q playback through the
transmit DAC on all but the classic ESP32, which only receives.

Ported from `ESPARGOS esp-sdr <https://github.com/ESPARGOS/esp-sdr>`__ and extended. The
repository is a west manifest repository and a Zephyr module, laid out like
`example-application <https://github.com/zephyrproject-rtos/example-application>`__.

Safety
******

.. warning::

   **This project is neither intended nor qualified for productive use!**

   The main purpose of this project is "Spaß am Gerät".

Operating limits
================

This project views safe operating parameters as suggestions:

- Radios may be driven beyond their design specifications.
- Generated signals may not be compliant with your local regulations.

Spectrum etiquette
==================

Be a good neighbor in the spectrum:

- Don't perform radiative tests.
- For development you can connect two devices through coaxial cable with a 30 to 60 dB
  attenuator.

Tainted devices
===============

The firmware may **permanently** alter the radios' analog characteristics:

- Devices that ran any firmware from this project should be regarded as tainted.
- This **disqualifies the device from any future use in certified products**.
- It is recommended to mark any used devices in a visible way.

Getting started
***************

::

   west init -m https://github.com/cyber-murmel/esp-sdr-zephyr esp-sdr-ws
   cd esp-sdr-ws
   west update
   west blobs fetch hal_espressif
   west blobs fetch esp-sdr-zephyr
   west build --pristine --sysbuild --board xiao_esp32s3/esp32s3/procpu esp-sdr-zephyr/apps/multiapp
   west flash
   west dfu --storage

This puts multiapp on a XIAO ESP32S3. On its USB shell ``multiapp list`` shows the apps and
``multiapp run <name>`` starts one, for example ``multiapp run osmosdr``
(`multiapp <apps/multiapp/README.rst>`__).

Each app also builds on its own. `Applications <doc/apps/index.md>`__ has the steps all apps share
and how an app's configuration splits into SoC and board files. Releases carry the multiapp image
and the SoapySDR module for gqrx and GNU Radio.

The library
***********

``lib/esp_sdr`` gives raw I/Q access to the radio. This section sums it up; the guides
(`Documentation`_) teach it part by part.

Chips and rates
===============

Per chip, the highest rate at which each kind of sample path moves samples while it runs.
`Samples in and out <doc/samples/index.md>`__ has the full matrix: every path with its average
rate, Kconfig option and source. `The radio <doc/radio/index.md>`__ has each chip's boards and
radio.

=========  ===========================  ==============  ===================  =============  ====================
Chip       PLL lock range               Capture, burst  Capture, continuous  Replay, burst  Replay, continuous
=========  ===========================  ==============  ===================  =============  ====================
ESP32-S3   1817 to 2793 MHz             80 MS/s         16 MS/s (ring)       80 MS/s        80 MS/s (loop)
ESP32-C6   1774 to at least 2856 MHz    80 MS/s         80 MS/s (pruned)     none           250 kS/s (paced)
ESP32-C5   not measured (no lock flag)  80 MS/s         40 MS/s (4+4 bit)    80 MS/s        80 MS/s (loop)
ESP32-S31  2200 to some 2900 MHz        80 MS/s         40 MS/s (8+8 bit)    80 MS/s        80 MS/s (loop)
ESP32      not measured                 80 MS/s         none                 none           none
=========  ===========================  ==============  ===================  =============  ====================

- The lock range is the LO's reach on air: the PLL directly from 2210 MHz up and, below it,
  through the 5/6 divider on the S3, the C6 and the ESP32; the C5 and the S31 have none.
- "Loop" repeats a waveform held in memory, "pruned" keeps the windows above the noise, "paced"
  moves one sample at a time from the CPU.
- Sources disagree on the S3's edges and the S31's high edge:
  `Samples in and out <doc/samples/index.md>`__ keeps all figures under "Conflicting figures".

Capabilities
============

- **Tuning:** 100 to 6000 MHz in the API, but the PLL locks over a chip-specific range (the table
  above); ``-ERANGE`` when the VCO is out of range. `Tuning <doc/radio/tuning.md>`__
- **Gain:** AGC or a fixed index; the fixed gain and the transmit power steps use ``librftest.a``.
  `Gain <doc/radio/gain.md>`__
- **Capture:** bursts of 256 to 16380 samples through the analog low-pass filter, with optional
  CIC decimation. `Burst capture <doc/samples/bursts.md>`__
- **Gapless receive:** rings without a gap; on the S3 ring, a grid of channels with one capture
  per packet (``esp_sdr_chancap.h``, ``CONFIG_ESP_SDR_CHANCAP``).
  `Gapless receive <doc/samples/rings.md>`__, `Channelizer <doc/modem/channelizer.md>`__
- **Transmit:** playback, waveform loops and gapless streaming at 40 MS/s / n
  (``CONFIG_ESP_SDR_TX``, needs ``librftest.a`` and PSRAM), 18 power steps.
  `Transmit <doc/samples/transmit.md>`__
- **Paced transmit:** CPU-paced transmit on the C6, C5 and S31.
  `Paced streams <doc/samples/paced.md>`__

Enabling it
===========

::

   CONFIG_NETWORKING=y
   CONFIG_WIFI=y
   CONFIG_ESP32_WIFI_STA_POWER_SAVE_NONE=y
   CONFIG_ESP_SDR=y

``esp_sdr_init()`` takes over the radio the Wi-Fi driver brought up; do not scan, connect or start
an access point while using it.

Some sample paths run without the Wi-Fi driver: `Samples in and out <doc/samples/index.md>`__
lists them under "Radio without the Wi-Fi driver". Chip details are in
`The radio <doc/radio/index.md>`__.

Documentation
*************

Published at https://cyber-murmel.github.io/esp-sdr-zephyr. The guides come in five parts, each
building on the one before:

- `The radio <doc/radio/index.md>`__: the analog receive and transmit paths, tuning, gain,
  filters, impairments and crystal calibration.
- `Samples in and out <doc/samples/index.md>`__: how samples enter and leave the radio: burst
  capture, gapless receive, transmit, paced streams and host transports.
- `Modulation and coding <doc/modem/index.md>`__: the channelizer, IEEE 802.15.4, GFSK and
  Bluetooth LE, burst detection, QAM and OFDM, error correction.
- `Applications <doc/apps/index.md>`__: the apps, their internals and the MCH2022 badge.
- `Reference <doc/dev/index.md>`__: chip notes, code placement, testing, the ZigBit reference and
  the register pages.

The `API reference <https://cyber-murmel.github.io/esp-sdr-zephyr/doxygen/index.html>`__ documents
the headers.

Building the site
=================

::

   cd esp-sdr-zephyr/doc
   doxygen
   make html

The site is in ``doc/_build_sphinx/html``, with the API reference in its ``doxygen/`` folder. CI
builds it with warnings as errors (``SPHINXOPTS=-W``) and publishes it.

Repository layout
*****************

============  ====================================================================
Path          What
============  ====================================================================
``lib/``      The libraries (below)
``include/``  Their API headers (below)
``apps/``     The apps (below)
``tests/``    The test tiers (below)
``boards/``   Boards defined in this repository (below)
``doc/``      Guides, register pages, the Doxygen and Sphinx build (`Documentation`_)
``scripts/``  ``west dfu`` (``west_commands/``), test runners
``zephyr/``   ``module.yml`` and the ``librftest.a`` blobs
``west.yml``  The workspace pins (`Dependencies`_)
============  ====================================================================

Libraries
=========

.. list-table::
   :header-rows: 1
   :widths: 15 25 40 20

   * - Library
     - Kconfig
     - What
     - Guide
   * - ``esp_sdr``
     - ``CONFIG_ESP_SDR``
     - The library: raw I/Q receive and transmit
     - `The radio <doc/radio/index.md>`__, `Samples in and out <doc/samples/index.md>`__
   * - ``ieee802154``
     - ``CONFIG_ESP_SDR_IEEE802154``
     - Software O-QPSK PHY and Zephyr radio driver
     - `IEEE 802.15.4 <doc/modem/ieee802154.md>`__
   * - ``zigbee``
     - ``CONFIG_ZIGBEE_ZNP``
     - Zigbee coordinator stack (MAC, NWK, APS, security) behind the Z-Stack ZNP protocol,
       portable C
     - `zstack <apps/zstack/README.rst>`__

API headers
===========

- ``include/esp_sdr/``: the API. The
  `API reference <https://cyber-murmel.github.io/esp-sdr-zephyr/doxygen/index.html>`__ lists its
  headers by group.
- ``include/zigbee/``: the Zigbee stack's headers.

Apps
====

Each app is a Zephyr application in ``apps/<name>``; ``apps/common`` holds what they share.
`Applications <doc/apps/index.md>`__ lists them by group with the chips each runs on.

Tests
=====

``tests/`` holds ``unit`` (host), ``host`` (the Python tools, pytest), ``integration`` (one board)
and ``regression`` (two boards); the smoke tier is each app's ``sample.yaml``.
`Testing <doc/dev/testing.md>`__ has what each tier covers.

Boards
======

Two boards live in ``boards/``; the others are Zephyr boards from the fork in ``modules/zephyr``
(`Dependencies`_). Each chip's radio is described in `The radio <doc/radio/index.md>`__.

.. list-table::
   :header-rows: 1
   :widths: 35 35 30

   * - Board
     - Chip
     - Defined in
   * - ``waveshare/esp32c5_zero`` (Waveshare ESP32-C5-Zero)
     - ESP32-C5
     - ``boards/``
   * - ``badgeteam/mch2022_rp2040`` (the MCH2022 badge's RP2040,
       `MCH2022 badge (RP2040 coprocessor) <boards/badgeteam/mch2022_rp2040/doc/index.rst>`__)
     - RP2040
     - ``boards/``
   * - ``hackaday/communicator_badge`` (Hackaday Communicator)
     - ESP32-S3
     - Zephyr fork
   * - ``why2025/why2025_badge`` (WHY2025)
     - ESP32-P4 with an ESP32-C6
     - Zephyr fork
   * - ``badgeteam/mch2022`` (the MCH2022 badge's ESP32)
     - ESP32
     - Zephyr fork
   * - ``espressif/esp32s31_function_coreboard``
     - ESP32-S31
     - Zephyr fork
   * - ``viewe/ues31s043h800v480c_u``
     - ESP32-S31
     - Zephyr fork

Versioning
**********

- ``VERSION`` (Zephyr's format) versions the library and the apps; each app prints it at boot.
- Major for breaking API changes, minor for compatible extensions, patch for fixes.
- A tag ``v<VERSION>`` makes a GitHub release.

Dependencies
************

``west.yml`` pins these projects. A fork carries this project's changes on its ``zsdr`` branch.

.. list-table::
   :header-rows: 1
   :widths: 30 15 55

   * - Project
     - Fork or upstream
     - Used for
   * - `Zephyr zsdr <https://github.com/cyber-murmel/zephyr/tree/zsdr>`__
     - fork
     - ESP32-S3 SMP (zephyrproject-rtos/zephyr#120082), CDC-NCM aggregation, llext and Xtensa
       changes for multiapp, the Communicator, WHY2025 and MCH2022 badges with their drivers
   * - `hal_espressif zsdr <https://github.com/cyber-murmel/hal_espressif/tree/zsdr>`__
     - fork
     - The HAL side of the SMP port, the ESP32-P4 flash erase fix
   * - `libvrt zsdr <https://github.com/cyber-murmel/libvrt/tree/zsdr>`__
     - fork
     - VITA 49.2
   * - `esp-sdr <https://github.com/ESPARGOS/esp-sdr>`__
     - upstream, unmodified
     - Receiver helper headers
   * - `esp-dsp <https://github.com/espressif/esp-dsp>`__
     - upstream
     - The ESP32-S3 FFT kernels for ``apps/link`` OFDM, the MicroPython app's Scan
       (``apps/common/esp_dsp.cmake``) and ``apps/ubertooth``'s spectrum analyser
   * - `MicroPython zsdr <https://github.com/cyber-murmel/micropython/tree/zsdr>`__ (a Zephyr
       module; ``CONFIG_MICROPY_STDIO_HOOK`` gives the REPL to an SSH session on the ESP32-S31)
     - fork
     - ``apps/micropython``
   * - micropython-lib
     - upstream
     - ``apps/micropython``
   * - LVGL
     - upstream
     - ``apps/micropython``
   * - `lv_binding_micropython zsdr <https://github.com/cyber-murmel/lv_binding_micropython/tree/zsdr>`__
     - fork
     - ``apps/micropython``: the LVGL binding
   * - pycparser
     - upstream
     - ``apps/micropython``

License
*******

GPL-3.0-or-later, like upstream esp-sdr; images that enable the library are subject to the GPL.

.. nav-start

Next: `The radio <doc/radio/index.md>`__

.. nav-end
