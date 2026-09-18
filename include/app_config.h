#pragma once

// -----------------------------------------------------------------------------
// Non-secret, compile-time tunables. Secrets (WiFi + API creds + station codes)
// live in secrets.h — copy secrets.example.h to secrets.h and fill it in.
// -----------------------------------------------------------------------------

// How often to poll the API for fresh departures (seconds).
#define REFRESH_SECONDS       60

// How many rows of departures/arrivals a screen holds.
//
// Set per board in platformio.ini rather than in board.h, because the API
// clients cap their results with it and they must not have to pull in the
// display library to find out how tall the screen is. Three fits the
// T-Display-S3's 170px; the CYD's 240px takes four.
#ifndef BOARD_LIST_ROWS
#define BOARD_LIST_ROWS       3
#endif
#define MAX_DEPARTURES        BOARD_LIST_ROWS

// Pixel size of the full-screen clock's font. Also set per board, and for the
// same reason the row count is: it selects which baked font gets linked, so it
// has to be visible to the preprocessor rather than only to C++.
#ifndef BOARD_BIG_CLOCK_PX
#define BOARD_BIG_CLOCK_PX    104
#endif

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

// How many bus arrivals to show (see BOARD_LIST_ROWS).
#define MAX_BUS_ARRIVALS      BOARD_LIST_ROWS

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

// How many river arrivals to show (see BOARD_LIST_ROWS).
#define MAX_RIVER_ARRIVALS    BOARD_LIST_ROWS

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
// London Underground arrivals (TfL Unified API) — optional fourth screen.
//
// The same open feed as the river bus, asked one line at a time. A screen is
// only active when a station, a line and a direction are all provisioned; see
// tube_api.cpp for why all three are required rather than optional.
//
// One station can carry up to Config::kTubeSlots line+direction pairs, each
// polled and backed off independently. TUBE_SCREEN_SECONDS is the time the
// station gets as a whole, divided between however many are set.
// -----------------------------------------------------------------------------

// How many Tube arrivals to show (see BOARD_LIST_ROWS).
#define MAX_TUBE_ARRIVALS     BOARD_LIST_ROWS

// How long the Tube screen stays up in the rotation (seconds).
#define TUBE_SCREEN_SECONDS   15

// How often to poll TfL for fresh predictions (seconds). Same 30s cache at
// source as the bus feed, and the same reason not to poll inside it.
#define TUBE_REFRESH_SECONDS  30

// Ignore trains further out than this (minutes). A tube runs every two or three
// minutes off-peak, so the four rows are always filled from the next quarter of
// an hour; a wider window would only admit predictions too vague to trust.
#define TUBE_MAX_ETA_MINUTES  20

// Hard cap on the TfL response we will buffer (bytes). Anything larger is
// treated as a failed fetch rather than being allowed to exhaust the heap.
//
// 24 KB was set from too small a sample and was simply wrong: measured live,
// Euston on the Northern answers with 34,057 bytes and Acton Town on the
// Piccadilly with 26,703, so both of those screens failed on every poll. The
// driver is not interchange size but how many trains are predicted at once, so
// a branch junction can beat King's Cross (13,607). 48 KB clears the worst seen
// with room to spare, and is 16 KB above the river feed's long-standing 32 KB.
#define TUBE_MAX_RESPONSE     49152

// Dump the parsed Tube predictions to Serial once per poll.
#define RAW_TUBE_DEBUG        0

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
