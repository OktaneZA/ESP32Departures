# Departure Buddy — Requirements

## Overview

**Departure Buddy** is a UK National Rail departure board that runs on a **LilyGo
T-Display-S3** (ESP32-S3) microcontroller. It fetches live departures over WiFi
directly from the National Rail Live Departure Board (LDBWS) JSON API and renders
them on the board's built-in 170×320 colour LCD — no host computer, server, or
cloud service.

It can also show **live London bus arrivals** for one bus stop, from TfL's open
Countdown feed, and **live river boat sailings** for one Thames pier — Uber Boat
by Thames Clippers and the Woolwich Ferry — from TfL's Unified API. Every
service is optional and independent: the board shows any combination and cycles
through whichever are enabled.

This document is a **retrospective** specification: it records the requirements
the delivered firmware and installer actually satisfy.

### Attribution / lineage

- **Original project:** [chrisys/train-departure-display](https://github.com/chrisys/train-departure-display)
  by Chris Crocker-White — the origin of the departure-board concept, layout, and
  the dot-matrix fonts reused here (`fonts/`).
- **Native Raspberry Pi rewrite:** [OktaneZA/PiDepartures](https://github.com/OktaneZA/PiDepartures)
  — replaced Balena/Docker with a native systemd deployment and the OpenLDBWS
  SOAP client. This ESP32 project began life inside that repo before being split out.
- **This project (Departure Buddy):** a from-scratch C++ port of that behaviour to
  the ESP32-S3, replacing Python/SOAP with Arduino/JSON and adding a self-contained
  Windows installer that provisions the board over USB.

### Key differences from the Pi version

- Runs on a **£15–25 ESP32-S3 board**, not a Raspberry Pi; no OS, no SD card.
- **Colour LCD (170×320)** instead of the 256×64 mono OLED.
- Data via the **Rail Data Marketplace LDBWS REST/JSON API**, not OpenLDBWS SOAP
  (the legacy SOAP token no longer works; RTT's API is also being retired).
- Configuration stored **on-device (NVS)** and set over USB serial by a bundled
  **Windows `.exe` installer** — one pre-built binary serves every user.
- **A second, optional screen** the board cycles to: live London bus arrivals
  from TfL's open Countdown feed. The Pi version has no equivalent.

---

## 1. Hardware Requirements

| Component | Requirement |
|---|---|
| **Board** | LilyGo T-Display-S3 (ESP32-S3) |
| **MCU** | ESP32-S3, dual-core, **2.4 GHz WiFi only** (no 5 GHz) |
| **Display** | 1.9″ ST7789 colour LCD, **170×320**, 8-bit parallel (i80) bus |
| **Memory** | 16 MB flash, 8 MB PSRAM |
| **USB** | USB-C (native USB — used for flashing and configuration) |
| **Wiring** | None — LCD, WiFi, and USB are on-board |

Incompatible look-alikes (different display/driver): T-Display-S3 **AMOLED**
(1.91″), **Pro** (2.33″), **Long** (3.4″), and the older ESP32 **T-Display** (1.14″).

### Board-specific notes

| ID | Requirement |
|---|---|
| HW-01 | Panel power is gated on **GPIO15** — firmware drives it HIGH at boot |
| HW-02 | Panel config: ST7789, offset_x 35, `invert=true`, 8-bit parallel bus (pins per `display.cpp`) |
| HW-03 | Backlight on GPIO38 via PWM; brightness is runtime-configurable (0–255) |
| HW-04 | Rendered in landscape (rotation 1) → 320×170 usable canvas |

---

## 2. Software Stack

**Firmware** (PlatformIO, `platformio.ini`):

| Component | Version / Package |
|---|---|
| Framework | Arduino-ESP32 (via PlatformIO `espressif32`) |
| Board | `lilygo-t-display-s3` |
| Display | `lovyan03/LovyanGFX` ≥ 1.1.16 |
| JSON | `bblanchon/ArduinoJson` ≥ 7.1.0 |
| Storage | `Preferences` (NVS) — Arduino core |

**Installer** (`installer/`):

| Component | Version / Package |
|---|---|
| Python | 3.x (build host only; not required on target PC) |
| Flashing | `esptool` ≥ 4.7, < 5 |
| Serial | `pyserial` 3.x |
| Packaging | `pyinstaller` (one-file exe) |
| Timezone | `tzlocal` + `tzdata` (optional; detects the PC's POSIX TZ) |

The installer reaches three services, all keyless and all on the user's PC only:
the TfL Countdown feed (verifying a bus stop and searching for one), TfL's
StopPoint search and postcodes.io (turning a name or postcode into coordinates),
and the LDBWS endpoint (verifying the station and API key).

**Documentation tooling** (`docs/`, not required to build or run):

| Component | Purpose |
|---|---|
| `render_mockup.py` (Pillow) | Pixel-accurate renders of all three boards, mirroring `display.cpp` — colours derived from its RGB565 constants, and any theme renderable by passing a palette |
| `ttf_to_lgfx.py` | Converts the dot-matrix TTFs to an `lgfx::GFXfont` header |

---

## 3. Data Source — National Rail LDBWS (REST/JSON)

| Item | Detail |
|---|---|
| **API** | National Rail Live Departure Board (LDBWS) — Rail Data Marketplace |
| **Endpoint** | `https://api1.raildata.org.uk/<product>/LDBWS/api/20220120/GetDepBoardWithDetails/{CRS}` |
| **Protocol** | REST / JSON over HTTPS |
| **Authentication** | Consumer key in `x-apikey` request header |
| **Registration** | Free account at https://raildata.org.uk → subscribe to "Live Departure Board" (open tier) |
| **Query params** | `numRows`, `timeWindow`, optional `filterCrs` + `filterType=to` |
| **Data returned** | Scheduled/expected time, platform, destination, calling points, cancellation flag |
| **Polling interval** | Configurable (`refresh`, default 60 s) |

| ID | Requirement |
|---|---|
| API-01 | HTTPS enforced for the LDBWS endpoint; no HTTP fallback |
| API-02 | `destination` and `subsequentCallingPoints` parsed defensively (both array/object serialisations), verified against live data |
| API-03 | JSON parsed with an ArduinoJson filter into PSRAM to bound heap usage |
| API-04 | Malformed/short responses fail the fetch cleanly without crashing the task |
| API-05 | `RAW_JSON_DEBUG` build flag dumps the raw response for field verification |

---

## 3a. Data Source — TfL Live Bus & River Bus Arrivals (optional)

Used only when the user configures a bus stop. Sourced from TfL's Countdown
system via the "URA" instant interface, documented in
[TfL's Live Bus & River Bus Arrivals API interface spec](https://content.tfl.gov.uk/tfl-live-bus-river-bus-arrivals-api-documentation.pdf)
(v2.1, 05/08/2016).

| Item | Detail |
|---|---|
| **API** | TfL Live Bus & River Bus Arrivals (Countdown / URA), instant interface |
| **Endpoint** | `https://countdown.api.tfl.gov.uk/interfaces/ura/instant_V1` |
| **Protocol** | HTTPS. The body is **JSON-like but not a JSON document**: one JSON array per line, newline-separated |
| **Authentication** | **None** — the instant feed is open, no key or registration. (The *streaming* interface needs Digest auth and is not used) |
| **Data returned** | Route, destination, absolute arrival time (Unix epoch **milliseconds**, UTC) |
| **Coverage** | London buses and TfL river bus piers only |
| **Freshness** | Refreshed at source every 30 s and cached 30 s; the spec states there is no benefit to polling faster |
| **Prediction horizon** | The next 30 minutes |
| **Attribution** | "Data provided by Transport for London". Applications must not carry TfL branding or imply they are official TfL apps |

### 3a.1 Requests used

**Arrivals (firmware, `src/bus_api.cpp`):**

```
GET /interfaces/ura/instant_V1
    ?StopCode1={5-digit SMS stop code}
    &StopAlso=True
    [&LineName={route}]
    &ReturnList=StopPointName,LineName,DestinationText,EstimatedTime,ExpireTime
```

**Stop search (installer, `installer/installer.py`):**

```
GET /interfaces/ura/instant_V1
    ?Circle={lat},{lon},{radius_m}
    &StopPointState=0
    &StopAlso=True
    &ReturnList=StopCode1,StopPointName,StopPointIndicator,Towards,Latitude,Longitude
```

### 3a.2 Response format

The first element of every line is the array type:

| Type | Array | Used for |
|---|---|---|
| 0 | Stop | The stop's own record (name); returned for a stop with no predictions when `StopAlso=True` |
| 1 | Prediction | One expected arrival |
| 2 | Flexible Message | Service disruption text — not requested |
| 3 | Baseversion | Reference-data version — not requested |
| 4 | URA Version | Always the **first** line; carries the server timestamp |

**Fields are returned in the spec's own sequence order, not the order given in
`ReturnList`.** With the requests above the lines are therefore:

| Line | Shape |
|---|---|
| URA version | `[4, "1.0", serverTimeMs]` |
| Prediction (firmware) | `[1, StopPointName, LineName, DestinationText, EstimatedTime, ExpireTime]` |
| Stop (firmware, `StopAlso`) | `[0, StopPointName]` — only two elements |
| Stop (installer search) | `[0, StopPointName, StopCode1, Towards, StopPointIndicator, Latitude, Longitude]` |

For reference, the full documented sequences are — Prediction: `ResponseType,
StopPointName, StopID, StopCode1, StopCode2, StopPointType, Towards, Bearing,
StopPointIndicator, StopPointState, Latitude, Longitude, VisitNumber, LineID,
LineName, DirectionID, DestinationText, DestinationName, VehicleID, TripID,
RegistrationNumber, EstimatedTime, ExpireTime`; Stop: the same list up to
`Longitude`.

### 3a.3 Status codes

| Code | Meaning | Handling |
|---|---|---|
| 200 | OK | Parse |
| 400 | Malformed request (e.g. an unknown `ReturnList` field) | Transient failure + back-off |
| 416 | **Stop code unknown** (`Stop code unknown: SMS:nnnnn`) | Configuration error — see BUS-09 |
| 401 / 500 / 502 | Auth (streaming only) / server / upstream error | Transient failure + back-off |

### 3a.4 Supporting services (installer only)

Finding a stop from a place needs a coordinate, which the URA feed does not
provide. Both of these run **on the user's PC during setup only** — the board
itself never calls them:

| Service | Use | Auth |
|---|---|---|
| `https://api.tfl.gov.uk/StopPoint/Search` | Place / stop name — coordinates | None |
| `https://api.postcodes.io/postcodes/{postcode}` | UK postcode — coordinates | None |

### 3a.5 Requirements

| ID | Requirement |
|---|---|
| BUS-01 | HTTPS enforced for the TfL endpoint; no HTTP fallback |
| BUS-02 | Response parsed **line by line**, each line a JSON array whose first element is the array type |
| BUS-03 | Fields are read by the **documented sequence order**, not the `ReturnList` order — the feed reorders them |
| BUS-04 | Arrival countdowns computed against the feed's own timestamp (the type-4 line), so they stay correct even if the board's clock is wrong |
| BUS-05 | `ReturnList` kept to the five fields actually rendered, per TfL's guidance; `ExpireTime` requested alongside `EstimatedTime` as the spec requires |
| BUS-06 | Predictions arrive unordered and are sorted soonest-first before display |
| BUS-07 | The response is read with a bounded line buffer and a total byte cap (`BUS_MAX_RESPONSE`), so a malformed or huge response cannot exhaust the heap |
| BUS-08 | HTTP/1.0 is requested so the server returns an unchunked body, which the bounded line reader can consume directly |
| BUS-09 | An unknown stop code (HTTP 416) is a configuration error: logged once, retried only every 5 minutes, and the bus screen is withheld entirely |
| BUS-10 | Zero arrivals is a valid result ("No buses due"), not a fetch failure |
| BUS-11 | `RAW_BUS_DEBUG` build flag dumps the raw response lines for field verification |
| BUS-12 | `StopAlso=True` is requested so the stop's **name** is returned even when it has no predictions — otherwise a quiet stop shows only its bare code |
| BUS-13 | A Stop array is only two elements long, so line-length guards must not assume a minimum of three |
| BUS-14 | Arrivals further ahead than `BUS_MAX_ETA_MINUTES` are discarded |
| BUS-15 | The stop code and route filter are percent-encoded into the query string. (The feed additionally defines escaping \`a` for `&` and \`c` for `,` inside *values*; neither character is valid in a stop code or route name, so it does not arise) |
| BUS-16 | Stops whose `StopCode1` is null or the literal string `"NONE"` are bus stands or withdrawn stops, not boardable, and are excluded from search results |
| BUS-17 | Polling is no more frequent than `BUS_REFRESH_SECONDS` (30 s), matching the feed's cache — faster polling returns identical data |

---

## 3b. Data Source — TfL River Bus (Unified API, optional)

Used only when the user configures a pier. Uber Boat by Thames Clippers
(RB1/RB4/RB6) and the Woolwich Ferry are operated as TfL `river-bus` services,
so their live predictions come from the ordinary
[TfL Unified API](https://api.tfl.gov.uk/), not from the Countdown feed the bus
screen uses.

> **Why a second TfL client.** The Countdown feed is titled "Live Bus **& River
> Bus** Arrivals", but in practice it returns only buses: a `Circle=` query
> centred on Canary Wharf Pier returns bus stops exclusively, and
> `StopPointName=Canary Wharf Pier` returns HTTP 416. Piers are reachable only
> through the Unified API, which is ordinary JSON rather than the URA
> line-per-record format — hence a separate parser.

| Item | Detail |
|---|---|
| **API** | TfL Unified API, `river-bus` mode |
| **Endpoint** | `https://api.tfl.gov.uk/StopPoint/{naptan}/Arrivals` |
| **Protocol** | HTTPS. Standard JSON: an array of `Tfl.Api.Presentation.Entities.Prediction` objects |
| **Authentication** | **None**. An `app_key` is optional and only raises the rate limit (50 req/min unregistered, 500 req/min registered) |
| **Data returned** | `lineName`, `destinationName`, `stationName`, `timeToStation` (**seconds from now**), `vehicleId`, `platformName` (inbound/outbound) |
| **Coverage** | RB1, RB4, RB6 and the Woolwich Ferry. **RB2 (Tate to Tate) is not published on this feed** |
| **Freshness** | Genuinely live, not a timetable dump: sampling the whole mode twice 100 s apart and matching on `vehicleId`+`naptanId`, sailings already under way shifted their `expectedArrival` by −66 s and −39 s. Sailings not yet departed sit on scheduled whole-minute times until they move |
| **Prediction horizon** | Open-ended; the firmware caps it at `RIVER_MAX_ETA_MINUTES` |
| **Attribution** | "Data provided by Transport for London" |

### 3b.1 Pier identifiers

A pier has two kinds of Naptan ID and the distinction matters:

| Form | Example | Meaning |
|---|---|---|
| `930G…` | `930GCAW` | `NaptanFerryPort` — the **pier**, aggregating both directions |
| `9300…` | `9300CAW1` | `NaptanFerryBerth` — one **berth**, roughly one direction |

The board must be pointed at the port. A berth sees only the sailings that use
it, so a board configured with one would silently miss half its pier's boats.

There are 26 piers across the four lines. The installer fetches them live from
`/Line/{id}/StopPoints` and keeps a baked-in fallback list for offline setup.

### 3b.2 Requirements

| ID | Requirement |
|---|---|
| RIV-01 | HTTPS enforced for the TfL endpoint; no HTTP fallback |
| RIV-02 | Only `NaptanFerryPort` (`930G…`) IDs are offered by the installer and accepted as a pier; berths are never presented |
| RIV-03 | `timeToStation` is used directly as the countdown. It is already relative to the moment TfL answered, so the river screen is correct even before NTP has synced |
| RIV-04 | Response parsed with an ArduinoJson **filter**, so only the five rendered fields are ever allocated out of a ~6 KB document |
| RIV-05 | The body is read into a bounded buffer capped at `RIVER_MAX_RESPONSE`; a larger response is discarded rather than allowed to exhaust the heap |
| RIV-06 | The body is bulk-read from the socket rather than parsed byte-by-byte off the stream, because each `read()` is an lwip call and per-byte parsing of a 6 KB document is far slower |
| RIV-07 | HTTP/1.0 is requested so the server returns an unchunked body (TfL sends no `Content-Length`, so size can only be bounded while reading) |
| RIV-08 | Predictions arrive unordered and are sorted soonest-first before display |
| RIV-09 | Sailings further ahead than `RIVER_MAX_ETA_MINUTES` are discarded. The window is 120 minutes, not the buses' 30: RB6 headways reach 40 minutes, so a bus-sized window would leave the screen empty most of the day |
| RIV-10 | Duplicate predictions for one sailing (the same `vehicleId` listed at both of a pier's berths) are collapsed, so a duplicate cannot cost a real departure one of the three rows |
| RIV-11 | An unknown pier (HTTP 404) is a configuration error: logged once, retried only every 5 minutes, and the river screen withheld entirely |
| RIV-12 | Zero sailings is a valid result ("No boats due"), not a fetch failure. Verified live: Rotherhithe and Woolwich Ferry North both return `200 []` off-peak |
| RIV-13 | TfL names a pier only *inside* a prediction, so a pier with nothing due would identify itself only by its raw Naptan. The installer stores the friendly name it already showed the user (`rivername`), which the firmware falls back to |
| RIV-14 | The pier ID is percent-encoded into the request path |
| RIV-15 | A route filter (`riverline`) is matched case-insensitively, so `rb1` matches TfL's `RB1` |
| RIV-16 | `RAW_RIVER_DEBUG` build flag dumps parsed sailings for field verification |
| RIV-17 | Polling is no more frequent than `RIVER_REFRESH_SECONDS` (60 s) |

---

## 4. Configuration (on-device)

Settings are stored in NVS (namespace `esp32dep`) and set by the installer over
serial. **No settings are compiled into the binary**, so one firmware image is
generic and shareable.

The namespace keeps its original `esp32dep` spelling despite the rename to
Departure Buddy: it is a storage key, not a label. Changing it would orphan the
settings on every board already in the field, which would boot to "Awaiting
setup" having silently lost its WiFi, API key and station.

| Key | Required | Default | Description |
|---|---|---|---|
| `ssid` | Yes | — | WiFi network name (2.4 GHz) |
| `pass` | — | — | WiFi password |
| `key` | Yes | — | LDBWS consumer (API) key |
| `dep` | Yes | — | Departure station CRS code |
| `dest` | No | — | Destination filter CRS (empty = all) |
| `plat` | No | — | Platform filter (empty = all) |
| `tz` | No | UK | POSIX timezone string; the installer sets it from the PC's locale |
| `mode` | No | `train,bus` | Comma-separated set of services to show, e.g. `train,bus,river` |
| `bus` | No | - | TfL bus stop SMS code (empty = no bus screen at all) |
| `busline` | No | - | Bus route filter, e.g. `38` (empty = every route at the stop) |
| `river` | No | - | TfL pier Naptan **port** ID, e.g. `930GCAW` (empty = no river screen at all) |
| `riverline` | No | - | River route filter, e.g. `RB1` (empty = every route at the pier) |
| `rivername` | No | - | Friendly pier name, stored by the installer so a pier with nothing due still shows a name |
| `bstart` | No | `-1` | Screen-blank start hour — when the screen goes OFF (−1 = never blank). The installer offers `22` on a new board |
| `bend` | No | `-1` | Screen-blank end hour — when the screen comes back ON (−1 = never blank). The installer offers `6` on a new board |
| `bright` | No | `180` | Backlight brightness (0–255) |
| `colfg` | No | `-1` | Primary text colour, RGB565 (−1 = the compiled-in amber) |
| `coldim` | No | `-1` | Secondary / dimmed text colour, RGB565 |
| `colwarn` | No | `-1` | Cancellation and alert colour, RGB565 |
| `colbg` | No | `-1` | Background colour, RGB565 |
| `dwtrain` | No | `-1` | Seconds the train screen holds (−1 = `TRAIN_SCREEN_SECONDS`) |
| `dwbus` | No | `-1` | Seconds the bus screen holds |
| `dwriver` | No | `-1` | Seconds the river screen holds |
| `dwclock` | No | `-1` | Seconds the big-clock screen holds |
| `dwwx` | No | `-1` | Seconds the weather screen holds |
| `wlat` | No | unset | Weather latitude × 100000 (NVS has no float type) |
| `wlon` | No | unset | Weather longitude × 100000 |
| `wname` | No | - | Place label for the weather header |
| `nmode` | No | `-1` | Blank hours: `0` = screen off, otherwise a dimmed clock |
| `refr` | No | `60` | API poll interval (seconds) |

| ID | Requirement |
|---|---|
| CFG-01 | Config persisted in NVS; survives power cycles and firmware re-flash of the same layout |
| CFG-02 | Device is "provisioned" when `ssid` is set and at least one service is live (see CFG-07) |
| CFG-03 | Until provisioned, an "Awaiting setup" screen is shown and normal operation is suspended |
| CFG-04 | Reconfiguration is possible at any time over serial without re-flashing |
| CFG-05 | Each TfL screen is opt-in: with `bus` (or `river`) empty the firmware never contacts that feed and behaves exactly as a board without it |
| CFG-06 | `mode` is a **set**, so any combination of services can be on at once. It expresses **intent only** — a service is live when its mode allows it *and* its settings are present, so switching trains off keeps the API key and station stored for switching back |
| CFG-07 | A board is provisioned once it has WiFi and **at least one** live service. A boats-only board needs no API key and no station at all |
| CFG-08 | Legacy `mode` values are still honoured, so a board flashed before the river screen keeps working with its stored settings untouched: absent and `both` both mean `train,bus`; a lone `train` or `bus` is already a valid one-element set |
| CFG-09 | `mode` matching is on whole comma-separated tokens, never substrings, so a malformed value cannot switch on a service the user did not choose |
| CFG-10 | Colours and dwell times default to `-1` meaning **not set**, and `Config::pick()` falls back to the `app_config.h` value. A board provisioned before these existed therefore looks and behaves exactly as it did — the same mechanism `rivername` uses (RIV-13) |
| CFG-11 | Out-of-range values are treated as unset rather than applied: a colour outside 0–0xFFFF cannot be drawn, and dwell times are clamped to 3–300s so a stored value can never strobe the board or freeze a screen |
| CFG-12 | The weather position is stored as degrees × 100000 in two ints. NVS has no float type, and this keeps roughly a metre of precision — far more than a weather forecast resolves to |
| CFG-13 | `nmode` is the **one** setting whose unset default changes existing behaviour: a board upgrading with no `nmode` shows the dimmed night clock rather than going dark. Every other setting follows CFG-10 and keeps its old behaviour; this one is the point of the change, and a dark screen is strictly less useful than a dim clock |
| CFG-14 | A clock-only board is fully provisioned with no feed at all — the clock needs only WiFi, and only to set itself |

---

## 5. Display Behaviour

| ID | Requirement |
|---|---|
| DISP-01 | Show up to 3 next departures on the 320×170 canvas |
| DISP-02 | Each row: scheduled time, destination, and service status (expected/on-time/cancelled) with platform when present |
| DISP-03 | A destination too wide for its column scrolls horizontally, clipped so it never overwrites the status |
| DISP-04 | `HIDE_ONTIME_STATUS` build option: hide "On time" to give destinations the full width |
| DISP-05 | Cancellations rendered in red; delays shown as "Exp HH:MM" |
| DISP-06 | Large clock (HH:MM:SS) at the bottom in a **dot-matrix font**; train rows use a clear sans font |
| DISP-07 | Startup/attribution screen shown while WiFi + NTP come up |
| DISP-08 | Display blanks during configured blank hours (`bstart`/`bend`, wraps past midnight) |
| DISP-09 | Flicker-free rendering via a full-frame PSRAM sprite back-buffer at ~30 fps |
| DISP-10 | Clock time from NTP; timezone from the provisioned POSIX `tz` (set from the user's PC locale), falling back to UK. Set via `configTzTime` so DST applies |
| DISP-11 | An invalid departure CRS (API returns HTTP 400) shows a dedicated red "Unknown station" screen, not a generic connectivity error |
| DISP-12 | The board cycles through every live service in a fixed order — trains (30 s), buses (15 s), boats (15 s) — skipping those that are off; with one service it stays on that screen |
| DISP-13 | The bus screen shows up to 3 arrivals laid out like a train row: expected time, route number, destination, and a right-aligned countdown ("Due" under a minute, otherwise "N min") |
| DISP-14 | Bus countdowns tick down live between polls rather than freezing for the 30 s poll interval |
| DISP-15 | A TfL screen is only entered once TfL has answered successfully for that stop or pier at least once; an unconfigured or rejected one simply never joins the rotation |
| DISP-16 | Marquees reset on every screen change, so long names restart rather than resuming mid-scroll |
| DISP-17 | The header shows the station/stop/pier name in the large font, with a small dim "TRAIN" / "BUS" / "RIVER" (or "BUS <route>" / "RIVER <route>") tag beside it |
| DISP-18 | All three boards share one layout: a header row (mode tag + station/stop/pier name), three identical rows, then the clock. Buses and boats share the row renderer outright — a boat's "RB1" sits where a bus's "38" does — and differ only in tag and empty-state text |
| DISP-19 | All three rows use the same font; times and statuses use the smaller font, vertically centred, so the destination column gets the width |
| DISP-20 | A board with no train screen and no TfL answer yet shows a "Loading arrivals..." splash, not an empty departure board for a station that was never configured |
| DISP-21 | The river screen shows up to 3 sailings — expected time, route ("RB1"), destination pier, right-aligned countdown — and its countdowns tick down live between polls exactly as the bus screen's do |
| DISP-22 | If the screen currently displayed drops out of the rotation (its feed starts failing, or the user switches it off), the board moves to the first screen still in it rather than rendering one nothing feeds |
| DISP-23 | The big clock fills the panel with `HH:MM` in the provisioned palette, with a flip-clock seam drawn in the background colour so it reads as a fold rather than a line laid over the digits |
| DISP-24 | During blank hours the board shows that clock dimmed to `NIGHT_BRIGHTNESS` rather than going dark, unless `nmode` says otherwise |
| DISP-25 | The night clock drifts a few pixels every `NIGHT_DRIFT_SECONDS`. Blank hours run for eight hours with three of the four digits unchanging, so moving it keeps any one pixel from being lit all night — cheap insurance even on an IPS panel |
| DISP-26 | The weather screen reuses the shared header and clock. Its body must fit between the header and `CLOCK_Y`, which is why the temperature is sized down from the mockup's first attempt — it collided with the clock |
| DISP-27 | The clock screen needs no feed, so unlike the others it joins the rotation the moment it is enabled rather than waiting for data |
| DISP-28 | The weather screen carries an icon for the current conditions, drawn from primitives rather than shipped as bitmaps: it costs no flash worth counting, scales to any size, and takes the theme colour for free |
| DISP-29 | Overlapping shapes in a weather icon are separated by a **knockout** — the cloud drawn once oversized in the background colour, then again at true size in the foreground. On a single-colour panel two overlapping filled shapes otherwise fuse, and "sun behind cloud" comes out looking like a snowman |
| DISP-30 | Drizzle uses straight drops and rain slanted ones, so the two are distinguishable at icon size; fog bars sit clear of the cloud base, which they otherwise fuse with into a barcode |
| DISP-31 | The weather detail rows use the bold face at full brightness, not `DIM`. That colour is for the mode tag; using it for real readings made them the hardest thing on the board to read |

---

## 6. Provisioning Protocol (USB serial)

Newline-terminated line protocol on the USB CDC serial port (`src/config.cpp`).

| ID | Requirement |
|---|---|
| PROV-01 | `PING` → `PONG Departure Buddy` (discovery/handshake). The installer matches the bare `PONG` token, never the product name after it, so the banner is informational — a rename does not stop a new installer recognising an old board, or an old installer a new one |
| PROV-02 | `CFG <key>=<value>` → `ACK <key>` (stages a value) |
| PROV-03 | `COMMIT` → `SAVED`, then the device saves to NVS and reboots |
| PROV-04 | `GET` → current config as `key=value` lines, then `END`. Reports `dep`, `dest`, `plat`, `bus`, `busline`, `river`, `riverline`, `rivername`, `mode`, `ssid`, `passlen`, `bstart`, `bend`, `bright`, `refr`, `wifi`, `prov` |
| PROV-05 | Protocol available whether provisioned or not, so reconfiguration always works |
| PROV-06 | Host opens serial with `dtr=True, rts=False` to avoid resetting the ESP32-S3 |
| PROV-07 | `GET` never returns a secret: the API key is not reported at all and the WiFi password only as `passlen` (its length), which distinguishes an empty or truncated password from a wrong one |
| PROV-08 | `SCAN` → one `SSID|rssi=|ch=|auth=` line per visible network, then `END`. The ESP32-S3 has no 5 GHz radio, so a network missing here but visible on a phone is the usual explanation for a board that will not connect |
| PROV-09 | A `COMMIT` stages on top of the current config, so **omitting** a key preserves its stored value — the mechanism INST-11 relies on to avoid retyping secrets |
| PROV-10 | `GET` reports a legacy `mode` as the set it means (`train,bus`), so the installer only ever has to understand the comma-separated form |
| PROV-11 | The protocol is transport-agnostic: the same `PING`/`CFG`/`COMMIT` exchange is driven by the Python installer over pyserial and by the web configurator over Web Serial, with no firmware-side difference |

---

## 7. Installer (`DepartureBuddyInstaller.exe`)

A self-contained Windows 10/11 executable (`installer/installer.py`, packaged with
PyInstaller) that flashes the firmware and provisions the board.

| ID | Requirement |
|---|---|
| INST-01 | Single `.exe`; bundles firmware binaries, esptool, and pyserial — no Python/toolchain on the target PC |
| INST-02 | Auto-detects the board's COM port (prefers Espressif VID 0x303A); prompts if several devices |
| INST-03 | Console wizard prompts for all settings with sensible defaults; WiFi password entry hidden |
| INST-04 | Detects existing Departure Buddy firmware and offers three paths: change settings only, **update firmware only** (asks nothing, keeps all settings), or both |
| INST-05 | Flashes bootloader (0x0), partitions (0x8000), boot_app0 (0xe000), firmware (0x10000) via esptool |
| INST-06 | After flashing, re-detects the (possibly re-enumerated) port before provisioning |
| INST-07 | Provisions over serial using the PROV protocol; confirms `SAVED` |
| INST-08 | `--auto <cfg.json>` non-interactive mode for testing/automation. A key absent from the JSON keeps the board's existing value (matching INST-11); an explicit `""` clears it |
| INST-09 | Never persists entered secrets to disk; they go straight to the device |
| INST-10 | The wizard pre-fills every prompt from the board's current config, read back over `GET` |
| INST-11 | The WiFi password and API key — the only two values the board will not read back — accept a blank answer meaning "keep"; `provision()` then omits that key so the firmware's COMMIT preserves it |
| INST-12 | Re-flashing the app does not disturb settings: they live in a separate NVS partition |
| INST-13 | **Bus stop search:** one prompt accepts a postcode, a place/stop name, or a 5-digit stop code, and works out which was entered rather than making the user pick a search mode first |
| INST-14 | A place or postcode resolves to coordinates, then to a list of nearby stops showing the **direction each serves** (`Towards`) and its distance, so the user can pick the correct side of the road |
| INST-15 | Name matches that share a name are the same place indexed several times (one per station entrance) and are searched together, not offered as identical-looking choices |
| INST-16 | The chosen stop is verified against the live feed and the next few arrivals printed, so a wrong stop is caught before anything is written to the board |
| INST-17 | A blank answer at the stop search **keeps** an already-configured stop; only a board with no stop treats blank as "skip" |
| INST-18 | Screen hours are echoed back as the resulting **ON** window with its duration, and any ON window shorter than half a day must be confirmed. ON/OFF entered the wrong way round (22 then 6) still leaves 8 hours, which looks deliberate on its own but almost never is |
| INST-19 | The wizard runs in two parts: **Part 1 (board basics)** — WiFi, screen hours, refresh, brightness, timezone — then **Part 2 (what the board shows)**. Everything every board needs is settled before any service-specific question |
| INST-20 | A single-service board must have that service's stop or pier: the prompt repeats until one is given, since declining would leave nothing at all to display |
| INST-21 | Services are chosen as a **multi-select** ("1,3"), and only the chosen ones are then asked about. A service switched off has its keys **omitted rather than cleared**, so switching it back on later costs no retyping |
| INST-22 | Piers are chosen **by name from a list**, never by typing a Naptan ID. The list is fetched live from TfL and merged across all four river lines, with a baked-in fallback so setup still works offline |
| INST-23 | After a stop or pier is chosen the installer prints the next few arrivals from it, so a wrong choice is caught before anything is written to the board |
| INST-24 | The installer stores the pier's display name alongside its ID (`rivername`), satisfying RIV-13 without the firmware needing a second request |
| INST-25 | Screen hours are asked **ON first, then OFF** ("on at 6, off at 22"), which is how people describe them, and converted to the firmware's `bstart`/`bend` blank-window form on the way out |
| INST-26 | A board that has never been configured defaults to **on 06:00–22:00** rather than never blanking; an already-configured board keeps its own hours as the default, including an explicit "never blank" |
| INST-27 | A service is opted into **once**, in Part 2. The stop and pier sections ask *which*, never *whether* — and a service chosen there but then left without a stop or pier is pruned from `mode`, so the board is never enabled for a screen with nothing behind it |
| INST-28 | Pier names are folded to ASCII before being shown or stored: TfL returns "St Mary's Wandsworth Pier" with a U+2019 quote, which the board's font cannot draw |
| INST-29 | The exe is built by CI on a tag and published as a GitHub Release, with a SHA-256 beside it. The firmware is **compiled from that tag's source** in the same job rather than taken from whatever is committed in `installer/firmware/`, so a release always ships an installer and a firmware image from one commit |
| INST-30 | The exe is unsigned, so Windows SmartScreen warns about it. A code-signing certificate costs more per year than the hardware; the published checksum is the alternative, and the warning is documented rather than left to surprise people |

---

## 8. Architectural Resilience

| ID | Requirement |
|---|---|
| ARCH-01 | API fetch uses exponential back-off on failure: 2s → 4s → 8s → … → 120s cap |
| ARCH-02 | On API failure, the last-known departures stay on screen with a "No signal (Nx)" overlay |
| ARCH-03 | After 3 consecutive failures, a dedicated connectivity-warning screen is shown |
| ARCH-04 | No data yet → a dim "No departures" / "No buses due" message under the header, never a blank screen or a crash. (Replaced the earlier "Welcome to <station>" screen, which duplicated the station name now shown in the header) |
| ARCH-05 | Two-core split: fetch task on core 0, render loop on core 1 |
| ARCH-06 | Shared departure state guarded by a FreeRTOS mutex; render never fetches, fetch never draws |
| ARCH-07 | Fetch TLS task given a 16 KB stack (mbedTLS handshakes are stack-hungry) |
| ARCH-08 | WiFi auto-reconnect handled within the fetch task |
| ARCH-09 | Serial provisioning polled from the main loop and during WiFi waits, so the board stays responsive |
| ARCH-10 | An invalid station (HTTP 400) is treated as a configuration error, not a transient one: dedicated error screen and a slow 30s retry rather than exponential-backoff hammering |
| ARCH-11 | The installer verifies the station + key against the API before provisioning and re-prompts on rejection |
| ARCH-12 | Both feeds are polled from the **one** fetch task on independent deadlines — a second task would need its own 16 KB TLS stack for no benefit |
| ARCH-13 | The scheduler compares deadlines as signed differences, so it survives `millis()` wrapping |
| ARCH-14 | The bus feed has its own error counter and back-off, independent of the rail feed |
| ARCH-15 | A feed the user has switched off is never polled at all |

---

## 8a. Web Configurator

A static single-page app (`web/`) published to GitHub Pages, which configures a
board either directly over Web Serial or by exporting a settings file for the
installer.

| ID | Requirement |
|---|---|
| WEB-01 | **No backend.** The page is static files only: no server, no database, no telemetry. Every lookup is a direct browser call to a third-party API |
| WEB-02 | This is possible because every API used is CORS-open — TfL Unified and Countdown and postcodes.io send `Access-Control-Allow-Origin: *`, and Rail Data Marketplace reflects the requesting origin |
| WEB-03 | Requests carry **no custom headers**. Adding one makes them non-simple, and the TfL endpoints answer the GET but not the resulting CORS preflight, so the call fails. The `.exe` sends a `User-Agent` because it is not bound by CORS; the page must not |
| WEB-04 | Secrets never leave the browser. The WiFi password and API key are held in memory and written only to the device over USB, or to a file the user explicitly downloads |
| WEB-05 | The page live-validates the train API key and CRS against the real endpoint before the user connects a board, so a typo is caught with no hardware involved |
| WEB-06 | Stop, pier and postcode lookups are ports of the `installer.py` functions and must resolve a given search to the same result, so both front-ends configure a board identically |
| WEB-07 | Station search uses a baked `data/stations.json`: no CORS-friendly UK-wide station search exists, so the lookup has to be local |
| WEB-08 | The preview shows colours **round-tripped through RGB565**, since the panel is 16-bit and promising a shade it cannot draw would make the preview a lie |
| WEB-09 | Form state is derived from the DOM, not tracked separately. Browsers restore control state on reload, and a separately-tracked copy would silently disagree with what the user sees — exporting a config for services no longer shown as selected |
| WEB-10 | Web Serial is feature-detected. Where it is missing (Firefox, Safari, mobile) the page falls back to the downloadable settings file and says so plainly |
| WEB-11 | After `COMMIT` the board reboots and, being native USB CDC, its port re-enumerates as a new device — invalidating the held handle. The page waits for reconnection and degrades to a plain success message rather than reporting a failure |
| WEB-12 | A downloaded settings file contains the WiFi password in plain text. The page says so where the download is offered; this is the documented exception to SEC-05 |

---

## 8b. Weather (Open-Meteo, optional)

| Item | Detail |
|---|---|
| **API** | [Open-Meteo](https://open-meteo.com) forecast endpoint |
| **Endpoint** | `https://api.open-meteo.com/v1/forecast` |
| **Authentication** | **None** — no key and no registration, matching the TfL feeds |
| **Data returned** | Temperature, apparent temperature, WMO weather code, wind speed, today's max and min |
| **Response size** | ~700 bytes — by far the smallest of the four feeds |
| **Freshness** | The response carries `interval: 900`, so polling faster than 15 minutes returns identical data |

| ID | Requirement |
|---|---|
| WX-01 | HTTPS enforced; no HTTP fallback |
| WX-02 | No key is required, so nothing sensitive travels on the request |
| WX-03 | Units are requested rather than converted: Celsius is the default and `wind_speed_unit=mph` gives the usual British pairing, so the firmware never does arithmetic it could have asked for |
| WX-04 | Parsed with an ArduinoJson filter, so only the seven numbers drawn are ever allocated |
| WX-05 | WMO codes are mapped to plain words, grouped rather than exhaustive — the board has one line for it, and nobody plans a morning around drizzle intensity. An unrecognised code shows nothing rather than a guess |
| WX-06 | Polling is no more frequent than `WEATHER_REFRESH_SECONDS` (900s), matching the feed's own update interval |
| WX-07 | Coordinates rejected by the feed (HTTP 400) are a configuration error: logged once, retried every 5 minutes, and the weather screen withheld |
| WX-08 | A failure keeps the last reading on screen and backs off, exactly as the transport feeds do. Weather must never take down a departure board |
| WX-09 | The location is **never asked for**: it is derived from whatever station, stop or pier the user already chose, all of which carry coordinates |
| WX-10 | Temperatures are shown as whole degrees — a tenth is noise at a glance, and the row font is wide enough that the extra character costs more than it says |

---

## 8c. Buttons

The board has two tactile buttons on its front edge, `BUTTON_1` (GPIO0, the BOOT
pin) and `BUTTON_2` (GPIO14).

| ID | Requirement |
|---|---|
| BTN-01 | The **clock** button puts the big clock on screen and holds it there; pressing it again resumes the rotation |
| BTN-02 | The **next** button steps immediately to the next panel and restarts that panel's dwell, so it gets its full time rather than the tail of the one it interrupted |
| BTN-03 | Stepping implies leaving a held clock: the point of that button is "show me the boards" |
| BTN-04 | The held clock works whether or not the clock is one of the chosen screens — it is what the button was pressed for |
| BTN-05 | Presses are debounced by requiring the level to be stable for 40ms. These are bare tactile switches; without it one push registers several times |
| BTN-06 | Either button wakes the board from night mode to normal brightness for `NIGHT_WAKE_SECONDS`, then it settles back to the dimmed clock on its own. A press that does nothing reads as broken hardware; staying lit because someone brushed it at 3am defeats the point of blank hours |
| BTN-07 | GPIO0 is physically the **lower** button in this rotation and GPIO14 the upper — the opposite of what the pin numbering suggests. The constants are therefore named for what they do (`PIN_BTN_CLOCK`, `PIN_BTN_NEXT`) rather than where they sit |
| BTN-08 | GPIO0 is a strapping pin, special only while the chip is resetting; at runtime it is an ordinary input. Both use the internal pull-up and are active-low |

---

## 9. Security Requirements

| ID | Requirement |
|---|---|
| SEC-01 | The API key is **not** compiled into the firmware binary — it lives only in on-device NVS |
| SEC-02 | HTTPS enforced for LDBWS; TLS used for all API traffic |
| SEC-03 | The API key is never printed to serial (masked in `GET`) |
| SEC-04 | No credentials committed to source; `secrets.h` is git-ignored and unused at runtime |
| SEC-05 | Installer keeps entered secrets in memory only; nothing written to disk on the target PC. **Documented exception:** a settings file exported from the web configurator carries the WiFi password and API key by design, so the file alone can set a board up — the page warns about it at the point of download (WEB-12) |
| SEC-06 | TLS server-certificate verification is a documented hardening option (`setCACert`); default `setInsecure()` is called out as a trade-off |
| SEC-07 | Both TfL requests (bus and river) carry no credentials of any kind, so nothing sensitive is exposed by them |
| SEC-08 | The bus stop code and route filter are percent-encoded into the query string, and the pier ID into the request path, so a stray character cannot alter either request |

---

## 10. Fonts

| ID | Requirement |
|---|---|
| FONT-01 | Clock rendered in a dot-matrix font converted from `fonts/*.ttf` |
| FONT-02 | Conversion tool `docs/ttf_to_lgfx.py` emits an `lgfx::GFXfont` header (`include/dotmatrix_fonts.h`) |
| FONT-03 | Train rows use LovyanGFX built-in FreeSans for legibility (per user preference over full dot-matrix) |

---

## 11. Out of Scope

- Journey planning, ticket purchasing, arrivals boards
- Buses outside London (the TfL feed is London-only)
- More than one bus stop, or TfL's streaming interface (which needs authentication)
- 5 GHz WiFi (unsupported by the ESP32-S3 radio)
- macOS / Linux installer builds (firmware is cross-platform buildable; the packaged installer targets Windows)
- OTA firmware updates (re-flash via the installer)
- A second display / dual-screen mode
- The Pi version's web configuration portal (replaced by the USB installer)
