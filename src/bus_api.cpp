// TfL Live Bus & River Bus Arrivals client — the Countdown "URA" instant feed.
//
// Documented in TfL's "Live Bus & River Bus Arrivals API" interface spec
// (https://content.tfl.gov.uk/tfl-live-bus-river-bus-arrivals-api-documentation.pdf).
// Unlike the rail feed this one is open: no key, no registration, just a stop.
//
//   GET {BASE}?StopCode1={SMS code}&StopAlso=True[&LineName={route}]&ReturnList=...
//
// The response is deliberately *not* standard JSON: it is one JSON array per
// line, which is why it is parsed line-by-line below rather than as a document.
// Each line's first element is the array type — 4 = URA version (always first,
// and carries the server timestamp), 1 = prediction, 0 = stop, 2 = message.
//
// Crucially, fields come back in the *documented sequence order*, not the order
// they were asked for in ReturnList. With the ReturnList used here a prediction
// line is:
//
//   [1, StopPointName, LineName, DestinationText, EstimatedTime, ExpireTime]
//
// Times are absolute Unix epoch in milliseconds (UTC). They are converted to a
// countdown against the URA version array's own timestamp, so the arrival times
// stay right even if the board's clock is off.

#include "bus_api.h"
#include "app_config.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "tls.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <algorithm>
#include <vector>

namespace {

constexpr char kBase[] =
    "https://countdown.api.tfl.gov.uk/interfaces/ura/instant_V1";

// Only the fields the bus screen renders. TfL asks clients to keep this list to
// the minimum, and requesting ExpireTime alongside EstimatedTime is a documented
// requirement of the feed.
constexpr char kReturnList[] =
    "StopPointName,LineName,DestinationText,EstimatedTime,ExpireTime";

// Array types, from the interface spec (section 4.2).
constexpr int kTypeStop       = 0;
constexpr int kTypePrediction = 1;
constexpr int kTypeUraVersion = 4;

constexpr size_t kMaxLine = 320;   // longest sane prediction line, plus slack

// Read one newline-terminated line from `s` into `line`, bounded in both length
// and time. Returns false when the response is over (connection closed and
// drained) or the deadline passed. Over-long lines are truncated rather than
// grown, so a malformed response can never exhaust the heap.
bool readLine(WiFiClient& s, String& line, uint32_t deadline) {
    line = "";
    bool any = false;
    while (millis() < deadline) {
        if (!s.available()) {
            if (!s.connected()) return any;   // socket closed: last line, if any
            delay(5);
            continue;
        }
        char c = (char)s.read();
        any = true;
        if (c == '\n') return true;
        if (c == '\r') continue;
        if (line.length() < kMaxLine) line += c;
    }
    return false;   // timed out
}

// URL-escape the characters TfL's parameter syntax reserves, so a route name
// with a space or ampersand cannot break out of the query string.
String escapeParam(const String& in) {
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

}  // namespace

namespace bus {

// Pick the provider. Anything but "national" is TfL, so a board provisioned
// before providers existed keeps the London feed without being told to.
Fetch fetchArrivals(const Config& cfg, std::vector<BusArrival>& out, String& stopName) {
    return cfg.bus_national() ? fetchNational(cfg, out, stopName)
                              : fetchTfl(cfg, out, stopName);
}

Fetch fetchTfl(const Config& cfg, std::vector<BusArrival>& out, String& stopName) {
    if (WiFi.status() != WL_CONNECTED) return Fetch::Failed;
    if (cfg.bus_stop.isEmpty()) return Fetch::Failed;

    // StopAlso=True makes the feed return a Stop array for the stop even when it
    // has no predictions, which is the only way to learn the stop's name on a
    // quiet evening - without it the "No buses due" screen has nothing to show
    // but the bare code.
    String url = String(kBase) + "?StopCode1=" + escapeParam(cfg.bus_stop) +
                 "&StopAlso=True&ReturnList=" + kReturnList;
    if (!cfg.bus_line.isEmpty()) {
        url += "&LineName=" + escapeParam(cfg.bus_line);
    }

    // TLS: verified against the embedded Mozilla root store (see tls.h). No
    // credentials travel on this request — the feed is unauthenticated.
    WiFiClientSecure client;
    net::trustRoots(client);

    HTTPClient http;
    http.setTimeout(15000);
    http.setConnectTimeout(15000);
    if (!http.begin(client, url)) return Fetch::Failed;
    http.addHeader("User-Agent", "DepartureBuddy/1.0");
    // HTTP/1.0 makes the server answer with a plain, unchunked body it closes at
    // the end — exactly what the bounded line reader below wants.
    http.useHTTP10(true);

    int code = http.GET();
    if (code != 200) {
        Serial.printf("[bus] HTTP %d\n", code);
        http.end();
        // 416 "Stop code unknown" is a config error, not a transient one.
        return code == 416 ? Fetch::BadStop : Fetch::Failed;
    }

    WiFiClient& stream = http.getStream();
    const uint32_t deadline = millis() + 15000;

    int64_t serverNowMs = 0;      // from the URA version array (first line)
    size_t  bytesRead = 0;
    String  name, line;
    std::vector<BusArrival> parsed;
    bool overflow = false;

    while (readLine(stream, line, deadline)) {
        bytesRead += line.length() + 1;
        if (bytesRead > BUS_MAX_RESPONSE) { overflow = true; break; }
        if (line.length() < 3) continue;      // blank / heartbeat line

#if RAW_BUS_DEBUG
        Serial.print("[bus] "); Serial.println(line);
#endif

        JsonDocument doc;
        if (deserializeJson(doc, line)) continue;   // skip a line we can't read
        JsonArrayConst a = doc.as<JsonArrayConst>();
        // A Stop array is only [type, name], so the minimum useful line is two
        // elements; each type checks its own length before indexing further.
        if (a.isNull() || a.size() < 2) continue;

        int type = a[0] | -1;
        if (type == kTypeUraVersion) {
            if (a.size() < 3) continue;
            serverNowMs = a[2] | (int64_t)0;
            continue;
        }
        if (type == kTypeStop) {
            if (name.isEmpty()) name = (const char*)(a[1] | "");
            continue;
        }
        if (type != kTypePrediction || a.size() < 6) continue;

        if (name.isEmpty()) name = (const char*)(a[1] | "");

        int64_t etaMs = a[4] | (int64_t)0;
        if (etaMs <= 0 || serverNowMs <= 0) continue;   // no usable timestamp

        int32_t eta = (int32_t)((etaMs - serverNowMs) / 1000);
        if (eta < 0) eta = 0;                            // already at the stop
        if (eta > BUS_MAX_ETA_MINUTES * 60) continue;    // beyond the window

        BusArrival ar;
        ar.line = (const char*)(a[2] | "");
        ar.destination = (const char*)(a[3] | "");
        ar.etaSeconds = eta;
        if (ar.line.isEmpty()) continue;
        parsed.push_back(ar);
    }
    http.end();

    if (overflow) {
        Serial.printf("[bus] response over %d bytes - discarded\n", BUS_MAX_RESPONSE);
        return Fetch::Failed;
    }
    if (serverNowMs <= 0) {
        Serial.println("[bus] no URA version array in response");
        return Fetch::Failed;   // truncated or unparseable response
    }

    // The feed returns predictions unordered; the board wants soonest first.
    std::sort(parsed.begin(), parsed.end(),
              [](const BusArrival& a, const BusArrival& b) {
                  return a.etaSeconds < b.etaSeconds;
              });
    if (parsed.size() > MAX_BUS_ARRIVALS) parsed.resize(MAX_BUS_ARRIVALS);

    // Commit outputs only after a fully successful read. Zero arrivals is a
    // valid answer (nothing due in the next 30 minutes), not a failure.
    out = parsed;
    stopName = name.length() ? name : cfg.bus_stop;
    return Fetch::Ok;
}

}  // namespace bus
