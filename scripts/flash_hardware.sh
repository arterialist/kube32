#!/usr/bin/env bash
# ==============================================================================
# Kube32 Physical Hardware Flashing & Monitor Script
# ==============================================================================
set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_DIR"

ESPTOOL="~/.platformio/penv/bin/esptool.py"
BOOTLOADER=".pio/build/esp32s3/bootloader.bin"
PARTITIONS=".pio/build/esp32s3/partitions.bin"
FIRMWARE=".pio/build/esp32s3/firmware.bin"

PORT="${1:-/dev/cu.usbmodem2101}"
BAUD="${2:-460800}"

echo "================================================================="
echo "  Kube32 ESP32-S3 Firmware Flasher"
echo "  Target Port: $PORT"
echo "  Baud Rate:   $BAUD"
echo "================================================================="

if [ ! -f "$FIRMWARE" ]; then
    echo "[BUILD] Compiling firmware via PlatformIO..."
    ~/.platformio/penv/bin/pio run -e esp32s3
fi

echo "[FLASH] Writing bootloader (0x0), partitions (0x8000), and firmware (0x20000)..."
"$ESPTOOL" --port "$PORT" --chip esp32s3 -b "$BAUD" \
    --before default_reset --after hard_reset write_flash \
    --flash_mode dio --flash_size 8MB --flash_freq 80m \
    0x0 "$BOOTLOADER" \
    0x8000 "$PARTITIONS" \
    0x20000 "$FIRMWARE"

echo "================================================================="
echo "  Flashing Complete! Launching Serial Monitor..."
echo "================================================================="
python3 -m serial.tools.miniterm "$PORT" 115200
