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
    "bpm": 120,
    "playing": False,
    "current_step": 0,
    "volume": 20,
    "waveform": "sawtooth",
    "attack_ms": 20,
    "decay_ms": 120,
    "sustain_pct": 65,
    "release_ms": 180,
    "filter_cutoff_hz": 2400,
    "resonance_q": 2.5,
    "tracks": {
        "kick":  [1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0,  1, 0, 0, 0],
        "snare": [0, 0, 0, 0,  1, 0, 0, 0,  0, 0, 0, 0,  1, 0, 0, 0],
        "hihat": [1, 0, 1, 0,  1, 0, 1, 0,  1, 0, 1, 0,  1, 0, 1, 0],
        "synth": [0, 2, 0, 4,  0, 5, 0, 7,  0, 9, 0, 11, 0, 12, 0, 0], # Note index
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
}

# Workload 2: Aether Workspace Documents & Knowledge Graph
WORKSPACE_DOCS = {
    "doc_arch": {
        "id": "doc_arch",
        "title": "System Architecture & High-Performance State Engine",
        "category": "Architecture",
        "updated_at": time.time() - 3600,
        "content": """# System Architecture & High-Performance State Engine

High-throughput distributed systems require predictable memory consumption, zero garbage collection pauses, and low-latency virtual message passing across processing nodes.

## Core Architectural Pillars

1. **Zero-Copy Memory Arenas**: Pre-allocated monotonic memory blocks eliminate dynamic heap fragmentation.
2. **Radix-Tree State Store**: Adaptive compact radix tree provides $O(k)$ key retrieval with sub-microsecond locking.
3. **Synchronous Hardware Bridges**: Decoupled hardware scanner rings poll at 100 Hz without inducing cache eviction.

### Data Flow Diagram

```text
[ Client Request ] ---> [ Edge Gateway ] ---> [ Service Mesh Router ]
                                                     |
                                                     v
                                          [ State Memory Arena ]
```

### System Invariants
- Memory allocation bounded at boot
- Network round-trip latency $\le 1.2\\text{ ms}$
- Audit trail cryptographic verification enabled
"""
    },
    "doc_specs": {
        "id": "doc_specs",
        "title": "Autonomous Coordination Protocol Specs v3",
        "category": "Specifications",
        "updated_at": time.time() - 7200,
        "content": """# Autonomous Coordination Protocol Specs v3

This specification outlines the peer-to-peer packet framing, consensus verification, and fault-tolerance parameters.

## Packet Envelope Structure

| Offset (Bytes) | Field Name | Type | Description |
| :--- | :--- | :--- | :--- |
| 0..3 | Magic Header | uint32 | `0x4B333201` protocol signature |
| 4..5 | Sequence ID | uint16 | Monotonically increasing counter |
| 6..7 | Payload Length | uint16 | Wire packet byte length |
| 8..11 | CRC32 | uint32 | Frame integrity checksum |

## Heartbeat & Eviction Rules
- Periodic heartbeat interval: **500 ms**
- Leeway grace period: **15,000 ms**
- Node failover and eviction trigger: **30,000 ms**
"""
    },
    "doc_ops": {
        "id": "doc_ops",
        "title": "Tactical Field Operations & Sensor Deployment",
        "category": "Operations",
        "updated_at": time.time() - 14400,
        "content": """# Tactical Field Operations & Sensor Deployment

Operational protocols for remote autonomous fleets, sensor array telemetry, and mission command dispatch.

### Mission Readiness Checklist
- [x] Radio telemetry link RSSI verified $> -72\\text{ dBm}$
- [x] Inertial Measurement Unit (IMU) calibrated
- [x] Fail-safe Return-to-Base (RTL) coordinates programmed
- [ ] Sensor optical payload armed
"""
    }
}

GRAPH_NODES = [
    {"id": "doc_arch", "name": "Architecture Specs", "group": "doc", "val": 18},
    {"id": "doc_specs", "name": "Protocol Specs", "group": "doc", "val": 14},
    {"id": "doc_ops", "name": "Field Ops", "group": "doc", "val": 12},
    {"id": "state_engine", "name": "State Engine", "group": "tech", "val": 10},
    {"id": "mesh_router", "name": "Mesh Router", "group": "tech", "val": 10},
    {"id": "hardware_hsm", "name": "Hardware Terminal", "group": "hw", "val": 14},
    {"id": "telemetry", "name": "Telemetry Feed", "group": "hw", "val": 8},
]

GRAPH_LINKS = [
    {"source": "doc_arch", "target": "state_engine"},
    {"source": "doc_arch", "target": "mesh_router"},
    {"source": "doc_specs", "target": "mesh_router"},
    {"source": "doc_specs", "target": "hardware_hsm"},
    {"source": "doc_ops", "target": "telemetry"},
    {"source": "hardware_hsm", "target": "telemetry"},
    {"source": "doc_arch", "target": "hardware_hsm"},
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
        "alt_m": 124.5,
        "speed_mps": 16.8,
        "heading_deg": 48.0,
        "battery_pct": 84,
        "rssi_dbm": -68,
        "current_cmd": "WAYPOINT_TRANSIT",
        "waypoints": [[37.7749, -122.4194], [37.7780, -122.4120], [37.7810, -122.4160]]
    },
    "BRAVO-2": {
        "id": "BRAVO-2",
        "name": "Bravo Lifter (Hexacopter)",
        "type": "UAV-Cargo",
        "status": "HOLD_ORBIT",
        "lat": 37.7825,
        "lng": -122.4080,
        "alt_m": 210.0,
        "speed_mps": 8.2,
        "heading_deg": 135.0,
        "battery_pct": 92,
        "rssi_dbm": -62,
        "current_cmd": "HOLD_STATION",
        "waypoints": [[37.7825, -122.4080]]
    },
    "TITAN-4": {
        "id": "TITAN-4",
        "name": "Titan Rover (Unmanned Ground)",
        "type": "UGV-Ground",
        "status": "SURVEYING",
        "lat": 37.7710,
        "lng": -122.4240,
        "alt_m": 14.0,
        "speed_mps": 4.1,
        "heading_deg": 280.0,
        "battery_pct": 78,
        "rssi_dbm": -74,
        "current_cmd": "PATH_PATROL",
        "waypoints": [[37.7710, -122.4240], [37.7730, -122.4280]]
    },
    "VALKYRIE-9": {
        "id": "VALKYRIE-9",
        "name": "Valkyrie Recon (Fixed-Wing)",
        "type": "UAV-FixedWing",
        "status": "HIGH_CRUISE",
        "lat": 37.7950,
        "lng": -122.3990,
        "alt_m": 820.0,
        "speed_mps": 34.5,
        "heading_deg": 195.0,
        "battery_pct": 89,
        "rssi_dbm": -58,
        "current_cmd": "WIDE_SWEEP",
        "waypoints": [[37.7950, -122.3990], [37.7650, -122.4350]]
    }
}

MISSION_EVENTS = [
    {"time": time.time() - 180, "unit": "ALPHA-1", "level": "INFO", "msg": "Waypoint 2 reached. Adjusting bearing 048 deg."},
    {"time": time.time() - 120, "unit": "VALKYRIE-9", "level": "SUCCESS", "msg": "Recon sweep 4 completed. Sensor payload active."},
    {"time": time.time() - 45, "unit": "TITAN-4", "level": "WARN", "msg": "Terrain incline detected: 14 deg slope compensation engaged."}
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
                    step_duration = 60.0 / (SYNTH_STATE["bpm"] * 4.0) # 16th notes
                    if now - last_seq_tick >= step_duration:
                        step = (SYNTH_STATE["current_step"] + 1) % 16
                        SYNTH_STATE["current_step"] = step
                        last_seq_tick = now

                        # Check if Synth Track has a note on this step
                        synth_note = SYNTH_STATE["tracks"]["synth"][step]
                        if synth_note > 0 and send_serial_cmd:
                            freq = int(NOTE_FREQS.get(synth_note, 440.0))
                            vol = SYNTH_STATE["volume"]
                            # Trigger single-shot hardware audio chime
                            send_serial_cmd(f"K32_CMD:BEEP:{freq}:45:{vol}")

                # 2. Autonomous Fleet Position Updates (Physics Simulation)
                if now - last_fleet_tick >= 1.0:
                    last_fleet_tick = now
                    for uid, u in FLEET_UNITS.items():
                        # Realistic subtle flight dynamics
                        rad = math.radians(u["heading_deg"])
                        # Move slightly along heading vector
                        d_lat = (u["speed_mps"] * 0.000008) * math.cos(rad)
                        d_lng = (u["speed_mps"] * 0.000008) * math.sin(rad)
                        u["lat"] = round(u["lat"] + d_lat, 5)
                        u["lng"] = round(u["lng"] + d_lng, 5)
                        # Small altitude drift
                        if u["type"] != "UGV-Ground":
                            u["alt_m"] = round(u["alt_m"] + random.uniform(-0.8, 0.8), 1)

                # 3. Physical Hardware Display Sync (every 2.5s)
                if send_serial_cmd and (now - last_screen_tick >= 2.5):
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
    <title>Aether Suite | High-Performance Distributed Workbenches</title>
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
            
            --border-dim: rgba(255, 255, 255, 0.07);
            --border-subtle: rgba(255, 255, 255, 0.12);
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
            gap: 12px;
            font-weight: 700;
            font-size: 15px;
            letter-spacing: -0.02em;
        }

        .brand-badge {
            width: 24px;
            height: 24px;
            background: linear-gradient(135deg, #6366f1, #8b5cf6);
            border-radius: 6px;
            display: flex;
            align-items: center;
            justify-content: center;
            font-size: 12px;
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

        /* Workload Canvas Container */
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
            grid-template-columns: 320px 1fr;
            min-height: calc(100vh - 52px);
            background: var(--border-dim);
            gap: 1px;
        }

        .daw-sidebar {
            background: var(--bg-canvas);
            padding: 20px;
            display: flex;
            flex-direction: column;
            gap: 18px;
        }

        .daw-main {
            background: var(--bg-canvas);
            padding: 24px;
            overflow-y: auto;
        }

        .transport-bar {
            background: var(--bg-card);
            border: 1px solid var(--border-dim);
            border-radius: 8px;
            padding: 12px 16px;
            display: flex;
            align-items: center;
            justify-content: space-between;
            margin-bottom: 20px;
        }

        .btn-transport {
            background: var(--bg-elevated);
            border: 1px solid var(--border-dim);
            color: var(--text-main);
            padding: 6px 16px;
            border-radius: 6px;
            font-weight: 700;
            font-size: 12px;
            cursor: pointer;
            transition: all 0.15s;
        }

        .btn-transport.play { background: var(--accent-green); color: #fff; border: none; }
        .btn-transport.play.active { background: var(--accent-red); }

        .sequencer-grid {
            background: var(--bg-card);
            border: 1px solid var(--border-dim);
            border-radius: 8px;
            padding: 16px;
            margin-bottom: 24px;
        }

        .track-row {
            display: grid;
            grid-template-columns: 100px repeat(16, 1fr);
            gap: 6px;
            align-items: center;
            margin-bottom: 10px;
        }

        .track-label {
            font-family: var(--font-mono);
            font-size: 11px;
            font-weight: 700;
            text-transform: uppercase;
            color: var(--text-muted);
        }

        .seq-step {
            aspect-ratio: 1;
            background: var(--bg-sub);
            border: 1px solid var(--border-dim);
            border-radius: 4px;
            cursor: pointer;
            transition: all 0.1s;
        }

        .seq-step.active {
            background: var(--accent-cyan);
            border-color: #38bdf8;
            box-shadow: 0 0 10px rgba(6, 182, 212, 0.4);
        }

        .seq-step.cursor {
            border-color: #fff !important;
            transform: scale(1.08);
        }

        /* Keyboard Keys */
        .piano-keyboard {
            display: flex;
            height: 90px;
            background: var(--bg-card);
            border: 1px solid var(--border-dim);
            border-radius: 8px;
            padding: 8px;
            position: relative;
            user-select: none;
            margin-bottom: 24px;
        }

        .key-white {
            flex: 1;
            background: #f8fafc;
            border: 1px solid #94a3b8;
            border-radius: 0 0 4px 4px;
            cursor: pointer;
            position: relative;
            display: flex;
            align-items: flex-end;
            justify-content: center;
            padding-bottom: 4px;
            font-family: var(--font-mono);
            font-size: 10px;
            color: #0f172a;
            font-weight: 700;
        }

        .key-white:active, .key-white.pressed { background: #38bdf8; }

        .key-black {
            width: 5%;
            height: 55%;
            background: #0f172a;
            position: absolute;
            z-index: 2;
            border-radius: 0 0 3px 3px;
            cursor: pointer;
        }

        .key-black:active, .key-black.pressed { background: var(--accent-purple); }

        /* ============================================================== */
        /* WORKLOAD 2: AETHER WORKSPACE STYLING                           */
        /* ============================================================== */
        .workspace-layout {
            display: grid;
            grid-template-columns: 260px 1fr 340px;
            min-height: calc(100vh - 52px);
            background: var(--border-dim);
            gap: 1px;
        }

        .ws-sidebar {
            background: var(--bg-canvas);
            padding: 16px;
            overflow-y: auto;
        }

        .doc-item {
            padding: 8px 12px;
            border-radius: 6px;
            cursor: pointer;
            font-size: 12px;
            font-weight: 600;
            color: var(--text-muted);
            margin-bottom: 4px;
            display: flex;
            align-items: center;
            gap: 8px;
            transition: all 0.15s;
        }

        .doc-item:hover, .doc-item.active {
            background: var(--bg-card);
            color: #fff;
        }

        .ws-editor-pane {
            background: var(--bg-canvas);
            display: flex;
            flex-direction: column;
        }

        .editor-header {
            height: 48px;
            border-bottom: 1px solid var(--border-dim);
            background: var(--bg-sub);
            padding: 0 20px;
            display: flex;
            align-items: center;
            justify-content: space-between;
        }

        .editor-container {
            display: grid;
            grid-template-columns: 1fr 1fr;
            flex: 1;
            background: var(--border-dim);
            gap: 1px;
        }

        .editor-textarea {
            background: var(--bg-canvas);
            color: var(--text-main);
            font-family: var(--font-mono);
            font-size: 13px;
            line-height: 1.6;
            padding: 24px;
            border: none;
            outline: none;
            resize: none;
            width: 100%;
            height: 100%;
        }

        .preview-pane {
            background: var(--bg-canvas);
            padding: 24px;
            overflow-y: auto;
            line-height: 1.6;
        }

        .preview-pane h1 { font-size: 20px; font-weight: 800; margin-bottom: 14px; letter-spacing: -0.02em; }
        .preview-pane h2 { font-size: 16px; font-weight: 700; margin: 18px 0 10px 0; letter-spacing: -0.01em; }
        .preview-pane p { margin-bottom: 12px; color: #cbd5e1; }
        .preview-pane pre { background: var(--bg-card); border: 1px solid var(--border-dim); padding: 12px; border-radius: 6px; font-family: var(--font-mono); font-size: 12px; margin-bottom: 14px; overflow-x: auto; }
        .preview-pane table { width: 100%; border-collapse: collapse; margin-bottom: 14px; font-size: 12px; }
        .preview-pane th, .preview-pane td { border: 1px solid var(--border-dim); padding: 6px 10px; text-align: left; }
        .preview-pane th { background: var(--bg-sub); }

        .ws-graph-pane {
            background: var(--bg-canvas);
            padding: 16px;
            display: flex;
            flex-direction: column;
        }

        /* ============================================================== */
        /* WORKLOAD 3: AEGIS FLEET OPERATIONS STYLING                     */
        /* ============================================================== */
        .fleet-layout {
            display: grid;
            grid-template-columns: 320px 1fr 340px;
            min-height: calc(100vh - 52px);
            background: var(--border-dim);
            gap: 1px;
        }

        .fleet-sidebar {
            background: var(--bg-canvas);
            padding: 16px;
            overflow-y: auto;
        }

        .unit-card {
            background: var(--bg-card);
            border: 1px solid var(--border-dim);
            border-radius: 8px;
            padding: 12px;
            margin-bottom: 10px;
            cursor: pointer;
            transition: all 0.15s;
        }

        .unit-card:hover, .unit-card.active {
            border-color: var(--accent-cyan);
            background: var(--bg-elevated);
        }

        .unit-top {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 6px;
        }

        .unit-name { font-weight: 700; font-size: 13px; }
        .unit-status {
            font-size: 10px;
            padding: 2px 6px;
            border-radius: 4px;
            font-weight: 700;
            font-family: var(--font-mono);
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
            background: #06080d;
            position: relative;
            height: 100%;
            min-height: 400px;
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
            color: var(--accent-cyan);
            pointer-events: none;
            background: rgba(9, 11, 16, 0.7);
            backdrop-filter: blur(4px);
            padding: 8px 12px;
            border: 1px solid var(--border-dim);
            border-radius: 6px;
        }

        .fleet-controls-pane {
            background: var(--bg-canvas);
            padding: 16px;
            display: flex;
            flex-direction: column;
            gap: 16px;
        }

        .btn-cmd {
            width: 100%;
            padding: 10px;
            border-radius: 6px;
            font-size: 12px;
            font-weight: 700;
            border: 1px solid var(--border-dim);
            background: var(--bg-card);
            color: var(--text-main);
            cursor: pointer;
            transition: all 0.15s;
        }

        .btn-cmd:hover { background: var(--bg-hover); border-color: var(--border-subtle); }
        .btn-cmd.alert { border-color: rgba(244, 63, 94, 0.3); color: var(--accent-red); }
        .btn-cmd.primary { background: var(--accent-cyan); color: #000; border: none; }
    </style>
</head>
<body>

    <!-- Unified Suite Top Navigation -->
    <header class="suite-header">
        <div class="brand-cluster">
            <div class="brand-badge">Ω</div>
            <span>AETHER SUITE</span>
            <span style="font-size: 11px; color: var(--text-faint); font-weight: 600;">ENTERPRISE WORKBENCH OS</span>
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
            <div style="display: flex; align-items: center; gap: 6px;">
                <div class="dot-green"></div>
                <span>SILICON MESH DISTRIBUTED</span>
            </div>
            <span style="color: var(--accent-cyan);" id="hsmBadge">HSM SYNC 0.35ms</span>
        </div>
    </header>

    <!-- ================================================================ -->
    <!-- WORKLOAD 1: VORTEX DAW & MODULAR SYNTHESIZER                     -->
    <!-- ================================================================ -->
    <div class="workload-view active" id="view-daw">
        <div class="daw-grid">
            
            <!-- Left Sidebar: ADSR & Sound Controls -->
            <aside class="daw-sidebar">
                <h3 style="font-size: 13px; font-weight: 800; text-transform: uppercase; color: var(--text-muted); font-family: var(--font-mono);">
                    Modular Synth Engine
                </h3>

                <!-- Waveform Select -->
                <div>
                    <label style="display: block; font-size: 11px; color: var(--text-muted); margin-bottom: 6px; font-family: var(--font-mono);">OSCILLATOR WAVEFORM</label>
                    <select id="synthWaveform" style="width: 100%; background: var(--bg-card); border: 1px solid var(--border-dim); border-radius: 6px; color: #fff; padding: 6px 10px; font-family: var(--font-mono);" onchange="updateSynthParams()">
                        <option value="sawtooth">Sawtooth (Lead)</option>
                        <option value="sine">Sine (Warm)</option>
                        <option value="square">Square (Pulse)</option>
                        <option value="triangle">Triangle (Soft)</option>
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
                        <span id="cutoffVal" style="color: var(--accent-purple);">2400 Hz</span>
                    </div>
                    <input type="range" id="synthCutoff" min="200" max="8000" value="2400" style="width: 100%; accent-color: var(--accent-cyan);" oninput="document.getElementById('cutoffVal').textContent = this.value + ' Hz'; updateSynthParams();">
                </div>

                <!-- Live Oscilloscope Visualizer -->
                <div>
                    <div style="font-size: 11px; color: var(--text-muted); margin-bottom: 6px; font-family: var(--font-mono);">REAL-TIME WAVEFORM</div>
                    <canvas id="scopeCanvas" width="280" height="110" style="background: var(--bg-card); border: 1px solid var(--border-dim); border-radius: 6px; width: 100%;"></canvas>
                </div>

                <!-- Hardware Keyboard Hotkey Pill -->
                <div style="background: var(--bg-card); border: 1px solid var(--border-dim); border-radius: 6px; padding: 10px; font-family: var(--font-mono); font-size: 11px;">
                    <div style="color: var(--accent-green); font-weight: 700; margin-bottom: 4px;">HARDWARE SYNTH HOTKEYS</div>
                    <div style="color: var(--text-muted); font-size: 10px;">Press physical keys Q..U or click virtual piano below to play real-time sound through physical speaker.</div>
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
                            <input type="number" id="dawBpm" min="60" max="200" value="120" style="width: 50px; background: var(--bg-sub); border: 1px solid var(--border-dim); border-radius: 4px; color: #fff; text-align: center; font-family: var(--font-mono); padding: 3px;" onchange="updateBpm(this.value)">
                            <span> BPM</span>
                        </div>
                        <div>STEP: <strong style="color: var(--accent-cyan);" id="stepCounter">01 / 16</strong></div>
                    </div>
                </div>

                <!-- 16-Step Pattern Sequencer -->
                <div class="sequencer-grid">
                    <div class="track-row">
                        <div class="track-label">Kick</div>
                        <div id="row-kick" style="display: contents;"></div>
                    </div>
                    <div class="track-row">
                        <div class="track-label">Snare</div>
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
                <div style="font-family: var(--font-mono); font-size: 11px; color: var(--text-muted); margin-bottom: 8px;">INTERACTIVE SYNTHESIZER KEYBOARD</div>
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
                <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 14px;">
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
                        <span>Words: 412</span> | <span>Saved</span>
                    </div>
                </div>
                <div class="editor-container">
                    <textarea class="editor-textarea" id="docTextarea" oninput="handleDocInput()"></textarea>
                    <div class="preview-pane" id="docPreview"></div>
                </div>
            </main>

            <!-- Knowledge Graph Visualizer -->
            <aside class="ws-graph-pane">
                <h4 style="font-size: 12px; font-weight: 700; text-transform: uppercase; color: var(--text-muted); font-family: var(--font-mono); margin-bottom: 12px;">
                    Knowledge Graph
                </h4>
                <canvas id="graphCanvas" width="308" height="340" style="background: var(--bg-card); border: 1px solid var(--border-dim); border-radius: 6px; width: 100%; height: 340px;"></canvas>
                <div style="margin-top: 14px; font-family: var(--font-mono); font-size: 11px; color: var(--text-muted);">
                    Interactive associative link graph. Nodes represent architectural specifications, protocol models, and hardware satellite tokens.
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
                <div style="font-weight: 700; font-size: 11px; text-transform: uppercase; color: var(--text-muted); font-family: var(--font-mono); margin-bottom: 12px;">
                    Autonomous Vehicles (4)
                </div>
                <div id="fleetUnitsContainer"></div>
            </aside>

            <!-- Geospatial Radar Map -->
            <main class="radar-canvas-container" id="radarContainer">
                <div class="radar-hud-overlay">
                    <div>TACTICAL RADAR VIEW [37.7749 N, 122.4194 W]</div>
                    <div>ACTIVE SECTOR: SAN FRANCISCO BAY</div>
                    <div style="color: var(--accent-green); margin-top: 4px;">TELEMETRY LINK: ENCRYPTED 2.4 GHz</div>
                </div>
                <canvas id="radarCanvas" width="700" height="550" style="width: 100%; height: 100%; cursor: crosshair;" onclick="handleRadarClick(event)"></canvas>
            </main>

            <!-- Mission Command Dispatcher & Avionics -->
            <aside class="fleet-controls-pane">
                <h4 style="font-size: 12px; font-weight: 700; text-transform: uppercase; color: var(--text-muted); font-family: var(--font-mono);">
                    Mission Command Dispatch
                </h4>

                <button class="btn-cmd primary" onclick="sendFleetCmd('ARM_LAUNCH')">ARM / INITIATE PATROL</button>
                <button class="btn-cmd" onclick="sendFleetCmd('HOLD_ORBIT')">HOLD ORBIT / STATION</button>
                <button class="btn-cmd" onclick="sendFleetCmd('RTL')">RETURN TO BASE (RTL)</button>
                <button class="btn-cmd alert" onclick="sendFleetCmd('EMERGENCY_HALT')">EMERGENCY HALT</button>

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
        // State
        let currentWorkload = 'daw';
        let audioCtx = null;
        let isPlaying = false;
        let synthTracks = {};
        let currentDocId = 'doc_arch';
        let currentSelectedUnit = 'ALPHA-1';

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
        // WORKLOAD 1: VORTEX DAW LOGIC
        // ===================================================================
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
                    <div class="seq-step ${val > 0 ? 'active' : ''}" id="step-${trk}-${idx}" onclick="toggleStep('${trk}', ${idx})"></div>
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

        function playWebTone(freq, duration = 0.15) {
            initAudio();
            try {
                const osc = audioCtx.createOscillator();
                const gain = audioCtx.createGain();
                const cutoff = parseFloat(document.getElementById('synthCutoff')?.value || 2400);

                osc.type = document.getElementById('synthWaveform')?.value || 'sawtooth';
                osc.frequency.setValueAtTime(freq, audioCtx.currentTime);

                gain.gain.setValueAtTime(0.3, audioCtx.currentTime);
                gain.gain.exponentialRampToValueAtTime(0.001, audioCtx.currentTime + duration);

                osc.connect(gain);
                gain.connect(audioCtx.destination);

                osc.start();
                osc.stop(audioCtx.currentTime + duration);
            } catch (e) {}

            // Mirror note to physical speaker
            fetch('/api/v1/synth/trigger', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ freq: Math.round(freq), duration_ms: Math.round(duration * 1000) })
            });
        }

        function renderPiano() {
            const kb = document.getElementById('pianoKeyboard');
            const notes = [
                { name: 'C4', f: 261.63, b: false }, { name: 'C#4', f: 277.18, b: true, l: '5%' },
                { name: 'D4', f: 293.66, b: false }, { name: 'D#4', f: 311.13, b: true, l: '12%' },
                { name: 'E4', f: 329.63, b: false },
                { name: 'F4', f: 349.23, b: false }, { name: 'F#4', f: 369.99, b: true, l: '26%' },
                { name: 'G4', f: 392.00, b: false }, { name: 'G#4', f: 415.30, b: true, l: '33%' },
                { name: 'A4', f: 440.00, b: false }, { name: 'A#4', f: 466.16, b: true, l: '40%' },
                { name: 'B4', f: 493.88, b: false },
                { name: 'C5', f: 523.25, b: false }, { name: 'C#5', f: 554.37, b: true, l: '54%' },
                { name: 'D5', f: 587.33, b: false }, { name: 'D#5', f: 622.25, b: true, l: '61%' },
                { name: 'E5', f: 659.25, b: false },
                { name: 'F5', f: 698.46, b: false }, { name: 'F#5', f: 739.99, b: true, l: '75%' },
                { name: 'G5', f: 783.99, b: false }, { name: 'G#5', f: 830.61, b: true, l: '82%' },
                { name: 'A5', f: 880.00, b: false }, { name: 'A#5', f: 932.33, b: true, l: '89%' },
                { name: 'B5', f: 987.77, b: false },
            ];

            kb.innerHTML = notes.filter(n => !n.b).map(n => `
                <div class="key-white" onclick="playWebTone(${n.f})">${n.name}</div>
            `).join('');

            notes.filter(n => n.b).forEach(n => {
                const el = document.createElement('div');
                el.className = 'key-black';
                el.style.left = n.l;
                el.onclick = () => playWebTone(n.f);
                kb.appendChild(el);
            });
        }

        // Live Scope Visualizer
        function startVisualizer() {
            const canvas = document.getElementById('scopeCanvas');
            if (!canvas) return;
            const ctx = canvas.getContext('2d');
            let phase = 0;

            function draw() {
                ctx.fillStyle = '#151923';
                ctx.fillRect(0, 0, canvas.width, canvas.height);

                ctx.strokeStyle = isPlaying ? '#06b6d4' : '#475569';
                ctx.lineWidth = 2;
                ctx.beginPath();

                for (let x = 0; x < canvas.width; x++) {
                    const freq = isPlaying ? 0.05 : 0.02;
                    const amp = isPlaying ? 28 : 6;
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
                            <span style="font-size: 14px;">📄</span>
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
            // High-speed lightweight markdown parser
            let html = md
                .replace(/^# (.*$)/gim, '<h1>$1</h1>')
                .replace(/^## (.*$)/gim, '<h2>$1</h2>')
                .replace(/^### (.*$)/gim, '<h3>$1</h3>')
                .replace(/^\\*\\*\\s*(.*$)/gim, '<p><strong>$1</strong></p>')
                .replace(/```([\\s\\S]*?)```/gim, '<pre><code>$1</code></pre>')
                .replace(/\\n\\n/gim, '<p></p>');
            document.getElementById('docPreview').innerHTML = html;

            const words = md.trim().split(/\\s+/).length;
            document.getElementById('docMetrics').textContent = `Words: ${words} | Saved`;
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
                body: JSON.stringify({ id, title: 'Untitled Document', content: '# Untitled Document\\n\\nStart typing...' })
            }).then(() => {
                currentDocId = id;
                fetchDocs();
            });
        }

        function drawKnowledgeGraph() {
            const canvas = document.getElementById('graphCanvas');
            if (!canvas) return;
            const ctx = canvas.getContext('2d');
            const w = canvas.width;
            const h = canvas.height;

            const nodes = [
                { id: 1, x: w * 0.35, y: h * 0.35, r: 8, label: 'Architecture', color: '#6366f1' },
                { id: 2, x: w * 0.65, y: h * 0.3, r: 7, label: 'Protocols', color: '#8b5cf6' },
                { id: 3, x: w * 0.5, y: h * 0.7, r: 9, label: 'State Engine', color: '#06b6d4' },
                { id: 4, x: w * 0.2, y: h * 0.65, r: 6, label: 'Hardware Keyring', color: '#10b981' }
            ];

            const links = [
                [nodes[0], nodes[1]],
                [nodes[0], nodes[2]],
                [nodes[1], nodes[2]],
                [nodes[0], nodes[3]]
            ];

            ctx.fillStyle = '#151923';
            ctx.fillRect(0, 0, w, h);

            // Draw links
            ctx.strokeStyle = 'rgba(255, 255, 255, 0.12)';
            ctx.lineWidth = 1.5;
            links.forEach(([n1, n2]) => {
                ctx.beginPath();
                ctx.moveTo(n1.x, n1.y);
                ctx.lineTo(n2.x, n2.y);
                ctx.stroke();
            });

            // Draw nodes
            nodes.forEach(n => {
                ctx.beginPath();
                ctx.arc(n.x, n.y, n.r, 0, Math.PI * 2);
                ctx.fillStyle = n.color;
                ctx.fill();

                ctx.fillStyle = '#94a3b8';
                ctx.font = '10px JetBrains Mono';
                ctx.fillText(n.label, n.x + 12, n.y + 3);
            });
        }

        // ===================================================================
        // WORKLOAD 3: AEGIS FLEET OPERATIONS LOGIC
        // ===================================================================
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

                ctx.fillStyle = '#06080d';
                ctx.fillRect(0, 0, w, h);

                // Grid circles
                ctx.strokeStyle = 'rgba(6, 182, 212, 0.15)';
                ctx.lineWidth = 1;
                [60, 120, 180, 240].forEach(r => {
                    ctx.beginPath();
                    ctx.arc(cx, cy, r, 0, Math.PI * 2);
                    ctx.stroke();
                });

                // Crosshairs
                ctx.beginPath();
                ctx.moveTo(cx, 0); ctx.lineTo(cx, h);
                ctx.moveTo(0, cy); ctx.lineTo(w, cy);
                ctx.stroke();

                // Sweep line
                ctx.save();
                ctx.translate(cx, cy);
                ctx.rotate(sweepAngle);
                ctx.strokeStyle = 'rgba(6, 182, 212, 0.6)';
                ctx.lineWidth = 2;
                ctx.beginPath();
                ctx.moveTo(0, 0);
                ctx.lineTo(240, 0);
                ctx.stroke();
                ctx.restore();

                // Blips
                const blips = [
                    { x: cx + 45, y: cy - 70, label: 'ALPHA-1', c: '#10b981' },
                    { x: cx + 110, y: cy + 40, label: 'BRAVO-2', c: '#38bdf8' },
                    { x: cx - 80, y: cy + 90, label: 'TITAN-4', c: '#f59e0b' },
                    { x: cx - 140, y: cy - 130, label: 'VALKYRIE-9', c: '#8b5cf6' }
                ];

                blips.forEach(b => {
                    ctx.beginPath();
                    ctx.arc(b.x, b.y, 4, 0, Math.PI * 2);
                    ctx.fillStyle = b.c;
                    ctx.fill();

                    ctx.fillStyle = '#cbd5e1';
                    ctx.font = '10px JetBrains Mono';
                    ctx.fillText(b.label, b.x + 8, b.y + 3);
                });

                sweepAngle += 0.03;
                requestAnimationFrame(frame);
            }
            frame();
        }

        // Global Periodic Poller
        setInterval(() => {
            if (currentWorkload === 'fleet') fetchFleet();
        }, 1500);

        // Auto-initialize current tab
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
                    # Tactical chime
                    send_serial_cmd(f"K32_CMD:BEEP:1200:60:{SYNTH_STATE['volume']}")
            return (200, 'application/json', b'{"status":"OK"}')
        except Exception as e:
            return (500, 'application/json', json.dumps({"error": str(e)}).encode('utf-8'))

    if path_clean == '/api/v1/fleet/waypoint' and method == 'POST':
        try:
            d = json.loads(body.decode('utf-8') if isinstance(body, bytes) else body)
            unit_id = d.get('unit', 'ALPHA-1')
            with suite_lock:
                MISSION_EVENTS.append({
                    "time": time.time(),
                    "unit": unit_id,
                    "level": "INFO",
                    "msg": f"Target waypoint registered at coordinates."
                })
                if send_serial_cmd:
                    send_serial_cmd(f"K32_CMD:DISPLAY:WAYPOINT: {unit_id}")
            return (200, 'application/json', b'{"status":"OK"}')
        except Exception as e:
            return (500, 'application/json', json.dumps({"error": str(e)}).encode('utf-8'))

    # Backward compatibility with existing hardware endpoints
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
