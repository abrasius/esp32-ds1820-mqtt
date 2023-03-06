# esp32-ds1820-mqtt

ESP32 port of ESP8266 DS1820 MQTT client by OH2MP

(porting, and potential random typos and idiotisms by OH8TH)

NEW: Now in functional state! Detects sensors, configuration works, connects to a WiFi,
sends messages to the MQTT broker. What there is not to like?

See https://github.com/oh2mp/esp8266_ds1820_mqtt what this is about.

For ESP32, you have to upload the files using different tool, see this page for more info:
https://randomnerdtutorials.com/install-esp32-filesystem-uploader-arduino-ide/

Compiling options with Arduino IDE:
- set both Arduino and Events to run on same core!

Hardware I've been playing with:
- Amazon sourced ESP32 that has text "ESP-WROOM-32" on the chip.
- "ESP32 Dev Module" from the Arduino ESP32 device list
- probably very very generic model

3D printable stuff:
- https://www.thingiverse.com/thing:5876305 (ESP32 cases)
- https://www.thingiverse.com/thing:5878825 (DS1820 TO-92 holders for 20mm pipe)


TODO:
- support for a longer MQTT topic defaulting to "homeassistant/sensor"
- Temperature reporting in regular Celsius values
- upkeep the README along with actual changes

MAYBE TODO:
- support for corrective values to get sensor readings agree on what is zero degrees Celsius
(if this can be done in HA, I won't bother)
