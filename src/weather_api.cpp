// Open-Meteo current-conditions client.
//
//   GET https://api.open-meteo.com/v1/forecast?latitude=..&longitude=..
//         &current=temperature_2m,apparent_temperature,weather_code,wind_speed_10m
//         &daily=temperature_2m_max,temperature_2m_min
//         &wind_speed_unit=mph&timezone=auto&forecast_days=1
//
// No key, no registration, no quota — the same bargain as the TfL feeds. The
// response is about 700 bytes, so this is the cheapest of the four clients by
// a wide margin.
//
// Units are set in the request rather than converted here: Celsius is the
// default and `wind_speed_unit=mph` gives the usual British pairing, so the
// firmware never does arithmetic it could have asked for.

#include "weather_api.h"
#include "app_config.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

namespace {

constexpr char kBase[] = "https://api.open-meteo.com/v1/forecast";

// Round to whole degrees for display. A tenth of a degree is noise at a glance
// and the row font is wide, so the extra character costs more than it says.
String whole(float v) {
    return String((int)lroundf(v));
}

}  // namespace

namespace weather {

// WMO 4677 present-weather codes, as published by Open-Meteo. Grouped rather
// than exhaustive: "Drizzle" covers its three intensities because the board has
// one line for it and nobody plans their morning around drizzle intensity.
const char* describe(int code) {
    switch (code) {
        case 0:  return "Clear";
        case 1:  return "Mainly clear";
        case 2:  return "Partly cloudy";
        case 3:  return "Overcast";
        case 45: case 48: return "Fog";
        case 51: case 53: case 55: return "Drizzle";
        case 56: case 57: return "Freezing drizzle";
        case 61: return "Light rain";
        case 63: return "Rain";
        case 65: return "Heavy rain";
        case 66: case 67: return "Freezing rain";
        case 71: return "Light snow";
        case 73: return "Snow";
        case 75: return "Heavy snow";
        case 77: return "Snow grains";
        case 80: return "Light showers";
        case 81: return "Showers";
        case 82: return "Heavy showers";
        case 85: case 86: return "Snow showers";
        case 95: return "Thunderstorm";
        case 96: case 99: return "Thunder + hail";
        default: return "";        // unknown code: say nothing rather than guess
    }
}

Fetch fetchCurrent(const Config& cfg, Weather& out) {
    if (WiFi.status() != WL_CONNECTED) return Fetch::Failed;
    if (cfg.wx_lat == INT32_MIN || cfg.wx_lon == INT32_MIN) return Fetch::Failed;

    char url[320];
    snprintf(url, sizeof(url),
             "%s?latitude=%.5f&longitude=%.5f"
             "&current=temperature_2m,apparent_temperature,weather_code,wind_speed_10m"
             "&daily=temperature_2m_max,temperature_2m_min"
             "&wind_speed_unit=mph&timezone=auto&forecast_days=1",
             kBase, cfg.wx_latitude(), cfg.wx_longitude());

    // TLS: as with the other clients, setInsecure() encrypts without
    // authenticating the server. No credentials travel on this request.
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setTimeout(15000);
    http.setConnectTimeout(15000);
    if (!http.begin(client, url)) return Fetch::Failed;
    http.addHeader("User-Agent", "DepartureBuddy/1.0");
    http.useHTTP10(true);

    int code = http.GET();
    if (code != 200) {
        Serial.printf("[wx] HTTP %d\n", code);
        http.end();
        // 400 is Open-Meteo's answer to coordinates it cannot use.
        return code == 400 ? Fetch::BadLocation : Fetch::Failed;
    }

    // Small enough to parse straight off the stream, unlike the river feed:
    // a filter keeps the document to the seven numbers actually drawn.
    JsonDocument filter;
    filter["current"]["temperature_2m"] = true;
    filter["current"]["apparent_temperature"] = true;
    filter["current"]["weather_code"] = true;
    filter["current"]["wind_speed_10m"] = true;
    filter["daily"]["temperature_2m_max"] = true;
    filter["daily"]["temperature_2m_min"] = true;

    JsonDocument doc;
    DeserializationError err = deserializeJson(
        doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();
    if (err) {
        Serial.printf("[wx] parse failed: %s\n", err.c_str());
        return Fetch::Failed;
    }

    JsonObjectConst cur = doc["current"];
    if (cur.isNull() || !cur["temperature_2m"].is<float>()) {
        Serial.println("[wx] response had no current temperature");
        return Fetch::Failed;
    }

    Weather w;
    w.temp = whole(cur["temperature_2m"] | 0.0f);
    w.feels = whole(cur["apparent_temperature"] | 0.0f);
    w.wind = whole(cur["wind_speed_10m"] | 0.0f);
    w.code = cur["weather_code"] | -1;
    w.condition = describe(w.code);

    // The daily arrays hold one entry because the request asks for one day.
    JsonArrayConst hi = doc["daily"]["temperature_2m_max"];
    JsonArrayConst lo = doc["daily"]["temperature_2m_min"];
    if (!hi.isNull() && hi.size()) w.high = whole(hi[0] | 0.0f);
    if (!lo.isNull() && lo.size()) w.low = whole(lo[0] | 0.0f);

#if RAW_WEATHER_DEBUG
    Serial.printf("[wx] %s°C (feels %s) %s, wind %s mph, %s/%s\n",
                  w.temp.c_str(), w.feels.c_str(), w.condition.c_str(),
                  w.wind.c_str(), w.high.c_str(), w.low.c_str());
#endif

    out = w;
    return Fetch::Ok;
}

}  // namespace weather
