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

// Fetch live bus arrivals for the configured stop, from whichever provider
// `cfg.bus_prov` selects — TfL's keyless Countdown feed in London, TransportAPI
// anywhere else. Both fill the same BusArrival, so nothing above this line
// (the rotation, the renderer, the staleness logic) knows which one ran.
//
// On Ok: fills `out` with up to MAX_BUS_ARRIVALS arrivals sorted soonest-first,
// each `etaSeconds` measured against the provider's own response timestamp
// rather than the board's clock, and sets `stopName`. On Failed/BadStop: leaves
// outputs untouched so stale data persists.
Fetch fetchArrivals(const Config& cfg, std::vector<BusArrival>& out, String& stopName);

// The two providers. Call fetchArrivals() rather than these directly; they are
// declared here only so each can live in its own translation unit.
Fetch fetchTfl(const Config& cfg, std::vector<BusArrival>& out, String& stopName);
Fetch fetchNational(const Config& cfg, std::vector<BusArrival>& out, String& stopName);

}  // namespace bus
