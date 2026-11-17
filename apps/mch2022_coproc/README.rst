.. SPDX-License-Identifier: GPL-3.0-or-later

.. _esp_sdr_mch2022_coproc:

##############
mch2022_coproc
##############

Firmware for the RP2040 of the MCH2022 badge, in place of the stock
`mch2022-firmware-rp2040 <https://github.com/badgeteam/mch2022-firmware-rp2040>`_.

Overview
********

It does all the stock firmware does and keeps its interfaces (`Kept interfaces`_), so the stock
launcher, ``west flash`` on the ESP32, Zephyr's ``mch2022`` board drivers and the
``/dev/serial/by-id`` names go on as they were.

What it adds (``include/esp_sdr/mch2022_coproc.h``):

- both UARTs on DMA;
- interface 6, a bulk pipe to the ESP32's UART with requests for the chip itself;
- the FPGA's fast line into that pipe;
- registers 100 to 103, by which the ESP32 can tell it from the stock firmware.

Why, how they work and what they carry is in `MCH2022 badge <../../doc/apps/mch2022.md>`__.

Hardware
********

The RP2040 of the MCH2022 badge. Its roles between USB, the ESP32 and the FPGA are on the board
page, `MCH2022 badge (RP2040 coprocessor) <../../boards/badgeteam/mch2022_rp2040/doc/index.rst>`__.

Kept interfaces
===============

The first four rows are on USB ``16d0:0f9a``:

.. list-table::
   :header-rows: 1

   * - Interface
     - What stays
   * - ``if00``
     - the CDC ACM port to the ESP32, with its line settings
   * - ``if02``
     - the CDC ACM port to the FPGA, with its line settings
   * - interfaces 4 and 5
     - WebUSB, with their requests
   * - DTR and RTS
     - the reset of the ESP32
   * - I2C coprocessor at 0x17
     - buttons, backlight, FPGA reset and CDONE, VUSB, VBAT, temperature, charger, SAO port,
       IR LED, WS2812, reset lock

What goes beyond this list (descriptors, WebUSB, reset timings, the stock firmware's bugs) is in
`MCH2022 badge <../../doc/apps/mch2022.md>`__.

Building and flashing
*********************

.. code-block:: console

   west build -b mch2022_rp2040 apps/mch2022_coproc
   west rp2040 flash

- The image goes behind the badge's own bootloader, which stays in flash.
- From the stock firmware, the first time takes SELECT at power-up; ``west rp2040`` says how.
- Afterwards a request gets it into the ROM's bootloader.

Back to the stock firmware
==========================

.. code-block:: console

   west rp2040 rollback rp2040_firmware.bin

The other ways back are on the board page,
`MCH2022 badge (RP2040 coprocessor) <../../boards/badgeteam/mch2022_rp2040/doc/index.rst>`__.

Usage
*****

``west rp2040 info``, ``log`` and ``stats`` show what it runs and how it fares.

See also
********

- `MCH2022 badge <../../doc/apps/mch2022.md>`__: the firmware's design, the FPGA accelerator and
  the links to the host.
- `MCH2022 badge (RP2040 coprocessor) <../../boards/badgeteam/mch2022_rp2040/doc/index.rst>`__:
  flash layout, the ROM bootloader and the ways back.
- `osmosdr_serial <../osmosdr_serial/README.rst>`__: the SDR app on the badge's ESP32.
- `micropython <../micropython/README.rst>`__: MicroPython on the badge's ESP32.
