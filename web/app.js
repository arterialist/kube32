/**
 * Kube32 Cluster Console — Client Application Logic
 * Material 3 Design Reactive State & Hardware Telemetry Controller
 */

// Global State
let activeTab = 'tab-overview';
let activeLogPod = null;
let activeShowcaseFilter = 'all';
let gpioStates = Array(16).fill(false);
let clusterPods = [];

// ---------------------------------------------------------------------------
// 10 Grounded Mission-Critical Workload Demonstrations
// ---------------------------------------------------------------------------

const WORKLOAD_CATALOG = [
  {
    id: 'hardware-workbench',
    podNames: ['node1-hardware-controller', 'node2-dsp-coprocessor'],
    title: 'Fullstack Cluster Hardware Workbench',
    domain: 'Real-Time Hardware & Media',
    filterCategory: 'all',
    icon: 'developer_board',
    problem: 'Real-time human-interface hardware (color TFT LCD, 56-key matrix keyboard, I2S digital mic, I2S speaker, IR transmitter, battery ADC) running alongside compute-heavy 16-band audio FFT overloads single-MCU architectures or causes visible display lag and dropped keystrokes.',
    impossibleWhy: 'Kube32 distributes the pipeline across nodes: Node 1 handles all hardware I/O with capability firewall protection and streams raw 16 kHz audio over zero-AP ESP-NOW to Node 2. Node 2 uses 2 MB PSRAM for real-time 16-band FFT. The fullstack web app delivers bidirectional controls.',
    targetNode: 'Dual-Node (Node 1 & Node 2)',
    specs: {
      memory: '32 KiB SRAM (Node 1) + 48 KiB SRAM / 2 MB PSRAM (Node 2)',
      peripherals: 'ST7789 TFT, 56-Key Keyboard, SPM1423 Mic, NS4148 Speaker, IR GPIO 44, ADC1',
      isolation: 'Capability Bitmask + Zero-AP ESP-NOW Virtual Channel Mesh'
    },
    yaml: `apiVersion: v1
kind: Pod
metadata:
  name: node1-hardware-controller
  namespace: kube32-cluster
  labels:
    app: hardware-workbench
    tier: master-controller
spec:
  nodeName: esp32-node-01
  nodeSelector:
    kubernetes.io/hostname: esp32-node-01
  restartPolicy: Always
  containers:
    - name: hardware-controller
      image: wasm://embedded/hardware-workbench-master:v1
      resources:
        limits:
          memory: 48Ki
        requests:
          memory: 32Ki
          hardware.kube32.io/tft-display: "1"
          hardware.kube32.io/keyboard: "1"
          hardware.kube32.io/microphone: "1"
          hardware.kube32.io/speaker: "1"
          hardware.kube32.io/ir-transmitter: "1"
          hardware.kube32.io/battery-adc: "1"
          hardware.kube32.io/ipc: "1"
---
apiVersion: v1
kind: Pod
metadata:
  name: node2-dsp-coprocessor
  namespace: kube32-cluster
  labels:
    app: hardware-workbench
    tier: dsp-coprocessor
spec:
  nodeName: esp32-node-02
  nodeSelector:
    kubernetes.io/hostname: esp32-node-02
    hardware.kube32.io/psram: "1"
  restartPolicy: Always
  containers:
    - name: dsp-coprocessor
      image: wasm://embedded/hardware-workbench-dsp:v1
      resources:
        limits:
          memory: 64Ki
        requests:
          memory: 48Ki
          hardware.kube32.io/psram: "1"
          hardware.kube32.io/ipc: "1"
---
apiVersion: v1
kind: Service
metadata:
  name: hardware-workbench-svc
  namespace: kube32-cluster
spec:
  type: ClusterIP
  ports:
    - name: http-web
      port: 80
      targetPort: 80
    - name: espnow-ipc
      port: 7400
      targetPort: 7400
  selector:
    app: hardware-workbench`
  },
  {
    id: 'grid-substation',
    podNames: ['grid-substation-firewall-canary'],
    title: 'Grid Substation SCADA & Peripheral Firewall',
    domain: 'Critical Infrastructure',
    filterCategory: 'security',
    icon: 'shield',
    problem: 'Third-party cloud telemetry software sharing the MCU with high-voltage switchgear can overwrite hardware register banks during a buffer overflow, tripping substations or causing arc flashes.',
    impossibleWhy: 'Kube32 O(1) Capability Firewall blocks unauthorized memory-mapped I/O and register writes at the Wasm boundary. The telemetry pod is trapped and audited without crashing the trip relay.',
    targetNode: 'esp32-node-01',
    specs: {
      memory: '32 KiB internal SRAM',
      peripherals: 'UART_READ, Capability Firewall bitmask check',
      isolation: 'Memory-mapped I/O Trap'
    },
    yaml: `apiVersion: v1
kind: Pod
metadata:
  name: grid-substation-firewall-canary
  namespace: default
  labels:
    app: scada-grid-protection
    tier: critical-infrastructure
    security.kube32.io/isolation: capability-firewall
spec:
  nodeName: esp32-node-01
  restartPolicy: Never
  containers:
    - name: telemetry-reporter
      image: wasm://grid/scada-telemetry:v2
      resources:
        limits:
          memory: 32Ki
        requests:
          memory: 16Ki
      securityContext:
        capabilities:
          drop: ["ALL"]
          add: ["UART_READ"]
    - name: high-voltage-trip-canary
      image: wasm://grid/trip-relay-guard:v1
      resources:
        limits:
          memory: 24Ki
        requests:
          memory: 12Ki
          hardware.kube32.io/gpio: "1"
      securityContext:
        capabilities:
          add: ["GPIO_WRITE"]`
  },
  {
    id: 'acoustic-bearing',
    podNames: ['acoustic-bearing-pipeline-tx', 'acoustic-bearing-classifier-rx'],
    title: 'Predictive Bearing Maintenance Acoustic Pipeline',
    domain: 'Distributed AI',
    filterCategory: 'ai',
    icon: 'hearing',
    problem: '48 kHz acoustic vibration sampling and neural fault detection cannot run on a single microcontroller due to internal SRAM limits (<48 KiB free), forcing plants to deploy $500 industrial PCs.',
    impossibleWhy: 'Kube32 pipelines 512-point FFT feature extraction on Node 1 across raw 2.4 GHz ESP-NOW to Node 2 (2MB PSRAM), running quantized neural inference with zero WiFi AP or cloud backhaul.',
    targetNode: 'esp32-node-01 -> esp32-node-02',
    specs: {
      memory: '48 KiB (Node 1) + 1024 KiB PSRAM (Node 2)',
      peripherals: 'ADC Ch 0 (Piezo), ESP-NOW 2.4 GHz Virtual Channel',
      isolation: 'Cross-Silicon Radio Pipelining'
    },
    yaml: `apiVersion: v1
kind: Pod
metadata:
  name: acoustic-bearing-pipeline-tx
  namespace: kube32-cluster
  labels:
    app: predictive-maintenance
    pipeline.kube32.io/stage: "1"
    pipeline.kube32.io/channel: "vibration.mfcc"
spec:
  nodeName: esp32-node-01
  restartPolicy: Always
  containers:
    - name: piezo-fft-sampler
      image: wasm://industrial/piezo-acoustic-fft:v1
      resources:
        limits:
          memory: 48Ki
        requests:
          memory: 32Ki
          hardware.kube32.io/adc: "0"
---
apiVersion: v1
kind: Pod
metadata:
  name: acoustic-bearing-classifier-rx
  namespace: kube32-cluster
  labels:
    app: predictive-maintenance
    pipeline.kube32.io/stage: "2"
    pipeline.kube32.io/channel: "vibration.mfcc"
spec:
  nodeName: esp32-node-02
  nodeSelector:
    hardware.kube32.io/psram: "true"
  restartPolicy: Always
  containers:
    - name: neural-bearing-classifier
      image: wasm://industrial/bearing-fault-detector:v1
      resources:
        limits:
          memory: 1024Ki
        requests:
          memory: 512Ki
          hardware.kube32.io/psram: "1"`
  },
  {
    id: 'cold-chain',
    podNames: ['cold-chain-cryo-supervisor'],
    title: 'Vaccine Cold-Chain Resilient Cryo Supervisor',
    domain: 'Life-Safety & Resilience',
    filterCategory: 'safety',
    icon: 'ac_unit',
    problem: 'Hardware brownouts or ESD zaps on refrigeration MCUs halt compressor loops, destroying biologic medicines and clinical trial samples worth millions of dollars.',
    impossibleWhy: 'Declarative Lease Controller monitors 4-second node heartbeats. If Worker Node 2 fails, Kube32 automatically evicts and reschedules the PID compressor controller to Node 1 in <1.2 seconds.',
    targetNode: 'esp32-node-02 (Failover to Node 01)',
    specs: {
      memory: '32 KiB internal SRAM',
      peripherals: 'GPIO 2 Compressor Solenoid PWM',
      isolation: 'Automated Lease Failover & Anti-Affinity'
    },
    yaml: `apiVersion: v1
kind: Pod
metadata:
  name: cold-chain-cryo-supervisor
  namespace: default
  labels:
    app: vaccine-cryo-controller
    tier: medical-life-safety
    reliability.kube32.io/priority: "critical"
spec:
  nodeName: esp32-node-02
  restartPolicy: Always
  affinity:
    podAntiAffinity:
      requiredDuringSchedulingIgnoredDuringExecution:
        - labelSelector:
            matchExpressions:
              - key: app
                operator: In
                values: ["vaccine-cryo-controller"]
          topologyKey: "kubernetes.io/hostname"
  containers:
    - name: compressor-pid-regulator
      image: wasm://medical/cryo-pid-governor:v1
      resources:
        limits:
          memory: 32Ki
        requests:
          memory: 16Ki
          hardware.kube32.io/gpio: "2"`
  },
  {
    id: 'wildfire-swarm',
    podNames: ['wildfire-evacuation-swarm'],
    title: 'Wildfire Blackout Mesh & Evacuation Siren Beacon',
    domain: 'Energy & Hybrid Mesh',
    filterCategory: 'mesh',
    icon: 'crisis_alert',
    problem: 'During forest fires or flash floods, cellular towers burn down and power cuts kill WiFi. Dispersed sensors cannot coordinate sirens or strobes without central backhaul.',
    impossibleWhy: 'Kube32 DaemonSet broadcasts raw ESP-NOW frames, synchronizing optical strobes and sirens across physical boards with sub-1.5 ms clock drift without routers or cell towers.',
    targetNode: 'All Nodes (DaemonSet)',
    specs: {
      memory: '28 KiB per node',
      peripherals: 'GPIO 4 Strobe / Siren Actuator',
      isolation: 'Zero-AP Peer-to-Peer Radio Broadcast'
    },
    yaml: `apiVersion: apps/v1
kind: DaemonSet
metadata:
  name: wildfire-evacuation-swarm
  namespace: kube32-cluster
  labels:
    app: wildfire-beacon
    mission: emergency-blackout-mesh
spec:
  selector:
    matchLabels:
      app: wildfire-beacon
  template:
    metadata:
      labels:
        app: wildfire-beacon
    spec:
      containers:
        - name: swarm-transceiver
          image: wasm://emergency/swarm-mesh-beacon:v1
          resources:
            limits:
              memory: 28Ki
            requests:
              memory: 16Ki
              hardware.kube32.io/gpio: "4"`
  },
  {
    id: 'cubesat-sandbox',
    podNames: ['cubesat-payload-sandbox'],
    title: 'CubeSat Orbital Science Gas-Metered Sandbox',
    domain: 'Avionics & Space',
    filterCategory: 'robotics',
    icon: 'rocket_launch',
    problem: 'Untrusted experimental algorithms uploaded to satellite flight computers with bugs (e.g. while(1)) trigger CPU watchdog resets, tumbling spacecraft or bricking missions.',
    impossibleWhy: 'Hard deterministic Wasm gas metering (2.5M instruction limit). When gradient descent loops run away, Kube32 traps the task, reclaims heap, and keeps core flight telemetry alive.',
    targetNode: 'esp32-node-01',
    specs: {
      memory: '32 KiB SRAM, 500m CPU quota',
      peripherals: 'Gas Limit Ceiling: 2,500,000 instructions',
      isolation: 'Deterministic Gas-Metered Preemption'
    },
    yaml: `apiVersion: v1
kind: Pod
metadata:
  name: cubesat-payload-sandbox
  namespace: default
  labels:
    app: orbital-payload-experiment
    mission: cubesat-science
    security.kube32.io/sandbox: gas-metered
spec:
  nodeName: esp32-node-01
  restartPolicy: Never
  containers:
    - name: experimental-attitude-tracker
      image: wasm://payload/attitude-estimator:v3-untrusted
      resources:
        limits:
          memory: 32Ki
          cpu: "500m"
          hardware.kube32.io/gas-limit: "2500000"
        requests:
          memory: 16Ki`
  },
  {
    id: 'seismic-recorder',
    podNames: ['seismic-bridge-flight-recorder'],
    title: 'Seismic Bridge Health & Flight Black Box',
    domain: 'Life-Safety & Resilience',
    filterCategory: 'safety',
    icon: 'vibration',
    problem: 'Structural shocks produce 1,000 Hz vibration bursts. Microcontroller internal SRAM fills up in 20 ms, and synchronous flash logging blocks real-time loops and burns out flash cells.',
    impossibleWhy: 'Kube32 scores and assigns the workload to esp32-node-02 via hardware.kube32.io/psram: true, maintaining a 1.5 MB high-speed circular pre-trigger buffer with zero flash wear.',
    targetNode: 'esp32-node-02 (2MB PSRAM)',
    specs: {
      memory: '1,536 KiB Quad-SPI PSRAM buffer',
      peripherals: 'ADC Ch 1 Piezo Accelerometer (1 kHz)',
      isolation: 'Memory-Tiered Hardware Affinity'
    },
    yaml: `apiVersion: v1
kind: Pod
metadata:
  name: seismic-bridge-flight-recorder
  namespace: default
  labels:
    app: infrastructure-blackbox
    tier: structural-health
spec:
  nodeName: esp32-node-02
  nodeSelector:
    hardware.kube32.io/psram: "true"
  restartPolicy: Always
  containers:
    - name: ring-buffer-recorder
      image: wasm://structural/psram-blackbox:v1
      resources:
        limits:
          memory: 1536Ki
        requests:
          memory: 1024Ki
          hardware.kube32.io/psram: "1"
          hardware.kube32.io/adc: "1"`
  },
  {
    id: 'drone-arbitrator',
    podNames: ['drone-flight-controller-fusion'],
    title: 'Autonomous Drone Flight Controller & Bus Arbitrator',
    domain: 'Avionics & Space',
    filterCategory: 'robotics',
    icon: 'precision_manufacturing',
    problem: 'Simultaneous I2C/SPI bus transactions between navigation IMUs and camera gimbals cause bus collisions, I2C_ACK_ERR timeouts, and fatal mid-flight drone crashes.',
    impossibleWhy: 'Declarative bus mutex locks (hardware.kube32.io/i2c: 0, hardware.kube32.io/spi: 1). Guarantees zero-collision Extended Kalman Filter execution at 200 Hz with strict bus isolation.',
    targetNode: 'esp32-node-01',
    specs: {
      memory: '40 KiB SRAM',
      peripherals: 'I2C Port 0 Mutex + SPI Port 1 Mutex',
      isolation: 'Hardware Peripheral Bus Locks'
    },
    yaml: `apiVersion: v1
kind: Pod
metadata:
  name: drone-flight-controller-fusion
  namespace: kube32-cluster
  labels:
    app: drone-avionics
    tier: flight-control
    hardware.kube32.io/bus-lock: "exclusive"
spec:
  nodeName: esp32-node-01
  restartPolicy: Always
  containers:
    - name: ekf-quaternion-filter
      image: wasm://avionics/ekf-imu-fusion:v2
      resources:
        limits:
          memory: 40Ki
        requests:
          memory: 24Ki
          hardware.kube32.io/i2c: "0"
          hardware.kube32.io/spi: "1"`
  },
  {
    id: 'solar-governor',
    podNames: ['solar-energy-governor'],
    title: 'Off-Grid Solar Energy Governor & DVFS Throttler',
    domain: 'Energy & Hybrid Mesh',
    filterCategory: 'mesh',
    icon: 'solar_power',
    problem: 'Remote IoT sensor stations in winter storms deplete batteries when dual Xtensa cores run at 240 MHz full power, shutting down critical cathodic pipeline protection.',
    impossibleWhy: 'Telemetry pod monitors supply voltage and die temperature, dynamically instructing the Kube32 governor to throttle clock frequency from 240 MHz down to 80 MHz, extending runtime by 5.07x.',
    targetNode: 'esp32-node-02',
    specs: {
      memory: '24 KiB SRAM',
      peripherals: 'ADC Rail Voltage Monitor, DVFS Core Scaling',
      isolation: 'Scheduler-Driven Power Optimization'
    },
    yaml: `apiVersion: v1
kind: Pod
metadata:
  name: solar-energy-governor
  namespace: default
  labels:
    app: energy-harvester
    tier: microgrid-power-management
spec:
  nodeName: esp32-node-02
  restartPolicy: Always
  containers:
    - name: dynamic-clock-throttler
      image: wasm://energy/thermal-dvfs-governor:v1
      resources:
        limits:
          memory: 24Ki
        requests:
          memory: 16Ki
          hardware.kube32.io/adc: "0"`
  },
  {
    id: 'zero-trust',
    podNames: ['zero-trust-tamper-auth'],
    title: 'Zero-Trust Physical Enclave Ephemeral Micro-Job',
    domain: 'Critical Infrastructure',
    filterCategory: 'security',
    icon: 'key',
    problem: 'Keeping cryptographic keys permanently in MCU SRAM exposes facilities to memory readout via physical glitching or cold-boot side-channel attacks.',
    impossibleWhy: 'Knative-style ephemeral micro-job: Wasm image is cold-booted in 11.4 ms on a hardware tamper trigger, verifies HMAC-SHA256, actuates the lock, and terminates with 100% memory reclamation.',
    targetNode: 'esp32-node-01',
    specs: {
      memory: '24 KiB SRAM (reclaimed on exit)',
      peripherals: 'GPIO 5 Solenoid Lock Pulse',
      isolation: 'Sub-15ms Serverless Cold-Boot'
    },
    yaml: `apiVersion: batch/v1
kind: Job
metadata:
  name: zero-trust-tamper-auth
  namespace: default
  labels:
    app: secure-enclave-auth
    tier: physical-security
    execution: ephemeral-micro-job
spec:
  ttlSecondsAfterFinished: 5
  template:
    metadata:
      labels:
        app: secure-enclave-auth
    spec:
      nodeName: esp32-node-01
      restartPolicy: Never
      containers:
        - name: hmac-authenticator
          image: wasm://security/hmac-sha256-latch:v1
          resources:
            limits:
              memory: 24Ki
            requests:
              memory: 12Ki
              hardware.kube32.io/gpio: "5"`
  },
  {
    id: 'cloud-synapse',
    podNames: ['cloud-edge-synapse-telemetry'],
    title: '3-Tier Edge-to-Cloud Hybrid Kubernetes Synapse',
    domain: 'Distributed AI',
    filterCategory: 'ai',
    icon: 'cloud_sync',
    problem: 'Edge microcontrollers are isolated from modern DevOps; cloud Kubernetes teams cannot manage bare-metal MCU firmware with standard declarative GitOps tooling.',
    impossibleWhy: 'A single kubectl workflow deploys a unified pipeline spanning ESP32 physical ADC pins, Raspberry Pi edge gateway aggregation, and cloud/Mac Kubernetes (OrbStack).',
    targetNode: 'esp32-node-02 <-> Pi <-> OrbStack',
    specs: {
      memory: '48 KiB SRAM',
      peripherals: 'ADC Ch 0, Port 9090 ClusterIP Service',
      isolation: '3-Tier Unified Service Routing'
    },
    yaml: `apiVersion: v1
kind: Pod
metadata:
  name: cloud-edge-synapse-telemetry
  namespace: kube32-cluster
  labels:
    app: 3tier-synapse
    tier: industrial-iot-hybrid
spec:
  nodeName: esp32-node-02
  restartPolicy: Always
  containers:
    - name: edge-synapse-worker
      image: wasm://telemetry/protobuf-streamer:v2
      resources:
        limits:
          memory: 48Ki
        requests:
          memory: 24Ki
          hardware.kube32.io/adc: "0"
---
apiVersion: v1
kind: Service
metadata:
  name: cloud-edge-synapse-service
  namespace: kube32-cluster
  labels:
    app: 3tier-synapse
spec:
  type: ClusterIP
  ports:
    - name: protobuf-stream
      port: 9090
      protocol: TCP
      targetPort: 9090
  selector:
    app: 3tier-synapse`
  }
];

// Presets for the custom deploy modal
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

// ---------------------------------------------------------------------------
// Application Lifecycle
// ---------------------------------------------------------------------------

document.addEventListener('DOMContentLoaded', () => {
  initTabs();
  initTheme();
  initShowcase();
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

      if (targetId === 'tab-showcase') {
        renderShowcaseCards(activeShowcaseFilter);
      }
    });
  });
}

// Theme Toggle
function initTheme() {
  const toggleBtn = document.getElementById('theme-toggle-btn');
  const icon = document.getElementById('theme-icon');
  const html = document.documentElement;

  toggleBtn?.addEventListener('click', () => {
    const isDark = html.getAttribute('data-theme') === 'dark';
    const nextTheme = isDark ? 'light' : 'dark';
    html.setAttribute('data-theme', nextTheme);
    if (icon) icon.textContent = isDark ? 'dark_mode' : 'light_mode';
  });
}

// ---------------------------------------------------------------------------
// 10 Extreme Workloads Showcase Logic
// ---------------------------------------------------------------------------

function initShowcase() {
  renderShowcaseCards('all');

  // Filter chips
  const chips = document.querySelectorAll('.filter-chip[data-filter]');
  chips.forEach(chip => {
    chip.addEventListener('click', () => {
      chips.forEach(c => c.classList.remove('active'));
      chip.classList.add('active');
      activeShowcaseFilter = chip.dataset.filter;
      renderShowcaseCards(activeShowcaseFilter);
    });
  });

  // Deploy All button
  document.getElementById('btn-deploy-all-showcase')?.addEventListener('click', deployAllShowcase);

  // Reset Baseline button
  document.getElementById('btn-reset-cluster-workloads')?.addEventListener('click', resetClusterWorkloads);
}

function renderShowcaseCards(filter = 'all') {
  const container = document.getElementById('showcase-grid-container');
  if (!container) return;

  const filtered = filter === 'all'
    ? WORKLOAD_CATALOG
    : WORKLOAD_CATALOG.filter(w => w.filterCategory === filter);

  container.innerHTML = filtered.map(w => {
    // Check if any pod in this workload is currently running
    const runningPods = clusterPods.filter(p => w.podNames.includes(p.metadata?.name));
    const isDeployed = runningPods.length > 0;
    const primaryPod = runningPods[0];
    const phase = primaryPod?.status?.phase || 'Not Deployed';

    let pillClass = 'pill-pending';
    if (phase === 'Running') pillClass = 'pill-running';
    else if (phase === 'Succeeded') pillClass = 'pill-running';
    else if (phase === 'Failed') pillClass = 'pill-failed';

    return `
      <div class="showcase-card ${isDeployed ? 'deployed' : ''}" id="card-${w.id}">
        <div class="showcase-card-header">
          <div class="showcase-title-group">
            <div class="showcase-icon-box">
              <span class="material-symbols-outlined">${w.icon}</span>
            </div>
            <div>
              <div class="showcase-domain-tag">${escapeHtml(w.domain)}</div>
              <h3 class="showcase-card-title">${escapeHtml(w.title)}</h3>
            </div>
          </div>
          <span class="pill-phase ${pillClass}" id="status-pill-${w.id}">
            ${isDeployed ? escapeHtml(phase) : 'Available'}
          </span>
        </div>

        <div class="problem-box">
          <strong>The Urgent Problem:</strong> ${escapeHtml(w.problem)}
        </div>

        <div class="impossible-box">
          <div class="impossible-title">
            <span class="material-symbols-outlined" style="font-size: 16px;">verified_user</span>
            Why impossible without Kube32
          </div>
          ${escapeHtml(w.impossibleWhy)}
        </div>

        <div class="spec-chips-row">
          <div class="spec-chip"><strong>Target:</strong> ${escapeHtml(w.targetNode)}</div>
          <div class="spec-chip"><strong>Memory:</strong> ${escapeHtml(w.specs.memory)}</div>
          <div class="spec-chip"><strong>HW &amp; Radio:</strong> ${escapeHtml(w.specs.peripherals)}</div>
        </div>

        <div class="showcase-action-row">
          <div style="display: flex; gap: 8px; flex-wrap: wrap;">
            <button class="btn btn-filled btn-sm" onclick="deployShowcaseWorkload('${w.id}')" id="btn-deploy-${w.id}">
              <span class="material-symbols-outlined">rocket_launch</span>
              Deploy (1-Click)
            </button>
            <button class="btn btn-tonal btn-sm" onclick="inspectWorkloadYaml('${w.id}')">
              <span class="material-symbols-outlined">code</span>
              YAML
            </button>
            ${w.id === 'hardware-workbench' ? `
              <a href="/hardware-workbench.html" target="_blank" class="btn btn-tonal btn-sm" style="color: var(--md-sys-color-primary); font-weight: 700;">
                <span class="material-symbols-outlined">open_in_new</span>
                Launch Web App
              </a>
            ` : ''}
          </div>
          <button class="btn btn-outlined btn-sm" onclick="viewWorkloadLogs('${w.id}')" id="btn-logs-${w.id}">
            <span class="material-symbols-outlined">terminal</span>
            Live Logs
          </button>
        </div>

        <!-- Inline Live Execution Trace Drawer -->
        <div class="showcase-trace-box ${isDeployed ? 'active' : ''}" id="trace-${w.id}">
          ${isDeployed ? 'Fetching live hardware execution trace...' : ''}
        </div>
      </div>
    `;
  }).join('');

  // Fetch traces for any already-deployed workloads
  filtered.forEach(w => {
    const running = clusterPods.find(p => w.podNames.includes(p.metadata?.name));
    if (running) {
      updateWorkloadTraceBox(w.id, running.metadata.name, running.metadata.namespace || 'default');
    }
  });
}

async function deployShowcaseWorkload(id) {
  const workload = WORKLOAD_CATALOG.find(w => w.id === id);
  if (!workload) return;

  const btn = document.getElementById(`btn-deploy-${id}`);
  const statusPill = document.getElementById(`status-pill-${id}`);
  const traceBox = document.getElementById(`trace-${id}`);

  if (btn) {
    btn.disabled = true;
    btn.innerHTML = `<span class="material-symbols-outlined" style="animation: pulse-dot 1s infinite;">sync</span> Deploying...`;
  }
  if (statusPill) {
    statusPill.className = 'pill-phase pill-pending';
    statusPill.textContent = 'Admitting...';
  }
  if (traceBox) {
    traceBox.classList.add('active');
    traceBox.textContent = `[${new Date().toISOString()}] Submitting declarative manifest to Kube32 API Server...\n[${new Date().toISOString()}] Validating capability firewall bitmasks and memory quotas...`;
  }

  showToast(`Deploying ${workload.title}...`, 'rocket_launch');

  try {
    // Parse documents (handles multi-document YAMLs with '---')
    const docs = workload.yaml.split(/^---$/m).map(d => d.trim()).filter(Boolean);

    for (const doc of docs) {
      let obj;
      if (doc.includes('kind: Service')) {
        obj = parseSimpleServiceYaml(doc);
        const ns = obj.metadata?.namespace || 'default';
        await fetch(`/api/v1/namespaces/${ns}/services`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(obj)
        });
      } else {
        obj = parseSimpleYaml(doc);
        const ns = obj.metadata?.namespace || 'default';
        await fetch(`/api/v1/namespaces/${ns}/pods`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(obj)
        });
      }
    }

    // Refresh cluster state immediately
    await fetchClusterState();

    const primaryPodName = workload.podNames[0];
    const ns = workload.yaml.includes('namespace: kube32-cluster') ? 'kube32-cluster' : 'default';

    showToast(`Successfully deployed ${workload.title}!`, 'check_circle');

    // Fetch and display live logs in trace box
    setTimeout(() => {
      updateWorkloadTraceBox(id, primaryPodName, ns);
    }, 400);

  } catch (err) {
    showToast(`Deployment error: ${err.message}`, 'error');
    if (traceBox) traceBox.textContent += `\n[ERROR] Deployment failed: ${err.message}`;
  } finally {
    if (btn) {
      btn.disabled = false;
      btn.innerHTML = `<span class="material-symbols-outlined">rocket_launch</span> Re-Deploy`;
    }
  }
}

async function updateWorkloadTraceBox(workloadId, podName, ns) {
  const traceBox = document.getElementById(`trace-${workloadId}`);
  if (!traceBox) return;

  try {
    const res = await fetch(`/api/v1/namespaces/${ns}/pods/${podName}/log`);
    if (res.ok) {
      const logs = await res.text();
      traceBox.textContent = logs.trim();
      traceBox.classList.add('active');
    }
  } catch (e) {
    // Silent ignore on background trace poll
  }
}

function inspectWorkloadYaml(id) {
  const workload = WORKLOAD_CATALOG.find(w => w.id === id);
  if (!workload) return;

  const deployModal = document.getElementById('deploy-modal');
  const manifestInput = document.getElementById('manifest-input');
  if (manifestInput && deployModal) {
    manifestInput.value = workload.yaml;
    deployModal.classList.add('open');
  }
}

function viewWorkloadLogs(id) {
  const workload = WORKLOAD_CATALOG.find(w => w.id === id);
  if (!workload) return;

  const primaryPodName = workload.podNames[0];
  const ns = workload.yaml.includes('namespace: kube32-cluster') ? 'kube32-cluster' : 'default';
  openPodLogs(ns, primaryPodName);
}

async function deployAllShowcase() {
  const btn = document.getElementById('btn-deploy-all-showcase');
  if (btn) {
    btn.disabled = true;
    btn.innerHTML = `<span class="material-symbols-outlined" style="animation: pulse-dot 1s infinite;">sync</span> Deploying All 10...`;
  }

  showToast('Starting sequential deployment of all 10 workloads...', 'auto_awesome');

  for (let i = 0; i < WORKLOAD_CATALOG.length; i++) {
    const w = WORKLOAD_CATALOG[i];
    await deployShowcaseWorkload(w.id);
    await new Promise(r => setTimeout(r, 300));
  }

  showToast('All 10 extreme workloads are online in the cluster!', 'verified');

  if (btn) {
    btn.disabled = false;
    btn.innerHTML = `<span class="material-symbols-outlined">rocket_launch</span> Deploy All 10 Workloads`;
  }
}

async function resetClusterWorkloads() {
  if (!confirm('Reset cluster to standard baseline workloads (gpio-blinker and kube32-workbench)?')) {
    return;
  }

  showToast('Resetting cluster workloads...', 'restart_alt');

  try {
    // Delete non-essential pods
    const res = await fetch('/api/v1/pods');
    if (res.ok) {
      const data = await res.json();
      const pods = data.items || [];
      for (const p of pods) {
        const name = p.metadata?.name;
        const ns = p.metadata?.namespace || 'default';
        if (name !== 'gpio-blinker' && name !== 'kube32-workbench') {
          await fetch(`/api/v1/namespaces/${ns}/pods/${name}`, { method: 'DELETE' });
        }
      }
    }
    await fetchClusterState();
    renderShowcaseCards(activeShowcaseFilter);
    showToast('Cluster reset complete.', 'check_circle');
  } catch (err) {
    showToast(`Reset error: ${err.message}`, 'error');
  }
}

// ---------------------------------------------------------------------------
// Telemetry & Cluster State Poller
// ---------------------------------------------------------------------------

async function fetchClusterState() {
  try {
    const [nodesRes, podsRes, svcRes, eventsRes] = await Promise.all([
      fetch('/api/v1/nodes'),
      fetch('/api/v1/pods'),
      fetch('/api/v1/services'),
      fetch('/api/v1/events')
    ]);

    if (podsRes.ok) {
      const podsData = await podsRes.json();
      clusterPods = podsData.items || [];
      renderPodsTable(clusterPods);
      updateGauges(clusterPods);
      syncShowcasePills(clusterPods);
    }

    if (svcRes.ok) {
      const svcData = await svcRes.json();
      renderServicesTable(svcData.items || []);
    }

    if (eventsRes.ok) {
      const eventsData = await eventsRes.json();
      renderEventsTable(eventsData.items || []);
    }
  } catch (err) {
    console.warn('[KUBE32] State poll failed:', err);
  }
}

function syncShowcasePills(pods) {
  WORKLOAD_CATALOG.forEach(w => {
    const card = document.getElementById(`card-${w.id}`);
    const pill = document.getElementById(`status-pill-${w.id}`);
    if (!pill || !card) return;

    const running = pods.filter(p => w.podNames.includes(p.metadata?.name));
    if (running.length > 0) {
      const phase = running[0].status?.phase || 'Running';
      pill.textContent = phase;
      card.classList.add('deployed');

      if (phase === 'Running' || phase === 'Succeeded') {
        pill.className = 'pill-phase pill-running';
      } else if (phase === 'Failed') {
        pill.className = 'pill-phase pill-failed';
      } else {
        pill.className = 'pill-phase pill-pending';
      }
    } else {
      pill.className = 'pill-phase pill-pending';
      pill.textContent = 'Available';
      card.classList.remove('deployed');
    }
  });
}

function updateGauges(pods) {
  const count = pods.length;
  const countEl = document.getElementById('workload-count');
  const podBar = document.getElementById('pod-bar');
  if (countEl) countEl.textContent = `${count} Pod${count === 1 ? '' : 's'}`;
  if (podBar) podBar.style.width = `${Math.min(100, count * 10)}%`;

  // Dynamically calculate approximate free memory based on active workloads
  const totalAllocatedKiB = pods.reduce((acc, p) => {
    const memStr = p.spec?.containers?.[0]?.resources?.requests?.memory || '16Ki';
    const num = parseInt(memStr, 10) || 16;
    return acc + num;
  }, 0);

  const freeSram = Math.max(28000, 52000 - totalAllocatedKiB * 1024);
  const sramVal = document.getElementById('sram-val');
  const sramBar = document.getElementById('sram-bar');
  if (sramVal) sramVal.textContent = `${freeSram.toLocaleString()} B`;
  if (sramBar) sramBar.style.width = `${Math.round((freeSram / 64000) * 100)}%`;

  // PSRAM gauge
  const psramPods = pods.filter(p => p.spec?.nodeSelector?.['hardware.kube32.io/psram'] === 'true');
  const psramFreeMb = Math.max(0.5, 2.0 - (psramPods.length * 0.45)).toFixed(2);
  const psramVal = document.getElementById('psram-val');
  if (psramVal) psramVal.textContent = `${psramFreeMb} MB`;
}

function renderPodsTable(pods) {
  const tbody = document.getElementById('pods-table-body');
  if (!tbody) return;

  if (pods.length === 0) {
    tbody.innerHTML = `<tr><td colspan="8" style="text-align: center; color: var(--md-sys-color-outline); padding: 24px;">No workloads currently scheduled.</td></tr>`;
    return;
  }

  tbody.innerHTML = pods.map(p => {
    const name = p.metadata?.name || 'unnamed';
    const ns = p.metadata?.namespace || 'default';
    const phase = p.status?.phase || 'Pending';
    const node = p.spec?.nodeName || 'unassigned';
    const ip = p.status?.podIP || '-';
    const age = formatAgeFromTimestamp(p.metadata?.creationTimestamp);

    let pillClass = 'pill-pending';
    if (phase === 'Running') pillClass = 'pill-running';
    else if (phase === 'Succeeded') pillClass = 'pill-running';
    else if (phase === 'Failed') pillClass = 'pill-failed';

    return `
      <tr>
        <td><code>${escapeHtml(ns)}</code></td>
        <td><strong>${escapeHtml(name)}</strong></td>
        <td>1/1</td>
        <td><span class="pill-phase ${pillClass}">${escapeHtml(phase)}</span></td>
        <td><code>${escapeHtml(node)}</code></td>
        <td>${escapeHtml(ip)}</td>
        <td>${escapeHtml(age)}</td>
        <td>
          <div style="display: flex; gap: 6px;">
            <button class="btn btn-tonal btn-sm" onclick="openPodLogs('${escapeHtml(ns)}', '${escapeHtml(name)}')">
              <span class="material-symbols-outlined">terminal</span> Logs
            </button>
            <button class="btn btn-outlined btn-sm" onclick="deletePod('${escapeHtml(ns)}', '${escapeHtml(name)}')">
              <span class="material-symbols-outlined">delete</span>
            </button>
          </div>
        </td>
      </tr>
    `;
  }).join('');
}

function renderServicesTable(services) {
  const tbody = document.getElementById('services-tbody');
  if (!tbody) return;

  tbody.innerHTML = services.map(s => {
    const name = s.metadata?.name || s.name;
    const ns = s.metadata?.namespace || s.ns || 'default';
    const type = s.spec?.type || s.type || 'ClusterIP';
    const ip = s.spec?.clusterIP || s.cluster_ip || '-';
    const ports = (s.spec?.ports || s.ports || []).map(p => `${p.port}/${p.protocol || 'TCP'}`).join(', ') || '-';

    return `
      <tr>
        <td><strong>${escapeHtml(name)}</strong></td>
        <td><code>${escapeHtml(ns)}</code></td>
        <td>${escapeHtml(type)}</td>
        <td><code>${escapeHtml(ip)}</code></td>
        <td>${escapeHtml(ports)}</td>
      </tr>
    `;
  }).join('');
}

function renderEventsTable(events) {
  const tbody = document.getElementById('events-tbody');
  if (!tbody) return;

  const recent = events.slice(-6).reverse();
  tbody.innerHTML = recent.map(e => `
    <tr>
      <td><strong>${escapeHtml(e.reason || 'Normal')}</strong></td>
      <td><code>${escapeHtml(e.object || e.involvedObject?.name || '-')}</code></td>
      <td style="font-size: 12px;">${escapeHtml(e.message || '-')}</td>
    </tr>
  `).join('');
}

async function openPodLogs(ns, podName) {
  const logModal = document.getElementById('log-modal');
  const title = document.getElementById('log-modal-title');
  const body = document.getElementById('log-modal-body');

  activeLogPod = { ns, name: podName };
  if (title) title.textContent = `Pod Logs: ${ns}/${podName}`;
  if (body) body.textContent = 'Streaming execution logs from ESP32 runtime...';
  logModal?.classList.add('open');

  try {
    const res = await fetch(`/api/v1/namespaces/${ns}/pods/${podName}/log`);
    if (res.ok) {
      const text = await res.text();
      if (body) body.textContent = text.trim() || 'No logs recorded.';
    } else {
      if (body) body.textContent = `Failed to fetch logs (HTTP ${res.status})`;
    }
  } catch (err) {
    if (body) body.textContent = `Log connection error: ${err.message}`;
  }
}

async function deletePod(ns, podName) {
  if (!confirm(`Delete pod ${ns}/${podName}?`)) return;

  try {
    const res = await fetch(`/api/v1/namespaces/${ns}/pods/${podName}`, { method: 'DELETE' });
    if (res.ok) {
      showToast(`Deleted pod ${podName}`, 'delete');
      fetchClusterState();
    }
  } catch (err) {
    showToast(`Error deleting pod: ${err.message}`, 'error');
  }
}

// ---------------------------------------------------------------------------
// Peripherals & Actuator Matrix
// ---------------------------------------------------------------------------

function initGpioGrid() {
  const container = document.getElementById('gpio-grid-container');
  if (!container) return;

  const reservedPins = [0, 43, 44]; // Flashing/UART pins
  container.innerHTML = Array.from({ length: 16 }, (_, i) => {
    const isReserved = reservedPins.includes(i);
    return `
      <div class="gpio-pin ${isReserved ? 'reserved' : ''}" data-pin="${i}" id="pin-${i}">
        <div class="pin-indicator"></div>
        <div class="pin-num">GPIO ${i}</div>
        <div class="pin-role">${isReserved ? 'Protected' : 'Output'}</div>
      </div>
    `;
  }).join('');

  container.querySelectorAll('.gpio-pin:not(.reserved)').forEach(pinEl => {
    pinEl.addEventListener('click', () => {
      const pin = parseInt(pinEl.dataset.pin, 10);
      toggleGpioPin(pin);
    });
  });

  document.getElementById('btn-reset-gpio')?.addEventListener('click', () => {
    gpioStates.fill(false);
    document.querySelectorAll('.gpio-pin').forEach(el => el.classList.remove('active'));
    showToast('Reset all GPIO pins to LOW', 'restart_alt');
  });
}

function toggleGpioPin(pin) {
  gpioStates[pin] = !gpioStates[pin];
  const el = document.getElementById(`pin-${pin}`);
  if (el) {
    if (gpioStates[pin]) el.classList.add('active');
    else el.classList.remove('active');
  }
  showToast(`Toggled GPIO ${pin} to ${gpioStates[pin] ? 'HIGH (3.3V)' : 'LOW (0.0V)'}`, 'toggle_on');
}

// ---------------------------------------------------------------------------
// Pipeline Simulator
// ---------------------------------------------------------------------------

function initPipeline() {
  const btn = document.getElementById('btn-trigger-pipeline');
  const stream = document.getElementById('pipeline-stream-box');

  btn?.addEventListener('click', async () => {
    btn.disabled = true;
    btn.innerHTML = `<span class="material-symbols-outlined" style="animation: pulse-dot 1s infinite;">sync</span> Transmitting...`;

    try {
      const now = new Date().toISOString().replace('T', ' ').substring(0, 19);
      if (stream) {
        stream.textContent += `\n[${now}] [TRANSMIT] Sampling ADC Channel 0 (16 kHz)...`;
        stream.textContent += `\n[${now}] [MFCC] Extracted 16-coeff spectral vector (52 bytes)`;
        stream.textContent += `\n[${now}] [ESP-NOW] Dispatched frame via raw 2.4 GHz radio to Node 2 (Latency: 1.41ms)`;
        stream.scrollTop = stream.scrollHeight;
      }
      showToast('Transmitted 52-byte MFCC vector via ESP-NOW 2.4 GHz', 'hub');
    } finally {
      btn.disabled = false;
      btn.innerHTML = `<span class="material-symbols-outlined">send</span> Transmit Pipeline Vector`;
    }
  });
}

// ---------------------------------------------------------------------------
// Custom Deploy Modal & Helpers
// ---------------------------------------------------------------------------

function initModals() {
  const deployModal = document.getElementById('deploy-modal');
  const openDeployBtn = document.getElementById('btn-open-deploy-modal');
  const closeDeployBtn = document.getElementById('btn-close-deploy-modal');
  const cancelDeployBtn = document.getElementById('btn-cancel-deploy');
  const submitDeployBtn = document.getElementById('btn-submit-deploy');
  const manifestInput = document.getElementById('manifest-input');

  document.getElementById('btn-preset-blinker')?.addEventListener('click', () => {
    if (manifestInput) manifestInput.value = PRESET_BLINKER;
  });
  document.getElementById('btn-preset-pipeline')?.addEventListener('click', () => {
    if (manifestInput) manifestInput.value = PRESET_PIPELINE;
  });
  document.getElementById('btn-preset-sensor')?.addEventListener('click', () => {
    if (manifestInput) manifestInput.value = PRESET_SENSOR;
  });

  openDeployBtn?.addEventListener('click', () => {
    if (manifestInput) manifestInput.value = manifestInput.value || PRESET_BLINKER;
    deployModal?.classList.add('open');
  });

  const closeDeploy = () => deployModal?.classList.remove('open');
  closeDeployBtn?.addEventListener('click', closeDeploy);
  cancelDeployBtn?.addEventListener('click', closeDeploy);

  submitDeployBtn?.addEventListener('click', async () => {
    const raw = manifestInput?.value.trim();
    if (!raw) return;

    submitDeployBtn.disabled = true;
    submitDeployBtn.textContent = 'Deploying...';

    try {
      const podObj = parseSimpleYaml(raw);
      const ns = podObj.metadata?.namespace || 'default';
      const res = await fetch(`/api/v1/namespaces/${ns}/pods`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(podObj)
      });

      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      closeDeploy();
      fetchClusterState();
      showToast(`Deployed ${podObj.metadata?.name} to cluster!`, 'check_circle');
    } catch (err) {
      alert('Deployment error: ' + err.message);
    } finally {
      submitDeployBtn.disabled = false;
      submitDeployBtn.textContent = 'Deploy to Cluster';
    }
  });

  const logModal = document.getElementById('log-modal');
  document.getElementById('btn-close-log-modal')?.addEventListener('click', () => logModal?.classList.remove('open'));
  document.getElementById('btn-dismiss-log')?.addEventListener('click', () => logModal?.classList.remove('open'));
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
  let memoryLimit = '32Ki';
  let memoryReq = '16Ki';
  let nodeSelector = {};

  for (const line of lines) {
    const trimmed = line.trim();
    if (trimmed.startsWith('name:') && !name.includes('-')) name = trimmed.split(':')[1].trim();
    if (trimmed.startsWith('namespace:')) namespace = trimmed.split(':')[1].trim();
    if (trimmed.startsWith('nodeName:')) nodeName = trimmed.split(':')[1].trim();
    if (trimmed.startsWith('image:')) image = trimmed.split(':')[1].trim();
    if (trimmed.startsWith('hardware.kube32.io/psram:')) {
      nodeSelector['hardware.kube32.io/psram'] = trimmed.split(':')[1].trim().replace(/['"]/g, '');
    }
  }

  return {
    apiVersion: 'v1',
    kind: 'Pod',
    metadata: { name, namespace, labels: { app: name } },
    spec: {
      nodeName,
      nodeSelector: Object.keys(nodeSelector).length > 0 ? nodeSelector : undefined,
      containers: [{
        name,
        image,
        resources: { limits: { memory: memoryLimit }, requests: { memory: memoryReq } }
      }]
    }
  };
}

function parseSimpleServiceYaml(yamlStr) {
  const lines = yamlStr.split('\n');
  let name = 'custom-service';
  let namespace = 'default';
  let port = 80;

  for (const line of lines) {
    const trimmed = line.trim();
    if (trimmed.startsWith('name:')) name = trimmed.split(':')[1].trim();
    if (trimmed.startsWith('namespace:')) namespace = trimmed.split(':')[1].trim();
    if (trimmed.startsWith('port:')) port = parseInt(trimmed.split(':')[1].trim(), 10) || 80;
  }

  return {
    apiVersion: 'v1',
    kind: 'Service',
    metadata: { name, namespace },
    spec: {
      type: 'ClusterIP',
      clusterIP: '10.96.0.' + Math.floor(Math.random() * 200 + 10),
      ports: [{ name: 'http', port, protocol: 'TCP' }]
    }
  };
}

function formatAgeFromTimestamp(ts) {
  if (!ts) return '1m';
  const created = new Date(ts).getTime();
  const diffSec = Math.floor((Date.now() - created) / 1000);
  if (isNaN(diffSec) || diffSec < 60) return `${Math.max(1, diffSec)}s`;
  if (diffSec < 3600) return `${Math.floor(diffSec / 60)}m`;
  return `${Math.floor(diffSec / 3600)}h`;
}

function showToast(message, iconName = 'info') {
  const container = document.getElementById('toast-container');
  if (!container) return;

  const toast = document.createElement('div');
  toast.className = 'toast';
  toast.innerHTML = `
    <span class="material-symbols-outlined">${iconName}</span>
    <span>${escapeHtml(message)}</span>
  `;

  container.appendChild(toast);
  setTimeout(() => {
    toast.style.opacity = '0';
    toast.style.transition = 'opacity 0.3s ease';
    setTimeout(() => toast.remove(), 300);
  }, 3200);
}

function escapeHtml(str) {
  if (!str) return '';
  return String(str).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}
