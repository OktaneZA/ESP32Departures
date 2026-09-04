// Panel identification for the Cheap Yellow Display.
//
// The CYD ships with either an ILI9341 or an ST7789 behind the same product
// name, and guessing wrong produces a lit screen full of garbage rather than a
// dark one — which is exactly what the board did. Rather than flash one build
// per guess, ask the controller what it is.
//
// Both parts answer the MIPI DCS identification commands, with different
// values:
//
//   0xD3 (RDDID4)  ILI9341 -> 00 93 41
//   0x04 (RDDID)   ST7789  -> 85 85 52
//
// Reads need a dummy clock cycle after the command, so the first byte back is
// discarded. MISO is wired on this board (IO12), which is what makes any of
// this possible.
//
// Build and run with:
//     pio run -e cyd-panelid -t upload --upload-port COM10
//
// It is a diagnostic, not part of the firmware: no env builds it by accident.

#include <Arduino.h>
#include <SPI.h>

constexpr int PIN_SCK  = 14;
constexpr int PIN_MOSI = 13;
constexpr int PIN_MISO = 12;
constexpr int PIN_CS   = 15;
constexpr int PIN_DC   = 2;
constexpr int PIN_BL   = 21;

SPIClass hspi(HSPI);

// Read `n` data bytes back from a controller register.
static void readReg(uint8_t cmd, uint8_t* out, int n) {
    // Slowly: these reads are specified far below the write clock, and a read
    // that is too fast returns plausible-looking rubbish rather than failing.
    hspi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_CS, LOW);
    digitalWrite(PIN_DC, LOW);          // command phase
    hspi.transfer(cmd);
    digitalWrite(PIN_DC, HIGH);         // data phase
    hspi.transfer(0x00);                // dummy clock, discarded
    for (int i = 0; i < n; ++i) out[i] = hspi.transfer(0x00);
    digitalWrite(PIN_CS, HIGH);
    hspi.endTransaction();
}

static void dump(const char* label, uint8_t cmd, int n) {
    uint8_t b[8] = {0};
    readReg(cmd, b, n);
    Serial.printf("  %-22s 0x%02X ->", label, cmd);
    for (int i = 0; i < n; ++i) Serial.printf(" %02X", b[i]);
    Serial.println();
}

void setup() {
    Serial.begin(115200);
    delay(600);
    Serial.println("\n[panel-id] Cheap Yellow Display controller identification");

    pinMode(PIN_CS, OUTPUT);  digitalWrite(PIN_CS, HIGH);
    pinMode(PIN_DC, OUTPUT);  digitalWrite(PIN_DC, HIGH);
    pinMode(PIN_BL, OUTPUT);  digitalWrite(PIN_BL, HIGH);   // backlight on
    hspi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);

    // Wake the controller first: some parts answer identification only once
    // they are out of sleep, and a sleeping panel reads back as all zeroes.
    hspi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_CS, LOW);
    digitalWrite(PIN_DC, LOW);
    hspi.transfer(0x11);            // SLPOUT
    digitalWrite(PIN_CS, HIGH);
    hspi.endTransaction();
    delay(150);

    Serial.println("\nidentification registers:");
    dump("RDDID4  (ILI9341)", 0xD3, 3);
    dump("RDDID   (ST7789)",  0x04, 3);
    dump("RDDST   status",    0x09, 4);
    dump("RDDPM   power",     0x0A, 1);
    dump("RDDMADCTL",         0x0B, 1);
    dump("RDDCOLMOD",         0x0C, 1);

    uint8_t d3[3] = {0}, id[3] = {0};
    readReg(0xD3, d3, 3);
    readReg(0x04, id, 3);

    Serial.println();
    if (d3[1] == 0x93 && d3[2] == 0x41) {
        Serial.println("VERDICT: ILI9341  -> build without -DCYD_PANEL_ST7789");
    } else if (id[0] == 0x85 || (id[1] == 0x85 && id[2] == 0x52)) {
        Serial.println("VERDICT: ST7789   -> build with -DCYD_PANEL_ST7789");
    } else if (!d3[0] && !d3[1] && !d3[2] && !id[0] && !id[1] && !id[2]) {
        Serial.println("VERDICT: no reply at all. Either MISO is not wired on this");
        Serial.println("         revision, or the read clock is still too fast.");
        Serial.println("         Fall back to trying each driver in turn.");
    } else {
        Serial.println("VERDICT: unrecognised controller - see the raw bytes above.");
    }
}

void loop() {
    delay(1000);
}
