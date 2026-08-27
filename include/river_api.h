#pragma once
#include <Arduino.h>
#include <vector>
#include "model.h"
#include "config.h"

namespace river {

// Result of a fetch attempt (mirrors rail::Fetch and bus::Fetch).
enum class Fetch {
    Ok,       // outputs filled with fresh data (possibly zero sailings)
    Failed,   // transient failure (WiFi, HTTP, parse) — keep stale data
    BadPier,  // the pier ID is unknown to TfL (HTTP 404) — a config error
};

// Fetch live river-bus arrivals for the configured pier from TfL's Unified API.
//
// Covers Uber Boat by Thames Clippers (RB1/RB4/RB6) and the Woolwich Ferry,
// which TfL publishes as ordinary "river-bus" predictions alongside its other
// modes. The feed is open — no key — so `cfg.river_pier` (a pier Naptan such as
// "930GCAW") is the only thing required.
//
// On Ok: fills `out` with up to MAX_RIVER_ARRIVALS sailings sorted soonest-first
// and sets `pierName`. On Failed/BadPier: leaves outputs untouched so stale data
// persists.
Fetch fetchArrivals(const Config& cfg, std::vector<RiverArrival>& out, String& pierName);

}  // namespace river
