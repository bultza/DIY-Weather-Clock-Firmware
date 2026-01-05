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
#include <Fonts/TomThumb.h> //Smallest font available
#include <Fonts/FreeSans9pt7b.h>  //Small cute font
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
const int ADDR_VARIABLES = 300;
#define DEVICE_SIGNATURE '2'  //Change this byte to force the clock to ignore the current EEPROM configuration
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
String   config_wifiSSID          = "";
String   config_wifiPass          = "";
String   config_city              = "";
bool     config_showSeconds       = false;
bool     config_imperial          = false;
String   config_timezone          = "CET-1CEST,M3.5.0/2,M10.5.0/3";
bool     config_timezone_manual   = false;
bool     config_variableContrast  = false;
bool     config_contrastFollowSun = false;
uint8_t  config_dayContrast       = 255;
uint8_t  config_nightContrast     = 1;
uint16_t config_dawnDuskDuration  = 30;  //minutes
uint32_t config_sunRise           = 25200;  //in seconds 7h in the morning
uint32_t config_sunSet            = 75600;   //in seconds 21h at night

// Weather data variables:
String weather_temp    = "N/A";
String weather_cond    = "";
String weather_hum     = "";
String weather_wind    = "";
String weather_press   = "";
bool   weather_valid   = false;
time_t weather_lastSuccessfulUpdate = 0;
String weather_sundawn = "07:00";
String weather_sunrise = "07:30";
String weather_sunset  = "19:00";
String weather_sundusk = "19:30";

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
void setDisplayBrightness(uint8_t contrast);
uint8_t calculateDisplayBrightness();

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
    display.println("Booting...");
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
    display.display();
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
      startConfigPortal("WiFi connect failed!");
      rebootIn10mins = true;
      return; // Exit setup to avoid running normal mode without WiFi
    }
    setupTimeWithDST();

    // Prepare first weather fetch
    lastWeatherFetch = 0; // force immediate fetch on first weather screen display
    weather_valid = false;
    Serial.println(F("Setup complete, entering loop."));
  }
  Serial.println(F("Fetching initial weather..."));
  display.println("Fetching weather...");
  display.display();
  weather_valid = getWeather();
  lastWeatherFetch = millis();
  if (weather_valid) 
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
  uint32_t now = millis();

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

    /*//Debug:
    uint32_t timeBridgness = (now / 10) % 255;
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    setDisplayBrightness(timeBridgness);
    display.setCursor(0, 0);
    display.setTextSize(1);
    display.println("HOLA");
    display.println(timeBridgness);
    display.println("======");
    display.display();
    Serial.println(timeBridgness);*/
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
        weather_valid = getWeather();
        lastWeatherFetch = millis();
        if (weather_valid) 
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
  if (showWeatherScreen && weather_valid) 
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

static String formatHHMM(uint32_t secondsOfDay)
{
  // Clamp to 0..86399 just in case
  if (secondsOfDay > 86399UL) secondsOfDay = 0;

  uint32_t h = secondsOfDay / 3600UL;
  uint32_t m = (secondsOfDay % 3600UL) / 60UL;

  char buf[6];
  snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)h, (unsigned long)m);
  return String(buf);
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
    //display.setFont(&TomThumb); //the smallest font available
    //display.setCursor(0, 16);
    display.setCursor(0, 0);
    display.setTextSize(1);
    //display.print("Error message: ");
    display.println(errorMessage);
    display.println("----------------");
    display.println("AP mode for config");
    //display.setCursor(0, 20);
    display.print("SSID: ");
    display.println(AP_SSID);
    //display.setCursor(0, 30);
    display.print("PASSWD: ");
    display.println(AP_PASSWD);
    //display.setCursor(0, 40);
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
    page += "input[type=text],input[type=password],input[type=number],select{width:100%;padding:10px;border:1px solid #ccc;border-radius:8px;box-sizing:border-box;font-size:14px;}";
    page += ".checkwrap{display:flex;align-items:center;gap:10px;}";
    page += "input[type=checkbox]{transform:scale(1.15);}";
    page += ".radiowrap{display:flex;gap:18px;align-items:center;flex-wrap:wrap;}";
    page += ".radioopt{display:flex;align-items:center;gap:8px;}";
    page += ".btn{margin-top:16px;width:100%;padding:12px;border:0;border-radius:8px;background:#4caf50;color:#fff;font-size:16px;cursor:pointer;}";
    page += ".btn:hover{background:#45a049;}";
    page += ".section{margin-top:16px;padding-top:10px;border-top:1px solid #eee;}";
    page += ".hint{font-size:12px;color:#666;line-height:1.3;}";
    page += "@media(max-width:520px){.row{grid-template-columns:1fr;}}";
    page += "</style>";

    page += "</head><body><div class='container'>";
    page += "<h2>Device Configuration</h2>";
    page += "<form id='cfgform' method='POST' action='/'>";

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

    page += "<option value='MANUAL'";
    if (config_timezone_manual) page += " selected";
    page += ">Manual (advanced)</option>";

    page += "</select></div>";

    // Manual TZ row
    page += "<div class='row' id='tz_manual_row' style='";
    if (!config_timezone_manual) page += "display:none;";
    page += "'>";
    page += "<label for='tz_manual'>Manual TZ string:</label>";
    page += "<input id='tz_manual' type='text' name='tz_manual' ";
    page += "placeholder='e.g. CET-1CEST,M3.5.0/2,M10.5.0/3' ";
    page += "value='";
    if (config_timezone_manual) page += htmlEscape(config_timezone);
    page += "'>";
    page += "</div>";

    // Show seconds checkbox
    page += "<div class='row'><label>Show seconds:</label>";
    page += "<div class='checkwrap'>";
    page += "<input type='checkbox' name='showseconds' value='1'";
    if (config_showSeconds) page += " checked";
    page += ">";
    page += "<span></span>";
    page += "</div></div>";

    // Units radio
    page += "<div class='row'><label>Units:</label>";
    page += "<div class='radiowrap'>";

    page += "<label class='radioopt'><input type='radio' name='units' value='metric'";
    if (!config_imperial) page += " checked";
    page += ">Metric</label>";

    page += "<label class='radioopt'><input type='radio' name='units' value='imperial'";
    if (config_imperial) page += " checked";
    page += ">Imperial</label>";

    page += "</div></div>";

    // -------------------------
    // Contrast configuration UI
    // -------------------------
    page += "<div class='section'></div>";

    // Variable contrast master toggle
    page += "<div class='row'><label>Variable contrast:</label>";
    page += "<div class='checkwrap'>";
    page += "<input id='variableContrast' type='checkbox' name='variableContrast' value='1'";
    if (config_variableContrast) page += " checked";
    page += ">";
    page += "<span></span>";
    page += "</div></div>";

    // Wrapper shown only if variable contrast enabled
    page += "<div id='contrast_block' style='";
    if (!config_variableContrast) page += "display:none;";
    page += "'>";

    // Follow sun toggle
    page += "<div class='row' id='followSun_row'><label>Contrast follows sun:</label>";
    page += "<div class='checkwrap'>";
    page += "<input id='contrastFollowSun' type='checkbox' name='contrastFollowSun' value='1'";
    if (config_contrastFollowSun) page += " checked";
    page += ">";
    page += "<span></span>";
    page += "</div></div>";

    // Day / Night contrast
    page += "<div class='row' id='dayContrast_row'><label for='dayContrast'>Day contrast (0-255):</label>";
    page += "<input id='dayContrast' type='number' min='1' max='255' name='dayContrast' value='" + String(config_dayContrast) + "'></div>";

    page += "<div class='row' id='nightContrast_row'><label for='nightContrast'>Night contrast (0-255):</label>";
    page += "<input id='nightContrast' type='number' min='0' max='255' name='nightContrast' value='" + String(config_nightContrast) + "'></div>";

    page += "<div class='hint' style='margin:-4px 0 10px 0;'>When <b>follows sun</b> is enabled, the clock auto-computes transitions. Otherwise you can set times manually.</div>";

    // Sunrise / Sunset / DawnDusk (hidden when followSun ON)
    page += "<div id='sunTimes_block' style='";
    if (config_contrastFollowSun) page += "display:none;";
    page += "'>";

    // Human sunrise/sunset (HH:MM) + hidden numeric fields (seconds) actually submitted
    page += "<div class='row'><label for='sunRise_hm'>Sunrise (HH:MM):</label>";
    page += "<input id='sunRise_hm' type='text' inputmode='numeric' placeholder='07:00' value='" + formatHHMM(config_sunRise) + "'></div>";

    page += "<div class='row'><label for='sunSet_hm'>Sunset (HH:MM):</label>";
    page += "<input id='sunSet_hm' type='text' inputmode='numeric' placeholder='21:00' value='" + formatHHMM(config_sunSet) + "'></div>";

    page += "<div class='row'><label for='dawnDusk'>Dawn/Dusk duration (min):</label>";
    page += "<input id='dawnDusk' type='number' min='0' max='240' name='dawnDusk' value='" + String(config_dawnDuskDuration) + "'></div>";

    // Hidden fields (backend keeps receiving seconds)
    page += "<input type='hidden' name='sunRise' id='sunRise' value='" + String(config_sunRise) + "'>";
    page += "<input type='hidden' name='sunSet' id='sunSet' value='" + String(config_sunSet) + "'>";

    page += "</div>"; // sunTimes_block
    page += "</div>"; // contrast_block

    // -------------------------
    // JS (TZ + Contrast UI + HH:MM conversion)
    // -------------------------
    page += "<script>";

    // TZ manual toggle
    page += "function toggleTZManual(){";
    page += "var tz=document.getElementById('tz').value;";
    page += "document.getElementById('tz_manual_row').style.display=(tz==='MANUAL')?'grid':'none';";
    page += "}";

    // Contrast UI toggle
    page += "function toggleContrastUI(){";
    page += "var vc=document.getElementById('variableContrast');";
    page += "var fs=document.getElementById('contrastFollowSun');";
    page += "var vcOn = vc && vc.checked;";
    page += "var fsOn = fs && fs.checked;";
    page += "document.getElementById('contrast_block').style.display = vcOn ? '' : 'none';";
    page += "document.getElementById('sunTimes_block').style.display = (vcOn && !fsOn) ? '' : 'none';";
    page += "}";

    // HH:MM -> seconds (0 if invalid)
    page += "function hmToSeconds(hm){";
    page += "if(!hm) return 0;";
    page += "hm = hm.trim();";
    page += "var p = hm.split(':');";
    page += "if(p.length!==2) return 0;";
    page += "var h=parseInt(p[0],10), m=parseInt(p[1],10);";
    page += "if(isNaN(h)||isNaN(m)) return 0;";
    page += "if(h<0||h>23||m<0||m>59) return 0;";
    page += "return h*3600 + m*60;";
    page += "}";

    // Wire events + initial state
    page += "document.addEventListener('DOMContentLoaded', function(){";
    page += "toggleTZManual();";
    page += "toggleContrastUI();";

    page += "var tzSel=document.getElementById('tz');";
    page += "if(tzSel) tzSel.addEventListener('change', toggleTZManual);";

    page += "var vc=document.getElementById('variableContrast');";
    page += "var fs=document.getElementById('contrastFollowSun');";
    page += "if(vc) vc.addEventListener('change', toggleContrastUI);";
    page += "if(fs) fs.addEventListener('change', toggleContrastUI);";

    // Before submit: convert HH:MM to seconds into hidden fields
    page += "var form=document.getElementById('cfgform');";
    page += "if(form){";
    page += "form.addEventListener('submit', function(){";
    page += "var sr=document.getElementById('sunRise_hm');";
    page += "var ss=document.getElementById('sunSet_hm');";
    page += "var srSec = hmToSeconds(sr?sr.value:'');";
    page += "var ssSec = hmToSeconds(ss?ss.value:'');";
    page += "document.getElementById('sunRise').value = srSec;";
    page += "document.getElementById('sunSet').value  = ssSec;";
    page += "});";
    page += "}";

    page += "});";

    // Also run once right now (safety)
    page += "toggleTZManual();";
    page += "toggleContrastUI();";

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

  // Existing fields
  bool   showSeconds = server.hasArg("showseconds"); // checkbox: present => true
  String units       = server.arg("units");          // "metric" or "imperial"

  // New contrast fields (from the updated UI)
  bool variableContrast   = server.hasArg("variableContrast");
  bool contrastFollowSun  = server.hasArg("contrastFollowSun");

  // Day/Night contrast are meaningful only if variableContrast is on
  String dayContrastStr   = server.arg("dayContrast");     // 0..255
  String nightContrastStr = server.arg("nightContrast");   // 0..255

  // Manual sun times (only meaningful if variableContrast is on AND followSun is off)
  // NOTE: the UI sends these as seconds via hidden fields sunRise/sunSet
  String sunRiseStr       = server.arg("sunRise");         // seconds of day
  String sunSetStr        = server.arg("sunSet");          // seconds of day
  String dawnDuskStr      = server.arg("dawnDusk");        // minutes

  Serial.println(F("Received configuration:"));
  Serial.print(F("SSID: ")); Serial.println(ssid);
  Serial.print(F("Password: ")); Serial.println(pass);
  Serial.print(F("City: ")); Serial.println(newCity);
  Serial.print(F("Timezone select: ")); Serial.println(tzSelect);
  Serial.print(F("Timezone manual: ")); Serial.println(tzManual);
  Serial.print(F("Show seconds: ")); Serial.println(showSeconds ? "true" : "false");
  Serial.print(F("Units: ")); Serial.println(units);

  Serial.print(F("Variable contrast: ")); Serial.println(variableContrast ? "true" : "false");
  Serial.print(F("Contrast follows sun: ")); Serial.println(contrastFollowSun ? "true" : "false");
  Serial.print(F("Day contrast: ")); Serial.println(dayContrastStr);
  Serial.print(F("Night contrast: ")); Serial.println(nightContrastStr);
  Serial.print(F("Sunrise (s): ")); Serial.println(sunRiseStr);
  Serial.print(F("Sunset (s): ")); Serial.println(sunSetStr);
  Serial.print(F("Dawn/Dusk (min): ")); Serial.println(dawnDuskStr);

  // Basic validation
  if (ssid.length() == 0 || newCity.length() == 0 || tzSelect.length() == 0)
  {
    server.send(400, "text/html",
      "<html><body><h3>Invalid input, please fill all required fields.</h3></body></html>");
    return;
  }

  // Units validation (default to metric if missing)
  bool imperial = (units == "imperial");

  // Timezone handling
  String finalTZ;
  bool tzIsManual = false;

  if (tzSelect == "MANUAL")
  {
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

  // ---- Parse + validate contrast settings ----
  // Defaults fall back to current config values (so missing fields won't nuke them)
  uint8_t  dayC   = config_dayContrast;
  uint8_t  nightC = config_nightContrast;
  uint16_t dawnMin = config_dawnDuskDuration;
  uint32_t sunRiseSec = config_sunRise;
  uint32_t sunSetSec  = config_sunSet;

  if (variableContrast)
  {
    // Day/Night contrast (required-ish when variableContrast is enabled)
    // If empty, keep previous to be forgiving.
    if (dayContrastStr.length() > 0)
    {
      long v = dayContrastStr.toInt();
      if (v < 0) v = 0;
      if (v > 255) v = 255;
      dayC = (uint8_t)v;
    }

    if (nightContrastStr.length() > 0)
    {
      long v = nightContrastStr.toInt();
      if (v < 0) v = 0;
      if (v > 255) v = 255;
      nightC = (uint8_t)v;
    }

    // Manual sun times only used if followSun is OFF
    if (!contrastFollowSun)
    {
      // Dawn/Dusk duration in minutes
      if (dawnDuskStr.length() > 0)
      {
        long v = dawnDuskStr.toInt();
        if (v < 0) v = 0;
        if (v > 240) v = 240; // sane cap
        dawnMin = (uint16_t)v;
      }

      // Sunrise/Sunset seconds-of-day (0..86399)
      if (sunRiseStr.length() > 0)
      {
        long v = sunRiseStr.toInt();
        if (v < 0) v = 0;
        if (v > 86399) v = 86399;
        sunRiseSec = (uint32_t)v;
      }

      if (sunSetStr.length() > 0)
      {
        long v = sunSetStr.toInt();
        if (v < 0) v = 0;
        if (v > 86399) v = 86399;
        sunSetSec = (uint32_t)v;
      }
    }
    // If followSun is ON, we intentionally ignore manual sunrise/sunset/duration
  }
  else
  {
    // If variableContrast is OFF, we ignore all contrast-related inputs.
    // (We keep stored values untouched; the device just won't use them.)
    contrastFollowSun = false; // optional: force consistent state
  }

  // Save into config variables
  config_wifiSSID = ssid;
  config_wifiPass = pass;
  config_city     = newCity;

  config_timezone        = finalTZ;
  config_timezone_manual = tzIsManual;

  config_showSeconds = showSeconds;
  config_imperial    = imperial;

  // New config vars
  config_variableContrast  = variableContrast;
  config_contrastFollowSun = contrastFollowSun;
  config_dayContrast       = dayC;
  config_nightContrast     = nightC;
  config_dawnDuskDuration  = dawnMin;
  config_sunRise           = sunRiseSec;
  config_sunSet            = sunSetSec;

  // Persist to EEPROM/NVS
  saveSettings();

  // Response + reboot
  server.send(200, "text/html",
    "<html><body><h3>Settings saved. Rebooting...</h3></body></html>");
  delay(800);
  ESP.restart();
}


static uint16_t eepromReadU16(int addr)
{
  uint16_t v = 0;
  v |= (uint16_t)EEPROM.read(addr + 0);
  v |= (uint16_t)EEPROM.read(addr + 1) << 8;
  return v;
}

static uint32_t eepromReadU32(int addr)
{
  uint32_t v = 0;
  v |= (uint32_t)EEPROM.read(addr + 0);
  v |= (uint32_t)EEPROM.read(addr + 1) << 8;
  v |= (uint32_t)EEPROM.read(addr + 2) << 16;
  v |= (uint32_t)EEPROM.read(addr + 3) << 24;
  return v;
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
    config_city     = "";

    config_timezone        = "UTC0";
    config_timezone_manual = false;

    config_showSeconds = false;
    config_imperial    = false;

    // NEW defaults (match your globals, but explicit here)
    config_variableContrast = false;
    config_dayContrast      = 255;
    config_nightContrast    = 10;
    config_dawnDuskDuration = 30;
    config_sunRise          = 25200; // 07:00
    config_sunSet           = 75600; // 21:00

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
  uint8_t flags = EEPROM.read(ADDR_VARIABLES);
  config_timezone_manual   = (flags & (1 << 0)) != 0;
  config_showSeconds       = (flags & (1 << 1)) != 0;
  config_imperial          = (flags & (1 << 2)) != 0;
  config_variableContrast  = (flags & (1 << 3)) != 0;
  config_contrastFollowSun = (flags & (1 << 4)) != 0;

  // --- Read extra variables (layout must match saveSettings) ---
  const int A_DAY_CONTRAST   = ADDR_VARIABLES + 1;
  const int A_NIGHT_CONTRAST = ADDR_VARIABLES + 2;
  const int A_DAWN_DUSK_U16  = ADDR_VARIABLES + 3; // +3..+4
  const int A_SUNRISE_U32    = ADDR_VARIABLES + 5; // +5..+8
  const int A_SUNSET_U32     = ADDR_VARIABLES + 9; // +9..+12

  config_dayContrast      = EEPROM.read(A_DAY_CONTRAST);
  config_nightContrast    = EEPROM.read(A_NIGHT_CONTRAST);
  config_dawnDuskDuration = eepromReadU16(A_DAWN_DUSK_U16);
  config_sunRise          = eepromReadU32(A_SUNRISE_U32);
  config_sunSet           = eepromReadU32(A_SUNSET_U32);

  // --- Sanity checks / fallbacks ---
  if (config_timezone.length() == 0)
  {
    config_timezone = "UTC0";
    config_timezone_manual = false;
  }

  // Clamp sunrise/sunset to valid seconds-of-day (0..86399)
  if (config_sunRise > 86399UL) config_sunRise = 25200UL;
  if (config_sunSet  > 86399UL) config_sunSet  = 75600UL;

  // Dawn/dusk duration: avoid crazy values (0..240 minutes as a reasonable cap)
  if (config_dawnDuskDuration > 240) config_dawnDuskDuration = 30;

  Serial.println(F("Configuration loaded from EEPROM."));
  Serial.print(F("SSID: ")); Serial.println(config_wifiSSID);
  Serial.print(F("Password: ")); Serial.println(config_wifiPass);
  Serial.print(F("City: ")); Serial.println(config_city);
  Serial.print(F("Timezone select: ")); Serial.println(config_timezone);
  Serial.print(F("Timezone manual: ")); Serial.println(config_timezone_manual ? "true" : "false");
  Serial.print(F("Show seconds: ")); Serial.println(config_showSeconds ? "true" : "false");
  Serial.print(F("Units imperial: ")); Serial.println(config_imperial ? "true" : "false");

  // NEW debug prints
  Serial.print(F("Variable contrast: ")); Serial.println(config_variableContrast ? "true" : "false");
  Serial.print(F("Contrast to follow Sun: ")); Serial.println(config_contrastFollowSun ? "true" : "false");
  Serial.print(F("Day contrast: ")); Serial.println(config_dayContrast);
  Serial.print(F("Night contrast: ")); Serial.println(config_nightContrast);
  Serial.print(F("Dawn/Dusk duration (min): ")); Serial.println(config_dawnDuskDuration);
  Serial.print(F("Sunrise (s): ")); Serial.println(config_sunRise);
  Serial.print(F("Sunset (s): ")); Serial.println(config_sunSet);
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

// Helpers to write integers in little-endian
static void eepromWriteU16(int addr, uint16_t v)
{
  EEPROM.write(addr + 0, (uint8_t)(v & 0xFF));
  EEPROM.write(addr + 1, (uint8_t)((v >> 8) & 0xFF));
}

static void eepromWriteU32(int addr, uint32_t v)
{
  EEPROM.write(addr + 0, (uint8_t)(v & 0xFF));
  EEPROM.write(addr + 1, (uint8_t)((v >> 8) & 0xFF));
  EEPROM.write(addr + 2, (uint8_t)((v >> 16) & 0xFF));
  EEPROM.write(addr + 3, (uint8_t)((v >> 24) & 0xFF));
}

void saveSettings() 
{
  const int MAX_SSID = 60;
  const int MAX_PASS = 60;
  const int MAX_CITY = 60;
  const int MAX_TZ   = 80; // 210..299 gives you ~90 bytes total (len + payload)

  // Write SSID / PASS / CITY / TZ
  eepromWriteString(ADDR_SSID, config_wifiSSID, MAX_SSID);
  eepromWriteString(ADDR_PASS, config_wifiPass, MAX_PASS);
  eepromWriteString(ADDR_CITY, config_city,     MAX_CITY);
  eepromWriteString(ADDR_TZ,   config_timezone, MAX_TZ);

  // Pack booleans into one byte (flags)
  uint8_t flags = 0;
  if (config_timezone_manual) flags  |= (1 << 0);
  if (config_showSeconds)     flags  |= (1 << 1);
  if (config_imperial)        flags  |= (1 << 2);
  if (config_variableContrast)flags  |= (1 << 3); 
  if (config_contrastFollowSun)flags |= (1 << 4);
  

  EEPROM.write(ADDR_VARIABLES, flags);

  // Layout from ADDR_VARIABLES + 1:
  // +1  : dayContrast (u8)
  // +2  : nightContrast (u8)
  // +3  : dawnDuskDuration (u16)   -> uses +3..+4
  // +5  : sunRise (u32)            -> uses +5..+8
  // +9  : sunSet (u32)             -> uses +9..+12
  const int A_DAY_CONTRAST   = ADDR_VARIABLES + 1;
  const int A_NIGHT_CONTRAST = ADDR_VARIABLES + 2;
  const int A_DAWN_DUSK_U16  = ADDR_VARIABLES + 3;
  const int A_SUNRISE_U32    = ADDR_VARIABLES + 5;
  const int A_SUNSET_U32     = ADDR_VARIABLES + 9;

  EEPROM.write(A_DAY_CONTRAST,   config_dayContrast);
  EEPROM.write(A_NIGHT_CONTRAST, config_nightContrast);
  eepromWriteU16(A_DAWN_DUSK_U16, config_dawnDuskDuration);
  eepromWriteU32(A_SUNRISE_U32,   config_sunRise);
  eepromWriteU32(A_SUNSET_U32,    config_sunSet);

  // Optional: clear a little padding for future expansion
  //for (int i = ADDR_VARIABLES + 13; i < ADDR_VARIABLES + 20; i++)
  //  EEPROM.write(i, 0);

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

static uint32_t hhmmToSeconds(const String& hhmm)
{
  int colon = hhmm.indexOf(':');
  int h = hhmm.substring(0, colon).toInt();
  int m = hhmm.substring(colon + 1).toInt();

  // Assume caller already validated; just clamp lightly
  if (h < 0) h = 0; if (h > 23) h = 23;
  if (m < 0) m = 0; if (m > 59) m = 59;

  return (uint32_t)h * 3600UL + (uint32_t)m * 60UL;
}

static uint8_t lerpU8(uint8_t a, uint8_t b, float t)
{
  if (t <= 0.0f) return a;
  if (t >= 1.0f) return b;
  float v = (float)a + ((float)b - (float)a) * t;
  if (v < 0.0f) v = 0.0f;
  if (v > 255.0f) v = 255.0f;
  return (uint8_t)(v + 0.5f);
}

uint8_t lastBrightness_ = 0;

uint8_t calculateDisplayBrightness()
{
  uint8_t brightness = calculateDisplayBrightness_d(false);
  if(brightness != lastBrightness_)
  {
    lastBrightness_ = brightness;
    //calculateDisplayBrightness_d(true);
    Serial.print("Calculated Brightness: ");
    Serial.println(brightness);
  }
  
  return brightness;
}

uint8_t calculateDisplayBrightness_d(bool debug)
{
   if (config_variableContrast == false)
    return 255;

  time_t localSunsetSec  = 0;
  time_t localSunriseSec = 0;
  time_t localSundawnSec = 0;
  time_t localSunduskSec = 0;

  if (!config_contrastFollowSun)
  {
    // Manually set times (seconds-of-day)
    localSunsetSec  = (time_t)config_sunSet;
    localSunriseSec = (time_t)config_sunRise;
    localSundawnSec = (time_t)config_sunRise - (time_t)config_dawnDuskDuration * 60;
    localSunduskSec = (time_t)config_sunSet  + (time_t)config_dawnDuskDuration * 60;
  }
  else
  {
    // Follow sun times from weather (seconds-of-day)
    localSunsetSec  = (time_t)hhmmToSeconds(weather_sunset);
    localSunriseSec = (time_t)hhmmToSeconds(weather_sunrise);
    localSundawnSec = (time_t)hhmmToSeconds(weather_sundawn);
    localSunduskSec = (time_t)hhmmToSeconds(weather_sundusk);
  }

  struct tm timeinfo;
  getLocalTime(&timeinfo);

  timeinfo.tm_isdst = -1;
  time_t nowEpoch = mktime(&timeinfo);

  const time_t SEC_PER_DAY = 3600 * 24;
  // Start of "today" in local epoch
  struct tm dayStart = timeinfo;
  dayStart.tm_hour = 0;
  dayStart.tm_min  = 0;
  dayStart.tm_sec  = 0;
  dayStart.tm_isdst = -1;
  time_t todayStartsSec = mktime(&dayStart);

  // Convert seconds-of-day to today's epochs
  localSunsetSec  += todayStartsSec;
  localSunriseSec += todayStartsSec;
  localSundawnSec += todayStartsSec;
  localSunduskSec += todayStartsSec;

  if(debug)
  {
    Serial.print("Now: ");
    Serial.print(nowEpoch);
  
    Serial.print(" - localSundawnSec: ");
    Serial.print(localSundawnSec);

    Serial.print(" - localSunriseSec: ");
    Serial.print(localSunriseSec);

    Serial.print(" - localSunsetSec: ");
    Serial.print(localSunsetSec);    
    
    Serial.print(" - localSunduskSec: ");
    Serial.println(localSunduskSec);
  }

  //If night or day, return config_nightContrast or config_dayContrast
  //However if we are between dawn and sunrise or sunset and dusk, we need
  //to calculate linearly the contrast between both variables.
  // --- Handle wrap-around across midnight for dawn/dusk (manual mode can do this) ---
  // If sundawn was computed as sunrise - duration and went negative, it will be < todayStartsSec
  // => it actually belongs to yesterday.
  if (localSundawnSec < todayStartsSec) localSundawnSec += SEC_PER_DAY;

  // If sundusk goes beyond end of today, it belongs to tomorrow.
  if (localSunduskSec >= todayStartsSec + SEC_PER_DAY) localSunduskSec -= SEC_PER_DAY;

  // Instead of trying to "sort" the events, we treat the day as 4 segments:
  // Night:  [dusk .. dawn)
  // Dawn ramp: [dawn .. sunrise)
  // Day:   [sunrise .. sunset)
  // Dusk ramp: [sunset .. dusk)

  // But if dawn ended up "tomorrow" (e.g. 23:50 dawn), it means our night interval spans midnight.
  // So we do comparisons in a way that works even when dusk < dawn (wrap-around).

  auto inRange = [&](time_t x, time_t a, time_t b) -> bool {
    // returns true if x in [a,b) with wrap-around allowed
    if (a <= b) return (x >= a && x < b);
    // wraps midnight
    return (x >= a || x < b);
  };

  // Ensure sunrise/sunset are in today's range (they should be)
  // If your inputs can make sunrise/sunset wrap, you can normalize similarly.

  // 1) Dawn ramp
  if (inRange(nowEpoch, localSundawnSec, localSunriseSec))
  {
    float span = (float)(localSunriseSec - localSundawnSec);
    // If wrapped, add a day to span
    if (span <= 0.0f) span += (float)SEC_PER_DAY;

    float dt = (float)(nowEpoch - localSundawnSec);
    if (dt < 0.0f) dt += (float)SEC_PER_DAY;

    float t = dt / span;
    return lerpU8(config_nightContrast, config_dayContrast, t);
  }

  // 2) Day plateau
  if (inRange(nowEpoch, localSunriseSec, localSunsetSec))
  {
    return config_dayContrast;
  }

  // 3) Dusk ramp
  if (inRange(nowEpoch, localSunsetSec, localSunduskSec))
  {
    float span = (float)(localSunduskSec - localSunsetSec);
    if (span <= 0.0f) span += (float)SEC_PER_DAY;

    float dt = (float)(nowEpoch - localSunsetSec);
    if (dt < 0.0f) dt += (float)SEC_PER_DAY;

    float t = dt / span;
    return lerpU8(config_dayContrast, config_nightContrast, t);
  }

  // 4) Night plateau (everything else)
  return config_nightContrast;
}

void setDisplayBrightness(uint8_t contrast)
{
  // contrast: 0 (very dim) .. 255 (very bright)
  display.ssd1306_command(SSD1306_SETCONTRAST);
  display.ssd1306_command(contrast);
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
  setDisplayBrightness(calculateDisplayBrightness());

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
  uint16_t w, h, w2, h2;
  display.getTextBounds(dayName, 0, 0, &x1, &y1, &w, &h);
  // center horizontally
  int dayX = (128 - w) / 2;
  display.setCursor(dayX, 0);
  display.print(dayName);
  if(config_showSeconds)
  {
    // Time HH:MM in large font, and :ss in small font
    /*display.setFont(&FreeMonoBold12pt7b);
    // Format time as HH:MM:SS
    char timeBuf[9];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    String timeStr = String(timeBuf);
    display.getTextBounds(timeStr, 0, 30, &x1, &y1, &w, &h);
    int timeX = (128 - w) / 2;
    // Vertically center the text around mid (y=32)
    int timeY = 32 + (h / 2);
    display.setCursor(timeX, timeY);
    display.print(timeStr);*/

    
    // Format time as HH:MM
    char timeBuf[6];
    char secBuf[5];
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    snprintf(secBuf, sizeof(secBuf), " :%02d", timeinfo.tm_sec);
    String timeStr = String(timeBuf);
    String secStr = String(secBuf);
    display.getTextBounds(secStr, 0, 0, &x1, &y1, &w2, &h2);
    //w2 and h2 contains the size of the seconds
    
    display.setFont(&FreeMonoBold18pt7b);
    display.getTextBounds(timeStr, 0, 30, &x1, &y1, &w, &h);
    int timeX = (128 - w - w2) / 2;
    // Vertically center the text around mid (y=32)
    int timeY = 32 + (h / 2);
    display.setCursor(timeX, timeY);
    display.print(timeStr);

    // Bottom right: seconds
    display.setFont(NULL);
    //display.setCursor(timeX + w, timeY - h + h2 /*+ h - h2*/);
    display.setCursor(timeX + w, timeY - h2 + 2);
    display.print(secBuf);
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
  if (weather_valid && weather_temp != "N/A" && weather_hum != "") 
  {
    String tempDisplay = sanitizeTempForDisplay(weather_temp);
    display.print(tempDisplay);
    display.print((char)247); // degree symbol (your display's charset)
    display.print(config_imperial ? "F " : "C ");
    display.print(weather_hum);
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
  setDisplayBrightness(calculateDisplayBrightness());
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
  if (weather_valid && weather_temp != "N/A") 
  {
    // weatherTemp is e.g. "+12" or "-3" as string (cleaned)
    String tempNum = sanitizeTempForDisplay(weather_temp);
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
  String cond = weather_valid ? weather_cond : "";
  cond.trim();
  display.getTextBounds(cond, 0, 40, &x1, &y1, &w, &h);
  int condX = (128 - w) / 2;
  display.setCursor(condX, 40);
  display.print(cond);
  // Bottom right: H:% W:m/s P:mm
  display.setFont(NULL);
  if (weather_valid && weather_temp != "N/A") 
  {
    String windDisplay = sanitizeWindForDisplay(weather_wind);
    String bottomStr = String("H:") + weather_hum;
    bottomStr += " " + windDisplay;
    bottomStr += (config_imperial ? "mph" : "km/h");
    //if (weatherWind != "N/A") bottomStr += "km/h";
    bottomStr += " " + weather_press;
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

  if(weather_lastSuccessfulUpdate == 0)
  {
    Serial.println("Fetching weather data failed and we never got a valid data :(. Expired!");
    return false; // We never got a valid weather :(
  }

  if(weather_lastSuccessfulUpdate + 5400000UL > now)
  {
    Serial.println("Fetching weather data failed but previous stored data did not expired. So we keep the data :-D");
    Serial.print(F("Temp=")); Serial.println(weather_temp);
    Serial.print(F("Cond=")); Serial.println(weather_cond);
    Serial.print(F("Hum=")); Serial.println(weather_hum);
    Serial.print(F("Wind=")); Serial.println(weather_wind);
    Serial.print(F("Pressure=")); Serial.println(weather_press);
    return true;  // Last update was < 90 minutes ago, so it is still valid.
  }

  Serial.println("Weather data expired! :((((");
  return false;
}

uint32_t getWeatherCounter = 0;

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
  bool sunDataAvailable = false;

  if(getWeatherCounter % 4 == 0 && config_variableContrast && config_contrastFollowSun)
  {
    //Also ask for Sun timings, but only once every 4 times (because in some latitudes the website crashes)
    getWeatherCounter++;
    query += "|%25D|%25S|%25s|%25d";
    sunDataAvailable = true;
  }
  
  // Add imperial units if configured
  if (config_imperial) 
  {
    query += "&lang=en&u";
  }
  else
  {
    query += "&lang=en&m";
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

  // Parse fields: temp|cond|hum|wind|press|Dawn|Sunrise|Sunset|Dusk
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

  String sunDawnStr  = "";
  String sunRiseStr  = "";
  String sunSetStr   = "";
  String sunDuskStr  = "";

  if(sunDataAvailable)
  {
    int idx5 = result.indexOf('|', idx4 + 1);
    int idx6 = result.indexOf('|', idx5 + 1);
    int idx7 = result.indexOf('|', idx6 + 1);
    int idx8 = result.indexOf('|', idx7 + 1);

    pressStr    = result.substring(idx4 + 1, idx5);
    sunDawnStr  = result.substring(idx5 + 1, idx6);
    sunRiseStr  = result.substring(idx6 + 1, idx7);
    sunSetStr   = result.substring(idx7 + 1, idx8);
    sunDuskStr  = result.substring(idx8 + 1);
  }

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

  weather_temp = tempStr; // keep + or - sign if present and the Degree
  weather_cond = condStr;
  weather_hum = humStr;
  weather_wind = windStr;
  weather_press = pressStr;

  if(sunDataAvailable)
  {
    Serial.print(F("Sun dawn=")); Serial.println(sunDawnStr);
    Serial.print(F("Sunrise=")); Serial.println(sunRiseStr);
    Serial.print(F("Sunset=")); Serial.println(sunSetStr);
    Serial.print(F("Sun dusk=")); Serial.println(sunDuskStr);
    weather_sundawn = sunDawnStr;
    weather_sunrise = sunRiseStr;
    weather_sunset  = sunSetStr;
    weather_sundusk = sunDuskStr;
  }
  weather_lastSuccessfulUpdate = millis();   //Register when we did get the last weather update

  return true;
}