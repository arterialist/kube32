#!/usr/bin/env bash
# Flash a kube32 build to one board.
#
#   scripts/flash.sh <serial port> [baud]
#   scripts/flash.sh --build <idf.py build directory> <serial port> [baud]
#
# Writes the bootloader, the partition table, an empty otadata partition and
# the firmware into the first app slot, at the offsets the partition table
# gives: the board boots the image just flashed, whichever slot a firmware
# rollout had switched it to. NVS and the object store journal are left as
# they are.
#
# Without --build it flashes the PlatformIO esp32s3 build, building it first
# if the images are missing. PlatformIO is taken from PATH, then from
# $PLATFORMIO_CORE_DIR or ~/.platformio. With --build it flashes an idf.py
# build of any target (ESP32-S31, C3, P4) as that directory's flash_args
# lists it.
#
# esptool comes from $KUBE32_PYTHON, PlatformIO's virtualenv or python3 on
# PATH, the first with an esptool new enough for the chip: the ESP32-S31
# needs esptool 5.6 or later.
set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_DIR"

BUILD_DIR=""
if [ "$1" = "--build" ]; then
    BUILD_DIR="$2"
    shift 2
    if [ ! -f "$BUILD_DIR/flash_args" ] || [ ! -f "$BUILD_DIR/project_description.json" ]; then
        echo "$BUILD_DIR has no flash_args or project_description.json: build it with idf.py -B $BUILD_DIR build" >&2
        exit 1
    fi
    BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"
fi

PORT="${1:-${KUBE32_SERIAL_PORT:-}}"
BAUD="${2:-460800}"
if [ -z "$PORT" ]; then
    echo "usage: scripts/flash.sh [--build <idf.py build directory>] <serial port> [baud]   (or set KUBE32_SERIAL_PORT)" >&2
    exit 2
fi

if [ -n "$BUILD_DIR" ]; then
    CHIP="$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["target"])' "$BUILD_DIR/project_description.json")"
else
    CHIP=esp32s3
fi
# The oldest esptool that flashes the chip
case "$CHIP" in
    esp32s31) MIN_ESPTOOL=5.6 ;;
    *) MIN_ESPTOOL=4.0 ;;
esac

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
PYTHON=""
for candidate in "${KUBE32_PYTHON:-}" "$PENV/python3" python3; do
    [ -n "$candidate" ] || continue
    if "$candidate" -c '
import sys, esptool
have = tuple(int(x) for x in esptool.__version__.split(".")[:2])
want = tuple(int(x) for x in sys.argv[1].split("."))
sys.exit(0 if have >= want else 1)' "$MIN_ESPTOOL" 2>/dev/null; then
        PYTHON="$candidate"
        break
    fi
done
if [ -z "$PYTHON" ]; then
    echo "no esptool $MIN_ESPTOOL or later for the $CHIP: looked in \$KUBE32_PYTHON, $PENV/python3 and python3 on PATH." >&2
    echo "Install PlatformIO Core (https://platformio.org/install/cli) or run 'pip install \"esptool>=$MIN_ESPTOOL\"'." >&2
    exit 1
fi

echo "flashing $PORT ($CHIP) at $BAUD baud"

BOOTLOADER=".pio/build/esp32s3/bootloader.bin"
PARTITIONS=".pio/build/esp32s3/partitions.bin"
FIRMWARE=".pio/build/esp32s3/firmware.bin"
OTADATA=".pio/build/esp32s3/ota_data_initial.bin"

if [ -z "$BUILD_DIR" ] && { [ ! -f "$FIRMWARE" ] || [ ! -f "$BOOTLOADER" ] || [ ! -f "$PARTITIONS" ]; }; then
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
chip = "$CHIP"
build_dir = "$BUILD_DIR"
bootloader = "$BOOTLOADER"
partitions = "$PARTITIONS"
firmware = "$FIRMWARE"
otadata = "$OTADATA"

if build_dir:
    # idf.py's flash_args: the flash settings, then the bootloader (at the
    # chip's own offset), the partition table, an empty otadata and the app
    # in the first slot
    with open(os.path.join(build_dir, "flash_args")) as f:
        lines = [l.split() for l in f if l.strip()]
    flash_opts = lines[0]
    images = []
    for offset, path in lines[1:]:
        images += [offset, os.path.join(build_dir, path)]
else:
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
    flash_opts = ['--flash-mode', 'dio', '--flash-size', '8MB', '--flash-freq', '80m']


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

# A reset over the ESP32-S3's USB-JTAG can leave it in download mode: the
# watchdog resets it instead
after = 'watchdog-reset' if chip == 'esp32s3' and not bridge else 'hard-reset'
sys.argv = [
    'esptool.py',
    '--port', port,
    '--chip', chip,
    '-b', str(baud),
    '--before', os.environ.get('KUBE32_FLASH_BEFORE', 'default-reset'),   # no-reset when the board is already in download mode
    '--after', os.environ.get('KUBE32_FLASH_AFTER', after),
    'write-flash',
    '-u',
] + flash_opts + images

esptool._main()
EOF

echo "flashed $PORT"
