// Display + rendering for the LilyGo T-Display-S3 (ST7789, 170x320, 8-bit i80).
//
// Uses LovyanGFX with a full-frame sprite in PSRAM as a back buffer so the
// scrolling destinations and ticking clock render without flicker.
//
// Layout is landscape 320x170 (rotation 1) — a wide "platform sign" shape,
// reimagining the Python app's 256x64 board with room for a big clock.
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
#include "app_config.h"
#include <time.h>

// -----------------------------------------------------------------------------
// Panel definition — the known-good pin map for the T-Display-S3.
// (8-bit parallel bus; the 170-wide panel sits at x-offset 35 on the ST7789.)
// -----------------------------------------------------------------------------
class LGFX_TDisplayS3 : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789   _panel;
    lgfx::Bus_Parallel8  _bus;
    lgfx::Light_PWM      _light;

public:
    LGFX_TDisplayS3() {
        {
            auto cfg = _bus.config();
            cfg.freq_write = 20000000;
            cfg.pin_wr = 8;
            cfg.pin_rd = 9;
            cfg.pin_rs = 7;   // D/C
            cfg.pin_d0 = 39; cfg.pin_d1 = 40; cfg.pin_d2 = 41; cfg.pin_d3 = 42;
            cfg.pin_d4 = 45; cfg.pin_d5 = 46; cfg.pin_d6 = 47; cfg.pin_d7 = 48;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs = 6;
            cfg.pin_rst = 5;
            cfg.pin_busy = -1;
            cfg.panel_width = 170;
            cfg.panel_height = 320;
            cfg.offset_x = 35;
            cfg.offset_y = 0;
            cfg.readable = false;
            cfg.invert = true;
            cfg.rgb_order = false;
            cfg.dlen_16bit = false;
            cfg.bus_shared = false;
            _panel.config(cfg);
        }
        {
            auto cfg = _light.config();
            cfg.pin_bl = 38;
            cfg.freq = 44100;
            cfg.pwm_channel = 7;
            _light.config(cfg);
            _panel.setLight(&_light);
        }
        setPanel(&_panel);
    }
};

namespace {

constexpr int W = 320;
constexpr int H = 170;

// Colours (RGB565).
constexpr uint16_t BLACK = 0x0000;
constexpr uint16_t AMBER = 0xFD20;  // classic departure-board amber
constexpr uint16_t RED   = 0xF800;  // cancellations
constexpr uint16_t DIM   = 0x8300;  // dimmed amber for secondary text

LGFX_TDisplayS3 lcd;
lgfx::LGFX_Sprite spr(&lcd);

// Per-row horizontal scroll state for destination names that overflow their
// column. Reset (offset 0) when the row's text changes or when it fits.
struct RowScroll {
    String text;
    int offset = 0;
};
RowScroll s_rowScroll[MAX_DEPARTURES];
RowScroll s_busScroll[MAX_BUS_ARRIVALS];
RowScroll s_riverScroll[MAX_RIVER_ARRIVALS];
RowScroll s_headerScroll;

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
    // The T-Display-S3 gates the panel power on GPIO15 — must be HIGH.
    pinMode(15, OUTPUT);
    digitalWrite(15, HIGH);

    lcd.init();
    lcd.setRotation(1);          // landscape 320x170
    lcd.setBrightness(brightness);

    spr.setPsram(true);          // back buffer in PSRAM (~106 KB)
    spr.setColorDepth(16);
    spr.createSprite(W, H);
    spr.setTextWrap(false);
}

void setBrightness(uint8_t brightness) {
    lcd.setBrightness(brightness);
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
    const char* l2 = "the Esp32Departures installer";
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

void renderBlank() {
    spr.fillScreen(BLACK);
    spr.pushSprite(0, 0);
}

}  // namespace ui
