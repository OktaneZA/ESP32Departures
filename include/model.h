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
