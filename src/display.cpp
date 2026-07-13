// Display + rendering for the LilyGo T-Display-S3 (ST7789, 170x320, 8-bit i80).
//
// Uses LovyanGFX with a full-frame sprite in PSRAM as a back buffer so the
// scrolling calling-points line and ticking clock render without flicker.
//
// Layout is landscape 320x170 (rotation 1) — a wide "platform sign" shape,
// reimagining the Python app's 256x64 board with room for a big clock.

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

// One departure row: [time]  [destination...]  [right-aligned status (+platform)].
// The status uses a smaller font than the row so the destination gets more width.
// If the destination is wider than its column it scrolls (marquee), clipped so
// it never overwrites the status.
void drawRow(int y, int idx, const Departure& dep, const lgfx::IFont* rowFont,
             const lgfx::IFont* statusFont) {
    uint16_t colour = dep.cancelled ? RED : AMBER;

    String right = dep.status;
    if (dep.platform.length()) right += "  P" + dep.platform;

    // Hide an "On time" status (with no platform) so the destination gets the
    // full row width — delays, cancellations, and platforms are always shown.
    bool onTime = !dep.cancelled && dep.status == "On time";
    bool showStatus = !(HIDE_ONTIME_STATUS && onTime && dep.platform.length() == 0);

    int rw = 0, statusH = 0;
    if (showStatus) {
        spr.setFont(statusFont);
        rw = spr.textWidth(right);
        statusH = spr.fontHeight();
    }

    // Time (never scrolls).
    spr.setFont(rowFont);
    int rowH = spr.fontHeight();
    spr.setTextColor(AMBER, BLACK);
    spr.setCursor(0, y);
    spr.print(dep.aimed);

    // Destination column runs from destX up to the status (or the right edge).
    const int destX = 70;
    const int destMax = W - destX - (showStatus ? rw + 8 : 4);
    int destW = spr.textWidth(dep.destination);
    spr.setTextColor(colour, BLACK);

    if (destW <= destMax) {
        // Fits — draw statically and clear any scroll for this row.
        s_rowScroll[idx].text = dep.destination;
        s_rowScroll[idx].offset = 0;
        spr.setCursor(destX, y);
        spr.print(dep.destination);
    } else {
        // Too long — marquee within the column, clipped so it never reaches the
        // status. Two copies with a gap give a seamless loop (DISP-03 style).
        RowScroll& st = s_rowScroll[idx];
        if (st.text != dep.destination) { st.text = dep.destination; st.offset = 0; }
        const int gap = 32;
        spr.setClipRect(destX, y - 2, destMax, rowH + 6);
        spr.setCursor(destX + st.offset, y);
        spr.print(dep.destination);
        spr.setCursor(destX + st.offset + destW + gap, y);
        spr.print(dep.destination);
        spr.clearClipRect();
        st.offset -= 1;                                    // scroll speed (px/frame)
        if (st.offset <= -(destW + gap)) st.offset = 0;    // wrap
    }

    // Status right-aligned, vertically centred against the taller row font.
    if (showStatus) {
        spr.setFont(statusFont);
        spr.setTextColor(colour, BLACK);
        spr.setCursor(W - rw, y + (rowH - statusH) / 2);
        spr.print(right);
    }
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

    if (deps.empty()) {
        // No data yet / no services — welcome screen with clock (mirrors ARCH-04).
        spr.setFont(&fonts::FreeSansBold12pt7b);
        spr.setTextColor(AMBER, BLACK);
        String welcome = "Welcome to";
        String st = station;
        int ww = spr.textWidth(welcome);
        spr.setCursor((W - ww) / 2, 20);
        spr.print(welcome);
        int sw = spr.textWidth(st);
        spr.setCursor((W - sw) / 2, 48);
        spr.print(st);
        drawClock(105);
    } else {
        // Top three departures, larger type and roomier spacing (no calling-at
        // line, no date). Row 1 bold; status in the smaller 9pt font.
        drawRow(6, 0, deps[0], &fonts::FreeSansBold12pt7b, &fonts::FreeSans9pt7b);
        if (deps.size() > 1) drawRow(42, 1, deps[1], &fonts::FreeSans12pt7b, &fonts::FreeSans9pt7b);
        if (deps.size() > 2) drawRow(78, 2, deps[2], &fonts::FreeSans12pt7b, &fonts::FreeSans9pt7b);
        drawClock(120);
    }
    (void)callingAt;  // calling-at line removed from the layout

    // Stale-data indicator: API is failing but we're still showing last-good data.
    if (errCount > 0) {
        char msg[24];
        snprintf(msg, sizeof(msg), "No signal (%dx)", errCount);
        spr.setFont(&fonts::Font0);
        spr.setTextColor(RED, BLACK);
        spr.setCursor(2, H - 10);
        spr.print(msg);
    }

    spr.pushSprite(0, 0);
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
