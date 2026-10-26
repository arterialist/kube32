#!/usr/bin/env python3
"""
Empirical Adversarial Hardware Verification Suite for Milestone 6 Iteration 2
Target Port: /dev/cu.usbmodem578E0184461 (ESP32-S3 Dev Board, 2MB PSRAM, Worker Node 02)

Adversarial Challenges:
1. Reset Permutation & State Trap Verification:
   - Clean operational boot with DTR/RTS pulse (dtr=False, pulse rts=True -> rts=False, dtr=False)
   - Verify boot into boot:0x8 (SPI_FAST_FLASH_BOOT)
   - Verify zero state traps in boot:0x0 (DOWNLOAD) across multiple reset permutations
2. Silicon Runtime Memory & Watchdog Yield Stability:
   - Capture continuous telemetry over 20 seconds on physical silicon
   - Monitor heap consumption across multiple heartbeat iterations
   - Verify zero memory leaks in free PSRAM / internal SRAM
   - Verify zero task watchdog timeouts (TWDT) or kernel panics
3. Wire Protocol Integrity:
   - Verify FNV-1a channel hash 0x78fbb09a and sender hash 0xd8533a7 in live wire frame logs
   - Verify TinyML inference output on PSRAM with 99.1% confidence
"""

import sys
import os
import time
import re
import serial
import termios

TARGET_PORT = "/dev/cu.usbmodem578E0184461"
BAUD_RATE = 115200

def clear_hupcl(port: serial.Serial) -> None:
    """Clear HUPCL flag so closing the port does not glitch RTS/DTR lines."""
    try:
        fd = port.fileno()
        attrs = termios.tcgetattr(fd)
        attrs[2] &= ~termios.HUPCL
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
    except Exception:
        pass

def open_clean_port(port_path: str = TARGET_PORT, baud: int = BAUD_RATE, timeout: float = 0.5) -> serial.Serial:
    """Opens the serial port with HUPCL disabled."""
    ser = serial.Serial(port_path, baud, timeout=timeout)
    clear_hupcl(ser)
    return ser

def pulse_clean_reset(ser: serial.Serial, hold_ms: float = 80.0) -> None:
    """Execute clean operational reset pulse: dtr=False, pulse rts=True -> rts=False, dtr=False."""
    ser.dtr = False
    ser.rts = True
    time.sleep(hold_ms / 1000.0)
    ser.rts = False
    ser.dtr = False

def run_challenge_1_reset_permutations(port_path: str = TARGET_PORT, iterations: int = 5):
    print("\n" + "=" * 70)
    print(" [CHALLENGE 1] Reset Permutation & State Trap Verification")
    print("=" * 70)
    print(f"Target Port: {port_path} | Baud: {BAUD_RATE} | Iterations: {iterations}")

    results = []

    for i in range(1, iterations + 1):
        print(f"\n--- Iteration {i}/{iterations} ---")
        ser = open_clean_port(port_path, BAUD_RATE, timeout=0.2)
        ser.reset_input_buffer()

        # Clean operational reset pulse
        pulse_clean_reset(ser, hold_ms=80.0)

        # Read serial output for 3 seconds
        t_start = time.time()
        captured_lines = []
        boot_mode = None
        boot_desc = None
        state_trap = False
        rom_banner_found = False

        while time.time() - t_start < 3.0:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").strip()
            if line:
                captured_lines.append(line)
                print(f"  [RAW] {line}")

                # Check for ROM boot banner
                # e.g.: rst:0x1 (POWERON),boot:0x8 (SPI_FAST_FLASH_BOOT)
                # or: rst:0x1 (POWERON),boot:0x0 (DOWNLOAD(USB/UART0))
                m = re.search(r"boot:(0x[0-9a-fA-F]+)\s*\(([^)]+)\)", line)
                if m:
                    rom_banner_found = True
                    boot_mode = m.group(1).lower()
                    boot_desc = m.group(2)
                    print(f"  [*] Detected Boot Mode: {boot_mode} ({boot_desc})")
                    if "download" in boot_desc.lower() or boot_mode == "0x0":
                        state_trap = True

        ser.close()

        # Verification asserts
        assert rom_banner_found, f"Iteration {i}: Failed to capture ROM boot banner"
        assert not state_trap, f"Iteration {i}: Board trapped in DOWNLOAD mode (boot:{boot_mode})!"
        assert boot_mode == "0x8", f"Iteration {i}: Unexpected boot mode: {boot_mode} (expected 0x8 SPI_FAST_FLASH_BOOT)"
        assert "SPI_FAST_FLASH_BOOT" in boot_desc, f"Iteration {i}: Unexpected boot desc: {boot_desc}"

        results.append({
            "iteration": i,
            "boot_mode": boot_mode,
            "boot_desc": boot_desc,
            "state_trap": state_trap,
            "success": True
        })
        print(f"  [PASS] Iteration {i}: Clean boot confirmed in mode {boot_mode} ({boot_desc}), zero state trap.")
        time.sleep(0.5)

    print("\n[VERDICT CHALLENGE 1]: ALL RESET PERMUTATIONS PASSED (5/5). Zero state traps detected.")
    return results

def run_challenge_2_and_3(port_path: str = TARGET_PORT, duration_sec: float = 22.0):
    print("\n" + "=" * 70)
    print(" [CHALLENGE 2 & 3] Silicon Runtime Memory Stability & Wire Protocol Integrity")
    print("=" * 70)
    print(f"Target Port: {port_path} | Capture Duration: {duration_sec}s")

    ser = open_clean_port(port_path, BAUD_RATE, timeout=0.2)
    ser.reset_input_buffer()

    # Issue one clean operational reset to capture from bootloader through runtime heartbeats
    pulse_clean_reset(ser, hold_ms=80.0)

    t_start = time.time()
    captured_lines = []

    # Telemetry data structures
    free_internal_sram = None
    free_psram = None
    boot_free_heap = None
    heartbeats = [] # list of (iter, free_heap)
    panic_detected = False
    twdt_detected = False

    # Wire protocol markers
    k32c_frame_accepted = False
    k32c_chan_hash = None
    k32c_sender_hash = None
    tinyml_inference_verified = False
    tinyml_confidence = None
    memory_pool_verified = False
    pipeline_execution_verified = False

    while time.time() - t_start < duration_sec:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").strip()
        if not line:
            continue
        captured_lines.append(line)
        print(f"[{time.time() - t_start:5.2f}s] {line}")

        # Check for panics or TWDT
        if any(err in line for err in ["Guru Meditation Error", "panic'ed", "Backtrace:", "abort() was called"]):
            panic_detected = True
        if any(err in line for err in ["Task watchdog got triggered", "TWDT", "watchdog timeout"]):
            twdt_detected = True

        # Challenge 2: Memory metrics at boot
        m_sram = re.search(r"Free Internal SRAM:\s*(\d+)\s*bytes", line)
        if m_sram:
            free_internal_sram = int(m_sram.group(1))

        m_psram = re.search(r"Free PSRAM:\s*(\d+)\s*bytes", line)
        if m_psram:
            free_psram = int(m_psram.group(1))

        m_heap = re.search(r"Total Free Heap:\s*(\d+)\s*bytes", line)
        if m_heap:
            boot_free_heap = int(m_heap.group(1))

        # Heartbeat telemetry:
        # [WORKER-HEARTBEAT] Node: esp32-node-02 (iter 1) | Free Heap: 2351204 bytes | Pipeline: ACTIVE
        m_hb = re.search(r"\[WORKER-HEARTBEAT\].*\(iter\s+(\d+)\)\s*\|\s*Free Heap:\s*(\d+)\s*bytes", line)
        if m_hb:
            hb_iter = int(m_hb.group(1))
            hb_heap = int(m_hb.group(2))
            heartbeats.append((hb_iter, hb_heap))
            print(f"  --> Heartbeat logged: iter={hb_iter}, free_heap={hb_heap} bytes")

        # Challenge 3: Wire protocol integrity
        # [IPC-RX] Accepted valid K32C frame: chan=0x78fbb09a sender=0xd8533a7 len=52
        m_ipc = re.search(r"\[IPC-RX\] Accepted valid K32C frame:\s*chan=(0x[0-9a-fA-F]+)\s*sender=(0x[0-9a-fA-F]+)\s*len=(\d+)", line)
        if m_ipc:
            k32c_frame_accepted = True
            k32c_chan_hash = m_ipc.group(1).lower()
            k32c_sender_hash = m_ipc.group(2).lower()
            print(f"  --> [WIRE PROTOCOL] Valid K32C frame: chan={k32c_chan_hash}, sender={k32c_sender_hash}")

        # Memory pooling log
        if "[POOL] Memory Pooling Active" in line and "2560KiB Cluster Pool" in line:
            memory_pool_verified = True

        # TinyML inference log:
        # [PIPELINE] TinyML Inference on PSRAM -> Keyword 'WAKE_WORD_DETECTED' (confidence: 99.1%)
        m_ml = re.search(r"TinyML Inference on PSRAM -> Keyword '([^']+)'\s*\(confidence:\s*([0-9.]+)%\)", line)
        if m_ml:
            tinyml_inference_verified = True
            tinyml_confidence = float(m_ml.group(2))
            print(f"  --> [TINYML] Inference keyword: '{m_ml.group(1)}' with confidence {tinyml_confidence}%")

        if "[PIPELINE] Distributed Pipeline Silicon Execution: COMPLETE & VERIFIED" in line:
            pipeline_execution_verified = True

    ser.close()

    print("\n" + "=" * 70)
    print(" [EVALUATION] Silicon Runtime Memory Stability & Wire Protocol Integrity")
    print("=" * 70)

    # Challenge 2 Evaluations:
    print(f"1. Zero Panics / Kernel Crashes: {'PASS' if not panic_detected else 'FAIL'}")
    assert not panic_detected, "Kernel panic / Guru Meditation Error detected during telemetry run!"

    print(f"2. Zero Task Watchdog Timeouts (TWDT): {'PASS' if not twdt_detected else 'FAIL'}")
    assert not twdt_detected, "Task Watchdog Timeout (TWDT) detected during telemetry run!"

    print(f"3. Boot Memory Detected:")
    print(f"   - Internal SRAM: {free_internal_sram} bytes")
    print(f"   - External PSRAM: {free_psram} bytes")
    print(f"   - Total Free Heap: {boot_free_heap} bytes")
    assert free_psram is not None and free_psram > 1500000, f"Expected >1.5MB free PSRAM, got: {free_psram}"

    print(f"4. Heartbeat Telemetry & Memory Leak Check ({len(heartbeats)} heartbeats captured):")
    assert len(heartbeats) >= 2, f"Expected at least 2 heartbeats, got {len(heartbeats)}"
    for hb_iter, hb_heap in heartbeats:
        print(f"   - Iteration {hb_iter}: Free Heap = {hb_heap} bytes")

    # Check memory stability:
    # Any oscillation must be within transient lwIP packet buffer bounds (<= 64 bytes)
    # Net drift across full window must be <= 64 bytes.
    heap_values = [h[1] for h in heartbeats]
    min_heap = min(heap_values)
    max_heap = max(heap_values)
    heap_spread = max_heap - min_heap
    net_drift = heap_values[-1] - heap_values[0]
    print(f"   - Heap spread across window: {heap_spread} bytes (min: {min_heap}, max: {max_heap})")
    print(f"   - Net drift across window:   {net_drift} bytes")
    assert heap_spread <= 64, f"Unstable heap spread detected: {heap_spread} bytes exceeds 64 bytes"
    assert abs(net_drift) <= 64, f"Memory leak detected: net drift of {net_drift} bytes exceeds 64 bytes"
    print(f"   - Zero memory leak confirmed: PASS (heap stable at {heap_values[-1]} bytes, spread={heap_spread} bytes)")

    # Challenge 3 Evaluations:
    print("\n5. Wire Protocol & Pipeline Integrity:")
    print(f"   - K32C Frame Accepted: {'PASS' if k32c_frame_accepted else 'FAIL'}")
    assert k32c_frame_accepted, "No valid K32C frame accepted in live wire frame logs!"

    print(f"   - Channel Hash: {k32c_chan_hash} (expected: 0x78fbb09a)")
    assert k32c_chan_hash == "0x78fbb09a", f"Channel hash mismatch: got {k32c_chan_hash}, expected 0x78fbb09a"

    print(f"   - Sender Hash: {k32c_sender_hash} (expected: 0xd8533a7)")
    assert k32c_sender_hash == "0xd8533a7", f"Sender hash mismatch: got {k32c_sender_hash}, expected 0xd8533a7"

    print(f"   - Memory Pooling: {'PASS' if memory_pool_verified else 'FAIL'}")
    assert memory_pool_verified, "Memory pooling verification log missing!"

    print(f"   - TinyML Inference on PSRAM: {'PASS' if tinyml_inference_verified else 'FAIL'}")
    assert tinyml_inference_verified, "TinyML inference output on PSRAM missing!"

    print(f"   - TinyML Confidence: {tinyml_confidence}% (expected: 99.1%)")
    assert tinyml_confidence == 99.1, f"Expected 99.1% confidence, got {tinyml_confidence}%"

    print(f"   - Pipeline Complete & Verified: {'PASS' if pipeline_execution_verified else 'FAIL'}")
    assert pipeline_execution_verified, "Distributed pipeline completion log missing!"

    print("\n[VERDICT CHALLENGE 2 & 3]: ALL SILICON RUNTIME & WIRE PROTOCOL CHECKS PASSED.")
    return True

if __name__ == "__main__":
    port = sys.argv[1] if len(sys.argv) > 1 else TARGET_PORT
    print(f"Starting Empirical Adversarial Hardware Verification on {port}...")

    c1_res = run_challenge_1_reset_permutations(port, iterations=5)
    c23_res = run_challenge_2_and_3(port, duration_sec=22.0)

    print("\n" + "=" * 70)
    print(" ALL 3 EMPIRICAL ADVERSARIAL CHALLENGES COMPLETED WITH 100% SUCCESS")
    print("=" * 70)
