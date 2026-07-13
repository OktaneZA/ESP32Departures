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
