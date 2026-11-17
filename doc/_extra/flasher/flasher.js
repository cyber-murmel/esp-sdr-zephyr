// SPDX-License-Identifier: GPL-3.0-or-later
//
// The web flasher: writes one image of the latest release (firmware/firmware.json, copied into
// the site by .github/workflows/docs.yml from the assets nix/release.py made) at 0x0 with
// esptool-js over Web Serial. Each image is a whole flash image (bootloader or MCUboot, the
// app, multiapp's FAT volume), so one write at 0x0 is all.

const ESPTOOL_JS = "https://cdn.jsdelivr.net/npm/esptool-js@0.7.1/bundle.js";
const RELEASES = "https://github.com/cyber-murmel/esp-sdr-zephyr/releases";
// The esp-sdr apps' USB vendor ID (doc/apps/index.md, USB IDs and host access).
const ESP_SDR_VID = 0x2fe3;

// esptool-js's CHIP_NAME per chip of firmware.json.
const CHIPS = {
  esp32: "ESP32",
  esp32s3: "ESP32-S3",
  esp32c5: "ESP32-C5",
  esp32c6: "ESP32-C6",
  esp32p4: "ESP32-P4",
  esp32s31: "ESP32-S31",
  rp2040: "RP2040",
};

const USJ = "<p>Its USB-Serial-JTAG resets the chip into download mode by itself.</p>";
const HANDOVER =
  "<p>A running esp-sdr app owns the USB port: pick its port (\"ESP-SDR ...\") and the flasher " +
  "sends it <code>usj</code>, which hands the port to the USB-Serial-JTAG. Then press " +
  "<b>Connect and flash</b> again and pick the new port.</p>";

// Display names and how each board reaches its ROM bootloader (doc/apps/index.md, Building and
// flashing; doc/dev/chips.md), per chip where a board has two.
const BOARDS = {
  xiao_esp32s3: {
    name: "Seeed XIAO ESP32S3",
    hint: HANDOVER + "<p>A board without an esp-sdr app: plug it in. If nothing answers, hold " +
      "BOOT while plugging it in and choose \"Already in download mode\".</p>",
  },
  communicator_badge: {
    name: "Hackaday Communicator badge (ESP32-S3)",
    hint: HANDOVER + "<p>The badge has no boot button. When no app answers <code>usj</code>, " +
      "short test pad TP1 (IO0) to ground while pressing reset, then choose \"Already in " +
      "download mode\".</p>",
  },
  xiao_esp32c6: {
    name: "Seeed XIAO ESP32C6",
    hint: USJ + "<p>If not, hold BOOT while plugging it in and choose \"Already in download " +
      "mode\".</p>",
  },
  waveshare_esp32c5_zero: {
    name: "Waveshare ESP32-C5-Zero",
    hint: USJ + "<p>If not, hold BOOT while plugging it in and choose \"Already in download " +
      "mode\".</p>",
  },
  why2025_badge: {
    name: "WHY2025 badge (ESP32-P4 and ESP32-C6)",
    chips: {
      esp32p4: "<p>Connect to the P4's USB port; the automatic reset puts it into download mode.</p>",
      esp32c6: "<p>The C6 reaches USB-C port J8 only while the P4 drives its GPIO2 high: run " +
        "<code>esdr.usb_to_c6()</code> in the P4's MicroPython, or a P4 image built with " +
        "<code>CONFIG_WHY2025_BADGE_USB_TO_C6=y</code>, then connect to J8.</p>",
    },
  },
  esp32p4_function_ev_board: {
    name: "ESP32-P4-Function-EV-Board",
    chips: {
      esp32p4: "<p>Connect to the board's USB-UART port; RTS and DTR reset the P4 into download " +
        "mode.</p>",
      esp32c6: "<p>The on-board ESP32-C6 has no USB port of its own: <code>c6_bootstrap</code> on " +
        "the P4 installs C6 images. Download them from here.</p>",
    },
  },
  esp32s31_function_coreboard: {
    name: "ESP32-S31 Function CoreBoard",
    hint: "<p>Connect to the UART bridge's USB port; RTS and DTR reset the chip into download " +
      "mode. If not, hold BOOT while pressing RST and choose \"Already in download mode\".</p>",
  },
  ues31s043h800v480c_u: {
    name: "VIEWE UES31S043H800V480C-U (ESP32-S31, 4.3\" display)",
    hint: "<p>The USB-C socket is a CH340C; RTS and DTR reset the chip into download mode. If " +
      "not, hold BOOT while pressing RST and choose \"Already in download mode\".</p>",
  },
  mch2022: {
    name: "MCH2022 badge (ESP32)",
    hint: "<p>The RP2040 bridges the ESP32's UART to USB and resets it into download mode from " +
      "RTS and DTR, as esptool expects.</p>",
  },
  mch2022_rp2040: {
    name: "MCH2022 badge (RP2040)",
    hint: "<p>The RP2040 takes a <code>.uf2</code>: start its USB bootloader (hold SELECT while " +
      "plugging the badge in) and copy the file onto the RPI-RP2 drive. See " +
      "<a href=\"../apps/mch2022_coproc.html\">mch2022_coproc</a>.</p>",
  },
};

const $ = (id) => document.getElementById(id);
const state = { release: null, board: null, image: null, busy: false };

function store(key, value) {
  try {
    if (value === undefined) return localStorage.getItem(key);
    localStorage.setItem(key, value);
  } catch {
    return null;
  }
  return null;
}

function el(tag, attrs = {}, ...children) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "class") e.className = v;
    else if (k === "html") e.innerHTML = v;
    else e.setAttribute(k, v);
  }
  e.append(...children.filter((c) => c !== null && c !== undefined));
  return e;
}

function size(bytes) {
  return bytes >= 1 << 20 ? `${(bytes / (1 << 20)).toFixed(1)} MiB` : `${Math.ceil(bytes / 1024)} KiB`;
}

function log(line) {
  const pre = $("log");
  pre.textContent += line + "\n";
  pre.scrollTop = pre.scrollHeight;
}

function status(text, kind = "") {
  $("status").textContent = text;
  $("status").className = kind;
}

const flashable = (img) => img && img.chip !== "rp2040" && CHIPS[img.chip] !== undefined;

// Board and image lists

function boardImages(board) {
  return state.release.images.filter((i) => i.board === board);
}

function renderBoards() {
  const boards = [...new Set(state.release.images.map((i) => i.board))];
  boards.sort((a, b) => (BOARDS[a]?.name ?? a).localeCompare(BOARDS[b]?.name ?? b));
  const select = $("board");
  select.replaceChildren(...boards.map((b) => el("option", { value: b }, BOARDS[b]?.name ?? b)));
  const saved = store("esp-sdr-flasher-board");
  select.value = boards.includes(saved) ? saved : boards.includes("xiao_esp32s3") ? "xiao_esp32s3" : boards[0];
  select.addEventListener("change", () => selectBoard(select.value));
  selectBoard(select.value);
}

function selectBoard(board) {
  state.board = board;
  store("esp-sdr-flasher-board", board);
  const info = BOARDS[board] ?? {};
  const hint = $("hint");
  hint.innerHTML = info.hint ?? "";
  for (const [chip, text] of Object.entries(info.chips ?? {})) {
    hint.append(el("h3", {}, CHIPS[chip]), el("div", { html: text }));
  }
  $("reset").value = store(`esp-sdr-flasher-reset-${board}`) ?? "default_reset";
  renderImages();
}

function imageRow(img) {
  const input = el("input", { type: "radio", name: "image", value: img.file });
  input.addEventListener("change", () => selectImage(img));
  const tags = [];
  if (img.rank < 2) tags.push(el("span", { class: "tag" }, "recommended"));
  if (img.inMultiapp) tags.push(el("span", { class: "tag" }, "also in multiapp"));
  if (!flashable(img)) tags.push(el("span", { class: "tag" }, "download only"));
  const download = el("a", { href: `firmware/${img.file}`, download: img.file }, "download");
  return el("label", { class: "image" },
    input,
    el("span", { class: "name" }, img.name, ...tags),
    el("span", { class: "meta" }, `${size(img.size)} · `, download),
    img.description ? el("span", { class: "desc" }, img.description) : null);
}

function renderImages() {
  const images = boardImages(state.board);
  const chips = [...new Set(images.map((i) => i.chip))].sort();
  const box = $("images");
  box.replaceChildren();
  for (const chip of chips) {
    const list = images.filter((i) => i.chip === chip)
      .sort((a, b) => a.rank - b.rank || a.name.localeCompare(b.name));
    if (chips.length > 1) box.append(el("h3", {}, CHIPS[chip] ?? chip));
    const recommended = list.filter((i) => i.rank < 2);
    const others = list.filter((i) => i.rank >= 2);
    box.append(...recommended.map(imageRow));
    if (others.length) {
      box.append(el("h3", {}, recommended.length ? "Other apps" : "Apps"), ...others.map(imageRow));
    }
  }
  const first = images.filter(flashable).sort((a, b) => a.rank - b.rank)[0] ?? images[0];
  const input = box.querySelector(`input[value="${CSS.escape(first.file)}"]`);
  input.checked = true;
  selectImage(first);
}

function selectImage(img) {
  state.image = img;
  updateButton();
}

function updateButton() {
  const ok = "serial" in navigator && flashable(state.image) && !state.busy;
  $("flash").disabled = !ok;
  if (!flashable(state.image) && !state.busy) {
    status("This image is download only.");
  } else if (!state.busy && $("status").textContent === "This image is download only.") {
    status("");
  }
}

// Flashing

async function sha256(data) {
  const digest = await crypto.subtle.digest("SHA-256", data);
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

async function fetchImage(img) {
  const response = await fetch(`firmware/${img.file}`);
  if (!response.ok) throw new Error(`${img.file}: HTTP ${response.status}`);
  const data = new Uint8Array(await response.arrayBuffer());
  if ((await sha256(data)) !== img.sha256) throw new Error(`${img.file}: checksum mismatch`);
  return data;
}

// An esp-sdr app's shell (USB CDC-ACM) hands the port to the USB-Serial-JTAG on "usj".
async function handOver(port) {
  await port.open({ baudRate: 115200 });
  try {
    await port.setSignals({ dataTerminalReady: true });
    const writer = port.writable.getWriter();
    await writer.write(new TextEncoder().encode("\r\nusj\r\n"));
    writer.releaseLock();
    await new Promise((r) => setTimeout(r, 500));
  } catch {
    // The app leaves the bus as it answers.
  }
  try {
    await port.close();
  } catch {
    // Already gone.
  }
}

async function flash() {
  const img = state.image;
  let port;
  try {
    port = await navigator.serial.requestPort();
  } catch {
    return; // Chooser closed.
  }

  state.busy = true;
  updateButton();
  $("log").textContent = "";
  const progress = $("progress");
  progress.hidden = true;
  let transport;
  try {
    if (port.getInfo().usbVendorId === ESP_SDR_VID) {
      status("Asking the app to hand over the USB port...");
      log("esp-sdr app port: sending usj");
      await handOver(port);
      status("Handed over. Press Connect and flash again and pick the new port " +
        "(USB JTAG/serial debug unit).", "ok");
      return;
    }

    status("Downloading the image...");
    const data = await fetchImage(img);
    log(`${img.file}: ${data.length} bytes, sha256 ok`);

    status("Connecting...");
    const { ESPLoader, Transport } = await import(ESPTOOL_JS);
    transport = new Transport(port, false);
    const terminal = { clean() {}, writeLine: log, write: (s) => log(s) };
    const loader = new ESPLoader({
      transport,
      baudrate: Number($("baud").value),
      romBaudrate: 115200,
      terminal,
    });
    // RTS/DTR or the USB-Serial-JTAG reset, or none for a board already in download mode.
    const before = $("reset").value;
    await loader.main(before);
    const found = loader.chip.CHIP_NAME;
    if (found !== CHIPS[img.chip]) {
      throw new Error(`this is an ${found}, the image is for the ${CHIPS[img.chip]}`);
    }

    status("Writing...");
    progress.hidden = false;
    progress.value = 0;
    await loader.writeFlash({
      fileArray: [{ data, address: 0 }],
      flashMode: "keep",
      flashFreq: "keep",
      flashSize: "keep",
      eraseAll: $("erase").checked,
      compress: true,
      reportProgress: (_, written, total) => { progress.value = written / total; },
      calculateMD5Hash: md5,
    });
    progress.value = 1;

    await loader.after("hard_reset");
    status(`Done: ${img.name} written and verified. Press reset if the board does not start.`, "ok");
  } catch (e) {
    log(String(e?.stack ?? e));
    status(`Failed: ${e?.message ?? e}`, "err");
    $("log-box").open = true;
  } finally {
    if (transport) {
      try {
        await transport.disconnect();
      } catch {
        // The port went with the reset.
      }
    }
    state.busy = false;
    updateButton();
  }
}

// MD5 for esptool-js's check of the written flash (WebCrypto has none).
function md5(bytes) {
  const s = [7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21];
  const k = new Uint32Array(64);
  for (let i = 0; i < 64; i++) k[i] = Math.floor(Math.abs(Math.sin(i + 1)) * 2 ** 32);
  const n = bytes.length;
  const padded = new Uint8Array(((n + 8) >> 6) * 64 + 64);
  padded.set(bytes);
  padded[n] = 0x80;
  const view = new DataView(padded.buffer);
  view.setUint32(padded.length - 8, (n * 8) >>> 0, true);
  view.setUint32(padded.length - 4, Math.floor(n / 2 ** 29), true);
  let a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
  const m = new Uint32Array(16);
  for (let off = 0; off < padded.length; off += 64) {
    for (let i = 0; i < 16; i++) m[i] = view.getUint32(off + 4 * i, true);
    let a = a0, b = b0, c = c0, d = d0;
    for (let i = 0; i < 64; i++) {
      let f, g;
      if (i < 16) { f = (b & c) | (~b & d); g = i; }
      else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
      else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
      else { f = c ^ (b | ~d); g = (7 * i) & 15; }
      const t = d;
      d = c;
      c = b;
      const x = (a + f + k[i] + m[g]) >>> 0;
      const r = s[(i >> 4) * 4 + (i & 3)];
      b = (b + ((x << r) | (x >>> (32 - r)))) >>> 0;
      a = t;
    }
    a0 = (a0 + a) >>> 0; b0 = (b0 + b) >>> 0; c0 = (c0 + c) >>> 0; d0 = (d0 + d) >>> 0;
  }
  const out = new DataView(new ArrayBuffer(16));
  [a0, b0, c0, d0].forEach((v, i) => out.setUint32(4 * i, v, true));
  return [...new Uint8Array(out.buffer)].map((x) => x.toString(16).padStart(2, "0")).join("");
}

// Start

async function start() {
  if (!("serial" in navigator)) $("unsupported").hidden = false;
  let release;
  try {
    const response = await fetch("firmware/firmware.json");
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    release = await response.json();
  } catch {
    $("missing").hidden = false;
    return;
  }
  state.release = release;
  const tag = el("a", { href: `${RELEASES}/tag/${release.tag}` }, release.tag);
  $("release").replaceChildren("Images of release ", tag, `: ${release.images.length} images.`);
  for (const id of ["step-board", "step-image", "step-flash"]) $(id).hidden = false;
  $("flash").addEventListener("click", flash);
  $("reset").addEventListener("change", () => {
    store(`esp-sdr-flasher-reset-${state.board}`, $("reset").value);
  });
  renderBoards();
}

start();
