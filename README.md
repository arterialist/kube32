# Kube32

Kube32 is an embedded Kubernetes-compatible orchestration system engineered specifically for ESP32 microcontrollers. It runs the full control plane (REST API server, scheduler, state storage, and node lifecycle controller) and worker agent (Kubelet with a sandboxed WebAssembly runtime) natively on microcontroller silicon. You interact with it directly using standard `kubectl`.

```
                    ┌──────────────────────────────────────────────┐
                    │               kubectl / Client               │
                    └──────────────────────┬───────────────────────┘
                                           │ HTTP/1.1 REST
                                           ▼
┌──────────────────────────────────────────────────────────────────────────────────────────┐
│                                ESP32 Control Plane Node                                  │
│                                                                                          │
│   ┌────────────────────┐     ┌─────────────────────┐     ┌───────────────────────────┐   │
│   │  HTTP API Server   │◄───►│ Radix State Store   │◄───►│ LittleFS Journal (Flash)  │   │
│   │  (Chunked Stream)  │     │ (Zero-Alloc Slabs)  │     │ (Atomic WAL + Recovery)   │   │
│   └─────────┬──────────┘     └──────────▲──────────┘     └───────────────────────────┘   │
│             │                           │                                                │
│             ▼                           ▼                                                │
│   ┌────────────────────┐     ┌─────────────────────┐     ┌───────────────────────────┐   │
│   │    Scheduler       │     │ Node Lifecycle Ctrl │     │ mDNS Discovery Provider   │   │
│   │ (Resource Scorer)  │     │ (Leases & Eviction) │     │ (_kube32-api._tcp.local)  │   │
│   └────────────────────┘     └─────────────────────┘     └───────────────────────────┘   │
└──────────────────────────────────────────┬───────────────────────────────────────────────┘
                                           │
                    ┌──────────────────────┴───────────────────────┐
                    │  Virtual Channel IPC (ESP-NOW / Peer UDP)    │
                    │  Sub-millisecond latency, zero AP overhead   │
                    └──────────────────────┬───────────────────────┘
                                           ▼
┌──────────────────────────────────────────────────────────────────────────────────────────┐
│                                 ESP32 Worker Node                                        │
│                                                                                          │
│   ┌────────────────────┐     ┌─────────────────────┐     ┌───────────────────────────┐   │
│   │      Kubelet       │◄───►│ Capability Firewall │◄───►│ Hardware HAL              │   │
│   │ (Pod & Lease Sync) │     │ (O(1) Pin/Bus Grants)│    │ (GPIO, I2C, SPI, ADC)     │   │
│   └─────────┬──────────┘     └─────────────────────┘     └───────────────────────────┘   │
│             │                                                                            │
│             ▼                                                                            │
│   ┌──────────────────────────────────────────────────────────────────────────────────┐   │
│   │                       Wasm3 Sandboxed Execution Runtime                          │   │
│   │      32 KB Stack | 64 KB Linear Memory | CPU Gas Quota | Syscall Intercept       │   │
│   └──────────────────────────────────────────────────────────────────────────────────┘   │
└──────────────────────────────────────────────────────────────────────────────────────────┘
```

## Key features

- **Self-contained embedded cluster**: Runs directly on ESP32 silicon with no external etcd, no Linux kernel, and no container daemon required.
- **Full `kubectl` compatibility**: Implements standard Kubernetes core endpoints (`/api/v1/pods`, `/api/v1/nodes`, `/api/v1/namespaces`, `/api/v1/services`, `/apis/coordination.k8s.io/v1/leases`). Native `meta.k8s.io/v1 Table` serialization formats wide tables directly in your terminal.
- **Ultra-low memory footprint**: Streams JSON responses using HTTP chunked transfer encoding with under 512 bytes of RAM overhead per connection. Custom slab pools and monotonic arenas eliminate heap fragmentation.
- **Sandboxed execution with hardware firewall**: Executes WebAssembly bytecode via an embedded Wasm3 engine. Pod manifests declare peripheral permissions (GPIO, I2C, SPI, UART, ADC) enforced at runtime with O(1) bitmask checks.
- **Distributed computing over virtual channels**: High-throughput, sub-millisecond inter-node IPC using raw 802.11 ESP-NOW action frames on hardware and peer UDP in simulation. Workloads too large for a single chip run as partitioned dataflow pipelines across connected boards.
- **Autonomous self-healing**: Node agents renew leases every 5 seconds. The controller marks nodes `NotReady` after 15 seconds without a heartbeat, evicting and rescheduling pods onto healthy nodes after 30 seconds.
- **Crash recovery**: State changes write to an append-only write-ahead journal on LittleFS flash with CRC32 verification and atomic snapshotting.

## Architecture

### API server and chunked streaming

Standard Kubernetes tooling expects JSON payloads that easily reach tens of kilobytes. On an ESP32 with 512 KB of SRAM, buffering whole payloads in memory triggers heap exhaustion.

Kube32 solves this by implementing zero-copy chunked HTTP/1.1 streaming. The serializer walks the state tree and writes objects into a fixed 256-byte output buffer, flushing chunks to the socket as soon as the buffer fills. This guarantees constant memory usage regardless of cluster size.

### Storage engine

Cluster state is stored in an in-memory radix tree indexed by resource key (for example, `/registry/pods/default/sensor-reader`). Modifying state appends a record to an on-flash write-ahead journal managed by LittleFS. On boot, Kube32 replays the journal, validates record CRC32 checksums, and rebuilds the radix tree in memory.

### Scheduler and node lifecycle controller

The autonomous scheduler monitors unassigned pods (`spec.nodeName == ""`). It scores candidate nodes using available memory, CPU gas capacity, and hardware peripheral availability.

The node lifecycle controller tracks heartbeats through `coordination.k8s.io/v1` Leases. If a node misses its lease renewal window, the controller marks it `NotReady` and coordinates failover to healthy nodes.

### Kubelet and sandboxed WebAssembly runtime

The Kubelet agent runs on worker nodes. It synchronizes assigned pods from the control plane and manages container lifecycles:

1. **Admission and hardware allocation**: The capability firewall inspects container resource requests and claims physical pins. If a requested pin is strapped or already reserved, admission is denied.
2. **Bytecode compilation**: The agent validates Wasm bytecode and sets memory bounds (32 KB stack, 64 KB maximum heap).
3. **Execution**: The runtime runs in a dedicated FreeRTOS task, enforcing CPU gas limits to prevent infinite loops from starving the network stack.

### Distributed computing model

Microcontrollers cannot run transparent Distributed Shared Memory (DSM). Fetching a remote variable over Wi-Fi takes 1.2 milliseconds (over 288,000 CPU cycles at 240 MHz), compared to 4 nanoseconds for local SRAM.

Instead, Kube32 implements coarse-grained pipeline dataflow over virtual channels (`kube32::ipc::Channel`). Pods communicate using four system calls linked into the Wasm environment:

- `kube32_channel_open(name, flags)`: Connects to a virtual channel by name.
- `kube32_channel_send(handle, data, len)`: Transmits an action frame to channel subscribers.
- `kube32_channel_recv(handle, buf, max_len, timeout_ms)`: Reads an incoming frame.
- `kube32_channel_close(handle)`: Closes the channel handle.

This allows multi-stage workloads (such as audio feature extraction on node 1 and dense neural classification on node 2) to maintain compute-to-communication ratios above 15:1.

## Quick start

### Prerequisites

- CMake 3.20 or newer
- C++17 compatible compiler (Clang or GCC)
- Python 3.9 or newer
- PlatformIO Core (for flashing physical ESP32 boards)

### Host simulator

You can build and run a multi-node cluster directly on your workstation using the POSIX host simulator.

```bash
# Clone the repository
git clone https://github.com/arterialist/kube32.git
cd kube32

# Configure and compile
cmake -B build -S .
cmake --build build -j
```

#### Running a standalone node

A standalone node runs both the control plane and a local Kubelet agent:

```bash
./build/src/kube32-sim --role standalone --port 6443
```

#### Running a distributed multi-node cluster

Run the control plane in one terminal:

```bash
./build/src/kube32-sim --role control-plane --port 6443 --ipc-port 7400
```

Join worker nodes from separate terminals:

```bash
# Start worker node 1
./build/src/kube32-sim --role worker --node-name esp32-node-01 --node-id 1 --join http://127.0.0.1:6443 --ipc-port 7401

# Start worker node 2
./build/src/kube32-sim --role worker --node-name esp32-node-02 --node-id 2 --join http://127.0.0.1:6443 --ipc-port 7402
```

### Interacting with kubectl

Point your local `kubectl` client to the Kube32 API server:

```bash
# Configure cluster context
kubectl config set-cluster kube32 --server=http://127.0.0.1:6443
kubectl config set-context kube32 --cluster=kube32
kubectl config use-context kube32

# Verify cluster nodes
kubectl get nodes -o wide
```

Output:

```text
NAME            STATUS   ROLES    AGE   VERSION   INTERNAL-IP   OS-IMAGE   KERNEL-VERSION   CONTAINER-RUNTIME
esp32-node-01   Ready    worker   42s   v1.28.0   127.0.0.1     freertos   v5.1.0           wasm3://1.0.0
esp32-node-02   Ready    worker   35s   v1.28.0   127.0.0.1     freertos   v5.1.0           wasm3://1.0.0
```

Deploy a sample workload:

```bash
kubectl apply -f examples/gpio-blinker.yaml
kubectl get pods -o wide
```

Output:

```text
NAME           READY   STATUS    RESTARTS   AGE   IP          NODE            NOMINATED NODE   READINESS GATES
gpio-blinker   1/1     Running   0          12s   127.0.0.1   esp32-node-01   <none>           <none>
```

## Workload manifests

### Peripheral access pod

Workloads request hardware access in `spec.containers.resources.requests` and `securityContext.capabilities`:

```yaml
apiVersion: v1
kind: Pod
metadata:
  name: gpio-blinker
  namespace: default
spec:
  nodeSelector:
    kubernetes.io/arch: xtensa
  containers:
    - name: blinker-task
      image: wasm://embedded/blinker:v1
      resources:
        limits:
          memory: 32Ki
        requests:
          memory: 16Ki
          hardware.kube32.io/gpio: "1"
      securityContext:
        capabilities:
          add:
            - GPIO_WRITE
```

### Distributed pipeline workload

Partitioned tasks exchange data across nodes using virtual channels:

```yaml
# Stage 1: Feature extractor scheduled to Node 1
apiVersion: v1
kind: Pod
metadata:
  name: audio-preprocessor
  namespace: default
spec:
  nodeSelector:
    kubernetes.io/hostname: esp32-node-01
  containers:
    - name: mfcc-extractor
      image: wasm://edge/mfcc-extractor:v1
      resources:
        requests:
          hardware.kube32.io/adc: "1"
      securityContext:
        capabilities:
          add:
            - ADC_READ
            - CHANNEL_IPC
      env:
        - name: KUBE32_IPC_CHANNEL
          value: "audio.features"
---
# Stage 2: Classifier scheduled to Node 2
apiVersion: v1
kind: Pod
metadata:
  name: keyword-classifier
  namespace: default
spec:
  nodeSelector:
    kubernetes.io/hostname: esp32-node-02
  containers:
    - name: tinyml-classifier
      image: wasm://edge/tinyml-classifier:v1
      resources:
        requests:
          hardware.kube32.io/gpio: "1"
      securityContext:
        capabilities:
          add:
            - GPIO_WRITE
            - CHANNEL_IPC
      env:
        - name: KUBE32_IPC_CHANNEL
          value: "audio.features"
```

## Physical hardware deployment

### Flashing an ESP32-S3

Kube32 includes a PlatformIO configuration (`platformio.ini`) targeting ESP32-S3 boards.

```bash
# Build firmware
pio run -e esp32s3

# Flash target node
pio run -e esp32s3 -t upload --upload-port /dev/cu.usbmodem2101

# View serial console logs
pio device monitor -b 115200
```

### Hardware pinout and safety masks

The capability firewall automatically blocks user workloads from touching critical system pins:

| Pin range | Function | Firewall enforcement |
| :--- | :--- | :--- |
| `GPIO 0` | Boot / Strapping pin | Protected. Workload allocations denied. |
| `GPIO 19 - 20` | Native USB D- / D+ | Protected for JTAG and serial console. |
| `GPIO 26 - 32` | Octal SPI Flash and PSRAM bus | Protected. Direct access blocked to prevent cache corruption. |
| `GPIO 43 - 44` | UART0 Debug Console | Protected for system logs. |
| `GPIO 1 - 18, 21, 38 - 42` | General purpose IO | Available for workload allocation via capability grants. |

## Automated testing

Kube32 contains both a C++ unit test suite and a Python end-to-end integration test runner.

### Running CTest suites

The project compiles with AddressSanitizer and UndefinedBehaviorSanitizer:

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

CTest validates 20 test targets, covering:
- OSAL threading, mutexes, and sockets
- Slab allocator bounds and double-free bitmask protections
- Radix tree concurrency and LittleFS journal recovery
- Chunked HTTP serialization under high connection loads
- Capability firewall denial rules and memory quotas
- Virtual channel reassembly and packet corruption handling

### Running the Python integration suite

The integration runner tests complete cluster workflows against simulated nodes:

```bash
python3 tests/runner.py --self-test
```

The test runner verifies:
- Node registration and lease maintenance
- Pod admission, state updates, and deletion
- Table formatting and wide output negotiation
- Node failure detection, eviction, and automated pod rescheduling
- Inter-node channel IPC transmission and fragment reassembly

## Repository structure

```text
├── CMakeLists.txt              # Host build and test configuration
├── platformio.ini              # ESP32-S3 PlatformIO hardware build configuration
├── sdkconfig.defaults          # ESP-IDF kernel settings and C++ exception flags
├── partitions.csv              # Flash partition map (App, LittleFS, NVS)
├── include/                    # Public subsystem headers
│   ├── api/                    # HTTP server and chunked Table serializers
│   ├── controller/             # Node lifecycle and lease controllers
│   ├── hal/                    # Hardware abstraction layer and capability firewall
│   ├── ipc/                    # Virtual channel IPC and packet reassembly
│   ├── kubelet/                # Node agent and API client
│   ├── mem/                    # Slab pools and monotonic arenas
│   ├── osal/                   # Portable OS abstraction layer (FreeRTOS / POSIX)
│   ├── runtime/                # Wasm3 runtime, host functions, and gas metering
│   └── storage/                # Radix state store and LittleFS write-ahead journal
├── src/                        # Subsystem implementations and host daemon
├── tests/                      # Unit test suites, adversarial fuzzers, and E2E runner
└── examples/                   # Kubernetes Pod manifests for embedded workloads
```

## License

Kube32 is open source software licensed under the [Apache License 2.0](LICENSE).
