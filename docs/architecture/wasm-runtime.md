# Wasm runtime

[wasm3](https://github.com/wasm3/wasm3), an interpreter in `third_party/wasm3`, executes pods as WebAssembly modules. Each pod gets isolated linear memory and reaches the node through host functions in the `env` module. The ABI is in `include/runtime/k32_app_abi.h`. See [Writing app pods](../app-development.md) for guest examples.

The build disables exception handling, typed function references, stack switching, snapshots, and backtraces via `-Dd_m3*` flags in `platformio.ini` and `CMakeLists.txt`.

## Pod kinds

| Kind | Exports | Execution |
| --- | --- | --- |
| Request pod | `k32_alloc`, `k32_handle`, and optionally `k32_init`, `k32_tick`, `k32_event` | Runs on the node's shared interpreter stack (64 KiB in PSRAM, or 28 KiB of internal RAM on boards without PSRAM). Calls are serialized across the node. `k32_init` runs once at creation, and a non-zero return fails the pod. |
| Entry-point pod | `_start`, `app_main`, or `main` | Runs on a dedicated pod thread until the function returns. Requires a board with PSRAM. |

## Images

The kubelet resolves `spec.containers[0].image` in order:

1. `wasm://sd/<name>[:<tag>]` reads `<card>/kube32/images/<name>[-<tag>].wasm` from the [storage](../storage.md).
2. `wasm://embedded/<name>` or any other reference matches `images/*.wasm` in the embedded firmware registry by the last path component before `:` and parses modules in place from flash.
3. The `kube32.io/wasm-base64` pod annotation, written by `tools/kube32-cli deploy`.

Unresolved images or images too large for node memory fail the pod with `ErrImagePull`.

## Memory

- Linear memory is capped by `limits.memory` or `requests.memory`, raised to `esp32.io/psram` if larger, with a 64 KiB default. Limits below `64Ki` cap the backing of the first Wasm page to that byte limit; `memory.grow` past the limit returns -1 and accesses past it trap.
- At boot, PSRAM nodes reserve one 1,284 KiB PSRAM block (`20 * 64 KiB + 4 KiB` header) and one shared 64 KiB interpreter stack in PSRAM. Nodes without PSRAM reserve one 80 KiB internal RAM block, which holds the shared 28 KiB interpreter stack.
- On boards without PSRAM, the pool block holds one large linear memory (>= 48 KiB and at least half the block) from its start and places smaller pod allocations (wasm3 runtime, compiled code, small linear memories) from its end. Every pod allocation comes from the pool block first and falls back to the general heap only when the block is full, except while a pod whose linear memory needs >= 48 KiB loads, when its runtime structures go to the general heap so the block stays whole. The general heap stays for the kubelet, Wi-Fi, and lwIP, with 24 KiB reserved for the system (16 KiB on boards with PSRAM).
- The per-pod wasm3 value stack defaults to 4 KiB when the pod memory limit is 64 KiB or less and 16 KiB above. The pod annotation `kube32.io/wasm-stack` sets it between `512` and `64Ki`. C and Rust locals use a shadow stack in linear memory.
- Every function is compiled when the pod loads, and the unused end of the last 4 KiB code page goes back to the pool block. A module that runs out of memory while it loads or compiles fails to load instead of failing in a later call.
- Pod allocations never take the system reserve of the internal heap. A pod that needs more fails to load, or traps with `OOMKilled` when `memory.grow` or a later allocation is refused.

## Memory admission

Before a pod starts, the kubelet estimates the memory it needs to load: value stack, linear memory, compiled code (about 3.2 times the code section on a 32-bit board), module structures, host-side state and the compiler's scratch. It compares the estimate with what pods can still be given: the free part of the pool block, the general heap above the system reserve, and PSRAM above a 16 KiB stack reserve. On a board without PSRAM only the pool block counts: free general heap is what Wi-Fi and lwIP need in a burst.

| Outcome | Pod status | Event |
| --- | --- | --- |
| Fits | Started | `Pulled`, `Created`, `Started` |
| Fits once other pods free their memory | `Pending`, container waiting with reason `InsufficientMemory`; admitted again every 5 s | `InsufficientMemory` (Warning), once |
| Larger than the node can give with no other pod running | `Failed` with reason `OutOfmemory`, as the Kubernetes kubelet rejects a pod that does not fit | `OutOfmemory` (Warning) |

A pod that passes admission but runs out of memory while it loads also waits with `InsufficientMemory` instead of crash looping. A pod deleted and created again under the same name replaces the old container even when the kubelet did not see the deletion.

## Threads

| Thread | Stack, with PSRAM / without PSRAM | Role |
| --- | --- | --- |
| `pod_<ns>/<name>` | 64 KiB PSRAM / not supported | Dedicated thread per entry-point pod (PSRAM boards only). |
| `k32_tick` | 8 KiB PSRAM / 4 KiB internal | Calls `k32_tick(uptime_ms)` at 1 Hz, or every 10 ms while a pod returns > 0. |
| `k32_events` | 8 KiB PSRAM / 4 KiB internal | Runs while any pod subscribes via `kube32.io/event-topics` and dispatches messages to `k32_event`. |

All host-initiated pod calls (`k32_init`, `k32_alloc`, `k32_handle`, `k32_tick`, `k32_event`) switch onto the shared interpreter stack (64 KiB in PSRAM, or 28 KiB of internal RAM without PSRAM) and run serialized across the node. `k32_init` runs from `k32_kubelet` (24 KiB PSRAM stack or 12 KiB internal stack), and `k32_handle` runs from the ingress worker or pod server thread. Run `K32_CMD:TASKS` to inspect native stack sizes and high-water marks.

## Execution budget

Every call into a pod (`k32_init`, `k32_alloc`, `k32_handle`, `k32_tick`, `k32_event`) starts with a fresh wasm3 gas budget, default 4,000,000,000 units. The pod annotation `kube32.io/execution-budget` sets the unit count, or `"off"` or `"0"` to disable metering. Unmetered pods run 25 to 60 percent faster, but their calls cannot be interrupted mid-call. Stopping a pod interrupts metered guest loops and wakes blocking host calls (`esp32_delay_ms`, `kube32_channel_recv`, `k32_tone`, `k32_http_call`).

## Traps and exit codes

Guest faults terminate the container and leave the node running. Container status messages include the wasm3 trap text.

| Condition or trap | Exit code | Reason |
| --- | --- | --- |
| `_start` returns, or `app_main` / `main` returns 0 | 0 | `Completed` |
| `app_main` / `main` returns non-zero `N` | `N` | `Error` |
| Module parse/load failure, `k32_init` non-zero, or no pod thread | 128 | `StartError` |
| Unsupported instruction | 132 | `Error` |
| `unreachable`, abort, or uncaught trap | 134 | `Error` |
| Unaligned atomic | 135 | `Error` |
| Integer division by zero, integer overflow, or bad conversion | 136 | `Error` |
| Allocation failure, `memory.grow` refused at the limit, or abort after refused `memory.grow` | 137 | `OOMKilled` |
| Stopped by the kubelet | 137 | `Error` |
| Out-of-bounds memory/table access, stack overflow, bad indirect call, or null reference | 139 | `Error` |
| Wasm value stack exhausted (`the module's stack of N bytes is exhausted (raise it with the annotation kube32.io/wasm-stack)`) | 139 | `Error` |
| Execution budget exhausted via `kube32.io/execution-budget` | 152 | `Error` |

## Restarts

When a container exits, the kubelet releases its memory, thread, open files, and firewall claims, then applies `restartPolicy`:

| `restartPolicy` | Exit code 0 | Non-zero exit or trap |
| --- | --- | --- |
| `Always`, default | restart | restart |
| `OnFailure` | pod `Succeeded` | restart |
| `Never` | pod `Succeeded` | pod `Failed` |

Restarts back off for 10 s, doubling up to 5 minutes, and reset to 10 s after 10 minutes of running. While waiting, the pod stays `Running` with the container in `CrashLoopBackOff`, `lastState` holding the exit code and reason, and a `BackOff` event. Each restart loads a fresh instance, increments `restartCount`, and rotates the container log so `kubectl logs --previous` serves the prior instance.

## Host functions

Every module links two function groups in the `env` module:

- `src/runtime/app_pod.cpp` links `k32_resp_begin`, `k32_resp_header`, `k32_resp_write`, `k32_resp_end`, `k32_resp_asset`, `k32_http_call`, `k32_event_emit`, `k32_time_ms`, `k32_log`, `k32_random`, `k32_env_get`, `k32_mem_stats`, `k32_display_text`, `k32_display_rect`, `k32_display_bar`, `k32_key_read`, `k32_tone`, and `k32_fs_open`/`read`/`write`/`close`/`size`/`list`/`remove`.
- `src/runtime/host_functions.cpp` links `esp32_gpio_set_mode`, `esp32_gpio_write`, `esp32_gpio_read`, `esp32_adc_read`, `esp32_i2c_write`, `esp32_i2c_read`, `esp32_uart_write`, `esp32_uart_read`, `esp32_get_time_us`, `esp32_delay_ms`, and `kube32_channel_open`/`send`/`recv`/`close`.

The [capability firewall](capability-firewall.md) gates hardware, storage, IPC, and `SERVICE_CALL` requests. Run `tools/kube32-cli check` to validate module imports and memory against the ABI.

## Limitations

- Liveness and readiness probes are not run by the kubelet.
- Without PSRAM, only one 80 KiB pool block, holding the shared 28 KiB interpreter stack, is reserved at boot. Entry-point (`app_main`) pods are not supported without PSRAM, and allocations beyond the pool block may fail once Wi-Fi fragments the internal heap.

