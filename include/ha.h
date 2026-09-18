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
// library to add), runs its own task, and reconnects by itself. Our code is a
// config block, an event handler and some publish helpers.
namespace ha {

// Start the client if a broker is configured. Safe to call when it is not —
// it returns having done nothing, and the board opens no socket. Call after
// WiFi is up; the client reconnects on its own thereafter.
void begin();

// "off" (not configured), "down" (configured, not connected), "up", or
// "auth" (the broker refused our credentials). Reported by GET, so a board
// that is not appearing in Home Assistant can say why without a debugger.
const char* statusWord();

// The device id Home Assistant knows this board by, e.g. "db-3c71bf". Derived
// from the eFuse MAC, so it is stable across reboots and reflashes.
String deviceId();

}  // namespace ha
