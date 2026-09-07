// London Underground arrivals client — TfL's Unified API prediction feed.
//
// The same open feed the river screen uses, asked a narrower question:
//
//   GET https://api.tfl.gov.uk/Line/{line}/Arrivals/{naptan}
//
// No key, no registration. Two details of that URL are worth the paragraphs.
//
// Why per *line* and not per station. The obvious endpoint is the river client's
// /StopPoint/{naptan}/Arrivals, and it works — but measured against real
// stations it answers with 24 KB at Oxford Circus, 37 KB at Waterloo and 71 KB
// at King's Cross, where six lines and both directions all report at once. The
// CYD has no PSRAM and is already spending 153 KB of its ~200 KB of DRAM on the
// framebuffer, and rail_api.cpp records that parsing straight off the TLS socket
// on that board fails about five polls in six. Asking one line at a time caps
// the worst case at ~19 KB (Piccadilly at King's Cross), which is the same order
// as the bus feed's existing budget and safe to buffer whole.
//
// Why the direction filter matches `platformName` and not `direction`. TfL's
// `direction` field ("inbound"/"outbound") looks like the right key and is not:
// it is absent on the entire Circle line, and on some records at Edgware Road.
// `platformName` is always present, in one of two shapes — "Northbound -
// Platform 3", or a bare "Platform 2" where TfL has no compass word to give.
// The token stored at provisioning time is the part before the " - ", so both
// shapes are pickable and both compare the same way here.
//
// `timeToStation` is already relative ("seconds from now" at the moment TfL
// answered), so as with the river feed the countdown is right even before NTP
// has synced.

#include "tube_api.h"
#include "app_config.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "tls.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <algorithm>
#include <vector>

namespace {

constexpr char kBase[] = "https://api.tfl.gov.uk/Line/";

// URL-escape everything outside the unreserved set, so a mistyped line id or
// station code can never break out of the path it is interpolated into.
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

// Case-insensitive equality, so a direction typed as "northbound" matches
// TfL's "Northbound".
bool equalsIgnoreCase(const String& a, const String& b) {
    if (a.length() != b.length()) return false;
    for (size_t i = 0; i < a.length(); ++i) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    }
    return true;
}

// The part of a platformName before the " - ": "Northbound - Platform 3" gives
// "Northbound", and a bare "Platform 2" gives itself. This is the token the
// installer stored, so the two ends of the config agree by construction.
String platformToken(const String& platformName) {
    int sep = platformName.indexOf(" - ");
    String token = sep < 0 ? platformName : platformName.substring(0, sep);
    token.trim();
    return token;
}

// The direction, shrunk to fit the row's route column. That column runs from
// BUS_ROUTE_X to BUS_DEST_X — 52px, about four characters of the row font, which
// is why bus routes like "N38" fit and "Northbound" would sprawl across the
// destination beside it. Three characters is the safe budget.
//
// A station with no compass word keeps its platform number instead ("Platform 2"
// -> "P2"), which is the only thing left that distinguishes one direction from
// the other there.
String dirTag(const String& token) {
    if (equalsIgnoreCase(token, "Northbound")) return "N/B";
    if (equalsIgnoreCase(token, "Southbound")) return "S/B";
    if (equalsIgnoreCase(token, "Eastbound"))  return "E/B";
    if (equalsIgnoreCase(token, "Westbound"))  return "W/B";
    if (token.startsWith("Platform ") || token.startsWith("platform ")) {
        return "P" + token.substring(9);
    }
    return token.substring(0, 3);
}

// TfL sometimes has no real destination to give and says so in the `towards`
// field instead. Neither of these is a place, so neither is what the
// destination column wants if there is anything better available.
bool isPlaceholderTowards(const String& towards) {
    return towards.isEmpty() ||
           equalsIgnoreCase(towards, "Check Front of Train") ||
           equalsIgnoreCase(towards, "Circle Line");
}

// ...but "better" is not always available. Measured at Edgware Road, a record
// whose `towards` is "Check Front of Train" carries no destinationName at all —
// TfL genuinely does not know where that train is going — so treating the
// placeholder as nothing leaves the row blank, which tells the reader less than
// the placeholder did.
//
// So it gets a display form of its own. "Check front" is what the platform
// indicator beside the reader says, and it is short enough to sit in the
// destination column without setting the marquee going.
String placeholderLabel(const String& towards) {
    if (equalsIgnoreCase(towards, "Check Front of Train")) return "Check front";
    return towards;   // "Circle Line", or whatever TfL invents next
}

// "Seven Sisters Underground Station" is the same place as "Seven Sisters" and
// the row is only so wide.
String stripStationSuffix(String name) {
    const char* suffix = " Underground Station";
    if (name.endsWith(suffix)) name = name.substring(0, name.length() - strlen(suffix));
    return name;
}

}  // namespace

namespace tube {

String lineLabel(const String& lineId) {
    // Only the two ampersanded names actually need shrinking; the rest are
    // short enough to print in full, and printing them in full is what makes
    // the header read like the roundel on the platform.
    if (equalsIgnoreCase(lineId, "hammersmith-city")) return "H&C";
    if (equalsIgnoreCase(lineId, "waterloo-city"))    return "W&C";

    // Title-case the id: "victoria" -> "Victoria". The tag is upper-cased for
    // display anyway, so this is really about the id's hyphens and about what
    // shows up in a log line.
    String out;
    bool startOfWord = true;
    for (size_t i = 0; i < lineId.length(); ++i) {
        char c = lineId[i];
        if (c == '-') { out += ' '; startOfWord = true; continue; }
        out += startOfWord ? (char)toupper((unsigned char)c) : c;
        startOfWord = false;
    }
    return out;
}

Fetch fetchArrivals(const Config& cfg, std::vector<TubeArrival>& out, String& stationName) {
    if (WiFi.status() != WL_CONNECTED) return Fetch::Failed;
    if (cfg.tube_stop.isEmpty() || cfg.tube_line.isEmpty()) return Fetch::Failed;

    String url = String(kBase) + escapePath(cfg.tube_line) +
                 "/Arrivals/" + escapePath(cfg.tube_stop);

    // TLS: verified against the embedded Mozilla root store (see tls.h). No
    // credentials travel on this request — the feed is open.
    WiFiClientSecure client;
    net::trustRoots(client);

    HTTPClient http;
    http.setTimeout(15000);
    http.setConnectTimeout(15000);
    if (!http.begin(client, url)) return Fetch::Failed;
    // Not decoration: TfL answers a request with no User-Agent with a 403.
    http.addHeader("User-Agent", "DepartureBuddy/1.0");
    http.useHTTP10(true);   // plain unchunked body the reader below can bound

    int code = http.GET();
    if (code != 200) {
        Serial.printf("[tube] HTTP %d\n", code);
        http.end();
        // TfL splits the two halves of this URL across two status codes: an
        // unknown line id is a 404 (EntityNotFoundException), but an unknown
        // station is a 400 (ApiArgumentException, "the following stop points
        // are not recognised"). Both are the same thing to the user — a code
        // that will never start working — so both stop the retries and drop
        // the screen rather than backing off and trying again forever.
        return (code == 404 || code == 400) ? Fetch::BadStation : Fetch::Failed;
    }

    // Read the body into a bounded buffer rather than parsing straight off the
    // socket, for the reason the river client gives: ArduinoJson pulls one byte
    // per read() and each is a call into lwip. The cap is the same guarantee
    // every other client makes — an unexpectedly huge response is discarded,
    // never allowed to exhaust the heap.
    WiFiClient& stream = http.getStream();
    String body;
    // Reserved at the full cap rather than at a guess. This is the largest of
    // the four feeds, and growing a String past its reservation means holding
    // the old buffer and the new one at once — a spike the CYD's PSRAM-less
    // heap should not have to absorb when the ceiling is known up front.
    body.reserve(TUBE_MAX_RESPONSE);
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
        if (body.length() + n > TUBE_MAX_RESPONSE) { overflow = true; break; }
        body.concat((const char*)buf, n);
    }
    http.end();

    if (overflow) {
        Serial.printf("[tube] response over %d bytes - discarded\n", TUBE_MAX_RESPONSE);
        return Fetch::Failed;
    }
    if (body.isEmpty()) {
        Serial.println("[tube] empty response");
        return Fetch::Failed;
    }

    // Keep only the six fields the screen needs. Applied during parsing, so the
    // ~40 other fields per prediction are never allocated.
    JsonDocument filter;
    {
        JsonObject f = filter.add<JsonObject>();
        f["platformName"] = true;
        f["towards"] = true;
        f["destinationName"] = true;
        f["stationName"] = true;
        f["timeToStation"] = true;
        f["vehicleId"] = true;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
    body = String();   // release the buffer before building the output
    if (err) {
        Serial.printf("[tube] parse failed: %s\n", err.c_str());
        return Fetch::Failed;
    }

    JsonArrayConst preds = doc.as<JsonArrayConst>();
    if (preds.isNull()) {
        Serial.println("[tube] response was not a prediction array");
        return Fetch::Failed;
    }

    String name;
    std::vector<TubeArrival> parsed;
    std::vector<String> seen;   // vehicleIds already taken, to drop duplicates

    for (JsonObjectConst p : preds) {
        String platformName = (const char*)(p["platformName"] | "");
        if (platformName.isEmpty()) continue;

        String token = platformToken(platformName);
        if (!cfg.tube_dir.isEmpty() && !equalsIgnoreCase(token, cfg.tube_dir)) continue;

        int32_t eta = p["timeToStation"] | (int32_t)-1;
        if (eta < 0) continue;
        if (eta > TUBE_MAX_ETA_MINUTES * 60) continue;   // beyond the window

        // A train reported twice — once approaching, once at the platform —
        // would cost a real departure its slot on a four-row screen.
        String vehicle = (const char*)(p["vehicleId"] | "");
        if (vehicle.length()) {
            if (std::find(seen.begin(), seen.end(), vehicle) != seen.end()) continue;
            seen.push_back(vehicle);
        }

        if (name.isEmpty()) name = stripStationSuffix((const char*)(p["stationName"] | ""));

        // `towards` is the front-of-train destination and the shorter of the
        // two ("Seven Sisters", not "Seven Sisters Underground Station"), so it
        // leads. When TfL has nothing real to say there, destinationName is
        // sometimes still a place — and when it is not there either, the
        // placeholder itself is the most honest thing left to show.
        String towards = (const char*)(p["towards"] | "");
        String destination = towards;
        if (isPlaceholderTowards(towards)) {
            destination = stripStationSuffix((const char*)(p["destinationName"] | ""));
            if (destination.isEmpty()) destination = placeholderLabel(towards);
        }

        TubeArrival ar;
        ar.line = dirTag(token);
        ar.destination = destination;
        ar.etaSeconds = eta;
        parsed.push_back(ar);

#if RAW_TUBE_DEBUG
        Serial.printf("[tube] %s %s -> %s in %ds\n",
                      platformName.c_str(), ar.line.c_str(),
                      ar.destination.c_str(), (int)ar.etaSeconds);
#endif
    }

    // The feed returns predictions unordered; the board wants soonest first.
    std::sort(parsed.begin(), parsed.end(),
              [](const TubeArrival& a, const TubeArrival& b) {
                  return a.etaSeconds < b.etaSeconds;
              });
    if (parsed.size() > MAX_TUBE_ARRIVALS) parsed.resize(MAX_TUBE_ARRIVALS);

    // Commit outputs only after a fully successful read. Zero trains is a valid
    // answer — engineering hours, or a suspended line — not a failure.
    //
    // TfL only names the station inside a prediction, so a platform with nothing
    // due identifies itself only by the Naptan in the URL. The installer knew
    // the friendly name when the user picked it from the list, so that is the
    // fallback rather than showing a raw code on an otherwise quiet screen.
    out = parsed;
    if (name.isEmpty()) name = cfg.tube_name;
    stationName = name.length() ? name : cfg.tube_stop;
    return Fetch::Ok;
}

}  // namespace tube
