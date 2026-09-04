#pragma once
// ESP32-2432S028R, the "Cheap Yellow Display" — classic ESP32-D0WD-V3,
// 2.8" 320x240 on SPI, with a resistive XPT2046 touchscreen.
//
// Verified on the board: ESP32-D0WD-V3 rev 3.1, dual core 240 MHz, 4 MB flash,
// and **no PSRAM** — which is the fact that shapes everything below. The
// full-frame back buffer the S3 keeps in PSRAM would be 153,600 bytes of DRAM
// here, competing with WiFi and an mbedTLS handshake -- and measurably will not
// allocate. See COLOR_DEPTH below for what this board does instead.
//
// Two panel controllers ship under the same product name depending on revision.
// This unit is an ST7789, confirmed by putting each candidate on screen with
// its own name on it (diag/panel_try.cpp) -- the identification registers were
// no help, because MISO here is IO12, an ESP32 strapping pin that these clones
// often leave unwired to the LCD. So ST7789 is the default, and the older
// revision is selected with -DCYD_PANEL_ILI9341.
//
// The symptom of getting this wrong is a lit screen full of garbage, not a
// dark one, which is easy to mistake for a wiring fault.
//
// Pin map from the project's PINS.md. Note the display and the touch controller
// are on *different* SPI buses, which is why touch needs its own bus config
// rather than sharing the panel's.

#include <LovyanGFX.hpp>

namespace board {

constexpr int SCREEN_W = 320;
constexpr int SCREEN_H = 240;

// 70 more pixels of height than the S3, which buys a fourth row and a little
// more air between them.
constexpr int LIST_ROWS = BOARD_LIST_ROWS;
constexpr int ROW_Y0   = 34;
constexpr int ROW_STEP = 30;
constexpr int CLOCK_Y  = 176;

constexpr bool HAS_PSRAM = false;

// Bits per pixel in the back buffer. Measured on the board: a 16bpp full frame
// is 153,600 bytes, and although 285 KB of heap was free the allocation still
// failed -- the heap had the total but no contiguous block that size.
//
// The whole board is a four-colour design (background, primary, dimmed, alert),
// so a 4-bit palette is not a compromise: sixteen entries hold four colours
// exactly, and nothing here is anti-aliased. 320x240 at 4bpp is 38,400 bytes,
// a quarter of the frame, which fits comfortably and needs no banding.
constexpr int COLOR_DEPTH = 4;

// No usable buttons: BOOT (GPIO0) is on the PCB edge rather than the bezel, and
// it is the strapping pin. Touch does the job instead — left half holds the
// clock, right half steps to the next panel.
constexpr bool HAS_BUTTONS = false;
constexpr bool HAS_TOUCH   = true;
// Named so the shared input code compiles on both boards; never read, because
// HAS_BUTTONS gates every use.
constexpr int PIN_BTN_CLOCK = -1;
constexpr int PIN_BTN_NEXT  = -1;

// Nothing gates the panel's power rail on this board.
constexpr int PIN_PANEL_POWER = -1;

constexpr const char* NAME = "ESP32 Cheap Yellow Display";

class Display : public lgfx::LGFX_Device {
#ifdef CYD_PANEL_ILI9341
    lgfx::Panel_ILI9341  _panel;
#else
    lgfx::Panel_ST7789   _panel;
#endif
    lgfx::Bus_SPI        _bus;
    lgfx::Light_PWM      _light;
    lgfx::Touch_XPT2046  _touch;

public:
    Display() {
        {
            auto cfg = _bus.config();
            cfg.spi_host = HSPI_HOST;
            cfg.spi_mode = 0;
            // The ILI9341 tops out around 40 MHz; the ST7789 will take 80, but
            // there is nothing to gain here — the whole frame is text.
            cfg.freq_write = 40000000;
            cfg.freq_read = 16000000;
            cfg.spi_3wire = false;
            cfg.use_lock = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = 14;
            cfg.pin_mosi = 13;
            cfg.pin_miso = 12;
            cfg.pin_dc = 2;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs = 15;
            cfg.pin_rst = -1;    // tied to the module's own reset
            cfg.pin_busy = -1;
            cfg.panel_width = 240;
            cfg.panel_height = 320;
            cfg.offset_x = 0;
            cfg.offset_y = 0;
            cfg.offset_rotation = 0;
            // MISO goes to IO12 and is not reliably wired to the LCD on these
            // clones, so never read back from the panel: the values that come
            // back look plausible and are not.
            cfg.readable = false;
            // Confirmed by eye on the real board rather than assumed. An ST7789
            // in a CYD is commonly inverted; this one is not.
            cfg.invert = false;
            // These panels are wired BGR, so leaving this true swaps red and
            // blue — which on an amber board looks like a blue-grey mess.
            cfg.rgb_order = false;
            cfg.dlen_16bit = false;
            // Touch is on its own bus, so the panel does not have to give the
            // bus up between transactions.
            cfg.bus_shared = false;
            _panel.config(cfg);
        }
        {
            auto cfg = _light.config();
            cfg.pin_bl = 21;
            cfg.freq = 44100;
            cfg.pwm_channel = 7;
            _light.config(cfg);
            _panel.setLight(&_light);
        }
        {
            // XPT2046 on VSPI, entirely separate from the display's HSPI.
            // Raw ranges are the calibration; they are refined against the real
            // panel rather than taken from the datasheet.
            auto cfg = _touch.config();
            cfg.x_min = 300;
            cfg.x_max = 3900;
            cfg.y_min = 200;
            cfg.y_max = 3700;
            cfg.pin_int = 36;
            cfg.bus_shared = false;
            cfg.offset_rotation = 0;
            cfg.spi_host = VSPI_HOST;
            cfg.freq = 1000000;
            cfg.pin_sclk = 25;
            cfg.pin_mosi = 32;
            cfg.pin_miso = 39;
            cfg.pin_cs = 33;
            _touch.config(cfg);
            _panel.setTouch(&_touch);
        }
        setPanel(&_panel);
    }
};

}  // namespace board
