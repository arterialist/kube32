#pragma once

#include <string_view>

namespace kube32::api {

inline constexpr std::string_view EMBEDDED_WORKBENCH_HTML = R"rawhtml(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Kube32 Cluster Hardware Workbench</title>
  <style>
    :root {
      --bg: #0c0b10;
      --surface: #14131a;
      --surface-low: #1c1a24;
      --primary: #a78bfa;
      --primary-dim: #7c3aed;
      --on-surface: #f1f0f7;
      --outline: #2e2b3d;
      --success: #34d399;
      --warning: #fbbf24;
      --cyan: #38bdf8;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      background: var(--bg);
      color: var(--on-surface);
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
      padding: 20px;
      line-height: 1.5;
    }
    .header {
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding-bottom: 16px;
      margin-bottom: 24px;
      border-bottom: 1px solid var(--outline);
    }
    .title-group h1 {
      font-size: 22px;
      font-weight: 700;
      letter-spacing: -0.5px;
      color: #fff;
    }
    .title-group p {
      font-size: 13px;
      color: #9ca3af;
    }
    .pill {
      display: inline-flex;
      align-items: center;
      gap: 6px;
      background: rgba(52, 211, 153, 0.12);
      color: var(--success);
      padding: 6px 12px;
      border-radius: 999px;
      font-size: 12px;
      font-weight: 600;
      border: 1px solid rgba(52, 211, 153, 0.3);
    }
    .pill-dot {
      width: 7px;
      height: 7px;
      background: var(--success);
      border-radius: 50%;
      box-shadow: 0 0 8px var(--success);
    }
    .grid {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 20px;
    }
    @media (max-width: 900px) {
      .grid { grid-template-columns: 1fr; }
    }
    .card {
      background: var(--surface);
      border: 1px solid var(--outline);
      border-radius: 12px;
      padding: 20px;
      display: flex;
      flex-direction: column;
      gap: 16px;
    }
    .card-title {
      font-size: 15px;
      font-weight: 600;
      color: #e5e7eb;
      display: flex;
      align-items: center;
      justify-content: space-between;
    }
    .screen-bezel {
      background: #000;
      border: 3px solid #27272a;
      border-radius: 8px;
      padding: 6px;
      display: flex;
      justify-content: center;
      box-shadow: inset 0 0 10px rgba(0,0,0,0.8);
    }
    #screen-canvas {
      width: 100%;
      max-width: 480px;
      height: 270px;
      image-rendering: pixelated;
      background: #141218;
      border-radius: 4px;
    }
    .input-row {
      display: flex;
      gap: 8px;
    }
    input[type="text"] {
      flex: 1;
      background: var(--surface-low);
      border: 1px solid var(--outline);
      color: #fff;
      padding: 8px 12px;
      border-radius: 6px;
      font-size: 13px;
    }
    button {
      background: var(--primary-dim);
      color: #fff;
      border: none;
      padding: 8px 16px;
      border-radius: 6px;
      font-size: 13px;
      font-weight: 600;
      cursor: pointer;
      transition: background 0.15s ease;
    }
    button:hover {
      background: var(--primary);
    }
    button:active {
      transform: translateY(1px);
    }
    .secondary-btn {
      background: var(--surface-low);
      border: 1px solid var(--outline);
      color: #d1d5db;
    }
    .secondary-btn:hover {
      background: #282535;
      color: #fff;
    }
    .slider-group {
      display: flex;
      flex-direction: column;
      gap: 8px;
      background: var(--surface-low);
      padding: 12px;
      border-radius: 8px;
      border: 1px solid var(--outline);
    }
    .slider-label {
      display: flex;
      justify-content: space-between;
      font-size: 12px;
      color: #9ca3af;
    }
    .slider-label span:last-child {
      color: var(--cyan);
      font-weight: 600;
      font-family: monospace;
    }
    input[type="range"] {
      width: 100%;
      accent-color: var(--primary);
    }
    .stat-row {
      display: grid;
      grid-template-columns: repeat(3, 1fr);
      gap: 10px;
    }
    .stat-box {
      background: var(--surface-low);
      padding: 12px;
      border-radius: 8px;
      border: 1px solid var(--outline);
    }
    .stat-label {
      font-size: 11px;
      color: #9ca3af;
      text-transform: uppercase;
      letter-spacing: 0.5px;
    }
    .stat-val {
      font-size: 16px;
      font-weight: 700;
      color: #fff;
      margin-top: 4px;
      font-family: monospace;
    }
    .key-badge {
      display: inline-block;
      background: #312e81;
      color: #c7d2fe;
      padding: 2px 8px;
      border-radius: 4px;
      font-family: monospace;
      font-size: 13px;
      font-weight: 700;
      border: 1px solid #4338ca;
    }
    .log-box {
      background: #09090d;
      border: 1px solid var(--outline);
      border-radius: 8px;
      padding: 12px;
      font-family: ui-monospace, "SF Mono", Menlo, Consolas, monospace;
      font-size: 12px;
      height: 140px;
      overflow-y: auto;
      color: #a1a1aa;
    }
    .log-line {
      margin-bottom: 4px;
    }
    .log-success { color: var(--success); }
    .log-cyan { color: var(--cyan); }
  </style>
</head>
<body>
  <div class="header">
    <div class="title-group">
      <h1>Kube32 Silicon Hardware Workbench</h1>
      <p>Hosted directly on microcontroller cluster silicon (Node 1 Control Plane & Node 2 Worker)</p>
    </div>
    <div class="pill">
      <span class="pill-dot"></span>
      <span>Cluster Direct Link (192.168.4.1)</span>
    </div>
  </div>

  <div class="stat-row" style="margin-bottom: 20px;">
    <div class="stat-box">
      <div class="stat-label">Node 1 Free SRAM</div>
      <div class="stat-val" id="n1-sram">-- KB</div>
    </div>
    <div class="stat-box">
      <div class="stat-label">Physical Key Pressed</div>
      <div class="stat-val"><span class="key-badge" id="key-val">NONE</span></div>
    </div>
    <div class="stat-box">
      <div class="stat-label">Cluster Workload Pods</div>
      <div class="stat-val" id="pod-val">2 Active</div>
    </div>
  </div>

  <div class="grid">
    <!-- Card 1: Physical ST7789 Display Control -->
    <div class="card">
      <div class="card-title">
        <span>ST7789 Color Display (240x135)</span>
        <span style="font-size: 11px; color: var(--success);">Physical SPI Mirror</span>
      </div>
      <div class="screen-bezel">
        <canvas id="screen-canvas" width="240" height="135"></canvas>
      </div>
      <div class="input-row">
        <input type="text" id="display-text" placeholder="Type text to display on physical screen..." value="Hello from Kube32 Silicon!">
        <button id="btn-push-text">Push to LCD</button>
      </div>
      <div class="input-row" style="gap: 8px;">
        <button class="secondary-btn" id="btn-mode-k8s" style="flex:1;">Status Dashboard</button>
        <button class="secondary-btn" id="btn-mode-clear" style="flex:1;">Clear Screen</button>
      </div>
    </div>

    <!-- Card 2: Acoustic Speaker & Execution Telemetry -->
    <div class="card">
      <div class="card-title">
        <span>Acoustic Speaker (PWM / Audio)</span>
        <span style="font-size: 11px; color: var(--cyan);">Physical Hardware</span>
      </div>
      <div class="slider-group">
        <div class="slider-label">
          <span>Frequency (Hz)</span>
          <span id="freq-text">1000 Hz</span>
        </div>
        <input type="range" id="freq-slider" min="200" max="4000" step="50" value="1000">
      </div>
      <div class="slider-group">
        <div class="slider-label">
          <span>Duration (ms)</span>
          <span id="dur-text">150 ms</span>
        </div>
        <input type="range" id="dur-slider" min="50" max="800" step="25" value="150">
      </div>
      <div class="input-row">
        <button id="btn-play-tone" style="flex: 1;">Emit Tone on Physical Device</button>
        <button class="secondary-btn" id="btn-play-chime">Boot Chime</button>
      </div>
      <div class="card-title" style="margin-top: 4px;">
        <span>Direct Cluster Hardware Log</span>
      </div>
      <div class="log-box" id="log-box">
        <div class="log-line log-success">[INIT] Connected directly to Kube32 silicon host (http://192.168.4.1)</div>
        <div class="log-line log-cyan">[HARDWARE] ST7789 (240x135) + 56-key matrix + Speaker active</div>
      </div>
    </div>
  </div>

  <script>
    const canvas = document.getElementById('screen-canvas');
    const ctx = canvas.getContext('2d');
    const logBox = document.getElementById('log-box');

    let currentDisplayText = "KUBE32 v1.33 SILICON MESH\nSTATUS: Control-Plane Active\nNODE:   esp32-node-01 (Master)\nIP:     192.168.4.1 (SoftAP)\nHEAP:   48,400 B | PSRAM: 2MB\nPODS:   hardware-controller\nREADY:  Listening for k8s...";

    function log(msg, cls = '') {
      const line = document.createElement('div');
      line.className = 'log-line ' + cls;
      line.textContent = `[${new Date().toLocaleTimeString()}] ${msg}`;
      logBox.appendChild(line);
      logBox.scrollTop = logBox.scrollHeight;
    }

    function renderScreenMirror() {
      ctx.fillStyle = '#141218';
      ctx.fillRect(0, 0, 240, 135);

      // Header bar
      ctx.fillStyle = '#1e1b4b';
      ctx.fillRect(0, 0, 240, 20);
      ctx.fillStyle = '#38bdf8';
      ctx.font = '10px monospace';
      ctx.fillText('KUBE32 v1.33 SILICON MESH', 8, 14);

      // Body lines
      ctx.fillStyle = '#34d399';
      ctx.font = '10px monospace';
      const lines = currentDisplayText.split('\n');
      lines.forEach((l, idx) => {
        if (idx === 0) ctx.fillStyle = '#38bdf8';
        else if (idx === 1) ctx.fillStyle = '#34d399';
        else ctx.fillStyle = '#e2e8f0';
        ctx.fillText(l, 8, 34 + idx * 14);
      });
    }
    renderScreenMirror();

    // Push text to physical screen
    document.getElementById('btn-push-text').addEventListener('click', async () => {
      const text = document.getElementById('display-text').value;
      if (!text) return;
      currentDisplayText = text;
      renderScreenMirror();
      log(`Pushing text "${text.slice(0, 20)}..." to physical ST7789 LCD`);
      try {
        const res = await fetch('/api/v1/hardware/display', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ text: text })
        });
        if (res.ok) log('Physical LCD buffer updated successfully', 'log-success');
      } catch (err) {
        log('Error communicating with hardware: ' + err.message);
      }
    });

    document.getElementById('btn-mode-k8s').addEventListener('click', async () => {
      currentDisplayText = "KUBE32 v1.33 SILICON MESH\nSTATUS: Control-Plane Active\nNODE:   esp32-node-01 (Master)\nIP:     192.168.4.1 (SoftAP)\nHEAP:   48,400 B | PSRAM: 2MB\nPODS:   hardware-controller\nREADY:  Listening for k8s...";
      renderScreenMirror();
      fetch('/api/v1/hardware/display', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ text: currentDisplayText })
      });
      log('Restored status dashboard on physical screen');
    });

    document.getElementById('btn-mode-clear').addEventListener('click', async () => {
      currentDisplayText = "";
      renderScreenMirror();
      fetch('/api/v1/hardware/display', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ text: "" })
      });
      log('Cleared physical LCD screen');
    });

    // Tone controls
    const freqSlider = document.getElementById('freq-slider');
    const freqText = document.getElementById('freq-text');
    freqSlider.addEventListener('input', e => freqText.textContent = e.target.value + ' Hz');

    const durSlider = document.getElementById('dur-slider');
    const durText = document.getElementById('dur-text');
    durSlider.addEventListener('input', e => durText.textContent = e.target.value + ' ms');

    document.getElementById('btn-play-tone').addEventListener('click', () => {
      const f = parseInt(freqSlider.value);
      const d = parseInt(durSlider.value);
      triggerTone(f, d);
    });

    document.getElementById('btn-play-chime').addEventListener('click', () => {
      const notes = [523, 659, 784, 1046];
      notes.forEach((n, i) => {
        setTimeout(() => triggerTone(n, 90), i * 110);
      });
      log('Playing boot chime melody on physical speaker');
    });

    async function triggerTone(freq, dur) {
      log(`Triggering tone ${freq} Hz (${dur} ms) on physical hardware`, 'log-cyan');
      try {
        await fetch('/api/v1/hardware/speaker', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ frequency_hz: freq, duration_ms: dur })
        });
      } catch (err) {}
    }

    // Telemetry polling directly from cluster
    async function pollTelemetry() {
      try {
        const res = await fetch('/api/v1/hardware/state');
        if (res.ok) {
          const data = await res.json();
          if (data.nodes && data.nodes['esp32-node-01']) {
            const n1 = data.nodes['esp32-node-01'];
            if (n1.free_sram_bytes) {
              document.getElementById('n1-sram').textContent = (n1.free_sram_bytes / 1024).toFixed(1) + ' KB';
            }
            if (n1.keyboard && n1.keyboard.last_key) {
              const k = n1.keyboard.last_key;
              const el = document.getElementById('key-val');
              if (el.textContent !== k && k !== 'NONE') {
                el.textContent = k;
                log(`Physical keystroke detected: '${k}'`, 'log-success');
              }
            }
          }
        }
      } catch (e) {}
      setTimeout(pollTelemetry, 1500);
    }
    pollTelemetry();
  </script>
</body>
</html>
)rawhtml";

} // namespace kube32::api
