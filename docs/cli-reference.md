# CLI reference

<div class="blueprint-card">
  <strong>Command</strong>: <code>cli/kube32-cli</code><br>
  <strong>Connection modes</strong>: Direct silicon serial (<code>--port</code>) and HTTP REST (<code>--server</code>).<br>
  <strong>Manifest format</strong>: Standard Kubernetes YAML and JSON manifests.
</div>

`kube32-cli` is the command-line utility for managing Kube32 clusters. It supports two connection modes:

1. **Direct silicon serial mode (`--port`)**: Sends requests across USB serial lines directly to the ESP32-S3 control plane (`/dev/cu.usbmodem...`). This mode works without disconnecting your computer from its regular Wi-Fi network.
2. **HTTP REST mode (`--server`)**: Sends standard HTTP requests to port 6443 over the `Kube32-Cluster` Wi-Fi network.

---

## Global options

| Option | Short | Default | Purpose |
| :--- | :--- | :--- | :--- |
| `--port` | `-p` | None | Serial device path (such as `/dev/cu.usbmodem2101`) |
| `--server` | | `http://192.168.4.1:6443` | API server HTTP address |
| `--help` | `-h` | | Show help text and exit |

---

## Commands

### `status`
Queries cluster state, listing registered nodes, health statuses, free memory, and active pods:

```bash
./cli/kube32-cli --port /dev/cu.usbmodem2101 status
```

Output:
```
Connecting to Kube32 cluster at /dev/cu.usbmodem2101...

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

### `apply`
Submits a declarative workload manifest to the cluster:

```bash
./cli/kube32-cli --port /dev/cu.usbmodem2101 apply -f examples/distributed-pipeline.yaml
```

Output:
```
pod/audio-preprocessor applied successfully -> scheduled to esp32-node-01 (HTTP 201)
pod/keyword-classifier applied successfully -> scheduled to esp32-node-02 (HTTP 201)
```

---

### `delete`
Removes a pod from the cluster, freeing its memory and releasing hardware pins:

```bash
./cli/kube32-cli --port /dev/cu.usbmodem2101 delete gpio-blinker --namespace default
```

Output:
```
pod "gpio-blinker" deleted
```

---

### `pipeline`
Triggers an over-the-air ESP-NOW transmission from Stage 1 on Node 1 to Stage 2 on Node 2:

```bash
./cli/kube32-cli --port /dev/cu.usbmodem2101 pipeline
```

Output:
```
Triggering Stage 1 over-the-air ESP-NOW transmission on /dev/cu.usbmodem2101...
  Node 1: PIPELINE-OTA-TX: Transmitting Stage 1 MFCC features (52 bytes) over ESP-NOW to Node 2 (02:00:00:00:00:01)...
  Node 1: PIPELINE-OTA-TX: ESP-NOW transmission result: 0 (0 = OK)
```

---

### `package`
Encodes a compiled WebAssembly binary into a base64-embedded Kubernetes Pod manifest:

```bash
./cli/kube32-cli package build/blinker.wasm \
  --name gpio-blinker \
  --memory 32Ki \
  --capabilities GPIO_WRITE \
  -o manifests/blinker-pod.yaml
```

---

### `version`
Displays version information for the CLI and API server:

```bash
./cli/kube32-cli version
```

Output:
```
Kube32 CLI v1.0.0-m6 (Target: ESP32-S3 Xtensa dual-core at 240 MHz)
API compliance: Kubernetes Core v1 and Apps v1
```
