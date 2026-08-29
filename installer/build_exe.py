"""Build the single-file Windows installer with PyInstaller.

    python build_exe.py                 (from this directory)
    python installer/build_exe.py       (from anywhere)

Produces dist/DepartureBuddyInstaller.exe — a self-contained Win 10/11 executable
that bundles the firmware binaries, esptool, and pyserial. No Python or toolchain
needed on the target PC.

Every path handed to PyInstaller is absolute. It resolves --add-data against the
process working directory rather than the script's, so a relative "firmware" is
only found when this is run from inside installer/ — which fails in CI, where it
is invoked from the repository root.
"""

import os
import PyInstaller.__main__

HERE = os.path.dirname(os.path.abspath(__file__))

PyInstaller.__main__.run([
    os.path.join(HERE, "installer.py"),
    "--onefile",
    "--console",
    "--clean",
    "--noconfirm",
    "--name", "DepartureBuddyInstaller",
    # Keep the outputs beside this script wherever it was invoked from.
    "--distpath", os.path.join(HERE, "dist"),
    "--workpath", os.path.join(HERE, "build"),
    "--specpath", HERE,
    # Bundle the four firmware binaries (Windows uses ';' as the data separator).
    "--add-data", os.path.join(HERE, "firmware") + ";firmware",
    # esptool ships stub-flasher data + submodules that must be collected.
    "--collect-all", "esptool",
    "--collect-submodules", "serial",
    "--hidden-import", "esptool",
    # Timezone auto-detection: tzdata is data-only; tzlocal has platform submodules.
    "--collect-all", "tzdata",
    "--collect-all", "tzlocal",
])

print("\nBuilt", os.path.join(HERE, "dist", "DepartureBuddyInstaller.exe"))
