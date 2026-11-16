.. _esp_sdr_rfprobe:

rfprobe
#######

radio bench probe

Overview
********

A bench tool for the radio itself, on the ESP32-S3, the ESP32-C6 and the ESP32-C5: tune
through the library's frequency plan or program the PLL directly, loop a tone
(S3), dump raw captures and read the analog I2C registers; on the C5 also the
diagnostic-bus ring and the transmit paths. The firmware only
moves data; ``tools/rfprobe.py`` does the analysis on the host. It found the
usable LO range, the VCO window comparator, the fractional-N divider words and
the C6's stale modem state (``doc/undocumented-registers-esp32s3.md``,
``doc/undocumented-registers-esp32c6.md``, ``doc/hardware-quirks.md``).

Every filter is open at boot (receive and, on the S3, transmit low-pass code 0).
Two-board measurements need the boards' RF ports joined, as for
``tests/regression/lo_tuning`` (``doc/testing.md``).

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

The S3 build runs on one core without PSRAM; its USB product string is
``ESP-SDR probe``.

Shell
*****

.. code-block:: none

   probe f <MHz>               tune through the library's plan; prints esp_sdr_set_freq()'s
                               return (-34: -ERANGE, the VCO left its window)
   probe raw <PLL MHz> [kHz]   program the PLL directly and stop retuning around captures
                               and transmissions; the LO divider stays as the last
                               "probe f" left it (on below 2210 MHz: LO = PLL / 1.2)
   probe lpf <rx> [txa [txb]]  low-pass codes, -1 the PHY's, 0 the widest
   probe cbw <0..2>            the PHY's channel bandwidth mode, applied by retuning: 0 20 MHz,
                               2 40 MHz (the analog filters open to about +-18 MHz)
   probe gain <rx|-1> [tx]     fixed receive gain index or AGC; transmit step (S3)
   probe tone on <Hz> [amp] [rate 0|1]
                               S3: loop a tone at the LO + Hz (80 or 40 MS/s DAC), gapless,
                               until "probe tone off"; the radio is held meanwhile
   probe tone off
   probe rx [n] [rate 0..6]    one capture, printed as base64 lines ("d ...") of the raw
                               little-endian words, then "end" (S3: 0, 1, 6; C6: 0 only,
                               80 MS/s; C5: 0 to 5, 80 to 4 MS/s)
   probe dump <block> [regs]   analog I2C registers 0..regs-1 of one block (read only)

   C5 only:
   probe diag level            the nibble window (lsb) the signal needs
   probe diag run <s> [div] [lsb|-1] [edge] [R]
                               the gapless diagnostic-bus ring for s seconds: byte rate, skips,
                               ppm against the system timer, sink cost
   probe diag snap [lsb|-1] [bytes] [edge]
                               the first ring bytes (Q nibble low, I nibble high), base64
   probe diag iq <R> [pairs] [lsb|-1]
                               box16 and CIC output (2.5 MS/s / R), base64
   probe diag poke [<addr> <clear> <set>]
                               a register change applied once the dump engine runs (bench
                               ablation); without arguments clears the list
   probe diag regs             0x600a9000, 0x600a9400 and 0x600a9c00 (64 words each) as the
                               last run had them right after the pokes
   probe tx replay <Hz> [amp] [rate 0|1] [passes] [n] [power] [extra] [scale]
                               a whole-cycle tone played by the DAC engine (80 or 40 MS/s);
                               passes 0 loops until "probe tx stop", otherwise prints the
                               cycles the passes took and holds the radio for "probe tx rearm"
   probe tx rearm <ms>         restart the finite replay back to back for ms (restart hole)
   probe tx dc <rate> <Hz> [ms] [amp] [interp] [power]
                               the paced TX DC path fed from a tone table on the chip: rate
                               kept, lateness
   probe tx dcraw <rate> <Hz> [ms] [amp]
                               the TX DC register from a tight loop: the path's rate ceiling
   probe tx carrier <i> <q> [rx]
                               a constant TX DC value until "probe tx stop"; rx bits turn
                               receive parts back on (1 power, 2 clock, 4 pbus work mode)
   probe tx cost <reg> [n]     CPU cycles per store and load of a register, the paced loop's
                               parts
   probe tx paint size <w> <h> a blank image for a receiver's waterfall, up to 256 x 128
   probe tx paint row <r> <hex>
                               row r's pixels as hex digits, the leftmost in the most
                               significant bit
   probe tx paint run <row ms> [amp] [rate 0|1] [n] [scale] [gap ms]
                               play the image until "probe tx stop": a row at a time, the
                               bottom row first (a waterfall scrolling down shows it upright),
                               every set pixel a tone, column j at (j - w / 2) x rate / n (125
                               kHz for n 320 at 40 MS/s, 3125 Hz for n 12800); the transmitter
                               stays off for gap ms after each pass
   probe tx paint state        rows played, the longest row change (transmitter off meanwhile)
   probe tx stop               stop a replay, carrier or painting
   probe sleep <ms>            C6: deep sleep with a timer wakeup (USB drops meanwhile)

Host tool
*********

``tools/rfprobe.py`` (pyserial, numpy) talks to the shells. A USB-Serial-JTAG
port (the C6) is reset cleanly on open: DTR and RTS low, then a pulse on RTS
alone. A default open can leave the chip in the ROM bootloader, silent. The S3's
CDC-ACM port is opened without a reset.

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

A sweep passes a pair when the tone stands at least 20 dB above the local floor
(the median of the bins within 1 MHz; the receiver's floor is not flat) at the
offset the LOs predict, and is the strongest tone there within 150 kHz (the two
crystals plus an FFT bin). Equal LOs alone would miss two PLLs that land on the
same wrong frequency, hence the cross pairs.
