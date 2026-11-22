# Coexistence

<!-- nav-start -->
Previous: [Crystal calibration](crystal-calibration.md) · Up: [The radio](index.md) · Next: [Samples in and out](../samples/index.md)
<!-- nav-end -->

This page explains how the SDR, Wi-Fi and ESP-NOW can live on one radio: what they share, the
modes that work, how the S3's PHY power-down was measured, and what runtime loading of the
radio stacks (llext) needs per chip. The bench is [coexbench](../../apps/coexbench/README.rst).

## Answer

- **They cannot use the radio at the same instant, except in one case.** There is one LO, one
  analog chain, one AGC and one baseband. The exception is a listen-only SDR on the Wi-Fi channel
  that leaves the AGC, the front end and the MAC alone. That already happens today on the C5 and
  on builds without `CONFIG_ESP_SDR_PHY_ONLY`: the SDR runs next to a started driver.
- **Time-sharing with the Wi-Fi state kept is the mode worth building.** The blob gives the radio
  up in its own sleep windows, through the OS adapter's PHY enable and disable entries. We compile
  the adapter (`apps/multiapp/wifi.cmake`), so a small radio arbiter can sit there.
- **A full swap (deinit or unload, state lost) always works.** It is the only option on the C6
  while the gapless ring runs.
- **ESP-NOW is a small add-on to Wi-Fi, not a separate radio.** libespnow is 7.5 KB of code on
  the S3 and 11.3 to 11.6 KB on the C5, C6 and S31. It calls into net80211 and pp, so it needs
  `esp_wifi_start()` in STA or AP mode, but no supplicant and no `net_if`.
- **The radio stacks load at run time where there is PSRAM.** The S3 and the S31 do. On the C6 and
  the C5-Zero the limit is RAM for code, so the stacks stay resident there.

## What the three share

| Resource | Wi-Fi blob | SDR (`lib/esp_sdr`) | Conflict and fix |
|---|---|---|---|
| libphy and its calibration | `esp_phy_enable()` calibrates once; `phy_track` re-tracks the PLL every second; `noise_check_loop` runs periodically | Same libphy, through `esp_phy_enable()` in `radio_up()` | Two copies of libphy must never run together. One shared `phy` module exports what Wi-Fi needs too (`ext/phy.ver` has 46 names since step 3). |
| PHY on/off | `esp_phy_enable/disable(PHY_MODEM_WIFI)` from the adapter | `esp_phy_enable(PHY_MODEM_WIFI)` too | The HAL tracks modems as bit flags, not counts: Wi-Fi's disable would power the RF down under the SDR. The arbiter owns the real call and counts its clients. |
| LO and channel | `chip_v7_set_chan`, `phy_change_channel` | `set_chanfreq`, `chip_v7_set_chan`, `phy_set_freq` plus a kHz offset | The SDR retunes away. On handback restore Wi-Fi's channel and offset 0, or the stored crystal offset. |
| RX gain | Hardware AGC | `force_rx_gain(1, idx)` | Release with `force_rx_gain(0, ...)` on handback. |
| Analog filters | Codes calibrated by the PHY | `lpf_apply()` writes 0x67:4/5 (C5: 0x67:6/7) | Save the codes before the first write and restore them. |
| Front-end forcing | The MAC switches RX and TX | `rx_front_end()` forces workmode, `xpd_rx_on`, rxclk; TX uses `force_txon_mode(1)` | Forcing breaks MAC TX (ACKs). Undo it on handback. |
| Dump engine | The MAC peripheral holding the engine is reset by Wi-Fi start | Engine set up in `esp_sdr_rx_prepare()` | Start Wi-Fi before arming the engine. Re-prepare after any Wi-Fi restart. |
| Interrupts | MAC, PWR and BB sources; on the S3 multiapp reserves the Wi-Fi interrupt on core 0 | GDMA in-channel ISRs, ring timers | No shared source. An unload must free the module's ISRs. |
| I2C to the analog blocks | libphy takes `regi2c_enter_critical()` | Every `rom_chip_i2c_*` sequence takes it too ([Transceiver](transceiver.md)) | Already safe. |
| esp_timer | PHY tracking timer | Must stay on CPU 0 | One libphy means one tracking timer. Its once-a-second run opens gaps in S3 rings. |
| CPU | Blob task and ISR; on the C6 the same single core | C6 gapless and paced are CPU-bound | Concurrency on the C6 costs gapless units. Time-share instead. |
| libcoexist | `coex_schm_*` phases and callbacks | none | Fallback lever: the SDR could pose as the 802.15.4 client of the scheduler. It is more opaque than the sleep callbacks. |

The driver also puts a 1.75 us glitch into the receiver every 139 ms in some states
([Bursts](../samples/bursts.md)). Every concurrent mode inherits it.

## The modes

### A: Concurrent, on the Wi-Fi channel, listen-only

- Wi-Fi stays associated (or ESP-NOW active) with power save off. The SDR captures on the Wi-Fi
  channel and skips `rx_front_end()` forcing and gain forcing. It does not transmit.
- Use it for spectrum views, packet-adjacent captures and channel occupancy.
- The gain follows the AGC, so levels need a per-capture gain readout. The S3's AGC register
  index field is not one (see the measurements).

### B: Time-shared, Wi-Fi and ESP-NOW state kept

The radio arbiter (`lib/esp_sdr/src/esp_sdr_arb.c`, `include/esp_sdr/esp_sdr_arb.h`,
`CONFIG_ESP_SDR_ARB`, ESP32-S3 first) sits between the Wi-Fi blob's adapter and the PHY's power
calls. It owns the real `esp_phy_enable(PHY_MODEM_WIFI)` and `esp_phy_disable(PHY_MODEM_WIFI)`
(the library adds the linker `--wrap` for both) and counts two clients, Wi-Fi and the SDR: the PHY
is powered while either is set. Other modems pass straight through.

```
wifi esp_phy_disable -> client WIFI off; PHY down unless the SDR holds it; wake a waiting claim
wifi esp_phy_enable  -> client WIFI on; if the SDR holds the radio: preempt (below), then return
sdr  claim           -> wait until Wi-Fi sleeps (or is absent) with a window long enough, power
                        the PHY, re-apply the SDR's frequency, front end, gain and filter
sdr  release         -> restore Wi-Fi's state, drop the client (PHY down if Wi-Fi sleeps too)
```

State (guarded by one mutex; the fast flags are volatile words readable with interrupts masked):

| Wi-Fi | SDR | PHY | Next |
|---|---|---|---|
| awake | idle | on | Wi-Fi's disable: it sleeps |
| asleep | idle | off | a claim powers it up |
| asleep | holds | on (the SDR's) | deadline: the SDR releases; Wi-Fi's enable: preempt |
| awake (preempting) | holds | on | the stop hook ran; the release hands over; on a timeout Wi-Fi goes on |

**The window.** The arbiter keeps the times of Wi-Fi's last eight wakes (an enable counts only if its disable follows at least `CONFIG_ESP_SDR_ARB_MIN_WAKE_US`, default 1 ms: after the SDR releases at its deadline the blob makes a spurious enable and disable pair about 330 us later, which is not a beacon wake; these are counted as short wakes)
(`lib/esp_sdr/src/esp_sdr_arb_pred.h`, plain C, tested by `tests/unit/arb_pred`). The period is
the median of the enable-to-enable intervals (it needs three; one long awake phase does not move
it), the phase is the latest enable. A claim gets the deadline `next predicted enable less
CONFIG_ESP_SDR_ARB_MARGIN_US` (default 3 ms: the ring ends within 1.5 units, about 1.2 ms, a
restore takes about a millisecond), and waits for a later sleep when less than
`CONFIG_ESP_SDR_ARB_MIN_WINDOW_US` (default 8 ms: the PHY power-up and retune are about 3.5 ms)
is left. Three cases:

- Periodic wake (modem sleep, ESP-NOW windows, `MIN` 102.4 ms and `MAX` 307.2 ms alike): windows
  until the deadline.
- No periodic wake (Wi-Fi never started, stopped, or stale past four periods): the deadline is
  `ESP_SDR_ARB_NO_DEADLINE`; Wi-Fi's enable is only the preemption backstop.
- Power save off: Wi-Fi never disables, so a claim times out with `-EBUSY`. The SDR gets no
  windows; only passive captures (mode A) work then.

**Preemption.** Wi-Fi's enable (always the `wifi` thread on CPU 0, task context) finding the SDR
holding calls the SDR's stop hook (`esp_sdr_arb_set_stop_hook()`, optional), waits at most
`CONFIG_ESP_SDR_ARB_PREEMPT_WAIT_US` (default 5 ms) for `esp_sdr_arb_release()`, and goes on. It
counts the preemption, the longest wait and the timeouts. Every poll of `esp_sdr_arb_window_over()`
(a compare of the systimer microseconds, usable with interrupts masked) is also true once Wi-Fi asked, so a
ring or a burst loop needs no hook. A ring that masks CPU 0 would keep the `wifi` thread from
running at all: a ring has to run on CPU 1 (SMP), as in osmosdr; the standalone coexbench has no ring (its DRAM image does not leave room for the three banks), so its windows are bursts. The multiapp `coex` app has the ring.

**What release restores.** All with the PHY powered, before the client drops:

- gain back to the AGC (`force_rx_gain(0, ...)`, the call that releases a forced gain);
- the PHY's low-pass codes (`lpf_saved[]` written back; read again at the next claim, since the PHY
  may have been power-cycled between);
- TX forcing off (`force_txon_mode(0, 0, 0)`, with `CONFIG_ESP_SDR_RFTEST`);
- the 5/6 LO divider off;
- Wi-Fi's channel: `set_chanfreq(centre MHz, bw)`, a full channel setup that also clears the kHz
  offset of `esp_sdr_tune()`. The application gives the channel to the arbiter (`esp_sdr_arb_wifi_set_channel()`, from
  `esp_wifi_get_channel()`, in its own thread before the SDR starts): the call goes into the blob,
  which can wait on the `wifi` thread or on a CPU the SDR's thread holds (a hang on multiapp, where
  the worker owned CPU 1 and the blob's spinlock holder could not run), so the arbiter makes none. HT40 shifts the centre by 10 MHz and sets
  bw 1; that value is unverified. A passive run tuned nothing and retunes nothing.

`CONFIG_ESP_SDR_ARB_PHY_CYCLE` adds a real PHY power-down and up after the restore when Wi-Fi is
already awake (it resets what the explicit restore might miss, at the cost of the power-up time in
Wi-Fi's wake); off by default.

**The library under the arbiter.** Bursts, ring runs and TX (`esp_sdr_dac_begin()`) return `-EBUSY`
without a claim; a passive capture needs Wi-Fi's PHY on (`-EAGAIN` otherwise). Setting frequency,
gain or filter without a claim stores the setting; the claim applies it (`esp_sdr_rx_prepare()`).
Without `CONFIG_ESP_SDR_ARB` every path is as before.

**Ring.** `esp_sdr_ring_run()` checks `esp_sdr_arb_window_over()` where it checks the stop
request (once per unit, at half the unit) and ends like a stop, with status
`ESP_SDR_RING_WINDOW` and return 0; a failure keeps its own status (`LATE`, `AGE`, ...). An app
counts window ends apart from failures that way. Each run restarts the output index at 0, so a
sink sees the windows as runs; the gap between them is the time between claims.

API, in `esp_sdr_arb.h` (group `esp_sdr_arb`):

| Call | Purpose |
|---|---|
| `esp_sdr_arb_claim(wait, &deadline_us)` | take the radio in a Wi-Fi sleep; `-EBUSY`, `-EAGAIN` on timeout |
| `esp_sdr_arb_release()` | restore, drop the client |
| `esp_sdr_arb_window_over()` | deadline passed or Wi-Fi asked; IRAM, interrupt safe |
| `esp_sdr_arb_set_stop_hook(fn, user)` | what Wi-Fi's wake calls (cleared by the release) |
| `esp_sdr_arb_get_stats()` | windows, SDR time, preemptions, timeouts, wait max, deadline misses, Wi-Fi on and off counts, learned period, interval error |
| `esp_sdr_arb_trace()` | weak hook, called for every event; coexbench logs it |

Kconfig: `ESP_SDR_ARB` (needs `ESP_SDR_WIFI_SHARED`), `ESP_SDR_ARB_MARGIN_US` 3000,
`ESP_SDR_ARB_MIN_WINDOW_US` 8000, `ESP_SDR_ARB_PREEMPT_WAIT_US` 5000, `ESP_SDR_ARB_MIN_WAKE_US` 1000, `ESP_SDR_ARB_WIFI_FLAG` (y: the SDR's power calls also set `phy_wifi_enable_set`), `ESP_SDR_ARB_PHY_CYCLE`.
Under multiapp (`ESP_SDR_AS_EXTENSION`, S3) the arbiter is in the shared `phy` module, with the same
names: `ESP_SDR_ARB` needs `ESP_SDR_WIFI_SHARED` or `ESP_SDR_AS_EXTENSION`. Added for the module
split: `esp_sdr_arb_set_ops()` (the SDR's prepare and restore, since `phy` cannot import from `sdr`),
`esp_sdr_arb_wifi_attach()` (the driver's channel getter; without Wi-Fi attached `esp_sdr_init()`
keeps a standing claim with no deadline) and `esp_sdr_arb_wifi_attached()`.

Bench: `coex sdr <seconds> [ring|burst] [freq_mhz]` claims, runs to the deadline and releases in a
loop, and prints windows, SDR time and duty, samples, preemptions, deadline misses, ring window
ends and index gaps (ring only); `coex arb` prints the arbiter's counters. coexbench has no ring:
Wi-Fi, USB and the net shell leave no room for the three dump banks, so it runs bursts on CPU 1.
See [coexbench](../../apps/coexbench/README.rst).

- Each switch costs one retune, about 3.2 to 3.8 ms on the S3 ([Tuning](tuning.md)).
- The windows depend on the Wi-Fi side:
  - **ESP-NOW only:** the wake window and interval are ours to pick, through
    `esp_now_set_wake_window()` and `esp_wifi_connectionless_module_set_wake_interval()`.
  - **Station, modem sleep:** the windows follow the beacon interval, or the listen interval in
    `WIFI_PS_MAX_MODEM`.
  - **SoftAP:** never sleeps, so no windows. Only A or C.
- Streams carry the gaps between windows; osmosdr's gap counters already report index jumps.

#### T5: Wi-Fi under SDR windows

2026-11-24, the same bench as T1 to T4, station in `MIN` modem sleep, `coex sdr` taking bursts at
2412 MHz in every window (so each handback has to put channel 11 back).

| Case | SDR duty | Windows | Preemptions | Wi-Fi side |
|---|---|---|---|---|
| No traffic, 30 s | 88.4 % | 290 of 91.4 ms | 0 | period learned 102.404 ms, interval error -28 to +54 us |
| PC pings at 5 Hz, 60 s | 49.8 % (first predictor) | 924 of 30.6 ms | 459, none timed out, wait max 3.6 ms | 0 of 300 lost before, during and after; RTT mean 77, 87 and 83 ms (modem sleep) |
| ESP-NOW 10 ms / 100 ms window, peer sends 500 at 20 ms | 83.3 % | 136 of 85.7 ms | 0 | 395 of 500 ACKed, 403 received; without the SDR 397 and 408 |

- After 924 windows away on 2412 MHz the station was still associated on channel 11 at the same
  RSSI (-54 dBm). The restore (gain back to the AGC, filter codes, TX forcing off,
  `set_chanfreq()` to Wi-Fi's channel) is enough; the PHY power cycle option was not needed.
- Traffic wakes the station between beacons (frames to fetch, replies to send), so with pings the
  SDR's windows are shorter and Wi-Fi preempts more. The backstop covered every case.
- **The spurious wake.** When the SDR gives the PHY up while Wi-Fi sleeps, the blob powers it up
  again about 330 us later for about 350 us. Whether the SDR's power calls set
  `phy_wifi_enable_set()` makes no difference. The first predictor took these as beacon wakes, so
  every other window was cut short by the real wake (the 49.8 % row). The predictor now learns
  only from wakes of at least `ESP_SDR_ARB_MIN_WAKE_US`; the pairs are counted as short wakes.
- The restore's retune takes about 3.5 ms, so the release starts at the deadline and the PHY is
  Wi-Fi's again a little after it. A miss counts a release later than the predicted wake: none in
  any run above.

### C: Swap, state lost

`esp_now_deinit()`, `esp_wifi_stop()`, `esp_wifi_deinit()`, then the SDR, then the reverse.
Reconnect costs about 1 to 3 s with DHCP. A multiapp reboot into the other app costs 177 to
386 ms to `main()`, plus the join. The Zephyr driver never calls `esp_wifi_deinit()` yet.

## Measurements on the S3

Bench: ESP32-S3 DevKitC N16R8 as the device under test (DUT), a XIAO ESP32S3 as the ESP-NOW peer,
an access point on channel 11 with beacon interval 100 TU and DTIM 1, DUT RSSI -55 to -62 dBm.
The app is [coexbench](../../apps/coexbench/README.rst) (2026-11-23).

### When the blob powers the PHY down (T3)

The adapter's `esp_phy_enable()` and `esp_phy_disable()` are linker-wrapped and time-stamped.

| Power save | Period | On time min / mean / max | Off window min / mean / max |
|---|---|---|---|
| `MIN` modem, DTIM 1 | 102.4 ms | 4.0 / 9.4 / 76.5 ms | 25.9 / 92.9 / 98.5 ms |
| `MAX` modem, listen interval 3 | 307.2 ms | 4.0 / 6.8 / 17.4 ms | 287 / 300 / 303 ms |

- Every call comes from the `wifi` thread on CPU 0, in task context, never in an ISR.
- No channel call (`phy_change_channel`, `chip_v7_set_chan`, `set_chanfreq`) happens on wake. The
  arbiter has to restore the channel itself after an SDR window.
- The long on-time outliers (76.5 ms in `MIN`) are the rare cases where the station stays awake.
  The arbiter must not count on every window being long.

### What the RF does while the PHY is off

Passive captures at a random phase in `MIN` modem sleep:

| State | Captures near 0 dBFS (garbage) |
|---|---|
| Wi-Fi asleep (PHY off) | 207 of 300 |
| Awake (no power save) | 0 of 300 above -3 dBFS (max -3.5) |

So `esp_phy_disable()` really powers the RF down. An SDR window needs its own PHY enable, which
the arbiter provides.

### What captures cost a Wi-Fi link (T1, T2)

No power save. The PC pings the DUT at 5 Hz for 60 s (300 pings). The DUT takes 250 captures of
4096 samples at 40 MS/s, one every 200 ms.

| Case | Lost | RTT avg / max | Capture power (median, p90, max) |
|---|---|---|---|
| Idle | 0 / 300 | 5.9 / 28.9 ms | none |
| Passive captures | 0 / 300 | 6.3 / 28.1 ms | -24.1, -16.6, -9.1 dBFS |
| Forced captures | 0 / 300 | 6.4 / 23.9 ms | -24.8, -17.8, -0.5 dBFS |

- Passive captures are live: they see the Wi-Fi traffic.
- At this duty, forcing did not break Wi-Fi. That does not show that it is safe at a higher duty
  or with TX in the window.
- The `AGC_FORCE_REG` index field is not a live AGC readout: it read 54 in every passive capture
  and 40 in every forced one.

### ESP-NOW with a wake window (T4)

The XIAO sends 500 unicast frames of 100 bytes, one every 20 ms, to the DUT.

| DUT state | ACKed by the MAC | Received | Awake per ~103 ms |
|---|---|---|---|
| Awake | 500 / 500 | 500 | all |
| `MIN` modem, `esp_now_set_wake_window(10)`, wake interval 100 ms | 397 / 500 | 412 | 16.6 ms mean on, 89.8 ms mean off |

- Wake windows work while the DUT is associated to the access point.
- Delivery is far above the roughly 15 % awake fraction. Hypothesis, not measured: the sender's
  MAC retries span the sleep gap.
- The window's on time ranged from 10.4 to 130 ms and the off window from 74.7 to 92.2 ms.

### Consequence for the arbiter

A running S3 ring masks the MAC interrupt, and the wake is periodic and in task context. So the
arbiter does not wait for a wake to preempt. It schedules SDR windows that end a margin before the
predicted wake (mode B above). The `_phy_enable` call is the backstop: it preempts the SDR if the
prediction was wrong.

## llext: what stays resident, what loads

### The loader per chip

- The llext loader is not Xtensa-only. Relocatable-object extensions are generic in Zephyr
  (`subsys/llext/Kconfig` defaults `LLEXT_TYPE_ELF_RELOCATABLE` on RISC-V, with
  `arch/riscv/core/elf.c`) and already run on the S31 in this tree
  ([multiapp internals](../apps/multiapp-internals.md), S31 section).
- Only `LLEXT_EXEC_ALIAS` is Xtensa-only. It is the S3's PSRAM instruction alias.
- Without PSRAM the limit is RAM for code, not the loader. Executing extensions in place from
  flash would need a new loader built on the hal's `esp_mmu_map()` plus llext's pre-located mode
  or `LLEXT_RODATA_NO_RELOC`. None exists, so the radio stacks stay resident on the C6 and on the
  C5-Zero.

### Module graph

```
host:    kernel, net stack, net_if stub, loader, esp_timer, regi2c lock, IRQ reservation,
         cal store
phy      libphy + librftest + HAL glue + ARBITER (esp_sdr_arb.c); exports = SDR set + Wi-Fi blob
         set + arbiter API (built)
 |-- sdr        lib/esp_sdr                          REQUIRES phy
 |    `-- ieee154  lib/ieee802154                    REQUIRES sdr
 `-- wifi       pp, net80211, core, manatick.o, esp_adapter.c, wifi_init.c, Zephyr esp_wifi_drv.c,
      |         supplicant (as built: not split into wifi_core and wifi_net)       REQUIRES phy
      `-- espnow    espnow.o + API (include/esp_sdr/espnow.h) + shell, PSRAM only    REQUIRES wifi
```

- The phy, wifi and espnow split must stay portable to the RISC-V `ld -r` recipe, not S3-only.
- Splitting `wifi_core` from the supplicant is optional but cheap to try. No blob imports a
  supplicant symbol at link time: it registers through `esp_wifi_register_wpa_cb_internal()`. An
  ESP-NOW-only boot then skips most of the 540 KB.
- Dropping libphy from the `wifi` module needed `phy.ver` to export the blobs' set
  (`phy_change_channel`, `noise_check_loop`, `register_chipv7_phy_init_param` and more): done,
  step 3 (below). `--gc-sections` then keeps more of libphy in `phy`.
- On the S31 the `sdr` module holds the `--wrap` wrappers for two ROM PHY calls. They must move
  into the `phy` module.

### What unload needs

Zephyr has `llext_unload()`. multiapp calls it since step 5 (as built, below; the list is what it had
to cover, the build is [Unloading](../apps/multiapp-internals.md#unloading)).

1. **Quiesce through the API:** `esp_now_deinit()`, `esp_wifi_stop()`, `esp_wifi_deinit()` (the
   driver must gain this call), `esp_supplicant_deinit()`. For the SDR: stop ring and engine,
   then release the arbiter claim.
2. **Zephyr objects the module created:** threads, `k_timer`, work items, `net_mgmt` callbacks,
   its `DEVICES` entries, the `net_if` takeover and proxy shell roots.
3. **Host pointers into the module:** ISRs (`esp_intr_free`), esp_timer callbacks, arbiter hooks
   and PHY or coexistence callbacks. An entry left behind jumps into freed PSRAM.
4. **Memory:** the Wi-Fi heap is private (`--wrap` of `k_malloc`, `ext/wifi_glue.c`), so unload
   drops it whole. Its PSRAM buffers come from the shared heap today and need a private arena.
5. **Reload is a bonus.** A fresh load gives fresh blob `.data` and `.bss`, which is cleaner than
   ESP-IDF's in-place re-init.
6. **`phy`** unloads only with no client left. Reloading recalibrates; use the stored calibration.

### RAM pressure on the S3

A Wi-Fi boot held 29532 of the 31640 bytes of `module_heap`. As built (step 3), `wifi` shrank by
the private PHY (7 KB of IRAM code, data and bss) and `phy` grew by the arbiter and the blobs' extra
set, so a boot with `wifi` and `phy` needs about 33.4 KB. With `sdr` as well (the `coex` app) the sum
of internal code, data and bss (everything but `sdr`'s high-region bss) is about 43.0 KB: `wifi` 22.7,
`phy` 10.7, `sdr` 9.6, against the 35.8 KB `module_heap` has now. The gap below bank 0 does not
allow more (the XIAO's was about 1.2 KB at 38.4 KB of heap). `coex` therefore takes SRAM dump bank 2
as a second module heap (`BANK2`): `sdr` does not fit `module_heap` and loads into the bank, and the
SDR's rings in that boot run on banks 0 and 1 only (`spare_bank_app`, unit 2048 pairs), with no
bursts. See [multiapp internals](../apps/multiapp-internals.md), "Bank 2 as a module heap".
`osmosdr` and Wi-Fi both want the "area" (24064 of 24128 bytes taken by `osmosdr` and
`micropython`), so the two together need the driver's heap in PSRAM or a bigger area.

## Per chip

| Chip | Concurrent (A) | Time-shared (B) | Swap (C) | llext | Recommendation |
|---|---|---|---|---|---|
| ESP32-S3 | Plausible for bursts and the 16 MS/s ring on the Wi-Fi channel; Wi-Fi on core 1 | Yes: arbiter in `esp_adapter.c`; STA modem sleep or ESP-NOW windows | Yes | Yes; multiapp loads `phy`, `sdr`, `ieee154` and `wifi` | Main target. Build B here first. |
| ESP32-C5 | Already co-resident: the SDR needs the started driver | Yes, same arbiter; one LO | Yes | The chip supports PSRAM (`psram0` in `esp32c5_common.dtsi`; Zephyr's `esp32c5_devkitc` has 4 MB). The C5-Zero (C5HF4) has none. | Zero: keep resident, add the arbiter and ESP-NOW (+11 KB). A board with PSRAM could take the S31 recipe. |
| ESP32-C6 | Paced and trig: yes. Gapless: no. | Paced and trig: yes. Gapless: no. | Gapless: the only option | 512 KB HP SRAM, no PSRAM: RAM for code is the limit | Resident Wi-Fi, heap over the gapless ring region so it exists only while gapless is off. MCUboot image switch stays the heavy fallback. |
| ESP32-S31 | Unknown: no Zephyr Wi-Fi driver | Same design once a driver exists | Yes | Yes, RISC-V `ld -r` modules in PSRAM, already used by multiapp | The missing piece is blobs plus driver glue, not llext (below). Split `phy` from `sdr` first. |
| ESP32-P4 | n/a | n/a | n/a | 768 KB HP SRAM and PSRAM, SMP; could run multiapp by the S31 recipe | No radio. Its Wi-Fi comes from the C6. |

- **S31 Wi-Fi:** the hal has `components/esp_wifi/esp32s31/esp_adapter.c`, but its blobs directory
  holds only libphy (no net80211, pp or coexist), and the Zephyr Wi-Fi driver does not list the
  S31.
- **P4 with a C6:** the P4 takes Wi-Fi from the C6 through esp_hosted, and our C6 SDR image replaces
  that firmware (`apps/c6_bootstrap`). Coexistence there means a C6 image that carries both on the
  SDIO link: the SDR stream on one channel, 802.3 frames on another.

## Plan

1. **Bench** (done for T3, T1 and T2, T4 on the S3, see above). Open: T3 at DTIM 3 in `MIN` modem,
   and the same on a second access point.
2. **Arbiter in the host** (built, standalone S3: mode B above): client counting over
   `esp_phy_enable/disable`; restore of channel, offset, 0x67 codes, AGC and front-end forcing; an
   SDR claim and preempt API; scheduled windows with the `_phy_enable` backstop. T5 done: Wi-Fi
   unharmed, 88 % to the SDR with no traffic. Open: a UDP rate test, and the cause of the spurious
   wake after a release.
3. **Shared `phy` on the S3** (built, not yet run on the board): `wifi` has no libphy and requires
   `phy`; `phy.ver` is widened; the arbiter is in `phy`; PLL tracking is filtered to Wi-Fi's own
   holds; `module_heap` is 35840 bytes. `wifi` and `phy` fill it, `sdr` loads into dump bank 2
   ([RAM pressure](#ram-pressure-on-the-s3)). The `coex` app is the bench (ring windows only).
4. **`espnow` module** (built, not yet run on the board): `espnow.o` of libespnow, a Zephyr-side API
   and the `espnow` shell, in PSRAM; `manatick.o` stays in `wifi` because net80211 reads it. `coex`
   requires it, and `espnowtest` boots ESP-NOW without an access point. Not done: the split of
   `wifi_core` and `wifi_net`.
5. **Driver deinit and llext unload** (done on the S3, `tmp/coex/t6soak.py`):
   - the driver gains `esp32_wifi_shutdown()` (disconnect, stop, `esp_wifi_deinit()`, event thread
     ended, callback removed), which `wifi`'s `multiapp_module_down()` calls, then the hal's interrupt
     release, `multiapp_wifi_unbind()` and `esp_sdr_arb_wifi_detach()`;
   - `sdr` stops its ring and DAC threads and releases the arbiter (`esp_sdr_release_radio()`), `espnow`
     calls `esp_now_deinit()`, `phy` shuts the arbiter down and unloads only with no client left;
   - `multiapp run` switches apps in place when the USB profile matches and no console app is involved,
     unloading the modules the next boot's list does not share (as a stack: the first difference and
     everything after it), else reboots;
   - not done, so a reboot: the blobs' PSRAM buffers are not in a private arena (a leak there shows in
     `multiapp info` as `psram_*` or `kernel` shrinking), and the soak is the check.
   - T6 passed (DevKitC, 2026-11-24): 100 in-place cycles of `wifitest`, join, `rfprobe`,
     `probe rx 1024`, no reboot, every heap's free and largest block unchanged from the first cycle
     to the last, the Wi-Fi sources parked after each unload, the FROM_CPU rows unchanged.
6. **C6 resident variant:** Wi-Fi heap over the gapless region, deinit before gapless; measure
   gapless after deinit and the rejoin time.
7. **C5:** add the arbiter and ESP-NOW to the resident build.
8. **S31 and P4:** after the S31 has Wi-Fi blobs and driver glue.

## Evidence

- `apps/coexbench/` (the bench), `apps/multiapp/wifi.cmake`, `ext/phy.ver`,
  [multiapp internals](../apps/multiapp-internals.md).
- `lib/esp_sdr/src/esp_sdr.c` `radio_up()`, [Transceiver](transceiver.md) (front-end forcing,
  regi2c locking), [Tuning](tuning.md) (retune time), [Bursts](../samples/bursts.md) (the 139 ms
  glitch).
- Blob symbol tables: libespnow imports and sizes; the blobs import 11 (C6), 14 (S3) and 18 (C5)
  libphy symbols directly; no supplicant imports; libcoexist `coex_schm_*`.
- C6 `libnet80211` `wifi_rf_phy_disable()` calls the PHY off through `g_osi_funcs_p`; `libpp`
  `pm.o` calls enable and disable on sleep and wake.
- The hal fork's `esp_phy_enable/disable` use per-modem bit flags, not counts (`phy_init.c`,
  `phy_common.c`): Wi-Fi's disable powers the RF down under an SDR that called enable too, which
  is why the arbiter counts the clients itself.

<!-- nav-start -->
Previous: [Crystal calibration](crystal-calibration.md) · Up: [The radio](index.md) · Next: [Samples in and out](../samples/index.md)
<!-- nav-end -->
