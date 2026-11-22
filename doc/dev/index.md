# Reference

<!-- nav-start -->
Previous: [MCH2022 badge](../apps/mch2022.md) · Up: [esp-sdr-zephyr](../index.rst) · Next: [Chip notes](chips.md)
<!-- nav-end -->

This is the fifth and last part of the docs. The first four parts teach the radio, its sample
paths, the modems and the apps in reading order. This part holds what you look up while working
on them: chip and board quirks, where code and data are placed, how the tree is tested, a plan
for a reference radio, three roadmap pages, a design for PHY-less Ethernet links between S31
boards and the register pages.

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
5. [DMA chains](dma-chains.md): a roadmap for moving I/Q with descriptor lists: how each chip does
   it today, four levers, a common layer and the experiments.
6. [Hot loops](hot-loops.md): where retro and demoscene tricks could still speed up a hot loop,
   ranked, with locations and status.
7. [Ideas from elsewhere](ideas-from-elsewhere.md): what was taken or left from six outside
   projects (cnlohr, bitluni, WCH).
8. [S31 PHY-less Ethernet links](s31-link.md): a design (not built or measured) for MAC-to-MAC
   links between S31 boards: two jumper setups in MII and a dual-module RGMII board.
9. [S31 to host at 80 MS/s](s31-fpga-bridge.md): a plan (not built) for a Colorlight 5A-75B
   bridge that taps the S31's diag bus at 80 MS/s and sends it over gigabit Ethernet.
10. [Transmit stream](txs.md): the S31 streaming DAC transmit (txs): position model, flips,
   credits and the network feed.
11. [Network services](network-services.md): the S31 multiapp host on Ethernet: buffers and
   memory placement, the board findings, MCUmgr over UDP, the SSH server, later SFTP.
12. [ESP32-S3 registers](registers-esp32s3.md): the undocumented S3 radio registers the library
   relies on, with fields and evidence.
13. [ESP32-C6 registers](registers-esp32c6.md): the same for the ESP32-C6.

<!-- nav-start -->
Previous: [MCH2022 badge](../apps/mch2022.md) · Up: [esp-sdr-zephyr](../index.rst) · Next: [Chip notes](chips.md)
<!-- nav-end -->
