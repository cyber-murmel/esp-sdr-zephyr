.. SPDX-License-Identifier: GPL-3.0-or-later

.. _esp_sdr_rfprobe:

#######
rfprobe
#######

.. nav-start

Previous: `capture <../capture/README.rst>`__ · Up: `Applications <../../doc/apps/index.md>`__ · Next: `stream <../stream/README.rst>`__

.. nav-end

A radio bench probe for the ESP32-S3, the ESP32-C6, the ESP32-C5 and the ESP32-S31.

Overview
********

A bench tool for the radio itself. The firmware only moves data; ``tools/rfprobe.py`` does the
analysis on the host.

.. list-table::
   :header-rows: 1

   * - Chips
     - Functions
   * - all
     - tune through the library's frequency plan or program the PLL directly, dump raw captures,
       read the analog I2C registers
   * - ESP32-S3
     - also loop a tone
   * - ESP32-C5
     - also the diagnostic-bus ring and the transmit paths
   * - ESP32-S31
     - also the gapless ring and the transmit paths

It found the usable LO range (`Tuning <../../doc/radio/tuning.md>`__), the VCO window comparator
and the fractional-N divider words (`ESP32-S3 registers <../../doc/dev/registers-esp32s3.md>`__,
`ESP32-C6 registers <../../doc/dev/registers-esp32c6.md>`__) and the C6's stale modem state
(`Chip notes <../../doc/dev/chips.md>`__).

Hardware
********

.. list-table::
   :header-rows: 1

   * - Chip
     - Boards
   * - ESP32-C6
     - XIAO ESP32C6
   * - ESP32-C5
     - Waveshare ESP32-C5-Zero
   * - ESP32-S3
     - XIAO ESP32S3, or a generic ESP32-S3 devkit
   * - ESP32-S31
     - ESP32-S31-Function-CoreBoard-1 (also inside `multiapp <../multiapp/README.rst>`__ there),
       the VIEWE display board

- Every filter is open at boot (receive and, on the S3, transmit low-pass code 0).
- Two-board measurements need the boards' RF ports joined, as for ``tests/regression/lo_tuning``,
  which runs this app (`Testing <../../doc/dev/testing.md>`__).

Building and flashing
*********************

.. code-block:: console

   # ESP32-C6: console on USB-Serial-JTAG
   west build -d build/rfprobe-c6 -b xiao_esp32c6/esp32c6/hpcore esp-sdr-zephyr/apps/rfprobe
   west flash -d build/rfprobe-c6

   # ESP32-C5 (Waveshare ESP32-C5-Zero): console on USB-Serial-JTAG, with ESP_SDR_DIAG and
   # ESP_SDR_REPLAY (boards/waveshare_esp32c5_zero_hpcore.conf)
   west build -d build/rfprobe-c5 -b waveshare_esp32c5_zero/esp32c5/hpcore esp-sdr-zephyr/apps/rfprobe
   west flash -d build/rfprobe-c5

   # ESP32-S3: CDC-ACM shell and DFU through MCUboot, like the other S3 apps
   west build -d build/rfprobe-s3 --sysbuild -b xiao_esp32s3/esp32s3/procpu esp-sdr-zephyr/apps/rfprobe
   west flash -d build/rfprobe-s3    # first install; later: west dfu -d build/rfprobe-s3

ESP32-S3
========

- The S3 build runs on one core without PSRAM; its USB product string is ``ESP-SDR probe``.
- A generic ESP32-S3 devkit takes the same build with ``-b esp32s3_devkitc/esp32s3/procpu``.
- The devkit's first install goes through esptool on the USB-Serial-JTAG port
  (`Applications <../../doc/apps/index.md>`__, "Recovery with esptool").
- ``west dfu`` works from a clean boot. After a shell session the probe's USB device stops
  answering string descriptor reads: dfu-util then cannot match the serial and the update fails
  with "no DFU mode".
- So reset the board first (a devkit's UART bridge does it through RTS).

Usage
*****

Host tool
=========

``tools/rfprobe.py`` (pyserial, numpy) talks to the shells.

- A USB-Serial-JTAG port (the C6) is reset cleanly on open: DTR and RTS low, then a pulse on RTS
  alone. A default open can leave the chip in the ROM bootloader, silent.
- The S3's CDC-ACM port is opened without a reset.

.. code-block:: console

   C6=/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<MAC>-if00
   S3=/dev/serial/by-id/usb-Zephyr_Project_ESP-SDR_probe_<serial>-if00

   # tones at LO pairs: each LO, and 10 MHz apart both ways round
   tools/rfprobe.py sweep --tx $S3 --rx $C6 --range 1800:2800:10 --out sweep.jsonl
   tools/rfprobe.py summarize sweep.jsonl

   # the same with the PLLs programmed directly, divider on (base below 2210 MHz)
   tools/rfprobe.py raw --tx $S3 --rx $C6 --base 1900 --range 2280:2100:10

   # capacitor code and VCO window comparator per PLL frequency
   tools/rfprobe.py pll --port $C6 --freqs 2100:2200:2,2412,2800:2920:4

   # analog registers before and after something, and what changed
   tools/rfprobe.py dump --port $C6 --out before.json
   tools/rfprobe.py dump --port $C6 --out after.json
   tools/rfprobe.py diff before.json after.json

   # receive level with nothing transmitted (rms about 2 at gain 40 on a clean C6)
   tools/rfprobe.py floor --port $C6 --freqs 2412,2413,1900

When a sweep passes a pair, and why it also tests cross pairs, is in
`Testing <../../doc/dev/testing.md>`__ (``lo_tuning``).

Shell
=====

Tuning, filters and gain
------------------------

.. list-table::
   :header-rows: 1

   * - Command
     - Effect
   * - ``probe f <MHz>``
     - tune through the library's plan; prints ``esp_sdr_set_freq()``'s return (-34: -ERANGE, the
       VCO left its window)
   * - ``probe raw <PLL MHz> [kHz]``
     - program the PLL directly and stop retuning around captures and transmissions; the LO
       divider stays as the last ``probe f`` left it (on below 2210 MHz: LO = PLL / 1.2)
   * - ``probe lpf <rx> [txa [txb]]``
     - low-pass codes, -1 the PHY's, 0 the widest
   * - ``probe cbw <0..2>``
     - the PHY's channel bandwidth mode, applied by retuning: 0 20 MHz, 2 40 MHz (the analog
       filters open to about +-18 MHz)
   * - ``probe gain <rx|-1> [tx]``
     - fixed receive gain index or AGC; transmit step (S3)

Tone, captures and registers
----------------------------

.. list-table::
   :header-rows: 1

   * - Command
     - Effect
   * - ``probe tone on <Hz> [amp] [rate 0|1]``
     - S3: loop a tone at the LO + Hz (80 or 40 MS/s DAC), gapless, until ``probe tone off``; the
       radio is held meanwhile
   * - ``probe tone off``
     - stop the tone
   * - ``probe rx [n] [rate 0..6]``
     - one capture, printed as base64 lines (``d ...``) of the raw little-endian words, then
       ``end`` (S3: 0, 1, 6; C6: 0 only, 80 MS/s; C5 and S31: 0 to 5, 80 to 4 MS/s)
   * - ``probe dump <block> [regs]``
     - analog I2C registers 0..regs-1 of one block (read only)

Crystal calibration
-------------------

How the measurement works is in `Crystal calibration <../../doc/radio/crystal-calibration.md>`__.

.. list-table::
   :header-rows: 1

   * - Command
     - Effect
   * - ``probe cal lte [start MHz] [stop MHz] [gain|-1]``
     - measure the crystal's error against an LTE cell (default band 1 downlink, 2110 to
       2170 MHz), apply and store it with the die temperature; minutes. Where carriers fill the
       band nothing stands out of it: start = stop at a known carrier's centre searches only there
   * - ``probe cal show``
     - correction in force, where it leaves the LO (whole kHz at the PLL), temperature, the stored
       record, its temperature points and what the model gives now
   * - ``probe cal set <ppb>``
     - apply and store a constant correction (drops the points)
   * - ``probe cal clear``
     - correction 0, nothing stored
   * - ``probe temp``
     - the die temperature

ESP32-S31 ring
--------------

.. list-table::
   :header-rows: 1

   * - Command
     - Effect
   * - ``probe ring <MS/s> <s> [edge] [nodes] [pad] [pad] [dc]``
     - the gapless ring: its numbers, large samples in one node of 64, the DC servo to choose,
       then a dump of some nodes in base64, bytes, I then Q. A pad of -1 keeps the board's pads;
       a pad number chooses two lines' pads, the others as on the Function CoreBoard
   * - ``probe dc``
     - the DC servo at the fixed gain

ESP32-C5 diagnostic-bus ring
----------------------------

.. list-table::
   :header-rows: 1

   * - Command
     - Effect
   * - ``probe diag level``
     - the nibble window (lsb) the signal needs
   * - ``probe diag run <s> [div] [lsb|-1] [edge] [R]``
     - the gapless diagnostic-bus ring for s seconds: byte rate, skips, ppm against the system
       timer, sink cost
   * - ``probe diag snap [lsb|-1] [bytes] [edge]``
     - the first ring bytes (Q nibble low, I nibble high), base64
   * - ``probe diag iq <R> [pairs] [lsb|-1]``
     - box16 and CIC output (2.5 MS/s / R), base64
   * - ``probe diag poke [<addr> <clear> <set>]``
     - a register change applied once the dump engine runs (bench ablation); without arguments
       clears the list
   * - ``probe diag regs``
     - 0x600a9000, 0x600a9400 and 0x600a9c00 (64 words each) as the last run had them right
       after the pokes

Transmit (ESP32-C5, ESP32-S31)
------------------------------

.. list-table::
   :header-rows: 1

   * - Command
     - Effect
   * - ``probe tx replay <Hz> [amp] [rate 0|1|2] [passes] [n] [power] [extra] [scale]``
     - a whole-cycle tone played by the DAC engine (80 or 40 MS/s, on the S31 also 20); passes 0
       loops until ``probe tx stop``, otherwise prints the cycles the passes took and holds the
       radio for ``probe tx rearm``
   * - ``probe tx rearm <ms>``
     - restart the finite replay back to back for ms (restart hole)
   * - ``probe tx dc <rate> <Hz> [ms] [amp] [interp] [power]``
     - the paced TX DC path fed from a tone table on the chip: rate kept, lateness (the table
       takes about a second to build before the tone starts)
   * - ``probe tx dcraw <rate> <Hz> [ms] [amp]``
     - the TX DC register from a tight loop: the path's rate ceiling
   * - ``probe tx carrier <i> <q> [rx]``
     - a constant TX DC value until ``probe tx stop``; rx bits turn receive parts back on
       (1 power, 2 clock, 4 pbus work mode)
   * - ``probe tx cost <reg> [n]``
     - CPU cycles per store and load of a register, the paced loop's parts
   * - ``probe tx gap <reg|0> <ms> [chain] [iram]``
     - the longest gaps between cycle-counter reads in a loop storing to reg (0: none), with the
       transmit chain on (chain 1), from IRAM (iram 1) or flash: where a paced loop's stalls come
       from
   * - ``probe tx stop``
     - stop a replay, carrier or painting

Waterfall painting
------------------

The C5 and the S31 paint an image into a receiver's waterfall through the transmitter:

.. list-table::
   :header-rows: 1

   * - Command
     - Effect
   * - ``probe tx paint size <w> <h>``
     - a blank image for a receiver's waterfall, up to 256 x 128
   * - ``probe tx paint row <r> <hex>``
     - row r's pixels as hex digits, the leftmost in the most significant bit
   * - ``probe tx paint run <row ms> [amp] [rate 0|1|2] [n] [scale] [gap ms]``
     - play the image until ``probe tx stop``: a row at a time, the bottom row first (a waterfall
       scrolling down shows it upright), every set pixel a tone, column j at (j - w / 2) x rate / n
       (125 kHz for n 320 at 40 MS/s, 3125 Hz for n 12800); the transmitter stays off for gap ms
       after each pass
   * - ``probe tx paint state``
     - rows played, the longest row change (transmitter off meanwhile)

Sleep (ESP32-C6)
----------------

``probe sleep <ms>`` puts the C6 into deep sleep with a timer wakeup (USB drops meanwhile).

Limitations
***********

The ESP32-C5-Zero has no SRAM for the calibration's work memory or the settings volume: no
``probe cal lte`` there, and ``probe cal set`` holds for the session.

See also
********

- `Tuning <../../doc/radio/tuning.md>`__: the PLL, the divider and the LO range this app measures.
- `Crystal calibration <../../doc/radio/crystal-calibration.md>`__: the LTE measurement behind
  ``probe cal``.
- `Testing <../../doc/dev/testing.md>`__: the ``lo_tuning`` regression test.
- `ESP32-S3 registers <../../doc/dev/registers-esp32s3.md>`__,
  `ESP32-C6 registers <../../doc/dev/registers-esp32c6.md>`__.
- `Applications <../../doc/apps/index.md>`__: DFU updates and recovery.

.. nav-start

Previous: `capture <../capture/README.rst>`__ · Up: `Applications <../../doc/apps/index.md>`__ · Next: `stream <../stream/README.rst>`__

.. nav-end
