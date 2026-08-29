// Talking to the board over Web Serial.
//
// The provisioning protocol is the same newline-terminated one the .exe uses
// (src/config.cpp):
//
//   PING              -> PONG <product name>
//   CFG <key>=<value> -> ACK <key>
//   COMMIT            -> SAVED, then the board reboots
//   GET               -> key=value lines, then END
//
// Two rules carried over from installer.py, both load-bearing:
//
//   * Match the bare `PONG` token, never the product name after it (PROV-01).
//     That is what lets a renamed board still be recognised, and why the rename
//     to Departure Buddy did not break older installers.
//   * A COMMIT stages on top of the board's current config, so *omitting* a key
//     preserves its stored value (PROV-09). Send only what the user set.

export function isSupported() {
  return typeof navigator !== 'undefined' && 'serial' in navigator;
}

const ENC = new TextEncoder();
const DEC = new TextDecoder();

export class Board {
  constructor(port) {
    this.port = port;
    this.reader = null;
    this.writer = null;
    this._buf = '';
  }

  // Ask the user to pick a port. Must be called from a user gesture.
  static async request() {
    if (!isSupported()) throw new Error('This browser has no Web Serial support.');
    const port = await navigator.serial.requestPort({});
    return new Board(port);
  }

  async open(baudRate = 115200) {
    // The board is native USB CDC, so the baud rate is nominal — but the API
    // requires one, and 115200 matches the firmware's Serial.begin.
    await this.port.open({ baudRate });
    this.reader = this.port.readable.getReader();
    this.writer = this.port.writable.getWriter();
    this._buf = '';
  }

  async close() {
    try { if (this.reader) { await this.reader.cancel(); this.reader.releaseLock(); } } catch { }
    try { if (this.writer) { this.writer.releaseLock(); } } catch { }
    try { await this.port.close(); } catch { }
    this.reader = this.writer = null;
  }

  async write(line) {
    await this.writer.write(ENC.encode(line + '\n'));
  }

  // Read whole lines until `predicate` accepts one, or the deadline passes.
  // Returns every line seen, so callers can inspect the whole exchange.
  async readUntil(predicate, timeoutMs = 3000) {
    const lines = [];
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      const remaining = deadline - Date.now();
      let chunk;
      try {
        chunk = await withTimeout(this.reader.read(), remaining);
      } catch {
        break;                       // timed out waiting for more bytes
      }
      if (!chunk || chunk.done) break;
      this._buf += DEC.decode(chunk.value, { stream: true });
      let nl;
      while ((nl = this._buf.indexOf('\n')) >= 0) {
        const line = this._buf.slice(0, nl).replace(/\r$/, '');
        this._buf = this._buf.slice(nl + 1);
        if (line) lines.push(line);
        if (line && predicate(line)) return lines;
      }
    }
    return lines;
  }

  // Retry PING while the board finishes booting. Resolves to the banner it
  // answered with, or null.
  async handshake(timeoutMs = 8000) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      await this.write('PING');
      const lines = await this.readUntil((l) => l.includes('PONG'), 600);
      const pong = lines.find((l) => l.includes('PONG'));
      if (pong) return pong.trim();
    }
    return null;
  }

  // Current settings, as the board reports them. Secrets never come back: the
  // API key is not reported at all and the WiFi password only as `passlen`.
  async readConfig(timeoutMs = 6000) {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      await this.write('GET');
      const lines = await this.readUntil((l) => l === 'END', 900);
      if (lines.includes('END')) {
        const out = {};
        for (const l of lines) {
          const i = l.indexOf('=');
          if (i > 0) out[l.slice(0, i).trim()] = l.slice(i + 1).trim();
        }
        return out;
      }
    }
    return null;
  }

  // Send the config and commit it. `onProgress(done, total, key)` is optional.
  // Returns true once the board answers SAVED.
  async provision(cfg, keys, onProgress) {
    const entries = keys.filter((k) => cfg[k] !== undefined && cfg[k] !== null);
    let done = 0;
    for (const k of entries) {
      await this.write(`CFG ${k}=${cfg[k]}`);
      await this.readUntil((l) => l.startsWith('ACK ') || l.startsWith('ERR '), 500);
      onProgress?.(++done, entries.length, k);
    }
    await this.write('COMMIT');
    const lines = await this.readUntil((l) => l.includes('SAVED'), 5000);
    return lines.some((l) => l.includes('SAVED'));
  }

  // Networks the board's own radio can see. The ESP32-S3 has no 5 GHz radio, so
  // a network missing here but visible on a phone is the usual explanation for
  // a board that will not connect (PROV-08).
  async scan(timeoutMs = 15000) {
    await this.write('SCAN');
    const lines = await this.readUntil((l) => l === 'END', timeoutMs);
    return lines
      .filter((l) => l.includes('|rssi='))
      .map((l) => {
        const [ssid, ...rest] = l.split('|');
        const f = Object.fromEntries(rest.map((p) => p.split('=')));
        return { ssid, rssi: Number(f.rssi), channel: Number(f.ch), auth: f.auth };
      });
  }
}

function withTimeout(promise, ms) {
  return new Promise((resolve, reject) => {
    const t = setTimeout(() => reject(new Error('timeout')), ms);
    promise.then((v) => { clearTimeout(t); resolve(v); },
                 (e) => { clearTimeout(t); reject(e); });
  });
}

// After a COMMIT the board reboots, and because it is native USB CDC the port
// disappears and comes back as a new device. Any handle held across that is
// stale. Wait for the port to be usable again, reopening the same one if the
// browser still offers it; the caller falls back to asking the user to re-pick.
export async function waitForReconnect(board, timeoutMs = 15000) {
  await board.close();
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    await sleep(500);
    try {
      await board.open();
      return true;
    } catch {
      /* still enumerating */
    }
  }
  return false;
}

export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
