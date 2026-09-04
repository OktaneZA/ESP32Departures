// Display + rendering.
//
// The panel, its pin map and the screen's size all come from board.h, so this
// file is about layout and nothing else. Two boards are supported and neither
// is named below deliberately: anything that needs to know which one it is has
// got the abstraction wrong.
//
// Uses LovyanGFX with a full-frame sprite as a back buffer so the scrolling
// destinations and ticking clock render without flicker. On the S3 that buffer
// lives in PSRAM and costs no DRAM; on a board without PSRAM it is a real
// allocation, which is why ui::begin() reports what it cost.
//
// Layout is landscape (rotation 1) — a wide "platform sign" shape, reimagining
// the Python app's 256x64 board with room for a big clock.
//
// Three boards are drawn here — trains, London buses and river boats — and they
// deliberately share one layout: a header row (mode tag + station/stop/pier
// name), three identical rows, then the clock. The shared geometry constants,
// drawHeader() and drawArrivalsBoard() below are what keep them from drifting
// apart; buses and boats differ only in their tag and their "nothing due" text.

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "dotmatrix_fonts.h"  // baked dot-matrix GFX fonts (needs lgfx types above)

#include "display.h"
#include "board.h"          // panel, pin map, geometry — see the note there
#include "app_config.h"
#include <time.h>

namespace {

// The screen, as the layout sees it: landscape, after setRotation(1).
constexpr int W = board::SCREEN_W;
constexpr int H = board::SCREEN_H;

// The board's four colours, and what to pass to a draw call to get one.
//
// On a direct-colour buffer these variables hold RGB565 values, exactly as they
// always did. On a board whose buffer is a palette they hold palette *indices*
// instead, and the real colours live in s_rgb. Either way the ~60 call sites
// below say BLACK or AMBER and do not care which board they are on — which is
// the only reason a four-bit buffer was cheap enough to be worth having.
uint16_t BLACK, AMBER, RED, DIM;

// The actual RGB565 colours, in palette-index order. Defaults are the classic
// departure-board amber-on-black, used verbatim when nothing was provisioned.
constexpr int PAL_N = 4;
uint16_t s_rgb[PAL_N] = {
    0x0000,   // background
    0xFD20,   // classic departure-board amber
    0xF800,   // cancellations
    0x8300,   // dimmed amber for secondary text
};

constexpr bool PALETTED = board::COLOR_DEPTH <= 8;


board::Display lcd;
lgfx::LGFX_Sprite spr(&lcd);

// A thick line, with or without anti-aliasing depending on what the buffer can
// take.
//
// LovyanGFX's drawWideLine() is anti-aliased, and anti-aliasing means alpha
// blending, which means reading the buffer back to blend against. That read
// path dereferences a null palette on a paletted sprite and panics the board:
//
//   draw_wedgeline -> fillRectAlpha -> readRect -> copy_palette_affine
//   Guru Meditation Error: Core 1 panic'ed (LoadProhibited)
//
// So on a paletted board the same shape is built from two triangles instead:
// hard-edged, which is what a four-colour board wants anyway, since blended
// pixels have no palette entry to land in.
void wideLine(float x0, float y0, float x1, float y1, float w, uint16_t colour) {
    if (!PALETTED) {
        spr.drawWideLine(x0, y0, x1, y1, w, colour);
        return;
    }
    const float dx = x1 - x0, dy = y1 - y0;
    const float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.01f) return;
    // Half-width perpendicular to the line, giving the quad's four corners.
    const float nx = -dy / len * (w * 0.5f);
    const float ny =  dx / len * (w * 0.5f);
    const int ax = lroundf(x0 + nx), ay = lroundf(y0 + ny);
    const int bx = lroundf(x0 - nx), by = lroundf(y0 - ny);
    const int cx = lroundf(x1 - nx), cy = lroundf(y1 - ny);
    const int dx2 = lroundf(x1 + nx), dy2 = lroundf(y1 + ny);
    spr.fillTriangle(ax, ay, bx, by, cx, cy, colour);
    spr.fillTriangle(ax, ay, cx, cy, dx2, dy2, colour);
}

// Publish s_rgb to wherever the draw calls will actually read it from.
void applyPalette() {
    if (PALETTED) {
        for (int i = 0; i < PAL_N; ++i) spr.setPaletteColor(i, s_rgb[i]);
        BLACK = 0; AMBER = 1; RED = 2; DIM = 3;
    } else {
        BLACK = s_rgb[0]; AMBER = s_rgb[1]; RED = s_rgb[2]; DIM = s_rgb[3];
    }
}

// Per-row horizontal scroll state for destination names that overflow their
// column. Reset (offset 0) when the row's text changes or when it fits.
struct RowScroll {
    String text;
    int offset = 0;
};
RowScroll s_rowScroll[MAX_DEPARTURES];
RowScroll s_busScroll[MAX_BUS_ARRIVALS];
RowScroll s_riverScroll[MAX_RIVER_ARRIVALS];
RowScroll s_wxScroll;          // the weather condition, if it overflows
RowScroll s_headerScroll;

// The user's chosen brightness, remembered so the night clock can dim the panel
// and every other screen can put it back without consulting the config.
uint8_t s_brightness = BRIGHTNESS;

// All three boards share one layout so they read as the same instrument: a
// header row naming the mode and the station/stop/pier, then three identical
// rows, then the clock. Times sit in the small font — they are fixed-width and always legible,
// so shrinking them buys the destination column ~20px it can actually use.
const lgfx::IFont* const HEAD_FONT  = &fonts::FreeSansBold12pt7b;
const lgfx::IFont* const ROW_FONT   = &fonts::FreeSans12pt7b;
const lgfx::IFont* const SMALL_FONT = &fonts::FreeSans9pt7b;

constexpr int ROW_Y0   = 32;   // first row baseline-top, below the header
constexpr int ROW_STEP = 28;   // vertical pitch between rows
constexpr int CLOCK_Y  = 120;

constexpr int TRAIN_DEST_X = 48;   // after the (now smaller) departure time
constexpr int BUS_ROUTE_X  = 48;   // after the (now smaller) arrival time
constexpr int BUS_DEST_X   = 100;  // after the route number

// Draw `text` at (x, y), scrolling it horizontally when it is wider than `maxW`.
// Two copies separated by a gap give a seamless loop; the clip rect stops the
// text spilling into whatever sits to its right. Returns nothing — `st` carries
// the per-row offset between frames.
void drawScrolling(RowScroll& st, const String& text, int x, int y, int maxW, int rowH) {
    int w = spr.textWidth(text);
    if (w <= maxW) {
        st.text = text;
        st.offset = 0;
        spr.setCursor(x, y);
        spr.print(text);
        return;
    }
    if (st.text != text) { st.text = text; st.offset = 0; }
    const int gap = 32;
    spr.setClipRect(x, y - 2, maxW, rowH + 6);
    spr.setCursor(x + st.offset, y);
    spr.print(text);
    spr.setCursor(x + st.offset + w + gap, y);
    spr.print(text);
    spr.clearClipRect();
    st.offset -= 1;                              // scroll speed (px/frame)
    if (st.offset <= -(w + gap)) st.offset = 0;  // wrap
}

// Header row: a small dim mode tag ("TRAIN" / "BUS" / "BUS 38") followed by the
// station or stop name in the large font, scrolling if it overflows.
void drawHeader(const String& tag, const String& name) {
    spr.setFont(SMALL_FONT);
    int tagH = spr.fontHeight();
    int tagW = spr.textWidth(tag) + 10;

    spr.setFont(HEAD_FONT);
    int headH = spr.fontHeight();

    spr.setFont(SMALL_FONT);
    spr.setTextColor(DIM, BLACK);
    spr.setCursor(0, (headH - tagH) / 2);   // centred against the taller name
    spr.print(tag);

    spr.setFont(HEAD_FONT);
    spr.setTextColor(AMBER, BLACK);
    drawScrolling(s_headerScroll, name, tagW, 0, W - tagW, headH);
}

// "Due" under a minute out, otherwise whole minutes — the wording TfL's own
// Countdown signs use.
String formatEta(int32_t seconds) {
    if (seconds < 60) return String("Due");
    return String(seconds / 60) + " min";
}

// The "HH:MM" the bus is actually expected, `seconds` from now.
String clockTimeIn(int32_t seconds) {
    time_t t = time(nullptr) + seconds;
    struct tm tm;
    localtime_r(&t, &tm);
    char buf[8];
    strftime(buf, sizeof(buf), "%H:%M", &tm);
    return String(buf);
}

// One arrival row: [expected time] [route] [destination] [countdown right-aligned].
// Time and countdown use the small font; route and destination the row font.
// Shared by the bus and river screens — a boat's "RB1" sits where a bus's "38"
// does — with the caller passing the marquee state for its own screen.
void drawArrivalRow(int y, RowScroll& scroll, const BusArrival& ar) {
    String eta = formatEta(ar.etaSeconds);
    String when = clockTimeIn(ar.etaSeconds);

    spr.setFont(ROW_FONT);
    int rowH = spr.fontHeight();

    spr.setFont(SMALL_FONT);
    int smallH = spr.fontHeight();
    int ew = spr.textWidth(eta);
    int smallDy = (rowH - smallH) / 2;          // sit the small text on the row's centre
    spr.setTextColor(AMBER, BLACK);
    spr.setCursor(0, y + smallDy);
    spr.print(when);
    spr.setCursor(W - ew, y + smallDy);
    spr.print(eta);

    spr.setFont(ROW_FONT);
    spr.setTextColor(AMBER, BLACK);
    spr.setCursor(BUS_ROUTE_X, y);
    spr.print(ar.line);

    const int destMax = W - BUS_DEST_X - ew - 8;
    drawScrolling(scroll, ar.destination, BUS_DEST_X, y, destMax, rowH);
}

// One departure row: [time]  [destination...]  [right-aligned status (+platform)].
// The status uses a smaller font than the row so the destination gets more width.
// If the destination is wider than its column it scrolls (marquee), clipped so
// it never overwrites the status.
void drawRow(int y, int idx, const Departure& dep) {
    uint16_t colour = dep.cancelled ? RED : AMBER;

    String right = dep.status;
    if (dep.platform.length()) right += "  P" + dep.platform;

    // Hide an "On time" status (with no platform) so the destination gets the
    // full row width — delays, cancellations, and platforms are always shown.
    bool onTime = !dep.cancelled && dep.status == "On time";
    bool showStatus = !(HIDE_ONTIME_STATUS && onTime && dep.platform.length() == 0);

    spr.setFont(ROW_FONT);
    int rowH = spr.fontHeight();

    // Time and status share the small font, vertically centred on the row.
    spr.setFont(SMALL_FONT);
    int smallH = spr.fontHeight();
    int smallDy = (rowH - smallH) / 2;
    int rw = showStatus ? spr.textWidth(right) : 0;
    spr.setTextColor(AMBER, BLACK);
    spr.setCursor(0, y + smallDy);
    spr.print(dep.aimed);
    if (showStatus) {
        spr.setTextColor(colour, BLACK);
        spr.setCursor(W - rw, y + smallDy);
        spr.print(right);
    }

    // Destination column runs from TRAIN_DEST_X up to the status (or the right
    // edge); a name too wide for it marquees, clipped so it never reaches the
    // status (DISP-03).
    spr.setFont(ROW_FONT);
    const int destMax = W - TRAIN_DEST_X - (showStatus ? rw + 8 : 4);
    spr.setTextColor(colour, BLACK);
    drawScrolling(s_rowScroll[idx], dep.destination, TRAIN_DEST_X, y, destMax, rowH);
}

// Centred clock (HH:MM:SS), drawn from local time each frame.
void drawClock(int y) {
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);

    char timeStr[16];
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &tm);
    spr.setFont(&DotMatrix_Bold_38);
    spr.setTextColor(AMBER, BLACK);
    int tw = spr.textWidth(timeStr);
    spr.setCursor((W - tw) / 2, y);
    spr.print(timeStr);
}

// --- Weather icons ----------------------------------------------------------
// Drawn from primitives rather than shipped as bitmaps: no flash cost worth
// counting, any size, and they take the theme colour for free.
//
// The one trick a single-colour panel needs is the knockout. Two overlapping
// filled shapes in the same colour fuse into an unreadable blob -- "sun behind
// cloud" comes out looking like a snowman -- so the cloud is drawn once
// oversized in the background colour to cut a clean gap, then again at true
// size in the foreground.

void drawSun(int cx, int cy, int r) {
    spr.fillCircle(cx, cy, r, AMBER);
    for (int i = 0; i < 8; ++i) {
        float a = i * (float)M_PI / 4.0f;
        int x0 = cx + (int)(cosf(a) * r * 1.45f), y0 = cy + (int)(sinf(a) * r * 1.45f);
        int x1 = cx + (int)(cosf(a) * r * 2.05f), y1 = cy + (int)(sinf(a) * r * 2.05f);
        wideLine(x0, y0, x1, y1, r * 0.30f, AMBER);
    }
}

void drawCloud(int cx, int cy, int w, uint16_t colour) {
    int r = w * 30 / 100;
    spr.fillCircle(cx - w * 38 / 100, cy + r * 3 / 10, r * 8 / 10, colour);
    spr.fillCircle(cx - w * 2 / 100,  cy - r / 20,     r,          colour);
    spr.fillCircle(cx + w * 30 / 100, cy + r * 4 / 10, r * 75 / 100, colour);
    spr.fillRoundRect(cx - w * 48 / 100, cy + r * 15 / 100,
                      w * 96 / 100, r * 80 / 100, r * 4 / 10, colour);
}

void drawDrops(int cx, int cy, int w, bool slanted) {
    for (int i = -1; i <= 1; ++i) {
        int x = cx + i * w * 26 / 100;
        int dx = slanted ? w * 10 / 100 : 0;
        wideLine(x + dx, cy, x - dx, cy + w * 30 / 100, w * 0.055f, AMBER);
    }
}

// `w` is the icon's nominal width; it is drawn centred on (cx, cy).
void drawWeatherIcon(int code, int cx, int cy, int w) {
    switch (code) {
        case 0:                                        // clear
            drawSun(cx, cy, w * 24 / 100);
            break;
        case 1: case 2:                                // sun behind cloud
            drawSun(cx + w * 22 / 100, cy - w * 24 / 100, w * 16 / 100);
            drawCloud(cx - w * 6 / 100, cy + w * 14 / 100, w * 92 / 100, BLACK);
            drawCloud(cx - w * 6 / 100, cy + w * 14 / 100, w * 80 / 100, AMBER);
            break;
        case 45: case 48:                              // fog
            drawCloud(cx, cy - w * 20 / 100, w * 85 / 100, AMBER);
            // Clear of the cloud base, or the bars fuse with it into a barcode.
            for (int i = 0; i < 3; ++i) {
                int y = cy + w * 22 / 100 + i * w * 15 / 100;
                int th = w * 5 / 100 < 2 ? 2 : w * 5 / 100;
                spr.fillRect(cx - w * 36 / 100, y, w * 72 / 100, th, AMBER);
            }
            break;
        case 51: case 53: case 55: case 56: case 57:   // drizzle: straight
            drawCloud(cx, cy - w * 14 / 100, w * 85 / 100, AMBER);
            drawDrops(cx, cy + w * 22 / 100, w, false);
            break;
        case 61: case 63: case 65: case 66: case 67:
        case 80: case 81: case 82:                     // rain: slanted, so the
            drawCloud(cx, cy - w * 14 / 100, w * 85 / 100, AMBER);   // two are
            drawDrops(cx, cy + w * 20 / 100, w, true);               // telling apart
            break;
        case 71: case 73: case 75: case 77: case 85: case 86:   // snow
            drawCloud(cx, cy - w * 14 / 100, w * 85 / 100, AMBER);
            for (int i = -1; i <= 1; ++i) {
                spr.fillCircle(cx + i * w * 26 / 100, cy + w * 28 / 100,
                               w * 55 / 1000, AMBER);
            }
            break;
        case 95: case 96: case 99: {                   // thunderstorm
            drawCloud(cx, cy - w * 16 / 100, w * 85 / 100, AMBER);
            int b = w * 20 / 100;
            spr.fillTriangle(cx + b * 15 / 100, cy + w * 10 / 100,
                             cx - b * 55 / 100, cy + w * 46 / 100,
                             cx + b * 10 / 100, cy + w * 36 / 100, AMBER);
            spr.fillTriangle(cx - b * 5 / 100,  cy + w * 44 / 100,
                             cx - b * 35 / 100, cy + w * 80 / 100,
                             cx + b * 65 / 100, cy + w * 34 / 100, AMBER);
            break;
        }
        default:                                       // overcast, and anything
            drawCloud(cx, cy, w * 95 / 100, AMBER);    // we do not recognise
            break;
    }
}

// Stale-data indicator: the feed is failing but the last good data is still on
// screen. Every board draws it the same way, in the same corner.
void drawStaleIndicator(int errCount) {
    if (errCount <= 0) return;
    char msg[24];
    snprintf(msg, sizeof(msg), "No signal (%dx)", errCount);
    spr.setFont(&fonts::Font0);
    spr.setTextColor(RED, BLACK);
    spr.setCursor(2, H - 10);
    spr.print(msg);
}

// The whole of a bus or river board: header, up to three rows counted down from
// the moment of the fetch, clock, and the stale-data marker. The two screens are
// the same instrument pointed at a different feed, so they are the same code.
void drawArrivalsBoard(const String& tag, const String& name,
                       const std::vector<BusArrival>& arrivals, RowScroll* scroll,
                       size_t maxRows, const char* emptyMsg,
                       uint32_t sinceFetchMs, int errCount) {
    spr.fillScreen(BLACK);
    drawHeader(tag, name);

    if (arrivals.empty()) {
        spr.setFont(HEAD_FONT);
        spr.setTextColor(DIM, BLACK);
        spr.setCursor((W - spr.textWidth(emptyMsg)) / 2, 52);
        spr.print(emptyMsg);
    } else {
        // Count the ETAs down from the moment of the fetch, so the numbers keep
        // moving between polls instead of freezing until the next one.
        int32_t elapsed = (int32_t)(sinceFetchMs / 1000);
        for (size_t i = 0; i < arrivals.size() && i < maxRows; ++i) {
            BusArrival ar = arrivals[i];
            ar.etaSeconds = ar.etaSeconds > elapsed ? ar.etaSeconds - elapsed : 0;
            drawArrivalRow(ROW_Y0 + (int)i * ROW_STEP, scroll[i], ar);
        }
    }

    drawClock(CLOCK_Y);
    drawStaleIndicator(errCount);
    spr.pushSprite(0, 0);
}

}  // namespace

namespace ui {

void begin(uint8_t brightness) {
    s_brightness = brightness;
    // Some boards gate the panel's power rail behind a pin that must be driven
    // HIGH before init(), or the display stays dark with no other symptom.
    if (board::PIN_PANEL_POWER >= 0) {
        pinMode(board::PIN_PANEL_POWER, OUTPUT);
        digitalWrite(board::PIN_PANEL_POWER, HIGH);
    }

    lcd.init();
    lcd.setRotation(1);          // landscape
    lcd.setBrightness(brightness);

    // Without PSRAM this is a real DRAM allocation competing with WiFi and TLS,
    // so report what it cost rather than assuming it was free. A board that is
    // going to run out of heap does it during a TLS handshake, hours later and
    // far from here, which is a miserable thing to debug backwards.
    const uint32_t heapBefore = ESP.getFreeHeap();
    const uint32_t bufBytes = (uint32_t)W * H * board::COLOR_DEPTH / 8;
    spr.setPsram(board::HAS_PSRAM);
    spr.setColorDepth(board::COLOR_DEPTH);
    if (!spr.createSprite(W, H)) {
        // Worth reporting the largest *contiguous* block, not just the total:
        // this allocation failed once with 285 KB free, because the heap had
        // the total and no single block big enough.
        Serial.printf("[display] FAILED to allocate a %dx%d %d-bit back buffer "
                      "(%u bytes); free heap %u, largest block %u\n",
                      W, H, board::COLOR_DEPTH, (unsigned)bufBytes,
                      (unsigned)heapBefore,
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }
    applyPalette();
    spr.setTextWrap(false);
    Serial.printf("[display] %s %dx%d, %d-bit buffer %u bytes in %s, "
                  "free heap %u -> %u (largest block %u)\n",
                  board::NAME, W, H, board::COLOR_DEPTH, (unsigned)bufBytes,
                  board::HAS_PSRAM ? "PSRAM" : "DRAM",
                  (unsigned)heapBefore, (unsigned)ESP.getFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

bool getTouch(int& x, int& y) {
    if (!board::HAS_TOUCH) return false;
    int32_t tx = 0, ty = 0;
    if (!lcd.getTouch(&tx, &ty)) return false;
    x = tx;
    y = ty;
    return true;
}

void setBrightness(uint8_t brightness) {
    s_brightness = brightness;
    lcd.setBrightness(brightness);
}

void setTheme(int fg, int dim, int warn, int bg) {
    // Each colour is applied only when the caller actually has one; a negative
    // or out-of-range value means "not provisioned", leaving the classic amber
    // default in place. Callers pass Config's raw ints straight through, so the
    // range check lives here rather than being repeated at every call site.
    auto apply = [](uint16_t& target, int value) {
        if (value >= 0 && value <= 0xFFFF) target = (uint16_t)value;
    };
    // Written into s_rgb rather than into BLACK/AMBER/... because on a
    // paletted board those hold indices, and an index is not a colour.
    apply(s_rgb[1], fg);
    apply(s_rgb[3], dim);
    apply(s_rgb[2], warn);
    apply(s_rgb[0], bg);
    applyPalette();
}

void renderSetup() {
    spr.fillScreen(BLACK);
    spr.setTextColor(AMBER, BLACK);
    spr.setFont(&fonts::FreeSansBold12pt7b);
    const char* title = "Awaiting setup";
    spr.setCursor((W - spr.textWidth(title)) / 2, 40);
    spr.print(title);

    spr.setFont(&fonts::FreeSans9pt7b);
    spr.setTextColor(DIM, BLACK);
    const char* l1 = "Connect USB and run";
    const char* l2 = "the Departure Buddy installer";
    spr.setCursor((W - spr.textWidth(l1)) / 2, 82);
    spr.print(l1);
    spr.setCursor((W - spr.textWidth(l2)) / 2, 104);
    spr.print(l2);
    spr.pushSprite(0, 0);
}

void showStartup(const char* line1, const char* line2) {
    spr.fillScreen(BLACK);
    spr.setTextColor(AMBER, BLACK);
    spr.setFont(&fonts::FreeSansBold12pt7b);
    int w1 = spr.textWidth(line1);
    spr.setCursor((W - w1) / 2, 55);
    spr.print(line1);
    spr.setFont(&fonts::FreeSans9pt7b);
    spr.setTextColor(DIM, BLACK);
    int w2 = spr.textWidth(line2);
    spr.setCursor((W - w2) / 2, 95);
    spr.print(line2);
    spr.pushSprite(0, 0);
}

void renderBoard(const std::vector<Departure>& deps, const String& station,
                 const String& callingAt, int errCount) {
    spr.fillScreen(BLACK);

    drawHeader("TRAIN", station);

    if (deps.empty()) {
        // No data yet / no services — never blank, never a crash (ARCH-04).
        spr.setFont(HEAD_FONT);
        spr.setTextColor(DIM, BLACK);
        const char* msg = "No departures";
        spr.setCursor((W - spr.textWidth(msg)) / 2, 52);
        spr.print(msg);
    } else {
        // Top three departures, all in the same row font so the board reads as
        // one list rather than a headline plus also-rans.
        for (size_t i = 0; i < deps.size() && i < MAX_DEPARTURES; ++i) {
            drawRow(ROW_Y0 + (int)i * ROW_STEP, (int)i, deps[i]);
        }
    }
    drawClock(CLOCK_Y);
    (void)callingAt;  // calling-at line removed from the layout

    drawStaleIndicator(errCount);
    spr.pushSprite(0, 0);
}

void renderBusBoard(const std::vector<BusArrival>& arrivals, const String& stopName,
                    const String& lineFilter, uint32_t sinceFetchMs, int errCount) {
    drawArrivalsBoard(lineFilter.length() ? ("BUS " + lineFilter) : String("BUS"),
                      stopName, arrivals, s_busScroll, MAX_BUS_ARRIVALS,
                      "No buses due", sinceFetchMs, errCount);
}

void renderRiverBoard(const std::vector<RiverArrival>& arrivals, const String& pierName,
                      const String& lineFilter, uint32_t sinceFetchMs, int errCount) {
    // "RIVER" rather than "BOAT": it is what TfL calls the mode, and it is what
    // is printed on the piers the board is quoting.
    drawArrivalsBoard(lineFilter.length() ? ("RIVER " + lineFilter) : String("RIVER"),
                      pierName, arrivals, s_riverScroll, MAX_RIVER_ARRIVALS,
                      "No boats due", sinceFetchMs, errCount);
}

void resetScroll() {
    for (auto& r : s_rowScroll) { r.text = ""; r.offset = 0; }
    for (auto& r : s_busScroll) { r.text = ""; r.offset = 0; }
    for (auto& r : s_riverScroll) { r.text = ""; r.offset = 0; }
    s_wxScroll.text = "";
    s_wxScroll.offset = 0;
    s_headerScroll.text = "";
    s_headerScroll.offset = 0;
}

void renderConnectivityWarning(const String& station, int errCount) {
    spr.fillScreen(BLACK);
    spr.setTextColor(AMBER, BLACK);
    spr.setFont(&fonts::FreeSansBold12pt7b);
    int sw = spr.textWidth(station);
    spr.setCursor((W - sw) / 2, 24);
    spr.print(station);

    char msg[32];
    snprintf(msg, sizeof(msg), "No network (%d attempts)", errCount);
    spr.setFont(&fonts::FreeSans9pt7b);
    spr.setTextColor(RED, BLACK);
    int mw = spr.textWidth(msg);
    spr.setCursor((W - mw) / 2, 60);
    spr.print(msg);

    drawClock(105);
    spr.pushSprite(0, 0);
}

void renderError(const String& title, const String& detail) {
    spr.fillScreen(BLACK);
    spr.setFont(&fonts::FreeSansBold12pt7b);
    spr.setTextColor(RED, BLACK);
    spr.setCursor((W - spr.textWidth(title)) / 2, 38);
    spr.print(title);

    spr.setFont(&fonts::FreeSans12pt7b);
    spr.setTextColor(AMBER, BLACK);
    spr.setCursor((W - spr.textWidth(detail)) / 2, 76);
    spr.print(detail);

    spr.setFont(&fonts::FreeSans9pt7b);
    spr.setTextColor(DIM, BLACK);
    const char* hint = "Re-run the installer to fix";
    spr.setCursor((W - spr.textWidth(hint)) / 2, 116);
    spr.print(hint);
    spr.pushSprite(0, 0);
}

void renderClock(bool night, int driftX, int driftY) {
    spr.fillScreen(BLACK);

    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[8];
    strftime(buf, sizeof(buf), "%H:%M", &tm);

    spr.setFont(&Clock_Bold_104);
    spr.setTextColor(AMBER, BLACK);
    int tw = spr.textWidth(buf);
    int th = spr.fontHeight();
    spr.setCursor((W - tw) / 2 + driftX, (H - th) / 2 + driftY);
    spr.print(buf);

    // The flip-clock split. Drawn in the background colour rather than a border
    // colour so it reads as a seam in the digits, which is what a real one is.
    spr.fillRect(0, H / 2 + driftY - 1, W, 3, BLACK);

    spr.pushSprite(0, 0);
    lcd.setBrightness(night ? NIGHT_BRIGHTNESS : s_brightness);
}

void renderWeatherBoard(const Weather& wx, const String& place, int errCount) {
    spr.fillScreen(BLACK);
    drawHeader("WEATHER", place);

    // The body has to live between the header and the clock at CLOCK_Y, so the
    // temperature is sized to leave room for two detail rows underneath.
    spr.setFont(&fonts::FreeSansBold24pt7b);
    spr.setTextColor(AMBER, BLACK);
    String temp = wx.temp.length() ? wx.temp + "\xB0" : "--";
    spr.setCursor(0, 30);
    spr.print(temp);
    int tw = spr.textWidth(temp);

    if (wx.condition.length()) {
        spr.setFont(ROW_FONT);
        drawScrolling(s_wxScroll, wx.condition, tw + 10, 40,
                      W - tw - 14, spr.fontHeight());
    }

    // Full brightness and the bold face: DIM is for the mode tag, and using
    // it here made the actual readings the hardest thing on the board to read.
    spr.setFont(&fonts::FreeSansBold9pt7b);
    spr.setTextColor(AMBER, BLACK);
    if (wx.feels.length() || wx.wind.length()) {
        String row = "";
        if (wx.feels.length()) row += "Feels " + wx.feels + "\xB0";
        if (wx.wind.length()) row += (row.length() ? "   " : "") + String("Wind ") + wx.wind + " mph";
        spr.setCursor(0, 72);
        spr.print(row);
    }
    if (wx.high.length() && wx.low.length()) {
        spr.setCursor(0, 92);
        spr.print("High " + wx.high + "\xB0   Low " + wx.low + "\xB0");
    }


    // The icon fills the space the text leaves on the right.
    if (wx.code >= 0) drawWeatherIcon(wx.code, 262, 68, 64);

    drawClock(CLOCK_Y);
    drawStaleIndicator(errCount);
    spr.pushSprite(0, 0);
}

void renderBlank() {
    spr.fillScreen(BLACK);
    spr.pushSprite(0, 0);
}

}  // namespace ui
