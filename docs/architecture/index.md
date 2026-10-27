# Architecture overview

<div class="blueprint-card">
  <strong>Design pattern</strong>: Distributed micro-kernel orchestrator for dual-core MCUs.<br>
  <strong>Concurrency</strong>: FreeRTOS SMP with pinned tasks on Xtensa LX7.<br>
  <strong>Inter-node networking</strong>: 802.11 SoftAP for TCP/IP and ESP-NOW for low-latency messaging.
</div>

Kube32 implements the Kubernetes separation between control plane and worker agent directly on dual-core microcontrollers, without Linux or container daemons.

---

## Dual-core task partitioning

The ESP32-S3 contains two Xtensa LX7 cores running at 240 MHz. FreeRTOS can schedule tasks across both cores, but Wi-Fi packet processing and serial interrupts require consistent latency. Kube32 pins networking, cluster state storage, and scheduling to Core 0. Core 1 runs the worker agent, guest WebAssembly bytecode, and peripheral access audits.

```mermaid
graph TB
    subgraph Core0["Xtensa LX7 Core 0: Network and control plane"]
        WIFI_DRIVER["ESP32 Wi-Fi driver task (priority 23)"]
        HTTP_SRV["k32_http: API server on port 6443 (stack: 12 KB)"]
        SCHED["k32_sched: Scheduler (stack: 8 KB)"]
        NLC["k32_nlc: Node lifecycle controller (stack: 4 KB)"]
        RADIX["Radix state store (internal SRAM)"]

        HTTP_SRV <--> RADIX
        SCHED <--> RADIX
        NLC <--> RADIX
    end

    subgraph Core1["Xtensa LX7 Core 1: Worker agent and runtime"]
        KUBELET["k32_kubelet: Worker agent (stack: 8 KB)"]
        WASM_VM["Wasm3 interpreter (stack: 16 KB)"]
        FIREWALL["Capability firewall for GPIO and ADC"]
        IPC_RX["ESP-NOW frame handler ring buffer"]

        KUBELET --> WASM_VM
        WASM_VM --> FIREWALL
        IPC_RX --> WASM_VM
    end

    Core0 <==>|"FreeRTOS queues and spinlocks"| Core1
```

### Core 0 subsystems

- **Wi-Fi driver**: Handles the 802.11 physical layer, generates SoftAP beacons, and routes inbound IP frames.
- **HTTP API server (`k32_http`)**: Accepts TCP connections on port 6443. Parses Kubernetes REST requests and routes them to the radix store. Uses a 12 KB task stack.
- **Scheduler (`k32_sched`)**: Evaluates unassigned pods every 1,000 ms. Disqualifies nodes that lack requested memory or hardware pins, then scores candidate nodes by available memory. Uses an 8 KB task stack.
- **Node lifecycle controller (`k32_nlc`)**: Monitors worker heartbeats. Marks a node as `NotReady` if heartbeats stop for 15 seconds, and begins pod eviction after 30 seconds. Uses a 4 KB task stack.

### Core 1 subsystems

- **Worker agent (`k32_kubelet`)**: Checks for newly scheduled pods assigned to this node, initializes sandboxes, and reports execution state back to the control plane. Uses an 8 KB task stack.
- **Wasm3 interpreter**: Executes guest WebAssembly modules inside a memory sandbox bounded between 32 KB and 64 KB. Uses a 16 KB task stack.
- **Capability firewall**: Intercepts guest host calls such as `esp32_gpio_write` and `esp32_adc_read`. Checks each call against permissions granted in the pod manifest.
- **ESP-NOW frame handler**: Inspects incoming radio frames, verifies CRC16 checksums, and pushes payloads into container ring buffers.

---

## State synchronization flow

When a client creates a pod, the control plane persists the manifest and schedules it to a qualified node. The worker node pulls the pod definition and starts the WebAssembly container.

```mermaid
sequenceDiagram
    autonumber
    participant Dev as Client (kube32-cli or kubectl)
    participant API as API server on port 6443
    participant Store as Radix state store
    participant Sched as Scheduler
    participant Kube as Worker agent (Node 2)
    participant Wasm as Wasm3 sandbox

    Dev->>API: POST /api/v1/namespaces/default/pods
    API->>Store: Put /registry/pods/default/app (Phase: Pending)
    API-->>Dev: HTTP 201 Created
    
    loop Every 1000 ms
        Sched->>Store: List /registry/pods/
        Sched->>Store: Evaluate nodes by available memory
        Sched->>Store: Bind pod to esp32-node-02
    end

    loop Every 2000 ms
        Kube->>API: GET /api/v1/pods
        API-->>Kube: Pod assigned to esp32-node-02
    end

    Kube->>Wasm: Instantiate module (memory limit: 64 KB)
    Kube->>Store: Patch pod status (Phase: Running)
    Wasm->>Wasm: Run container entrypoint
```

---

## Task memory and stack limits

| Task name | Core affinity | Stack size | Priority | Purpose |
| :--- | :--- | :--- | :--- | :--- |
| `k32_http` | Core 0 | 12 KB | 5 | Kubernetes REST API server |
| `k32_sched` | Core 0 | 8 KB | 4 | Pod scheduling and node scoring |
| `k32_nlc` | Core 0 | 4 KB | 3 | Node health and lease monitoring |
| `k32_kubelet` | Core 1 | 8 KB | 4 | Pod synchronization and container supervision |
| `wifi` | Core 0 | 6.5 KB | 23 | Espressif 802.11 PHY driver |
