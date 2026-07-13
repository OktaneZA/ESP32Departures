"""Esp32Departures installer — console wizard.

Flashes the bundled firmware to a LilyGo T-Display-S3 and configures it (WiFi,
National Rail LDBWS key, station, filters, blank hours, brightness) over USB
serial. One pre-built binary; all settings are written to the board at runtime,
so no toolchain is needed on the user's PC.

Packaged into a single Windows .exe with PyInstaller (see build_exe.py).

Usage:
    installer.exe                 interactive wizard
    installer.exe --auto cfg.json non-interactive (for testing/automation)
"""

import argparse
import json
import os
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
                    "bstart", "bend", "bright", "refr"):
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


def wizard(defaults=None):
    d = defaults or {}
    print("\nEnter your settings (press Enter to accept a [default]):\n")
    cfg = {}
    cfg["ssid"] = ask("WiFi network (2.4GHz)", d.get("ssid", ""), required=True)
    import getpass
    try:
        cfg["pass"] = getpass.getpass("WiFi password: ") or d.get("pass", "")
    except Exception:
        cfg["pass"] = ask("WiFi password", d.get("pass", ""))
    cfg["key"] = ask("LDBWS API key (raildata.org.uk)", d.get("key", ""), required=True)
    cfg["dep"] = ask("Departure station CRS", d.get("dep", ""), required=True).upper()
    cfg["dest"] = ask("Destination CRS filter (optional)", d.get("dest", "")).upper()
    cfg["plat"] = ask("Platform filter (optional)", d.get("plat", ""))
    cfg["bstart"] = ask("Screen blank START hour (-1 = off)", d.get("bstart", -1),
                        cast=int, lo=-1, hi=23)
    cfg["bend"] = ask("Screen blank END hour (-1 = off)", d.get("bend", -1),
                      cast=int, lo=-1, hi=23)
    cfg["bright"] = ask("Brightness (0-255)", d.get("bright", 180), cast=int, lo=0, hi=255)
    cfg["refr"] = ask("Refresh seconds", d.get("refr", 60), cast=int, lo=15, hi=3600)
    cfg["tz"] = ask("Timezone (POSIX TZ; blank = UK default)", d.get("tz", detect_tz()))
    return cfg


def summary(cfg):
    masked_key = (cfg["key"][:6] + "…") if len(cfg["key"]) > 6 else "(set)"
    print("\n  Summary")
    print(f"    WiFi        {cfg['ssid']}  (password set)")
    print(f"    API key     {masked_key}")
    print(f"    Station     {cfg['dep']}" + (f" -> {cfg['dest']}" if cfg["dest"] else ""))
    if cfg["plat"]:
        print(f"    Platform    {cfg['plat']}")
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
    if has_fw:
        choice = input("\nBoard already has Esp32Departures firmware.\n"
                       "  [C] Configure only (fast)\n"
                       "  [F] Re-flash firmware, then configure\n"
                       "Choose [C]: ").strip().lower()
        do_flash = (choice == "f")

    cfg = wizard()
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
    cfg = {k: d.get(k, "") for k in
           ("ssid", "pass", "key", "dep", "dest", "plat", "tz",
            "bstart", "bend", "bright", "refr")}
    if not cfg["tz"]:
        cfg["tz"] = detect_tz()
    for k, dv in (("bstart", -1), ("bend", -1), ("bright", 180), ("refr", 60)):
        cfg[k] = d.get(k, dv)
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
