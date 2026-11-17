# Reference

<!-- nav-start -->
Previous: [MCH2022 badge](../apps/mch2022.md) · Up: [esp-sdr-zephyr](../index.rst) · Next: [Chip notes](chips.md)
<!-- nav-end -->

This is the fifth and last part of the docs. The first four parts teach the radio, its sample
paths, the modems and the apps in reading order. This part holds what you look up while working
on them: chip and board quirks, where code and data are placed, how the tree is tested, a plan
for a reference radio, and the register pages.

Each page stands on its own. The guides of Parts 1 to 4 link here for the details.

## Confidence levels

The register pages, and the guides that quote them, grade every statement by how sure it is:
measured, working, upstream, decompile or speculative. The grades and the evidence sources are
explained in [ESP32-S3 registers](registers-esp32s3.md#how-to-read-this-page);
[ESP32-C6 registers](registers-esp32c6.md#how-to-read-this-page) lists where the C6 differs.

## Pages in this part

1. [Chip notes](chips.md): CPU and toolchain costs, reset and boot quirks, board files and the
   VIEWE display, per chip.
2. [Code placement](code-placement.md): memory regions, usage categories and placement rules, and
   the tools that check and measure where code and data went.
3. [Testing](testing.md): the five test tiers from unit tests to smoke tests, CI and the git
   hooks.
4. [ZigBit reference](zigbit.md): a plan for an RF233 reference radio to check the software
   802.15.4 radio against.
5. [ESP32-S3 registers](registers-esp32s3.md): the undocumented S3 radio registers the library
   relies on, with fields and evidence.
6. [ESP32-C6 registers](registers-esp32c6.md): the same for the ESP32-C6.

<!-- nav-start -->
Previous: [MCH2022 badge](../apps/mch2022.md) · Up: [esp-sdr-zephyr](../index.rst) · Next: [Chip notes](chips.md)
<!-- nav-end -->
