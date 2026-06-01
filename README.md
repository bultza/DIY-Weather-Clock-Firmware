# DIY-Weather-Clock-Firmware

This is an Alternative firmware for the DIY Weather Clock WiFi kit that can be easily found on Amazon
or AliExpress. The kit includes a plexiglass structure and three PCB boards:

- An ESP-01S module with an ESP8266 MCU
- An Adafruit OLED display (0.96", 128x64 px)
- An interface PCB that is usually hand-soldered

<div align="center">
<img src="docs/photo_clock_composition.jpg" alt="Picture of the Clock and Weather face" width="80%"/>
<br/>Clock Face on the left, Weather face on the right
</div>

## Why
This kit already ships with a ready-to-use firmware, but it requires registering
on an external website and you have no real control over what the firmware does
or what data it sends.

In the original [WHYNOT blog](https://www.whynot.org.ua/en/electronic-kits/hu-061-diy-kit-wi-fi-weather-forecast-clock) you can find more information about this kit and its
firmware. This project started as a fork of that firmware, and has been heavily
modified and cleaned up to:

- remove external dependencies
- fix several corner cases
- add proper configuration and robustness
- support metric / imperial units
- support real automatic daylight-saving time (DST)

## How it works

On first boot, the firmware looks for a magic signature in EEPROM.

If the signature is not found:
- The device starts in Access Point (AP) mode
- The OLED display shows connection instructions
- You connect to the AP and open the configuration web portal
- You configure:
  - Wi-Fi credentials
  - City (used for weather)
  - Timezone (preset or manual)
  - Metric / imperial units
  - Seconds display

<div align="center">
<img src="docs/ESP8266_web_config.png" alt="Screenshot of the configuration website" width="50%"/>
<br/>Configuration web screenshot
</div>
Once configured and rebooted:

- The clock connects to your Wi-Fi network
- Time is synchronized using NTP pool servers
- Timezone handling uses proper DST rules (not fixed offsets)
- Weather data is retrieved from wttr.in every 15 minutes

Every 15 seconds the display toggles between:
- Clock view
- Weather view

If weather retrieval fails (no internet, server down, etc.), the device keeps
showing the clock only.

If the internet goes away for hours and later comes back, the ESP reconnects
automatically without rebooting.

## Changes from the original firmware

- Metric / Imperial units selection
- Proper timezone handling with automatic DST
- Optional seconds display
- Support for cities with spaces and special characters
- Weather hidden when not available
- More predictable behavior


## What you need to compile and install

Hardware:

- You can use a generic FTDI adapter, but it MUST be set to 3.3V
  (never use 5V, you will kill the ESP-01)
- Much easier: use an ESP-01 USB adapter (cheap to find in internet)
- To flash the firmware, the ESP must be in UART flash mode:
  - GPIO0 connected to GND during power-up
- Some ESP-01 boards (if you don't use the original) do not include a pull-up on GPIO2
  - This can cause random behavior when installed on the Clock.
  - Fix: solder a 12 kΩ pull-up resistor between GPIO2 and 3.3V

> :warning: FTDI you must configure it to 3.3V

> :warning: GPIO0 must be connected to GND at power up to enter in UART Flashing mode. See image attached here.

> :warning: GPIO2 needs a 12kohm pullup if you use another ESP-01 module that is not coming from the clock DIY kit.

<div align="center">
<img src="docs/photo_programming_02.jpg" alt="Picture of the ESP-01 USB adapter board with the ESP-01 connected and the GPIO0 connected to GND to enter in programming mode" width="70%"/>
<br/>ESP-01 USB adapter board with the ESP-01 connected and the GPIO0 connected to GND to enter in programming mode.
</div>

Software:
- Download and install Arduino IDE: https://www.arduino.cc/en/software/

- Install the ESP8266 board package:
  - File -> Preferences
  - Add to "Additional Boards Manager URLs": https://arduino.esp8266.com/stable/package_esp8266com_index.json
  - Tools -> Board -> Boards Manager...
  - Search for ESP8266
  - Install "esp8266 by ESP8266 Community"
  - Select board: "Generic ESP8266 Module"

- Clone this repository into your Arduino sketch folder
- Install required libraries using the Arduino Library Manager:
  - Adafruit SSD1306 (by Adafruit)
  - Adafruit GFX Library (by Adafruit)
  - **wolfssl** (by wolfSSL Inc.) — only needed on this `feature_tls1.3` branch, see below
  - Any dependencies pulled by those libraries
- Compile and upload the firmware

## TLS 1.3 build (this branch only — experimental)

wttr.in dropped everything below TLS 1.3, and the ESP8266's built-in BearSSL
only speaks up to TLS 1.2. This branch swaps the weather transport to **wolfSSL**
(which supports TLS 1.3) driven over a plain `WiFiClient`.

To reproduce the exact working-build state:

1. Install the **wolfssl** library from the Arduino Library Manager (tested with
   v5.8.2).
2. Patch the library's `user_settings.h` (it lives in
   `<your sketchbook>/libraries/wolfssl/src/user_settings.h`). Run the included
   [`apply_wolfssl_patch.ps1`](apply_wolfssl_patch.ps1) from PowerShell — it edits
   the file in place, is content-based (survives version differences) and safe to
   re-run:
   ```powershell
   .\apply_wolfssl_patch.ps1
   # or, if your sketchbook is not in the default Documents\Arduino location:
   .\apply_wolfssl_patch.ps1 -UserSettings "C:\path\to\libraries\wolfssl\src\user_settings.h"
   ```
   If you'd rather edit by hand, the script makes exactly these four changes
   inside the `#if defined(ESP8266)` block / globals:
   - add `#define HAVE_SNI` (SNI for the wttr.in vhost),
   - add `#define WOLFSSL_NO_TLS12` (drop TLS 1.2 to save RAM/flash),
   - comment out `#define DEBUG_WOLFSSL` (its strings sit in DRAM on ESP8266),
   - comment out `#define WOLFSSL_HW_METRICS` (static counters, ESP-IDF only).
3. Set a generous Flash Size in Tools (e.g. `1M (FS:none)`), compile and flash.

> ⚠️ **Known result on the ESP-01S: it links but does not run.** wolfSSL's
> constant crypto tables (ECC, ASN/OID) are kept in DRAM by the Arduino ESP8266
> core (no automatic `.rodata`-in-flash), eating ~26 KB and leaving only ~6.5 KB
> of free heap at runtime — far below the ~20-30 KB a TLS 1.3 handshake needs, so
> `wolfSSL_new()` fails out of memory. This branch is kept as a documented
> dead-end; the shipping firmware on `main`/`develop` talks to wttr.in a
> different way.


## Resources
- Original firmware and inspiration: https://www.whynot.org.ua/en/electronic-kits/hu-061-diy-kit-wi-fi-weather-forecast-clock
- Huge thanks to wttr.in for providing free weather data: https://github.com/chubin/wttr.in
- In your source website for DIY projects just search for "ESP8266 DIY" or "weather clock diy" to find the hardware, usually for less than 10€

Simple clock, honest code.
Less magic, more control.
