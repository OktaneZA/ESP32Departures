// On-device configuration + USB-serial provisioning.
//
// Config lives in NVS so a single pre-built firmware binary serves every user;
// the installer sets it over serial. Line-based protocol (newline-terminated),
// all replies newline-terminated:
//
//   PING                 -> PONG Departure Buddy
//   CFG <key>=<value>    -> ACK <key>        (stages a value)
//   COMMIT               -> SAVED, then reboots into the new config
//   GET                  -> key=value lines (secrets masked), then END
//   SCAN                 -> WiFi networks the board can actually see, then END
//   HASH                 -> md5/size of the running firmware, then END
//
// Keys: ssid pass key dep dest plat tz bus busline river riverline rivername mode
//       bstart bend bright refr colfg coldim colwarn colbg dwtrain dwbus dwriver
//       dwclock dwwx wlat wlon wname nmode

#include "config.h"
#include "app_config.h"   // compile-time defaults
#include <Preferences.h>
#include <WiFi.h>

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
    c.bus_stop    = prefs.getString("bus",  "");
    c.bus_line    = prefs.getString("busln", "");
    c.river_pier  = prefs.getString("riv",   "");
    c.river_line  = prefs.getString("rivln", "");
    c.river_name  = prefs.getString("rivnm", "");
    c.mode        = prefs.getString("mode", "");   // "" = train+bus (pre-mode configs)
    c.blank_start = prefs.getInt("bstart", -1);
    c.blank_end   = prefs.getInt("bend",   -1);
    c.brightness  = prefs.getInt("bright", BRIGHTNESS);
    c.refresh     = prefs.getInt("refr",   REFRESH_SECONDS);
    // Appearance and dwell default to -1 ("not set") rather than to the real
    // values, so Config::pick() can tell "the user chose this" from "nobody ever
    // said", and a board provisioned before these existed keeps its old look.
    c.col_fg      = prefs.getInt("colfg",  -1);
    c.col_dim     = prefs.getInt("coldim", -1);
    c.col_warn    = prefs.getInt("colwarn", -1);
    c.col_bg      = prefs.getInt("colbg",  -1);
    c.dwell_train = prefs.getInt("dwtrain", -1);
    c.dwell_bus   = prefs.getInt("dwbus",  -1);
    c.dwell_river = prefs.getInt("dwriver", -1);
    c.dwell_clock = prefs.getInt("dwclock", -1);
    c.dwell_wx    = prefs.getInt("dwwx",   -1);
    c.wx_lat      = prefs.getInt("wlat",   INT32_MIN);
    c.wx_lon      = prefs.getInt("wlon",   INT32_MIN);
    c.wx_name     = prefs.getString("wname", "");
    c.night_mode  = prefs.getInt("nmode",  -1);
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
    else if (k == "bus")    g_stage.bus_stop   = v;
    else if (k == "busline") g_stage.bus_line  = v;
    else if (k == "river")  g_stage.river_pier = v;
    else if (k == "riverline") g_stage.river_line = v;
    else if (k == "rivername") g_stage.river_name = v;
    else if (k == "mode")   g_stage.mode       = v;
    else if (k == "bstart") g_stage.blank_start = v.toInt();
    else if (k == "bend")   g_stage.blank_end   = v.toInt();
    else if (k == "bright") g_stage.brightness  = v.toInt();
    else if (k == "refr")   g_stage.refresh     = v.toInt();
    else if (k == "colfg")  g_stage.col_fg      = v.toInt();
    else if (k == "coldim") g_stage.col_dim     = v.toInt();
    else if (k == "colwarn") g_stage.col_warn   = v.toInt();
    else if (k == "colbg")  g_stage.col_bg      = v.toInt();
    else if (k == "dwtrain") g_stage.dwell_train = v.toInt();
    else if (k == "dwbus")  g_stage.dwell_bus   = v.toInt();
    else if (k == "dwriver") g_stage.dwell_river = v.toInt();
    else if (k == "dwclock") g_stage.dwell_clock = v.toInt();
    else if (k == "dwwx")   g_stage.dwell_wx    = v.toInt();
    else if (k == "wlat")   g_stage.wx_lat      = v.toInt();
    else if (k == "wlon")   g_stage.wx_lon      = v.toInt();
    else if (k == "wname")  g_stage.wx_name     = v;
    else if (k == "nmode")  g_stage.night_mode  = v.toInt();
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
    prefs.putString("bus",  g_stage.bus_stop);
    prefs.putString("busln", g_stage.bus_line);
    prefs.putString("riv",   g_stage.river_pier);
    prefs.putString("rivln", g_stage.river_line);
    prefs.putString("rivnm", g_stage.river_name);
    prefs.putString("mode", g_stage.mode);
    prefs.putInt("bstart", g_stage.blank_start);
    prefs.putInt("bend",   g_stage.blank_end);
    prefs.putInt("bright", g_stage.brightness);
    prefs.putInt("refr",   g_stage.refresh);
    prefs.putInt("colfg",  g_stage.col_fg);
    prefs.putInt("coldim", g_stage.col_dim);
    prefs.putInt("colwarn", g_stage.col_warn);
    prefs.putInt("colbg",  g_stage.col_bg);
    prefs.putInt("dwtrain", g_stage.dwell_train);
    prefs.putInt("dwbus",  g_stage.dwell_bus);
    prefs.putInt("dwriver", g_stage.dwell_river);
    prefs.putInt("dwclock", g_stage.dwell_clock);
    prefs.putInt("dwwx",   g_stage.dwell_wx);
    prefs.putInt("wlat",   g_stage.wx_lat);
    prefs.putInt("wlon",   g_stage.wx_lon);
    prefs.putString("wname", g_stage.wx_name);
    prefs.putInt("nmode",  g_stage.night_mode);
    prefs.end();
    Serial.println("SAVED");
    Serial.flush();
    delay(300);
    ESP.restart();
}

// Short label for an AP's security mode, for the SCAN diagnostic.
const char* auth_name(int mode) {
    switch (mode) {
        case WIFI_AUTH_OPEN:            return "open";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ent";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
        case WIFI_AUTH_WAPI_PSK:        return "WAPI";
        default:                        return "?";
    }
}

// List every AP the radio can see. The ESP32-S3 has no 5 GHz radio, so a network
// that is missing here but visible on a phone is almost always 5 GHz-only - the
// single most common reason a board will not connect.
void scan_networks() {
    Serial.println("scanning...");
    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks();
    if (n <= 0) {
        Serial.println("no networks found");
    } else {
        for (int i = 0; i < n; ++i) {
            Serial.printf("%s|rssi=%d|ch=%d|auth=%s\n",
                          WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                          WiFi.channel(i), auth_name(WiFi.encryptionType(i)));
        }
    }
    WiFi.scanDelete();
    Serial.println("END");
}

void handle_line(String line) {
    line.trim();
    if (line.isEmpty()) return;

    if (line == "PING") {
        Serial.println("PONG Departure Buddy");
    } else if (line.startsWith("CFG ")) {
        stage_kv(line.substring(4));
    } else if (line == "COMMIT") {
        commit_and_reboot();  // does not return
    } else if (line == "SCAN") {
        scan_networks();
    } else if (line == "HASH") {
        // Lets a user prove the board is running the firmware that was
        // published, rather than taking it on trust. getSketchMD5() hashes
        // exactly the image the flasher wrote -- getSketchSize() returns the
        // image length, not the free space -- so this is directly comparable
        // to the md5 of firmware.bin in the release manifest.
        Serial.print("md5=");  Serial.println(ESP.getSketchMD5());
        Serial.print("size="); Serial.println(ESP.getSketchSize());
        Serial.println("END");
    } else if (line == "GET") {
        Serial.print("dep=");    Serial.println(g_cfg.dep_crs);
        Serial.print("dest=");   Serial.println(g_cfg.dest_crs);
        Serial.print("plat=");   Serial.println(g_cfg.platform);
        Serial.print("bus=");    Serial.println(g_cfg.bus_stop);
        Serial.print("busline="); Serial.println(g_cfg.bus_line);
        Serial.print("river=");  Serial.println(g_cfg.river_pier);
        Serial.print("riverline="); Serial.println(g_cfg.river_line);
        Serial.print("rivername="); Serial.println(g_cfg.river_name);
        // Report the legacy word as the set it means, so the installer only
        // ever has to understand the comma-separated form.
        Serial.print("mode=");   Serial.println(
            (g_cfg.mode.isEmpty() || g_cfg.mode == "both") ? String("train,bus") : g_cfg.mode);
        Serial.print("ssid=");   Serial.println(g_cfg.wifi_ssid);
        // Length only, never the password itself - enough to tell an empty or
        // truncated password from a wrong one without leaking it.
        Serial.print("passlen="); Serial.println(g_cfg.wifi_pass.length());
        Serial.print("bstart="); Serial.println(g_cfg.blank_start);
        Serial.print("bend=");   Serial.println(g_cfg.blank_end);
        Serial.print("bright="); Serial.println(g_cfg.brightness);
        Serial.print("refr=");   Serial.println(g_cfg.refresh);
        Serial.print("colfg=");  Serial.println(g_cfg.col_fg);
        Serial.print("coldim="); Serial.println(g_cfg.col_dim);
        Serial.print("colwarn="); Serial.println(g_cfg.col_warn);
        Serial.print("colbg=");  Serial.println(g_cfg.col_bg);
        Serial.print("dwtrain="); Serial.println(g_cfg.dwell_train);
        Serial.print("dwbus=");  Serial.println(g_cfg.dwell_bus);
        Serial.print("dwriver="); Serial.println(g_cfg.dwell_river);
        Serial.print("dwclock="); Serial.println(g_cfg.dwell_clock);
        Serial.print("dwwx=");   Serial.println(g_cfg.dwell_wx);
        Serial.print("wlat=");   Serial.println(g_cfg.wx_lat);
        Serial.print("wlon=");   Serial.println(g_cfg.wx_lon);
        Serial.print("wname=");  Serial.println(g_cfg.wx_name);
        Serial.print("nmode=");  Serial.println(g_cfg.night_mode);
        Serial.print("md5=");    Serial.println(ESP.getSketchMD5());
        Serial.print("size=");   Serial.println(ESP.getSketchSize());
        Serial.print("wifi=");   Serial.println(WiFi.status() == WL_CONNECTED ? "up" : "down");
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
