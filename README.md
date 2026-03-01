# esp32-ds1820-mqtt

ESP32 port of ESP8266 DS1820 MQTT client by OH2MP

(porting, and potential random typos and idiotisms by OH8TH)

NEW: Now in functional state! Detects sensors, configuration works, connects to a WiFi,
sends messages to the MQTT broker. What there is not to like?

See https://github.com/oh2mp/esp8266_ds1820_mqtt what this is about.

## Install/flash from command line

This project has two parts to flash:
1. Firmware (`esp32_ds1820_mqtt/esp32_ds1820_mqtt.ino`)
2. Web UI files from `data/` into SPIFFS (`index.html`, `sensors.html`, etc.)

### Prerequisites

- `arduino-cli` installed
- ESP32 core installed for Arduino CLI
- Serial port for your board (example: `/dev/ttyUSB0`)

### 1) Install ESP32 core (once)

```bash
arduino-cli config init
arduino-cli core update-index
arduino-cli core install esp32:esp32
```

### 2) Compile and upload firmware

From repository root:

```bash
FQBN=esp32:esp32:esp32
PORT=/dev/ttyUSB0

arduino-cli compile --fqbn "$FQBN" esp32_ds1820_mqtt
arduino-cli upload -p "$PORT" --fqbn "$FQBN" esp32_ds1820_mqtt
```

### 3) Build and upload SPIFFS image (web UI files)

Use the tools bundled by ESP32 core:

```bash
MK_SPIFFS="$(find "$HOME/.arduino15/packages/esp32/tools/mkspiffs" -type f -name mkspiffs | sort | tail -n1)"
ESPTOOL_BIN="$(find "$HOME/.arduino15/packages/esp32/tools/esptool_py" -type f -name esptool | sort | tail -n1)"
```

For default ESP32 partition table (`default.csv`), SPIFFS is:
- offset `0x290000`
- size `0x160000`

Create and flash image:

```bash
SPIFFS_SIZE=0x160000
SPIFFS_OFFSET=0x290000
SPIFFS_IMAGE=/tmp/esp32-ds1820-spiffs.bin
PORT=/dev/ttyUSB0

"$MK_SPIFFS" -c data -b 4096 -p 256 -s "$SPIFFS_SIZE" "$SPIFFS_IMAGE"
"$ESPTOOL_BIN" --chip esp32 --port "$PORT" --baud 921600 write_flash "$SPIFFS_OFFSET" "$SPIFFS_IMAGE"
```

If you use a different partition scheme, update `SPIFFS_SIZE` and `SPIFFS_OFFSET` from the matching CSV in:
`~/.arduino15/packages/esp32/hardware/esp32/*/tools/partitions/`

### Alternative: PlatformIO CLI

If you prefer PlatformIO, it can upload firmware and filesystem from CLI as well (`upload` + `uploadfs`), but this repo does not currently include a `platformio.ini`.

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
