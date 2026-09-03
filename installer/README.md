# Departure Buddy Installer

A self-contained **Windows 10/11** installer for the LilyGo T-Display-S3 departure
board. It flashes the firmware and configures it (WiFi, National Rail LDBWS key,
station, filters, an optional London bus stop, an optional Thames pier, blank
hours, brightness) over USB — **no Python, PlatformIO, or toolchain needed** on
the target PC.

*Part of Departure Buddy, derived from
[chrisys/train-departure-display](https://github.com/chrisys/train-departure-display).*

## Before you reach for this

Most people should use the **web configurator** instead: it picks stations,
stops and piers for you, previews the result, and in Chrome or Edge sends the
settings straight to the board over USB. This installer is for flashing a
brand-new board, for Firefox and Safari users, and for anyone who prefers it.

It also accepts a settings file exported from that page — see
[Testing without the GUI](#testing-without-the-gui) for the format.

## Get it

### → **[Download DepartureBuddyInstaller.exe](https://github.com/OktaneZA/ESP32Departures/releases/latest)**

Built by CI from the tagged source, with the firmware compiled from the same
commit — so the installer and the board it flashes always match.

> Windows SmartScreen will warn about an unrecognised app. The exe is unsigned:
> a code-signing certificate costs more per year than the hardware does. Choose
> **More info → Run anyway**, or check the SHA-256 against the checksum file
> published beside it.

## Before you start

You need two things, both covered elsewhere:

- **The board** — a LilyGo T-Display-S3. Which one to buy and where is on the
  [front page](../README.md#1-buy-the-board).
- **A train data key** — free, from the Rail Data Marketplace. Step-by-step with
  screenshots: **[Getting your train data key](../docs/api-key.md)**. Skip it
  entirely for a buses-or-boats-only board; those feeds need no key.

## For end users

1. Plug the T-Display-S3 into a USB-C port.
2. Double-click **`DepartureBuddyInstaller.exe`**.
3. Follow the prompts (it auto-detects the board's COM port).
4. When it says *Done*, the board reboots and shows live departures.

If the board is already set up, it tells you what it is set to and offers three
choices:

| | What it does |
|---|---|
| **[C] Change settings** | Keeps the firmware, walks the prompts (all pre-filled from the board) |
| **[U] Update firmware only** | Asks nothing at all — flashes new firmware and keeps every setting |
| **[F] Update firmware AND change settings** | Both |

**You never have to retype your WiFi password or API key.** They are the only two
things the board will not read back, so their prompts accept a blank answer
meaning *keep the one already on the board*. Everything else — station, bus stop,
pier, blank hours, brightness — is pre-filled with what the board is currently
using, so pressing Enter through the whole wizard changes nothing.

### What you'll be asked

The wizard runs in two parts: the settings every board needs, then what it
should actually show.

**Part 1 — board basics**
- WiFi network + password (2.4 GHz only — the ESP32-S3 has no 5 GHz radio)
- **Screen on/off hours** — the display turns itself off overnight so it isn't
  lighting an empty room. A new board defaults to **on at 06:00, off at 22:00**;
  enter `-1` for both to keep it on all the time. A board you've already
  configured keeps its own hours as the default
- Refresh interval, then brightness
- Timezone — **auto-detected from your PC's locale** (just press Enter to accept)

**Part 2 — what the board shows**
- **Which services** — trains, London buses, river boats, or any combination.
  You pick them from a list (`1,3` for trains and boats), and are then asked
  only for what those need
- *(trains)* LDBWS API key (your consumer key from [raildata.org.uk](https://raildata.org.uk))
- *(trains)* Departure station CRS (e.g. `MOT`), optional destination + platform filters
- *(buses)* Your London bus stop (see below)
- *(boats)* Your Thames pier (see below)

> **Finding your station's CRS code:** it's the 3-letter code for your station
> (e.g. `PAD` = London Paddington, `MOT` = Motspur Park). Look it up here:
> [railwaycodes.org.uk CRS list](http://www.railwaycodes.org.uk/crs/crs0.shtm) or
> search your station on [nationalrail.co.uk/stations](https://www.nationalrail.co.uk/stations/).

The installer also **checks your station code and key against the API** before
configuring, and re-prompts if either is rejected — so a typo'd station is caught
up front. (This check is skipped if you kept the board's existing key, since it
has no key to check with; the board itself then shows an "Unknown station" screen
rather than a blank error.)

## Adding a London bus stop (optional)

Buses can be shown on their own, or alongside the trains. With both, the board
cycles **trains for 30 seconds, then your bus stop for 15 seconds**, over and
over. It uses TfL's open Countdown data, so there is **no extra API key to
get** — but it only covers **London** buses (and TfL river bus piers).

A **buses-only** board needs no API key and no station, so the wizard skips
those prompts entirely — but it does require a stop, since otherwise there
would be nothing to display.

Because you already chose buses in Part 2, the installer doesn't ask *whether*
you want a stop — just which one. A single prompt finds it for you, so you don't
need to know any codes. Type whichever you have and it works out which is which:

- a **postcode** — `SW19 7NL`
- a **place or stop name** — `Green Park Station`
- the **5-digit code** printed on the stop itself — `52053`

You then get a numbered list of nearby stops **with the direction each one
serves**, so you can pick the right side of the road:

```
  Where is your stop? Enter a postcode (e.g. 'SW19 7NL'), a place
  name (e.g. 'Green Park Station'), or the 5-digit code on the stop.
  Postcode / place / code (blank to skip): SW19 7NL

  Bus stops near SW19 7NL:
    [0] Alexandra Road / Wimbledon Stn (B)  -  56m  -  towards Wimbledon Village
    [1] Wimbledon Station (P)  -  61m  -  towards Southfields
    [2] Alexandra Road / Wimbledon Stn (A)  -  77m  -  towards Southfields Or Tooting
    [3] Wimbledon Station (L)  -  117m  -  towards Putney Heath Or Raynes Park
    [4] Wimbledon Station (D)  -  117m  -  towards Colliers Wood, Morden Or Wimbledon Chase
    [5] Wimbledon Police Station (J)  -  164m  -  towards Colliers Wood
    [6] Francis Grove (M)  -  185m  -  towards Raynes Park
    [7] Wimbledon Hill Road (S)  -  198m  -  towards South Wimbledon, Colliers Wood Or Tooting
    [8] Wimbledon Police Station (K)  -  204m  -  towards Wimbledon Village
    [9] Francis Grove (N)  -  213m  -  towards Colliers Wood, Southfields Or Wimbledon Chase
    [10] Francis Grove  -  226m  -  (hail & ride)
    [11] Wimbledon Hill Road (R)  -  226m  -  towards Southfields, Putney Heath Or Raynes Park
    ... 11 more not shown - search a more specific place to narrow it down
    [s] Search somewhere else
  Choose a stop [0]:
```

You can then optionally **limit the screen to a single route** (e.g. just the
`38`), and the installer prints the next few buses so you can confirm you picked
the right stop before it writes anything to the board. Leaving the search prompt
blank skips the bus screen entirely.

To turn the bus screen off later, re-run the installer, choose **Change
settings**, and answer `n` to the bus question. (Pressing Enter at the stop
search keeps the stop you already have.)

> Buses outside London aren't covered — the feed is TfL's. Bus data provided by
> Transport for London.

## Adding a river boat pier (optional)

The board can show live **Uber Boat by Thames Clippers** sailings (RB1, RB4, RB6)
and the **Woolwich Ferry**, for one pier. It uses TfL's open data, so — as with
the buses — there is **no extra API key to get**. These are real live
predictions, not a printed timetable: a boat running late shows as running late.

A **boats-only** board needs no API key and no station, so the wizard skips those
prompts entirely.

As with the buses, choosing river boats in Part 2 is the opt-in — the installer
doesn't ask again. It lists every pier on the network by name with the routes it
serves, and you pick a number. There are no codes to look up:

```
  Your pier
    Live Uber Boat by Thames Clippers sailings (RB1/RB4/RB6) and the
    Woolwich Ferry, from TfL's open data - no extra key needed.
    Enter 's' at the pier list to drop the river screen after all.

  Fetching the list of piers from TfL...

  Piers on the river bus network (26):
    [ 0] Bankside Pier  (RB1, RB6)
    [ 1] Barking Riverside Pier  (RB1, RB6)
    [ 2] Battersea Power Station Pier  (RB1, RB6)
    ...
    [ 5] Canary Wharf Pier  (RB1, RB4, RB6)
    ...
  Choose a pier: 5
  Show only one route? (e.g. RB1; blank = all routes):

  Next boats from Canary Wharf Pier right now:
     RB1  Barking Riverside Pier             Due
     RB1  Westminster Pier                   10 min
     RB6  Putney Pier                        18 min
```

As with the buses you can **limit the screen to a single route** (e.g. just
`RB1`), and the installer prints the next few sailings so you can confirm you
picked the right pier before it writes anything to the board.

To turn the river screen off later, re-run the installer, choose **Change
settings**, and leave river boats out of the service list.

> **RB2 (Tate to Tate) isn't available** — TfL doesn't publish it on this feed.
> Boat data provided by Transport for London.

## How it works

One pre-built firmware binary is flashed to every board. Settings are **not**
compiled in — the installer writes them to the device's flash (NVS) over a small
USB-serial protocol (`PING`/`CFG`/`COMMIT`), so no recompilation is ever required.
`GET` reads the current settings back (which is how the wizard pre-fills itself)
and `SCAN` lists the WiFi networks the board can see. Serial is opened with
`dtr=True, rts=False` to avoid resetting the S3.

Turning a service off **omits** its settings rather than clearing them, so a bus
stop or pier you stop showing is still there when you switch it back on later.

## Troubleshooting

The board can tell you what is wrong. Open its serial port at 115200 (PlatformIO's
`pio device monitor`, or any terminal) and type a command — `GET` reports the
current settings, `SCAN` lists every WiFi network the board's own radio can see.
Neither ever prints your password or API key.

### The screen is blank

Almost always **screen-blank hours set the wrong way round**. `GET` shows them:

```
bstart=6
bend=22
```

That means *off* at 06:00 and *back on* at 22:00 — blank for 16 hours a day. You
probably wanted `bstart=22`, `bend=6`, which is what a board configured with a
current installer gets by default.

The wizard now asks these the natural way round — **"Screen comes ON at"** then
**"Screen goes OFF at"** — so `6` then `22` gives you the right thing. It prints
the resulting ON window back to you and queries anything under half a day.
Re-run the installer and choose **Change settings** to fix a board that was set
up the old way (or enter `-1` for both to disable blanking entirely).

A board with no clock yet (no WiFi) can also blank unpredictably, because it does
not know the time — fix the WiFi first.

### It says "No network"

Check the serial log. `AUTH_FAIL` means the **password is wrong**:

```
[wifi] connecting to MYNET
Reason: 202 - AUTH_FAIL
[wifi] connect timed out
```

Then rule things out with `SCAN`:

```
MYNET|rssi=-58|ch=6|auth=WPA2/WPA3
```

- **Not listed at all?** It is almost certainly **5 GHz only**. The ESP32-S3 has
  no 5 GHz radio. Enable the 2.4 GHz band, or use a 2.4 GHz guest network.
- **Listed but `rssi` worse than about -80?** Too far from the router.
- **Listed with a good signal?** The password is wrong. `GET` reports `passlen`
  (the number of characters stored, never the password itself) — if that does not
  match your real password's length, it was mistyped. Re-run the installer and
  re-enter it.

`GET` also reports `wifi=up` or `wifi=down`, so you can confirm a fix without
watching the log.

### "Unknown station" on screen

The departure CRS code was rejected by National Rail. Re-run the installer and
correct it — the code is 3 letters, e.g. `MOT`, not the station's full name.

### The bus screen never appears

- It only appears once TfL has answered for your stop at least once. A stop code
  TfL rejects (HTTP 416) is logged as `unknown stop code` and the screen is
  skipped entirely — the trains keep working.
- `GET` shows `bus=` empty if no stop is configured.
- "No buses due" with the stop's name is **not** an error: it means the stop is
  valid and nothing is due in the next 30 minutes. Quiet routes late in the
  evening look exactly like this.

### The installer cannot find the board

- Try a different USB-C cable. Charge-only cables are common and carry no data.
- The board should appear as an Espressif device (VID `0x303A`). If several
  serial devices are listed, pick that one.

## For developers — building the exe

Requires Python 3.x with these packages:

```bash
pip install pyserial "esptool>=4.7,<5" pyinstaller
python build_exe.py      # -> dist/DepartureBuddyInstaller.exe
```

The bundled firmware binaries live in `installer/firmware/` and are produced by
`pio run` (copy `bootloader.bin`, `partitions.bin`, `firmware.bin` from
`.pio/build/lilygo-t-display-s3/` and `boot_app0.bin` from the framework package).
Re-copy `firmware.bin` after any firmware change, then rebuild the exe.

### Testing without the GUI

```bash
python installer.py --auto cfg.json
```

where `cfg.json` has `port`, `flash`, and any of the settings keys
(`ssid pass key dep dest plat tz bus busline busbudget river riverline rivername
mode bstart bend bright refr`). `mode` is a comma-separated set — `train`, `bus`,
`river`, or any combination such as `train,bus,river`. The older exclusive
values (`both`, or a lone `train`/`bus`) are still accepted.

A key you **leave out** keeps whatever the board already has; pass an explicit
`""` to clear one. So a partial update is just:

```json
{ "port": "COM9", "flash": false, "bus": "53441" }
```

`bus` is the stop's 5-digit code and `busline` an optional route filter; `"bus": ""`
gives a train-only board.

`busbudget` is how many requests a day the bus feed may spend. Leave it `0` for
TfL, which is keyless and uncapped and polls every 30 seconds. Set it to a
provider's daily allowance and the board paces itself: the allowance divided
evenly across the hours the screen is on, and nothing spent overnight. So `30`
on a board that is on 06:00–22:00 works out at one poll every 32 minutes, and
`300` at one every 3.2 minutes.

## Notes

- PyInstaller one-file exes can trip antivirus false-positives; code-signing the
  exe avoids this for wider distribution.
- Flash layout (ESP32-S3): `0x0` bootloader, `0x8000` partitions, `0xe000`
  boot_app0, `0x10000` firmware.
