#pragma once
#include <Arduino.h>

// Home Assistant over MQTT — optional, and inert until a broker is provisioned.
//
// The one rule this module is built on, and the reason it can never break the
// board: **MQTT never draws, and the renderer never talks to MQTT.** Everything
// inbound lands in a few POD fields behind a mutex; main.cpp reads them on its
// own schedule and is the only code that calls ui::*. A broker that is down,
// wrong, or hostile therefore costs exactly one thing — nothing on the display.
//
// The client is ESP-IDF's esp-mqtt, which ships inside the Arduino core (no
// library to add), runs its own task, and reconnects by itself.
namespace ha {

// The screens Home Assistant can ask for, in rotation order. Deliberately its
// own enum rather than main.cpp's Screen: this one crosses the wire and appears
// in a Home Assistant dropdown, so it must stay stable even if the rotation is
// reordered. main.cpp maps these onto its own Screen values.
enum ViewId {
    VIEW_TRAIN = 0,
    VIEW_BUS,
    VIEW_RIVER,
    VIEW_TUBE1,
    VIEW_TUBE2,
    VIEW_TUBE3,
    VIEW_WEATHER,
    VIEW_CLOCK,
    VIEW_COUNT,
};

// The label Home Assistant shows for a view, e.g. "Tube 1". Never null.
const char* viewName(int id);

// Start the client if a broker is configured. Safe to call when it is not —
// it returns having done nothing, and the board opens no socket. Call after
// WiFi is up; the client reconnects on its own thereafter.
void begin();

// "off" (not configured), "down" (configured, not connected), "up", or
// "auth" (the broker refused our credentials). Reported by GET, so a board
// that is not appearing in Home Assistant can say why without a debugger.
const char* statusWord();

// The device id Home Assistant knows this board by, e.g. "db-f604a7". Derived
// from the eFuse MAC, so it is stable across reboots and reflashes.
String deviceId();

// --- what the board tells Home Assistant -----------------------------------
// Call these from the render loop as often as you like: each compares before it
// writes and only wakes the publisher when something actually changed. None of
// them talks to the network.

void setCurrentView(int viewId);
void setScreenOn(bool on);
void setBrightnessActual(uint8_t brightness);

// --- what Home Assistant asks of the board ---------------------------------
// Each latch has exactly one consumer, and reading it clears it, so a command
// can be neither lost nor acted on twice.

bool takeNextPress();                  // consumed by loop() only
bool takeForceRefresh();               // consumed by fetchTask only
bool takeViewRequest(int& viewId);     // consumed by loop() only

// --- the backlight override -------------------------------------------------
// Home Assistant's light entity. While the override is active it outranks the
// configured blank hours in both directions: a motion sensor can light the
// board at 02:00, and an automation can blank it at noon. It lives in RAM only
// and dies at reboot, deliberately — a board that came back from a power cut
// still dark, because of an automation nobody remembers writing, is a fault
// report waiting to happen.

bool lightOverrideActive();
bool lightOn();
uint8_t lightBrightness();

// Hand control back to the board's own settings, and tell Home Assistant. A
// physical button press or a touch calls this: a press that appears to do
// nothing reads as broken hardware, and on a board whose broker has died with
// the screen forced off, this is the only way back.
void clearLightOverride();

}  // namespace ha
