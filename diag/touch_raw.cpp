// Does the CYD's XPT2046 answer at all, on the pins the pin map claims?
//
// LovyanGFX reported nothing with the IRQ gated *and* nothing with the bus
// polled, which rules out the interrupt line and points at the SPI link or the
// pin numbers themselves. So this drops the library and drives the controller
// directly, exactly as diag/panel_id.cpp did for the display.
//
// The XPT2046 is simple: pull CS low, send one control byte, clock back twelve
// bits. Reading pressure (Z1/Z2) matters as much as position — an untouched
// panel reads a huge resistance, so Z is the honest "is a finger on it" signal
// and it moves even when X and Y look like noise.
//
// Also reports the IRQ pin's level, which should sit high and fall on touch.
//
//     pio run -e cyd-touchraw -t upload --upload-port COM10
//
// A diagnostic, not firmware.

#include <Arduino.h>
#include <SPI.h>

namespace {

constexpr int T_SCK  = 25;
constexpr int T_MOSI = 32;
constexpr int T_MISO = 39;
constexpr int T_CS   = 33;
constexpr int T_IRQ  = 36;

// Control bytes: start bit, channel, 12-bit, differential mode.
constexpr uint8_t CMD_X  = 0xD0;
constexpr uint8_t CMD_Y  = 0x90;
constexpr uint8_t CMD_Z1 = 0xB0;
constexpr uint8_t CMD_Z2 = 0xC0;

SPIClass vspi(VSPI);

uint16_t read12(uint8_t cmd) {
    vspi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    digitalWrite(T_CS, LOW);
    vspi.transfer(cmd);
    // The result arrives in the next two bytes, left-aligned by one bit.
    const uint8_t hi = vspi.transfer(0x00);
    const uint8_t lo = vspi.transfer(0x00);
    digitalWrite(T_CS, HIGH);
    vspi.endTransaction();
    return ((uint16_t)hi << 8 | lo) >> 3;
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(600);
    Serial.println("\n[touch-raw] XPT2046 direct read");
    Serial.printf("            SCK=%d MOSI=%d MISO=%d CS=%d IRQ=%d\n",
                  T_SCK, T_MOSI, T_MISO, T_CS, T_IRQ);
    Serial.println("            touch the screen; Z should rise well above 0");
    Serial.println("            all-zero or all-4095 forever means no reply");

    pinMode(T_CS, OUTPUT);
    digitalWrite(T_CS, HIGH);
    pinMode(T_IRQ, INPUT);
    vspi.begin(T_SCK, T_MISO, T_MOSI, T_CS);
}

void loop() {
    static uint32_t n = 0;
    const uint16_t z1 = read12(CMD_Z1);
    const uint16_t z2 = read12(CMD_Z2);
    const uint16_t x  = read12(CMD_X);
    const uint16_t y  = read12(CMD_Y);
    const int irq = digitalRead(T_IRQ);

    // Print every sample while touched, and once a second when idle, so the log
    // shows both the resting values and what changes.
    const bool touched = (z1 > 100) || (irq == LOW);
    if (touched || (n % 20) == 0) {
        Serial.printf("  %s irq=%d  z1=%4u z2=%4u  x=%4u y=%4u\n",
                      touched ? "TOUCH" : "  -  ", irq, z1, z2, x, y);
    }
    ++n;
    delay(50);
}
