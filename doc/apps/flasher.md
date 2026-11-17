# Web flasher

<!-- nav-start -->
Previous: [Applications](index.md) · Up: [Applications](index.md) · Next: [capture](../../apps/capture/README.rst)
<!-- nav-end -->

The [web flasher](https://cyber-murmel.github.io/esp-sdr-zephyr/flasher/) writes an image of the
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

The ROM bootloader takes the image. The page reaches it one of three ways:

- Automatic: esptool's reset, through the USB-Serial-JTAG on chips that have one, else RTS and
  DTR on the UART bridge.
- Already in download mode: for a board put there by hand (BOOT held while plugging in or while
  pressing reset), the page connects without touching RTS and DTR.
- From a running esp-sdr app: the apps own the S3's USB port (USB IDs 2fe3:0005), where no reset
  reaches the bootloader. Picking that port makes the page send the shell command `usj`, which
  hands the port to the USB-Serial-JTAG ([Handing the port over](index.md#handing-the-port-over));
  the second attempt then picks the new port. Where multiapp runs MicroPython (its autostart on
  the XIAO ESP32S3 and the Communicator), the port is the REPL: the page stops the running
  Python app with Ctrl-C and sends `esdr.shell("usj")` instead, as `west dfu` does.

The boards' own ways are in [Badges](index.md#badges) and the page's hints.

## Where the images come from

GitHub's release downloads cannot be fetched from a page, so the documentation build
(`.github/workflows/docs.yml`) copies the latest release's images and its `firmware.json` into the
site, at `flasher/firmware/`. `release.yml` deploys the documentation again after each release.
The page is `doc/_extra/flasher/`; esptool-js comes from jsDelivr at a pinned version.

<!-- nav-start -->
Previous: [Applications](index.md) · Up: [Applications](index.md) · Next: [capture](../../apps/capture/README.rst)
<!-- nav-end -->
