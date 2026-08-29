# Departure Buddy — technical detail

Everything that used to crowd the [README](README.md): building the firmware,
how it maps to the original Raspberry Pi app, the project layout, and the
hard-won notes about the feeds it talks to.

For the formal specification — every requirement, with IDs — see
[REQUIREMENTS.md](REQUIREMENTS.md). For the web configurator specifically, see
[web/README.md](web/README.md).

## What it does

- Live departures for a station (optionally filtered to a destination or platform)
- **Trains, buses, boats — any combination** — you choose at setup. The board
  cycles through whatever is enabled (trains 30s, buses 15s, boats 15s); with a
  single service it stays on that screen
- **London bus arrivals** for one stop from TfL's open Countdown feed — expected
  time, route number, destination, and a "Due" / "N min" countdown that ticks
  between polls
- **River boat sailings** for one Thames pier from TfL's open Unified API —
  Uber Boat by Thames Clippers (RB1/RB4/RB6) and the Woolwich Ferry, drawn on
  the same layout as the bus screen with the route ("RB1") where the bus number
  goes. Real live predictions, not a timetable
- Top three departures: time + destination, with status (delay/cancellation)
  and platform when relevant; long names scroll
- **Your own colours and timings** — theme the board (classic amber, white,
  phosphor green, high contrast, or anything you pick) and choose how long each
  screen holds, all without recompiling
- Big NTP clock in a dot-matrix font, with automatic BST
- Exponential back-off on API failure, stale data kept on screen with a
  "No signal" indicator, and a connectivity-warning screen after 3 failures
- Optional screen-blank hours; runtime-adjustable brightness

## Build from source (developers only)

You only need this to modify the firmware. Configuration is **not** compiled in —
the board is provisioned at runtime (by the installer, or the serial protocol in
`src/config.cpp`), so there is **no `secrets.h` to edit**.

1. Install **[PlatformIO](https://platformio.org/install)** (VS Code extension or
   the `pio` CLI).
2. Build / flash / monitor:
   ```bash
   pio run                 # compile
   pio run -t upload       # flash over USB-C
   pio device monitor      # serial logs (115200)
   ```
3. Provision the freshly-flashed board with the installer, or by sending the
   serial protocol directly (`PING` / `CFG key=value` / `COMMIT`). `GET` reports
   the current config (secrets never echoed — the WiFi password only as
   `passlen`) and `SCAN` lists the networks the board's own radio can see, which
   is usually the fastest way to diagnose a board that will not connect.

Compile-time tunables live in `include/app_config.h`: layout, back-off, time
window, `HIDE_ONTIME_STATUS`, `RAW_JSON_DEBUG`, the bus settings
(`TRAIN_SCREEN_SECONDS`, `BUS_SCREEN_SECONDS`, `BUS_REFRESH_SECONDS`,
`MAX_BUS_ARRIVALS`, `RAW_BUS_DEBUG`) and the river settings
(`RIVER_SCREEN_SECONDS`, `RIVER_REFRESH_SECONDS`, `MAX_RIVER_ARRIVALS`,
`RIVER_MAX_ETA_MINUTES`, `RAW_RIVER_DEBUG`). After a firmware change, refresh
the installer's bundled binary: copy `.pio/build/lilygo-t-display-s3/firmware.bin`
into `installer/firmware/` and run `installer/build_exe.py`.

## How it maps to the Pi app

| Pi app (Python)                         | This firmware (C++)                          |
|-----------------------------------------|----------------------------------------------|
| OpenLDBWS SOAP + `xmltodict`            | LDBWS REST/JSON + ArduinoJson (`rail_api.cpp`) |
| `luma.oled` SSD1322 256×64 mono         | LovyanGFX ST7789 320×170 colour (`display.cpp`) |
| render thread + fetch thread + `Lock`   | `loop()` (core 1) + fetch task (core 0) + mutex |
| exponential back-off (ARCH-01)          | `backoffMs()` in `main.cpp`                  |
| stale data + "No signal" (ARCH-02)      | `errCount` overlay in `renderBoard()`        |
| connectivity warning (ARCH-03)          | `renderConnectivityWarning()`                |
| blank hours (DISP-05)                   | `isBlankHour()`                              |
| config file + install.sh                | on-device NVS + USB installer (`config.cpp`) |
| PIL dot-matrix TTF fonts                | dot-matrix clock via `docs/ttf_to_lgfx.py` (rows use FreeSans) |
| (no bus support)                        | optional TfL bus screen (`bus_api.cpp`), cycled from `loop()` |
| (no river support)                      | optional TfL river screen (`river_api.cpp`), same rotation |

## Project layout

```
ESP32Departures/
├── platformio.ini            PlatformIO env, board, libs
├── REQUIREMENTS.md           canonical requirements + attribution
├── include/
│   ├── app_config.h          compile-time tunables
│   ├── config.h              on-device (NVS) runtime config
│   ├── dotmatrix_fonts.h     generated dot-matrix font (clock)
│   ├── model.h / rail_api.h / bus_api.h / river_api.h / display.h
├── src/
│   ├── main.cpp              WiFi, NTP, fetch task, render loop
│   ├── config.cpp            NVS config + USB-serial provisioning
│   ├── rail_api.cpp          National Rail LDBWS REST/JSON client
│   ├── bus_api.cpp           TfL live bus arrivals (Countdown/URA) client
│   ├── river_api.cpp         TfL live river sailings (Unified API) client
│   └── display.cpp           LovyanGFX panel config + rendering
├── docs/                     mockup renderer + font-conversion script
├── web/                      the web configurator (static, hosted on Azure)
│   ├── index.html            the form, preview and installer UI
│   ├── js/config.js          config model, mode set, RGB565 colour maths
│   ├── js/api.js             TfL / postcode / rail lookups (ports of installer.py)
│   ├── js/serial.js          Web Serial provisioning (PING/CFG/COMMIT)
│   ├── js/flash.js           flashing a blank board via esptool-js
│   ├── build-firmware.py     stages firmware + writes its manifest
│   ├── vendor/               esptool-js, byte-identical to upstream
│   └── data/stations.json    2,608 UK stations: CRS, name, position
└── installer/                self-contained Windows installer (.exe)
```

## Notes

- **Verify field names on first run:** the LDBWS JSON serialises the SOAP schema;
  `destination` and `subsequentCallingPoints` vary in nesting. `rail_api.cpp`
  parses both known shapes; set `RAW_JSON_DEBUG 1` in `app_config.h` to print a
  raw response and confirm, then set it back.
- **API route:** if your subscription's "API" tab shows a base path other than
  `1010-live-departure-board-dep1_2/...`, update `kBase` in `rail_api.cpp`.
- **TLS:** `rail_api.cpp` calls `client.setInsecure()` — encrypted but the server
  certificate isn't verified. For full protection paste `api1.raildata.org.uk`'s
  root CA and switch to `client.setCACert(...)` (a hook is already in place).
- **Calling points** arrive in the same response but aren't shown in the current
  top-three layout (`FETCH_CALLING_POINTS 0`).
- **London buses** come from TfL's
  [Live Bus & River Bus Arrivals API](https://content.tfl.gov.uk/tfl-live-bus-river-bus-arrivals-api-documentation.pdf)
  (the Countdown "URA" feed), which needs no key. Its responses are one JSON
  array per line, and fields come back in the spec's own sequence order rather
  than the order requested — `bus_api.cpp` documents the exact shape it relies
  on, and `RAW_BUS_DEBUG` in `app_config.h` prints raw lines to check it.
  Screen timings are `TRAIN_SCREEN_SECONDS` / `BUS_SCREEN_SECONDS`.
  Data provided by Transport for London.
- **River boats** come from TfL's Unified API `river-bus` mode
  (`https://api.tfl.gov.uk/StopPoint/{pier}/Arrivals`), which also needs no key.
  Despite its name, the Countdown/URA feed the bus screen uses returns *only*
  buses — piers are not in it — which is why this is a separate client. Note
  the pier ID must be the **port** (`930GCAW`), not one of the berths
  (`9300CAW1`): a berth sees only half its pier's sailings. `timeToStation` is
  already relative, so the countdown is right even before NTP has synced.
  RB2 (Tate to Tate) is not published on this feed. Screen timing is
  `RIVER_SCREEN_SECONDS`. Data provided by Transport for London.
- **The web configurator needs no backend.** Every API it uses is CORS-open, so
  it is plain static hosting: TfL Unified and Countdown send
  `Access-Control-Allow-Origin: *`, postcodes.io the same, and Rail Data
  Marketplace reflects the requesting origin — which even lets the page check
  your API key before you plug anything in. One catch worth knowing if you
  extend it: adding *any* custom request header makes those calls non-simple,
  and the TfL endpoints answer the GET but not the CORS preflight, so the
  request fails. `web/js/api.js` deliberately sends none.
- **Station positions** come from NaPTAN (Open Government Licence v3); CRS codes
  are cross-checked against it, with 2,606 of 2,608 confirmed to within 2km.
