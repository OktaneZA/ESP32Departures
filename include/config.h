#pragma once
#include <Arduino.h>

// Runtime configuration, stored on-device in NVS (flash) and set by the
// installer over USB serial — so one pre-built firmware binary works for every
// user without recompiling. See config.cpp for the provisioning protocol.
struct Config {
    String wifi_ssid;
    String wifi_pass;
    String api_key;      // National Rail LDBWS consumer key
    String dep_crs;      // departure station CRS
    String dest_crs;     // optional destination filter CRS ("" = all)
    String platform;     // optional platform filter ("" = all)
    String tz;           // POSIX TZ string ("" = firmware default, UK)
    String bus_stop;     // optional TfL bus stop SMS code ("" = bus screen off)
    String bus_line;     // optional bus route filter, e.g. "38" ("" = all routes)
    String mode;         // "both" | "train" | "bus" ("" = both, pre-mode configs)
    int    blank_start = -1;   // screen-blank start hour (-1 = off)
    int    blank_end   = -1;   // screen-blank end hour (-1 = off)
    int    brightness  = 180;  // 0-255
    int    refresh     = 60;   // poll interval, seconds

    // Which services the user asked for. `mode` only expresses intent; a service
    // is actually live when its settings are present too, so switching trains
    // off keeps the API key and station stored for switching back.
    bool wants_train() const { return mode != "bus"; }
    bool wants_bus() const { return mode != "train"; }

    bool train_enabled() const {
        return wants_train() && api_key.length() && dep_crs.length();
    }

    // The TfL feed needs no key of its own, so a stop code is the only setting
    // the bus screen requires.
    bool bus_enabled() const { return wants_bus() && bus_stop.length(); }

    // Usable once there is WiFi and at least one service to show. A bus-only
    // board is fully provisioned with no API key and no station at all.
    bool provisioned() const {
        return wifi_ssid.length() && (train_enabled() || bus_enabled());
    }
};

namespace cfg {

// Load configuration from NVS into the live config. Call once at boot.
void load();

// The live configuration (valid after load()).
Config& get();

// Service the USB-serial provisioning protocol (call frequently from loops).
// On a COMMIT command it saves to NVS and reboots, so this never returns true;
// the bool is reserved for future use.
bool poll_serial();

}  // namespace cfg
