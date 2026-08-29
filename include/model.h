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

// A live river-bus sailing carries exactly the same three fields as a bus —
// the line ("RB1"), where it terminates, and how far off it is — and the pier
// screen draws them with the same row renderer, so it shares the type rather
// than duplicating it. TfL gives river predictions as seconds-to-station, so
// `etaSeconds` means the same thing here as it does for buses.
using RiverArrival = BusArrival;

// Current conditions for the weather screen, already formatted for display.
// Temperatures are whole degrees because the row font is wide and a tenth of a
// degree is noise at a glance; the "feels like" is the one people actually act
// on, so it keeps its own line rather than being folded into the headline.
struct Weather {
    int     code = -1;   // raw WMO code, for choosing the icon
    String  temp;        // headline temperature, e.g. "18"
    String  condition;   // plain words from the WMO code, e.g. "Light rain"
    String  feels;       // apparent temperature, e.g. "17"
    String  wind;        // wind speed in mph, e.g. "12"
    String  high;        // today's maximum
    String  low;         // today's minimum
};
