# Kube32

<div class="blueprint-card">
  <strong>Target</strong>: ESP32-S3 microcontroller cluster running FreeRTOS.<br>
  <strong>Design</strong>: Kubernetes control plane and WebAssembly runtime in 47 KB of RAM.<br>
  <strong>API</strong>: Kubernetes Core API v1 and Apps v1 over HTTP and serial.
</div>

Kube32 runs a Kubernetes-compatible control plane and a WebAssembly runtime directly on ESP32-S3 microcontrollers. It does not run Linux. It provides declarative scheduling, memory sandboxing, peripheral access controls, and direct node-to-node messaging on real silicon.

If you work with microcontrollers, Kube32 lets you deploy code as small WebAssembly binaries instead of reflashing entire firmware images every time logic changes. If you work with Kubernetes or edge clusters, Kube32 lets you use familiar tools like `kubectl` and standard Pod manifests to manage microcontrollers as real nodes.

<div class="metric-grid">
  <div class="metric-card">
    <div class="metric-value">47 KB</div>
    <div class="metric-label">RAM used out of 320 KB</div>
  </div>
  <div class="metric-card">
    <div class="metric-value">1.59 MB</div>
    <div class="metric-label">Firmware binary size</div>
  </div>
  <div class="metric-card">
    <div class="metric-value">&lt; 15 ms</div>
    <div class="metric-label">Cold container start time</div>
  </div>
  <div class="metric-card">
    <div class="metric-value">0 B</div>
    <div class="metric-label">Heap allocations on query path</div>
  </div>
</div>

---

## How it works

The cluster splits into a master control plane node and one or more worker nodes.

```mermaid
graph TD
    CLI["kube32-cli or kubectl"] -->|"HTTP :6443 or USB serial"| API

    subgraph Node1["Node 1: Control plane"]
        API["HTTP API server on port 6443"]
        STORE[("In-memory radix store")]
        SCHED["Scheduler with LeastAllocated scoring"]
        NLC["Node lifecycle controller"]
        KUBELET1["Local Kubelet agent"]
        WASM1["Wasm3 runtime"]
        FW1["Capability firewall for GPIO and ADC"]

        API <--> STORE
        SCHED <--> STORE
        NLC <--> STORE
        KUBELET1 <--> API
        KUBELET1 --> WASM1
        WASM1 --> FW1
    end

    subgraph Air["2.4 GHz wireless link"]
        WIFI["802.11 SoftAP network"]
        ESPNOW["ESP-NOW virtual channels"]
    end

    subgraph Node2["Node 2: Worker node"]
        KUBELET2["Worker Kubelet agent"]
        WASM2["Wasm3 runtime in external PSRAM"]
        FW2["Capability firewall"]
        TASK["Running container workload"]

        KUBELET2 --> WASM2
        WASM2 --> FW2
        WASM2 --> TASK
    end

    API <-->|"192.168.4.0/24 SoftAP"| WIFI
    WIFI <-->|"Wi-Fi station"| KUBELET2
    FW1 <-->|"K32C frames"| ESPNOW
    ESPNOW <-->|"Named data channel"| FW2
```

---

## What makes up the system

### In-memory radix store
Standard Kubernetes uses etcd, which requires several gigabytes of memory and fast disks. Kube32 stores all cluster state in an in-memory prefix tree inside SRAM. Read requests use a bump-allocated memory arena to avoid heap churn. Prefix scans for `/registry/pods/` or `/registry/nodes/` complete in microseconds.

### Autonomous scheduler
The scheduler runs every second on Core 0. It checks resource limits, matches node labels, and applies a LeastAllocated scoring algorithm. Workloads that request more memory than a board has available stay in a Pending state.

### Wasm3 runtime sandbox
Workloads run as WebAssembly bytecode rather than native ELF binaries. The interpreter restricts guest execution to a bounded memory block, typically 32 KB to 64 KB. If guest code crashes or divides by zero, the runtime catches the trap and terminates the container. The underlying ESP32-S3 kernel and other tasks continue running uninterrupted.

### Hardware capability firewall
Microcontrollers often share pins across multiple buses. A bug in user code could accidentally trigger an actuator or write to a flash line. Kube32 intercepts hardware calls at the runtime boundary. Containers only get access to specific GPIO pins or ADC channels if their manifest explicitly lists the matching capability in `securityContext`.

### ESP-NOW virtual channel IPC
Instead of opening heavy TCP sockets across Wi-Fi for sensor data, Kube32 routes inter-pod messages over raw ESP-NOW frames. Packets carry a 22-byte header with a CRC16-CCITT checksum and 32-bit hashes for channel names. Latency between boards stays under 3 milliseconds.

---

## Hardware setup

```mermaid
graph TD
    subgraph Host["Developer workstation"]
        CLI["kube32-cli / kubectl"]
    end

    subgraph Cluster["Bare-metal ESP32-S3 cluster"]
        subgraph Node1["Node 1: Control plane (esp32-node-01)"]
            direction TB
            N1_HW["ESP32-S3 (512 KB SRAM, 8 MB Flash)"]
            N1_ROLE["API server, scheduler, radix store"]
            N1_IP["IP: 192.168.4.1 (SoftAP host)"]
            N1_HW --- N1_ROLE
            N1_ROLE --- N1_IP
        end

        subgraph Node2["Node 2: Worker node (esp32-node-02)"]
            direction TB
            N2_HW["ESP32-S3 (512 KB SRAM, 2 MB - 8 MB PSRAM)"]
            N2_ROLE["Worker agent, Wasm3 VM, firewall"]
            N2_IP["IP: 192.168.4.2 (Wi-Fi station)"]
            N2_HW --- N2_ROLE
            N2_ROLE --- N2_IP
        end
    end

    CLI -->|"Serial link (USB-CDC / USB-UART)"| Node1
    CLI -.->|"Wi-Fi REST on port 6443 (optional)"| Node1
    Node1 <==>|"Wi-Fi SoftAP (Kube32-Cluster 192.168.4.0/24)"| Node2
    Node1 <==>|"ESP-NOW 2.4 GHz virtual channel IPC (&le; 3 ms)"| Node2
```
