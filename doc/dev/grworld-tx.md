# Transmit from GNU Radio World to an S31

<!-- nav-start -->
Previous: [Network services on the S31 multiapp host](network-services.md) · Up: [Reference](index.md) · Next: [ESP32-S3 registers](registers-esp32s3.md)
<!-- nav-end -->

This page plans a browser transmit path for the ESP32-S31: a flowgraph in
[GNU Radio World](https://gnuradioworld.com) sends I/Q over a WebSocket to a GRWire daemon on a PC,
the daemon passes it through the esdr SoapySDR module, and the module feeds the board's network
transmit stream over Ethernet. The site is served from GitHub Pages.

**Plan only, nothing built.** The state of each part is from the tree and the upstream repository
as read on 2026-11-27; rates are arithmetic unless marked as measured.

## Answer

- **Path.** Browser (GNU Radio World fork) → WebSocket `grwire.v1` → GRWire daemon (fork) on a PC →
  SoapySDR module `esdr` → UDP to [txstream_eth](../../apps/txstream_eth/README.rst) on the S31.
- **Forks.** `cyber-murmel/gnuradio-world`, which holds both the browser app and the daemon
  (`grwire/`). The site is served at `cyber-murmel.github.io/gnuradio-world/`.
- **Five steps:** finish Soapy transmit, arm and retune over the network, transmit in GRWire, a
  GRWire sink block in the browser, hosting on GitHub Pages.

## Why this path

- GNU Radio World is GNU Radio 3.x and its Qt sinks compiled to WebAssembly (repository
  [777arc/gnuradio-world](https://github.com/777arc/gnuradio-world), GPL-3). It has no UDP, ZMQ or
  TCP blocks in the browser; network radios come in only through GRWire, a Rust daemon that talks
  SoapySDR through the `seify` crate. GRWire receives only today.
- A browser cannot receive raw UDP. Sending straight from the S31 to the browser is capped near
  3 MS/s of 8-bit I/Q: TCP send on the S31 measured 48 Mbit/s, CPU bound in the stack (from the
  ngscopeclient work, not yet on a page of these docs). A WebRTC
  data channel needs DTLS, and the S31's AES engine has GCM disabled on v0.0 silicon
  (`soc_caps.h`, IDF-14633), so the encryption would run in software at about the same rate
  (estimate).
- The daemon on the PC removes both limits: the browser sends at whatever rate its flowgraph
  runs, the daemon interpolates to the board's rate and sends UDP over gigabit Ethernet.

## State today

- **Firmware.** `apps/txstream_eth` is on main (protocol in [Transmit stream](txs.md),
  structs in `apps/txstream_eth/src/tx_proto.h`): control on UDP 4992, data on UDP 4994, iq8 or
  w32 samples, 20 or 40 MS/s only, credit flow control, counters for underruns and late or lost
  packets. Measured: 20 MS/s iq8 from a PC on air for 40 s with nothing lost or late (bring-up
  notes, not yet on a page of these docs). RF needs `tx init [MHz]` (default 2437) and
  `tx arm <minutes>` on the board's UART; the band is 2400 to 2483.5 MHz. The arming ends on a
  takeover, a new subscriber, VRTX or 5 s of silence, not at the end of a burst
  ([Transmit stream](txs.md)).
- **Soapy module.** On main, `apps/osmosdr/soapy/SoapyEsdr.cpp` receives only over `vrt=`. A
  transmit link (`VrtTx`, built on `apps/txstream_eth/tools/tx_core.h`, with `writeStream`,
  `readStreamStatus` and loopback cases) exists uncommitted in the `s31-soapytx` worktree, with
  review fixes half done. This plan takes that work over.
- **GRWire.** `Session::open` refuses any direction but `rx`, the `Radio` trait has only `read`,
  and the binary frame header already reserves `kind = 2` for `tx_iq`. `seify` has a
  `TxStreamer` for the Soapy backend (Complex32 only); the device string is
  `driver=soapy,soapy_driver=esdr,vrt=<address>`.
- **Browser.** Transmit exists only for WebUSB HackRF and Pluto sinks (`blocks/src/hackrf_sink.cpp`,
  `runner/src/hackrf_worker.js`).
- **Hosting.** Upstream deploys to Cloudflare Pages and sets COOP and COEP through a `_headers`
  file, which GitHub Pages ignores. The editor uses root-relative paths, which break under a
  `/gnuradio-world/` subpath (a fix sits in the unmerged upstream PR #12).

## Step 1: finish Soapy transmit

1. Commit the worktree state as it is, rebase on main, resolve against the multiapp changes in
   `apps/txstream_eth`.
2. Build the module and run `make -C apps/txstream_eth/tools test` and the transmit cases of
   `apps/osmosdr/soapy/tests/loopback_test.sh` (bursts, short writes, quick restart,
   deactivation). Fix what fails, including the open 40 MS/s retries.
3. Let `findEsdr` also list VRT devices from an environment variable
   (`ESDR_VRT=<address>[,<address>...]`), so `seify` enumeration shows the boards.
4. Review, a board test at 20 MS/s iq8, merge.

## Step 2: arm and retune over the network

- Two new control messages in `tx_proto.h` and [Transmit stream](txs.md): `TXAR` (arm request:
  frequency, minutes, board nonce, HMAC-SHA256) and its answer `TXAA` (result code and the next
  board nonce). `TXAK` stays the answer to TXGO and TXEN, unchanged. Retuning is a new arm with
  another frequency.
- The first nonce: a `TXAR` with an empty HMAC is refused, and its `TXAA` carries a fresh board
  nonce; the PC then sends the real request. Every `TXAA` carries the next nonce, so a recorded
  request cannot be replayed.
- On the board: off by default. `tx netarm on|off` and `tx key set <hex>` on the UART shell, the
  key kept in settings. The HMAC covers the session cookie, the board nonce, the frequency and the
  minutes; the SHA engine computes it.
- The gates of today stay: the band allow-list, a cap on the minutes, disarm on takeover, a new
  subscriber, VRTX or 5 s of silence. New for a network arm: it also ends at the end of a burst
  (TXEN), so every burst needs its own arm.
- Soapy: device argument `tx_keyfile=<path>`; `activateStream` arms, `setFrequency` re-arms;
  refusal codes map to Soapy errors.
- Tests: unit tests in `tests/unit/txs`, and cases in `tx_fake` and the loopback script: bad HMAC,
  replay and an out-of-band frequency are refused.

## Step 3: transmit in GRWire

- Protocol `grwire.v1` (`grwire/src/proto.rs`, `grwire/proto/wire.json`, `docs/grwire.md`):
  `open` with `direction: "tx"`; binary frames from the client with `kind = 2`; new daemon events
  `credit` (the reverse of the client's `flow`), `tx_state` (armed, frequency) and transmit
  counters in `stats`.
- `grwire/src/radio/`: a `TxRadio` trait with `write`; the `seify` backend converts ci8 or ci16
  to Complex32 and writes through `tx_streamer`; the fake radio gets a transmit sink for tests.
- `grwire/src/dsp/`: a polyphase interpolator from the browser's rate to the hardware rate
  (20 MS/s for the S31).
- Safety: transmit is refused unless the daemon runs with `--allow-tx`, and stops on disconnect,
  error or stop. The origin allowlist gains `https://cyber-murmel.github.io`. The device identity
  keeps the `vrt` key so that an enumerated device opens again.

## Step 4: a GRWire sink in the browser

- `blocks/src/grwire_sink.{cpp,hpp}`, `blocks/grc/wasm_grwire_sink.block.yml`, a transmit mode in
  `runner/src/grwire_worker.js` (the worker drains the ring into the WebSocket within the credit)
  and the transmit state with an arm button that asks for confirmation in `editor/src/grwire.ts`.
- As in `hackrf_sink.cpp`: `work()` fills the ring, transmit is forced off on start, stop and
  error, and no example transmits when it loads.

## Step 5: hosting on GitHub Pages

- A base-path fix so that the editor and the runner work under `/gnuradio-world/`.
- Cross-origin isolation (needed for SharedArrayBuffer) from
  [coi-serviceworker](https://github.com/gzuidhof/coi-serviceworker) (MIT), which reloads the
  page once on the first visit. Cross-origin fetches such as the upstream recordings must pass
  COEP, or use its credentialless mode, or go.
- The site is built both locally (about an hour and 10 GB the first time) and in CI. A workflow
  `.github/workflows/pages.yml` runs after the upstream build job and deploys its output with
  `actions/upload-pages-artifact` and `actions/deploy-pages`, within the GitHub Pages limits
  (1 GB per site).
- Afterwards an issue upstream that offers transmit in GRWire, pointing at PR #12.

## Verification

- Soapy and firmware: `make -C apps/txstream_eth/tools test`, the loopback transmit cases,
  `tests/unit/txs` on native_sim, the pre-commit hooks.
- GRWire: `cargo test` with new tests (transmit refused without `--allow-tx`, credit flow,
  interpolator against a reference, the fake transmit sink).
- End to end without a board: a browser flowgraph from a local build → GRWire sink → daemon →
  esdr Soapy → `tx_fake`; sample counts match and no underruns.
- On a board: arm from the browser, a 2.44 GHz tone received by a second board running
  `stream_eth` or seen on the bench scope; the refusals checked (no key, wrong key, replay, out of
  band).
- Pages: the site loads at `cyber-murmel.github.io/gnuradio-world/`, `crossOriginIsolated` is true
  after the first reload, and GRWire connects to a daemon on the local machine in Chrome and
  Firefox.

## Open points

- Chrome's Local Network Access permission for WebSockets from a public page to a daemon on the
  LAN: ship version and behaviour not verified; `ws://127.0.0.1` from an https page is allowed in
  Chrome and Firefox, reportedly not in Safari.
- Whether a WebSocket works under COEP `require-corp` in practice, and whether GRWire has run on
  an isolated deployed site, is not checked.
- The browser's real-time ceiling for a transmit flowgraph in WebAssembly is not measured.

<!-- nav-start -->
Previous: [Network services on the S31 multiapp host](network-services.md) · Up: [Reference](index.md) · Next: [ESP32-S3 registers](registers-esp32s3.md)
<!-- nav-end -->
