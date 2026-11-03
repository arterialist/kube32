# Wasm runtime

[wasm3](https://github.com/wasm3/wasm3), an interpreter in `third_party/wasm3`, executes pods as WebAssembly modules. Each pod gets isolated linear memory and reaches the node through host functions in the `env` module. The ABI is in `include/runtime/k32_app_abi.h`. See [Writing app pods](../app-development.md) for guest examples.

The build disables exception handling, typed function references, stack switching, snapshots, and backtraces via `-Dd_m3*` flags in `platformio.ini` and `CMakeLists.txt`.

## Pod kinds

| Kind | Exports | Execution |
| --- | --- | --- |
| Request pod | `k32_alloc`, `k32_handle`, and optionally `k32_init`, `k32_tick`, `k32_event` | Runs on the node's shared interpreter stack (64 KiB in PSRAM, or 28 KiB of internal RAM on boards without PSRAM), or on its [pod worker's](#pod-workers) own stack. Calls into one pod are serialized. `k32_init` runs once at creation, and a non-zero return fails the pod. |
| Entry-point pod | `_start`, `app_main`, or `main` | Runs on a dedicated pod thread until the function returns. Requires a board with PSRAM. |

## Images

The kubelet resolves `spec.containers[0].image` in order:

1. `wasm://sd/<name>[:<tag>]` reads `<card>/kube32/images/<name>[-<tag>].wasm` from the [storage](../storage.md).
2. `wasm://embedded/<name>` or any other reference matches `images/*.wasm` in the embedded firmware registry by the last path component before `:` and parses modules in place from flash.
3. An inline image: the module the API server took out of the `kube32.io/wasm-base64` annotation (written by `kube32 deploy`) and keeps under the pod's `kube32.io/wasm-sha256`. The kubelet reads it from `GET /apis/kube32.io/v1/images/<sha256>` into one allocation of the module's size and checks it against the digest. See [Writing app pods](../app-development.md#inline-image-kube32-cli-deploy).

wasm3 reads a module's bytes in place for as long as the module is loaded. An embedded image is mapped from flash and takes no RAM. The bytes of any other image are pod memory: the kubelet reads them into one allocation, placed as the pod's other allocations are (the pool block first, see [Memory](#memory)), and the container keeps them for restarts and charges them to the pod. Thus a module needs no contiguous piece of the general heap.

Unresolved images, and card images over the board's size limit, fail the pod with `ErrImagePull`. A module that finds no room in pod memory makes the pod wait for memory, as a load does ([Memory admission](#memory-admission)).

## Memory

- Linear memory is capped by `limits.memory` or `requests.memory`, raised to `esp32.io/psram` if larger, with a 64 KiB default. Limits below `64Ki` cap the backing of the first Wasm page to that byte limit; `memory.grow` past the limit returns -1 and accesses past it trap.
- At boot, PSRAM nodes reserve one 1,284 KiB PSRAM block (`20 * 64 KiB + 4 KiB` header) and one shared 64 KiB interpreter stack in PSRAM. Nodes without PSRAM reserve one 80 KiB internal RAM block for pods and, apart from it, the shared 28 KiB interpreter stack.
- On boards without PSRAM, the pool block holds one large linear memory (>= 48 KiB and at least half the block) from its start and places smaller pod allocations (wasm3 runtime, compiled code, small linear memories, the bytes of a module that is not embedded) from its end. Every pod allocation comes from the pool block first and falls back to the general heap only when the block is full, except for a pod whose linear memory needs >= 48 KiB: while it loads, its runtime structures and its module's bytes go to the general heap first, so the block stays whole. The general heap stays for the kubelet, Wi-Fi, and lwIP, with 24 KiB reserved for the system (16 KiB on boards with PSRAM).
- The pod annotation `kube32.io/wasm-stack` sets the per-pod wasm3 value stack between `512` and `64Ki`. Without it, the stack equals what the module's heaviest call chain needs once every function is compiled (each frame as wasm3 compiled it, with an indirect call reaching every function of its type) plus 64 bytes. It is at most 4 KiB when the pod memory limit is 64 KiB or less and 16 KiB above. A module that recurses gets that maximum. C and Rust locals use a shadow stack in linear memory.
- The runtime compiles every function when the pod loads, onto 4 KiB code pages (smaller ones where the pool block has no 4 KiB piece left), and places each function's constants after its code. The unused end of the last page goes back to the pool block. A module that runs out of memory while it loads or compiles fails to load instead of failing in a later call.
- On boards without PSRAM, the shared interpreter stack lends one scratch area (about 6 KB) to the parser, validator, and compiler while a module loads. The pool block needs no room for this scratch area, and pod calls wait until the load ends.
- When a pod loads on a board without PSRAM, its console line `[POD] <pod>: ... pod pool <n> bytes (<free> free, largest <largest>)` reports what it took from the pool block, the block's free bytes, and its largest free piece. `K32_CMD:POOL` lists the block chunk by chunk.
- Pod allocations never take the system reserve of the internal heap. A pod that needs more fails to load, or traps with `OOMKilled` when `memory.grow` or a later allocation is refused.

## Memory admission

Before a pod starts, the kubelet estimates the memory it needs to load from the module's sections. This estimate covers the value stack (what its call chains need, unless the pod sets it), linear memory, compiled code (about 3.1 bytes per byte of code section on the 32-bit board), function and module structures, the module's bytes when the image is not embedded, the compiler's scratch where it is not lent, and the pod's host-side state where pods share the general heap. For an image the node has not read yet (from the card or the API server), the estimate counts the whole module as code section. The estimate counts wasm3's structures at their ESP32-S3 size in the simulator too, so both admit the same pods. The kubelet compares the estimate with what pods can be given now: the free part of the pool block, and on a board with PSRAM the PSRAM above its system reserve (128 KiB on a control plane, 64 KiB on a worker). A linear memory also has to fit in one piece: the pool block, or the PSRAM besides it when that is larger. The API server, object store, console relay, NAT, and firmware staging use this reserve. Pods' https calls use it too. On a 2 MB control plane, a TLS session takes about 30 KB of PSRAM for its handshake, plus a 12 KiB stack and an 8 KiB buffer for the response head. Control plane use peaks about 109 KiB above its boot level while staging a 3 MB image and serving lists and a watch. On a board without PSRAM, only the pool block counts because Wi-Fi and lwIP need the free general heap in a burst.

The Node reports what pods can be given with no pod running as its allocatable `memory` and `esp32.io/psram`, and what they can be given now in its `kube32.io/memory-free` annotation. Allocatable leaves out the node's system-reserved memory (`kube32.io/system-reserved` annotation, [memory figures](../hardware-spec.md#node-and-pod-memory-figures)), which holds the control plane's own allocations: 192 KiB of PSRAM on a control plane. When a pod is created, the API server sets its `spec.overhead.memory` (Pod Overhead) to the part of the estimate beyond the pod's linear memory: wasm3's structures, compiled code and value stack, and the module's bytes when the image is in its image store. It leaves out the compiler's scratch, which a pod holds only while its module loads, so the figure is the same whichever node computes it. It estimates from the image when the control plane has it (embedded, or in its image store); a pod with an image on a worker's card gets no overhead and the kubelet's admission counts it. The scheduler adds the overhead to the pod's requests (see [the scheduler](scheduler.md#filters)), so it binds pods on the figures the kubelet admits them on.

| Outcome | Pod status | Event |
| --- | --- | --- |
| Fits | Started | `Pulled`, `Created`, `Started` |
| Fits once other pods free their memory | `Pending`, container waiting with reason `InsufficientMemory`; admitted again every 5 s | `InsufficientMemory` (Warning), once |
| Larger than the node can give with no other pod running | `Failed` with reason `OutOfmemory`, as the Kubernetes kubelet rejects a pod that does not fit | `OutOfmemory` (Warning) |

A pod that passes admission but runs out of memory while the kubelet reads its module or loads it also waits with `InsufficientMemory` instead of failing or crash looping. On a board without PSRAM, the kubelet starts at most one pod in each sync pass (2 s). A pass keeps the manifest and the parsed form of each pod that it starts. Thus pods that are due at the same time, for example after a reboot, start one after the other. A pod deleted and created again under the same name replaces the old container even when the kubelet did not see the deletion.

## Threads

| Thread | Stack, with PSRAM / without PSRAM | Role |
| --- | --- | --- |
| `pod_<ns>/<name>` | 64 KiB PSRAM / not supported | Dedicated thread per entry-point pod (PSRAM boards only). |
| `k32_pods` (core 1) | 8 KiB PSRAM / none | Pod worker of core 1 on boards with PSRAM: calls the `k32_tick` and `k32_event` of the pods placed on it. |
| `k32_pods` (core 0) | 68 KiB of the pod block / none | Pod worker of core 0 on boards with PSRAM. Its stack holds the interpreter, so its calls run there and not on the shared stack. The stack comes from the pod block at boot, and allocatable memory leaves it out. Priority 3, below Wi-Fi, lwIP and the node's services on that core. |
| `k32_tick` | 8 KiB PSRAM with `K32_CMD:PODCORES:1` / 4 KiB internal | One worker only: calls `k32_tick(uptime_ms)` at 1 Hz, or every 10 ms while a pod returns > 0. |
| `k32_events` | 8 KiB PSRAM with `K32_CMD:PODCORES:1` / 4 KiB internal | One worker only: runs while any pod subscribes via `kube32.io/event-topics` and dispatches messages to `k32_event`. |

Host-initiated pod calls (`k32_init`, `k32_alloc`, `k32_handle`, `k32_tick`, `k32_event`) switch onto the shared interpreter stack (64 KiB in PSRAM, or 28 KiB of internal RAM without PSRAM), where they run one at a time. The exception is the pod worker of core 0, whose own 68 KiB stack holds the interpreter. `k32_init` runs from `k32_kubelet` (24 KiB PSRAM stack or 12 KiB internal stack), and `k32_handle` runs from the ingress worker or pod server thread. Run `K32_CMD:TASKS` to inspect native stack sizes and high-water marks.

### Pod workers

A board with PSRAM runs two pod workers, one per core. Each worker ticks the pods placed on it and delivers their events, so two busy pods on different workers run at the same time. Requests (`k32_handle`) still come from the ingress and pod server threads. Calls into one pod are serialized by its call lock, wherever they come from.

- **Placement.** The annotation `kube32.io/core: "0"` or `"1"` puts a pod on that core's worker. Without it (or with `"auto"`), a pod goes to the worker whose pods used the least CPU in their ticks and events over the last 2 s, and then to the worker with fewer pods.
- **Pods that call each other.** A pod that calls another pod of the node in-process (`k32_http_call` to a Service) brings the callee onto its own worker, and the two stay together from then on. This keeps the ticks of a pipeline's stages on one worker. On two workers, two stages that call each other from their ticks would each wait for the other's call lock until the call timed out.
- **Balance.** Every 2 s, the busier worker gives one group of auto-placed pods to the other worker when its pods used a quarter of the window more CPU and the move lowers the busier figure. A group that moved stays where it is for 10 s. The node logs every placement and move: `[POD] <namespace>/<name>: core <n> (<reason>)`.
- **Order.** Events reach each pod in the order they arrived, also across a move.

Boards without PSRAM keep one `k32_tick` thread and one `k32_events` thread on core 1, which leaves internal RAM for Wi-Fi. `K32_CMD:PODCORES:1` gives a board with PSRAM these two threads from its next boot on, and `K32_CMD:PODCORES:2` restores the two workers. `kube32-sim` runs two workers, or one with `--psram 0` or `--pod-cores 1`.

## CPU usage

`kubectl top pod` reports each container's CPU time over the kubelet's 5 s sample window. The runtime counts the thread CPU time of every call into the pod (`k32_init`, `k32_alloc`, `k32_handle`, `k32_tick`, `k32_event`) from the moment the call holds the pod's call lock, plus the CPU time of the pod's own thread when it has an entry point. On the boards the figure is the FreeRTOS run-time counter of the task (esp_timer microseconds). In the simulator it is the thread's CPU clock. Time a call spends blocked does not count: waiting for the pod's call lock or the shared interpreter stack, socket I/O in `k32_http_call`, `esp32_delay_ms`, and waits in channel receives. A pod that calls another pod of the same node in-process (`k32_http_call` to a Service) is charged for its own work only, and the callee for its work. `kubectl top node` reports the time both cores spend outside their idle tasks.

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

A node restart (a reboot, a crash, a firmware rollout) ends its containers, and the kubelet that comes up has no record of them. It reads the container's history from the pod's status: `restartCount` continues from the stored count, and a container that had run before (running, crash looping, or restarted earlier) starts again as a restart and increments it, as on a Kubernetes node that reboots. `startTime` keeps its first value. A container that was running when the node went down has no exit code; its `lastState` is `terminated` with reason `ContainerStatusUnknown`, exit code 137, and the `startedAt` it had, as the Kubernetes kubelet reports a container it lost. A crash-looping container keeps the `lastState` it had.

## Host functions

Every module links three function groups in the `env` module:

- `src/runtime/app_pod.cpp` links `k32_resp_begin`, `k32_resp_header`, `k32_resp_write`, `k32_resp_end`, `k32_resp_asset`, `k32_http_call`, `k32_http_status`, `k32_event_emit`, `k32_time_ms`, `k32_unix_time_ms`, `k32_log`, `k32_random`, `k32_env_get`, `k32_mem_stats`, `k32_display_text`, `k32_display_rect`, `k32_display_bar`, `k32_key_read`, `k32_tone`, and `k32_fs_open`/`read`/`write`/`close`/`size`/`list`/`remove`.
- `src/runtime/pod_linalg.cpp` links `k32_matvec_i8`, `k32_matvec_i4`, `k32_matmul_i8`, `k32_matmul_i4`, `k32_matvec_f32_i8`, `k32_vecmat_f32_i8`, `k32_softmax_f32`, `k32_rmsnorm_f32`, `k32_quantize_i8`, `k32_rope_f32`, `k32_swiglu_f32`, `k32_add_f32`, `k32_argmax_f32`, `k32_fft_q15`, `k32_add_f32_i8`, `k32_quantize_i8_static`, `k32_conv3x3_i8` and `k32_attention_f32_i8`, native kernels for quantized neural networks and audio front ends.
- `src/runtime/host_functions.cpp` links `esp32_gpio_set_mode`, `esp32_gpio_write`, `esp32_gpio_read`, `esp32_adc_read`, `esp32_chip_temp_read`, `esp32_i2c_write`, `esp32_i2c_read`, `esp32_uart_write`, `esp32_uart_read`, `esp32_rmt_send`, `esp32_rmt_receive`, `esp32_get_time_us`, `esp32_delay_ms`, and `kube32_channel_open`/`send`/`recv`/`close`.

The [capability firewall](capability-firewall.md) gates hardware, storage, IPC, and `SERVICE_CALL` requests. Run `kube32 check` to validate module imports and memory against the ABI.

## Limitations

- Liveness and readiness probes are not run by the kubelet.
- Without PSRAM, only one 80 KiB pool block, holding the shared 28 KiB interpreter stack, is reserved at boot. Entry-point (`app_main`) pods are not supported without PSRAM, and allocations beyond the pool block may fail once Wi-Fi fragments the internal heap.

