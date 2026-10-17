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
- support weather icons
- support for **Ecowitt** weather stations, read locally over your LAN (this is optional)

<div align="center">
<img src="tools/screen_sim/screens.png" alt="Different possible Weather faces samples" width="80%"/>
<br/>Different Faces examples showing different possible customizations
</div>

## How it works

On first boot, the firmware looks for a magic signature in EEPROM.

If the signature is not found:
- The device starts in Access Point (AP) mode
- The OLED display shows connection instructions
- You connect to the AP and open the configuration web portal
- You configure:
  - Wi-Fi credentials (a **Scan** button lists nearby networks so you don't have to type the SSID)
  - City (used for weather)
  - Timezone (preset or manual)
  - Metric / imperial units
  - Pressure unit (hPa or mmHg)
  - Seconds display
  - Optionally, an **Ecowitt** weather station (see the Ecowitt section below)

<div align="center">
<img src="docs/ESP8266_web_config.png" alt="Screenshot of the configuration website" width="50%"/>
<br/>Configuration web screenshot
</div>
Once configured and rebooted:

- The clock connects to your Wi-Fi network
- Time is synchronized using NTP pool servers
- Timezone handling uses proper DST rules (not fixed offsets)
- Weather data is retrieved from wttr.in every 15 minutes
- If Ecowitt is enabled, the outdoor temperature/humidity and pressure are read
  from your own station instead, also every 15 minutes (the condition, icon and sun times still come
  from wttr.in)
- The configuration web portal stays available at the device's **IP (shown on the OLED at boot for 8 seconds)**, so you can reconfigure it from a browser at any time — no need
  to force AP mode or re-flash
- The device's serial log is kept in a small RAM buffer and can be viewed from a
  web page, so you can debug the clock over Wi-Fi without a serial cable.
- Firmware can be updated over the air (OTA): once it is running, you can upload a
  new `.bin` from a browser at `http://<device-ip>/update`, so you only need the
  USB/FTDI cable for the very first flash
- On boot (and once a day) the clock checks GitHub for a newer firmware version and
  lets you know if one is available (see the update-notification section below); it
  never flashes anything on its own

Every 15 seconds the display toggles between:
- Clock view
- Weather view

If weather retrieval fails (no internet, server down, etc.), the device keeps
showing the clock only.

If the internet goes away for hours and later comes back, the ESP reconnects
automatically without rebooting.

## Changes from the original firmware

- Metric / Imperial units selection
- Pressure display in **hPa or mmHg** (handy where mmHg is the norm, e.g. Russia)
- Proper timezone handling with automatic DST
- Optional seconds display
- Support for cities with spaces and special characters
- Weather hidden when not available
- Weather condition icon on the weather screen, with day/night variants (can be turned off)
- More display options: 12/24-hour time, DD/MM/YYYY or MM/DD/YYYY date, hide the '+' on positive temperatures
- Configuration web portal always reachable on the network (reconfigure anytime)
- Wi-Fi network scanner in the config portal (pick your SSID from a list)
- Optional **Ecowitt** integration: show outdoor temperature/humidity and pressure
  from your own weather station, read locally from the gateway (no cloud, no keys)
- Status icons on the clock screen: a Wi-Fi signal meter and an Ecowitt health mark
- Serial log viewable from a web page (remote debugging without a cable)
- Over-the-air (OTA) firmware updates from a web page (no cable after the first flash)
- Automatic **update notification**: the clock checks GitHub for a newer version and
  tells you (on the OLED, with a beating-heart icon, and on the config page)
- More predictable behavior


## Ecowitt (optional): use your own weather station

By default the clock gets all its weather from [wttr.in](https://wttr.in). If you
own an [Ecowitt](https://www.ecowitt.com/) weather station (a gateway or console such
as the GW1100 or GW2000, or any console that offers the local HTTP API) you can have
the clock show **your own measurements** instead: outdoor **temperature** and
**humidity** (from the outdoor sensor) and **pressure** (from the gateway's barometer).
The weather **condition**, the **icon** and the **sunrise/sunset** times still come
from wttr.in — the station doesn't provide those — so the two work together.

The clock talks to the gateway **directly on your local network**, using the
gateway's *local* HTTP API (`GET http://<gateway>/get_livedata_info`). There is
no Ecowitt cloud account, no API key and no application key involved, and nothing
leaves your LAN.

If the station is unreachable, the clock automatically falls back to the wttr.in
values, and the clock screen shows a small `!` next to the Wi-Fi meter (it shows
an "OK" mark when the last update succeeded).

### Configure the clock

1. Find the **IP address of your gateway** (your router's device list, or the
   Ecowitt app) and, ideally, give it a fixed IP / DHCP reservation in the router so
   it never changes.
2. Open the config portal (the AP `Clock-ESP01-Setup` on first setup, or the
   clock's IP afterwards) and tick **"Use Ecowitt station"**.
3. Type the gateway address in **Gateway IP/host** (e.g. `192.168.1.50`; a
   `:port` suffix is accepted if you changed the port).
4. Save. The clock reboots and starts overlaying your station's readings.

To check that the gateway answers, open `http://<gateway-ip>/get_livedata_info` in a
browser on the same network: you should see a JSON document with a `common_list`.

Notes:
- **Units:** use whatever units your gateway is set to (°C / °F, hPa / inHg /
  mmHg). The clock reads the unit the gateway reports and converts to the clock's
  own units (the **Metric / imperial** and **Pressure unit** options).
- **Which values:** outdoor temperature (`0x02`) and outdoor humidity (`0x07`) from
  the gateway's `common_list`, and the **relative (sea-level) pressure** from the
  `wh25` block. The relative pressure follows the altitude/offset you configured in
  the gateway. Extra channel sensors (WN34, WH31, ...) are not used.
- Only the gateway's **local HTTP API** is used. It must be reachable from the
  clock's Wi-Fi network (the same LAN/VLAN) and not blocked by client isolation.
- **Upgrading from the Netatmo version:** Netatmo support was removed. On the first
  boot the stored Netatmo credentials are erased from flash and the station option
  starts disabled, so just enable Ecowitt in the portal.


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
  - Any dependencies pulled by those libraries
- Before compiling, set the flash layout so OTA updates fit (see warning below):
  - Tools -> Flash Size -> **"1MB (FS:none OTA:~502KB)"**
- Compile and upload the firmware

> :warning: You MUST select **Flash Size = "1MB (FS:none OTA:~502KB)"** in the
> Tools menu. This firmware uses no filesystem, so this layout reclaims that space
> for the program and leaves ~500KB free for the new image during an OTA update.
> Any other 1MB layout (the default reserves 256KB for a filesystem) leaves almost
> no room for OTA and the `/update` page will reject the new firmware. If you build
> with PlatformIO instead, this is already handled by `platformio.ini`
> (`board_build.ldscript = eagle.flash.1m.ld`).

### Building with PlatformIO (VS Code)

If you prefer VS Code, the repo ships a `platformio.ini`, so you don't need the
Arduino IDE, and the flash layout for OTA is already set for you:

1. Install [VS Code](https://code.visualstudio.com/) and the **PlatformIO IDE** extension.
2. Open this repository folder in VS Code — PlatformIO picks up `platformio.ini`
   automatically and downloads the toolchain and libraries (Adafruit SSD1306 + GFX)
   on the first build.
3. Build / flash / monitor from the PlatformIO toolbar, or from a terminal:
   - Build: `pio run`
   - Flash over USB/FTDI (first time only, ESP in flash mode): `pio run -t upload`
   - Serial monitor: `pio device monitor` (115200)

The target board is `esp01_1m` (ESP-01S, 1 MB flash). After the first USB flash you
can update over Wi-Fi from the `/update` page (see below).

## Updating over the air (OTA)

You can use OTA only if you managed to update the firmware beforehand already. You cannot do OTA over the original firmware. So, after the firmware is running, you will no longer need the USB/FTDI cable to update it to future versions:

1. Build the new firmware and locate the binary:
   - Arduino IDE: **Sketch -> Export Compiled Binary**, then grab
     `DIY-Weather-Clock-Firmware.ino.bin`
   - PlatformIO: `.pio/build/esp01_1m/firmware.bin`
2. Open `http://<device-ip>/update` in a browser (the device IP is shown on the
   OLED at boot, and there is also an "Update firmware (OTA)" button on the
   configuration page).
3. Upload the `.bin`. The clock flashes it and reboots into the new version. Your
   saved configuration in EEPROM is preserved.

> :warning: The very first flash must still be done over the USB/FTDI cable — the
> factory firmware does not have the OTA update page.

### Update notifications

The clock can tell you when a newer firmware is available so you don't have to
check by hand. Right after boot (once the first weather data is in) and then once
every 24 hours, it fetches a tiny [`firmware/latest.json`](firmware/latest.json)
from this GitHub repo over HTTPS and compares its version with the one running.

If a newer version exists, it lets you know in three places:

- an **8-second notice on the OLED at boot**, showing the new version and the
  device's `http://<ip>/update` address;
- a **beating-heart icon** in the top-left corner of both the clock and weather
  screens;
- a **red banner** at the top of the configuration web page, with an *Update now*
  link straight to the `/update` page.

It **never flashes anything by itself** — updating is always your manual OTA
(upload the new `.bin` at `/update`). `latest.json` is published automatically
from the firmware version on each release, so it always reflects the latest build.

## Tools

Two small Python helpers live under [`tools/`](tools/) and are **not** part of the
firmware build:

- [`tools/icon_sim/`](tools/icon_sim/README.md) — preview the weather icons and
  regenerate `weather_icons.h`.
- [`tools/screen_sim/`](tools/screen_sim/README.md) — a bit-for-bit simulator of the
  128×64 OLED that mirrors both screens (icons, status marks, the update heart…),
  so you can preview layout changes without flashing hardware.

## Resources
- Original firmware and inspiration: https://www.whynot.org.ua/en/electronic-kits/hu-061-diy-kit-wi-fi-weather-forecast-clock
- Huge thanks to wttr.in for providing free weather data: https://github.com/chubin/wttr.in
- Weather icons by Dhole (pixel weather icons, CC BY-SA 4.0): https://github.com/Dhole/weather-pixel-icons
- In your source website for DIY projects just search for "ESP8266 DIY" or "weather clock diy" to find the hardware, usually for less than 10€

Simple clock, honest code.
Less magic, more control.
