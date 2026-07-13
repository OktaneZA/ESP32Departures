#pragma once
#include <Arduino.h>
#include <vector>
#include "model.h"

namespace ui {

// Bring up the panel (power rail, backlight, PSRAM back-buffer). Call once.
void begin(uint8_t brightness);

// Set the LCD backlight brightness (0-255) at runtime.
void setBrightness(uint8_t brightness);

// "Awaiting setup" screen shown until the device is provisioned by the installer.
void renderSetup();

// Splash while WiFi/NTP come up.
void showStartup(const char* line1, const char* line2);

// Render one full frame of the departure board. Call ~30x/sec — the scrolling
// "Calling at:" line and the clock advance per frame. `errCount` > 0 overlays a
// stale-data "No signal" indicator (the caller keeps passing the last good data).
void renderBoard(const std::vector<Departure>& deps, const String& station,
                 const String& callingAt, int errCount);

// Dedicated screen after repeated fetch failures (network clearly down).
void renderConnectivityWarning(const String& station, int errCount);

// Full-screen error (red title + detail) — e.g. an invalid station code.
void renderError(const String& title, const String& detail);

// Blank the screen (screen-blank hours).
void renderBlank();

}  // namespace ui
