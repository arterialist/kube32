#!/usr/bin/env python3
import sys
import time
import glob
import serial.tools.list_ports

print("[MONITOR] Waiting for ESP32 boards to connect on USB...", flush=True)

while True:
    ports = glob.glob("/dev/cu.usbmodem*")
    cardputer = [p for p in ports if "578E018446" not in p]
    dev_board = [p for p in ports if "578E018446" in p]

    if cardputer and dev_board:
        print(f"[DETECTED] Both physical boards online!", flush=True)
        print(f"  Node 1 (Cardputer): {cardputer[0]}", flush=True)
        print(f"  Node 2 (Dev Board): {dev_board[0]}", flush=True)
        sys.exit(0)
    elif cardputer:
        print(f"[DETECTED] Cardputer online at {cardputer[0]}, waiting for dev board...", flush=True)
    elif dev_board:
        print(f"[DETECTED] Dev board online at {dev_board[0]}, waiting for Cardputer...", flush=True)

    time.sleep(1.0)
