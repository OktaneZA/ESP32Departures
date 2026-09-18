// Home Assistant over MQTT — connection, discovery, commands and state.
//
// Uses ESP-IDF's esp-mqtt rather than an Arduino MQTT library. It is already in
// the core the firmware is built against (`-lmqtt` is on the default link line),
// so it adds no dependency to pin or hash-check; it runs its own task, so the
// fetch task blocking for seconds inside a TLS handshake cannot starve our
// keepalive; and it reconnects on its own.
//
// Note the config struct here is the FLAT one from IDF 4.4, which is what the
// Arduino 2.0.x core is built on — `.host`, `.lwt_topic`, `.buffer_size`. The
// current esp-mqtt documentation describes IDF 5.x's nested form
// (`broker.address.uri`), which does not compile against this core.
//
// Threading: esp-mqtt's task delivers events, a small publisher task of ours
// sends state, and the render loop only ever touches POD fields behind
// s_mutex. No accessor here holds that mutex across anything but a copy.

#include "ha.h"
#include "config.h"
#include "app_config.h"
#include "board.h"

#include <ArduinoJson.h>
#include <WiFi.h>
#include "mqtt_client.h"

namespace {

esp_mqtt_client_handle_t s_client = nullptr;

// esp-mqtt keeps the pointers it is given rather than copying the strings, so
// every one of these has to outlive begin(). File scope is that guarantee.
String s_host, s_user, s_pass, s_id, s_base, s_availTopic;

volatile bool s_connected  = false;
volatile bool s_authFailed = false;

SemaphoreHandle_t s_mutex = nullptr;

// --- inbound: written by the MQTT task, consumed by exactly one reader each ---
bool    s_lightSet    = false;      // has Home Assistant taken control?
bool    s_lightOn     = true;
uint8_t s_lightBright = BRIGHTNESS;
bool    s_nextLatch    = false;     // loop() consumes
bool    s_refreshLatch = false;     // fetchTask consumes
int8_t  s_viewReq      = -1;        // loop() consumes; a value latch, last wins

// --- outbound: written by the render loop, published by our publisher task ---
int      s_view         = -1;
bool     s_screenOn     = true;
uint8_t  s_brightActual = BRIGHTNESS;
uint32_t s_outEpoch     = 0;

// Retained messages arrive the instant we subscribe. A retained command is
// nobody's intent — it is an echo of an old one — so ignore everything for a
// moment after subscribing. This is also what makes "overrides never persist"
// true: a retained light/set cannot re-blank the board on the next boot.
uint32_t s_subscribedAt = 0;
constexpr uint32_t RETAINED_GUARD_MS = 1000;

// Below this, decline to start. An MQTT client that squeezes in with nothing to
// spare does not fail here — it fails hours later as a TLS handshake in the
// fetch task, and the departure board goes stale for reasons nobody connects to
// the feature added last month. The CYD has no PSRAM and is the board that
// makes this real.
constexpr uint32_t MIN_FREE_HEAP = 40000;

struct Lock {
    Lock()  { if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY); }
    ~Lock() { if (s_mutex) xSemaphoreGive(s_mutex); }
};

void bumpEpoch() { s_outEpoch++; }

String topic(const char* leaf) { return s_base + "/" + leaf; }

void pub(const String& t, const String& payload, int qos, bool retain) {
    if (!s_client) return;
    esp_mqtt_client_publish(s_client, t.c_str(), payload.c_str(),
                            (int)payload.length(), qos, retain ? 1 : 0);
}

// Which views this board can actually show, so Home Assistant's dropdown offers
// the screens that exist rather than every screen that could. Fixed at boot:
// the live rotation shrinks as feeds fail, and a dropdown that changes under
// the user is worse than one that occasionally offers a screen still loading.
bool viewConfigured(int id) {
    const Config& c = cfg::get();
    switch (id) {
        case ha::VIEW_TRAIN:   return c.train_enabled();
        case ha::VIEW_BUS:     return c.bus_enabled();
        case ha::VIEW_RIVER:   return c.river_enabled();
        case ha::VIEW_TUBE1:   return c.tube_slot_enabled(0);
        case ha::VIEW_TUBE2:   return c.tube_slot_enabled(1);
        case ha::VIEW_TUBE3:   return c.tube_slot_enabled(2);
        case ha::VIEW_WEATHER: return c.weather_enabled();
        case ha::VIEW_CLOCK:   return c.clock_enabled();
        default:               return false;
    }
}

// One discovery message. Home Assistant's abbreviated keys and the `~`
// base-topic substitution roughly halve each payload, which is what keeps every
// one of these inside the 1 KB client buffer on a board with no PSRAM.
void addDevice(JsonDocument& d) {
    d["~"] = s_base;
    d["avty_t"] = "~/status";
    JsonObject dev = d["dev"].to<JsonObject>();
    dev["ids"][0] = s_id;
    dev["name"] = "Departure Buddy";
    dev["mdl"] = board::NAME;
    dev["mf"] = "Departure Buddy";
    dev["sw"] = FW_VERSION;
}

void publishOne(const char* component, const char* object, JsonDocument& d) {
    String t = String(HA_DISCOVERY_PREFIX) + "/" + component + "/" + s_id + "/" +
               object + "/config";
    String payload;
    serializeJson(d, payload);
    pub(t, payload, 1, /*retain=*/true);
    // Let lwIP's buffers drain between entities rather than stacking seven
    // payloads in DRAM at once.
    vTaskDelay(pdMS_TO_TICKS(20));
}

void publishDiscovery() {
    {
        JsonDocument d;
        addDevice(d);
        d["name"] = "Backlight";
        d["uniq_id"] = s_id + "_backlight";
        d["schema"] = "json";
        d["brightness"] = true;
        d["brightness_scale"] = 255;
        d["stat_t"] = "~/light/state";
        d["cmd_t"] = "~/light/set";
        publishOne("light", "backlight", d);
    }
    {
        JsonDocument d;
        addDevice(d);
        d["name"] = "Next panel";
        d["uniq_id"] = s_id + "_next";
        d["cmd_t"] = "~/button/next/press";
        d["icon"] = "mdi:page-next-outline";
        publishOne("button", "next", d);
    }
    {
        JsonDocument d;
        addDevice(d);
        d["name"] = "Refresh now";
        d["uniq_id"] = s_id + "_refresh";
        d["cmd_t"] = "~/button/refresh/press";
        d["icon"] = "mdi:refresh";
        publishOne("button", "refresh", d);
    }
    {
        JsonDocument d;
        addDevice(d);
        d["name"] = "View";
        d["uniq_id"] = s_id + "_view";
        d["cmd_t"] = "~/view/set";
        d["stat_t"] = "~/view/state";
        JsonArray opts = d["options"].to<JsonArray>();
        for (int i = 0; i < ha::VIEW_COUNT; ++i) {
            if (viewConfigured(i)) opts.add(ha::viewName(i));
        }
        d["icon"] = "mdi:television-guide";
        publishOne("select", "view", d);
    }
    {
        JsonDocument d;
        addDevice(d);
        d["name"] = "Current view";
        d["uniq_id"] = s_id + "_viewstate";
        d["stat_t"] = "~/view/state";
        d["icon"] = "mdi:monitor-dashboard";
        publishOne("sensor", "viewstate", d);
    }
    {
        JsonDocument d;
        addDevice(d);
        d["name"] = "WiFi signal";
        d["uniq_id"] = s_id + "_rssi";
        d["stat_t"] = "~/rssi/state";
        d["dev_cla"] = "signal_strength";
        d["unit_of_meas"] = "dBm";
        d["stat_cla"] = "measurement";
        d["ent_cat"] = "diagnostic";
        publishOne("sensor", "rssi", d);
    }
    {
        JsonDocument d;
        addDevice(d);
        d["name"] = "Screen on";
        d["uniq_id"] = s_id + "_screen";
        d["stat_t"] = "~/screen/state";
        d["pl_on"] = "ON";
        d["pl_off"] = "OFF";
        d["ent_cat"] = "diagnostic";
        publishOne("binary_sensor", "screen", d);
    }
    Serial.println("[mqtt] published discovery for 7 entities");
}

void publishStates() {
    bool on;
    uint8_t bright;
    int view;
    {
        Lock l;
        on = s_screenOn;
        bright = s_brightActual;
        view = s_view;
    }
    JsonDocument d;
    d["state"] = on ? "ON" : "OFF";
    d["brightness"] = bright;
    String light;
    serializeJson(d, light);
    pub(topic("light/state"), light, 0, true);
    pub(topic("screen/state"), on ? "ON" : "OFF", 0, true);
    if (view >= 0 && view < ha::VIEW_COUNT) {
        pub(topic("view/state"), ha::viewName(view), 0, true);
    }
}

void handleCommand(const String& t, const String& payload) {
    if (millis() - s_subscribedAt < RETAINED_GUARD_MS) {
        Serial.printf("[mqtt] ignoring retained '%s' arriving on subscribe\n", t.c_str());
        return;
    }

    // Home Assistant restarting republishes its birth message; discovery is
    // retained, but a fresh HA that purged its store would otherwise show our
    // entities as unavailable forever.
    if (t == String(HA_DISCOVERY_PREFIX) + "/status") {
        if (payload == "online") {
            Serial.println("[mqtt] Home Assistant restarted - re-announcing");
            publishDiscovery();
            publishStates();
        }
        return;
    }

    if (t == topic("light/set")) {
        JsonDocument d;
        if (deserializeJson(d, payload)) {
            Serial.printf("[mqtt] could not parse light command: %s\n", payload.c_str());
            return;
        }
        Lock l;
        s_lightSet = true;
        if (!d["state"].isNull()) s_lightOn = (d["state"].as<String>() == "ON");
        if (!d["brightness"].isNull()) {
            int b = d["brightness"].as<int>();
            s_lightBright = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
            // Home Assistant sends brightness alone when the slider moves, and
            // a light being dimmed is a light that is on.
            if (d["state"].isNull()) s_lightOn = true;
        }
        bumpEpoch();
        return;
    }
    if (t == topic("button/next/press"))    { Lock l; s_nextLatch = true; return; }
    if (t == topic("button/refresh/press")) { Lock l; s_refreshLatch = true; return; }
    if (t == topic("view/set")) {
        for (int i = 0; i < ha::VIEW_COUNT; ++i) {
            if (payload == ha::viewName(i)) { Lock l; s_viewReq = (int8_t)i; return; }
        }
        Serial.printf("[mqtt] unknown view '%s'\n", payload.c_str());
    }
}

void subscribeAll() {
    esp_mqtt_client_subscribe(s_client, topic("light/set").c_str(), 1);
    esp_mqtt_client_subscribe(s_client, topic("view/set").c_str(), 1);
    esp_mqtt_client_subscribe(s_client, topic("button/next/press").c_str(), 1);
    esp_mqtt_client_subscribe(s_client, topic("button/refresh/press").c_str(), 1);
    esp_mqtt_client_subscribe(s_client, (String(HA_DISCOVERY_PREFIX) + "/status").c_str(), 0);
    s_subscribedAt = millis();
}

void onMqttEvent(void*, esp_event_base_t, int32_t id, void* data) {
    auto* ev = static_cast<esp_mqtt_event_handle_t>(data);

    switch ((esp_mqtt_event_id_t)id) {
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            s_authFailed = false;
            // Order matters: availability first, or Home Assistant briefly has
            // entities it believes are offline; discovery before state, or it
            // drops the first state it is sent.
            pub(s_availTopic, "online", 1, true);
            subscribeAll();
            publishDiscovery();
            publishStates();
            Serial.printf("[mqtt] connected to %s:%d as %s (free heap %u)\n",
                          s_host.c_str(), cfg::get().mqtt_port_or_default(),
                          s_id.c_str(), (unsigned)ESP.getFreeHeap());
            break;

        case MQTT_EVENT_DISCONNECTED:
            if (s_connected) Serial.println("[mqtt] disconnected - reconnecting");
            s_connected = false;
            break;

        case MQTT_EVENT_DATA: {
            if (!ev || ev->topic_len <= 0) break;   // a continuation fragment
            String t(ev->topic, ev->topic_len);
            String p(ev->data, ev->data_len);
            handleCommand(t, p);
            break;
        }

        case MQTT_EVENT_ERROR:
            // A refused connection is a configuration problem and will not fix
            // itself; a transport error is weather. Say which, once, rather
            // than filling the log with the same line every ten seconds.
            if (ev && ev->error_handle &&
                ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                const int rc = ev->error_handle->connect_return_code;
                if (!s_authFailed) {
                    Serial.printf("[mqtt] broker refused the connection (rc=%d)%s\n", rc,
                                  rc == MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
                                  rc == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED
                                      ? " - check the username and password" : "");
                }
                s_authFailed = true;
            }
            break;

        default:
            break;
    }
}

// Publishing lives here rather than in the render loop, so the loop never waits
// on a socket. It is cheap: a 250ms tick that does nothing at all unless
// something actually changed.
void publisherTask(void*) {
    uint32_t published = 0xFFFFFFFF;
    uint32_t nextRssi = 0;
    for (;;) {
        if (s_connected) {
            uint32_t epoch;
            { Lock l; epoch = s_outEpoch; }
            if (epoch != published) {
                publishStates();
                published = epoch;
            }
            if ((int32_t)(millis() - nextRssi) >= 0) {
                pub(topic("rssi/state"), String(WiFi.RSSI()), 0, true);
                nextRssi = millis() + HA_RSSI_SECONDS * 1000UL;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

}  // namespace

namespace ha {

const char* viewName(int id) {
    switch (id) {
        case VIEW_TRAIN:   return "Trains";
        case VIEW_BUS:     return "Buses";
        case VIEW_RIVER:   return "River";
        case VIEW_TUBE1:   return "Tube 1";
        case VIEW_TUBE2:   return "Tube 2";
        case VIEW_TUBE3:   return "Tube 3";
        case VIEW_WEATHER: return "Weather";
        case VIEW_CLOCK:   return "Clock";
        default:           return "Unknown";
    }
}

String deviceId() {
    if (s_id.length()) return s_id;
    // The eFuse MAC is readable before the radio starts, so identity never
    // depends on WiFi having come up first.
    const uint64_t mac = ESP.getEfuseMac();
    char buf[16];
    snprintf(buf, sizeof(buf), "db-%02x%02x%02x",
             (unsigned)((mac >> 24) & 0xFF),
             (unsigned)((mac >> 16) & 0xFF),
             (unsigned)((mac >> 8) & 0xFF));
    s_id = buf;
    return s_id;
}

void begin() {
    const Config& c = cfg::get();
    if (!c.mqtt_enabled()) return;          // no broker: open nothing, say nothing

    const uint32_t heap = ESP.getFreeHeap();
    if (heap < MIN_FREE_HEAP) {
        Serial.printf("[mqtt] not starting: only %u bytes free, need %u. The board "
                      "keeps its screens; Home Assistant will not see it.\n",
                      (unsigned)heap, (unsigned)MIN_FREE_HEAP);
        return;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) { Serial.println("[mqtt] no mutex - not starting"); return; }

    // Populate s_id before handing its pointer over: deviceId() returns by
    // value, so `deviceId().c_str()` would point into a temporary that dies at
    // the end of the statement, leaving the client to send whatever was left in
    // that memory as our client id.
    deviceId();

    s_host = c.mqtt_host;
    s_user = c.mqtt_user;
    s_pass = c.mqtt_pass;
    s_base = c.mqtt_base_prefix() + "/" + s_id;
    s_availTopic = s_base + "/status";
    s_brightActual = (uint8_t)Config::pick(c.brightness, BRIGHTNESS, 0, 255);
    s_lightBright = s_brightActual;

    esp_mqtt_client_config_t mc = {};
    mc.host       = s_host.c_str();
    mc.port       = (uint32_t)c.mqtt_port_or_default();
    mc.transport  = MQTT_TRANSPORT_OVER_TCP;
    mc.client_id  = s_id.c_str();
    // An empty username means an anonymous broker, which is the common case on
    // a home network. Passing "" rather than nullptr makes esp-mqtt send an
    // empty credential, which some brokers reject outright.
    if (s_user.length()) mc.username = s_user.c_str();
    if (s_pass.length()) mc.password = s_pass.c_str();

    mc.lwt_topic  = s_availTopic.c_str();
    mc.lwt_msg    = "offline";
    mc.lwt_qos    = 1;
    mc.lwt_retain = 1;

    mc.keepalive            = 30;
    mc.reconnect_timeout_ms = 10000;
    // Trimmed from the 6144 default: this task parses small JSON commands and
    // publishes short payloads, and the CYD's DRAM is spoken for.
    mc.task_stack           = 4096;
    mc.buffer_size          = 1024;   // one discovery payload, not seven

    s_client = esp_mqtt_client_init(&mc);
    if (!s_client) {
        Serial.println("[mqtt] client init failed - carrying on without it");
        return;
    }
    esp_mqtt_client_register_event(s_client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID,
                                   onMqttEvent, nullptr);
    if (esp_mqtt_client_start(s_client) != ESP_OK) {
        Serial.println("[mqtt] client start failed - carrying on without it");
        s_client = nullptr;
        return;
    }
    xTaskCreatePinnedToCore(publisherTask, "ha-pub", 3072, nullptr, 1, nullptr, 0);
    Serial.printf("[mqtt] connecting to %s:%u, topics under %s\n",
                  s_host.c_str(), (unsigned)mc.port, s_base.c_str());
}

const char* statusWord() {
    if (!cfg::get().mqtt_enabled() || !s_client) return "off";
    if (s_authFailed) return "auth";
    return s_connected ? "up" : "down";
}

void setCurrentView(int viewId) {
    Lock l;
    if (s_view != viewId) { s_view = viewId; bumpEpoch(); }
}

void setScreenOn(bool on) {
    Lock l;
    if (s_screenOn != on) { s_screenOn = on; bumpEpoch(); }
}

void setBrightnessActual(uint8_t brightness) {
    Lock l;
    if (s_brightActual != brightness) { s_brightActual = brightness; bumpEpoch(); }
}

bool takeNextPress() {
    Lock l;
    bool v = s_nextLatch;
    s_nextLatch = false;
    return v;
}

bool takeForceRefresh() {
    Lock l;
    bool v = s_refreshLatch;
    s_refreshLatch = false;
    return v;
}

bool takeViewRequest(int& viewId) {
    Lock l;
    if (s_viewReq < 0) return false;
    viewId = s_viewReq;
    s_viewReq = -1;
    return true;
}

bool lightOverrideActive() { Lock l; return s_lightSet; }
bool lightOn()             { Lock l; return s_lightOn; }
uint8_t lightBrightness()  { Lock l; return s_lightBright; }

void clearLightOverride() {
    {
        Lock l;
        if (!s_lightSet) return;      // nothing to hand back
        s_lightSet = false;
        bumpEpoch();                  // tell Home Assistant what actually happened
    }
    Serial.println("[mqtt] local press - Home Assistant's screen override cleared");
}

}  // namespace ha
