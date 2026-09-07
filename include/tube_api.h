#pragma once
#include <Arduino.h>
#include <vector>
#include "model.h"
#include "config.h"

namespace tube {

// Result of a fetch attempt (mirrors rail::Fetch, bus::Fetch and river::Fetch).
enum class Fetch {
    Ok,          // outputs filled with fresh data (possibly zero trains)
    Failed,      // transient failure (WiFi, HTTP, parse) — keep stale data
    BadStation,  // the line or station is unknown to TfL (HTTP 404) — a config error
};

// Fetch live London Underground arrivals for one line at one station, in one
// direction, from TfL's Unified API. The feed is open — no key — so the three
// settings the user picked are all that is required:
//
//   cfg.tube_stop  station Naptan, e.g. "940GZZLUKSX"
//   cfg.tube_line  line id,        e.g. "victoria"
//   cfg.tube_dir   platform token, e.g. "Northbound"
//
// All three are mandatory, and that is a deliberate narrowing rather than a
// missing feature: King's Cross answers a whole-station query with 71 KB across
// six lines and both directions, which neither fits the CYD's heap nor a
// four-row screen. Asking per line keeps the response near 19 KB at its worst
// and makes every row on the board one the reader can actually catch.
//
// On Ok: fills `out` with up to MAX_TUBE_ARRIVALS trains sorted soonest-first
// and sets `stationName`. On Failed/BadStation: leaves outputs untouched so
// stale data persists.
Fetch fetchArrivals(const Config& cfg, std::vector<TubeArrival>& out, String& stationName);

// The screen's label for a TfL line id: "victoria" -> "Victoria", and
// "hammersmith-city" -> "H&C". The header sets this in the small font beside a
// station name that needs every pixel it can keep, so the two long ones are
// abbreviated rather than left to crowd it.
//
// An unrecognised id is title-cased and returned as-is, so a line TfL adds
// later still labels its screen sensibly without a firmware update.
String lineLabel(const String& lineId);

}  // namespace tube
