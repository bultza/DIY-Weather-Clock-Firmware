# Netatmo integration — research & design notes

Status: **design / research** (no firmware code written yet). This file is the
"separate file" with everything needed to access Netatmo; the actual driver will
live in its own source file (`netatmo.h`) once the open decisions below are made.

The goal (per the plan): add a **toggle** in the config portal to enable Netatmo
and fields for the credentials it needs. When enabled, Netatmo provides the
**measured** values from the user's own station; **wttr.in stays** as the source
for everything Netatmo cannot give (see "What Netatmo does NOT give you").

---

## 1. What Netatmo gives you — and what it does NOT

The Netatmo **Weather** API (`getstationsdata`) returns *sensor measurements*
from the user's own station, per module:

| Module (`type`)        | Useful `dashboard_data` fields                          |
|------------------------|---------------------------------------------------------|
| Main indoor (`NAMain`) | `Temperature`, `Humidity`, `Pressure` (sea-level), `AbsolutePressure`, `CO2`, `Noise` |
| Outdoor (`NAModule1`)  | `Temperature`, `Humidity`                               |
| Wind (`NAModule2`)     | `WindStrength`, `WindAngle`, `GustStrength`, `GustAngle`|
| Rain (`NAModule3`)     | `Rain`, `sum_rain_1`, `sum_rain_24`                     |
| Indoor extra (`NAModule4`) | `Temperature`, `Humidity`, `CO2`                    |

Units depend on the account: temperature °C/°F, pressure mbar/inHg, wind km/h/mph
— set per user in their Netatmo profile, returned as numbers (no unit string).
The response echoes them in `body.user.administrative`: `unit` (0=metric °C),
`pressureunit` (0=mbar), `windunit` (0=km/h). The sample account is all-metric.

**Reality check from the real sample (`netatmo_sample.json`):** an account can see
**several stations** — the sample has **3** `NAMain` devices (one owned, two
`read_only`: a shared one and a foreign `favorite`). So "use the first station" is
unsafe. The user must name which one (see §5/§7). Each owned/shared `NAMain` may
have an outdoor `NAModule1`; the indoor `NAMain.Temperature` is the *room* temp
(e.g. 23.2 in "Salón") — we want the **outdoor module** temp (e.g. 17.9 "Terraza").

**What Netatmo does NOT give you (this is the key point for the hybrid design):**

- ❌ No **weather condition** text ("Sunny", "Light rain", …)
- ❌ No **condition code** → so **no icon** (our `weatherIconForCode()` needs the
  wttr.in `%i` code)
- ❌ No **forecast**
- ❌ No **sunrise/sunset** (we already get these from wttr.in `%S %s` for the
  brightness-follows-sun feature)

➡️ **Conclusion:** Netatmo replaces the *numbers we measure locally* (outdoor
temp & humidity, pressure, optionally wind/rain). wttr.in must stay for the
**condition string + icon code + sun times**. This matches the original intuition
in the plan. The two are complementary, not interchangeable.

### Proposed data policy (hybrid)

```
weather_temp   ← Netatmo outdoor Temperature   (else wttr.in)
weather_hum    ← Netatmo outdoor Humidity       (else wttr.in)
weather_press  ← Netatmo main   Pressure        (else wttr.in)
weather_wind   ← Netatmo wind   WindStrength    (else wttr.in)   [optional / if module present]
weather_cond   ← ALWAYS wttr.in
weather_code   ← ALWAYS wttr.in   (icon)
sun times      ← ALWAYS wttr.in
```

If the Netatmo fetch fails (network/token), fall back to the wttr.in value for
that field — so the clock never shows worse data than today.

---

## 2. Authentication — the hard part

Netatmo uses **OAuth2**. Two things changed in recent years that shape our design:

1. **The password / client-credentials grant was removed (Oct 2022).** You can no
   longer send a username+password to get a token. Only the **Authorization Code**
   flow exists — which normally needs a browser redirect, impossible to do cleanly
   on an ESP-01.
2. **Refresh tokens now rotate.** Each time you refresh the access token, Netatmo
   returns a **new `refresh_token`** and invalidates the old one. **We must persist
   the new refresh_token every time** or we lock ourselves out.

### The practical path for a personal device: the dev-portal Token Generator

You don't implement the redirect flow on the device. Instead:

1. The user creates an app at **https://dev.netatmo.com** → gets a **`client_id`**
   and **`client_secret`**.
2. On that app's page there is a **"Token generator"**: pick the scope
   **`read_station`**, click generate → it returns an **`access_token`** and a
   **`refresh_token`** bound to the user's own account.
3. The user pastes **client_id + client_secret + refresh_token** into our config
   portal. The device never needs a browser redirect.

From then on the device only ever uses the **refresh_token grant** to mint fresh
access tokens. (Token generator existence/labels should be re-verified in the
current portal — see "Open questions".)

### Token endpoint (refresh grant)

```
POST https://api.netatmo.com/oauth2/token
Content-Type: application/x-www-form-urlencoded

grant_type=refresh_token
&refresh_token=<stored>
&client_id=<id>
&client_secret=<secret>
```

Response (JSON):

```json
{ "access_token": "...", "refresh_token": "...NEW...", "expires_in": 10800, "scope": ["read_station"] }
```

- `expires_in` ≈ **10800 s (3 h)** → keep the access token in RAM, only refresh
  when it's near expiry.
- **Save the returned `refresh_token` back to EEPROM immediately** (it rotated).

### Data endpoint

```
GET https://api.netatmo.com/api/getstationsdata?get_favorites=false
Authorization: Bearer <access_token>
```

Rate limits (well within our 15-min poll): ~500 req/h per app, 50 req/10 s per user.

---

## 3. The other hard part: HTTPS on the ESP8266

Unlike wttr.in (which we deliberately do over **plain HTTP** because the ESP8266's
BearSSL maxes at TLS 1.2 and wttr.in forces 1.3), **`api.netatmo.com` is HTTPS and
there is no plain-HTTP option.** Good news: Netatmo serves **TLS 1.2**, which
BearSSL *can* negotiate. The challenge is **RAM**:

- BearSSL wants a **16 KB RX buffer** by default. The ESP-01S has only ~40 KB heap.
- **MFLN** (smaller negotiated buffers) only works if the server supports it;
  big servers usually don't. We must `probeMaxFragmentLength()` first and, if it
  fails, accept the full 16 KB RX buffer (TX can be 512 B).
- Use `BearSSL::WiFiClientSecure` with `setBufferSizes(rx, tx)`.
- **`client.setInsecure()`** (skip certificate validation) avoids storing a root
  CA and the extra memory/maintenance. Trade-off: no protection against a MITM on
  the local network. Alternative is pinning the CA cert (more RAM + breaks when
  Netatmo rotates CAs). For a hobby clock, `setInsecure()` is the pragmatic choice
  — to be confirmed.

**Memory discipline (critical):**
- Do the Netatmo HTTPS work when heap is highest; the firmware already logs
  `ESP.getFreeHeap()` around the fetch — we must watch it on real hardware.
- **Do not** build a giant `String` of the JSON response like the wttr.in path
  does. `getstationsdata` can be several KB. **Stream-parse** straight off the
  TLS client.
- Two TLS calls may be needed per cycle (token refresh + data). Refresh only when
  expired, so most 15-min cycles are a single GET.

➡️ **This is the single biggest feasibility risk.** It is known to work on ESP8266
in other projects, but it's tight on an ESP-01S. First milestone should be a
bare "can we even TLS-handshake + GET getstationsdata and see free heap survive"
spike before wiring up the UI.

---

## 4. JSON parsing

`getstationsdata` is nested JSON; hand-parsing like the wttr.in pipe format would
be fragile. **ArduinoJson** with a **filter** + streaming input keeps only the few
fields we need in RAM (a `[0]` key in the filter applies to every array element):

```cpp
StaticJsonDocument<256> filter;
JsonObject dev = filter["body"]["devices"][0].to<JsonObject>();
dev["station_name"] = true;
dev["module_name"]  = true;          // NAMain's own name
dev["read_only"]    = true;
dev["dashboard_data"]["Pressure"] = true;
JsonObject mod = dev["modules"][0].to<JsonObject>();
mod["type"] = true;
mod["module_name"] = true;
mod["dashboard_data"]["Temperature"] = true;
mod["dashboard_data"]["Humidity"]    = true;
// deserializeJson(doc, tlsClient, DeserializationOption::Filter(filter));
```

### Selection logic (matches §7)

Config holds a **station/module name** string (the user types e.g. `Terraza`):

1. Walk `body.devices[]`; for each, walk `modules[]`.
2. Pick the `NAModule1` whose `module_name` equals the configured name
   (case-insensitive, trimmed) → its `dashboard_data` gives outdoor **Temperature
   + Humidity**; that module's **parent device** `dashboard_data.Pressure` gives
   **Pressure**.
3. Also accept a match on the parent device's `station_name`/`module_name` (so the
   user can name the station instead of the outdoor module).
4. **Fallback** if the field is empty: first device without `read_only:true`, its
   first `NAModule1`. (Deterministic for the sample: "Salón" → "Terraza".)
5. If nothing matches or the module is `reachable:false`, treat as a Netatmo miss
   → keep the wttr.in values for those fields.

---

## 5. Config & storage changes (in the main `.ino`)

New config fields (the toggle + credentials the plan asks for):

```cpp
bool   config_netatmo_enabled       = false;
String config_netatmo_client_id     = "";   // ~24 chars
String config_netatmo_client_secret = "";   // ~48 chars
String config_netatmo_refresh_token = "";   // ~65+ chars, ROTATES — must be re-saved
String config_netatmo_station       = "";   // outdoor-module or station name, e.g. "Terraza"
```

### EEPROM problem

The current map is full: `ADDR_SSID=0, ADDR_PASS=70, ADDR_CITY=140, ADDR_TZ=210,
ADDR_VARIABLES=300, ADDR_SIGNATURE=500` in a **512-byte** region. There is **no
room** for ~140 bytes of Netatmo credentials.

Required changes (in `loadSettings`/`saveSettings`, the only places that touch
offsets):
- **Bump `EEPROM_SIZE`** (ESP8266 EEPROM emulation allows up to 4096 B; e.g. 1024).
- Add `ADDR_NETATMO_*` offsets in the new space and move `ADDR_SIGNATURE` past them.
- **Bump `DEVICE_SIGNATURE`** so the layout change is detected — this forces every
  device into the config portal on next boot (expected for a config-layout change).
- The driver must call back into the main sketch to persist a **rotated**
  refresh_token (e.g. `netatmoSaveRefreshToken(const String&)` → writes EEPROM).

Config portal HTML: a checkbox "Enable Netatmo" that reveals three text inputs
(client id / secret / refresh token), wired into `handleConfigForm()`.

---

## 6. Proposed separate driver file — `netatmo.h`

Self-contained, mirrors how `getWeather()` fills the `weather_*` globals. Sketch:

```cpp
// netatmo.h — Netatmo Weather API client (token refresh + getstationsdata).
struct NetatmoReadings {
  bool  haveTemp = false;  float tempC;      // outdoor module
  bool  haveHum  = false;  int   hum;        // outdoor module
  bool  havePress= false;  float pressure;   // main module (sea level)
  bool  haveWind = false;  float windKmh; int windDir;   // wind module, if any
};

// Returns true and fills `out` on success. Refreshes the access token first if
// needed; persists a rotated refresh_token via the main-sketch callback.
bool netatmoFetch(NetatmoReadings &out);

bool netatmoEnabledAndConfigured();   // toggle on + 3 creds present
```

`loop()` (or the existing weather refresh block) would, when Netatmo is enabled,
call `netatmoFetch()` and overlay its values on top of the wttr.in result per the
policy in §1. wttr.in still runs for cond/code/sun.

---

## 7. Resolved decisions

1. **Data overridden by Netatmo:** outdoor **Temperature + Humidity** (`NAModule1`)
   **and Pressure** (`NAMain`). **Not** wind/rain for v1 → no UI changes needed,
   and wind still comes from wttr.in. Condition + icon + sun times always wttr.in.
2. **JSON:** **ArduinoJson** with a filter + streaming input (added to `lib_deps`).
3. **Module selection:** **config text field** (`config_netatmo_station`) where the
   user types the outdoor-module or station name (e.g. `Terraza`). Matching per the
   logic in §4; empty → first non-`read_only` station. Needed because accounts can
   see several stations (the sample has 3).
4. **EEPROM:** **bump `EEPROM_SIZE` (→ 1024) and `DEVICE_SIGNATURE`** — accepted;
   existing devices reconfigure once after the update.
5. **TLS:** **`setInsecure()`** — no cert validation (less RAM, no CA maintenance).

## 7b. Validated on real hardware (ESP-01S @ 192.168.1.81, core 3.1.2)

The driver in `netatmo.h` was proven end-to-end over OTA on the live device:

- **TLS 1.2 handshake** to api.netatmo.com with **MFLN 512-byte buffers** (server
  supports MFLN) — handshake ~1 s, leaves ~18 KB heap. The full-16 KB fallback is
  never needed for Netatmo.
- **Token refresh** (`/oauth2/token`): HTTP 200, `expires_in` = 10800 s (3 h),
  and Netatmo **returns a rotated refresh_token every time** → MUST be persisted.
- **getstationsdata**: HTTP 200, ~4 KB body, parsed with **ArduinoJson v6** + a
  filter, streamed straight from the response (no 4 KB String). Heap cost ~0.4 KB.
- **Selection by name** ("Terraza") picks the right NAModule1 + parent NAMain;
  readings correct (outdoor temp/hum + sea-level pressure). 3/3 consistent runs.

Three findings that shaped the final design:

1. **OTA size ceiling (~502 KB).** The sketch is near the 1 MB-flash OTA limit.
   **ArduinoJson v7 pushed the image over and OTA failed with `ERROR[4]: Not
   Enough Space`; v6 fits (~495 KB).** Pinned to `ArduinoJson @ ^6`. Watch the
   image size when adding the config-portal HTML — budget is tight.
2. **HTTPClient + chunked truncation.** `getString()` intermittently returned a
   truncated body (`IncompleteInput`) — *not* a memory issue (14.5 KB contiguous
   free). Fixed cleanly with **`http.useHTTP10(true)`** (server then replies
   un-chunked) and deserializing directly from `http.getStream()`. A retry loop
   is kept as a cheap safety net.
3. **refresh_token rotation** is real on every refresh → persist to EEPROM each
   time (the driver returns the new token to the caller).

## 8. What I need from you to build it

- ✅ **Sample `getstationsdata` JSON** — received (`netatmo_sample.json`); structure
  and units (all-metric) understood. Parser will target `NAMain.Pressure` +
  `NAModule1` outdoor temp/humidity, selected by name.
- ⏳ A **Netatmo Connect app** at runtime: `client_id` + `client_secret`, and a
  generated **`refresh_token`** with scope `read_station` (dev-portal token
  generator). These go into the **device via the config portal**, never into git.
- ⚠️ **Scrub/ignore `netatmo_sample.json` before committing** — it contains the
  account email, home GPS coordinates and `home_id`s.

---

### Sources
- Netatmo OAuth / auth changes: https://dev.netatmo.com/apidocumentation/oauth ,
  https://github.com/openhab/openhab-addons/issues/12677
- Weather API reference: https://dev.netatmo.com/en-US/resources/technical/reference/weatherstation/getstationsdata
- Refresh-token rotation: https://helpcenter.netatmo.com/hc/en-us/community/posts/19320250276626
- ESP8266 BearSSL / buffer sizes / MFLN: https://arduino-esp8266.readthedocs.io/en/latest/esp8266wifi/bearssl-client-secure-class.html
