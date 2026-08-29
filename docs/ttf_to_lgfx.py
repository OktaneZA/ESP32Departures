"""Convert TrueType fonts to LovyanGFX GFXfont C headers.

Rasterises each printable ASCII glyph (0x20-0x7E) to a 1-bit bitmap and emits
an `lgfx::GFXfont` (Adafruit-compatible, MSB-first, byte-aligned per glyph) that
LovyanGFX accepts directly via `setFont(&font)`.

Used to bake the project's dot-matrix TTFs into the firmware. Run from this dir:
    python ttf_to_lgfx.py
Writes ../include/dotmatrix_fonts.h
"""

import os

from PIL import Image, ImageFont, ImageDraw

FONT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "fonts")

# A GFXfont covers one contiguous codepoint range, so a subset is expressed as
# narrower first/last bounds rather than an arbitrary set.
ASCII = (0x20, 0x7E)      # everything printable
DIGITS = (0x2E, 0x3A)     # . / 0-9 : — all the big clock needs

# (C identifier, TTF filename, pixel size, glyph range).
# Sizes are tuned for the 320x170 layout.
FONTS = [
    ("DotMatrix_Regular_22", "Dot Matrix Regular.ttf", 22, ASCII),
    ("DotMatrix_Bold_22",    "Dot Matrix Bold.ttf",    22, ASCII),
    ("DotMatrix_Regular_15", "Dot Matrix Regular.ttf", 15, ASCII),
    ("DotMatrix_Bold_38",    "Dot Matrix Bold.ttf",    38, ASCII),
    # The full-screen clock. Digits and colon only: at this size the full ASCII
    # range would cost roughly 70KB of flash to bake 84 glyphs nothing draws.
    ("Clock_Bold_104",       "Roboto-Bold.ttf",       104, DIGITS),
]


def build_font(path, size, rng):
    first, last = rng
    font = ImageFont.truetype(path, size)
    ascent, descent = font.getmetrics()
    line = ascent + descent

    glyphs = []          # (bitmapOffset, w, h, xAdvance, xOffset, yOffset)
    bitmap = bytearray()

    for code in range(first, last + 1):
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


def emit(name, line, glyphs, bitmap, rng):
    first, last = rng
    out = []
    out.append(f"// {name} — {len(bitmap)} bitmap bytes, {len(glyphs)} glyphs "
               f"(0x{first:02X}-0x{last:02X})")
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
        f"0x{first:02X}, 0x{last:02X}, {line} );"
    )
    return "\n".join(out)


def main():
    parts = [
        "#pragma once",
        "// Fonts baked from fonts/*.ttf by docs/ttf_to_lgfx.py.",
        "// Dot-matrix faces for the boards; Roboto Bold (Apache-2.0) for the",
        "// full-screen clock. See fonts/README.md for licensing.",
        "// Include AFTER <LovyanGFX.hpp> so lgfx::GFXfont / lgfx::GFXglyph exist.",
        "",
    ]
    total = 0
    for name, fname, size, rng in FONTS:
        line, glyphs, bitmap = build_font(os.path.join(FONT_DIR, fname), size, rng)
        parts.append(emit(name, line, glyphs, bitmap, rng))
        parts.append("")
        total += len(bitmap)
        print(f"  {name}: {len(bitmap):,} bytes, {len(glyphs)} glyphs")
    print(f"  total: {total:,} bytes of flash")

    header = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "..", "include", "dotmatrix_fonts.h")
    with open(header, "w") as f:
        f.write("\n".join(parts))
    print(f"wrote {header}")


if __name__ == "__main__":
    main()
