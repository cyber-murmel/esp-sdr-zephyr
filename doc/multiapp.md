# multiapp: one firmware that runs the others

Status: all six apps (`capture`, `rfprobe`, `wpan`, `stream`, `osmosdr`, `link`) run as
llext extensions from PSRAM, built from their unchanged sources, one app per boot, on the image of
`apps/multiapp/` ("the host"). The radio is extensions as well: the PHY, `lib/esp_sdr` and
`lib/ieee802154` are modules a boot loads only if its app needs them. So is the stock Wi-Fi
driver, which the small `wifitest` app uses. MicroPython is an app too: on the XIAO with its display, LVGL, knob, SD card and
two USB drives, on the Communicator with its display and keyboard, on boards without a display headless. The host loads each one from its `<name>.llext` file on a FAT volume in flash,
which the PC sees as a USB drive in boots without an app; built-in copies are the fallback. It needs
Zephyr changes that are on the fork `west.yml` pins ("Zephyr changes"). On the ESP32-S31 the
extensions are RISC-V relocatable objects and the board boots into `osmosdr_hs` ("The ESP32-S31").
Usage: `apps/multiapp/README.rst`.

## Constraints

- **Every app still builds and runs standalone**, with its own `prj.conf`, overlay and `main()`,
  unchanged in behavior. multiapp compiles the same sources; no app source moves.
- **Internal RAM is the binding constraint.** IRAM and DRAM share one pool, and the image must
  end below the dump banks the configured apps reserve ([hardware-quirks.md](hardware-quirks.md),
  "Memory"). Extensions therefore run from PSRAM.
- **Kernel configuration cannot differ per app.** USB classes, network buffers, SMP and heap
  sizes are fixed when the host is built; the host carries the union of what its apps need.
- **Constructs that register through linker iterable sections** (`SHELL_CMD_REGISTER`,
  `USBD_DEFINE_CLASS`, `SYS_INIT`, `LOG_MODULE_REGISTER`, `K_THREAD_STACK_DEFINE`, static memory
  slabs, `NET_BUF_POOL_DEFINE`) do not work in an extension as written: its sections do not join
  the host's.
- **Exit is a reboot.** The Wi-Fi PHY, ring and DAC state cannot be torn down cleanly.
  `multiapp run` with an app loaded leaves the name in RTC memory and resets.

## How it works

- **Code in PSRAM.** The S3's cache MMU maps the data bus window (0x3c000000) and the
  instruction bus window (0x42000000) through one table, so a PSRAM page written at data address
  D executes at D + 0x06000000 after a data cache write-back and an instruction cache
  invalidate. llext keeps one address per region, so a local Zephyr change
  (`CONFIG_LLEXT_EXEC_ALIAS`, `llext_heap_init_harvard_exec()`) copies and patches through the
  writable address and uses the alias for relocations, the symbol table and the init/fini check.
  The ESP cache driver has no instruction cache operations: the host syncs the range itself after
  `llext_load()` (`sync_text()` in `src/apps.c`). Heaps: `CONFIG_MULTIAPP_INSTR_HEAP_KB` (1344) and
  `CONFIG_MULTIAPP_DATA_HEAP_KB` (1024), both in PSRAM and not zeroed at boot.
- **Modules** (`multiapp_module()`): extensions without a `main()` that other extensions call, each
  named by the apps and modules that need it (`REQUIRES`). The host loads them in order before
  the app, each from its file with its trailer checked as an app's, and runs their constructors.
  llext finds what an extension imports in the host first, then among the loaded extensions:
  `exports.py` leaves what a module defines out of the host's exports, and modules link with
  `-Bsymbolic`, so their own globals stay theirs even where the host has the same name.
- **Modules in internal RAM** (`INTERNAL`). Such a module is placed as a standalone image places
  the same code: `IRAM_ATTR` code (`.module_fast`, `ext/module_fast.ld`), data and bss in internal
  RAM, the rest of its code and its read-only data in PSRAM, where a standalone image
  has flash. `CONFIG_LLEXT_REGION_HEAPS` lets one load take each region from a heap of its own, and
  `LLEXT_MEM_TEXT_ALT` gives llext the second code region. That region comes from `module_heap`
  (`src/apps.c`, below bank 0) and runs at the instruction bus alias of its data bus address
  (`SOC_I_D_OFFSET`), the same trick as PSRAM without a cache to sync. `HIGH_BSS` puts bss in the
  high region (`sdr`'s DAC state). What llext only reads while loading stays in PSRAM. These
  modules link with `--no-relax`, and `fastcheck.py` fails the build if a blob's fast code reaches
  other sections PC-relative: the linker leaves no relocation for that, so it would point elsewhere
  once the two regions are loaded apart. The PLT stays at the start of the cold code, where the
  linker's own script puts it next to `.got.plt`: llext writes each import straight into the
  caller's literal (`R_XTENSA_JMP_SLOT`), so the PLT's lazy-binding stubs should never run.
- **The radio as modules.** `phy` (`libphy`, `librftest`, the HAL's PHY glue), `sdr`
  (`lib/esp_sdr`, brought up by `multiapp_module_up()` in `ext/sdr_up.c`) and `ieee154`
  (`lib/ieee802154`). The host has none of them and no Wi-Fi driver
  (`CONFIG_ESP_SDR_PHY_ONLY`, `CONFIG_ESP_SDR_AS_EXTENSION`): a boot loads what its app names, and a
  boot without an app brings no radio up. The sources come from each library's `sources.cmake`, so
  standalone apps and modules build the same files.
- **Devices in a module** (`DEVICES`). A module's `DEVICE_DEFINE()` cannot join the host's
  sections. `ext/module_devices.ld` keeps its init entries in the kernel's order, the host runs them
  after the module's constructors (`do_device_init()`), and the prelude sends
  `device_get_binding()` to `multiapp_device_get_binding()`, which also searches the loaded modules.
  The table's bounds reach the host through an object (`ext/module_devices.c`), because llext exports
  only functions and objects, not the untyped symbols of a linker script. A `DEVICES` module whose
  table is missing or empty refuses to load.
- **Blobs in a module** (`multiapp_blob()`, `LIBS`, `VERSION_SCRIPT`). Espressif's libraries are not
  built for a shared object: their pointer tables in `.rodata*` need dynamic relocations, which
  the linker refuses in a read-only section, so a copy of the archive gets them writable. llext
  then merges sections by flags into regions, and writable data in front of `.got.loc` would make
  the data region enclose it: `ext/module_blobs.ld` moves it behind `.got`. The archives also hold
  test code calling functions the host does not have. A version script names the exports and
  `--gc-sections` drops the rest (`ext/phy.ver`: 36 KB of code against 84 KB, 132 bytes of bss
  against 42 KB).
- **The stock Wi-Fi driver as a module** (`wifi.cmake`). The host is built without it, so neither
  fork changes.
  - *What it builds:* what a build with `CONFIG_WIFI_ESP32` adds to one without: the driver, the
    hal's glue, 108 supplicant files and the blobs. The supplicant uses its internal crypto, so there
    is no mbedTLS (`ext/wifi_shim`). It has its own copy of the PHY's blob and glue, compiled for
    Wi-Fi, and such a boot loads neither `phy` nor `sdr`.
  - *Its Kconfig:* a second Kconfig pass, on the host's fragments with `ext/wifi.conf` on top.
    `extconfig.py` writes the difference as a header each source includes after `autoconf.h`, and
    fails the build if a symbol that is not the module's own differs, because it could change what
    host and module agree on, such as a kernel struct.
  - *Placement:* as an image with the driver and its IRAM options off (`ext/module_wifi.ld`):
    `.wifi*iram` and the blobs' read-only data in PSRAM, where such an image has flash, and only
    `.iram1`, `.dram1`, data and bss in internal RAM. The read-only data needs dynamic relocations,
    so its input sections are writable, and a `(READONLY)` output section tells llext to load it
    with the rest of the read-only data.
  - *Its memory:* the driver's `k_malloc()` and thread stacks come from a private heap in the area,
    through `--wrap` (`ext/wifi_glue.c`), since no other module uses the area in such a boot. The
    blobs' own buffers are in PSRAM (`CONFIG_ESP_WIFI_HEAP_SPIRAM`). The host's kernel heap stays
    small.
  - *Its interface:* a `net_if` must be in a host section, so the host defines one
    (`src/wifi_netif.c`), down on a device that does nothing. As the module loads, its private
    device takes over `iface->if_dev->dev`, and the stack reaches the driver through that pointer
    on every call. The driver's own name for its interface is pointed at the host's
    (`ext/wifi_prelude.h`), and a `NET_IF_DECLARE()` left in it fails the build. `wifi_mgmt.c` stays
    in the host, along with IPv4 and DHCPv4.
  - *Its core:* the blob wants its task and interrupt on one core, and a host without the driver
    reserves that interrupt on core 1 (`esp_cpu_intr.c`), so the module pins its task there.
  - *No built-in copy* (`FILE_ONLY`): it runs from its file or not at all, and the boot then falls
    back to the drive. The file is only stat-ed at each boot, not read.
- **MicroPython as an app** (`micropython.cmake`, `ext/mp/`). The host stays `CONFIG_MICROPYTHON=n`,
  so the module's own build does not run.
  - *What it builds:* the port's sources (`ports/zephyr/sources.cmake`, on the MicroPython fork), the
    C modules in `apps/micropython/cmod`, `boot.c`, `setjmp.S`, `usb_disks.c` and the frozen Python,
    with the same flags as the port, through MicroPython's own `py/mkrules.cmake` on the extension's
    target. That file reads a target's flags as plain lists, which an llext target's generator
    expressions are not, so `micropython.cmake` gives the qstr scan its flags itself. `-Bsymbolic` is
    not needed: `ext/mp/micropython.ver` exports only `multiapp_info`, and `--gc-sections` drops the
    rest.
  - *Its Kconfig:* the second pass, as for Wi-Fi (`ext/mp/micropython.conf`). The port's
    `mpconfigport.h` includes `autoconf.h` again, which undoes any `#undef` made before it, so those
    are in `ext/mp/mpconfigport_multiapp.h`.
  - *The console* (`CONSOLE`): before its constructors run, the boot's host stops the serial shell
    (`shell_uninit()`, `k_thread_join()`), which also ends its log backend, so nothing prints into
    the REPL. `multiapp_usb_up()` starts its `main()` once USB is up, as the standalone app's order
    is. The host's console driver has no input path, so `ext/mp/mp_glue.c` provides the two functions
    the port's `zephyr_getchar.c` calls, on the CDC-ACM port's callback. `multiapp run` always reboots
    into such an app, since it runs on the shell that has to go.
  - *`esdr.shell()`:* Zephyr's `shell_dummy.c` built into the extension, its stack and buffers in
    PSRAM, so every other boot pays nothing. `west dfu` drives the host's commands through it, as
    standalone. It must not become a log backend (`CONFIG_SHELL_DUMMY_INIT_LOG_LEVEL_NONE`), since the
    host's list of those cannot take one.
  - *The heap:* `multiapp_psram_spare()`: everything from the end of what the linker placed in PSRAM
    to the top. It holds the shared heap, which only the `wifi` module imports, and the buffer that
    held the app's file, which is why that buffer is the top of PSRAM and not an array of its own.
    One block, so a large buffer fits. `CONFIG_APP_MP_PSRAM_HEAP_KB` is a token here.
  - *The stack* stays in internal RAM, in the area, because Python writes the flash: `mp_stack`,
    `fast_heap` and the radio threads' stacks take 24064 of its 24128 bytes.
  - *The file system:* `/FLASH:` is the host's FatFs with MicroPython's settings (8 files, 4
    directories, long names up to 64) and an fstab entry, with its own FATFS object in internal RAM.
    The flash disk's cache moved out of bank 0, because the ring may run while Python reads a file.
    The host's read-only mount for the app files is gone by then.
  - *The display and the board's other peripherals* (`CONFIG_MULTIAPP_MICROPYTHON_GUI`, set by the
    board's fragment): LVGL and the board's drivers compile into the extension (`DEVICES`), so no other
    boot pays for them: on the XIAO SPI, MIPI-DBI, ST7789V, the knob's input drivers and the SD stack, on
    the Communicator SPI, MIPI-DBI, NV3007, I2C and the TCA8418 keyboard. `micropython.cmake` picks them
    from what the extension's Kconfig pass enabled, and a device without its driver fails the link. The host's `.config`
    has none of them; the XIAO's devicetree would otherwise switch the SD stack on in the host, which
    the second Kconfig pass caught (`CONFIG_DISK_DRIVER_SDMMC=n` there). The peripherals come from
    `apps/micropython/boards/<board>_periph.dtsi`, shared with the standalone app, and the
    extension reads the same board fragment the standalone app does. A display board without one
    fails at configure time: without it the panel's colours come out wrong.
  - *Two USB drives:* the host defines two LUNs, because their list is a host section (`src/drive.c`).
    In a drive boot the first is the volume and the second, on a board with `sd_fs`, has no medium. A
    MicroPython boot points the first at `UFLASH` and the second at `USD`, the proxies `src/usb_disks.c`
    registers from the extension, unchanged from the standalone app: they report no medium while the
    board writes a volume.
- **A prelude** (`include/multiapp/ext.h`, put in front of every C source with `-include`) turns
  what an extension cannot do into what it can: `SHELL_CMD_REGISTER()` and `SYS_INIT()` become
  constructors, `LOG_MODULE_REGISTER()` a declaration, slabs are initialized in a constructor,
  `NET_BUF_POOL_DEFINE` leaves a descriptor in `.multiapp_host` from which the host defines the
  pool (net_buf finds pools by their index in a host section). `.noinit` and the PSRAM section
  attributes join `.bss`, because llext loads only one NOBITS section. The app's `app_usb_init()`
  becomes a call that does nothing (see "Bugs found").
- **Generated host glue** (`exports.py`): the host exports exactly the symbols the extensions
  import, in its own export group (Zephyr's default groups, every syscall kept in IRAM, are off),
  and registers their log modules. A symbol the host lacks fails the host's link. With
  `CONFIG_LLEXT_IMPORT_ALL_GLOBALS` the host finds an app's hooks by name, such as
  `app_usb_dfu_prepare()`.
- **The app's own Kconfig values.** An extension compiles against the host's `autoconf.h`;
  `appconfig.py` writes the app's `CONFIG_APP_*` values into a header included after it.
- **Proxy roots** (`src/apps.c`): the host registers the apps' root commands (`probe`, `wpan`,
  `sdr`, `link`, `ofdm`, `hello`) as dynamic commands that forward to the loaded app, so the shell
  and the tools that drive it look as standalone. The app's `main()` runs in a host thread
  (`CONFIG_MULTIAPP_STACK_SIZE`, stack in PSRAM).
- **USB profiles.** All class code lives in the host. The S3's OTG controller has 4 IN endpoints
  for classes (`GHWCFG4.INEps`), and the union of the apps' classes needs 6, so the host registers
  a subset per boot: `app_usb_classes()` (a weak hook in `apps/common` between registering the
  classes and `usbd_init()`) unregisters what the boot's app does not name in its `USB` mask.

  | Class | Used by | IN endpoints |
  | --- | --- | --- |
  | CDC-ACM (shell) | every boot | 2 |
  | CDC-NCM | stream | 2 |
  | vendor bulk (`usb_sdr.c`) | osmosdr | 1 |
  | MSC (the volume as a drive) | boots without an app | 1 |
  | DFU | every boot | 0 |

  osmosdr's vendor class is built in the host; its calls into the app go through forwarders in
  `src/osmosdr.c` that look the app's functions up by name, so the boot's app is loaded before
  USB starts.
- **NCM's DMA buffers in bank 0** (`multiapp_bank0.ld`): USB DMA needs internal RAM, and
  `stream` never runs the ring, so bank 0 is free in its boots.
- **An internal RAM area, one user per boot.** Buffers an app must keep in internal RAM
  (`ESP_SDR_HIGH_BUF_DEFINE()`, `ESP_SDR_HIGH_STACK_DEFINE()`) become host objects in
  `multiapp_area`, 24 KB of the high region (`AREA` in `multiapp_extension()`). osmosdr takes
  23 KB; the 802.15.4 driver gets it in the other boots
  (`CONFIG_ESP_SDR_IEEE802154_HIGH_RAM_APP`). `multiapp run` reboots when the profile or the area
  owner changes.
- **The apps as files** (`src/drive.c`). A FAT volume in the upper 4 MB of the flash, which the
  board's 4 MB partition table leaves out (partition `fat` at 0x400000, flash disk `FLASH`),
  holds `<name>.llext`. The host mounts it read-only at boot, before USB starts and before an app
  runs, compares every file with the built-in copy (`multiapp list`), and reads the boot's app whole
  into a PSRAM buffer (`CONFIG_MULTIAPP_FILE_MAX_KB`); llext then loads it from there. That mount's
  FATFS object and the drive's SCSI buffer are in bank 0, which the ring overwrites once an app runs:
  nothing of the host touches the volume after that. Reads go in 256-byte chunks through the FATFS
  window, so the flash driver never gets a PSRAM buffer.
- **A trailer on every file** (`apptrail.py`): ELF size, CRC-32, the stack the app's `main()`
  needs and a 16-byte build id (`buildid.py`: a hash of `autoconf.h`, the devicetree, the
  extensions' imports, the headers both sides compile against and the compiler). The host checks
  it before llext reads the file: a truncated, damaged or foreign file (built for another host)
  is refused and the built-in copy runs (`CONFIG_MULTIAPP_EMBEDDED`, default y; the copies hold
  340 KB of the image, and without them such a boot comes up without an app). A boot that dies on
  the way to its app is skipped once, to end a crash loop. A file refused cleanly is not: it is
  tried again at the next boot, so a correct volume takes effect at once. `multiapp info` says where the
  app came from.
- **The drive.** A boot without an app (no request, no autostart, or an app that failed to
  start) registers USB mass storage on the volume next to the shell and DFU, for copying app
  files from the PC; boots with an app unregister it, and `multiapp run` from a drive boot reboots
  into the app. The PC must eject the drive first: the host cannot see what the PC still caches.
  `west dfu --storage` writes the build's volume image (`mkfat.py`: deterministic FAT12, 4 KiB
  clusters on the erase page, long names) to the DFU target `fat`, and `west dfu --esptool`
  writes MCUboot, the image and the volume over the USB-Serial-JTAG, which the running app hands
  over with the shell command `usj`.

## Memory budget

The host's image ends 12120 bytes below bank 0 (0x3fcb0000) on the devkit, 10096 on the XIAO, whose host
also carries a second LUN, exFAT and a larger drive thread. That image holds `module_heap`
(31744 bytes), the volume, the drive (2.2 KB) and what MicroPython's `/FLASH:` needs from the host
(5.5 KB: the flash disk's cache, a FATFS object and the extra file slots). What got it there: no radio
and no Wi-Fi driver in the host, a kernel heap of 3072 bytes (peak 2144 in any boot, 236 in a drive
boot, against 12216 with the driver), `CONFIG_ESP32_WIFI_NET_ALLOC_SPIRAM`, the export group above,
`CONFIG_NUM_MBOX_ASYNC_MSGS=0`, the app's `main()` stack in PSRAM. The signed image is 880 KB, against
1096 KB with the radio in the host. Serving Wi-Fi costs every boot about 4 KB of internal RAM and
80 KB of flash (IPv4, DHCPv4, `net_mgmt` and its thread, the interface).

PSRAM (8192 KiB): 3.4 MiB the linker places, mostly the code heap (1344 KiB), the data heap (1024 KiB)
and the transmit ring (1 MiB); the shared heap only `wifi` allocates from (1 MiB); the top 2048 KiB
(`CONFIG_MULTIAPP_FILE_MAX_KB`) for the file being loaded. In a MicroPython boot everything above the
first of those is its heap: 4785664 bytes, one block, against 5126144 standalone, the same with a
display. On the XIAO the app takes 1116427 bytes of the 2368 KiB of code and data heaps.

| Module | Code in PSRAM | Code internal | Data | Bss | Read-only in PSRAM | File |
| --- | --- | --- | --- | --- | --- | --- |
| `phy` | 31721 | 5931 | 3948 | 120 | 1616 | 55892 |
| `sdr` | 22750 | 7110 | 328 | 14384 (high region) | 824 | 50860 |
| `ieee154` | 16024 | 722 | 768 | 1216 | 452 | 27224 |
| `wifi` | 324221 | 8477 | 5460 | 15580 | 62134 | 539948 |

The `micropython` app, in PSRAM, headless: 168558 bytes of code, 113 KB of read-only data, 38 KB of data
and 163 KB of bss (`esdr`'s FFT and monitor buffers are 128 KB of it), in a 441656-byte file. 8.7 k
relocations, and 181 symbols imported from the host and the modules. With display and LVGL (the XIAO):
586195 bytes of code, 236 KB of read-only data, 122 KB of data and 164 KB of bss, in a 1275216-byte file.
26.4 k relocations, 220 imported symbols.

`module_heap` holds 29532 of 31640 bytes in a Wi-Fi boot and 20168 in a `wpan` boot (`multiapp
info`), and is empty in a drive boot. The high region is full: the area (24128 bytes, of which osmosdr takes 24064) and
`module_high_mem` (14528, of which `sdr` takes 14372). micropython takes 24064 of the area too. Log queue 256 messages, link's shell stack
3072 bytes. Internal RAM stacks are sized so their peak with every app exercised is at most 85 %
(`kernel thread stacks`; main's in `multiapp info`, the USB thread's in the last DFU download in
`multiapp crash`): main 1936 of 2432 (the PHY calibrates on it as its module loads), `esdr_rx` 1792 of
2560, `esdr_tx` 800 of 2048, the driver's
event thread 1152 of 1536, the network receive thread 1680 of 2560 (in PSRAM), USB thread 1008 of
1280, drive thread 880 of 1152 while the PC copies 2 MB. The highest of the others is
`logging`, 77 %. With the display, the drive thread needs more, because the SD stack runs on it: 1264,
so 1728 on the XIAO (73 %). That peak comes from a card that never reports an error, so the SD stack's error
paths have not run on this stack, which is why it has more room than the rule asks. The kernel heap
peaks at 2364 of 3072, `IRQ 01` at 512 of 768. `mp_stack` is the highest: 13696 of 16384 (83 %) with the
`receiver` and `datalink` eggs, 10080 with the others, and 13664 standalone. Python's own check stops recursion
at about 15520, and C code called from Python at that depth has about 860 bytes left.

## Measured with the radio in modules

On an ESP32-S3 devkit (N16R8) behind a USB hub, which limits bulk transfers to about 890 kB/s, on
the production image. The column on the right is the same app built standalone with the Wi-Fi
driver, on the same bench. The XIAO has built this host, not run it.

| Test | Modules | Standalone with driver |
| --- | --- | --- |
| `stream` over NCM (bytes on the PC's interface) | 780, 796, 788 kB/s | 788 kB/s |
| `wpan` C6 to S3, 800 frames on channels 11, 15, 20, 25 | 799, 798, 799 | 799 |
| `wpan` S3 to C6, 800 frames | 794, 799, 800 | 799 |
| `wpan` S3 sends, ACK requested, 200 frames | 197, 196, 197 | 196 |
| `wpan` C6 sends, S3 ACKs, 1600 frames on channels 15, 20 | 1599, 1599 | not on this script |
| `link` QAM decode, qpsk to 4096qam | 10567 to 14657 us | 10414 to 14279 us |
| `link` QAM build, qpsk to 4096qam | 5661 to 9361 us | 5423 to 8883 us |

The plain frames lost were on channels 20 and 25, which are busy here. With or without modules, a
batch of 50 frames asking for an ACK usually misses one, as a missing ACK or a transmit error (-5).
That has not been looked into.

- `osmosdr` (`esdr_usb`, each test from a fresh boot): `test-in` 893 kB/s, receive at 250 kS/s
  8 bit and 125 kS/s 16 bit with 0 gaps and 0 overflows, 1.26 M samples transmitted with none
  dropped. 250 kS/s 16 bit overflows as it does with the driver: it exceeds this link. Ring 10.7 to
  11.3 cycles per pair, and 11.3 to 12.3 at 32x, varying from run to run with or without modules.
- `capture`'s survey: 0 failed. `rfprobe`: tune, gain, tone and receive work. `probe temp` returns
  -ENOTSUP, because the host has no temperature sensor driver.
- `wifitest` (the Wi-Fi module on its own PHY), on a WPA2 access point on channel 6 at -44 dBm: it
  joins and gets IPv4 by DHCP and IPv6 by router advertisement. The board's pings to the gateway: 100
  of 100, the replies sampled taking 4 to 11 ms. A PC's pings to the board: 160 of 160, at 56 and 1000
  bytes, up to about 240 ms round trip (not looked into; Wi-Fi power save is a likely cause). Leaving and
  rejoining works, and so does a reboot with the link up (reset reason 12). 10 scans in a row found
  13 to 16 access points. Updating the image and the volume with the link up and pings running: both
  confirmed, reset reason 12, 119 of 120 pings answered over about the first 6 s of each.
  `sample.esp_sdr.multiapp.wifi.hil` passes; it scans, since joining needs credentials.
- After an abrupt reset the access point still has the board associated: the first join fails with
  status 1 in 1.2 s, and the next one works (4 of 4). After `wifi disconnect` the first join works
  (3 of 3). An app should try again. Not compared with a standalone image.
- 56 app switches on the image with Wi-Fi scanning, 28 more on the final one, each into the
  requested app, all with reset reason 12 (a software reset). DFU with the ring running. The liveness watchdog fires with its feeder suspended, and the next boot
  falls back to the drive.
- A drive boot loads no module and starts no radio thread.
- From the kernel's start to the app's `main()`, with its modules loaded and the PHY calibrated:
  177 ms (`capture`) to 238 ms (`link`), 386 ms for `wifitest`.
- `link bench`'s float loop and power sum are 1.4 and 1.9 times slower than standalone. This is the
  shell on CPU 0 noted below.

## Measured with MicroPython as an app

On the same devkit, with `manifest_headless.py`, against a standalone `apps/micropython` built
headless for this board. The next section has the XIAO; the Communicator builds this host and has not
run it.

- From the host's `multiapp run` the REPL answers within about 1.5 s. Its boot takes 84 ms for `phy`
  and `sdr` and 222 ms for the app (`multiapp info`).
- Three soft resets give the same heap each time, and files and `esdr.shell()` keep working. A 4.69 MB
  buffer reaching within 11 KB of the top of PSRAM takes a pattern every 4 KiB and reads it back.
- `/FLASH:`: 128 KiB written, read back whole and from a seek, directories made and removed, files still
  there after soft resets, the apps' files still loading after that.
- Leaving: three times to `wpan` and back with `esdr.shell("multiapp run wpan")`, and `machine.reset()`,
  all with reset reason 12 and no crash recorded.
- From the REPL, `west dfu`, `west dfu --storage`, `west dfu --no-confirm` and `west dfu --esptool` all
  work; a new image boots as a drive boot, so its check and confirm go through the shell. With an
  autostarted REPL the confirm goes through `esdr.shell()`, which works (the Communicator). `--storage` hit "Cannot claim
  interface - LIBUSB_ERROR_BUSY" on its first try in 3 of 11 updates from the REPL and in 0 of 6 from a
  shell boot, and the retry worked each time. Later three tries in one second all failed and left the board
  in DFU mode (`west dfu --dfu-board` recovers it). fwupd's DFU plugin logged a probe of the new device at that
  moment, so `west dfu` now waits 2 s between tries.
- `mp_stack` peaks at 4880 of 16384 bytes at the REPL, against 4912 standalone. Deep recursion takes it
  to 15520, where Python's own stack check stops it, and the same applies standalone.
- Python runs slower, because the interpreter runs from PSRAM and is built position independent. Three
  rounds of the same script, in milliseconds, each stable to 1 ms except one wrapped reading on the
  standalone image (a negative time, left out):

  | | Loop | Calls | Float | List | String | Dict | Bytes | Allocate |
  | --- | --- | --- | --- | --- | --- | --- | --- | --- |
  | Standalone | 179 | 115 | 128 | 104 | 138 | 62 | 218 | 74 |
  | As an app | 203 | 127 | 141 | 116 | 143 | 67 | 247 | 70 |
  | Change | +13 % | +10 % | +10 % | +12 % | +4 % | +8 % | +13 % | -5 % |

- Receiving through `esdr`: 0 gaps at every rate from 500 kS/s to 15.6 kS/s, and at 250 kS/s counting
  for 15 s, into a 4 MB PSRAM buffer, into a wrapping ring for 15 s, and with `rx_latest()` polled as a
  display does while Python allocates on the other CPU. Ring cycles per pair match standalone within
  0.1 (12.63 counting, 13.12 to a buffer, 13.30 wrapping), and with the monitor they are lower (13.6,
  against 14.0). So the sink running from PSRAM has not cost anything yet; a display is still to come.
- Transmitting: 0 underruns at 500, 250 and 125 kS/s, generated or from a buffer. At 500 kS/s a 10 s
  session usually has one DAC restart, dropping about 550 samples: 9 restarts in 12 sessions over three
  runs here, 3 in 4 standalone. None at the lower rates.
- Over the air, from Python, against the WHY2025 badge's radio: the badge hears the S3's tone at -44 dB
  and the S3 hears the badge's at -50 to -52 dB, on this image and standalone alike.
- A flowgraph on the frozen GNU Radio shim (`soapy.source`, `head`, `null_sink`): 500000 samples, 0
  gaps. Resets, leaving for another app and firmware updates with the ring or the transmitter on: all
  clean. A soft reset leaves the next capture at 0 gaps.
- File reads with the ring on return correct data, but the flash operation parks CPU 1 and a bank
  switch can come late (`RING_LATE`), which restarts the ring with about 3 ms lost. A 4 KiB read every
  100 ms restarted it 41 to 63 times per 145 reads, against 9 to 26 standalone, losing about 1 % of the
  samples. That difference is not explained: reads are faster here (694 us against 908 us), and pinning
  `main` did not change it. A tight read loop breaks the ring on both images (750 to 1010 and 584 to
  875 restarts in 15 s). Writing 2 MiB with the ring on took 12 s and read back correctly, with 285
  restarts; not compared with standalone. Neither image is gapless while files are in use.
- On this image the other apps match the numbers above: `wpan` against the C6, 799 of 799 received, 785
  of 800 sent, 199 of 200 ACKs, 1586 of 1600 ACKs sent by the S3, the losses nearly all on channel 20,
  which is busy here; `stream` 779 kB/s; `osmosdr` with the same ring cycles and 0 gaps; `capture`,
  `rfprobe`, `link`; 28 app switches, all reset reason 12.

## Measured on the XIAO, as an app with its display

A XIAO ESP32S3 with the ST7789V panel (320x240), knob and SD slot, panel and card on one SPI bus,
against the standalone app built from the same tree. That standalone image is byte for byte the one built
before this work moved its overlay and source lists. Panel, colours, launcher and knob were checked by
eye.

- Starting it: `multiapp run micropython` takes 87 to 95 ms for `phy` and `sdr` and 781 ms for the app, and
  the REPL answers 1.95 s after the reset, against 1.31 s standalone.
- Since 2026-11-10 it boots to the launcher by itself (`CONFIG_MULTIAPP_AUTOSTART`, as the Communicator):
  checked after `west dfu` with `--storage`, after `machine.reset()` and after a soft reset, and Scan
  started from it (`launcher.run("scan")`) and ended by Ctrl-C back to it. `esdr.shell("multiapp run
  <name>")` starts another app.
- Python, three rounds of the same script, in milliseconds. Slower as on the devkit, because the
  interpreter runs from PSRAM and position independent. Strings are faster here:

  | | Loop | Calls | Float | List | String | Dict | Bytes | Allocate |
  | --- | --- | --- | --- | --- | --- | --- | --- | --- |
  | Standalone | 179 | 115 | 129 | 105 | 179 | 64 | 218 | 500 |
  | As an app | 203 | 127 | 141 | 117 | 150 | 70 | 247 | 520 |
  | Change | +13 % | +10 % | +9 % | +11 % | -16 % | +9 % | +13 % | +4 % |

- The display and the ring. The eggs from the launcher, in their own interpreter. The sink running
  from PSRAM costs nothing measurable here: 12.99 to 13.02 cycles per pair, against 13.00 to 13.02, so it
  stays there.

  | Run | As an app | Standalone |
  | --- | --- | --- |
  | `spectrum`, `waterfall`, `constellation` at 250 kS/s, `receiver` at 125 kS/s, 30 s each | 0 gaps, 1 run each | the same |
  | `spectrum`, 120 s, an event on a control every 250 ms, gc and poison every 1 s | 0 gaps, 954 frames per sink, loop 78 % busy | 0 gaps, 945, 86 % |
  | The same for 600 s with a retune every 2 s | 296 retunes, 0 gaps at every reading (twice a second), 4731 frames, 79 % | 292, 0 gaps, 4671, 87 % |
  | Heap in those 600 s, 10th reading to the last | -4032 B | -3840 B |
  | `spectrum` at 500 kS/s and 2 MS/s, 40 s | 0 gaps | 0 gaps |
  | `spectrum` at 1 MS/s, 40 s | 2 gaps (2 samples, 17 pairs lost) in 1 of 4 runs | 0 gaps in 3 runs |

  The 1 MS/s gaps are one event, not explained; there are too few runs to tell it from chance. About
  half of the heap's fall is the test's own list of readings. The rest is on both images, so in the
  shared Python.
- LVGL: six cycles of 24 objects sharing a style, each with an event callback and an animation, and a
  timer that ends itself, with Python holding no reference and the heap filled with poison and
  collected in between: every event, tick and animation still fires, and the heap ends 16 B lower.
- Python's free heap in an egg: 4.53 MB against 4.87 MB.
- The USB drive of the volume in a MicroPython boot: 256 KiB from the PC at 87 kB/s (59 kB/s with
  `spectrum` running) and read back at 670 kB/s after a remount, identical on the PC and on the board;
  1 MiB from the board, identical on the PC. A drive boot shows the volume and a second drive with no
  medium.
- The rest of the XIAO on this image: `capture`, `rfprobe`, `link` run, `stream` 780 kB/s, `osmosdr`
  `test-in` 890 kB/s with 0 gaps at 250 kS/s 8 bit and 125 kS/s 16 bit and none dropped on transmit, 28 app
  switches, and both desk checks pass. This USB port gives standalone `osmosdr` 893 kB/s as well, so the
  1154 kB/s below was another port, and 250 kS/s 16 bit overflows here for that reason. `wpan` boots,
  but has not met the C6 on this image, which was not on the desk.
- Open from the devkit, not repeated here: file reads restarting the ring more often than standalone.
- After these runs the drivers' list in `micropython.cmake` changed for the Communicator and the layout
  gained `fit()`. The XIAO's host stayed byte for byte the same through the first, and its eggs have the same
  plots as before through the second. That final build runs `waterfall` and `spectrum` with 0 gaps.
- **SD writes are untested.** The only card to hand keeps nothing it is given (see "Bugs found"), so no file
  written to an SD card has been read back on this image. What has run: the card mounts, the files list, raw
  reads at 903 kB/s at 12 MHz, and the path from USB to the card, 16 MiB at 217 kB/s and 4 MiB at 126 kB/s
  with `spectrum` drawing on the same bus, with 0 gaps and 1 run. A boot with no card comes up normally,
  though the port's message is "FS is invalid".

## Measured on the Communicator, as an app with its display

The Hackaday Communicator: NV3007 panel (428x142) on SPI, TCA8418 keyboard on I2C, no SD card and no
knob, against the standalone app from the same tree. Panel, colours, keys, starting an app from the
keyboard and Esc back to the launcher were checked by hand.

- Starting it: 95 ms for `phy` and `sdr`, 947 ms for the app, in a 1267392-byte file. The REPL answers
  2.35 s after the reset, against 1.64 s standalone. The host's image ends 11888 bytes below bank 0.
- Python, fresh interpreter against standalone: calls 128 against 115 ms, float 141 against 128, list 117
  against 105, string 156 against 148, dict 68 against 63, bytes 247 against 218.
- The display and the ring:

  | Run | As an app | Standalone |
  | --- | --- | --- |
  | `spectrum`, `constellation` at 250 kS/s, `receiver` at 125 kS/s, 30 s each | 0 gaps, 1 run each | the same |
  | `spectrum`, 120 s, an event on a control every 250 ms, gc and poison every 1 s | 0 gaps, 961 frames per sink, loop 51 % busy | 0 gaps, 955, 59 % |
  | `spectrum` at 500 kS/s, 1 MS/s and 2 MS/s, 40 s each | 0 gaps | 0 gaps |
  | The stress for 600 s with a retune every 2 s | 299 retunes, 0 gaps at every reading | not run |

  Those runs had `spectrum` before the layout's `fit()`, with its plots stacked. With it, `spectrum`'s plots are
  171x41 instead of 428x15 and `waterfall`'s 171x124, and each ran for 20 s with 0 gaps and 1 run. The plots beside
  the rows, the keys reaching every control and retuning from the slider were checked by hand.
- LVGL's stress as on the XIAO: everything still fires, and the heap ends 16 B lower.
- The USB drive of the volume in a MicroPython boot: 256 KiB from the PC at 101 kB/s (96 kB/s with
  `spectrum` running), read back at 760 kB/s after a remount, identical on the PC and on the board; 1 MiB
  from the board, identical on the PC. Writing with the ring on restarts the ring, as on the XIAO.
- Over the air, from Python, against the WHY2025 badge's radio: the badge hears the tone at -49 to -50 dB,
  and the Communicator hears the badge's at -46 to -52 dB.
- `wpan` against the badge's C6: of 200 frames each way on channels 11, 15, 20 and 25, the S3 received 799 of
  799 sent, and the C6 784 of 799, 12 of the losses on channel 25. 194 of 200 ACKs came back to the S3, and the
  S3's own ACKs reached the C6 for 1599 of 1600 frames.
- It boots to the launcher by itself (`CONFIG_MULTIAPP_AUTOSTART`): from a power-on, after `machine.reset()`,
  after another app and a reset, after `west dfu` and `--storage`. The runs here were before that, with a host
  otherwise the same; both desk checks' autostarts override it.
- The other apps: `capture`, `rfprobe`, `link` run, `stream` 1027 kB/s, `osmosdr` `test-in` 1173 kB/s, receive at
  250 kS/s 8 and 16 bit and 125 kS/s 16 bit with 0 gaps and 0 overflows, 1254692 samples transmitted with none
  dropped. 28 app switches, and both desk checks pass. This board is on a port without a hub.
- Stacks at their peak over all of that, keys pressed included: `mp_stack` 13680 of 16384 (83 %, `receiver`;
  13664 standalone), the drive thread 944 of 1152 (81 %), main 1808 of 2432, `sysworkq` 896 of 1152 (77 %, in
  `stream`; 752 with the keyboard), `IRQ 01` 512 of 768, kernel heap 2392 of 3072.

## Measured on the XIAO, with the radio in the host

- wpan against the C6 (`interop.py`, 100 frames of 100 bytes): 100 of 100 each way and 50 of 50
  ACKs on channels 11, 15, 20 and 25; DFU while the ring runs.
- `stream`: 947 kB/s of VITA 49.2 over NCM (standalone 968 kB/s); `vrt_tx.py` passes as
  standalone.
- osmosdr (`esdr_usb`): `test-in` 1154 kB/s, receive at 250 kS/s (8 and 16 bit) without overflow
  or gaps, 1.25 M samples transmitted with none dropped, ring at 13.3 of 15 cycles per pair.
- link: QAM build and decode within 1 to 4 % of standalone, FFT and RS the same, OFDM test frame
  5583 us (standalone 5589). `link bench`'s float loop and power sum are 1.4 and 2.1 times slower
  (the shell runs them on CPU 0 here, CPU 1 standalone; not isolated). PSRAM reads miss the cache
  once, then match internal RAM. Not tested: a link between two boards.
- 48 app switches, each into the requested app; all six apps run from their files with the same
  results.
- The volume: `west dfu --storage` reads back byte for byte (DFU upload), fsck.vfat is clean;
  copying through the drive runs at about 63 kB/s (every 4 KiB page erased and rewritten), files
  read back identical after a remount and run after ejecting. `west dfu --esptool --storage`
  writes MCUboot, image and volume in 27 s. A host built with one Kconfig value changed refuses
  the old files. The desk check `sample.esp_sdr.multiapp.hil` ([testing.md](testing.md)) passes.

## The ESP32-C6: complete images, not modules

Multiapp does not exist for the C6, and what the WHY2025 badge's C6 has instead does the same job
differently (`apps/osmosdr_serial`, "The C6's other apps in the flash"). This design cannot move there: no
PSRAM for code, about 13 KB left in SRAM block 0 with the SDR's image, blocks 1 and 2 the gapless capture's, and
loader options and module linking built around Xtensa shared objects. So MCUboot holds five complete images, and
`lib/boot_select` starts the SDR unless a flash sector asks for another, until the next power-on.

The SDR had to move from the ROM's simple boot to MCUboot, and its capture was measured on both, 15 s each:

| Through the P4 | Simple boot | Under MCUboot |
| --- | --- | --- |
| Gapless 80 MS/s: radio pairs, ring errors, bad blocks | 1199765484, 0, 0 | 1199766546, 0, 0 |
| Gapless 40 MS/s: radio pairs, ring errors, bad blocks | 1799686878, 0, 0 | 1799680769, 0, 0 |
| `spectrum` at 250 kS/s, 20 s: gaps, overflows | 0, 0 | 0, 0 |

Over the air against the Communicator, two rounds on the same desk: about -55 dB on both images in the first, about
-65 dB on both in the second. The level moves with the room, not the image. Each app ran from its slot (`capture`'s
survey, 0 failed; `rfprobe` and `wpan` answer on their shells; `zstack` boots), a damaged image fell back to the SDR,
and a power-on ended a request.

## The ESP32-S31: RISC-V extensions

On the ESP32-S31-Function-CoreBoard-1 the host has one core, 16 MB of octal PSRAM and the USB 2.0
HS port J4 for the shell, the drive, DFU and osmosdr_hs's stream; it boots into `osmosdr_hs`, and
`hello`, `capture` and `rfprobe` are the other apps. Where it differs from the S3:

- **Relocatable objects.** On RISC-V llext loads only those (`ld -r`). A shared object's
  `.rela.dyn` names no section, so llext skips it, and its PLT reaches `.got.plt` PC-relative across
  regions llext places apart. `ext/llext_riscv.ld` groups each kind in one section per llext region,
  makes one `.bss` (llext takes a single NOBITS section; libphy alone has six), collects the
  IRAM_ATTR code as `.module_fast` and drops `.eh_frame` (its SET6/SUB6 relocations are not
  llext's). An extension with archives links with `--gc-sections`: what it compiles is kept, of the
  archives only what it reaches, and so are the hal's sources it compiles, built with a section per
  function (the PHY glue's antenna helper calls `gpio_config()`, which the host lacks).
  `exports.py` reads the objects' symbol tables.
- **No alias.** Code runs where llext writes it: one heap of 2368 KiB in PSRAM
  (`.ext_ram_noinit`, not zeroed at boot; this SoC's `EXT_RAM_NOINIT_ATTR` is internal RAM, so
  `esp_sdr_place.h` names the section). `sync_text()` writes the data cache back, invalidates both
  instruction caches (`Cache_*_Addr()` with this SoC's cache map) and runs `fence.i`. PMA marks
  PSRAM executable and PMP's entries are unlocked, so the code runs from PSRAM and internal RAM
  alike.
- **TLS.** The libc uses thread-local storage, and llext relocates no TLS access:
  `CONFIG_CURRENT_THREAD_USE_TLS=n` makes `k_current_get()` a call.
- **The radio as one module**, `sdr`: the hal's PHY glue, the library and the blobs. The PHY
  library's calls into two ROM functions go to wrappers in the library (`--wrap` in the partial
  link), which only a module holding both can link; the host links the ROM's PHY symbols. 9.2 KB of
  it is internal RAM (IRAM_ATTR code, data, bss), the rest PSRAM; it comes up in about 116 ms.
- **osmosdr_hs**, from its own sources: built `INTERNAL`, so its 104 KB of USB batches, which carry
  40 MB/s, are internal RAM. `ext/usbd_prelude.h` gives it the host's USB device where it brings up
  its own, and `CONFIG_APP_USB_MSG_HOOK` passes it the device's messages (a suspend stops the
  stream). Its crystal calibration lives in settings, a file on the volume: an app's boot mounts the
  volume read-write before its modules load (a boot with an app never hands it to the PC), and
  `ext.h` registers settings handlers as the extension loads. `west dfu --storage` writes the volume
  anew, calibration included.
- **Memory.** The image ends 12.9 KB below the receive ring at 0x2F040000, with a module heap of
  120 KB of which the radio and osmosdr_hs take 116 KB.

Measured on the CoreBoard, against the standalone builds on the same board (2026-11-13 and 14):

| | multiapp | standalone |
| --- | --- | --- |
| osmosdr_hs, 300 s at 20 MS/s | 5,993 M samples, 0 gaps, 0 bad blocks | 5,986 M samples, 0 gaps |
| osmosdr_hs, free CPU at 20 MS/s (loadlog) | 83 %, idle reads 102 to 107 % | 79 %, idle reads 96 to 101 % |
| osmosdr_hs, overflows the device counts | 0 while streaming, 3 to 4 at the stop (0 since, below) | 0 while streaming, 4 at the stop |
| rfprobe, bursts at the six rates | duration 1.000 of the rate's | 1.000 |
| rfprobe, ring at 20 and 40 MS/s, 3 s | 0 gaps | 0 gaps |

The overflows at the stop were `esdr_usb`'s: it cancelled its transfers before it turned the device
off, which then streamed on into nothing. It turns the device off first now: three 10 s runs at
20 MS/s each way, alternating, counted 2 overflows at every stop before and none after (2026-11-15).

osmosdr_hs under multiapp also ran 30 s at 10 and 5 MS/s with 0 gaps, showed the 2400 MHz crystal
harmonic at -3.000 MHz from LO 2403 and +5.000 MHz from LO 2395, kept a stored calibration across a
reboot, streamed through 360 random retunes and a 3 s host stall without a fault, and streamed
through SoapySDR. Its vendor interface is number 3 there (0 standalone); `esdr_usb` and the SoapySDR
module find it by its class.

## Bugs found

- **The standalone app no longer fit the Communicator**, by 32 bytes: "DRAM image overlaps the ring banks",
  with code that had come in since (`corr_model` and friends in `lib/esp_sdr`). The board's fragment gives
  up 64 bytes of the kernel heap's 15872, which the host's `extconfig.py` pass is told to ignore. That heap's
  peak cannot be read on the image (no runtime statistics); it boots and runs the whole set of tests above,
  and 13312 once failed to bring USB up, so there is not much more to take from it.
- **`waterfall` did not start on the Communicator**, as an app or standalone: `ValueError` from
  `framebuf.FrameBuffer` in `gnuradio/qtgui.py`. Its rows take 130 of the 142-pixel panel's 124, which left a
  plot of no height. `spectrum` started, with 6 pixels of plot under each label. Now `_layout.fit()` puts plots
  that would get less than 30 pixels beside the rows, using the panel's width. Displays with room keep the stack.
- **A pointer just past a section failed the load.** `R_XTENSA_RELATIVE` looks a GOT entry's target up
  by section, and a table's end pointer, one past its last object, lies in no section when padding
  follows. llext said "0x136204 not found in any of the sections", `Failed to link, ret -2`, and the
  app did not start. Small extensions never had a table end at such a place. The lookup now accepts one
  past the end of an allocated section (`arch/xtensa/core/elf.c`).
- **Colours wrong, and nothing said so.** The extension looked for the board's Kconfig fragment under a
  wrong name, because `BOARD_QUALIFIERS` has no leading slash, so `CONFIG_LV_COLOR_16_SWAP` was missing.
  The panel worked, in the wrong colours. A missing fragment now fails configure.
- **Three SD cards, none of them able to test writes on the XIAO.** After the two below, a 32 GB card that
  reads fine in the WHY2025 badge's native SD mode holds MISO low whenever it is selected on the XIAO's
  SPI bus, power cycled or not, so even CMD0 reads 00. Not the slot: another 32 GB card passed writes there
  in October, and the 4 GB one below initialized. Sent only reset and identify commands. Not explained.
- **Two SD cards that looked like firmware faults.** One, sold as 64 GB, presents 1.9 GB, sets the high
  capacity bit yet is byte addressed, so LBA n reads sector n/512 and `/SD:` reports "FS is invalid". One, 4
  GB, went read-only without saying so: every write is acknowledged, with no error at any level, and the old
  data reads back, in a PC's card reader too (4 MiB "written" in 0.6 s, gone after a remount, and Linux's
  "not properly unmounted" at every mount, since the flag never clears). Both behave so on the standalone
  image as well. Before suspecting the driver, read whole sectors and write one.
- **The ROM calls PHY code with the cache off.** With `phy`'s code in PSRAM, a reboot with the radio
  up ended in a double exception (reset reason 16, not the software reset's 12): an illegal
  instruction in `ram_chip_i2c_readReg`, on the way down in `rtc_clk_cpu_set_to_default_config`.
  This placed blob code in internal RAM ([hardware-quirks.md](hardware-quirks.md)), and
  `test_reset_was_clean` watches for it.
- **A module's init table went unseen.** Its bounds were linker script symbols, which llext does not
  export, so `wpan`'s radio device never initialized and nothing reported it until `wpan` said "no
  radio". See "Devices in a module"; `hello mod` covers it now.
- **A zeroed interface that links and scans.** The driver names its interface through a
  `NET_IF_DECLARE()` that `NET_DEVICE_DT_INST_DEFINE()` fills in. With that macro replaced it was a
  zeroed `net_if` of the module's own. Scans never use it; the first join crashed in
  `net_if_dormant_off()`.
- **The network receive stack overflowed on real traffic.** 1024 bytes were enough for `stream`, and
  the first router advertisement over Wi-Fi used all of them. It lies directly above `net_mgmt`'s
  stack in PSRAM, which an overflow would corrupt. One update failed while such an image was
  connected, and none has since. Now 2560, in PSRAM, peak 1680.
- **One IPv4 slot, taken.** `CONFIG_NET_IF_MAX_IPV4_COUNT=1` went to NCM's interface, unused, so
  DHCP on Wi-Fi never finished, and with `CONFIG_WIFI_STA_AUTO_DHCPV4` the result of a join comes
  only then.
- **`main` could start USB on the radio's CPU.** USB's and the watchdog's interrupts land on the CPU
  that allocates them, and a thread can come back from any wait on the other CPU. A `capture` boot
  reported CPU 1. With MicroPython's ring on, a firmware update then died part way through on a
  watchdog reset (reset reason 7) in 2 of 3 tries, with no crash record, because the ring masks
  interrupts on that CPU. `main` now pins itself to CPU 0 first (`app_cpu_pin_self()`): 6 of 6 updates
  with the ring on and 3 of 3 with the transmitter on. Every boot had this hazard, not only this app.
- **`esdr.init()` with a capture running never returns.** It waits for the radio's lock, which the
  running ring holds, and Ctrl-C cannot reach it: the REPL goes silent. The standalone app has the same
  code. Not fixed: stop the capture first.
- **The port's `os.rename()` cannot work.** `zephyr_fs_make_path()` returns one shared buffer, so both
  names are the second one and it fails with ENOENT. The standalone app does the same. Not fixed.
- **Long prints lose their middle.** `uart_poll_out()` on CDC-ACM discards what does not fit its TX
  buffer unless flow control is on, and the REPL's output goes that way. The standalone app does the
  same. Not fixed: with flow control, a print with no PC attached could block for good.
- **The PLT in internal RAM was a mistake, and hid a limit.** It sat with the fast code, on the
  belief that fast code called the host through it. Its stubs only do lazy binding, which llext does
  not use. It cost `sdr` 3.2 KB, and for `wifi`, whose code is over 256 KB, it failed the link: a
  stub's `l32r` reaches only 256 KB back to `.got.plt`, and `ld` reports that as a bare assertion.
- **A clean refusal looked like a crash.** After a new image, a file-only module's old file is
  refused and the boot comes up as a drive. The guard against crash loops then skipped the autostart
  once more, even with the right volume written. Now only a boot that dies is skipped.
- **The hal's name mapping ran too early.** `zephyr_compat.h` maps Zephyr's Wi-Fi options to
  ESP-IDF's and comes first on every compile line, before an extension's own values:
  `ext/wifi_compat.h` runs it again.
- **Atomics in PSRAM are not atomic** (`hello atomic`); local Zephyr fix
  ([hardware-quirks.md](hardware-quirks.md), "Memory").
- **No flash access from a PSRAM stack.** The app's `main()` raced the host's `app_usb_init()`
  and ran MCUboot's image check on its PSRAM stack: double fault, board hung after some app
  switches ([hardware-quirks.md](hardware-quirks.md), "Memory").
- **Extensions built at `-O0`:** Zephyr drops `-Os` from Xtensa extensions; a 1472-byte frame
  (32 at `-Os`) overflowed a 2048-byte stack. multiapp keeps the host's `-Os`.
- **llext relocations:** `R_XTENSA_GLOB_DAT` dropped its addend; alignment padding made `.bss`
  claim `.exported_sym`'s relocations; a shared object's `.bss` sat 12 bytes off its 16-byte
  alignment, which the S3's 128-bit loads round down silently. Local Zephyr fixes.
- **Stale extensions:** Zephyr's llext CMake rule never rebuilt `<target>_debug.elf` after an
  edit. Local Zephyr fix.
- **Unregistering NCM before init** crashed (its shutdown removed a descriptor it never added).
  Local Zephyr fix.
- `link decode` and `link bench` overflowed the shell stack (a 3.6 KB decode chain): they run on
  the paused MAC thread now (`link_mac_run_paused()`). osmosdr's receive thread has 2560 bytes.
- llext frees an extension's symbol table after loading; the shell recognizes dynamic entries by
  host section address.
- `flash_img` writes an MCUboot image, from the start the swap-using-offset layout gives the
  update slot: the volume landed 4 KiB into its partition. Its DFU target uses `stream_flash`.

## Zephyr changes

Commits on the fork's `zsdr` branch, which `west.yml` pins, on top of the SMP port: exec alias, llext repackage dependency, NCM/ECM unregister before init, Xtensa
GLOB_DAT addend, alignment padding, PSRAM atomics (two commits), shared object region alignment.
They are candidates for upstream Zephyr. The modules add `CONFIG_LLEXT_REGION_HEAPS` with
`LLEXT_MEM_TEXT_ALT` (`subsys/llext`), and `CONFIG_ESP32_WIFI_NET_ALLOC_SPIRAM` moves to
`soc/espressif/common/Kconfig.spiram`, because it also serves the network stack without Wi-Fi.

The display adds two: `arch/xtensa/core/elf.c` takes a pointer one past the end of a section (see "Bugs
found"), and LVGL's source lists move to `modules/lvgl/sources.cmake`, in the same order, so that an
application can build LVGL its own way.

On the MicroPython fork, the port's source lists move to `ports/zephyr/sources.cmake`, for
`micropython.cmake` to use.

The ESP32-S31 adds two, on `zsdr` past the commit `west.yml` pins, as is the S31's support
itself: `CONFIG_LLEXT_REGION_HEAPS` depends on
`CONFIG_LLEXT_HEAP_DYNAMIC` alone (the exec alias it required is Xtensa's), and
`arch/riscv/core/elf.c` checks `R_RISCV_JAL` against the J type's +-1 MiB instead of the
2 GiB of an `auipc` pair, which let a jump between regions too far apart through.

## The standalone apps (2026-10-09)

Each app's S3 build, read from `zephyr.elf` (the gap is from the end of the DRAM image to the
first dump bank the app reserves):

| App | IRAM | DRAM | Gap to first bank | High region | Flash code | Flash rodata | PSRAM |
| --- | --- | --- | --- | --- | --- | --- | --- |
| capture | 61 KB | 170 KB | 125 KB | 0 | 341 KB | 447 KB | 0 |
| rfprobe | 67 KB | 189 KB | 106 KB | 0 | 408 KB | 528 KB | 0 |
| wpan | 89 KB | 164 KB | 0.2 KB | 37 KB | 430 KB | 532 KB | 2.6 MB |
| osmosdr | 87 KB | 163 KB | 0.4 KB | 37 KB | 413 KB | 530 KB | 2.6 MB |
| stream | 84 KB | 228 KB | 1.7 KB | 20 KB | 491 KB | 628 KB | 2.6 MB |
| link | 92 KB | 228 KB | 1.0 KB | 39 KB | 460 KB | 599 KB | 1.7 MB |

The part every app has (kernel, drivers, Wi-Fi and PHY blobs) is 60 to 75 KB of IRAM and 60 to
90 KB of DRAM; the apps' own DRAM is mostly buffers. Imports per app from the host: capture 17,
rfprobe 56, wpan 60, osmosdr 68, stream 108, link 92; 197 symbols in the union.

## Design choices

- **No new partition table.** The volume takes the upper 4 MB the board's 4 MB table leaves
  unused, so MCUboot, the slots and DFU stay compatible with the standalone apps.
- **No `fs_loader`.** It reads the symbol table and every relocation entry by entry (about four
  times the file size in flash reads) and would overflow main's stack; the host reads the file
  whole into PSRAM and keeps the buffer loader.
- **The module builds its own list of the driver's sources** (`wifi.cmake`), not the hal's. The
  alternative, a host with `CONFIG_WIFI_ESP32` and those sources taken off its targets, would leave
  the hal's global Wi-Fi definitions on everything the host compiles, and the radio's PHY-only
  path depends on the driver being off.
- **A build id, not an ABI hash with a manual bump.** llext binds by name only, so a file is
  accepted only from a host built from the same inputs.
- **Built-in copies stay on** (`CONFIG_MULTIAPP_EMBEDDED`) until the volume has proven itself on
  more boards; off, the image shrinks by 340 KB. `wifi` has none: at 540 KB it would be most of the
  image.

## Risks

- Extension code cannot run while flash is written (cache off): interrupt handlers and code that
  runs then stay in the host.
- No clean unload: exit is a reboot.
- The local Zephyr commits above, until upstream.
- link throughput between two boards is untested.
- A hal update can change what the Wi-Fi driver needs. A renamed source fails the build and a missing
  symbol fails the host's link, but `wifi.cmake`'s list and definitions are ours to keep in step.
- USB has stopped answering twice on the devkit, and this has not been explained. The first time was a
  `wpan` boot idle after a shell session: no shell output and failed control requests for at least 6
  minutes, with no reset from the liveness watchdog. The second was a drive boot with no app or radio,
  after 12 idle hours. A standalone `rfprobe` has shown the same symptom. The second time, the chip
  answered normally over its UART, but its USB failed to enumerate even in ROM mode, with none of this
  firmware running, until a later replug. So the USB path or the chip's USB pins are involved, not only
  the firmware. A reset through the devkit's UART bridge recovered it after that.
- The PC's write cache: files copied onto the drive reach the flash only when the PC writes them
  out, which ejecting guarantees; `multiapp run` reminds of it in a drive boot.
