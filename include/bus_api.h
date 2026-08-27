#pragma once
#include <Arduino.h>
#include <vector>
#include "model.h"
#include "config.h"

namespace bus {

// Result of a fetch attempt (mirrors rail::Fetch).
enum class Fetch {
    Ok,       // outputs filled with fresh data (possibly zero arrivals)
    Failed,   // transient failure (WiFi, HTTP, parse) — keep stale data
    BadStop,  // the stop code is unknown to TfL (HTTP 416) — a config error
};

// Fetch live bus/river-bus arrivals for the configured stop from TfL's
// Countdown (URA) feed. The feed is open — no API key — so `cfg.bus_stop`
// (the stop's 5-digit SMS code) is the only thing required.
//
// On Ok: fills `out` with up to MAX_BUS_ARRIVALS arrivals sorted soonest-first,
// each `etaSeconds` measured against TfL's own response timestamp, and sets
// `stopName`. On Failed/BadStop: leaves outputs untouched so stale data persists.
Fetch fetchArrivals(const Config& cfg, std::vector<BusArrival>& out, String& stopName);

}  // namespace bus
