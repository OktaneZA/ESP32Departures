# Esp32Departures Installer

A self-contained **Windows 10/11** installer for the LilyGo T-Display-S3 departure
board. It flashes the firmware and configures it (WiFi, National Rail LDBWS key,
station, filters, blank hours, brightness) over USB — **no Python, PlatformIO, or
toolchain needed** on the target PC.

*Part of Esp32Departures, derived from
[chrisys/train-departure-display](https://github.com/chrisys/train-departure-display).*

## The hardware you need

This project runs on the **LilyGo T-Display-S3** development board:

| Spec | Detail |
|---|---|
| MCU | ESP32-S3 (dual-core, 2.4 GHz WiFi — **no 5 GHz**) |
| Display | 1.9″ 170×320 ST7789 colour LCD |
| Memory | 16 MB flash, 8 MB PSRAM |
| USB | USB-C (native USB, used for flashing + config) |

Get the **standard T-Display-S3** (the non-touch version is all you need; the
"Touch" variant also works since the app doesn't use touch). Nothing else to buy —
the LCD, WiFi, and USB are all on-board, no wiring required.

**Where to buy** (made by LilyGo — buy from the official store to avoid clones):

- Official product page: <https://lilygo.cc/en-us/products/t-display-s3>
- AliExpress (LilyGo official store): <https://www.aliexpress.com/item/1005004496543314.html>
- Alibaba (standard "with shell" listing): <https://www.alibaba.com/product-detail/LILYGO-TTGO-T-Display-S3-with_1601237505007.html>
- Alibaba search (all sellers): <https://www.alibaba.com/showroom/lilygo-t--display--s3-esp32--s3.html>

Typical price is around **US$15–27** (varies by seller, promotion, and shipping).
When ordering, pick the option **with pin headers pre-soldered** if you don't want
to solder (some listings sell it "unsoldered").

⚠️ Get the **1.9″ ST7789, 170×320** version (the plain "T-Display-S3" or its "Touch
Edition" — both work). Do **not** buy the look-alikes: the **AMOLED** (1.91″), **Pro**
(2.33″), **Long** (3.4″), or the older **T-Display** (ESP32, 1.14″) — none are
compatible with this firmware.

## Get your National Rail API key

The board needs a free API key for live departure data:

1. **Create an account** at **[raildata.org.uk](https://raildata.org.uk/)** — register
   a **personal account** and verify your email.
2. **Subscribe to the data feed.** Once your account is set up, open the data-product
   catalogue and search for **"Live Departure Board"** (LDBWS). Subscribe to it — it's
   the **free open tier** and approval is usually instant.
   - ⚠️ Don't pick the **"Demo Version"** — it's capped at 100 calls / 30 days, which a
     60-second refresh uses up almost immediately.
3. **Copy your key.** Open the subscribed product and go to the **"Specification"** tab.
   Under **"API access credentials"** you'll find your **Consumer key** — copy it.
   (Ignore the "Consumer secret"; the board only needs the key.)
4. Paste that Consumer key into the installer when it asks for the **LDBWS API key**.

## For end users

1. Plug the T-Display-S3 into a USB-C port.
2. Double-click **`Esp32DeparturesInstaller.exe`**.
3. Follow the prompts (it auto-detects the board's COM port).
4. When it says *Done*, the board reboots and shows live departures.

To change stations or settings later, just run it again and pick **Configure only**
(no re-flash needed — settings are stored on the device).

### What you'll be asked
- WiFi network + password (2.4 GHz only — the ESP32-S3 has no 5 GHz radio)
- LDBWS API key (your consumer key from [raildata.org.uk](https://raildata.org.uk))
- Departure station CRS (e.g. `MOT`), optional destination + platform filters
- Optional screen-blank hours, brightness, refresh interval
- Timezone — **auto-detected from your PC's locale** (just press Enter to accept)

> **Finding your station's CRS code:** it's the 3-letter code for your station
> (e.g. `PAD` = London Paddington, `MOT` = Motspur Park). Look it up here:
> [railwaycodes.org.uk CRS list](http://www.railwaycodes.org.uk/crs/crs0.shtm) or
> search your station on [nationalrail.co.uk/stations](https://www.nationalrail.co.uk/stations/).

The installer also **checks your station code and key against the API** before
configuring, and re-prompts if either is rejected — so a typo'd station is caught
up front. (If the code is wrong anyway, the board itself shows an "Unknown station"
screen rather than a blank error.)

## How it works

One pre-built firmware binary is flashed to every board. Settings are **not**
compiled in — the installer writes them to the device's flash (NVS) over a small
USB-serial protocol (`PING`/`CFG`/`COMMIT`), so no recompilation is ever required.
Serial is opened with `dtr=True, rts=False` to avoid resetting the S3.

## For developers — building the exe

Requires Python 3.x with these packages:

```bash
pip install pyserial "esptool>=4.7,<5" pyinstaller
python build_exe.py      # -> dist/Esp32DeparturesInstaller.exe
```

The bundled firmware binaries live in `installer/firmware/` and are produced by
`pio run` (copy `bootloader.bin`, `partitions.bin`, `firmware.bin` from
`.pio/build/lilygo-t-display-s3/` and `boot_app0.bin` from the framework package).
Re-copy `firmware.bin` after any firmware change, then rebuild the exe.

### Testing without the GUI

```bash
python installer.py --auto cfg.json
```

where `cfg.json` has `port`, `flash`, and the settings keys
(`ssid pass key dep dest plat bstart bend bright refr`).

## Notes

- PyInstaller one-file exes can trip antivirus false-positives; code-signing the
  exe avoids this for wider distribution.
- Flash layout (ESP32-S3): `0x0` bootloader, `0x8000` partitions, `0xe000`
  boot_app0, `0x10000` firmware.
