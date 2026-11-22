# multiapp internals

<!-- nav-start -->
Previous: [multiapp](../../apps/multiapp/README.rst) · Up: [Applications](index.md) · Next: [micropython](../../apps/micropython/README.rst)
<!-- nav-end -->

The design and measurement log of [multiapp](../../apps/multiapp/README.rst), one firmware that
runs the others. Its README has the usage: building and flashing, the shell commands, adding an
app, the S31 board. This page follows it in [Applications](index.md).

Status:

- All six apps (`capture`, `rfprobe`, `wpan`, `stream`, `osmosdr`, `link`) run as llext extensions
  from PSRAM, built from their unchanged sources, one app per boot, on the image of
  `apps/multiapp/` ("the host").
- The radio is extensions as well: the PHY, `lib/esp_sdr` and `lib/ieee802154` are modules a boot
  loads only if its app needs them ([The radio as modules](#the-radio-as-modules)).
- So is the stock Wi-Fi driver, which the small `wifitest` app uses
  ([The stock Wi-Fi driver as a module](#the-stock-wi-fi-driver-as-a-module)).
- MicroPython is an app too: on the XIAO with its display, LVGL, knob, SD card and two USB drives,
  on the Communicator with its display and keyboard, on boards without a display headless
  ([MicroPython as an app](#micropython-as-an-app)).
- The host loads each one from its `<name>.llext` file on a FAT volume in flash, which the PC sees
  as a USB drive in boots without an app; built-in copies are the fallback
  ([Apps as files](#apps-as-files)).
- It needs Zephyr changes that are on the fork `west.yml` pins ([Zephyr changes](#zephyr-changes)).
- On the ESP32-S31 the extensions are RISC-V relocatable objects and the board boots into
  `osmosdr_hs` ([ESP32-S31: RISC-V extensions](#esp32-s31-risc-v-extensions)).

## Constraints

- **Every app still builds and runs standalone**, with its own `prj.conf`, overlay and `main()`,
  unchanged in behavior. multiapp compiles the same sources; no app source moves.
- **Internal RAM is the binding constraint.** IRAM and DRAM share one pool, and the image must end
  below the dump banks the configured apps reserve ([Code placement](../dev/code-placement.md)).
  Extensions therefore run from PSRAM.
- **Kernel configuration cannot differ per app.** USB classes, network buffers, SMP and heap sizes
  are fixed when the host is built; the host carries the union of what its apps need.
- **Constructs that register through linker iterable sections** do not work in an extension as
  written, because its sections do not join the host's: `SHELL_CMD_REGISTER`,
  `USBD_DEFINE_CLASS`, `SYS_INIT`, `LOG_MODULE_REGISTER`, `K_THREAD_STACK_DEFINE`, static memory
  slabs, `NET_BUF_POOL_DEFINE`.
- **Exit is an unload, or a reboot.** `multiapp run` stops the loaded app, unloads it and the
  modules the next app does not take over, and loads the rest, when the two boots are compatible
  ([Unloading](#unloading)). A different USB profile, a console app, anything that refuses to stop
  or an error half way leaves the name in RTC memory and resets, as before.

## How it works

### Code in PSRAM

- The S3's cache MMU maps the data bus window (0x3c000000) and the instruction bus window
  (0x42000000) through one table.
- So a PSRAM page written at data address D executes at D + 0x06000000, after a data cache
  write-back and an instruction cache invalidate.
- llext keeps one address per region. A local Zephyr change (`CONFIG_LLEXT_EXEC_ALIAS`,
  `llext_heap_init_harvard_exec()`) copies and patches through the writable address and uses the
  alias for relocations, the symbol table and the init/fini check.
- The ESP cache driver has no instruction cache operations: the host syncs the range itself after
  `llext_load()` (`sync_text()` in `src/apps.c`).

Both heaps are in PSRAM and not zeroed at boot:

| Heap | Option | Size |
| --- | --- | --- |
| Code | `CONFIG_MULTIAPP_INSTR_HEAP_KB` | 1344 KiB |
| Data | `CONFIG_MULTIAPP_DATA_HEAP_KB` | 1024 KiB |

### Prelude, host glue and app Kconfig

#### The prelude

`include/multiapp/ext.h`, put in front of every C source with `-include`, turns what an extension
cannot do into what it can:

- `SHELL_CMD_REGISTER()` and `SYS_INIT()` become constructors.
- `LOG_MODULE_REGISTER()` becomes a declaration.
- Slabs are initialized in a constructor.
- `NET_BUF_POOL_DEFINE` leaves a descriptor in `.multiapp_host`, from which the host defines the
  pool: net_buf finds pools by their index in a host section.
- `.noinit` and the PSRAM section attributes join `.bss`, because llext loads only one NOBITS
  section.
- The app's `app_usb_init()` becomes a call that does nothing
  ([Flash access from a PSRAM stack](#flash-access-from-a-psram-stack)).

#### Generated host glue

- `exports.py` makes the host export exactly the symbols the extensions import, in its own export
  group, and register their log modules.
- Zephyr's default export groups, every syscall kept in IRAM, are off.
- A symbol the host lacks fails the host's link.
- With `CONFIG_LLEXT_IMPORT_ALL_GLOBALS` the host finds an app's hooks by name, such as
  `app_usb_dfu_prepare()`.

#### The app's own Kconfig values

An extension compiles against the host's `autoconf.h`. `appconfig.py` writes the app's
`CONFIG_APP_*` values into a header included after it.

#### Proxy shell roots

- The host (`src/apps.c`) registers the apps' root commands (`probe`, `wpan`, `sdr`, `link`,
  `ofdm`, `hello`) as dynamic commands that forward to the loaded app.
- So the shell, and the tools that drive it, look as standalone.
- The app's `main()` runs in a host thread (`CONFIG_MULTIAPP_STACK_SIZE`, stack in PSRAM).

### Modules

Modules (`multiapp_module()`) are extensions without a `main()` that other extensions call.

- Each is named by the apps and modules that need it (`REQUIRES`).
- The host loads them in order before the app, each from its file with its
  [trailer](#the-trailer) checked as an app's, and runs their constructors.
- llext finds what an extension imports in the host first, then among the loaded extensions.
- `exports.py` leaves what a module defines out of the host's exports, and modules link with
  `-Bsymbolic`, so their own globals stay theirs even where the host has the same name.

#### Modules in internal RAM

`INTERNAL` places a module as a standalone image places the same code:

- `IRAM_ATTR` code (`.module_fast`, `ext/module_fast.ld`), data and bss in internal RAM;
- the rest of its code and its read-only data in PSRAM, where a standalone image has flash.

Loading it:

- `CONFIG_LLEXT_REGION_HEAPS` lets one load take each region from a heap of its own, and
  `LLEXT_MEM_TEXT_ALT` gives llext the second code region.
- That region comes from `module_heap` (`src/apps.c`, below bank 0, see
  [Internal RAM: bank 0 and the area](#internal-ram-bank-0-and-the-area)) and runs at the
  instruction bus alias of its data bus address (`SOC_I_D_OFFSET`): the same trick as PSRAM,
  without a cache to sync.
- `HIGH_BSS` puts bss in the high region ([Code placement](../dev/code-placement.md)): `sdr`'s DAC
  state.
- What llext only reads while loading stays in PSRAM.

#### Linking internal modules

- `INTERNAL` modules link with `--no-relax`.
- `fastcheck.py` fails the build if a blob's fast code reaches other sections PC-relative. The
  linker leaves no relocation for that, so it would point elsewhere once the two regions are loaded
  apart.
- The PLT stays at the start of the cold code, where the linker's own script puts it next to
  `.got.plt`. llext writes each import straight into the caller's literal (`R_XTENSA_JMP_SLOT`), so
  the PLT's lazy-binding stubs should never run
  ([The PLT in internal RAM](#the-plt-in-internal-ram)).

#### The radio as modules

| Module | Holds |
| --- | --- |
| `phy` | `libphy`, `librftest`, the HAL's PHY glue, the radio arbiter (`esp_sdr_arb.c`, S3) |
| `sdr` | `lib/esp_sdr`, brought up by `multiapp_module_up()` in `ext/sdr_up.c` |
| `ieee154` | `lib/ieee802154` |
| `wifi` | the Wi-Fi driver and its blobs, REQUIRES `phy` ([the driver](#the-stock-wi-fi-driver-as-a-module)) |
| `espnow` | `espnow.o` of `libespnow.a`, a Zephyr-side API and the `espnow` shell, REQUIRES `wifi`, all in PSRAM ([ESP-NOW](#esp-now-as-a-module)) |

- The host has none of them and no Wi-Fi driver (`CONFIG_ESP_SDR_PHY_ONLY`,
  `CONFIG_ESP_SDR_AS_EXTENSION`).
- A boot loads what its app names, and a boot without an app brings no radio up.
- On the S3 the arbiter (`CONFIG_ESP_SDR_ARB=y` in `socs/esp32s3_procpu.conf`) sits in `phy`,
  so one copy serves `sdr` and `wifi`, whichever loads. `phy` links with `--wrap` of
  `esp_phy_enable()` and `esp_phy_disable()`, so the arbiter's `__real_` calls are `phy`'s own;
  `wifi` links with the same wraps, and its calls reach `phy`'s exported `__wrap_` pair.
- The library does not call `phy`'s code the other way: `phy` loads first and cannot import from
  `sdr` or `wifi`. The SDR registers its side of a claim and a release
  (`esp_sdr_arb_set_ops()`, from `esp_sdr_init()`), and `wifi` its channel
  (`esp_sdr_arb_wifi_attach()`, from its `multiapp_module_up()`).
- The arbiter is S3 only; on the S31 `phy` and `sdr` are one module with its own wrap list.
- The sources come from each library's `sources.cmake`, so standalone apps and modules build the
  same files.

#### Devices in a module

`DEVICES` marks a module that defines devices. Its `DEVICE_DEFINE()` cannot join the host's
sections, so:

- `ext/module_devices.ld` keeps its init entries in the kernel's order, and the host runs them after
  the module's constructors (`do_device_init()`).
- The prelude sends `device_get_binding()` to `multiapp_device_get_binding()`, which also searches
  the loaded modules.
- The table's bounds reach the host through an object (`ext/module_devices.c`), because llext
  exports only functions and objects, not the untyped symbols of a linker script.
- A `DEVICES` module whose table is missing or empty refuses to load.

#### Blobs in a module

`multiapp_blob()`, `LIBS` and `VERSION_SCRIPT` bring Espressif's libraries into a module:

- They are not built for a shared object. Their pointer tables in `.rodata*` need dynamic
  relocations, which the linker refuses in a read-only section, so a copy of the archive gets them
  writable.
- llext then merges sections by flags into regions, and writable data in front of `.got.loc` would
  make the data region enclose it: `ext/module_blobs.ld` moves it behind `.got`.
- The archives also hold test code calling functions the host does not have. A version script names
  the exports and `--gc-sections` drops the rest.
- `ext/phy.ver` leaves 36 KB of code against 84 KB, and 132 bytes of bss against 42 KB. With
  `wifi` sharing the module it also exports the blobs' set, 17 names more (`phy_change_channel`,
  `noise_check_loop`, `register_chipv7_phy_init_param`, `esp_phy_modem_init` and others), the
  wrapped pair and the arbiter's API. A name missing there fails the host's link (`exports.py`
  exports an import no module defines), and `tests/host/test_exports.py` checks the list.

### USB profiles

- All class code lives in the host.
- The S3's OTG controller has 4 IN endpoints for classes (`GHWCFG4.INEps`), and the union of the
  apps' classes needs 6.
- So the host registers a subset per boot: `app_usb_classes()`, a weak hook in `apps/common`
  between registering the classes and `usbd_init()`, unregisters what the boot's app does not name
  in its `USB` mask.

| Class | Used by | IN endpoints |
| --- | --- | --- |
| CDC-ACM (shell) | every boot | 2 |
| CDC-NCM | stream | 2 |
| vendor bulk (`usb_sdr.c`) | osmosdr | 1 |
| MSC (the volume as a drive) | boots without an app | 1 |
| DFU | every boot | 0 |

osmosdr's vendor class is built in the host. Its calls into the app go through forwarders in
`src/osmosdr.c` that look the app's functions up by name, so the boot's app is loaded before USB
starts.

### Internal RAM: bank 0 and the area

#### NCM's buffers in bank 0

NCM's DMA buffers are in bank 0 (`multiapp_bank0.ld`): USB DMA needs internal RAM, and `stream`
never runs the ring, so bank 0 is free in its boots.

#### The area

- An internal RAM area has one user per boot.
- Buffers an app must keep in internal RAM (`ESP_SDR_HIGH_BUF_DEFINE()`,
  `ESP_SDR_HIGH_STACK_DEFINE()`) become host objects in `multiapp_area`, 24 KB of the high region
  (`AREA` in `multiapp_extension()`).
- osmosdr takes 23 KB; the 802.15.4 driver gets the area in the other boots
  (`CONFIG_ESP_SDR_IEEE802154_HIGH_RAM_APP`).
- The area has one owner per boot: an `AREA` app, `ieee154`, or the `wifi` module (its private heap
  is the whole area, so `wifi` is declared `AREA`). The objects of every extension start at the
  area's beginning, so two owners would overlay each other.
- The rule is checked three times. `exports.py` fails the build when an extension that does not
  own the area has buffers in it (`--area-owner`). The loader refuses a second owner
  (`area_claim()`, `-EBUSY`) and an extension with a region inside the area (`area_check()`,
  `-EFAULT`); `ieee154` gets no area when anything owns it. `tests/host/test_exports.py` covers
  the first.
- What went wrong in `coex`: `ESP_SDR_STACK_DEFINE()` follows `ESP_SDR_PLACE_STACK`, the high
  region on the S3, which in an extension is `ESP_SDR_HIGH_STACK_DEFINE()`: a host object in the
  area at offset 0. `coex`'s worker stack (3 KB, painted 0xaa when the thread started) overlaid
  the wifi module's heap. It is pinned to PSRAM now (`place.h`). `wifitest` has no such object.
- `multiapp run` reboots when the USB profile or the area's owner changes.

### Unloading

Replacing the running app without a reboot (`CONFIG_MULTIAPP_UNLOAD`, on by default on the
ESP32-S3 and the ESP32-S31). The code is in `src/apps.c`. The S3's is soaked on the board
(`tmp/coex/t6soak.py`, see the end of this section); the S31's builds and its host tests pass, and
no board has run it yet ([The ESP32-S31, in place](#the-esp32-s31-in-place)).

#### When it applies

`multiapp_request()` switches in place when all hold, and reboots otherwise:

- the next app has the same USB profile as the boot (the classes the host registered at boot stay);
- neither the running app nor the next one is a `console` app (the shell gave up the port for good);
- with an app loaded, the profile is not the MSC drive (the PC may hold it);
- the app stops (below) and every step of the unload succeeds. An error after the app ended leaves
  what was loaded and reboots into the next app; the boot then has no state to clean.

The area has one owner per boot, and the owner leaves with its extension (`area_owner`, `area_app`
follow the loaded app: osmosdr's buffers and the Wi-Fi heap take turns). Bank 2 as a second module
heap (`BANK2`, `coex`) changes what the SDR module learns when it loads, so a switch that adds or
drops `BANK2` unloads every module and re-initializes the heap first (it logs what a leak left in
it). Still a reboot: console apps (`micropython`), a changed USB profile (osmosdr with its vendor
class, stream with NCM, against the rest), and any app whose `main()` does not return and that has
no stop hook (`capture` has one, between two steps of its survey: `rfprobe` is the app of the soak).

#### The order

1. **The app.** Its optional `multiapp_app_stop()` (found by name, as `app_usb_dfu_prepare()`; the
   `wifitest` and `coex` apps have one: net_mgmt callback, the `coex` run's thread) asks `main()` and
   the app's threads to end. The host joins `app_thread` for `CONFIG_MULTIAPP_STOP_TIMEOUT_MS`; an app
   still running then keeps running and the request reboots. `multiapp_app_stop()` returning an error
   does the same.
2. **The app's extension** (`ext_unload()`): use count checked first (`llext_unload()` counts down
   even when it returns early, so a module that is still imported from must not reach it),
   `llext_teardown()` (the destructors, below), a sweep of threads that live in its regions (the
   thread, its stack or its entry in a region: aborted, logged as a warning, since such a thread may
   hold a host lock), its shell roots, then `llext_unload()` (which flushes the deferred log first).
   The USB message hook the app set is dropped, and the loaded app, its info and its devices are
   forgotten.
3. **The modules**, newest first, as far as the next boot's order differs. The next boot's list is
   what `load_modules()` would load (each module after those it requires). The loaded modules are
   a stack: those of the common prefix stay, everything after the first difference goes. So `coex`
   (phy, wifi, sdr, espnow) to `wifitest` (phy, wifi) drops espnow and sdr; `wifitest` to `rfprobe` (phy,
   sdr) drops wifi and keeps phy; `rfprobe` to `wpan` keeps phy and sdr. The prefix rule also makes
   the SDR reload after Wi-Fi attaches or detaches, which it needs: an SDR that initialized without
   Wi-Fi holds the radio for good, one that initialized beside it claims per window. Per module,
   `multiapp_module_down()` runs first (below), then `ext_unload()`, then the area owner is cleared,
   and its entry leaves the table (the devices it defined are forgotten there:
   `multiapp_device_get_binding()` no longer finds them).
4. **The next app** loads as at boot (`multiapp_start()`): the missing modules, then the app, its
   constructors and `main()` in `app_thread`. The RTC request record names it, so `multiapp crash`
   and a crash loop see what died.

#### Destructors

`include/multiapp/ext.h` and `ext/wifi_prelude.h` turn registrations into constructors; each has its
destructor in the same macro, which `llext_teardown()` runs (the extension's `.fini_array`):

| Constructor | Destructor |
| --- | --- |
| `SHELL_CMD_REGISTER` root | `multiapp_shell_unregister()` |
| `SETTINGS_STATIC_HANDLER_DEFINE` | `settings_deregister()` (new in the fork) |
| `K_THREAD_DEFINE` | join for 100 ms, then abort |
| `NET_MGMT_REGISTER_EVENT_HANDLER` (wifi) | `net_mgmt_del_event_callback()` |
| `SYS_INIT`, slabs | none: no inverse, the module's down hook is it |

`tests/host/test_multiapp_unload.py` fails a constructor macro without a destructor or a
`no destructor:` reason.

#### What `multiapp_module_down()` does

Symmetric to `multiapp_module_up()`, exported by name. A nonzero return keeps the module loaded
(the request then reboots).

- **wifi** (`ext/wifi_glue.c`): `esp32_wifi_shutdown()` of the driver ([Zephyr changes](#zephyr-changes))
  disconnects, cancels the driver's work, takes the interface down, `esp_wifi_stop()` and waits for
  the stopped event, `esp_wifi_deinit()` (which deinitializes the supplicant, coexistence and ends
  the blob's tasks), removes the driver's `net_mgmt` callback and ends the event thread cleanly
  (sentinel, `k_thread_join()`; never aborted, it may hold net locks). Then a flush of the system
  work queue (the blob's task cleanup is a work item of the module), the hal's
  `esp_wifi_adapter_release_isrs()`, `multiapp_wifi_unbind()` (interface down, IPv4 addresses and
  gateway removed, the host's idle device back, `NO_AUTO_START`), `esp_sdr_arb_wifi_detach()`. It
  prints what the private heap still holds (a leak) and any clock reference left.
- **sdr** (`ext/sdr_up.c`): `esp_sdr_release_radio()` stops a ring run (`esp_sdr_ring_stop()`, waits)
  and a burst, ends the DAC's player and filler threads (`esp_sdr_tx_dac_release()`: quit flag,
  semaphores, `k_thread_join()`), releases the arbiter's claim (standing or window: the PHY powers
  down unless Wi-Fi holds it) and clears the arbiter's hooks into the library (`set_ops(NULL)`,
  stop hook).
- **espnow** (`ext/espnow.c`): callbacks off, `esp_now_deinit()`, the receive queue and callback
  forgotten.
- **phy** (`ext/phy_down.c`): `esp_sdr_arb_shutdown()`: refuses (`-EBUSY`) if a client holds the PHY,
  else powers it down and drops the hooks. The PHY's PLL timer goes with the last `esp_phy_disable()`.
  It unloads only when no module of the next boot needs it; it reloads with the stored calibration.

#### Interrupts of the blob (hal fork)

The S3 adapter records what the blob installs: `set_intr_wrapper()` the (CPU, source, line) routes,
`set_isr_wrapper()` the line and the CPU that enabled it. `esp_wifi_adapter_release_isrs()` routes
every recorded source to the invalid line (6) on both CPUs, disables each line on the CPU that
enabled it (a thread pinned to the other CPU does it when that is not the caller's: the enable bits
are per CPU) and connects the spurious handler back. `clear_intr_wrapper()`, empty in the vendor's
adapter, does the same for one source. Wi-Fi is on core 0 in a multiapp boot (`ext/wifi.conf`), but
nothing here assumes it. The soak reads the matrix rows of the Wi-Fi sources (`0x600C2000 + 4 *
source` for core 0, `0x600C2800` for core 1) and the FROM_CPU rows, which must not change.

#### Memory and the heaps

- `module_heap`, `module_high_heap` and the bank-2 heap are `k_heap`s that llext's region heaps free
  into. The modules unload newest first so that the first-fit free list merges back; `multiapp info`
  prints, per heap, free, **largest free block**, used and peak (`heap NAME: free F largest L used U
  peak P`, found by trying allocations since a heap's statistics have no such number): module_heap,
  module_high_heap, bank2_heap in a `BANK2` boot, `psram_code`, `psram_data`, `kernel`. Fragmentation
  shows in `largest` first. It also prints `uptime` and the area's owner.
- The Wi-Fi heap is the area, one `k_heap` the module initializes at load: a leak is invisible after a
  reload, so module_down prints what is still allocated.
- Blob buffers in PSRAM come from the shared heap (`CONFIG_ESP_WIFI_HEAP_SPIRAM`) today, wrapped to
  the private heap in the Wi-Fi module; the soak's `psram_*` and kernel lines show if any leaks into
  the host's.

#### The ESP32-S31, in place

The same code, with these differences (nothing of it has run on a board yet):

- **USB profiles.** `hello`, `capture` and `rfprobe` have no class of their own (profile 0) and
  switch among themselves in place; `capture` ends through its `multiapp_app_stop()` (a flag looked
  at between two survey steps and a semaphore for the pause), as its `main()` never returns.
  `osmosdr_hs` has the vendor class (profile 2): to and from it, a reboot.
- **`osmosdr_hs`** (`apps/osmosdr_hs/src/main.c`, in the multiapp build only, under
  `#if defined(MULTIAPP_NAME)`): `multiapp_app_stop()` sets a quit flag, ends a ring run
  (`stream_replan()`) and wakes `main()`, which leaves its loop. The library's ring interrupt is
  taken and freed per run, so no ISR or thread of the app is left. The flag is read after the
  generation (`multiapp_app_stop()` sets the flag first) and by the ring's sink, so that a stop
  landing between the two cannot be lost. Before `main()` returns it cancels the IN batches still
  queued and waits for their completions, which would otherwise reach the next app's handler. Its
  destructor, which the host runs when nothing calls into the app any more, cancels the work items
  and every transfer the vendor class holds (the class stays in the host, open on the PC). A
  restarted app arms the OUT reads itself, as the PC sends no new link event.
- **The vendor class forwarders** (`src/osmosdr.c`) keep the app's handlers (found by name) until
  it unloads: `app_cutoff()` makes `multiapp_app_fn()` return nothing (`loaded_info` cleared),
  clears the USB message hook, then `multiapp_osmosdr_drop()` forgets the handlers, waits for the
  USB thread to leave the ones it is in, and forgets again (a forwarder counts itself in before it
  reads the cache, so one that starts after the first forget finds nothing). `-EBUSY` after a
  second puts the app back as it was, loaded, and reboots; the same cut-off runs when an app's
  constructors fail. `tests/host/test_multiapp_forwarders.py` runs the forwarders on the host.
- **`sdr`** (the one radio module, `ext/sdr_up.c`): `esp_sdr_release_radio()` has an S31 version in
  `lib/esp_sdr/src/esp_sdr.c`. It ends a ring run that is still going, takes `esp_sdr_lock` with a
  one second timeout (a lock a dead thread holds is `-EBUSY`: the replay and the paced streams must
  be stopped by the thread that started them), stops a DAC loop and a tone, marks the library not
  ready and undoes `radio_up()`: `phy_wifi_enable_set(0)`, `esp_phy_disable()` (the PHY's RF off),
  the host's modem clock reference, the power domain (a no-op on this chip). There is no arbiter,
  and no thread or interrupt of the library outside a run. What it does not undo: the dump engine's
  forced clocks and the TCM clock force (`dump_regs()`), which a reload sets up again, and the ROM
  PHY's own state. The module unloads only if the next boot's list leaves it out (no app does
  today) or through `multiapp_module_unload("sdr")`; a reload runs the PHY's full calibration again.
- `multiapp info` has a `psram_ext` line (the one llext heap in PSRAM) where the S3 has
  `psram_code` and `psram_data`.

#### The per-extension API, and the kernel swap

`src/apps.h` exposes `multiapp_module_load(name)` (a module with the modules it `REQUIRES`, its
constructors, its devices and `multiapp_module_up()`) and `multiapp_module_unload(name)` (its down
hook, use count, destructors, thread sweep, shell roots, regions; `-EBUSY` while another extension
imports from it). They work on any module, not only the newest. A later feature, one small
kernel as an llext swapped into a running app for one DSP stage, plugs in here without a
reboot or a stop of the app:

1. The kernel is a module (`multiapp_module()`, no `main()`) that exports one function, built by
   the same CMake function as the others; its file is on the volume and built in.
2. The app (or a host helper) calls `multiapp_module_load("kernel_b")`, reads the function with
   `llext_find_sym()`, waits for a quiescent point of its stage (the ring's unit boundary, a frame
   gap), stores the new function pointer, and calls `multiapp_module_unload("kernel_a")`.
3. The unload then does what it does for any module: use count (the app does not import the kernel
   by symbol, so the count is 1: it holds only a pointer, which is why the swap must have stored the
   new pointer first), destructors, the thread sweep (a kernel with a thread is a bug and is logged),
   regions back to their heap. The only thing the pointer swap must guarantee is that no thread is
   inside the old code: a spinlock-free handoff at the stage boundary, and a wait of one more unit.
4. Both versions are in the code heap at once for that moment: the PSRAM code heap has to hold two
   kernels (`psram_code` free in `multiapp info`).

#### The soak

`tmp/coex/t6soak.py` (scratch) drives the CDC-ACM shell: N cycles of `multiapp run wifitest`,
join, `multiapp run rfprobe`, `probe rx 1024`, the matrix and `multiapp info`. A reboot shows as
the uptime going down. It prints the cycles that passed, reboots and the drift of each heap's free
and largest block.

Run on the DevKitC (2026-11-24, 100 cycles): 100 of 100 passed, no reboot, every join on the first
try, about 6 s a cycle. Every heap (kernel, `module_heap`, the high region, PSRAM code and data)
ended at the same free and largest block it had after the first cycle. The MAC row sat on core 0
line 0 while Wi-Fi ran, and the Wi-Fi sources were parked after each unload. Switches into and out
of `coex` (a `BANK2` flip) also stay in place, the bank-2 heap is back to the same use, and a ring
right after the switch had its windows (91 % of the time, no failure).

### Apps as files

#### The volume

- A FAT volume in the upper 4 MB of the flash holds `<name>.llext` (`src/drive.c`). The board's
  4 MB partition table leaves that part out (partition `fat` at 0x400000, flash disk `FLASH`).
- The host mounts it read-only at boot, before USB starts and before an app runs, and compares
  every file with the built-in copy (`multiapp list`).
- It reads the boot's app whole into a PSRAM buffer (`CONFIG_MULTIAPP_FILE_MAX_KB`); llext then
  loads it from there.
- That mount's FATFS object and the drive's SCSI buffer are in bank 0, which the ring overwrites
  once an app runs: nothing of the host touches the volume after that.
- Reads go in 256-byte chunks through the FATFS window, so the flash driver never gets a PSRAM
  buffer.

#### The trailer

- `apptrail.py` puts a trailer on every file: ELF size, CRC-32, the stack the app's `main()` needs
  and a 16-byte build id.
- The build id (`buildid.py`) is a hash of `autoconf.h`, the devicetree, the extensions' imports,
  the headers both sides compile against and the compiler.
- The host checks the trailer before llext reads the file. A truncated, damaged or foreign file
  (built for another host) is refused and the built-in copy runs.
- `CONFIG_MULTIAPP_EMBEDDED` (default y) keeps the built-in copies. They hold 340 KB of the image;
  the [multiapp](../../apps/multiapp/README.rst) README gives them as 165 KB of flash.
- Without them, a boot whose file is refused comes up without an app.
- `multiapp info` says where the app came from.

#### Crash loops

- A boot that dies on the way to its app is skipped once, to end a crash loop.
- A file refused cleanly is not: it is tried again at the next boot, so a correct volume takes
  effect at once.

#### The drive

- A boot without an app (no request, no autostart, or an app that failed to start) registers USB
  mass storage on the volume next to the shell and DFU, for copying app files from the PC.
- Boots with an app unregister it, and `multiapp run` from a drive boot reboots into the app.
- The PC must eject the drive first: the host cannot see what the PC still caches.
- `west dfu --storage` writes the build's volume image to the DFU target `fat`. `mkfat.py` builds
  that image: deterministic FAT12, 4 KiB clusters on the erase page, long names.
- `west dfu --esptool` and the shell command `usj` are in the
  [multiapp](../../apps/multiapp/README.rst) README.

#### Writers and readers of the volume

Who may touch the volume while an app runs (src/drive.c, `include/multiapp/multiapp.h`):

- **Writers take the lock.** The device's own writes (the crystal calibration today, files later)
  are bracketed by `multiapp_drive_write_begin(timeout)` and `multiapp_drive_write_end()`, called by
  the same thread. The lock is one `k_mutex`: `-EAGAIN` when another writer kept it for the
  timeout, `-EBUSY` while the PC has the volume as a USB drive (`multiapp_drive_msc_set()`, set by
  `multiapp_drive_export()` in a boot without an app; the hook the boots that run an app beside MSC
  will drive). `esp_sdr_cal_save()` and `esp_sdr_cal_clear()` (`lib/esp_sdr/src/esp_sdr_calstore.c`,
  the settings file `/FLASH:/settings`, osmosdr_hs's SET_CAL) take it in the multiapp build only
  (`CONFIG_ESP_SDR_AS_EXTENSION`); standalone apps name no multiapp symbol. The DFU target `fat`
  rewrites the whole volume at the PC's request and is not routed through it.
- **Readers remount.** The PC may have written the volume since the device mounted it, so a reader
  mounts it afresh (`f_mount(..., 1)`, which reads the boot sector and FAT again), checks the
  trailer CRC of what it read and retries on a mismatch (a write may be half done).
- **Threads that touch flash need an internal stack.** The flash driver turns the cache off while
  it erases or writes, and a stack in PSRAM is behind that cache: the thread that writes (the USB
  thread for SET_CAL, 4096 bytes) and any thread of an extension that calls a writer must have one
  in internal RAM.

### The stock Wi-Fi driver as a module

The module is built by `wifi.cmake`. The host is built without the driver, so neither fork
changes.

#### What it builds

- What a build with `CONFIG_WIFI_ESP32` adds to one without: the driver, the hal's glue, 108
  supplicant files and the blobs.
- The supplicant uses its internal crypto, so there is no mbedTLS (`ext/wifi_shim`).
- The PHY is the `phy` module's (`REQUIRES phy`), shared with the SDR. The module has no `libphy`
  or `phy_init.c` of its own; `phy.ver` exports what it imports, and `esp_phy_enable()` and
  `esp_phy_disable()` go to the arbiter (`WRAP`). Its `multiapp_module_up()` tells the arbiter
  that Wi-Fi is in this boot (`esp_sdr_arb_wifi_attach()`).
- A Wi-Fi boot therefore loads `phy` first, and `sdr` too when its app asks. `wifi` also cannot
  load with an `AREA` app or `ieee154`: its heap is the area (a CMake error).

#### The PHY, the arbiter and PLL tracking

- The arbiter owns the real PHY power calls. Wi-Fi's enable and disable are two clients of it
  beside the SDR's claim ([Coexistence](../radio/coexistence.md)).
- The hal's PLL tracking (`phy_common.c`, a timer every second and a call at each enable) is on
  only where `CONFIG_ESP_WIFI_ENABLED` is set. Wi-Fi had it in its private copy of the file; the
  shared `phy` is compiled with it too, which would also track while the SDR holds the PHY (its
  windows, its standing claim) and open gaps in the ring.
- So `phy` links with `--wrap=phy_param_track_tot` and the arbiter lets that call through only
  while Wi-Fi holds the PHY and the SDR does not (`win.wifi_on && !win.claimed`, read without a
  lock because the timer holds the PHY lock the arbiter's real calls take). The timer runs as it
  did before; its callback does nothing in boots without Wi-Fi and under an SDR claim.
- The SDR alone: `esp_sdr_init()` finds no Wi-Fi attached and takes a standing claim with no
  deadline, so `osmosdr`, `wpan`, `capture` and the others need no claims. With Wi-Fi attached it
  takes none and an app claims per window (`coex`). A standing claim held when Wi-Fi attaches
  (`sdr` loaded first) is released; an app that needs both should require `wifi` before `sdr`.

#### ESP-NOW as a module

- `libespnow.a` has two objects. `manatick.o` (the `mt_` layer, `g_mt`, `mt_get_peer_info`,
  `g_espnow_user_oui`) is read by `libnet80211`, so it stays in `wifi`; `espnow.o` (the `esp_now_`
  API) is the `espnow` module's, which loads after `wifi` and cannot be imported from. `multiapp_blob()`
  makes both copies with `ar d` (`NAME`, `DROP`): `wifi` without `espnow.o`, `espnow` without
  `manatick.o`. `manatick.o` needs nothing from `espnow.o`.
- `espnow.o` imports 9 functions, 2 objects (`g_wifi_menuconfig`, `g_wifi_nvs`) and the 20 `mt_`
  functions from `wifi`, and `g_osi_funcs_p` and `memcpy` from the host. `wifi.ver` exports them,
  and `esp_wifi_get_mode()`, `esp_wifi_set_mode()`, `esp_wifi_get_mac()` and
  `esp_wifi_connectionless_module_set_wake_interval()` for the module's own code
  (`tests/host/test_exports.py`).
- `ext/espnow.c` is the module's code: `include/esp_sdr/espnow.h` (init in station mode if the
  driver left none, peers, `esp_sdr_espnow_send()` with a counted send status, receive into a
  `k_msgq` or a callback, wake window, statistics) and the `espnow` shell (`init`, `peer`, `send`,
  `stats`, `window`, `rx`), the commands of `coexbench`. `espnow.ver` exports that API and
  `esp_now_*`; a root command needs a `PROXY(espnow)` in `src/apps.c`.
- Nothing is in internal RAM (not `INTERNAL`, no `IRAM` sections in `espnow.o`): 6806 bytes of code,
  1261 of blob data and 2380 of bss, a 19 KB file, so `coex`'s boot, whose `module_heap` the `phy` and
  `wifi` modules fill and whose bank 2 holds `sdr`, takes it without the internal RAM changing.
  The file has no built-in copy (`FILE_ONLY`).
- The apps: `coex` requires it, and `espnowtest` (`ext/espnowtest.c`, no code of its own) boots
  `phy`, `wifi` and `espnow` without joining an access point.
- `wifi` itself grew: it exports what `espnow.o` calls, so the linker keeps more of `manatick.o` and
  `net80211` (code 256669 to 268465 bytes, read-only 43746 to 46578, all PSRAM), and its internal
  bytes by 116 (data 3592 to 3596, bss 15432 to 15544; code 2529 as before). Its internal total is
  22.8 KB, `phy` 10.7 KB, so `wifi` and `phy` still take 33.6 of `module_heap`'s 35.8 KB.

#### The coex app

- `ext/coex.c` and `ext/wifitest.c` (the `wifi` commands, the AP password only from the shell) on
  `REQUIRES wifi sdr espnow`, no `AREA`: ESP-NOW under the ring windows.
- `coex sdr`, `coex arb`, `coex ps` are `coexbench`'s on CPU 1 with ring windows
  (`ESP_SDR_RING_WINDOW` ends, sink gaps counted), plus `coex listen` and `coex chan`. The driver
  calls it makes (`esp_wifi_set_ps()` and a few more) are exported by `wifi.ver`. The PHY event
  log stays in `coexbench`.
- Its ring sink runs from PSRAM (`place.h`), as osmosdr's.

#### Its Kconfig

- A second Kconfig pass runs on the host's fragments with `ext/wifi.conf` on top.
- `extconfig.py` writes the difference as a header each source includes after `autoconf.h`.
- It fails the build if a symbol that is not the module's own differs, because that could change
  what host and module agree on, such as a kernel struct.

#### Placement

- It is placed as an image with the driver and its IRAM options off (`ext/module_wifi.ld`).
- `.wifi*iram` and the blobs' read-only data go to PSRAM, where such an image has flash.
- Only `.iram1`, `.dram1`, data and bss go to internal RAM.
- The read-only data needs dynamic relocations, so its input sections are writable. A
  `(READONLY)` output section tells llext to load it with the rest of the read-only data.

#### Its memory

- The driver's `k_malloc()` and thread stacks come from a private heap in the area, through
  `--wrap` (`ext/wifi_glue.c`), since no other module uses the area in such a boot.
- The blobs' own buffers are in PSRAM (`CONFIG_ESP_WIFI_HEAP_SPIRAM`).
- The host's kernel heap stays small.

#### Its interface

- A `net_if` must be in a host section, so the host defines one (`src/wifi_netif.c`), down, on a
  device that does nothing.
- As the module loads, its private device takes over `iface->if_dev->dev`, and the stack reaches
  the driver through that pointer on every call.
- The driver's own name for its interface is pointed at the host's (`ext/wifi_prelude.h`), and a
  `NET_IF_DECLARE()` left in it fails the build.
- `wifi_mgmt.c` stays in the host, along with IPv4 and DHCPv4.

#### Its core

The blob wants its task and interrupt on one core, and core 1 is the SDR's in a `coex` boot (its
worker and the ring): a Wi-Fi task pinned there starved with a blob lock held, and `coex sdr`
hung. So the module pins its task to core 0 (`ext/wifi.conf`) as a standalone image does, with USB.
A host without the driver reserves the WMAC interrupt on core 1 (`esp_cpu_intr.c` tests
`CONFIG_ESP32_WIFI_TASK_PINNED_TO_CORE_0`), so the host defines that name for the build
(`zephyr_compile_definitions()` in `CMakeLists.txt`; nothing else in the tree reads it but the
driver) and interrupt 0 is reserved on core 0 in every boot, not on core 1. The S31 does not use
this.

#### No built-in copy

The module is `FILE_ONLY`: it runs from its file or not at all, and the boot then falls back to the
drive. The file is only stat-ed at each boot, not read.

### MicroPython as an app

The app is built by `micropython.cmake` and `ext/mp/`. The host stays `CONFIG_MICROPYTHON=n`, so the
module's own build does not run.

#### What it builds

- The port's sources (`ports/zephyr/sources.cmake`, on the MicroPython fork), the C modules in
  `apps/micropython/cmod`, `boot.c`, `setjmp.S`, `usb_disks.c` and the frozen Python.
- They build with the same flags as the port, through MicroPython's own `py/mkrules.cmake` on the
  extension's target.
- That file reads a target's flags as plain lists, which an llext target's generator expressions are
  not, so `micropython.cmake` gives the qstr scan its flags itself.
- `-Bsymbolic` is not needed: `ext/mp/micropython.ver` exports only `multiapp_info`, and
  `--gc-sections` drops the rest.

#### Its Kconfig

- The second pass, as for Wi-Fi, with `ext/mp/micropython.conf`.
- The port's `mpconfigport.h` includes `autoconf.h` again, which undoes any `#undef` made before it,
  so those are in `ext/mp/mpconfigport_multiapp.h`.

#### The console

- `CONSOLE`: before its constructors run, the boot's host stops the serial shell (`shell_uninit()`,
  `k_thread_join()`), which also ends its log backend, so nothing prints into the REPL.
- `multiapp_usb_up()` starts its `main()` once USB is up, as the standalone app's order is.
- The host's console driver has no input path, so `ext/mp/mp_glue.c` provides the two functions the
  port's `zephyr_getchar.c` calls, on the CDC-ACM port's callback.
- `multiapp run` always reboots into such an app, since it runs on the shell that has to go.

#### esdr.shell()

- Zephyr's `shell_dummy.c` is built into the extension, its stack and buffers in PSRAM, so every
  other boot pays nothing.
- `west dfu` drives the host's commands through it, as standalone.
- It must not become a log backend (`CONFIG_SHELL_DUMMY_INIT_LOG_LEVEL_NONE`), since the host's list
  of those cannot take one.

#### The heap

- The heap is `multiapp_psram_spare()`: everything from the end of what the linker placed in PSRAM
  to the top.
- That space holds the shared heap, which only the `wifi` module imports, and the buffer that held
  the app's file. That is why that buffer is the top of PSRAM and not an array of its own.
- It is one block, so a large buffer fits.
- `CONFIG_APP_MP_PSRAM_HEAP_KB` is a token here.

#### The stack

The stack stays in internal RAM, in the area, because Python writes the flash: `mp_stack`,
`fast_heap` and the radio threads' stacks take 24064 of its 24128 bytes.

#### The file system

- `/FLASH:` is the host's FatFs with MicroPython's settings (8 files, 4 directories, long names up
  to 64) and an fstab entry, with its own FATFS object in internal RAM.
- The flash disk's cache moved out of bank 0, because the ring may run while Python reads a file.
- The host's read-only mount for the app files is gone by then.

#### The display and the board's peripherals

- `CONFIG_MULTIAPP_MICROPYTHON_GUI`, set by the board's fragment, compiles LVGL and the board's
  drivers into the extension (`DEVICES`), so no other boot pays for them.
- On the XIAO: SPI, MIPI-DBI, ST7789V, the knob's input drivers and the SD stack.
- On the Communicator: SPI, MIPI-DBI, NV3007, I2C and the TCA8418 keyboard.
- `micropython.cmake` picks them from what the extension's Kconfig pass enabled, and a device
  without its driver fails the link.
- The host's `.config` has none of them. The XIAO's devicetree would otherwise switch the SD stack
  on in the host, which the second Kconfig pass caught (`CONFIG_DISK_DRIVER_SDMMC=n` there).
- The peripherals come from `apps/micropython/boards/<board>_periph.dtsi`, shared with the
  standalone app, and the extension reads the same board fragment the standalone app does.
- A display board without a fragment fails at configure time: without it the panel's colours come
  out wrong ([Wrong colours](#wrong-colours)).

#### Two USB drives

- The host defines two LUNs, because their list is a host section (`src/drive.c`).
- In a drive boot the first is the volume, and the second, on a board with `sd_fs`, has no medium.
- A MicroPython boot points the first at `UFLASH` and the second at `USD`, the proxies
  `src/usb_disks.c` registers from the extension, unchanged from the standalone app.
- They report no medium while the board writes a volume.

## Design choices

- **No new partition table.** The volume takes the upper 4 MB the board's 4 MB table leaves unused,
  so MCUboot, the slots and DFU stay compatible with the standalone apps.
- **No `fs_loader`.** It reads the symbol table and every relocation entry by entry (about four
  times the file size in flash reads) and would overflow main's stack. The host reads the file
  whole into PSRAM and keeps the buffer loader.
- **The module builds its own list of the driver's sources** (`wifi.cmake`), not the hal's. A host
  with `CONFIG_WIFI_ESP32` and those sources taken off its targets would leave the hal's global
  Wi-Fi definitions on everything the host compiles, and the radio's PHY-only path depends on the
  driver being off.
- **A build id, not an ABI hash with a manual bump.** llext binds by name only, so a file is
  accepted only from a host built from the same inputs.
- **Built-in copies stay on** (`CONFIG_MULTIAPP_EMBEDDED`) until the volume has proven itself on
  more boards; off, the image shrinks by 340 KB. `wifi` has none: at 540 KB it would be most of the
  image.

## Memory budget

### The host

#### Internal RAM

- The host's image (`_end`) ends 5248 bytes below bank 0 (0x3fcb0000) on the devkit.
- On the XIAO it ends 3800 bytes below: its host also carries a second LUN, exFAT and a larger
  drive thread.
- Before the shared `phy` (`module_heap` 31744) the gaps were 12120 and 10096 bytes (2026-10);
  the build of 2026-11-08 had 10144 on the XIAO.
- The image holds `module_heap` (35840 bytes), the volume, the drive (2.2 KB) and what
  MicroPython's `/FLASH:` needs from the host (5.5 KB: the flash disk's cache, a FATFS object and
  the extra file slots).
- The signed image is 880 KB, against 1096 KB with the radio in the host.
- Serving Wi-Fi costs every boot about 4 KB of internal RAM and 80 KB of flash (IPv4, DHCPv4,
  `net_mgmt` and its thread, the interface).

What got the image there:

- no radio and no Wi-Fi driver in the host;
- a kernel heap of 3072 bytes (peak 2144 in any boot, 236 in a drive boot, against 12216 with the
  driver);
- `CONFIG_ESP32_WIFI_NET_ALLOC_SPIRAM`;
- the export group ([Generated host glue](#generated-host-glue));
- `CONFIG_NUM_MBOX_ASYNC_MSGS=0`;
- the app's `main()` stack in PSRAM.

#### PSRAM

Of the 8192 KiB:

- 3.4 MiB the linker places, mostly the code heap (1344 KiB), the data heap (1024 KiB) and the
  transmit ring (1 MiB);
- the shared heap only `wifi` allocates from (1 MiB);
- the top 2048 KiB (`CONFIG_MULTIAPP_FILE_MAX_KB`) for the file being loaded.

In a MicroPython boot everything above the first of those is its heap: 4785664 bytes, one block,
against 5126144 standalone, the same with a display. On the XIAO the app takes 1116427 bytes of the
2368 KiB of code and data heaps.

#### Module sizes

In bytes, from the section sizes of the 2026-11-24 build (the `ieee154` row is from 2026-11-08, its
file is 28304 bytes now). Before the shared `phy` the rows were `phy` 55892 and `wifi` 539948 as
files, `wifi` 8477 internal code, 5460 data and 15580 bss with its own libphy:

| Module | Code in PSRAM | Code internal | Data | Bss | Read-only in PSRAM | File |
| --- | --- | --- | --- | --- | --- | --- |
| `phy` | 38641 | 6025 | 4339 | 364 | 1992 | 68736 |
| `sdr` | 24678 | 8963 | 604 | 14384 (high region) | 864 | 55040 |
| `ieee154` | 16024 | 722 | 768 | 1216 | 452 | 27224 |
| `wifi` | 290816 | 2529 | 4758 | 15432 | 57074 | 485376 |
| `wifi` (`espnow.o` split off, 2026-11-24) | 302612 | 2529 | 4762 | 15544 | 60506 | 509956 |
| `espnow` | 6806 | 0 | 0 | 2380 (PSRAM) | 1261 (data) | 19080 |

The `micropython` app, in PSRAM:

| Build | Code | Read-only data | Data | Bss | File | Relocations | Imported symbols |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Headless | 168558 bytes | 113 KB | 38 KB | 163 KB | 441656 bytes | 8.7 k | 181 |
| With display and LVGL (the XIAO) | 586195 bytes | 236 KB | 122 KB | 164 KB | 1275216 bytes | 26.4 k | 220 |

`esdr`'s FFT and monitor buffers are 128 KB of the headless bss. The symbols are imported from the
host and the modules.

#### module_heap and the high region

- `module_heap` held 29532 of 31640 bytes in a Wi-Fi boot and 20168 in a `wpan` boot
  (`multiapp info`, 2026-11-08), and is empty in a drive boot. It is 35840 bytes now.
- The sum of a module's internal code, data and bss (all but `sdr`'s high-region bss) matches
  `multiapp info` (29528 against 29532 for the old `wifi`). From the 2026-11-24 build: `phy` 10.7
  KB, `sdr` 9.6 KB, `wifi` 22.7 KB, so a Wi-Fi boot (`wifi`, `phy`) needs 33.4 KB and `coex`
  (`wifi`, `phy`, `sdr`) 43.0 KB. That is 7.2 KB more than `module_heap` (35840) has, and the gap
  below bank 0 does not leave it (at 38400 bytes the XIAO's was 1240 bytes and the devkit's 2688).
- So an app can take dump bank 2 as a second heap (`BANK2` in `multiapp_extension()`, set for
  `coex`; [Bank 2 as a module heap](#bank-2-as-a-module-heap)).
- The high region is full: the area (24128 bytes, of which osmosdr takes 24064) and
  `module_high_mem` (14528, of which `sdr` takes 14372).
- micropython takes 24064 of the area too.
- The log queue holds 256 messages; link's shell stack is 3072 bytes.

#### Bank 2 as a module heap

- `BANK2` (an app flag next to `AREA` and `CONSOLE`, in the app table) gives the boot dump bank 2,
  64 KiB at 0x3fcd0000 (`__esp_sdr_bank_start`, the capture bank; the ring's banks 0 and 1 are
  below it). `heaps_init()` makes it a `k_heap` only in such a boot; other boots are as before.
- Loading a module, llext first takes its regions from `module_heap`. On `-ENOMEM` (llext frees
  what it took) the module loads again, as a whole, with the bank-2 heap in `module_heap`'s place
  (`region_heaps_bank2`, the same per-region mechanism; its bss stays in the high region where it
  was). Whole modules, not regions, so a module's code and data stay together. In `coex`
  `phy` and `wifi` (33.4 KB) fill `module_heap` and `sdr` (9.6 KB) goes to bank 2, which has room
  for all three (65536 bytes).
- The SDR then stays off the bank. `ext/sdr_up.c` calls `esp_sdr_bank2_reserve()` when the host
  says so (`multiapp_bank2_reserved()`), and the library:
  - runs every ring on banks 0 and 1 (`spare_bank_app`, unit 0 becomes 2048 pairs; a longer unit
    is `-EINVAL`), whatever the caller's configuration;
  - gives no burst buffer (`esp_sdr_ring_burst_buf()`), and `burst_arm()` refuses already;
  - makes `esp_sdr_bank_usage()` fail with `-EBUSY`, which every user of the bank checks first:
    `esp_sdr_rx_capture()`, `_capture_with()` and the other captures into it, transmit
    (`esp_sdr_tx.c`, DAC), the trigger snapshot and replay (not built here). `esp_sdr_init()`
    does not need the check.
  - Not touched: the bank-1 capture (`esp_sdr_rx_capture_bank()`), which runs outside a ring.
  - The ring stage and chancap, which use bank 2 with `spare_bank_app`, are not built into
    multiapp; `esp_sdr_stage_init()` has no refusal and must not be enabled with `BANK2`.
- Nothing else is in the bank: MCUboot's segments load at 0x3fcb6900 and 0x3fce4f08, outside it
  and only at boot. The heap is not zeroed (llext zeroes its bss).
- `multiapp run` reboots into an app with `BANK2` unless the boot already is one, as for the area.
- `coex` therefore has the ring only (`coex sdr ... burst` is refused).
- Run on the DevKitC: `sdr` lands in the bank (data 0x3fcd0060, fast code at its instruction
  alias 0x403c027c) and its ring windows run beside Wi-Fi.

#### Stacks

Internal RAM stacks are sized so their peak with every app exercised is at most 85 %. The peaks
come from `kernel thread stacks`; main's is in `multiapp info`, the USB thread's in the last DFU
download in `multiapp crash`. In bytes:

| Stack | Peak of size | Note |
| --- | --- | --- |
| main | 1936 of 2432 | the PHY calibrates on it as its module loads |
| `esdr_rx` | 1792 of 2560 | |
| `esdr_tx` | 800 of 2048 | |
| the driver's event thread | 1152 of 1536 | |
| network receive thread | 1680 of 2560 | in PSRAM |
| USB thread | 1008 of 1280 | |
| drive thread | 880 of 1152 | while the PC copies 2 MB |
| drive thread, with the display | 1264 of 1728 on the XIAO (73 %) | the SD stack runs on it |
| `logging` | 77 % | the highest of the others |
| kernel heap | 2364 of 3072 | |
| `IRQ 01` | 512 of 768 | |
| `mp_stack` | 13696 of 16384 (83 %) | the highest; with the `receiver` and `datalink` eggs; 10080 with the others, 13664 standalone |

- The drive thread's peak with the display comes from a card that never reports an error. The SD
  stack's error paths have not run on this stack, which is why it has more room than the rule asks.
- Python's own check stops recursion at about 15520, and C code called from Python at that depth
  has about 860 bytes left.

### The standalone apps, 2026-10-09

Each app's S3 build, read from `zephyr.elf`. The gap is from the end of the DRAM image to the first
dump bank the app reserves.

| App | IRAM | DRAM | Gap to first bank | High region | Flash code | Flash rodata | PSRAM |
| --- | --- | --- | --- | --- | --- | --- | --- |
| capture | 61 KB | 170 KB | 125 KB | 0 | 341 KB | 447 KB | 0 |
| rfprobe | 67 KB | 189 KB | 106 KB | 0 | 408 KB | 528 KB | 0 |
| wpan | 89 KB | 164 KB | 0.2 KB | 37 KB | 430 KB | 532 KB | 2.6 MB |
| osmosdr | 87 KB | 163 KB | 0.4 KB | 37 KB | 413 KB | 530 KB | 2.6 MB |
| stream | 84 KB | 228 KB | 1.7 KB | 20 KB | 491 KB | 628 KB | 2.6 MB |
| link | 92 KB | 228 KB | 1.0 KB | 39 KB | 460 KB | 599 KB | 1.7 MB |

- The part every app has (kernel, drivers, Wi-Fi and PHY blobs) is 60 to 75 KB of IRAM and 60 to
  90 KB of DRAM; the apps' own DRAM is mostly buffers.
- Imports per app from the host: capture 17, rfprobe 56, wpan 60, osmosdr 68, stream 108, link 92;
  197 symbols in the union.

## Measurements

### Radio in modules

On an ESP32-S3 devkit (N16R8) behind a USB hub, which limits bulk transfers to about 890 kB/s, on
the production image. The right column is the same app built standalone with the Wi-Fi driver, on
the same bench. The XIAO has built this host, not run it (its runs:
[XIAO with display](#xiao-with-display),
[Radio in the host, older design](#radio-in-the-host-older-design)).

| Test | Modules | Standalone with driver |
| --- | --- | --- |
| `stream` over NCM (bytes on the PC's interface) | 780, 796, 788 kB/s | 788 kB/s |
| `wpan` C6 to S3, 800 frames on channels 11, 15, 20, 25 | 799, 798, 799 | 799 |
| `wpan` S3 to C6, 800 frames | 794, 799, 800 | 799 |
| `wpan` S3 sends, ACK requested, 200 frames | 197, 196, 197 | 196 |
| `wpan` C6 sends, S3 ACKs, 1600 frames on channels 15, 20 | 1599, 1599 | not on this script |
| `link` QAM decode, qpsk to 4096qam | 10567 to 14657 us | 10414 to 14279 us |
| `link` QAM build, qpsk to 4096qam | 5661 to 9361 us | 5423 to 8883 us |

- The plain frames lost were on channels 20 and 25, which are busy here.
- With or without modules, a batch of 50 frames asking for an ACK usually misses one, as a missing
  ACK or a transmit error (-5). That has not been looked into.

#### osmosdr

`esdr_usb`, each test from a fresh boot:

- `test-in` 893 kB/s.
- Receive at 250 kS/s 8 bit and 125 kS/s 16 bit with 0 gaps and 0 overflows.
- 1.26 M samples transmitted with none dropped.
- 250 kS/s 16 bit overflows as it does with the driver: it exceeds this link.
- Ring 10.7 to 11.3 cycles per pair, and 11.3 to 12.3 at 32x, varying from run to run with or
  without modules.

#### capture and rfprobe

- `capture`'s survey: 0 failed.
- `rfprobe`: tune, gain, tone and receive work.
- `probe temp` returns -ENOTSUP, because the host has no temperature sensor driver.

#### wifitest

The Wi-Fi module on its own PHY, on a WPA2 access point on channel 6 at -44 dBm:

- It joins and gets IPv4 by DHCP and IPv6 by router advertisement.
- The board's pings to the gateway: 100 of 100, the replies sampled taking 4 to 11 ms.
- A PC's pings to the board: 160 of 160, at 56 and 1000 bytes, up to about 240 ms round trip (not
  looked into; Wi-Fi power save is a likely cause).
- Leaving and rejoining works, and so does a reboot with the link up (reset reason 12).
- 10 scans in a row found 13 to 16 access points.
- Updating the image and the volume with the link up and pings running: both confirmed, reset
  reason 12, 119 of 120 pings answered over about the first 6 s of each.
- `sample.esp_sdr.multiapp.wifi.hil` passes; it scans, since joining needs credentials.

After an abrupt reset the access point still has the board associated:

- The first join fails with status 1 in 1.2 s, and the next one works (4 of 4).
- After `wifi disconnect` the first join works (3 of 3).
- An app should try again. Not compared with a standalone image.

#### App switches and boot time

- 56 app switches on the image with Wi-Fi scanning, 28 more on the final one, each into the
  requested app, all with reset reason 12 (a software reset).
- DFU with the ring running.
- The liveness watchdog fires with its feeder suspended, and the next boot falls back to the drive.
- A drive boot loads no module and starts no radio thread.
- From the kernel's start to the app's `main()`, with its modules loaded and the PHY calibrated:
  177 ms (`capture`) to 238 ms (`link`), 386 ms for `wifitest`.
- `link bench`'s float loop and power sum are 1.4 and 1.9 times slower than standalone. This is the
  shell on CPU 0 ([Radio in the host, older design](#radio-in-the-host-older-design)).

### MicroPython on the devkit

On the same devkit, with `manifest_headless.py`, against a standalone `apps/micropython` built
headless for this board. The XIAO is in [XIAO with display](#xiao-with-display). The
Communicator builds this host and has not run it; its runs are in
[Communicator with display](#communicator-with-display).

#### Start and resets

- From the host's `multiapp run` the REPL answers within about 1.5 s. Its boot takes 84 ms for
  `phy` and `sdr` and 222 ms for the app (`multiapp info`).
- Three soft resets give the same heap each time, and files and `esdr.shell()` keep working.
- A 4.69 MB buffer reaching within 11 KB of the top of PSRAM takes a pattern every 4 KiB and reads
  it back.
- `/FLASH:`: 128 KiB written, read back whole and from a seek, directories made and removed, files
  still there after soft resets, the apps' files still loading after that.
- Leaving: three times to `wpan` and back with `esdr.shell("multiapp run wpan")`, and
  `machine.reset()`, all with reset reason 12 and no crash recorded.

#### Firmware updates from the REPL

- `west dfu`, `west dfu --storage`, `west dfu --no-confirm` and `west dfu --esptool` all work from
  the REPL.
- A new image boots as a drive boot, so its check and confirm go through the shell. With an
  autostarted REPL the confirm goes through `esdr.shell()`, which works (the Communicator).
- `--storage` hit "Cannot claim interface - LIBUSB_ERROR_BUSY" on its first try in 3 of 11 updates
  from the REPL and in 0 of 6 from a shell boot; the retry worked each time.
- Later three tries in one second all failed and left the board in DFU mode (`west dfu
  --dfu-board` recovers it).
- fwupd's DFU plugin logged a probe of the new device at that moment, so `west dfu` now waits 2 s
  between tries.

#### Stack

- `mp_stack` peaks at 4880 of 16384 bytes at the REPL, against 4912 standalone.
- Deep recursion takes it to 15520, where Python's own stack check stops it, and the same applies
  standalone.

#### Python speed

Python runs slower, because the interpreter runs from PSRAM and is built position independent.
Three rounds of the same script, in milliseconds, each stable to 1 ms except one wrapped reading on
the standalone image (a negative time, left out):

| | Loop | Calls | Float | List | String | Dict | Bytes | Allocate |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Standalone | 179 | 115 | 128 | 104 | 138 | 62 | 218 | 74 |
| As an app | 203 | 127 | 141 | 116 | 143 | 67 | 247 | 70 |
| Change | +13 % | +10 % | +10 % | +12 % | +4 % | +8 % | +13 % | -5 % |

#### Receiving and transmitting

- Receiving through `esdr`: 0 gaps at every rate from 500 kS/s to 15.6 kS/s, and at 250 kS/s
  counting for 15 s, into a 4 MB PSRAM buffer, into a wrapping ring for 15 s, and with
  `rx_latest()` polled as a display does while Python allocates on the other CPU.
- Ring cycles per pair match standalone within 0.1 (12.63 counting, 13.12 to a buffer, 13.30
  wrapping), and with the monitor they are lower (13.6, against 14.0).
- So the sink running from PSRAM has not cost anything yet; a display was still to come
  ([XIAO with display](#xiao-with-display)).
- Transmitting: 0 underruns at 500, 250 and 125 kS/s, generated or from a buffer.
- At 500 kS/s a 10 s session usually has one DAC restart, dropping about 550 samples: 9 restarts in
  12 sessions over three runs here, 3 in 4 standalone. None at the lower rates.

#### Over the air and GNU Radio

- From Python, against the WHY2025 badge's radio: the badge hears the S3's tone at -44 dB and the
  S3 hears the badge's at -50 to -52 dB, on this image and standalone alike.
- A flowgraph on the frozen GNU Radio shim (`soapy.source`, `head`, `null_sink`): 500000 samples,
  0 gaps.
- Resets, leaving for another app and firmware updates with the ring or the transmitter on: all
  clean. A soft reset leaves the next capture at 0 gaps.

#### File reads with the ring on

- File reads with the ring on return correct data. But the flash operation parks CPU 1, and a bank
  switch can come late (`RING_LATE`), which restarts the ring with about 3 ms lost.
- A 4 KiB read every 100 ms restarted it 41 to 63 times per 145 reads, against 9 to 26 standalone,
  losing about 1 % of the samples.
- That difference is not explained: reads are faster here (694 us against 908 us), and pinning
  `main` did not change it.
- A tight read loop breaks the ring on both images (750 to 1010 and 584 to 875 restarts in 15 s).
- Writing 2 MiB with the ring on took 12 s and read back correctly, with 285 restarts; not compared
  with standalone.
- Neither image is gapless while files are in use.

#### The other apps

On this image the other apps match the numbers above.

`wpan` against the C6:

- 799 of 799 received.
- 785 of 800 sent.
- 199 of 200 ACKs.
- 1586 of 1600 ACKs sent by the S3.
- The losses nearly all on channel 20, which is busy here.

The rest:

- `stream` 779 kB/s.
- `osmosdr` with the same ring cycles and 0 gaps.
- `capture`, `rfprobe` and `link` as well.
- 28 app switches, all reset reason 12.

### XIAO with display

A XIAO ESP32S3 with the ST7789V panel (320x240), knob and SD slot, panel and card on one SPI bus,
against the standalone app built from the same tree. That standalone image is byte for byte the one
built before this work moved its overlay and source lists. Panel, colours, launcher and knob were
checked by eye.

#### Starting it

- `multiapp run micropython` takes 87 to 95 ms for `phy` and `sdr` and 781 ms for the app.
- The REPL answers 1.95 s after the reset, against 1.31 s standalone.
- Since 2026-11-10 it boots to the launcher by itself (`CONFIG_MULTIAPP_AUTOSTART`, as the
  Communicator).
- That was checked after `west dfu` with `--storage`, after `machine.reset()` and after a soft
  reset; Scan started from it (`launcher.run("scan")`) and ended by Ctrl-C back to it.
- `esdr.shell("multiapp run <name>")` starts another app.

#### Python speed

Slower as on the devkit, because the interpreter runs from PSRAM and position independent. Strings
are faster here. Three rounds of the same script, in milliseconds:

| | Loop | Calls | Float | List | String | Dict | Bytes | Allocate |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Standalone | 179 | 115 | 129 | 105 | 179 | 64 | 218 | 500 |
| As an app | 203 | 127 | 141 | 117 | 150 | 70 | 247 | 520 |
| Change | +13 % | +10 % | +9 % | +11 % | -16 % | +9 % | +13 % | +4 % |

#### The display and the ring

The eggs ran from the launcher, in their own interpreter. The sink running from PSRAM costs nothing
measurable here: 12.99 to 13.02 cycles per pair, against 13.00 to 13.02, so it stays there.

| Run | As an app | Standalone |
| --- | --- | --- |
| `spectrum`, `waterfall`, `constellation` at 250 kS/s, `receiver` at 125 kS/s, 30 s each | 0 gaps, 1 run each | the same |
| `spectrum`, 120 s, an event on a control every 250 ms, gc and poison every 1 s | 0 gaps, 954 frames per sink, loop 78 % busy | 0 gaps, 945, 86 % |
| The same for 600 s with a retune every 2 s | 296 retunes, 0 gaps at every reading (twice a second), 4731 frames, 79 % | 292, 0 gaps, 4671, 87 % |
| Heap in those 600 s, 10th reading to the last | -4032 B | -3840 B |
| `spectrum` at 500 kS/s and 2 MS/s, 40 s | 0 gaps | 0 gaps |
| `spectrum` at 1 MS/s, 40 s | 2 gaps (2 samples, 17 pairs lost) in 1 of 4 runs | 0 gaps in 3 runs |

- The 1 MS/s gaps are one event, not explained; there are too few runs to tell it from chance.
- About half of the heap's fall is the test's own list of readings. The rest is on both images, so
  in the shared Python.

#### LVGL and the heap

- LVGL: six cycles of 24 objects sharing a style, each with an event callback and an animation, and
  a timer that ends itself, with Python holding no reference and the heap filled with poison and
  collected in between.
- Every event, tick and animation still fires, and the heap ends 16 B lower.
- Python's free heap in an egg: 4.53 MB against 4.87 MB.

#### The USB drive

- The volume's drive in a MicroPython boot: 256 KiB from the PC at 87 kB/s (59 kB/s with `spectrum`
  running), read back at 670 kB/s after a remount, identical on the PC and on the board.
- 1 MiB from the board, identical on the PC.
- A drive boot shows the volume and a second drive with no medium.

#### The other apps

- `capture`, `rfprobe` and `link` run; `stream` 780 kB/s.
- `osmosdr`: `test-in` 890 kB/s with 0 gaps at 250 kS/s 8 bit and 125 kS/s 16 bit, and none dropped
  on transmit.
- 28 app switches, and both desk checks pass.
- This USB port gives standalone `osmosdr` 893 kB/s as well, so the 1154 kB/s of
  [Radio in the host, older design](#radio-in-the-host-older-design) was another port. 250 kS/s
  16 bit overflows here for that reason.
- `wpan` boots, but has not met the C6 on this image, which was not on the desk.
- Open from the devkit, not repeated here: file reads restarting the ring more often than
  standalone.

#### Later changes

- After these runs the drivers' list in `micropython.cmake` changed for the Communicator, and the
  layout gained `fit()`.
- The XIAO's host stayed byte for byte the same through the first, and its eggs have the same plots
  as before through the second.
- That final build runs `waterfall` and `spectrum` with 0 gaps.

#### SD writes untested

- The only card to hand keeps nothing it is given ([SD cards](#sd-cards)), so no file written to an
  SD card has been read back on this image.
- What has run: the card mounts, the files list, raw reads at 903 kB/s at 12 MHz.
- The path from USB to the card: 16 MiB at 217 kB/s, and 4 MiB at 126 kB/s with `spectrum` drawing
  on the same bus, with 0 gaps and 1 run.
- A boot with no card comes up normally, though the port's message is "FS is invalid".

### Communicator with display

The Hackaday Communicator: NV3007 panel (428x142) on SPI, TCA8418 keyboard on I2C, no SD card and
no knob, against the standalone app from the same tree. Panel, colours, keys, starting an app from
the keyboard and Esc back to the launcher were checked by hand. This board is on a port without a
hub.

#### Starting it

- 95 ms for `phy` and `sdr`, 947 ms for the app, in a 1267392-byte file.
- The REPL answers 2.35 s after the reset, against 1.64 s standalone.
- The host's image ends 11888 bytes below bank 0.
- It boots to the launcher by itself (`CONFIG_MULTIAPP_AUTOSTART`): from a power-on, after
  `machine.reset()`, after another app and a reset, after `west dfu` and `--storage`.
- The runs here were before that, with a host otherwise the same; both desk checks' autostarts
  override it.

#### Python speed

A fresh interpreter against standalone, in milliseconds:

| | Calls | Float | List | String | Dict | Bytes |
| --- | --- | --- | --- | --- | --- | --- |
| Standalone | 115 | 128 | 105 | 148 | 63 | 218 |
| As an app | 128 | 141 | 117 | 156 | 68 | 247 |

#### The display and the ring

| Run | As an app | Standalone |
| --- | --- | --- |
| `spectrum`, `constellation` at 250 kS/s, `receiver` at 125 kS/s, 30 s each | 0 gaps, 1 run each | the same |
| `spectrum`, 120 s, an event on a control every 250 ms, gc and poison every 1 s | 0 gaps, 961 frames per sink, loop 51 % busy | 0 gaps, 955, 59 % |
| `spectrum` at 500 kS/s, 1 MS/s and 2 MS/s, 40 s each | 0 gaps | 0 gaps |
| The stress for 600 s with a retune every 2 s | 299 retunes, 0 gaps at every reading | not run |

- Those runs had `spectrum` before the layout's `fit()`, with its plots stacked.
- With it, `spectrum`'s plots are 171x41 instead of 428x15 and `waterfall`'s 171x124, and each ran
  for 20 s with 0 gaps and 1 run.
- The plots beside the rows, the keys reaching every control and retuning from the slider were
  checked by hand.
- LVGL's stress as on the XIAO: everything still fires, and the heap ends 16 B lower.

#### The USB drive

- 256 KiB from the PC at 101 kB/s (96 kB/s with `spectrum` running), read back at 760 kB/s after a
  remount, identical on the PC and on the board.
- 1 MiB from the board, identical on the PC.
- Writing with the ring on restarts the ring, as on the XIAO.

#### Radio

- Over the air, from Python, against the WHY2025 badge's radio: the badge hears the tone at -49 to
  -50 dB, and the Communicator hears the badge's at -46 to -52 dB.
- `wpan` against the badge's C6, 200 frames each way on channels 11, 15, 20 and 25: the S3 received
  799 of 799 sent, and the C6 784 of 799, 12 of the losses on channel 25.
- 194 of 200 ACKs came back to the S3, and the S3's own ACKs reached the C6 for 1599 of 1600 frames.

#### The other apps

- `capture`, `rfprobe` and `link` run.
- `stream` 1027 kB/s.
- 28 app switches, and both desk checks pass.

`osmosdr`:

- `test-in` 1173 kB/s.
- Receive at 250 kS/s 8 and 16 bit and 125 kS/s 16 bit with 0 gaps and 0 overflows.
- 1254692 samples transmitted with none dropped.

#### Stacks

At their peak over all of that, keys pressed included, in bytes:

| Stack | Peak |
| --- | --- |
| `mp_stack` | 13680 of 16384 (83 %, `receiver`; 13664 standalone) |
| drive thread | 944 of 1152 (81 %) |
| main | 1808 of 2432 |
| `sysworkq` | 896 of 1152 (77 %, in `stream`; 752 with the keyboard) |
| `IRQ 01` | 512 of 768 |
| kernel heap | 2392 of 3072 |

### Radio in the host, older design

On the XIAO, with the radio in the host.

#### wpan

Against the C6 (`interop.py`, 100 frames of 100 bytes), on channels 11, 15, 20 and 25:

- 100 of 100 frames each way.
- 50 of 50 ACKs.

#### stream

- 947 kB/s of VITA 49.2 over NCM (standalone 968 kB/s).
- `vrt_tx.py` passes as standalone.

#### osmosdr

`esdr_usb`:

- `test-in` 1154 kB/s.
- Receive at 250 kS/s (8 and 16 bit) without overflow or gaps.
- 1.25 M samples transmitted with none dropped.
- Ring at 13.3 of 15 cycles per pair.

#### link

- QAM build and decode within 1 to 4 % of standalone.
- FFT and RS the same.
- OFDM test frame 5583 us (standalone 5589).
- `link bench`'s float loop and power sum are 1.4 and 2.1 times slower: the shell runs them on
  CPU 0 here, CPU 1 standalone (not isolated).
- PSRAM reads miss the cache once, then match internal RAM.
- Not tested: a link between two boards.

#### App switches and DFU

- 48 app switches, each into the requested app.
- All six apps run from their files with the same results.
- DFU while the ring runs.

#### The volume

- `west dfu --storage` reads back byte for byte (DFU upload), and fsck.vfat is clean.
- Copying through the drive runs at about 63 kB/s (every 4 KiB page erased and rewritten). Files
  read back identical after a remount and run after ejecting.
- `west dfu --esptool --storage` writes MCUboot, image and volume in 27 s.
- A host built with one Kconfig value changed refuses the old files.
- The desk check `sample.esp_sdr.multiapp.hil` passes ([Testing](../dev/testing.md)).

## Other chips

### ESP32-C6: complete images

- multiapp does not exist for the C6. What the WHY2025 badge's C6 has instead does the same job
  differently; its usage is in [osmosdr_serial](../../apps/osmosdr_serial/README.rst).
- This design cannot move there: no PSRAM for code, about 13 KB left in SRAM block 0 with the SDR's
  image, blocks 1 and 2 the gapless capture's, and loader options and module linking built around
  Xtensa shared objects.
- So MCUboot holds five complete images, and `lib/boot_select` starts the SDR unless a flash sector
  asks for another, until the next power-on.

#### Capture under MCUboot

The SDR had to move from the ROM's simple boot to MCUboot, and its capture was measured on both,
15 s each:

| Through the P4 | Simple boot | Under MCUboot |
| --- | --- | --- |
| Gapless 80 MS/s: radio pairs, ring errors, bad blocks | 1199765484, 0, 0 | 1199766546, 0, 0 |
| Gapless 40 MS/s: radio pairs, ring errors, bad blocks | 1799686878, 0, 0 | 1799680769, 0, 0 |
| `spectrum` at 250 kS/s, 20 s: gaps, overflows | 0, 0 | 0, 0 |

- Over the air against the Communicator, two rounds on the same desk: about -55 dB on both images in
  the first, about -65 dB on both in the second. The level moves with the room, not the image.
- Each app ran from its slot: `capture`'s survey, 0 failed; `rfprobe` and `wpan` answer on their
  shells; `zstack` boots.
- A damaged image fell back to the SDR, and a power-on ended a request.

### ESP32-S31: RISC-V extensions

On the ESP32-S31-Function-CoreBoard-1 the host has one core, 16 MB of octal PSRAM and the USB 2.0
HS port J4 for the shell, the drive, DFU and osmosdr_hs's stream. It boots into `osmosdr_hs`, and
`hello`, `capture` and `rfprobe` are the other apps. Where it differs from the S3:

#### Relocatable objects

- On RISC-V llext loads only those (`ld -r`).
- A shared object's `.rela.dyn` names no section, so llext skips it, and its PLT reaches `.got.plt`
  PC-relative across regions llext places apart.
- `ext/llext_riscv.ld` groups each kind in one section per llext region and makes one `.bss`
  (llext takes a single NOBITS section; libphy alone has six).
- It also collects the IRAM_ATTR code as `.module_fast` and drops `.eh_frame` (its SET6/SUB6
  relocations are not llext's).
- An extension with archives links with `--gc-sections`: what it compiles is kept, and of the
  archives only what it reaches.
- So are the hal's sources it compiles, built with a section per function: the PHY glue's antenna
  helper calls `gpio_config()`, which the host lacks.
- `exports.py` reads the objects' symbol tables.

#### No alias

- Code runs where llext writes it: one heap of 2368 KiB in PSRAM (`.ext_ram_noinit`, not zeroed at
  boot).
- This SoC's `EXT_RAM_NOINIT_ATTR` is internal RAM, so `esp_sdr_place.h` names the section.
- `sync_text()` writes the data cache back, invalidates both instruction caches (`Cache_*_Addr()`
  with this SoC's cache map) and runs `fence.i`.
- PMA marks PSRAM executable and PMP's entries are unlocked, so the code runs from PSRAM and
  internal RAM alike.

#### TLS

The libc uses thread-local storage, and llext relocates no TLS access:
`CONFIG_CURRENT_THREAD_USE_TLS=n` makes `k_current_get()` a call.

#### The radio as one module

- `sdr` holds the hal's PHY glue, the library and the blobs.
- The PHY library's calls into two ROM functions go to wrappers in the library (`--wrap` in the
  partial link), which only a module holding both can link; the host links the ROM's PHY symbols.
- 9.2 KB of it is internal RAM (IRAM_ATTR code, data, bss), the rest PSRAM.
- It comes up in about 116 ms.

#### osmosdr_hs

- It is built from its own sources as `INTERNAL`, so its 104 KB of USB batches, which carry
  40 MB/s, are internal RAM.
- `ext/usbd_prelude.h` gives it the host's USB device where it brings up its own, and
  `CONFIG_APP_USB_MSG_HOOK` passes it the device's messages (a suspend stops the stream).
- Its crystal calibration lives in settings, a file on the volume. An app's boot mounts the volume
  read-write before its modules load (a boot with an app never hands it to the PC), and `ext.h`
  registers settings handlers as the extension loads.
- `west dfu --storage` writes the volume anew, calibration included.

#### Memory

The image ends 12.9 KB below the receive ring at 0x2F040000, with a module heap of 120 KB of which
the radio and osmosdr_hs take 116 KB.

#### Measurements on the CoreBoard

Against the standalone builds on the same board (2026-11-13 and 14):

| | multiapp | standalone |
| --- | --- | --- |
| osmosdr_hs, 300 s at 20 MS/s | 5,993 M samples, 0 gaps, 0 bad blocks | 5,986 M samples, 0 gaps |
| osmosdr_hs, free CPU at 20 MS/s (loadlog) | 83 %, idle reads 102 to 107 % | 79 %, idle reads 96 to 101 % |
| osmosdr_hs, overflows the device counts | 0 while streaming, 3 to 4 at the stop (0 since, below) | 0 while streaming, 4 at the stop |
| rfprobe, bursts at the six rates | duration 1.000 of the rate's | 1.000 |
| rfprobe, ring at 20 and 40 MS/s, 3 s | 0 gaps | 0 gaps |

- The overflows at the stop were `esdr_usb`'s: it cancelled its transfers before it turned the
  device off, which then streamed on into nothing.
- It turns the device off first now: three 10 s runs at 20 MS/s each way, alternating, counted 2
  overflows at every stop before and none after (2026-11-15).

#### More osmosdr_hs runs

osmosdr_hs under multiapp also:

- ran 30 s at 10 and 5 MS/s with 0 gaps;
- showed the 2400 MHz crystal harmonic at -3.000 MHz from LO 2403 and +5.000 MHz from LO 2395;
- kept a stored calibration across a reboot;
- streamed through 360 random retunes and a 3 s host stall without a fault, and through SoapySDR.

Its vendor interface is number 3 there (0 standalone); `esdr_usb` and the SoapySDR module find it
by its class.

## Bugs found

### Loader and relocations

#### A pointer past a section

- `R_XTENSA_RELATIVE` looks a GOT entry's target up by section. A table's end pointer, one past its
  last object, lies in no section when padding follows.
- llext said "0x136204 not found in any of the sections", `Failed to link, ret -2`, and the app did
  not start. Small extensions never had a table end at such a place.
- The lookup now accepts one past the end of an allocated section (`arch/xtensa/core/elf.c`).

#### An unseen init table

- A module's init table bounds were linker script symbols, which llext does not export.
- So `wpan`'s radio device never initialized, and nothing reported it until `wpan` said "no radio".
- See [Devices in a module](#devices-in-a-module); `hello mod` covers it now.

#### The PLT in internal RAM

- The PLT in internal RAM was a mistake, and hid a limit. It sat with the fast code, on the belief
  that fast code called the host through it.
- Its stubs only do lazy binding, which llext does not use. It cost `sdr` 3.2 KB.
- For `wifi`, whose code is over 256 KB, it failed the link: a stub's `l32r` reaches only 256 KB
  back to `.got.plt`, and `ld` reports that as a bare assertion.

#### Extensions built at -O0

Zephyr drops `-Os` from Xtensa extensions. A 1472-byte frame (32 at `-Os`) overflowed a 2048-byte
stack. multiapp keeps the host's `-Os`.

#### llext relocations

Local Zephyr fixes for:

- `R_XTENSA_GLOB_DAT` dropping its addend;
- alignment padding making `.bss` claim `.exported_sym`'s relocations;
- a shared object's `.bss` 12 bytes off its 16-byte alignment, which the S3's 128-bit loads round
  down silently.

#### Stale extensions

Zephyr's llext CMake rule never rebuilt `<target>_debug.elf` after an edit. Local Zephyr fix.

#### The freed symbol table

llext frees an extension's symbol table after loading; the shell recognizes dynamic entries by host
section address.

### USB, CPUs and threads

#### USB on the radio's CPU

- `main` could start USB on the radio's CPU. USB's and the watchdog's interrupts land on the CPU
  that allocates them, and a thread can come back from any wait on the other CPU.
- A `capture` boot reported CPU 1.
- With MicroPython's ring on, a firmware update then died part way through on a watchdog reset
  (reset reason 7) in 2 of 3 tries, with no crash record, because the ring masks interrupts on that
  CPU.
- `main` now pins itself to CPU 0 first (`app_cpu_pin_self()`): 6 of 6 updates with the ring on and
  3 of 3 with the transmitter on.
- Every boot had this hazard, not only this app.

#### NCM unregistered before init

Unregistering NCM before init crashed: its shutdown removed a descriptor it never added. Local
Zephyr fix.

#### link's shell stack

`link decode` and `link bench` overflowed the shell stack (a 3.6 KB decode chain). They run on the
paused MAC thread now (`link_mac_run_paused()`). osmosdr's receive thread has 2560 bytes.

### Updates and the volume

#### Refusals taken for crashes

- After a new image, a file-only module's old file is refused and the boot comes up as a drive.
- The guard against crash loops then skipped the autostart once more, even with the right volume
  written.
- Now only a boot that dies is skipped.

#### The volume 4 KiB off

`flash_img` writes an MCUboot image, from the start the swap-using-offset layout gives the update
slot: the volume landed 4 KiB into its partition. Its DFU target uses `stream_flash`.

### Wi-Fi and network

#### A zeroed interface

- The driver names its interface through a `NET_IF_DECLARE()` that `NET_DEVICE_DT_INST_DEFINE()`
  fills in.
- With that macro replaced it was a zeroed `net_if` of the module's own, which links and scans:
  scans never use it.
- The first join crashed in `net_if_dormant_off()`.

#### The network receive stack

- The network receive stack overflowed on real traffic. 1024 bytes were enough for `stream`, and the
  first router advertisement over Wi-Fi used all of them.
- It lies directly above `net_mgmt`'s stack in PSRAM, which an overflow would corrupt.
- One update failed while such an image was connected, and none has since.
- Now 2560, in PSRAM, peak 1680.

#### One IPv4 slot

`CONFIG_NET_IF_MAX_IPV4_COUNT=1` went to NCM's interface, unused, so DHCP on Wi-Fi never finished.
With `CONFIG_WIFI_STA_AUTO_DHCPV4` the result of a join comes only then.

#### The hal's name mapping

The hal's name mapping ran too early: `zephyr_compat.h` maps Zephyr's Wi-Fi options to ESP-IDF's and
comes first on every compile line, before an extension's own values. `ext/wifi_compat.h` runs it
again.

### SD cards

#### Three cards, no writes

- None of three SD cards could test writes on the XIAO.
- After the two below, a 32 GB card that reads fine in the WHY2025 badge's native SD mode holds MISO
  low whenever it is selected on the XIAO's SPI bus, power cycled or not, so even CMD0 reads 00.
- It is not the slot: another 32 GB card passed writes there in October, and the 4 GB one below
  initialized.
- The card was sent only reset and identify commands. Not explained.

#### Two faulty cards

- One, sold as 64 GB, presents 1.9 GB and sets the high capacity bit, yet is byte addressed. So LBA
  n reads sector n/512, and `/SD:` reports "FS is invalid".
- One, 4 GB, went read-only without saying so: every write is acknowledged, with no error at any
  level, and the old data reads back, in a PC's card reader too.
- On it, 4 MiB "written" in 0.6 s were gone after a remount, and Linux said "not properly
  unmounted" at every mount, since the flag never clears.
- Both behave so on the standalone image as well. Before suspecting the driver, read whole sectors
  and write one.

### MicroPython port and display

#### The standalone app on the Communicator

- The standalone app no longer fit the Communicator, by 32 bytes: "DRAM image overlaps the ring
  banks", with code that had come in since (`corr_model` and friends in `lib/esp_sdr`).
- The board's fragment gives up 64 bytes of the kernel heap's 15872, which the host's
  `extconfig.py` pass is told to ignore.
- That heap's peak cannot be read on the image (no runtime statistics). It boots and runs the whole
  set of tests above, and 13312 once failed to bring USB up, so there is not much more to take from
  it.

#### waterfall on the Communicator

- `waterfall` did not start on the Communicator, as an app or standalone: `ValueError` from
  `framebuf.FrameBuffer` in `gnuradio/qtgui.py`.
- Its rows take 130 of the 142-pixel panel's 124, which left a plot of no height. `spectrum`
  started, with 6 pixels of plot under each label.
- Now `_layout.fit()` puts plots that would get less than 30 pixels beside the rows, using the
  panel's width. Displays with room keep the stack.

#### Wrong colours

- Colours were wrong, and nothing said so. The extension looked for the board's Kconfig fragment
  under a wrong name, because `BOARD_QUALIFIERS` has no leading slash, so `CONFIG_LV_COLOR_16_SWAP`
  was missing.
- The panel worked, in the wrong colours.
- A missing fragment now fails configure.

#### esdr.init() during a capture

`esdr.init()` with a capture running never returns. It waits for the radio's lock, which the
running ring holds, and Ctrl-C cannot reach it: the REPL goes silent. The standalone app has the
same code. Not fixed: stop the capture first.

#### os.rename()

The port's `os.rename()` cannot work. `zephyr_fs_make_path()` returns one shared buffer, so both
names are the second one and it fails with ENOENT. The standalone app does the same. Not fixed.

#### Long prints

Long prints lose their middle. `uart_poll_out()` on CDC-ACM discards what does not fit its TX
buffer unless flow control is on, and the REPL's output goes that way. The standalone app does the
same. Not fixed: with flow control, a print with no PC attached could block for good.

### Memory rules

The rules behind these three are in [Code placement](../dev/code-placement.md).

#### PHY code with the cache off

- The ROM calls PHY code with the cache off. With `phy`'s code in PSRAM, a reboot with the radio up
  ended in a double exception (reset reason 16, not the software reset's 12).
- The fault was an illegal instruction in `ram_chip_i2c_readReg`, on the way down in
  `rtc_clk_cpu_set_to_default_config`.
- This placed blob code in internal RAM, and `test_reset_was_clean` watches for it.

#### Atomics in PSRAM

Atomics in PSRAM are not atomic (`hello atomic`). Local Zephyr fix.

#### Flash access from a PSRAM stack

No flash access from a PSRAM stack: the app's `main()` raced the host's `app_usb_init()` and ran
MCUboot's image check on its PSRAM stack. Double fault; the board hung after some app switches.

## Zephyr changes

Commits on the fork's `zsdr` branch, which `west.yml` pins, on top of the SMP port. They are
candidates for upstream Zephyr:

- exec alias;
- llext repackage dependency;
- NCM/ECM unregister before init;
- Xtensa GLOB_DAT addend;
- alignment padding;
- PSRAM atomics (two commits);
- shared object region alignment.

### For modules

- The modules add `CONFIG_LLEXT_REGION_HEAPS` with `LLEXT_MEM_TEXT_ALT` (`subsys/llext`).
- `CONFIG_ESP32_WIFI_NET_ALLOC_SPIRAM` moves to `soc/espressif/common/Kconfig.spiram`, because it
  also serves the network stack without Wi-Fi.

### For unloading

- `drivers/wifi/esp32`: `esp32_wifi_shutdown()` (public header `zephyr/drivers/wifi/esp_wifi/esp_wifi.h`),
  the driver's stop path: disconnect, work cancelled, interface down, `esp_wifi_stop()` and a wait for
  the stopped event, `esp_wifi_deinit()`, a shutdown flag after which `esp_event_post()` fails, a
  sentinel to the event queue and `k_thread_join()` of the event task. The driver's IPv4 `net_mgmt`
  callback is added at init and removed there, not a static handler. The vendor's driver had no
  stop path; the function is upstreamable as it stands (a driver that cannot start again, which is
  the point of a loadable extension).
- `settings_deregister()`, the inverse of `settings_register()`.
- On the hal fork, the S3 adapter's interrupt bookkeeping and release
  ([Interrupts of the blob](#interrupts-of-the-blob-hal-fork)).

### For the display

- `arch/xtensa/core/elf.c` takes a pointer one past the end of a section
  ([A pointer past a section](#a-pointer-past-a-section)).
- LVGL's source lists move to `modules/lvgl/sources.cmake`, in the same order, so that an
  application can build LVGL its own way.

### On the MicroPython fork

The port's source lists move to `ports/zephyr/sources.cmake`, for `micropython.cmake` to use.

### For the ESP32-S31

Two more, both in the pinned `zsdr` with the S31's support itself:

- `CONFIG_LLEXT_REGION_HEAPS` depends on `CONFIG_LLEXT_HEAP_DYNAMIC` alone (the exec alias is
  Xtensa's), part of the region heaps commit.
- `arch/riscv/core/elf.c` checks `R_RISCV_JAL` against the J type's +-1 MiB instead of the 2 GiB of
  an `auipc` pair, which let a jump between regions too far apart through.

## Risks

- Extension code cannot run while flash is written (cache off): interrupt handlers and code that
  runs then stay in the host.
- Unloading passed the 100-cycle soak ([The soak](#the-soak)), but: a thread of an app that no
  hook ended is aborted, which can leave a host lock held; a module whose down hook refuses
  reboots; the blobs' PSRAM buffers are not in a private arena, so a leak there shows only as the
  shared heap shrinking.
- The local Zephyr commits above, until upstream.
- link throughput between two boards is untested.
- A hal update can change what the Wi-Fi driver needs. A renamed source fails the build and a
  missing symbol fails the host's link, but `wifi.cmake`'s list and definitions are ours to keep in
  step.
- The PC's write cache: files copied onto the drive reach the flash only when the PC writes them
  out, which ejecting guarantees. `multiapp run` reminds of it in a drive boot.

### USB that stops answering

- USB has stopped answering twice on the devkit, and this has not been explained.
- The first time was a `wpan` boot idle after a shell session: no shell output and failed control
  requests for at least 6 minutes, with no reset from the liveness watchdog.
- The second was a drive boot with no app or radio, after 12 idle hours. A standalone `rfprobe` has
  shown the same symptom.
- The second time, the chip answered normally over its UART, but its USB failed to enumerate even
  in ROM mode, with none of this firmware running, until a later replug.
- So the USB path or the chip's USB pins are involved, not only the firmware. A reset through the
  devkit's UART bridge recovered it after that.

<!-- nav-start -->
Previous: [multiapp](../../apps/multiapp/README.rst) · Up: [Applications](index.md) · Next: [micropython](../../apps/micropython/README.rst)
<!-- nav-end -->
