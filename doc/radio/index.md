# The radio

<!-- nav-start -->
Previous: [ESP-SDR on Zephyr](../../README.rst) · Up: [esp-sdr-zephyr](../index.rst) · Next: [Transceiver](transceiver.md)
<!-- nav-end -->

This part follows the signal through the analog radio of each chip: from the antenna to the ADC on
receive, and from the DAC back to the antenna on transmit. Part 2,
[Samples in and out](../samples/index.md), starts where the samples leave the ADC.

Each page takes one property of that chain (the blocks, the LO, the gain, the filters, the
impairments, the crystal), explains it, and shows the registers and library calls that act on it.

## What a software defined radio does

### Complex baseband

The radio delivers its signal as complex samples, I and Q, centred on the local oscillator (LO):
frequency 0 in the samples is the LO's frequency on air.

A complex stream shows a band as wide as its sample rate, centred on the LO: a burst is as wide as
its rate ([micropython](../../apps/micropython/README.rst)). What lies further out folds into that
band unless a filter keeps it out ([Filters](filters.md)). Which rates each chip delivers, and how,
is the subject of [Samples in and out](../samples/index.md).

### Direct conversion

No source states the receiver's architecture. The evidence points to direct conversion (zero IF),
where the RF mixer brings the channel straight down to baseband:

- The baseband I/Q is centred on the LO.
- The receive DC stands far over the noise floor at a paced stream's centre
  ([Impairments](impairments.md#servos)).
- A tone of frequency 0 from the transmit tone generator is a carrier at the LO
  ([ESP32-C6 registers](../dev/registers-esp32c6.md#register-fe_tone0_reg-fe_tone1_reg-0x600a0420-0x600a0424)).
- The transmitter shows LO leakage and an I/Q image at -f ([Impairments](impairments.md)).
- The first baseband block after the mixer already has separate I and Q capacitor codes, so the
  90 degree split is inferred to sit at the RF mixer ([Transceiver](transceiver.md)).

In a direct conversion receiver, the DC offset, the LO leakage and the 1/f noise land at 0 Hz, in
the middle of the band. [Impairments](impairments.md) measures them; [Tuning](tuning.md) shows how
the library moves the LO off the channel to keep them out.

### One crystal

One crystal drives both the LO and the sample clock of every chip here. When it runs off, the
carrier and the sample rate move by the same ratio. [Impairments](impairments.md) measures the
offset between two boards; [Crystal calibration](crystal-calibration.md) measures the crystal
against an LTE base station and corrects it.

### Half duplex

Every chip has one RF front end and one antenna pin for both directions, so it receives or
transmits, never both at once. [Transmit](../samples/transmit.md) explains the lock that keeps the
two apart and the turnaround between them.

## Chip lineup

The library runs on five chips. Their sample paths (bursts, rings, paced streams, replay) are in
[Samples in and out](../samples/index.md); the apps per chip in [Applications](../apps/index.md).

### Chips and boards

| Chip | Boards | Cores | Board details |
|---|---|---|---|
| ESP32-S3 | XIAO ESP32S3, Hackaday Communicator badge | SMP or single core | 40 MHz crystal on the XIAO |
| ESP32-C6 | XIAO ESP32C6, the WHY2025 badge's radio co-processor | | 40 MHz crystal on the XIAO |
| ESP32-C5 | Waveshare ESP32-C5-Zero | | ESP32-C5HF4 rev v1.0, 48 MHz crystal, 4 MB flash, no PSRAM |
| ESP32-S31 | ESP32-S31-Function-CoreBoard-1 | SMP or single core (below) | ESP32-S31 rev v0.0, 40 MHz crystal, 16 MB flash, 16 MB octal PSRAM, PCB antenna |
| ESP32-S31 | VIEWE 4.3 inch touch display board | | [Chip notes](../dev/chips.md) |
| ESP32 (classic) | MCH2022 badge | | |

- The Function CoreBoard builds as `esp32s31_function_coreboard/esp32s31/hpcore`, by simple boot.
- On it Zephyr holds CPU1 in reset unless the image has `CONFIG_SMP`.
- The board files and per-chip build notes are in [Chip notes](../dev/chips.md).

### Radio per chip

| Chip | Receive rates (MS/s) | Transmit | PLL lock and LO reach | 5/6 divider | Wi-Fi driver | Registers |
|---|---|---|---|---|---|---|
| ESP32-S3 | 80, 40, 16 | DAC engine, 80 or 40 MS/s | PLL 2180 to 2793 MHz; LO 1817 to 2793 MHz on air | yes | needed, or without it (`CONFIG_ESP_SDR_PHY_ONLY`) | [ESP32-S3 registers](../dev/registers-esp32s3.md) |
| ESP32-C6 | 80 (40 disputed, see [Transceiver](transceiver.md)) | TX DC value, CPU-paced (the DAC engine is not used) | PLL 2128 to at least 2856 MHz; LO 1774 to at least 2856 MHz on air | yes | needed, or without it (`CONFIG_ESP_SDR_PHY_ONLY`) | [ESP32-C6 registers](../dev/registers-esp32c6.md) |
| ESP32-C5 | 80, 40, 20, 10, 8, 4 | TX DC value, CPU-paced; DAC engine, 80 or 40 MS/s | any MHz above 14, nothing below about 1100 MHz; edges not measured | no | needed | [Chip notes](../dev/chips.md) |
| ESP32-S31 | 80, 40, 20, 10, 8, 4 | tone generator; TX DC value, CPU-paced; DAC engine, 80, 40 or 20 MS/s | 2200 to some 2900 MHz | no | runs without it | [Chip notes](../dev/chips.md) |
| ESP32 (classic) | 80, 40, 16 | none | not measured | yes | needed | |

[Tuning](tuning.md) gives the lock ranges with their measurements, and
[Samples in and out](../samples/index.md) the radio without the Wi-Fi driver.

## Bench setups

These are the setups behind entries that name no other.

| Chip | Board | Partner and link | Items |
|---|---|---|---|
| ESP32-S3 | two XIAO ESP32S3 | each other, through a 30 dB attenuator | the link measurements |
| ESP32-C6 | one XIAO ESP32C6 | | the C6 entries |
| ESP32-C5 | one Waveshare ESP32-C5-Zero | a XIAO ESP32-C6 through a 30 dB coax attenuator | the receive items without transmitting; the transmit items with the partner |
| ESP32-S31 | one ESP32-S31-Function-CoreBoard-1 | | receive, a carrier, the CPU-paced transmit and the DAC engine's replay |
| ESP32-S31 | the VIEWE board | a XIAO ESP32-S3 a few cm away, over the air, which heard its transmissions | the carrier, the paced transmit and the replay |

The S31 sources name both boards for the transmit items: the opening of their S31 section names
the Function CoreBoard, while the carrier, paced and replay entries name the VIEWE board.

## This part

1. [Transceiver](transceiver.md): the analog chain block by block, with the registers and calls of
   each block, the analog I2C access and the vendor PHY functions.
2. [Tuning](tuning.md): the RF PLL and the 5/6 divider, the lock ranges and `-ERANGE`, the retune
   time and where the LO sits.
3. [Gain](gain.md): the receive gain index and the AGC, the clip guard, the transmit gain and the
   transmitter's compression.
4. [Filters](filters.md): the receive and transmit low-pass filters, the channel bandwidth mode,
   the 40 MS/s decimation filter and aliasing.
5. [Impairments](impairments.md): DC offset, LO leakage and image, spurs, ADC clipping products,
   the clock offset between boards and phase noise.
6. [Crystal calibration](crystal-calibration.md): the crystal's error measured against LTE, its
   temperature model and storage.
7. [Coexistence](coexistence.md): what the SDR, Wi-Fi and ESP-NOW share, the time-sharing modes, the
   S3's PHY power-down windows and runtime loading per chip.

<!-- nav-start -->
Previous: [ESP-SDR on Zephyr](../../README.rst) · Up: [esp-sdr-zephyr](../index.rst) · Next: [Transceiver](transceiver.md)
<!-- nav-end -->
