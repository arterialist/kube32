#!/usr/bin/env python3
"""
Kube32 Automated Dual-Board Flashing & Live Monitoring Tool
Continuously monitors connected ESP32-S3 devices and flashes Kube32 firmware.
Remediated for CH340 FIFO limits, UnicodeDecodeError, port locks, and reset backoff.
"""

import os
import sys
import time
import subprocess
import glob
import argparse

PROJECT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
PIO_PYTHON = "~/.platformio/penv/bin/python3"
ESPTOOL = "~/.platformio/penv/bin/esptool.py"
PYTHON_BIN = PIO_PYTHON if os.path.exists(PIO_PYTHON) else sys.executable

BOOTLOADER = os.path.join(PROJECT_DIR, ".pio/build/esp32s3/bootloader.bin")
PARTITIONS = os.path.join(PROJECT_DIR, ".pio/build/esp32s3/partitions.bin")
FIRMWARE = os.path.join(PROJECT_DIR, ".pio/build/esp32s3/firmware.bin")

KNOWN_PORTS = {
    # Dynamically detected or user-specified serial ports
}

def flash_port(port, baud=460800):
    """Flashes target port with CH340 FIFO protection (256B chunks, pacing, -u)."""
    script_path = os.path.join(os.path.dirname(__file__), "flash.sh")
    if os.path.exists(script_path):
        cmd = ["bash", script_path, port, str(baud)]
    else:
        fallback_script = """
import sys, time, os, esptool

port = sys.argv[1]
baud = int(sys.argv[2])
bootloader = sys.argv[3]
partitions = sys.argv[4]
firmware = sys.argv[5]

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
"""
        cmd = [
            PYTHON_BIN,
            "-c", fallback_script,
            port, str(baud),
            BOOTLOADER, PARTITIONS, FIRMWARE
        ]

    res = subprocess.run(
        cmd,
        cwd=PROJECT_DIR,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace"
    )
    return res.returncode == 0, res.stdout

def probe_port(port):
    """Probes port using 256B block size to prevent CH340 FIFO overflow."""
    probe_script = """
import sys, esptool
port = sys.argv[1]
esptool.loader.DEFAULT_TIMEOUT = 10.0
esptool.loader.ESPLoader.ESP_RAM_BLOCK = 256
sys.argv = ['esptool.py', '--port', port, '--chip', 'esp32s3', '-b', '115200', 'chip-id']
esptool._main()
"""
    cmd = [
        PYTHON_BIN,
        "-c", probe_script,
        port
    ]
    res = subprocess.run(
        cmd,
        cwd=PROJECT_DIR,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace"
    )
    return res.returncode == 0, res.stdout

def is_port_locked(output):
    """Checks whether the error output indicates port locking / resource contention."""
    lock_indicators = [
        "[Errno 35]",
        "Could not exclusively lock port",
        "Resource temporarily unavailable",
        "Permission denied"
    ]
    return any(ind in output for ind in lock_indicators)

def main():
    parser = argparse.ArgumentParser(description="Kube32 Automated Dual-Board Hardware Flashing Daemon")
    parser.add_argument("ports", nargs="*", help="Target serial port(s) (optional)")
    parser.add_argument("--single", action="store_true", help="Exit after successfully flashing one board")
    parser.add_argument("--once", action="store_true", help="Flash currently detected boards once and exit")
    parser.add_argument("--target-count", type=int, default=None, help="Target number of boards to flash (default: 2)")
    parser.add_argument("--max-retries", type=int, default=3, help="Max retry attempts per failing port (default: 3)")
    parser.add_argument("--retry-delay", type=float, default=5.0, help="Delay in seconds between retries (default: 5.0)")
    args = parser.parse_args()

    max_retries = args.max_retries
    retry_delay = args.retry_delay

    if args.ports:
        target_count = len(args.ports)
    elif args.single:
        target_count = 1
    elif args.target_count is not None:
        target_count = args.target_count
    else:
        target_count = 2

    print("=" * 65)
    print("  Kube32 Automated Dual-Board Hardware Flashing Daemon")
    print("=" * 65)
    print(f"Target Board Count: {target_count} | Max Retries: {max_retries} | Retry Backoff: {retry_delay}s")
    print("Waiting for boards in Download Mode...")
    print("  1. Node 1: Hold BOOT (GPIO 0) -> Tap RST / Power ON -> Release BOOT")
    print("  2. Node 2: Hold BOOT (GPIO 0) -> Tap RST / Power ON -> Release BOOT")
    print("-" * 65)

    flashed = set()
    failure_counts = {}
    skipped = set()

    while True:
        if args.ports:
            ports = [p for p in args.ports if os.path.exists(p)]
        else:
            ports = sorted(glob.glob("/dev/cu.usbmodem*"))

        # Clear failure counts for ports that disappeared (allow fresh attempt on replug)
        active_set = set(ports)
        for p in list(failure_counts.keys()):
            if p not in active_set:
                del failure_counts[p]
                skipped.discard(p)

        for port in ports:
            if port in flashed or port in skipped:
                continue

            desc = KNOWN_PORTS.get(port, "ESP32-S3 Board")
            print(f"\n[SCAN] Detected {port} ({desc})")

            # Check if exceeded retries
            if failure_counts.get(port, 0) >= max_retries:
                skipped.add(port)
                print(f"[SKIP] {port} exceeded max retry limit ({max_retries}). Skipping to prevent reset loops.")
                print(f"       (Re-plug board to reset retry counter)")
                continue

            print(f"[PROBE] Attempting flash at 460800 baud...")
            ok, out = flash_port(port, baud=460800)

            if not ok:
                if is_port_locked(out):
                    print(f"[LOCK] {port} is locked by another process (Errno 35).")
                    print(f"       Please close any active serial monitor/debugger sessions.")
                else:
                    print(f"[RETRY] 460800 baud failed, trying 115200 baud...")
                    ok, out = flash_port(port, baud=115200)

            if ok:
                print(f"[SUCCESS] Successfully flashed {port} ({desc})!")
                flashed.add(port)
                failure_counts.pop(port, None)
            else:
                failure_counts[port] = failure_counts.get(port, 0) + 1
                attempts = failure_counts[port]
                last_line = out.strip().splitlines()[-1] if out.strip() else "Timeout"
                print(f"[STATUS] {port}: {last_line} (attempt {attempts}/{max_retries})")

                if attempts >= max_retries:
                    skipped.add(port)
                    print(f"[SKIP] {port} reached max retries. Skipping further attempts.")
                else:
                    print(f"[BACKOFF] Waiting {retry_delay}s before retrying {port}...")
                    time.sleep(retry_delay)

        if len(flashed) >= target_count:
            print("\n" + "=" * 65)
            print(f"  TARGET REACHED: {len(flashed)}/{target_count} BOARD(S) FLASHED SUCCESSFULLY!")
            print("=" * 65)
            return 0

        if args.once:
            print("\n" + "=" * 65)
            print(f"  --once complete: {len(flashed)} board(s) flashed.")
            print("=" * 65)
            return 0 if len(flashed) > 0 else 1

        if len(ports) == 0:
            print("[SCAN] Waiting for boards to be connected...", end="\r", flush=True)
            time.sleep(2)
        else:
            # If all detected ports are either flashed or skipped
            unresolved = [p for p in ports if p not in flashed and p not in skipped]
            if not unresolved:
                print(f"[WAIT] {len(flashed)}/{target_count} flashed. Waiting for remaining boards or replug...", end="\r", flush=True)
                time.sleep(3)
            else:
                time.sleep(1)

if __name__ == "__main__":
    sys.exit(main())
