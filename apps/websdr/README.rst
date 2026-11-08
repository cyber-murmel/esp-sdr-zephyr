.. _esp_sdr_websdr:

websdr
######

Overview
********

The burst serial protocol of upstream's firmware on the ``esp-sdr`` receiver, so that
`ESP-WebSDR <https://espargos.net/espsdr/app/>`_ (``ESPARGOS/esp-web-sdr``, ``radio.js``) and
other clients of upstream's ``main/targets/esp32s3`` use this port as they use upstream:
newline-terminated ASCII commands, one text reply each, a capture reply being a ``DATA`` header
followed by the packed samples.

- ``INFO``: ``S3SDR 6 burst 16380`` (``C5SDR``, ``C6SDR`` on the C5, C6).
- ``CAPS``: ``RXLIMITS SERIALLEASE TUNEEXT RX40 RX16 GAIN HWAGC IQ8``, without ``RX40 RX16`` on
  the C5 and C6.
- ``LIMITS?``, ``RANGE?``: gain indices from the PHY, the analog bandwidth range, the rates
  ``esp_sdr_rx_rate_hz()`` reports, 8 and 10 bit; ``ESP_SDR_FREQ_MIN_MHZ`` to
  ``ESP_SDR_FREQ_MAX_MHZ``.
- ``FREQ``, ``BANDWIDTH``, ``LPF AUTO`` (not on the C6, as upstream), ``GAIN HARDWARE``,
  ``GAIN MANUAL <index>``, ``GAIN?``.
- ``CAP16 <samples> <rate>`` (IQ8) and ``CAP20 <samples> <rate>`` (packed IQ10): 256 to 16380
  samples, the rate index being ``enum esp_sdr_rate``: as upstream, an index above the chip's
  highest (6 on the S3, 5 on the C5, 0 on the C6) is ``ERR command``, an unsupported one below it
  ``ERR rate``. The reply is
  ``DATA <samples> <crc32> <capture-us>`` and the payload, CRC-32 (IEEE) over the payload.
- ``SYNC <nonce>`` echoes the line, ``RELEASE`` answers ``OK``.

Differences from upstream:

- ``FREQ`` answers ``ERR pll_range`` when ``esp_sdr_set_freq()`` returns ``-ERANGE`` (the PLL
  calibration's VCO window check), for example ``FREQ 1500`` on the S3 and C6 and ``FREQ 5500``
  on the C6; upstream answers ``OK`` wherever the PLL lands. The C5 has no such check and
  answers ``OK``, as upstream.
- The gain range is ``esp_sdr_rx_gain_max()``'s: 82 on the tested C5, where upstream reports 81.
- No on-chip spectra, ring, GPIO, baud changes or ``VERSION?`` (``SPEC``, ``RING``, ``GPIO``,
  ``UARTBAUD``, ``VERSION`` are not in ``CAPS``), and one serial port, so no lease.
- IQ8 keeps upstream's field order, the low 10-bit field first. ``esp_sdr_rx_pack_iq8()`` puts
  the high field (I, ``esp_sdr_rx_i()``) first, which the viewer would show mirrored; IQ10
  already matches (``esp_sdr_rx_pack_iq10()``).

The serial port is the devicetree chosen node ``esp-sdr,websdr-uart``, interrupt driven.

Requirements
************

An ESP32-S3, ESP32-C5 or ESP32-C6 board:

- ``esp32s3_devkitc/esp32s3/procpu``: the protocol on UART0, the USB-UART bridge ("COM" or
  "UART" port), at the viewer's default 2 MBaud, with the console and logs off. With
  ``-DFILE_SUFFIX=usb`` it is on USB-Serial-JTAG (the native "USB" port), and the console stays
  on UART0.
- ``xiao_esp32s3/esp32s3/procpu``: the protocol on USB-Serial-JTAG, the console on UART0 (D6 TX,
  D7 RX). Built, not run on hardware.
- ``esp32c5_devkitc/esp32c5/hpcore`` and ``esp32c6_devkitc/esp32c6/hpcore``: the protocol on
  UART0, the USB-UART bridge, at 2 MBaud, with the console and logs off. With
  ``-DFILE_SUFFIX=usb`` it is on USB-Serial-JTAG, which is then also the chosen console, with
  Zephyr's output off. On the tested C5 and C6 boards USB-Serial-JTAG passed no data either
  way unless it was the chosen console (``CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG``): Zephyr's
  ``echo_bot`` and ``shell_module`` samples behave the same, also polled, and so did direct
  access to its FIFO registers, while upstream's ESP-IDF firmware worked on the same ports.

USB-Serial-JTAG also flashes, so either port can take a new image.

Building and running
********************

.. code-block:: console

   west build -b esp32s3_devkitc/esp32s3/procpu esp-sdr-zephyr/apps/websdr
   west flash

   # The protocol on the native USB port instead
   west build -p -b esp32c6_devkitc/esp32c6/hpcore esp-sdr-zephyr/apps/websdr -- -DFILE_SUFFIX=usb

Open the viewer in a browser with Web Serial (Chrome, Edge) and connect to the board's port.
Only one program can hold a serial port: disconnect the viewer before using another client.

Testing
*******

On an ESP32-S3-DevKitC-1 clone (N16R8), against upstream's firmware on the same board:

- Every reply to the commands above, sent by a script, matched upstream byte for byte, except
  ``CAPS``, ``FREQ 1500`` (``ERR pll_range``) and the queries this app leaves out
  (``VERSION?``, ``TRANSPORT?``). IQ8 and IQ10 captures up to 16380 samples passed their CRC.
- Orientation: the 60th harmonic of the 40 MHz crystal, at 2400.000 MHz, appears at
  2400 MHz minus the LO for LOs from 2393 to 2408 MHz, in both formats, decoded as the
  viewer decodes them; 26 to 32 dB above the median bin, against 1 to 20 dB at the mirrored
  offset (the receiver's I/Q image).
- ESP-WebSDR connected over UART0 and, with ``-DFILE_SUFFIX=usb``, over USB-Serial-JTAG,
  and showed spectra.
- Over USB-Serial-JTAG (``-DFILE_SUFFIX=usb``) the same replies and orientation. Payloads that
  are a whole number of 64-byte packets (2048, 2560, 8192, 16384 bytes) stayed in the host until
  the next short packet before the app sent a zero-length packet after them; with it every size
  arrives at once, about 770 kB/s against about 200 kB/s on UART0 at 2 MBaud.

On a board with an ESP32-C5 (rev 1.0, 48 MHz crystal, 8 MB flash, no PSRAM), over UART0,
against upstream's firmware on the same board: the same replies but for ``CAPS`` and the gain
maximum, CRC-clean IQ8 and IQ10 captures, and the 2400.000 MHz marker (the 50th harmonic of
the crystal) at 2400 MHz minus the LO for LOs from 2393 to 2408 MHz in both formats, 34 to
37 dB above the median bin against 5 to 21 dB at the mirrored offset. ESP-WebSDR connected
and showed spectra at 2.4 and 5 GHz.

On an ESP32-C6-WROOM-1 board (40 MHz crystal) over UART0, against upstream's firmware on the
same board: the same replies but for ``CAPS`` and ``FREQ`` outside the PLL's window, CRC-clean
captures at 80 MS/s, and the 2400.000 MHz marker (the 60th harmonic) at 2400 MHz minus the LO
for LOs from 2393 to 2408 MHz in both formats, 54 to 61 dB above the median bin against 27 to
46 dB at the mirrored offset.

On the same ESP32-C6 and ESP32-C5 boards over USB-Serial-JTAG (``-DFILE_SUFFIX=usb``): the same
replies as over UART0, payloads of whole 64-byte packets included, and the same orientation;
ESP-WebSDR connected and showed spectra on both.
