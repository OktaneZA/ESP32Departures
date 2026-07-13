"""Convert TrueType fonts to LovyanGFX GFXfont C headers.

Rasterises each printable ASCII glyph (0x20-0x7E) to a 1-bit bitmap and emits
an `lgfx::GFXfont` (Adafruit-compatible, MSB-first, byte-aligned per glyph) that
LovyanGFX accepts directly via `setFont(&font)`.

Used to bake the project's dot-matrix TTFs into the firmware. Run from this dir:
    python ttf_to_lgfx.py
Writes ../include/dotmatrix_fonts.h
"""

from PIL import Image, ImageFont, ImageDraw

FONT_DIR = "../fonts"   # dot-matrix TTFs, relative to this docs/ folder

# (C identifier, TTF filename, pixel size) — sizes tuned for the 320x170 layout.
FONTS = [
    ("DotMatrix_Regular_22", "Dot Matrix Regular.ttf", 22),
    ("DotMatrix_Bold_22",    "Dot Matrix Bold.ttf",    22),
    ("DotMatrix_Regular_15", "Dot Matrix Regular.ttf", 15),
    ("DotMatrix_Bold_38",    "Dot Matrix Bold.ttf",    38),
]

FIRST, LAST = 0x20, 0x7E


def build_font(path, size):
    font = ImageFont.truetype(path, size)
    ascent, descent = font.getmetrics()
    line = ascent + descent

    glyphs = []          # (bitmapOffset, w, h, xAdvance, xOffset, yOffset)
    bitmap = bytearray()

    for code in range(FIRST, LAST + 1):
        ch = chr(code)
        adv = round(font.getlength(ch))

        pad = size * 2 + 4
        canvas = Image.new("L", (adv + pad * 2, line + size), 0)
        ImageDraw.Draw(canvas).text((pad, 0), ch, fill=255, font=font)
        mono = canvas.point(lambda p: 255 if p >= 128 else 0)
        bbox = mono.getbbox()

        if bbox is None:  # space / blank glyph
            glyphs.append((len(bitmap), 0, 0, adv, 0, 0))
            continue

        l, t, r, b = bbox
        w, h = r - l, b - t
        x_off = l - pad          # bearing relative to draw origin
        y_off = t - ascent       # top relative to baseline (negative = above)
        crop = mono.crop((l, t, r, b)).load()

        off = len(bitmap)
        cur = nbits = 0
        for yy in range(h):
            for xx in range(w):
                cur = (cur << 1) | (1 if crop[xx, yy] else 0)
                nbits += 1
                if nbits == 8:
                    bitmap.append(cur)
                    cur = nbits = 0
        if nbits:
            bitmap.append(cur << (8 - nbits))

        glyphs.append((off, w, h, adv, x_off, y_off))

    return line, glyphs, bytes(bitmap)


def emit(name, line, glyphs, bitmap):
    out = []
    out.append(f"// {name} — {len(bitmap)} bitmap bytes, {len(glyphs)} glyphs")
    # Bitmap array
    hexs = ", ".join(f"0x{b:02X}" for b in bitmap)
    out.append(f"static constexpr uint8_t {name}Bitmaps[] = {{ {hexs} }};")
    # Glyph array
    rows = ", ".join(
        f"{{{off},{w},{h},{adv},{xo},{yo}}}" for (off, w, h, adv, xo, yo) in glyphs
    )
    out.append(f"static constexpr lgfx::GFXglyph {name}Glyphs[] = {{ {rows} }};")
    # Font object (const_cast is safe — LovyanGFX never mutates these)
    out.append(
        f"const lgfx::GFXfont {name}"
        f"( (uint8_t*){name}Bitmaps, (lgfx::GFXglyph*){name}Glyphs, "
        f"0x{FIRST:02X}, 0x{LAST:02X}, {line} );"
    )
    return "\n".join(out)


def main():
    parts = [
        "#pragma once",
        "// Dot-matrix fonts baked from fonts/*.ttf by docs/ttf_to_lgfx.py.",
        "// Include AFTER <LovyanGFX.hpp> so lgfx::GFXfont / lgfx::GFXglyph exist.",
        "",
    ]
    for name, fname, size in FONTS:
        line, glyphs, bitmap = build_font(f"{FONT_DIR}/{fname}", size)
        parts.append(emit(name, line, glyphs, bitmap))
        parts.append("")
        print(f"{name}: {len(bitmap)} bytes")

    header = "../include/dotmatrix_fonts.h"
    with open(header, "w") as f:
        f.write("\n".join(parts))
    print(f"wrote {header}")


if __name__ == "__main__":
    main()
