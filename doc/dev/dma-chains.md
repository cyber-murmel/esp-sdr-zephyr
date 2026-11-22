# DMA chains for I/Q

<!-- nav-start -->
Previous: [ZigBit reference](zigbit.md) · Up: [Reference](index.md) · Next: [Hot loops](hot-loops.md)
<!-- nav-end -->

This page is a roadmap, with two built pieces (the S3 gather, [below](#the-s3-layer-as-built), and the S3 backup DMA's register lists, [below](#the-s3-backup-dma-register-lists)). It asks whether descriptor lists
(GDMA chains, rings and re-linking, REGDMA) can become one project-wide way of moving I/Q samples.
It says how each chip moves samples today, ranks the four places where a list would replace a CPU
copy, sketches one common layer (`esp_sdr_dma`) and lists the experiments that decide each lever.
It sits in the reference part because it plans work; the guides of Part 2 describe what exists.

Only the S3 layer is built, and its sections give the numbers measured on the DevKitC; nothing else here is measured, except where a figure cites a file or a page. Estimates are
marked **(estimate)** and claims not yet checked against the code or a board are marked
**(check)**. Hal paths are relative to `modules/hal/espressif/components/`.

## Answer

- **The radio's own sample mover is not a GDMA.** On every chip the capture and DAC playback use
  the Wi-Fi MAC's dump engine. It writes or plays one whole SRAM bank, takes that bank from the
  CPUs while it runs, and has no descriptors. Lists cannot replace it. They can only work around
  it: before playback, after capture, and on the links to the host.
- **GDMA rings are already in the sample path on two chips.** The C5 diagnostic ring (AHB GDMA, 16
  nodes of 3584 B, `lib/esp_sdr/src/esp_sdr_diag.c:46`) and the S31 diag16 ring (AXI GDMA, 4096 B
  nodes with a header hole, `include/esp_sdr/esp_sdr_diag16.h:48`) share one pattern: a circular
  list, length write-back and an interrupt that walks the list. The C6's SDIO link already sends a
  queue of descriptors over the caller's buffers with no copy
  (`apps/osmosdr_serial/src/link_sdio.c`, SDIO-slave DMA through `sdio_slave_ll_*`, not a GDMA).
  That code is the seed of the common layer.
- **Four places where lists would replace CPU copies** (the levers, below).
- **Where it does not help:** the S3 ring and the S3 gapless TX. Both run on the dump engine's
  banks, and their CPU work (FIR, unpacking, interpolation) is computation, not copying.
- **Order:** S31 re-linking first (lowest risk, known hardware), then the C6 gather (largest
  gain), then S3 slot gathers, then the REGDMA spikes. The S3 slot gather is the part that can
  be tried without another board, so it is planned next.

## Status of the levers

| # | Lever | Chip | Gain (estimate) | Status |
| --- | --- | --- | --- | --- |
| 1 | GDMA gather of kept windows into staging | C6 | whole bursts from about 50 us to the staging limit | deferred: needs the C6 badge |
| 2 | Hand ring nodes to USB by re-linking | S31 | no 32 KiB copy per batch | built 2026-11-24 (`CONFIG_APP_HS_ZEROCOPY`, `esdr_dma_ring`, `esp_sdr_diag16_slab_detach()`), measured 2026-11-24/25 on the CoreBoard at 20 MS/s: 86 % of the CPU free against 80 % with the copy; 300 s runs with 0 or 1 dropped batch (all three spares out), no rejected swap; off by default |
| 3 | TX waveforms as a gather of slots | S3 | 25k down to 3 to 6k cycles for the ACK tail (the CPU build is now 11.9k) | measured: the gather loses on latency (389 us against 192 us), the CPU build stays |
| 4 | REGDMA register lists for paced TX and RX | C6, C5, S31 | lifts the C6's 250 kS/s TX ceiling | deferred: needs the C6 (first), then C5 and S31 |
| 5 | Backup DMA register lists (bank switch, dump trigger, GDMA start) fired by one CPU store | S3 | CPU work of a dump or replay step down to one store plus the wait | built (`esp_sdr_bkdma_*`), measured 2026-11-24: 12 to 15 cycles a word, about 400 cycles a run, starts a GDMA chain; pays for longer lists, not for two words (see [the section](#the-s3-backup-dma-register-lists)); the S3 has no REGDMA, this is its stand-in |

Lever 3 has a decision gate: the gather goes into the tree only if it beats the CPU build of the
ACK tail (see [Hot loops](hot-loops.md), item 1) in CPU cycles. Otherwise the numbers are
documented and the CPU build stays.

## How each chip moves I/Q today

| Chip | Path | What moves the samples | CPU work on each sample | What limits it | Lever |
| --- | --- | --- | --- | --- | --- |
| S3 | Ring, 16 MS/s into 3 banks (`esp_sdr_ring.c`) | Dump engine, circular, banks switched by CPU 1 | FIR or box4 reads the bank in place; chancap's raw sink unpacks to int16 | CPU 1 filter budget, unit overhead | none for the data |
| S3 | TX stream and generator (`esp_sdr_tx_dac.c`) | Dump engine plays bank 0 or 1; CPU flips banks on a cycle (`LOOP_LEAD`, `LOOP_STEP` at line 290) | Interpolation or the generator fills the idle bank | Fill time | none for interpolation; gather for precomputed waveforms |
| S3 | Burst from a ring run (`burst_run()`, `esp_sdr_ring.c:1285`) and the 802.15.4 ACK | Dump engine plays bank 2 | `ieee154_dsp.h` copies 20-word slots: about 85 cycles a slot, 105 us for the part after slot 416 ([IEEE 802.15.4](../modem/ieee802154.md)) | CPU 0 time before the verdict | **3** |
| C6 | Gapless 80 MS/s (`esp_sdr_gapless.c`) | Dump engine, circular over blocks 1 and 2, units of 12288 samples (154 us) | Kept windows copied raw into staging at 2.5 to 3.6 cycles a sample, then packed to int8 (9 to 20 cycles) | About 4000 samples a unit can be copied before the block is reused ([Rings](../samples/rings.md), "What limits a burst") | **1** |
| C6 | Triggered snapshots (`esp_sdr_trig.c`) | Dump engine ring in block 2, stopped on a trigger | Read after the stop, packed | 4 ms dead time | gather into SDIO blocks **(minor)** |
| C6, C5 | Paced RX (`esp_sdr_paced.c`) | CPU reads the IQ estimator sums every sample | 59 of 80 cycles a sample at 2 MS/s ([Paced](../samples/paced.md)) | CPU | **4** |
| C6, C5, S31 | Paced TX | CPU stores the TX DC value register every sample | 15 to 48 cycles a store plus the FIR; the C6 stops at 250 kS/s | CPU; "ETM has no task that writes a register value" (`doc/samples/paced.md:138`) | **4** |
| C5 | Diag ring (`esp_sdr_diag.c`) | Diag bus, PARLIO RX, AHB GDMA ring of 16 x 3584 B | box16 and CIC at 4.1 to 4.4 cycles a byte (budget 6.0) | CPU decimation, USB-Serial-JTAG | already a ring; move it into the common layer |
| C5 | Replay (`esp_sdr_replay.c`) | Dump engine plays block 1, REPEAT and LOOP in hardware | none while it plays | 16383 words; block 1 is lost to the CPU | none |
| S31 | diag16 (`esp_sdr_diag16.c`) | Diag bus, PARLIO RX, AXI GDMA ring, 128 KiB of 4096 B nodes with a header hole (`lib/esp_sdr/Kconfig:237`) | osmosdr_hs `memcpy()`s 8 nodes (32 KiB) per transfer into `in_buf` (`apps/osmosdr_hs/src/main.c:140`) | CPU copy at 40 MB/s; overrun safety | **2** |
| S31 | Dump ring (`esp_sdr_dumpring.c`) | Dump engine over TCM groups; turns park the other core | PIE copies 4 words a load | Turns (up to 21 us) | none (engine) |
| C6 to P4 | SDIO (`link_sdio.c`) | SDIO device-side DMA over a queue of descriptors on the caller's buffers | none: blocks go out as they are | About 13 MB/s within a transfer | already zero-copy |
| P4 | Bridge (`osmosdr_bridge`) | SDIO host, then bulk IN | Blocks pass as they are (a 16-block pool) | C6 | none needed |

## The four levers

### 1. C6 gapless: gather kept windows by GDMA

**Today.** Each unit, the CPU reads the estimator's 10 us windows, decides which to keep (with one
window before and one after) and copies them raw from the block the engine has just left into
staging (`copy_words()`, `esp_sdr_gapless.c:1037`). Each block stays readable for about one unit
(154 us), so whatever the CPU cannot copy in that time is lost. That cap is about 4000 samples a
unit, so bursts arrive whole only up to about 50 us, and no 802.15.4 frame (an ACK lasts 352 us)
arrives whole.

**Staging size.** The staging ring defaults to 48 KiB (`lib/esp_sdr/Kconfig:183`, range 8 to 96),
that is 12288 samples or 154 us at 80 MS/s. The upper halves of blocks 1 and 2 add 2 x 64 KiB once
their canaries hold (`esp_sdr_gapless.c:149`). In all that is 176 KiB, 45056 samples, 563 us at
80 MS/s **(estimate from the Kconfig default)**. That holds a whole ACK (28160 samples). The page
[Rings](../samples/rings.md) still quotes the older 64 KiB ring (205 us) and 196 KB in all.

**With descriptors.**

- At each unit switch the CPU writes one descriptor pair per kept run: source is the run in the
  released block, destination is its staging slot. A 10 us window is 800 words, 3200 bytes, so it
  fits one descriptor (4095 bytes at most). Longer runs split at 4092.
- It starts a memory-to-memory channel and goes back to the estimator. The copy no longer competes
  with the window readings for the CPU.
- The unit's deadline still holds. The chain has to finish within a unit. Completion is the EOF
  interrupt or a poll of the length write-back. A chain that misses it is marked lost, as a late
  copy is today.
- **Gain (estimate).** If memory-to-memory moves 1 word every 2 bus cycles at 80 MHz (**check**:
  the C6 GDMA clock and throughput), a unit's 12288 words copy in about 300 us. That is too slow
  for everything, but 6000 to 8000 words a unit are plausible, 1.5 to 2 times today's cap. With
  the CPU copy beside it the cap could double or triple. A run longer than one unit then goes on
  until staging fills.
- **Packing stays on the CPU.** It works from staging with no deadline, so it limits the average
  kept rate, not the length of a burst. The threshold servo already holds what is kept short of
  the link.

**Risks to check first.**

- Can the GDMA read a block the engine has just released, and does it see the engine's data (the
  CPU does, for one unit)? Ownership is set in `HP_SYSTEM_SRAM_USAGE`. A read of a block the
  engine still owns has to fail safely.
- Bus contention: the engine writes the other block at 320 MB/s while the GDMA moves internal
  SRAM. Markers that prove each unit starts where the last ended would show a disturbed engine.
- Channel count. The C6 has 3 GDMA pairs. SDIO has its own DMA, so one pair should be free
  **(check)** against PARLIO and SPI use in the badge image.
- The C6 hal has `hal/gdma_ll.h` only (`esp_hal_dma/esp32c6/include/hal/gdma_ll.h`). It has no
  `ahb_dma_ll.h`, which the C5, S31 and P4 do have. A C6 backend uses the `gdma_ll` calls.

### 2. S31 osmosdr_hs: hand ring nodes to USB by re-linking

**Today.** The diag16 ring is 128 KiB of 4096 B nodes (32 nodes), each a wire-format block with
the header left as a hole. `batch_send()` copies 8 nodes (32 KiB) into one of 3 `in_buf`s and
submits that to USB (`apps/osmosdr_hs/src/main.c:140`, `IN_FLIGHT_MAX` 3), because the ring never
waits and the GDMA would overwrite a node USB is still reading. At 40 MB/s the copy is the app's
main CPU cost (**check** with `esp_sdr_prof`).

**With re-linking.**

- Keep a pool of spare nodes the size of `in_buf` (3 x 8 nodes = 96 KiB). That is the memory the
  copy uses today, so it needs no extra RAM.
- When a batch of 8 filled nodes is ready, write the header into each hole as now. Then point
  those 8 descriptors' `buffer` fields at 8 spare nodes and submit the original 8 to USB. When the
  transfer completes, the original nodes return to the pool.
- **It is safe without stopping the ring.** The re-pointed descriptors belong to nodes the DMA has
  just finished. It reaches them again only after a wrap, 24 nodes and about 2.4 ms at 40 MB/s
  later. A fence (`barrier_dmem_fence_full()`) before returning the owner bit is enough. The
  owner check is off, so the check that matters is the existing `esp_sdr_diag16_node_valid()`
  margin, done before the swap instead of after the copy.
- **A batch must be contiguous for one USB transfer.** After a swap it no longer is. Either submit
  8 transfers of one node (the DWC2 driver queues them; **check** the per-transfer cost at high
  speed), or give the pool whole 32 KiB slabs and swap 8 contiguous nodes at a time.
- Zephyr's DWC2 driver already does buffer DMA from TCM ([Transports](../samples/transports.md)),
  so USB reads the nodes in place.
- The same swap gives the C5 diag ring a zero-copy path for a raw dump into PSRAM or RAM. Its USB
  is full speed only, so that would be bursts, not a stream.

### 3. S3: TX waveforms as a gather of precomputed slots

**Today.** The 802.15.4 ACK is built from 18 half-sine slot templates of 20 words
(`IEEE154_DSP_SLOT`, `lib/ieee802154/ieee154_dsp.h:29`), copied by the CPU into the burst bank by
`ieee154_dsp_tx_gen()` (`ieee154_dsp.h:109`): about 85 cycles a slot, 105 us for the part that
depends on the sequence number and FCS, 251 us for a whole ACK ([IEEE 802.15.4](../modem/ieee802154.md)).

**With descriptors.**

- Precompute the slot table once in internal RAM. Each symbol becomes one descriptor pointing into
  the table. The other side of the memory-to-memory pair is one chain over bank 2 (4092 B each).
- The CPU writes 12 bytes a slot instead of copying 80. For the variable part (289 slots) that is
  about 3.5 KB of descriptors and perhaps 3 to 6k cycles instead of 25k **(estimate)**. The copy
  runs in the background.
- **The fixed head stays fixed.** A per-PAN head lives in bank 2 for the whole run, as now
  (`doc/modem/ieee802154.md`, "Precomputed waveform"). Only the tail is gathered.
- The same gather could build a whole frame (802.15.4, BLE advertising, later ESB) without
  running `gfsk_mod`.

**Limits.**

- The S3's GDMA reaches bank 2 only while the CPUs can, so not during playback. The late CRC
  decision of a shockburst-style ACK would still need the tail-bank flip or the cut.
- S3 memory-to-memory throughput and descriptor fetch cost are unmeasured. With 20-word slots the
  per-descriptor overhead could dominate. Experiment 5 measures it at descriptor sizes from 80 B
  to 4092 B.
- The S3 has 5 GDMA channel pairs, shared with SPI, LCD_CAM and I2S in apps that use them
  **(check)**. USB OTG has its own DMA.
- The S3 hal's `gdma_ll.h` has a memory-to-memory switch (`mem_trans_en`,
  `esp_hal_dma/esp32s3/include/hal/gdma_ll.h:376`). The repo does not use it yet.

### 4. REGDMA: register lists for paced TX and RX

**Why REGDMA.** Paced TX writes the TX DC value register once a sample and paced RX reads the IQ
estimator's sums once a sample. [Paced](../samples/paced.md) says ETM has no task that writes a
register value, and a memory-to-memory GDMA to a peripheral address is untested: ESP-IDF's async
memcpy allows only RAM, and PMS/APM may block it (experiment 8: on the S3 the GDMA itself refuses,
the [backup DMA](#the-s3-backup-dma-register-lists) does not; the C6 part is still open). REGDMA
(the PAU, present on C5, C6, P4 and S31, not on the S3) walks linked lists of register writes and
backups in RAM. That is the paced path's job.

**What the hal confirms** (read from the sources, nothing run):

| Claim | Where | Result |
| --- | --- | --- |
| Node types | `soc/include/soc/regdma.h:119` | continuous and address-map (backup and restore ranges), write (value and mask), wait (condition), plus branch variants; a 10-bit word count and an end-of-list flag in the head |
| Link format, builders | `esp_hw_support/port/regdma_link.c:30` ff. | `regdma_link_new_*()` allocate the nodes; a write node holds its value inline, so a sample stream needs one node per sample **(check)** |
| ETM start tasks | `soc/<chip>/include/soc/soc_etm_source.h` | `REGDMA_TASK_START0..3`: C6 128 to 131, C5 127 to 130, P4 161 to 164, S31 297 to 300; done events `REGDMA_EVT_DONE0..3` |
| ETM tasks for GDMA | same files | C6 `GDMA_TASK_IN_START_CH0..2` 159 to 161 and `OUT_START` 162 to 164; C5 143 to 148; not found by that name on S31 and P4 **(check)** |
| Read interval | `soc/esp32c6/register/soc/pau_reg.h:212` | `PAU_READ_INTERVAL`, bits 6:0, default 32, in `PAU_REGDMA_BKP_CONF_REG`; only the C6 file was read |

Everything else the earlier desk notes claimed about REGDMA stays unverified: that the interval
paces the nodes, that a list can loop back to its start, the write rate (perhaps 1 to 5 M writes a
second **(estimate)**) and the node size (the head and four words, about 20 B, **(check)**). The
note behind those claims is not in the repo.

**TX.** Pack the FIR output as a list of write nodes to the TX DC value register, paced by the
read interval or restarted per block by a GPTimer through ETM. That would take the C6 from 250
kS/s (CPU-paced, interrupts locked) to whatever REGDMA sustains. At about 20 B a node a 32 KiB list
holds about 1600 samples, so the CPU refills one list while REGDMA plays the other.

**RX.** Backup nodes copy the estimator's sum registers into RAM, one node per sample. The CPU
takes the differences later in bulk, without polling.

**Unknowns (the spike decides).**

- Pacing jitter: a boxcar sample that lands late shifts its window.
- Whether REGDMA's bus access reaches the modem's registers at all.
- Whether the PMU owns REGDMA in a PHY-only image. The regdma links of the PHY and the modem
  already exist for sleep retention (`REGDMA_PHY_LINK`, `REGDMA_MODEM_FE_LINK` in `regdma.h`).

## The common layer

One small library, `lib/esp_sdr/src/esp_sdr_dma.c` with `include/esp_sdr/esp_sdr_dma.h`, over the
hal's LL calls (`ahb_dma_ll.h`, `axi_dma_ll.h`, and `gdma_ll.h` for the S3 and C6), in IRAM, with
no interrupts required on the paced paths. Only the S3 backend is planned next.

| Piece | What it does | Replaces |
| --- | --- | --- |
| `esdr_dma_ring` | A circular descriptor list over N nodes with an optional header hole. A completion walk from the last node seen uses the length write-back, so merged interrupts lose nothing. Gives a produced count and a node-valid check | The duplicate ring code in `esp_sdr_diag.c` and `esp_sdr_diag16.c` |
| `esdr_dma_ring_swap(ring, node, spare)` | Re-points a finished node's descriptor at a spare buffer and returns the old one, with the margin check and a fence | `memcpy()` in osmosdr_hs `batch_send()` |
| `esdr_dma_gather(ch, dst, dst_len, src[], len[], n)` | Memory-to-memory: a chain over scattered sources and one over a contiguous destination. Returns when started; `esdr_dma_wait()` or EOF | The C6 staging copy, the S3 slot copies |
| `esdr_dma_chain` | A queue of descriptors over caller buffers, sent as one chain when idle and retired on EOF | The send queue of `link_sdio.c` (keep its LL calls, share the queue logic) |
| `esdr_regdma_list` (experimental) | Builds write and backup nodes for one register, at a pace | The paced TX and RX loops, if the spike works |
| Backends | S3 and C6 `gdma_ll`; C5 AHB GDMA; S31 and P4 AXI GDMA (and AHB GDMA for peripherals on the AHB) | |

Rules it enforces:

- Descriptors and buffers live outside any bank or group the dump engine can own. The linker
  snippets `esp_sdr_bank_*.ld` already check images; the descriptor sections join that check.
- No GDMA touches a bank while the engine owns it. The layer refuses a source or destination
  inside an owned bank, as the CPU code already avoids it.
- One channel claim per app. The S31's "every run resets the whole AXI GDMA group" (it kills the
  panel) means the layer resets only its own channel, never the group.

## The S3 layer as built

`include/esp_sdr/esp_sdr_dma.h` and `lib/esp_sdr/src/esp_sdr_dma.c` (Kconfig `ESP_SDR_DMA`, S3
only, off by default in `apps/wpan`, on in `apps/rfprobe`, see "Open crash"). It is the S3 backend of the layer above: `gdma_ll.h` calls, a pair of
GDMA channels in memory-to-memory mode (`mem_trans_en`), polled, no interrupt. The code is in
flash; an IRAM start and wait is a later step if a caller needs it.

| Piece | What it does |
| --- | --- |
| `esp_sdr_dma_claim()` / `_release()` | One of the five channel pairs, from channel 4 down; bus clock on, only the claimed channel reset, never the controller |
| `struct esp_sdr_dma_job` | Two caller-owned descriptor lists: `tx` reads the pieces, `rx` writes the contiguous destination |
| `esp_sdr_dma_gather_build()` | Pieces `(src, len)` (each up to 4092 B) into one destination, the source list ending in EOF; the destination list is cut at 4092 B |
| `esp_sdr_dma_copy_build()` | The same for one source cut into pieces of a chosen size |
| `esp_sdr_dma_desc_set_buf()` | Re-points a built source descriptor: the per-ACK cost of a gather whose lengths do not change |
| `esp_sdr_dma_start()` / `_wait()` / `_done()` | Reset the pair, re-arm the destination list, start; poll the RX success EOF (errors reset the pair) |
| `esp_sdr_dma_burst()` | 16 byte INCR bursts on both channels (pieces, source and destination multiples of 16) |
| `esp_sdr_dma_in_bank()` | The guard's test |

Decisions:

- **Channel ownership.** Zephyr's `dma_esp32_gdma` driver owns the controller when the `dma` node
  is enabled; the S3 devicetree leaves it `disabled`, and only the XIAO's `procpu_sense` board
  overlay enables it (for the camera and sensor variant, which no app of this repo builds). So
  the library keeps its own claim mask and `ESP_SDR_DMA` depends on `!DMA_ESP32`: the two never
  share the controller. It resets only its channel, because a controller reset would stop any
  other user (the S31 note above, "every run resets the whole AXI GDMA group"). SPI, LCD_CAM and
  I2S users that go through Zephyr's driver exclude the layer; one that drives `gdma_ll` itself
  has to pick another channel.
- **Banks.** A source or destination inside the capture bank, bank 1 (`ESP_SDR_BANK1`) or bank 0
  (`ESP_SDR_RING`) is refused with `-EACCES` unless the caller passes
  `ESP_SDR_DMA_CPU_OWNS_BANKS`: no capture, ring or playback may run. The test uses the linker
  symbols of `esp_sdr_bank_s3.ld`. `esp_sdr_bank_usage()` returns the usage word to write, not a
  state, so it cannot serve as a check.
- **Descriptors** are refused in any bank, flag or not. In the image they cannot be there: the
  S3 snippet already asserts that the DRAM image ends below the lowest bank, so descriptor arrays
  in `.bss` need no linker change (`apps/wpan` keeps 300 + 8, 3696 B of DRAM).
- **Source and destination** words: destination 4-byte aligned and the total a multiple of 4;
  bursts need multiples of 16.

### The bench

`probe dma sram|reg` in `apps/rfprobe` (`src/dmabench.c`, shell commands of the S3 build), and
`wpan bench dma ack` in `apps/wpan` (radio off, one `wpan on` since boot: the ACK code needs the
802.15.4 driver):

- `probe dma sram`: the capture bank (the CPUs' while nothing captures or loops) as source and
  destination, one 4092 B descriptor, then 16 KiB in pieces of 4092, 1600, 400, 160 and 80 B (the
  last four also with bursts of 16): CPU cycles to write the lists, DMA cycles from start to EOF,
  cycles per descriptor, MB/s, the result compared with the source; `memcpy` of 16 KiB for
  reference; a loop of 1024 CPU reads alone and while a 1600 B chain runs, over image data and
  over the destination. Moved from `wpan bench dma sram`, where the numbers below were taken
  (bank 1 to bank 2).
- `wpan bench dma ack`: the 289 slots from slot 416 of a 5-byte ACK gathered from the template
  table into bank 2, compared word for word with `ieee154_dsp_tx_gen()` (the CPU build, run under
  the ring's conditions). It reports the full list write, the pointer patch of an already built
  list (`tail_patch()`, what an ACK needs: lengths and links are fixed), the start call and the
  DMA time.
- `probe dma reg [mac]`: one 32-bit GDMA write into `RTC_CNTL_STORE7` (a scratch register of the
  always-on RTC block, used by the deep sleep wake stub only, no clock gate to stall the bus), or
  with `mac` into the Wi-Fi MAC dump engine's `DUMP_CONFIG_REG` (0x60033d90, radio initialised
  once, dump engine idle), with both PMS violation monitors (`DMA_APBPERI` and `BACKUP_BUS`)
  armed, then a read back, the old value restored. Experiment 8, result below.

### Measured numbers

DevKitC (S3, 240 MHz), the earlier `wpan bench dma` (now `probe dma sram` and `wpan bench dma ack`), bank 1 to bank 2, best of 4. Setup is the CPU time to write
both lists; a descriptor costs the CPU about 37 cycles to write.

| Row | Setup (CPU cycles) | DMA cycles | Cycles per descriptor | MB/s | Bursts of 16: DMA cycles, MB/s |
| --- | --- | --- | --- | --- | --- |
| 1 x 4092 B | 363 | 19079 | 19079 | 51.5 | n/a |
| 16 KiB in 5 x 4092 B | 535 | 74689 | 14937 | 52.6 | n/a |
| 16 KiB in 11 x 1600 B | 745 | 74841 | 6803 | 52.5 | 62487, 62.9 |
| 16 KiB in 41 x 400 B | 1795 | 75666 | 1845 | 52.0 | 63108, 62.3 |
| 16 KiB in 103 x 160 B | 3965 | 77358 | 751 | 50.8 | 64406, 61.1 |
| 16 KiB in 205 x 80 B | 7535 | 80102 | 390 | 49.1 | 66545, 59.1 |
| `memcpy` of 16 KiB (CPU) | | 10281 | | 382 | |

The transfer is bandwidth bound at about 50 MB/s (62 with bursts): the descriptor fetch costs
about 3 % at 80 B pieces. The CPU copies the same bank to bank at 7 times the speed. A CPU read
loop (1024 words) is not slowed by a chain running (image 4641 against 4500 cycles, destination
bank 4108 against 4107): the copy overlaps CPU work.

| ACK tail gather (289 slots, 23120 B) | Cycles |
| --- | --- |
| CPU build, ring conditions (item 3: 11.9k) | 11864 |
| Gather: write both lists whole | about 21.5k to 28.8k (first run cold) |
| Gather: patch the 289 pointers | 8140 (28 a slot, not the 7 estimated) |
| Gather: start call | 580 (4563 on a cold cache, code in flash) |
| Gather: patch + start, the CPU side | 8672 |
| Gather: DMA, start to EOF | 112702 (469 us) |
| Gather with bursts: DMA, start to EOF | 93574 (389 us) |
| Words differing from the CPU build | 0 |
| GDMA write to a peripheral register (`RTC_CNTL_STORE7`, `DUMP_CONFIG_REG`) | refused by the GDMA (IN_DSCR_ERR), nothing written; see "Experiment 8" below |

### Experiment 8: the GDMA cannot write a register

Measured 2026-11-24 on the DevKitC (`probe dma reg`, the same code ran first as
`wpan bench dma reg`): a memory-to-memory GDMA write of one word to `RTC_CNTL_STORE7`, and to
`DUMP_CONFIG_REG`, ends with the RX channel's raw `IN_DSCR_ERR` (0x8), the descriptor's owner bit
still set and the destination unchanged. Neither PMS monitor records a violation
(`SENSITIVE_DMA_APBPERI_PMS_MONITOR_*` for the GDMA's APB side, `SENSITIVE_BACKUP_BUS_PMS_MONITOR_*`
for the backup bus). So it is an address check of the GDMA itself (a destination in peripheral
space is a descriptor error), not a permission: no PMS flag, world or lock changes it. The CPU
writes both registers fine. Experiment 8 on the S3 therefore ends here for the GDMA; the other DMA
of the chip does write registers (next section).

### Decision

**The CPU build of the ACK tail stays.** The tail is built between the frame's end and the ACK
(192 us turnaround, [IEEE 802.15.4](../modem/ieee802154.md)), and the gather alone takes 389 us
with bursts, 469 us without: it cannot finish in time, whatever it overlaps. It would also save
only about 3.2k CPU cycles of 11.9k, because writing the pointers costs 8.7k. A gather would pay
for waveforms built well before they are sent (a burst queued ahead, a frame prepared while the
radio listens), not for the ACK. `wpan bench dma ack` prints the verdict against both the CPU cycles
and the 192 us.

### Open crash

With `CONFIG_ESP_SDR_DMA=y`, the earlier `wpan bench dma sram` (that bench has moved to
`probe dma sram` in rfprobe, which has no `wpan on`, so this cannot be reproduced from wpan now)
followed by `wpan on` resets the DevKitC a few
seconds after "radio on: 0": reset reason 0x7 (TG0WDT_SYS_RST), saved PC `_DoubleExceptionVector`
(`xtensa_asm2_util.S:649`), no fatal dump, garbage on the UART just before. `wpan on` alone and
`wpan bench dma ack` followed by `wpan on` are fine. The cause is not found. Checked by reading:
every chain ends (`next` NULL, EOF on the last source descriptor, sizes exact, the last
destination descriptor holds the rest); sources, destinations and lists are 4-byte aligned and
inside banks 1 and 2 and the image; nothing a chain targets is on a stack; `wait` now waits for
the channel to go idle, `release` stops and resets it; the ring rewrites the ACK head
(`ack_prefix()`) before each run, so bank 2's contents do not matter. Left unexplained: what
`sram` does that `ack` does not (16 KiB random fill of bank 1, chains into bank 2 from word 0, the
overlap rows with interrupts off, the shell stack, 2560 B with `cmd_bench` 240 B and `dma_row` 144
B added). The bench now prints the address ranges and checks canary words after both buffers on
every row. So the option is off by default in `apps/wpan` (`-DCONFIG_ESP_SDR_DMA=y` builds `bench dma ack`
only); the layer and its numbers stay. In rfprobe, which has the option on, `probe dma sram`
followed by `probe f 2440`, `probe rx 4096` and an 8 s `probe tone on` did not reset the DevKitC
(2026-11-24, uptime kept counting): the radio alone does not bring it back, so it needs something
wpan's path adds (the ring, the ACK build into bank 2). Still open.

### Rules learned

- The controller is gated and held in reset after boot; with Zephyr's driver off the layer
  enables the clock and resets the controller when it was not running (first claim).
- A chain into a dump bank must be waited for before the radio may take that bank:
  `esp_sdr_dma_wait()` also waits for the FSM to go idle, `esp_sdr_dma_start()` refuses
  while a chain runs (`-EBUSY`), `esp_sdr_dma_release()` stops and resets the channel.

## The S3 backup DMA (register lists)

Part of the same files (`esp_sdr_bkdma_*`, `include/esp_sdr/esp_sdr_dma.h:148` ff.,
`lib/esp_sdr/src/esp_sdr_dma.c:386` ff., Kconfig `ESP_SDR_DMA`). The S3 has a second DMA that the
GDMA layer does not use: the peripheral backup DMA (the "APB backup DMA"), which the ROM uses to
save and restore peripheral registers around sleep. It copies a list of words between SRAM and the
APB address space and, unlike the GDMA, writes registers.

**Base address, and a hal bug.** The block is at **0x6001A000**. The hal's
`DR_REG_PERI_BACKUP_BASE` was 0x6002A000
(`components/soc/esp32s3/register/soc/reg_base.h:44`); that address reads 0 and ignores writes.
The right one comes from the ROM: `ets_backup_dma_copy` (0x40000624, `esp32s3.rom.ld:30`, a jump to 0x40041804 and from there to the
worker at 0x400416a0) programs 0x6001A000, found by dumping the ROM with `devmem`. The hal
line is fixed in this workspace (the value above); nothing else in the hal or in Zephyr names the
block (no linker symbol, no devicetree node). Without the fix every `PERI_BACKUP_*` macro points
at the wrong place.

**Fields** (`soc/peri_backup_reg.h`, names as the hal has them):

| Register (offset) | Field | Meaning, as measured |
| --- | --- | --- |
| clock | `SYSTEM_PERIP_CLK_EN1_REG` `SYSTEM_PERI_BACKUP_CLK_EN`, reset `SYSTEM_PERIP_RST_EN1_REG` `SYSTEM_PERI_BACKUP_RST` | gated and held in reset after boot; the claim enables and releases them |
| `CONFIG` (0x0), reset value 0x6480 | `ENA` bit 31 | engine enable |
| | `TO_MEM` bit 30 | 0 SRAM to registers, 1 registers to SRAM |
| | `START` bit 29 | write trigger |
| | `SIZE` bits 28:19 | a word count (SIZE 1 wrote one word), at most 1023 |
| | `TOUT_THRES` 18:9 (50), `BURST_LIMIT` 8:4 (8) | kept at the reset values (0x6480) |
| | `ADDR_MAP_MODE` bit 3 | map mode, see below |
| | `FLOW_ERR` bits 2:0 | read only |
| `APB_ADDR` (0x4) | start register | word address of the first register |
| `MEM_ADDR` (0x8) | start SRAM address | |
| `REG_MAP0..3` (0xC to 0x18) | | in map mode a 128-bit mask of register words; the first set bit is `APB_ADDR` (below) |
| `INT_RAW` (0x1C) | `DONE` bit 0, `ERR` bit 1 | cleared through `INT_CLR` (0x28) |

Which registers it reaches is set by `SENSITIVE_BACKUP_BUS_PMS_CONSTRAIN_*` (one 2-bit field per
peripheral, open at reset, the monitor `SENSITIVE_BACKUP_BUS_PMS_MONITOR_*` latches a violation).
Measured to land on 2026-11-24: SRAM into `RTC_CNTL_STORE7`, into the MAC dump engine's
`DUMP_CONFIG_REG` (0x60033d90) and into `GDMA_DATE_REG`.

**The API** (S3 only, behind `CONFIG_ESP_SDR_DMA`, the GDMA backend untouched):

| Call | What it does |
| --- | --- |
| `esp_sdr_bkdma_claim()` / `_release()` | Clock on and reset released, `CONFIG` to its default; refuses a second user (`-EBUSY`). Release puts the clock and reset back as found |
| `esp_sdr_bkdma_prepare(run, apb, mem, words, flags, map)` | Builds the run: the `CONFIG` word (ENA, `TO_MEM`, SIZE, map mode), the two addresses and the map words. `ESP_SDR_BKDMA_TO_MEM`, `ESP_SDR_BKDMA_MAP`; a `mem` in a dump bank is refused like the GDMA's unless `ESP_SDR_DMA_CPU_OWNS_BANKS` |
| `esp_sdr_bkdma_start(run)` | Stores `APB_ADDR`, `MEM_ADDR`, map words and `CONFIG` only where they differ from the last run, then one store of `CONFIG | START`. A repeat of the same run is one store |
| `esp_sdr_bkdma_done()` / `_wait(cycles)` | Poll `INT_RAW`, clear it; `-EIO` on `ERR` or `FLOW_ERR`, `-ETIMEDOUT` after the cycles; a failed wait resets the engine (`esp_sdr_bkdma_reset()`) |
| `esp_sdr_bkdma_forget()` | The next start stores everything again (for measuring the saving) |
| `esp_sdr_dma_arm()`, `_kick_cpu()` | The GDMA side of "kick": everything `esp_sdr_dma_start()` does except the two starts, and the two link words and their register addresses (`struct esp_sdr_dma_kick`) for a run to store |

The ROM entry `ets_backup_dma_copy()` and its lock hook `ets_apb_backup_init_lock_func`
(`esp32s3.rom.ld:31`) are not used. Nothing else in our build uses the block: the hal's
`apb_backup_dma.c` (which installs the hook) is not compiled and `esp_apb_backup_dma_lock_init()`
is never called, and the IDF's one user, the Bluetooth controller, is the C3's. The software claim
is therefore the whole exclusion. Anything that later brings in the IDF's light sleep with CPU
retention must go through the claim (the hook would be the place to take the same lock).

**Benches** (`apps/rfprobe`, `src/dmabench.c`). The register block is RMT RAM (0x60016800, 384
words of plain read/write memory, no side effects, `SENSITIVE_BACKUP_BUS_PMS_CONSTRAIN_RMT`), with
the RMT clock on only while a row runs; its words, `SYS_CONF`, clock and reset are saved and put
back.

| Command | What it measures |
| --- | --- |
| `probe dma regs [n]` | n words (default sweep 1 to 384) SRAM to RMT RAM and back: CPU cycles of the start call, START to DONE (cold: every register stored; warm: the same run again), cycles per word, marginal cycles per word, words landed |
| `probe dma regs mac` | one word into `DUMP_CONFIG_REG` through the API (radio initialised once, MAC clock on, dump engine idle, value restored) |
| `probe dma map` | `ADDR_MAP_MODE` and `REG_MAP0..3`: which register words got which SRAM words, and the reverse |
| `probe dma kick` | the backup DMA stores a GDMA chain's `IN_LINK` and `OUT_LINK` (address and START in one word each) |

**Map mode, measured** (`probe dma map`, RMT RAM). `REG_MAP0..3` is a 128-bit mask over register
words, bit k of `REG_MAPj` being word 32j + k. The first set bit is the register at `APB_ADDR`;
every further set bit is a register at its distance from the first. The SRAM side is packed, one
word per set bit, in order. `SIZE` must equal the number of set bits: with more, the extra words
land at offsets that make no sense (9 and 11 words on, in two rows).

| Mask (`REG_MAP0`, `REG_MAP3`) | `SIZE` | Register words written, from `APB_ADDR` |
| --- | --- | --- |
| 0x5 (bits 0, 2) | 2 | 0, 2 |
| 0x5 | 1 | 0 |
| 0xa (bits 1, 3), registers to SRAM | 2 | 0, 2 read into SRAM words 0, 1 |
| 0x80000000 and bit 96 | 2 | 0, 65 (96 minus 31) |
| 0xffffffff | 16, 32 | 0 to 15, 0 to 31 |
| 0x5 with `APB_ADDR` 16 words on | 2 | 16, 18 |
| 0x5 | 3 | 0, 2, 11 (too large a `SIZE`) |
| 0 | 2 | 0, 9 (too large a `SIZE`) |

So a map run reaches up to 128 registers in a 512-byte window, in address order, from one packed
SRAM list. Registers in different blocks need one run each.

**Measured numbers** (DevKitC, 240 MHz, interrupts locked, CPU cycles; target RMT RAM unless named;
"cold" stores every register of the run, "warm" repeats the same run, which is one `CONFIG` store):

| Row | Cycles |
| --- | --- |
| `regs` 1 word, start call: first run after boot, cold, warm | 2093, 405 (to SRAM 849), 400 |
| `regs` 1 to 16 words, START to DONE | 409 to 413, the same for any size up to 16 |
| `regs` 64, 128, 256, 384 words to registers, START to DONE (warm) | 1197, 2085, 3861, 5637 (13.7 a word beyond the first) |
| `regs` 64, 128, 256, 384 words to SRAM, START to DONE (warm) | 1095, 1791, 3183, 4575 (10.9 a word beyond the first) |
| `regs mac` 1 word into `DUMP_CONFIG_REG`: start call, START to DONE (cold; warm) | 1595, 2049; 688, 697 |
| A warm start (one `CONFIG` store) is enough | yes: every warm row landed n of n, `ENA` and both addresses survive a run |
| `kick`, 4092 B chain: `esp_sdr_dma_start()` | 474 in the call; GDMA EOF after 18999 (79 us) |
| `kick`: the same two link words stored by the CPU (`esp_sdr_dma_arm()` before, `_kick_cpu()`) | 22 in the call; EOF after 18599 (77 us) |
| `kick`: two backup runs, `IN_LINK` then `OUT_LINK` | 127 in the calls; backup DONE after 320; EOF after 18904; data arrived |
| `kick`: two backup runs, `OUT_LINK` first | 127; 320; 18649; data arrived |
| `kick`: one map run (bits 0 and 24: `IN_LINK` 0x6003f320, `OUT_LINK` 0x6003f380) | 99; 150; 18726; data arrived |

Reading them:

- The start call into RMT RAM is about 400 cycles even warm, for one `CONFIG` store: the CPU's
  store does not complete until the run has moved its first words (up to 16 words fit in that
  time). Runs into the GDMA's registers take 99 to 127 cycles in the start calls and are done
  after 150 to 320. Not explained; it depends on the block written, not on the library (the
  timed window starts after a fence).
- Past the fixed cost a run moves a word in 11 to 14 cycles (68 to 87 MB/s), about what a CPU
  store to a GDMA register costs (11 cycles, from the `kick` row).
- For a GDMA start the CPU's own two stores (22 cycles) beat any backup run. The saving in that
  row is elsewhere: `esp_sdr_dma_start()` spends 474 cycles on work that `esp_sdr_dma_arm()` can do
  ahead, leaving the 22-cycle kick.
- Both orders of the link starts delivered the chain whole here (RX first stays the safe order).

**What it gives continuous dump and replay on the S3.** A list of register writes (bank switch,
dump trigger, a GDMA link start) is prepared once in SRAM and fired by one CPU store, with the CPU
free until it wants the result. The S3 has no ETM and no REGDMA, and nothing starts a DMA from a
hardware event: the GDMA cannot write its own `START` (the register is out of its reach, above) and
the backup DMA is started by a store to its `CONFIG`. So a run cannot be fired by the radio, a
timer or a GPIO: something on a CPU has to do the one store, at the right time (CPU 1 already
does, `wait_until_locked()` in `esp_sdr_tx_dac.c`). What the DMA saves is the work after that
store: the several register writes of a step, and the GDMA setup (`kick`).

The measured costs set where that pays. A run costs about 400 cycles before its first words land
and 11 to 14 a word after that, while the CPU's own stores to GDMA registers cost about 11 each.
So a step of a few register writes is cheaper from the CPU (pre-arm the chain with
`esp_sdr_dma_arm()`, then `esp_sdr_dma_kick_cpu()`). The backup DMA pays for a long list (a
reconfiguration of dozens of registers, a snapshot of a register block into SRAM) that a CPU on
a tight loop should not wait for, and it runs while the CPU does other work. For continuous dump
and replay on the S3 the per-step trigger therefore stays a few CPU stores on CPU 1, fired from
its loop, with the chains prepared ahead.

Open: why the CPU's `START` store into a run on RMT RAM takes about 400 cycles, and what a store
to the MAC dump registers costs the CPU directly (the comparison for a bank switch).

## Starting and stopping transfers

The movers run on their own once started. What has to be on time is the start and the stop.

- **C5, C6, P4, S31:** ETM can start or stop GDMA channels and REGDMA lists from timer and GPIO
  events with no CPU. The task ids are in `soc_etm_source.h` per chip (table above).
- **S3:** no ETM and no REGDMA. CPU 1 already starts transfers on a cycle with interrupts locked
  (`wait_until_locked()` in `esp_sdr_tx_dac.c`, `burst_run()` in `esp_sdr_ring.c`). A shared
  `esdr_seq` with "at cycle t, write register r" entries is the only sequencer the S3 needs; the
  [backup DMA](#the-s3-backup-dma-register-lists) can execute the register part of an entry (one
  store fires a prepared list), the "at cycle t" stays a CPU's.
- Branching on a CRC or waiting on ring positions stays in the apps that need it until a second
  user appears.

## Experiments, in order

| # | Experiment | Chip, board | Done when | Status |
| --- | --- | --- | --- | --- |
| 1 | `esdr_dma_ring` extracted from diag16 and diag, no behaviour change | S31, C5 | `tests/regression/stream_gapless` and osmosdr_hs 300 s at 20 MS/s unchanged | deferred: needs S31 and C5 |
| 2 | osmosdr_hs with `esdr_dma_ring_swap()` instead of `memcpy()` (slabs of 8 nodes) | S31 | 40.157 MB/s for 300 s with no bad block; CPU time per second down by the copy's share (`esp_sdr_prof`) | deferred: needs the S31 |
| 3 | C6 GDMA memory-to-memory: SRAM to SRAM throughput, and reading a block the engine just released while it writes the other | C6 (badge) | MB/s; data matches a CPU copy; markers unchanged | deferred: needs the C6 badge |
| 4 | C6 gapless with the gather beside the CPU copy | C6 and P4 | longest whole burst over 50 us; an 802.15.4 ACK decoded whole on the P4 | deferred: needs the C6 badge and P4 |
| 5 | S3 GDMA gather of the 802.15.4 ACK tail into bank 2 | S3 | bit-identical bank to the CPU's; CPU 0 cycles for the tail under 10k; ACK late count not worse | measured 2026-11-24: bit-exact, but 389 us against a 192 us turnaround: the CPU build stays (see [Decision](#decision)) |
| 6 | REGDMA spike: one write node to a GPIO register, then to the TX DC value register, looped, paced by the read interval and by ETM | C6 | write rate and jitter on a logic analyser or in a capture; a tone heard | deferred: needs the C6 |
| 7 | REGDMA backup of the estimator sums | C6 | samples equal to the CPU-paced ones at 250 kS/s | deferred: needs the C6 |
| 8 | GDMA memory-to-memory into a peripheral register | C6, S3 | writes land, or a PMS/APM fault is recorded | S3 measured 2026-11-24: refused by the GDMA itself (IN_DSCR_ERR, no PMS record), see [Experiment 8](#experiment-8-the-gdma-cannot-write-a-register); the backup DMA writes registers (`probe dma reg`, `probe dma regs`); C6 part deferred: needs the C6 |
| 9 | S3 backup DMA: n words SRAM to registers and back, map mode, starting a GDMA chain | S3 | cycles per word, whether the list can start the GDMA | measured 2026-11-24: 11 to 14 cycles a word after about 400 a run; map mode decoded; one map run starts a GDMA chain, but two CPU stores do it in 22 cycles (see [the section](#the-s3-backup-dma-register-lists)) |

The badge and the P4 board are not in `tests/regression/hardware-map.yaml` (it lists the XIAO C6, the C5-Zero, the S31 board and the two S3 boards), so experiments 3 and 4 also need a map entry.

## Open questions

- Whether the S3 or the C6 modem has a diagnostic bus the repo has not found. With one, PARLIO
  (C6) or LCD_CAM (S3) plus a GDMA ring would give them the C5's or S31's gapless path.
- GDMA throughput, memory-to-memory, on each chip.
- REGDMA's pacing and its reach into the modem registers.
- The S3 backup DMA: why a `START` store into a run on RMT RAM costs the CPU about 400 cycles;
  the C6's answer to experiment 8.
- The C6 dump engine's "four-slot mode", mentioned under "Chasing the engine" in
  [Rings](../samples/rings.md). If it takes several bank addresses, it is the closest the engine
  comes to a descriptor list.

<!-- nav-start -->
Previous: [ZigBit reference](zigbit.md) · Up: [Reference](index.md) · Next: [Hot loops](hot-loops.md)
<!-- nav-end -->
