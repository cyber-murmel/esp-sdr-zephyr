# Web flasher

<!-- nav-start -->
Previous: [Applications](index.md) · Up: [Applications](index.md) · Next: [capture](../../apps/capture/README.rst)
<!-- nav-end -->

The [web flasher](https://software.defined.rodeo/flasher/) writes an image of the
latest release onto a board from the browser, over Web Serial. It needs Chrome, Edge or Opera on
a desktop; elsewhere, download the image and use esptool.

## What it offers

- Every image the release has: every app on every board its `sample.yaml` allows, as
  `default.nix` builds them (`nix/release.py` turns them into release assets).
- Per board, `multiapp` first, then the board's osmosdr image (`osmosdr`, `osmosdr_serial`,
  `osmosdr_hs` or `osmosdr_bridge`) unless that board's multiapp carries it already, then the
  other apps.
- A board with two chips (the WHY2025 badge, the ESP32-P4-Function-EV-Board) lists each chip's
  images apart.
- The MCH2022 badge's RP2040 image is a `.uf2`, download only.

## How it writes

- Each image is a whole flash image: the bootloader or MCUboot, the app and multiapp's FAT
  volume, written at 0x0. MCUboot images carry the padded, confirmed app, so it is not on trial
  ([Test boot and revert](index.md#test-boot-and-revert)).
- The page checks the file's SHA-256 against `firmware.json`, the chip against the image's, and
  the written flash against the file's MD5.
- "Erase the whole flash first" also clears what the image does not cover, such as settings.
- At the end it resets the board through RTS (or the USB-Serial-JTAG); press reset where that
  does not reach the chip.

## Download mode

The ROM bootloader takes the image. The page finds out what answers on the picked port and gets
the board there by itself, in this order:

1. An esp-sdr app (USB vendor 2fe3): the apps own the S3's USB port, where no reset reaches the
   bootloader. The page sends Ctrl-C; a `>>>` means MicroPython (multiapp's autostart on the
   XIAO ESP32S3 and the Communicator), which gets `esdr.shell("download")`, else the shell gets
   `download` (the shell and the REPL as `west dfu` tells them apart). Both restart the S3 into
   the ROM's download mode on the USB-Serial-JTAG
   ([Handing the port over](index.md#handing-the-port-over)), which comes up as a new port;
   firmware without the command gets `usj` instead. The page goes on with the new port when the
   browser may use it already; the first time it asks for one more pick.
2. One short attempt without a reset, for a bootloader that waits already (BOOT held while
   plugging in or while pressing reset).
3. esptool's reset into download mode: the USB-Serial-JTAG's sequence on chips that have one,
   else RTS and DTR on the UART bridge. Opening the port may reset the chip out of download mode
   before step 2; this step brings it back.

When nothing answers, the page's hints for the board say what to do by hand; the boards' own
ways are in [Badges](index.md#badges).

## Where the images come from

GitHub's release downloads cannot be fetched from a page, so the documentation build
(`.github/workflows/docs.yml`) copies the latest release's images and its `firmware.json` into the
site, at `flasher/firmware/`. `release.yml` deploys the documentation again after each release.
The page is `doc/_extra/flasher/`; esptool-js comes from jsDelivr at a pinned version.

<!-- nav-start -->
Previous: [Applications](index.md) · Up: [Applications](index.md) · Next: [capture](../../apps/capture/README.rst)
<!-- nav-end -->
