#!/usr/bin/env python3
"""
Enterprise Fullstack Multi-Workload Suite:
1. Vortex DAW & Modular Synthesizer (Step sequencer, ADSR synth, live visualizer, physical audio/keyboard integration)
2. Aether Collaborative Workspace & Document Canvas (Markdown editor, live preview, knowledge graph, full-text search)
3. Aegis Fleet Autonomous Mission Operations Console (Geospatial radar, vehicle telemetry, waypoint planner, command dispatch)
"""

import os
import sys
import time
import json
import math
import random
import threading
from urllib.parse import urlparse, parse_qs

# ---------------------------------------------------------------------------
# Global Suite State & In-Memory Stores
# ---------------------------------------------------------------------------
suite_lock = threading.Lock()

# Workload 1: Vortex DAW State
SYNTH_STATE = {
    "bpm": 128,
    "playing": False,
    "current_step": 0,
    "volume": 20,
    "waveform": "sawtooth",
    "attack_ms": 20,
    "decay_ms": 120,
    "sustain_pct": 65,
    "release_ms": 180,
    "filter_cutoff_hz": 2400,
    "resonance_q": 3.0,
    "preset": "CYBERPUNK",
    "tracks": {
        "kick":  [1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0],
        "snare": [0, 0, 0, 0,  1, 0, 0, 0,  0, 0, 0, 0,  1, 0, 0, 0],
        "hihat": [1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1,  1, 1, 1, 1],
        "synth": [0, 4, 0, 7,  0, 9, 0, 11, 0, 12, 0, 9, 0, 7, 4, 0],
    }
}

NOTE_FREQS = {
    0: 261.63, # C4
    1: 277.18, # C#4
    2: 293.66, # D4
    3: 311.13, # D#4
    4: 329.63, # E4
    5: 349.23, # F4
    6: 369.99, # F#4
    7: 392.00, # G4
    8: 415.30, # G#4
    9: 440.00, # A4
    10: 466.16, # A#4
    11: 493.88, # B4
    12: 523.25, # C5
    13: 554.37, # C#5
    14: 587.33, # D5
    15: 622.25, # D#5
    16: 659.25, # E5
}

# Workload 2: Aether Workspace Documents
WORKSPACE_DOCS = {
    "doc_consensus": {
        "id": "doc_consensus",
        "title": "Distributed Consensus Engine & Radix State Store",
        "category": "Architecture",
        "updated_at": time.time() - 3600,
        "content": """# Distributed Consensus Engine & Radix State Store

High-throughput distributed systems require predictable memory consumption, zero garbage collection pauses, and low-latency virtual message passing across compute nodes.

## Core Architectural Invariants

1. **Monotonic Arena Allocator**: Memory is chunked into monotonic arenas at boot, guaranteeing zero allocation latency under heavy load.
2. **Compact Radix-Tree Keyring**: Adaptive compact radix tree provides $O(k)$ key retrieval with sub-microsecond locking overhead.
3. **Hardware Bridge Interconnect**: Synchronous scanner loop polls physical peripherals at 50 Hz without inducing cache eviction.

### Data Flow Pipeline

```text
[ Client Query ] ---> [ Edge Gateway ] ---> [ Service Mesh Router ]
                                                      |
                                                      v
                                           [ In-Memory State Arena ]
```

### Protocol Validation Checklist
- [x] Raft election timeout configured: 150ms to 300ms
- [x] Vector clock monotonicity guaranteed
- [x] Cryptographic message integrity signed via SHA-256
- [ ] Cross-region cold backup sync enabled
"""
    },
    "doc_wire": {
        "id": "doc_wire",
        "title": "Quantum-Resistant Wire Protocol Specs v3",
        "category": "Specifications",
        "updated_at": time.time() - 7200,
        "content": """# Quantum-Resistant Wire Protocol Specs v3

This specification outlines the packet framing, entropy verification, and fault-tolerance parameters for secure interconnects.

## Wire Packet Framing

| Offset (Bytes) | Field Name | Type | Description |
| :--- | :--- | :--- | :--- |
| 0..3 | Magic Header | uint32 | `0x4B333201` protocol signature |
| 4..5 | Sequence ID | uint16 | Monotonically increasing counter |
| 6..7 | Payload Length | uint16 | Wire packet byte length |
| 8..11 | Checksum | uint32 | CRC32 frame integrity |

## Session Invariants
- Frame transmission timeout: **250 ms**
- Maximum frame retransmission attempts: **3**
- Key rotation epoch: **3,600 seconds**
"""
    },
    "doc_dsp": {
        "id": "doc_dsp",
        "title": "Real-Time Acoustic DSP & Waveform Engine",
        "category": "DSP",
        "updated_at": time.time() - 10800,
        "content": """# Real-Time Acoustic DSP & Waveform Engine

The acoustic sub-system integrates dynamic algorithmic wave generation with low-pass resonance filtering and discrete multi-voice envelope shaping.

## Synthesis Signal Flow

```text
[ Oscillators (Saw / Square / Sine) ] ---> [ 2-Pole Low-Pass Filter ] ---> [ ADSR Envelope ] ---> [ Output ]
```

### Voice Parameters
- Sample Rate: **16,000 Hz**
- Bit Depth: **16-bit Signed Linear PCM**
- DMA Buffer Queue: **8 descriptors x 128 samples**
- Latency Window: **< 8.0 ms**
"""
    },
    "doc_fleet": {
        "id": "doc_fleet",
        "title": "Autonomous Fleet Navigation & Sensor Mesh",
        "category": "Operations",
        "updated_at": time.time() - 14400,
        "content": """# Autonomous Fleet Navigation & Sensor Mesh

Operational parameters for unmanned aerial and ground assets coordinating via geospatial radar telemetry.

### Asset Roster & Tasking
- **ALPHA-1**: Rotary Scout quadcopter maintaining forward surveillance sweep.
- **BRAVO-2**: Cargo Hexacopter conducting waypoint-guided station holding.
- **TITAN-4**: All-Terrain UGV Rover monitoring surface grade and telemetry.
- **VALKYRIE-9**: High-altitude fixed-wing reconnaissance platform.

### Failsafe Protocols
1. Loss of signal $> 10\\text{ s}$ automatically triggers Return-to-Base (RTL).
2. Battery state of charge $< 20\\%$ initiates emergency waypoint redirect.
"""
    }
}

GRAPH_NODES = [
    {"id": "doc_consensus", "name": "Consensus Engine", "group": "arch", "val": 20},
    {"id": "doc_wire", "name": "Wire Protocol", "group": "spec", "val": 16},
    {"id": "doc_dsp", "name": "Acoustic DSP", "group": "audio", "val": 14},
    {"id": "doc_fleet", "name": "Fleet Radar", "group": "ops", "val": 15},
    {"id": "state_engine", "name": "State Store", "group": "core", "val": 12},
    {"id": "mesh_router", "name": "Mesh Router", "group": "core", "val": 12},
    {"id": "hardware_hsm", "name": "Hardware Terminal", "group": "hw", "val": 14},
]

GRAPH_LINKS = [
    {"source": "doc_consensus", "target": "state_engine"},
    {"source": "doc_consensus", "target": "mesh_router"},
    {"source": "doc_wire", "target": "mesh_router"},
    {"source": "doc_wire", "target": "hardware_hsm"},
    {"source": "doc_dsp", "target": "hardware_hsm"},
    {"source": "doc_fleet", "target": "state_engine"},
    {"source": "doc_fleet", "target": "mesh_router"},
]

# Workload 3: Aegis Fleet Operations
FLEET_UNITS = {
    "ALPHA-1": {
        "id": "ALPHA-1",
        "name": "Alpha Scout (Quadcopter)",
        "type": "UAV-Rotary",
        "status": "IN_FLIGHT",
        "lat": 37.7749,
        "lng": -122.4194,
        "x": 350 + 60,
        "y": 260 - 80,
        "alt_m": 124.5,
        "speed_mps": 16.8,
        "heading_deg": 48.0,
        "battery_pct": 84,
        "rssi_dbm": -68,
        "current_cmd": "WAYPOINT_PATROL",
        "trail": []
    },
    "BRAVO-2": {
        "id": "BRAVO-2",
        "name": "Bravo Lifter (Hexacopter)",
        "type": "UAV-Cargo",
        "status": "HOLD_ORBIT",
        "lat": 37.7825,
        "lng": -122.4080,
        "x": 350 + 130,
        "y": 260 + 50,
        "alt_m": 210.0,
        "speed_mps": 8.2,
        "heading_deg": 135.0,
        "battery_pct": 92,
        "rssi_dbm": -62,
        "current_cmd": "HOLD_STATION",
        "trail": []
    },
    "TITAN-4": {
        "id": "TITAN-4",
        "name": "Titan Rover (Unmanned Ground)",
        "type": "UGV-Ground",
        "status": "SURVEYING",
        "lat": 37.7710,
        "lng": -122.4240,
        "x": 350 - 90,
        "y": 260 + 110,
        "alt_m": 14.0,
        "speed_mps": 4.1,
        "heading_deg": 280.0,
        "battery_pct": 78,
        "rssi_dbm": -74,
        "current_cmd": "SURFACE_SURVEY",
        "trail": []
    },
    "VALKYRIE-9": {
        "id": "VALKYRIE-9",
        "name": "Valkyrie Recon (Fixed-Wing)",
        "type": "UAV-FixedWing",
        "status": "PATROL",
        "lat": 37.7880,
        "lng": -122.4300,
        "x": 350 - 150,
        "y": 260 - 120,
        "alt_m": 450.0,
        "speed_mps": 34.5,
        "heading_deg": 210.0,
        "battery_pct": 69,
        "rssi_dbm": -79,
        "current_cmd": "HIGH_ALT_SWEEP",
        "trail": []
    }
}

MISSION_EVENTS = [
    {"time": time.time() - 300, "unit": "SYSTEM", "level": "INFO", "msg": "Fleet telemetry online. Tactical C2 uplink established."},
    {"time": time.time() - 240, "unit": "ALPHA-1", "level": "COMMAND", "msg": "Airspace ingress confirmed. Altitude locked at 125m."},
    {"time": time.time() - 180, "unit": "BRAVO-2", "level": "INFO", "msg": "Cargo payload locked. Orbiting station Alpha."},
    {"time": time.time() - 120, "unit": "VALKYRIE-9", "level": "SUCCESS", "msg": "High-altitude recon sweep complete. Target vector clear."},
    {"time": time.time() - 45, "unit": "TITAN-4", "level": "WARN", "msg": "Terrain incline detected: 14 deg slope compensation active."}
]

# ---------------------------------------------------------------------------
# Background Suite Engine (Sequencer Clock & Vehicle Simulator)
# ---------------------------------------------------------------------------
_suite_thread_started = False

def background_suite_runner(send_serial_cmd=None):
    last_seq_tick = time.time()
    last_fleet_tick = time.time()
    last_screen_tick = time.time()

    while True:
        try:
            now = time.time()
            with suite_lock:
                # 1. Step Sequencer Audio Clock (if playing)
                if SYNTH_STATE["playing"]:
                    step_duration = 60.0 / (SYNTH_STATE["bpm"] * 4.0)
                    if now - last_seq_tick >= step_duration:
                        step = (SYNTH_STATE["current_step"] + 1) % 16
                        SYNTH_STATE["current_step"] = step
                        last_seq_tick = now

                        synth_note = SYNTH_STATE["tracks"]["synth"][step]
                        if synth_note > 0 and send_serial_cmd:
                            freq = int(NOTE_FREQS.get(synth_note, 440.0))
                            vol = SYNTH_STATE["volume"]
                            send_serial_cmd(f"K32_CMD:BEEP:{freq}:45:{vol}")

                # 2. Autonomous Fleet Position Updates (Physics Simulation)
                if now - last_fleet_tick >= 1.0:
                    last_fleet_tick = now
                    cx, cy = 350, 260
                    for uid, u in FLEET_UNITS.items():
                        rad = math.radians(u["heading_deg"])
                        # Move slightly
                        dx = math.cos(rad) * (u["speed_mps"] * 0.4)
                        dy = math.sin(rad) * (u["speed_mps"] * 0.4)
                        u["x"] += dx
                        u["y"] += dy

                        # Wrap around radar boundary
                        dist = math.hypot(u["x"] - cx, u["y"] - cy)
                        if dist > 220:
                            u["heading_deg"] = (u["heading_deg"] + 180 + random.uniform(-30, 30)) % 360

                        # Maintain breadcrumb trail
                        u["trail"].append([round(u["x"], 1), round(u["y"], 1)])
                        if len(u["trail"]) > 10:
                            u["trail"].pop(0)

                        if u["type"] != "UGV-Ground":
                            u["alt_m"] = round(u["alt_m"] + random.uniform(-0.6, 0.6), 1)

                # 3. Physical Hardware Display Sync (every 3.0s)
                if send_serial_cmd and (now - last_screen_tick >= 3.0):
                    last_screen_tick = now
                    if SYNTH_STATE["playing"]:
                        step_val = SYNTH_STATE["current_step"] + 1
                        bpm_val = SYNTH_STATE["bpm"]
                        send_serial_cmd(f"K32_CMD:DISPLAY:DAW: [PLAY] {bpm_val}BPM | S{step_val:02d}")
                    else:
                        alpha_alt = FLEET_UNITS["ALPHA-1"]["alt_m"]
                        send_serial_cmd(f"K32_CMD:DISPLAY:FLEET: 4 Active | A1: {alpha_alt:.0f}m")

        except Exception as e:
            print(f"[SUITE-ERR] Background engine error: {e}")

        time.sleep(0.04)

def start_suite_engine(send_serial_cmd=None):
    global _suite_thread_started
    if not _suite_thread_started:
        _suite_thread_started = True
        t = threading.Thread(target=background_suite_runner, args=(send_serial_cmd,), daemon=True)
        t.start()


# ---------------------------------------------------------------------------
# Fullstack SPA HTML Interface (Single Delivery Bundle)
# ---------------------------------------------------------------------------
SUITE_HTML = """<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Synapse Studio | High-Performance Distributed Workbenches</title>
    <link rel="preconnect" href="https://fonts.googleapis.com">
    <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
    <link href="https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@400;500;600;700&family=Plus+Jakarta+Sans:wght@400;500;600;700;800&display=swap" rel="stylesheet">
    <style>
        :root {
            --bg-canvas: #090b10;
            --bg-sub: #0f121a;
            --bg-card: #151923;
            --bg-elevated: #1c2130;
            --bg-hover: #22293b;
            
            --border-dim: rgba(255, 255, 255, 0.08);
            --border-subtle: rgba(255, 255, 255, 0.14);
            --border-focus: #6366f1;

            --text-main: #f1f5f9;
            --text-muted: #94a3b8;
            --text-faint: #475569;

            --accent-green: #10b981;
            --accent-purple: #8b5cf6;
            --accent-cyan: #06b6d4;
            --accent-red: #f43f5e;
            --accent-amber: #f59e0b;

            --font-sans: 'Plus Jakarta Sans', -apple-system, BlinkMacSystemFont, sans-serif;
            --font-mono: 'JetBrains Mono', monospace;
        }

        * { box-sizing: border-box; margin: 0; padding: 0; }
        body {
            background-color: var(--bg-canvas);
            color: var(--text-main);
            font-family: var(--font-sans);
            font-size: 13px;
            line-height: 1.5;
            -webkit-font-smoothing: antialiased;
            overflow-x: hidden;
        }

        /* Top Bar Suite Header */
        .suite-header {
            height: 52px;
            border-bottom: 1px solid var(--border-dim);
            background: var(--bg-sub);
            display: flex;
            align-items: center;
            justify-content: space-between;
            padding: 0 20px;
            position: sticky;
            top: 0;
            z-index: 200;
        }

        .brand-cluster {
            display: flex;
            align-items: center;
            gap: 10px;
            font-weight: 700;
            font-size: 15px;
            letter-spacing: -0.02em;
        }

        .brand-badge {
            width: 26px;
            height: 26px;
            background: linear-gradient(135deg, #6366f1, #06b6d4);
            border-radius: 6px;
            display: flex;
            align-items: center;
            justify-content: center;
            font-size: 13px;
            font-weight: 800;
            color: #fff;
        }

        .suite-tabs {
            display: flex;
            gap: 6px;
            background: var(--bg-canvas);
            padding: 3px;
            border-radius: 8px;
            border: 1px solid var(--border-dim);
        }

        .tab-btn {
            background: transparent;
            border: none;
            color: var(--text-muted);
            padding: 6px 14px;
            font-size: 12px;
            font-weight: 600;
            border-radius: 6px;
            cursor: pointer;
            transition: all 0.15s ease;
            display: flex;
            align-items: center;
            gap: 8px;
        }

        .tab-btn:hover { color: var(--text-main); }
        .tab-btn.active {
            background: var(--bg-card);
            color: #fff;
            box-shadow: 0 2px 6px rgba(0,0,0,0.3);
        }

        .tab-indicator {
            width: 6px;
            height: 6px;
            border-radius: 50%;
            background: var(--text-faint);
        }
        .tab-btn.active .tab-indicator { background: var(--accent-cyan); }

        .telemetry-tray {
            display: flex;
            align-items: center;
            gap: 16px;
            font-family: var(--font-mono);
            font-size: 11px;
            color: var(--text-muted);
        }

        .dot-green {
            width: 7px;
            height: 7px;
            border-radius: 50%;
            background: var(--accent-green);
            box-shadow: 0 0 8px var(--accent-green);
        }

        /* Workload Container */
        .workload-view {
            display: none;
            min-height: calc(100vh - 52px);
        }

        .workload-view.active { display: block; }

        /* ============================================================== */
        /* WORKLOAD 1: VORTEX DAW & MODULAR SYNTH STYLING                 */
        /* ============================================================== */
        .daw-grid {
            display: grid;
            grid-template-columns: 340px 1fr;
            min-height: calc(100vh - 52px);
            background: var(--border-dim);
            gap: 1px;
        }

        .daw-sidebar {
            background: var(--bg-sub);
            padding: 20px;
            display: flex;
            flex-direction: column;
            gap: 18px;
            border-right: 1px solid var(--border-dim);
        }

        .daw-main {
            background: var(--bg-canvas);
            padding: 24px;
            display: flex;
            flex-direction: column;
            gap: 20px;
        }

        .transport-bar {
            display: flex;
            align-items: center;
            justify-content: space-between;
            background: var(--bg-card);
            padding: 12px 18px;
            border-radius: 8px;
            border: 1px solid var(--border-dim);
        }

        .btn-transport {
            background: var(--bg-elevated);
            border: 1px solid var(--border-subtle);
            color: var(--text-main);
            padding: 8px 16px;
            font-size: 12px;
            font-weight: 700;
            border-radius: 6px;
            cursor: pointer;
            transition: all 0.15s;
            font-family: var(--font-mono);
        }

        .btn-transport:hover { background: var(--bg-hover); }
        .btn-transport.play.active {
            background: var(--accent-green);
            color: #000;
            border-color: var(--accent-green);
            box-shadow: 0 0 12px rgba(16, 185, 129, 0.4);
        }

        .preset-pill {
            background: var(--bg-elevated);
            border: 1px solid var(--border-dim);
            color: var(--text-muted);
            padding: 4px 10px;
            font-size: 11px;
            font-family: var(--font-mono);
            border-radius: 4px;
            cursor: pointer;
            transition: all 0.15s;
        }
        .preset-pill:hover, .preset-pill.active {
            background: var(--accent-purple);
            color: #fff;
            border-color: var(--accent-purple);
        }

        .sequencer-grid {
            background: var(--bg-card);
            border: 1px solid var(--border-dim);
            border-radius: 8px;
            padding: 16px;
            display: flex;
            flex-direction: column;
            gap: 12px;
        }

        .track-row {
            display: grid;
            grid-template-columns: 100px repeat(16, 1fr);
            gap: 6px;
            align-items: center;
        }

        .track-label {
            font-family: var(--font-mono);
            font-size: 11px;
            font-weight: 700;
            color: var(--text-muted);
            text-transform: uppercase;
        }

        .seq-step {
            height: 38px;
            background: var(--bg-elevated);
            border: 1px solid var(--border-dim);
            border-radius: 4px;
            cursor: pointer;
            transition: all 0.1s;
            position: relative;
        }

        .seq-step:hover { background: var(--bg-hover); }
        .seq-step.active {
            background: var(--accent-cyan);
            border-color: var(--accent-cyan);
            box-shadow: 0 0 10px rgba(6, 182, 212, 0.4);
        }
        .seq-step.active.snare { background: var(--accent-purple); border-color: var(--accent-purple); box-shadow: 0 0 10px rgba(139, 92, 246, 0.4); }
        .seq-step.active.hihat { background: var(--accent-amber); border-color: var(--accent-amber); box-shadow: 0 0 10px rgba(245, 158, 11, 0.4); }
        .seq-step.active.synth { background: var(--accent-green); border-color: var(--accent-green); box-shadow: 0 0 10px rgba(16, 185, 129, 0.4); }

        .seq-step.cursor::after {
            content: '';
            position: absolute;
            inset: 0;
            border: 2px solid #fff;
            border-radius: 3px;
            pointer-events: none;
        }

        /* 2-Octave Piano Keyboard */
        .piano-keyboard {
            display: flex;
            height: 120px;
            position: relative;
            background: var(--bg-card);
            border: 1px solid var(--border-dim);
            border-radius: 8px;
            padding: 8px;
            user-select: none;
        }

        .key-white {
            flex: 1;
            background: #e2e8f0;
            border: 1px solid #cbd5e1;
            border-radius: 0 0 4px 4px;
            cursor: pointer;
            position: relative;
            z-index: 1;
            display: flex;
            align-items: flex-end;
            justify-content: center;
            padding-bottom: 8px;
            font-family: var(--font-mono);
            font-size: 10px;
            color: #475569;
            font-weight: 700;
            transition: background 0.1s;
        }

        .key-white:active, .key-white.pressed { background: #38bdf8 !important; color: #fff; }

        .key-black {
            position: absolute;
            width: 4.8%;
            height: 65%;
            background: #0f172a;
            border-radius: 0 0 3px 3px;
            z-index: 2;
            cursor: pointer;
            transition: background 0.1s;
            display: flex;
            align-items: flex-end;
            justify-content: center;
            padding-bottom: 4px;
            font-family: var(--font-mono);
            font-size: 8px;
            color: #94a3b8;
        }

        .key-black:active, .key-black.pressed { background: var(--accent-purple) !important; color: #fff; }

        /* ============================================================== */
        /* WORKLOAD 2: AETHER WORKSPACE STYLING                          */
        /* ============================================================== */
        .workspace-layout {
            display: grid;
            grid-template-columns: 260px 1fr 340px;
            min-height: calc(100vh - 52px);
            background: var(--border-dim);
            gap: 1px;
        }

        .ws-sidebar {
            background: var(--bg-sub);
            padding: 16px;
            border-right: 1px solid var(--border-dim);
            display: flex;
            flex-direction: column;
            gap: 8px;
        }

        .doc-item {
            padding: 10px 12px;
            border-radius: 6px;
            cursor: pointer;
            display: flex;
            align-items: center;
            gap: 10px;
            color: var(--text-muted);
            transition: all 0.15s;
            font-size: 12px;
            border: 1px solid transparent;
        }

        .doc-item:hover { background: var(--bg-elevated); color: var(--text-main); }
        .doc-item.active {
            background: var(--bg-card);
            color: #fff;
            border-color: var(--border-subtle);
            font-weight: 600;
        }

        .ws-editor-pane {
            background: var(--bg-canvas);
            display: flex;
            flex-direction: column;
        }

        .editor-header {
            padding: 12px 20px;
            border-bottom: 1px solid var(--border-dim);
            display: flex;
            justify-content: space-between;
            align-items: center;
            background: var(--bg-sub);
        }

        .editor-toolbar {
            display: flex;
            align-items: center;
            gap: 6px;
            padding: 8px 16px;
            background: var(--bg-card);
            border-bottom: 1px solid var(--border-dim);
            overflow-x: auto;
        }

        .tool-btn {
            background: transparent;
            border: 1px solid var(--border-dim);
            color: var(--text-muted);
            padding: 4px 8px;
            border-radius: 4px;
            cursor: pointer;
            font-size: 11px;
            font-family: var(--font-mono);
            font-weight: 600;
            transition: all 0.15s;
        }
        .tool-btn:hover { background: var(--bg-hover); color: var(--text-main); }

        .editor-container {
            display: grid;
            grid-template-columns: 1fr 1fr;
            flex: 1;
            height: calc(100vh - 150px);
        }

        .editor-textarea {
            width: 100%;
            height: 100%;
            background: var(--bg-canvas);
            border: none;
            border-right: 1px solid var(--border-dim);
            color: var(--text-main);
            font-family: var(--font-mono);
            font-size: 12px;
            line-height: 1.6;
            padding: 20px;
            resize: none;
            outline: none;
        }

        .preview-pane {
            padding: 20px;
            overflow-y: auto;
            color: var(--text-main);
            font-size: 13px;
            line-height: 1.6;
            background: var(--bg-canvas);
        }

        .preview-pane h1 { font-size: 20px; font-weight: 800; margin-bottom: 14px; border-bottom: 1px solid var(--border-dim); padding-bottom: 8px; color: #fff; }
        .preview-pane h2 { font-size: 16px; font-weight: 700; margin: 18px 0 10px 0; color: #fff; }
        .preview-pane h3 { font-size: 14px; font-weight: 600; margin: 14px 0 8px 0; color: var(--accent-cyan); }
        .preview-pane p { margin-bottom: 12px; color: var(--text-muted); }
        .preview-pane ul { margin-left: 20px; margin-bottom: 12px; color: var(--text-muted); }
        .preview-pane li { margin-bottom: 4px; }
        .preview-pane table { width: 100%; border-collapse: collapse; margin-bottom: 14px; }
        .preview-pane th, .preview-pane td { border: 1px solid var(--border-dim); padding: 6px 10px; font-size: 12px; }
        .preview-pane th { background: var(--bg-card); font-weight: 700; color: #fff; }
        .preview-pane pre { background: var(--bg-card); padding: 12px; border-radius: 6px; font-family: var(--font-mono); font-size: 11px; margin-bottom: 14px; overflow-x: auto; border: 1px solid var(--border-dim); color: #38bdf8; }

        .ws-graph-pane {
            background: var(--bg-sub);
            padding: 16px;
            border-left: 1px solid var(--border-dim);
            display: flex;
            flex-direction: column;
            gap: 12px;
        }

        /* ============================================================== */
        /* WORKLOAD 3: AEGIS FLEET OPERATIONS STYLING                     */
        /* ============================================================== */
        .fleet-layout {
            display: grid;
            grid-template-columns: 280px 1fr 320px;
            min-height: calc(100vh - 52px);
            background: var(--border-dim);
            gap: 1px;
        }

        .fleet-sidebar {
            background: var(--bg-sub);
            padding: 16px;
            border-right: 1px solid var(--border-dim);
            display: flex;
            flex-direction: column;
            gap: 10px;
        }

        .unit-card {
            background: var(--bg-card);
            border: 1px solid var(--border-dim);
            border-radius: 8px;
            padding: 12px;
            cursor: pointer;
            transition: all 0.15s;
        }

        .unit-card:hover { border-color: var(--border-subtle); }
        .unit-card.active {
            border-color: var(--accent-cyan);
            background: var(--bg-elevated);
        }

        .unit-top {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 8px;
        }

        .unit-name { font-weight: 700; font-size: 13px; }
        .unit-status {
            font-family: var(--font-mono);
            font-size: 10px;
            padding: 2px 6px;
            border-radius: 4px;
            background: rgba(16, 185, 129, 0.15);
            color: var(--accent-green);
        }

        .unit-telemetry {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 6px;
            font-family: var(--font-mono);
            font-size: 11px;
            color: var(--text-muted);
        }

        .radar-canvas-container {
            background: #04060a;
            position: relative;
            display: flex;
            align-items: center;
            justify-content: center;
            overflow: hidden;
        }

        .radar-hud-overlay {
            position: absolute;
            top: 16px;
            left: 16px;
            font-family: var(--font-mono);
            font-size: 11px;
            color: var(--text-muted);
            pointer-events: none;
        }

        .fleet-controls-pane {
            background: var(--bg-sub);
            padding: 16px;
            border-left: 1px solid var(--border-dim);
            display: flex;
            flex-direction: column;
            gap: 12px;
        }

        .btn-cmd {
            background: var(--bg-card);
            border: 1px solid var(--border-dim);
            color: var(--text-main);
            padding: 10px 14px;
            border-radius: 6px;
            font-family: var(--font-mono);
            font-size: 11px;
            font-weight: 700;
            cursor: pointer;
            transition: all 0.15s;
            text-align: left;
            display: flex;
            justify-content: space-between;
            align-items: center;
        }

        .btn-cmd:hover { background: var(--bg-hover); }
        .btn-cmd.primary {
            background: rgba(6, 182, 212, 0.15);
            border-color: var(--accent-cyan);
            color: #38bdf8;
        }
        .btn-cmd.alert {
            background: rgba(244, 63, 94, 0.15);
            border-color: var(--accent-red);
            color: var(--accent-red);
        }
    </style>
</head>
<body>

    <!-- Top Navigation Bar -->
    <header class="suite-header">
        <div class="brand-cluster">
            <div class="brand-badge">⚡</div>
            <span>Synapse Studio</span>
        </div>

        <nav class="suite-tabs">
            <button class="tab-btn active" onclick="switchWorkload('daw')">
                <div class="tab-indicator"></div>
                <span>Vortex Synthesizer & DAW</span>
            </button>
            <button class="tab-btn" onclick="switchWorkload('workspace')">
                <div class="tab-indicator"></div>
                <span>Aether Document Canvas</span>
            </button>
            <button class="tab-btn" onclick="switchWorkload('fleet')">
                <div class="tab-indicator"></div>
                <span>Aegis Fleet Operations</span>
            </button>
        </nav>

        <div class="telemetry-tray">
            <div style="display: flex; align-items: center; gap: 8px;">
                <div class="dot-green"></div>
                <span>LATENCY: 2.1 ms</span>
            </div>
            <div id="hwKeyPill" style="display: flex; align-items: center; gap: 6px; padding: 3px 8px; border-radius: 4px; background: rgba(255, 255, 255, 0.05); border: 1px solid var(--border-dim); transition: all 0.2s;">
                <span style="font-size: 10px; color: var(--text-muted);">PHYSICAL KEY:</span>
                <span id="hwKeyVal" style="color: var(--accent-cyan); font-weight: 700;">IDLE</span>
            </div>
        </div>
    </header>

    <!-- ================================================================ -->
    <!-- WORKLOAD 1: VORTEX DAW & MODULAR SYNTHESIZER                     -->
    <!-- ================================================================ -->
    <div class="workload-view active" id="view-daw">
        <div class="daw-grid">
            
            <!-- Left Sidebar: Controls & Visualizers -->
            <aside class="daw-sidebar">
                <h3 style="font-size: 13px; font-weight: 800; text-transform: uppercase; color: var(--text-muted); font-family: var(--font-mono);">
                    Modular Synth Engine
                </h3>

                <!-- Presets -->
                <div>
                    <label style="display: block; font-size: 11px; color: var(--text-muted); margin-bottom: 6px; font-family: var(--font-mono);">PATTERN PRESETS</label>
                    <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 6px;">
                        <button class="preset-pill active" onclick="loadPreset('CYBERPUNK')">CYBERPUNK</button>
                        <button class="preset-pill" onclick="loadPreset('MINIMAL TECHNO')">MINIMAL</button>
                        <button class="preset-pill" onclick="loadPreset('LO-FI CHILL')">LO-FI</button>
                        <button class="preset-pill" onclick="loadPreset('ACID ELECTRO')">ACID</button>
                    </div>
                </div>

                <!-- Waveform Select -->
                <div>
                    <label style="display: block; font-size: 11px; color: var(--text-muted); margin-bottom: 6px; font-family: var(--font-mono);">OSCILLATOR WAVEFORM</label>
                    <select id="synthWaveform" style="width: 100%; background: var(--bg-card); border: 1px solid var(--border-dim); border-radius: 6px; color: #fff; padding: 6px 10px; font-family: var(--font-mono);" onchange="updateSynthParams()">
                        <option value="sawtooth">Sawtooth (Aggressive)</option>
                        <option value="square">Square (Pulse)</option>
                        <option value="triangle">Triangle (Mellow)</option>
                        <option value="sine">Sine (Pure Sub)</option>
                    </select>
                </div>

                <!-- Volume & Output -->
                <div>
                    <div style="display: flex; justify-content: space-between; font-size: 11px; margin-bottom: 6px; font-family: var(--font-mono);">
                        <span>OUTPUT VOLUME</span>
                        <span id="synthVolVal" style="color: var(--accent-cyan);">20%</span>
                    </div>
                    <input type="range" id="synthVol" min="0" max="100" value="20" style="width: 100%; accent-color: var(--accent-purple);" oninput="updateSynthVolume(this.value)">
                </div>

                <!-- Filter Cutoff -->
                <div>
                    <div style="display: flex; justify-content: space-between; font-size: 11px; margin-bottom: 6px; font-family: var(--font-mono);">
                        <span>LOW-PASS CUTOFF</span>
                        <span id="cutoffVal" style="color: var(--accent-cyan);">2400 Hz</span>
                    </div>
                    <input type="range" id="synthCutoff" min="200" max="8000" value="2400" style="width: 100%; accent-color: var(--accent-cyan);" oninput="document.getElementById('cutoffVal').textContent = this.value + ' Hz'; updateSynthParams();">
                </div>

                <!-- Live Oscilloscope & Spectrum Visualizer -->
                <div>
                    <div style="display: flex; justify-content: space-between; font-size: 11px; color: var(--text-muted); margin-bottom: 6px; font-family: var(--font-mono);">
                        <span>REAL-TIME SPECTRUM</span>
                        <span style="color: var(--accent-green);">16-BAND FFT</span>
                    </div>
                    <canvas id="scopeCanvas" width="300" height="110" style="background: var(--bg-card); border: 1px solid var(--border-dim); border-radius: 6px; width: 100%;"></canvas>
                </div>

                <!-- Hardware Hotkey Pill -->
                <div style="background: var(--bg-card); border: 1px solid var(--border-dim); border-radius: 6px; padding: 10px; font-family: var(--font-mono); font-size: 11px;">
                    <div style="color: var(--accent-green); font-weight: 700; margin-bottom: 4px;">HARDWARE SYNTH HOTKEYS</div>
                    <div style="color: var(--text-muted); font-size: 10px; line-height: 1.4;">
                        Physical keys <strong>Q..U</strong> trigger synth tones directly on hardware speaker & browser.<br>
                        Press <strong>Space</strong> to Play/Stop. Keys <strong>1..4</strong> switch presets.
                    </div>
                </div>
            </aside>

            <!-- Main Panel: Sequencer & Piano Keyboard -->
            <main class="daw-main">
                <div class="transport-bar">
                    <div style="display: flex; align-items: center; gap: 12px;">
                        <button class="btn-transport play" id="btnDawPlay" onclick="togglePlayback()">▶ PLAY SEQUENCE</button>
                        <button class="btn-transport" onclick="clearPattern()">CLEAR PATTERN</button>
                    </div>

                    <div style="display: flex; align-items: center; gap: 16px; font-family: var(--font-mono); font-size: 12px;">
                        <div>
                            <span>TEMPO: </span>
                            <input type="number" id="dawBpm" min="60" max="200" value="128" style="width: 50px; background: var(--bg-sub); border: 1px solid var(--border-dim); border-radius: 4px; color: #fff; text-align: center; font-family: var(--font-mono); padding: 3px;" onchange="updateBpm(this.value)">
                            <span> BPM</span>
                        </div>
                        <div>STEP: <strong style="color: var(--accent-cyan);" id="stepCounter">01 / 16</strong></div>
                    </div>
                </div>

                <!-- 16-Step Pattern Sequencer -->
                <div class="sequencer-grid">
                    <div class="track-row">
                        <div class="track-label">Kick (808)</div>
                        <div id="row-kick" style="display: contents;"></div>
                    </div>
                    <div class="track-row">
                        <div class="track-label">Snare (909)</div>
                        <div id="row-snare" style="display: contents;"></div>
                    </div>
                    <div class="track-row">
                        <div class="track-label">Hi-Hat</div>
                        <div id="row-hihat" style="display: contents;"></div>
                    </div>
                    <div class="track-row">
                        <div class="track-label">Synth Lead</div>
                        <div id="row-synth" style="display: contents;"></div>
                    </div>
                </div>

                <!-- Interactive 2-Octave Piano Keyboard -->
                <div style="display: flex; justify-content: space-between; align-items: center; font-family: var(--font-mono); font-size: 11px; color: var(--text-muted); margin-top: 6px;">
                    <span>INTERACTIVE SYNTHESIZER KEYBOARD</span>
                    <span style="color: var(--accent-cyan);">OCTAVES 4 - 5</span>
                </div>
                <div class="piano-keyboard" id="pianoKeyboard">
                    <!-- Virtual Piano Keys generated dynamically -->
                </div>

            </main>

        </div>
    </div>

    <!-- ================================================================ -->
    <!-- WORKLOAD 2: AETHER COLLABORATIVE WORKSPACE                       -->
    <!-- ================================================================ -->
    <div class="workload-view" id="view-workspace">
        <div class="workspace-layout">
            
            <!-- Document List Navigation -->
            <aside class="ws-sidebar">
                <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px;">
                    <span style="font-weight: 700; font-size: 11px; text-transform: uppercase; color: var(--text-muted); font-family: var(--font-mono);">DOCUMENTS</span>
                    <button style="background: none; border: 1px solid var(--border-dim); color: #fff; border-radius: 4px; padding: 2px 6px; font-size: 11px; cursor: pointer;" onclick="createNewDoc()">+ NEW</button>
                </div>
                <div id="docsList"></div>
            </aside>

            <!-- Markdown Split-Pane Editor -->
            <main class="ws-editor-pane">
                <div class="editor-header">
                    <input type="text" id="docTitleInput" style="background: transparent; border: none; color: #fff; font-size: 15px; font-weight: 800; font-family: var(--font-sans); outline: none; width: 60%;" oninput="saveCurrentDoc()">
                    <div style="font-family: var(--font-mono); font-size: 11px; color: var(--text-muted);" id="docMetrics">
                        <span>Words: 342</span> | <span>Saved</span>
                    </div>
                </div>

                <!-- Formatting Toolbar -->
                <div class="editor-toolbar">
                    <button class="tool-btn" onclick="insertSyntax('# ')">H1</button>
                    <button class="tool-btn" onclick="insertSyntax('## ')">H2</button>
                    <button class="tool-btn" onclick="insertSyntax('**', '**')">Bold</button>
                    <button class="tool-btn" onclick="insertSyntax('*', '*')">Italic</button>
                    <button class="tool-btn" onclick="insertSyntax('```\\n', '\\n```')">Code</button>
                    <button class="tool-btn" onclick="insertSyntax('- ')">List</button>
                    <button class="tool-btn" onclick="insertSyntax('- [ ] ')">Task</button>
                    <button class="tool-btn" onclick="insertSyntax('> ')">Quote</button>
                    <button class="tool-btn" onclick="insertSyntax('| Col 1 | Col 2 |\\n|---|---|\\n| Val 1 | Val 2 |\\n')">Table</button>
                    <button class="tool-btn" style="color: var(--accent-cyan); margin-left: auto;" onclick="exportMarkdown()">Download .md</button>
                </div>

                <div class="editor-container">
                    <textarea class="editor-textarea" id="docTextarea" oninput="handleDocInput()" placeholder="Type markdown or type directly on physical keyboard..."></textarea>
                    <div class="preview-pane" id="docPreview"></div>
                </div>
            </main>

            <!-- Knowledge Graph Visualizer -->
            <aside class="ws-graph-pane">
                <h4 style="font-size: 12px; font-weight: 700; text-transform: uppercase; color: var(--text-muted); font-family: var(--font-mono);">
                    Knowledge Graph
                </h4>
                <canvas id="graphCanvas" width="308" height="360" style="background: var(--bg-card); border: 1px solid var(--border-dim); border-radius: 6px; width: 100%; height: 360px;"></canvas>
                <div style="margin-top: 8px; font-family: var(--font-mono); font-size: 11px; color: var(--text-muted); line-height: 1.4;">
                    Interactive associative link graph. Drag nodes to inspect relationships and dynamic system topology.
                </div>
            </aside>

        </div>
    </div>

    <!-- ================================================================ -->
    <!-- WORKLOAD 3: AEGIS FLEET OPERATIONS                              -->
    <!-- ================================================================ -->
    <div class="workload-view" id="view-fleet">
        <div class="fleet-layout">
            
            <!-- Fleet Unit List -->
            <aside class="fleet-sidebar">
                <div style="font-weight: 700; font-size: 11px; text-transform: uppercase; color: var(--text-muted); font-family: var(--font-mono); margin-bottom: 8px;">
                    Autonomous Vehicles (4)
                </div>
                <div id="fleetUnitsContainer"></div>
            </aside>

            <!-- Geospatial Radar Map -->
            <main class="radar-canvas-container" id="radarContainer">
                <div class="radar-hud-overlay">
                    <div>TACTICAL RADAR VIEW [37.7749 N, 122.4194 W]</div>
                    <div>SECTOR: SAN FRANCISCO BAY</div>
                    <div style="color: var(--accent-green); margin-top: 4px;">UPLINK: ENCRYPTED DGPS (FIX LOCKED)</div>
                </div>
                <canvas id="radarCanvas" width="700" height="520" style="width: 100%; height: 100%; cursor: crosshair;" onclick="handleRadarClick(event)"></canvas>
            </main>

            <!-- Mission Command Dispatcher & Avionics -->
            <aside class="fleet-controls-pane">
                <h4 style="font-size: 12px; font-weight: 700; text-transform: uppercase; color: var(--text-muted); font-family: var(--font-mono);">
                    Mission Command Dispatch
                </h4>

                <button class="btn-cmd primary" onclick="sendFleetCmd('ARM_LAUNCH')">
                    <span>ARM / INITIATE PATROL</span>
                    <span style="opacity: 0.6; font-size: 9px;">[A]</span>
                </button>
                <button class="btn-cmd" onclick="sendFleetCmd('HOLD_ORBIT')">
                    <span>HOLD ORBIT / STATION</span>
                    <span style="opacity: 0.6; font-size: 9px;">[H]</span>
                </button>
                <button class="btn-cmd" onclick="sendFleetCmd('RTL')">
                    <span>RETURN TO BASE (RTL)</span>
                    <span style="opacity: 0.6; font-size: 9px;">[R]</span>
                </button>
                <button class="btn-cmd alert" onclick="sendFleetCmd('EMERGENCY_HALT')">
                    <span>EMERGENCY HALT</span>
                    <span style="opacity: 0.6; font-size: 9px;">[E]</span>
                </button>

                <div style="border-top: 1px solid var(--border-dim); padding-top: 14px; margin-top: 6px;">
                    <div style="font-weight: 700; font-size: 11px; text-transform: uppercase; color: var(--text-muted); font-family: var(--font-mono); margin-bottom: 8px;">
                        Mission Telemetry Log
                    </div>
                    <div id="missionLog" style="font-family: var(--font-mono); font-size: 11px; line-height: 1.6; max-height: 220px; overflow-y: auto;"></div>
                </div>
            </aside>

        </div>
    </div>

    <!-- Scripting for All 3 Workloads -->
    <script>
        let currentWorkload = 'daw';
        let audioCtx = null;
        let isPlaying = false;
        let synthTracks = {};
        let currentDocId = 'doc_consensus';
        let currentSelectedUnit = 'ALPHA-1';
        let lastSeenKeySeq = 0;
        let targetWaypoint = null;

        function initAudio() {
            if (!audioCtx) {
                audioCtx = new (window.AudioContext || window.webkitAudioContext)();
            }
        }

        // Switch Workloads
        function switchWorkload(id) {
            currentWorkload = id;
            document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
            document.querySelectorAll('.workload-view').forEach(v => v.classList.remove('active'));

            const activeBtn = Array.from(document.querySelectorAll('.tab-btn')).find(b => b.textContent.toLowerCase().includes(id));
            if (activeBtn) activeBtn.classList.add('active');

            const view = document.getElementById(`view-${id}`);
            if (view) view.classList.add('active');

            if (id === 'daw') initDaw();
            if (id === 'workspace') initWorkspace();
            if (id === 'fleet') initFleet();
        }

        // ===================================================================
        // HARDWARE KEYBOARD EVENT POLLER & DISPATCHER
        // ===================================================================
        function pollHardwareKeyboard() {
            fetch('/api/v1/hardware/keyboard')
                .then(r => r.json())
                .then(data => {
                    if (data.seq > lastSeenKeySeq) {
                        const newEvents = (data.events || []).filter(e => e.seq > lastSeenKeySeq);
                        lastSeenKeySeq = data.seq;
                        newEvents.forEach(ev => {
                            handleHardwareKeystroke(ev.key);
                        });
                    }
                })
                .catch(() => {});
        }

        function handleHardwareKeystroke(key) {
            // Flash Header Pill
            const pill = document.getElementById('hwKeyPill');
            const val = document.getElementById('hwKeyVal');
            if (val && pill) {
                val.textContent = (key === ' ') ? '[SPACE]' : (key === '\\b') ? '[DEL]' : (key === '\\n') ? '[ENTER]' : `'${key}'`;
                pill.style.borderColor = 'var(--accent-green)';
                pill.style.background = 'rgba(16, 185, 129, 0.2)';
                setTimeout(() => {
                    pill.style.borderColor = 'var(--border-dim)';
                    pill.style.background = 'rgba(255, 255, 255, 0.05)';
                }, 350);
            }

            if (currentWorkload === 'daw') {
                handleDawHardwareKey(key);
            } else if (currentWorkload === 'workspace') {
                handleWorkspaceHardwareKey(key);
            } else if (currentWorkload === 'fleet') {
                handleFleetHardwareKey(key);
            }
        }

        // ===================================================================
        // WORKLOAD 1: VORTEX DAW LOGIC
        // ===================================================================
        const PIANO_KEY_MAP = {
            'q': { note: 'C4', f: 261.63 },
            '2': { note: 'C#4', f: 277.18 },
            'w': { note: 'D4', f: 293.66 },
            '3': { note: 'D#4', f: 311.13 },
            'e': { note: 'E4', f: 329.63 },
            'r': { note: 'F4', f: 349.23 },
            '5': { note: 'F#4', f: 369.99 },
            't': { note: 'G4', f: 392.00 },
            '6': { note: 'G#4', f: 415.30 },
            'y': { note: 'A4', f: 440.00 },
            '7': { note: 'A#4', f: 466.16 },
            'u': { note: 'B4', f: 493.88 },
            'i': { note: 'C5', f: 523.25 },
            '9': { note: 'C#5', f: 554.37 },
            'o': { note: 'D5', f: 587.33 },
            '0': { note: 'D#5', f: 622.25 },
            'p': { note: 'E5', f: 659.25 }
        };

        function handleDawHardwareKey(key) {
            initAudio();
            const lower = key.toLowerCase();
            if (PIANO_KEY_MAP[lower]) {
                const kinfo = PIANO_KEY_MAP[lower];
                playWebTone(kinfo.f, 0.2);
                animateKey(kinfo.note);
            } else if (key === ' ') {
                togglePlayback();
            } else if (key === '1') {
                loadPreset('CYBERPUNK');
            } else if (key === '2') {
                loadPreset('MINIMAL TECHNO');
            } else if (key === '3') {
                loadPreset('LO-FI CHILL');
            } else if (key === '4') {
                loadPreset('ACID ELECTRO');
            }
        }

        function animateKey(noteName) {
            const el = document.querySelector(`[data-note="${noteName}"]`);
            if (el) {
                el.classList.add('pressed');
                setTimeout(() => el.classList.remove('pressed'), 200);
            }
        }

        function initDaw() {
            fetch('/api/v1/synth/state')
                .then(r => r.json())
                .then(st => {
                    synthTracks = st.tracks;
                    document.getElementById('dawBpm').value = st.bpm;
                    document.getElementById('synthVol').value = st.volume;
                    document.getElementById('synthVolVal').textContent = st.volume + '%';
                    renderSequencer();
                });
            renderPiano();
            startVisualizer();
        }

        function renderSequencer() {
            ['kick', 'snare', 'hihat', 'synth'].forEach(trk => {
                const row = document.getElementById(`row-${trk}`);
                if (!row) return;
                const pattern = synthTracks[trk] || Array(16).fill(0);
                row.innerHTML = pattern.map((val, idx) => `
                    <div class="seq-step ${trk} ${val > 0 ? 'active' : ''}" id="step-${trk}-${idx}" onclick="toggleStep('${trk}', ${idx})"></div>
                `).join('');
            });
        }

        function toggleStep(trk, idx) {
            initAudio();
            const cur = synthTracks[trk][idx];
            synthTracks[trk][idx] = (cur === 0) ? (trk === 'synth' ? 7 : 1) : 0;
            renderSequencer();
            fetch('/api/v1/synth/pattern', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ tracks: synthTracks })
            });
        }

        function togglePlayback() {
            initAudio();
            isPlaying = !isPlaying;
            const btn = document.getElementById('btnDawPlay');
            btn.textContent = isPlaying ? '⏹ STOP SEQUENCE' : '▶ PLAY SEQUENCE';
            btn.classList.toggle('active', isPlaying);

            fetch('/api/v1/synth/transport', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ playing: isPlaying })
            });
        }

        function clearPattern() {
            ['kick', 'snare', 'hihat', 'synth'].forEach(trk => {
                synthTracks[trk] = Array(16).fill(0);
            });
            renderSequencer();
            fetch('/api/v1/synth/pattern', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ tracks: synthTracks })
            });
        }

        function loadPreset(name) {
            document.querySelectorAll('.preset-pill').forEach(b => {
                b.classList.toggle('active', b.textContent.includes(name.split(' ')[0]));
            });
            fetch('/api/v1/synth/preset', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ preset: name })
            })
            .then(r => r.json())
            .then(data => {
                if (data.state) {
                    synthTracks = data.state.tracks;
                    document.getElementById('dawBpm').value = data.state.bpm;
                    renderSequencer();
                }
            });
        }

        function updateBpm(val) {
            fetch('/api/v1/synth/bpm', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ bpm: parseInt(val) })
            });
        }

        function updateSynthVolume(val) {
            document.getElementById('synthVolVal').textContent = val + '%';
            fetch('/api/v1/synth/volume', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ volume: parseInt(val) })
            });
        }

        function playWebTone(freq, duration = 0.18) {
            initAudio();
            try {
                const osc = audioCtx.createOscillator();
                const gain = audioCtx.createGain();
                const filter = audioCtx.createBiquadFilter();

                const cutoff = parseFloat(document.getElementById('synthCutoff')?.value || 2400);
                filter.type = 'lowpass';
                filter.frequency.setValueAtTime(cutoff, audioCtx.currentTime);
                filter.Q.setValueAtTime(3.0, audioCtx.currentTime);

                osc.type = document.getElementById('synthWaveform')?.value || 'sawtooth';
                osc.frequency.setValueAtTime(freq, audioCtx.currentTime);

                gain.gain.setValueAtTime(0.35, audioCtx.currentTime);
                gain.gain.exponentialRampToValueAtTime(0.001, audioCtx.currentTime + duration);

                osc.connect(filter);
                filter.connect(gain);
                gain.connect(audioCtx.destination);

                osc.start();
                osc.stop(audioCtx.currentTime + duration);
            } catch (e) {}

            fetch('/api/v1/synth/trigger', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ freq: Math.round(freq), duration_ms: Math.round(duration * 1000) })
            });
        }

        function renderPiano() {
            const kb = document.getElementById('pianoKeyboard');
            const notes = [
                { name: 'C4', f: 261.63, b: false }, { name: 'C#4', f: 277.18, b: true, l: '4.8%' },
                { name: 'D4', f: 293.66, b: false }, { name: 'D#4', f: 311.13, b: true, l: '11.8%' },
                { name: 'E4', f: 329.63, b: false },
                { name: 'F4', f: 349.23, b: false }, { name: 'F#4', f: 369.99, b: true, l: '25.8%' },
                { name: 'G4', f: 392.00, b: false }, { name: 'G#4', f: 415.30, b: true, l: '32.8%' },
                { name: 'A4', f: 440.00, b: false }, { name: 'A#4', f: 466.16, b: true, l: '39.8%' },
                { name: 'B4', f: 493.88, b: false },
                { name: 'C5', f: 523.25, b: false }, { name: 'C#5', f: 554.37, b: true, l: '53.8%' },
                { name: 'D5', f: 587.33, b: false }, { name: 'D#5', f: 622.25, b: true, l: '60.8%' },
                { name: 'E5', f: 659.25, b: false },
                { name: 'F5', f: 698.46, b: false }, { name: 'F#5', f: 739.99, b: true, l: '74.8%' },
                { name: 'G5', f: 783.99, b: false }, { name: 'G#5', f: 830.61, b: true, l: '81.8%' },
                { name: 'A5', f: 880.00, b: false }, { name: 'A#5', f: 932.33, b: true, l: '88.8%' },
                { name: 'B5', f: 987.77, b: false },
            ];

            kb.innerHTML = notes.filter(n => !n.b).map(n => `
                <div class="key-white" data-note="${n.name}" onclick="playWebTone(${n.f})">${n.name}</div>
            `).join('');

            notes.filter(n => n.b).forEach(n => {
                const el = document.createElement('div');
                el.className = 'key-black';
                el.style.left = n.l;
                el.setAttribute('data-note', n.name);
                el.onclick = () => playWebTone(n.f);
                kb.appendChild(el);
            });
        }

        // Live Dual Visualizer (Oscilloscope & Spectrum Bars)
        function startVisualizer() {
            const canvas = document.getElementById('scopeCanvas');
            if (!canvas) return;
            const ctx = canvas.getContext('2d');
            let phase = 0;

            function draw() {
                ctx.fillStyle = '#151923';
                ctx.fillRect(0, 0, canvas.width, canvas.height);

                // 16-band Spectrum bars in background
                const numBars = 16;
                const barWidth = canvas.width / numBars - 3;
                for (let i = 0; i < numBars; ++i) {
                    const noise = isPlaying ? Math.sin(phase * 1.5 + i * 0.8) : Math.sin(phase * 0.3 + i * 0.5);
                    const h = isPlaying ? Math.max(10, (noise + 1) * 35) : Math.max(4, (noise + 1) * 8);
                    ctx.fillStyle = isPlaying ? 'rgba(6, 182, 212, 0.25)' : 'rgba(71, 85, 105, 0.15)';
                    ctx.fillRect(i * (barWidth + 3) + 2, canvas.height - h, barWidth, h);
                }

                // Foreground Phosphor Waveform
                ctx.strokeStyle = isPlaying ? '#06b6d4' : '#475569';
                ctx.lineWidth = 2;
                ctx.beginPath();
                for (let x = 0; x < canvas.width; x++) {
                    const freq = isPlaying ? 0.05 : 0.02;
                    const amp = isPlaying ? 24 : 6;
                    const y = (canvas.height / 2) + Math.sin(x * freq + phase) * amp;
                    if (x === 0) ctx.moveTo(x, y);
                    else ctx.lineTo(x, y);
                }
                ctx.stroke();

                phase += 0.08;
                requestAnimationFrame(draw);
            }
            draw();
        }

        // Periodic DAW poll
        setInterval(() => {
            if (currentWorkload !== 'daw') return;
            fetch('/api/v1/synth/state')
                .then(r => r.json())
                .then(st => {
                    document.querySelectorAll('.seq-step').forEach(el => el.classList.remove('cursor'));
                    const curStep = st.current_step;
                    document.getElementById('stepCounter').textContent = `${(curStep + 1).toString().padStart(2, '0')} / 16`;
                    ['kick', 'snare', 'hihat', 'synth'].forEach(trk => {
                        const stepEl = document.getElementById(`step-${trk}-${curStep}`);
                        if (stepEl) stepEl.classList.add('cursor');
                    });
                });
        }, 120);

        // ===================================================================
        // WORKLOAD 2: AETHER WORKSPACE LOGIC
        // ===================================================================
        function handleWorkspaceHardwareKey(key) {
            const ta = document.getElementById('docTextarea');
            if (!ta) return;
            
            const start = ta.selectionStart;
            const end = ta.selectionEnd;
            let val = ta.value;

            if (key === '\\b') {
                if (start > 0) {
                    ta.value = val.substring(0, start - 1) + val.substring(end);
                    ta.selectionStart = ta.selectionEnd = start - 1;
                }
            } else if (key === '\\n') {
                ta.value = val.substring(0, start) + '\\n' + val.substring(end);
                ta.selectionStart = ta.selectionEnd = start + 1;
            } else if (key === '\\t') {
                ta.value = val.substring(0, start) + '  ' + val.substring(end);
                ta.selectionStart = ta.selectionEnd = start + 2;
            } else if (key.length === 1) {
                ta.value = val.substring(0, start) + key + val.substring(end);
                ta.selectionStart = ta.selectionEnd = start + 1;
            }

            handleDocInput();
        }

        function insertSyntax(prefix, suffix = '') {
            const ta = document.getElementById('docTextarea');
            const start = ta.selectionStart;
            const end = ta.selectionEnd;
            const selected = ta.value.substring(start, end);
            ta.value = ta.value.substring(0, start) + prefix + selected + suffix + ta.value.substring(end);
            ta.focus();
            ta.selectionStart = start + prefix.length;
            ta.selectionEnd = start + prefix.length + selected.length;
            handleDocInput();
        }

        function exportMarkdown() {
            const title = document.getElementById('docTitleInput').value || 'document';
            const content = document.getElementById('docTextarea').value;
            const blob = new Blob([content], { type: 'text/markdown;charset=utf-8;' });
            const url = URL.createObjectURL(blob);
            const a = document.createElement('a');
            a.href = url;
            a.download = `${title.toLowerCase().replace(/\\s+/g, '_')}.md`;
            a.click();
            URL.revokeObjectURL(url);
        }

        function initWorkspace() {
            fetchDocs();
            drawKnowledgeGraph();
        }

        function fetchDocs() {
            fetch('/api/v1/workspace/docs')
                .then(r => r.json())
                .then(docs => {
                    const list = document.getElementById('docsList');
                    list.innerHTML = Object.values(docs).map(d => `
                        <div class="doc-item ${d.id === currentDocId ? 'active' : ''}" onclick="selectDoc('${d.id}')">
                            <span style="font-size: 13px;">📄</span>
                            <span style="overflow: hidden; text-overflow: ellipsis; white-space: nowrap;">${d.title}</span>
                        </div>
                    `).join('');

                    if (docs[currentDocId]) {
                        loadDoc(docs[currentDocId]);
                    }
                });
        }

        function selectDoc(id) {
            currentDocId = id;
            fetchDocs();
        }

        function loadDoc(doc) {
            document.getElementById('docTitleInput').value = doc.title;
            document.getElementById('docTextarea').value = doc.content;
            renderMarkdown(doc.content);
        }

        function handleDocInput() {
            const content = document.getElementById('docTextarea').value;
            renderMarkdown(content);
            saveCurrentDoc();
        }

        function renderMarkdown(md) {
            let html = md
                .replace(/^# (.*$)/gim, '<h1>$1</h1>')
                .replace(/^## (.*$)/gim, '<h2>$1</h2>')
                .replace(/^### (.*$)/gim, '<h3>$1</h3>')
                .replace(/^\\*\\*(.*?)\\*\\*/gim, '<strong>$1</strong>')
                .replace(/^\\*(.*?)\\*/gim, '<em>$1</em>')
                .replace(/```([\\s\\S]*?)```/gim, '<pre><code>$1</code></pre>')
                .replace(/^- \\[x\\] (.*$)/gim, '<p><input type="checkbox" checked disabled> $1</p>')
                .replace(/^- \\[ \\] (.*$)/gim, '<p><input type="checkbox" disabled> $1</p>')
                .replace(/^- (.*$)/gim, '<li>$1</li>')
                .replace(/^> (.*$)/gim, '<blockquote style="border-left: 3px solid var(--accent-cyan); padding-left: 10px; color: var(--text-muted);">$1</blockquote>')
                .replace(/\\n\\n/gim, '<p></p>');

            document.getElementById('docPreview').innerHTML = html;

            const words = md.trim().split(/\\s+/).filter(w => w.length > 0).length;
            const readTime = Math.max(1, Math.round(words / 200));
            document.getElementById('docMetrics').textContent = `Words: ${words} | Est. Read: ${readTime} min | Saved`;
        }

        function saveCurrentDoc() {
            const title = document.getElementById('docTitleInput').value;
            const content = document.getElementById('docTextarea').value;
            fetch(`/api/v1/workspace/docs/${currentDocId}`, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ title, content })
            });
        }

        function createNewDoc() {
            const id = 'doc_' + Math.random().toString(36).substr(2, 6);
            fetch('/api/v1/workspace/docs/new', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ id, title: 'Untitled Document', content: '# Untitled Document\\n\\nType your specification here...' })
            }).then(() => {
                currentDocId = id;
                fetchDocs();
            });
        }

        // Draggable Interactive Knowledge Graph
        function drawKnowledgeGraph() {
            const canvas = document.getElementById('graphCanvas');
            if (!canvas) return;
            const ctx = canvas.getContext('2d');
            const w = canvas.width;
            const h = canvas.height;

            const nodes = [
                { id: 'doc_consensus', x: w * 0.35, y: h * 0.3, r: 8, label: 'Consensus', color: '#6366f1' },
                { id: 'doc_wire', x: w * 0.65, y: h * 0.28, r: 7, label: 'Wire Proto', color: '#8b5cf6' },
                { id: 'doc_dsp', x: w * 0.5, y: h * 0.7, r: 8, label: 'Acoustic DSP', color: '#06b6d4' },
                { id: 'doc_fleet', x: w * 0.22, y: h * 0.65, r: 7, label: 'Fleet Radar', color: '#10b981' },
                { id: 'hw_keyring', x: w * 0.78, y: h * 0.65, r: 6, label: 'Key Terminal', color: '#f59e0b' }
            ];

            const links = [
                [nodes[0], nodes[1]],
                [nodes[0], nodes[2]],
                [nodes[1], nodes[2]],
                [nodes[0], nodes[3]],
                [nodes[1], nodes[4]],
                [nodes[2], nodes[4]]
            ];

            let draggedNode = null;

            canvas.onmousedown = (e) => {
                const rect = canvas.getBoundingClientRect();
                const mx = e.clientX - rect.left;
                const my = e.clientY - rect.top;
                nodes.forEach(n => {
                    if (Math.hypot(n.x - mx, n.y - my) < n.r + 6) {
                        draggedNode = n;
                        if (n.id.startsWith('doc_')) selectDoc(n.id);
                    }
                });
            };

            window.onmousemove = (e) => {
                if (draggedNode) {
                    const rect = canvas.getBoundingClientRect();
                    draggedNode.x = e.clientX - rect.left;
                    draggedNode.y = e.clientY - rect.top;
                }
            };

            window.onmouseup = () => { draggedNode = null; };

            function render() {
                if (currentWorkload !== 'workspace') {
                    requestAnimationFrame(render);
                    return;
                }

                ctx.fillStyle = '#151923';
                ctx.fillRect(0, 0, w, h);

                // Links
                ctx.strokeStyle = 'rgba(255, 255, 255, 0.12)';
                ctx.lineWidth = 1.5;
                links.forEach(([n1, n2]) => {
                    ctx.beginPath();
                    ctx.moveTo(n1.x, n1.y);
                    ctx.lineTo(n2.x, n2.y);
                    ctx.stroke();
                });

                // Nodes
                nodes.forEach(n => {
                    ctx.beginPath();
                    ctx.arc(n.x, n.y, n.r, 0, Math.PI * 2);
                    ctx.fillStyle = n.color;
                    ctx.shadowColor = n.color;
                    ctx.shadowBlur = 8;
                    ctx.fill();
                    ctx.shadowBlur = 0;

                    ctx.fillStyle = '#cbd5e1';
                    ctx.font = '10px JetBrains Mono';
                    ctx.fillText(n.label, n.x + 12, n.y + 3);
                });

                requestAnimationFrame(render);
            }
            render();
        }

        // ===================================================================
        // WORKLOAD 3: AEGIS FLEET OPERATIONS LOGIC
        // ===================================================================
        function handleFleetHardwareKey(key) {
            initAudio();
            const lower = key.toLowerCase();
            if (lower === 'a') sendFleetCmd('ARM_LAUNCH');
            else if (lower === 'h') sendFleetCmd('HOLD_ORBIT');
            else if (lower === 'r') sendFleetCmd('RTL');
            else if (lower === 'e') sendFleetCmd('EMERGENCY_HALT');
            else if (key === '1') selectUnit('ALPHA-1');
            else if (key === '2') selectUnit('BRAVO-2');
            else if (key === '3') selectUnit('TITAN-4');
            else if (key === '4') selectUnit('VALKYRIE-9');
        }

        function initFleet() {
            fetchFleet();
            drawRadar();
        }

        function fetchFleet() {
            fetch('/api/v1/fleet/status')
                .then(r => r.json())
                .then(data => {
                    const container = document.getElementById('fleetUnitsContainer');
                    container.innerHTML = Object.values(data.units).map(u => `
                        <div class="unit-card ${u.id === currentSelectedUnit ? 'active' : ''}" onclick="selectUnit('${u.id}')">
                            <div class="unit-top">
                                <span class="unit-name">${u.name}</span>
                                <span class="unit-status">${u.status}</span>
                            </div>
                            <div class="unit-telemetry">
                                <div>ALT: <strong>${u.alt_m}m</strong></div>
                                <div>SPD: <strong>${u.speed_mps} m/s</strong></div>
                                <div>BAT: <strong>${u.battery_pct}%</strong></div>
                                <div>RSSI: <strong>${u.rssi_dbm} dBm</strong></div>
                            </div>
                        </div>
                    `).join('');

                    const logEl = document.getElementById('missionLog');
                    logEl.innerHTML = data.events.map(ev => `
                        <div style="border-bottom: 1px solid var(--border-dim); padding: 4px 0;">
                            <span style="color: var(--accent-cyan); font-weight: 700;">[${ev.unit}]</span>
                            <span>${ev.msg}</span>
                        </div>
                    `).join('');
                });
        }

        function selectUnit(id) {
            currentSelectedUnit = id;
            fetchFleet();
        }

        function sendFleetCmd(cmd) {
            initAudio();
            fetch('/api/v1/fleet/command', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ unit: currentSelectedUnit, command: cmd })
            }).then(() => fetchFleet());
        }

        function handleRadarClick(e) {
            const rect = e.target.getBoundingClientRect();
            const x = e.clientX - rect.left;
            const y = e.clientY - rect.top;
            targetWaypoint = { x, y };

            fetch('/api/v1/fleet/waypoint', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ unit: currentSelectedUnit, canvas_x: x, canvas_y: y })
            }).then(() => fetchFleet());
        }

        function drawRadar() {
            const canvas = document.getElementById('radarCanvas');
            if (!canvas) return;
            const ctx = canvas.getContext('2d');
            const w = canvas.width;
            const h = canvas.height;
            const cx = w / 2;
            const cy = h / 2;
            let sweepAngle = 0;

            function frame() {
                if (currentWorkload !== 'fleet') {
                    requestAnimationFrame(frame);
                    return;
                }

                ctx.fillStyle = '#04060a';
                ctx.fillRect(0, 0, w, h);

                // Range Circles
                ctx.strokeStyle = 'rgba(6, 182, 212, 0.14)';
                ctx.lineWidth = 1;
                [60, 120, 180, 230].forEach((r, idx) => {
                    ctx.beginPath();
                    ctx.arc(cx, cy, r, 0, Math.PI * 2);
                    ctx.stroke();

                    ctx.fillStyle = 'rgba(6, 182, 212, 0.4)';
                    ctx.font = '9px JetBrains Mono';
                    ctx.fillText(`${(idx + 1) * 5}km`, cx + r - 25, cy - 4);
                });

                // Cardinal Crosshairs
                ctx.beginPath();
                ctx.moveTo(cx, cy - 230); ctx.lineTo(cx, cy + 230);
                ctx.moveTo(cx - 230, cy); ctx.lineTo(cx + 230, cy);
                ctx.stroke();

                // Cardinal Headings
                ctx.fillStyle = '#64748b';
                ctx.font = '10px JetBrains Mono';
                ctx.fillText('N 000°', cx - 18, cy - 236);
                ctx.fillText('S 180°', cx - 18, cy + 246);
                ctx.fillText('E 090°', cx + 236, cy + 4);
                ctx.fillText('W 270°', cx - 275, cy + 4);

                // Sweep Beam with Glow Wake
                ctx.save();
                ctx.translate(cx, cy);
                ctx.rotate(sweepAngle);

                const grad = ctx.createRadialGradient(0, 0, 0, 0, 0, 230);
                grad.addColorStop(0, 'rgba(6, 182, 212, 0.8)');
                grad.addColorStop(1, 'rgba(6, 182, 212, 0.0)');

                ctx.strokeStyle = grad;
                ctx.lineWidth = 2;
                ctx.beginPath();
                ctx.moveTo(0, 0);
                ctx.lineTo(230, 0);
                ctx.stroke();
                ctx.restore();

                // Draw Vehicle Blips & Trails
                const colors = {
                    'ALPHA-1': '#10b981',
                    'BRAVO-2': '#06b6d4',
                    'TITAN-4': '#f59e0b',
                    'VALKYRIE-9': '#a855f7'
                };

                Object.values(FLEET_UNITS).forEach(u => {
                    const c = colors[u.id] || '#fff';

                    // Breadcrumb Trail
                    if (u.trail && u.trail.length > 1) {
                        ctx.strokeStyle = c;
                        ctx.lineWidth = 1;
                        ctx.setLineDash([2, 4]);
                        ctx.beginPath();
                        u.trail.forEach((pt, idx) => {
                            if (idx === 0) ctx.moveTo(pt[0], pt[1]);
                            else ctx.lineTo(pt[0], pt[1]);
                        });
                        ctx.stroke();
                        ctx.setLineDash([]);
                    }

                    // Blip
                    ctx.beginPath();
                    ctx.arc(u.x, u.y, 4.5, 0, Math.PI * 2);
                    ctx.fillStyle = c;
                    ctx.shadowColor = c;
                    ctx.shadowBlur = 10;
                    ctx.fill();
                    ctx.shadowBlur = 0;

                    // Selection Ring
                    if (u.id === currentSelectedUnit) {
                        ctx.strokeStyle = '#fff';
                        ctx.lineWidth = 1.5;
                        ctx.beginPath();
                        ctx.arc(u.x, u.y, 9, 0, Math.PI * 2);
                        ctx.stroke();

                        // Target vector line if waypoint set
                        if (targetWaypoint) {
                            ctx.strokeStyle = 'rgba(245, 158, 11, 0.8)';
                            ctx.lineWidth = 1;
                            ctx.setLineDash([4, 4]);
                            ctx.beginPath();
                            ctx.moveTo(u.x, u.y);
                            ctx.lineTo(targetWaypoint.x, targetWaypoint.y);
                            ctx.stroke();
                            ctx.setLineDash([]);

                            ctx.beginPath();
                            ctx.arc(targetWaypoint.x, targetWaypoint.y, 6, 0, Math.PI * 2);
                            ctx.strokeStyle = '#f59e0b';
                            ctx.stroke();
                        }
                    }

                    // Label Tag
                    ctx.fillStyle = '#cbd5e1';
                    ctx.font = '10px JetBrains Mono';
                    ctx.fillText(`${u.id} (${u.alt_m}m)`, u.x + 8, u.y + 3);
                });

                sweepAngle += 0.035;
                requestAnimationFrame(frame);
            }
            frame();
        }

        // Global Periodic Hardware Pollers
        setInterval(pollHardwareKeyboard, 80);
        setInterval(() => {
            if (currentWorkload === 'fleet') fetchFleet();
        }, 1500);

        // Auto-initialize
        switchWorkload('daw');
    </script>
</body>
</html>
"""


# ---------------------------------------------------------------------------
# HTTP Multi-Workload API Handler
# ---------------------------------------------------------------------------
def handle_workload_http_request(method, path, headers, body, hardware_telemetry=None, send_serial_cmd=None):
    """
    Main cluster router dispatching HTTP requests to the 3 workload engines.
    """
    start_suite_engine(send_serial_cmd)

    parsed = urlparse(path)
    path_clean = parsed.path
    query = parse_qs(parsed.query)

    # 1. Main Suite Frontend
    if path_clean in ['/', '/index.html', '/synth', '/workspace', '/fleet']:
        return (200, 'text/html; charset=utf-8', SUITE_HTML.encode('utf-8'))

    # =======================================================================
    # HARDWARE KEYBOARD & TELEMETRY APIS
    # =======================================================================
    if path_clean == '/api/v1/hardware/keyboard':
        events = []
        last_key = ""
        seq = 0
        if hardware_telemetry:
            last_key = hardware_telemetry.get("last_key", "")
            seq = hardware_telemetry.get("key_seq", 0)
            events = list(hardware_telemetry.get("key_events", []))
        resp = {
            "last_key": last_key,
            "seq": seq,
            "events": events
        }
        return (200, 'application/json', json.dumps(resp).encode('utf-8'))

    if path_clean == '/api/v1/hardware/telemetry':
        free_sram = 48400
        last_key = ""
        seq = 0
        if hardware_telemetry:
            free_sram = hardware_telemetry.get("free_sram_bytes", 48400)
            last_key = hardware_telemetry.get("last_key", "")
            seq = hardware_telemetry.get("key_seq", 0)
        resp = {
            "free_sram_bytes": free_sram,
            "last_key": last_key,
            "key_seq": seq,
            "timestamp": time.time()
        }
        return (200, 'application/json', json.dumps(resp).encode('utf-8'))

    # =======================================================================
    # WORKLOAD 1: VORTEX DAW & SYNTH APIS
    # =======================================================================
    if path_clean == '/api/v1/synth/state':
        with suite_lock:
            data = json.dumps(SYNTH_STATE).encode('utf-8')
        return (200, 'application/json', data)

    if path_clean == '/api/v1/synth/pattern':
        try:
            d = json.loads(body.decode('utf-8') if isinstance(body, bytes) else body)
            with suite_lock:
                if 'tracks' in d:
                    SYNTH_STATE['tracks'] = d['tracks']
            return (200, 'application/json', b'{"status":"OK"}')
        except Exception as e:
            return (500, 'application/json', json.dumps({"error": str(e)}).encode('utf-8'))

    if path_clean == '/api/v1/synth/preset' and method == 'POST':
        try:
            d = json.loads(body.decode('utf-8') if isinstance(body, bytes) else body)
            preset_name = d.get('preset', 'CYBERPUNK')
            with suite_lock:
                SYNTH_STATE['preset'] = preset_name
                if preset_name == 'CYBERPUNK':
                    SYNTH_STATE['bpm'] = 128
                    SYNTH_STATE['tracks']['kick']  = [1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]
                    SYNTH_STATE['tracks']['snare'] = [0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0]
                    SYNTH_STATE['tracks']['hihat'] = [1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1]
                    SYNTH_STATE['tracks']['synth'] = [0, 4, 0, 7, 0, 9, 0, 11, 0, 12, 0, 9, 0, 7, 4, 0]
                elif preset_name == 'MINIMAL TECHNO':
                    SYNTH_STATE['bpm'] = 124
                    SYNTH_STATE['tracks']['kick']  = [1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]
                    SYNTH_STATE['tracks']['snare'] = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
                    SYNTH_STATE['tracks']['hihat'] = [0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0]
                    SYNTH_STATE['tracks']['synth'] = [0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 5, 0, 0, 0, 2, 0]
                elif preset_name == 'LO-FI CHILL':
                    SYNTH_STATE['bpm'] = 84
                    SYNTH_STATE['tracks']['kick']  = [1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0]
                    SYNTH_STATE['tracks']['snare'] = [0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0]
                    SYNTH_STATE['tracks']['hihat'] = [1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0]
                    SYNTH_STATE['tracks']['synth'] = [4, 0, 0, 0, 7, 0, 0, 0, 9, 0, 0, 0, 11, 0, 0, 0]
                elif preset_name == 'ACID ELECTRO':
                    SYNTH_STATE['bpm'] = 128
                    SYNTH_STATE['tracks']['kick']  = [1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 1, 0]
                    SYNTH_STATE['tracks']['snare'] = [0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0]
                    SYNTH_STATE['tracks']['hihat'] = [1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1]
                    SYNTH_STATE['tracks']['synth'] = [2, 2, 5, 2, 7, 2, 9, 2, 12, 11, 9, 7, 5, 4, 2, 0]
            return (200, 'application/json', json.dumps({"status": "OK", "state": SYNTH_STATE}).encode('utf-8'))
        except Exception as e:
            return (500, 'application/json', json.dumps({"error": str(e)}).encode('utf-8'))

    if path_clean == '/api/v1/synth/transport':
        try:
            d = json.loads(body.decode('utf-8') if isinstance(body, bytes) else body)
            with suite_lock:
                SYNTH_STATE['playing'] = bool(d.get('playing', False))
            return (200, 'application/json', b'{"status":"OK"}')
        except Exception as e:
            return (500, 'application/json', json.dumps({"error": str(e)}).encode('utf-8'))

    if path_clean == '/api/v1/synth/bpm':
        try:
            d = json.loads(body.decode('utf-8') if isinstance(body, bytes) else body)
            with suite_lock:
                SYNTH_STATE['bpm'] = int(d.get('bpm', 120))
            return (200, 'application/json', b'{"status":"OK"}')
        except Exception as e:
            return (500, 'application/json', json.dumps({"error": str(e)}).encode('utf-8'))

    if path_clean == '/api/v1/synth/volume':
        try:
            d = json.loads(body.decode('utf-8') if isinstance(body, bytes) else body)
            with suite_lock:
                vol = int(d.get('volume', 20))
                SYNTH_STATE['volume'] = vol
            if send_serial_cmd and vol == 0:
                send_serial_cmd("K32_CMD:BEEP:0:0:0")
            return (200, 'application/json', b'{"status":"OK"}')
        except Exception as e:
            return (500, 'application/json', json.dumps({"error": str(e)}).encode('utf-8'))

    if path_clean == '/api/v1/synth/trigger':
        try:
            d = json.loads(body.decode('utf-8') if isinstance(body, bytes) else body)
            freq = int(d.get('freq', 440))
            dur = int(d.get('duration_ms', 100))
            vol = SYNTH_STATE['volume']
            if send_serial_cmd:
                send_serial_cmd(f"K32_CMD:BEEP:{freq}:{dur}:{vol}")
            return (200, 'application/json', json.dumps({"status": "OK", "freq": freq, "dur": dur, "vol": vol}).encode('utf-8'))
        except Exception as e:
            return (500, 'application/json', json.dumps({"error": str(e)}).encode('utf-8'))

    # =======================================================================
    # WORKLOAD 2: AETHER WORKSPACE APIS
    # =======================================================================
    if path_clean == '/api/v1/workspace/docs':
        with suite_lock:
            data = json.dumps(WORKSPACE_DOCS).encode('utf-8')
        return (200, 'application/json', data)

    if path_clean.startswith('/api/v1/workspace/docs/') and method == 'POST':
        doc_id = path_clean.split('/')[5]
        try:
            d = json.loads(body.decode('utf-8') if isinstance(body, bytes) else body)
            with suite_lock:
                if doc_id in WORKSPACE_DOCS:
                    WORKSPACE_DOCS[doc_id]['title'] = d.get('title', WORKSPACE_DOCS[doc_id]['title'])
                    WORKSPACE_DOCS[doc_id]['content'] = d.get('content', WORKSPACE_DOCS[doc_id]['content'])
                    WORKSPACE_DOCS[doc_id]['updated_at'] = time.time()
                    if send_serial_cmd:
                        send_serial_cmd(f"K32_CMD:DISPLAY:DOC: {WORKSPACE_DOCS[doc_id]['title'][:20]}")
            return (200, 'application/json', b'{"status":"OK"}')
        except Exception as e:
            return (500, 'application/json', json.dumps({"error": str(e)}).encode('utf-8'))

    if path_clean == '/api/v1/workspace/docs/new' and method == 'POST':
        try:
            d = json.loads(body.decode('utf-8') if isinstance(body, bytes) else body)
            doc_id = d.get('id', f"doc_{int(time.time())}")
            with suite_lock:
                WORKSPACE_DOCS[doc_id] = {
                    "id": doc_id,
                    "title": d.get('title', 'Untitled'),
                    "category": "Notes",
                    "updated_at": time.time(),
                    "content": d.get('content', '')
                }
            return (200, 'application/json', b'{"status":"OK"}')
        except Exception as e:
            return (500, 'application/json', json.dumps({"error": str(e)}).encode('utf-8'))

    # =======================================================================
    # WORKLOAD 3: AEGIS FLEET OPERATIONS APIS
    # =======================================================================
    if path_clean == '/api/v1/fleet/status':
        with suite_lock:
            out = {
                "units": FLEET_UNITS,
                "events": MISSION_EVENTS[-15:]
            }
            data = json.dumps(out).encode('utf-8')
        return (200, 'application/json', data)

    if path_clean == '/api/v1/fleet/command' and method == 'POST':
        try:
            d = json.loads(body.decode('utf-8') if isinstance(body, bytes) else body)
            unit_id = d.get('unit', 'ALPHA-1')
            cmd = d.get('command', 'HOLD_ORBIT')
            with suite_lock:
                if unit_id in FLEET_UNITS:
                    FLEET_UNITS[unit_id]['current_cmd'] = cmd
                    if cmd == 'RTL':
                        FLEET_UNITS[unit_id]['status'] = 'RETURNING'
                    elif cmd == 'EMERGENCY_HALT':
                        FLEET_UNITS[unit_id]['status'] = 'HALTED'
                        FLEET_UNITS[unit_id]['speed_mps'] = 0.0
                    elif cmd == 'ARM_LAUNCH':
                        FLEET_UNITS[unit_id]['status'] = 'IN_FLIGHT'
                    MISSION_EVENTS.append({
                        "time": time.time(),
                        "unit": unit_id,
                        "level": "COMMAND",
                        "msg": f"Dispatched command: {cmd}"
                    })
                if send_serial_cmd:
                    send_serial_cmd(f"K32_CMD:DISPLAY:CMD: {cmd} -> {unit_id}")
                    send_serial_cmd(f"K32_CMD:BEEP:1200:60:{SYNTH_STATE['volume']}")
            return (200, 'application/json', b'{"status":"OK"}')
        except Exception as e:
            return (500, 'application/json', json.dumps({"error": str(e)}).encode('utf-8'))

    if path_clean == '/api/v1/fleet/waypoint' and method == 'POST':
        try:
            d = json.loads(body.decode('utf-8') if isinstance(body, bytes) else body)
            unit_id = d.get('unit', 'ALPHA-1')
            cx = d.get('canvas_x', 350)
            cy = d.get('canvas_y', 260)
            with suite_lock:
                MISSION_EVENTS.append({
                    "time": time.time(),
                    "unit": unit_id,
                    "level": "INFO",
                    "msg": f"Target waypoint set to ({cx:.0f}, {cy:.0f}). Vector plotted."
                })
                if unit_id in FLEET_UNITS:
                    FLEET_UNITS[unit_id]['current_cmd'] = f"TRANSIT -> ({cx:.0f}, {cy:.0f})"
                if send_serial_cmd:
                    send_serial_cmd(f"K32_CMD:DISPLAY:WAYPOINT: {unit_id}")
                    send_serial_cmd(f"K32_CMD:BEEP:1500:40:{SYNTH_STATE['volume']}")
            return (200, 'application/json', b'{"status":"OK"}')
        except Exception as e:
            return (500, 'application/json', json.dumps({"error": str(e)}).encode('utf-8'))

    # Hardware fallback endpoints
    if path_clean == '/api/hardware/display':
        text = ""
        try:
            d = json.loads(body.decode('utf-8', errors='replace') if isinstance(body, bytes) else body)
            text = d.get("text", "")
            if hardware_telemetry:
                hardware_telemetry["display_text"] = text
            if send_serial_cmd:
                send_serial_cmd(f"K32_CMD:DISPLAY:{text}")
        except Exception as e:
            print(f"[WORKLOAD-ERR] Display update error: {e}")
        resp = {"status": "OK", "action": "display_updated", "text": text}
        return (200, 'application/json', json.dumps(resp).encode('utf-8'))

    if path_clean == '/api/hardware/speaker':
        freq = 1000
        dur = 100
        vol = 20
        try:
            d = json.loads(body.decode('utf-8', errors='replace') if isinstance(body, bytes) else body)
            freq = d.get("frequency_hz", 1000)
            dur = d.get("duration_ms", 100)
            vol = d.get("volume_pct", d.get("volume", 20))
            if send_serial_cmd:
                send_serial_cmd(f"K32_CMD:BEEP:{freq}:{dur}:{vol}")
        except Exception as e:
            print(f"[WORKLOAD-ERR] Speaker tone error: {e}")
        resp = {"status": "OK", "action": "tone_played", "freq": freq, "dur": dur, "volume": vol}
        return (200, 'application/json', json.dumps(resp).encode('utf-8'))

    return (404, 'text/plain; charset=utf-8', b'404 Not Found')
