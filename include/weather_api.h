#pragma once
#include <Arduino.h>
#include "model.h"
#include "config.h"

namespace weather {

// Result of a fetch attempt (mirrors rail::Fetch, bus::Fetch and river::Fetch).
enum class Fetch {
    Ok,           // `out` filled with current conditions
    Failed,       // transient failure (WiFi, HTTP, parse) — keep stale data
    BadLocation,  // the coordinates were rejected — a config error
};

// Fetch current conditions for the configured position from Open-Meteo.
//
// Open, keyless and unmetered, in keeping with the TfL feeds: the only thing
// required is `cfg.wx_lat` / `cfg.wx_lon`, which the installer derives from
// whatever station, stop or pier was chosen, so the user is never asked for a
// location twice.
//
// On Ok: fills `out`. On Failed/BadLocation: leaves it untouched so the last
// good reading stays on screen.
Fetch fetchCurrent(const Config& cfg, Weather& out);

// Plain words for a WMO weather code, as used by `weather_code` in the feed.
const char* describe(int wmoCode);

}  // namespace weather
