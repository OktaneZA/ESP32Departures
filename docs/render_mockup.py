"""Render pixel-accurate mockups of the T-Display-S3 departure board.

Mirrors src/display.cpp (320x170 landscape, amber-on-black): FreeSans (approx.
by Arial) for the train rows, dot-matrix for the clock. Documentation aid only.
Scaled 4x for legibility.

  mockup-mot.png          live Motspur Park data
  mockup-scroll-demo.png  delayed + cancelled example (long name scrolls past status)
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


def draw_row(img, draw, y, dep, row_font, scroll_offset=0):
    colour = RED if dep.get("cancelled") else AMBER
    right = dep["status"] + (f"  P{dep['plat']}" if dep["plat"] else "")
    on_time = (dep["status"] == "On time") and not dep.get("cancelled")
    show_status = not (HIDE_ONTIME and on_time and not dep["plat"])

    rw = int(draw.textlength(right, font=f_status)) if show_status else 0
    row_h = font_h(row_font)
    status_h = font_h(f_status)

    draw.text((sx(0), sx(y)), dep["aimed"], font=row_font, fill=AMBER)

    dest_x = 70
    dest_max = W - dest_x - (rw / SCALE + 8 if show_status else 4)
    dest_w = draw.textlength(dep["dest"], font=row_font) / SCALE

    if dest_w <= dest_max:
        draw.text((sx(dest_x), sx(y)), dep["dest"], font=row_font, fill=colour)
    else:
        band = Image.new("RGB", (sx(dest_max), row_h + sx(4)), BLACK)
        bd = ImageDraw.Draw(band)
        gap = sx(32)
        dw = int(bd.textlength(dep["dest"], font=row_font))
        bd.text((sx(scroll_offset), 0), dep["dest"], font=row_font, fill=colour)
        bd.text((sx(scroll_offset) + dw + gap, 0), dep["dest"], font=row_font, fill=colour)
        img.paste(band, (sx(dest_x), sx(y)))

    if show_status:
        draw.text((sx(W) - rw, sx(y) + (row_h - status_h) / 2), right, font=f_status, fill=colour)


def render(departures, out_name, scroll_offsets=None):
    scroll_offsets = scroll_offsets or {}
    img = Image.new("RGB", (sx(W), sx(H)), BLACK)
    draw = ImageDraw.Draw(img)

    ys = [6, 42, 78]
    fonts = [f_row_bold, f_row, f_row]
    for i, dep in enumerate(departures[:3]):
        draw_row(img, draw, ys[i], dep, fonts[i], scroll_offsets.get(i, 0))

    now = datetime.now()
    clock = now.strftime("%H:%M:%S")
    cw = draw.textlength(clock, font=f_clock)
    draw.text(((sx(W) - cw) / 2, sx(120)), clock, font=f_clock, fill=AMBER)

    bez = 40
    canvas = Image.new("RGB", (sx(W) + bez * 2, sx(H) + bez * 2), (28, 28, 30))
    ImageDraw.Draw(canvas).rounded_rectangle(
        [bez - 6, bez - 6, bez + sx(W) + 5, bez + sx(H) + 5],
        radius=10, outline=(70, 70, 74), width=3)
    canvas.paste(img, (bez, bez))
    canvas.save(out_name)
    print(f"wrote {out_name}")


render([
    {"aimed": "17:10", "dest": "Chessington South", "status": "On time", "plat": ""},
    {"aimed": "17:15", "dest": "London Waterloo",    "status": "On time", "plat": ""},
    {"aimed": "17:16", "dest": "Guildford",          "status": "On time", "plat": ""},
], "mockup-mot.png", scroll_offsets={0: -45})

render([
    {"aimed": "17:10", "dest": "Chessington South", "status": "Exp 17:22", "plat": ""},
    {"aimed": "17:15", "dest": "London Waterloo",    "status": "On time",  "plat": ""},
    {"aimed": "17:16", "dest": "Guildford",          "status": "Cancelled", "plat": "", "cancelled": True},
], "mockup-scroll-demo.png", scroll_offsets={0: -55})
