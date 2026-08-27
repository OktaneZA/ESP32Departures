// TfL River Bus arrivals client — the Unified API's live prediction feed.
//
// Uber Boat by Thames Clippers (RB1, RB4, RB6) and the Woolwich Ferry run as
// TfL "river-bus" services, so their sailings are published as ordinary
// predictions on the open Unified API:
//
//   GET https://api.tfl.gov.uk/StopPoint/{naptan}/Arrivals
//
// No key, no registration. A pier is identified by its Naptan ID — the *port*
// ("930GCAW", Canary Wharf), not one of the individual berths ("9300CAW1"),
// because the port aggregates both directions onto one board.
//
// Unlike the bus feed's line-per-record URA format this is ordinary JSON: an
// array of Prediction objects, the same shape the rest of the Unified API uses.
// Only five fields per record matter here, so an ArduinoJson filter discards
// the rest during parsing rather than after — a pier's response is ~6 KB of
// which the board keeps a few hundred bytes.
//
// `timeToStation` is already relative ("seconds from now" at the moment TfL
// answered), so unlike the rail feed this needs no clock on the device to be
// correct — the countdown is right even before NTP has synced.

#include "river_api.h"
#include "app_config.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <algorithm>
#include <vector>

namespace {

constexpr char kBase[] = "https://api.tfl.gov.uk/StopPoint/";

// URL-escape everything outside the unreserved set, so a mistyped pier ID can
// never break out of the path it is interpolated into.
String escapePath(const String& in) {
    String out;
    for (size_t i = 0; i < in.length(); ++i) {
        char c = in[i];
        bool safe = isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) {
            out += c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
            out += buf;
        }
    }
    return out;
}

// Case-insensitive equality, so a pier filter typed as "rb1" matches TfL's "RB1".
bool equalsIgnoreCase(const String& a, const String& b) {
    if (a.length() != b.length()) return false;
    for (size_t i = 0; i < a.length(); ++i) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    }
    return true;
}

}  // namespace

namespace river {

Fetch fetchArrivals(const Config& cfg, std::vector<RiverArrival>& out, String& pierName) {
    if (WiFi.status() != WL_CONNECTED) return Fetch::Failed;
    if (cfg.river_pier.isEmpty()) return Fetch::Failed;

    String url = String(kBase) + escapePath(cfg.river_pier) + "/Arrivals";

    // TLS: as with the rail and bus clients, setInsecure() encrypts without
    // authenticating the server. Swap in setCACert() with api.tfl.gov.uk's root
    // CA to harden. No credentials travel on this request — the feed is open.
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setTimeout(15000);
    http.setConnectTimeout(15000);
    if (!http.begin(client, url)) return Fetch::Failed;
    http.addHeader("User-Agent", "DepartureBuddy/1.0");
    http.useHTTP10(true);   // plain unchunked body the reader below can bound

    int code = http.GET();
    if (code != 200) {
        Serial.printf("[river] HTTP %d\n", code);
        http.end();
        // 404 "stop point not recognised" is a config error, not a transient one.
        return code == 404 ? Fetch::BadPier : Fetch::Failed;
    }

    // Read the body into a bounded buffer rather than parsing straight off the
    // socket: ArduinoJson pulls one byte per read() and each of those is a call
    // into lwip, which on a 6 KB document is far slower than one bulk read.
    // The cap is the same guarantee the bus client makes — an unexpectedly huge
    // response is discarded, never allowed to exhaust the heap.
    WiFiClient& stream = http.getStream();
    String body;
    body.reserve(8192);
    const uint32_t deadline = millis() + 15000;
    bool overflow = false;
    uint8_t buf[512];
    while (millis() < deadline) {
        int avail = stream.available();
        if (avail <= 0) {
            if (!stream.connected()) break;   // socket closed: body complete
            delay(5);
            continue;
        }
        int n = stream.readBytes(buf, avail > (int)sizeof(buf) ? (int)sizeof(buf) : avail);
        if (n <= 0) continue;
        if (body.length() + n > RIVER_MAX_RESPONSE) { overflow = true; break; }
        body.concat((const char*)buf, n);
    }
    http.end();

    if (overflow) {
        Serial.printf("[river] response over %d bytes - discarded\n", RIVER_MAX_RESPONSE);
        return Fetch::Failed;
    }
    if (body.isEmpty()) {
        Serial.println("[river] empty response");
        return Fetch::Failed;
    }

    // Keep only the five fields the screen needs. Applied during parsing, so the
    // ~40 other fields per prediction are never allocated.
    JsonDocument filter;
    {
        JsonObject f = filter.add<JsonObject>();
        f["lineName"] = true;
        f["destinationName"] = true;
        f["stationName"] = true;
        f["timeToStation"] = true;
        f["vehicleId"] = true;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
    body = String();   // release the buffer before building the output
    if (err) {
        Serial.printf("[river] parse failed: %s\n", err.c_str());
        return Fetch::Failed;
    }

    JsonArrayConst preds = doc.as<JsonArrayConst>();
    if (preds.isNull()) {
        Serial.println("[river] response was not a prediction array");
        return Fetch::Failed;
    }

    String name;
    std::vector<RiverArrival> parsed;
    std::vector<String> seen;   // vehicleIds already taken, to drop duplicates

    for (JsonObjectConst p : preds) {
        String line = (const char*)(p["lineName"] | "");
        if (line.isEmpty()) continue;
        if (!cfg.river_line.isEmpty() && !equalsIgnoreCase(line, cfg.river_line)) continue;

        int32_t eta = p["timeToStation"] | (int32_t)-1;
        if (eta < 0) continue;
        if (eta > RIVER_MAX_ETA_MINUTES * 60) continue;   // beyond the window

        // TfL occasionally repeats a sailing across the two berths of one pier;
        // on a three-row screen a duplicate costs a real departure its slot.
        String vehicle = (const char*)(p["vehicleId"] | "");
        if (vehicle.length()) {
            if (std::find(seen.begin(), seen.end(), vehicle) != seen.end()) continue;
            seen.push_back(vehicle);
        }

        if (name.isEmpty()) name = (const char*)(p["stationName"] | "");

        RiverArrival ar;
        ar.line = line;
        ar.destination = (const char*)(p["destinationName"] | "");
        ar.etaSeconds = eta;
        parsed.push_back(ar);

#if RAW_RIVER_DEBUG
        Serial.printf("[river] %s -> %s in %ds\n",
                      ar.line.c_str(), ar.destination.c_str(), (int)ar.etaSeconds);
#endif
    }

    // The feed returns predictions unordered; the board wants soonest first.
    std::sort(parsed.begin(), parsed.end(),
              [](const RiverArrival& a, const RiverArrival& b) {
                  return a.etaSeconds < b.etaSeconds;
              });
    if (parsed.size() > MAX_RIVER_ARRIVALS) parsed.resize(MAX_RIVER_ARRIVALS);

    // Commit outputs only after a fully successful read. Zero sailings is a
    // valid answer (nothing due before the window closes), not a failure.
    // TfL only names the pier inside a prediction, so a pier with nothing due
    // identifies itself only by the ID in the URL. The installer already knew
    // the friendly name when the user picked it from the list, so that is the
    // fallback rather than showing a raw Naptan on an otherwise quiet screen.
    out = parsed;
    if (name.isEmpty()) name = cfg.river_name;
    pierName = name.length() ? name : cfg.river_pier;
    return Fetch::Ok;
}

}  // namespace river
