# esp32-ds1820-mqtt

ESP32 port of ESP8266 DS1820 MQTT client by OH2MP
..porting, and potential random typos by OH8TH.

See https://github.com/oh2mp/esp8266_ds1820_mqtt what this is about.

- Take the data directory from OH2MPs repo, it is not included here!

For ESP32, you have to upload the files using different tool, see this page for more info:
https://randomnerdtutorials.com/install-esp32-filesystem-uploader-arduino-ide/

Hardware:
- Amazon sourced ESP32 that has text "ESP-WROOM-32" on the chip.
- "ESP32 Dev Module" from the Arduino ESP32 device list
- probably very very generic model

*NOTE: 1-wire connectivity tested and works fine. There is an issue with WebServer.h though,
rendering this unusable until something to replace ESP8266WebServer.h is found.
