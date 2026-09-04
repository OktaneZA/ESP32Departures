#pragma once
#include <Arduino.h>
#include <vector>
#include "model.h"

namespace ui {

// Bring up the panel (power rail, backlight, PSRAM back-buffer). Call once.
void begin(uint8_t brightness);

// Set the LCD backlight brightness (0-255) at runtime.
void setBrightness(uint8_t brightness);

// Set the board's palette (RGB565). Any argument outside 0..0xFFFF — notably
// the -1 Config uses for "not provisioned" — leaves that colour at the classic
// amber-on-black default, so an unconfigured board looks exactly as it always
// did. Call once at boot, before the first render.
void setTheme(int fg, int dim, int warn, int bg);

// "Awaiting setup" screen shown until the device is provisioned by the installer.
void renderSetup();

// Splash while WiFi/NTP come up.
void showStartup(const char* line1, const char* line2);

// Render one full frame of the train departure board. Call ~30x/sec — the clock
// and any scrolling destination advance per frame. `errCount` > 0 overlays a
// stale-data "No signal" indicator (the caller keeps passing the last good data).
// `callingAt` is accepted but not drawn by the current layout.
void renderBoard(const std::vector<Departure>& deps, const String& station,
                 const String& callingAt, int errCount);

// Render one full frame of the London bus arrivals screen — one of the screens
// the board cycles to when a bus stop is configured. Call ~30x/sec like
// renderBoard(); `sinceFetchMs` is how long ago the arrivals were fetched, so
// the "N min" countdown ticks down live between polls.
void renderBusBoard(const std::vector<BusArrival>& arrivals, const String& stopName,
                    const String& lineFilter, uint32_t sinceFetchMs, int errCount);

// Render one full frame of the river bus (pier) screen — Uber Boat by Thames
// Clippers and the Woolwich Ferry. Identical contract to renderBusBoard(): call
// ~30x/sec, and `sinceFetchMs` keeps the countdown ticking between polls.
void renderRiverBoard(const std::vector<RiverArrival>& arrivals, const String& pierName,
                      const String& lineFilter, uint32_t sinceFetchMs, int errCount);

// Render one full frame of the big-clock screen: HH:MM filling the panel in the
// provisioned palette.
//
// `night` is for blank hours — it drops the backlight to NIGHT_BRIGHTNESS so the
// board is readable in the dark without lighting the room, and the caller is
// expected to restore the brightness on the way out.
//
// `drift` nudges the digits a few pixels from centre. Blank hours run for eight
// hours with three of the four digits unchanging, so moving it occasionally
// keeps any one pixel from being lit all night.
void renderClock(bool night, int driftX, int driftY);

// Render one full frame of the weather screen. Shares the header and clock with
// every other board; the body is a large temperature and condition over two dim
// detail rows.
void renderWeatherBoard(const Weather& wx, const String& place, int errCount);

// Reset the horizontal marquees so a screen that has just come back into view
// starts its long names from the beginning rather than mid-scroll.
void resetScroll();

// Dedicated screen after repeated fetch failures (network clearly down).
void renderConnectivityWarning(const String& station, int errCount);

// Full-screen error (red title + detail) — e.g. an invalid station code.
void renderError(const String& title, const String& detail);

// Blank the screen (screen-blank hours).
void renderBlank();

// Where the panel is being touched right now, in screen coordinates.
// False on a board with no touchscreen, and on one that simply is not
// being touched. Lives here because the panel object does, and input.cpp
// has no business reaching into it.
bool getTouch(int& x, int& y);

}  // namespace ui
