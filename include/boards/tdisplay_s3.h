#pragma once
// LilyGo T-Display-S3 — ESP32-S3, 1.9" 170x320 ST7789 on an 8-bit parallel bus.
//
// The original and, until the CYD arrived, the only supported board. Everything
// here was previously inline in display.cpp; it is unchanged, only moved, so
// this board's behaviour is identical to before the second board existed.

#include <LovyanGFX.hpp>

namespace board {

// Landscape, after setRotation(1). The panel itself is 170 wide by 320 tall.
constexpr int SCREEN_W = 320;
constexpr int SCREEN_H = 170;

// Rows that fit the list screens at the current type sizes. Kept in step with
// BOARD_LIST_ROWS, which platformio.ini passes to the API clients.
constexpr int LIST_ROWS = BOARD_LIST_ROWS;

// Where the list rows and the clock sit. The three boards -- trains, buses,
// boats -- deliberately share this geometry so they read as one instrument.
constexpr int ROW_Y0   = 32;    // top of the first row, below the header
constexpr int ROW_STEP = 28;    // vertical pitch between rows
constexpr int CLOCK_Y  = 120;   // top of the clock strip

// The S3 has PSRAM, so the full-frame back buffer costs no DRAM at all and
// there is nothing to gain from a palette. Direct colour, as it always was.
constexpr bool HAS_PSRAM = true;
constexpr int  COLOR_DEPTH = 16;

// Two real buttons, named for what they do rather than their silkscreen: GPIO0
// is physically the *lower* of the two, which is the opposite of the numbering.
constexpr bool HAS_BUTTONS = true;
constexpr bool HAS_TOUCH   = false;
constexpr int PIN_BTN_CLOCK = 0;    // BUTTON_1 — hold the clock on screen
constexpr int PIN_BTN_NEXT  = 14;   // BUTTON_2 — step to the next panel

// The panel's power rail is gated behind this pin and must be driven HIGH
// before init(), or the display stays dark with no other symptom.
constexpr int PIN_PANEL_POWER = 15;

constexpr const char* NAME = "LilyGo T-Display-S3";

// Pin map is the known-good one for this board. The 170-wide panel sits at
// x-offset 35 on the ST7789's 240-wide controller memory.
class Display : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789   _panel;
    lgfx::Bus_Parallel8  _bus;
    lgfx::Light_PWM      _light;

public:
    Display() {
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

}  // namespace board
