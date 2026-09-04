// Flashing the board from the browser, via Espressif's esptool-js over Web Serial.
//
// This is the same library and the same flash layout the .exe uses, so a board
// flashed here is byte-identical to one flashed with the installer:
//
//   0x0000  bootloader.bin
//   0x8000  partitions.bin
//   0xE000  boot_app0.bin
//   0x10000 firmware.bin
//
// Settings live in a separate NVS partition that writing the app does not
// touch, so flashing preserves an existing board's configuration (INST-12).

import { ESPLoader, Transport } from '../vendor/esptool.js';

// Matches installer.py's flash(): --flash_mode dio --flash_freq 80m
// --flash_size 16MB on an esp32s3 at 921600 baud.
const FLASH_MODE = 'dio';
const FLASH_FREQ = '80m';
const FLASH_SIZE = '16MB';
const BAUD = 921600;

// Load the manifest describing which binaries to write and where.
export async function loadManifest() {
  const r = await fetch('firmware/manifest.json', { cache: 'no-cache' });
  if (!r.ok) throw new Error('No firmware manifest published (HTTP ' + r.status + ')');
  return r.json();
}

async function sha256Hex(buf) {
  const d = await crypto.subtle.digest('SHA-256', buf);
  return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, '0')).join('');
}

// esptool-js takes each image as a binary *string*, not an ArrayBuffer.
function toBinaryString(buf) {
  const bytes = new Uint8Array(buf);
  let s = '';
  // Chunked to stay well clear of the argument-count limit on large images.
  for (let i = 0; i < bytes.length; i += 0x8000) {
    s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
  }
  return s;
}

// Fetch every image and verify it against the manifest before writing a single
// byte. A truncated download that bricked the board would be far worse than a
// refusal, and the check is cheap.
export async function fetchImages(manifest, onProgress) {
  const parts = [];
  for (const part of manifest.parts) {
    onProgress?.(`Downloading ${part.path.split('/').pop()}…`);
    const r = await fetch(part.path, { cache: 'no-cache' });
    if (!r.ok) throw new Error(`Could not download ${part.path} (HTTP ${r.status})`);
    const buf = await r.arrayBuffer();
    if (part.sha256) {
      const got = await sha256Hex(buf);
      if (got !== part.sha256.toLowerCase()) {
        throw new Error(`${part.path} failed its integrity check — refusing to flash.`);
      }
    }
    parts.push({ data: toBinaryString(buf), address: Number(part.offset) });
  }
  return parts;
}

// Which family a chip name belongs to, matching the `chip` field in a manifest.
//
// esptool reports a specific part -- "ESP32-D0WD-V3", "ESP32-S3" -- while a
// manifest names a family. The S3 check has to come first: its name contains
// "ESP32" too, and testing that first would call every board a classic ESP32.
function chipFamily(name) {
  const n = String(name || '').toUpperCase().replace(/[^A-Z0-9]/g, '');
  if (n.includes('ESP32S3')) return 'esp32s3';
  if (n.includes('ESP32S2')) return 'esp32s2';
  if (n.includes('ESP32C3')) return 'esp32c3';
  if (n.includes('ESP32C6')) return 'esp32c6';
  if (n.includes('ESP32H2')) return 'esp32h2';
  if (n.includes('ESP32')) return 'esp32';
  return null;
}

// Flash `port` (a raw SerialPort that must NOT be open). Returns the chip name.
// `onStatus(text)` for prose, `onProgress(fraction)` for the 0..1 bar.
// `expectChip` is the manifest's chip family; writing is refused if the board
// on the other end is not that.
export async function flash(port, parts, onStatus, onProgress, expectChip) {
  const transport = new Transport(port, true);
  // esptool-js expects a terminal-ish sink; route it to the status callback so
  // the user sees the real chip detection and erase messages.
  const terminal = {
    clean() {},
    writeLine(data) { if (data?.trim()) onStatus?.(data.trim()); },
    write(data) { if (data?.trim()) onStatus?.(data.trim()); },
  };

  const loader = new ESPLoader({
    transport,
    baudrate: BAUD,
    romBaudrate: 115200,
    terminal,
    enableTracing: false,
  });

  let chip;
  try {
    chip = await loader.main();
    onStatus?.(`Detected ${chip}.`);

    // Refuse before writing a byte. Firmware for the wrong chip is not merely
    // useless: the bootloader belongs at a different offset (0x0000 on an S3,
    // 0x1000 on a classic ESP32), so a mismatched flash leaves the board
    // unbootable and needing a recovery flash. esptool has already told us what
    // this board is, and the manifest says what the images are for, so there is
    // no reason to find out the hard way.
    const found = chipFamily(chip);
    const want = String(expectChip || '').toLowerCase();
    if (want && found && found !== want) {
      const e = new Error(
        `This firmware is for ${want}, but the board is ${chip} (${found}). `
        + 'Nothing has been written. Choose the right board and try again.');
      e.chipMismatch = true;
      throw e;
    }

    const total = parts.reduce((n, p) => n + p.data.length, 0);
    const written = new Array(parts.length).fill(0);

    await loader.writeFlash({
      fileArray: parts,
      flashSize: FLASH_SIZE,
      flashMode: FLASH_MODE,
      flashFreq: FLASH_FREQ,
      eraseAll: false,
      compress: true,
      reportProgress: (fileIndex, w) => {
        written[fileIndex] = w;
        onProgress?.(written.reduce((a, b) => a + b, 0) / total);
      },
    });
    onStatus?.('Written. Restarting the board…');
    await loader.after();
  } finally {
    // Always let the port go: the provisioning step needs to reopen it, and a
    // held lock here is indistinguishable to the user from a hung install.
    try { await transport.disconnect(); } catch { /* already gone */ }
  }
  return chip;
}
