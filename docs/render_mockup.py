"""Render pixel-accurate mockups of the T-Display-S3 board, trains and buses.

Mirrors src/display.cpp (320x170 landscape, amber-on-black), including its shared
layout: FreeSans (approximated by Arial) for the header and rows, dot-matrix for
the clock, and the same row geometry constants. Documentation aid only, so layout
changes can be checked without flashing a board. Scaled 4x for legibility.

Run it with no arguments to regenerate all four:

  mockup-mot.png          train board, live Motspur Park data
  mockup-scroll-demo.png  train board, delayed + cancelled, long name clipped
  mockup-bus.png          London bus arrivals screen
  mockup-bus-busy.png     bus screen with a destination too long for its column
"""

from datetime import datetime
from PIL import Image, ImageFont, ImageDraw

W, H = 320, 170
SCALE = 4
DM_DIR = "../fonts"
WIN = "C:/Windows/Fonts"

AMBER = (255, 176, 0)
RED = (255, 40, 40)
BLACK = (0, 0, 0)

HIDE_ONTIME = False  # matches HIDE_ONTIME_STATUS in app_config.h

# Train rows: FreeSans-like (Arial). Clock: dot-matrix. Sizes match display.cpp.
f_row_bold = ImageFont.truetype(f"{WIN}/arialbd.ttf", 22 * SCALE)   # FreeSansBold12pt7b
f_row = ImageFont.truetype(f"{WIN}/arial.ttf", 22 * SCALE)          # FreeSans12pt7b
f_status = ImageFont.truetype(f"{WIN}/arial.ttf", 15 * SCALE)       # FreeSans9pt7b
f_clock = ImageFont.truetype(f"{DM_DIR}/Dot Matrix Bold.ttf", 38 * SCALE)  # DotMatrix_Bold_38


def sx(v):
    return int(v * SCALE)


def font_h(fnt):
    a, d = fnt.getmetrics()
    return a + d


def draw_row(img, draw, y, dep):
    """Mirrors drawRow(): small time, destination, small status right-aligned."""
    colour = RED if dep.get("cancelled") else AMBER
    right = dep["status"] + (f"  P{dep['plat']}" if dep["plat"] else "")
    on_time = (dep["status"] == "On time") and not dep.get("cancelled")
    show_status = not (HIDE_ONTIME and on_time and not dep["plat"])

    row_h = font_h(f_row)
    small_h = font_h(f_status)
    dy = (row_h - small_h) / 2
    rw = int(draw.textlength(right, font=f_status)) if show_status else 0

    draw.text((sx(0), sx(y) + dy), dep["aimed"], font=f_status, fill=AMBER)
    if show_status:
        draw.text((sx(W) - rw, sx(y) + dy), right, font=f_status, fill=colour)

    dest_max = W - TRAIN_DEST_X - (rw / SCALE + 8 if show_status else 4)
    clip(img, draw, dep["dest"], f_row, TRAIN_DEST_X, y, dest_max, colour)


ROW_Y0, ROW_STEP, CLOCK_Y = 32, 28, 120
TRAIN_DEST_X, BUS_ROUTE_X, BUS_DEST_X = 48, 48, 100
DIM = (140, 96, 0)


def draw_header(img, draw, tag, name):
    """Mirrors drawHeader(): small dim mode tag, then the name in the head font."""
    tag_h = font_h(f_status)
    tag_w = draw.textlength(tag, font=f_status) / SCALE + 10
    head_h = font_h(f_row_bold)
    draw.text((sx(0), (head_h - tag_h) / 2), tag, font=f_status, fill=DIM)
    clip(img, draw, name, f_row_bold, tag_w, 0, W - tag_w, AMBER)


def clip(img, draw, text, fnt, x, y, maxw, fill):
    """Draw text, cropping to `maxw` the way the on-device marquee clips it."""
    w = draw.textlength(text, font=fnt) / SCALE
    if w <= maxw:
        draw.text((sx(x), sx(y)), text, font=fnt, fill=fill)
        return
    band = Image.new("RGB", (sx(maxw), font_h(fnt) + sx(4)), BLACK)
    ImageDraw.Draw(band).text((0, 0), text, font=fnt, fill=fill)
    img.paste(band, (sx(x), sx(y)))


def draw_bus_row(img, draw, y, ar):
    """Mirrors drawBusRow(): small time, route, destination, small ETA right."""
    row_h = font_h(f_row)
    small_h = font_h(f_status)
    dy = (row_h - small_h) / 2
    ew = int(draw.textlength(ar["eta"], font=f_status))

    draw.text((sx(0), sx(y) + dy), ar["when"], font=f_status, fill=AMBER)
    draw.text((sx(W) - ew, sx(y) + dy), ar["eta"], font=f_status, fill=AMBER)
    draw.text((sx(BUS_ROUTE_X), sx(y)), ar["line"], font=f_row, fill=AMBER)
    clip(img, draw, ar["dest"], f_row, BUS_DEST_X, y, W - BUS_DEST_X - ew / SCALE - 8, AMBER)


def render_bus(stop_name, line_filter, arrivals, out_name):
    """Mirrors renderBusBoard()."""
    img = Image.new("RGB", (sx(W), sx(H)), BLACK)
    draw = ImageDraw.Draw(img)
    draw_header(img, draw, f"BUS {line_filter}" if line_filter else "BUS", stop_name)

    if not arrivals:
        msg = "No buses due"
        draw.text(((sx(W) - draw.textlength(msg, font=f_row_bold)) / 2, sx(52)),
                  msg, font=f_row_bold, fill=DIM)
    else:
        for i, ar in enumerate(arrivals[:3]):
            draw_bus_row(img, draw, ROW_Y0 + i * ROW_STEP, ar)

    finish(img, draw, out_name)


def finish(img, draw, out_name):
    clock = datetime.now().strftime("%H:%M:%S")
    cw = draw.textlength(clock, font=f_clock)
    draw.text(((sx(W) - cw) / 2, sx(CLOCK_Y)), clock, font=f_clock, fill=AMBER)
    bez = 40
    canvas = Image.new("RGB", (sx(W) + bez * 2, sx(H) + bez * 2), (28, 28, 30))
    ImageDraw.Draw(canvas).rounded_rectangle(
        [bez - 6, bez - 6, bez + sx(W) + 5, bez + sx(H) + 5],
        radius=10, outline=(70, 70, 74), width=3)
    canvas.paste(img, (bez, bez))
    canvas.save(out_name)
    print(f"wrote {out_name}")


def render(departures, out_name, station="Motspur Park", scroll_offsets=None):
    """Mirrors renderBoard()."""
    img = Image.new("RGB", (sx(W), sx(H)), BLACK)
    draw = ImageDraw.Draw(img)
    draw_header(img, draw, "TRAIN", station)

    if not departures:
        msg = "No departures"
        draw.text(((sx(W) - draw.textlength(msg, font=f_row_bold)) / 2, sx(52)),
                  msg, font=f_row_bold, fill=DIM)
    else:
        for i, dep in enumerate(departures[:3]):
            draw_row(img, draw, ROW_Y0 + i * ROW_STEP, dep)

    finish(img, draw, out_name)


if __name__ == "__main__":
    render([
        {"aimed": "17:10", "dest": "Chessington South", "status": "On time", "plat": ""},
        {"aimed": "17:15", "dest": "London Waterloo",   "status": "On time", "plat": ""},
        {"aimed": "17:16", "dest": "Guildford",         "status": "On time", "plat": ""},
    ], "mockup-mot.png")

    render([
        {"aimed": "17:10", "dest": "Chessington South", "status": "Exp 17:22", "plat": ""},
        {"aimed": "17:15", "dest": "London Waterloo",   "status": "On time",   "plat": ""},
        {"aimed": "17:16", "dest": "Guildford",         "status": "Cancelled", "plat": "",
         "cancelled": True},
    ], "mockup-scroll-demo.png")

    render_bus("Motspur Park Station", "", [
        {"when": "19:07", "line": "K5",  "dest": "Ham",      "eta": "3 min"},
        {"when": "19:15", "line": "131", "dest": "Kingston", "eta": "11 min"},
        {"when": "19:28", "line": "K5",  "dest": "Morden",   "eta": "24 min"},
    ], "mockup-bus.png")

    render_bus("Green Park Station", "", [
        {"when": "19:05", "line": "38",  "dest": "Clapton Pond",         "eta": "Due"},
        {"when": "19:09", "line": "N19", "dest": "Tottenham Court Road", "eta": "5 min"},
        {"when": "19:14", "line": "14",  "dest": "Russell Square",       "eta": "10 min"},
    ], "mockup-bus-busy.png")
