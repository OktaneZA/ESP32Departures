#pragma once

// -----------------------------------------------------------------------------
// Non-secret, compile-time tunables. Secrets (WiFi + API creds + station codes)
// live in secrets.h — copy secrets.example.h to secrets.h and fill it in.
// -----------------------------------------------------------------------------

// How often to poll the API for fresh departures (seconds).
#define REFRESH_SECONDS       60

// How many departure rows to show on the board (max 3 fit the 170px height).
#define MAX_DEPARTURES        3

// Hide the "On time" status so the destination gets the full row width. Delays,
// cancellations, and platform numbers are always shown. Set to 0 to always show
// the status column (long names scroll rather than collide with it).
#define HIDE_ONTIME_STATUS    0

// How many services to request from the API per poll (numRows, 1–50).
#define FETCH_ROWS            10

// How far ahead to look, in minutes (timeWindow, 1–120).
#define LDBWS_TIME_WINDOW     120

// Build the "Calling at:" list for the first departure. The current layout does
// not display it (top-three-only board), so this is off. Set to 1 if you re-add
// a calling-at line — the data already arrives in the same response.
#define FETCH_CALLING_POINTS  0

// Dump the raw API response to Serial once per poll. Enable on first run to
// confirm the JSON field names against your account, then set back to 0.
#define RAW_JSON_DEBUG        0

// -----------------------------------------------------------------------------
// London bus arrivals (TfL Countdown / URA feed) — optional second screen.
// Only active when a bus stop code is provisioned; otherwise the board behaves
// exactly as before and never talks to TfL.
// -----------------------------------------------------------------------------

// How many bus arrivals to show on the bus screen (max 3 fit the 170px height).
#define MAX_BUS_ARRIVALS      3

// How long each screen stays up when a bus stop is configured (seconds).
#define TRAIN_SCREEN_SECONDS  30
#define BUS_SCREEN_SECONDS    15

// How often to poll TfL for fresh arrivals (seconds). TfL caches predictions
// for 30s at source, so polling faster than this returns identical data.
//
// Providers outside London meter by the day rather than the second. When a daily
// allowance is provisioned (Config::bus_budget) the interval is derived from it
// instead — the allowance spread evenly across the hours the screen is actually
// on — and this value becomes the floor rather than the interval.
#define BUS_REFRESH_SECONDS   30

// Ignore arrivals further out than this (minutes). The feed looks 30 minutes
// ahead; the top three are almost always much sooner than that.
#define BUS_MAX_ETA_MINUTES   30

// Hard cap on the TfL response we will buffer (bytes). A busy interchange with
// many routes is a few KB; anything larger is treated as a failed fetch rather
// than being allowed to exhaust the heap.
#define BUS_MAX_RESPONSE      24576

// Dump the raw TfL response to Serial once per poll (mirrors RAW_JSON_DEBUG).
#define RAW_BUS_DEBUG         0

// -----------------------------------------------------------------------------
// River bus arrivals (TfL Unified API) — optional third screen.
//
// Uber Boat by Thames Clippers (RB1/RB4/RB6) and the Woolwich Ferry run as TfL
// "river-bus" services, so their live predictions come from the same open feed
// as everything else. Only active when a pier is provisioned.
// -----------------------------------------------------------------------------

// How many river arrivals to show on the river screen (max 3 fit the 170px height).
#define MAX_RIVER_ARRIVALS    3

// How long the river screen stays up in the rotation (seconds).
#define RIVER_SCREEN_SECONDS  15

// How often to poll TfL for fresh river predictions (seconds).
#define RIVER_REFRESH_SECONDS 60

// Ignore sailings further out than this (minutes). Boats are far less frequent
// than buses — RB6 can be 40 minutes apart — so a bus-sized 30-minute window
// would leave the screen empty most of the day.
#define RIVER_MAX_ETA_MINUTES 120

// Hard cap on the TfL response we will buffer (bytes). A busy pier is ~6 KB;
// anything larger is treated as a failed fetch rather than exhausting the heap.
#define RIVER_MAX_RESPONSE    32768

// Dump the parsed river predictions to Serial once per poll.
#define RAW_RIVER_DEBUG       0

// -----------------------------------------------------------------------------
// Big clock and weather — optional extra screens.
// -----------------------------------------------------------------------------

// How long the full-screen clock holds in the rotation (seconds).
#define CLOCK_SCREEN_SECONDS  10

// How long the weather screen holds in the rotation (seconds).
#define WEATHER_SCREEN_SECONDS 15

// How often to poll Open-Meteo (seconds). The feed's own update interval is
// 900s, so polling faster returns identical data.
#define WEATHER_REFRESH_SECONDS 900

// Backlight brightness during blank hours when the night clock is showing.
// Low enough not to light a bedroom, high enough to read across one.
#define NIGHT_BRIGHTNESS      12

// How far the night clock drifts from centre, in pixels, and how often it moves.
// Nothing on an IPS panel burns in quickly, but a clock is on for eight hours a
// night with three of its four digits unchanging, so it is cheap insurance.
#define NIGHT_DRIFT_PX        14
#define NIGHT_DRIFT_SECONDS   60

// How long a button press wakes the board from night mode before it settles
// back to the dimmed clock. Long enough to read a departure board, short enough
// that brushing it at 3am does not leave the room lit.
#define NIGHT_WAKE_SECONDS    15

// Dump the parsed weather to Serial once per poll (mirrors RAW_JSON_DEBUG).
#define RAW_WEATHER_DEBUG     0

// Screen blank hours (24h clock). Blanks the display between START and END to
// avoid burn-in / light at night. Set both to -1 to disable.
// Example: START=1, END=5 blanks the board 01:00–05:00.
#define SCREEN_BLANK_START    -1
#define SCREEN_BLANK_END      -1

// Backlight brightness 0–255.
#define BRIGHTNESS            180

// Exponential back-off on fetch failure (ARCH-01 equivalent): 2s, 4s, 8s … cap.
#define BACKOFF_INITIAL_MS    2000
#define BACKOFF_MAX_MS        120000

// UK timezone with automatic BST switch (last Sun Mar 01:00 → last Sun Oct 02:00).
#define TZ_LONDON             "GMT0BST,M3.5.0/1,M10.5.0"
