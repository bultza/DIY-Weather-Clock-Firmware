# DIY-Weather-Clock-Firmware
Firmware for the DIY Weather Clock WiFi kit that you can easily find in Amazon or Aliexpress. The kit comes with a plexyglass structure and three PCB boards:
* An ESP-01S with a ESP8266 MCU
* An OLED display from Adafruit 0.96" 128x64px
* An interface board that you would usually manually solder

<div align="center">
<img src="docs/photo_clock.png" alt="Picture of the Clock face" width="70%"/>
</div>

<div align="center">
<img src="docs/photo_clock_weather.png" alt="Picture of the Weather face" width="70%"/>
</div>

## Why
This kit comes already with a ready to use firmware, however you must register in a website and ... you don't have control of what is really doing. In the WHYNOT blog you can find more information. This firmware comes in reallity from that blog post and I have modified it to fix a few corner cases and to allow easier configuration. (For example imperial, metric and Summer timezones support).

## How it works
The first time you boot the firmware it will search for a magic word in the EEPROM. If not found it will launch in Access Point mode with all the detailed information in the display. Then you connect to it and via a website you configure to which WiFi network it should connect, the city (to retrieve the weather) and a couple of other things.

<div align="center">
<img src="docs/ESP8266_web_config.png" alt="Screenshot of the configuration website" width="70%"/>
</div>

If correctly configured, the second time it will connect to your WiFi network and automatically it will sync the time with the typical pool servers and it will retrieve the weather. The clock will connect to wttr.in and retrieve the weather every 15 minutes.

Every 15s the screen will toogle between Clock and Weather. If the weather retrieval fails, it will only display the clock.

## Changes from the original firmware
* Metric and Imperial selection
* Timezones with automatic summer time correction
* Seconds display
* Cities with "spaces" and especial characters in the name
* Weather not shown if not available


## What do you need to compile and install
Hardware:
* You could make it with a FTDI (3.3V!) but it is so convenient just to buy the ESP-01 USB adapter and it would be plug an play (almost)
 * Warning. If you buy the ESP-01 USB adapter with other ESP-01 modules, some of them do not have the 12kohm pullup on the GPIO2, so it will behave randomly on the clock. You can easily solve this soldering a 12kohm resistor between the GPIO2 and 3.3V on the PCB adapter board.
* You need to boot the ESP in UART Flash mode when you want to reflash. Remember this is done by connecting GPIO0 to GND during power up.

Add warning icon that using a FTDI you must configure it to 3.3V
Add warning icon that GPIO0 must be connected to GND at power up to enter in UART Flashing mode. See image attached here.
Add warning icon that GPIO2 needs a 12kohm pullup if you use another ESP-01 module that is not coming from the clock DIY kit.

<div align="center">
<img src="docs/photo_programming_02.png" alt="Picture of the ESP-01 USB adapter board with the ESP-01 connected and the GPIO0 connected to GND to enter in programming mode" width="70%"/>
Picture of the ESP-01 USB adapter board with the ESP-01 connected and the GPIO0 connected to GND to enter in programming mode
</div>

Software:
* Download and install Arduino IDE: https://www.arduino.cc/en/software/
* Install the ESP8266 package:
  * File -> Preferences
  * In Additional Boards Manager URLs, add this: https://arduino.esp8266.com/stable/package_esp8266com_index.json
  * Tools -> Board -> Boards Manager...
  * Search ESP8266
  * Install "esp8266 by ESP8266 Community"
  * Go to Tools → Board and choose: "Generic ESP8266 Module"
* Clone this repository in your Arduino Sketch folder :-D
* Install the required libraries on the Arduino IDE -> Icon libraries -> search and install:
 * Adafruit SSD1306 by Adafruit
 * Adafruit GFX Library by Adafruit
 * Any other dependencies that the previous 2 create.
* Compile and upload the code.


## Resources & Thanks
* This firmware is a fork from the original (awesome and thank you so much) one that you can find here: https://www.whynot.org.ua/en/electronic-kits/hu-061-diy-kit-wi-fi-weather-forecast-clock
* Also thank you so much to wttr.in for providing weather information all over the world for free: https://github.com/chubin/wttr.in
* Aliexpress 
