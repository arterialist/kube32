#!/usr/bin/env bash
# Flash a kube32 build to one ESP32-S3 board.
#
#   scripts/flash.sh <serial port> [baud]
#
# Writes the bootloader, the partition table, an empty otadata partition and
# the firmware into the first app slot, at the offsets partitions.csv gives:
# the board boots the image just flashed, whichever slot a firmware rollout
# had switched it to. NVS and the object store journal are left as they are.
#
# Builds the esp32s3 environment first if the images are missing. PlatformIO
# is taken from PATH, then from $PLATFORMIO_CORE_DIR or ~/.platformio.
set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_DIR"

PORT="${1:-${KUBE32_SERIAL_PORT:-}}"
BAUD="${2:-460800}"
if [ -z "$PORT" ]; then
    echo "usage: scripts/flash.sh <serial port> [baud]   (or set KUBE32_SERIAL_PORT)" >&2
    exit 2
fi

PENV="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/penv/bin"
if command -v pio >/dev/null 2>&1; then
    PIO=pio
elif [ -x "$PENV/pio" ]; then
    PIO="$PENV/pio"
else
    PIO=""
fi
# esptool ships inside PlatformIO's virtualenv; a system python3 works too
# once `pip install esptool` has run.
if [ -x "$PENV/python3" ] && "$PENV/python3" -c "import esptool" 2>/dev/null; then
    PYTHON="$PENV/python3"
elif python3 -c "import esptool" 2>/dev/null; then
    PYTHON=python3
else
    echo "esptool not found: looked for it in $PENV/python3 and in python3 on PATH." >&2
    echo "Install PlatformIO Core (https://platformio.org/install/cli) or run 'pip install esptool'." >&2
    exit 1
fi

echo "flashing $PORT at $BAUD baud"

BOOTLOADER=".pio/build/esp32s3/bootloader.bin"
PARTITIONS=".pio/build/esp32s3/partitions.bin"
FIRMWARE=".pio/build/esp32s3/firmware.bin"
OTADATA=".pio/build/esp32s3/ota_data_initial.bin"

if [ ! -f "$FIRMWARE" ] || [ ! -f "$BOOTLOADER" ] || [ ! -f "$PARTITIONS" ]; then
    if [ -z "$PIO" ]; then
        echo "no firmware images in .pio/build/esp32s3 and no pio to build them:" >&2
        echo "pio is neither on PATH nor in $PENV. Install PlatformIO Core or set PLATFORMIO_CORE_DIR." >&2
        exit 1
    fi
    "$PIO" run -e esp32s3
fi

"$PYTHON" - <<EOF
import sys, time, os
import esptool

port = "$PORT"
baud = int("$BAUD")
bootloader = "$BOOTLOADER"
partitions = "$PARTITIONS"
firmware = "$FIRMWARE"
otadata = "$OTADATA"

# Offsets from partitions.csv: otadata, and the first app slot (ota_0)
otadata_offset = app_offset = None
with open("partitions.csv") as f:
    for line in f:
        cols = [c.strip() for c in line.split("#", 1)[0].split(",")]
        if len(cols) < 5 or not cols[3]:
            continue
        if cols[1] == "data" and cols[2] == "ota":
            otadata_offset, otadata_size = int(cols[3], 0), int(cols[4], 0)
        elif cols[1] == "app" and app_offset is None:
            app_offset = int(cols[3], 0)
if app_offset is None:
    sys.exit("partitions.csv has no app partition")
images = ['0x0', bootloader, '0x8000', partitions]
if otadata_offset is not None:
    # Erased otadata: the bootloader starts from the first app slot
    if not os.path.exists(otadata):
        with open(otadata, "wb") as f:
            f.write(b"\xff" * otadata_size)
    images += [hex(otadata_offset), otadata]
images += [hex(app_offset), firmware]


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
    print("USB-UART bridge: writing 256-byte paced blocks")
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
    '--after', os.environ.get('KUBE32_FLASH_AFTER', 'hard-reset' if bridge else 'watchdog-reset'),   # USB-JTAG reset can leave the chip in download mode
    'write-flash',
    '-u',
    '--flash-mode', 'dio',
    '--flash-size', '8MB',
    '--flash-freq', '80m',
] + images

esptool._main()
EOF

echo "flashed $PORT"
