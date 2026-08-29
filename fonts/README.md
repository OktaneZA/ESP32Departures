# Fonts

Baked into the firmware as `lgfx::GFXfont` C arrays by `docs/ttf_to_lgfx.py`.
The TTFs themselves are not shipped to the device — only the rasterised glyphs.

| Font | Used for | Licence |
|---|---|---|
| `Roboto-Bold.ttf` | the big clock face (night mode and the clock screen) | Apache-2.0 — see `Roboto-LICENSE.txt` |
| `Dot Matrix Bold.ttf` | the small clock on every departure screen | see note below |
| `Dot Matrix Regular.ttf` | unused at present | see note below |
| `Dot Matrix Bold Tall.ttf` | unused at present | see note below |

Roboto is from [googlefonts/roboto-2](https://github.com/googlefonts/roboto-2).
Only the digits and colon are baked in, so the cost is about 8KB of flash
rather than the ~70KB a full ASCII range would take.

> **Note on the Dot Matrix files.** These predate this documentation and arrived
> without a licence. They come from the original
> [chrisys/train-departure-display](https://github.com/chrisys/train-departure-display)
> lineage. If you are redistributing this project, satisfy yourself about their
> terms — or swap them for an openly licensed dot-matrix face and re-run the
> generator.
