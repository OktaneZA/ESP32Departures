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
//   * Every service is optional: the board shows any combination of trains,
//     London buses and river boats, cycling through whichever are configured
//     and parking on the only one when just one is.
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
#include "river_api.h"

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

// River screen state (only touched when a pier is configured).
static std::vector<RiverArrival> g_river;
static String   g_riverPierName;
static int      g_riverErrCount = 0;
static bool     g_badPier = false;      // pier rejected by TfL (HTTP 404)
static bool     g_riverHaveData = false;
static uint32_t g_riverFetchedMs = 0;
static uint32_t g_riverEpoch = 0;

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
// One task drives all three feeds rather than one task each: a TLS handshake
// needs a 16 KB stack, and the polls are short and never overlap, so sharing a
// single stack costs nothing and thirds the memory. Each feed keeps its own
// deadline, so the trains refresh on the user's interval, the buses follow
// TfL's 30-second cache, and the boats poll once a minute.
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

// Poll the TfL river feed once and publish the result. Returns how long to wait.
static uint32_t fetchRiverOnce() {
    std::vector<RiverArrival> sailings;
    String pierName;
    river::Fetch st = river::fetchArrivals(cfg::get(), sailings, pierName);
    bool ok = (st == river::Fetch::Ok);

    xSemaphoreTake(g_mutex, portMAX_DELAY);
    if (ok) {
        g_river = sailings;
        g_riverPierName = pierName;
        g_riverErrCount = 0;
        g_badPier = false;
        g_riverHaveData = true;
        g_riverFetchedMs = millis();
    } else if (st == river::Fetch::BadPier) {
        g_badPier = true;          // config error — drop the river screen
        g_riverHaveData = false;
        g_riverErrCount = 0;
    } else {
        g_riverErrCount++;         // keep last-good sailings on screen (stale)
        g_badPier = false;
    }
    g_riverEpoch++;
    int fails = g_riverErrCount;
    xSemaphoreGive(g_mutex);

    if (ok) {
        Serial.printf("[river] ok: %d sailings at %s\n", (int)sailings.size(), pierName.c_str());
        return RIVER_REFRESH_SECONDS * 1000UL;
    }
    if (st == river::Fetch::BadPier) {
        Serial.printf("[river] unknown pier '%s' - river screen disabled until reconfigured\n",
                      cfg::get().river_pier.c_str());
        return 300000;   // a wrong pier will not fix itself; check back rarely
    }
    uint32_t wait = backoffMs(fails);
    Serial.printf("[river] failed (%d) - retry in %us\n", fails, wait / 1000);
    return wait;
}

static void fetchTask(void*) {
    // Signed deadline comparisons, so the scheduler survives millis() wrapping.
    uint32_t nextTrain = millis();
    uint32_t nextBus = millis();
    uint32_t nextRiver = millis();

    for (;;) {
        if (WiFi.status() != WL_CONNECTED) connectWiFi();

        if (cfg::get().train_enabled() && (int32_t)(millis() - nextTrain) >= 0) {
            nextTrain = millis() + fetchTrainsOnce();
        }
        if (cfg::get().bus_enabled() && (int32_t)(millis() - nextBus) >= 0) {
            nextBus = millis() + fetchBusesOnce();
        }
        if (cfg::get().river_enabled() && (int32_t)(millis() - nextRiver) >= 0) {
            nextRiver = millis() + fetchRiverOnce();
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
    Serial.println("\n[boot] Departure Buddy - T-Display-S3");

    cfg::load();
    const Config& c = cfg::get();
    Serial.printf("[boot] provisioned=%d mode=%s station=%s bus=%s pier=%s\n",
                  c.provisioned(), c.mode.length() ? c.mode.c_str() : "train,bus",
                  c.dep_crs.c_str(), c.bus_stop.c_str(), c.river_pier.c_str());

    g_mutex = xSemaphoreCreateMutex();
    ui::begin(c.brightness);
    // Before the first frame, so even the "Awaiting setup" screen is themed.
    ui::setTheme(c.col_fg, c.col_dim, c.col_warn, c.col_bg);

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

    ui::showStartup("Departure Buddy","Connecting to WiFi...");
    connectWiFi();
    ui::showStartup("Departure Buddy","Syncing clock...");
    syncTime();

    // 16 KB stack — mbedTLS handshakes are stack-hungry.
    xTaskCreatePinnedToCore(fetchTask, "fetch", 16384, nullptr, 1, nullptr, 0);
}

// ---------------------------------------------------------------------------
// Screen rotation
//
// The user enables any combination of trains, London buses and river boats, and
// the board cycles through whichever are on — train (30s) -> bus (15s) ->
// river (15s) -> train -> ... With a single service it simply stays put.
//
// The two TfL screens also have to *earn* their slot by TfL having answered for
// the stop or pier at least once, so an ID TfL rejects costs the user nothing
// but that one screen. When nothing has earned a slot yet and there is no train
// screen to fall back on, the board says what it is waiting for rather than
// showing an empty departure board for a station that was never configured.
// ---------------------------------------------------------------------------
enum class Screen { Train, Bus, River };

// How long a screen holds before the rotation moves on. The provisioned value
// wins when there is one; otherwise the app_config.h default applies, so a board
// configured before these settings existed keeps its original timing. Clamped to
// 3..300s: a sub-second dwell would strobe the board, and the marquee needs long
// enough to actually read a scrolling name.
static uint32_t dwellMs(Screen s) {
    const Config& c = cfg::get();
    int seconds;
    switch (s) {
        case Screen::Bus:
            seconds = Config::pick(c.dwell_bus, BUS_SCREEN_SECONDS, 3, 300);
            break;
        case Screen::River:
            seconds = Config::pick(c.dwell_river, RIVER_SCREEN_SECONDS, 3, 300);
            break;
        default:
            seconds = Config::pick(c.dwell_train, TRAIN_SCREEN_SECONDS, 3, 300);
            break;
    }
    return (uint32_t)seconds * 1000UL;
}

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

    static uint32_t lastRiverEpoch = 0xFFFFFFFF;
    static std::vector<RiverArrival> river;
    static String riverPier;
    static int riverErr = 0;
    static bool riverReady = false;
    static uint32_t riverFetchedMs = 0;

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
    uint32_t riverEpoch = g_riverEpoch;
    riverErr = g_riverErrCount;
    riverReady = g_riverHaveData;
    riverFetchedMs = g_riverFetchedMs;
    if (riverEpoch != lastRiverEpoch) {
        river = g_river;
        riverPier = g_riverPierName;
        lastRiverEpoch = riverEpoch;
    }
    xSemaphoreGive(g_mutex);

    if (isBlankHour()) {
        ui::renderBlank();
        delay(1000);
        return;
    }

    // Which screens are in the rotation this frame, in a fixed order so the
    // cycle stays predictable as feeds come and go.
    const Config& c = cfg::get();
    Screen active[3];
    int nActive = 0;
    if (c.train_enabled())              active[nActive++] = Screen::Train;
    if (c.bus_enabled() && busReady)    active[nActive++] = Screen::Bus;
    if (c.river_enabled() && riverReady) active[nActive++] = Screen::River;

    static Screen screen = Screen::Train;
    static uint32_t screenSince = 0;
    static bool timerStarted = false;
    if (!timerStarted) { screenSince = millis(); timerStarted = true; }

    int idx = -1;
    for (int i = 0; i < nActive; ++i) if (active[i] == screen) idx = i;

    if (nActive == 0) {
        screenSince = millis();
    } else if (idx < 0) {
        // The screen we were on has dropped out of the rotation (TfL started
        // rejecting its ID, or the user switched it off) — land on the first
        // one still in it rather than rendering a screen nothing feeds.
        screen = active[0];
        screenSince = millis();
        ui::resetScroll();
    } else if (nActive == 1) {
        // Only one screen to show — park on it and hold the timer at zero so the
        // first cycle is a full dwell once another one appears.
        screenSince = millis();
    } else if (millis() - screenSince >= dwellMs(screen)) {
        screen = active[(idx + 1) % nActive];
        screenSince = millis();
        ui::resetScroll();    // long names restart rather than resume mid-scroll
    }

    if (nActive == 0) {
        // Trains are off and neither TfL feed has answered yet. Report the
        // worse of the two failures, since a board with no screen at all is
        // almost always a network problem rather than a quiet stop.
        int worst = 0;
        String label;
        if (c.bus_enabled() && busErr > worst) { worst = busErr; label = c.bus_stop; }
        if (c.river_enabled() && riverErr > worst) { worst = riverErr; label = c.river_name.length() ? c.river_name : c.river_pier; }
        if (worst >= 3) {
            ui::renderConnectivityWarning(label, worst);
        } else {
            ui::showStartup("Departure Buddy", "Loading arrivals...");
        }
    } else if (screen == Screen::Bus) {
        ui::renderBusBoard(bus, busStop, c.bus_line, millis() - busFetchedMs, busErr);
    } else if (screen == Screen::River) {
        ui::renderRiverBoard(river, riverPier, c.river_line, millis() - riverFetchedMs, riverErr);
    } else if (badStation) {
        ui::renderError("Unknown station", c.dep_crs);
    } else {
        String label = station.length() ? station : c.dep_crs;
        if (err >= 3) {
            ui::renderConnectivityWarning(label, err);
        } else {
            ui::renderBoard(deps, label, calling, err);
        }
    }

    delay(20);  // ~30-40 fps
}
