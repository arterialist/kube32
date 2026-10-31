# Benchmarks

Measurements of what kube32 costs on the boards it runs on: the Wasm sandbox against native code, the memory a pod and the platform take, the control plane's CPU and latency, and the host simulator next to k3s. Every number on this page was measured on the hardware named here; the commands to repeat them are at the end.

## Setup

| | Control plane | Worker |
| --- | --- | --- |
| Board | ESP32-S3 devkit | M5Stack Cardputer (ESP32-S3FN8) |
| CPU | 2 cores at 240 MHz | 2 cores at 240 MHz |
| Memory | 512 KB SRAM, 2 MB quad PSRAM at 80 MHz | 512 KB SRAM, no PSRAM |
| Role | API server, store, controllers, kubelet | kubelet, three device drivers |

Firmware: ESP-IDF 5.5.3, built with `-O2` (`CONFIG_COMPILER_OPTIMIZATION_PERF`), 3,303,200 bytes in a 3,407,872-byte app slot, 55,252 bytes of static RAM. Pods: freestanding C compiled with clang `-Oz` to Wasm, run by kube32's wasm3 interpreter. Both boards were on the same 2.4 GHz Wi-Fi network as the client, a laptop.

## Wasm against native code

The same C source, `src/bench/bench_kernels.c`, is built into the firmware and into the bench pod (`apps/bench`). The firmware runs a kernel natively on the console task (`K32_CMD:BENCH`); the pod runs it in its sandbox and times it with the node's microsecond clock. Each time is the fastest of five runs; both sides return the same checksum for every kernel. "Metered" is a pod with the default execution budget (gas accounting on); "unmetered" sets `kube32.io/execution-budget: "off"`.

TABLES

- A pod runs compute 32 to 95 times slower than native code on the ESP32-S3. Gas metering adds up to about 45 % on top.
- Where the pod's memory lives makes little difference: the Cardputer runs pods from internal SRAM, the devkit from PSRAM, and the ratios are about the same.
- The interpreter is the cost. GCC for the ESP32-S3 makes no tail calls, so wasm3 dispatches every Wasm instruction with a native call and return instead of a jump.

What this means for pods: a pod is the right place for control logic, protocol handling and glue, not for number crunching. Work that needs native speed belongs in a host function, which a pod calls for about 1.5 µs:

| | Cardputer | devkit |
| --- | ---: | ---: |
| `esp_timer_get_time()` in firmware | 0.99 µs | 0.98 µs |
| `k32_time_ms()` from an unmetered pod (the same call through the sandbox) | 2.44 µs | 2.64 µs |
| Cost of crossing the sandbox | 1.45 µs | 1.66 µs |

The drivers follow this split: a driver reads registers over I2C and does its arithmetic in Wasm, while the I2C transfers, the bus lock and the multiplexer run natively.

### Native stack

Because every executed instruction keeps a native frame (48 to 64 bytes) until its function returns, a long straight run of instructions in one function takes native stack in proportion to its length. The shared interpreter stack is 28 KiB on a board without PSRAM, of which about 21.9 KB is available to a call; a call that needs more traps with `stack overflow` and reports how much it used. Two things measured on the Cardputer:

- The bench pod built with `-O2`, where clang inlines the query parsing and output helpers into one function, used 23.2 KB for a request that runs `fib(1)` and trapped; the same source built with `-Oz` and its helpers kept out of line answers every request.
- The BME688 driver's first reading, with its compensation formulas inlined into `k32_tick`, used 22 KB; with each compensation step a function of its own it runs well inside the limit.

Pods for boards without PSRAM should be built with `-Oz` and keep long computations in functions of their own.

## Memory

### A pod

| Pod | Linear memory | Taken on the Cardputer (pod pool) | Taken on the devkit |
| --- | ---: | ---: | ---: |
| bme688 driver | 3 KiB | 25,400 bytes | |
| miniscale driver | 3 KiB | 20,072 bytes | |
| rfid driver | 3 KiB | 23,536 bytes | |
| bench | 24 KiB | 42,976 bytes | 55 to 59 KB of PSRAM, 1 KB of SRAM |

What a pod takes is its linear memory, its compiled code (about three bytes per byte of Wasm code), its value stack and the interpreter's structures. On a board without PSRAM it all comes from an 80 KiB pod pool reserved at boot, beside about 1.5 KB of kubelet and runtime bookkeeping in the general heap; the Cardputer runs the three drivers above together with 12.9 KB of the pool to spare.

### The platform

Free memory at each stage of boot:

| Stage | Cardputer (SRAM) | devkit (SRAM) | devkit (PSRAM) |
| --- | ---: | ---: | ---: |
| After peripherals, pod pool and interpreter stack reserved | 161,028 | 269,811 | 673,864 |
| After Wi-Fi | 124,192 | 242,003 | 669,264 |
| Cluster services up | 59,396 | 10,531 to 74,935 | 434,128 |
| Steady, with the workloads above | 22,700 to 26,000 | 18,071 | 311,332 |

On the worker, the kubelet, the pod HTTP server, event delivery and discovery take about 65 KB of SRAM; the 80 KiB pool and the 28 KiB interpreter stack are reserved before that. On the control plane, the API server, object store, controllers and their buffers take about 235 KB of PSRAM. The devkit's SRAM figure at "cluster services up" varies between boots with what the API server is serving at that moment.

## CPU

Share of the two cores busy over 60 s, from FreeRTOS run-time counters (`K32_CMD:TASKS`):

| Node | Busy | Largest consumers |
| --- | ---: | --- |
| devkit, control plane with one Deployment and three Devices | 20.8 % | Deployment controller 10.0 % of a core, TCP/IP 7.1 %, Wi-Fi 6.4 %, Device controller 4.3 % |
| Cardputer, three drivers | 18.7 % | Wi-Fi 11.2 %, pod ticks 11.1 % (rfid polls five times a second), TCP/IP 7.1 % |
| Cardputer, no pods | 8.1 % | Wi-Fi 6.5 %, TCP/IP 4.1 %, kubelet 1.5 % |

The Deployment and Device controllers reconcile on a timer (every 1 s and 2 s) by reading the objects they manage; that polling is most of the control plane's idle load.

## Latency

Over Wi-Fi from a laptop on the same network, median (90th percentile) of 15 to 30 requests:

| Request | Time |
| --- | ---: |
| `GET /api/v1/pods` to the control plane | 148 ms (245 ms) |
| `kubectl get pods -A`, process start included | 211 ms (313 ms) |
| `kubectl create configmap` then `kubectl delete configmap` | 380 ms (572 ms) |
| A request to a pod and back, through the node's pod HTTP server | 37 ms (54 ms) on the devkit, 76 ms (118 ms) on the Cardputer |
| A pod from `kubectl apply` to scheduled, on the devkit | 1.1 s |
| A pod from `kubectl apply` to `Running`, on the devkit | 2.8 s |

## On a computer: kube32-sim and k3s

`kube32-sim` is the same control plane, kubelet and runtime built for macOS or Linux. Measured in ARM64 Linux containers (Ubuntu 24.04, 4 CPUs each) on an Apple M3 Pro, after three minutes idle, then with ten pods; memory is the summed resident set of the container's processes, with the cgroup's figure in brackets; CPU is the share of one core over 60 s:

| | Memory | CPU |
| --- | ---: | ---: |
| kube32-sim, control plane and one worker | 6.4 MiB (6.5 MiB) | 0.33 % |
| kube32-sim, ten Wasm pods | 10.3 MiB (15.7 MiB) | 0.33 % |
| k3s v1.33.1, default install | 918 MiB (910 MiB) | 3.21 % |
| k3s v1.33.1, without Traefik, ServiceLB, metrics-server, local storage, Helm controller | 574 MiB (407 MiB) | 2.17 % |
| the same k3s with ten pause containers | 764 MiB (506 MiB) | 2.29 % |

The two are not the same thing: k3s runs containers through containerd and implements all of Kubernetes, while kube32 runs Wasm modules in one process and implements the subset in [the API reference](api-reference.md). The comparison shows what a Kubernetes-style control plane costs when it is built for kilobytes; natively on macOS the simulator idles at 9.6 MiB and 0.17 % of a core.

## Repeating the measurements

Native kernels, on a node's serial console:

```
K32_CMD:BENCH:crc32:4:5         -> K32_BENCH kernel=crc32 n=4 runs=5 us_min=... us_total=... sum=...
K32_CMD:BENCH:hostcall:100000   -> K32_BENCH hostcall calls=100000 us=...
```

Kernels: `fib` (n = depth), `crc32` (n = KiB), `sieve` (n = limit, at most 4096), `sort` (n = words, at most 1024), `mandel` (n = grid side), `json` (n = records).

The same kernels in a pod:

```sh
make -C apps/bench
tools/kube32-cli deploy images/sd/bench.wasm --name bench --node <node> --memory 24Ki --hardware none
curl -H 'X-Kube32-Pod: default/bench' 'http://<node address>:7000/run?k=crc32&n=4&r=5'
curl -H 'X-Kube32-Pod: default/bench' 'http://<node address>:7000/hostcall?n=100000'
```

For an unmetered pod, add `--dry-run`, put `kube32.io/execution-budget: "off"` in the pod's annotations, and apply the manifest with kubectl. CPU shares come from two `K32_CMD:TASKS` listings taken 60 s apart (`run_ms` per task).
