.. SPDX-License-Identifier: GPL-3.0-or-later

.. _esp_sdr_multiapp:

########
multiapp
########

.. nav-start

Previous: `pcap internals <../../doc/apps/pcap-internals.md>`__ · Up: `Applications <../../doc/apps/index.md>`__ · Next: `multiapp internals <../../doc/apps/multiapp-internals.md>`__

.. nav-end

One firmware that runs the others, on the ESP32-S3 and the ESP32-S31.

Overview
********

One image runs the other apps as Zephyr loadable extensions (llext), with the USB classes they
need living in the image itself. How it does that is in
`multiapp internals <../../doc/apps/multiapp-internals.md>`__.

- ``capture``, ``rfprobe``, ``wpan``, ``stream``, ``osmosdr``, ``link`` and ``micropython`` run
  as extensions, built from their own sources.
- They load from their files on a FAT volume in the flash, with copies built into the image as
  the fallback (`Apps as files`_).
- ``hello`` (``ext/hello.c``) is the loader check; ``hello atomic`` tests atomics in PSRAM from
  both CPUs.
- An extension's code and data are loaded into PSRAM and run from there.
- One app runs at a time. ``multiapp run`` stops the running app, unloads it and the modules
  the next app does not need (the Wi-Fi driver, the SDR) and starts the next in place, when the
  two are compatible; otherwise, and when anything refuses to stop, it reboots into it
  (``multiapp-internals``, Unloading).
- USB enumerates once per boot: the classes an app needs besides the CDC-ACM shell and DFU come with
  a reboot into it, so an app with another USB profile or one that takes the console (MicroPython)
  is a reboot.

Hardware
********

.. list-table::
   :header-rows: 1

   * - Board
     - Chip
     - Notes
   * - XIAO ESP32S3
     - ESP32-S3
     - see `MicroPython`_
   * - Hackaday Communicator
     - ESP32-S3
     - see `MicroPython`_
   * - ESP32-S31-Function-CoreBoard-1
     - ESP32-S31
     - see `ESP32-S31`_

ESP32-S31
=========

- On the ESP32-S31-Function-CoreBoard-1 the host boots into ``osmosdr_hs``, the SDR on the
  board's USB 2.0 HS port J4 (``CONFIG_MULTIAPP_AUTOSTART`` in the board's configuration file).
- ``hello``, ``capture`` and ``rfprobe`` are the other apps, and the radio is one module, ``sdr``
  (`Modules`_). ``multiapp run`` switches among them in place; to and from ``osmosdr_hs``, whose
  USB profile differs, it reboots.
  Built, not yet run on a board.
- J4's VBUS is the board's own: connect it with VBUS cut, as
  `osmosdr_hs <../osmosdr_hs/README.rst>`__ explains.
- The shell is on a CDC-ACM port of J4; the console and the log stay on the UART port.
- Flash (16 MB, ``boards/partitions_s31.dtsi``): MCUboot and two 3840 KiB slots in the lower 8 MB
  (Espressif's table), a 16 KiB ``calibration`` partition at 0x7e0000 for the crystal calibration
  (settings on ZMS), and the FAT volume in the upper 8 MB at 0x800000.
- The extensions are RISC-V relocatable objects and their code runs where llext writes it, in
  PSRAM; ``osmosdr_hs`` keeps its USB batches in internal RAM
  (`multiapp internals <../../doc/apps/multiapp-internals.md>`__).
- The CoreBoard is a network host (Ethernet, mDNS) and answers MCUmgr (Simple Management Protocol) over UDP on port 4996 after
  ``mcumgr arm <minutes>`` on its shell: ``west mcumgr`` uploads signed images and files. The images are signed
  with the project's own key, not MCUboot's: `doc/dev/network-services.md <../../doc/dev/network-services.md>`__.
- It also runs an SSH server (port 22, public key login with ed25519 or ECDSA keys, the Zephyr shell on the session, ``ssh <host> '<command>'``): ``ssh hostkey`` prints
  the host key's fingerprint, ``ssh key add "<authorized_keys line>"`` lets a key in, and ``mcumgr arm`` works from the session
  (`phase 3 <../../doc/dev/network-services.md>`__).
- Host tools and the SoapySDR module find its stream as they find the standalone app's
  (``esdr_usb -s <serial> info``).

Building and flashing
*********************

How ``west dfu`` and esptool recovery work is in `Applications <../../doc/apps/index.md>`__,
"DFU updates".

XIAO ESP32S3
============

.. code-block:: console

   west build -d build/multiapp --sysbuild -b xiao_esp32s3/esp32s3/procpu esp-sdr-zephyr/apps/multiapp
   west flash -d build/multiapp            # first install
   west dfu --storage -d build/multiapp    # the apps' files: the FAT volume

- Later, ``west dfu -d build/multiapp`` updates the image and ``west dfu --storage`` the volume.
- A new image needs its volume too (`Apps as files`_).
- ``west dfu --esptool [--storage]`` writes MCUboot, the image (and the volume) with esptool,
  without the boot button: the running app hands the USB port to the USB-Serial-JTAG first (shell
  command ``usj``).

ESP32-S31-Function-CoreBoard-1
==============================

.. code-block:: console

   west build -d build/multiapp-s31 --sysbuild -b esp32s31_function_coreboard/esp32s31/hpcore esp-sdr-zephyr/apps/multiapp
   west flash -d build/multiapp-s31 --esp-device /dev/serial/by-id/usb-Silicon_Labs_CP2102N_*   # first install, UART
   west dfu --storage -d build/multiapp-s31    # the apps' files, over J4

Later updates go over J4 with ``west dfu``, as on the S3 (``west dfu --esptool`` is the S3's).

The S31 image is signed with the project's own ED25519 key, ``~/.config/esp-sdr/keys/esdr-mcuboot-ed25519.pem``
(or the file in ``ESDR_SIGNING_KEY``): without it the configure step stops and says how to make one. For a build
that is not flashed (CI), ``ESDR_DEV_KEY=1`` in the environment uses MCUboot's public development key instead and
warns (`doc/dev/network-services.md <../../doc/dev/network-services.md>`__, "The signing key").

Usage
*****

Shell
=====

Example session
---------------

On the CDC-ACM shell:

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
   rfprobe: rebooting into it (unmount the drive on the PC first, if it wrote to it)
   (with an app of the same USB profile loaded, or none: "rfprobe: started", no reboot)

and, on the shell of the boot into it:

.. code-block:: none

   uart:~$ multiapp info
   host 0.1.0, build 8a61693d
   boot: rfprobe, start 0, USB profile 0x4
   rfprobe (file): text 3002 at 0x42167c5c, data 448, bss 4, rodata 872, 6067 bytes in heaps
     command probe
   uart:~$ probe f 2437
   f 2437 ret 0

Commands
--------

.. list-table::
   :header-rows: 1

   * - Command
     - Effect
   * - ``multiapp list``
     - each app's file next to the built-in copy (``differs`` when they are not the same)
   * - ``multiapp run <name>``
     - starts the app. With another app loaded it stops and unloads that one and the modules the new
       app leaves out, then loads the new one, without a reboot, if both have the same USB profile
       (the drive is in all but ``stream``'s), neither takes the console, and the running app ends
       (its ``main()`` returns or its ``multiapp_app_stop()`` makes it). Otherwise it reboots into
       the app. An error after the unload began reboots too. The shell stays up for an in-place
       switch.
   * - ``multiapp info``
     - the host's build id, the stack its ``main()`` used, the loaded app's origin, sizes and
       commands, the uptime, the area's owner, and a line per heap (module_heap, module_high_heap,
       bank2_heap in a BANK2 boot, psram_code and psram_data, or psram_ext on the S31, kernel):
       free, largest free block, used, peak. A soak watches these and the uptime, which only grows
       without a reboot
   * - ``multiapp stage list|put|data|end|load|revert``
     - hot-swaps a stage of the running app for a kernel (see below)
   * - ``multiapp crash``
     - what ended the previous boot (fatal error, watchdog, how far a boot into an app got) and
       the USB thread's stack use in the last DFU download

- The app's root commands (``probe`` here) answer as in the standalone app.
- ``CONFIG_MULTIAPP_AUTOSTART`` names an app to start at every boot.

Hot-swapping a stage
====================

A development feature (``CONFIG_MULTIAPP_STAGES``): replace one DSP function of the app that is
running with a small kernel, without a reboot. An app declares a stage
(``include/multiapp/stage.h``: ``rfprobe``'s ``ring_sink`` on the ESP32-S31, ``hello``'s
``hello_op`` on both chips), a kernel is built by ``multiapp_kernel()`` and loaded by the shell:

.. code-block:: console

   $ python3 apps/multiapp/scripts/kswap.py --port /dev/ttyACM0 --stage ring_sink \
         --build build/<name> --target sink_scan16        # builds the kernel, pushes it, swaps
   uart:~$ probe ring 20 1                                # runs the kernel's sink
   uart:~$ multiapp stage list                            # which stage runs what, where
   uart:~$ multiapp stage load ring_sink sink_scan16      # a file of the volume instead (S31)
   uart:~$ multiapp stage revert ring_sink                # the app's own sink again

- The swap happens at the app's quiescent point, or at once when the app is idle, and takes
  microseconds; the shell prints the latency, the kernel's size and where it was loaded. If the
  app never gets there in 2 s (``CONFIG_MULTIAPP_STAGE_TIMEOUT_MS``, or the last argument) the
  kernel is unloaded and nothing changed.
- A kernel imports only what the host exports already or the app defines: the build says
  otherwise. Changing a kernel never changes the host's build id, so no file goes stale.
- ``put``/``data``/``end`` upload the file as base64 lines over the shell (UART or CDC-ACM);
  ``kswap.py`` paces them. ``load`` reads a file of the volume, on both chips (the PC's
  drive is hidden meanwhile, `Copying over USB`_).
- ``CONFIG_MULTIAPP_KERNELS_IN_VOLUME`` also puts the kernels in the FAT image.
- On the ESP32-S3 the 802.15.4 driver's waveform generator ``tx_gen`` is a stage too, of the
  ``ieee154`` module (``apps/wpan/kernels``: ``tx_gen_ref``, the same algorithm, and ``tx_gen_alt``,
  one chip inverted on purpose). Swap with the radio off (``wpan off``); ``wpan bench ack`` then
  prints how many words differ from the built-in generator (0 for ``tx_gen_ref``).
- Details, the protocol and what it guarantees:
  `multiapp internals <../../doc/apps/multiapp-internals.md>`__.

Apps as files
=============

The volume
----------

- The upper 4 MB of the flash hold a FAT volume with one ``<name>.llext`` per app
  (``src/drive.c``).
- Every boot reads it before USB starts.
- The boot's app is loaded from its file when the file is good, else from the built-in copy, if
  it has one.
- "Good" is checked before llext sees the file, from a trailer the build appends: size, CRC-32,
  the stack the app needs, and the host's build id, so only files from the same build load. The
  trailer is described in `multiapp internals <../../doc/apps/multiapp-internals.md>`__.
- ``CONFIG_MULTIAPP_EMBEDDED=n`` drops the built-in copies (165 KB of flash; multiapp internals
  gives a different size); a boot then runs an app only from its file.

Copying over USB
----------------

- The PC sees the volume as a USB drive next to the shell in every boot, an app running or not,
  except ``stream``'s: its NCM network function takes the S3's last IN endpoint, so copy in a boot
  of another app. On the S31 ``osmosdr_hs``'s stream and the drive share the controller
  (``g-tx-fifo-size`` in the board's overlay).
- Copy files from ``build/multiapp/multiapp/llext/files/`` (apps) or the build's kernel files onto
  it, ``sync``, then ``udisksctl unmount -b /dev/sdX``. Do not ``eject``: Zephyr's mass storage then
  reports no medium until the next enumeration.
- Then ``multiapp run`` the app or ``multiapp stage load <stage> <file>``. The device mounts the
  volume afresh for the read and reads what the PC wrote. The drive stays: the PC's accesses wait
  for the read, then go on, so a copy in progress is delayed, not cut. A file the PC is still
  copying is read once more after 200 ms if it fails its CRC; a PC that copies without a pause
  starves the read, which gives up with ``-EBUSY`` after 2 s.
- The device never writes the volume: the S31's crystal calibration is in a partition of its own
  (``calibration``, settings on ZMS), so a calibration over USB does not disturb a PC that has the
  drive mounted. A device writer that did (``multiapp_drive_write_begin()``) would hide the drive
  for at least 3 s, and while a ring runs until it stops.
- Writes are slow, about 60 kB/s: every 4 KiB flash page is erased and rewritten, and while it
  is, the flash cache is off for both CPUs (a running ring may restart).
- ``west dfu --storage`` writes the whole volume (``mkfat.py`` builds its image); unmount first.

MicroPython
===========

MicroPython is the app ``micropython``, built by ``micropython.cmake``, with no built-in copy. It
takes the CDC-ACM port from the shell for the boot, so the port is its REPL, and the host's
commands go through ``esdr.shell()``:

.. code-block:: console

   uart:~$ multiapp run micropython
   >>> import esdr
   >>> print(esdr.shell("multiapp info"))
   >>> esdr.shell("multiapp run wpan")

- ``machine.reset()`` also leaves it, for a boot without an app or, with the autostart, a new
  start.
- ``west dfu`` works from the REPL as from the shell.
- ``/FLASH:`` is the FAT volume, so a script there sits next to the apps' files, and ``west dfu
  --storage`` replaces the whole volume, scripts included.
- The Python heap is the PSRAM the boot leaves free, about 4.7 MB.

Boards
------

- It reads the same board fragment as the standalone app (``apps/micropython/boards``).
- On the XIAO it has the display, LVGL, the knob and the SD card, all of them in its file: the
  launcher comes up on the panel as in the standalone app.
- ``/SD:`` is the card, and the PC sees two drives, the volume and the card, of which a boot
  without an app offers only the volume. MicroPython registers the proxies of both
  (``usb_disks.c``) and the host's own proxy for the volume stays out of that boot.
- A restart comes up without an app, with the panel dark: ``multiapp run micropython`` starts it,
  or ``CONFIG_MULTIAPP_AUTOSTART`` at every boot, which the Communicator sets.
- On the Communicator it has the display and the keyboard, and no SD card.
- A board without a display builds it headless, with ``manifest_headless.py``.
- SD writes have not been checked on this image for want of a working card
  (`multiapp internals <../../doc/apps/multiapp-internals.md>`__).

Wi-Fi driver module
===================

The stock Wi-Fi driver is the module ``wifi``, built by ``wifi.cmake``, and has no built-in copy
(``FILE_ONLY``), so its file must be on the volume. The app ``wifitest`` uses it:

.. code-block:: console

   uart:~$ multiapp run wifitest
   uart:~$ wifi scan
   uart:~$ wifi connect <ssid> [psk]
   uart:~$ wifi status
   uart:~$ net ping <address>

- ``wifi connect`` returns once DHCP has an address.
- After an abrupt reset the access point may still have the board associated, and the first
  attempt fails: try again.
- The module's PHY is the ``phy`` module's, shared with ``sdr`` through the radio arbiter
  (``esp_sdr_arb.c``, in ``phy``), so a Wi-Fi boot loads ``phy`` too.
- ``wifi`` needs the area for its heap: an app with ``AREA``, and ``ieee154``, cannot load with
  it (a CMake error).

Wi-Fi and the SDR: coex
-----------------------

``coex`` is ``wifitest``'s commands plus the SDR in the windows of Wi-Fi's sleep
(`Coexistence <../../doc/radio/coexistence.md>`__). It needs ``wifi`` and ``sdr``, which together
want about 43 KB of internal RAM: ``sdr`` loads into dump bank 2 (``BANK2``), so the ring runs on
banks 0 and 1 and there are no burst captures (`multiapp internals
<../../doc/apps/multiapp-internals.md>`__). ``wifi`` has no
built-in copy, so ``wifi.llext`` must be on the volume.

.. code-block:: console

   uart:~$ multiapp run coex
   uart:~$ wifi connect <ssid> <psk>     # typed here, never stored
   uart:~$ coex ps min                   # none | min | max modem sleep
   uart:~$ coex sdr 30 ring 2437         # <seconds> [ring] [MHz] [bg]
   uart:~$ coex sdr result               # after "bg", which returns at once
   uart:~$ coex arb                      # the arbiter's counters, "coex arb clear"
   uart:~$ coex chan
   uart:~$ net ping <gateway>

- ``coex sdr`` runs the window loop on CPU 1: claim, a ring to the deadline, release. It prints the windows, the SDR's duty, the ring runs that ended at
  the window against failures, and the gaps the sink counted inside runs.
- Without power save (``coex ps none``) Wi-Fi never sleeps and every claim times out with
  ``-EBUSY``.
- The other boots are unchanged: without ``wifi``, ``esp_sdr_init()`` keeps a standing claim, so
  ``osmosdr``, ``wpan``, ``capture`` and the rest need no claims.

ESP-NOW: espnow
---------------

The module ``espnow`` (``espnow.o`` of the Wi-Fi blobs, an API in ``include/esp_sdr/espnow.h`` and
the ``espnow`` shell, the same commands as ``apps/coexbench``) needs ``wifi``, and is in PSRAM
only. It has no built-in copy, so ``espnow.llext`` must be on the volume. ``coex`` loads it, and
``espnowtest`` loads it alone, without joining an access point:

.. code-block:: console

   uart:~$ multiapp run espnowtest       # or coex
   uart:~$ espnow init                   # station mode if needed; prints the own MAC
   uart:~$ espnow peer <aa:bb:cc:dd:ee:ff>
   uart:~$ espnow send 100 20 32         # <count> <interval_ms> [bytes]
   uart:~$ espnow stats [clear]
   uart:~$ espnow rx 4                   # the last frames received
   uart:~$ espnow window 20 100          # awake 20 ms of every 100 ms, for receiving under power save

- Both boards must be on the same channel; the peer is on the current one.
- In ``coex``, receiving needs the wake window while ``coex ps`` is ``min`` or ``max``, and the
  SDR's ring windows then fall between the wake windows.

Adding an app
=============

Extensions
----------

One line in ``CMakeLists.txt``:

.. code-block:: cmake

   multiapp_extension(rfprobe STACK 2048 SOURCES ${apps_dir}/rfprobe/src/main.c)

- The build puts ``include/multiapp/ext.h`` in front of every source: shell commands,
  ``SYS_INIT``, log modules, memory slabs and net_buf pools become things an extension can do.
- It exports from the host exactly what the extensions import (``exports.py``).
- ``APP <dir>`` gives the extension its app's own ``CONFIG_APP_*`` values (``appconfig.py``).
- ``USB`` names the classes it needs.
- ``AREA`` puts its internal RAM buffers (``ESP_SDR_HIGH_BUF_DEFINE()``) in the area.
- The host needs the app's other Kconfig options; a missing function fails the host's link.

Kernels
-------

A replacement for one stage of an app, loaded into it while it runs (`Hot-swapping a stage`_):

.. code-block:: cmake

   multiapp_kernel(sink_scan16 STAGE ring_sink APP rfprobe INTERNAL
     INCLUDES ${apps_dir}/rfprobe/src SOURCES ${apps_dir}/rfprobe/kernels/sink_scan16.c)

- ``APP`` is the name of the ``multiapp_extension()``, ``INTERNAL`` puts its IRAM code in
  ``module_heap``. The file is ``llext/files/<name>.llext``, build target ``<name>_kernel``.

Modules
-------

A module is code other extensions call, with no ``main()``, loaded before them:

.. code-block:: cmake

   multiapp_module(hellofast INTERNAL SOURCES ${CMAKE_CURRENT_LIST_DIR}/ext/hellofast.c)
   multiapp_module(hellolib REQUIRES hellofast SOURCES ${CMAKE_CURRENT_LIST_DIR}/ext/hellolib.c)
   multiapp_extension(hello REQUIRES hellolib SOURCES ${CMAKE_CURRENT_LIST_DIR}/ext/hello.c)

.. list-table::
   :header-rows: 1

   * - Option
     - Effect
   * - ``INTERNAL``
     - places it as a standalone image would: its ``IRAM_ATTR`` code, data and bss in internal RAM
       (``CONFIG_MULTIAPP_MODULE_HEAP_SIZE``), the rest of its code and its read-only data in
       PSRAM
   * - ``HIGH_BSS``
     - puts its bss in the high region (``CONFIG_MULTIAPP_MODULE_HIGH_HEAP_SIZE``)
   * - ``DEVICES``
     - says it defines Zephyr devices, which the host initializes as it loads the module and an
       app finds with ``device_get_binding()``
   * - ``KCONFIG``
     - gives a module Kconfig values the host lacks: a second Kconfig pass with that fragment on
       top of the host's, whose differences it compiles with (``extconfig.py``)

- A symbol that differs and is not listed in ``KCONFIG_OWN`` or ``KCONFIG_DROP`` fails the build,
  so module and host cannot disagree unnoticed.
- ``multiapp info`` shows each module's place and addresses and how much of those heaps is used.
- A module that does not load keeps the app from starting: the boot comes up as a boot without an
  app, with the drive.
- The radio is modules too, ``phy``, ``sdr`` and ``ieee154``, loaded only by the boots whose app
  names them (`multiapp internals <../../doc/apps/multiapp-internals.md>`__).

See also
********

- `multiapp internals <../../doc/apps/multiapp-internals.md>`__: how the host loads apps, its
  memory and its measurements.
- `Testing <../../doc/dev/testing.md>`__: the desk checks ``sample.esp_sdr.multiapp.*``.
- `micropython <../micropython/README.rst>`__, `osmosdr_hs <../osmosdr_hs/README.rst>`__.
- `Applications <../../doc/apps/index.md>`__: DFU updates and esptool recovery.

.. nav-start

Previous: `pcap internals <../../doc/apps/pcap-internals.md>`__ · Up: `Applications <../../doc/apps/index.md>`__ · Next: `multiapp internals <../../doc/apps/multiapp-internals.md>`__

.. nav-end
