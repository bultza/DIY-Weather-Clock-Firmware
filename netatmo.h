#pragma once
// =====================================================================
// Netatmo Weather API client for the DIY Weather Clock.
//
// Self-contained driver: depends only on the ESP8266 WiFi/HTTP stack and
// BearSSL. It talks to api.netatmo.com over HTTPS (TLS 1.2 with MFLN
// 512-byte buffers — verified to fit the ESP-01S's ~30 KB free heap).
// See docs/netatmo_integration.md for the design.
//
// Netatmo only provides *measured* values, so this fills temperature +
// humidity (from the outdoor NAModule1) and pressure (from its parent
// NAMain). The condition string, icon code and sun times still come from
// wttr.in — the caller overlays these readings on top of the wttr result.
//
// JSON parsing is done by hand (targeted string scans) rather than with a
// library: the Netatmo responses are stable and the firmware lives right at
// the 1 MB OTA size ceiling, so we cannot afford ArduinoJson's flash cost.
// Because we force HTTP/1.0 (useHTTP10), the body is un-chunked and small
// (~4 KB), so getString() returns it cleanly for scanning.
//
// API surface:
//   netatmoRefresh(...)  -> POST /oauth2/token  (refresh-token grant)
//   netatmoFetch(...)    -> GET  /api/getstationsdata + scan + select
// Both take a Print& for logging (the firmware passes its RingLog `Log`).
// =====================================================================
#include <ESP8266WiFi.h>
#include <WiFiClientSecureBearSSL.h>
#include <ESP8266HTTPClient.h>

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

// --- tiny JSON value extractors -------------------------------------------
// These are NOT a general JSON parser; they exploit the flat, stable shape of
// Netatmo's responses. The leading quote in the key pattern ("Pressure":)
// disambiguates against substrings like "AbsolutePressure":, and the trailing
// ':' avoids matching array entries such as data_type:["Temperature",...].

// String value of  "key":"value"  searched from `from`. "" if not found.
static String njsonStr(const String &s, const char *key, int from = 0)
{
  String pat = String('"') + key + "\":\"";
  int p = s.indexOf(pat, from);
  if (p < 0) return "";
  p += pat.length();
  int e = s.indexOf('"', p);
  if (e < 0) return "";
  return s.substring(p, e);
}

// Numeric value (int or float, as text) of  "key":<number>  searched from `from`.
static String njsonNum(const String &s, const char *key, int from = 0)
{
  String pat = String('"') + key + "\":";
  int p = s.indexOf(pat, from);
  if (p < 0) return "";
  p += pat.length();
  while (p < (int)s.length() && s[p] == ' ') p++;
  int e = p;
  while (e < (int)s.length())
  {
    char c = s[e];
    if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.') e++;
    else break;
  }
  return s.substring(p, e);
}

// POST /oauth2/token with the refresh-token grant. On success fills accessToken,
// newRefresh (Netatmo ROTATES it — caller MUST persist) and expiresInSec.
static bool netatmoRefresh(const String &clientId, const String &clientSecret,
                           const String &refreshToken, String &accessToken,
                           String &newRefresh, uint32_t &expiresInSec, Print &log)
{
  BearSSL::WiFiClientSecure client; netatmoConfigTLS(client, log);
  HTTPClient http; http.setReuse(false);
  http.useHTTP10(true);   // force HTTP/1.0 => no chunked encoding => clean read
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

  String payload = http.getString();   // small (~300 B), un-chunked
  http.end();

  accessToken  = njsonStr(payload, "access_token");
  newRefresh   = njsonStr(payload, "refresh_token");
  expiresInSec = (uint32_t)njsonNum(payload, "expires_in").toInt();
  if (accessToken.length() == 0)
  {
    log.println(F("[netatmo] token response missing access_token"));
    return false;
  }
  return true;
}

// GET /api/getstationsdata, then hand-scan for the chosen outdoor module.
// `wantName` matches a NAModule1 module_name (case-insensitive); empty => the
// first NAModule1 in the response. Temperature/Humidity come from that module's
// dashboard_data; Pressure from the nearest preceding device dashboard_data
// (its parent NAMain). Notes vs the old ArduinoJson path: read_only filtering
// and reachable checks are dropped for simplicity, and non-ASCII module names
// (returned as \uXXXX escapes) won't match — use an ASCII module name.
static bool netatmoFetch(const String &accessToken, const String &wantName,
                         NetatmoReadings &out, Print &log)
{
  BearSSL::WiFiClientSecure client; netatmoConfigTLS(client, log);
  HTTPClient http; http.setReuse(false);
  http.useHTTP10(true);   // force HTTP/1.0 => no chunked encoding => clean read
  if (!http.begin(client, F("https://api.netatmo.com/api/getstationsdata?get_favorites=false")))
  {
    log.println(F("[netatmo] data begin() failed"));
    return false;
  }
  http.addHeader(F("Authorization"), "Bearer " + accessToken);

  int code = http.GET();
  log.print(F("[netatmo] getstationsdata HTTP ")); log.println(code);
  if (code != 200) { http.end(); return false; }

  String s = http.getString();   // un-chunked body, ~4 KB
  http.end();
  log.print(F("[netatmo] payload bytes: ")); log.println(s.length());

  String want = wantName; want.trim();

  // Find the chosen NAModule1: scan each "type":"NAModule1" and read the
  // module_name that follows it inside the same object.
  const char *TYPE = "\"type\":\"NAModule1\"";
  int modPos = -1;
  String matchedName;
  int from = 0;
  while (true)
  {
    int t = s.indexOf(TYPE, from);
    if (t < 0) break;
    String mname = njsonStr(s, "module_name", t);   // next module_name after the type
    if (want.length() == 0 || mname.equalsIgnoreCase(want))
    {
      modPos = t; matchedName = mname; break;
    }
    from = t + (int)strlen(TYPE);
  }
  if (modPos < 0) { log.println(F("[netatmo] no matching NAModule1 found")); return false; }

  // Outdoor temp/humidity: first occurrences after the module's position (in
  // its dashboard_data). data_type:["Temperature",...] has no ':' so won't match.
  String tStr = njsonNum(s, "Temperature", modPos);
  String hStr = njsonNum(s, "Humidity",    modPos);
  if (tStr.length()) { out.tempC = tStr.toFloat(); out.haveTemp = true; }
  if (hStr.length()) { out.hum   = hStr.toInt();   out.haveHum  = true; }

  // Pressure: the parent device lists its dashboard_data (with Pressure) BEFORE
  // its modules[], so take the nearest "Pressure": before the module.
  int pPos = s.lastIndexOf("\"Pressure\":", modPos);
  if (pPos >= 0)
  {
    String pStr = njsonNum(s, "Pressure", pPos);
    if (pStr.length()) { out.pressure = pStr.toFloat(); out.havePress = true; }
  }

  // Parent station name (nearest before the module), for diagnostics.
  int snPos = s.lastIndexOf("\"station_name\":\"", modPos);
  if (snPos >= 0) out.station = njsonStr(s, "station_name", snPos);
  out.module = matchedName;

  log.print(F("[netatmo] matched station='")); log.print(out.station);
  log.print(F("' module='")); log.print(out.module); log.println('\'');

  // Log the parsed readings (the actual values we got from Netatmo).
  log.print(F("[netatmo] parsed:"));
  if (out.haveTemp)  { log.print(F(" temp=")); log.print(out.tempC, 1);   log.print(F("C")); }
  if (out.haveHum)   { log.print(F(" hum=")); log.print(out.hum);         log.print(F("%")); }
  if (out.havePress) { log.print(F(" press=")); log.print(out.pressure, 1); log.print(F("hPa")); }
  if (!out.haveTemp && !out.haveHum && !out.havePress) log.print(F(" (none)"));
  log.println();

  return out.haveTemp || out.haveHum || out.havePress;
}
