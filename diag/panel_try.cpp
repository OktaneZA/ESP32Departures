// Which display controller is on this Cheap Yellow Display? Look and see.
//
// Reading the identification registers did not settle it: 0xD3 came back all
// zeroes and 0x04 as C0 D9 FF, matching neither part. That is not surprising —
// MISO here is IO12, an ESP32 strapping pin, and plenty of these clones do not
// wire the LCD's read line at all. So the register values are not evidence.
//
// This cycles between the two candidate drivers, writing the driver's own name
// on screen in large text. Exactly one of them will be legible. Whichever it is
// goes into boards/cyd.h.
//
//     pio run -e cyd-paneltry -t upload --upload-port COM10
//
// A diagnostic, not firmware: no env builds it by accident.

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <Arduino.h>

namespace {

// The pin map is identical for both candidates; only the controller differs,
// which is the whole point of the test.
template <typename PanelT>
class Candidate : public lgfx::LGFX_Device {
    PanelT           _panel;
    lgfx::Bus_SPI    _bus;
    lgfx::Light_PWM  _light;

public:
    Candidate(bool invert) {
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
            cfg.offset_x = 0;
            cfg.offset_y = 0;
            cfg.readable = false;      // do not trust MISO on this board
            cfg.invert = invert;
            cfg.rgb_order = false;
            cfg.dlen_16bit = false;
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
        setPanel(&_panel);
    }
};

// Draw something unmistakable: the driver's name, plus colour bars so the
// reporter can also say whether red and blue have been swapped.
void show(lgfx::LGFX_Device& lcd, const char* name, const char* invert) {
    lcd.init();
    lcd.setRotation(1);
    lcd.setBrightness(200);
    lcd.fillScreen(lcd.color565(0, 0, 0));

    lcd.setTextColor(lcd.color565(255, 166, 0));
    lcd.setFont(&fonts::FreeSansBold18pt7b);
    lcd.setCursor(12, 30);
    lcd.print(name);

    lcd.setFont(&fonts::FreeSans12pt7b);
    lcd.setCursor(12, 80);
    lcd.print(invert);

    // Left to right: red, green, blue. If the first bar looks blue, rgb_order
    // is wrong rather than the driver.
    const int y = 130, h = 70, w = 320 / 3;
    lcd.fillRect(0,     y, w, h, lcd.color565(255, 0, 0));
    lcd.fillRect(w,     y, w, h, lcd.color565(0, 255, 0));
    lcd.fillRect(w * 2, y, w, h, lcd.color565(0, 0, 255));

    lcd.setFont(&fonts::FreeSans9pt7b);
    lcd.setTextColor(lcd.color565(255, 255, 255));
    lcd.setCursor(12, 215);
    lcd.print("R    G    B  <- left to right");
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(600);
    Serial.println("\n[panel-try] cycling candidate drivers every 6 seconds");
    Serial.println("            watch the screen; exactly one should be legible");
}

void loop() {
    {
        Serial.println("  -> ILI9341, invert=false");
        Candidate<lgfx::Panel_ILI9341> lcd(false);
        show(lcd, "ILI9341", "invert = false");
        delay(6000);
    }
    {
        Serial.println("  -> ST7789, invert=true");
        Candidate<lgfx::Panel_ST7789> lcd(true);
        show(lcd, "ST7789", "invert = true");
        delay(6000);
    }
    {
        Serial.println("  -> ST7789, invert=false");
        Candidate<lgfx::Panel_ST7789> lcd(false);
        show(lcd, "ST7789", "invert = false");
        delay(6000);
    }
}
