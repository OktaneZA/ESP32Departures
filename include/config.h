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
    int    blank_start = -1;   // screen-blank start hour (-1 = off)
    int    blank_end   = -1;   // screen-blank end hour (-1 = off)
    int    brightness  = 180;  // 0-255
    int    refresh     = 60;   // poll interval, seconds

    bool provisioned() const {
        return wifi_ssid.length() && api_key.length() && dep_crs.length();
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
