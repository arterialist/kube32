# Wasm3 Runtime Sandbox

<div class="blueprint-card">
  <strong>Execution Engine</strong>: Wasm3 Universal WebAssembly Interpreter<br>
  <strong>Linear Memory Sandboxing</strong>: 32KB - 64KB bounded pages with hardware fault trap handler<br>
  <strong>Host Call Boundary</strong>: Capability-gated host bindings via <code>runtime/host_functions.cpp</code>
</div>

Kube32 replaces native Linux container binaries (`elf`, `runc`) with sandboxed WebAssembly (Wasm) modules executed via the embedded `Wasm3` interpreter engine.

---

## Isolation Architecture

```mermaid
graph TD
    subgraph Host["ESP32-S3 Physical Host (FreeRTOS)"]
        SYS["FreeRTOS Kernel & Core Registers"]
        HEAP["System Heap (3FCA_7310)"]
        RADIO["802.11 / ESP-NOW Hardware"]
        PINS["Physical GPIO & ADC Silicon"]

        subgraph Sandbox["Wasm3 Isolated Execution Container"]
            GUEST_MEM["Linear Memory Pages<br/>(Max 64KB bounded)"]
            INTERP["M3 Runtime Interpreter"]
            STACK["Operand Execution Stack"]
        end

        GATE["HAL Capability Firewall<br/>Host Function Interceptor"]

        Sandbox <==>|"Trapped Host Calls Only"| GATE
        GATE --> PINS
        GATE --> RADIO
    end

    SYS -.->|"Strictly Inaccessible"| Sandbox
    HEAP -.->|"Strictly Inaccessible"| Sandbox
```

---

## Host Function Linkage & Interception

Guest WebAssembly modules interact with the physical world exclusively through registered host functions. Every host call passes through `hal::CapabilityFirewall`:

```cpp
// Registered host call signatures in Wasm3
m3_LinkRawFunction(module, "env", "esp32_gpio_write",   "i(ii)",   &raw_esp32_gpio_write);
m3_LinkRawFunction(module, "env", "esp32_gpio_read",    "i(i)",    &raw_esp32_gpio_read);
m3_LinkRawFunction(module, "env", "esp32_adc_read",      "i(i)",    &raw_esp32_adc_read);
m3_LinkRawFunction(module, "env", "esp32_channel_send", "i(iiii)", &raw_esp32_channel_send);
m3_LinkRawFunction(module, "env", "esp32_channel_recv", "i(iiii)", &raw_esp32_channel_recv);
```

### Host Function Execution Pipeline

```mermaid
sequenceDiagram
    autonumber
    participant Guest as Wasm Micro-Container
    participant W3 as Wasm3 Interpreter
    participant Gate as Capability Firewall
    participant HAL as Physical ESP32 Hardware

    Guest->>W3: call esp32_gpio_write(pin=10, val=1)
    W3->>Gate: Intercept syscall (pod="default/gpio-blinker")
    Gate->>Gate: Audit Capability: GPIO_WRITE granted?
    alt Capability Granted & Pin Permitted
        Gate->>HAL: gpio_set_level(GPIO_NUM_10, 1)
        HAL-->>Gate: ESP_OK
        Gate-->>W3: Return status = 0
        W3-->>Guest: Return 0 (Success)
    else Capability Missing or Unauthorized Pin
        Gate-->>W3: Return error = -1 (EACCES)
        W3-->>Guest: Return -1
        Note over Gate: Log Security Audit Violation
    end
```

---

## Memory Sandboxing & Fault Containment

### 1. Pointer Boundary Clamping
In WebAssembly, all memory access occurs within a contiguous 64KB page array. Addresses are indexed from `0` to `linear_memory_size - 1`. The interpreter clamps or traps any offset outside this buffer:
- Guest code cannot read host memory pointers.
- Guest code cannot modify FreeRTOS task control blocks (TCBs).

### 2. Trap Handlers & Host Survival
If guest code panics (e.g., division by zero, stack overflow, or memory out-of-bounds):
1. Wasm3 raises a execution trap (`m3Err_trapOutOfBoundsMemory`).
2. The `kubelet` worker catches the trap exception.
3. The pod phase transitions to `Failed` (`CrashLoopBackOff`).
4. **The ESP32 host kernel continues running normally** without rebooting or corrupting cluster state.
