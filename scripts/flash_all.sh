#!/usr/bin/env bash
set -euo pipefail

# Flash firmware + SPIFFS data for esp32-ds1820-mqtt.
#
# Environment variables:
#   PORT=/dev/ttyUSB0                    serial port (required)
#   FQBN=esp32:esp32:esp32               board fqbn
#   SKETCH_DIR=esp32_ds1820_mqtt         sketch directory
#   DATA_DIR=data                         SPIFFS source directory
#   SPIFFS_SIZE=0x160000                 SPIFFS size
#   SPIFFS_OFFSET=0x290000               SPIFFS flash offset
#   BAUD=921600                          esptool write_flash baud rate
#   SPIFFS_IMAGE=/tmp/esp32-spiffs.bin   generated image path

PORT="${PORT:-}"
FQBN="${FQBN:-esp32:esp32:esp32}"
SKETCH_DIR="${SKETCH_DIR:-esp32_ds1820_mqtt}"
DATA_DIR="${DATA_DIR:-data}"
SPIFFS_SIZE="${SPIFFS_SIZE:-0x160000}"
SPIFFS_OFFSET="${SPIFFS_OFFSET:-0x290000}"
BAUD="${BAUD:-921600}"
SPIFFS_IMAGE="${SPIFFS_IMAGE:-/tmp/esp32-ds1820-spiffs.bin}"

if [[ -z "$PORT" ]]; then
  echo "ERROR: PORT is required, e.g. PORT=/dev/ttyUSB0 $0" >&2
  exit 1
fi

if ! command -v arduino-cli >/dev/null 2>&1; then
  echo "ERROR: arduino-cli not found in PATH" >&2
  exit 1
fi

if [[ ! -d "$SKETCH_DIR" ]]; then
  echo "ERROR: sketch directory not found: $SKETCH_DIR" >&2
  exit 1
fi

if [[ ! -d "$DATA_DIR" ]]; then
  echo "ERROR: data directory not found: $DATA_DIR" >&2
  exit 1
fi

MK_SPIFFS="$(find "$HOME/.arduino15/packages/esp32/tools/mkspiffs" -type f -name mkspiffs 2>/dev/null | sort | tail -n1 || true)"
ESPTOOL_BIN="$(find "$HOME/.arduino15/packages/esp32/tools/esptool_py" -type f -name esptool 2>/dev/null | sort | tail -n1 || true)"

if [[ -z "$MK_SPIFFS" || ! -x "$MK_SPIFFS" ]]; then
  echo "ERROR: mkspiffs not found. Install ESP32 core with: arduino-cli core install esp32:esp32" >&2
  exit 1
fi

if [[ -z "$ESPTOOL_BIN" || ! -x "$ESPTOOL_BIN" ]]; then
  echo "ERROR: esptool not found. Install ESP32 core with: arduino-cli core install esp32:esp32" >&2
  exit 1
fi

echo "==> Compiling firmware ($FQBN)"
arduino-cli compile --fqbn "$FQBN" "$SKETCH_DIR"

echo "==> Uploading firmware to $PORT"
arduino-cli upload -p "$PORT" --fqbn "$FQBN" "$SKETCH_DIR"

echo "==> Building SPIFFS image from $DATA_DIR"
"$MK_SPIFFS" -c "$DATA_DIR" -b 4096 -p 256 -s "$SPIFFS_SIZE" "$SPIFFS_IMAGE"

echo "==> Uploading SPIFFS image to $PORT (offset $SPIFFS_OFFSET)"
"$ESPTOOL_BIN" --chip esp32 --port "$PORT" --baud "$BAUD" write_flash "$SPIFFS_OFFSET" "$SPIFFS_IMAGE"

echo "Done."
