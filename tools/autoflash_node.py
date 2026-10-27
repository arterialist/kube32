#!/usr/bin/env python3
"""
Auto-flash worker for Kube32 ESP32-S3 cluster nodes.
Monitors serial ports and automatically flashes the latest compiled firmware
as soon as the target device is connected.
"""

import os
import sys
import time
import subprocess
import serial.tools.list_ports

FIRMWARE_BIN = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), ".pio", "build", "esp32s3", "firmware.bin")
PYTHON_BIN = "~/.platformio/penv/bin/python3"
KNOWN_WORKER_SERIAL = "578E018446"

def get_target_port():
    ports = serial.tools.list_ports.comports()
    for p in ports:
        # Ignore known worker node
        if p.serial_number == KNOWN_WORKER_SERIAL or (p.hwid and KNOWN_WORKER_SERIAL in p.hwid):
            continue
        # Look for Espressif native USB-JTAG or target device
        if p.vid == 0x303A:
            return p.device
        if p.description and ("JTAG" in p.description or "Espressif" in p.description):
            return p.device
    for p in ports:
        if p.serial_number == KNOWN_WORKER_SERIAL or (p.hwid and KNOWN_WORKER_SERIAL in p.hwid):
            continue
        if "usbmodem" in p.device or "ttyACM" in p.device:
            return p.device
    return None

def main():
    if not os.path.exists(FIRMWARE_BIN):
        print(f"[AUTOFLASH-ERR] Firmware binary not found at {FIRMWARE_BIN}")
        sys.exit(1)

    print(f"[AUTOFLASH] Monitoring USB ports for target device...")
    print(f"[AUTOFLASH] Ready to flash: {FIRMWARE_BIN}")

    target_port = None
    while target_port is None:
        target_port = get_target_port()
        if target_port:
            break
        time.sleep(0.3)

    print(f"[AUTOFLASH] Detected target device on port: {target_port}")
    # Allow USB connection and bus settling
    time.sleep(0.5)

    flash_cmd = [
        PYTHON_BIN, "-m", "esptool",
        "--chip", "esp32s3",
        "--port", target_port,
        "--baud", "921600",
        "write_flash", "0x20000", FIRMWARE_BIN
    ]

    print(f"[AUTOFLASH] Flashing firmware to {target_port}...")
    res = subprocess.run(flash_cmd)
    if res.returncode != 0:
        print("[AUTOFLASH-ERR] Flash failed. Retrying at 460800 baud...")
        time.sleep(1.0)
        flash_cmd[6] = "460800"
        res = subprocess.run(flash_cmd)

    if res.returncode == 0:
        print("[AUTOFLASH] Flash successful!")
        time.sleep(0.5)
        # Reset device to boot into the new firmware
        reset_cmd = [PYTHON_BIN, "-m", "esptool", "--chip", "esp32s3", "--port", target_port, "run"]
        subprocess.run(reset_cmd)
        print("[AUTOFLASH] Target node reset into new quiet firmware.")

        # Give the device a moment to re-enumerate CDC before starting services
        time.sleep(2.0)

        # Launch gateway in background if requested
        print("[AUTOFLASH] Starting Kube32 API Server Gateway...")
        gateway_script = os.path.join(os.path.dirname(os.path.abspath(__file__)), "kube32_serial_gateway.py")
        subprocess.Popen([sys.executable, gateway_script, "6443"], env=dict(os.environ, KUBE32_SERIAL_PORT=target_port))

        time.sleep(1.5)
        print("[AUTOFLASH] Starting kubectl port-forward for hardware workbench...")
        subprocess.Popen([
            "kubectl", "--context", "kube32", "port-forward",
            "-n", "kube32-cluster", "svc/hardware-workbench-svc", "8080:80"
        ])
        print("[AUTOFLASH] Cluster services online at http://localhost:8080")
    else:
        print("[AUTOFLASH-ERR] Failed to flash target device.")

if __name__ == "__main__":
    main()
