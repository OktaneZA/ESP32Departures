# Esp32Departures — Requirements

## Overview

**Esp32Departures** is a UK National Rail departure board that runs on a **LilyGo
T-Display-S3** (ESP32-S3) microcontroller. It fetches live departures over WiFi
directly from the National Rail Live Departure Board (LDBWS) JSON API and renders
them on the board's built-in 170×320 colour LCD — no host computer, server, or
cloud service.

This document is a **retrospective** specification: it records the requirements
the delivered firmware and installer actually satisfy.

### Attribution / lineage

- **Original project:** [chrisys/train-departure-display](https://github.com/chrisys/train-departure-display)
  by Chris Crocker-White — the origin of the departure-board concept, layout, and
  the dot-matrix fonts reused here (`fonts/`).
- **Native Raspberry Pi rewrite:** [OktaneZA/PiDepartures](https://github.com/OktaneZA/PiDepartures)
  — replaced Balena/Docker with a native systemd deployment and the OpenLDBWS
  SOAP client. This ESP32 project began life inside that repo before being split out.
- **This project (Esp32Departures):** a from-scratch C++ port of that behaviour to
  the ESP32-S3, replacing Python/SOAP with Arduino/JSON and adding a self-contained
  Windows installer that provisions the board over USB.

### Key differences from the Pi version

- Runs on a **£15–25 ESP32-S3 board**, not a Raspberry Pi; no OS, no SD card.
- **Colour LCD (170×320)** instead of the 256×64 mono OLED.
- Data via the **Rail Data Marketplace LDBWS REST/JSON API**, not OpenLDBWS SOAP
  (the legacy SOAP token no longer works; RTT's API is also being retired).
- Configuration stored **on-device (NVS)** and set over USB serial by a bundled
  **Windows `.exe` installer** — one pre-built binary serves every user.

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

## 4. Configuration (on-device)

Settings are stored in NVS (namespace `esp32dep`) and set by the installer over
serial. **No settings are compiled into the binary**, so one firmware image is
generic and shareable.

| Key | Required | Default | Description |
|---|---|---|---|
| `ssid` | Yes | — | WiFi network name (2.4 GHz) |
| `pass` | — | — | WiFi password |
| `key` | Yes | — | LDBWS consumer (API) key |
| `dep` | Yes | — | Departure station CRS code |
| `dest` | No | — | Destination filter CRS (empty = all) |
| `plat` | No | — | Platform filter (empty = all) |
| `tz` | No | UK | POSIX timezone string; the installer sets it from the PC's locale |
| `bstart` | No | `-1` | Screen-blank start hour (−1 = off) |
| `bend` | No | `-1` | Screen-blank end hour (−1 = off) |
| `bright` | No | `180` | Backlight brightness (0–255) |
| `refr` | No | `60` | API poll interval (seconds) |

| ID | Requirement |
|---|---|
| CFG-01 | Config persisted in NVS; survives power cycles and firmware re-flash of the same layout |
| CFG-02 | Device is "provisioned" only when `ssid`, `key`, and `dep` are all set |
| CFG-03 | Until provisioned, an "Awaiting setup" screen is shown and normal operation is suspended |
| CFG-04 | Reconfiguration is possible at any time over serial without re-flashing |

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

---

## 6. Provisioning Protocol (USB serial)

Newline-terminated line protocol on the USB CDC serial port (`src/config.cpp`).

| ID | Requirement |
|---|---|
| PROV-01 | `PING` → `PONG Esp32Departures` (discovery/handshake) |
| PROV-02 | `CFG <key>=<value>` → `ACK <key>` (stages a value) |
| PROV-03 | `COMMIT` → `SAVED`, then the device saves to NVS and reboots |
| PROV-04 | `GET` → current config with secrets masked, then `END` |
| PROV-05 | Protocol available whether provisioned or not, so reconfiguration always works |
| PROV-06 | Host opens serial with `dtr=True, rts=False` to avoid resetting the ESP32-S3 |

---

## 7. Installer (`Esp32DeparturesInstaller.exe`)

A self-contained Windows 10/11 executable (`installer/installer.py`, packaged with
PyInstaller) that flashes the firmware and provisions the board.

| ID | Requirement |
|---|---|
| INST-01 | Single `.exe`; bundles firmware binaries, esptool, and pyserial — no Python/toolchain on the target PC |
| INST-02 | Auto-detects the board's COM port (prefers Espressif VID 0x303A); prompts if several devices |
| INST-03 | Console wizard prompts for all settings with sensible defaults; WiFi password entry hidden |
| INST-04 | Detects existing Esp32Departures firmware and offers **Configure only** (no re-flash) |
| INST-05 | Flashes bootloader (0x0), partitions (0x8000), boot_app0 (0xe000), firmware (0x10000) via esptool |
| INST-06 | After flashing, re-detects the (possibly re-enumerated) port before provisioning |
| INST-07 | Provisions over serial using the PROV protocol; confirms `SAVED` |
| INST-08 | `--auto <cfg.json>` non-interactive mode for testing/automation |
| INST-09 | Never persists entered secrets to disk; they go straight to the device |

---

## 8. Architectural Resilience

| ID | Requirement |
|---|---|
| ARCH-01 | API fetch uses exponential back-off on failure: 2s → 4s → 8s → … → 120s cap |
| ARCH-02 | On API failure, the last-known departures stay on screen with a "No signal (Nx)" overlay |
| ARCH-03 | After 3 consecutive failures, a dedicated connectivity-warning screen is shown |
| ARCH-04 | No data yet → a welcome/loading screen (never a blank or crash) |
| ARCH-05 | Two-core split: fetch task on core 0, render loop on core 1 |
| ARCH-06 | Shared departure state guarded by a FreeRTOS mutex; render never fetches, fetch never draws |
| ARCH-07 | Fetch TLS task given a 16 KB stack (mbedTLS handshakes are stack-hungry) |
| ARCH-08 | WiFi auto-reconnect handled within the fetch task |
| ARCH-09 | Serial provisioning polled from the main loop and during WiFi waits, so the board stays responsive |
| ARCH-10 | An invalid station (HTTP 400) is treated as a configuration error, not a transient one: dedicated error screen and a slow 30s retry rather than exponential-backoff hammering |
| ARCH-11 | The installer verifies the station + key against the API before provisioning and re-prompts on rejection |

---

## 9. Security Requirements

| ID | Requirement |
|---|---|
| SEC-01 | The API key is **not** compiled into the firmware binary — it lives only in on-device NVS |
| SEC-02 | HTTPS enforced for LDBWS; TLS used for all API traffic |
| SEC-03 | The API key is never printed to serial (masked in `GET`) |
| SEC-04 | No credentials committed to source; `secrets.h` is git-ignored and unused at runtime |
| SEC-05 | Installer keeps entered secrets in memory only; nothing written to disk on the target PC |
| SEC-06 | TLS server-certificate verification is a documented hardening option (`setCACert`); default `setInsecure()` is called out as a trade-off |

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
- 5 GHz WiFi (unsupported by the ESP32-S3 radio)
- macOS / Linux installer builds (firmware is cross-platform buildable; the packaged installer targets Windows)
- OTA firmware updates (re-flash via the installer)
- A second display / dual-screen mode
- The Pi version's web configuration portal (replaced by the USB installer)
