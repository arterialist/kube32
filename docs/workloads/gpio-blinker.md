# Workload: GPIO peripheral blinker

<div class="blueprint-card">
  <strong>Manifest</strong>: <code>examples/gpio-blinker.yaml</code><br>
  <strong>Tested capability</strong>: <code>GPIO_WRITE</code> verified by capability firewall.<br>
  <strong>Target node</strong>: Scheduled to Node 2 by memory headroom.
</div>

This guide walks through deploying a WebAssembly container that toggles a physical GPIO output pin under the supervision of the Kube32 capability firewall.

---

## Workload manifest

```yaml
apiVersion: v1
kind: Pod
metadata:
  name: gpio-blinker
  namespace: default
  labels:
    app: blinker
    environment: edge
spec:
  nodeSelector:
    kubernetes.io/arch: xtensa
  containers:
    - name: blinker-task
      image: wasm://embedded/blinker:v1
      resources:
        limits:
          memory: 32Ki
          cpu: 50m
        requests:
          memory: 16Ki
          hardware.kube32.io/gpio: "1"
      securityContext:
        capabilities:
          add:
            - GPIO_WRITE
```

---

## Scheduling and execution

```mermaid
graph TD
    YAML["examples/gpio-blinker.yaml"] --> CLI["kube32-cli apply"]
    CLI --> SCHED["Scheduler on Core 0"]
    SCHED -->|LeastAllocated scoring| BIND["Bind to esp32-node-02"]
    BIND --> KUBELET["Worker agent on Node 2"]
    
    subgraph Execution["Node 2 physical execution"]
        FW["Capability firewall"]
        WASM["Wasm3 sandbox (32 KB memory)"]
        PIN["Physical GPIO 10 (LED)"]
        
        KUBELET --> FW
        FW -->|Audit passed: GPIO_WRITE allowed| WASM
        WASM -->|esp32_gpio_write(10, 1)| PIN
    end
```

---

## Deploying the workload

Apply the manifest using `kube32-cli` over the USB serial interface:

```bash
./cli/kube32-cli --port /dev/cu.usbmodem2101 apply -f examples/gpio-blinker.yaml
```

Output:
```
pod/gpio-blinker applied successfully -> scheduled to esp32-node-02 (HTTP 201)
```

Check the active pod status:

```bash
./cli/kube32-cli --port /dev/cu.usbmodem2101 status
```

Output:
```
=== CLUSTER PODS ===
NAMESPACE    NAME                     NODE             STATUS       MEMORY
------------------------------------------------------------------------
default      gpio-blinker             esp32-node-02    Running      32Ki
```

---

## Verifying firewall protection

To confirm that unauthorized pin toggles are blocked, submit a modified manifest that omits the `GPIO_WRITE` capability:

```yaml
# unprivileged-pod.yaml
apiVersion: v1
kind: Pod
metadata:
  name: unprivileged-task
  namespace: default
spec:
  containers:
    - name: exploit-task
      image: wasm://embedded/blinker:v1
      # securityContext has no capabilities granted
```

Apply the pod and monitor Node 2 serial output:

```
FIREWALL-SECURITY: Violation in pod default/unprivileged-task
  Requested action: GPIO_WRITE on pin 10
  Verdict: REJECTED (Missing capability: GPIO_WRITE)
  Action: Container halted with exit code 139
```

The capability firewall blocks the write before any register change occurs. The ESP32-S3 kernel and other pods continue running normally.
