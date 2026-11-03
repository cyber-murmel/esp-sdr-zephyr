.. _esp_sdr_mch2022_coproc:

ESP-SDR MCH2022 coprocessor
###########################

Firmware for the RP2040 of the MCH2022 badge, in place of the stock
`mch2022-firmware-rp2040 <https://github.com/badgeteam/mch2022-firmware-rp2040>`_. It does all the
stock firmware does and keeps its interfaces, so the stock launcher, ``west flash`` on the ESP32,
Zephyr's ``mch2022`` board drivers and the ``/dev/serial/by-id`` names go on as they were:

- USB ``16d0:0f9a``: CDC ACM ports to the ESP32 (``if00``) and the FPGA (``if02``) with their line
  settings, WebUSB interfaces 4 and 5 with their requests, the DTR and RTS reset of the ESP32;
- the I2C coprocessor at 0x17: buttons, backlight, FPGA reset and CDONE, VUSB, VBAT, temperature,
  charger, SAO port, IR LED, WS2812, reset lock.

What it adds (``include/esp_sdr/mch2022_coproc.h``):

- both UARTs on DMA, so nothing is lost at the ESP32's 5 Mbaud (the stock firmware polls them, and
  ``apps/osmosdr_serial`` stopped at 2 Mbaud);
- interface 6, a bulk pipe to the ESP32's UART with requests for the chip itself: counters, log,
  resets, the ROM's USB bootloader;
- registers 100 to 103, by which the ESP32 can tell it from the stock firmware.

Building and installing
***********************

.. code-block:: console

   west build -b mch2022_rp2040 apps/mch2022_coproc
   west rp2040 flash

The image goes behind the badge's own bootloader, which stays in flash. From the stock firmware
the first time takes SELECT at power-up; ``west rp2040`` says how. Afterwards a request gets it into
the ROM's bootloader. The board's documentation
(``boards/badgeteam/mch2022_rp2040/doc/index.rst``) has the ways back:

.. code-block:: console

   west rp2040 rollback rp2040_firmware.bin

``west rp2040 info``, ``log`` and ``stats`` show what it runs and how it fares.
