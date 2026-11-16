.. _esp_sdr_multiapp:

multiapp
########

one firmware that runs the others

Overview
********

One image that runs the other apps as Zephyr loadable extensions (llext,
``doc/multiapp.md``), with the USB classes they need living in the image
itself. ``capture``, ``rfprobe``, ``wpan``, ``stream``, ``osmosdr``, ``link`` and
``micropython`` run as extensions, built from their own sources and loaded from their
files on a FAT volume in the flash, with copies built into the image as the
fallback; ``hello`` (``ext/hello.c``) is the loader check (``hello atomic``
tests atomics in PSRAM from both CPUs).

An extension's code and data are loaded into PSRAM; the code runs at the
instruction bus alias of its PSRAM copy (``CONFIG_LLEXT_EXEC_ALIAS``, on the
Zephyr branch ``west.yml`` pins). One app runs per boot: an app cannot be
stopped cleanly, so starting another one reboots into it. USB enumerates once
per boot too: the classes an app needs besides the CDC-ACM shell and DFU
(``stream``'s CDC-NCM, osmosdr's vendor class) come with a reboot into it, as
does the internal RAM area that osmosdr and the 802.15.4 driver take turns on.

Building and flashing
*********************

.. code-block:: console

   west build -d build/multiapp --sysbuild -b xiao_esp32s3/esp32s3/procpu esp-sdr-zephyr/apps/multiapp
   west flash -d build/multiapp            # first install
   west dfu --storage -d build/multiapp    # the apps' files: the FAT volume

Later, ``west dfu -d build/multiapp`` updates the image and ``west dfu
--storage`` the volume; a new image needs its volume too (see "Apps as
files"). ``west dfu --esptool [--storage]`` writes MCUboot, the image (and the
volume) with esptool, without the boot button: the running app hands the USB
port to the USB-Serial-JTAG first (shell command ``usj``).

Then, on the CDC-ACM shell:

.. code-block:: none

   uart:~$ multiapp list
   hello          3448 bytes, file 3448 bytes
   capture        5124 bytes, file 5124 bytes
   rfprobe        8596 bytes, file 8596 bytes
   wpan          18216 bytes, file 18216 bytes
   stream    30264 bytes, file 30264 bytes
   osmosdr       17900 bytes, file 17900 bytes
   link          81320 bytes, file 81320 bytes
   uart:~$ multiapp run rfprobe
   rfprobe: rebooting into it (eject the drive first: the PC may not have written it out)

and, on the shell of the boot into it:

.. code-block:: none

   uart:~$ multiapp info
   host 0.1.0, build 8a61693d
   boot: rfprobe, start 0, USB profile 0x0
   rfprobe (file): text 3002 at 0x42167c5c, data 448, bss 4, rodata 872, 6067 bytes in heaps
     command probe
   uart:~$ probe f 2437
   f 2437 ret 0

The app's root commands (``probe`` here) answer as in the standalone app.
``multiapp run <name>`` reboots into the app unless no app runs yet and USB already
has its classes; ``CONFIG_MULTIAPP_AUTOSTART`` names an app to start at every
boot. ``multiapp info`` shows the host's build id, the stack its ``main()`` used and
the loaded app's origin, sizes and commands; ``multiapp crash`` what ended the
previous boot (fatal error, watchdog, how far a boot into an app got) and the
USB thread's stack use in the last DFU download.

On the ESP32-S31
****************

On the ESP32-S31-Function-CoreBoard-1 the host boots into ``osmosdr_hs``, the SDR on the board's
USB 2.0 HS port J4 (``CONFIG_MULTIAPP_AUTOSTART`` in the board's configuration file).
``hello``, ``capture`` and ``rfprobe`` are the other apps, and the radio is one module,
``sdr``. J4's VBUS is the board's own: connect it with VBUS cut
(``apps/osmosdr_hs/README.rst``). The shell is on a CDC-ACM port of J4; the console and the log
stay on the UART port.

.. code-block:: console

   west build -d build/multiapp-s31 --sysbuild -b esp32s31_function_coreboard/esp32s31/hpcore esp-sdr-zephyr/apps/multiapp
   west flash -d build/multiapp-s31 --esp-device /dev/serial/by-id/usb-Silicon_Labs_CP2102N_*   # first install, UART
   west dfu --storage -d build/multiapp-s31    # the apps' files, over J4

Later updates go over J4 with ``west dfu``, as on the S3 (``west dfu --esptool`` is the S3's).
The extensions are RISC-V relocatable objects and their code runs where llext writes it, in
PSRAM; ``osmosdr_hs`` keeps its USB batches in internal RAM (``doc/multiapp.md``, "The
ESP32-S31"). Host tools and the SoapySDR module find its stream as they find the standalone
app's (``esdr_usb -s <serial> info``).

Apps as files
*************

The upper 4 MB of the flash hold a FAT volume with one ``<name>.llext`` per
app (``src/drive.c``). Every boot reads it before USB starts: ``multiapp list``
shows each app's file next to the built-in copy (``differs`` when they are
not the same), and the boot's app is loaded from its file when the file is
good, else from the built-in copy, if it has one. "Good" is checked before llext sees the
file, from a trailer the build appends (``apptrail.py``): size, CRC-32, the
stack the app needs, and the host's build id (``buildid.py``), so only files
from the same build load. ``CONFIG_MULTIAPP_EMBEDDED=n`` drops the built-in
copies (165 KB of flash); a boot then runs an app only from its file.

In a boot without an app the PC sees the volume as a USB drive next to the
shell. Copy files from ``build/multiapp/multiapp/llext/files/`` onto it, eject
it (the PC may cache what it writes), then ``multiapp run`` the app. Writes are
slow, about 60 kB/s: every 4 KiB flash page is erased and rewritten. ``west
dfu --storage`` writes the whole volume (``mkfat.py`` builds its image).

Adding an app
*************

One line in ``CMakeLists.txt``:

.. code-block:: cmake

   multiapp_extension(rfprobe STACK 2048 SOURCES ${apps_dir}/rfprobe/src/main.c)

The build puts ``include/multiapp/ext.h`` in front of every source (shell
commands, ``SYS_INIT``, log modules, memory slabs and net_buf pools become
things an extension can do) and exports from the host exactly what the
extensions import (``exports.py``). ``APP <dir>`` gives the extension its app's
own ``CONFIG_APP_*`` values (``appconfig.py``), ``USB`` the classes it needs,
``AREA`` its internal RAM buffers (``ESP_SDR_HIGH_BUF_DEFINE()``) in the area.
The host needs the app's other Kconfig options; a missing function fails the
host's link.

A module is code other extensions call, with no ``main()``, loaded before them:

.. code-block:: cmake

   multiapp_module(hellofast INTERNAL SOURCES ${CMAKE_CURRENT_LIST_DIR}/ext/hellofast.c)
   multiapp_module(hellolib REQUIRES hellofast SOURCES ${CMAKE_CURRENT_LIST_DIR}/ext/hellolib.c)
   multiapp_extension(hello REQUIRES hellolib SOURCES ${CMAKE_CURRENT_LIST_DIR}/ext/hello.c)

``INTERNAL`` places it as a standalone image would: its ``IRAM_ATTR`` code, data
and bss in internal RAM (``CONFIG_MULTIAPP_MODULE_HEAP_SIZE``), the rest of its code
and its read-only data in PSRAM. ``HIGH_BSS`` puts its bss in the high region
(``CONFIG_MULTIAPP_MODULE_HIGH_HEAP_SIZE``). ``DEVICES`` says it defines Zephyr
devices, which the host initializes as it loads the module and an app finds with
``device_get_binding()``. ``multiapp info`` shows each module's place and addresses
and how much of those heaps is used. A module that does not load keeps the app from
starting: the boot comes up as a boot without an app, with the drive.

The radio is modules too: ``phy``, ``sdr`` and ``ieee154``. Only the boots whose app
names them load them, so a boot without an app brings no radio up.

The stock Wi-Fi driver is the module ``wifi``, built by ``wifi.cmake``, and has no
built-in copy (``FILE_ONLY``), so its file must be on the volume:

.. code-block:: console

   uart:~$ multiapp run wifitest
   uart:~$ wifi scan
   uart:~$ wifi connect <ssid> [psk]
   uart:~$ wifi status
   uart:~$ net ping <address>

``wifi connect`` returns once DHCP has an address. After an abrupt reset the access
point may still have the board associated, and the first attempt fails: try again.

MicroPython is the app ``micropython``, built by ``micropython.cmake``, also with no
built-in copy. It takes the CDC-ACM port from the shell for the boot, so the port is its
REPL, and the host's commands go through ``esdr.shell()``:

.. code-block:: console

   uart:~$ multiapp run micropython
   >>> import esdr
   >>> print(esdr.shell("multiapp info"))
   >>> esdr.shell("multiapp run wpan")

``machine.reset()`` also leaves it, for a boot without an app or, with the autostart, a new start. ``west dfu`` works from the
REPL as from the shell. ``/FLASH:`` is the FAT volume, so a script there sits next to the
apps' files, and ``west dfu --storage`` replaces the whole volume, scripts included. The
Python heap is the PSRAM the boot leaves free, about 4.7 MB.

On the XIAO it has the display, LVGL, the knob and the SD card, all of them in its file: the
launcher comes up on the panel as in the standalone app. ``/SD:`` is the card, and the PC sees
two drives, the volume and the card, of which a boot without an app offers only the volume. A
restart comes up without an app, with the panel dark: ``multiapp run micropython`` starts it, or
``CONFIG_MULTIAPP_AUTOSTART`` at every boot, which the Communicator sets. It
reads the same board fragment as the standalone app (``apps/micropython/boards``). On the Communicator
it has the display and the keyboard, and no SD card. A board without a display builds it headless, with
``manifest_headless.py``. SD writes have not been checked on
this image for want of a working card (``doc/multiapp.md``).

``KCONFIG`` gives a module Kconfig values the host lacks: a second Kconfig pass with
that fragment on top of the host's, whose differences it compiles with
(``extconfig.py``). A symbol that differs and is not listed in ``KCONFIG_OWN`` or
``KCONFIG_DROP`` fails the build, so module and host cannot disagree unnoticed.

Testing
*******

``sample.esp_sdr.multiapp.hil`` checks a board on the desk: the image and its
volume go on with ``scripts/twister-flash.py``, the board boots into ``hello``
from its file, and ``pytest/test_multiapp.py`` checks the files, the boot,
hello's commands and, through ``CONFIG_MULTIAPP_MODULE_TESTS``, calls across
modules in PSRAM and internal RAM, a device in a module, and calls into the PHY's
blobs as a module (``doc/testing.md``). ``sample.esp_sdr.multiapp.wifi.hil`` boots
into ``wifitest`` and scans, so it needs an access point in range.
``sample.esp_sdr.multiapp.micropython.hil`` boots into ``micropython`` from its file and checks it through
its REPL (``pytest/test_micropython.py``): the modules it loaded, its heap, the volume, a gapless ring, a
soft reset and, on a board with a display, a GUI app drawing with the ring gapless.
``sample.esp_sdr.multiapp.s31.hil`` checks the ESP32-S31 CoreBoard in its own configuration
(``pytest/test_multiapp_s31.py``): the files, the boot into ``osmosdr_hs`` from its file, the
radio module's placement and a clean reset.
