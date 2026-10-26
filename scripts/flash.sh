#!/usr/bin/env bash
# ==============================================================================
# Kube32 Dual Physical ESP32-S3 Firmware Flasher & Verifier
# ==============================================================================
set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_DIR"

PORT="${1:-/dev/cu.usbmodem578E0184461}"
BAUD="${2:-460800}"

echo "================================================================="
echo "  Kube32 Production ESP32-S3 Silicon Flasher"
echo "  Target Port: $PORT"
echo "  Baud Rate:   $BAUD"
echo "================================================================="

BOOTLOADER=".pio/build/esp32s3/bootloader.bin"
PARTITIONS=".pio/build/esp32s3/partitions.bin"
FIRMWARE=".pio/build/esp32s3/firmware.bin"

if [ ! -f "$FIRMWARE" ] || [ ! -f "$BOOTLOADER" ] || [ ! -f "$PARTITIONS" ]; then
    echo "[BUILD] Compiling firmware via PlatformIO..."
    ~/.platformio/penv/bin/pio run -e esp32s3
fi

echo "[FLASH] Executing hardware flashing on physical silicon..."
~/.platformio/penv/bin/python3 - <<EOF
import sys, time, os
import esptool

port = "$PORT"
baud = int("$BAUD")
bootloader = "$BOOTLOADER"
partitions = "$PARTITIONS"
firmware = "$FIRMWARE"

print(f"[*] Connecting to ESP32-S3 on {port} at {baud} baud...")

# Ensure robust serial transmission across USB-UART bridges (e.g. CH340 / USB-CDC)
esptool.loader.DEFAULT_TIMEOUT = 30.0
esptool.loader.ESPLoader.ESP_RAM_BLOCK = 256
esptool.loader.StubMixin.FLASH_WRITE_SIZE = 256
esptool.loader.ESPLoader.FLASH_WRITE_SIZE = 256
esptool.loader.WRITE_BLOCK_ATTEMPTS = 5

orig_flash_block = esptool.loader.ESPLoader.flash_block
def paced_flash_block(self, data, seq, timeout=20.0, encrypted=False):
    res = orig_flash_block(self, data, seq, timeout=20.0, encrypted=encrypted)
    time.sleep(0.0015)
    return res
esptool.loader.ESPLoader.flash_block = paced_flash_block

sys.argv = [
    'esptool.py',
    '--port', port,
    '--chip', 'esp32s3',
    '-b', str(baud),
    '--before', 'default-reset',
    '--after', 'hard-reset',
    'write-flash',
    '-u',
    '--flash-mode', 'dio',
    '--flash-size', '8MB',
    '--flash-freq', '80m',
    '0x0', bootloader,
    '0x8000', partitions,
    '0x20000', firmware
]

esptool._main()
print("[+] Flashing complete on silicon!")
EOF

echo "================================================================="
echo "  Hardware Flashing Successful! Board hard reset triggered."
echo "================================================================="
