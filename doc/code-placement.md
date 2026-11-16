# Code and data placement

Where a function runs from and where a buffer lives decides, on these chips, whether a paced loop
keeps up. On the ESP32-S3 flash code and PSRAM code run through one instruction cache of 16 KB, 8
ways of 2 KB with 32 byte lines, which both CPUs share; IRAM, the DRAM image, the internal RAM above
the receive banks and PSRAM differ in size, speed and in what may live there.

The firmware therefore says how its code and data are used, and rules per SoC, board and app say
where that goes. Code moves by a change of the rules, one at a time, each measured.

## Usage categories

`include/esp_sdr/esp_sdr_place.h` (Doxygen group `esp_sdr_place`). The macros name a use, never a
place.

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

An assembler function's `.section` line is `ESP_SDR_ASM_SECTION(cat, fn)`. Attributes for the code
generator (`noinline`, `optimize("O3")`, `hot`) stay at the site; a file may wrap both in a macro of
its own (`#define HOT(cat, fn) ESP_SDR_CODE(cat, fn) __attribute__((optimize("O3")))`).

### Data

`ESP_SDR_DATA(cat, name)` on a definition, `ESP_SDR_DATA_F(cat, name, flag)` and
`ESP_SDR_DATA_F2(cat, name, flag, flag)` with constraints; `ESP_SDR_BUF_DEFINE(cat, type, name,
dims, align, flag)` defines an aligned buffer and `ESP_SDR_STACK_DEFINE(name, size)` a kernel
thread stack (category `STACK`):

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

| Flag | Checked against the region the rules chose |
| --- | --- |
| `DMA` | Internal RAM only |
| `ATOMIC` | Never PSRAM: kernel objects, spinlocks, atomics |
| `NOINIT` | The code initializes the object itself; needed in a region not zeroed at boot (the high region, the noinit sections) |

A clash stops the build with `esp_sdr_place: ...` (a `GCC error` pragma, so in assembler files too).
With two flags `NOINIT` comes first. `RTC_NOINIT_ATTR` data, the RP2040's `__ramfunc` and the
multiapp loader tests (`apps/multiapp/ext/hello*.c`) keep their attributes; builds for other
targets (native_sim) place nothing. `tests/host/test_place_lint.py` fails on a placement attribute
or section named anywhere else.

## Regions

Each region expands to the attribute such code or data carried before, so the default rules keep
every section name.

| Region | Expands to | Where it is |
| --- | --- | --- |
| `ESP_SDR_IRAM` | `IRAM_ATTR` (`.iram1.<n>`) | S3 and ESP32 IRAM, S31 TCM, C5 and C6 SRAM, P4 L2MEM; an extension's `.module_fast` |
| `ESP_SDR_HOTBLOCK` | `.text.esp_sdr_hot.<n>` | The S3's hot block (below); flash elsewhere |
| `ESP_SDR_FLASH` | nothing | Flash text |
| `ESP_SDR_DRAM` | `DRAM_ATTR` | The DRAM image, data |
| `ESP_SDR_DEFAULT` | nothing | `.data` and `.bss` |
| `ESP_SDR_HIGH` | `ESP_SDR_HIGH_RAM` (`.esp_sdr_high`, one per file), its buffer and stack forms | S3 internal RAM above the banks, C6 block 3, C5 and ESP32 DRAM; an extension's host area |
| `ESP_SDR_PSRAM` | `EXT_RAM_BSS_ATTR` | S3 and ESP32 PSRAM; nothing on the others, whose HAL leaves it empty |
| `ESP_SDR_PSRAM_FILE` | `.ext_ram.bss`, one per file | PSRAM on the P4 and the S31 |
| `ESP_SDR_NOINIT`, `ESP_SDR_PSRAM_NOINIT` | `__noinit`, `EXT_RAM_NOINIT_ATTR` | Not zeroed at boot |

`IRAM_ATTR` has no `noinline`: a tagged function the compiler inlines runs where its caller does.
The report below lists the tags that left no symbol.

## Rules

Rules are C headers. The build generates `include/generated/esp_sdr_place_rules.h`
(`lib/esp_sdr/place.cmake`), which includes those that exist, the first definition counting:

1. `apps/<app>/boards/<normalized board target>.place.h`
2. `apps/<app>/place.h`
3. `lib/esp_sdr/place/boards/<normalized board target>.h`
4. `lib/esp_sdr/place/<SoC series>.h`

`apps/<app>` is `APPLICATION_CONFIG_DIR`, so a regression test that builds an app from its
directory (`tests/regression/*`) gets the app's rules too (before 2026-11-15 it did not).

A rule gives a category its region, `#define ESP_SDR_PLACE_<cat> <region>`; a pin gives one function
or object its own, `ESP_SDR_PIN_CODE_<fn>` or `ESP_SDR_PIN_DATA_<name>`, and `ESP_SDR_RULE` takes a
lower level's pin back. Both can be narrowed:

- to a scope, the compile definition `ESP_SDR_PLACE_SCOPE` of an app's sources
  (`target_compile_definitions(app PRIVATE ESP_SDR_PLACE_SCOPE=link)`):
  `ESP_SDR_PLACE_<scope>_<cat>`, `ESP_SDR_PIN_CODE_<scope>_<fn>`;
- to a file that defines `ESP_SDR_PLACE_FILE` ahead of its tags (static names repeat across
  files): `ESP_SDR_PIN_DATA_<file>_<name>`.

Lookup: the file's pin, the scope's pin, the pin, the scope's rule, the rule.

The SoC headers hold the categories as the code was placed before them. Where a function or buffer
sat elsewhere than its category says, a pin marked "as before" keeps it there: the receive sinks
that run per sample from flash, the transmit DAC's player, the
link's OFDM, QAM and BCH split. Each such pin is a candidate for a
measured change.

## Proving that nothing moved

`scripts/place/place_diff.py record|diff|tree` compares where two images put their code and data:
the allocated output sections, and every symbol's address, size and section, local symbols keyed by
their source file. The baseline and the build under test are built at the same path, since path
lengths reach rodata (`-fmacro-prefix-map`); one board built twice comes out identical.
`--allow-size REGEX` lets named functions change size while everything behind them shifts by the
same amount, order and sizes unchanged: multiapp's build id hashes every header under `include/`,
and its bytes are immediates in `check_trailer()` and `cmd_info()`.

## The report

Each build writes `zephyr/place_report.txt` (`scripts/place/place_report.py`, a post-build step;
`-DESP_SDR_PLACE_REPORT=OFF` leaves it out): bytes per memory region, the room left below the banks
and in the high region, the hot block against its budget, and, from the build's
`compile_commands.json`, every tag with the symbols it became, as bytes per category and region:

```text
  categories, bytes by region (symbols):
    BULK          PSRAM 4586588 (11)
    SAMPLE        flash code 3128 (3)
    STACK         PSRAM 9216 (2)
  tagged, with no symbol (inlined, or left out by #if): 3
    ieee154_phy.c: EVENT crc16
```

A tag with no symbol was inlined (or not compiled); one whose symbol is in its object but not in the
image was removed by the linker. For each such code tag the report adds the callers DWARF says it
was inlined into with their regions, and the region the rules chose for it (`ESP_SDR_REGION_CODE()`,
preprocessed with the source's compile command); a caller elsewhere is marked `!`, since inlined
code runs where its caller does. Copies in callers the linker removed are left out, as they never
run, and tags in code the build leaves out by `#if` are listed by themselves (the source
preprocessed with its compile command): the function of the same name in the other branch is not
theirs, as link's esp-dsp FFT wrapper is not its C FFT. Last come the repository's own functions
and objects of 1 KB or more without a tag (`--untagged`), which should be none, or explained. A
multiapp extension's tags are counted from its objects by input section; a buffer the host places
for it shows as in the host's area.

## The hot block

`CONFIG_ESP_SDR_HOT_TEXT` (ESP32-S3) gathers an image's hottest flash code into one block of flash
text aligned to the cache's way (`lib/esp_sdr/hot_text.ld.in`): its lines keep their cache sets
whatever code comes before or after. The block holds the app's list,
`esp_sdr_hot_text(<list file>)` in its `CMakeLists.txt`, then the code its rules place in
`ESP_SDR_HOTBLOCK`; `CONFIG_ESP_SDR_HOT_TEXT_BUDGET` (12 KB, text and literals) limits it, so that
it takes 6 of the 8 ways of each set and leaves 2 to the rest. The list is linker script statements
per object that claim input sections from the flash text's catch-all; the Xtensa linker pools their
literals at the block's start, and any label or assignment between them would move those pools.

It needs the TEXT_SECTIONS hook of the ESP32-S3 linker script (Zephyr commit "soc: espressif: let
applications place code in esp32s3 flash text" in the zsdr fork): CMake stops without it, and the
link stops (`hot_text_check.ld`) should the block not reach the image.

`scripts/place/hot_text.py <build> <list file> <capture>...` writes the list from profiles of the
image: the flash functions sampled, densest first (samples per byte), while their text and
literals (sizes from the linked map) fit the budget less what the rules place there; functions
tagged `COLD` never go in. Every list is an A/B on the board.

## Measuring

- **The other CPU's profile.** `CONFIG_ESP_SDR_CPU_PROF` (`esp_sdr_prof.h`): while the receive ring
  runs, its CPU reads the other CPU's program counter (the debug assist's PC recorder) and current
  thread at a fixed period, into PSRAM. The sampled CPU takes no interrupt and runs no code for it,
  and the code it runs with interrupts masked is sampled too; there are no stacks. `prof start
  [period_us] | stop | dump` drives it (apps/common); a sniffing capture took 131072 samples at
  64 us over 8.8 s with none missed. The dump has the layout of perf's `printbuf`, each sample its
  program counter under a frame that stands for its thread (the thread's entry function; the table
  before the samples names the threads), so Zephyr's `scripts/profiling/stackcollapse.py` reads it
  too. 131072 samples are 7 MB, 11 s over CDC-ACM: with the `prof` shell, apps/common raises the
  shell's transmit buffer from 8 bytes (46 s) to 1 KB.
- **Zephyr's perf.** `subsys/profiling/perf` (`CONFIG_PROFILING` and `CONFIG_PROFILING_PERF`)
  samples the interrupted stack in the timer interrupt on CPU 0 and, through IPIs, on the other CPU
  (`perf record`, `perf printbuf`). The zsdr fork adds an Xtensa backend
  (`PROFILING_PERF_BACKEND_XTENSA`) and a CPU mask, `perf record <ms> <Hz> [thread [cpus]]`:
  `perf record 3000 1000 0 1` samples CPU 0 alone and sends the ring's CPU no IPI.
- **Functions.** `scripts/place/prof_map.py <elf> <capture>...` maps either capture to functions,
  samples per byte and section, or to collapsed stacks for a flame graph.
- **The cache.** `scripts/place/icache.py <port>` reads the I-cache's access and miss counters
  through Zephyr's devmem shell (`CONFIG_DEVMEM_SHELL`), window by window.

## Moving code

1. Change one rule or pin, in the app's `place.h` where the move concerns one app.
2. Build, and compare with `place_diff.py` and the report: only the intended code moved, and the
   regions it left and entered have the room.
3. Measure on the board against the image before, interleaved and repeated (the app's own figure:
   stream loss, skipped frames, cycles per sample), and the cache counters.
4. Keep the change only if it is no worse within the spread.
