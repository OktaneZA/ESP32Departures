// TransportAPI bus client — live departures for stops outside London.
//
// TfL's Countdown feed (bus_api.cpp) covers London only. The rest of the UK has
// no free, keyless, per-stop arrivals API; TransportAPI is the one source in the
// right shape, and it charges by the day rather than by the second, which is why
// polling here is paced from Config::bus_budget (BUS-18).
//
//   GET https://transportapi.com/v3/uk/bus/stop_timetables/{ATCO}.json
//         ?app_id=..&app_key=..&group=false&live=true&limit=..
//
// Three things about that URL are deliberate:
//
//   * `stop_timetables` is the canonical endpoint. The older
//     `/bus/stop/{atco}/live.json` still works but answers 301, and following a
//     redirect would spend two requests out of a thirty-a-day allowance.
//   * `group=false` returns one flat `all` array instead of an object keyed by
//     route. Grouped, a two-route stop came back as 17.8 KB; flat and limited it
//     is under 5 KB, comfortably inside BUS_MAX_RESPONSE.
//   * `limit` is small because only MAX_BUS_ARRIVALS are ever drawn.
//
// The response carries `request_time`, the server's own clock, so countdowns are
// measured against it rather than the board's — the same trick the TfL client
// uses with the URA timestamp.

#include "bus_api.h"
#include "app_config.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "tls.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <algorithm>

namespace {

constexpr char kBase[] = "https://transportapi.com/v3/uk/bus/stop_timetables/";

// Percent-encode everything that is not unreserved. The ATCO code and the
// credentials all land in the query string, so none of them may be able to
// introduce a parameter of their own.
String escapeParam(const String& in) {
    String out;
    for (size_t i = 0; i < in.length(); ++i) {
        char c = in[i];
        bool safe = isalnum((unsigned char)c) || c == '-' || c == '_' ||
                    c == '.' || c == '~';
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

// Minutes since midnight from "HH:MM" (the whole of a departure time) or from
// the "…T14:19:43+01:00" of `request_time`. Returns -1 if it does not parse.
// Only the local wall clock matters: every time in one response shares the same
// offset, so the offset itself never has to be interpreted.
int minutesOfDay(const char* s, int offset) {
    if (!s) return -1;
    size_t len = strlen(s);
    if (len < (size_t)offset + 5) return -1;
    const char* p = s + offset;
    if (!isdigit((unsigned char)p[0]) || !isdigit((unsigned char)p[1]) ||
        p[2] != ':' ||
        !isdigit((unsigned char)p[3]) || !isdigit((unsigned char)p[4])) return -1;
    int h = (p[0] - '0') * 10 + (p[1] - '0');
    int m = (p[3] - '0') * 10 + (p[4] - '0');
    if (h > 23 || m > 59) return -1;
    return h * 60 + m;
}

}  // namespace

namespace bus {

Fetch fetchNational(const Config& cfg, std::vector<BusArrival>& out, String& stopName) {
    if (WiFi.status() != WL_CONNECTED) return Fetch::Failed;
    if (cfg.bus_stop.isEmpty() || cfg.bus_id.isEmpty() || cfg.bus_key.isEmpty())
        return Fetch::Failed;

    // Ask for a few more than are drawn: a route filter, a cancellation or a
    // departure that has already gone can each discard one.
    String url = String(kBase) + escapeParam(cfg.bus_stop) + ".json"
                 "?app_id=" + escapeParam(cfg.bus_id) +
                 "&app_key=" + escapeParam(cfg.bus_key) +
                 "&group=false&live=true&limit=" + String(MAX_BUS_ARRIVALS * 3);

    // TLS: verified against the embedded Mozilla root store (see tls.h). This
    // request carries credentials, so an unauthenticated server is not merely
    // untidy here — it would be handed the user's API key.
    WiFiClientSecure client;
    net::trustRoots(client);

    HTTPClient http;
    http.setTimeout(15000);
    http.setConnectTimeout(15000);
    // Never follow a redirect: the canonical URL is pinned above, and a redirect
    // to somewhere else would forward the credentials with it.
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    if (!http.begin(client, url)) return Fetch::Failed;
    http.addHeader("User-Agent", "DepartureBuddy/1.0");

    int code = http.GET();
    if (code != 200) {
        // Deliberately not logging the body: the error text quotes the app_id
        // back, and the serial log is the one place a key must not appear.
        Serial.printf("[bus/national] HTTP %d\n", code);
        http.end();
        // 401/403 is a rejected or out-of-quota key and 404 an unknown stop.
        // None of those fix themselves, so all are treated as config errors
        // rather than being retried at the failure cadence.
        if (code == 401 || code == 403 || code == 404) { return Fetch::BadStop; }
        return Fetch::Failed;
    }

    if (http.getSize() > (int)BUS_MAX_RESPONSE) {
        Serial.printf("[bus/national] response over %d bytes - discarded\n", BUS_MAX_RESPONSE);
        http.end();
        return Fetch::Failed;
    }

    // Only the fields actually rendered are kept. Ungrouped, `departures` has a
    // single `all` member; the filter keeps the document small enough that the
    // whole thing fits in RAM without streaming.
    JsonDocument filter;
    filter["request_time"] = true;
    filter["stop_name"] = true;
    filter["name"] = true;
    JsonObject dep = filter["departures"]["all"].add<JsonObject>();
    dep["line_name"] = true;
    dep["direction"] = true;
    dep["best_departure_estimate"] = true;
    dep["status"]["cancellation"]["value"] = true;

    JsonDocument doc;
    DeserializationError err =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (err) {
        Serial.printf("[bus/national] parse failed: %s\n", err.c_str());
        return Fetch::Failed;
    }

    // "2026-09-03T14:19:43+01:00" — the time starts at index 11.
    int nowMin = minutesOfDay(doc["request_time"] | "", 11);
    if (nowMin < 0) {
        Serial.println("[bus/national] no usable request_time");
        return Fetch::Failed;
    }

    std::vector<BusArrival> parsed;
    for (JsonObjectConst d : doc["departures"]["all"].as<JsonArrayConst>()) {
        if (d["status"]["cancellation"]["value"] | false) continue;

        String line = (const char*)(d["line_name"] | "");
        if (line.isEmpty()) continue;
        // The route filter is applied here rather than in the query: the feed
        // has no per-line parameter, and filtering client-side costs nothing
        // once the response has been paid for.
        if (!cfg.bus_line.isEmpty() && !line.equalsIgnoreCase(cfg.bus_line)) continue;

        // best_departure_estimate is the live time when there is one and the
        // timetabled time when there is not, so it is the only field that is
        // always both present and correct.
        int depMin = minutesOfDay(d["best_departure_estimate"] | "", 0);
        if (depMin < 0) continue;

        int eta = depMin - nowMin;
        // Departure times are wall-clock with no date attached, so midnight has
        // to be reasoned about in both directions: a departure just after it,
        // seen from just before, reads as nearly a full day negative — and one
        // just before it, running late, seen from just after, as nearly a full
        // day positive. Anything within an hour the wrong side of now is the
        // same bus, either still to come or overdue.
        if (eta < -60) eta += 24 * 60;
        else if (eta > 24 * 60 - 60) eta -= 24 * 60;
        if (eta < 0) eta = 0;   // overdue: at the stop now
        if (eta > BUS_MAX_ETA_MINUTES) continue;

        BusArrival ar;
        ar.line = line;
        ar.destination = (const char*)(d["direction"] | "");
        ar.etaSeconds = eta * 60;
        parsed.push_back(ar);
    }

    // The feed returns departures in time order already, but the route filter
    // and the midnight wrap can both disturb that, so sort rather than assume.
    std::sort(parsed.begin(), parsed.end(),
              [](const BusArrival& a, const BusArrival& b) {
                  return a.etaSeconds < b.etaSeconds;
              });
    if (parsed.size() > MAX_BUS_ARRIVALS) parsed.resize(MAX_BUS_ARRIVALS);

    // Commit only after a fully successful read. Zero departures is a valid
    // answer on a quiet evening, not a failure.
    out = parsed;
    const char* nm = doc["stop_name"] | "";
    if (!*nm) nm = doc["name"] | "";
    stopName = *nm ? String(nm) : cfg.bus_stop;
    return Fetch::Ok;
}

}  // namespace bus
