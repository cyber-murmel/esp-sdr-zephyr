# Error correction

<!-- nav-start -->
Previous: [QAM and OFDM](qam-ofdm.md) · Up: [Modulation and coding](index.md) · Next: [Applications](../apps/index.md)
<!-- nav-end -->

The frames of [link](../../apps/link/README.rst) carry their payload under an error correcting
code, or under the CRC-32 alone. This page lists the codes, how they are decoded, and how to
choose one. It is the last page of [Modulation and coding](index.md) and builds on
[QAM and OFDM](qam-ofdm.md), whose frames carry these codes.

Each frame names its code in the header: 4 bits, chosen with `link client ... -f <code>` (the
shell option is in the link README).

## Codes in use

How the code units fill a frame is on the PHY's page: for single-carrier QAM in
[Coding units and frame pairs](qam-ofdm.md#coding-units-and-frame-pairs), for OFDM in
[Frame](qam-ofdm.md#frame).

### Reed-Solomon

- `rs`: RS(255,223), 12.5 % parity, 16 byte errors per 255.
- For raw error rates up to a few 1e-3.
- Both PHYs use it.

### Extended Hamming

- Single-carrier QAM frames can carry extended Hamming (128,120).
- `bch1-8` is a Hamming code of 56 data bits per 64.

### BCH codes

`bch<t>-<bytes>`: binary BCH codes (`src/bch.c`).

- On contiguous blocks of up to 64, 128 or 256 bytes (GF(2^9), 2^10, 2^11).
- Correcting t = 1 to 4 bit errors per block with m t parity bits.
- `bch2-128` is 20 parity bits per 1024, 2.3 %.

### CRC-32 alone

`none`: the CRC-32 alone.

## Decoding

### Reed-Solomon decoding cost

- About 49 us for a clean codeword, 220 us with 2 errors and 400 us with 10.
- With many corrections, RS decoding dominates a receiver.

### BCH decoder

- The receiver checks the CRC-32 first (four bytes per lookup step, slicing by 4).
- Only a frame that fails it is decoded, block by block: the remainder register (a reflected CRC
  with the code's generator, also four bytes per step while r <= 32).
- For a block whose remainder is not zero: the syndromes, Berlekamp-Massey and a Chien search.
- The sender's parity is the same register.

### Interleaving

The blocks are not interleaved:

- a 1024-bit block already spans every subcarrier twice;
- a symbol that goes wrong as a whole would sink an interleaved frame of t-error blocks just the
  same.

## Choosing a code

### Why lighter codes

With the sample clock drift corrected, the 64-QAM payload arrives with a raw bit error rate
around 3e-6 (MER 26 to 27 dB): about one bit error in five long frames. So RS(255,223) carries
far more protection than the channel needs.

### Simulated comparison

`tools/ofdm_sim.c`, `FRAME=1 FEC=<code>`, 64-QAM, 26 MHz, the 80 MS/s frame of 3213 payload
bytes; frames decoded of 20:

| Code | User bytes | 24 dB | 27 dB | 30 dB |
| --- | --- | --- | --- | --- |
| rs | 2791 | 20 | 20 | 20 |
| bch1-8 | 2803 | 5 | 20 | 20 |
| bch2-128 | 3121 | 0 | 20 | 20 |
| bch2-256 | 3168 | 0 | 20 | 20 |
| bch4-64 | 2954 | 20 | 20 | 20 |
| none | 3209 | 0 | 8 | 20 |

- `bch2-256` carries 13.5 % more than `rs` where the link runs (the board's MER, about the
  simulator's 27 to 30 dB).
- `bch4-64` keeps RS's margin at 24 dB with 6 % more.
- A Hamming code of 64-bit blocks costs what RS costs and corrects far less.
- On the sender, BCH coding is faster than RS ([Sender](qam-ofdm.md#sender)).

### Reed-Solomon parity

`CONFIG_APP_RS_PARITY`: 32 bytes per unit by default (RS(255,223), 16 corrections).

- The unit needs about 4.7 corrections at an MER of 23.5 dB and twice that at 22.3.
- The MER drifts that much within a session: it fell from 23.6 to 22.2 dB over four hours of bench
  runs, the carrier offset from -22 to -28 kHz.
- 24 parity bytes carry 3.6 % more payload and cut the FEC stage to 1.3 to 1.9 ms. At 23.5 dB that
  is a better link; at 22.3 dB it loses half the frames.

Blind stream ([QAM and OFDM](qam-ofdm.md#blind-stream)), 64-QAM, 26 MHz, received Mbit/s and
lost, by parity bytes and MER:

| Period | 32, 23.5 dB | 24, 23.5 dB | 32, 22.4 dB | 24, 22.3 dB |
| --- | --- | --- | --- | --- |
| 7000 us | 3.00, 0 | 3.11, 0 | 2.98, 0.4 % | 1.57, 47 % |
| 6500 us | 3.10, 3.7 % | 3.34, 0.2 % | | |
| 6000 us | 3.04, 12.5 % | 3.46, 4.1 % | 3.10, 10.7 % | 1.19, 66 % |

16 parity bytes lose a fifth of the frames at 23.5 dB.

### Bit loading and rate adaptation

- The edge carriers (19 to 20 dB) produce most of the errors.
- A model on the measured SNR gives a static 16-QAM zone on the outer carriers, or bit loading in
  general, only 4 % over flat 64-QAM at equal error rate, so neither is built.
- What the lower MER needs is 16-QAM (2.3 Mbit/s loss-free in the same state): a rate adaptation,
  not more parity.

<!-- nav-start -->
Previous: [QAM and OFDM](qam-ofdm.md) · Up: [Modulation and coding](index.md) · Next: [Applications](../apps/index.md)
<!-- nav-end -->
