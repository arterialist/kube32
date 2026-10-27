/**
 * Kube32 Cluster Console — Client Application Logic
 * Material 3 Design Reactive State & Hardware Telemetry Controller
 */

// Global State
let activeTab = 'tab-overview';
let activeLogPod = null;
let gpioStates = Array(16).fill(false);

const PRESET_BLINKER = `apiVersion: v1
kind: Pod
metadata:
  name: gpio-blinker
  namespace: default
  labels:
    app: blinker
    environment: edge
spec:
  nodeName: esp32-node-02
  containers:
    - name: blinker-task
      image: wasm://embedded/blinker:v1
      resources:
        limits:
          memory: 32Ki
        requests:
          memory: 16Ki
          hardware.kube32.io/gpio: "1"`;

const PRESET_PIPELINE = `apiVersion: v1
kind: Pod
metadata:
  name: audio-pipeline-tx
  namespace: kube32-cluster
  labels:
    app: mfcc-preprocessor
    pipeline.kube32.io/stage: "1"
spec:
  nodeName: esp32-node-01
  containers:
    - name: preprocessor
      image: wasm://embedded/audio-mfcc:v1
      resources:
        limits:
          memory: 48Ki
        requests:
          memory: 32Ki`;

const PRESET_SENSOR = `apiVersion: v1
kind: Pod
metadata:
  name: i2c-telemetry-loop
  namespace: default
  labels:
    app: environmental-sensor
spec:
  nodeName: esp32-node-02
  containers:
    - name: sensor-collector
      image: wasm://embedded/i2c-bme280:v1
      resources:
        limits:
          memory: 24Ki
        requests:
          memory: 16Ki
          hardware.kube32.io/i2c: "1"`;

// Initialize Application
document.addEventListener('DOMContentLoaded', () => {
  initTabs();
  initTheme();
  initGpioGrid();
  initModals();
  initPipeline();

  // Initial fetch and start auto-poll
  fetchClusterState();
  setInterval(fetchClusterState, 2000);
});

// Tab Navigation
function initTabs() {
  const navItems = document.querySelectorAll('.nav-item[data-tab]');
  navItems.forEach(item => {
    item.addEventListener('click', () => {
      const targetId = item.dataset.tab;
      if (!targetId) return;

      navItems.forEach(n => n.classList.remove('active'));
      item.classList.add('active');

      document.querySelectorAll('.tab-pane').forEach(p => p.classList.remove('active'));
      const activePane = document.getElementById(targetId);
      if (activePane) activePane.classList.add('active');
      activeTab = targetId;
    });
  });
}

// Dark / Light Theme
function initTheme() {
  const btn = document.getElementById('theme-toggle-btn');
  const icon = document.getElementById('theme-icon');
  const saved = localStorage.getItem('kube32-theme') || 'dark';

  document.documentElement.setAttribute('data-theme', saved);
  icon.textContent = saved === 'dark' ? 'light_mode' : 'dark_mode';

  btn.addEventListener('click', () => {
    const current = document.documentElement.getAttribute('data-theme');
    const next = current === 'dark' ? 'light' : 'dark';
    document.documentElement.setAttribute('data-theme', next);
    localStorage.setItem('kube32-theme', next);
    icon.textContent = next === 'dark' ? 'light_mode' : 'dark_mode';
  });
}

// Fetch Cluster State & Hardware Telemetry
async function fetchClusterState() {
  try {
    const res = await fetch('/api/cluster/status');
    if (!res.ok) throw new Error('API server unreachable');
    const data = await res.json();

    // Update status dot
    const statusText = document.getElementById('cluster-status-text');
    if (statusText) statusText.textContent = `${data.status} (esp32-s3)`;

    // Update Telemetry
    if (data.hardware) {
      const sramBytes = data.hardware.free_sram_bytes || 48400;
      const sramKb = (sramBytes / 1024).toFixed(1);
      const sramEl = document.getElementById('sram-val');
      if (sramEl) sramEl.textContent = `${sramBytes.toLocaleString()} B (${sramKb} KiB)`;

      const sramBar = document.getElementById('sram-bar');
      if (sramBar) {
        const pct = Math.min(100, Math.max(10, Math.round((sramBytes / 65536) * 100)));
        sramBar.style.width = `${pct}%`;
      }

      const otaEl = document.getElementById('ota-count');
      if (otaEl) otaEl.textContent = `${data.hardware.ota_packets_sent} tx`;
    }

    const wCount = document.getElementById('workload-count');
    if (wCount) wCount.textContent = `${data.pod_count} Pods`;

    // Fetch and render pods table
    fetchPods();
  } catch (err) {
    const statusText = document.getElementById('cluster-status-text');
    if (statusText) statusText.textContent = 'Disconnected';
  }
}

// Fetch Pods List
async function fetchPods() {
  try {
    const res = await fetch('/api/v1/pods');
    if (!res.ok) return;
    const doc = await res.json();
    renderPodsTable(doc.items || []);
  } catch (err) {}
}

function renderPodsTable(items) {
  const tbody = document.getElementById('pods-table-body');
  if (!tbody) return;

  if (items.length === 0) {
    tbody.innerHTML = `<tr><td colspan="8" style="text-align: center; color: var(--md-sys-color-outline); padding: 24px;">No workloads currently scheduled</td></tr>`;
    return;
  }

  tbody.innerHTML = items.map(p => {
    const ns = p.metadata?.namespace || 'default';
    const name = p.metadata?.name || 'pod';
    const phase = p.status?.phase || 'Running';
    const isRunning = phase === 'Running';
    const node = p.spec?.nodeName || 'esp32-node-02';
    const ip = p.status?.podIP || '192.168.4.2';
    const ready = isRunning ? '1/1' : '0/1';

    return `
      <tr>
        <td><strong>${escapeHtml(ns)}</strong></td>
        <td><code>${escapeHtml(name)}</code></td>
        <td>${ready}</td>
        <td>
          <span class="pill-phase ${isRunning ? 'pill-running' : 'pill-pending'}">
            ${escapeHtml(phase)}
          </span>
        </td>
        <td><span style="font-family: monospace;">${escapeHtml(node)}</span></td>
        <td>${escapeHtml(ip)}</td>
        <td>Active</td>
        <td>
          <div style="display: flex; gap: 8px;">
            <button class="btn btn-tonal" style="padding: 4px 12px; font-size: 12px;" onclick="openPodLogs('${escapeHtml(ns)}', '${escapeHtml(name)}')">
              <span class="material-symbols-outlined" style="font-size: 16px;">receipt_long</span>
              Logs
            </button>
            <button class="btn btn-outlined" style="padding: 4px 10px; font-size: 12px; color: var(--md-sys-color-error);" onclick="deletePod('${escapeHtml(ns)}', '${escapeHtml(name)}')">
              <span class="material-symbols-outlined" style="font-size: 16px;">delete</span>
            </button>
          </div>
        </td>
      </tr>
    `;
  }).join('');
}

// Log Viewer Modal
window.openPodLogs = async function(ns, name) {
  activeLogPod = { ns, name };
  const modal = document.getElementById('log-modal');
  const title = document.getElementById('log-modal-title');
  const body = document.getElementById('log-modal-body');

  title.textContent = `Logs: ${ns}/${name}`;
  body.textContent = 'Streaming logs from container...';
  modal.classList.add('open');

  try {
    const res = await fetch(`/api/v1/namespaces/${ns}/pods/${name}/log`);
    if (res.ok) {
      body.textContent = await res.text();
    } else {
      body.textContent = `Error streaming logs: HTTP ${res.status}`;
    }
  } catch (err) {
    body.textContent = `Error connecting to API server: ${err.message}`;
  }
};

// Delete Pod
window.deletePod = async function(ns, name) {
  if (!confirm(`Are you sure you want to delete pod ${ns}/${name}?`)) return;
  try {
    await fetch(`/api/v1/namespaces/${ns}/pods/${name}`, { method: 'DELETE' });
    fetchPods();
  } catch (err) {
    alert('Delete error: ' + err.message);
  }
};

// GPIO Matrix
function initGpioGrid() {
  const container = document.getElementById('gpio-grid-container');
  if (!container) return;

  container.innerHTML = gpioStates.map((state, idx) => {
    const pin = idx + 1;
    return `
      <div class="gpio-pin-btn ${state ? 'pin-high' : ''}" data-pin="${pin}" onclick="toggleGpio(${pin})">
        <span class="material-symbols-outlined" style="font-size: 20px;">
          ${state ? 'toggle_on' : 'toggle_off'}
        </span>
        <span class="pin-num">GPIO ${pin}</span>
        <span class="pin-state">${state ? 'HIGH' : 'LOW'}</span>
      </div>
    `;
  }).join('');

  document.getElementById('btn-reset-gpio')?.addEventListener('click', () => {
    gpioStates.fill(false);
    initGpioGrid();
  });
}

window.toggleGpio = function(pin) {
  const idx = pin - 1;
  gpioStates[idx] = !gpioStates[idx];
  initGpioGrid();

  // Send to hardware capability API if needed
  fetch('/api/gpio/write', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ pin, state: gpioStates[idx] ? 1 : 0 })
  }).catch(() => {});
};

// Distributed Pipeline Trigger
function initPipeline() {
  const btn = document.getElementById('btn-trigger-pipeline');
  const stream = document.getElementById('pipeline-stream-box');
  if (!btn || !stream) return;

  btn.addEventListener('click', async () => {
    btn.disabled = true;
    btn.innerHTML = `<span class="material-symbols-outlined status-dot-pulse">sync</span> Transmitting...`;

    try {
      const res = await fetch('/api/pipeline/transmit');
      const text = await res.text();
      const now = new Date().toISOString().replace('T', ' ').substring(0, 19);
      stream.textContent += `\n[${now}] ${text.trim()}`;
      stream.scrollTop = stream.scrollHeight;
    } catch (e) {
      stream.textContent += `\n[ERROR] Pipeline transmit failed: ${e.message}`;
    } finally {
      btn.disabled = false;
      btn.innerHTML = `<span class="material-symbols-outlined">send</span> Transmit Pipeline Vector`;
      fetchClusterState();
    }
  });
}

// Workload Deployment Modals
function initModals() {
  const deployModal = document.getElementById('deploy-modal');
  const openDeployBtn = document.getElementById('btn-open-deploy-modal');
  const closeDeployBtn = document.getElementById('btn-close-deploy-modal');
  const cancelDeployBtn = document.getElementById('btn-cancel-deploy');
  const submitDeployBtn = document.getElementById('btn-submit-deploy');
  const manifestInput = document.getElementById('manifest-input');

  // Presets
  document.getElementById('btn-preset-blinker')?.addEventListener('click', () => {
    manifestInput.value = PRESET_BLINKER;
  });
  document.getElementById('btn-preset-pipeline')?.addEventListener('click', () => {
    manifestInput.value = PRESET_PIPELINE;
  });
  document.getElementById('btn-preset-sensor')?.addEventListener('click', () => {
    manifestInput.value = PRESET_SENSOR;
  });

  openDeployBtn?.addEventListener('click', () => {
    manifestInput.value = manifestInput.value || PRESET_BLINKER;
    deployModal.classList.add('open');
  });

  const closeDeploy = () => deployModal.classList.remove('open');
  closeDeployBtn?.addEventListener('click', closeDeploy);
  cancelDeployBtn?.addEventListener('click', closeDeploy);

  submitDeployBtn?.addEventListener('click', async () => {
    const raw = manifestInput.value.trim();
    if (!raw) return;

    submitDeployBtn.disabled = true;
    submitDeployBtn.textContent = 'Deploying...';

    try {
      let podObj;
      if (raw.startsWith('{')) {
        podObj = JSON.parse(raw);
      } else {
        // Parse basic yaml
        podObj = parseSimpleYaml(raw);
      }

      const ns = podObj.metadata?.namespace || 'default';
      const res = await fetch(`/api/v1/namespaces/${ns}/pods`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(podObj)
      });

      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      closeDeploy();
      fetchPods();
    } catch (err) {
      alert('Deployment error: ' + err.message);
    } finally {
      submitDeployBtn.disabled = false;
      submitDeployBtn.textContent = 'Deploy to Cluster';
    }
  });

  // Log modal close buttons
  const logModal = document.getElementById('log-modal');
  document.getElementById('btn-close-log-modal')?.addEventListener('click', () => logModal.classList.remove('open'));
  document.getElementById('btn-dismiss-log')?.addEventListener('click', () => logModal.classList.remove('open'));
  document.getElementById('btn-refresh-log')?.addEventListener('click', () => {
    if (activeLogPod) openPodLogs(activeLogPod.ns, activeLogPod.name);
  });
}

function parseSimpleYaml(yamlStr) {
  const lines = yamlStr.split('\n');
  let name = 'custom-workload';
  let namespace = 'default';
  let nodeName = 'esp32-node-02';
  let image = 'wasm://embedded/task:v1';

  for (const line of lines) {
    const trimmed = line.trim();
    if (trimmed.startsWith('name:')) name = trimmed.split(':')[1].trim();
    if (trimmed.startsWith('namespace:')) namespace = trimmed.split(':')[1].trim();
    if (trimmed.startsWith('nodeName:')) nodeName = trimmed.split(':')[1].trim();
    if (trimmed.startsWith('image:')) image = trimmed.split(':')[1].trim();
  }

  return {
    apiVersion: 'v1',
    kind: 'Pod',
    metadata: { name, namespace, labels: { app: name } },
    spec: { nodeName, containers: [{ name, image }] }
  };
}

function escapeHtml(str) {
  if (!str) return '';
  return String(str).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}
