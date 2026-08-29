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
    String river_pier;   // optional TfL pier Naptan, e.g. "930GCAW" ("" = off)
    String river_line;   // optional river route filter, e.g. "RB1" ("" = all)
    String river_name;   // friendly pier name from the installer, e.g. "Canary Wharf Pier"
    String mode;         // comma-separated set, e.g. "train,bus,river" (see wants())
    int    blank_start = -1;   // screen-blank start hour (-1 = off)
    int    blank_end   = -1;   // screen-blank end hour (-1 = off)
    int    brightness  = 180;  // 0-255
    int    refresh     = 60;   // poll interval, seconds

    // Appearance and rotation timing. Every one of these is optional: -1 means
    // "not set", and the firmware falls back to the app_config.h default. That
    // is what keeps a board provisioned before these existed looking exactly as
    // it did — the same trick river_name uses for the pier label.
    int    col_fg   = -1;      // primary text colour, RGB565 (-1 = default amber)
    int    col_dim  = -1;      // secondary / dimmed text
    int    col_warn = -1;      // cancellations and alerts
    int    col_bg   = -1;      // background
    int    dwell_train = -1;   // seconds the train screen holds (-1 = default)
    int    dwell_bus   = -1;   // seconds the bus screen holds
    int    dwell_river = -1;   // seconds the river screen holds

    // A stored setting wins only when it was actually set; otherwise the
    // compile-time default applies. Colours are 16-bit, so any value outside
    // 0..0xFFFF is treated as unset rather than silently drawn as garbage.
    static int pick(int stored, int fallback, int lo, int hi) {
        return (stored >= lo && stored <= hi) ? stored : fallback;
    }

    // Which services the user asked for. `mode` is a comma-separated set, so any
    // combination can be enabled and the board cycles through whatever is on.
    //
    // Older boards stored a single exclusive word, and those configs survive a
    // firmware update untouched in NVS, so they are still read here: "" and
    // "both" both mean the trains-and-buses board that predates the river
    // screen. A lone "train" or "bus" needs no special case — it is already a
    // one-element set, and the token match below handles it as written.
    bool wants(const char* service) const {
        if (mode.isEmpty() || mode == "both") return strcmp(service, "river") != 0;
        for (int start = 0; start <= (int)mode.length(); ) {
            int comma = mode.indexOf(',', start);
            if (comma < 0) comma = mode.length();
            if (mode.substring(start, comma) == service) return true;
            start = comma + 1;
        }
        return false;
    }

    bool wants_train() const { return wants("train"); }
    bool wants_bus() const { return wants("bus"); }
    bool wants_river() const { return wants("river"); }

    // A service only actually runs when its settings are present as well as
    // wanted, so switching one off keeps its settings stored for switching back.
    bool train_enabled() const {
        return wants_train() && api_key.length() && dep_crs.length();
    }

    // The TfL feeds need no key of their own, so a stop code / pier ID is the
    // only setting the bus and river screens require.
    bool bus_enabled() const { return wants_bus() && bus_stop.length(); }
    bool river_enabled() const { return wants_river() && river_pier.length(); }

    // Usable once there is WiFi and at least one service to show. A river-only
    // board is fully provisioned with no API key and no station at all.
    bool provisioned() const {
        return wifi_ssid.length() &&
               (train_enabled() || bus_enabled() || river_enabled());
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
