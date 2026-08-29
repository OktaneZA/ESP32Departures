"""Render pixel-accurate mockups of the board: trains, London buses, river boats.

Mirrors src/display.cpp (320x170 landscape) including its shared layout:
FreeSans (approximated by Arial) for the header and rows, dot-matrix for the
clock, and the same geometry constants. Documentation aid only, so layout
changes can be checked without flashing a board. Scaled 4x for legibility.

Colours are derived from the firmware's own RGB565 constants rather than
eyeballed, and any theme can be rendered by passing a different palette — the
board's colours are runtime settings now, so the mockups can show them.

Bus and river share one renderer here because they share one on the device:
drawArrivalsBoard() draws both, differing only in tag and empty-state text.

Run with no arguments to regenerate everything:

  mockup-train.png        train board
  mockup-bus.png          London bus arrivals
  mockup-river.png        river boat sailings
  mockup-clock.png        the full-screen clock
  mockup-weather.png      current conditions
  mockup-scroll-demo.png  train board, delayed + cancelled
  mockup-bus-busy.png     bus screen, destination too long for its column
"""

import os
from datetime import datetime, timedelta
from PIL import Image, ImageFont, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
FONT_DIR = os.path.join(HERE, "..", "fonts")
WIN = "C:/Windows/Fonts"

W, H = 320, 170
SCALE = 4

HIDE_ONTIME = False  # matches HIDE_ONTIME_STATUS in app_config.h

# Geometry, straight from display.cpp.
ROW_Y0, ROW_STEP, CLOCK_Y = 32, 28, 120
TRAIN_DEST_X, BUS_ROUTE_X, BUS_DEST_X = 48, 48, 100


def rgb565(v):
    """The exact colour the 16-bit panel produces for an RGB565 value."""
    r5, g6, b5 = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
    return ((r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2))


# display.cpp's defaults: AMBER, DIM, RED, BLACK.
CLASSIC = {"fg": rgb565(0xFD20), "dim": rgb565(0x8300),
           "warn": rgb565(0xF800), "bg": rgb565(0x0000)}

f_row_bold = ImageFont.truetype(f"{WIN}/arialbd.ttf", 22 * SCALE)   # FreeSansBold12pt7b
f_row = ImageFont.truetype(f"{WIN}/arial.ttf", 22 * SCALE)          # FreeSans12pt7b
f_status = ImageFont.truetype(f"{WIN}/arial.ttf", 15 * SCALE)       # FreeSans9pt7b
f_clock = ImageFont.truetype(os.path.join(FONT_DIR, "Dot Matrix Bold.ttf"), 38 * SCALE)
# The big clock, matching Clock_Bold_104 baked from the same TTF by ttf_to_lgfx.py.
f_big = ImageFont.truetype(os.path.join(FONT_DIR, "Roboto-Bold.ttf"), 104 * SCALE)
f_temp = ImageFont.truetype(f"{WIN}/arialbd.ttf", 34 * SCALE)   # FreeSansBold24pt7b
f_row_bold_sm = ImageFont.truetype(f"{WIN}/arialbd.ttf", 15 * SCALE)  # FreeSansBold9pt7b


def sx(v):
    return int(v * SCALE)


def at(mins):
    """Clock time `mins` from now, so the rows agree with the clock below them."""
    return (datetime.now() + timedelta(minutes=mins)).strftime("%H:%M")


def eta(mins):
    """The countdown wording the board uses: "Due" under a minute."""
    return "Due" if mins < 1 else f"{mins} min"


def font_h(fnt):
    a, d = fnt.getmetrics()
    return a + d


def clip(img, draw, text, fnt, x, y, maxw, fill, bg):
    """Draw text, cropping to `maxw` the way the on-device marquee clips it."""
    w = draw.textlength(text, font=fnt) / SCALE
    if w <= maxw:
        draw.text((sx(x), sx(y)), text, font=fnt, fill=fill)
        return
    band = Image.new("RGB", (sx(maxw), font_h(fnt) + sx(4)), bg)
    ImageDraw.Draw(band).text((0, 0), text, font=fnt, fill=fill)
    img.paste(band, (sx(x), sx(y)))


def draw_header(img, draw, tag, name, pal):
    """Mirrors drawHeader(): small dim mode tag, then the name in the head font."""
    tag_h = font_h(f_status)
    tag_w = draw.textlength(tag, font=f_status) / SCALE + 10
    head_h = font_h(f_row_bold)
    draw.text((sx(0), (head_h - tag_h) / 2), tag, font=f_status, fill=pal["dim"])
    clip(img, draw, name, f_row_bold, tag_w, 0, W - tag_w, pal["fg"], pal["bg"])


def draw_row(img, draw, y, dep, pal):
    """Mirrors drawRow(): small time, destination, small status right-aligned."""
    colour = pal["warn"] if dep.get("cancelled") else pal["fg"]
    right = dep["status"] + (f"  P{dep['plat']}" if dep["plat"] else "")
    on_time = (dep["status"] == "On time") and not dep.get("cancelled")
    show_status = not (HIDE_ONTIME and on_time and not dep["plat"])

    dy = (font_h(f_row) - font_h(f_status)) / 2
    rw = int(draw.textlength(right, font=f_status)) if show_status else 0

    draw.text((sx(0), sx(y) + dy), dep["aimed"], font=f_status, fill=pal["fg"])
    if show_status:
        draw.text((sx(W) - rw, sx(y) + dy), right, font=f_status, fill=colour)

    dest_max = W - TRAIN_DEST_X - (rw / SCALE + 8 if show_status else 4)
    clip(img, draw, dep["dest"], f_row, TRAIN_DEST_X, y, dest_max, colour, pal["bg"])


def draw_arrival_row(img, draw, y, ar, pal):
    """Mirrors drawArrivalRow(): time, route, destination, countdown right."""
    dy = (font_h(f_row) - font_h(f_status)) / 2
    ew = int(draw.textlength(ar["eta"], font=f_status))

    draw.text((sx(0), sx(y) + dy), ar["when"], font=f_status, fill=pal["fg"])
    draw.text((sx(W) - ew, sx(y) + dy), ar["eta"], font=f_status, fill=pal["fg"])
    draw.text((sx(BUS_ROUTE_X), sx(y)), ar["line"], font=f_row, fill=pal["fg"])
    clip(img, draw, ar["dest"], f_row, BUS_DEST_X, y,
         W - BUS_DEST_X - ew / SCALE - 8, pal["fg"], pal["bg"])


def finish(img, draw, out_name, pal):
    clock = datetime.now().strftime("%H:%M:%S")
    cw = draw.textlength(clock, font=f_clock)
    draw.text(((sx(W) - cw) / 2, sx(CLOCK_Y)), clock, font=f_clock, fill=pal["fg"])

    bez = 40
    canvas = Image.new("RGB", (sx(W) + bez * 2, sx(H) + bez * 2), (28, 28, 30))
    ImageDraw.Draw(canvas).rounded_rectangle(
        [bez - 6, bez - 6, bez + sx(W) + 5, bez + sx(H) + 5],
        radius=10, outline=(70, 70, 74), width=3)
    canvas.paste(img, (bez, bez))
    out = os.path.join(HERE, out_name)
    canvas.save(out)
    print(f"  wrote {out_name}")


def render_train(departures, out_name, station="Motspur Park", pal=CLASSIC):
    """Mirrors renderBoard()."""
    img = Image.new("RGB", (sx(W), sx(H)), pal["bg"])
    draw = ImageDraw.Draw(img)
    draw_header(img, draw, "TRAIN", station, pal)

    if not departures:
        msg = "No departures"
        draw.text(((sx(W) - draw.textlength(msg, font=f_row_bold)) / 2, sx(52)),
                  msg, font=f_row_bold, fill=pal["dim"])
    else:
        for i, dep in enumerate(departures[:3]):
            draw_row(img, draw, ROW_Y0 + i * ROW_STEP, dep, pal)

    finish(img, draw, out_name, pal)


def render_arrivals(tag, name, arrivals, empty_msg, out_name, pal=CLASSIC):
    """Mirrors drawArrivalsBoard(), which draws both the bus and river screens."""
    img = Image.new("RGB", (sx(W), sx(H)), pal["bg"])
    draw = ImageDraw.Draw(img)
    draw_header(img, draw, tag, name, pal)

    if not arrivals:
        draw.text(((sx(W) - draw.textlength(empty_msg, font=f_row_bold)) / 2, sx(52)),
                  empty_msg, font=f_row_bold, fill=pal["dim"])
    else:
        for i, ar in enumerate(arrivals[:3]):
            draw_arrival_row(img, draw, ROW_Y0 + i * ROW_STEP, ar, pal)

    finish(img, draw, out_name, pal)


def render_clock(out_name, pal=CLASSIC, drift=(0, 0)):
    """Mirrors renderClock(): HH:MM filling the panel, with the flip-clock seam.
    `drift` is the few-pixel nudge night mode applies so nothing sits still."""
    img = Image.new("RGB", (sx(W), sx(H)), pal["bg"])
    draw = ImageDraw.Draw(img)

    now = datetime.now().strftime("%H:%M")
    bb = draw.textbbox((0, 0), now, font=f_big)
    x = (sx(W) - (bb[2] - bb[0])) / 2 - bb[0] + sx(drift[0])
    y = (sx(H) - (bb[3] - bb[1])) / 2 - bb[1] + sx(drift[1])
    draw.text((x, y), now, font=f_big, fill=pal["fg"])

    # The seam, drawn in the background colour so it reads as a fold rather
    # than a line laid over the digits.
    mid = sx(H) / 2 + sx(drift[1])
    draw.rectangle([0, mid - SCALE, sx(W), mid + SCALE], fill=pal["bg"])
    bezel_and_save(img, out_name)


# --- weather icons ----------------------------------------------------------
# Drawn from primitives rather than shipped as bitmaps: they cost almost no
# flash, scale to any size, and take the theme colour for free. Mirrors what
# drawWeatherIcon() does on the device.

def _sun(draw, cx, cy, r, fill, rays=True):
    draw.ellipse([cx - r, cy - r, cx + r, cy + r], fill=fill)
    if not rays:
        return
    import math
    for i in range(8):
        a = i * math.pi / 4
        x0, y0 = cx + math.cos(a) * r * 1.45, cy + math.sin(a) * r * 1.45
        x1, y1 = cx + math.cos(a) * r * 2.05, cy + math.sin(a) * r * 2.05
        draw.line([x0, y0, x1, y1], fill=fill, width=max(2, int(r * 0.28)))


def _cloud(draw, cx, cy, w, fill):
    """A cloud as three lobes over a slab — the shape reads at 60px."""
    r = w * 0.30
    draw.ellipse([cx - w * 0.46, cy - r * 0.5, cx - w * 0.46 + r * 1.6, cy - r * 0.5 + r * 1.6], fill=fill)
    draw.ellipse([cx - w * 0.12, cy - r * 1.05, cx - w * 0.12 + r * 2.0, cy - r * 1.05 + r * 2.0], fill=fill)
    draw.ellipse([cx + w * 0.12, cy - r * 0.35, cx + w * 0.12 + r * 1.5, cy - r * 0.35 + r * 1.5], fill=fill)
    draw.rounded_rectangle([cx - w * 0.48, cy + r * 0.15, cx + w * 0.48, cy + r * 0.95],
                           radius=r * 0.4, fill=fill)


def _drops(draw, cx, cy, w, fill, n=3, slant=True):
    for i in range(n):
        x = cx + (i - (n - 1) / 2) * w * 0.26
        dx = w * 0.10 if slant else 0
        draw.line([x + dx, cy, x - dx, cy + w * 0.30], fill=fill, width=max(2, int(w * 0.055)))


def _flakes(draw, cx, cy, w, fill, n=3):
    for i in range(n):
        x = cx + (i - (n - 1) / 2) * w * 0.26
        r = w * 0.055
        draw.ellipse([x - r, cy + w * 0.10 - r, x + r, cy + w * 0.10 + r], fill=fill)


def draw_weather_icon(draw, code, cx, cy, size, fill, bg=(0, 0, 0)):
    """`size` is the icon's nominal width; it is drawn centred on (cx, cy).
    `bg` is used to knock gaps between overlapping shapes, which a one-colour
    panel otherwise fuses into an unreadable blob."""
    w = size
    if code == 0:                                    # clear
        _sun(draw, cx, cy, w * 0.24, fill)
    elif code in (1, 2):                             # sun behind cloud
        _sun(draw, cx + w * 0.22, cy - w * 0.24, w * 0.16, fill)
        _cloud(draw, cx - w * 0.06, cy + w * 0.14, w * 0.92, bg)   # knockout
        _cloud(draw, cx - w * 0.06, cy + w * 0.14, w * 0.80, fill)
    elif code == 3:                                  # overcast
        _cloud(draw, cx, cy, w * 0.95, fill)
    elif code in (45, 48):                           # fog
        _cloud(draw, cx, cy - w * 0.20, w * 0.85, fill)
        # Clear of the cloud base, or the bars fuse with it into a barcode.
        for i in range(3):
            y = cy + w * 0.22 + i * w * 0.15
            draw.line([cx - w * 0.36, y, cx + w * 0.36, y], fill=fill, width=max(2, int(w * 0.05)))
    elif code in (51, 53, 55, 56, 57):               # drizzle
        _cloud(draw, cx, cy - w * 0.14, w * 0.85, fill)
        _drops(draw, cx, cy + w * 0.22, w, fill, 3, slant=False)
    elif code in (61, 63, 65, 66, 67, 80, 81, 82):   # rain / showers
        _cloud(draw, cx, cy - w * 0.14, w * 0.85, fill)
        _drops(draw, cx, cy + w * 0.20, w, fill, 3)
    elif code in (71, 73, 75, 77, 85, 86):           # snow
        _cloud(draw, cx, cy - w * 0.14, w * 0.85, fill)
        _flakes(draw, cx, cy + w * 0.18, w, fill)
    elif code in (95, 96, 99):                       # thunderstorm
        _cloud(draw, cx, cy - w * 0.16, w * 0.85, fill)
        b = w * 0.20
        draw.polygon([(cx + b * 0.15, cy + w * 0.10), (cx - b * 0.55, cy + w * 0.46),
                      (cx - b * 0.05, cy + w * 0.44), (cx - b * 0.35, cy + w * 0.80),
                      (cx + b * 0.65, cy + w * 0.34), (cx + b * 0.10, cy + w * 0.36)], fill=fill)
    else:
        _cloud(draw, cx, cy, w * 0.95, fill)


def render_weather(place, temp, cond, rows, out_name, pal=CLASSIC, code=2):
    """Mirrors renderWeatherBoard(): the shared header and clock, with a large
    temperature and condition over two dim detail rows. The body must fit
    between the header and CLOCK_Y or it collides with the clock."""
    img = Image.new("RGB", (sx(W), sx(H)), pal["bg"])
    draw = ImageDraw.Draw(img)
    draw_header(img, draw, "WEATHER", place, pal)

    draw.text((sx(0), sx(30)), temp, font=f_temp, fill=pal["fg"])
    tw = draw.textlength(temp, font=f_temp) / SCALE
    draw.text((sx(tw + 10), sx(40)), cond, font=f_row, fill=pal["fg"])

    # Detail rows in the bold row font at full brightness. They were the dim
    # secondary colour, which is for the mode tag — here they are real data and
    # were the hardest thing on the board to read across a room.
    for i, r in enumerate(rows):
        draw.text((sx(0), sx(72 + i * 20)), r, font=f_row_bold_sm, fill=pal["fg"])

    # The icon fills the space the text leaves on the right.
    draw_weather_icon(draw, code, sx(262), sx(68), sx(64), pal["fg"], pal["bg"])

    finish(img, draw, out_name, pal)


def bezel_and_save(img, out_name):
    """The clock has no ticking clock of its own to draw, so it skips finish()."""
    bez = 40
    canvas = Image.new("RGB", (sx(W) + bez * 2, sx(H) + bez * 2), (28, 28, 30))
    ImageDraw.Draw(canvas).rounded_rectangle(
        [bez - 6, bez - 6, bez + sx(W) + 5, bez + sx(H) + 5],
        radius=10, outline=(70, 70, 74), width=3)
    canvas.paste(img, (bez, bez))
    out = os.path.join(HERE, out_name)
    canvas.save(out)
    print(f"  wrote {out_name}")


def render_bus(stop, line_filter, arrivals, out_name, pal=CLASSIC):
    render_arrivals(f"BUS {line_filter}" if line_filter else "BUS",
                    stop, arrivals, "No buses due", out_name, pal)


def render_river(pier, line_filter, sailings, out_name, pal=CLASSIC):
    render_arrivals(f"RIVER {line_filter}" if line_filter else "RIVER",
                    pier, sailings, "No boats due", out_name, pal)


if __name__ == "__main__":
    print("Rendering board mockups...")

    render_train([
        {"aimed": at(4),  "dest": "Chessington South", "status": "On time", "plat": "2"},
        {"aimed": at(9),  "dest": "London Waterloo",   "status": "On time", "plat": "1"},
        {"aimed": at(19), "dest": "Guildford",         "status": "On time", "plat": "2"},
    ], "mockup-train.png")

    render_bus("Wimbledon Station", "", [
        {"when": at(3),  "line": "93",  "dest": "Putney Bridge", "eta": eta(3)},
        {"when": at(11), "line": "200", "dest": "Raynes Park",   "eta": eta(11)},
        {"when": at(24), "line": "163", "dest": "Morden",        "eta": eta(24)},
    ], "mockup-bus.png")

    render_river("Vauxhall St George Wharf Pier", "", [
        {"when": at(17), "line": "RB6", "dest": "Blackfriars",       "eta": eta(17)},
        {"when": at(23), "line": "RB6", "dest": "Putney Pier",       "eta": eta(23)},
        {"when": at(41), "line": "RB1", "dest": "Barking Riverside", "eta": eta(41)},
    ], "mockup-river.png")

    render_clock("mockup-clock.png")

    render_weather("Motspur Park", "18°", "Partly cloudy",
                   ["Feels 17°   Wind 12 mph", "High 21°   Low 14°"],
                   "mockup-weather.png", code=2)

    # Edge cases worth being able to eyeball after a layout change.
    render_train([
        {"aimed": at(4),  "dest": "Chessington South", "status": f"Exp {at(16)}", "plat": ""},
        {"aimed": at(9),  "dest": "London Waterloo",   "status": "On time",       "plat": ""},
        {"aimed": at(19), "dest": "Guildford",         "status": "Cancelled",     "plat": "",
         "cancelled": True},
    ], "mockup-scroll-demo.png")

    render_bus("Green Park Station", "", [
        {"when": at(0),  "line": "38",  "dest": "Clapton Pond",         "eta": eta(0)},
        {"when": at(5),  "line": "N19", "dest": "Tottenham Court Road", "eta": eta(5)},
        {"when": at(10), "line": "14",  "dest": "Russell Square",       "eta": eta(10)},
    ], "mockup-bus-busy.png")
