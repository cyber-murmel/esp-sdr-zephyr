# Network services on the S31 multiapp host

<!-- nav-start -->
Previous: [Transmit stream library (lib/esp_sdr/src/txs)](txs.md) · Up: [Reference](index.md) · Next: [Transmit from GNU Radio World to an S31](grworld-tx.md)
<!-- nav-end -->

The ESP32-S31 CoreBoard's multiapp host is a network host: Ethernet, addresses and a name, as the
base for SSH, MCUmgr and file transfer. This page records the choices of each phase, as they stand now. Phases
1 to 4 are built and run on the CoreBoard (the HIL suite `apps/multiapp/pytest/test_multiapp_s31.py`, 41 tests); phase 5 (SFTP)
was dropped and phase 6 (the Ethernet streamers as extensions) decided against ([Phases 5 and 6](#phases-5-and-6-decided-against-2026-11-27)).

## Phase 1: network host

Built and run on the CoreBoard (cb1, cb2). Shared fragments in `apps/common/net/`:

- `s31_eth.overlay`: `eth`, `mdio` and `phy` on, `twai0` off (GPIO13/14 are shared with the RGMII
  clocks), the PHY's `default-speeds` deleted for 1000BASE-T. Same content as the board overlay of
  `apps/stream_eth`, which stays as it is. multiapp's board overlay `#include`s it.
- `net.conf`: DHCPv4 and IPv6 (link-local, SLAAC, ND), UDP and TCP, the net shell, no static
  address, hostname `esdr` with the whole MAC appended in hex (`esdr0123456789ab`), the mDNS responder. Buffers
  for a control plane: 6 RX and 4 TX packets, 7 RX and 3 TX buffers of 1536 bytes (64 byte
  aligned for the MAC's DMA), 4 RX and 4 TX descriptors, 4 contexts, 3 connections (phase 2 raises them to 8 contexts and 10 connections, phase 3 to 10 and 12, below). The network
  threads and the TCP worker run preemptible at priority 10 and the MAC's refill thread at 9:
  below the radio, the app (0) and the USB threads (8) that run osmosdr_hs's stream. The system
  work queue stays cooperative (-1): it feeds the liveness watchdog and runs osmosdr_hs's link and
  suspend work.
- `apps/multiapp/sysbuild.cmake` adds `net.conf` to the multiapp image's `EXTRA_CONF_FILE` for
  the board `esp32s31_function_coreboard` only (also when the variable is given on the command
  line). `socs/esp32s31_hpcore.conf` sets `NETWORKING=n`, `net.conf` turns it on after it: a build
  without sysbuild has no network stack. The S3 targets keep `prj.conf`'s `NETWORKING=y`.
- `apps/multiapp/net_psram_s31.ld` (a `RAM_SECTIONS` snippet, the technique of
  `lib/esp_sdr/esp_sdr_txmem_s31.ld`) puts the network management and RX stacks, the TCP work queue
  and sockets service stacks, the log and esp_timer stacks and the two DNS message data pools into
  PSRAM (PSRAM is accessible only after `PRE_KERNEL_1` priority 40).
- `apps/multiapp/multiapp_lpram_s31.ld` puts cold stacks and buffers in LP RAM (below).
- `apps/multiapp/src/reboot_s31.c` stops the receive ring's DMA before every reset (below).
- `scripts/tcm_groups.py --multiapp`, run after the link, checks the placement.

### Findings on the board (cb2, 2026-11-26)

- The EMAC's DMA does not reach PSRAM. TX buffers in PSRAM: every send fails. RX buffers in PSRAM:
  pings of 500 bytes and more are lost. With both internal: ping 56 to 1400 bytes without loss,
  1 Gbit link, SLAAC works. So the network data buffers and the MAC's descriptors stay internal
  (no option for PSRAM; `ESP_SDR_S31_TXMEM_NETTX_PSRAM` is documented as broken).
- Stacks in PSRAM only for threads that never touch the flash: the flash driver turns the cache off
  during flash operations and such a thread stalls (the board hung after the banner: main scans the
  FAT volume). In PSRAM: the network threads, the log thread, esp_timer. See
  [Code placement](code-placement.md).
- Internal RAM is therefore short: the host's stacks and plain buffers go to the LP RAM, below.

### Fit with the LP RAM (TX and RX buffers internal)

`apps/multiapp/multiapp_lpram_s31.ld` puts into `lp_ram_seg` (from 0x2E0000B0) the COLD things only:
the stacks of main, the shell and the system work queue, the system heap (one
object: the k_heap struct and its bytes; no atomic_t in it without SMP), the log buffer and the shell
history. LP RAM is not behind the cache, so these
threads run through flash operations, and it is accessible from reset. It is slow: the first board run
with the app's stack, the USB device thread and the UDC thread there lost samples (osmosdr_hs at
20 MS/s: ring stopped, 1466 device overflows), so those stay internal. The linker's region check
bounds the section, and ASSERTs check the main stack and the heap are in it. The "no atomics in LP
RAM" rule of `esp_sdr_place.h` is an assumption of that header (commit 716790e), not a measurement;
nothing placed here has an atomic.
In this first fit the module heap was 117760 B (osmosdr_hs's measured use, 116292 B, plus a 1.4 KB margin) and the image
ended 512 B below the ring. Phase 2 moved osmosdr_hs out of it: the module heap is 16384 B now ([Memory: the group 3 heap](#memory-the-group-3-heap)).

### Board numbers and what they set (cb2, s31-qol-uart)

- Stack peaks (`kernel thread stacks`): system work queue 1016 of 1024 (DHCP, mDNS and net_config
  run there), so it is 2048 now; net_socket_service 1096 of 1536; shell 1312 of 4096 (now 3072);
  usbd 256 of 4096 (kept: standalone osmosdr_hs writes the calibration file from that thread, and a
  settings write through FAT needs more than the idle peak); rx_q 1080 of 4096 (2048); net_mgmt 336
  of 3072 (1536).
- System heap: 3024 of 3072 bytes in use at the peak with the network. No allocator was found by
  reading the net, shell, USB and file system sources (candidates: TCP, getaddrinfo, the fs shell);
  it is 3072 now (the later peak was 2224 B, 38 % headroom), in LP RAM. `kernel heap` on the shell shows the use.
- MTU 1492 is the router's: an RA MTU option (PPPoE uplink) goes through `net_if_set_mtu()`
  (`ipv6_nbr.c`); the driver reports 1500 (`NET_ETH_MTU`). IPv6 packets above 1492 are then not
  sent by the stack and one of 1464 bytes of ICMP payload does not fit. Nothing to fix here.

### Streaming with the network up, and a warm reset (cb1, 2026-11-26)

- The first product image dropped samples (20 MS/s: ring stopped, 1466 device overflows). Suspected
  cause, fixed by construction: the app's stack, the USB device thread and the UDC thread were in the
  slow LP RAM. They are internal again, and only cold things stay in LP RAM.
- The drive is not cold while a stream runs (hotswap's MSC-while-running): the PC polls it and reads it, and its
  thread, the flash disk's cache and the FATFS object were in LP RAM in the first rebased image. osmosdr_hs at
  20 MS/s on cb1, 60 s each: those three in LP RAM 567 gaps (only the thread's stack moved out: 567), all three
  internal 0 gaps; 300 s with the network up 0 gaps, 0 overflows, 40,155.7 kB/s, ring 0 errors (the pre-rebase
  image had 4 per 300 s). Nothing the USB drive touches while a stream runs may be in the LP RAM:
  `tcm_groups.py --multiapp` fails if the mass storage thread's stack, `flashdisk0_cache` or `multiapp_fat_fs`
  is there. They cost 5.7 KB of internal RAM (the 4 KB cache, the FATFS object, the thread's 1152 byte stack).
- Board gate (cb1): osmosdr_hs at 20 MS/s for 300 s with the network up, 0 gaps; a warm reboot in
  the middle of a stream boots MCUboot fine. Also on the board: DHCPv4 binds, SLAAC, mDNS resolves
  `esdr<mac>.local`, pings 56 to 1400 B over IPv4 and IPv6 without loss. A UART capture on the PC
  during a stream shows about 21 gaps/s: a measurement artefact of the capture, not of the image.
- Priorities: the stream runs on the app thread (0) and the USB device and UDC threads (8). The
  network threads and the TCP worker are at 10, the MAC's refill thread at 9; the system work queue is
  cooperative (-1) as before.
  The EMAC interrupt does the receive work per frame (net_pkt allocation, a cache invalidate over the
  whole 1536 B buffer, queueing to the RX thread): a few microseconds a frame, and a quiet LAN sends
  few. A busy LAN would add to the stream CPU's load (the driver is in `modules/zephyr`).
- After a CPU reset the ring's GDMA keeps writing into TCM group 2 (0x2F040000 to 0x2F060000) while
  the ROM loads MCUboot to 0x2F05AB00 ("SHA-256 comparison failed", then an illegal instruction).
  The dump engine is not the cause: its ownership register is cleared after the setup of a diag16
  run. `apps/multiapp/src/reboot_s31.c` wraps `sys_arch_reboot()` (so every `sys_reboot()`: the
  shell, the fatal error handler, DFU, the app's own) and stops PARLIO and the GDMA channel first.
  A watchdog reset runs no software: the liveness watchdog uses `WDT_FLAG_RESET_SOC` (a system
  reset), which should reset the GDMA and PARLIO blocks as well, not verified; a CPU-only or ROM
  reset path would still have the hazard.

## Phase 2: own signing key and MCUmgr over UDP

Status (2026-11-27): run on the boards (cb1, cb2): MCUboot with the own key, the gate, uploads and files, signed
extensions, the stream rules; see "Board results" and "What ran and what did not" below. The internal RAM comes from the group 3
heap below.

### The signing key

Until now every image was signed with MCUboot's public development key (`root-*.pem` in the MCUboot
tree), which protects nothing. The S31 multiapp now uses its own ED25519 key, kept outside the repository:

```
mkdir -p ~/.config/esp-sdr/keys && chmod 700 ~/.config/esp-sdr/keys
nix-shell -p mcuboot-imgtool --run \
  'imgtool keygen -k ~/.config/esp-sdr/keys/esdr-mcuboot-ed25519.pem -t ed25519'
chmod 600 ~/.config/esp-sdr/keys/esdr-mcuboot-ed25519.pem
```

There is no second copy: whoever loses the key reflashes MCUboot over the UART. Never print, copy or commit
it; the public key is compiled into MCUboot (not secret).

- `apps/multiapp/Kconfig.sysbuild` sources `apps/common/sysbuild/Kconfig.signing`, which defaults
  `SB_CONFIG_BOOT_SIGNATURE_TYPE_ED25519` and `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE` for the board
  `esp32s31_function_coreboard` (`SB_CONFIG_ESDR_SIGNING`, which any other app turns on the same way:
  source that file ahead of `share/sysbuild/Kconfig`, `include()` `apps/common/sysbuild/signing.cmake`).
- The key file is the environment variable `ESDR_SIGNING_KEY`, by default
  `~/.config/esp-sdr/keys/esdr-mcuboot-ed25519.pem`. A missing file fails the configure step with the
  recipe above (`signing.cmake` checks that it exists; it never reads it).
- `ESDR_DEV_KEY=1` asks for MCUboot's development key instead (`root-ed25519.pem`) and prints a boxed warning.
  Use it to check that an image signed with it is refused by a bootloader built with the own key
  (`s31-qol-devkey` is such a build: its bootloader accepts dev-key images, the product one does not).
  Remove the build directory when the key changes (a stale `.config` keeps the old file name).
- A build that is not flashed (CI: `.github/workflows/build.yml` sets `ESDR_DEV_KEY=1` for twister, the runner has no
  key; a clone without the key) uses `ESDR_DEV_KEY=1` too: the build then warns and its images are signed with
  the public key.
- No downgrade prevention: any image signed with the own key may be uploaded, an older one included.
- The version of an image is VERSION's `major.minor.patch` plus a build number that is the git short hash of the
  app's tree, 8 hex digits, the whole 32 bits (`git rev-parse --short=8 HEAD`, the first 8 characters). Bit 15
  (0x8000) of the patch field is set when the tree has uncommitted changes (tracked files), so a dirty tree
  reads `0.2.32768+<hash>` in the raw version. `signing.cmake` computes both and sets
  `CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION` of the image (imgtool `--version X.Y.Z+<number>`). Everything that
  prints a version decodes it like git: `0.2.0 (g01a2b3c4-dirty)` in `west mcumgr list`, `multiapp info` and the boot
  banner, always 8 hex digits with leading zeros (`src/gitver.h`, `mcumgrlib.version_text()`); without the
  sysbuild version the host prints VERSION's. A new HEAD (a commit, a checkout) re-runs the configure (CMake
  depends on the HEAD and branch files), so every `west build` after it re-reads the hash. The dirty flag is the
  state at the last configure: edits of tracked files after it do not re-run it (a pristine build or `cmake .`
  in the build directory refreshes it). The key check also stops a configure whose key is under the
  MCUboot tree or named `root-*` unless `ESDR_DEV_KEY=1` (a stale `.config` from a dev-key build).
- MCUboot with ED25519 (tinycrypt, `CONFIG_BOOT_ED25519_TINYCRYPT`; imgtool 2.4.0 signs it) is 61904 bytes
  with the defaults against the 57344 of its partition at 0x2000 (RSA-2048 was 52080). `sysbuild/mcuboot_s31.conf`
  cuts it to 55856: no log and printk (`LOG=n`, `PRINTK=n`: it printed at level 0 anyway), no GPIO driver
  (about 2 KB with the interrupt allocation it pulled in; MCUboot has no use for it), no ASN.1 parse of the public
  key (`BOOT_KEY_IMPORT_BYPASS_ASN`) and `-msave-restore`. Each saves its share, none alone fits. It boots on
  the board (cb2, below). Kconfig warns about the
  choice `MCUBOOT_LOG_LEVEL_OFF` once (harmless).

### MCUmgr over UDP

`apps/common/net/mcumgr.conf` (appended after `net.conf` by `sysbuild.cmake`, S31 CoreBoard only): the UDP transport
on port 4996 for IPv4 and IPv6, no DTLS (the gate and the signature are the protection), groups
`img` (upload, list, test, confirm, erase), `os` (echo, reset, MCUmgr parameters, info, bootloader info) and
`fs` (upload, download, status, CRC32; not SHA-256, which would pull the PSA crypto library). No shell group.
Frames are 1444 bytes (1492 router MTU less IPv6 and UDP headers; the stack never fragments IPv6), two buffers. The
UDP thread and the work queue run at priority 10 like the network threads. The work queue runs the handlers,
which write the flash (the cache is off meanwhile), so its stack and the two buffers are in the LP RAM
(`multiapp_lpram_s31.ld`), never PSRAM. `scripts/tcm_groups.py --multiapp` checks that.

zcbor, which MCUmgr needs, is a west project now: `west.yml` lists it in the allowlist (`modules/lib/zcbor`, the
revision Zephyr pins). Until the main checkout's manifest has the line, the build needs
`-Dmultiapp_ZEPHYR_EXTRA_MODULES=<workspace>/modules/lib/zcbor` after `--`.

### The arm gate (`apps/multiapp/src/netsvc.c`)

The board refuses every command until it is armed on a shell (the CDC-ACM or UART shell, as `tx arm` does).
A refused command is answered: the MCUmgr error "access denied" (rc 11) comes back, and nothing else happens (the
first board test saw "no answer" only because the client then could not decode the indefinite-length CBOR of the
reply and dropped it):

| shell | effect |
|---|---|
| `mcumgr arm <minutes>` | 1 to 60, MCUmgr commands are served until then (arming again restarts the time) |
| `mcumgr disarm` | closes the gate |
| `mcumgr status` | gate, seconds left, whether image uploads are allowed |

A reboot disarms (RAM only). Refused, and logged (`mcumgr: refused ...`, one line a second, the rest counted):

| when | what | answer |
|---|---|---|
| disarmed or expired | every command, any group (`MGMT_EVT_OP_CMD_RECV`) | access denied |
| a stream runs | every command of the image and file groups, reads included: list, state, upload, erase, test, confirm, file read, stat, hash, write (`MGMT_EVT_OP_CMD_RECV`, and `MGMT_EVT_OP_IMG_MGMT_DFU_CHUNK`) | "streaming: flash access refused" (rc 256, `MGMT_ERR_EPERUSER`; the volume busy answer is rc 10) |
| any time | file access outside `/FLASH:/`, or with a `..` element (`MGMT_EVT_OP_FS_MGMT_FILE_ACCESS`) | access denied |
| the PC is writing to the drive and its disk access does not end in 2 s, or another user holds the volume | a file command | busy (rc 10, `MGMT_ERR_EBUSY`); the volume would not mount: unknown (rc 1) |
| the build cannot verify extension signatures | writes of `*.llext` files | access denied |

"A stream runs" means a ring is running and, for the SDR's USB class, that the PC keeps reading it (see below): a ring is running: the radio module's `esp_sdr_diag16_active()` or `esp_sdr_ring_active()`
(`multiapp_ring_running()`, the same predicate that keeps the PC's drive away), checked at each command, or an open
`multiapp_stream_hold()`: an app that streams otherwise (stream_eth, when it is a multiapp app) calls it at start and
`multiapp_stream_release()` at the end; nothing calls it yet. An SDR host that is connected but not streaming allows
commands (first version: `usb_sdr_enabled()`, true whenever a PC attached, refused everything; board 2026-11-26).
A stopped stream allows commands again at once.

A transfer (an upload or a download) claims the volume with its first command (`MGMT_EVT_OP_CMD_RECV`) and keeps it
across its chunks: fs_mgmt keeps its file open between them, so a claim and mount that ended with each command left it
a dead handle (first board test of the rebase: `fs get` of 6224 B failed). It ends when fs_mgmt reports the transfer
done (`MGMT_EVT_OP_FS_MGMT_FILE_ACCESS_DONE`), when a stream starts (`multiapp_stream_hold()`, or the next refused
command), or 4 s after the last command (fs_mgmt closes an abandoned file after 3 s, `mcumgr.conf`). The short
commands (stat, hash, crc) take the volume for themselves and give it back at their end, so that `multiapp run` or
`multiapp stage load` right after a `west mcumgr fs put` (which ends with a crc request) finds it free; the loader
also tries its claim three times. A thread of its own, `mcumgr_fs_hold` (internal 3 KB stack), takes and gives the
claim, as a mutex wants its owner to give it; the MCUmgr thread does the file IO under it.

A new transfer, once the last has ended, gets a mount of its own (the volume is given back and claimed afresh):
reusing a held mount for the next transfer failed on the board, a get after a put and a get of another file after a
get ("unknown", rc 1; the same get worked 5 s later). While fs_mgmt may still have a file open (the last
transfer command is less than 3.5 s old: an abandoned upload, say) the mount is never given back, so that
fs_mgmt closes its file on a mount that is there (a lost-cluster risk otherwise); a command that needs more than the
held claim has is answered busy then.

Through `multiapp_drive_fs_begin()` / `_end()` in `drive.c`, which wrap the host's claim API
(`multiapp_drive_read_begin()` / `_write_begin()`, `src/drive_proxy.c`, "Writers and readers of the volume" in
`doc/apps/multiapp-internals.md`) and mount and unmount the volume, as nothing stays mounted between claims. The
claim takes the volume from the PC instead of refusing. A transfer hides the drive from the PC (the write claim: no
medium, for the transfer and at least 3 s after), with a read-only mount for a download and a read-write one for an
upload: a read claim would leave the PC's disk accesses waiting, and they wait for 10 s at most (`DRIVE_GATE_MAX_MS`)
before they fail, which a long download outlasts. The short commands take a read claim. There is no fs delete or mkdir
over MCUmgr (fs_mgmt has none here). The claim waits up to 2 s for the PC's disk access in flight to end. Answers:
`-EBUSY` (the PC's access did not end) and `-EAGAIN` (another user holds the lock) are "busy" (rc 10); `-EDEADLK`
cannot happen here (the MCUmgr thread is not the USB thread); a failed mount is "unknown". FatFs is not reentrant,
and the same lock orders the app loader's reads (`multiapp_drive_read()`) and `multiapp stage load`. In a boot
without an app that has the drive on the bus the claim is only the lock.

Why reads too: on the board (cb1, 20 MS/s) a file read of 3 KB during a stream cost about 2 gaps: flash reads
stall the cache as writes do. Only the os group (echo, parameters, info) runs from RAM and stays open. This is a
rule for every network way into the flash while an app streams, the SSH key file access of phase 3 included (SFTP, once
planned as phase 5, was not built): refuse them with the same "streaming" error, and take the check from `stream_busy()` (`netsvc.c`).

### `west mcumgr`

`scripts/west_commands/mcumgr.py` with a small client of its own, `mcumgrlib.py` (a CBOR subset and the 8 byte header):
smpmgr 0.18.0 does speak UDP but cannot choose the port (it always uses 1337), and the dev shell has no cbor2.
Targets: `-t <IPv4|IPv6|name>` (`fe80::1%eth0` for link-local; `esdr<mac>.local` needs mDNS on the PC), port 4996.
Tests: `tests/host/test_mcumgrlib.py` (CBOR vectors from RFC 8949, framing, chunking, TLV hash, a UDP loopback board).

A network update, start to end (the gate closes at every reset, and the new image boots on trial):

```
# shell: mcumgr arm 10
west mcumgr -t <host> upload build/multiapp/zephyr/zephyr.signed.bin   # marks it for a test boot
west mcumgr -t <host> reset                                            # swap, trial boot of the new image
# wait for the board (DHCP: ping it), then on its shell: mcumgr arm 10
west mcumgr -t <host> confirm                                          # within 2 minutes, else it reverts
```

```
# on the board's shell: mcumgr arm 10
west mcumgr -t esdr0123456789ab.local echo hello                  # is the gate open
west mcumgr -t <host> list                                        # slots, versions, hashes, flags
west mcumgr -t <host> upload build/multiapp/zephyr/zephyr.signed.bin   # uploads, marks for a test boot
west mcumgr -t <host> reset                                       # swap, boots the new image on trial
west mcumgr -t <host> confirm                                     # once it runs: permanent (else it reverts)
west mcumgr -t <host> upload image.bin --confirm                  # permanent at once (no revert)
west mcumgr -t <host> fs put app.llext /FLASH:/app.llext
west mcumgr -t <host> fs get /FLASH:/hello.llext hello.bin
west mcumgr -t <host> fs stat /FLASH:/app.llext                   # also: fs crc
west mcumgr -t <host> info                                        # MCUmgr buffers, bootloader
```

The image must be signed with the own key (the build's `zephyr.signed.bin`); the tool checks the MCUboot header
and finds the image hash itself. A dev-key image uploads fine and MCUboot then refuses to boot it.
An unconfirmed image reverts: the trial watchdog (`APP_TRIAL_WATCHDOG`, as with `west dfu`) resets the board when
the time is out. The shared default of `APP_TRIAL_TIMEOUT_S` is 60 s (`apps/common/Kconfig`, the other apps keep it);
the S31 multiapp (`socs/esp32s31_hpcore.conf`) sets 120 s: a reset closes the gate, so the re-arm on the shell
and the confirm are by hand after the reboot and the DHCP lease (measured: the board answers MCUmgr about 9 s after
the reset, a confirm by hand takes 10 to 25 s). The board resets at an uptime of
`APP_TRIAL_TIMEOUT_S` (the boot log says "trial boot: reverts at uptime N s"; the uptime starts after MCUboot's
swap, which adds its own time). Measured (2026-11-26): with 300 s set, the first build reverted at 162 s:
the Zephyr watchdog driver (`modules/zephyr/drivers/watchdog/wdt_esp32.c:41`, prescaler 40000) ticks 0.5 ms on the
S3 (APB clock) but 1 ms on the S31, C6 and P4 (40 MHz XTAL; the HAL's default there is 20000), and the old code
fed the liveness watchdog only until `TRIAL_S / 2` assuming 0.5 ms everywhere. `app_usb.c` now allows for the
chip (`WDT_REAL_MS`). Effect on the other users of that code, for 60 s: S3 apps with the liveness watchdog
reverted after about 32 s and now after 60 s; S3 apps with only the trial watchdog after 30 s, now 60 s; C6 and
P4 apps with the liveness watchdog after about 34 s, now 60 s. A
`confirm` that is allowed without `mcumgr arm` in a trial boot (a gate that opens for that one command) was
considered and not implemented.

Errors name what is known: access denied is the same code for a closed gate and for a path outside `/FLASH:/`,
so the tool says both.

### Memory: the group 3 heap

MCUmgr's stock UDP transport keeps a thread, its stack and a receive buffer per address family in one internal
object (5760 bytes for two), plus the TCP slab that grows with `NET_MAX_CONTEXTS` (+1360), the connection manager
and the image and file contexts: the first build ended 9600 bytes past the ring (0x2F042580). Moving that to
PSRAM or LP RAM is not possible (a thread control block must stay reachable while the flash cache is off, and
kernel objects stay out of LP RAM), so the room comes from where osmosdr_hs's 104 KB of USB batches were:

- `BANK2` (apps.c, `src/bank2.h`; the mechanism of the S3's dump bank 2) now exists on the S31 too, with TCM group 3
  (0x2F060000 up to the ROM boot stack at 0x2F07AFB0, 110512 bytes; the engine's aperture, the bank, is
  0x1A010 of it) as `bank2_heap`. An app with `BANK2` in `CMakeLists.txt` loads wholly into it (fast text,
  data, bss) instead of `module_heap`; the radio module (`sdr`, 9.2 KB) stays in group 0. Only osmosdr_hs
  has the flag: the boot of an app without it keeps group 3 as the capture bank. The text of an INTERNAL app
  in that heap is internal RAM, as `.module_fast` needs (the hot swap refuses a kernel outside it).
- `CONFIG_MULTIAPP_MODULE_HEAP_SIZE` goes from 117760 to 16384 (radio module 9.2 KB, about 7 KB for an app's
  fast kernels: rfprobe's hot swap needs at least 1.5 KB of that free). Group 0 gains 101376 bytes.
- The guard: `ext/sdr_up.c` calls `esp_sdr_bank2_reserve()` when the boot lent group 3; from then
  `esp_sdr_bank_usage()` (every capture, replay, transmit, trigger and gapless path) and `esp_sdr_s31_capture()`,
  `_dac_start()` and `_ring_start()` return `-EBUSY`. The diagnostic-bus ring (osmosdr_hs's stream) is unchanged: it
  writes group 2.
- The dump engine does own group 3 for a moment: `dump_setup()` (once, in `esp_sdr_init()`, before osmosdr_hs
  is loaded) and `esp_sdr_s31_diag_start()` (CPU parked and interrupts off, about 64 spins with the engine
  running, then ownership back). In that window nothing reads the heap (that code is NOCACHE IRAM and uses the
  radio module's data in group 0), but the engine may write words at the group's start: the heap therefore begins
  1 KiB (256 words) into the group (`BANK2_GUARD`, `src/bank2.h`), about 16 times what the window can write at 20 MS/s
  (64 spins is some 250 cycles; the real figure was not measured). The heap is 109488 bytes: osmosdr_hs needs
  about 107 KB. USB DMA or another bus master reading group 3 in that microsecond would see the engine's data:
  not measured (board test: a stream with many restarts).

Numbers of the phase 2 build `s31-qol-multiapp` (the product; SSH, MicroPython and FPU sharing came later and took most of the arena, below): end of image 0x2F02EE80 (after the rebase on the hotswap work and the drive's thread, cache and FATFS object going internal; 0x2F029980 before), so 70016 bytes of group 0
(the libc arena up to the ring at 0x2F040000) are free where 512 were; LP RAM 20388 of 32768 used (MCUmgr's part:
stack 3072 and two 1444 byte buffers, 6336 bytes with alignment); MCUboot 55856 of 57344.

| boot | group 0 free | group 3 free |
|---|---|---|
| osmosdr_hs | 70016 B above the image, about 7 KB in `module_heap` | about 2.4 KB in `bank2_heap` (107 of 109.5 KB) |
| rfprobe, capture | 70016 B above the image, about 7 KB in `module_heap` less rfprobe's kernels | 0: the capture bank (106512 bytes) and 4000 bytes of tail nobody uses |

SSH (phase 3) takes 40.6 KB of the 70 KB in group 0 (below), leaving 29440 B; MicroPython's imports and FPU sharing (phase 4) leave the arena at 23808 B (product) and 25984 B (UART variant) in the current build.

#### App switches and group 3

`multiapp run <app>` switches in place when `switch_ok()` (`apps.c`) allows it, else reboots into the app. The
in-place path (hotswap's unload work) handles a `BANK2` flip on its own: it unloads every module (the radio
module's `multiapp_module_down()` stops the diagnostic bus, so the dump engine is idle), checks that nothing
is left allocated in `bank2_heap`, then re-initialises the heap, or leaves group 3 to the engine, before the
radio module loads again and `multiapp_module_up()` reserves or does not reserve the bank. On the S31 that is
never taken: osmosdr_hs, the only `BANK2` app, has the vendor USB class the capture-bank apps (rfprobe,
capture) lack, so `profile_of()` differs and the switch reboots (`reboot_s31.c` stops the ring's DMA first). To keep
it so if another app gets `BANK2` with the same classes, `switch_ok()` refuses an in-place switch on the S31 whenever
the new app's `bank2` differs from the boot's: group 3 changes hands only through a reboot. Switches that keep the
flag (rfprobe and capture, osmosdr_hs to itself) stay in place; the heap and the reservation then do not change.
The in-place switches ran on the CoreBoard in the hot-swap round (before the network host); the `BANK2` flip path itself is never taken
on the S31 and has not run.

### Board results (cb2, UART variant, 2026-11-26)

MCUboot with ED25519 boots. Disarmed, the client saw no answer (see the gate above: the board answers "access denied"); `mcumgr arm 10` opens it. echo, image list, MCUmgr
parameters, `fs stat` and `fs get` work; a path with `..` is refused. An upload of 777616 bytes takes 13 to 15 s
(51 to 59 kB/s). `test` plus a reset gives the trial boot ("reverting in 60 s unless confirmed", with the
earlier timeout), an unconfirmed image reverts, a confirmed one stays. An image signed with the dev key uploads
and MCUboot refuses it: the old image keeps running. OS bootloader info answered "not found": the group is enabled, but the client asked for the query
"bootloader", which Zephyr does not know (it answers a query-less request with "MCUboot" and "mode"): fixed in
`mcumgrlib`. Two bugs the test found: `NET_MAX_CONN` 3 and `NET_MAX_CONTEXTS` 4 were too small for the two
UDP sockets (`smp_udp` bind failed with -ENOENT: now 10 and 8, in `net.conf`, which makes `tcp_conns_slab` and the context tables bigger), and the board's replies use
indefinite-length CBOR (mcumgrlib decodes them).

### Signed extensions (`apptrail.py --key`, `src/extsig.c`)

Files loaded from the volume are code the host runs, so they are signed like the images. `apptrail.py --key
<ed25519 pem> --name <name>` (the MCUboot signing key, same `ESDR_SIGNING_KEY` / `ESDR_DEV_KEY` rules; the build
passes `CONFIG_MCUBOOT_SIGNATURE_KEY_FILE`) appends a block after the 32 byte trailer: the 64 byte ED25519
signature, then `SIG1`. What is signed is the SHA-256 of `MAPSIG1`, 0, the name, 0, the ELF and the trailer (its
magic, size, CRC, stack and build id): the name binds the file, so a signed `hello.llext` copied to `capture.llext`
or `sdr.llext` does not verify. The trailer itself is unchanged (hotswap's `--kernel` trailer, magic MKT1, takes
the same block, signed under the kernel's name). `check_trailer()` in `apps.c` calls `multiapp_sig_check(name,
source, ...)` first, with the name of the app, module or kernel it is asked for: it strips the block, and for every
source but `built-in` (MCUboot's image signature covers those) verifies it against the public key compiled into
the host (`pubkey.py` writes it from the signing key at configure time; MCUboot's own ED25519 code, `ext/fiat` and
tinycrypt's SHA-512 and SHA-256, is built into the host: about 16 KB of flash). The verifier needs a 4.3 KB stack
(its frame is 3136 bytes, `-fstack-usage`), so it runs on a worker thread with its own 6 KiB stack in group 0, which
main and the shell wait on. A file that fails is not used: the host logs why and falls back to the built-in copy
(a module or a kernel has none: the load fails and the stage keeps its function). A build without the own key
(`SB_CONFIG_ESDR_SIGNING=n`, the S3) warns at configure, signs and checks nothing; one with it (`MULTIAPP_SIGNED_EXTENSIONS`)
stops at configure if the key cannot sign extensions (not ED25519, unreadable), and `fs_mgmt` refuses writes of
names that FAT would open as an extension (`*.llext`, any case, trailing dots, `~` aliases). The log says
`verifying N bytes` and `signature ok after N us`. Board (cb2, 2026-11-26): `sdr` 113524 bytes in 45 ms,
`osmosdr_hs` 13200 bytes in 13 ms; a tampered `hello.llext` written with `fs put` is refused ("signature does not
match") and the built-in copy runs. Tests: `tests/host/test_extsig.py` builds the verifier and `extsig_core.c` for
the PC (RFC 8032 test 1, strip and length, name and content binding, the built-in exemption), `test_apptrail.py`.

Hot-swap kernels (hotswap's `MKT1` files) are signed too: `check_trailer()` is one function for apps and kernels and
starts with `multiapp_sig_check()`, so a kernel with no or a bad signature fails the load and the stage keeps its
function. The signature is made under the kernel's own name (`multiapp_kernel()` passes `--key --name <kernel>` to
`apptrail.py --kernel`) and checked under it: `multiapp stage load <stage> <file>` takes the name from the file
(without `.llext`), `multiapp stage put <stage> <size> <crc> [timeout ms, 0 for the default] [name]` from its last
argument (`kswap.py` passes the kernel file's name). The sources the shell passes are `"file"` and `"upload"`, never
`"built-in"` (which skips the check). A build that does not verify (no key) ignores the block.

### The volume's lock

FatFs is not reentrant. The host's claim (see above) serialises its paths: each MCUmgr file command (`netsvc.c`),
`multiapp_drive_read()` (the app files) and `multiapp stage load`. MCUmgr's own idle close of an abandoned
transfer runs on the system work queue outside the claim, so the claim outlasts it (`mcumgr.conf`: it closes after 3 s, the claim ends 4 s after the last file command). The crystal calibration is not a FAT file on this
host any more: it is settings on ZMS in the `calibration` partition (16 KiB at 0x7e0000,
`boards/partitions_s31.dtsi`), written from the USB thread, so `fs get /FLASH:/settings` finds nothing and the
volume's lock does not cover it.

### SMP experiment (symmetric multiprocessing, not adopted)

The host was made SMP with the network on CPU 1 (local branch `s31-qol-smp`, not pushed: 71156552 plus a work-in-progress commit with
Kconfig ints for the CPU of the network, the app and USB): pin-only scheduling, the EMAC interrupt freed on CPU 0 and
allocated again on CPU 1 (`--wrap=esp_intr_alloc`; Zephyr's deferred device init cannot do it, `net_if_init()` never
initialises an interface whose device was not ready), the network's threads pinned by name. The image grew by 26 KB and the libc
arena fell to 50 to 55 KB. osmosdr_hs at 20 MS/s on cb1, a quiet PC, two runs of 60 s each, gaps:

| build | gaps |
|---|---|
| single core, network up | about 1 per 75 s |
| SMP, network on CPU 1 | 8 and 9 |
| SMP, app, ring and USB on CPU 1 (the S3 layout) | 1301 and 1394 |
| SMP, nothing pinned to CPU 1 | 243 and 21 |
| SMP, FPU sharing off | 16 and 1 |
| SMP, with the stack fixes | 824 and 1352 |

The runs differ a lot from each other and an interface that is down still gave 8 to 16 gaps per 60 s, so SMP itself
costs the USB path. Not adopted: the S31 multiapp stays single core. Ideas for later: CPU 1's idle and the scheduler
IPI (`IPI_OPTIMIZE`), contention for the shared data cache and the PSRAM between the cores, the ring's interrupt and
the USB interrupt when both are on one core and the other core runs a PSRAM-bound thread, and the diag16 slab swap,
which takes the global `irq_lock()` under SMP.

### Module heap slack

`tcm_groups.py --module-heap SIZE:sdr.llext` runs after every link: the radio module's fast code, data and bss
take 9792 of the 16384 bytes of `module_heap`, 6592 free (the check fails below 1536). Only `sdr` loads there in
the rfprobe and capture boots; osmosdr_hs lives in group 3.

### What ran and what did not

Run on the boards (cb1, cb2): the gate, upload, swap and revert, the dev-key refusal, the trial window (120 s),
the stream refusals, the signature check and its time, a tampered file refused, file put, get and crc in every
order. osmosdr_hs at 20 MS/s for 300 s with the network up: the first runs of the phase 2 image had 11 and 13 gaps, in bursts
(traffic on the LAN that the single core takes), 0 gaps with the interface down; ring 0 errors. Later runs of the current image
with no traffic, and with an idle SSH session open, had 0 gaps (with and without FPU sharing); what traffic costs is in
"Single core: SSH and the stream" below. Not run: a network update of a build with a changed bootloader.

## Phase 3: SSH server

`apps/common/net/ssh.conf` (added after `mcumgr.conf` by `sysbuild.cmake`, S31 CoreBoard only; it needs MCUmgr, whose
`netsvc.c` holds the stream rule): the Zephyr SSH server (`subsys/net/lib/ssh`, experimental) on TCP port 22, IPv4 and IPv6
(the server's socket takes both; `NET_IPV4_MAPPING_TO_IPV6` is selected by it, MCUmgr's sockets keep their own port space),
public key login only, the Zephyr shell on the session. Run on the boards (cb1 and cb2); the HIL tests are
in `apps/multiapp/pytest/test_multiapp_s31.py` ("SSH"). The sections below mix the board findings of the first versions (an RSA
host key, since removed) with the current rules; where a number is old, the text says so.

Is SSH always on? Yes: the server starts at boot, once there is a host key, and needs no `arm` of its own. A key that is
not in the key table cannot log in, and an empty table (no `authorized_keys`, a fresh board) means nobody can: the
authorisation is the file on the volume and the board's own shell. `mcumgr arm` and `mcumgr disarm` work from an SSH
session like from the CDC-ACM or UART shell, so the session is the authenticated channel that opens the update path
(`ssh <host> 'mcumgr arm 10'`, then `west mcumgr ...`).

### Board results of the ECDSA version

The ECDSA host key is made in well under a second and replaced the stored RSA key (cb2 had finished its RSA key after about 40 minutes); the
fingerprint is the same after resets, and OpenSSH's `known_hosts` matches `ssh hostkey`. ed25519 and ECDSA logins work, an unknown key is
refused, password login is not offered (`Permission denied (publickey)`), exec output and status come back (0, and 8 for an unknown command),
`mcumgr arm 1` works over SSH. A connection takes about 0.6 s (the key exchange). Stacks: `sshd_work` had 1472 of 6144 bytes unused after boot, the server thread
3872 of 6144 after sessions. What the boards found, and what changed after it:

- **A crash with sessions back to back** (`ssh key remove`, `ssh key add`, a login with that key, as separate exec sessions): `fatal error 0:
  thread ssh_sh`, `z_swap`, a load fault with `mepc` 6, the thread's saved context overwritten. Cause: the session shell is one Zephyr shell
  instance, and its thread object is made again by every `shell_init()`. `shell_uninit()` is asynchronous: the shell thread runs the uninit
  callback (where `console_mux.c` marked the shell free) and only then aborts itself (`shell.c`, `kill_handler()`). The server thread has the higher
  priority, so a new session ran in between and `shell_init()` made the thread again while the old one was still alive. Now a new session
  waits for the callback (`sh_gone_sem`) and then `k_thread_join()`s the old thread before it starts the shell; Zephyr's own SSH shell backend
  has the same window. HIL test `test_ssh_fast_exec_cycles_all_succeed` (20 rounds of remove, add, login, command; then `multiapp crash`).
- **Over-long exec commands** (more than 255 characters) and a busy shell no longer drop the connection: the request is accepted, the answer is
  a line on stderr and the exit status is 1.
- **The first connection after a boot** (39.6 s once, `Connection closed` once): the server now starts after the key file was read (a few tries,
  then it starts anyway and the work thread keeps trying), so an early login does not meet an empty key table. The cause of the slow first
  connection is not proven: on the test PC
  two interfaces share the subnet and the first IPv4 packet to a freshly booted board is often lost until ARP settles (the same as for
  MCUmgr, `west mcumgr` notes); `ssh -o ConnectTimeout` retries cover that.

### The Zephyr server and the patches this phase needed

First run on the boards (cb1, cb2): an RSA 2048 host key was still being made after 20 minutes (GDB: `ssh_keygen` ->
`mbedtls_rsa_gen_key` -> `mbedtls_mpi_is_prime_ext` -> `mpi_check_small_factors` -> `mbedtls_mpi_gcd`, the constant-time
GCD of mbedTLS, about 2400 iterations per candidate), and at the shell's priority and above it starved the shell and the log.
So the library got these changes (modules/zephyr, branch `zsdr`, one commit each; all but the first two are upstream-style
candidates):

- **ECDSA P-256 keys** (`ecdsa-sha2-nistp256`, `SSH_HOST_KEY_ECDSA_P256`, on by default) for the host key and for users, with
  PSA (`ssh_keygen(..., SSH_HOST_KEY_TYPE_ECDSA_P256, 256)`, `SSH_HOST_KEY_FORMAT_RAW` to import and export the 32 byte scalar
  or the 65 byte point). A server only offers the host key algorithms that fit its host key's type (an RSA host key still
  works where RSA is built in: `rsa-sha2-256`). The multiapp host's host key is ECDSA: made in a moment, and it is what OpenSSH prefers.
- **Ed25519 user keys** (`ssh-ed25519`, `SSH_USER_KEY_ED25519`, off by default): the library has no Ed25519 code and does not
  pull any in; the application provides `int ssh_ed25519_verify(pub, msg, len, sig)` (`zephyr/net/ssh/keygen.h`), which gets the
  whole signed message. `sshd.c` implements it with MCUboot's fiat `ED25519_verify` that the host links for the extension
  signatures (`extsig.c`: `multiapp_ed25519_verify()`), on that file's worker thread (the verifier needs a 3.1 KB frame and
  4.3 KB of stack in all) at priority 10 instead of its 5. Ed25519 cannot be a host key.
- **The exec command and the subsystem name** reach the channel callback (`ssh_channel_event_channel_request.data`), and
  `ssh_channel_exit()` ends a server channel after the command: once the TX data is out it sends `exit-status`, EOF and close.
- `SSH_SERVER_THREAD_PRIORITY` (default 5, the old fixed value; 10 here) and `SSH_SERVER_STACK_SIZE` (default 4096; 6144 here).
- The authentication methods the server lists follow how it was started: no `password` without a password, no `publickey`
  without authorized keys (and a request for a method that is not allowed is refused).
- **RSA optional** (`SSH_HOST_KEY_RSA`, default y): with it off the PSA and mbedTLS RSA options, the DER and `mbedtls_pk` code and the rsa-sha2
  algorithms are not built. The multiapp host turns it off (user decision: no RSA host key, no RSA user keys): `ssh-rsa` lines in
  `authorized_keys` are listed with their size and `NOT USED`. Built both ways (`tests/net/lib/ssh/ssh_server` with it on, the scratch key test
  with it off); the client, which signs with RSA only, has no public key authentication without it.
- `ssh_server_set_banner()`: the text sent before the login (the default is "Welcome to Zephyr OS build ..." and a tilde-dot hint, to every
  session, exec included). The host sends `esp-sdr multiapp 0.2.0 (g<hash>)`; OpenSSH shows it on stderr unless `LogLevel` is below INFO.
- `NET_SHELL_SSH_KEYGEN` (default y): the `net ssh_key` commands, which can print private keys, with their 2 KB buffer; the
  host turns it off.

Still true of the server: key exchange `curve25519-sha256`, cipher `aes128-ctr`, MAC `hmac-sha2-256`, signatures `ecdsa-sha2-nistp256` and `ssh-ed25519` (and `rsa-sha2-256` where RSA is built in); any user name is accepted (the key is the
identity). No RSA in the multiapp host.

**Post-quantum key exchange.** OpenSSH 10 warns `connection is not using a post-quantum key exchange algorithm`, because the server only has
`curve25519-sha256`. The warning is harmless for a board on a LAN; a client silences it with `-o WarnWeakCrypto=no` (or in `~/.ssh/config`:
`Host esdr*` and `WarnWeakCrypto no`). The hybrid `mlkem768x25519-sha256` is not available here: the TF-PSA-Crypto of this tree has no ML-KEM
(only an ML-DSA driver, `drivers/pqcp`, which SSH does not use), and the SSH library would need the key exchange method as well. Not implemented. No locking in the build's mbedTLS (Zephyr's port has no threading layer): PSA calls of two
threads must not interleave. `sshd.c` changes the key table at a priority above the server thread's (the server is blocked
outside PSA then). The per-connection buffers are fixed in `ssh_transport.h` (4 KB key exchange heap, two 2 KB packet buffers,
10.5 KB with the rest). Tests: the library's own `tests/net/lib/ssh` (`ssh_pkt` and `ssh_server`) pass on `native_sim/native/64`
(the 32 bit target needs a 32 bit libc this shell does not have); a scratch test of the new key code (not in the tree:
`tmp/s31-qol/sshkeytest`) checks an ECDSA sign and verify round trip, a signature made by Python's `cryptography` and the
board's format verified by Python, the raw import and export, and the Ed25519 plumbing; `samples/net/ssh` still builds.

### Host key

An ECDSA P-256 key, made by `ssh_keygen()` (PSA) on the first boot with the hardware TRNG (`zephyr,entropy` is the SoC's
`trng0`; `ssh.conf` selects `CSPRNG_NEEDED`, which gives `MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG` with `HARDWARE_DEVICE_CS_GENERATOR`;
`TEST_RANDOM_GENERATOR` is off and `sshd.c` has a `BUILD_ASSERT` for both). Where it lives: **the settings (ZMS, the
`calibration` partition), key `ssh/hostkey`**, the 32 byte private scalar. Why that and not the FAT volume: the volume is the
PC's USB drive, a file there is readable and replaceable by anyone who plugs in (the private key would be a file on a
removable disk), the device never writes it except for what the user asks, and a write takes the drive away from the PC for
seconds. The settings partition is the device's own, already there (calibration, 16 KiB, four sectors), written once. Not
encrypted: flash encryption is not on this board; anyone with the flash chip has the key. A stored key that is not 32 bytes
(the RSA key of the first build, or damage) is replaced by a new ECDSA key, with a warning in the log, and its fingerprint
differs: clients that pinned the old one see a changed key.

`sshd_work` (priority 14, the shell's and the log's, so they take turns with it) makes it and writes it to the settings, after
which the server starts. The log and the UART say `ssh: new host key ecdsa-sha2-nistp256 SHA256:...`. Writing waits for the
end of a stream (`multiapp_net_stream_busy()`: the flash write stops the cache); until then `ssh hostkey` says "NOT saved
yet" and a reset loses it (the loop looks again every 5 s). To get a new key, erase the settings entry (no command yet).

`ssh hostkey` prints the fingerprint as OpenSSH does (`SHA256:` and 43 base64 characters, the same string the client asks
you to confirm on the first contact and `ssh-keygen -lf known_hosts` prints). Check it over the serial or CDC-ACM shell
before trusting the first connection; clients that pin the key (`StrictHostKeyChecking=yes`) then notice a changed key.

### Users' keys

`/FLASH:/ssh/authorized_keys` on the FAT volume (the drive the PC sees, so a PC can edit it directly, and `west mcumgr fs put
authorized_keys /FLASH:/ssh/authorized_keys` writes it over the network), OpenSSH lines: `ssh-ed25519 <base64> [comment]`,
`ecdsa-sha2-nistp256 ...`. Other types (`ssh-rsa`: there is no RSA in this build, `ecdsa-sha2-nistp384`/`521`, `sk-*`, `ssh-dss`) are listed with
their fingerprint and `NOT USED`. Blank lines and `#` lines are skipped; a line with key options (`command=`, `from=`,
`restrict`, ...) is refused as a whole, because none of them would be honoured. Loaded at boot (when no stream runs and the PC
is not using the drive: some tries), by `ssh keys reload` and after every `ssh key add` / `ssh key remove`. Six keys fit
(`CONFIG_SSH_MAX_HOST_KEYS` 7, slot 0 is the host key); more lines are listed as `NOT USED: no free key slot`.

| shell | effect |
|---|---|
| `ssh status` | server, sessions, the Zephyr shell taken or free, the server thread's unused stack after the last session, keys loaded |
| `ssh hostkey` | host key fingerprint (and where it is kept) |
| `ssh key list` | the lines of the file: index, type, bits, fingerprint, comment, `NOT USED: why` |
| `ssh key add <line>` | the line, quoted or typed as it is (the words are joined), appended to the file and loaded; refused when the key is not usable, already there, or a stream runs |
| `ssh key remove <SHA256:... or comment>` | the file is copied without the matching lines, then loaded again |
| `ssh keys reload` | read the file again |

The shell line of `ssh key add` is long: `ssh.conf` sets the shell's command buffer to 1024 characters and the serial
RX ring to 1024 bytes (an ed25519 or ECDSA line is 100 to 200 characters). On a serial or
CDC-ACM shell paste it whole or let the host's terminal send slowly. The file writes take the volume with the write claim
like MCUmgr's transfers (`multiapp_drive_fs_begin(true, true)`: the drive is hidden from the PC for at least 3 s). A PC that
writes to the drive at that moment gets `refused: ... the PC is writing to the drive`.

### Sessions and the console mux (`src/console_mux.c`, `src/console_mux_core.c`, `include/multiapp/console.h`)

Zephyr's own SSH shell backend (`SHELL_BACKEND_SSH`) takes every channel for itself and refuses exec requests, so it is
off (`CONFIG_SHELL_BACKEND_SSH=n`); `console_mux.c` is the server's channel callback and carries one more Zephyr shell
instance (`ssh_sh`: transport over `ssh_channel_read`/`_write`, prompt `esdr:~$ `). A new session is served like this:

| client | request | served by |
|---|---|---|
| `ssh <host>` | shell | the console registered with `multiapp_console_register()` if there is one, else the interactive Zephyr shell |
| `ssh <host> shell` | exec "shell" | the interactive Zephyr shell, also when a console is registered |
| `ssh <host> '<command>'` | exec | the command in the Zephyr shell (`shell_execute_cmd()`, on the `sshd_work` thread: the server thread has to move the output): its output, then the exit status (`exit-status`, EOF, close). The command is at most 255 characters: a longer one, or a busy shell, is answered on stderr with status 1. No echo, no prompt, no colours. The shell's negative errno becomes its absolute value as status (a missing command: 8). Input of the client is ignored |
| `pty-req`, `env`, `window-change` | | accepted and ignored (the client's terminal does the editing) |
| `ssh -s <host> sftp` | subsystem | refused, the channel is closed (SFTP was not built, [Phases 5 and 6](#phases-5-and-6-decided-against-2026-11-27)) |

`multiapp_console_register(const struct multiapp_console *)` is the hook phase 4 uses ([Phase 4](#phase-4-the-micropython-repl-over-ssh)): the MicroPython REPL registers
`open()` (a new session: the console takes over `io->read` and `io->write`, and does its work in a thread of its own),
`rx_ready()`, `tx_ready()` and `closed()`; `multiapp_console_unregister()` closes its sessions (their `closed()` runs
first) and new ones get the Zephyr shell again; only one console at a time (`-EBUSY`); `open()` may decline with an error and
the session gets the Zephyr shell. The registry and the choice (`console_mux_core.c`, no Zephyr
in it) are tested on the PC (`tests/host/test_sshkeys.py`); the MicroPython app registers one while it runs.

**Back-to-back sessions.** Board run: 18 of 20 fast cycles failed, either with `Connection closed` within 10 ms or with `Connection refused`.
The server (`ssh_server.c`) had a listen backlog of 1 and, when no client slot was free (one slot here, still held by the session that
just ended), accepted a new connection and closed it at once. Now it stops polling the listen socket while every slot is busy, so a new
client waits in the backlog (`SSH_SERVER_LISTEN_BACKLOG`, 2 here; each waiting connection takes a network context and a handler, so
`ssh.conf` raises `NET_MAX_CONTEXTS` to 10 and `NET_MAX_CONN` to 12, 1.9 KB of internal RAM) and is accepted when the slot is free; the
client waits in its banner exchange, which OpenSSH tolerates (`ConnectTimeout` counts the TCP connect, not the wait). A client that
dropped while queued is accepted all the same and its first read fails, which frees the slot (and `accept()` errors for an aborted
connection no longer end the server). How long a slot stays busy: the channel close goes out when the output is sent
(exec: `ssh_channel_exit()`), the client answers with its own close and a disconnect and closes TCP, and the server frees the slot on
that read: a few round trips, in the order of milliseconds on a LAN (not measured). The session shell is freed by its own thread
(priority 14) a few milliseconds after `shell_uninit()`, and the next session waits for that (`sh_gone_sem`, then `k_thread_join()`)
before it starts a shell, after its key exchange of about 0.5 s, during which the server thread keeps the CPU anyway. Nothing waits on a
timer on this path. `ssh status` shows the numbers of the last session (slot free after the channel closed, shell free after its stop;
also in the log at "client disconnected").

**Second review (fixed).** A session whose channel never opened (an RST after the open request, a failed confirmation, the open result queued during a
key exchange) never got a CLOSED event and kept the only slot for ever; the transport's CLOSED event now marks its sessions and the next client frees
them. The output of a command that outlived its session (the client killed, a new one connected at once) could reach the new client: the
session lookup is under the channel lock, the slot has a generation number that a late job must match, and the shell stays taken until the
orphan command ends. `accept()` backing off no longer stalls the open sessions (the listen socket is left out of the poll for 200 ms instead of
sleeping), the keep-alive toggle moved to the server thread (no other thread touches a socket that may be closed or reused) and the transport
is closed before its socket. A command on the work thread that submits a key request no longer waits for a shell submitter that waits for the work
thread. The private scalar buffer is wiped on every path, and a REPL's `closed()` runs under the registry lock (said in `console.h`).

**Review fixes.**
- *A killed client wedged the shell.* A shell writer waits without a limit for room in the channel (`shell_pend_on_txdone()`); when the client
  went away mid-output the channel never had room again and every later exec was answered "the previous command is still ending". The
  session is marked closing under a lock (`ch_lock`) when its channel closes, the shell's writers are woken (`SHELL_TRANSPORT_EVT_TX_RDY`)
  and a closing session drops the output at once. Every call into a channel from another thread than the server's takes that lock and finds the
  session open or closing, so a writer of lower priority can no longer use a channel the server thread has closed and reset for the next
  client. HIL test: five clients killed mid-output, then a normal command.
- *One idle connection locked everyone out.* The server closes a connection that has not logged in after `SSH_SERVER_LOGIN_GRACE_TIME` (30 s here),
  checks every second (it woke every 10 s and checked nothing), and enables TCP keep-alive on the clients (30 s idle, 3 probes 10 s apart) to
  free the slot of a client that vanished. HIL test: an idle TCP connection, then a login that must work within the grace time and a margin.
- *Accept errors.* Out of sockets or buffers (`ENFILE`, `EMFILE`, `ENOMEM`, `ENOBUFS`) the server waits and goes on; if its thread ends all the
  same `sshd.c` starts it again (`ssh status` says `NOT RUNNING` meanwhile). A failed transport start no longer leaves a closed descriptor in the
  poll set.
- *Ed25519 without the signing key* would have failed every login: the build now stops (CMake) when `SSH_USER_KEY_ED25519` is set and the verifier is
  not built.
- *Requests after a timeout:* a key command that outlasts the 20 s wait stays the work thread's until it ends; new ones get `-EBUSY`, and the answer
  text is copied out under the lock. `authorized_keys` lines with control bytes (a newline in an exec command, which would have been written as
  a second, unchecked line) are refused. The private host key scalar is wiped from RAM after it was imported and after it was saved.
  The registered console's callbacks are called under `cmux_lock()`. `ssh status` shows the unused stack of `sshd_work` after its commands.
- *Stream rule over SSH:* HIL test `test_ssh_key_file_changes_are_refused_while_a_stream_runs` runs `build-clanker/host/esdr_usb ... rx` (20 MS/s) in the
  background (skipped without the tool; `ESDR_USB_SERIAL` names the board). By hand: start that stream, then `ssh <host> 'ssh keys reload'` must say
  `refused`, `ssh key list` works, and after the stream ends the reload works.

**Single core: SSH and the stream.** The host has one core (the SMP experiment, above, made it worse), so network work costs the USB stream
gaps. Measured on cb1 at 20 MS/s over USB HS, 60 s each, with the user's rule that it is acceptable for USB and the network not to work
perfectly at the same time:

| during the 60 s | gaps |
|---|---|
| an idle SSH session open (300 s run) | 0 |
| 10 exec logins, one every 5 s | 30 |
| 10 commands in one open interactive session, one every 5 s | 61 |
| 11 pings, one every 5 s | 15 |
| the same pings with the phase 2 image (before SSH existed) | 28 |

The per-packet work of the network stack on the one core costs gaps, and that came before SSH. Use SSH and MCUmgr when no stream runs; an open
idle session costs nothing (300 s at 20 MS/s: 0 gaps, with and without FPU sharing, once the keep-alive is paused during a stream). A login costs about 0.5 s of CPU at priority 10. `ssh key list` works during a stream (it prints the table in RAM, with
a note that the file is not read then); `ssh keys reload`, `ssh key add` and `ssh key remove` are refused (below).

**Keep-alive and the stream.** The TCP keep-alive probes of an idle session (`ssh.conf`: 30 s idle, 3 probes 10 s apart) cost the stream a gap each
on this core (cb1, 300 s at 20 MS/s with an idle session open and the keep-alive always on: 4 gaps, one a minute; none
without the keep-alive). So a system work queue item looks once a second whether a stream runs and pauses them then (`ssh_server_set_tcp_keepalive(false)` sets a flag that
the server thread, which owns the sockets, applies within a second to the open connections and to new ones (`SO_KEEPALIVE` cleared); Zephyr's TCP stops probing at once, its timer fires once more without
sending), and starts them again when the stream ends. A client that vanished during a stream is found after it ends; the login grace time
still protects the slot from clients that never log in. (Zephyr's `keep_alive_timer_stop()` cancels the wrong timer, `fin_timer`, so a pause
only relies on the probe timer finding `keep_alive` cleared, which it does.)

**A killed PC program.** A program that is killed or crashes does not stop the device's ring, which then runs with nobody reading it. Until
this was fixed that left `multiapp_net_stream_busy()` true until a later client started and stopped a stream cleanly (measured on cb1: a
SIGTERM to `esdr_usb` kept `ssh keys reload` refused for more than 50 s, and MCUmgr's image and file commands with it). Now a ring that
runs counts as a stream only while the PC finishes IN transfers of the SDR's USB class (`multiapp_osmosdr_last_in_ms()`, noted in
`usb_sdr_in_done()`; the rule is `stream_consumer_live()` in `src/streamgate.h`, tested on the PC with wrap-around of the 32 bit uptime,
a stale transfer of an earlier stream, a consumer that stops and a slow one). The window is 5 s: with no finished transfer for 5 s,
counted from the ring's first look while there has been none, the flash is free, so updates and key changes work 5 s after the program
died (the HIL test allows 10 s). A PC that pauses its reads for more than 5 s looks gone and a flash write may then take samples from
the stream that resumes; a shorter window made an MCUmgr upload (its first chunk erases the slot) run into a resumed stream. The ring
itself keeps running; whether osmosdr_hs should stop it when the host went away is a question for its owner. A ring in a boot without
that USB class (an app that does not stream to the PC) is a stream for as long as it runs.

**SMP.** The SSH host code (`sshd.c`, `console_mux.c`) is not SMP safe: the PSA exclusion is by thread priority, which holds on one CPU only, and
its flags and tables have no locks against a second CPU. The S31 multiapp host is single core; both files have `BUILD_ASSERT(!IS_ENABLED(CONFIG_SMP))`,
so an SMP build stops until the locking is done. (The apps and the rest of the host keep their own SMP rules.)

One Zephyr shell session or command at a time (one instance, a 3 KB stack in the LP RAM): another one reads `The shell is in
use by another SSH session.` and is closed. The server takes one connection (`SSH_SERVER_MAX_CLIENTS=1`: 10.5 KB each; a second
connect waits until the first ends). When the client's input ends (EOF: `ssh <host> shell < file`) the board closes the
channel at once, before any output the shell still has to write: keep the input open (a terminal, or a pipe that waits) until
the answer is there; an exec command is not cut by it (`ssh -n` is fine). A command that outlasts its session (the client left)
is let run to its end, its output dropped, and then its shell is taken down. The commands run at the shell's priority and may touch the flash
(its stack is not in PSRAM); the stream rule below covers what the key commands do, the other commands are the board's shell
commands and check for themselves.

### What is refused when

| when | what |
|---|---|
| always | password login (not offered: the server's userauth failure lists `publickey` only); a key that is not in the table; a key of another type, RSA included; file lines with options; subsystems |
| a stream runs (`multiapp_net_stream_busy()`, as MCUmgr's image and file groups) | `ssh key add`, `remove`, `keys reload` (all read or write the volume); saving a new host key (done later). Logins, `ssh key list`, `ssh hostkey`, `ssh status` and the rest of the shell go on: they touch RAM only |
| the PC is writing to the drive, or another claim holds the lock | the same commands: `refused: a stream runs, or the PC is writing to the drive` (-EBUSY) |
| the host key is being made (a moment) | the key commands: `the SSH server is not ready yet`; no logins (the server is not started) |

The shell commands a logged-in user runs are the board's shell commands: `multiapp ...`, `stage ...`, the file system
shell if it is built in and the kernel ones; none of them checks the stream rule today except the loader's own claims. The session
is as trusted as the CDC-ACM shell.

**The stream predicate (`stream_busy()`, `src/netsvc.c`, `src/streamgate.h`).** Busy while a ring runs and the PC finished an IN
transfer less than 5 s ago (the first 5 s of a ring count as busy, a killed client is free after 5 s). Board run, 2026-11-27: `ssh status`
said "allowed" at moments of a live stream and a key file was read. Cause: `osmosdr.c` stamped a transfer with `k_uptime_get_32() | 1U`,
which rounds an even millisecond up by one: a caller whose "now" was in that millisecond saw the transfer 1 ms in the future, its unsigned
age wrapped and the consumer read as gone. Now the exact uptime is stored, and `stream_consumer_live()` takes a stamp slightly after "now" as
age 0 (`test_streamgate.py`). A ring that is off between two runs of one stream had been guarded by a 3 s grace in a first version; the
counters showed the ring-off branch never fired during live streams, so it was dropped. A ring that goes off and on between two looks of the predicate is a new stream: the library exposes no run counter (only `esp_sdr_diag16_active()`, a flag), so
the host stamps every control OUT of the PC (`multiapp_osmosdr_last_ctl_ms()`: the SET_MODE that starts or restarts a stream, a retune) and treats one newer
than the ring's first sighting as a new first sighting. A run counter in `lib/esp_sdr` would make this exact. `ssh status` prints the inputs of the last verdict
(ring running, last IN age, ring seen age) and how often each branch said no (no ring, no consumer), and the oldest IN age of a live stream.

### Costs and placement

| what | where | size |
|---|---|---|
| server object (`ssh_server_instances`: thread, 6 KB stack, one connection) | internal (kernel objects) | 17040 B |
| `sshd_work` stack (host key, key file, PSA imports, the commands of exec requests) | LP RAM (it writes the flash) | 6144 B |
| session shell stack and history bytes (`ssh_sh_stack`) | LP RAM | 3072 + 512 B |
| session shell instance (`ssh_sh_ctx`, 1 KB command buffer, 1 KB `temp_buff`) | internal | 2244 B |
| key table, key buffer, line buffer, request, exec command (`entries`, `der`, `line_buf`, `req`, `exec_cmd`) | internal | 1216 + 640 + 1024 + 1128 + 256 B |
| the command buffer and RX ring of the CDC-ACM/UART shell, grown for long key lines | internal | about 2.5 KB more |
| PSA key slots (24), file descriptors (20), fdtable, `ZVFS` | internal | about 2 KB |
| mbedTLS heap (`_mbedtls_heap`, `MBEDTLS_HEAP_CUSTOM_SECTION`, `net_psram_s31.ld`) | PSRAM | 65536 B (was 131072 for RSA key generation; the key exchange of a login is the largest user now; the peak is not measured) |
| code (mbedTLS and PSA ECDSA and curve25519, the SSH library) | flash | about 70 KB more |

Group 0 free above the image (the libc arena) was 29440 B with SSH (UART variant 32000), from 70016 before SSH: 40.6 KB, 17 KB of it the server's object (the 6 KB
stack included). With MicroPython's imports and FPU sharing (phase 4) it is 23808 B (UART variant 25984) now. LP RAM used: 30116 of 32768 B (about 830 B left), from 20388. Dropping RSA (the ECDSA build before it had 31744 B and the same LP RAM) saved
about 20 KB of instruction flash (`irom0` 455168 to 434688 B) and 1 KB of data flash, 512 B of internal RAM, nothing in LP RAM or PSRAM; the signed image
file shrank by 1.1 KB only, its segments being 64 KB pages. The checks that hold the rules
(`scripts/tcm_groups.py --multiapp`: `ssh_sh_stack` and `sshd_work_stack` in the LP RAM and never PSRAM, `_mbedtls_heap` in PSRAM,
`ssh_server_instances` and `ssh_sh_ctx` internal) run after every link. The mbedTLS heap is a plain buffer that mbedTLS formats at
`POST_KERNEL`, after PSRAM works (PRE_KERNEL_1 priority 40; the other placements in `net_psram_s31.ld` are stacks and data pools);
k_heap and slab storage never go there. The SSH server thread runs at 10 like the network threads and `sshd_work` at 14 (the shell's), all
below the app (0, 1) and the USB threads (8); a login's signature check runs on the extension-signature worker at 10 for the
Ed25519 case. The server thread's unused stack after the sessions was 3872 of 6144 B on the board (above); the current figure is in `ssh status`
(`CONFIG_INIT_STACKS`), and `sshd_work`'s are logged once the keys are loaded.

### Tests

Host (`tests/host/test_sshkeys.py`, in `scripts/run-host-tests.sh`): `authkeys.c` and `console_mux_core.c` built for the PC;
SHA-256 against `hashlib` for the block edges; fingerprints of RSA (listed, never used), ed25519 and ECDSA keys against `ssh-keygen -lf`;
authorized_keys lines (CRLF, comments, options, ed25519, ECDSA P-256 and other curves, broken base64, a wrong type name, too long, even
modulus); the raw key data, the SPKI DER against OpenSSL's, the wire blob from PKCS#1 and from a point against OpenSSH's; the
console registry (default shell, register, `-EBUSY`, exec never gets a console, unregister tells the glue under the lock).

Board (`apps/multiapp/pytest/test_multiapp_s31.py`, with the OpenSSH client of the PC, `-i key -o StrictHostKeyChecking=no
-o UserKnownHostsFile=/dev/null`): the fingerprint of `ssh hostkey` is the one the client records, an ECDSA key; logins with an
an ed25519 and an ECDSA key made in a temp dir and added with `ssh key add`; `ssh <host> 'kernel version'` and
`'multiapp list'` over exec with their output and status, an unknown command's status; `ssh <host> 'mcumgr arm 1'` and an MCUmgr
echo after it; the interactive shell by `ssh <host> shell` and by a plain `ssh -T <host>`; an unknown key of each type refused (RSA included), an RSA line refused by `ssh key add`, an over-long command answered on stderr, 20 rounds of fast
exec sessions with key add and remove in which every command must succeed, followed by `multiapp crash`; three concurrent clients that must all be served; killed clients and an idle connection that must not wedge the shell
or a login; the stream rule over SSH (it starts `esdr_usb` and skips without that tool);
password login not offered; a subsystem refused; `ssh status` and an option line refused by `ssh key add`; the host key's
fingerprint the same after `kernel reboot cold` over SSH (last). The keys are removed at the end (`ssh key remove <comment>`). The REPL path has its own tests ([Phase 4](#phase-4-the-micropython-repl-over-ssh)).

### Setup

```
# once, on the PC: a key for the board (ed25519 or ECDSA P-256; RSA is not taken)
ssh-keygen -t ed25519 -f ~/.ssh/id_esdr
# on the board's shell (CDC-ACM or UART): the host key's fingerprint, the key
ssh hostkey                                        # note SHA256:...
ssh key add "<contents of ~/.ssh/id_esdr.pub>"
# on the PC: the first contact shows the same fingerprint
ssh -i ~/.ssh/id_esdr -o IdentitiesOnly=yes esdr0123456789ab.local            # or the address
ssh -i ~/.ssh/id_esdr esdr0123456789ab.local 'mcumgr arm 10'                   # one command
```

Not measured: the generation time of the ECDSA key ("well under a second"), the stack headroom of the session shell, the mbedTLS
heap's peak. Logins with each key type from OpenSSH, exec
output and status, and a cold reboot (`kernel reboot cold`) that keeps the host key are tests of the suite; a warm reboot is not.

## Phase 4: the MicroPython REPL over SSH

MicroPython is an app of the S31 multiapp host (`apps/multiapp/micropython.cmake`, the same sources as the S3's), and a plain
`ssh <host>` reaches its REPL while it runs. State: it builds (product and UART variant) and runs on the CoreBoard; the REPL, `esdr.shell()`,
the flash worker and the stack sizes below are measured there and covered by the HIL suite (marker `switches_app`). What is not
checked is listed in [Not checked on a board](#not-checked-on-a-board-phase-4).

### What runs

`multiapp run micropython` reboots (a console app, always: the USB profile and the bank 2 flag are the smaller reasons) into a boot
with the radio module `sdr` and `micropython.llext`, a file only (1.0 MB: no built-in copy). The host's shell gives up the CDC-ACM
port as for the S3's MicroPython (`console_take()`), so the REPL is on the port; in the UART build (the scratch variant with the
shell on UART0) the UART log backend is switched off at the same moment, so that the host's log lines do not print into the REPL.
The Zephyr shell stays reachable over SSH only: `ssh <host> shell` (exec "shell") and `ssh <host> '<command>'` are the Zephyr shell, and
`multiapp run osmosdr_hs` there (or any reset) goes back to the default app. `esdr.shell("...")` in the REPL runs a host command too.

What differs from the S3's build (`micropython.cmake`, `ext/mp/`):

| part | S3 | S31 |
|---|---|---|
| thread | `src/boot.c`: a thread with a stack in the high region, USB started there | `ext/mp/mp_repl_s31.c`: a thread of priority 13 whose stack is 32 KB in PSRAM (`CONFIG_MULTIAPP_MICROPYTHON_STACK`, the extension's bss), its flash accesses on a worker with an internal stack |
| USB | the app's own disks (`usb_disks.c`, profile 0xc) | the host's drive (profile 0x4); no disks of the app |
| setjmp | wrapped by `src/setjmp.S` (windowed ABI) | the C library's |
| esp-dsp FFT, `esdr_scan_vec.S`, `esdr_scan_pie.c` | Xtensa assembly (PIE vector unit) | not built: `esdr_scan.c`, `esdr_gui.c` and `esdr_wf.c` use their portable code |
| `esdr` module | `modesdr.c` with the ring, TX DAC, Pcap engine | the chip's list in `cmod/micropython.cmake`: `esdr_radio.c` and `esdr_local.c` (bursts from the radio on the chip), `esdr_board.c`, `esdr_chan.c`, `esdr_fsk.c`, `esdr_link.c`, `esdr_lte.c`, `esdr_gui.c`, `esdr_scan.c`, `esdr_wf.c`, `esdr_pcap.c` and its decoders (no engine: the ring stage is not in the host), `esdr_cal.c` for the crystal requests |
| exported symbols | `micropython.ver` | none (a RISC-V relocatable object: `-r`, the partial link of the other S31 extensions) |
| display, LVGL, SD | on the XIAO and the Communicator | none: the headless manifest (`manifest_headless.py`, eggs: `datalink`) |

`import esdr` exists: all of it compiles, links into the extension and every symbol it needs is in the host or the `sdr` module (the
host's link would fail otherwise: `exports.py`). What the S31 loses against the S3's `esdr`: the buffered, gapless receive (the S31's
local path needs `CONFIG_ESP_SDR_DUMPRING` and two CPUs, `esdr_local.c`: this host has neither; osmosdr_hs has the diagnostic-bus ring
in the same `sdr` module, but MicroPython's `esdr` does not use it), the Pcap engine (needs the ring stage), the PIE FFT and frame
passes (the portable code runs instead and is slower; the S31's PIE could do them, `lib/esp_sdr` has no such kernel for them yet), and
the display apps. What is there: bursts through the library, transmit bursts (`CONFIG_ESP_SDR_REPLAY` is on in the host), the packet
modem, `esdr.shell()`. Of the radio parts only the crystal calibration requests run in the HIL suite on this host; `esdr_local.c`'s bursts
ran standalone on the VIEWE board only.

### Memory

- **GC heap in PSRAM**: `multiapp_psram_spare()`, the PSRAM above what the linker placed and the file buffer, as on the S3 (`mp_glue.c`
  adds it with `gc_add()`; the 16 KiB token heap of `esdr_radio.c` is the extension's bss). 13.99 MB read with `gc.mem_free()` on the board (the first
  build-time computation, 13,207,488 B, was 16 MiB less the image's 3,569,728 B: the host's llext heaps, the network stacks and its own bss). No `k_heap` or `k_mem_slab` is
  in PSRAM (`tcm_groups.py --multiapp`, unchanged); the PSRAM is `gc_add()` memory only.
- **The thread's stack is in PSRAM** (32 KB, `CONFIG_MULTIAPP_MICROPYTHON_STACK`, the extension's bss). An 8 KB internal stack from the arena
  gave a Python recursion depth of 11 on the board (`micropython.mem_info()`: "stack: 1044 out of 7168"), and the arena (23808 B in the product
  build, 25984 B in the UART variant, with `FPU_SHARING`; 28544 B before it) cannot spare more. The price: a thread whose stack is in PSRAM must never touch the flash itself (the
  flash driver turns the cache off, and its own stack goes with it). So:
  - everything MicroPython calls that can reach the flash runs on a worker, `mp_flash` (`ext/mp/mp_repl_s31.c`: a work queue, priority 13
    like MicroPython, an internal stack from the libc arena allocated at the first call, `CONFIG_MULTIAPP_MICROPYTHON_WORKER_STACK`, 2144 B);
    MicroPython waits for it. Today that is `esdr.shell()`, which also answers `EBUSY` while a stream runs (the SSH rule,
    `multiapp_shell_exec()` below) and `machine.reset()` (`sys_reboot()` is wrapped in the extension's link and runs on the worker). The
    VFS is not mounted on this host (`_boot.py`). `zephyr.FlashArea` and `zephyr.DiskAccess` of the port (`flash_area_read/write/erase`,
    `disk_access_read/write`) are wrapped too and raise `OSError: the host owns the flash and the volume (use esdr.shell())`: the PC, SSH and
    MCUmgr share the volume by claims, so Python gets no direct access (`test_mp_flash_access_from_python_is_an_error_not_a_hang`). The crystal
    calibration requests of `esdr.request()` (`GET_CAL`, `SET_CAL` with `CAL_*`: `esdr_cal.c`, which loads and saves the settings on the flash) run
    on the worker too (`ESDR_FLASH_CALL`, `esdr_local.c`; `test_mp_calibration_request_runs_on_the_worker`), so `ltecal.py` works;
  - **esdr.shell() through the host.** The first version ran the commands on a Zephyr dummy shell compiled into the extension and crashed the
    board (`multiapp crash`: a load fault at 0x8 in `z_heap_alloc_helper` on thread `mp_flash`): the dummy shell's history is a `k_heap`, which the
    kernel initializes at boot for the host's own objects only (`init_static_heaps()` walks the host's iterable sections), so in an extension it
    is an uninitialized heap. Now `esdr.shell()` calls the host's `multiapp_shell_exec(cmd, out, ctx)` (`include/multiapp/multiapp.h`,
    `src/console_mux.c`), which runs the command on the SSH exec path's shell instance (`ssh_sh`, no echo, no colours) on the worker and passes its
    output to a callback; an SSH session that has the shell makes it wait (up to 5 s, then `EBUSY`). The extension has no `shell_dummy.c` any more;
  - **a build check** (`exports.py`, next to the NOBITS check) fails when the extension's own objects hold kernel objects that need the host's
    boot-time initialization: `._k_heap`, `._k_mem_slab`, `._net_buf_pool` and `._static_thread_data` (a `K_THREAD_DEFINE` thread never starts) sections,
    naming the object and section. Semaphores, mutexes, queues and work items are initialized at compile time and are fine;
  - the host refuses a flash operation from any thread whose stack is in PSRAM: `src/flashguard.c` wraps `esp_flash_read`, `_write`,
    `_erase_region` and the encrypted variants (`--wrap`; the wrappers are what the flash driver calls, checked in the image), returns
    `ESP_ERR_NOT_SUPPORTED` and logs `flash read from thread micropython, whose stack is in PSRAM: refused`. A slip is an error and a log
    line, not a hang;
  - `tcm_groups.py --mp-ext` allows `mp_stack_mem` (and the radio worker's stack) among the extension's stacks and
    fails on any other.
  The arena is back to the rest of the host: the check is the worker's stack plus the measured use of everything else.
  **Sizing rule (user, 2026-11-27):** a stack may be at most 90 % used at its measured peak. It applies to the worker's stack (peak over 0.9)
  and to the C callout margin below Python's recursion limit: `MICROPY_STACK_CHECK_MARGIN` = `CONFIG_MULTIAPP_MICROPYTHON_STACK_MARGIN` = the
  deepest C use past the limit over 0.9. `esdr.stacks()` returns (Python stack, its peak, worker stack, its peak, libc arena free, libc arena allocated); `kernel thread stacks`
  on the Zephyr shell (`ssh <host> 'kernel thread stacks'`, `CONFIG_KERNEL_THREAD_SHELL_STACKS=y`) lists every thread's stack use,
  `micropython` and `mp_flash` among them. **Measured on cb1 (2026-11-27):** with 32768 B for Python, `esdr.stacks()` after boot gave a Python peak of 5476 B; the pure
  recursion reaches depth 61 (about 520 B per level) with a peak of 32024 B, 280 B past the limit (31744); `esdr.shell("multiapp info")` at depth 60
  gave 31224 B, below the limit, so the 1024 B margin (280 / 0.9 = 311 B) holds and stays. The worker's peak was 936 B after `multiapp list` and
  1016 B after `multiapp info` at depth 60; `sshd_work`, which runs the same kind of commands, peaked at 1720 B (`ssh status`). The worker's size
  follows the peak over a set of commands. The read-only set (`multiapp list` and `info`, `kernel thread stacks` and `list`, `net iface`, `ipv6` and
  `mem`, `ssh status`, `mcumgr status`, `help`, `kernel heap`) peaked at 1112 B on the board, which was 1248 B by the rule. But `esdr.shell()` exists for
  commands that read the volume or write settings, whose FAT, ZMS and flash driver frames that set did not include, and the worker's stack comes
  from the arena with no guard. The test (`test_mp_esdr_shell_plain_and_at_the_recursion_limit`) runs the read-only set plus `multiapp crash`, `ssh key add`, `key list`, `key remove` and `keys reload` of a throwaway key, `mcumgr arm` and `disarm`, `fs ls` and `fs statvfs` of `/FLASH:` and `settings list`, and asserts the 90 % rule over them. Measured on the board with a 4096 B stack, that set peaked at 1916 B (with the 1920 B size tried before, 4 bytes were left): 1916 / 0.9 = **2144 B**. The failure message prints the peaks. A job that takes the worker past 90 % logs `the flash worker's stack peaked at ...`
  once. **Measuring the rest of the arena:** after the HIL run, `esdr.stacks()[5]` is the arena's allocated bytes; less the worker's stack
  (2144 B, once started) is what the rest of the host takes after MicroPython started: set `CONFIG_MULTIAPP_MICROPYTHON_ARENA_USE` from it
  (it is 0 today, so the build assumes 8192 B and says "assumed, not measured" in its message; one board reading after the HIL set gave 4 B besides the worker's stack, so 8192 B is a reserve). `tcm_groups.py --mp-ext` fails the build when the arena is
  smaller than the worker's stack plus `CONFIG_MULTIAPP_MICROPYTHON_ARENA_USE`.
- **FPU:** `CONFIG_FPU_SHARING=y` in `socs/esp32s31_hpcore.conf` (measured on cb1, 300 s at 20 MS/s: 0 gaps both ways, 40153.2 kB/s with, 40150.5
  without). On RISC-V Zephyr saves the FP state lazily at the first FP instruction of a thread (`arch/riscv/core/fpu.c` sets `K_FP_REGS`
  itself): no thread has to be created with `K_FP_REGS`; the flag the MicroPython thread passes is harmless. The cost is internal RAM:
  the arena fell by 4.6 KB, 28544 B to 23936 B at the time (the save area in every thread); 23808 B in the product build now.
- **Extension**: `.text` 307 KB, `.rodata` 185 KB, `.bss` 317 KB (the esdr buffers, the FFT and burst arrays), all in the
  extension heap in PSRAM; 1.0 MB file; `.module_fast` is 1.4 KB (the burst decoders), which a non-INTERNAL extension runs from PSRAM.
- **The host grew by 0.9 KB of internal RAM** (the symbols the extension imports: 256 B of IRAM, 640 B of bss, 40 B of data):
  libc arena 29440 B to 28544 B (UART variant 32000 to 30720; before FPU sharing), LP RAM unchanged (30116 of 32768 B, about 830 B left: nothing of
  MicroPython is there), module heap 6592 B free after `sdr` (the radio module is the only one MicroPython needs; MicroPython has no
  INTERNAL parts).
- **Threads**: MicroPython's own at 13, esdr's radio worker and Scan worker at 12 (`mp_prelude.h`: `ESDR_WORKER_PRIO`,
  `ESDR_SCAN_PRIO`; standalone the worker is cooperative 4), below the SSH server (10), the network threads and USB (8), and nothing of
  it near the stream: osmosdr_hs is never loaded in the same boot. The radio worker's 2 KB stack is in the
  extension's bss (PSRAM): neither touches the flash (`MP_EXT_STACKS_OK` in `tcm_groups.py`; a new stack in the extension fails the check).

- **Measuring the stacks.** `esdr.stacks()` after the REPL tests gives the normal peak of the Python thread and of the worker (run
  `esdr.shell("multiapp info")` and a few more commands first). Python's own recursion stops by itself at stack minus the margin, so a
  recursion test says nothing about the size: it shows the depth Python reaches (a function that calls itself with a counter, inside
  `exec()`, counted up to the error; the stack over the depth is the cost per level), and the peak past the limit is the C callout margin:
  `esdr.stacks()[1] - (32768 - margin)` after a deep recursion, over 0.9.
- **The exec stall** (`multiapp info` over SSH stopped mid-output in the first minute of a MicroPython boot, intermittent, CPU idle):
  gone after MicroPython's input wait stopped waking every millisecond (`uart_core.c`: 1000 ms timeout and an event from every input byte);
  35 early execs and 4 switch cycles ran at about 540 ms on the board. The cause is **not proven**: the 1 ms wake-ups are the likely one.
  A 25 ms periodic "poke" of an earlier version is gone too (it alone cost 632 gaps in a stream run). The exec
  path logs `tx: N of M bytes taken` and `tx_ready` with the log's timestamps, to show where an output waits if it comes back.

- **Piped and pasted input.** `echo 'print(6*7)' | ssh <host>` ends its input at once: for a registered console the mux no longer closes the
  channel at the client's EOF but, as for the shell, waits until the console has read all its input (`input_empty()` in `console.h`: the ring is empty
  and the last pull found nothing) and the output has been quiet for 400 ms (5 s at most), then ends the session with status 0
  (`test_mp_piped_input_gets_its_output`). The input ring (512 B) is no longer overrun by a long paste: the feed reads only while the ring has room
  for a chunk, so what is left stays in the channel and SSH's flow control holds the client's rest back; the REPL's thread pulls it as it
  empties the ring (`test_mp_a_long_paste_arrives_whole`, 2 KB in one line; `test_mp_repl.py`). A Ctrl-C behind more than a ring's worth of
  unread input is seen only when the REPL has read up to it.
- **No `/FLASH:` in MicroPython on the CoreBoard.** The host owns the FAT volume (the PC, SSH and MCUmgr share it by claims), so MicroPython has no
  file system of its own there: `_boot.py` mounts neither `/FLASH:` nor `/SD:` when the board's name is
  `esp32s31_function_coreboard`, whichever build it is in (a standalone `apps/micropython` build for that board, none exists today, would have no
  volume either). Files are reached through `esdr.shell()` (host commands) or MCUmgr.

### Switching from and to osmosdr_hs

osmosdr_hs has the `BANK2` flag: its code and data are in TCM group 3 (the host's `bank2_heap`), the capture bank of the dump
engine. MicroPython has not: group 3 is the capture bank in its boot, which `esdr_local.c` uses for bursts. A switch that changes the
flag is a reboot (`switch_ok()`), and so is every switch to or from a console app, so `multiapp run micropython` from osmosdr_hs
(over SSH exec or the CDC-ACM shell) stops the ring through the warm reset path (`reboot_s31.c`: the PARLIO/GDMA of the diagnostic bus
are stopped before the reset), and the new boot starts without bank 2. Going back is the same.

### The REPL stream

Parts: the port's stdio hook (modules/micropython fork, `ports/zephyr/stdio_hook.h`, `CONFIG_MICROPY_STDIO_HOOK`, three functions
`rx`, `poll`, `tx` that return "inactive" when the application has no stream), `ext/mp/mp_repl_s31.c` (the registered console, the
hook, the thread) and `ext/mp/repl_core.h` (the input ring, tested on the PC).

- While no SSH session is the REPL's, `mp_hal_stdin_rx_chr`, `mp_hal_stdout_tx_strn` and `mp_hal_stdio_poll` do what the port always did: the
  console UART (here the shell's CDC-ACM port or UART0) through `zephyr_getchar.c` and `uart_poll_out()`.
- `ssh <host>` (a shell request) makes the mux call the console's `open()`. It takes the channel's `io`, empties the ring, puts a
  carriage return in it (the REPL answers with a fresh prompt, which is what a client waits for) and switches the hook on. From then
  on input and output go through the channel only; the local port is not read or written. A second session while one is open is declined
  (`-EBUSY`): it gets the Zephyr shell, as for any declined console.
- Input arrives on the server's thread (`rx_ready()`), which reads the channel into the ring and looks at every byte for
  `mp_interrupt_char` (Ctrl-C, 0x03): that byte is not stored, and `mp_sched_keyboard_interrupt()` runs from the server's thread, so a
  `while True: pass` that reads nothing is interrupted all the same, as the console hook does for the UART. The REPL's thread also
  pulls from the channel when the ring is empty. The mux passes every byte of a REPL session to the console; the Zephyr shell never
  sees them, so a 0x03 does not reach it. Ctrl-D is data for the REPL's reader: the soft reset runs inside the port's main loop,
  which keeps the hook and the session (the console stays registered over the reset).
- Output is written under `io_lock` in slices the channel takes; a short write waits for `tx_ready()` (20 ms timeouts cover a missed
  one). A write that fails because the session went ends: the rest of the text goes to the local port.
- `closed()` (the client left, or the extension unregisters) runs under the mux's registry lock: it clears the hook's flag, takes
  `io_lock` (the REPL's thread holds it only inside one non-blocking channel call, never while it waits) and clears the pointer to `io`
  before the mux reuses the session slot. It waits for nobody else, as `console.h` requires. The REPL is on the local port again.
- When MicroPython's `main()` returns (the port's `real_main()`; `machine.reset()` does not return) the console is unregistered and
  the hook cleared.
- The same stack serves MicroPython's raw REPL (`mpremote`, `pyboard.py`) over `ssh -tt`.

### Tests

Host: `tests/host/test_mp_repl.py` (the ring: order, Ctrl-C found anywhere in a long stream and not stored, an interrupt character of -1,
a full ring, wrap-around, an error from the channel). Placement: `scripts/tcm_groups.py --selftest` has the `--mp-ext` cases.

Board (`apps/multiapp/pytest/test_multiapp_s31.py`, marker `switches_app`, which `pytest/conftest.py` runs after all others since an
app switch reboots the board): `multiapp run micropython` over an SSH exec; `multiapp info` over exec says MicroPython runs;
`ssh -tt` reaches `>>>` and `print(1+1)` gives 2; Ctrl-C ends `while True: pass` with `KeyboardInterrupt`; Ctrl-D prints `soft reboot`
and the session goes on with fresh state; `ssh <host> shell` is the Zephyr shell; the CDC-ACM port answers `print(3*11)` when no
session is open and again after a session ended; then `multiapp run osmosdr_hs` (a reset if that does not bring it back).
The same marker covers `esdr.shell()` for plain commands and at the recursion limit (with the 90 % rule for the worker's stack),
flash access from Python being an error and not a hang, a calibration request on the worker, piped input and a long paste.

### Not checked on a board (phase 4)

The boot of `micropython.llext` (1.0 MB in the extension heap), the REPL on CDC-ACM and over SSH, Ctrl-C and Ctrl-D, the stack sizes, `esdr.shell()`
and the flash guard are run and measured (above) or tests of the suite. Not checked: `import esdr` beyond the calibration requests (bursts,
the packet modem), the output throughput over the channel, two clients in a row beyond the suite's alternation of SSH and the local port,
the CPU use of the REPL's wait with no stream running, and the UART variant's log switch-off on a board.

## Design: attaching the DWMAC fast receive hook at run time

Status: a design only, not built (`src/netrx.c` does not exist). Its purpose was phase 6, `stream_eth` as an extension, which
was decided against ([Phases 5 and 6](#phases-5-and-6-decided-against-2026-11-27)). It is kept for the case that a second
user of the receive hook (PTP next to a streamer) comes to the host.

### How the hook is bound today

`CONFIG_ETH_DWMAC_RX_FAST` (`drivers/ethernet/dwc_mac/`, `eth_dwmac_fast.h`) offers
`dwmac_rx_hook_set(dev, hook, batch_end, user)`. It is already a run-time registration, not a
link-time hook: the driver keeps `rx_hook`, `rx_batch` and `rx_user` in its per-device data and the
receive interrupt reads them once per interrupt. One hook per device (`-EBUSY` for a second).
Removal (`hook == NULL`) waits, with `rx_busy`, until an interrupt on the other CPU that holds a
snapshot has finished, so the owner may free what the hook uses. `apps/txstream_eth` and
`apps/rxbench` call it from their own `main`. What stays link-time is the code: with
`ETH_DWMAC_TX_RAMFUNC` the receive path runs from IRAM, and `ETH_DWMAC_RX_FAST` must be built into
the host.

### What an llext extension lacks

1. The symbols: `dwmac_rx_hook_set`, `dwmac_rx_stats_get`, `dwmac_send_frames`, `dwmac_tx_free`,
   `dwmac_tx_poll` must be in multiapp's export list (`exports.py` takes what an extension imports
   from the host, so a plain call already works once the host links them).
2. One owner: a second extension (PTP next to stream_eth) gets `-EBUSY`, and nothing records who
   holds the hook.
3. Unload safety: an extension that is unloaded with the hook set, or with claimed `net_buf`s
   not returned, leaves the interrupt calling freed text, or starves the RX pool.
4. Where the hook runs: the interrupt calls into the extension's text. Extension code lives in
   PSRAM; with the flash cache or PSRAM cache busy that is slower than IRAM, and a hot hook must be
   an `INTERNAL` extension or have its hot function in the module's IRAM area.

### Proposed API (host side, `apps/multiapp/src/netrx.c`, exported to extensions)

A small registry in the host; the driver is unchanged.

```
struct mnet_rx_client {
	const char *name;             /* "stream_eth", "ptp": shown by the shell */
	dwmac_rx_hook_t hook;         /* same contract as dwmac_rx_hook_set() */
	dwmac_rx_batch_t batch_end;   /* optional */
	void *user;
	uint8_t prio;                 /* lower first among the clients */
};
int  mnet_rx_attach(struct mnet_rx_client *c);   /* 0, -EEXIST same name, -ENOSPC, -ENODEV */
void mnet_rx_detach(struct mnet_rx_client *c);   /* thread context; returns when no interrupt uses c */
```

- The host registers one multiplexing hook with `dwmac_rx_hook_set()` when the first client
  attaches and removes it with the last detach (so a build without clients keeps the stack's
  path unchanged). The multiplexer offers each frame to the clients in `prio` order until one
  claims it; its batch callback calls each client's `batch_end` that claimed in this interrupt.
- The client table is a small fixed array read by the interrupt as an RCU-like snapshot: attach
  appends and publishes the count after a full barrier; detach clears the slot, then waits for the
  driver's `rx_busy` handshake (detach of the last client is `dwmac_rx_hook_set(NULL)`, for the
  others the multiplexer keeps its own `busy` counter).
- Ownership of buffers: a claimed `net_buf` belongs to the client. `mnet_rx_detach()` does not
  free them: the extension's unload hook (`apps.c` stop path) first stops its consumer, detaches,
  and unrefs what it still holds. The loader refuses an unload while `mnet_rx_attached(name)`.
- The same registry takes the transmit side later (`dwmac_send_frames()` has no state to share, so
  no wrapper is needed beyond the export).
- Extension rules: the hook and `batch_end` run in interrupt context on either CPU, so they are
  short, take no sleeping locks, and when hot sit in internal RAM (`INTERNAL` extension or
  `IRAM_ATTR` placement). Shell: `multiapp net` lists clients and `dwmac_rx_stats_get()`.

Not changed in the driver: its single hook slot stays the interface to the multiplexer, so
`txstream_eth` and `rxbench` (standalone, no host) keep working as they are.

## Phases 5 and 6: decided against (2026-11-27)

- **SFTP (phase 5): not built.** MCUmgr's `fs` commands (`west mcumgr`) already move files to and from the
  volume. SFTP would have added encryption and standard tools (`sftp`, `scp`, `sshfs`), but an `sshfs` mount holds
  the server's only client slot, and a second slot costs about 10.5 KB of internal RAM that the host does not have.
- **`stream_eth` and `txstream_eth` as extensions (phase 6): they stay standalone images**, updated as a whole with
  `west mcumgr upload`. Read-only studies against this host found:
  - Both need driver options that are compile-time (`ETH_DWMAC_TX_FAST`, and for TX `RX_FAST` and `TX_RAMFUNC`) and
    far larger pools: `stream_eth` 64 TX descriptors against the host's 4, and 12/8 RX/TX buffers against 7/3. Only
    `dwmac_rx_hook_set()` is switchable at run time.
  - Both rely on two cores. `stream_eth` pins its sender on CPU1 and was a factor 20 better with SMP than on one
    core; `txstream_eth` pins `tx_flip` on CPU1. The host on SMP cost osmosdr_hs gaps (69 against 4 per 300 s; the 60 s runs of the
    "SMP experiment" above show the same direction), and a single-core 40 MS/s Ethernet stream was never shown.
  - Memory: `stream_eth`'s 54 KB of cells and headers would have to go to the group 3 heap, leaving the arena at
    6 to 14 KB for SSH and MicroPython. `txstream_eth` with the host is 40 to 50 KB over groups 0 and 3.
  - The gain over a standalone image is switching apps without a reflash. A switch is a reboot anyway.

<!-- nav-start -->
Previous: [Transmit stream library (lib/esp_sdr/src/txs)](txs.md) · Up: [Reference](index.md) · Next: [Transmit from GNU Radio World to an S31](grworld-tx.md)
<!-- nav-end -->
