#pragma once
#include <Arduino.h>

// A single departure row, already formatted for the display.
// Mirrors the fields the Python app renders (aimed time, status, destination,
// platform) but the status label is pre-computed here instead of at render time.
struct Departure {
    String aimed;        // scheduled departure "HH:MM"
    String status;       // "On time" | "Cancelled" | "Delayed" | "Exp HH:MM"
    String destination;  // e.g. "London Paddington"
    String platform;     // "" if the API gave no platform
    bool cancelled = false;
};

// A single live bus arrival at a stop, already formatted for the display.
// TfL's Countdown (URA) feed gives an absolute arrival time; `etaSeconds` is
// that time converted to "seconds from now" *at the moment of the fetch*, so
// the render loop can count it down without depending on the device clock.
struct BusArrival {
    String  line;         // route number, e.g. "38"
    String  destination;  // DestinationText, e.g. "Hackney Central"
    int32_t etaSeconds;   // seconds until arrival, measured at fetch time
};
