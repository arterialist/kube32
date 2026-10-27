#!/usr/bin/env python3
import sys
import time
import glob

print("[MONITOR] Waiting for physical ESP32 boards to connect on USB...", flush=True)

while True:
    ports = glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.usbserial*") + glob.glob("/dev/cu.wch*")
    if len(ports) >= 2:
        print(f"[DETECTED] Both physical boards detected on USB ({len(ports)} ports)!", flush=True)
        for i, p in enumerate(ports[:2]):
            print(f"  Port {i+1}: {p}", flush=True)
        sys.exit(0)
    elif len(ports) == 1:
        print(f"[DETECTED] 1 physical board online at {ports[0]}...", flush=True)

    time.sleep(1.5)
