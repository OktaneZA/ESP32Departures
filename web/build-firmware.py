#!/usr/bin/env python3
"""Stage the firmware binaries into the web site and write their manifest.

The binaries live in installer/firmware/ and are shared with the .exe, so they
are copied here at publish time rather than committed twice. Run this before
serving web/ locally; CI runs it before deploying to Pages.

The manifest records a SHA-256 per image. Both flashers verify those before
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
LAYOUT = [
    (0x0000, "bootloader.bin"),
    (0x8000, "partitions.bin"),
    (0xE000, "boot_app0.bin"),
    (0x10000, "firmware.bin"),
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


def main():
    os.makedirs(DST, exist_ok=True)
    parts, missing = [], []
    for offset, name in LAYOUT:
        src = os.path.join(SRC, name)
        if not os.path.exists(src):
            missing.append(name)
            continue
        shutil.copy2(src, os.path.join(DST, name))
        data = open(src, "rb").read()
        parts.append({
            "path": f"firmware/{name}",
            "offset": offset,
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        })
        print(f"  {name:<18} {len(data):>9,} bytes  {parts[-1]['sha256'][:16]}…")

    if missing:
        print("\nMissing binaries: " + ", ".join(missing), file=sys.stderr)
        print("Build the firmware and copy it into installer/firmware/ first.",
              file=sys.stderr)
        return 1

    manifest = {
        "name": "Departure Buddy",
        "version": version(),
        "chip": "esp32s3",
        "parts": parts,
    }
    out = os.path.join(DST, "manifest.json")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print(f"\nwrote {out}  (version {manifest['version']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
