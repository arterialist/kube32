#!/usr/bin/env python3
import sys
import time
import glob
import subprocess
import serial.tools.list_ports

WORKER_PORT_ID = "578E0184461"

print("[MONITOR] Scanning for Cardputer USB connection (VID: 0x303A / usbmodem)...", flush=True)

start_time = time.time()
while True:
    # 1. Inspect serial comports
    for p in serial.tools.list_ports.comports():
        dev = p.device
        hwid = p.hwid or ""
        if WORKER_PORT_ID not in dev and ("usbmodem" in dev or "303a" in hwid.lower()):
            print(f"[DETECTED] Cardputer serial port online: {dev} ({p.description}) [{hwid}]", flush=True)
            sys.exit(0)

    # 2. Inspect /dev/cu.usbmodem* device nodes directly
    for path in glob.glob("/dev/cu.usbmodem*"):
        if WORKER_PORT_ID not in path:
            print(f"[DETECTED] Cardputer device node online: {path}", flush=True)
            sys.exit(0)

    # 3. Inspect IORegistry USB tree for Espressif VID 0x303A
    try:
        res = subprocess.run(["ioreg", "-p", "IOUSB", "-w0", "-l"], capture_output=True, text=True)
        if "303a" in res.stdout.lower():
            time.sleep(1.0)
            ports = [p.device for p in serial.tools.list_ports.comports() if WORKER_PORT_ID not in p.device]
            print(f"[DETECTED] Cardputer hardware connected on USB (ports: {ports})", flush=True)
            sys.exit(0)
    except Exception:
        pass

    time.sleep(1.0)
