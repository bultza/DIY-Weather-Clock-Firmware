/**
 * ESP8266 ESP-01 Clock and Weather Firmware
 *
 * This firmware uses an SSD1306 128x64 OLED display to show alternating screens:
 * - Clock screen: shows day of week, current time (HH:MM), temperature & humidity, and date.
 * - Weather screen: shows city name, current temperature, weather condition, humidity, wind speed, and pressure.
 *
 * Wi-Fi Configuration:
 * If no configuration is found or if a button on GPIO3 is held at boot, the device
 * starts as an access point (SSID: Clock-ESP01-Setup) and serves a configuration page. Password is
 * shown on display :-D
 * The page allows setting Wi-Fi SSID, password, city for weather, timezone and metric vs imperial.
 * Settings are saved in EEPROM and the device reboots to normal mode.
 *
 * Normal Operation:
 * Connects to configured Wi-Fi and retrieves time via NTP.
 * Retrieves weather from wttr.in for the specified city (HTTPS GET request).
 * Displays time and weather data on the OLED, switching screens every 15 seconds.
 *
 * Hardware:
 * - ESP8266 ESP-01 (GPIO0=SDA, GPIO2=SCL for I2C OLED; GPIO3 as input for config button).
 * - OLED display SSD1306 128x64 via I2C (address 0x3C).
 *
 * Note: Uses custom fonts (FreeMonoBold18pt7b for time, FreeMonoBold12pt7b for temperature).
 * All other text uses default font. Display is rotated 180 degrees (setRotation(2)).
 */
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>
#include <Fonts/FreeMonoBold18pt7b.h>
//#include <Fonts/FreeMonoBold16pt7b.h>
//#include <Fonts/FreeMonoBold14pt7b.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <time.h>

// Pin definitions (ESP-01):
const uint8_t SDA_PIN = 0;           // I2C SDA connected to GPIO0
const uint8_t SCL_PIN = 2;           // I2C SCL connected to GPIO2
const uint8_t BUTTON_PIN = 3;        // Button on GPIO3 (RX pin)

// EEPROM addresses for configuration data:
const int EEPROM_SIZE = 512;
const int ADDR_SSID = 0;
const int ADDR_PASS = 70;
const int ADDR_CITY = 140;
const int ADDR_TZ   = 210;  //timezone offset
const int ADDR_BOOLEANS = 300;
#define DEVICE_SIGNATURE '1'  //Change this byte to force the clock to ignore the current EEPROM configuration
const int ADDR_SIGNATURE = 500;  // 4-byte signature "CFGx" to indicate valid config, the "x" is the DEVICE_SIGNATURE define

// Wi-Fi and server:
ESP8266WebServer server(80);
const char *AP_SSID = "Clock-ESP01-Setup";  // Access Point SSID for config mode
const char *AP_PASSWD = "hackmeinnow";      //Password for the Access Point

// Display:
Adafruit_SSD1306 display(128, 64, &Wire, -1);
bool displayInitialized = false;

// Time and weather:
WiFiUDP ntpUDP;
unsigned long lastScreenSwitch = 0;
bool showWeatherScreen = false;
bool displayReady = false;
unsigned long lastWeatherFetch = 0;

// --- NTP servers ---
const char* NTP1 = "pool.ntp.org";
const char* NTP2 = "time.nist.gov";

// --- Resync period (15 minutes) ---
const unsigned long RESYNC_MS = 15UL * 60UL * 1000UL;
unsigned long lastResync = 0;

// Configuration variables:
String config_wifiSSID = "";
String config_wifiPass = "";
String config_city = "";
//int timezoneOffset = 0;  // in seconds
bool config_showSeconds = false;
bool config_imperial = false;
String config_timezone = "CET-1CEST,M3.5.0/2,M10.5.0/3";
bool config_timezone_manual = false;

// Weather data variables:
String weatherTemp = "N/A";
String weatherCond = "";
String weatherHum = "";
String weatherWind = "";
String weatherPress = "";
bool weatherValid = false;
time_t weatherLastSuccessfulUpdate = 0;

static const uint32_t REBOOT_AFTER_MS = 49UL * 24UL * 60UL * 60UL * 1000UL;
bool rebootIn10mins = false;

// Function prototypes:
void loadSettings();
void saveSettings();
void startConfigPortal(String errorMessage);
void handleConfigForm();
void drawTimeScreen();
void drawWeatherScreen();
bool getWeather();
void setupTimeWithDST();
void timeSyncCallback(struct timeval *tv);
void serialPrintTime();
void debugTZ();

void setup() 
{
  String errorMessageDisplay = "";
  Serial.begin(115200);
  Serial.println();
  Serial.println(F("Booting..."));

  // Initialize display
  Wire.begin(SDA_PIN, SCL_PIN);
  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) 
  {
    displayInitialized = true;
    display.setRotation(2);
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.print("Booting...");
    display.display();
    displayReady = true;
  } 
  else 
  {
    Serial.println(F("SSD1306 allocation failed"));
    // Leave displayInitialized as false
  }

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  // Initialize EEPROM and load stored settings if available
  EEPROM.begin(EEPROM_SIZE);
  bool configMode = false;

  // Check if config signature is present in EEPROM
  if (EEPROM.read(ADDR_SIGNATURE) == 'C' &&
      EEPROM.read(ADDR_SIGNATURE + 1) == 'F' &&
      EEPROM.read(ADDR_SIGNATURE + 2) == 'G' &&
      EEPROM.read(ADDR_SIGNATURE + 3) == DEVICE_SIGNATURE) 
  {
    Serial.println(F("Config signature found in EEPROM."));
  } 
  else 
  {
    Serial.println(F("No config signature found (EEPROM uninitialized)."));
    errorMessageDisplay = "No valid configuration found!";
  }

  // Check button press in first 300ms of boot
  bool buttonPressed = false;
  unsigned long startTime = millis();
  while (millis() - startTime < 300) 
  {
    if (digitalRead(BUTTON_PIN) == LOW) 
    {
      buttonPressed = true;
      errorMessageDisplay = "Reset button pressed!";
    }
    delay(10);
  }
  if (buttonPressed) 
  {
    Serial.println(F("Config button held - entering AP configuration mode."));
  }

  // Determine if we should start config portal
  if (EEPROM.read(ADDR_SIGNATURE) != 'C' 
    || EEPROM.read(ADDR_SIGNATURE+1) != 'F' 
    || EEPROM.read(ADDR_SIGNATURE+2) != 'G' 
    || EEPROM.read(ADDR_SIGNATURE+3) != DEVICE_SIGNATURE 
    || buttonPressed) 
  {
    configMode = true;
  }

  if (configMode) 
  {
    // Load existing settings (if any) to pre-fill form
    if (EEPROM.read(ADDR_SIGNATURE) == 'C' 
      && EEPROM.read(ADDR_SIGNATURE + 1) == 'F' 
      && EEPROM.read(ADDR_SIGNATURE + 2) == 'G' 
      && EEPROM.read(ADDR_SIGNATURE + 3) == DEVICE_SIGNATURE) 
    {
      loadSettings();
    }
    // Start configuration portal (Access Point mode)
    startConfigPortal(errorMessageDisplay);
    // After configuration, ESP will reboot. If not rebooted (e.g. user didn't submit),
    // it will remain in AP mode and handleClient in loop.
  } 
  else 
  {
    // Load settings from EEPROM
    loadSettings();
    Serial.print(F("Connecting to WiFi: "));
    Serial.println(config_wifiSSID);
    display.println("Connecting to WiFi...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(config_wifiSSID.c_str(), config_wifiPass.c_str());
    // Wait up to 30 seconds for connection
    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 30000) 
    {
      delay(500);
      Serial.print('.');
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) 
    {
      Serial.println(F("WiFi connected."));
      Serial.print(F("IP Address: "));
      Serial.println(WiFi.localIP());
    } 
    else 
    {
      Serial.println(F("WiFi connection failed. Starting AP mode instead."));
      startConfigPortal("WiFi connection failed.");
      rebootIn10mins = true;
      return; // Exit setup to avoid running normal mode without WiFi
    }
    setupTimeWithDST();

    // Prepare first weather fetch
    lastWeatherFetch = 0; // force immediate fetch on first weather screen display
    weatherValid = false;
    Serial.println(F("Setup complete, entering loop."));
  }
  Serial.println(F("Getting initial weather..."));
  weatherValid = getWeather();
  lastWeatherFetch = millis();
  if (weatherValid) 
  {
    Serial.println(F("Initial weather fetch successful."));
  } 
  else 
  {
    Serial.println(F("Initial weather fetch failed."));
  } 
}

void loop() 
{
  time_t now = millis();

  //Reboot after 49 days of continues use, to avoid millis() rollover problems :-D
  if (now > REBOOT_AFTER_MS) 
  {
    Serial.println(F("Uptime > 49 days, rebooting to avoid millis() rollover"));
    delay(100);
    ESP.restart();
  }
  // If in config portal mode, handle web server
  if (WiFi.getMode() == WIFI_AP) 
  {
    //If more than 10min reboot:
    if(rebootIn10mins)
    {
      if (now > 600000)
      {
        Serial.println(F("Uptime > 10 minutes and no Wifi was connected so we reboot..."));
        delay(100);
        ESP.restart();
      }
    }
    server.handleClient();
    // In AP mode, do not run normal display loop
    return;
  }

  // Switch screen every 15 seconds
  if (now - lastScreenSwitch > 15000) 
  {
    //Debug time:
    serialPrintTime();
    //debugTZ();
    showWeatherScreen = !showWeatherScreen;
    lastScreenSwitch = now;
    // If switching to weather screen, update weather data (limit fetch frequency)
    if (showWeatherScreen) 
    {
      // Update weather every 15 minutes
      if (millis() - lastWeatherFetch > 900000UL /*|| !weatherValid*/) 
      {
        Serial.println(F("Updating weather data..."));
        weatherValid = getWeather();
        lastWeatherFetch = millis();
        if (weatherValid) 
        {
          Serial.println(F("Weather update successful."));
        } 
        else 
        {
          Serial.println(F("Weather update failed or data invalid."));
        }
      }
    }
  }

  // Draw the appropriate screen
  if (showWeatherScreen && weatherValid) 
  {
    drawWeatherScreen();
  } 
  else 
  {
    drawTimeScreen();
  }

  // Small delay to yield to system
  delay(10);
}

void serialPrintTime()
{
  //Debug function to show the time via Serial
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo)) {
    Serial.println("Time is: <not set>");
    return;
  }

  char buf[40];
  strftime(
    buf,
    sizeof(buf),
    "%Y-%m-%d %H:%M:%S %Z",
    &timeinfo
  );

  Serial.println(buf);
}

// Callback when SNTP sets time
void timeSyncCallback(struct timeval *tv) 
{
  Serial.println("SNTP: time synchronized.");
}

void setupTimeWithDST() 
{
  const char* tz = "UTC0";
  if (config_timezone.length() > 0) 
  {
    tz = config_timezone.c_str();
  }

  Serial.print("Setting TZ via configTzTime(): ");
  Serial.println(tz);

  configTzTime(tz, NTP1, NTP2);
}

void debugTZ()
{
  Serial.print("config_timezone (String): '");
  Serial.print(config_timezone);
  Serial.println("'");

  const char* tzEnv = getenv("TZ");
  Serial.print("getenv('TZ'): '");
  Serial.print(tzEnv ? tzEnv : "<null>");
  Serial.println("'");

  time_t now = time(nullptr);

  struct tm local;
  localtime_r(&now, &local);

  char buf[64];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &local);
  Serial.print("localtime: ");
  Serial.println(buf);

  // numeric offset proof (local - utc)
  struct tm utc;
  gmtime_r(&now, &utc);

  long offset =
      (local.tm_hour - utc.tm_hour) * 3600 +
      (local.tm_min  - utc.tm_min)  * 60;

  if (local.tm_yday != utc.tm_yday) {
    offset += (local.tm_yday > utc.tm_yday) ? 86400 : -86400;
  }

  Serial.print("UTC offset (s): ");
  Serial.println(offset);
}

static String htmlEscape(const String& s)
{
  String out;
  out.reserve(s.length());

  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&':  out += F("&amp;");  break;
      case '<':  out += F("&lt;");   break;
      case '>':  out += F("&gt;");   break;
      case '"':  out += F("&quot;"); break;
      case '\'': out += F("&#39;");  break;
      default:   out += c;           break;
    }
  }
  return out;
}

void startConfigPortal(String errorMessage)
{
  // Stop any existing WiFi and start AP
  // Password is in AP_PASSWD
  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWD);
  IPAddress apIP = WiFi.softAPIP();
  Serial.print(F("Started AP mode with SSID "));
  Serial.print(AP_SSID);
  Serial.print(F(". Connect and browse to http://"));
  Serial.println(apIP);

  if (displayReady) 
  {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println(errorMessage);
    display.setCursor(0, 10);
    display.println("AP mode");
    display.setCursor(0, 20);
    display.print("SSID: ");
    display.println(AP_SSID);
    display.setCursor(0, 30);
    display.print("PASSWD: ");
    display.println(AP_PASSWD);
    display.setCursor(0, 40);
    display.print("IP: ");
    display.println(apIP);
    display.display();
  }
  // Setup web server routes
  server.on("/", HTTP_GET, []() 
  {
    // HTML page for config
    String page = "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
    page += "<title>Clock ESP Setup</title>";

    // --- Improved CSS ---
    page += "<style>";
    page += "body{margin:0;background:#f0f2f5;font-family:sans-serif;}";
    page += ".container{max-width:520px;margin:32px auto;padding:22px;background:#fff;border:1px solid #e0e0e0;border-radius:10px;box-shadow:0 2px 10px rgba(0,0,0,.06);}";
    page += "h2{margin:0 0 18px;text-align:center;color:#222;}";
    page += "form{margin:0;}";
    page += ".row{display:grid;grid-template-columns:160px 1fr;gap:10px;align-items:center;margin:10px 0;}";
    page += "label{color:#333;font-size:14px;}";
    page += "input[type=text],input[type=password],select{width:100%;padding:10px;border:1px solid #ccc;border-radius:8px;box-sizing:border-box;font-size:14px;}";
    page += ".checkwrap{display:flex;align-items:center;gap:10px;}";
    page += "input[type=checkbox]{transform:scale(1.15);}";
    page += ".radiowrap{display:flex;gap:18px;align-items:center;flex-wrap:wrap;}";
    page += ".radioopt{display:flex;align-items:center;gap:8px;}";
    page += ".btn{margin-top:16px;width:100%;padding:12px;border:0;border-radius:8px;background:#4caf50;color:#fff;font-size:16px;cursor:pointer;}";
    page += ".btn:hover{background:#45a049;}";
    page += "@media(max-width:520px){.row{grid-template-columns:1fr;}}";
    page += "</style>";

    page += "</head><body><div class='container'>";
    page += "<h2>Device Configuration</h2><form method='POST' action='/'>";

    // WiFi SSID field
    page += "<div class='row'><label for='ssid'>Wi-Fi SSID:</label>";
    page += "<input id='ssid' type='text' name='ssid' value='" + htmlEscape(config_wifiSSID) + "' required></div>";

    // Password field
    page += "<div class='row'><label for='pass'>Password:</label>";
    page += "<input id='pass' type='password' name='pass' value='" + htmlEscape(config_wifiPass) + "' placeholder=''></div>";

    // City field
    page += "<div class='row'><label for='city'>City:</label>";
    page += "<input id='city' type='text' name='city' value='" + htmlEscape(config_city) + "' required></div>";

    // Timezone select
    page += "<div class='row'><label for='tz'>Timezone:</label>";
    page += "<select name='tz' id='tz' onchange='toggleTZManual()'>";

    // Basic presets (keep it simple)
    page += "<option value='UTC0'";
    if (config_timezone == "UTC0" && !config_timezone_manual) page += " selected";
    page += ">UTC</option>";

    page += "<option value='CET-1CEST,M3.5.0/2,M10.5.0/3'";
    if (config_timezone == "CET-1CEST,M3.5.0/2,M10.5.0/3" && !config_timezone_manual) page += " selected";
    page += ">Central Europe (CET/CEST)</option>";

    page += "<option value='GMT0BST,M3.5.0/1,M10.5.0/2'";
    if (config_timezone == "GMT0BST,M3.5.0/1,M10.5.0/2" && !config_timezone_manual) page += " selected";
    page += ">UK / Ireland (GMT/BST)</option>";

    page += "<option value='EST5EDT,M3.2.0/2,M11.1.0/2'";
    if (config_timezone == "EST5EDT,M3.2.0/2,M11.1.0/2" && !config_timezone_manual) page += " selected";
    page += ">US Eastern (EST/EDT)</option>";

    page += "<option value='PST8PDT,M3.2.0/2,M11.1.0/2'";
    if (config_timezone == "PST8PDT,M3.2.0/2,M11.1.0/2" && !config_timezone_manual) page += " selected";
    page += ">US Pacific (PST/PDT)</option>";

    // Manual option
    page += "<option value='MANUAL'";
    if (config_timezone_manual) page += " selected";
    page += ">Manual (advanced)</option>";

    page += "</select></div>";

    // Manual TZ row (grid row so it aligns with everything else)
    page += "<div class='row' id='tz_manual_row' style='";
    if (!config_timezone_manual) page += "display:none;";
    page += "'>";
    page += "<label for='tz_manual'>Manual TZ string:</label>";
    page += "<input id='tz_manual' type='text' name='tz_manual' ";
    page += "placeholder='e.g. CET-1CEST,M3.5.0/2,M10.5.0/3' ";
    // If manual is enabled, show the stored TZ string; otherwise keep it empty
    page += "value='";
    if (config_timezone_manual) page += htmlEscape(config_timezone);
    page += "'>";
    page += "</div>";

    // Show seconds checkbox (aligned)
    page += "<div class='row'><label>Show seconds:</label>";
    page += "<div class='checkwrap'>";
    page += "<input type='checkbox' name='showseconds' value='1'";
    if (config_showSeconds) page += " checked";
    page += ">";
    page += "<span></span>";
    page += "</div></div>";

    // Units radio (aligned)
    page += "<div class='row'><label>Units:</label>";
    page += "<div class='radiowrap'>";

    page += "<label class='radioopt'><input type='radio' name='units' value='metric'";
    if (!config_imperial) page += " checked";
    page += ">Metric</label>";

    page += "<label class='radioopt'><input type='radio' name='units' value='imperial'";
    if (config_imperial) page += " checked";
    page += ">Imperial</label>";

    page += "</div></div>";

    // JS (toggle + initial state)
    page += "<script>";
    page += "function toggleTZManual(){";
    page += "var tz=document.getElementById('tz').value;";
    page += "document.getElementById('tz_manual_row').style.display = (tz==='MANUAL')?'grid':'none';";
    page += "}";
    page += "toggleTZManual();";
    page += "</script>";

    // Submit button
    page += "<button class='btn' type='submit'>Save</button>";
    page += "</form></div></body></html>";

    server.send(200, "text/html", page);
  });

  server.on("/", HTTP_POST, handleConfigForm);
  server.begin();
  Serial.println(F("HTTP server started for config portal."));
}

void handleConfigForm() 
{
  // Get form values
  String ssid     = server.arg("ssid");
  String pass     = server.arg("pass");
  String newCity  = server.arg("city");
  String tzSelect = server.arg("tz");          // preset or "MANUAL"
  String tzManual = server.arg("tz_manual");   // only meaningful if MANUAL

  // New fields
  bool showSeconds = server.hasArg("showseconds");  // checkbox: present => true
  String units     = server.arg("units");           // "metric" or "imperial"

  Serial.println(F("Received configuration:"));
  Serial.print(F("SSID: ")); Serial.println(ssid);
  Serial.print(F("Password: ")); Serial.println(pass);
  Serial.print(F("City: ")); Serial.println(newCity);
  Serial.print(F("Timezone select: ")); Serial.println(tzSelect);
  Serial.print(F("Timezone manual: ")); Serial.println(tzManual);
  Serial.print(F("Show seconds: ")); Serial.println(showSeconds ? "true" : "false");
  Serial.print(F("Units: ")); Serial.println(units);

  // Basic validation
  if (ssid.length() == 0 || newCity.length() == 0 || tzSelect.length() == 0) 
  {
    server.send(400, "text/html",
      "<html><body><h3>Invalid input, please fill all required fields.</h3></body></html>");
    return;
  }

  // Units validation (default to metric if missing)
  bool imperial = false;
  if (units == "imperial") imperial = true;
  else imperial = false; // includes "metric" or empty

  // Timezone handling
  String finalTZ;
  bool tzIsManual = false;

  if (tzSelect == "MANUAL") 
  {
    // Require manual string if manual mode
    if (tzManual.length() == 0) 
    {
      server.send(400, "text/html",
        "<html><body><h3>Manual timezone selected but TZ string is empty.</h3></body></html>");
      return;
    }
    finalTZ = tzManual;
    tzIsManual = true;
  } 
  else 
  {
    finalTZ = tzSelect;   // preset string
    tzIsManual = false;
  }

  // Save into config variables
  config_wifiSSID = ssid;
  config_wifiPass = pass;
  config_city = newCity;

  config_timezone = finalTZ;
  config_timezone_manual = tzIsManual;

  config_showSeconds = showSeconds;
  config_imperial = imperial;

  // Persist to EEPROM/NVS
  saveSettings();

  // Response + reboot
  server.send(200, "text/html",
    "<html><body><h3>Settings saved. Rebooting...</h3></body></html>");
  delay(800);
  ESP.restart();
}

static String eepromReadString(int baseAddr, int maxLen) 
{
  uint8_t len = EEPROM.read(baseAddr);
  if (len == 0 || len == 0xFF) return "";
  if (len > maxLen) len = maxLen;

  char buf[128];              // enough for our maxLen values below
  for (int i = 0; i < len; i++) 
  {
    buf[i] = char(EEPROM.read(baseAddr + 1 + i));
  }
  buf[len] = '\0';
  return String(buf);
}

void loadSettings() 
{
  // --- Check signature "CFGx" ---
  char s0 = char(EEPROM.read(ADDR_SIGNATURE));
  char s1 = char(EEPROM.read(ADDR_SIGNATURE + 1));
  char s2 = char(EEPROM.read(ADDR_SIGNATURE + 2));
  char s3 = char(EEPROM.read(ADDR_SIGNATURE + 3));

  bool sigOk = (s0 == 'C' && s1 == 'F' && s2 == 'G' && s3 == DEVICE_SIGNATURE);

  if (!sigOk) 
  {
    // Defaults if EEPROM not initialized for this version
    config_wifiSSID = "";
    config_wifiPass = "";
    config_city = "";

    config_timezone = "UTC0";
    config_timezone_manual = false;

    config_showSeconds = false;
    config_imperial = false;

    Serial.println(F("EEPROM signature invalid or mismatched. Using defaults."));
    return;
  }

  // Max lengths must match what you used when saving (or smaller)
  const int MAX_SSID = 60;
  const int MAX_PASS = 60;
  const int MAX_CITY = 60;
  const int MAX_TZ   = 80;

  // --- Read strings ---
  config_wifiSSID = eepromReadString(ADDR_SSID, MAX_SSID);
  config_wifiPass = eepromReadString(ADDR_PASS, MAX_PASS);
  config_city     = eepromReadString(ADDR_CITY, MAX_CITY);
  config_timezone = eepromReadString(ADDR_TZ,   MAX_TZ);

  // --- Read packed booleans ---
  uint8_t flags = EEPROM.read(ADDR_BOOLEANS);
  config_timezone_manual = (flags & (1 << 0)) != 0;
  config_showSeconds     = (flags & (1 << 1)) != 0;
  config_imperial        = (flags & (1 << 2)) != 0;

  // Safety fallback if timezone string somehow empty
  if (config_timezone.length() == 0) 
  {
    config_timezone = "UTC0";
    config_timezone_manual = false;
  }

  Serial.println(F("Configuration loaded from EEPROM."));
  Serial.print(F("SSID: ")); Serial.println(config_wifiSSID);
  Serial.print(F("Password: ")); Serial.println(config_wifiPass);
  Serial.print(F("City: ")); Serial.println(config_city);
  Serial.print(F("Timezone select: ")); Serial.println(config_timezone);
  Serial.print(F("Timezone manual: ")); Serial.println(config_timezone_manual ? "true" : "false");
  Serial.print(F("Show seconds: ")); Serial.println(config_showSeconds ? "true" : "false");
  Serial.print(F("Units imperial: ")); Serial.println(config_imperial ? "true" : "false");
}

// Helper: write a length-prefixed string into EEPROM and clear leftover bytes
static void eepromWriteString(int baseAddr, const String& s, int maxLen) 
{
  int len = s.length();
  if (len > maxLen) len = maxLen;

  EEPROM.write(baseAddr, (uint8_t)len);
  for (int i = 0; i < len; i++) 
  {
    EEPROM.write(baseAddr + 1 + i, (uint8_t)s[i]);
  }
  // Clear remaining bytes (avoid stale data if new string is shorter)
  for (int i = len; i < maxLen; i++) 
  {
    EEPROM.write(baseAddr + 1 + i, 0);
  }
}

void saveSettings() 
{
  // Define max lengths so we don't run outside EEPROM sections.
  // With your spacing (0,70,140,210,300,500) you have 69 bytes payload for each string region if you want.
  // We use max 60 to leave margin.
  const int MAX_SSID = 60;
  const int MAX_PASS = 60;
  const int MAX_CITY = 60;
  const int MAX_TZ   = 80; // TZ strings can be longer; you have room from 210 to 299 (~89 bytes)

  // Write SSID / PASS / CITY / TZ
  eepromWriteString(ADDR_SSID, config_wifiSSID, MAX_SSID);
  eepromWriteString(ADDR_PASS, config_wifiPass, MAX_PASS);
  eepromWriteString(ADDR_CITY, config_city,     MAX_CITY);
  eepromWriteString(ADDR_TZ,   config_timezone, MAX_TZ);   // NEW: store TZ string

  // Pack booleans into one byte
  uint8_t flags = 0;
  if (config_timezone_manual) flags |= (1 << 0);
  if (config_showSeconds)     flags |= (1 << 1);
  if (config_imperial)        flags |= (1 << 2);

  EEPROM.write(ADDR_BOOLEANS, flags);

  // Optional: clear next few bytes for future expansion (safe)
  EEPROM.write(ADDR_BOOLEANS + 1, 0);
  EEPROM.write(ADDR_BOOLEANS + 2, 0);
  EEPROM.write(ADDR_BOOLEANS + 3, 0);

  // Write signature 'CFGx'
  EEPROM.write(ADDR_SIGNATURE,     'C');
  EEPROM.write(ADDR_SIGNATURE + 1, 'F');
  EEPROM.write(ADDR_SIGNATURE + 2, 'G');
  EEPROM.write(ADDR_SIGNATURE + 3, DEVICE_SIGNATURE);

  EEPROM.commit();
  Serial.println(F("Configuration saved to EEPROM."));
}

static String sanitizeWindForDisplay(const String& w)
{
  // Extract digits, optional sign, and optional decimal point
  String out;
  out.reserve(w.length());

  bool dotUsed = false;

  for (size_t i = 0; i < w.length(); i++) {
    char c = w[i];

    if ((c >= '0' && c <= '9') || c == '+' || c == '-') {
      out += c;
    } else if (c == '.' && !dotUsed) {
      out += c;
      dotUsed = true;
    }
  }

  out.trim();
  return out;
}

static String sanitizeTempForDisplay(const String& t)
{
  // Keep only: digits, leading +/-, and optionally one dot
  String out;
  out.reserve(t.length());

  bool dotUsed = false;

  for (size_t i = 0; i < t.length(); i++) {
    char c = t[i];
    if ((c >= '0' && c <= '9') || c == '+' || c == '-') {
      out += c;
    } else if (c == '.' && !dotUsed) {
      out += c;
      dotUsed = true;
    }
  }
  out.trim();
  return out;
}

void drawTimeScreen() 
{
  if (!displayInitialized) 
  {
    return;
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  // Day name at top center
  // Get current epoch time and derive day of week
  //time_t epoch = timeClient.getEpochTime();
  // Calculate day of week (0=Sunday .. 6=Saturday)
  struct tm timeinfo;
  getLocalTime(&timeinfo);

  int wday = timeinfo.tm_wday;  // tm_wday: days since Sunday (0-6)
  String dayName = "";
  switch(wday) 
  {
    case 0: dayName = "Sunday"; break;
    case 1: dayName = "Monday"; break;
    case 2: dayName = "Tuesday"; break;
    case 3: dayName = "Wednesday"; break;
    case 4: dayName = "Thursday"; break;
    case 5: dayName = "Friday"; break;
    case 6: dayName = "Saturday"; break;
  }
  display.setFont(NULL); // default font
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(dayName, 0, 0, &x1, &y1, &w, &h);
  // center horizontally
  int dayX = (128 - w) / 2;
  display.setCursor(dayX, 0);
  display.print(dayName);
  if(config_showSeconds)
  {
    // Time HH:MM in large font, centered
    display.setFont(&FreeMonoBold12pt7b);
    // Format time as HH:MM:SS
    char timeBuf[9];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    String timeStr = String(timeBuf);
    display.getTextBounds(timeStr, 0, 30, &x1, &y1, &w, &h);
    int timeX = (128 - w) / 2;
    // Vertically center the text around mid (y=32)
    int timeY = 32 + (h / 2);
    display.setCursor(timeX, timeY);
    display.print(timeStr);
  }
  else
  {
    // Time HH:MM in large font, centered
    display.setFont(&FreeMonoBold18pt7b);
    // Format time as HH:MM
    char timeBuf[6];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    String timeStr = String(timeBuf);
    display.getTextBounds(timeStr, 0, 30, &x1, &y1, &w, &h);
    int timeX = (128 - w) / 2;
    // Vertically center the text around mid (y=32)
    int timeY = 32 + (h / 2);
    display.setCursor(timeX, timeY);
    display.print(timeStr);
  }

  // Bottom left: temperature and humidity
  display.setFont(NULL);
  display.setCursor(0, 56);
  if (weatherValid && weatherTemp != "N/A" && weatherHum != "") 
  {
    String tempDisplay = sanitizeTempForDisplay(weatherTemp);
    display.print(tempDisplay);
    display.print((char)247); // degree symbol (your display's charset)
    display.print(config_imperial ? "F  " : "C  ");
    display.print(weatherHum);
  } 
  else 
  {
    display.print("N/A");
  }

  // Bottom right: date dd.mm.yyyy
  int day = timeinfo.tm_mday;
  int month = timeinfo.tm_mon + 1;
  int year = timeinfo.tm_year + 1900;
  char dateBuf[12];
  snprintf(dateBuf, sizeof(dateBuf), "%02d/%02d/%04d", day, month, year);
  String dateStr = String(dateBuf);
  display.getTextBounds(dateStr, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(128 - w, 56);
  display.print(dateStr);

  display.display();
}

void drawWeatherScreen() 
{
  if (!displayInitialized) 
  {
    return;
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setFont(NULL);
  // Top center: city name
  String cityName = config_city;
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(cityName, 0, 0, &x1, &y1, &w, &h);
  int cityX = (128 - w) / 2;
  display.setCursor(cityX, 0);
  display.print(cityName);
  // Middle center: temperature (big font FreeMonoBold12pt7b)
  display.setFont(&FreeMonoBold12pt7b);
  if (weatherValid && weatherTemp != "N/A") 
  {
    // weatherTemp is e.g. "+12" or "-3" as string (cleaned)
    String tempNum = sanitizeTempForDisplay(weatherTemp);
    // Center the numeric part
    display.getTextBounds(tempNum, 0, 30, &x1, &y1, &w, &h);
    int tempX = (128 - (w + 12)) / 2; // leave space for degree and C (~12px)
    int tempY = 30; // baseline for 12pt font around mid screen
    display.setCursor(tempX, tempY);
    display.print(tempNum);
    
    // Now draw degree symbol and 'C' in default font after the number
    // Place small degree symbol near top of big text and 'C' or 'F' after it
    int degX = tempX + w + 3; // position degree just right of number
    int degY = tempY - 16;    // raise small text (approx half big font height)
    if (degY < 0) degY = 0;
    display.setCursor(degX, degY);
    //display.print((char)247);
    display.drawCircle(degX, degY, 2, SSD1306_WHITE);
    display.setCursor(degX + 3, tempY);
    display.setFont(&FreeMonoBold12pt7b);    
    display.print(config_imperial ? "F " : "C ");
    display.setFont(NULL);
  } else {
    // If weather not available, show N/A in big font
    String na = "N/A";
    display.getTextBounds(na, 0, 30, &x1, &y1, &w, &h);
    int naX = (128 - w) / 2;
    display.setCursor(naX, 30);
    display.print(na);
    // No degree symbol or 'C' in this case
  }
  // Below temperature: condition string (centered)
  display.setFont(NULL);
  String cond = weatherValid ? weatherCond : "";
  cond.trim();
  display.getTextBounds(cond, 0, 40, &x1, &y1, &w, &h);
  int condX = (128 - w) / 2;
  display.setCursor(condX, 40);
  display.print(cond);
  // Bottom right: H:% W:m/s P:mm
  display.setFont(NULL);
  if (weatherValid && weatherTemp != "N/A") 
  {
    String windDisplay = sanitizeWindForDisplay(weatherWind);
    String bottomStr = String("H:") + weatherHum;
    bottomStr += " " + windDisplay;
    bottomStr += (config_imperial ? "mph" : "km/h");
    //if (weatherWind != "N/A") bottomStr += "km/h";
    bottomStr += " " + weatherPress;
    display.getTextBounds(bottomStr, 0, 56, &x1, &y1, &w, &h);
    display.setCursor((128 - w)/2, 56);
    display.print(bottomStr);
  } else {
    String bottomStr = "H:N/A W:N/A P:N/A";
    display.getTextBounds(bottomStr, 0, 56, &x1, &y1, &w, &h);
    display.setCursor(128 - w, 56);
    display.print(bottomStr);
  }
  display.display();
}

static String urlEncode(const String& s)
{
  //Function to safely encode the names of the Cities
  String out;
  out.reserve(s.length() * 3);

  const char* hex = "0123456789ABCDEF";

  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];

    // Unreserved per RFC3986: ALPHA / DIGIT / "-" / "." / "_" / "~"
    if ((c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') ||
        c == '-' || c == '.' || c == '_' || c == '~') {
      out += char(c);
    } else if (c == ' ') {
      out += "%20";
    } else {
      out += '%';
      out += hex[(c >> 4) & 0x0F];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

bool getWeatherExpired()
{
  //This function is called only when the getWeather from wttr.in failed. If we have
  //good weather from previous calls (less than 90minutes ago) we do not declare it
  //as not valid
  time_t now = millis();

  if(weatherLastSuccessfulUpdate == 0)
  {
    Serial.println("Fetching weather data failed and we never got a valid data :(. Expired!");
    return false; // We never got a valid weather :(
  }

  if(weatherLastSuccessfulUpdate + 5400000UL > now)
  {
    Serial.println("Fetching weather data failed but previous stored data did not expired. So we keep the data :-D");
    Serial.print(F("Temp=")); Serial.println(weatherTemp);
    Serial.print(F("Cond=")); Serial.println(weatherCond);
    Serial.print(F("Hum=")); Serial.println(weatherHum);
    Serial.print(F("Wind=")); Serial.println(weatherWind);
    Serial.print(F("Pressure=")); Serial.println(weatherPress);
    return true;  // Last update was < 90 minutes ago, so it is still valid.
  }

  Serial.println("Weather data expired! :((((");
  return false;
}

bool getWeather() 
{
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("WiFi not connected, cannot get weather."));
    return getWeatherExpired();
  }

  WiFiClientSecure client;
  client.setInsecure(); // Skip certificate validation for simplicity
  const char* host = "wttr.in";
  // Encode city only for URL
  String cityEnc = urlEncode(config_city);
  // Base query
  String query = "?format=%25t|%25C|%25h|%25w|%25P";
  // Add imperial units if configured
  if (config_imperial) 
  {
    query += "&u";
  }
  String url = "/" + cityEnc + query;

  Serial.print(F("Connecting to weather server '"));
  Serial.print(host);
  Serial.print(F("', with URL: '"));
  Serial.print(url);
  Serial.println(F("'"));

  client.setTimeout(10000);          // read timeout (ms)
  //client.setHandshakeTimeout(15);    // TLS handshake (s)

  if (!client.connect(host, 443)) 
  {
    Serial.println(F("Connection failed."));
    return getWeatherExpired();
  }

  // Send GET request
  client.print(String("GET ") + url + " HTTP/1.1\r\n" +
               "Host: " + host + "\r\n" +
               "User-Agent: ESP8266\r\n" +
               "Connection: close\r\n\r\n");
 // Read full response as raw text
  String response = "";
  unsigned long timeout = millis() + 15000;
  while (millis() < timeout && client.connected()) 
  {
    while (client.available()) 
    {
      char c = client.read();
      response += c;
    }
  }
  client.stop();

  Serial.println(F("---- RAW RESPONSE ----"));
  Serial.println(response);
  Serial.println(F("----------------------"));

  // --- Extract line with weather data ---
  String result = "";
  int from = 0;
  while (from >= 0) {
    int to = response.indexOf('\n', from);
    if (to == -1) break;
    String line = response.substring(from, to);
    line.replace("\r", "");
    line.trim();

    Serial.print("DEBUG LINE: >");
    Serial.print(line);
    Serial.println("<");

    if (line.indexOf('|') != -1) {
      result = line;
      break;
    }

    from = to + 1;
  }

  // If no \n at the end — catch the remaining part
  if (result.length() == 0 && from < response.length()) {
    String line = response.substring(from);
    line.replace("\r", "");
    line.trim();
    Serial.print("FALLBACK LINE: >");
    Serial.print(line);
    Serial.println("<");
    if (line.indexOf('|') != -1) {
      result = line;
    }
  }

  Serial.print(F("Weather raw response: "));
  Serial.println(result);

  if (result.length() == 0) {
    Serial.println(F("No weather data found."));
    return getWeatherExpired();
  }

  // Parse fields: temp|cond|hum|wind|press
  int idx1 = result.indexOf('|');
  int idx2 = result.indexOf('|', idx1 + 1);
  int idx3 = result.indexOf('|', idx2 + 1);
  int idx4 = result.indexOf('|', idx3 + 1);
  if (idx1 < 0 || idx2 < 0 || idx3 < 0 || idx4 < 0) 
    return getWeatherExpired();

  String tempStr   = result.substring(0, idx1);
  String condStr   = result.substring(idx1 + 1, idx2);
  String humStr    = result.substring(idx2 + 1, idx3);
  String windStr   = result.substring(idx3 + 1, idx4);
  String pressStr  = result.substring(idx4 + 1);

  // Clean Condition:
  tempStr.trim();
  condStr.trim();
  // Clean Humidity: ensure '%' present
  humStr.trim();

  if (windStr.length() == 0) 
  {
    windStr = "N/A";
  } 

  if(pressStr.length() == 0) 
  {
    pressStr = "N/A";
  } 

  Serial.println(F("Parsed weather data:"));
  Serial.print(F("Temp=")); Serial.println(tempStr);
  Serial.print(F("Cond=")); Serial.println(condStr);
  Serial.print(F("Hum=")); Serial.println(humStr);
  Serial.print(F("Wind=")); Serial.println(windStr);
  Serial.print(F("Pressure=")); Serial.println(pressStr);

  // Validate critical fields
  if (tempStr == "" || condStr == "") 
  {
    return getWeatherExpired();
  }

  //Data is valid!!!

  weatherTemp = tempStr; // keep + or - sign if present and the Degree
  weatherCond = condStr;
  weatherHum = humStr;
  weatherWind = windStr;
  weatherPress = pressStr;
  weatherLastSuccessfulUpdate = millis();   //Register when we did get the last weather update

  return true;
}