// Home Assistant over MQTT — connection, availability and identity.
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

#include "ha.h"
#include "config.h"
#include "app_config.h"
#include "board.h"

#include <WiFi.h>
#include "mqtt_client.h"

namespace {

esp_mqtt_client_handle_t s_client = nullptr;

// esp-mqtt keeps the pointers it is given rather than copying the strings, so
// every one of these has to outlive begin(). File scope is that guarantee.
String s_host;
String s_user;
String s_pass;
String s_id;
String s_availTopic;

volatile bool s_connected  = false;
volatile bool s_authFailed = false;

// Below this, decline to start. An MQTT client that squeezes in with nothing to
// spare does not fail here — it fails hours later as a TLS handshake in the
// fetch task, and the departure board goes stale for reasons nobody connects to
// the feature added last month. The CYD has no PSRAM and is the board that
// makes this real.
constexpr uint32_t MIN_FREE_HEAP = 40000;

void onMqttEvent(void*, esp_event_base_t, int32_t id, void* data) {
    auto* ev = static_cast<esp_mqtt_event_handle_t>(data);

    switch ((esp_mqtt_event_id_t)id) {
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            s_authFailed = false;
            // Retained, so Home Assistant learns we are here even if it starts
            // after we do. The matching "offline" is the will the broker sends
            // on our behalf if we drop without saying goodbye.
            esp_mqtt_client_publish(s_client, s_availTopic.c_str(), "online", 0, 1, 1);
            Serial.printf("[mqtt] connected to %s:%d as %s (free heap %u)\n",
                          s_host.c_str(), cfg::get().mqtt_port_or_default(),
                          s_id.c_str(), (unsigned)ESP.getFreeHeap());
            break;

        case MQTT_EVENT_DISCONNECTED:
            if (s_connected) Serial.println("[mqtt] disconnected - reconnecting");
            s_connected = false;
            break;

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

}  // namespace

namespace ha {

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

    s_host = c.mqtt_host;
    s_user = c.mqtt_user;
    s_pass = c.mqtt_pass;
    s_availTopic = c.mqtt_base_prefix() + "/" + deviceId() + "/status";

    // Populate s_id before handing its pointer over: deviceId() returns by
    // value, so `deviceId().c_str()` would point into a temporary that dies at
    // the end of the statement, leaving the client to send whatever was left in
    // that memory as our client id.
    deviceId();

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
    Serial.printf("[mqtt] connecting to %s:%u, availability on %s\n",
                  s_host.c_str(), (unsigned)mc.port, s_availTopic.c_str());
}

const char* statusWord() {
    if (!cfg::get().mqtt_enabled() || !s_client) return "off";
    if (s_authFailed) return "auth";
    return s_connected ? "up" : "down";
}

}  // namespace ha
