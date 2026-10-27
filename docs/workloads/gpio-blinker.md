# Workload: GPIO Peripheral Blinker

<div class="blueprint-card">
  <strong>Manifest</strong>: <code>examples/gpio-blinker.yaml</code><br>
  <strong>Tested Capabilities</strong>: <code>GPIO_WRITE</code> via Capability Firewall<br>
  <strong>Target Node</strong>: Scheduled to Node 2 (or Node 1 based on selector)
</div>

This tutorial demonstrates deploying a micro-container that securely toggles a physical GPIO pin under the governance of the Kube32 Hardware Capability Firewall.

---

## The Workload Manifest

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

## Execution Flow

```mermaid
graph TD
    YAML["examples/gpio-blinker.yaml"] --> CLI["kube32-cli apply"]
    CLI --> SCHED["Autonomous Scheduler"]
    SCHED -->|Evaluates Node 1 &amp; Node 2| BIND["Bind -> esp32-node-02"]
    BIND --> KUBELET["Kubelet on esp32-node-02"]
    
    subgraph Execution["Node 2 Physical Silicon"]
        FW["Capability Firewall"]
        WASM["Wasm3 VM (32KiB RAM)"]
        PIN["Physical GPIO 10 (LED)"]
        
        KUBELET --> FW
        FW -->|Audit Pass: GPIO_WRITE granted| WASM
        WASM -->|esp32_gpio_write(10, 1)| PIN
    end
```

---

## Deployment & Verification

Deploy the manifest using `kube32-cli`:

```bash
# Apply workload to the physical cluster
./cli/kube32-cli --port /dev/cu.usbmodem2101 apply -f examples/gpio-blinker.yaml
```

Output:
```
pod/gpio-blinker applied successfully -> scheduled to [esp32-node-02] (HTTP 201)
```

Query the active status:
```bash
./cli/kube32-cli --port /dev/cu.usbmodem2101 status
```

```
=== CLUSTER PODS ===
NAMESPACE    NAME                     NODE             STATUS       MEMORY
------------------------------------------------------------------------
default      gpio-blinker             esp32-node-02    Running      32Ki
```

---

## Capability Firewall Security Audit

To verify that unauthorized access is blocked, consider a modified manifest attempting to read from the system console or write to an ungranted pin:

```yaml
# MALICIOUS POD ATTEMPT
spec:
  containers:
    - name: exploit-task
      # No GPIO_WRITE capability in securityContext
      # Tries calling esp32_gpio_write(10, 1)
```

Output in Node 2 serial monitor:
```
[FIREWALL-SECURITY] Violation in pod default/exploit-task:
  Requested action: GPIO_WRITE on pin 10
  Verdict: REJECTED (Missing capability: GPIO_WRITE)
  Action: Container halted with SIGSEGV (Exit code 139)
```
The microcontroller kernel and all other containers remain entirely unaffected.
