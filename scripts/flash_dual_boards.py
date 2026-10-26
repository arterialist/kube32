#!/usr/bin/env python3
"""
Kube32 Automated Dual-Board Flashing & Live Monitoring Tool
Continuously monitors connected ESP32-S3 devices and flashes Kube32 firmware.
"""

import os
import sys
import time
import subprocess
import glob

ESPTOOL = "~/.platformio/penv/bin/esptool.py"
BOOTLOADER = ".pio/build/esp32s3/bootloader.bin"
PARTITIONS = ".pio/build/esp32s3/partitions.bin"
FIRMWARE = ".pio/build/esp32s3/firmware.bin"

KNOWN_PORTS = {
    "/dev/cu.usbmodem2101": "Node 1 (8MB Flash, Master / Control Plane)",
    "/dev/cu.usbmodem578E0184461": "Node 2 (2MB PSRAM, Worker Node 02)"
}

def flash_port(port, baud=460800):
    cmd = [
        ESPTOOL,
        "--port", port,
        "--chip", "esp32s3",
        "-b", str(baud),
        "--before", "default_reset",
        "--after", "hard_reset",
        "write_flash",
        "--flash_mode", "dio",
        "--flash_size", "8MB",
        "--flash_freq", "80m",
        "0x0", BOOTLOADER,
        "0x8000", PARTITIONS,
        "0x20000", FIRMWARE
    ]
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return res.returncode == 0, res.stdout

def probe_port(port):
    cmd = [
        ESPTOOL,
        "--port", port,
        "--chip", "esp32s3",
        "-b", "115200",
        "chip_id"
    ]
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return res.returncode == 0, res.stdout

def main():
    print("=" * 65)
    print("  Kube32 Automated Dual-Board Hardware Flashing Daemon")
    print("=" * 65)
    
    flashed = set()
    ports = glob.glob("/dev/cu.usbmodem*")
    print(f"[SCAN] Found USB serial devices: {ports}")
    
    for port in ports:
        desc = KNOWN_PORTS.get(port, "Unknown ESP32-S3")
        print(f"\n[TARGET] Probing {port} ({desc})...")
        ok, out = probe_port(port)
        if ok:
            print(f"[PROBE] {port} is in download mode! Initiating firmware flash...")
            f_ok, f_out = flash_port(port)
            if f_ok:
                print(f"[SUCCESS] Flashed {port} cleanly!")
                flashed.add(port)
            else:
                print(f"[FAIL] Flash failed on {port}:\n{f_out}")
        else:
            print(f"[WAIT] {port} needs manual bootloader trigger (Hold BOOT, click RST).")
            # Try flashing at 115200 just in case
            f_ok, f_out = flash_port(port, baud=115200)
            if f_ok:
                print(f"[SUCCESS] Flashed {port} at 115200 baud!")
                flashed.add(port)
            else:
                print(f"       Status: {f_out.splitlines()[-1] if f_out else 'No response'}")

    return 0

if __name__ == "__main__":
    sys.exit(main())
