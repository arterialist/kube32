# CLI Reference

<div class="blueprint-card">
  <strong>Executable</strong>: <code>cli/kube32-cli</code><br>
  <strong>Communication Modes</strong>: Direct Silicon Serial (<code>--port</code>) &amp; HTTP REST API (<code>--server</code>)<br>
  <strong>Compatibility</strong>: Standard Kubernetes YAML / JSON Manifests
</div>

`kube32-cli` is the developer and operations command-line interface for physical Kube32 clusters. It provides two operational modes:

1. **Direct Silicon Serial Mode (`--port`)**: Sends requests over physical USB serial lines directly to the ESP32-S3 control plane (`/dev/cu.usbmodem...`). This mode operates without requiring the host machine to switch Wi-Fi networks off the internet.
2. **HTTP REST Mode (`--server`)**: Communicates with the embedded Kubernetes API Server at `http://192.168.4.1:6443` over the `Kube32-Cluster` Wi-Fi network.

---

## Global Options

| Flag | Short | Default | Description |
| :--- | :--- | :--- | :--- |
| `--port` | `-p` | None | Serial device path (e.g. `/dev/cu.usbmodem2101`) |
| `--server` | | `http://192.168.4.1:6443` | HTTP API Server endpoint |
| `--help` | `-h` | | Display command options and exit |

---

## Commands

### 1. `status`
Inspects the physical cluster topology, displaying registered nodes, readiness states, allocated memory, and active Pods:

```bash
./cli/kube32-cli --port /dev/cu.usbmodem2101 status
```

Output:
```
Connecting to Kube32 Cluster at /dev/cu.usbmodem2101...

=== CLUSTER NODES ===
NAME             STATUS     ROLES          INTERNAL-IP      ALLOCATABLE-MEM
------------------------------------------------------------------------
esp32-node-01    Ready      control-plane  192.168.4.1      400Ki
esp32-node-02    Ready      worker         192.168.4.2      2048Ki

=== CLUSTER PODS ===
NAMESPACE    NAME                     NODE             STATUS       MEMORY
------------------------------------------------------------------------
default      audio-preprocessor       esp32-node-01    Running      48Ki
default      keyword-classifier       esp32-node-02    Running      64Ki
```

---

### 2. `apply`
Applies a declarative workload manifest (single or multi-document YAML/JSON) to the cluster:

```bash
./cli/kube32-cli --port /dev/cu.usbmodem2101 apply -f examples/distributed-pipeline.yaml
```

Output:
```
pod/audio-preprocessor applied successfully -> scheduled to [esp32-node-01] (HTTP 201)
pod/keyword-classifier applied successfully -> scheduled to [esp32-node-02] (HTTP 201)
```

---

### 3. `delete`
Deletes a Pod from the cluster, reclaiming allocated resources and resetting peripheral claims:

```bash
./cli/kube32-cli --port /dev/cu.usbmodem2101 delete gpio-blinker --namespace default
```

Output:
```
pod "gpio-blinker" deleted
```

---

### 4. `pipeline`
Triggers an over-the-air ESP-NOW dataflow transmission from Stage 1 on Node 1 across physical 2.4GHz RF to Stage 2 on Node 2:

```bash
./cli/kube32-cli --port /dev/cu.usbmodem2101 pipeline
```

Output:
```
Triggering Stage 1 over-the-air ESP-NOW transmission on /dev/cu.usbmodem2101...
  [Node 1] [PIPELINE-OTA-TX] Transmitting Stage 1 MFCC features (52 bytes) over ESP-NOW to Node 2 (02:00:00:00:00:01)...
  [Node 1] [PIPELINE-OTA-TX] ESP-NOW transmission result: 0 (0 = OK)
```

---

### 5. `package`
Packages a compiled WebAssembly binary (`.wasm`) into a self-contained, base64-embedded Kubernetes Pod manifest:

```bash
./cli/kube32-cli package build/blinker.wasm \
  --name gpio-blinker \
  --memory 32Ki \
  --capabilities GPIO_WRITE \
  -o manifests/blinker-pod.yaml
```

---

### 6. `version`
Displays CLI and API compliance versions:

```bash
./cli/kube32-cli version
```

Output:
```
Kube32 CLI v1.0.0-m6 (Target: ESP32-S3 Xtensa dual-core @ 240MHz)
API Compliance: Kubernetes Core v1 / Apps v1
```
