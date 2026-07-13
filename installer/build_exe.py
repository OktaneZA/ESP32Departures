"""Build the single-file Windows installer with PyInstaller.

    python build_exe.py

Produces dist/Esp32DeparturesInstaller.exe — a self-contained Win 10/11 executable
that bundles the firmware binaries, esptool, and pyserial. No Python or toolchain
needed on the target PC.
"""

import os
import PyInstaller.__main__

here = os.path.dirname(os.path.abspath(__file__))
os.chdir(here)

PyInstaller.__main__.run([
    "installer.py",
    "--onefile",
    "--console",
    "--clean",
    "--noconfirm",
    "--name", "Esp32DeparturesInstaller",
    # Bundle the four firmware binaries (Windows uses ';' as the data separator).
    "--add-data", "firmware;firmware",
    # esptool ships stub-flasher data + submodules that must be collected.
    "--collect-all", "esptool",
    "--collect-submodules", "serial",
    "--hidden-import", "esptool",
    # Timezone auto-detection: tzdata is data-only; tzlocal has platform submodules.
    "--collect-all", "tzdata",
    "--collect-all", "tzlocal",
])

print("\nBuilt dist/Esp32DeparturesInstaller.exe")
