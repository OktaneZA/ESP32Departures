// On-device configuration + USB-serial provisioning.
//
// Config lives in NVS so a single pre-built firmware binary serves every user;
// the installer sets it over serial. Line-based protocol (newline-terminated),
// all replies newline-terminated:
//
//   PING                 -> PONG Esp32Departures
//   CFG <key>=<value>    -> ACK <key>        (stages a value)
//   COMMIT               -> SAVED, then reboots into the new config
//   GET                  -> key=value lines (secrets masked), then END
//
// Keys: ssid pass key dep dest plat bstart bend bright refr

#include "config.h"
#include "app_config.h"   // compile-time defaults
#include <Preferences.h>

namespace {

Preferences prefs;
Config g_cfg;
Config g_stage;            // staged values received during provisioning
bool   g_have_stage = false;
String g_line;

constexpr const char* NS = "esp32dep";

void load_from_nvs(Config& c) {
    prefs.begin(NS, true);
    c.wifi_ssid   = prefs.getString("ssid", "");
    c.wifi_pass   = prefs.getString("pass", "");
    c.api_key     = prefs.getString("key",  "");
    c.dep_crs     = prefs.getString("dep",  "");
    c.dest_crs    = prefs.getString("dest", "");
    c.platform    = prefs.getString("plat", "");
    c.tz          = prefs.getString("tz",   "");
    c.blank_start = prefs.getInt("bstart", -1);
    c.blank_end   = prefs.getInt("bend",   -1);
    c.brightness  = prefs.getInt("bright", BRIGHTNESS);
    c.refresh     = prefs.getInt("refr",   REFRESH_SECONDS);
    prefs.end();
}

void stage_kv(const String& kv) {
    int eq = kv.indexOf('=');
    if (eq < 0) { Serial.println("ERR no '='"); return; }
    String k = kv.substring(0, eq);
    String v = kv.substring(eq + 1);
    if (!g_have_stage) { g_stage = g_cfg; g_have_stage = true; }

    if      (k == "ssid")   g_stage.wifi_ssid  = v;
    else if (k == "pass")   g_stage.wifi_pass  = v;
    else if (k == "key")    g_stage.api_key    = v;
    else if (k == "dep")    g_stage.dep_crs    = v;
    else if (k == "dest")   g_stage.dest_crs   = v;
    else if (k == "plat")   g_stage.platform   = v;
    else if (k == "tz")     g_stage.tz         = v;
    else if (k == "bstart") g_stage.blank_start = v.toInt();
    else if (k == "bend")   g_stage.blank_end   = v.toInt();
    else if (k == "bright") g_stage.brightness  = v.toInt();
    else if (k == "refr")   g_stage.refresh     = v.toInt();
    else { Serial.print("ERR key "); Serial.println(k); return; }

    Serial.print("ACK "); Serial.println(k);
}

void commit_and_reboot() {
    if (!g_have_stage) g_stage = g_cfg;
    prefs.begin(NS, false);
    prefs.putString("ssid", g_stage.wifi_ssid);
    prefs.putString("pass", g_stage.wifi_pass);
    prefs.putString("key",  g_stage.api_key);
    prefs.putString("dep",  g_stage.dep_crs);
    prefs.putString("dest", g_stage.dest_crs);
    prefs.putString("plat", g_stage.platform);
    prefs.putString("tz",   g_stage.tz);
    prefs.putInt("bstart", g_stage.blank_start);
    prefs.putInt("bend",   g_stage.blank_end);
    prefs.putInt("bright", g_stage.brightness);
    prefs.putInt("refr",   g_stage.refresh);
    prefs.end();
    Serial.println("SAVED");
    Serial.flush();
    delay(300);
    ESP.restart();
}

void handle_line(String line) {
    line.trim();
    if (line.isEmpty()) return;

    if (line == "PING") {
        Serial.println("PONG Esp32Departures");
    } else if (line.startsWith("CFG ")) {
        stage_kv(line.substring(4));
    } else if (line == "COMMIT") {
        commit_and_reboot();  // does not return
    } else if (line == "GET") {
        Serial.print("dep=");    Serial.println(g_cfg.dep_crs);
        Serial.print("dest=");   Serial.println(g_cfg.dest_crs);
        Serial.print("plat=");   Serial.println(g_cfg.platform);
        Serial.print("ssid=");   Serial.println(g_cfg.wifi_ssid);
        Serial.print("bright="); Serial.println(g_cfg.brightness);
        Serial.print("prov=");   Serial.println(g_cfg.provisioned() ? 1 : 0);
        Serial.println("END");
    } else {
        Serial.print("ERR cmd "); Serial.println(line);
    }
}

}  // namespace

namespace cfg {

void load() { load_from_nvs(g_cfg); }

Config& get() { return g_cfg; }

bool poll_serial() {
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (g_line.length()) {
                String l = g_line;
                g_line = "";
                handle_line(l);
            }
        } else {
            g_line += c;
            if (g_line.length() > 250) g_line = "";  // overflow guard
        }
    }
    return false;
}

}  // namespace cfg
