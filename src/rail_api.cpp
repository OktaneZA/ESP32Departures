// National Rail LDBWS client — Rail Data Marketplace REST/JSON API.
//
// Uses the official National Rail "Live Departure Board" product on
// https://raildata.org.uk (free open tier). This is the same Darwin data the
// Python app consumed via SOAP/OpenLDBWS, but the marketplace now exposes it as
// REST + JSON, which parses cleanly on-device with ArduinoJson.
//
// Endpoint (GetDepBoardWithDetails returns calling points in a single call):
//   GET {BASE}/GetDepBoardWithDetails/{CRS}?numRows=..&timeWindow=..[&filterCrs=..&filterType=to]
//   Header: x-apikey: <your consumer key>
//
// The JSON is a serialization of the SOAP schema. Two per-service shapes are
// version/serializer-dependent — `destination` and `subsequentCallingPoints` —
// so those are parsed defensively below. Set RAW_JSON_DEBUG in app_config.h to
// dump the first response and confirm the field names against your account.

#include "rail_api.h"
#include "app_config.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "tls.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <vector>

namespace {

// Marketplace gateway base + product route. If your subscription's "API" tab
// shows a different route segment, update this line to match.
constexpr char kBase[] =
    "https://api1.raildata.org.uk/1010-live-departure-board-dep1_2/LDBWS/api/20220120";
constexpr char kOperation[] = "GetDepBoardWithDetails";  // has calling points

// Route large JSON documents into PSRAM so a full WithDetails board (calling
// points for every service) never pressures the ~200 KB internal heap.
struct SpiRamAllocator : ArduinoJson::Allocator {
    void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
    void  deallocate(void* p) override { heap_caps_free(p); }
    void* reallocate(void* p, size_t n) override {
        return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM);
    }
};

// Join calling-point names as "A, B and C" (matches the Python board's phrasing).
String joinCalling(const std::vector<String>& names) {
    if (names.empty()) return String("");
    if (names.size() == 1) return names[0];
    String out;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i == names.size() - 1) out += " and ";
        else if (i > 0) out += ", ";
        out += names[i];
    }
    return out;
}

// destination may serialize as {"location":[{locationName}]}, [{locationName}],
// or {locationName}. Handle all three; join splits (multiple destinations) with " & ".
String parseDestination(JsonObjectConst svc) {
    JsonVariantConst dest = svc["destination"];

    JsonArrayConst arr;
    if (dest["location"].is<JsonArrayConst>()) arr = dest["location"].as<JsonArrayConst>();  // {"location":[...]}
    else if (dest.is<JsonArrayConst>())        arr = dest.as<JsonArrayConst>();              // [...]
    if (!arr.isNull()) {
        String out;
        for (JsonObjectConst l : arr) {
            const char* n = l["locationName"] | "";
            if (!n[0]) continue;
            if (out.length()) out += " & ";
            out += n;
        }
        if (out.length()) return out;
    }
    const char* single = dest["locationName"] | "";  // {"locationName":...}
    return single[0] ? String(single) : String("?");
}

// subsequentCallingPoints may be an array of {callingPoint:[...]} sections, or an
// object wrapping {callingPointList: ...}. Return the first section's stops.
String parseCallingPoints(JsonObjectConst svc) {
    JsonVariantConst scp = svc["subsequentCallingPoints"];
    JsonArrayConst cps;

    if (scp.is<JsonArrayConst>()) {
        cps = scp[0]["callingPoint"].as<JsonArrayConst>();          // [{callingPoint:[...]}]
    } else {
        JsonVariantConst cpl = scp["callingPointList"];
        if (cpl.is<JsonArrayConst>()) cps = cpl[0]["callingPoint"].as<JsonArrayConst>();
        else                          cps = cpl["callingPoint"].as<JsonArrayConst>();
    }
    if (cps.isNull()) return String("");

    std::vector<String> names;
    for (JsonObjectConst cp : cps) {
        const char* n = cp["locationName"] | "";
        if (n[0]) names.push_back(String(n));
    }
    return joinCalling(names);
}

}  // namespace

namespace rail {

Fetch fetchDepartures(const Config& cfg, std::vector<Departure>& out,
                      String& stationName, String& callingAt) {
    if (WiFi.status() != WL_CONNECTED) return Fetch::Failed;
    if (cfg.dep_crs.isEmpty() || cfg.api_key.isEmpty()) return Fetch::Failed;

    // Build the request URL with query parameters.
    String url = String(kBase) + "/" + kOperation + "/" + cfg.dep_crs +
                 "?numRows=" + String(FETCH_ROWS) +
                 "&timeWindow=" + String(LDBWS_TIME_WINDOW);
    if (!cfg.dest_crs.isEmpty()) {
        url += "&filterCrs=" + cfg.dest_crs + "&filterType=to";
    }

    // TLS: verified against the embedded Mozilla root store (see tls.h).
    WiFiClientSecure client;
    net::trustRoots(client);

    HTTPClient http;
    http.setTimeout(15000);
    http.setConnectTimeout(15000);
    if (!http.begin(client, url)) return Fetch::Failed;
    http.addHeader("x-apikey", cfg.api_key);
    http.addHeader("User-Agent", "DepartureBuddy/1.0");
    http.useHTTP10(true);  // plain HTTP/1.0 body — friendliest for ArduinoJson streams

    int code = http.GET();
    if (code != 200) {
        Serial.printf("[rail] HTTP %d\n", code);
        http.end();
        // 400 "Invalid crs code supplied" is a config error, not a transient one.
        return code == 400 ? Fetch::BadStation : Fetch::Failed;
    }

    // Keep only the fields we render; `true` on subsequentCallingPoints/destination
    // keeps their whole subtree so the defensive parsers above see every shape.
    JsonDocument filter;
    filter["locationName"] = true;
    JsonObject fsvc = filter["trainServices"].add<JsonObject>();
    fsvc["std"] = true;
    fsvc["etd"] = true;
    fsvc["platform"] = true;
    fsvc["operator"] = true;
    fsvc["isCancelled"] = true;
    fsvc["destination"] = true;
    fsvc["subsequentCallingPoints"] = true;

    SpiRamAllocator allocator;
    JsonDocument doc(&allocator);
    DeserializationError err;

#if RAW_JSON_DEBUG
    // One-time verification aid: print the full body, then parse it.
    String body = http.getString();
    Serial.println("[rail] ---- raw response ----");
    Serial.println(body);
    Serial.println("[rail] ----------------------");
    err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
#else
    err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
#endif
    http.end();
    if (err) {
        Serial.printf("[rail] JSON parse error: %s\n", err.c_str());
        return Fetch::Failed;
    }

    std::vector<Departure> parsed;
    JsonArrayConst services = doc["trainServices"].as<JsonArrayConst>();
    for (JsonObjectConst s : services) {
        if ((int)parsed.size() >= MAX_DEPARTURES) break;

        const char* std = s["std"] | "";
        if (!std[0]) continue;  // no scheduled departure time — skip

        // Platform filter (client-side; the API has no server-side platform filter).
        const char* plat = s["platform"] | "";
        if (!cfg.platform.isEmpty() && cfg.platform != plat) continue;

        const char* etd = s["etd"] | "On time";
        bool isCancelled = (s["isCancelled"] | false) || strcmp(etd, "Cancelled") == 0;

        Departure dep;
        dep.aimed = std;                      // LDBWS times are already "HH:MM"
        dep.platform = plat;
        dep.destination = parseDestination(s);
        dep.cancelled = isCancelled;

        if (isCancelled) {
            dep.status = "Cancelled";
        } else if (strcmp(etd, "On time") == 0 || strcmp(etd, std) == 0) {
            dep.status = "On time";
        } else if (strcmp(etd, "Delayed") == 0) {
            dep.status = "Delayed";
        } else {
            dep.status = "Exp " + String(etd);  // etd is an "HH:MM" estimate
        }
        parsed.push_back(dep);
    }

    String station = doc["locationName"] | cfg.dep_crs.c_str();
    String calling;
    if (FETCH_CALLING_POINTS && !services.isNull() && services.size() > 0) {
        calling = parseCallingPoints(services[0].as<JsonObjectConst>());
    }

    // Commit outputs only after a fully successful parse.
    out = parsed;
    stationName = station;
    callingAt = calling;
    return Fetch::Ok;
}

}  // namespace rail
