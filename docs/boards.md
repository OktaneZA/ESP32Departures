# Supported boards

Departure Buddy runs on two boards. They share every feed, every screen and the
same setup page; what differs is the hardware, and all of it lives behind
[`include/board.h`](../include/board.h).

If you are choosing one to buy, the [README](../README.md#1-buy-a-board) has the
short version. This page is the detail — what actually differs, and why.

| | LilyGo T-Display-S3 | ESP32 Cheap Yellow Display |
|---|---|---|
| Board id | `tdisplay-s3` | `cyd` |
| Model | T-Display-S3 | ESP32-2432S028R |
| Chip | ESP32-S3 | ESP32-D0WD-V3 |
| Flash | 16 MB | 4 MB (`huge_app` partitions) |
| PSRAM | Yes | **No** |
| Screen | 320×170 ST7789, 8-bit parallel | 320×240 ST7789 or ILI9341, SPI |
| Back buffer | Full frame, 16bpp, in PSRAM | Full frame, **4-bit palette**, in DRAM |
| Rows | 3 | 4 |
| Clock font | 104px | 148px |
| Input | Two buttons (GPIO0, GPIO14) | Touch (XPT2046), where it is connected |
| USB | Native CDC | CH340 bridge (`1A86:7523`) |
| Bootloader offset | `0x0000` | `0x1000` |
| PlatformIO env | `lilygo-t-display-s3` | `cyd` |

---

## Why the differences matter

### The bootloader offset is not cosmetic

A classic ESP32 reserves the first 4 KB of flash, so its bootloader starts at
`0x1000`; the S3's starts at `0x0000`. Write one board's images at the other's
offsets and it will not boot. This is not a hypothetical — it happened during
development, before the guard below existed.

The web flasher now compares the chip `esptool` reports against the chip the
manifest names, and refuses before writing a byte. The manifest carries the
offsets per board so the images and their addresses always travel together.

### No PSRAM changes more than it sounds like

The S3 keeps its whole 320×170 frame in PSRAM for nothing. The CYD's frame would
be 153,600 bytes of DRAM, and it will not allocate: with 285 KB of heap free the
largest contiguous block was about 149 KB.

Rather than render in bands, the CYD uses a **4-bit palette** buffer — 38,400
bytes. That is lossless here, because the board only ever draws four colours
(background, primary, dimmed, alert) and nothing is anti-aliased. The colour
names in `display.cpp` hold RGB565 values on one board and palette indices on
the other, so the ~60 drawing calls are identical either way.

One consequence worth knowing if you are editing the renderer: **anti-aliased
primitives cannot be used on a paletted board**. Anti-aliasing alpha-blends,
blending reads the buffer back, and that read dereferences a null palette and
panics. `wideLine()` in `display.cpp` exists for exactly this reason — it draws
a two-triangle quad instead of calling `drawWideLine()`.

The same assumption bit the rail client, which allocated its JSON document with
`MALLOC_CAP_SPIRAM` and therefore never parsed anything on the CYD at all.

### Flashing the CYD needs a finger

Most of these boards have no working auto-program circuit. Measured on the unit
here: the ROM reports `boot:0x13` whether DTR is asserted or not, while the chip
resets on command every time — so RTS reaches EN and DTR does not reach GPIO0.

Hold BOOT, start the flash, and keep holding until the log says it detected the
chip. The setup page says so, and recognises the specific failure if you forget.
Sending settings to an already-flashed board needs no button.

### Two panel controllers, one product name

The CYD ships with an ILI9341 or an ST7789 depending on revision. The default is
ST7789; build with `-DCYD_PANEL_ILI9341` for the older one. Getting it wrong
gives a lit screen full of garbage rather than a dark one.

The identification registers are no help: `0xD3` returned all zeroes and `0x04`
returned `C0 D9 FF`, matching neither part, because MISO is IO12 — an ESP32
strapping pin these clones often leave unwired to the LCD. Panel reads are
disabled for the same reason: the values that come back look plausible and are
not.

`diag/panel_try.cpp` settles it by putting each candidate on screen with its own
name written on it.

### Touch

The CYD's XPT2046 is wired on its own SPI bus (CLK 25, MOSI 32, MISO 39, CS 33,
IRQ 36) and responds — its Z channel produces real, jittering ADC conversions.
But on the unit tested here the touch layer itself reads as an open circuit:
`z1=1`, `x=0`, `y=4095`, and `/PENIRQ` never falls. That is a seating fault in
the ribbon, reported often enough on these clones to expect it.

Where touch does work, the left half of the screen holds the clock and the right
half steps to the next panel — the same two actions the S3's buttons drive.

---

## Diagnostics

Each of these builds instead of the firmware, and none is built by accident.

```bash
pio run -e cyd-panelid   -t upload --upload-port COM10   # ask the panel what it is
pio run -e cyd-paneltry  -t upload --upload-port COM10   # show each driver, named
pio run -e cyd-touch     -t upload --upload-port COM10   # XPT2046 via LovyanGFX
pio run -e cyd-touchraw  -t upload --upload-port COM10   # XPT2046 over raw SPI
```

They exist because each one answered a question that guessing had got wrong.

---

## Adding a third board

1. Add `include/boards/<id>.h` with the panel, pin map, geometry, buffer depth
   and input hardware. Copy the closest existing one.
2. Select it in `include/board.h` behind a `BOARD_<ID>` flag.
3. Add a PlatformIO env passing that flag, plus `BOARD_LIST_ROWS` and
   `BOARD_BIG_CLOCK_PX` if its screen is a different size.
4. Add it to `BOARDS` in `web/build-firmware.py`, with its chip family, flash
   size and bootloader offset.
5. Stage its binaries into `installer/firmware/<id>/`.

Nothing above `board.h` should need to change. If it does, the difference is in
the wrong place.
