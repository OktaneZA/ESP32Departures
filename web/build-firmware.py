#!/usr/bin/env python3
"""Stage the firmware binaries into the web site and write their manifest.

The binaries live in installer/firmware/<board>/ and are shared with the .exe,
so they are copied here at publish time rather than committed twice. Run this
before serving web/ locally; CI runs it before deploying.

Two boards are published, and they are not interchangeable: a classic ESP32
wants its bootloader at 0x1000 where an ESP32-S3 wants 0x0000, so flashing one
board's images onto the other leaves it unbootable. The manifest therefore
carries the offsets *and* the chip family per board, and the flasher refuses a
mismatch before writing anything.

The manifest records a SHA-256 per file. Both flashers verify those before
writing, so a truncated download is refused rather than half-flashed onto a
board.
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "installer", "firmware")
DST = os.path.join(ROOT, "web", "firmware")

# Offsets must match installer.py's FLASH_LAYOUT and flash.js.
#
# Only the bootloader moves between the two: the ESP32-S3 boots from 0x0000 and
# the classic ESP32 from 0x1000, because the older part reserves the first 4 KB.
# Everything else is the same partition scheme.
S3_LAYOUT = [
    (0x0000, "bootloader.bin"),
    (0x8000, "partitions.bin"),
    (0xE000, "boot_app0.bin"),
    (0x10000, "firmware.bin"),
]
ESP32_LAYOUT = [
    (0x1000, "bootloader.bin"),
    (0x8000, "partitions.bin"),
    (0xE000, "boot_app0.bin"),
    (0x10000, "firmware.bin"),
]

# `id` is what the firmware reports from GET, so a board can be recognised
# without guessing from a USB vendor id — that names the bridge chip, not the
# board. `env` is the PlatformIO environment that builds it.
BOARDS = [
    {
        "id": "tdisplay-s3",
        "name": "LilyGo T-Display-S3",
        "note": "1.9\" 320x170, two buttons",
        "chip": "esp32s3",
        "flash_size": "16MB",
        "env": "lilygo-t-display-s3",
        "layout": S3_LAYOUT,
    },
    {
        "id": "cyd",
        "name": "ESP32 Cheap Yellow Display",
        "note": "2.8\" 320x240 touch (ESP32-2432S028R)",
        "chip": "esp32",
        "flash_size": "4MB",
        "env": "cyd",
        # No auto-program circuit on the units seen so far: DTR does not reach
        # GPIO0, so the flasher has to tell the user to hold BOOT.
        "hold_boot": True,
        "layout": ESP32_LAYOUT,
    },
]


def version():
    """Best-effort build label: the short commit, marked if the tree is dirty."""
    try:
        rev = subprocess.check_output(["git", "rev-parse", "--short", "HEAD"],
                                      cwd=ROOT, text=True).strip()
        dirty = subprocess.check_output(["git", "status", "--porcelain"],
                                        cwd=ROOT, text=True).strip()
        return rev + ("+local" if dirty else "")
    except Exception:
        return "unknown"


def stage(board):
    """Copy one board's images into web/firmware/<id>/ and describe them.

    Returns (entry, missing). A board whose binaries are absent is reported
    rather than silently skipped: publishing a site that offers a board it
    cannot actually flash would be worse than failing here.
    """
    src_dir = os.path.join(SRC, board["id"])
    dst_dir = os.path.join(DST, board["id"])
    os.makedirs(dst_dir, exist_ok=True)

    parts, missing = [], []
    for offset, name in board["layout"]:
        src = os.path.join(src_dir, name)
        if not os.path.exists(src):
            missing.append(os.path.join(board["id"], name))
            continue
        shutil.copy2(src, os.path.join(dst_dir, name))
        data = open(src, "rb").read()
        parts.append({
            "path": f"firmware/{board['id']}/{name}",
            "offset": offset,
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            # The board reports MD5, because that is what Arduino exposes for
            # the running image. Published so a user can check that what is on
            # their device is what was released here.
            "md5": hashlib.md5(data).hexdigest(),
        })
        print(f"  {board['id']}/{name:<16} {len(data):>9,} bytes  "
              f"{parts[-1]['sha256'][:16]}…")

    entry = {k: board[k] for k in ("id", "name", "note", "chip", "flash_size")}
    if board.get("hold_boot"):
        entry["hold_boot"] = True
    entry["parts"] = parts
    return entry, missing


def main():
    os.makedirs(DST, exist_ok=True)
    boards, missing = [], []
    for board in BOARDS:
        entry, gaps = stage(board)
        missing += gaps
        boards.append(entry)

    if missing:
        print("\nMissing binaries: " + ", ".join(missing), file=sys.stderr)
        print("Build with `pio run` and stage them into installer/firmware/<board>/.",
              file=sys.stderr)
        return 1

    manifest = {
        "name": "Departure Buddy",
        "version": version(),
        "boards": boards,
    }
    out = os.path.join(DST, "manifest.json")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print(f"\nwrote {out}  (version {manifest['version']}, "
          f"{len(boards)} boards)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
