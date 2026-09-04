// Is the CYD's touch controller responding, and on what numbers?
//
// Touch did nothing in the firmware, which has several possible causes that
// look identical from outside: the XPT2046 not being read at all, being read
// but returning nothing, or returning values the calibration then throws away.
//
// This tries two configurations in turn and prints everything:
//
//   A. pin_int = 36  — LovyanGFX only reads the controller when the IRQ line
//      says it is being touched. Cheap, but silently dead if that line is not
//      wired the way the pin map claims.
//   B. pin_int = -1  — poll the controller over SPI regardless. Slower and
//      always works if the SPI wiring is right.
//
// Raw values are printed alongside mapped ones, because a controller that
// responds with sensible raw numbers but nonsense mapped ones is a calibration
// problem, not a wiring one — a completely different fix.
//
//     pio run -e cyd-touch -t upload --upload-port COM10
//
// A diagnostic, not firmware.

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <Arduino.h>

namespace {

class TouchPanel : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789   _panel;
    lgfx::Bus_SPI        _bus;
    lgfx::Light_PWM      _light;
    lgfx::Touch_XPT2046  _touch;

public:
    explicit TouchPanel(int pinInt) {
        {
            auto cfg = _bus.config();
            cfg.spi_host = HSPI_HOST;
            cfg.spi_mode = 0;
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
            cfg.pin_rst = -1;
            cfg.pin_busy = -1;
            cfg.panel_width = 240;
            cfg.panel_height = 320;
            cfg.readable = false;
            cfg.invert = false;
            cfg.rgb_order = false;
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
            auto cfg = _touch.config();
            // Deliberately the full ADC span: this is a measurement, so nothing
            // is filtered out before we have seen it.
            cfg.x_min = 0;
            cfg.x_max = 4095;
            cfg.y_min = 0;
            cfg.y_max = 4095;
            cfg.pin_int = pinInt;
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

void banner(lgfx::LGFX_Device& lcd, const char* title) {
    lcd.fillScreen(lcd.color565(0, 0, 0));
    lcd.setTextColor(lcd.color565(255, 166, 0));
    lcd.setFont(&fonts::FreeSansBold12pt7b);
    lcd.setCursor(10, 20);
    lcd.print(title);
    lcd.setFont(&fonts::FreeSans9pt7b);
    lcd.setCursor(10, 60);
    lcd.print("Touch anywhere. 15 seconds.");
    lcd.setCursor(10, 85);
    lcd.print("Watch the serial log.");
}

// Run one configuration for 15 seconds, reporting anything the panel says.
void trial(int pinInt, const char* label) {
    Serial.printf("\n=== %s ===\n", label);
    TouchPanel lcd(pinInt);
    lcd.init();
    lcd.setRotation(1);
    lcd.setBrightness(200);
    banner(lcd, label);

    uint32_t hits = 0;
    const uint32_t end = millis() + 15000;
    while (millis() < end) {
        int32_t rx = 0, ry = 0;
        const bool raw = lcd.getTouchRaw(&rx, &ry);
        int32_t x = 0, y = 0;
        const bool mapped = lcd.getTouch(&x, &y);
        if (raw || mapped) {
            ++hits;
            Serial.printf("  raw=%s (%4d,%4d)   mapped=%s (%4d,%4d)\n",
                          raw ? "yes" : " no ", (int)rx, (int)ry,
                          mapped ? "yes" : " no ", (int)x, (int)y);
            // Mark the spot, so it is obvious whether the axes are sane.
            if (mapped) lcd.fillCircle(x, y, 4, lcd.color565(255, 166, 0));
            delay(120);
        }
        delay(10);
    }
    Serial.printf("  -> %u reading(s) in 15s\n", (unsigned)hits);
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(600);
    Serial.println("\n[touch] XPT2046 check — touch the screen during each trial");
    Serial.println("        IO36 is input-only, so if trial A is silent and B is");
    Serial.println("        not, the IRQ line is the problem rather than the SPI.");
}

void loop() {
    trial(36, "A: pin_int = 36 (IRQ gated)");
    trial(-1, "B: pin_int = -1 (SPI polled)");
}
