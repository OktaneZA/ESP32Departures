// The configuration model: the exact set of keys the firmware understands, the
// comma-separated `mode` format, and the colour maths that turns a browser hex
// colour into the RGB565 the display actually draws.
//
// This mirrors installer.py deliberately. The two front-ends (this page and the
// .exe) must produce byte-identical configs, so the key names, the mode format
// and the "omit a key to keep what the board has" rule all live here in the
// same shape they do there.

// Every key the provisioning protocol accepts, in the order the installer sends
// them. See config.cpp's `stage_kv`.
export const KEYS = [
  'ssid', 'pass', 'key', 'dep', 'dest', 'plat', 'tz',
  'bus', 'busline', 'busprov', 'busid', 'buskey', 'busbudget', 'river', 'riverline', 'rivername', 'mode',
  'bstart', 'bend', 'bright', 'refr',
  'colfg', 'coldim', 'colwarn', 'colbg',
  'dwtrain', 'dwbus', 'dwriver', 'dwclock', 'dwwx',
  'wlat', 'wlon', 'wname', 'nmode',
];

// Port of installer.py's SERVICES. Order matters: it is the order the board
// cycles screens in, so the UI must list them the same way.
export const SERVICES = [
  { id: 'train', label: 'Trains', note: 'UK-wide, National Rail' },
  { id: 'bus', label: 'Buses', note: 'London free; elsewhere needs a TransportAPI key' },
  { id: 'river', label: 'River boats', note: 'Uber Boat by Thames Clippers + Woolwich Ferry' },
  { id: 'weather', label: 'Weather', note: 'For wherever you picked above — no extra setup' },
  { id: 'clock', label: 'Big clock', note: 'The time, filling the screen' },
];

// Port of installer.py's parse_mode. Boards flashed before the river screen
// stored a single exclusive word, and those settings survive a firmware update,
// so "" and "both" still mean the trains-and-buses board they meant then.
export function parseMode(raw) {
  const s = (raw || '').trim();
  if (!s || s === 'both') return ['train', 'bus'];
  const known = SERVICES.map((x) => x.id);
  return s.split(',').map((t) => t.trim()).filter((t) => known.includes(t));
}

// Keep the canonical service order rather than the order they were ticked, so
// the stored mode matches the rotation the firmware will actually run.
export function buildMode(selected) {
  return SERVICES.filter((s) => selected.includes(s.id)).map((s) => s.id).join(',');
}

// --- Colour -----------------------------------------------------------------
// The panel is 16-bit: 5 bits red, 6 green, 5 blue. Converting there and back
// is not lossless, so the preview deliberately shows the round-tripped colour —
// otherwise the page would promise a shade the board cannot draw.

export function hexToRgb565(hex) {
  const m = /^#?([0-9a-f]{6})$/i.exec(hex || '');
  if (!m) return -1;
  const v = parseInt(m[1], 16);
  const r = (v >> 16) & 0xff, g = (v >> 8) & 0xff, b = v & 0xff;
  return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

export function rgb565ToHex(v) {
  if (typeof v !== 'number' || v < 0 || v > 0xffff) return null;
  // Replicate the high bits into the low ones so full-scale stays full-scale
  // (0x1f -> 0xff, not 0xf8), which is what the panel does.
  const r5 = (v >> 11) & 0x1f, g6 = (v >> 5) & 0x3f, b5 = v & 0x1f;
  const r = (r5 << 3) | (r5 >> 2);
  const g = (g6 << 2) | (g6 >> 4);
  const b = (b5 << 3) | (b5 >> 2);
  return '#' + [r, g, b].map((c) => c.toString(16).padStart(2, '0')).join('');
}

// What the board will really show for a colour the user picked.
//
// Falls back to black rather than to the input. The result is interpolated into
// a style attribute, and returning whatever was passed in would make a bad
// colour a CSS injection -- unreachable today, since the only source is an
// <input type=color>, but one config-import away from mattering.
export function quantise(hex) {
  return rgb565ToHex(hexToRgb565(hex)) || '#000000';
}

// The firmware's compiled-in defaults, as hex. Matches display.cpp's AMBER,
// DIM, RED and BLACK, so "Classic amber" reproduces an unconfigured board.
export const THEMES = {
  amber: { name: 'Classic amber', fg: '#ffa400', dim: '#846100', warn: '#ff0000', bg: '#000000' },
  white: { name: 'Platform white', fg: '#ffffff', dim: '#8c8c8c', warn: '#ff4949', bg: '#000000' },
  green: { name: 'Phosphor green', fg: '#31ff31', dim: '#107b10', warn: '#ffb500', bg: '#000000' },
  contrast: { name: 'High contrast', fg: '#ffffff', dim: '#cecece', warn: '#ff0000', bg: '#000000' },
};

// --- Defaults ---------------------------------------------------------------
// Screen hours follow INST-26: a board that has never been configured turns
// itself off overnight rather than burning the panel all night.
export function defaultConfig() {
  let tz = '';
  try {
    tz = Intl.DateTimeFormat().resolvedOptions().timeZone || '';
  } catch { /* older browser: the firmware falls back to UK time */ }
  return {
    ssid: '', pass: '', key: '', dep: '', dest: '', plat: '',
    tz: tz === 'Europe/London' ? 'GMT0BST,M3.5.0/1,M10.5.0' : '',
    bus: '', busline: '', river: '', riverline: '', rivername: '',
    // Which bus feed, and the credentials the national one needs. TfL is the
    // default because it is the one that needs no account at all.
    busprov: 'tfl', busid: '', buskey: '',
    // Requests per day the bus feed may spend. 0 means unmetered, which is what
    // TfL's keyless feed is; a metered provider replaces this with its allowance
    // and the board paces itself to fit.
    busbudget: 0,
    services: ['train'],
    onHour: 6, offHour: 22,      // converted to bstart/bend on the way out
    bright: 180, refr: 60,
    theme: 'amber',
    colours: { ...THEMES.amber },
    dwtrain: 30, dwbus: 15, dwriver: 15, dwclock: 10, dwwx: 15,
    // Weather position is filled in from whichever stop the user picks, so it
    // is never asked for directly.
    wxLat: null, wxLon: null, wxName: '',
    nightClock: true,      // show a dimmed clock during blank hours
  };
}

// --- Export -----------------------------------------------------------------
// Turn the UI state into the flat key/value set the firmware and the .exe both
// consume. Screen hours are inverted here: the user thinks "on at 6, off at 22"
// (INST-25) but the firmware stores the blank window as bstart/bend.
export function toDeviceConfig(ui) {
  const services = ui.services.slice();

  // A service selected but left without a stop or pier would put a screen in
  // the rotation with nothing behind it (INST-27), so drop it here too.
  const pruned = services.filter((s) => {
    if (s === 'bus') return !!ui.bus;
    if (s === 'river') return !!ui.river;
    if (s === 'train') return !!ui.dep;
    // Weather needs somewhere to be the weather *for*, which comes from the
    // stop the user already chose rather than a question of its own.
    if (s === 'weather') return ui.wxLat !== null && ui.wxLon !== null;
    return true;      // the clock needs nothing
  });

  const blanking = ui.onHour >= 0 && ui.offHour >= 0 && ui.onHour !== ui.offHour;
  const c = ui.colours;

  return {
    ssid: ui.ssid,
    pass: ui.pass,
    key: pruned.includes('train') ? ui.key : '',
    dep: pruned.includes('train') ? ui.dep.toUpperCase() : '',
    dest: pruned.includes('train') ? (ui.dest || '').toUpperCase() : '',
    plat: pruned.includes('train') ? (ui.plat || '') : '',
    tz: ui.tz || '',
    bus: pruned.includes('bus') ? ui.bus : '',
    busline: pruned.includes('bus') ? (ui.busline || '') : '',
    busprov: pruned.includes('bus') ? (ui.busprov || 'tfl') : 'tfl',
    // Credentials and an allowance only mean anything to the national feed, so
    // a London board sends them empty rather than leaving a stale key on NVS.
    busid: pruned.includes('bus') && ui.busprov === 'national' ? ui.busid : '',
    buskey: pruned.includes('bus') && ui.busprov === 'national' ? ui.buskey : '',
    busbudget: pruned.includes('bus') && ui.busprov === 'national' ? (ui.busbudget || 30) : 0,
    river: pruned.includes('river') ? ui.river : '',
    riverline: pruned.includes('river') ? (ui.riverline || '') : '',
    rivername: pruned.includes('river') ? (ui.rivername || '') : '',
    mode: buildMode(pruned),
    bstart: blanking ? ui.offHour : -1,
    bend: blanking ? ui.onHour : -1,
    bright: ui.bright,
    refr: ui.refr,
    colfg: hexToRgb565(c.fg),
    coldim: hexToRgb565(c.dim),
    colwarn: hexToRgb565(c.warn),
    colbg: hexToRgb565(c.bg),
    dwtrain: ui.dwtrain,
    dwbus: ui.dwbus,
    dwriver: ui.dwriver,
    dwclock: ui.dwclock,
    dwwx: ui.dwwx,
    // Degrees x100000: NVS has no float type, and this keeps ~1m of precision.
    wlat: pruned.includes('weather') ? Math.round(ui.wxLat * 100000) : -2147483648,
    wlon: pruned.includes('weather') ? Math.round(ui.wxLon * 100000) : -2147483648,
    wname: pruned.includes('weather') ? (ui.wxName || '') : '',
    nmode: ui.nightClock ? 1 : 0,
  };
}

// The downloadable file. `port` and `flash` are the .exe's own keys, not the
// firmware's — run_auto() reads them to pick a device and decide whether to
// flash before provisioning.
export function toJsonFile(ui) {
  return JSON.stringify({ flash: true, ...toDeviceConfig(ui) }, null, 2);
}
