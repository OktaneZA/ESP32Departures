#pragma once
#include <Arduino.h>
#include <vector>
#include "model.h"
#include "config.h"

namespace rail {

// Result of a fetch attempt.
enum class Fetch {
    Ok,          // outputs filled with fresh data
    Failed,      // transient failure (WiFi, HTTP, parse) — keep stale data
    BadStation,  // the departure CRS is invalid (HTTP 400) — a config error
};

// Fetch the departure board from the National Rail LDBWS REST/JSON API
// (Rail Data Marketplace), using the runtime `cfg` (station, key, filters).
//
// On Ok: fills `out` (up to MAX_DEPARTURES, after any platform filter), sets
// `stationName`, and — if FETCH_CALLING_POINTS is enabled — sets `callingAt`.
// On Failed/BadStation: leaves outputs untouched so stale data can persist.
Fetch fetchDepartures(const Config& cfg, std::vector<Departure>& out,
                      String& stationName, String& callingAt);

}  // namespace rail
