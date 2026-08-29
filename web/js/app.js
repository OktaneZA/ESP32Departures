// UI wiring for the configurator: form state, the pickers, the board preview,
// and the two ways of getting settings onto a device.

import * as api from './api.js';
import * as cfg from './config.js';
import { Board, isSupported, waitForReconnect, sleep } from './serial.js';

const $ = (id) => document.getElementById(id);
const ui = cfg.defaultConfig();

let previewScreen = 'train';
let pierList = [];

// ─────────────────────────────── basics ──────────────────────────────────
function initHours() {
  for (const sel of [$('onHour'), $('offHour')]) {
    sel.append(new Option('Never blank the screen', '-1'));
    for (let h = 0; h < 24; h++) {
      sel.append(new Option(String(h).padStart(2, '0') + ':00', String(h)));
    }
  }
  $('onHour').value = String(ui.onHour);
  $('offHour').value = String(ui.offHour);
  const sync = () => {
    ui.onHour = Number($('onHour').value);
    ui.offHour = Number($('offHour').value);
    // -1 in either box means "on all the time"; keep the pair consistent so the
    // exported bstart/bend cannot describe half a blank window.
    if (ui.onHour < 0 || ui.offHour < 0) {
      ui.onHour = ui.offHour = -1;
      $('onHour').value = $('offHour').value = '-1';
      $('hoursSummary').textContent = 'The screen stays on all the time.';
    } else if (ui.onHour === ui.offHour) {
      $('hoursSummary').textContent = 'ON and OFF can’t be the same hour — pick two different times.';
    } else {
      const lit = (ui.offHour - ui.onHour + 24) % 24;
      const warn = lit < 12
        ? '  That’s less than half the day — check they’re the right way round.'
        : '';
      $('hoursSummary').textContent =
        `On ${pad(ui.onHour)}:00–${pad(ui.offHour)}:00 (${lit}h), off the other ${24 - lit}h.${warn}`;
    }
    render();
  };
  $('onHour').onchange = $('offHour').onchange = sync;
  sync();
}
const pad = (n) => String(n).padStart(2, '0');

function initSliders() {
  const wire = (id, out, fmt, key) => {
    const el = $(id);
    el.value = ui[key];
    const sync = () => { ui[key] = Number(el.value); $(out).textContent = fmt(ui[key]); render(); };
    el.oninput = sync;
    sync();
  };
  wire('bright', 'brightOut', (v) => `${Math.round((v / 255) * 100)}%`, 'bright');
  wire('refr', 'refrOut', (v) => `${v}s`, 'refr');
}

// ────────────────────────────── services ─────────────────────────────────
function initServices() {
  const wrap = $('services');
  const sync = () => {
    // Read the checkboxes rather than tracking them separately. On reload the
    // browser restores tick state on its own, and anything that kept its own
    // copy would quietly disagree with what the user can see — exporting a
    // config for services the page is no longer showing as selected.
    ui.services = [...wrap.querySelectorAll('input:checked')].map((i) => i.value);
    syncServiceVisibility();
    if (!ui.services.includes(previewScreen)) previewScreen = ui.services[0] || 'train';
    render();
  };
  for (const s of cfg.SERVICES) {
    const el = document.createElement('label');
    el.className = 'svc-toggle';
    // autocomplete=off asks the browser not to restore these across a reload;
    // sync() below makes us correct even where it ignores that.
    el.innerHTML = `<input type="checkbox" autocomplete="off" value="${s.id}">
      <span><strong>${s.label}</strong><span>${s.note}</span></span>`;
    const box = el.querySelector('input');
    box.checked = ui.services.includes(s.id);
    box.onchange = sync;
    wrap.append(el);
  }
  sync();
}

function syncServiceVisibility() {
  for (const s of cfg.SERVICES) {
    const on = ui.services.includes(s.id);
    $('svc-' + s.id).hidden = !on;
    $('services').querySelectorAll('.svc-toggle')[cfg.SERVICES.indexOf(s)]
      .classList.toggle('on', on);
  }
  if (ui.services.includes('river') && !pierList.length) loadPiers();
  buildDwellSliders();
  buildPreviewTabs();
}

// ─────────────────────────────── trains ──────────────────────────────────
function initTrains() {
  const box = $('stationSearch');
  box.oninput = debounce(async () => {
    const q = box.value.trim();
    const out = $('stationResults');
    if (q.length < 2) { out.innerHTML = ''; return; }
    const hits = await api.searchStations(q);
    out.innerHTML = '';
    if (!hits.length) {
      out.innerHTML = '<p class="hint">No station matches that. Try fewer letters.</p>';
      return;
    }
    for (const s of hits) {
      const b = document.createElement('button');
      b.type = 'button';
      b.className = 'result';
      b.innerHTML = `${escapeHtml(s.n)} <span class="meta">${s.c}</span>`;
      b.onclick = () => {
        ui.dep = s.c;
        ui.depName = s.n;
        $('stationChosen').hidden = false;
        $('stationChosen').textContent = `Showing departures from ${s.n} (${s.c}).`;
        out.innerHTML = '';
        box.value = s.n;
        render();
      };
      out.append(b);
    }
  }, 180);

  $('apikey').oninput = () => { ui.key = $('apikey').value.trim(); render(); };
  $('dest').oninput = () => { ui.dest = $('dest').value.trim(); };
  $('plat').oninput = () => { ui.plat = $('plat').value.trim(); };

  $('checkKey').onclick = async () => {
    const st = $('keyStatus');
    if (!ui.key || !ui.dep) {
      st.className = 'status bad';
      st.textContent = 'Pick a station and paste your key first.';
      return;
    }
    st.className = 'status';
    st.textContent = 'checking…';
    const r = await api.validateStation(ui.key, ui.dep);
    const msg = {
      ok: ['ok', `Working — ${ui.dep} accepted.`],
      bad_station: ['bad', `The key works, but ${ui.dep} was rejected.`],
      bad_key: ['bad', 'That key was rejected. Check you copied the whole thing.'],
      net: ['', 'Couldn’t reach the API just now — that doesn’t mean the key is wrong.'],
    }[r];
    st.className = 'status ' + msg[0];
    st.textContent = msg[1];
  };
}

// ──────────────────────────────── buses ──────────────────────────────────
function initBuses() {
  const box = $('busSearch');
  const go = async () => {
    const q = box.value.trim();
    const out = $('busResults');
    if (!q) return;
    out.innerHTML = '<p class="hint">Searching…</p>';

    // A bare stop code needs no search — check it and take it as given.
    if (/^\d+$/.test(q)) {
      const { status } = await api.busArrivals(q);
      if (status === 'bad_stop') {
        out.innerHTML = `<p class="hint">TfL doesn’t know stop code ${escapeHtml(q)}.</p>`;
        return;
      }
      out.innerHTML = '';
      chooseStop({ code: q, name: `Stop ${q}`, towards: '', indicator: '' });
      return;
    }

    const { stops, label } = await api.findStops(q);
    if (!stops.length) {
      out.innerHTML = `<p class="hint">No London bus stops found for “${escapeHtml(label)}”.
        Try a postcode, or a nearby landmark.</p>`;
      return;
    }
    out.innerHTML = '';
    for (const s of stops.slice(0, 12)) {
      const b = document.createElement('button');
      b.type = 'button';
      b.className = 'result';
      const ind = s.indicator ? ` (${escapeHtml(s.indicator)})` : '';
      const towards = s.towards ? `towards ${escapeHtml(s.towards)}` : 'hail &amp; ride';
      b.innerHTML = `${escapeHtml(s.name)}${ind}
        <span class="meta">${Math.round(s.distance)}m · ${towards}</span>`;
      b.onclick = () => { out.innerHTML = ''; chooseStop(s); };
      out.append(b);
    }
    if (stops.length > 12) {
      out.insertAdjacentHTML('beforeend',
        `<p class="hint">…${stops.length - 12} more. Search somewhere more specific to narrow it down.</p>`);
    }
  };
  $('busGo').onclick = go;
  box.onkeydown = (e) => { if (e.key === 'Enter') { e.preventDefault(); go(); } };
  $('busline').oninput = () => { ui.busline = $('busline').value.trim(); render(); };
}

async function chooseStop(stop) {
  ui.bus = stop.code;
  ui.busName = stop.name;
  const el = $('busChosen');
  el.hidden = false;
  el.textContent = `Showing ${stop.name} (${stop.code}). Checking what’s due…`;
  render();
  const { status, rows } = await api.busArrivals(stop.code, ui.busline);
  el.textContent = status !== 'ok'
    ? `Showing ${stop.name} (${stop.code}).`
    : rows.length
      ? `Showing ${stop.name} (${stop.code}). Next: `
        + rows.slice(0, 3).map((r) => `${r.line} to ${r.dest} ${r.mins < 1 ? 'due' : r.mins + ' min'}`).join(', ')
      : `Showing ${stop.name} (${stop.code}) — nothing due right now, but the stop is valid.`;
  ui.busPreview = rows.slice(0, 3);
  render();
}

// ──────────────────────────────── river ──────────────────────────────────
async function loadPiers() {
  const sel = $('pierSelect');
  pierList = await api.riverPiers();
  sel.innerHTML = '<option value="">Choose a pier…</option>';
  for (const p of pierList) {
    sel.append(new Option(`${p.name}  (${p.lines.join(', ')})`, p.id));
  }
  sel.onchange = async () => {
    const p = pierList.find((x) => x.id === sel.value);
    const el = $('pierChosen');
    if (!p) { ui.river = ui.rivername = ''; el.hidden = true; render(); return; }
    ui.river = p.id;
    ui.rivername = p.name;
    el.hidden = false;
    el.textContent = `Showing ${p.name}. Checking what’s sailing…`;
    render();
    const { status, rows } = await api.riverArrivals(p.id, ui.riverline);
    el.textContent = status !== 'ok'
      ? `Showing ${p.name}.`
      : rows.length
        ? `Showing ${p.name}. Next: `
          + rows.slice(0, 3).map((r) => `${r.line} to ${r.dest} ${r.mins < 1 ? 'due' : r.mins + ' min'}`).join(', ')
        : `Showing ${p.name} — nothing due right now, but the pier is valid.`;
    ui.riverPreview = rows.slice(0, 3);
    render();
  };
  $('riverline').oninput = () => { ui.riverline = $('riverline').value.trim().toUpperCase(); render(); };
}

// ───────────────────────────── appearance ────────────────────────────────
function initThemes() {
  const wrap = $('themes');
  for (const [id, t] of Object.entries(cfg.THEMES)) {
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'theme' + (id === ui.theme ? ' on' : '');
    b.dataset.theme = id;
    b.innerHTML = `<div class="swatch" style="background:${t.bg};color:${t.fg}">08:42</div>
                   <div class="tname">${t.name}</div>`;
    b.onclick = () => {
      ui.theme = id;
      ui.colours = { ...t };
      syncColourInputs();
      wrap.querySelectorAll('.theme').forEach((x) => x.classList.toggle('on', x.dataset.theme === id));
      render();
    };
    wrap.append(b);
  }
  for (const k of ['fg', 'dim', 'warn', 'bg']) {
    $('col' + k).oninput = () => {
      ui.colours[k] = $('col' + k).value;
      ui.theme = 'custom';
      wrap.querySelectorAll('.theme').forEach((x) => x.classList.remove('on'));
      render();
    };
  }
  syncColourInputs();
}

function syncColourInputs() {
  for (const k of ['fg', 'dim', 'warn', 'bg']) $('col' + k).value = ui.colours[k];
}

// Only the enabled services get a dwell slider — a slider for a screen that
// will never appear is just noise.
function buildDwellSliders() {
  const wrap = $('dwells');
  wrap.innerHTML = '';
  const keys = { train: 'dwtrain', bus: 'dwbus', river: 'dwriver' };
  const shown = cfg.SERVICES.filter((s) => ui.services.includes(s.id));
  $('dwellWrap').hidden = shown.length < 2;   // nothing rotates with one screen
  for (const s of shown) {
    const key = keys[s.id];
    const d = document.createElement('div');
    d.className = 'field';
    d.innerHTML = `<label for="${key}">${s.label} <output id="${key}Out"></output></label>
                   <input id="${key}" type="range" min="5" max="120" step="5">`;
    wrap.append(d);
    const el = $(key);
    el.value = ui[key];
    const sync = () => { ui[key] = Number(el.value); $(key + 'Out').textContent = ui[key] + 's'; };
    el.oninput = sync;
    sync();
  }
}

// ─────────────────────────────── preview ─────────────────────────────────
function buildPreviewTabs() {
  const wrap = $('previewTabs');
  wrap.innerHTML = '';
  const shown = cfg.SERVICES.filter((s) => ui.services.includes(s.id));
  if (shown.length < 2) return;
  for (const s of shown) {
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'ptab' + (s.id === previewScreen ? ' on' : '');
    b.textContent = s.label;
    b.onclick = () => { previewScreen = s.id; render(); };
    wrap.append(b);
  }
}

// The panel is 16-bit, so show the round-tripped colour: promising a shade the
// board cannot draw would make the preview a lie.
function render() {
  const c = ui.colours;
  const fg = cfg.quantise(c.fg), dim = cfg.quantise(c.dim);
  const warn = cfg.quantise(c.warn), bg = cfg.quantise(c.bg);

  let tag = 'TRAIN', name = ui.depName || 'Your station', rows, empty = null;
  if (previewScreen === 'bus') {
    tag = ui.busline ? `BUS ${ui.busline}` : 'BUS';
    name = ui.busName || 'Your stop';
    rows = (ui.busPreview || []).map((r) => ({ a: clock(r.mins), b: r.line, c: r.dest, d: r.mins < 1 ? 'Due' : r.mins + ' min' }));
    if (!rows.length) empty = 'No buses due';
  } else if (previewScreen === 'river') {
    tag = ui.riverline ? `RIVER ${ui.riverline}` : 'RIVER';
    name = ui.rivername || 'Your pier';
    rows = (ui.riverPreview || []).map((r) => ({ a: clock(r.mins), b: r.line, c: r.dest, d: r.mins < 1 ? 'Due' : r.mins + ' min' }));
    if (!rows.length) empty = 'No boats due';
  } else {
    rows = [
      { a: clock(5), c: 'London Waterloo', d: 'On time', p: '1' },
      { a: clock(15), c: 'Guildford', d: 'Exp 09:12', p: '2', late: true },
      { a: clock(25), c: 'London Waterloo', d: 'On time', p: '1' },
    ];
    if (!ui.dep) empty = null;   // sample data is more useful than an empty board
  }

  const rowHtml = rows.slice(0, 3).map((r) => `
    <div class="b-row">
      <span class="b-time">${r.a}</span>
      ${r.b ? `<span>${escapeHtml(r.b)}</span>` : ''}
      <span class="b-dest">${escapeHtml(r.c)}</span>
      <span class="b-right" style="color:${r.late ? warn : fg}">${escapeHtml(r.d)}${r.p ? '  P' + r.p : ''}</span>
    </div>`).join('');

  $('preview').innerHTML = `
    <div class="board" style="background:${bg};color:${fg}">
      <div class="b-head">
        <span class="b-tag" style="color:${dim}">${escapeHtml(tag)}</span>
        <span class="b-name">${escapeHtml(name)}</span>
      </div>
      ${empty
        ? `<div class="b-empty" style="color:${dim}">${escapeHtml(empty)}</div>`
        : `<div class="b-rows">${rowHtml}</div>`}
      <div class="b-clock">${nowClock()}</div>
    </div>`;

  document.querySelectorAll('.ptab').forEach((b) =>
    b.classList.toggle('on', b.textContent === cfg.SERVICES.find((s) => s.id === previewScreen)?.label));

  showProblems();
}

function clock(minsAhead) {
  const d = new Date(Date.now() + minsAhead * 60000);
  return pad(d.getHours()) + ':' + pad(d.getMinutes());
}
function nowClock() {
  const d = new Date();
  return `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
}

// ───────────────────────────── validation ────────────────────────────────
function problems() {
  const out = [];
  if (!ui.ssid) out.push('A WiFi network name — without it the board can’t fetch anything.');
  if (!ui.services.length) out.push('At least one thing to show.');
  if (ui.services.includes('train') && !ui.dep) out.push('A station, or turn trains off.');
  if (ui.services.includes('train') && !ui.key) out.push('A National Rail API key, or turn trains off.');
  if (ui.services.includes('bus') && !ui.bus) out.push('A bus stop, or turn buses off.');
  if (ui.services.includes('river') && !ui.river) out.push('A pier, or turn river boats off.');
  return out;
}

function showProblems() {
  const list = problems();
  const el = $('problems');
  el.hidden = !list.length;
  if (list.length) {
    el.innerHTML = 'Before you can set the board up, it still needs:<ul>'
      + list.map((p) => `<li>${escapeHtml(p)}</li>`).join('') + '</ul>';
  }
  $('btnConnect').disabled = !!list.length;
  $('btnDownload').disabled = !!list.length;
}

// ─────────────────────────────── install ─────────────────────────────────
function initInstall() {
  const supported = isSupported();
  $('webserial').hidden = !supported;
  $('nowebserial').hidden = supported;

  $('btnDownload').onclick = () => {
    const blob = new Blob([cfg.toJsonFile(ui)], { type: 'application/json' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'departure-buddy.json';
    a.click();
    URL.revokeObjectURL(a.href);
  };

  $('btnConnect').onclick = connectAndConfigure;
}

function logLine(msg, cls = '') {
  const el = $('serialLog');
  el.hidden = false;
  el.insertAdjacentHTML('beforeend', `<div class="${cls}">${escapeHtml(msg)}</div>`);
  el.scrollTop = el.scrollHeight;
}

async function connectAndConfigure() {
  const btn = $('btnConnect');
  btn.disabled = true;
  $('serialLog').innerHTML = '';
  let board;
  try {
    board = await Board.request();
  } catch {
    logLine('No device selected.', 'bad');
    btn.disabled = false;
    return;
  }

  try {
    logLine('Opening the port…');
    await board.open();

    logLine('Saying hello…');
    const banner = await board.handshake();
    if (!banner) {
      logLine('The board didn’t answer. Is it running Departure Buddy firmware?', 'bad');
      logLine('A brand-new board needs flashing first — use the installer for now.');
      await board.close();
      btn.disabled = false;
      return;
    }
    logLine(`Found: ${banner}`, 'ok');

    const device = cfg.toDeviceConfig(ui);
    logLine(`Sending ${Object.keys(device).length} settings…`);
    const saved = await board.provision(device, cfg.KEYS, (done, total, key) => {
      if (done === total) logLine(`Sent all ${total} settings.`);
      else if (key === 'mode') logLine(`  showing: ${device.mode}`);
    });

    if (!saved) {
      logLine('The board didn’t confirm the save. Try again, or replug it.', 'bad');
      await board.close();
      btn.disabled = false;
      return;
    }
    logLine('Saved. The board is rebooting…', 'ok');

    // The board is native USB CDC: rebooting drops the port off the bus and it
    // comes back as a new device, so the handle we hold is now stale.
    const back = await waitForReconnect(board);
    if (!back) {
      logLine('Done — the board is restarting with your settings.', 'ok');
      logLine('(Its USB port reconnected as a new device, so this page let go of it.)');
      btn.disabled = false;
      return;
    }
    await sleep(1200);
    const check = await board.readConfig();
    await board.close();
    if (check) {
      logLine(`Confirmed: showing ${check.mode || '?'}, WiFi ${check.ssid || '?'}, `
        + `WiFi ${check.wifi === 'up' ? 'connected' : 'connecting…'}`, 'ok');
    }
    logLine('All done. Enjoy your board.', 'ok');
  } catch (e) {
    logLine('Something went wrong: ' + (e?.message || e), 'bad');
    try { await board.close(); } catch { }
  }
  btn.disabled = false;
  showProblems();
}

// ──────────────────────────────── utils ──────────────────────────────────
function escapeHtml(s) {
  return String(s ?? '').replace(/[&<>"']/g,
    (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

function debounce(fn, ms) {
  let t;
  return (...a) => { clearTimeout(t); t = setTimeout(() => fn(...a), ms); };
}

// ──────────────────────────────── boot ───────────────────────────────────
$('ssid').oninput = () => { ui.ssid = $('ssid').value; showProblems(); };
$('pass').oninput = () => { ui.pass = $('pass').value; };

initHours();
initSliders();
initServices();
initTrains();
initBuses();
initThemes();
initInstall();
render();
setInterval(() => { if (!document.hidden) render(); }, 1000);
