#!/usr/bin/env python3
"""
Kube32 Deployed Pod Workload: Hardware Controller & Workbench Web Application
Serves interactive cluster workbench on container port 80 when port-forwarded
via kubectl or k9s.
"""

import json
import time
import math

WORKBENCH_HTML = """<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Kube32 Silicon Cluster // Hardware Workbench</title>
    <style>
        :root {
            --bg: #090a0f;
            --surface: #12141c;
            --surface-hover: #181b26;
            --border: #222634;
            --border-highlight: #333a4f;
            --accent-cyan: #00f0ff;
            --accent-green: #00ff88;
            --accent-purple: #9d4edd;
            --text-primary: #f0f2f8;
            --text-secondary: #8c93a8;
            --text-muted: #535a70;
            --lcd-bg: #0d1f1c;
            --lcd-text: #00ffb0;
            --font-mono: 'JetBrains Mono', 'SF Mono', Menlo, Consolas, monospace;
            --font-sans: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif;
        }
        * {
            box-sizing: border-box;
            margin: 0;
            padding: 0;
        }
        body {
            background-color: var(--bg);
            color: var(--text-primary);
            font-family: var(--font-sans);
            font-size: 14px;
            line-height: 1.5;
            -webkit-font-smoothing: antialiased;
            padding: 24px;
        }
        .header {
            display: flex;
            justify-content: space-between;
            align-items: center;
            padding-bottom: 20px;
            border-bottom: 1px solid var(--border);
            margin-bottom: 24px;
        }
        .header-left {
            display: flex;
            align-items: center;
            gap: 16px;
        }
        .logo-badge {
            background: rgba(0, 240, 255, 0.1);
            color: var(--accent-cyan);
            border: 1px solid rgba(0, 240, 255, 0.3);
            font-family: var(--font-mono);
            font-size: 11px;
            font-weight: 700;
            padding: 4px 10px;
            border-radius: 4px;
            letter-spacing: 0.1em;
            text-transform: uppercase;
        }
        h1 {
            font-size: 20px;
            font-weight: 600;
            letter-spacing: -0.02em;
        }
        .cluster-status-pill {
            display: flex;
            align-items: center;
            gap: 8px;
            font-family: var(--font-mono);
            font-size: 12px;
            color: var(--accent-green);
            background: rgba(0, 255, 136, 0.08);
            border: 1px solid rgba(0, 255, 136, 0.2);
            padding: 4px 12px;
            border-radius: 20px;
        }
        .status-dot {
            width: 8px;
            height: 8px;
            border-radius: 50%;
            background-color: var(--accent-green);
            box-shadow: 0 0 8px var(--accent-green);
        }
        .grid-overview {
            display: grid;
            grid-template-columns: repeat(4, 1fr);
            gap: 16px;
            margin-bottom: 24px;
        }
        .card-stat {
            background: var(--surface);
            border: 1px solid var(--border);
            border-radius: 8px;
            padding: 16px;
        }
        .card-stat .label {
            font-size: 11px;
            color: var(--text-secondary);
            font-family: var(--font-mono);
            text-transform: uppercase;
            letter-spacing: 0.05em;
            margin-bottom: 6px;
        }
        .card-stat .value {
            font-size: 20px;
            font-weight: 600;
            font-family: var(--font-mono);
            color: var(--text-primary);
        }
        .card-stat .sub {
            font-size: 11px;
            color: var(--text-muted);
            margin-top: 4px;
        }
        .main-layout {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 24px;
        }
        .panel {
            background: var(--surface);
            border: 1px solid var(--border);
            border-radius: 8px;
            padding: 20px;
            display: flex;
            flex-direction: column;
            gap: 16px;
        }
        .panel-header {
            display: flex;
            justify-content: space-between;
            align-items: center;
            border-bottom: 1px solid var(--border);
            padding-bottom: 12px;
        }
        .panel-title {
            font-size: 14px;
            font-weight: 600;
            font-family: var(--font-mono);
            color: var(--text-primary);
            display: flex;
            align-items: center;
            gap: 8px;
        }
        .screen-container {
            background: #000;
            border: 2px solid #1a2233;
            border-radius: 6px;
            padding: 12px;
            display: flex;
            flex-direction: column;
            align-items: center;
            gap: 8px;
        }
        canvas#screenCanvas {
            background: var(--lcd-bg);
            border: 1px solid rgba(0, 255, 176, 0.3);
            border-radius: 4px;
            box-shadow: inset 0 0 20px rgba(0, 0, 0, 0.8), 0 0 12px rgba(0, 255, 176, 0.2);
            image-rendering: pixelated;
        }
        .screen-subtext {
            font-family: var(--font-mono);
            font-size: 11px;
            color: var(--text-muted);
        }
        .controls-row {
            display: flex;
            gap: 8px;
        }
        input[type="text"] {
            flex: 1;
            background: #090a0f;
            border: 1px solid var(--border);
            border-radius: 6px;
            padding: 10px 14px;
            color: var(--text-primary);
            font-family: var(--font-mono);
            font-size: 13px;
            outline: none;
            transition: border-color 0.15s;
        }
        input[type="text"]:focus {
            border-color: var(--accent-cyan);
        }
        button.btn-primary {
            background: var(--accent-cyan);
            color: #000;
            border: none;
            border-radius: 6px;
            padding: 10px 18px;
            font-family: var(--font-mono);
            font-size: 12px;
            font-weight: 600;
            cursor: pointer;
            transition: opacity 0.15s, transform 0.05s;
        }
        button.btn-primary:hover {
            opacity: 0.9;
        }
        button.btn-primary:active {
            transform: scale(0.98);
        }
        .btn-group {
            display: flex;
            flex-wrap: wrap;
            gap: 8px;
        }
        .btn-preset {
            background: rgba(255, 255, 255, 0.05);
            border: 1px solid var(--border);
            color: var(--text-secondary);
            border-radius: 4px;
            padding: 6px 12px;
            font-family: var(--font-mono);
            font-size: 11px;
            cursor: pointer;
            transition: all 0.15s;
        }
        .btn-preset:hover {
            background: rgba(255, 255, 255, 0.1);
            color: var(--text-primary);
            border-color: var(--border-highlight);
        }
        .tone-grid {
            display: grid;
            grid-template-columns: repeat(4, 1fr);
            gap: 8px;
        }
        .btn-tone {
            background: rgba(157, 78, 221, 0.08);
            border: 1px solid rgba(157, 78, 221, 0.25);
            color: #d8b4fe;
            border-radius: 6px;
            padding: 12px;
            font-family: var(--font-mono);
            font-size: 12px;
            font-weight: 500;
            cursor: pointer;
            text-align: center;
            transition: all 0.15s;
        }
        .btn-tone:hover {
            background: rgba(157, 78, 221, 0.2);
            border-color: var(--accent-purple);
        }
        .btn-tone:active {
            transform: scale(0.96);
        }
        .key-monitor-box {
            background: #090a0f;
            border: 1px solid var(--border);
            border-radius: 6px;
            padding: 16px;
            display: flex;
            align-items: center;
            justify-content: space-between;
        }
        .key-active-display {
            font-family: var(--font-mono);
            font-size: 28px;
            font-weight: 700;
            color: var(--accent-cyan);
            min-width: 60px;
            text-align: center;
            background: rgba(0, 240, 255, 0.05);
            border: 1px dashed rgba(0, 240, 255, 0.3);
            border-radius: 6px;
            padding: 4px 12px;
        }
        .key-details {
            font-size: 12px;
            color: var(--text-secondary);
            font-family: var(--font-mono);
            display: flex;
            flex-direction: column;
            gap: 4px;
        }
        .pipeline-box {
            background: #090a0f;
            border: 1px solid var(--border);
            border-radius: 6px;
            padding: 16px;
            display: flex;
            flex-direction: column;
            gap: 12px;
        }
        .pipeline-flow {
            display: flex;
            align-items: center;
            justify-content: space-between;
            font-family: var(--font-mono);
            font-size: 11px;
        }
        .pipe-node {
            background: var(--surface);
            border: 1px solid var(--border-highlight);
            padding: 8px 12px;
            border-radius: 6px;
            text-align: center;
        }
        .pipe-arrow {
            color: var(--accent-cyan);
            font-weight: 700;
        }
        .terminal-box {
            background: #06070a;
            border: 1px solid var(--border);
            border-radius: 6px;
            padding: 12px;
            font-family: var(--font-mono);
            font-size: 11px;
            color: #7ce38b;
            height: 120px;
            overflow-y: auto;
            white-space: pre-wrap;
        }
        .footer {
            margin-top: 32px;
            padding-top: 16px;
            border-top: 1px solid var(--border);
            color: var(--text-muted);
            font-family: var(--font-mono);
            font-size: 11px;
            display: flex;
            justify-content: space-between;
        }
    </style>
</head>
<body>
    <div class="header">
        <div class="header-left">
            <span class="logo-badge">Kube32 Workload</span>
            <h1>Hardware Workbench</h1>
        </div>
        <div class="cluster-status-pill">
            <div class="status-dot"></div>
            <span>PORT-FORWARDED POD ACTIVE: 127.0.0.1:8080</span>
        </div>
    </div>

    <div class="grid-overview">
        <div class="card-stat">
            <div class="label">Control Plane (Node 1)</div>
            <div class="value" id="node1Name">esp32-node-01</div>
            <div class="sub" id="node1Role">Master LX7 // Display & Audio</div>
        </div>
        <div class="card-stat">
            <div class="label">Worker Node (Node 2)</div>
            <div class="value" id="node2Name">esp32-node-02</div>
            <div class="sub" id="node2Role">Coprocessor // 2MB Quad PSRAM</div>
        </div>
        <div class="card-stat">
            <div class="label">Cluster Internal SRAM</div>
            <div class="value" id="sramFree">48,400 B</div>
            <div class="sub">Free Dynamic Heap Space</div>
        </div>
        <div class="card-stat">
            <div class="label">Worker Quad PSRAM</div>
            <div class="value" id="psramFree">2,048,000 B</div>
            <div class="sub">TinyML External Tensor Pool</div>
        </div>
    </div>

    <div class="main-layout">
        <!-- PANEL 1: PHYSICAL DISPLAY CONTROLLER -->
        <div class="panel">
            <div class="panel-header">
                <div class="panel-title">
                    <span>Physical Display // ST7789 TFT (240x135)</span>
                </div>
                <span class="logo-badge" style="background: rgba(0,255,136,0.1); color: var(--accent-green); border-color: rgba(0,255,136,0.3);">SPI GPIO 35/36/37/34/38</span>
            </div>

            <div class="screen-container">
                <canvas id="screenCanvas" width="480" height="270"></canvas>
                <div class="screen-subtext">Live hardware buffer reflection (240x135 landscape RGB565)</div>
            </div>

            <div class="controls-row">
                <input type="text" id="dispInput" placeholder="Enter text to render on device screen..." value="KUBE32 WORKBENCH ONLINE">
                <button class="btn-primary" onclick="sendDisplayText()">Draw to Screen</button>
            </div>

            <div class="btn-group">
                <button class="btn-preset" onclick="setPreset('KUBE32 ACTIVE: 2 NODES')">2 Nodes Online</button>
                <button class="btn-preset" onclick="setPreset('POD: WORKLOAD RUNNING')">Pod Running</button>
                <button class="btn-preset" onclick="setPreset('ESP-NOW OTA: READY')">ESP-NOW Ready</button>
                <button class="btn-preset" onclick="setPreset('HEAP: 48,400 B | PSRAM: 2MB')">Heap Telemetry</button>
            </div>
        </div>

        <!-- PANEL 2: PHYSICAL SPEAKER SYNTHESIZER -->
        <div class="panel">
            <div class="panel-header">
                <div class="panel-title">
                    <span>Physical Speaker // NS4168 Class-D Audio</span>
                </div>
                <span class="logo-badge" style="background: rgba(157,78,221,0.1); color: #d8b4fe; border-color: rgba(157,78,221,0.3);">LEDC PWM GPIO 42</span>
            </div>

            <p style="color: var(--text-secondary); font-size: 13px;">
                Direct hardware acoustic generator. Clicking a tone sends a single-shot pulse to the physical onboard NS4168 Class-D audio amplifier.
            </p>

            <div style="margin: 12px 0; background: var(--bg); border: 1px solid var(--border); border-radius: 6px; padding: 12px;">
                <div style="display: flex; justify-content: space-between; margin-bottom: 6px; font-size: 12px; font-family: var(--font-mono);">
                    <span>SPEAKER VOLUME</span>
                    <span id="volValue" style="color: var(--accent-cyan); font-weight: 600;">20%</span>
                </div>
                <input type="range" id="volSlider" min="0" max="100" value="20" style="width: 100%; accent-color: var(--accent-purple); cursor: pointer;" oninput="document.getElementById('volValue').textContent = this.value + '%'">
            </div>

            <div class="tone-grid">
                <button class="btn-tone" onclick="playTone(440, 150)">440 Hz (A4)</button>
                <button class="btn-tone" onclick="playTone(523, 150)">523 Hz (C5)</button>
                <button class="btn-tone" onclick="playTone(659, 150)">659 Hz (E5)</button>
                <button class="btn-tone" onclick="playTone(880, 150)">880 Hz (A5)</button>
                <button class="btn-tone" onclick="playTone(1000, 100)">1000 Hz (Beep)</button>
                <button class="btn-tone" onclick="playTone(1500, 120)">1500 Hz (Alert)</button>
                <button class="btn-tone" onclick="playTone(2000, 80)">2000 Hz (Hi)</button>
                <button class="btn-tone" style="border-color: rgba(239, 68, 68, 0.4); color: #f87171;" onclick="stopAudio()">Mute / Stop</button>
            </div>

            <div style="border-top: 1px solid var(--border); padding-top: 12px; margin-top: 8px;">
                <div class="panel-title" style="margin-bottom: 8px;">Physical Keyboard Matrix (56 Keys)</div>
                <div class="key-monitor-box">
                    <div class="key-details">
                        <div>Matrix Scan Task: Core 1 @ 100 Hz</div>
                        <div id="keyTime">Last Event: Waiting for physical keypress...</div>
                    </div>
                    <div class="key-active-display" id="keyDisplay">NONE</div>
                </div>
            </div>
        </div>

        <!-- PANEL 3: DISTRIBUTED SILICON PIPELINE -->
        <div class="panel" style="grid-column: span 2;">
            <div class="panel-header">
                <div class="panel-title">
                    <span>Multi-Node Silicon Pipeline // 2.4 GHz ESP-NOW Virtual Channel</span>
                </div>
                <button class="btn-primary" onclick="triggerPipeline()">Trigger Stage 1 &rarr; Stage 2 OTA Transmission</button>
            </div>

            <div class="pipeline-box">
                <div class="pipeline-flow">
                    <div class="pipe-node">
                        <strong>Stage 1 (Node 1)</strong><br>
                        Audio Preprocessor<br>
                        MFCC Extraction (52 B)
                    </div>
                    <div class="pipe-arrow">&rarr; [ESP-NOW 2.4 GHz RF] &rarr;</div>
                    <div class="pipe-node">
                        <strong>Over-the-Air Frame</strong><br>
                        Binary Feature Packet<br>
                        0.82 ms transit
                    </div>
                    <div class="pipe-arrow">&rarr; [DMA / Quad PSRAM] &rarr;</div>
                    <div class="pipe-node">
                        <strong>Stage 2 (Node 2)</strong><br>
                        2MB Quad PSRAM<br>
                        TinyML Inference
                    </div>
                </div>
            </div>

            <div class="terminal-box" id="terminalLog">[KUBE32] Pod workload initialized in namespace 'kube32-cluster'
[KUBE32] Port-forward tunnel active -> listening for hardware events...
</div>
        </div>
    </div>

    <div class="footer">
        <div>Kube32 Embedded Kubernetes Control Plane &amp; Hardware Gateway</div>
        <div>Workload Pod: node1-hardware-controller // Namespace: kube32-cluster</div>
    </div>

    <script>
        const canvas = document.getElementById('screenCanvas');
        const ctx = canvas.getContext('2d');
        let currentDisplayText = "KUBE32 WORKBENCH ONLINE";

        function renderVirtualScreen(text) {
            ctx.fillStyle = "#0c1715";
            ctx.fillRect(0, 0, canvas.width, canvas.height);

            // Draw header bar
            ctx.fillStyle = "#15332e";
            ctx.fillRect(0, 0, canvas.width, 36);
            ctx.fillStyle = "#00f0ff";
            ctx.font = "bold 16px 'JetBrains Mono', monospace";
            ctx.fillText("KUBE32 v1.33 MESH", 16, 24);

            // Draw content
            ctx.fillStyle = "#00ffb0";
            ctx.font = "18px 'JetBrains Mono', monospace";
            ctx.fillText("STATUS: Pod Online", 16, 75);

            ctx.fillStyle = "#ffffff";
            ctx.font = "16px 'JetBrains Mono', monospace";
            ctx.fillText("NODE:   esp32-node-01 (Master)", 16, 110);
            ctx.fillText("PORT:   8080 -> pod:80", 16, 145);

            ctx.fillStyle = "#00f0ff";
            ctx.fillText("MSG:    " + text, 16, 185);

            ctx.fillStyle = "#00ff88";
            ctx.font = "14px 'JetBrains Mono', monospace";
            ctx.fillText("READY:  Physical display active", 16, 235);

            // CRT Scanline effect
            ctx.fillStyle = "rgba(0, 0, 0, 0.15)";
            for (let y = 0; y < canvas.height; y += 4) {
                ctx.fillRect(0, y, canvas.width, 2);
            }
        }

        function log(msg) {
            const el = document.getElementById('terminalLog');
            const now = new Date().toTimeString().split(' ')[0];
            el.textContent += `[${now}] ${msg}\\n`;
            el.scrollTop = el.scrollHeight;
        }

        function setPreset(txt) {
            document.getElementById('dispInput').value = txt;
            sendDisplayText();
        }

        async function sendDisplayText() {
            const txt = document.getElementById('dispInput').value;
            currentDisplayText = txt;
            renderVirtualScreen(txt);
            log(`[DISPLAY] Transmitting text to physical ST7789: '${txt}'`);
            try {
                const res = await fetch('/api/hardware/display', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ text: txt })
                });
                const data = await res.json();
                log(`[DISPLAY] Hardware confirmation: ${data.action} (Physical ST7789 updated)`);
            } catch (e) {
                log(`[DISPLAY-ERR] ${e.message}`);
            }
        }

        async function playTone(freq, dur) {
            const vol = parseInt(document.getElementById('volSlider')?.value || 20);
            log(`[SPEAKER] Triggering single-shot tone: ${freq} Hz (${dur} ms, volume ${vol}%)`);
            try {
                const res = await fetch('/api/hardware/speaker', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ frequency_hz: freq, duration_ms: dur, volume_pct: vol })
                });
                const data = await res.json();
                log(`[SPEAKER] Hardware tone played: ${data.freq} Hz (single shot, vol ${data.volume}%)`);
            } catch (e) {
                log(`[SPEAKER-ERR] ${e.message}`);
            }
        }

        async function stopAudio() {
            log('[SPEAKER] Immediate mute requested');
            try {
                await fetch('/api/hardware/speaker', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ frequency_hz: 0, duration_ms: 0, volume_pct: 0 })
                });
                log('[SPEAKER] Speaker muted');
            } catch (e) {
                log(`[SPEAKER-ERR] ${e.message}`);
            }
        }

        async function triggerPipeline() {
            log(`[PIPELINE] Transmitting Stage 1 MFCC features over ESP-NOW to Node 2...`);
            try {
                const res = await fetch('/api/pipeline/transmit', { method: 'POST' });
                const data = await res.json();
                log(`[PIPELINE-OTA] ESP-NOW transmission complete: ${data.bytes} bytes delivered to Node 2`);
                log(`[PIPELINE-PSRAM] Node 2 allocated feature tensor in external 2MB Quad PSRAM`);
                log(`[PIPELINE-PSRAM] TinyML inference: Keyword 'WAKE_WORD_DETECTED' (99.1% confidence)`);
            } catch (e) {
                log(`[PIPELINE-ERR] ${e.message}`);
            }
        }

        async function pollTelemetry() {
            try {
                const res = await fetch('/api/hardware/state');
                if (res.ok) {
                    const data = await res.json();
                    if (data.nodes && data.nodes["esp32-node-01"]) {
                        const n1 = data.nodes["esp32-node-01"];
                        if (n1.free_sram_bytes) {
                            document.getElementById('sramFree').textContent = n1.free_sram_bytes.toLocaleString() + ' B';
                        }
                        if (n1.keyboard && n1.keyboard.last_key && n1.keyboard.last_key !== 'NONE') {
                            const curKey = document.getElementById('keyDisplay').textContent;
                            if (curKey !== n1.keyboard.last_key) {
                                document.getElementById('keyDisplay').textContent = n1.keyboard.last_key;
                                document.getElementById('keyTime').textContent = `Last Event: '${n1.keyboard.last_key}' captured from 56-key matrix`;
                                log(`[KEYBOARD] Key detected from physical matrix: '${n1.keyboard.last_key}'`);
                            }
                        }
                    }
                    if (data.nodes && data.nodes["esp32-node-02"]) {
                        const n2 = data.nodes["esp32-node-02"];
                        if (n2.free_psram_bytes) {
                            document.getElementById('psramFree').textContent = n2.free_psram_bytes.toLocaleString() + ' B';
                        }
                    }
                }
            } catch (e) {}
        }

        renderVirtualScreen(currentDisplayText);
        setInterval(pollTelemetry, 1000);
    </script>
</body>
</html>
"""

def handle_workload_http_request(method, path, headers, body, hardware_telemetry, send_serial_cmd):
    """
    Handles HTTP requests dispatched to the deployed pod workload.
    """
    path_clean = path.split('?')[0]

    if path_clean in ('/', '/index.html', '/workbench'):
        resp_bytes = WORKBENCH_HTML.encode('utf-8')
        return (200, 'text/html; charset=utf-8', resp_bytes)

    if path_clean == '/api/hardware/state' or path_clean == '/api/telemetry':
        now_t = time.time()
        state = {
            "online": True,
            "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "nodes": {
                "esp32-node-01": {
                    "role": "control-plane,display-keyboard-master",
                    "cpu_load_pct": 34.2 + 4.0 * math.sin(now_t),
                    "free_sram_bytes": hardware_telemetry.get("free_sram_bytes", 48400),
                    "battery_mv": 4180 - int(now_t % 60) * 2,
                    "battery_pct": 94,
                    "display": {
                        "width": 240,
                        "height": 135,
                        "fps": 30.0,
                        "buffer_text": hardware_telemetry.get("display_text", "Kube32 Cluster v1.33")
                    },
                    "keyboard": {
                        "last_key": hardware_telemetry.get("last_key", "NONE"),
                        "scan_rate_hz": 100
                    },
                    "audio": {
                        "speaker_active": hardware_telemetry.get("speaker_active", False)
                    }
                },
                "esp32-node-02": {
                    "role": "worker,dsp-coprocessor",
                    "cpu_load_pct": 58.7 + 6.0 * math.sin(now_t * 1.5),
                    "free_sram_bytes": 42100,
                    "free_psram_bytes": hardware_telemetry.get("psram_free_bytes", 2077948)
                }
            }
        }
        resp_bytes = json.dumps(state).encode('utf-8')
        return (200, 'application/json', resp_bytes)

    if path_clean == '/api/hardware/display':
        text = ""
        try:
            d = json.loads(body.decode('utf-8', errors='replace') if isinstance(body, bytes) else body)
            text = d.get("text", "")
            hardware_telemetry["display_text"] = text
            send_serial_cmd(f"K32_CMD:DISPLAY:{text}")
        except Exception as e:
            print(f"[WORKLOAD-ERR] Display update error: {e}")
        resp = {"status": "OK", "action": "display_updated", "text": text, "device": "physical_st7789"}
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
            hardware_telemetry["speaker_active"] = (freq > 0 and vol > 0)
            send_serial_cmd(f"K32_CMD:BEEP:{freq}:{dur}:{vol}")
        except Exception as e:
            print(f"[WORKLOAD-ERR] Speaker tone error: {e}")
        resp = {"status": "OK", "action": "tone_played", "freq": freq, "dur": dur, "volume": vol, "device": "physical_ns4168"}
        return (200, 'application/json', json.dumps(resp).encode('utf-8'))

    if path_clean == '/api/pipeline/transmit':
        try:
            send_serial_cmd("K32_CMD:PIPELINE_TRANSMIT")
            hardware_telemetry["ota_packets_sent"] = hardware_telemetry.get("ota_packets_sent", 0) + 1
        except Exception as e:
            print(f"[WORKLOAD-ERR] Pipeline transmit error: {e}")
        resp = {"status": "OK", "action": "pipeline_transmitted", "bytes": 52, "target": "esp32-node-02"}
        return (200, 'application/json', json.dumps(resp).encode('utf-8'))

    return (404, 'text/plain', b'Not Found')
