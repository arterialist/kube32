#!/usr/bin/env bash
# ==============================================================================
# Kube32 Dual Physical ESP32-S3 Firmware Flasher & Verifier
# ==============================================================================
set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_DIR"

PORT="${1:-${KUBE32_SERIAL_PORT:-}}"
BAUD="${2:-460800}"
if [ -z "$PORT" ]; then
    echo "usage: scripts/flash.sh <serial port> [baud]   (or set KUBE32_SERIAL_PORT)" >&2
    exit 2
fi

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

# CH340-style USB-UART bridges (VID 0x1a86) drop bytes with esptool's default
# block size; they get 256-byte paced blocks. Native USB-Serial/JTAG ports
# flash the 1.9 MB image in well under a minute with the defaults.
esptool.loader.DEFAULT_TIMEOUT = 30.0
esptool.loader.WRITE_BLOCK_ATTEMPTS = 5
bridge = False
try:
    import serial.tools.list_ports
    for p in serial.tools.list_ports.comports():
        if p.device == port and p.vid == 0x1A86:
            bridge = True
except Exception:
    pass
if bridge:
    print("[*] USB-UART bridge detected: 256-byte paced blocks")
    esptool.loader.ESPLoader.ESP_RAM_BLOCK = 256
    esptool.loader.StubMixin.FLASH_WRITE_SIZE = 256
    esptool.loader.ESPLoader.FLASH_WRITE_SIZE = 256
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
    '--before', os.environ.get('KUBE32_FLASH_BEFORE', 'default-reset'),   # no-reset when the board is already in download mode
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
