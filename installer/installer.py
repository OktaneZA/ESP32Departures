"""Esp32Departures installer — console wizard.

Flashes the bundled firmware to a LilyGo T-Display-S3 and configures it (WiFi,
National Rail LDBWS key, station, filters, an optional London bus stop, blank
hours, brightness) over USB serial. One pre-built binary; all settings are
written to the board at runtime, so no toolchain is needed on the user's PC.

Packaged into a single Windows .exe with PyInstaller (see build_exe.py).

Usage:
    installer.exe                 interactive wizard
    installer.exe --auto cfg.json non-interactive (for testing/automation)
"""

import argparse
import json
import os
import re
import sys
import time

import serial
import serial.tools.list_ports

# ESP32-S3 flash layout (offset, filename) — captured from PlatformIO.
FLASH_LAYOUT = [
    (0x0000, "bootloader.bin"),
    (0x8000, "partitions.bin"),
    (0xE000, "boot_app0.bin"),
    (0x10000, "firmware.bin"),
]
ESPRESSIF_VID = 0x303A


def resource_path(rel):
    """Path to a bundled resource, whether running as a script or PyInstaller exe."""
    base = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(base, rel)


def fw(name):
    return resource_path(os.path.join("firmware", name))


def detect_tz():
    """POSIX TZ string for this PC's timezone (e.g. 'GMT0BST,M3.5.0/1,M10.5.0'),
    or '' if it can't be determined (the firmware then uses its UK default)."""
    try:
        import tzlocal
        key = tzlocal.get_localzone_name()
        if not key:
            return ""
        import importlib.resources as ir
        p = ir.files("tzdata.zoneinfo")
        for part in key.split("/"):
            p = p.joinpath(part)
        data = p.read_bytes()                               # TZif file
        return data.rsplit(b"\n", 2)[-2].decode("ascii", "ignore")  # POSIX footer
    except Exception:
        return ""


def validate_station(key, crs):
    """Best-effort online check of the CRS + key.
    Returns (status, detail) where status is 'ok' | 'bad_station' | 'bad_key' | 'net'."""
    import urllib.request
    import urllib.error
    url = ("https://api1.raildata.org.uk/1010-live-departure-board-dep1_2/LDBWS/"
           f"api/20220120/GetDepBoardWithDetails/{crs}?numRows=1&timeWindow=30")
    req = urllib.request.Request(
        url, headers={"x-apikey": key, "User-Agent": "Esp32Departures-installer"})
    try:
        with urllib.request.urlopen(req, timeout=12) as r:
            return ("ok", "") if r.status == 200 else ("net", f"HTTP {r.status}")
    except urllib.error.HTTPError as e:
        if e.code == 400:
            return ("bad_station", "invalid CRS code")
        if e.code in (401, 403):
            return ("bad_key", "API key rejected")
        return ("net", f"HTTP {e.code}")
    except Exception as e:
        return ("net", str(e))


def verify_config(cfg):
    """Re-prompt until the station (and key) are accepted online, or the check
    can't run (offline). Only warns in the offline case."""
    if cfg["key"] is None:
        # Keeping the board's existing key, which it never hands back, so there
        # is nothing to check with. The board itself will show "Unknown station".
        print("  (keeping the key already on the board - skipping the online check)")
        return
    while True:
        status, detail = validate_station(cfg["key"], cfg["dep"])
        if status == "ok":
            print(f"  station {cfg['dep']} OK")
            return
        if status == "bad_station":
            print(f"  ! Station '{cfg['dep']}' was rejected ({detail}). Check the CRS code.")
            cfg["dep"] = ask("Departure station CRS", cfg["dep"], required=True).upper()
        elif status == "bad_key":
            print(f"  ! API key rejected ({detail}).")
            cfg["key"] = ask("LDBWS API key", cfg["key"], required=True)
        else:
            print(f"  (couldn't verify online: {detail} - continuing)")
            return


# --------------------------------------------------------------------------- #
# London buses — TfL Countdown (URA) feed
#
# The bus feed is open: no key, no registration. Stops are identified by their
# 5-digit SMS code (the number printed on the stop). Most people don't know it,
# so the wizard finds it for them from a place name or a postcode.
# --------------------------------------------------------------------------- #
TFL_URA = "https://countdown.api.tfl.gov.uk/interfaces/ura/instant_V1"


def _http_json_lines(url, timeout=15):
    """GET a URA request and return its lines already parsed from JSON.
    Returns (status, lines) where status is 'ok' | 'bad_stop' | 'net'."""
    import urllib.request
    import urllib.error
    req = urllib.request.Request(
        url, headers={"User-Agent": "Esp32Departures-installer"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            body = r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        # 416 is the feed's way of saying "no such stop code".
        return ("bad_stop" if e.code == 416 else "net", [])
    except Exception:
        return ("net", [])
    out = []
    for line in body.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            out.append(json.loads(line))
        except ValueError:
            continue
    return ("ok", out)


def _haversine_m(lat1, lon1, lat2, lon2):
    import math
    r = 6371000.0
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp = math.radians(lat2 - lat1)
    dl = math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * r * math.asin(math.sqrt(a))


def stops_near(lat, lon, radius_m):
    """Bus stops within `radius_m` of a point, nearest first.

    URA returns fields in the *documented sequence order*, not the order they
    were requested, so this ReturnList comes back as
    [0, StopPointName, StopCode1, Towards, StopPointIndicator, Lat, Lon].
    Stops without a usable code are bus stands or withdrawn stops - not
    boardable, so they are dropped. The feed marks those either as null or as
    the literal string "NONE", hence the digits-only test."""
    url = (f"{TFL_URA}?Circle={lat:.6f},{lon:.6f},{int(radius_m)}&StopPointState=0"
           "&StopAlso=True&ReturnList=StopCode1,StopPointName,StopPointIndicator,"
           "Towards,Latitude,Longitude")
    status, lines = _http_json_lines(url)
    if status != "ok":
        return []
    stops = []
    for a in lines:
        if not isinstance(a, list) or len(a) < 7 or a[0] != 0:
            continue
        name, code, towards, indicator, slat, slon = a[1], a[2], a[3], a[4], a[5], a[6]
        if not code or not str(code).isdigit():
            continue
        stops.append({
            "code": str(code),
            "name": name or "",
            "towards": towards or "",
            "indicator": indicator or "",
            "distance": _haversine_m(lat, lon, float(slat), float(slon)),
        })
    stops.sort(key=lambda s: s["distance"])
    return stops


def stops_near_any(origins, radius_m):
    """Stops near any of several points, deduplicated by code, nearest first.

    A named place often matches several coordinates (TfL indexes each entrance
    of a station separately), and a single 250m circle around just one of them
    misses stops around the others."""
    merged = {}
    for lat, lon in origins:
        for s in stops_near(lat, lon, radius_m):
            prev = merged.get(s["code"])
            if prev is None or s["distance"] < prev["distance"]:
                merged[s["code"]] = s
    return sorted(merged.values(), key=lambda s: s["distance"])


def geocode_place(query):
    """Look up a place or stop name via TfL's own stop search.
    Returns a list of {name, lat, lon} candidates (may be empty)."""
    import urllib.parse
    import urllib.request
    url = ("https://api.tfl.gov.uk/StopPoint/Search?query="
           + urllib.parse.quote(query) + "&modes=bus&maxResults=6")
    req = urllib.request.Request(url, headers={"User-Agent": "Esp32Departures-installer"})
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            d = json.loads(r.read().decode("utf-8", "replace"))
    except Exception:
        return []
    out = []
    for m in d.get("matches", []):
        if m.get("lat") and m.get("lon"):
            out.append({"name": m.get("name", query),
                        "lat": float(m["lat"]), "lon": float(m["lon"])})
    return out


def geocode_postcode(postcode):
    """Lat/lon for a UK postcode via the free postcodes.io service, or None."""
    import urllib.parse
    import urllib.request
    url = "https://api.postcodes.io/postcodes/" + urllib.parse.quote(postcode.replace(" ", ""))
    req = urllib.request.Request(url, headers={"User-Agent": "Esp32Departures-installer"})
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            d = json.loads(r.read().decode("utf-8", "replace"))
        res = d["result"]
        return float(res["latitude"]), float(res["longitude"])
    except Exception:
        return None


def bus_arrivals(code, line_filter=""):
    """Live arrivals at a stop. Returns (status, [(route, destination, mins), ...]).

    With this ReturnList a prediction line is
    [1, StopPointName, LineName, DestinationText, EstimatedTime, ExpireTime] -
    the same shape and indices the firmware relies on."""
    url = (f"{TFL_URA}?StopCode1={code}&ReturnList=StopPointName,LineName,"
           "DestinationText,EstimatedTime,ExpireTime")
    if line_filter:
        url += "&LineName=" + line_filter
    status, lines = _http_json_lines(url)
    if status != "ok":
        return status, []
    now_ms = 0
    arrivals = []
    for a in lines:
        if not isinstance(a, list) or not a:
            continue
        if a[0] == 4 and len(a) >= 3:          # URA version array carries the clock
            now_ms = a[2]
        elif a[0] == 1 and len(a) >= 6 and now_ms:
            arrivals.append((str(a[2]), str(a[3]), max(0, int((a[4] - now_ms) // 60000))))
    arrivals.sort(key=lambda x: x[2])
    return "ok", arrivals


POSTCODE_RE = re.compile(r"^[A-Z]{1,2}\d[A-Z\d]?\s*\d[A-Z]{2}$", re.I)


def find_stops(search):
    """Turn whatever the user typed into a list of nearby stops.

    One prompt handles all three sensible inputs rather than making the user
    pick a search mode first: a postcode, a place or stop name, or the stop's
    own code. Returns (stops, label) - `stops` is empty if nothing was found."""
    if POSTCODE_RE.match(search):
        point = geocode_postcode(search)
        if not point:
            return [], search.upper()
        return stops_near_any([point], 500), search.upper()

    matches = geocode_place(search)
    if not matches:
        return [], search
    # Matches sharing a name are the same place indexed several times (TfL lists
    # each station entrance separately), so search around all of them at once
    # instead of offering an unhelpful list of identical-looking choices.
    label = matches[0]["name"]
    origins = [(m["lat"], m["lon"]) for m in matches if m["name"] == label]
    return stops_near_any(origins, 250), label


def choose_bus_stop(current=""):
    """Interactive stop finder. Returns the chosen 5-digit stop code, or ''."""
    while True:
        print("\n  Where is your stop? Enter a postcode (e.g. 'KT3 6PF'), a place")
        print("  name (e.g. 'Green Park Station'), or the 5-digit code on the stop.")
        # Blank keeps the stop already configured rather than silently dropping
        # it - only a board with no stop yet treats blank as "skip".
        blank = f"blank = keep {current}" if current else "blank to skip"
        search = input(f"  Postcode / place / code ({blank}): ").strip()
        if not search:
            return current

        # A bare stop code needs no search - check it and take it as given.
        if search.isdigit():
            status, _ = bus_arrivals(search)
            if status == "bad_stop":
                print(f"  ! TfL doesn't know stop code '{search}'. Check the number on the stop.")
                continue
            if status != "ok":
                print("  (couldn't verify online - accepting the code as entered)")
            return search

        stops, label = find_stops(search)
        if not stops:
            print(f"  ! No London bus stops found for '{label}'. "
                  "Try a postcode, or a nearby landmark.")
            continue

        print(f"\n  Bus stops near {label}:")
        shown = stops[:12]
        for i, s in enumerate(shown):
            ind = f" ({s['indicator']})" if s["indicator"] else ""
            towards = f"towards {s['towards']}" if s["towards"] else "(hail & ride)"
            print(f"    [{i}] {s['name']}{ind}  -  {s['distance']:.0f}m  -  {towards}")
        if len(stops) > len(shown):
            print(f"    ... {len(stops) - len(shown)} more not shown - "
                  "search a more specific place to narrow it down")
        print("    [s] Search somewhere else")
        sel = input("  Choose a stop [0]: ").strip() or "0"
        if not (sel.isdigit() and int(sel) < len(shown)):
            continue
        return shown[int(sel)]["code"]


def bus_wizard(defaults):
    """Optional London bus section of the wizard. Returns (stop_code, line_filter)."""
    print("\nLondon buses (optional)")
    print("  The board can cycle to a live bus arrivals screen: trains for 30s,")
    print("  then your bus stop for 15s, over and over. Uses TfL's open data -")
    print("  no extra key needed. London only.")

    current = defaults.get("bus", "")
    default_yes = bool(current)
    prompt = "  Add a London bus stop? [Y/n]: " if default_yes else "  Add a London bus stop? [y/N]: "
    answer = input(prompt).strip().lower()
    if not answer:
        answer = "y" if default_yes else "n"
    if not answer.startswith("y"):
        return "", ""

    code = choose_bus_stop(current)
    if not code:
        return "", ""

    line = ask("  Show only one route? (e.g. 38; blank = all routes)",
               defaults.get("busline", ""))
    status, arrivals = bus_arrivals(code, line)
    if status == "ok":
        if arrivals:
            print("  Next buses right now:")
            for route, dest, mins in arrivals[:3]:
                when = "Due" if mins < 1 else f"{mins} min"
                print(f"    {route:>4}  {dest:<28} {when}")
        else:
            print("  (nothing due at this stop right now - the stop is valid)")
    return code, line


# --------------------------------------------------------------------------- #
# Serial helpers
# --------------------------------------------------------------------------- #
def list_candidate_ports():
    """Return COM ports, Espressif (native-USB) devices first."""
    ports = list(serial.tools.list_ports.comports())
    ports.sort(key=lambda p: (p.vid != ESPRESSIF_VID, p.device))
    return ports


def open_serial(port):
    """Open the port without triggering the S3 reset (dtr=True, rts=False)."""
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200
    ser.timeout = 0.1
    ser.dtr = True
    ser.rts = False
    ser.open()
    return ser


def read_lines(ser, secs, echo=False):
    end = time.time() + secs
    buf, out = b"", []
    while time.time() < end:
        data = ser.read(512)
        if data:
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                s = line.decode(errors="replace").strip()
                if s:
                    out.append(s)
                    if echo:
                        print("   <", s)
        else:
            time.sleep(0.03)
    return out


def probe(port, timeout=3.0):
    """Return True if Esp32Departures firmware answers PING on this port."""
    try:
        ser = open_serial(port)
    except Exception:
        return False
    try:
        time.sleep(0.3)
        end = time.time() + timeout
        while time.time() < end:
            ser.write(b"PING\n")
            if any("PONG" in l for l in read_lines(ser, 0.4)):
                return True
        return False
    finally:
        ser.close()


def read_config(port, timeout=6.0):
    """Read the board's current settings via GET. Returns a dict (may be empty).

    Secrets never come back - the firmware reports the WiFi password only as
    `passlen` and never sends the API key at all - so those two are the only
    things the user ever has to retype, and only if they want to change them."""
    try:
        ser = open_serial(port)
    except Exception:
        return {}
    try:
        time.sleep(0.3)
        end = time.time() + timeout
        while time.time() < end:
            ser.write(b"GET\n")
            lines = read_lines(ser, 0.6)
            if any(l == "END" for l in lines):
                out = {}
                for l in lines:
                    if "=" in l:
                        k, v = l.split("=", 1)
                        out[k.strip()] = v.strip()
                return out
        return {}
    finally:
        ser.close()


def provision(port, cfg, wait_boot=20.0):
    """Send config to the board and COMMIT. Returns True on SAVED."""
    ser = open_serial(port)
    try:
        time.sleep(0.3)
        # Handshake — retry PING while the board finishes booting.
        end = time.time() + wait_boot
        ready = False
        while time.time() < end:
            ser.write(b"PING\n")
            if any("PONG" in l for l in read_lines(ser, 0.4)):
                ready = True
                break
        if not ready:
            print("  ! board did not respond (no PONG)")
            return False

        for key in ("ssid", "pass", "key", "dep", "dest", "plat", "tz",
                    "bus", "busline", "bstart", "bend", "bright", "refr"):
            # None means "leave whatever the board already has". The firmware
            # stages a COMMIT on top of its current config, so simply not
            # sending a key preserves it.
            if cfg.get(key) is None:
                continue
            ser.write(f"CFG {key}={cfg[key]}\n".encode())
            read_lines(ser, 0.25)

        ser.write(b"COMMIT\n")
        return any("SAVED" in l for l in read_lines(ser, 4))
    finally:
        ser.close()


# --------------------------------------------------------------------------- #
# Flashing
# --------------------------------------------------------------------------- #
def flash(port):
    """Flash the four bundled binaries with esptool (in-process)."""
    import esptool
    args = [
        "--chip", "esp32s3", "--port", port, "--baud", "921600",
        "--before", "default_reset", "--after", "hard_reset",
        "write_flash", "-z", "--flash_mode", "dio",
        "--flash_freq", "80m", "--flash_size", "16MB",
    ]
    for offset, name in FLASH_LAYOUT:
        args += [hex(offset), fw(name)]
    esptool.main(args)  # raises SystemExit / exception on failure


# --------------------------------------------------------------------------- #
# Wizard
# --------------------------------------------------------------------------- #
def ask(prompt, default="", required=False, cast=str, lo=None, hi=None):
    while True:
        d = f" [{default}]" if default != "" else ""
        raw = input(f"{prompt}{d}: ").strip()
        if not raw:
            raw = str(default)
        if required and not raw:
            print("  ! required")
            continue
        try:
            val = cast(raw) if raw != "" else raw
        except ValueError:
            print("  ! invalid")
            continue
        if cast is int and raw != "":
            if lo is not None and val < lo:
                print(f"  ! min {lo}"); continue
            if hi is not None and val > hi:
                print(f"  ! max {hi}"); continue
        return val


def wizard(defaults=None, on_board=False):
    """Ask for every setting. `defaults` pre-fills from the board's current
    config; `on_board` means the board is already configured, so the two secrets
    it will not hand back (WiFi password, API key) can be left alone instead of
    retyped - a blank answer stores None, which provision() skips."""
    d = defaults or {}
    print("\nEnter your settings (press Enter to accept a [default]):\n")
    cfg = {}
    cfg["ssid"] = ask("WiFi network (2.4GHz)", d.get("ssid", ""), required=True)

    keep = "  (Enter = keep the one already on the board)" if on_board else ""
    import getpass
    try:
        pw = getpass.getpass(f"WiFi password{keep}: ")
    except Exception:
        pw = ask(f"WiFi password{keep}", "")
    cfg["pass"] = pw if pw else (None if on_board else "")

    api = ask(f"LDBWS API key (raildata.org.uk){keep}", "", required=not on_board)
    cfg["key"] = api if api else (None if on_board else "")
    cfg["dep"] = ask("Departure station CRS", d.get("dep", ""), required=True).upper()
    cfg["dest"] = ask("Destination CRS filter (optional)", d.get("dest", "")).upper()
    cfg["plat"] = ask("Platform filter (optional)", d.get("plat", ""))
    cfg["bus"], cfg["busline"] = bus_wizard(d)
    print("\nScreen blank hours turn the display OFF between two times (e.g. START 23,")
    print("END 7 blanks it overnight). Enter -1 for both to leave it on all the time.")
    while True:
        cfg["bstart"] = ask("Screen blank START hour - screen goes OFF (-1 = never blank)",
                            d.get("bstart", -1), cast=int, lo=-1, hi=23)
        cfg["bend"] = ask("Screen blank END hour - screen comes back ON (-1 = never blank)",
                          d.get("bend", -1), cast=int, lo=-1, hi=23)
        if cfg["bstart"] == -1 or cfg["bend"] == -1:
            cfg["bstart"] = cfg["bend"] = -1
            print("  Screen stays on all the time.")
            break
        off = (cfg["bend"] - cfg["bstart"]) % 24
        print(f"  -> Screen OFF {cfg['bstart']:02d}:00-{cfg['bend']:02d}:00 "
              f"({off}h), ON the other {24 - off}h.")
        # START/END the wrong way round is easy to do and blanks the board for
        # most of the day, which reads as a broken screen rather than a setting.
        if off > 12 and input("  That's off for most of the day - is that right? [y/N]: "
                              ).strip().lower() not in ("y", "yes"):
            continue
        break
    cfg["bright"] = ask("Brightness (0-255)", d.get("bright", 180), cast=int, lo=0, hi=255)
    cfg["refr"] = ask("Refresh seconds", d.get("refr", 60), cast=int, lo=15, hi=3600)
    cfg["tz"] = ask("Timezone (POSIX TZ; blank = UK default)", d.get("tz", detect_tz()))
    return cfg


def summary(cfg):
    if cfg["key"] is None:
        masked_key = "(unchanged)"
    elif len(cfg["key"]) > 6:
        masked_key = cfg["key"][:6] + "…"
    else:
        masked_key = "(set)"
    pw = "(password unchanged)" if cfg["pass"] is None else "(password set)"
    print("\n  Summary")
    print(f"    WiFi        {cfg['ssid']}  {pw}")
    print(f"    API key     {masked_key}")
    print(f"    Station     {cfg['dep']}" + (f" -> {cfg['dest']}" if cfg["dest"] else ""))
    if cfg["plat"]:
        print(f"    Platform    {cfg['plat']}")
    if cfg.get("bus"):
        route = f" (route {cfg['busline']} only)" if cfg.get("busline") else ""
        print(f"    Bus stop    {cfg['bus']}{route}")
    if cfg["bstart"] != -1 or cfg["bend"] != -1:
        print(f"    Blank hours {cfg['bstart']}:00 - {cfg['bend']}:00")
    if cfg.get("tz"):
        print(f"    Timezone    {cfg['tz']}")
    print(f"    Brightness  {cfg['bright']}   Refresh {cfg['refr']}s\n")


# --------------------------------------------------------------------------- #
# Main
# --------------------------------------------------------------------------- #
def pick_port():
    ports = list_candidate_ports()
    if not ports:
        return None
    if len(ports) == 1:
        return ports[0].device
    print("\nMultiple serial devices found:")
    for i, p in enumerate(ports):
        tag = " (Espressif)" if p.vid == ESPRESSIF_VID else ""
        print(f"  [{i}] {p.device}  {p.description}{tag}")
    while True:
        s = input("Choose device number: ").strip()
        if s.isdigit() and 0 <= int(s) < len(ports):
            return ports[int(s)].device


def run_interactive():
    print("=" * 44)
    print(" Esp32Departures Installer  (T-Display-S3)")
    print("=" * 44)

    input("\nPlug the board into USB, then press Enter...")
    port = pick_port()
    if not port:
        print("\nNo serial device found. Check the USB cable and drivers, then re-run.")
        return 1
    print(f"Using board on {port}.")

    has_fw = probe(port, timeout=2.5)
    do_flash = True
    current = {}
    if has_fw:
        current = read_config(port)
        if current.get("dep"):
            bus = f", bus {current['bus']}" if current.get("bus") else ""
            print(f"\nBoard is set up for station {current['dep']}{bus} "
                  f"on WiFi '{current.get('ssid', '?')}'.")
        choice = input("\nBoard already has Esp32Departures firmware.\n"
                       "  [C] Change settings (keeps the firmware)\n"
                       "  [U] Update firmware only - keeps all your settings\n"
                       "  [F] Update firmware AND change settings\n"
                       "Choose [C]: ").strip().lower()

        if choice == "u":
            # Nothing is asked and nothing is provisioned: the settings live in
            # a separate NVS partition that re-flashing the app does not touch.
            print("\nUpdating firmware (do not unplug)...")
            try:
                flash(port)
            except SystemExit as e:
                if e.code not in (0, None):
                    print(f"\nFlashing failed (esptool exit {e.code}).")
                    return 1
            except Exception as e:
                print(f"\nFlashing failed: {e}")
                return 1
            print("\n  ✓ Firmware updated. Your settings were left untouched -")
            print("    the board is rebooting with the same station, WiFi and bus stop.")
            return 0

        do_flash = (choice == "f")

    cfg = wizard(current, on_board=bool(current.get("prov") == "1"))
    print("\nVerifying station and key online...")
    verify_config(cfg)
    summary(cfg)
    if input("Flash and configure the board now? [Y/n]: ").strip().lower() == "n":
        print("Cancelled.")
        return 1

    if do_flash:
        print("\nFlashing firmware (do not unplug)...")
        try:
            flash(port)
        except SystemExit as e:
            if e.code not in (0, None):
                print(f"\nFlashing failed (esptool exit {e.code}).")
                return 1
        except Exception as e:
            print(f"\nFlashing failed: {e}")
            return 1
        print("Flash complete. Waiting for the board to reboot...")
        time.sleep(3)
        port = pick_port() or port  # port may re-enumerate

    print("Configuring board...")
    if provision(port, cfg):
        print("\n  ✓ Done! The board is rebooting and will show departures shortly.")
        return 0
    print("\n  ✗ Configuration failed. Re-run and try 'Configure only', or replug the board.")
    return 1


def run_auto(path):
    with open(path) as f:
        d = json.load(f)
    # A key absent from the JSON means "leave whatever the board already has",
    # matching the interactive wizard. Pass an explicit "" to clear a setting.
    cfg = {k: d.get(k) for k in
           ("ssid", "pass", "key", "dep", "dest", "plat", "tz",
            "bus", "busline", "bstart", "bend", "bright", "refr")}
    if cfg["tz"] == "":
        cfg["tz"] = detect_tz()
    port = d.get("port") or pick_port()
    print(f"[auto] port={port} flash={d.get('flash', True)}")
    if d.get("flash", True):
        print("[auto] flashing...")
        flash(port)
        time.sleep(3)
        port = pick_port() or port
    print("[auto] provisioning...")
    ok = provision(port, cfg)
    print("[auto] result:", "OK" if ok else "FAILED")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--auto", help="non-interactive: JSON config file")
    args = ap.parse_args()
    try:
        rc = run_auto(args.auto) if args.auto else run_interactive()
    except KeyboardInterrupt:
        rc = 1
    if not args.auto:
        input("\nPress Enter to close...")
    sys.exit(rc)


if __name__ == "__main__":
    main()
