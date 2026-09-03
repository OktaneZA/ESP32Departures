// Live lookups for the pickers: bus stops, river piers, postcodes, and a check
// of the user's train API key.
//
// Every endpoint here is CORS-open, which is what lets this page be pure static
// hosting with no backend of its own:
//   api.tfl.gov.uk              Access-Control-Allow-Origin: *
//   countdown.api.tfl.gov.uk    Access-Control-Allow-Origin: *
//   api.postcodes.io            Access-Control-Allow-Origin: *
//   api1.raildata.org.uk        reflects the requesting origin
//   transportapi.com            Access-Control-Allow-Origin: * (on 200 and 403,
//                               so a key can be checked before any hardware is)
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

// --- buses outside London ---------------------------------------------------
//
// TfL's feed stops at the M25, and the picker above with it. Outside London both
// the departures and the stop list come from TransportAPI, for reasons that took
// some finding:
//
//   * bustimes.org has stop data but no spatial search at all: it silently
//     ignores bbox, latitude/longitude/distance and search, and answered a
//     London bounding box with stops in Downpatrick and Las Vegas.
//   * OpenStreetMap via Overpass is quota-free and does carry naptan:AtcoCode,
//     but it is a heavily loaded public service — measured 504, then 429, then
//     87 seconds for one Nottingham query. Not something a setup page can use.
//   * TransportAPI's /bus/stops/near.json is 403 on the free plan, but
//     /places.json?type=bus_stop is not, and is both fast and CORS-open.
//
// So a search costs one request from the daily allowance. Geocoding stays on
// postcodes.io, which is unmetered, so it is exactly one however it was phrased.

const TRANSPORTAPI = 'https://transportapi.com/v3/uk';

// Bus stops within `radiusM` of a point, nearest first. Same shape as
// stopsNear() so the picker UI does not care which provider filled it.
// Stops near a point, from TransportAPI's own place index. Same shape as
// stopsNear() so the picker UI does not care which provider filled it.
//
// This costs one request out of the daily allowance, which is why it is not used
// for anything but an explicit search. That is a fair price: OpenStreetMap via
// Overpass is quota-free and was tried first, but it is a heavily loaded public
// service that answered 504, then 429, then took 87 seconds to return a single
// Nottingham query. A setup page cannot be built on that.
export async function nationalStopsNear(lat, lon, radiusM, appId, appKey) {
  const url = `${TRANSPORTAPI}/places.json?app_id=${encodeURIComponent(appId)}`
    + `&app_key=${encodeURIComponent(appKey)}&type=bus_stop`
    + `&lat=${lat.toFixed(6)}&lon=${lon.toFixed(6)}`;
  const d = await getJson(url);
  const stops = [];
  for (const m of d.member || []) {
    if (!m.atcocode || m.latitude == null) continue;
    // Names come through as "Canal Street (Stop C3) - E-bound": the bracketed
    // indicator and the compass bearing are separated out so the picker can
    // show them the way the London one does, rather than one long line.
    let raw = plain(m.name || m.atcocode);
    // Peel the compass bearing off the end first, and only when it really is
    // one -- names contain hyphens of their own, so a looser split would eat
    // half of "Newcastle-under-Lyme".
    let towards = '';
    const bound = /\s*-\s*([NSEW]{1,2}-bound)\s*$/i.exec(raw);
    if (bound) { towards = bound[1]; raw = raw.slice(0, bound.index); }
    let indicator = '';
    const br = /^(.*?)\s*\((.*?)\)\s*$/.exec(raw);
    if (br) { indicator = br[2]; raw = br[1]; }
    const dist = m.distance != null ? Number(m.distance)
                                    : haversineM(lat, lon, +m.latitude, +m.longitude);
    if (radiusM && dist > radiusM) continue;
    stops.push({
      code: String(m.atcocode),
      name: raw,
      towards,
      indicator,
      lat: Number(m.latitude),
      lon: Number(m.longitude),
      distance: dist,
    });
  }
  stops.sort((a, b) => a.distance - b.distance);
  return stops;
}

// UK-wide place lookup. geocodePlace() above is TfL's index and so is London
// only; postcodes.io covers the whole country and is already trusted here for
// postcodes, which keeps the origin list short.
export async function geocodePlaceUK(query) {
  const url = 'https://api.postcodes.io/places?limit=5&q=' + encodeURIComponent(query);
  try {
    const d = await getJson(url);
    return (d.result || [])
      .filter((p) => p.latitude != null)
      .map((p) => ({ name: plain(p.name_1 || query), lat: +p.latitude, lon: +p.longitude }));
  } catch { return []; }
}

// The national twin of findStops(). A wider radius than London's 250-500m:
// stops are much further apart outside a city, and an empty list is a worse
// answer than a slightly long one.
export async function findNationalStops(search, appId, appKey) {
  const q = search.trim();
  // Geocoding stays on postcodes.io, which is free and unmetered, so a search
  // spends exactly one TransportAPI request however it was phrased.
  const point = POSTCODE_RE.test(q) ? await geocodePostcode(q) : null;
  if (point) {
    return { stops: await nationalStopsNear(point[0], point[1], 1500, appId, appKey),
             label: q.toUpperCase() };
  }
  if (POSTCODE_RE.test(q)) return { stops: [], label: q.toUpperCase() };
  const matches = await geocodePlaceUK(q);
  if (!matches.length) return { stops: [], label: q };
  const m = matches[0];
  return { stops: await nationalStopsNear(m.lat, m.lon, 1500, appId, appKey), label: m.name };
}

// Live departures at an ATCO stop: (status, rows). Status is
// 'ok' | 'bad_stop' | 'bad_key' | 'net'. Mirrors bus_national.cpp exactly — same
// canonical endpoint, same best_departure_estimate, same midnight handling — so
// the preview here and the board agree.
export async function nationalArrivals(atco, appId, appKey, lineFilter = '') {
  const url = `${TRANSPORTAPI}/bus/stop_timetables/${encodeURIComponent(atco)}.json`
    + `?app_id=${encodeURIComponent(appId)}&app_key=${encodeURIComponent(appKey)}`
    + '&group=false&live=true&limit=12';
  let d;
  try {
    d = await getJson(url);
  } catch (e) {
    if (e.status === 401 || e.status === 403) return { status: 'bad_key', rows: [] };
    // An unknown stop code answers 400 ("A stop with code X doesn't exist"),
    // not 404. The only variable in the URL is the stop, so 400 means just that.
    if (e.status === 400 || e.status === 404) return { status: 'bad_stop', rows: [] };
    return { status: 'net', rows: [] };
  }

  const mod = (s, off) => {
    const m = /^(\d{2}):(\d{2})/.exec(String(s || '').slice(off));
    if (!m) return -1;
    const h = +m[1], mi = +m[2];
    return h > 23 || mi > 59 ? -1 : h * 60 + mi;
  };
  const now = mod(d.request_time, 11);
  if (now < 0) return { status: 'net', rows: [] };

  const rows = [];
  for (const x of (d.departures || {}).all || []) {
    if (x.status?.cancellation?.value) continue;
    const line = String(x.line_name || '');
    if (!line) continue;
    if (lineFilter && line.toLowerCase() !== lineFilter.toLowerCase()) continue;
    const dep = mod(x.best_departure_estimate, 0);
    if (dep < 0) continue;
    let mins = dep - now;
    if (mins < -60) mins += 1440;            // departure is tomorrow
    else if (mins > 1440 - 60) mins -= 1440; // departure was yesterday, running late
    if (mins < 0) mins = 0;
    if (mins > 30) continue;
    rows.push({ line, dest: plain(String(x.direction || '')), mins });
  }
  rows.sort((a, b) => a.mins - b.mins);
  return { status: 'ok', rows, stopName: plain(d.stop_name || d.name || '') };
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
