#!/usr/bin/env python3
"""Flash kube32 onto the kit: partition table and app slot only.

Private tool (branch local/kit). It never writes the bootloader, otadata,
sysmeta, vibe_mode or coredump, and it refuses any offset inside the retained
first 2 MiB except the partition table at 0x8000. By default it only prints
the esptool command (dry run); pass --write to run it.

Usage:
  tools/kit_flash.py --build build-esp32s31 --port /dev/cu.usbmodemXXXX [--write]

Recovery (read this before the first flash):
  * The chip enters ROM download mode with: power off, hold Boot, press Power.
    The screen stays black in that mode.
  * To return the kit to factory state: browser flasher at mosaico.ai/en/firmware
    (Latest Official Firmware), or `python mosaico.py recover` from the vibe repo
    (writes bootloader 0x2000, partition table 0x8000, ota_data, vibe-mode at
    0x20000), then Manage (+) -> code 100000 in Vibe Mode to reinstall Mosaico-Claw.
  * Never `esptool erase-flash` on a kit: sysmeta (device identity) lives at
    0xc000-0x20000 and the factory image holds calibration and identity data.
"""
import argparse
import csv
import os
import shutil
import subprocess
import sys

RETAINED_END = 0x200000     # the factory's first 2 MiB are read-only by construction
PARTITION_TABLE_OFFSET = 0x8000
BAUD = 921600


def csv_rows(path):
    with open(path) as f:
        return [[c.strip() for c in row] for row in csv.reader(l for l in f if l.strip() and not l.strip().startswith("#"))]


def app_slot(csv_path, name="ota_0"):
    for row in csv_rows(csv_path):
        if row[0] == name:
            return int(row[3], 0), int(row[4], 0)
    raise SystemExit("no %s partition in %s" % (name, csv_path))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True, help="the idf.py build directory")
    ap.add_argument("--port", required=True)
    ap.add_argument("--partitions", default="partitions-kit.csv")
    ap.add_argument("--slot", default="ota_0", help="app slot to write (default ota_0)")
    ap.add_argument("--write", action="store_true", help="run esptool (default: print the command only)")
    args = ap.parse_args()

    app_bin = os.path.join(args.build, "kube32.bin")
    table_bin = os.path.join(args.build, "partition_table", "partition-table.bin")
    for path in (app_bin, table_bin, args.partitions):
        if not os.path.exists(path):
            sys.exit("missing %s" % path)

    offset, size = app_slot(args.partitions, args.slot)
    if offset < RETAINED_END:
        sys.exit("refused: slot %s at 0x%x is inside the retained prefix (below 0x%x)" % (args.slot, offset, RETAINED_END))
    image = os.path.getsize(app_bin)
    if image > size:
        sys.exit("refused: image is %d bytes, slot %s holds %d" % (image, args.slot, size))
    print("image %d bytes, %.1f%% of slot %s at 0x%x" % (image, 100.0 * image / size, args.slot, offset))

    esptool = shutil.which("esptool") or shutil.which("esptool.py")
    if not esptool:
        sys.exit("no esptool in PATH (5.6 or newer knows this chip)")
    cmd = [esptool, "--chip", "esp32s31", "--port", args.port, "--baud", str(BAUD),
           "--before", "default-reset", "--after", "hard-reset",
           "write-flash", "--flash-size", "16MB",
           hex(PARTITION_TABLE_OFFSET), table_bin, hex(offset), app_bin]
    print(" ".join(cmd))
    if not args.write:
        print("dry run: nothing written (pass --write to flash)")
        return 0
    return subprocess.call(cmd)


if __name__ == "__main__":
    sys.exit(main())
