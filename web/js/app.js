// UI wiring for the configurator: form state, the pickers, the board preview,
// and the two ways of getting settings onto a device.

import * as api from './api.js';
import * as cfg from './config.js';
import { Board, isSupported, waitForReconnect, sleep } from './serial.js';
import * as flasher from './flash.js';

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
      $('nightField').hidden = true;      // nothing blanks, so nothing to dim
    } else if (ui.onHour === ui.offHour) {
      $('hoursSummary').textContent = 'ON and OFF can’t be the same hour — pick two different times.';
    } else {
      const lit = (ui.offHour - ui.onHour + 24) % 24;
      const warn = lit < 12
        ? '  That’s less than half the day — check they’re the right way round.'
        : '';
      $('hoursSummary').textContent =
        `On ${pad(ui.onHour)}:00–${pad(ui.offHour)}:00 (${lit}h), off the other ${24 - lit}h.${warn}`;
      $('nightField').hidden = false;
    }
    render();
  };
  $('onHour').onchange = $('offHour').onchange = sync;
  $('nightClock').checked = ui.nightClock;
  $('nightClock').onchange = () => { ui.nightClock = $('nightClock').checked; render(); };
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
      b.innerHTML = `${escapeHtml(s.n)} <span class="meta">${escapeHtml(s.c)}</span>`;
      b.onclick = () => {
        ui.dep = s.c;
        ui.depName = s.n;
        setWeatherFrom(s.y, s.x, s.n);
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
// True when the user is asking for a stop outside London, which is the only
// case that needs a key, a different picker and a paced refresh.
function national() { return ui.busprov === 'national'; }

function initBuses() {
  const box = $('busSearch');

  // Any failure below used to leave the panel showing "Searching..." for ever,
  // because nothing caught it. Overpass makes that easy to hit -- it is a free,
  // heavily loaded service that answers 504 when overloaded and 429 when rate
  // limiting -- but the London path could hang the same way, just more rarely.
  const go = async () => {
    try {
      await runSearch();
    } catch (err) {
      const st = err && err.status;
      const msg = (st === 401 || st === 403)
        ? 'TransportAPI rejected the search. Check your app_id and app_key, and that'
          + ' today’s allowance is not already spent.'
        : (st === 429 || st === 503 || st === 504)
          ? 'The stop lookup service is busy right now. Wait a few seconds and search again.'
          : 'Could not search for stops: '
            + escapeHtml((err && err.message) || 'network error')
            + '. Check your connection and try again.';
      $('busResults').innerHTML = `<p class="hint">${msg}</p>`;
    }
  };

  const runSearch = async () => {
    const q = box.value.trim();
    const out = $('busResults');
    if (!q) return;
    if (national() && !(ui.busid && ui.buskey)) {
      out.innerHTML = '<p class="hint">Enter your TransportAPI app_id and app_key first —'
        + ' outside London the stop list needs them to show what is due.</p>';
      return;
    }
    out.innerHTML = '<p class="hint">Searching…</p>';

    // A bare stop code needs no search — check it and take it as given. Only
    // London codes are all digits; an ATCO code is longer and usually mixed,
    // so outside London this recognises the ATCO form instead.
    const bare = national() ? /^[0-9A-Za-z]{8,14}$/.test(q) && /\d/.test(q)
                            : /^\d+$/.test(q);
    if (bare) {
      const { status } = national()
        ? await api.nationalArrivals(q, ui.busid, ui.buskey)
        : await api.busArrivals(q);
      if (status === 'bad_key') {
        out.innerHTML = '<p class="hint">Those TransportAPI credentials were rejected.</p>';
        return;
      }
      if (status === 'bad_stop') {
        out.innerHTML = `<p class="hint">No stop found with the code ${escapeHtml(q)}.</p>`;
        return;
      }
      out.innerHTML = '';
      chooseStop({ code: q, name: `Stop ${q}`, towards: '', indicator: '' });
      return;
    }

    const { stops, label } = national()
      ? await api.findNationalStops(q, ui.busid, ui.buskey)
      : await api.findStops(q);
    if (!stops.length) {
      out.innerHTML = `<p class="hint">No bus stops found for “${escapeHtml(label)}”.
        Try a postcode, or a nearby landmark.</p>`;
      return;
    }
    out.innerHTML = '';
    for (const s of stops.slice(0, 12)) {
      const b = document.createElement('button');
      b.type = 'button';
      b.className = 'result';
      const ind = s.indicator ? ` (${escapeHtml(s.indicator)})` : '';
      const towards = s.towards ? escapeHtml(s.towards)
                                : (national() ? '' : 'hail &amp; ride');
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

  const prov = $('busProv');
  prov.onchange = () => {
    ui.busprov = prov.value;
    // The stop belongs to the provider that found it: a TfL SMS code means
    // nothing to TransportAPI and an ATCO code nothing to TfL, so switching
    // has to clear it rather than carry across a code that cannot work.
    ui.bus = ''; ui.busName = ''; ui.busPreview = [];
    $('busChosen').hidden = true;
    $('busResults').innerHTML = '';
    box.value = '';
    syncBusProvider();
    render();
  };
  $('busid').oninput = () => { ui.busid = $('busid').value.trim(); render(); };
  $('buskey').oninput = () => { ui.buskey = $('buskey').value.trim(); render(); };
  $('busbudget').onchange = () => {
    ui.busbudget = Number($('busbudget').value);
    syncBusProvider();
    render();
  };
  $('busKeyCheck').onclick = checkBusKey;
  syncBusProvider();
}

// Show the right half of the bus panel, and say plainly what the chosen
// allowance actually buys — the arithmetic is the firmware's (BUS-18), and
// hiding it is how someone ends up disappointed by a 32-minute refresh.
function syncBusProvider() {
  const isNat = national();
  $('busNational').hidden = !isNat;
  $('busProv').value = ui.busprov || 'tfl';
  $('busSearch').placeholder = isNat
    ? 'Postcode, town or village name, or the stop’s ATCO code'
    : 'Postcode, place name, or the 5-digit code on the stop';
  $('busProvHint').textContent = isNat
    ? 'TransportAPI covers the whole UK, but meters requests by the day, so the board'
      + ' paces itself to fit your allowance.'
    : 'TfL publishes London arrivals openly, so a London board needs no account'
      + ' and refreshes every 30 seconds.';
  if (isNat) {
    const onHours = hoursOn();
    const every = Math.max(30, Math.floor((onHours * 3600) / (ui.busbudget || 30)));
    const mins = (every / 60).toFixed(1);
    $('busbudget').value = String(ui.busbudget || 30);
    $('busBudgetHint').textContent =
      `Spread over the ${onHours} hours a day your screen is on, that is one update `
      + `every ${mins} minutes. Nothing is spent overnight.`
      + (every > 900 ? ' At that rate a bus can come and go between updates.' : '');
  }
}

// The hours the screen is on, matching Config::on_hours() on the board.
function hoursOn() {
  if (ui.onHour < 0 || ui.offHour < 0 || ui.onHour === ui.offHour) return 24;
  let blank = ui.onHour - ui.offHour;
  if (blank < 0) blank += 24;
  return blank <= 0 || blank >= 24 ? 24 : 24 - blank;
}

// Validate the credentials against a stop we know exists, before the user has
// spent any time picking their own. CORS is open on the 403 as well as the 200,
// so a wrong key gives a real answer rather than an opaque network error.
async function checkBusKey() {
  const el = $('busKeyStatus');
  el.hidden = false;
  if (!(ui.busid && ui.buskey)) { el.textContent = 'Enter both values first.'; return; }
  el.textContent = 'Checking…';
  const { status } = await api.nationalArrivals('370023135', ui.busid, ui.buskey);
  el.textContent = {
    ok: 'Working — these credentials are good.',
    bad_key: 'Rejected. Check the app_id and app_key, and that your plan is active.',
    bad_stop: 'The credentials work, but the test stop was not found.',
    net: 'Could not reach TransportAPI. Check your connection and try again.',
  }[status];
}

async function chooseStop(stop) {
  ui.bus = stop.code;
  ui.busName = stop.name;
  if (stop.lat != null && stop.lon != null) setWeatherFrom(stop.lat, stop.lon, stop.name);
  const el = $('busChosen');
  el.hidden = false;
  el.textContent = `Showing ${stop.name} (${stop.code}). Checking what’s due…`;
  render();
  const { status, rows } = national()
    ? await api.nationalArrivals(stop.code, ui.busid, ui.buskey, ui.busline)
    : await api.busArrivals(stop.code, ui.busline);
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
    if (p.lat != null && p.lon != null) setWeatherFrom(p.lat, p.lon, p.name);
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

// The weather location is never asked for: it comes from whatever stop, station
// or pier was chosen. The first pick wins, so switching a bus stop later does
// not silently move the weather away from the station someone set up first.
function setWeatherFrom(lat, lon, name) {
  if (ui.wxLat !== null) return;
  ui.wxLat = lat;
  ui.wxLon = lon;
  ui.wxName = name;
  const el = $('wxChosen');
  if (el) {
    el.hidden = false;
    el.textContent = `Weather for ${name}.`;
  }
  render();
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
  const keys = { train: 'dwtrain', bus: 'dwbus', river: 'dwriver',
                 weather: 'dwwx', clock: 'dwclock' };
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

  // The clock screen is not a header-and-rows board, so it is drawn whole.
  if (previewScreen === 'clock') {
    $('preview').innerHTML = `
      <div class="board bigclock" style="background:${bg};color:${fg}">
        <div class="bc-time">${nowClock().slice(0, 5)}</div>
      </div>`;
    showProblems();
    return;
  }

  let tag = 'TRAIN', name = ui.depName || 'Your station', rows, empty = null;
  if (previewScreen === 'bus') {
    tag = ui.busline ? `BUS ${ui.busline}` : 'BUS';
    name = ui.busName || 'Your stop';
    rows = (ui.busPreview || []).map((r) => ({ a: clock(r.mins), b: r.line, c: r.dest, d: r.mins < 1 ? 'Due' : r.mins + ' min' }));
    if (!rows.length) empty = 'No buses due';
  } else if (previewScreen === 'weather') {
    tag = 'WEATHER';
    name = ui.wxName || 'Your area';
    rows = [];
    empty = 'Live conditions';
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
  if (ui.services.includes('bus') && national() && !(ui.busid && ui.buskey))
    out.push('Your TransportAPI app_id and app_key, for buses outside London.');
  if (ui.services.includes('river') && !ui.river) out.push('A pier, or turn river boats off.');
  if (ui.services.includes('weather') && ui.wxLat === null) {
    out.push('Somewhere to show the weather for — pick a station, stop or pier above.');
  }
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
  $('btnFlash').disabled = !!list.length;
  $('btnDownload').disabled = !!list.length;
}

// ─────────────────────────────── install ─────────────────────────────────

// Which board the user says they have. Only the flashing step needs it: the
// settings protocol is identical on both, which is why step 2 has always
// worked on either without knowing.
let boardList = [];

async function initBoards() {
  const sel = $('boardPick');
  try {
    boardList = flasher.boards(await flasher.loadManifest());
  } catch {
    boardList = [];                    // no firmware published; step 2 still works
  }
  if (boardList.length < 2) {
    // One board, or none: a picker with a single entry is just noise.
    $('boardField').hidden = true;
    if (boardList.length === 1) ui.board = boardList[0].id;
    return;
  }
  sel.innerHTML = '';
  for (const b of boardList) sel.append(new Option(b.name, b.id));
  sel.value = ui.board && boardList.some((b) => b.id === ui.board)
    ? ui.board : boardList[0].id;
  ui.board = sel.value;
  sel.onchange = () => { ui.board = sel.value; describeBoard(); };
  describeBoard();
}

function describeBoard() {
  const b = boardList.find((x) => x.id === ui.board);
  if (!b) return;
  $('boardHint').textContent = b.note
    + (b.hold_boot
      ? ' \u2014 this board cannot put itself into programming mode, so hold its '
        + 'BOOT button while you press Flash and keep holding until the log says '
        + 'it detected the chip.'
      : '');
}

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

  // Two deliberate entry points. Until now the page would only flash a board
    // that failed to answer at all, so a board running *old* firmware could never
    // be updated from here -- it answered PING, went straight to provisioning, and
    // silently refused every setting it had never heard of.
  $('btnConnect').onclick = () => connectAndConfigure({ flashFirst: false });
  $('btnFlash').onclick = () => connectAndConfigure({ flashFirst: true });
}

function logLine(msg, cls = '') {
  const el = $('serialLog');
  el.hidden = false;
  el.insertAdjacentHTML('beforeend', `<div class="${cls}">${escapeHtml(msg)}</div>`);
  el.scrollTop = el.scrollHeight;
}

async function connectAndConfigure({ flashFirst = false } = {}) {
  const btn = $(flashFirst ? 'btnFlash' : 'btnConnect');
  const other = $(flashFirst ? 'btnConnect' : 'btnFlash');
  btn.disabled = other.disabled = true;
  const release = () => { btn.disabled = other.disabled = false; };
  $('serialLog').innerHTML = '';
  let board;
  try {
    board = await Board.request();
  } catch {
    logLine('No device selected.', 'bad');
    release();
    return;
  }

  try {
    logLine('Opening the port…');
    await board.open();

    let banner = null;
    if (flashFirst) {
      logLine('Flashing first, then applying your settings.');
      await board.close();
      if (!(await flashFirmware(board.port))) { release(); return; }
      logLine('Reconnecting after the flash…');
      if (!(await waitForReconnect(board, 20000))) {
        logLine('Firmware written. The board restarted and came back as a new '
          + 'USB device, so this page had to let go of it.', 'ok');
        logLine('Press “Send my settings” (step 2) and pick the board again.');
        release();
        return;
      }
      banner = await board.handshake(15000);
      if (!banner) {
        logLine('Firmware written, but the board is not answering yet. Unplug it, '
          + 'plug it back in, then press “Send my settings”.', 'bad');
        await board.close();
        release();
        return;
      }
    }

    if (!banner) logLine('Saying hello…');
    if (!banner) banner = await board.handshake();

    if (!banner) {
      // Nothing answered, so this is almost certainly a board that has never
      // been flashed. Offer to do it rather than dead-ending the user.
      logLine('No response — this board has no Departure Buddy firmware yet.');
      logLine('Flashing it now. (This is what step 1 does; you can start there '
        + 'next time.)');
      await board.close();
      if (!(await flashFirmware(board.port))) { release(); return; }

      logLine('Reconnecting after the flash…');
      // The board reboots into new firmware and its USB port re-enumerates, so
      // the handle is stale — reopen before trying to talk to it.
      if (!(await waitForReconnect(board, 20000))) {
        logLine('The board restarted but its port came back as a new device.', 'bad');
        logLine('Press “Send my settings” (step 2) and pick it once more — the '
          + 'firmware is already on there, so this time it will just apply your settings.');
        release();
        return;
      }
      banner = await board.handshake(15000);
      if (!banner) {
        logLine('Flashed, but the board is not answering yet. Unplug it, plug it '
          + 'back in, and press “Send my settings” (step 2).', 'bad');
        await board.close();
        release();
        return;
      }
    }
    logLine(`Found: ${banner}`, 'ok');

    const device = cfg.toDeviceConfig(ui);
    logLine(`Sending ${Object.keys(device).length} settings…`);
    const { saved, rejected } = await board.provision(device, cfg.KEYS, (done, total, key) => {
      if (key === 'mode') logLine(`  showing: ${device.mode}`);
      if (done === total) logLine(`Sent all ${total} settings.`);
    });

    // A board on older firmware refuses the settings it has never heard of. It
    // still saves the rest and still says SAVED, so without this the page would
    // claim success for a config that was half thrown away.
    if (rejected.length) {
      logLine(`Your board refused ${rejected.length} setting${rejected.length > 1 ? 's' : ''}: `
        + rejected.join(', '), 'bad');
      logLine('That means it is running older firmware than this page expects. '
        + 'Press “Flash the firmware” (step 1) — it updates the board and reapplies '
        + 'these settings — otherwise screens, buttons and colours will not work '
        + 'as configured.', 'bad');
    }

    if (!saved) {
      logLine('The board didn’t confirm the save. Try again, or replug it.', 'bad');
      await board.close();
      release();
      return;
    }
    logLine('Saved. The board is rebooting…', 'ok');

    const back = await waitForReconnect(board);
    if (!back) {
      logLine('Done — the board is restarting with your settings.', 'ok');
      logLine('(Its USB port reconnected as a new device, so this page let go of it.)');
      release();
      return;
    }
    await sleep(1200);
    const check = await board.readConfig();
    await verifyFirmware(board);
    await board.close();
    if (check) {
      logLine(`Confirmed: showing ${check.mode || '?'}, WiFi ${check.ssid || '?'}, `
        + `${check.wifi === 'up' ? 'connected' : 'still connecting…'}`, 'ok');
    }
    logLine('All done. Enjoy your board.', 'ok');
  } catch (e) {
    logLine('Something went wrong: ' + (e?.message || e), 'bad');
    try { await board.close(); } catch { }
  }
  release();
  showProblems();
}

// Ask the board what firmware it is running and compare it with what this site
// published. This is the whole point of publishing a digest: the user gets to
// check rather than trust.
async function verifyFirmware(board) {
  let manifest;
  try {
    manifest = await flasher.loadManifest();
  } catch {
    return;                       // nothing published here to compare against
  }
  const got = await board.readHash();
  if (!got) {
    logLine('This board is running firmware too old to report its checksum.');
    return;
  }
  // Compare against what the *device* says it is rather than what was picked in
  // the dropdown: the two can disagree, and the device is the authority.
  const entry = flasher.boardById(manifest, got.board || ui.board);
  const want = entry?.parts?.find((p) => p.path.endsWith('firmware.bin'))?.md5;
  if (!want) return;              // an older manifest, before md5 was published
  if (got.board && got.board !== ui.board && boardList.length > 1) {
    logLine(`This is a ${entry?.name || got.board}, not the board selected above.`);
  }
  if (got.md5.toLowerCase() === want.toLowerCase()) {
    logLine(`Verified: running the published firmware (${manifest.version}).`, 'ok');
  } else {
    logLine('This board is NOT running the firmware published here.', 'bad');
    logLine(`  board:     ${got.md5}`);
    logLine(`  published: ${want}`);
    logLine('That is expected if you built it yourself. Otherwise the board is on '
      + 'an older release: press “Flash the firmware” (step 1) to bring it up to '
      + 'date, which also reapplies your settings.');
  }
}

// Download, verify and write the firmware. Returns true if the board was
// flashed. Kept separate from provisioning because they are genuinely
// different operations: one replaces the program, the other only its settings.
async function flashFirmware(port) {
  let manifest;
  try {
    manifest = await flasher.loadManifest();
  } catch (e) {
    logLine('No firmware is published here to flash: ' + e.message, 'bad');
    logLine('Use the installer for a brand-new board, then come back here to '
      + 'change settings.');
    return false;
  }
  const board = flasher.boardById(manifest, ui.board);
  if (!board) {
    logLine('Pick which board you have before flashing.', 'bad');
    return false;
  }
  logLine(`Firmware available: ${manifest.name} ${manifest.version} `
    + `for ${board.name}.`);
  if (board.hold_boot) {
    logLine('Hold the BOOT button now, and keep holding until the log says it '
      + 'detected the chip.');
  }
  logLine('Flashing takes about a minute. Do not unplug the board.');

  try {
    const parts = await flasher.fetchImages(board, (m) => logLine('  ' + m));
    logLine('All images verified against their checksums.', 'ok');
    let lastPct = -1;
    await flasher.flash(port, parts,
      (msg) => logLine('  ' + msg),
      (frac) => {
        const pct = Math.floor(frac * 100 / 5) * 5;   // log every 5%
        if (pct !== lastPct) { lastPct = pct; logLine(`  writing… ${pct}%`); }
      },
      // What these images are for. The flasher compares it against the chip it
      // detects and refuses rather than bricking a board with the wrong build.
      board.chip, board.flash_size);
    logLine('Firmware written.', 'ok');
    return true;
  } catch (e) {
    if (e?.chipMismatch) {
      logLine(e.message, 'bad');
      logLine('Nothing was written, so the board is exactly as it was.');
      return false;
    }
    // The board could not be put into its ROM download mode. On boards with no
    // auto-program circuit -- which includes many ESP32 clones -- that is not a
    // fault, it just means the BOOT button has to be held by hand.
    const msg = String(e?.message || e);
    if (/boot mode|download mode/i.test(msg)) {
      logLine('The board did not enter its programming mode.', 'bad');
      logLine('Hold down the BOOT button on the board, press "Flash the '
        + 'firmware" again, and keep holding until the log says it detected '
        + 'the chip. Some boards cannot switch themselves over.');
      return false;
    }
    logLine('Flashing failed: ' + msg, 'bad');
    return false;
  }
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
// Populates the board picker from the published manifest. Async and deliberately
// not awaited: the whole page works without firmware published, and step 2 does
// not care which board it is talking to.
initBoards();
render();
setInterval(() => { if (!document.hidden) render(); }, 1000);
