// Live lookups for the pickers: bus stops, river piers, postcodes, and a check
// of the user's train API key.
//
// Every endpoint here is CORS-open, which is what lets this page be pure static
// hosting with no backend of its own:
//   api.tfl.gov.uk              Access-Control-Allow-Origin: *
//   countdown.api.tfl.gov.uk    Access-Control-Allow-Origin: *
//   api.postcodes.io            Access-Control-Allow-Origin: *
//   api1.raildata.org.uk        reflects the requesting origin
//
// These are ports of the equivalents in installer.py. Behaviour is kept
// identical on purpose: the page and the .exe must resolve a given search to
// the same stop.

const TFL_URA = 'https://countdown.api.tfl.gov.uk/interfaces/ura/instant_V1';
const TFL_API = 'https://api.tfl.gov.uk';

const RETURN_LIST_STOPS =
  'StopCode1,StopPointName,StopPointIndicator,Towards,Latitude,Longitude';
const RETURN_LIST_ARRIVALS =
  'StopPointName,LineName,DestinationText,EstimatedTime,ExpireTime';

// --- shared helpers ---------------------------------------------------------

// Deliberately no custom request headers. Adding even one (a User-Agent-ish
// marker, say) makes this a non-simple cross-origin request, so the browser
// sends a CORS preflight OPTIONS first — and the TfL endpoints answer the GET
// but not the preflight, so the whole thing fails. The .exe sends a User-Agent
// because it is not bound by CORS; this cannot.
async function getText(url) {
  const r = await fetch(url);
  if (!r.ok) { const e = new Error('HTTP ' + r.status); e.status = r.status; throw e; }
  return r.text();
}

async function getJson(url, init) {
  const r = await fetch(url, init);
  if (!r.ok) { const e = new Error('HTTP ' + r.status); e.status = r.status; throw e; }
  return r.json();
}

// The URA feed is deliberately not a JSON document: it is one JSON array per
// line. Parsed line-by-line, exactly as bus_api.cpp and installer.py do.
function uraLines(body) {
  const out = [];
  for (const line of body.split('\n')) {
    const t = line.trim();
    if (!t) continue;
    try { out.push(JSON.parse(t)); } catch { /* skip an unreadable line */ }
  }
  return out;
}

export function haversineM(lat1, lon1, lat2, lon2) {
  const R = 6371000, rad = Math.PI / 180;
  const p1 = lat1 * rad, p2 = lat2 * rad;
  const dp = (lat2 - lat1) * rad, dl = (lon2 - lon1) * rad;
  const a = Math.sin(dp / 2) ** 2 + Math.cos(p1) * Math.cos(p2) * Math.sin(dl / 2) ** 2;
  return 2 * R * Math.asin(Math.sqrt(a));
}

// TfL spells some names with typographic punctuation the board's font has no
// glyph for (RIV-13/INST-28). Fold it before showing or storing.
export function plain(text) {
  return (text || '')
    .replace(/[‘’]/g, "'")
    .replace(/[“”]/g, '"')
    .replace(/[–—]/g, '-');
}

// --- buses ------------------------------------------------------------------

// Stops within `radiusM` of a point, nearest first. URA returns fields in the
// documented sequence order, not the order requested, so this array shape is
// fixed: [0, StopPointName, StopCode1, Towards, StopPointIndicator, Lat, Lon].
export async function stopsNear(lat, lon, radiusM) {
  const url = `${TFL_URA}?Circle=${lat.toFixed(6)},${lon.toFixed(6)},${Math.round(radiusM)}`
    + `&StopPointState=0&StopAlso=True&ReturnList=${RETURN_LIST_STOPS}`;
  const stops = [];
  for (const a of uraLines(await getText(url))) {
    if (!Array.isArray(a) || a.length < 7 || a[0] !== 0) continue;
    const [, name, code, towards, indicator, slat, slon] = a;
    // Stops with no usable code are stands or withdrawn stops, not boardable.
    if (!code || !/^\d+$/.test(String(code))) continue;
    stops.push({
      code: String(code),
      name: plain(name || ''),
      towards: plain(towards || ''),
      indicator: indicator || '',
      // The stop's own position, kept so the weather screen can be pointed at
      // it without asking the user for a location twice.
      lat: Number(slat),
      lon: Number(slon),
      distance: haversineM(lat, lon, Number(slat), Number(slon)),
    });
  }
  stops.sort((a, b) => a.distance - b.distance);
  return stops;
}

// A named place often matches several coordinates (TfL indexes each station
// entrance separately), and one circle around just one of them misses stops
// around the others.
export async function stopsNearAny(origins, radiusM) {
  const merged = new Map();
  const lists = await Promise.all(origins.map(([la, lo]) => stopsNear(la, lo, radiusM)));
  for (const list of lists) {
    for (const s of list) {
      const prev = merged.get(s.code);
      if (!prev || s.distance < prev.distance) merged.set(s.code, s);
    }
  }
  return [...merged.values()].sort((a, b) => a.distance - b.distance);
}

export async function geocodePlace(query) {
  const url = `${TFL_API}/StopPoint/Search?query=${encodeURIComponent(query)}`
    + '&modes=bus&maxResults=6';
  try {
    const d = await getJson(url);
    return (d.matches || [])
      .filter((m) => m.lat && m.lon)
      .map((m) => ({ name: plain(m.name || query), lat: +m.lat, lon: +m.lon }));
  } catch { return []; }
}

export async function geocodePostcode(postcode) {
  const url = 'https://api.postcodes.io/postcodes/'
    + encodeURIComponent(String(postcode).replace(/\s+/g, ''));
  try {
    const d = await getJson(url);
    return [Number(d.result.latitude), Number(d.result.longitude)];
  } catch { return null; }
}

const POSTCODE_RE = /^[A-Z]{1,2}\d[A-Z\d]?\s*\d[A-Z]{2}$/i;

// One search box handles all three sensible inputs rather than making the user
// pick a mode first: a postcode, a place or stop name, or the stop's own code.
export async function findStops(search) {
  const q = search.trim();
  if (POSTCODE_RE.test(q)) {
    const point = await geocodePostcode(q);
    if (!point) return { stops: [], label: q.toUpperCase() };
    return { stops: await stopsNearAny([point], 500), label: q.toUpperCase() };
  }
  const matches = await geocodePlace(q);
  if (!matches.length) return { stops: [], label: q };
  const label = matches[0].name;
  const origins = matches.filter((m) => m.name === label).map((m) => [m.lat, m.lon]);
  return { stops: await stopsNearAny(origins, 250), label };
}

// Live arrivals at a stop: (status, rows). Status is 'ok' | 'bad_stop' | 'net'.
export async function busArrivals(code, lineFilter = '') {
  let url = `${TFL_URA}?StopCode1=${encodeURIComponent(code)}`
    + `&ReturnList=${RETURN_LIST_ARRIVALS}`;
  if (lineFilter) url += '&LineName=' + encodeURIComponent(lineFilter);
  let body;
  try {
    body = await getText(url);
  } catch (e) {
    // 416 is the feed's way of saying "no such stop code".
    return { status: e.status === 416 ? 'bad_stop' : 'net', rows: [] };
  }
  let nowMs = 0;
  const rows = [];
  for (const a of uraLines(body)) {
    if (!Array.isArray(a) || !a.length) continue;
    if (a[0] === 4 && a.length >= 3) nowMs = a[2];            // URA clock
    else if (a[0] === 1 && a.length >= 6 && nowMs) {
      rows.push({
        line: String(a[2]),
        dest: plain(String(a[3])),
        mins: Math.max(0, Math.floor((a[4] - nowMs) / 60000)),
      });
    }
  }
  rows.sort((x, y) => x.mins - y.mins);
  return { status: 'ok', rows };
}

// --- river ------------------------------------------------------------------

const RIVER_LINES = ['rb1', 'rb4', 'rb6', 'woolwich-ferry'];

// Every pier served by a TfL river-bus line. Asked per line and merged: a pier
// sits on several routes and TfL lists it once per route. Only NaptanFerryPort
// entries are kept — the 9300xxx IDs alongside them are individual berths, and
// a board pointed at one berth would miss half its pier's sailings (RIV-02).
export async function riverPiers() {
  const piers = new Map();
  const results = await Promise.all(RIVER_LINES.map(async (line) => {
    try { return [line, await getJson(`${TFL_API}/Line/${line}/StopPoints`)]; }
    catch { return [line, null]; }
  }));
  for (const [line, data] of results) {
    if (!data) continue;
    const tag = line === 'woolwich-ferry' ? 'WF' : line.toUpperCase();
    for (const sp of data) {
      if (sp.stopType !== 'NaptanFerryPort' || !sp.id) continue;
      const e = piers.get(sp.id) || {
        id: sp.id,
        name: plain(sp.commonName || sp.id),
        lat: sp.lat != null ? Number(sp.lat) : null,
        lon: sp.lon != null ? Number(sp.lon) : null,
        lines: [],
      };
      if (!e.lines.includes(tag)) e.lines.push(tag);
      piers.set(sp.id, e);
    }
  }
  const out = [...piers.values()];
  out.forEach((p) => p.lines.sort());
  out.sort((a, b) => a.name.localeCompare(b.name));
  return out;
}

// Live sailings at a pier: (status, rows). 'ok' | 'bad_pier' | 'net'.
export async function riverArrivals(pier, lineFilter = '') {
  let data;
  try {
    data = await getJson(`${TFL_API}/StopPoint/${encodeURIComponent(pier)}/Arrivals`);
  } catch (e) {
    return { status: e.status === 404 ? 'bad_pier' : 'net', rows: [] };
  }
  const seen = new Set();
  const rows = [];
  for (const p of Array.isArray(data) ? data : []) {
    const line = String(p.lineName || '');
    if (!line) continue;
    if (lineFilter && line.toLowerCase() !== lineFilter.toLowerCase()) continue;
    // TfL repeats a sailing across a pier's two berths; on a three-row screen a
    // duplicate costs a real departure its slot (RIV-10).
    if (p.vehicleId) {
      if (seen.has(p.vehicleId)) continue;
      seen.add(p.vehicleId);
    }
    rows.push({
      line,
      dest: plain(String(p.destinationName || '')),
      mins: Math.max(0, Math.floor((p.timeToStation || 0) / 60)),
    });
  }
  rows.sort((a, b) => a.mins - b.mins);
  return { status: 'ok', rows };
}

// --- trains -----------------------------------------------------------------

// Best-effort online check of the CRS + key, mirroring installer.py's
// validate_station. Returns 'ok' | 'bad_station' | 'bad_key' | 'net'.
export async function validateStation(key, crs) {
  const url = 'https://api1.raildata.org.uk/1010-live-departure-board-dep1_2/LDBWS/'
    + `api/20220120/GetDepBoardWithDetails/${encodeURIComponent(crs)}?numRows=1&timeWindow=30`;
  try {
    const r = await fetch(url, { headers: { 'x-apikey': key } });
    if (r.ok) return 'ok';
    if (r.status === 400) return 'bad_station';
    if (r.status === 401 || r.status === 403) return 'bad_key';
    return 'net';
  } catch { return 'net'; }
}

// The station list is baked in: there is no CORS-friendly UK-wide station
// search, so name lookup has to be local. See web/data/stations.json.
let _stations = null;
export async function stations() {
  if (!_stations) _stations = await getJson('data/stations.json');
  return _stations;
}

export async function searchStations(query, limit = 12) {
  const q = query.trim().toLowerCase();
  if (!q) return [];
  const all = await stations();
  if (/^[a-z]{3}$/.test(q)) {
    const exact = all.filter((s) => s.c.toLowerCase() === q);
    if (exact.length) return exact;
  }
  const starts = [], contains = [];
  for (const s of all) {
    const n = s.n.toLowerCase();
    if (n.startsWith(q)) starts.push(s);
    else if (n.includes(q)) contains.push(s);
    if (starts.length >= limit) break;
  }
  return starts.concat(contains).slice(0, limit);
}
