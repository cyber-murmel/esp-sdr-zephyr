# Code placement

<!-- nav-start -->
Previous: [Chip notes](chips.md) · Up: [Reference](index.md) · Next: [Testing](testing.md)
<!-- nav-end -->

Where a function runs from and where a buffer lives decides, on these chips, whether a paced loop
keeps up. The firmware therefore says how its code and data are used, and rules per SoC, board and
app say where that goes. Code moves by a change of the rules, one at a time, each measured.

This page follows [Chip notes](chips.md), which lists the CPU costs that placement works around.
It explains the memory regions first, then the categories and rules that pick them, then the tools
that check and measure a placement.

## Why placement matters

On the ESP32-S3, flash code and PSRAM code run through one instruction cache, which both CPUs
share:

| Instruction cache | ESP32-S3 |
|---|---|
| Size | 16 KB |
| Ways | 8 ways of 2 KB |
| Line | 32 bytes |

- IRAM, the DRAM image, the internal RAM above the receive banks and PSRAM differ in size, speed
  and in what may live there.
- Hot code belongs in IRAM (`IRAM_ATTR`): flash code goes through the cache, which the other core
  and PSRAM traffic contend for.
- Ubertooth showed how much it matters. Its hottest flash code, gathered into one 12 KB block
  aligned to the cache's way, cut the stream lost while following a connection from 0.062 % to
  0.0007 to 0.0025 % ([ubertooth internals](../apps/ubertooth-internals.md#the-hot-block)).

## Regions

Each region expands to the attribute such code or data carried before. So the default rules keep
every section name, and multiapp's extension builds (`multiapp/ext.h`) still turn them into the
loader's sections.

### Region table

| Region | Expands to | Where it is |
| --- | --- | --- |
| `ESP_SDR_IRAM` | `IRAM_ATTR` (`.iram1.<n>`) | S3 and ESP32 IRAM, S31 TCM, C5 and C6 SRAM, P4 L2MEM; an extension's `.module_fast` |
| `ESP_SDR_HOTBLOCK` | `.text.esp_sdr_hot.<n>` | The S3's hot block ([The hot block](#the-hot-block)); flash elsewhere |
| `ESP_SDR_FLASH` | nothing | Flash text |
| `ESP_SDR_DRAM` | `DRAM_ATTR` | The DRAM image, data |
| `ESP_SDR_DEFAULT` | nothing | `.data` and `.bss` |
| `ESP_SDR_HIGH` | `ESP_SDR_HIGH_RAM` (`.esp_sdr_high`, one per file), its buffer and stack forms | S3 internal RAM above the banks ([SRAM bank ownership](../samples/index.md#sram-bank-ownership)), C6 block 3, C5 and ESP32 DRAM; an extension's host area |
| `ESP_SDR_PSRAM` | `EXT_RAM_BSS_ATTR` | S3 and ESP32 PSRAM; nothing on the others, whose HAL leaves it empty |
| `ESP_SDR_PSRAM_FILE` | `.ext_ram.bss`, one per file | PSRAM on the P4 and the S31 |
| `ESP_SDR_NOINIT`, `ESP_SDR_PSRAM_NOINIT` | `__noinit`, `.ext_ram_noinit.<n>` on the S31 (its `EXT_RAM_NOINIT_ATTR` is internal RAM; `ESP_SDR_PSRAM_NOINIT_ATTR`, also for `ESP_SDR_BUF_DEFINE()` and `ESP_SDR_STACK_DEFINE()`) | Not zeroed at boot |
| `ESP_SDR_LPRAM` | `.rtc_noinit.<n>` (`ESP_SDR_LPRAM_ATTR`) | The S31's LP RAM, 0x2E000000, about 30 KB the image does not use: small cold data. Not zeroed, no DMA, no atomics (the flags `DMA` and `ATOMIC` stop the build), slower than TCM from the HP core (not measured). `apps/txstream_eth` keeps its control thread's state there |

`IRAM_ATTR` has no `noinline`: a tagged function the compiler inlines runs where its caller does.
[The report](#the-report) lists the tags that left no symbol.

### ESP32-S3 internal RAM

Internal RAM is the scarce resource.

- Besides the banks, the region above the capture bank (`.esp_sdr_high`, `ESP_SDR_HIGH_RAM`) holds
  `apps/link`'s QAM transmit context and OFDM receive context, with about 128 bytes left.
- The build message "high buffers beyond user DRAM" means it is full.
- Large buffers that are not timing critical go to PSRAM (`EXT_RAM_BSS_ATTR`).

#### Past user_dram_end

- Zephyr's `user_dram_end` (0x3fce4f00) only protects MCUboot's loader while the image loads.
- The ROM's download-mode buffers above it are free at runtime, so NOLOAD buffers can use the
  whole range, about 38 KiB.

#### Three banks for the ring

- Three banks for the ring move the DRAM limit to 0x3fcb0000.
- [osmosdr](../../apps/osmosdr/README.rst) and [wpan](../../apps/wpan/README.rst) fit by trimming
  the network stack and moving buffers and stacks into the high region.
- The Wi-Fi driver peaks at about 15.5 KiB of kernel heap with its default receive buffers,
  12 KiB with 2 ([multiapp internals](../apps/multiapp-internals.md#memory-budget)).

#### Not zeroed at boot

- Neither `.esp_sdr_high` nor PSRAM `.bss` is cleared at boot.
- `apps/link` clears what needs it at init (`link_mac_init()`).

### IRAM must be internal RAM

The S3's ROM calls PHY code, also with the cache off.

- The ROM's analog I2C goes through a table the PHY blob patches, to `ram_chip_i2c_readReg` in the
  blob's `.iram1`.
- `esp_restart_noos()` reaches it through `rtc_clk_cpu_set_to_default_config()` with the cache
  disabled.
- Blob `IRAM_ATTR` code in PSRAM ended a reboot with the radio up in an illegal instruction, then a
  double exception, then reset reason 16.
- So `IRAM_ATTR`, which `ESP_SDR_IRAM` expands to, really has to mean internal RAM, in a loaded
  module as well. Code that runs while the cache is off is the `NOCACHE` category
  ([Code](#code)).

### ESP32-S3 PSRAM

#### Slow for strided work

- PSRAM is slow for strided or per-sample work.
- RS units and payload bytes may live there.
- Hot buffers should not: FFT work buffers, the RS codeword being decoded (which goes on the
  stack).

#### Compare-and-set is not atomic

- S32C1I on external RAM lets both CPUs succeed: 2 x 200000 `atomic_inc()` from the two CPUs on
  one PSRAM counter lost counts.
- A spinlock, `k_msgq`, `k_mem_slab` or `atomic_t` in PSRAM breaks under SMP.
- Espressif's HAL emulates compare-and-set (`esp_cpu_compare_and_set()`), and so does the Zephyr
  branch `west.yml` pins ("soc: espressif: esp32s3: emulate atomics in PSRAM"): every atomic in
  the PSRAM range goes through a lock in internal RAM.
- The `ATOMIC` flag keeps such objects out of PSRAM ([Flags and checks](#flags-and-checks)).

#### No flash access from a PSRAM stack

- A flash read or write (the flash API, so also MCUboot's `boot_is_img_confirmed()`) parks the
  other CPU and suspends the cache, and PSRAM goes with it.
- The thread's register window spills read back as 0xbad00add, and it double-faults with the
  other CPU still parked.
- Before the watchdog is armed (at boot, before USB starts) the board then hangs for good.
- A multiapp app's `main()` runs on a PSRAM stack. That is why the prelude turns its
  `app_usb_init()` into a call that does nothing
  ([multiapp internals](../apps/multiapp-internals.md#the-prelude)).

### ESP32-C5 image split

On the C5 the capture engine owns all of SRAM block 1
([SRAM bank ownership](../samples/index.md#sram-bank-ownership)), so the image is split around
it. `esp_sdr_bank_c5.ld` is a `RAM_SECTIONS` snippet:

| Block | What links there |
|---|---|
| 0, below 0x40820000 | code and initialized data; the network stack's and the Wi-Fi driver's `.noinit`, to balance the halves |
| 1 | the capture bank |
| 2 | every section linked after the snippet: logging and kernel object data, the rest of `.noinit`, `.bss`, the libc heap |

Margins with the Wi-Fi driver's defaults:

| App | Left in block 0 | libc heap in block 2 |
|---|---|---|
| `apps/capture` | 8.3 KiB | 21 KiB |
| `apps/rfprobe` | 4.9 KiB | 20 KiB |

| Link error | Meaning |
|---|---|
| "code and data overlap the capture bank" | block 0 is full |
| "will not fit in region `sram0_0_seg`" | block 2 is full |

## Usage categories

`include/esp_sdr/esp_sdr_place.h` (Doxygen group `esp_sdr_place`) defines the categories. The
macros name a use, never a place.

### Code

`ESP_SDR_CODE(cat, fn)` stands before the definition of function `fn`:

```c
ESP_SDR_CODE(PACED_SAMPLE, fir_feed)
static __attribute__((optimize("O3"))) void fir_feed(const uint32_t *p, uint32_t a, uint32_t m)
```

| Category | Use |
| --- | --- |
| `PACED_SAMPLE`, `PACED_CHUNK`, `PACED_EVENT` | On a paced path, where a stall loses samples or misses a deadline (the receive ring and its sinks, DMA interrupts, transmit feeders, an ACK burst): per sample, per chunk or unit, per burst |
| `SAMPLE`, `CHUNK`, `EVENT` | A consumer with slack behind a buffer: per sample, per chunk or poll, per packet |
| `COLD` | Init, shell, logging, errors: always flash, never in the hot block |
| `NOCACHE` | Runs while the cache is off (the ring's park loop, the double exception vector): always IRAM, no rule moves it |

- An assembler function's `.section` line is `ESP_SDR_ASM_SECTION(cat, fn)`.
- Attributes for the code generator (`noinline`, `optimize("O3")`, `hot`) stay at the site.
- A file may wrap both in a macro of its own:
  `#define HOT(cat, fn) ESP_SDR_CODE(cat, fn) __attribute__((optimize("O3")))`.

### Data

| Macro | Use |
|---|---|
| `ESP_SDR_DATA(cat, name)` | on a definition |
| `ESP_SDR_DATA_F(cat, name, flag)`, `ESP_SDR_DATA_F2(cat, name, flag, flag)` | the same, with constraints |
| `ESP_SDR_BUF_DEFINE(cat, type, name, dims, align, flag)` | defines an aligned buffer |
| `ESP_SDR_STACK_DEFINE(name, size)` | defines a kernel thread stack (category `STACK`) |

```c
static struct esp_sdr_iq16 burst[ESP_SDR_SAMPLES_MAX] ESP_SDR_DATA(BULK, burst);
static ESP_SDR_DATA_F2(STATE, sk, NOINIT, ATOMIC) struct { struct k_sem go, done; } sk;
ESP_SDR_STACK_DEFINE(esdr_rx_stack, 2560);
```

| Category | Use |
| --- | --- |
| `STATE` | Read and written per sample or chunk |
| `TABLE` | Read-only, read per sample |
| `BULK` | Captures, histories, profiler samples, data touched only now and then |
| `SCRATCH` | Reused between calls |
| `STACK` | Thread stacks |

### Flags and checks

| Flag | Checked against the region the rules chose |
| --- | --- |
| `DMA` | Internal RAM only |
| `ATOMIC` | Never PSRAM: kernel objects, spinlocks, atomics |
| `NOINIT` | The code initializes the object itself; needed in a region not zeroed at boot (the high region, the noinit sections) |

- A clash stops the build with `esp_sdr_place: ...`, a `GCC error` pragma, so in assembler files
  too.
- With two flags, `NOINIT` comes first.
- `RTC_NOINIT_ATTR` data, the RP2040's `__ramfunc` and the multiapp loader tests
  (`apps/multiapp/ext/hello*.c`) keep their attributes.
- Builds for other targets (native_sim) place nothing.
- `tests/host/test_place_lint.py` fails on a placement attribute or section named anywhere else:
  outside `include/esp_sdr/esp_sdr_place.h` and these exceptions ([Testing](testing.md#host-tests)).

## Rules

Rules are C headers. The build generates `include/generated/esp_sdr_place_rules.h`
(`lib/esp_sdr/place.cmake`), which includes those that exist; the first definition counts.

### Lookup order

1. `apps/<app>/boards/<normalized board target>.place.h`
2. `apps/<app>/place.h`
3. `lib/esp_sdr/place/boards/<normalized board target>.h`
4. `lib/esp_sdr/place/<SoC series>.h`

`apps/<app>` is `APPLICATION_CONFIG_DIR`. So a regression test that builds an app from its
directory (`tests/regression/*`) gets the app's rules too; before 2026-11-15 it did not.

### Pins and scopes

- A rule gives a category its region: `#define ESP_SDR_PLACE_<cat> <region>`.
- A pin gives one function or object its own region: `ESP_SDR_PIN_CODE_<fn>` or
  `ESP_SDR_PIN_DATA_<name>`.
- `ESP_SDR_RULE` takes a lower level's pin back.

Both rules and pins can be narrowed:

- To a scope, the compile definition `ESP_SDR_PLACE_SCOPE` of an app's sources
  (`target_compile_definitions(app PRIVATE ESP_SDR_PLACE_SCOPE=ubertooth)`):
  `ESP_SDR_PLACE_<scope>_<cat>`, `ESP_SDR_PIN_CODE_<scope>_<fn>`. multiapp builds each extension
  with its name as the scope, and its `place.h` includes the apps', so each keeps its rules.
- To a file that defines `ESP_SDR_PLACE_FILE` ahead of its tags (static names repeat across
  files): `ESP_SDR_PIN_DATA_<file>_<name>`.

A tag is looked up in this order: the file's pin, the scope's pin, the pin, the scope's rule, the
rule.

### Pins kept as before

The SoC headers hold the categories as the code was placed before them. Where a function or buffer
sat elsewhere than its category says, a pin marked "as before" keeps it there:

- the receive sinks that run per sample from flash;
- the transmit DAC's player;
- Ubertooth's per-poll chain in IRAM;
- the link's OFDM, QAM and BCH split;
- MicroPython's PSRAM stacks.

Each such pin is a candidate for a measured change.

## Tools

The tools live in `scripts/place/`.

### The report

Each build writes `zephyr/place_report.txt` (`scripts/place/place_report.py`, a post-build step;
`-DESP_SDR_PLACE_REPORT=OFF` leaves it out). It holds:

- bytes per memory region;
- the room left below the banks and in the high region;
- the hot block against its budget;
- from the build's `compile_commands.json`, every tag with the symbols it became, as bytes per
  category and region.

```text
  categories, bytes by region (symbols):
    BULK          PSRAM 4586588 (11)
    SAMPLE        flash code 3128 (3)
    STACK         PSRAM 9216 (2)
  tagged, with no symbol (inlined, or left out by #if): 3
    ieee154_phy.c: EVENT crc16
```

#### Tags without a symbol

- A tag with no symbol was inlined (or not compiled); one whose symbol is in its object but not in
  the image was removed by the linker.
- For each such code tag the report adds the callers DWARF says it was inlined into, with their
  regions, and the region the rules chose for it (`ESP_SDR_REGION_CODE()`, preprocessed with the
  source's compile command).
- A caller elsewhere is marked `!`, since inlined code runs where its caller does.
- Copies in callers the linker removed are left out, as they never run.
- Tags in code the build leaves out by `#if` are listed by themselves (the source preprocessed
  with its compile command). The function of the same name in the other branch is not theirs, as
  link's esp-dsp FFT wrapper is not its C FFT.

#### Untagged objects and extensions

- Last come the repository's own functions and objects of 1 KB or more without a tag
  (`--untagged`), which should be none, or explained.
- A multiapp extension's tags are counted from its objects by input section.
- A buffer the host places for an extension shows as in the host's area.

### Proving that nothing moved

`scripts/place/place_diff.py record|diff|tree` compares where two images put their code and data.
It proves a change moved nothing, or names what moved.

- It compares the allocated output sections, and every symbol's address, size and section, local
  symbols keyed by their source file.
- The baseline and the build under test are built at the same path, since path lengths reach
  rodata (`-fmacro-prefix-map`). One board built twice comes out identical.
- `--allow-size REGEX` lets named functions change size while everything behind them shifts by the
  same amount, order and sizes unchanged.
- multiapp needs that: its build id hashes every header under `include/`, and its bytes are
  immediates in `check_trailer()` and `cmd_info()`.

## Measuring

### The other CPU's profile

`CONFIG_ESP_SDR_CPU_PROF` (`esp_sdr_prof.h`) samples one CPU from the other.

- While the receive ring runs, its CPU reads the other CPU's program counter (the debug assist's
  PC recorder) and current thread at a fixed period, into PSRAM.
- The sampled CPU takes no interrupt and runs no code for it. The code it runs with interrupts
  masked is sampled too; there are no stacks.
- `prof start [period_us] | stop | dump` drives it (apps/common).
- A sniffing capture took 131072 samples at 64 us over 8.8 s with none missed.
- The dump has the layout of perf's `printbuf`: each sample is its program counter under a frame
  that stands for its thread (the thread's entry function; the table before the samples names the
  threads). So Zephyr's `scripts/profiling/stackcollapse.py` reads it too.
- 131072 samples are 7 MB, 11 s over CDC-ACM: with the `prof` shell, apps/common raises the
  shell's transmit buffer from 8 bytes (46 s) to 1 KB.

### Zephyr's perf

`subsys/profiling/perf` (`CONFIG_PROFILING` and `CONFIG_PROFILING_PERF`) samples the interrupted
stack in the timer interrupt on CPU 0 and, through IPIs, on the other CPU (`perf record`,
`perf printbuf`).

- The zsdr fork adds an Xtensa backend (`PROFILING_PERF_BACKEND_XTENSA`) and a CPU mask:
  `perf record <ms> <Hz> [thread [cpus]]`.
- `perf record 3000 1000 0 1` samples CPU 0 alone and sends the ring's CPU no IPI. Why IPIs
  matter to the ring is in [Gapless receive](../samples/rings.md#interrupts-inside-the-ring).
- With Ubertooth's ring at 4 MS/s, in runs of 3 s at 10 kHz:

| perf | Runs that abandoned a ring unit |
|---|---|
| sampling both CPUs | every run (6 to 10 pairs) |
| sampling CPU 0 alone | 3 of 7 (5 to 11 pairs) |
| no perf | none of 6 |

- perf prints the end of a recording from the system work queue, where a CDC-ACM shell transmits
  too unless `CONFIG_USBD_CDC_ACM_WORKQUEUE` gives it a queue of its own.
- Without it the print waits for itself until the liveness watchdog resets the board, so
  apps/common turns that option on in builds with perf.

### Functions and the cache

- `scripts/place/prof_map.py <elf> <capture>...` maps either capture (`prof dump` or perf's
  `printbuf`) to functions, samples per byte and section, or to collapsed stacks for a flame
  graph.
- `scripts/place/icache.py <port>` reads the ESP32-S3 I-cache's access and miss counters through
  Zephyr's devmem shell (`CONFIG_DEVMEM_SHELL`), window by window.

## The hot block

`CONFIG_ESP_SDR_HOT_TEXT` (ESP32-S3) gathers an image's hottest flash code into one block of flash
text aligned to the cache's way (`lib/esp_sdr/hot_text.ld.in`). Its lines keep their cache sets
whatever code comes before or after.

### Contents and budget

- The block holds the app's list, `esp_sdr_hot_text(<list file>)` in its `CMakeLists.txt`, then
  the code its rules place in `ESP_SDR_HOTBLOCK`.
- `CONFIG_ESP_SDR_HOT_TEXT_BUDGET` (12 KB, text and literals) limits it, so that it takes 6 of the
  8 ways of each set and leaves 2 to the rest.
- The list is linker script statements per object that claim input sections from the flash
  text's catch-all.
- The Xtensa linker pools their literals at the block's start, and any label or assignment
  between them would move those pools.

### Linker hook

- The block needs the TEXT_SECTIONS hook of the ESP32-S3 linker script (Zephyr commit "soc:
  espressif: let applications place code in esp32s3 flash text" in the zsdr fork).
- CMake stops without it, and the link stops (`hot_text_check.ld`) should the block not reach the
  image.

### Making the list

`scripts/place/hot_text.py <build> <list file> <capture>...` writes the list from profiles of the
image ([Measuring](#measuring)).

- It takes the flash functions sampled, densest first (samples per byte), while their text and
  literals (sizes from the linked map) fit the budget less what the rules place there.
- Functions tagged `COLD` never go in.
- Profiles of the time on average are not always the right input. Ubertooth's stream losses come
  in bursts, when the USB driver's code thrashes the cache.
- A list made from fresh profiles of Ubertooth's image without the Wi-Fi driver lost five times as
  much as the list it was to replace
  ([ubertooth internals](../apps/ubertooth-internals.md#the-hot-list-remade)).
- Every list is an A/B on the board.

## Moving code

1. Change one rule or pin, in the app's `place.h` where the move concerns one app.
2. Build, and compare with `place_diff.py` and the report: only the intended code moved, and the
   regions it left and entered have the room.
3. Measure on the board against the image before, interleaved and repeated: the app's own figure
   (stream loss, skipped frames, cycles per sample) and the cache counters.
4. Keep the change only if it is no worse within the spread.

## multiapp extensions

The categories reach an extension's sections through `multiapp/ext.h`. How the loader places
them is in [multiapp internals](../apps/multiapp-internals.md#modules).

| Region | In an extension |
|---|---|
| `ESP_SDR_IRAM` | `.module_fast` |
| PSRAM and the rest | the loader's heaps |
| the high region's buffers and stacks | the host's area |

A hot block inside an extension is out of scope: the loader places modules at load time, and
`sdr` and `ieee154` build without `-ffunction-sections`.

<!-- nav-start -->
Previous: [Chip notes](chips.md) · Up: [Reference](index.md) · Next: [Testing](testing.md)
<!-- nav-end -->
