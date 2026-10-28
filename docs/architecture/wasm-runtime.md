# Wasm runtime

Pods are WebAssembly modules executed by [wasm3](https://github.com/wasm3/wasm3),
an interpreter vendored in `third_party/wasm3`. The vendored copy carries
Kube32 changes, for example the allocator hooks in `m3_core.c`. It is built
with exception
handling, typed function references, stack switching, snapshots and
backtraces compiled out; see the `-Dd_m3*` flags in `platformio.ini` and
`CMakeLists.txt`. The interpreter gives each pod its own linear memory. A
guest can read and write only that memory, and it reaches the node only
through the host functions linked into the `env` module.

The contract between host and guest is `include/runtime/k32_app_abi.h`.
[Writing app pods](../app-development.md) covers it from the guest's side,
with examples in Rust, C and AssemblyScript.

## Two kinds of pod

| Kind | Exports | How it runs |
| --- | --- | --- |
| Request pod | `k32_alloc`, `k32_handle`; optionally `k32_init`, `k32_tick`, `k32_event` | Has no thread of its own. The ingress or pod HTTP server calls `k32_handle` once per request, on its own thread, serialized per pod. The tick thread calls `k32_tick`. Events arrive through `k32_event`. |
| Entry-point pod | `_start`, `app_main` or `main` | Runs on a thread of its own. The pod completes when the function returns. |

`k32_init` runs once when the container is created. A non-zero return fails
the pod.

## Images

The kubelet resolves `spec.containers[0].image` in this order:

1. **microSD card.** `wasm://sd/<name>[:<tag>]` is read from the card (see
   [microSD card](../sd-card.md)). If that fails, the pod fails with
   `ErrImagePull`; there is no fallback.
2. **Embedded registry.** The firmware's registry, built from `images/*.wasm`
   at compile time and looked up by the last path component before `:`.
   Modules there are parsed in place from flash.
3. **Inline annotation.** The `kube32.io/wasm-base64` annotation on the pod,
   which `tools/kube32-cli deploy` writes.

## Memory

- **Linear memory** is what the module declares, capped by the pod's memory
  limit. The limit is `limits.memory`, else `requests.memory`, raised to
  `esp32.io/psram` when that is larger, and at least 64 KiB. A module that
  declares more than its limit does not load. `memory.grow` past the limit
  returns -1 to the guest.
- **The large block.** Allocations of 48 KiB or more are served first from
  a single block reserved at boot: 67 KiB of internal RAM on a board without
  PSRAM, 1,340 KiB of PSRAM on a board with it. The block serves one pod at a
  time. A second large allocation goes to the general heap. On the
  Cardputer, once Wi-Fi was up, that heap was observed to have no contiguous
  48 KB block left.
- **Value stack.** wasm3's value stack is 16 KiB, or 4 KiB for pods whose
  limit is 64 KiB or less.
- **Shadow stack.** C and Rust locals live in linear memory, in a shadow
  stack the module's own linker sets up.

## Work limits

- **Per call.** Each `k32_handle` and `k32_event` call gets a budget of 400
  million wasm3 gas units. A call that runs out traps.
- **Other calls.** `k32_init`, `k32_tick` and entry points get no fresh
  budget.
- **Stopping a pod** forces the remaining gas negative, which ends a guest
  stuck in a loop at its next metered instruction.

## Faults

wasm3 checks every memory access and every trap condition, so a guest fault
ends that guest and leaves the node running.

| Trap | Reason reported |
| --- | --- |
| out-of-bounds memory access | `OOMKilled` (the name is misleading: it is an access fault, not memory exhaustion) |
| integer division by zero | exit code 136 |
| `unreachable` | exit code 134 |
| stack overflow | `StackOverflow` |
| out of gas (entry-point pods) | `ExecutionTimeout` |
| anything else | `RuntimeTrap` |

After a trap the kubelet reports the pod `Failed`. Nothing restarts it:
`restartPolicy` is not read, whatever the reason text says. A request pod
that trapped stays registered and is still called for later requests.

When an entry point returns normally, the kubelet reports the pod
`Succeeded` and `kubectl` shows `Completed`. The return value is not read.
On the simulator, a C `app_main` that returned 3 was reported as "exited with
code 0".

## Native stacks

Request pods run on the caller's thread, so the kubelet, tick, ingress and
event-dispatch threads are sized for interpreter recursion. On the Xtensa
build, wasm3's operation chain is not fully tail-call optimised. An entry-point
pod gets its own thread; on a board without PSRAM it runs on a 40 KiB stack
reserved at boot. `K32_CMD:TASKS` shows the current sizes and high-water
marks.

## Host functions

Two groups are linked into every module:

- `src/runtime/app_pod.cpp`: responses (`k32_resp_*`), `k32_log`, time,
  random, environment, memory statistics, `k32_event_emit`, display, keypad,
  tone, and the `k32_fs_*` file calls.
- `src/runtime/host_functions.cpp`: `esp32_gpio_*`, `esp32_adc_read`,
  `esp32_i2c_*`, `esp32_uart_*`, `esp32_delay_ms`, `esp32_get_time_us`, and
  `kube32_channel_*`.

The calls that touch hardware or other nodes are checked by the
[capability firewall](capability-firewall.md). `tools/kube32-cli check` holds
the full list of names and signatures. It rejects a module that imports
anything else.
