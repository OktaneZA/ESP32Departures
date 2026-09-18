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
#include "board.h"
#include "model.h"
#include "display.h"
#include "ha.h"
#include "input.h"
#include "rail_api.h"
#include "bus_api.h"
#include "river_api.h"
#include "tube_api.h"
#include "weather_api.h"

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

// Tube screen state, one set per slot: a station can carry up to three
// line+direction pairs, and each keeps its own errors and staleness, so a line
// closed for works withholds only its own screen (#10).
static constexpr int TUBE_SLOTS = Config::kTubeSlots;
static std::vector<TubeArrival> g_tube[TUBE_SLOTS];
static String   g_tubeStationName[TUBE_SLOTS];
static int      g_tubeErrCount[TUBE_SLOTS] = {0};
static bool     g_badTubeStation[TUBE_SLOTS] = {false};   // rejected by TfL
static bool     g_tubeHaveData[TUBE_SLOTS] = {false};
static uint32_t g_tubeFetchedMs[TUBE_SLOTS] = {0};
static uint32_t g_tubeEpoch[TUBE_SLOTS] = {0};

// Weather screen state (only touched when a position is configured).
static Weather  g_wx;
static int      g_wxErrCount = 0;
static bool     g_badWxLocation = false;
static bool     g_wxHaveData = false;
static uint32_t g_wxEpoch = 0;

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

// Never wait less than the bus feed's budgeted interval. On an unmetered feed
// this is the identity function, so TfL's back-off behaviour is untouched.
static uint32_t busWait(uint32_t ms, uint32_t intervalMs) {
    if (cfg::get().bus_budget <= 0) return ms;
    return ms < intervalMs ? intervalMs : ms;
}

// Poll the bus feed once and publish the result. Returns how long to wait.
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

    uint32_t interval = (uint32_t)cfg::get().bus_interval(BUS_REFRESH_SECONDS) * 1000UL;

    if (ok) {
        Serial.printf("[bus] ok: %d arrivals at %s (next poll in %us)\n",
                      (int)arrivals.size(), stopName.c_str(), interval / 1000);
        return interval;
    }
    if (st == bus::Fetch::BadStop) {
        Serial.printf("[bus] unknown stop code '%s' - bus screen disabled until reconfigured\n",
                      cfg::get().bus_stop.c_str());
        return busWait(300000, interval);   // a wrong code will not fix itself
    }
    // A failed attempt still costs a request, so a metered feed cannot back off
    // any faster than its budget allows — otherwise a flaky evening would spend
    // the whole day's allowance on retries.
    uint32_t wait = busWait(backoffMs(fails), interval);
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

// Poll the TfL Tube feed once and publish the result. Returns how long to wait.
static uint32_t fetchTubeOnce(int slot) {
    const Config& c = cfg::get();
    const String line = c.tube_line_at(slot);
    const String dir = c.tube_dir_at(slot);
    std::vector<TubeArrival> trains;
    String stationName;
    tube::Fetch st = tube::fetchArrivals(c, line, dir, trains, stationName);
    bool ok = (st == tube::Fetch::Ok);

    xSemaphoreTake(g_mutex, portMAX_DELAY);
    if (ok) {
        g_tube[slot] = trains;
        g_tubeStationName[slot] = stationName;
        g_tubeErrCount[slot] = 0;
        g_badTubeStation[slot] = false;
        g_tubeHaveData[slot] = true;
        g_tubeFetchedMs[slot] = millis();
    } else if (st == tube::Fetch::BadStation) {
        g_badTubeStation[slot] = true;   // config error — drop this screen only
        g_tubeHaveData[slot] = false;
        g_tubeErrCount[slot] = 0;
    } else {
        g_tubeErrCount[slot]++;          // keep last-good trains on screen (stale)
        g_badTubeStation[slot] = false;
    }
    g_tubeEpoch[slot]++;
    int fails = g_tubeErrCount[slot];
    xSemaphoreGive(g_mutex);

    if (ok) {
        Serial.printf("[tube%d] ok: %d trains at %s (%s %s)\n", slot + 1,
                      (int)trains.size(), stationName.c_str(), line.c_str(), dir.c_str());
        return TUBE_REFRESH_SECONDS * 1000UL;
    }
    if (st == tube::Fetch::BadStation) {
        Serial.printf("[tube%d] unknown line/station '%s'/'%s' - this screen disabled "
                      "until reconfigured\n", slot + 1, line.c_str(), c.tube_stop.c_str());
        return 300000;   // a wrong code will not fix itself; check back rarely
    }
    uint32_t wait = backoffMs(fails);
    Serial.printf("[tube%d] failed (%d) - retry in %us\n", slot + 1, fails, wait / 1000);
    return wait;
}

// Poll Open-Meteo once and publish the result. Returns how long to wait.
static uint32_t fetchWeatherOnce() {
    Weather wx;
    weather::Fetch st = weather::fetchCurrent(cfg::get(), wx);
    bool ok = (st == weather::Fetch::Ok);

    xSemaphoreTake(g_mutex, portMAX_DELAY);
    if (ok) {
        g_wx = wx;
        g_wxErrCount = 0;
        g_badWxLocation = false;
        g_wxHaveData = true;
    } else if (st == weather::Fetch::BadLocation) {
        g_badWxLocation = true;
        g_wxHaveData = false;
        g_wxErrCount = 0;
    } else {
        g_wxErrCount++;          // keep the last reading on screen (stale)
        g_badWxLocation = false;
    }
    g_wxEpoch++;
    int fails = g_wxErrCount;
    xSemaphoreGive(g_mutex);

    if (ok) {
        Serial.printf("[wx] ok: %s deg, %s\n", wx.temp.c_str(), wx.condition.c_str());
        return WEATHER_REFRESH_SECONDS * 1000UL;
    }
    if (st == weather::Fetch::BadLocation) {
        Serial.println("[wx] coordinates rejected - weather screen disabled until reconfigured");
        return 300000;
    }
    uint32_t wait = backoffMs(fails);
    Serial.printf("[wx] failed (%d) - retry in %us\n", fails, wait / 1000);
    return wait;
}

static bool isBlankHour();

// Whether the panel is dark right now — written once a frame by the render loop
// and read here on the fetch task. One word written by one task and read by
// another is benign, and it says the true thing: what a metered feed cares
// about is whether anybody can see the screen, not what the clock reads.
static volatile bool g_screenDark = false;

// A metered bus feed must not spend requests while the screen is dark. Its
// allowance is divided across Config::on_hours() precisely so that none of it
// goes on hours nobody is looking; polling through a dark screen would
// overspend the day by exactly those hours. Deferring rather than consuming
// also means the first poll after the screen wakes is immediate, because the
// deadline has been sitting in the past all night.
static bool busPollAllowed() {
    return cfg::get().bus_budget <= 0 || !g_screenDark;
}

static void fetchTask(void*) {
    // Signed deadline comparisons, so the scheduler survives millis() wrapping.
    uint32_t nextTrain = millis();
    uint32_t nextBus = millis();
    uint32_t nextRiver = millis();
    uint32_t nextTube[TUBE_SLOTS];
    for (auto& t : nextTube) t = millis();
    uint32_t nextWx = millis();

    for (;;) {
        if (WiFi.status() != WL_CONNECTED) connectWiFi();

        // "Refresh now" from Home Assistant: bring every deadline forward
        // rather than fetching here, so one path does the fetching and the
        // back-off rules still apply.
        if (ha::takeForceRefresh()) {
            Serial.println("[mqtt] refresh requested");
            nextTrain = nextBus = nextRiver = nextWx = millis();
            for (auto& t : nextTube) t = millis();
        }

        if (cfg::get().train_enabled() && (int32_t)(millis() - nextTrain) >= 0) {
            nextTrain = millis() + fetchTrainsOnce();
        }
        if (cfg::get().bus_enabled() && busPollAllowed() &&
            (int32_t)(millis() - nextBus) >= 0) {
            nextBus = millis() + fetchBusesOnce();
        }
        if (cfg::get().river_enabled() && (int32_t)(millis() - nextRiver) >= 0) {
            nextRiver = millis() + fetchRiverOnce();
        }
        for (int slot = 0; slot < TUBE_SLOTS; ++slot) {
            if (cfg::get().tube_slot_enabled(slot) &&
                (int32_t)(millis() - nextTube[slot]) >= 0) {
                nextTube[slot] = millis() + fetchTubeOnce(slot);
            }
        }
        if (cfg::get().weather_enabled() && (int32_t)(millis() - nextWx) >= 0) {
            nextWx = millis() + fetchWeatherOnce();
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
// What the panel should be doing this frame.
//
// One place decides three things together — whether to go dark, whether to show
// the drifting night clock, and what the backlight must be — because they are
// one decision and used to be three. Brightness in particular was nobody's job:
// it was restored as a side effect of drawing the clock, so after a night, or
// after a button press woke the board, every other screen stayed at
// NIGHT_BRIGHTNESS until the rotation happened to come back round to the clock.
// A board with the clock switched off never recovered at all.
//
// Renderers draw. This decides. The caller applies it once per frame.
// ---------------------------------------------------------------------------
struct ScreenState {
    bool    dark;         // backlight off entirely, nothing worth drawing
    bool    nightClock;   // the dimmed drifting clock instead of the rotation
    uint8_t brightness;   // what the backlight must be this frame
};

static ScreenState screenState(bool awake) {
    const Config& c = cfg::get();

    // Home Assistant outranks the clock, in both directions: a motion sensor
    // can light the board at 02:00, and an automation can blank it at noon.
    // That is the whole point of the feature — a fixed window cannot know
    // whether anybody is in the room. The override lives in RAM only, so a
    // reboot returns the board to its configured behaviour.
    if (ha::lightOverrideActive()) {
        if (!ha::lightOn()) return { true, false, 0 };
        return { false, false, ha::lightBrightness() };
    }

    if (isBlankHour() && !awake) {
        // Blank hours show the dimmed clock unless told to go properly dark.
        // "Dark" now means the backlight really is off: renderBlank() paints
        // black, but a black screen with the backlight still lit is a glowing
        // rectangle, which is not what anyone means by off.
        if (c.night_clock()) return { false, true, (uint8_t)NIGHT_BRIGHTNESS };
        return { true, false, 0 };
    }

    return { false, false, (uint8_t)Config::pick(c.brightness, BRIGHTNESS, 0, 255) };
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.printf("\n[boot] Departure Buddy - %s\n", board::NAME);

    cfg::load();
    const Config& c = cfg::get();
    Serial.printf("[boot] provisioned=%d mode=%s station=%s bus=%s pier=%s tube=%s/%s/%s\n",
                  c.provisioned(), c.mode.length() ? c.mode.c_str() : "train,bus",
                  c.dep_crs.c_str(), c.bus_stop.c_str(), c.river_pier.c_str(),
                  c.tube_stop.c_str(), c.tube_line.c_str(), c.tube_dir.c_str());
    Serial.printf("[boot] screens: train=%d bus=%d river=%d tube=%d weather=%d clock=%d\n",
                  c.train_enabled(), c.bus_enabled(), c.river_enabled(),
                  c.tube_enabled(), c.weather_enabled(), c.clock_enabled());
    if (c.tube_slots()) {
        Serial.printf("[boot] tube: %d screen(s) at %s\n",
                      c.tube_slots(), c.tube_stop.c_str());
        for (int i = 0; i < Config::kTubeSlots; ++i) {
            if (c.tube_slot_enabled(i)) {
                Serial.printf("[boot]   %d: %s %s\n", i + 1,
                              c.tube_line_at(i).c_str(), c.tube_dir_at(i).c_str());
            }
        }
    }
    Serial.printf("[boot] weather at %d,%d (%s)\n",
                  c.wx_lat, c.wx_lon, c.wx_name.c_str());

    g_mutex = xSemaphoreCreateMutex();
    ui::begin(c.brightness);
    // After ui::begin(): on a touch board the controller comes up with the
    // panel, so there is nothing to configure until the panel exists.
    input::begin();
    // Before the first frame, so even the "Awaiting setup" screen is themed.
    ui::setTheme(c.col_fg, c.col_dim, c.col_warn, c.col_bg);
    ui::setRowLayout(c.row_time_shown());

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

    // Home Assistant, if a broker was provisioned. Runs its own task and its
    // own reconnection; nothing below waits on it, and a board with no broker
    // does not even open a socket.
    ha::begin();

    // 16 KB stack — mbedTLS handshakes are stack-hungry.
    xTaskCreatePinnedToCore(fetchTask, "fetch", 16384, nullptr, 1, nullptr, 0);
}

// ---------------------------------------------------------------------------
// Screen rotation
//
// The user enables any combination of trains, London buses, river boats and the
// Tube, and the board cycles through whichever are on — train (30s) -> bus (15s)
// -> river (15s) -> tube (15s) -> train -> ... With a single service it simply
// stays put.
//
// The three TfL screens also have to *earn* their slot by TfL having answered
// for the stop, pier or station at least once, so an ID TfL rejects costs the
// user nothing but that one screen. When nothing has earned a slot yet and there
// is no train screen to fall back on, the board says what it is waiting for
// rather than showing an empty departure board for a station that was never
// configured.
// ---------------------------------------------------------------------------
enum class Screen { Train, Bus, River, Tube, Tube2, Tube3, Clock, Weather };

// Which Tube slot a screen shows, or -1 if it is not a Tube screen.
static int tubeSlotOf(Screen s) {
    if (s == Screen::Tube) return 0;
    if (s == Screen::Tube2) return 1;
    if (s == Screen::Tube3) return 2;
    return -1;
}

// Screen <-> the ids Home Assistant uses. Two enums rather than one because
// they answer to different masters: Screen is the rotation's business and may
// be reordered freely, while ha::ViewId crosses the wire and shows up in a
// dropdown, so it has to stay put.
static int screenToView(Screen s) {
    switch (s) {
        case Screen::Bus:     return ha::VIEW_BUS;
        case Screen::River:   return ha::VIEW_RIVER;
        case Screen::Tube:    return ha::VIEW_TUBE1;
        case Screen::Tube2:   return ha::VIEW_TUBE2;
        case Screen::Tube3:   return ha::VIEW_TUBE3;
        case Screen::Weather: return ha::VIEW_WEATHER;
        case Screen::Clock:   return ha::VIEW_CLOCK;
        default:              return ha::VIEW_TRAIN;
    }
}

static Screen viewToScreen(int v) {
    switch (v) {
        case ha::VIEW_BUS:     return Screen::Bus;
        case ha::VIEW_RIVER:   return Screen::River;
        case ha::VIEW_TUBE1:   return Screen::Tube;
        case ha::VIEW_TUBE2:   return Screen::Tube2;
        case ha::VIEW_TUBE3:   return Screen::Tube3;
        case ha::VIEW_WEATHER: return Screen::Weather;
        case ha::VIEW_CLOCK:   return Screen::Clock;
        default:               return Screen::Train;
    }
}

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
        case Screen::Tube:
        case Screen::Tube2:
        case Screen::Tube3: {
            // The Tube's dwell is the time the station gets as a whole, shared
            // between however many line+direction screens are configured: a
            // minute across three lines is twenty seconds each.
            int total = Config::pick(c.dwell_tube, TUBE_SCREEN_SECONDS, 3, 300);
            int slots = c.tube_slots() > 0 ? c.tube_slots() : 1;
            seconds = total / slots;
            if (seconds < 3) seconds = 3;   // a sub-3s screen strobes
            break;
        }
        case Screen::Clock:
            seconds = Config::pick(c.dwell_clock, CLOCK_SCREEN_SECONDS, 3, 300);
            break;
        case Screen::Weather:
            seconds = Config::pick(c.dwell_wx, WEATHER_SCREEN_SECONDS, 3, 300);
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

    static uint32_t lastWxEpoch = 0xFFFFFFFF;
    static Weather wx;
    static int wxErr = 0;
    static bool wxReady = false;

    static uint32_t lastRiverEpoch = 0xFFFFFFFF;
    static std::vector<RiverArrival> river;
    static String riverPier;
    static int riverErr = 0;
    static bool riverReady = false;
    static uint32_t riverFetchedMs = 0;

    static uint32_t lastTubeEpoch[TUBE_SLOTS] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
    static std::vector<TubeArrival> tubeTrains[TUBE_SLOTS];
    static String tubeStation[TUBE_SLOTS];
    static int tubeErr[TUBE_SLOTS] = {0};
    static bool tubeReady[TUBE_SLOTS] = {false};
    static uint32_t tubeFetchedMs[TUBE_SLOTS] = {0};

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
    for (int slot = 0; slot < TUBE_SLOTS; ++slot) {
        uint32_t tubeEpoch = g_tubeEpoch[slot];
        tubeErr[slot] = g_tubeErrCount[slot];
        tubeReady[slot] = g_tubeHaveData[slot];
        tubeFetchedMs[slot] = g_tubeFetchedMs[slot];
        if (tubeEpoch != lastTubeEpoch[slot]) {
            tubeTrains[slot] = g_tube[slot];
            tubeStation[slot] = g_tubeStationName[slot];
            lastTubeEpoch[slot] = tubeEpoch;
        }
    }
    uint32_t wxEpoch = g_wxEpoch;
    wxErr = g_wxErrCount;
    wxReady = g_wxHaveData;
    if (wxEpoch != lastWxEpoch) {
        wx = g_wx;
        lastWxEpoch = wxEpoch;
    }
    xSemaphoreGive(g_mutex);

    const Config& c = cfg::get();

    // Buttons: the top one holds the clock on screen, the bottom one steps to
    // the next panel. Read every frame so a press is never missed between the
    // long dwells.
    static bool clockHold = false;
    bool stepScreen = false;

    const input::Press press = input::poll();
    const bool clockPress = press.clock;
    // A button and a Home Assistant button mean the same thing, so they arrive
    // at the same place. The latch is cleared by reading it, here and nowhere
    // else, so a press can be neither lost nor acted on twice.
    const bool nextPress = press.next || ha::takeNextPress();

    if (clockPress) {
        clockHold = !clockHold;
        ui::resetScroll();
    }
    if (nextPress) {
        // Stepping to the next panel implies leaving the held clock: the point
        // of this button is "show me the boards".
        clockHold = false;
        stepScreen = true;
    }

    static uint32_t wokeAt = 0;
    if (press.clock || press.next) {
        wokeAt = millis();
        // A real press at the board wins. A press that appears to do nothing
        // reads as broken hardware, and when a broker has died with the screen
        // forced off this is the only way back — on the CYD, whose only local
        // control is the touchscreen, it is the whole recovery path.
        ha::clearLightOverride();
    }
    bool awake = wokeAt && (millis() - wokeAt < NIGHT_WAKE_SECONDS * 1000UL);

    // The backlight is set here, once a frame, and nowhere else. No renderer
    // touches it, so no renderer can leave it somewhere the next screen did not
    // ask for.
    const ScreenState ss = screenState(awake);
    ui::setBrightness(ss.brightness);
    g_screenDark = ss.dark;
    // Cheap: each compares before it writes, and only a real change wakes the
    // publisher. The loop never touches the network itself.
    ha::setScreenOn(!ss.dark);
    ha::setBrightnessActual(ss.brightness);

    static bool darkPainted = false;
    if (ss.dark) {
        // Paint black once, not every second: with the backlight off there is
        // nothing to see, and repainting only spends SPI bandwidth. The short
        // delay is about input, not drawing — it is how quickly a button press
        // gets us out of here.
        if (!darkPainted) { ui::renderBlank(); darkPainted = true; }
        delay(50);
        return;
    }
    darkPainted = false;

    if (ss.nightClock) {
        // Both blank-hour paths return early, so anything that reports what the
        // board is showing has to be told here too — otherwise Home Assistant
        // sits on "Unknown" for the whole night, which is exactly the stretch
        // somebody is most likely to be looking at it from their phone.
        ha::setCurrentView(ha::VIEW_CLOCK);
    }

    if (ss.nightClock) {
        // Nudge the digits every NIGHT_DRIFT_SECONDS so no pixel is lit for
        // the whole night. Four positions on a slow rotation is enough —
        // the point is that nothing stays put, not that it wanders.
        time_t now = time(nullptr);
        int step = (int)((now / NIGHT_DRIFT_SECONDS) & 3);
        int dx = (step == 1) ? NIGHT_DRIFT_PX : (step == 3) ? -NIGHT_DRIFT_PX : 0;
        int dy = (step == 0) ? -NIGHT_DRIFT_PX / 2 : (step == 2) ? NIGHT_DRIFT_PX / 2 : 0;
        // The clock changes once a minute and drifts every NIGHT_DRIFT_SECONDS,
        // so redrawing twice a second is already generous; the frame budget goes
        // to noticing a press instead.
        static uint32_t lastNightDraw = 0;
        if (millis() - lastNightDraw >= 500) {
            ui::renderClock(dx, dy);
            lastNightDraw = millis();
        }
        delay(50);
        return;
    }

    // Which screens are in the rotation this frame, in a fixed order so the
    // cycle stays predictable as feeds come and go.
    Screen active[5 + Config::kTubeSlots];
    int nActive = 0;
    if (c.train_enabled())               active[nActive++] = Screen::Train;
    if (c.bus_enabled() && busReady)     active[nActive++] = Screen::Bus;
    if (c.river_enabled() && riverReady) active[nActive++] = Screen::River;
    for (int slot = 0; slot < TUBE_SLOTS; ++slot) {
        if (c.tube_slot_enabled(slot) && tubeReady[slot]) {
            active[nActive++] = slot == 0 ? Screen::Tube
                              : slot == 1 ? Screen::Tube2 : Screen::Tube3;
        }
    }
    if (c.weather_enabled() && wxReady)  active[nActive++] = Screen::Weather;
    // The clock needs no feed, so unlike the others it is ready the moment it
    // is asked for — and it is what a board with nothing else shows.
    if (c.clock_enabled())               active[nActive++] = Screen::Clock;

    static Screen screen = Screen::Train;
    static uint32_t screenSince = 0;
    static bool timerStarted = false;
    if (!timerStarted) { screenSince = millis(); timerStarted = true; }

    // A screen asked for from Home Assistant. Matched against the rotation by
    // Screen value, never by position: active[] is rebuilt every frame and
    // shrinks as feeds drop out, so an index would mean a different screen from
    // one frame to the next.
    int wantView;
    if (ha::takeViewRequest(wantView)) {
        const Screen want = viewToScreen(wantView);
        bool inRotation = false;
        for (int i = 0; i < nActive; ++i) if (active[i] == want) inRotation = true;
        if (inRotation) {
            clockHold = false;
            screen = want;
            screenSince = millis();
            ui::resetScroll();
        } else if (want == Screen::Clock) {
            // The clock needs no feed, so honour it the way the button does:
            // held whether or not it is one of the chosen screens (BTN-04).
            clockHold = true;
        } else {
            // A screen whose feed has never answered. Ignore it and let the
            // report below tell Home Assistant what is really showing, so the
            // dropdown snaps back to the truth instead of lying.
            Serial.printf("[mqtt] '%s' is not in the rotation - ignoring\n",
                          ha::viewName(wantView));
        }
    }

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
    } else if (stepScreen && nActive > 1) {
        // A button press moves on immediately and restarts the dwell, so the
        // panel you asked for gets its full time rather than the tail of the
        // one you interrupted.
        screen = active[(idx + 1) % nActive];
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

    // What is actually on the panel, which is what Home Assistant should show —
    // a held clock is the clock, whatever the rotation thinks.
    ha::setCurrentView(clockHold ? ha::VIEW_CLOCK : screenToView(screen));

    // The held clock wins over everything: it is what the button was pressed
    // for, and it works whether or not the clock is one of the chosen screens.
    if (clockHold) {
        ui::renderClock(0, 0);
    } else if (nActive == 0) {
        // Trains are off and no TfL feed has answered yet. Report the worst of
        // the failures, since a board with no screen at all is almost always a
        // network problem rather than a quiet stop.
        int worst = 0;
        String label;
        if (c.bus_enabled() && busErr > worst) { worst = busErr; label = c.bus_stop; }
        if (c.river_enabled() && riverErr > worst) { worst = riverErr; label = c.river_name.length() ? c.river_name : c.river_pier; }
        for (int slot = 0; slot < TUBE_SLOTS; ++slot) {
            if (c.tube_slot_enabled(slot) && tubeErr[slot] > worst) {
                worst = tubeErr[slot];
                label = c.tube_name.length() ? c.tube_name : c.tube_stop;
            }
        }
        if (worst >= 3) {
            ui::renderConnectivityWarning(label, worst);
        } else {
            ui::showStartup("Departure Buddy", "Loading arrivals...");
        }
    } else if (screen == Screen::Bus) {
        ui::renderBusBoard(bus, busStop, c.bus_line, millis() - busFetchedMs, busErr);
    } else if (screen == Screen::River) {
        ui::renderRiverBoard(river, riverPier, c.river_line, millis() - riverFetchedMs, riverErr);
    } else if (tubeSlotOf(screen) >= 0) {
        const int slot = tubeSlotOf(screen);
        ui::renderTubeBoard(tubeTrains[slot], tubeStation[slot],
                            tube::lineLabel(c.tube_line_at(slot)),
                            millis() - tubeFetchedMs[slot], tubeErr[slot]);
    } else if (screen == Screen::Weather) {
        ui::renderWeatherBoard(wx, c.wx_name.length() ? c.wx_name : String("Weather"), wxErr);
    } else if (screen == Screen::Clock) {
        ui::renderClock(0, 0);
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
