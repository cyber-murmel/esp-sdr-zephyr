# The MCH2022 badge's FPGA as a DSP and graphics accelerator

The MCH2022 badge carries an iCE40UP5K next to its ESP32. The gateware in `gateware/`
(Amaranth) makes it the badge's display controller and spectrum engine: the ESP32 sends I/Q
samples and its user interface over one SPI port, the FPGA computes the spectrum and draws a
spectrum trace and a waterfall straight to the ILI9341, at the panel's rate.

## What runs where

| | ESP32 | FPGA |
|---|---|---|
| Panel | Zephyr's ILI9341 driver and LVGL, through `esdr,mch2022-fpga-dbi` | 8080 bus, passthrough, its own regions |
| Samples | bursts from `lib/esp_sdr`, sent as they are (WORD32) or packed | 8192-sample ring |
| Spectrum | settings in dB (`esdr_fpga_dsp_config()`) | window, FFT 16..512, power, log2, average, columns |
| Graphics | palette, region placement | waterfall ring (64 KiB), trace, rendering per TE |

The FPGA owns the panel's bus while the ESP32 holds LCD_MODE (IO26) high; the ESP32 floats its
D/C and CS (they meet the FPGA's WR and CS through 47 ohm) and keeps the panel's reset. Without
a bitstream the panel controller drives the panel over SPI as before.

## Building and loading

```sh
nix-shell shell.nix
cd gateware
python3 -m esdr_fpga.build top --out <dir>     # bitstream <dir>/top/top.bin, utilisation, fmax
python3 -m pytest tests                       # simulations against model.py
python3 -m esdr_fpga.regs > ../include/esp_sdr/esdr_fpga_regs.h
```

`CONFIG_ESDR_FPGA_IMAGE` embeds a bitstream; the driver loads it at boot (38 ms): the RP2040
holds CRESET (its register 5, bit 0) and reports CDONE, the image goes over SPI3 with SS low.
`apps/fpga_demo` has shell commands for the link (`fpga test`), graphics from software
(`fpga gfx`), the DSP on samples made on the ESP32 (`fpga dsp`) and, built with
`-DEXTRA_CONF_FILE=radio.conf`, the badge's radio (`fpga radio`, `fpga dump`).
`tests/integration/esdr_fpga` checks loading, the link, the panel and the DSP on the badge
(bitstream from `ESDR_FPGA_IMAGE`).

## The link

One SPI frame (CS low) is one command; the opcodes, memory windows and registers are in
`include/esp_sdr/esdr_fpga_regs.h`, generated from `gateware/esdr_fpga/regs.py`.

| Opcode | Payload |
|---|---|
| 0x01 WRITE | address, 16-bit words |
| 0x02 READ | address, count, two pad bytes; then the words come back |
| 0x03 SAMPLES | format (WORD32, IQ10P, IQ8, IQ16; RESYNC), samples |
| 0x04 DBI | flags (HOLD, NOCMD), command, data: the panel's traffic |
| 0x05 WFLINE | one waterfall line of palette indices |

Writes run at 40 MHz (about 5 MB/s), reads at 20 MHz: MISO comes through the GPIO matrix, and
at 26.7 MHz and above reads fail. Reads must be read-only SPI chunks: the classic ESP32's DMA
cannot do a write phase and a read phase in one half-duplex transaction (Zephyr commit
"drivers: spi: esp32: no write phase before half-duplex reads").

## The renderer

Two regions, a waterfall and a spectrum trace, each placed in screen pixels. Every pixel is a
palette index (248 colours for the waterfall, the rest for the trace and centre line). The
renderer writes with the host's MADCTL minus MV, so a write runs along a native panel row, and
one memory write per row, so the host gets the bus between any two rows: when it has bytes
queued or holds the bus (DBI flag HOLD, set by the panel controller from CASET/PASET against the
bytes written). A full 320x240 screen takes about 10.5 ms; on the TE edge (88 per second on
this panel) that is 44 renders per second.

## The DSP chain

Per frame: the window (Q15, from `esp_sdr_window()`) and a halving, a radix-2 FFT that halves
every stage, `L = 256 log2 |X|^2` plus 512 per halving plus an offset that puts a full-scale
tone at 0 dB, the average `s += F(L - s)` from a table that makes it the linear exponential
average of `esdr.psd()`, and every few frames the columns (the max of their bins, with a start
bin and step for zoom) scaled into a waterfall line and the trace's rows. `model.py` is the
reference, bit for bit.

Measured on the badge (N = 512, 30 MHz):

| | |
|---|---|
| frames per second | about 1860 (950 kS/s) |
| a full-scale tone at +fs/8 | column 200 of 320, 0.1 dB |
| radio bursts of 16380 at 40 MS/s, WORD32 | 73 per second: capture 0.43 ms, send 13.3 ms |
| the same, IQ10P / IQ8 | 102 / 127 per second (packing 1.1 / 0.8 ms) |

With interrupt-driven SPI (`CONFIG_SPI_ESP32_INTERRUPT`, which the driver implies) the ESP32
spends about 0.7 ms of CPU per burst. The FFT, not the link, limits the throughput.

## Fitting the UP5K

The gateware fills 4960 of 5280 logic cells (all 4 SPRAMs, 18 of 30 block RAMs, 4 of 8
multipliers) at 31 MHz. To fit, the FFT stops at 512 points (the screen has 320 columns), it
halves every stage instead of keeping a block exponent, the trace has no fill or grid, and the
test source (a tone generator) is gone: the ESP32 makes test samples. The UART probe line is a
build variant (`debug`). New features need cells found first: `synth_ice40 -noflatten` with
yosys `stat` shows them per module.

Zephyr's `lattice,ice40-fpga` driver configures the FPGA, at 25 MHz, the configuration port's
limit in the data sheet; frames run at 40 MHz. The board holds it in reset until a load
(`creset-hold`), and its CRESET and CDONE go through a GPIO driver on the RP2040.

## Sample formats

`gateware/tools/bfp_eval.py` compares formats on real bursts (`fpga dump`): eight bursts at
16, 40 and 80 MS/s, spectra at N = 512 against the lossless 10 bits, worst case:

| Format | Bits per pair | Floor shift | Worst error, top 40 dB | Weak tone's margin lost |
|---|---|---|---|---|
| IQ10P | 20 | 0 | 0 | 0 |
| BFP8 (16 pairs share an exponent) | 16.5 | 0.28 dB | 0.41 dB | 0.1 dB |
| IQ8 | 16 | 0.73 dB | 1.20 dB | 0.1 dB |
| BFP6 | 12.5 | 3.0 dB | 3.7 dB | 1.5 dB |
| BFP4 | 8.5 | 13.1 dB | 10.8 dB | 8.8 dB |
| NVFP4 (E2M1, E4M3 scale per 16) | 9 | 13.1 dB | 10.7 dB | 8.7 dB |
| 4-bit | 8 | 17.0 dB | 16.7 dB | 7.5 dB |

Only BFP8 stays within 0.5 dB of the 10 bits, for 17 % fewer bytes; with the FFT as the limit
that buys nothing, so the gateware has IQ10P and IQ8 and no block formats.
