#pragma once
// =====================================================================
// Ecowitt local-gateway client for the DIY Weather Clock.
//
// Self-contained driver: depends only on the ESP8266 WiFi/HTTP stack.
// It reads the live data of an Ecowitt gateway / console that exposes the
// *local* HTTP API (GW1100, GW2000, WN1900, ...) straight from the LAN:
//
//     GET http://<gateway-ip>/get_livedata_info
//
// No cloud account, no API/application keys, no TLS: plain HTTP on port 80.
// That also makes it much lighter on the ESP-01S's ~30 KB free heap than an
// HTTPS client would be.
//
// Ecowitt only provides *measured* values, so this fills temperature +
// humidity (outdoor sensor, common_list ids 0x02 / 0x07) and sea-level
// ("relative") pressure (the wh25 block). The condition string, icon code and
// sun times still come from wttr.in — the caller overlays these readings on
// top of the wttr result.
//
// Response shape (all values are strings; the unit is either a separate
// "unit" field or appended to the value):
//   { "common_list": [ { "id": "0x02", "val": "21.4", "unit": "C" },
//                      { "id": "0x07", "val": "63%" }, ... ],
//     "wh25": [ { "intemp": "23.1", "unit": "C", "inhumi": "48%",
//                 "abs": "1004.2 hPa", "rel": "1015.0 hPa" } ], ... }
//
// The JSON is NOT parsed with a library (the firmware lives right at the 1 MB
// OTA ceiling, so ArduinoJson's flash cost is out of the question) and it is not
// buffered whole either: with many sensors attached the document can reach
// several KB. Every object we care about is flat ({...} without nesting), so the
// body is consumed as a stream, cut into small "{...}" chunks, and each chunk is
// scanned for the keys we need. Scanning stops as soon as everything is found.
//
// The readings are returned in canonical units (°C, %, hPa) whatever unit the
// gateway is configured with; the caller converts to the clock's display units.
//
// API surface:
//   ecowittNormalizeHost(...)  -> clean up what the user typed in the portal
//   ecowittFetch(...)          -> GET /get_livedata_info + stream-scan
// Both log through a Print& (the firmware passes its RingLog `Log`).
// =====================================================================
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>

struct EcowittReadings
{
  bool  haveTemp  = false; float tempC    = 0.0f;   // outdoor sensor, converted to °C
  bool  haveHum   = false; int   hum      = 0;      // outdoor sensor, %
  bool  havePress = false; float pressHpa = 0.0f;   // wh25 relative (sea level), converted to hPa
};

// Clean what the user typed as the gateway address: trim, drop a pasted
// "http(s)://" prefix and any path, keep only characters valid in host[:port].
static String ecowittNormalizeHost(String h)
{
  h.trim();
  if (h.startsWith("http://"))        h = h.substring(7);
  else if (h.startsWith("https://"))  h = h.substring(8);   // the gateway speaks plain HTTP anyway
  int slash = h.indexOf('/');
  if (slash >= 0) h = h.substring(0, slash);

  String o; o.reserve(h.length());
  for (size_t i = 0; i < h.length(); i++)
  {
    char c = h[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '.' || c == '-' || c == ':')
      o += c;
  }
  return o;
}

// --- tiny JSON value extractors -------------------------------------------
// NOT a general JSON parser: they work on one flat {...} chunk. Whitespace around
// the ':' is tolerated because the gateway's output is pretty-printed
// ("id": "0x02"), unlike a compact API.

// String value of  "key" : "value"  inside `o`. "" if not found / not a string.
static String ecowittStr(const String &o, const char *key)
{
  String pat = String('"') + key + '"';
  int p = o.indexOf(pat);
  if (p < 0) return "";
  p += pat.length();
  int n = o.length();
  while (p < n && (o[p] == ' ' || o[p] == '\t' || o[p] == '\r' || o[p] == '\n')) p++;
  if (p >= n || o[p] != ':') return "";
  p++;
  while (p < n && (o[p] == ' ' || o[p] == '\t' || o[p] == '\r' || o[p] == '\n')) p++;
  if (p >= n || o[p] != '"') return "";
  p++;
  int e = o.indexOf('"', p);
  if (e < 0) return "";
  return o.substring(p, e);
}

// Leading number of a value like "21.4", "-3.5", "63%", "1015.0 hPa". Returns
// false for "--", "None", "" and the like (sensor missing). `rest` receives what
// follows the number (the appended unit, if any).
static bool ecowittNumber(const String &s, float &out, String *rest = nullptr)
{
  int n = s.length();
  int i = 0;
  while (i < n && s[i] == ' ') i++;
  int b = i;
  if (i < n && (s[i] == '-' || s[i] == '+')) i++;
  bool digit = false, dot = false;
  while (i < n)
  {
    char c = s[i];
    if (c >= '0' && c <= '9') { digit = true; i++; }
    else if (c == '.' && !dot) { dot = true; i++; }
    else break;
  }
  if (!digit) return false;
  out = s.substring(b, i).toFloat();
  if (rest) *rest = s.substring(i);
  return true;
}

// Pressure value + unit text ("hPa", "inHg", "mmHg", "kPa", "mbar") -> hPa.
static float ecowittToHpa(float v, String unit)
{
  unit.toLowerCase();
  if (unit.indexOf("inhg") >= 0) return v * 33.8639f;
  if (unit.indexOf("mmhg") >= 0) return v * 1.33322f;
  if (unit.indexOf("kpa")  >= 0) return v * 10.0f;
  return v;   // hPa / mbar
}

// Inspect one flat {...} chunk and fill whatever it carries. First match wins.
static void ecowittHandleObject(const String &o, EcowittReadings &r)
{
  String id = ecowittStr(o, "id");
  if (id.length())
  {
    // common_list entry: { "id": "0x02", "val": "21.4", "unit": "C" }
    if (id.equalsIgnoreCase("0x02") && !r.haveTemp)           // outdoor temperature
    {
      float v; String rest;
      if (ecowittNumber(ecowittStr(o, "val"), v, &rest))
      {
        String u = ecowittStr(o, "unit");
        if (u.length() == 0) u = rest;
        // "F", "°F" or the single-glyph U+2109 (UTF-8 E2 84 89)
        if (u.indexOf('F') >= 0 || u.indexOf('f') >= 0 || u.indexOf("\xE2\x84\x89") >= 0)
          v = (v - 32.0f) * 5.0f / 9.0f;
        if (v > -60.0f && v < 70.0f) { r.tempC = v; r.haveTemp = true; }
      }
    }
    else if (id.equalsIgnoreCase("0x07") && !r.haveHum)        // outdoor humidity
    {
      float v;
      if (ecowittNumber(ecowittStr(o, "val"), v) && v >= 0.0f && v <= 100.0f)
      {
        r.hum = (int)(v + 0.5f); r.haveHum = true;
      }
    }
    return;
  }

  // wh25 block: { "intemp": "...", "unit": "C", "inhumi": "48%", "abs": "...", "rel": "1015.0 hPa" }
  if (!r.havePress)
  {
    String rel = ecowittStr(o, "rel");
    float v; String rest;
    if (rel.length() && ecowittNumber(rel, v, &rest))
    {
      float hpa = ecowittToHpa(v, rest);
      if (hpa > 300.0f && hpa < 1100.0f) { r.pressHpa = hpa; r.havePress = true; }
    }
  }
}

// GET http://<host>/get_livedata_info and stream-scan the body. `host` is the
// gateway's IP or name, optionally with :port (see ecowittNormalizeHost).
static bool ecowittFetch(const String &host, EcowittReadings &out, Print &log)
{
  String url = String(F("http://")) + host + F("/get_livedata_info");

  WiFiClient client;
  HTTPClient http; http.setReuse(false);
  http.useHTTP10(true);        // HTTP/1.0 => no chunked encoding => raw JSON on the stream
  http.setTimeout(4000);       // keep a dead/unplugged gateway from freezing the clock for long
  if (!http.begin(client, url))
  {
    log.println(F("[ecowitt] begin() failed"));
    return false;
  }

  int code = http.GET();
  log.print(F("[ecowitt] get_livedata_info HTTP ")); log.println(code);
  if (code != 200) { http.end(); return false; }

  WiFiClient &stream = http.getStream();
  String obj; obj.reserve(192);
  bool inObj = false;
  size_t total = 0;
  unsigned long deadline = millis() + 8000;

  // Keep going while data is available OR the socket is still open: the gateway
  // closes right after the body, and gating only on connected() would drop the
  // bytes already buffered.
  while (millis() < deadline && (stream.connected() || stream.available()))
  {
    while (stream.available())
    {
      char c = (char)stream.read();
      total++;
      if (c == '{')       { obj = ""; inObj = true; }
      else if (c == '}')
      {
        if (inObj) ecowittHandleObject(obj, out);
        inObj = false; obj = "";
        if (out.haveTemp && out.haveHum && out.havePress) break;   // got everything
      }
      else if (inObj && obj.length() < 192) obj += c;   // cap: objects we need are ~60 chars
    }
    if (out.haveTemp && out.haveHum && out.havePress) break;
    delay(1);   // yield to the WiFi/TCP stack so pending bytes can arrive
  }
  http.end();
  log.print(F("[ecowitt] bytes scanned: ")); log.println((unsigned long)total);

  // Log the parsed readings (the actual values we got from the gateway).
  log.print(F("[ecowitt] parsed:"));
  if (out.haveTemp)  { log.print(F(" temp=")); log.print(out.tempC, 1);      log.print(F("C")); }
  if (out.haveHum)   { log.print(F(" hum="));  log.print(out.hum);           log.print(F("%")); }
  if (out.havePress) { log.print(F(" press=")); log.print(out.pressHpa, 1);  log.print(F("hPa")); }
  if (!out.haveTemp && !out.haveHum && !out.havePress) log.print(F(" (none)"));
  log.println();

  return out.haveTemp || out.haveHum || out.havePress;
}
