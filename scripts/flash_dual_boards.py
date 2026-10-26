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
    "/dev/cu.usbmodem101": "Node 1 (8MB Flash, Master / Control Plane)",
    "/dev/cu.usbmodem2101": "Node 1 (8MB Flash, Master / Control Plane)",
    "/dev/cu.usbmodem578E0184461": "Node 2 (2MB PSRAM, Worker Node 02)"
}

def flash_port(port, baud=460800):
    script_path = os.path.join(os.path.dirname(__file__), "flash.sh")
    if os.path.exists(script_path):
        cmd = ["bash", script_path, port, str(baud)]
    else:
        cmd = [
            ESPTOOL,
            "--port", port,
            "--chip", "esp32s3",
            "-b", str(baud),
            "--before", "default-reset",
            "--after", "hard-reset",
            "write-flash",
            "-u",
            "--flash-mode", "dio",
            "--flash-size", "8MB",
            "--flash-freq", "80m",
            "0x0", BOOTLOADER,
            "0x8000", PARTITIONS,
            "0x20000", FIRMWARE
        ]
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    return res.returncode == 0, res.stdout

def probe_port(port):
    cmd = [
        ESPTOOL,
        "--port", port,
        "--chip", "esp32s3",
        "-b", "115200",
        "chip-id"
    ]
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    return res.returncode == 0, res.stdout

def main():
    print("=" * 65)
    print("  Kube32 Automated Dual-Board Hardware Flashing Daemon")
    print("=" * 65)
    print("Waiting for both boards in Download Mode...")
    print("  1. Cardputer: Switch OFF -> Hold G0 -> Plug in USB / Switch ON")
    print("  2. Dev Board: Hold BOOT -> Tap RST -> Release BOOT")
    print("-" * 65)
    
    flashed = set()
    while len(flashed) < 2:
        ports = glob.glob("/dev/cu.usbmodem*")
        for port in ports:
            if port in flashed:
                continue
            desc = KNOWN_PORTS.get(port, "ESP32-S3 Board")
            print(f"\n[SCAN] Detected {port} ({desc})")
            print(f"[PROBE] Attempting flash at 460800 baud...")
            ok, out = flash_port(port, baud=460800)
            if not ok:
                print(f"[RETRY] 460800 baud failed, trying 115200 baud...")
                ok, out = flash_port(port, baud=115200)
            if ok:
                print(f"[SUCCESS] Successfully flashed {port} ({desc})!")
                flashed.add(port)
            else:
                last_line = out.strip().splitlines()[-1] if out.strip() else "Timeout"
                print(f"[STATUS] {port}: {last_line}")
                time.sleep(3) # Throttled backoff on flash failure
        time.sleep(2)
        if len(ports) == 0:
            print("[SCAN] Waiting for boards to be connected...", end="\r", flush=True)

    print("\n" + "=" * 65)
    print("  ALL PHYSICAL BOARDS FLASHED SUCCESSFULLY!")
    print("=" * 65)
    return 0

if __name__ == "__main__":
    sys.exit(main())
