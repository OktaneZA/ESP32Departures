// UK Train Departure Display — LilyGo T-Display-S3 firmware.
//
// C++/JSON rewrite of the Raspberry Pi (Python/SOAP) app, using the National
// Rail LDBWS REST/JSON feed. Configuration lives on-device (NVS) and is set by
// the installer over USB serial — one pre-built binary works for every user.
//
//   * Fetch task (core 0) polls the APIs; render loop (core 1) draws the board.
//   * Shared state guarded by a mutex (render never fetches, fetch never draws).
//   * Exponential back-off; stale data kept with a "No signal" overlay; a
//     connectivity-warning screen after repeated failures; screen-blank hours.
//   * Optional second screen: live London bus arrivals from TfL's open Countdown
//     feed. When a bus stop is configured the board cycles train -> bus -> train.
//   * Until provisioned, shows an "Awaiting setup" screen and listens on serial.

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <vector>

#include "app_config.h"
#include "config.h"
#include "model.h"
#include "display.h"
#include "rail_api.h"
#include "bus_api.h"

// ---------------------------------------------------------------------------
// Shared state (fetch task writes, render loop reads) — guarded by g_mutex.
// ---------------------------------------------------------------------------
static SemaphoreHandle_t g_mutex;
static std::vector<Departure> g_deps;
static String   g_station;
static String   g_callingAt;
static int      g_errCount = 0;
static bool     g_badStation = false;   // departure CRS rejected by the API
static uint32_t g_epoch = 0;

// Bus screen state (only touched when a bus stop is configured).
static std::vector<BusArrival> g_bus;
static String   g_busStopName;
static int      g_busErrCount = 0;
static bool     g_badStop = false;      // stop code rejected by TfL (HTTP 416)
static bool     g_busHaveData = false;  // at least one successful TfL fetch
static uint32_t g_busFetchedMs = 0;     // millis() of that fetch, for the countdown
static uint32_t g_busEpoch = 0;

// ---------------------------------------------------------------------------
// WiFi + time
// ---------------------------------------------------------------------------
static void connectWiFi() {
    const Config& c = cfg::get();
    if (WiFi.status() == WL_CONNECTED) return;
    Serial.printf("[wifi] connecting to %s\n", c.wifi_ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(c.wifi_ssid.c_str(), c.wifi_pass.c_str());
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
        cfg::poll_serial();   // stay responsive to reconfiguration
        delay(250);
    }
    Serial.println(WiFi.status() == WL_CONNECTED
                       ? "[wifi] connected" : "[wifi] connect timed out");
}

static void syncTime() {
    // Use the provisioned POSIX TZ (from the installer / the user's PC locale),
    // falling back to the UK default. configTzTime sets TZ correctly — plain
    // configTime(0,0,...) would clobber TZ back to UTC and drop BST (clock 1h off).
    const Config& c = cfg::get();
    const char* tz = c.tz.length() ? c.tz.c_str() : TZ_LONDON;
    configTzTime(tz, "pool.ntp.org", "time.nist.gov");
    struct tm tm;
    for (int i = 0; i < 40 && !getLocalTime(&tm, 250); ++i) { /* up to ~10s */ }
}

static uint32_t backoffMs(int failures) {
    uint32_t d = BACKOFF_INITIAL_MS;
    for (int i = 1; i < failures && d < BACKOFF_MAX_MS; ++i) d <<= 1;
    return d > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : d;
}

// ---------------------------------------------------------------------------
// Fetch task — core 0. Never touches the display.
//
// One task drives both feeds rather than one task each: a TLS handshake needs a
// 16 KB stack, and the two polls are short and never overlap, so sharing a
// single stack costs nothing and halves the memory. Each feed keeps its own
// deadline, so the trains can refresh on the user's interval while the buses
// follow TfL's 30-second cache.
// ---------------------------------------------------------------------------

// Poll the rail API once and publish the result. Returns how long to wait.
static uint32_t fetchTrainsOnce() {
    std::vector<Departure> deps;
    String station, calling;
    rail::Fetch st = rail::fetchDepartures(cfg::get(), deps, station, calling);
    bool ok = (st == rail::Fetch::Ok);

    xSemaphoreTake(g_mutex, portMAX_DELAY);
    if (ok) {
        g_deps = deps;
        g_station = station;
        g_callingAt = calling;
        g_errCount = 0;
        g_badStation = false;
    } else if (st == rail::Fetch::BadStation) {
        g_badStation = true;   // config error — show a dedicated screen
        g_errCount = 0;        // not a connectivity problem
    } else {
        g_errCount++;          // keep last-good g_deps on screen (stale)
        g_badStation = false;
    }
    g_epoch++;
    int fails = g_errCount;
    xSemaphoreGive(g_mutex);

    if (ok) {
        Serial.printf("[fetch] ok: %d departures for %s\n", (int)deps.size(), station.c_str());
        uint32_t wait_ms = (uint32_t)cfg::get().refresh * 1000;
        return wait_ms ? wait_ms : 60000;
    }
    if (st == rail::Fetch::BadStation) {
        Serial.printf("[fetch] invalid station '%s' - reconfigure via installer\n",
                      cfg::get().dep_crs.c_str());
        return 30000;   // don't hammer a doomed request
    }
    uint32_t wait = backoffMs(fails);
    Serial.printf("[fetch] failed (%d) - retry in %us\n", fails, wait / 1000);
    return wait;
}

// Poll the TfL bus feed once and publish the result. Returns how long to wait.
static uint32_t fetchBusesOnce() {
    std::vector<BusArrival> arrivals;
    String stopName;
    bus::Fetch st = bus::fetchArrivals(cfg::get(), arrivals, stopName);
    bool ok = (st == bus::Fetch::Ok);

    xSemaphoreTake(g_mutex, portMAX_DELAY);
    if (ok) {
        g_bus = arrivals;
        g_busStopName = stopName;
        g_busErrCount = 0;
        g_badStop = false;
        g_busHaveData = true;
        g_busFetchedMs = millis();
    } else if (st == bus::Fetch::BadStop) {
        g_badStop = true;        // config error — drop the bus screen entirely
        g_busHaveData = false;
        g_busErrCount = 0;
    } else {
        g_busErrCount++;         // keep last-good arrivals on screen (stale)
        g_badStop = false;
    }
    g_busEpoch++;
    int fails = g_busErrCount;
    xSemaphoreGive(g_mutex);

    if (ok) {
        Serial.printf("[bus] ok: %d arrivals at %s\n", (int)arrivals.size(), stopName.c_str());
        return BUS_REFRESH_SECONDS * 1000UL;
    }
    if (st == bus::Fetch::BadStop) {
        Serial.printf("[bus] unknown stop code '%s' - bus screen disabled until reconfigured\n",
                      cfg::get().bus_stop.c_str());
        return 300000;   // a wrong code will not fix itself; check back rarely
    }
    uint32_t wait = backoffMs(fails);
    Serial.printf("[bus] failed (%d) - retry in %us\n", fails, wait / 1000);
    return wait;
}

static void fetchTask(void*) {
    // Signed deadline comparisons, so the scheduler survives millis() wrapping.
    uint32_t nextTrain = millis();
    uint32_t nextBus = millis();

    for (;;) {
        if (WiFi.status() != WL_CONNECTED) connectWiFi();

        if ((int32_t)(millis() - nextTrain) >= 0) {
            nextTrain = millis() + fetchTrainsOnce();
        }
        if (cfg::get().bus_enabled() && (int32_t)(millis() - nextBus) >= 0) {
            nextBus = millis() + fetchBusesOnce();
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// ---------------------------------------------------------------------------
// Screen-blank hours
// ---------------------------------------------------------------------------
static bool isBlankHour() {
    const Config& c = cfg::get();
    if (c.blank_start < 0 || c.blank_end < 0) return false;
    struct tm tm;
    if (!getLocalTime(&tm, 0)) return false;
    int h = tm.tm_hour;
    if (c.blank_start <= c.blank_end)
        return h >= c.blank_start && h < c.blank_end;
    return h >= c.blank_start || h < c.blank_end;   // wraps past midnight
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[boot] Esp32Departures - T-Display-S3");

    cfg::load();
    const Config& c = cfg::get();
    Serial.printf("[boot] provisioned=%d station=%s\n", c.provisioned(), c.dep_crs.c_str());

    g_mutex = xSemaphoreCreateMutex();
    ui::begin(c.brightness);

    // Not configured yet: show the setup screen and wait for the installer.
    // (A COMMIT over serial saves to NVS and reboots into the provisioned path.)
    if (!c.provisioned()) {
        Serial.println("[boot] unprovisioned - awaiting installer over serial");
        uint32_t last = 0;
        for (;;) {
            cfg::poll_serial();
            if (millis() - last > 500) { ui::renderSetup(); last = millis(); }
            delay(20);
        }
    }

    ui::showStartup("Esp32Departures","Connecting to WiFi...");
    connectWiFi();
    ui::showStartup("Esp32Departures","Syncing clock...");
    syncTime();

    // 16 KB stack — mbedTLS handshakes are stack-hungry.
    xTaskCreatePinnedToCore(fetchTask, "fetch", 16384, nullptr, 1, nullptr, 0);
}

// ---------------------------------------------------------------------------
// Screen rotation
//
// With no bus stop configured the board is exactly what it always was: trains,
// permanently. Once a stop is configured *and* TfL has answered for it at least
// once, the board alternates train (30s) -> bus (15s) -> train (30s) -> ...
// A stop code TfL rejects never earns a slot, so a typo costs the user nothing
// more than the trains they already had.
// ---------------------------------------------------------------------------
enum class Screen { Train, Bus };

void loop() {
    cfg::poll_serial();  // allow reconfiguration at any time (COMMIT reboots)

    static uint32_t lastEpoch = 0xFFFFFFFF;
    static std::vector<Departure> deps;
    static String station, calling;
    static int err = 0;
    static bool badStation = false;

    static uint32_t lastBusEpoch = 0xFFFFFFFF;
    static std::vector<BusArrival> bus;
    static String busStop;
    static int busErr = 0;
    static bool busReady = false;
    static uint32_t busFetchedMs = 0;

    xSemaphoreTake(g_mutex, portMAX_DELAY);
    uint32_t epoch = g_epoch;
    err = g_errCount;
    badStation = g_badStation;
    if (epoch != lastEpoch) {
        deps = g_deps;
        station = g_station;
        calling = g_callingAt;
        lastEpoch = epoch;
    }
    uint32_t busEpoch = g_busEpoch;
    busErr = g_busErrCount;
    busReady = g_busHaveData;
    busFetchedMs = g_busFetchedMs;
    if (busEpoch != lastBusEpoch) {
        bus = g_bus;
        busStop = g_busStopName;
        lastBusEpoch = busEpoch;
    }
    xSemaphoreGive(g_mutex);

    if (isBlankHour()) {
        ui::renderBlank();
        delay(1000);
        return;
    }

    // Decide which screen this frame belongs to.
    static Screen screen = Screen::Train;
    static uint32_t screenSince = 0;
    static bool timerStarted = false;
    if (!timerStarted) { screenSince = millis(); timerStarted = true; }

    bool showBus = cfg::get().bus_enabled() && busReady;
    if (!showBus) {
        if (screen != Screen::Train) { screen = Screen::Train; ui::resetScroll(); }
        screenSince = millis();   // hold the clock at zero while there is no bus screen
    } else {
        uint32_t dwell = (screen == Screen::Bus ? BUS_SCREEN_SECONDS : TRAIN_SCREEN_SECONDS) * 1000UL;
        if (millis() - screenSince >= dwell) {
            screen = (screen == Screen::Train) ? Screen::Bus : Screen::Train;
            screenSince = millis();
            ui::resetScroll();    // long names restart rather than resume mid-scroll
        }
    }

    if (screen == Screen::Bus) {
        ui::renderBusBoard(bus, busStop, cfg::get().bus_line,
                           millis() - busFetchedMs, busErr);
    } else if (badStation) {
        ui::renderError("Unknown station", cfg::get().dep_crs);
    } else {
        String label = station.length() ? station : cfg::get().dep_crs;
        if (err >= 3) {
            ui::renderConnectivityWarning(label, err);
        } else {
            ui::renderBoard(deps, label, calling, err);
        }
    }

    delay(20);  // ~30-40 fps
}
