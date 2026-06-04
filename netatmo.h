#pragma once
// =====================================================================
// Netatmo Weather API client for the DIY Weather Clock.
//
// Self-contained driver: depends only on the ESP8266 WiFi/HTTP stack,
// BearSSL and ArduinoJson. It talks to api.netatmo.com over HTTPS
// (TLS 1.2 with MFLN 512-byte buffers — verified to fit the ESP-01S's
// ~30 KB free heap). See docs/netatmo_integration.md for the design.
//
// Netatmo only provides *measured* values, so this fills temperature +
// humidity (from the outdoor NAModule1) and pressure (from its parent
// NAMain). The condition string, icon code and sun times still come from
// wttr.in — the caller overlays these readings on top of the wttr result.
//
// API surface:
//   netatmoRefresh(...)  -> POST /oauth2/token  (refresh-token grant)
//   netatmoFetch(...)    -> GET  /api/getstationsdata + parse + select
// Both take a Print& for logging (the firmware passes its RingLog `Log`).
// =====================================================================
#include <ESP8266WiFi.h>
#include <WiFiClientSecureBearSSL.h>
#include <ESP8266HTTPClient.h>
#include <ArduinoJson.h>

struct NetatmoReadings
{
  bool   haveTemp  = false; float tempC    = 0.0f;   // outdoor module
  bool   haveHum   = false; int   hum      = 0;       // outdoor module
  bool   havePress = false; float pressure = 0.0f;    // parent NAMain (sea level)
  String station;   // diagnostics: which station/module actually matched
  String module;
};

static const char NETATMO_HOST[] = "api.netatmo.com";

// MFLN probe result, cached across calls (-1 unknown / 0 no / 1 yes). The probe
// itself opens a throwaway connection, so we only do it once per boot.
static int8_t s_netatmoMfln = -1;

// Configure a BearSSL client for api.netatmo.com: no cert validation (per the
// design decision) and the smallest buffers the server will accept.
static void netatmoConfigTLS(BearSSL::WiFiClientSecure &client, Print &log)
{
  client.setInsecure();
  if (s_netatmoMfln < 0)
  {
    s_netatmoMfln = BearSSL::WiFiClientSecure::probeMaxFragmentLength(NETATMO_HOST, 443, 512) ? 1 : 0;
    log.print(F("[netatmo] MFLN 512 supported: "));
    log.println(s_netatmoMfln == 1 ? F("yes") : F("no"));
  }
  if (s_netatmoMfln == 1) client.setBufferSizes(512, 512);
  else                    client.setBufferSizes(16384, 512);
}

// RFC3986 form-value encoding (the refresh token contains a '|').
static String netatmoUrlEncode(const String &s)
{
  static const char *hex = "0123456789ABCDEF";
  String o; o.reserve(s.length() * 3);
  for (size_t i = 0; i < s.length(); i++)
  {
    uint8_t c = (uint8_t)s[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~')
      o += char(c);
    else { o += '%'; o += hex[(c >> 4) & 0xF]; o += hex[c & 0xF]; }
  }
  return o;
}

// POST /oauth2/token with the refresh-token grant. On success fills accessToken,
// newRefresh (Netatmo ROTATES it — caller MUST persist) and expiresInSec.
static bool netatmoRefresh(const String &clientId, const String &clientSecret,
                           const String &refreshToken, String &accessToken,
                           String &newRefresh, uint32_t &expiresInSec, Print &log)
{
  BearSSL::WiFiClientSecure client; netatmoConfigTLS(client, log);
  HTTPClient http; http.setReuse(false);
  http.useHTTP10(true);   // force HTTP/1.0 => no chunked encoding => clean stream read
  if (!http.begin(client, F("https://api.netatmo.com/oauth2/token")))
  {
    log.println(F("[netatmo] token begin() failed"));
    return false;
  }
  http.addHeader(F("Content-Type"), F("application/x-www-form-urlencoded"));

  String body = F("grant_type=refresh_token&refresh_token=");
  body += netatmoUrlEncode(refreshToken);
  body += F("&client_id=");      body += netatmoUrlEncode(clientId);
  body += F("&client_secret=");  body += netatmoUrlEncode(clientSecret);

  int code = http.POST(body);
  log.print(F("[netatmo] token refresh HTTP ")); log.println(code);
  if (code != 200)
  {
    log.print(F("[netatmo] token error body: ")); log.println(http.getString());
    http.end();
    return false;
  }

  StaticJsonDocument<384> doc;   // token response is small (access/refresh/expiry)
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) { log.print(F("[netatmo] token JSON err: ")); log.println(err.c_str()); return false; }

  accessToken  = (const char *)(doc["access_token"]  | "");
  newRefresh   = (const char *)(doc["refresh_token"] | "");
  expiresInSec = doc["expires_in"] | 0;
  return accessToken.length() > 0;
}

// GET /api/getstationsdata, de-chunk, parse (filtered) and select the outdoor
// module by name. `wantName` matches a NAModule1 module_name OR a station name
// (case-insensitive); empty => first non-read_only station's first NAModule1.
static bool netatmoFetch(const String &accessToken, const String &wantName,
                         NetatmoReadings &out, Print &log)
{
  BearSSL::WiFiClientSecure client; netatmoConfigTLS(client, log);
  HTTPClient http; http.setReuse(false);
  http.useHTTP10(true);   // force HTTP/1.0 => no chunked encoding => clean stream read
  if (!http.begin(client, F("https://api.netatmo.com/api/getstationsdata?get_favorites=false")))
  {
    log.println(F("[netatmo] data begin() failed"));
    return false;
  }
  http.addHeader(F("Authorization"), "Bearer " + accessToken);

  int code = http.GET();
  log.print(F("[netatmo] getstationsdata HTTP ")); log.println(code);
  if (code != 200) { http.end(); return false; }

  // Filter: keep only the handful of fields we need, so the parsed document
  // stays tiny regardless of how many stations/modules the account exposes.
  // Deserialize straight from the (HTTP/1.0, un-chunked) stream -- no 4 KB String.
  StaticJsonDocument<512> filter;
  {
    JsonObject d = filter["body"]["devices"][0].to<JsonObject>();
    d["station_name"] = true; d["module_name"] = true; d["read_only"] = true;
    d["dashboard_data"]["Pressure"] = true;
    JsonObject m = d["modules"][0].to<JsonObject>();
    m["type"] = true; m["module_name"] = true; m["reachable"] = true;
    m["dashboard_data"]["Temperature"] = true;
    m["dashboard_data"]["Humidity"]    = true;
  }
  DynamicJsonDocument doc(3072);   // filtered output is tiny; 3 KB is ample headroom
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) { log.print(F("[netatmo] data JSON err: ")); log.println(err.c_str()); return false; }

  String want = wantName; want.trim();
  JsonArray devices = doc["body"]["devices"];
  if (devices.isNull()) { log.println(F("[netatmo] no devices array")); return false; }

  for (JsonObject dev : devices)
  {
    bool   readOnly = dev["read_only"] | false;
    String devName  = (const char *)(dev["station_name"] | "");
    String devMod   = (const char *)(dev["module_name"]  | "");
    bool   devMatch = want.length() && (devName.equalsIgnoreCase(want) || devMod.equalsIgnoreCase(want));

    for (JsonObject m : dev["modules"].as<JsonArray>())
    {
      if (String((const char *)(m["type"] | "")) != "NAModule1") continue;
      String mName = (const char *)(m["module_name"] | "");

      bool modMatch  = want.length() && mName.equalsIgnoreCase(want);
      bool emptyPick = (want.length() == 0 && !readOnly);
      if (!(modMatch || devMatch || emptyPick)) continue;

      if (!(m["reachable"] | true))
      {
        log.print(F("[netatmo] matched module not reachable: ")); log.println(mName);
        continue;
      }

      JsonObject dd = m["dashboard_data"];
      if (!dd.isNull())
      {
        if (!dd["Temperature"].isNull()) { out.tempC = dd["Temperature"].as<float>(); out.haveTemp = true; }
        if (!dd["Humidity"].isNull())    { out.hum   = dd["Humidity"].as<int>();       out.haveHum  = true; }
      }
      JsonObject pdd = dev["dashboard_data"];
      if (!pdd.isNull() && !pdd["Pressure"].isNull())
      {
        out.pressure = pdd["Pressure"].as<float>(); out.havePress = true;
      }
      out.station = devName; out.module = mName;
      log.print(F("[netatmo] matched station='")); log.print(devName);
      log.print(F("' module='")); log.print(mName); log.println('\'');
      return out.haveTemp || out.haveHum || out.havePress;
    }
  }

  log.println(F("[netatmo] no matching module found"));
  return false;
}
