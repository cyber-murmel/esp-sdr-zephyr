MCH2022 badge (RP2040 coprocessor)
##################################

Overview
********

The RP2040 of the `MCH2022 badge <https://badge.team/docs/badges/mch2022/>`_, next to its ESP32
(Zephyr's board ``mch2022``) and iCE40UP5K. It sits between USB and the other two chips: it
bridges the ESP32's UART0 and the FPGA's UART, holds the ESP32's EN and IO0 and the FPGA's CRESET
and reads its CDONE, and answers the ESP32 on I2C as a coprocessor with the buttons, the
backlight, the IR LED, the SAO port and the VUSB and VBAT readings.

The firmware for it is ``apps/mch2022_coproc``.

Flash layout
************

The 2 MiB W25Q16JV holds, as on a stock badge:

.. list-table::
   :header-rows: 1

   * - Address
     - Content
   * - ``0x10000000``
     - The badge's bootloader (stock, with its second stage bootloader). Never written here.
   * - ``0x10008000``
     - The image header: vector table address, size, CRC-32 over the image.
   * - ``0x10010000``
     - The application, vector table first, up to 1984 KiB.

The bootloader runs the application if the header holds, the stack pointer is in RAM and the
reset vector is Thumb code inside the image. It has already set up XIP and the clocks. The build's
``zephyr.uf2`` (``support/rp2040_image.py``) runs from the header to the application's end and
writes nothing below ``0x10008000``, so the bootloader stays and a stock image can always go back
on the same way.

Getting into the ROM's bootloader
*********************************

The RP2040's ROM takes images over USB, whatever is in flash (``2e8a:0003``, ``picotool`` or the
``RPI-RP2`` drive). Any of these gets there:

#. From a running ``apps/mch2022_coproc``: ``west rp2040 bootsel``.
#. By hand: SELECT is the RP2040's BOOTSEL. Take the power away (USB out and the battery switch
   off), hold SELECT, plug USB in, let go. With the battery on, grounding test point TP10 (RUN)
   for a moment while holding SELECT does the same.
#. Over SWD, with a probe on the test points TP9 (SWDIO) and TP14 (SWCLK), TP10 (RUN).

Going back to the stock firmware
********************************

Any of these restores it:

- With a backup of the whole flash (``picotool save -a``): ``picotool load`` it in the ROM's
  bootloader.
- With the stock application (``resources/rp2040_firmware.bin`` of
  `mch2022-firmware-esp32 <https://github.com/badgeteam/mch2022-firmware-esp32>`_, 30188 bytes,
  SHA-256 ``ebfb25a013d1f8574a04e953363666b76b160b2fba140ec774e8485e5be015fc``):
  ``support/rp2040_image.py rp2040_firmware.bin stock.uf2``, then ``picotool load stock.uf2`` in
  the ROM's bootloader.
- With the stock launcher on the ESP32: its menu Settings, "Flash RP2040 firmware". Or, whatever
  runs on the RP2040: power off, hold MENU, power on. The badge's bootloader then stays in charge
  (register 0 reads 0xFF) and the launcher flashes the firmware it carries.
