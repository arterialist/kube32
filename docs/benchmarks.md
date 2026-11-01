# Benchmarks

This page gives what kube32 costs on its boards. It compares the Wasm sandbox with native code. It gives the memory that a pod and the platform use, and the CPU load and latency of the control plane. It also compares the host simulator with k3s. The Setup section names the hardware. The commands that repeat the measurements are at the end of the page.

## Setup

| | Control plane | Worker |
| --- | --- | --- |
| Board | ESP32-S3 devkit | M5Stack Cardputer (ESP32-S3FN8) |
| CPU | 2 cores at 240 MHz | 2 cores at 240 MHz |
| Memory | 512 KB SRAM, 2 MB quad PSRAM at 80 MHz | 512 KB SRAM, no PSRAM |
| Role | API server, store, controllers, kubelet | kubelet, three device drivers |

The firmware uses ESP-IDF 5.5.3 and the `-O2` build option (`CONFIG_COMPILER_OPTIMIZATION_PERF`). The image is 3,303,200 bytes in a 3,407,872-byte app slot, with 55,252 bytes of static RAM. The pods are freestanding C. Clang compiles them to Wasm with `-Oz`, and the kube32 wasm3 interpreter runs them. Both boards and the client, a laptop, use the same 2.4 GHz Wi-Fi network.

## Wasm against native code

The firmware and the bench pod (`apps/bench`) contain the same C source, `src/bench/bench_kernels.c`. The firmware runs a kernel natively on the console task (`K32_CMD:BENCH`). The pod runs the same kernel in its sandbox and times it with the node's microsecond clock. Each time is the fastest of five runs. For each kernel, the two sides return the same checksum.

A metered pod has the default execution budget, so gas accounting is on. An unmetered pod sets `kube32.io/execution-budget: "off"`.

### Cardputer

| Kernel | Native | Pod, metered | Pod, unmetered |
| --- | ---: | ---: | ---: |
| fib: recursive Fibonacci, n = 15 (calls) | 76 µs | 4,194 µs (55x) | 2,974 µs (39x) |
| crc32: bitwise CRC-32 of 4 KiB (byte loads, shifts) | 880 µs | 74,191 µs (84x) | 51,672 µs (59x) |
| sieve: sieve of Eratosthenes below 4096 (byte stores) | 458 µs | 21,630 µs (47x) | 14,826 µs (32x) |
| sort: heapsort of 1024 words (compares, swaps) | 1,057 µs | 86,463 µs (82x) | 71,573 µs (68x) |
| mandel: 32 x 32 Mandelbrot, 64 iterations (float) | 2,622 µs | 123,495 µs (47x) | 103,922 µs (40x) |
| json: 200 small JSON records (text) | 1,394 µs | 71,198 µs (51x) | 50,600 µs (36x) |

### devkit

| Kernel | Native | Pod, metered | Pod, unmetered |
| --- | ---: | ---: | ---: |
| fib: recursive Fibonacci, n = 15 (calls) | 60 µs | 4,085 µs (68x) | 3,560 µs (59x) |
| crc32: bitwise CRC-32 of 4 KiB (byte loads, shifts) | 890 µs | 84,411 µs (95x) | 72,342 µs (81x) |
| sieve: sieve of Eratosthenes below 4096 (byte stores) | 456 µs | 15,903 µs (35x) | 16,332 µs (36x) |
| sort: heapsort of 1024 words (compares, swaps) | 1,057 µs | 86,001 µs (81x) | 84,988 µs (80x) |
| mandel: 32 x 32 Mandelbrot, 64 iterations (float) | 2,962 µs | 124,576 µs (42x) | 115,538 µs (39x) |
| json: 200 small JSON records (text) | 1,752 µs | 71,822 µs (41x) | 64,361 µs (37x) |

- On the ESP32-S3, a pod runs compute 32 to 95 times slower than native code. Gas metering adds up to about 45 %.
- The location of the pod's memory changes little. The Cardputer runs pods from internal SRAM and the devkit runs them from PSRAM, but the ratios are about the same.
- The interpreter causes the cost. GCC for the ESP32-S3 makes no tail calls. Thus wasm3 dispatches each Wasm instruction with a native call and return, not with a jump.

Use a pod for control logic, protocol handling and glue code. Do not use a pod for heavy computation. Put work that needs native speed in a host function. A call from a pod to a host function costs about 1.5 µs:

| | Cardputer | devkit |
| --- | ---: | ---: |
| `esp_timer_get_time()` in firmware | 0.99 µs | 0.98 µs |
| `k32_time_ms()` from an unmetered pod (the same call through the sandbox) | 2.44 µs | 2.64 µs |
| Cost of crossing the sandbox | 1.45 µs | 1.66 µs |

The device drivers use this split. A driver reads registers over I2C and calculates the readings in Wasm. The I2C transfers, the bus lock and the multiplexer run natively.

### Native stack

Each instruction that wasm3 executes keeps a native frame of 48 to 64 bytes until its function returns. Thus a long sequence of instructions in one function uses native stack in proportion to its length.

On a board without PSRAM, the shared interpreter stack is 28 KiB. A call can use about 21.9 KB of it. A call that needs more traps with `stack overflow`, and the trap message tells how much stack the call used. These two cases occurred on the Cardputer:

- Clang `-O2` puts the query parsing and output helpers of the bench pod into one function. A request that runs `fib(1)` then used 23.2 KB and trapped. With `-Oz` and the helpers kept as separate functions, the same source answers every request.
- The BME688 driver with its compensation formulas inside `k32_tick` used 22 KB for its first reading. With each compensation step in a separate function, the driver stays well inside the limit.

For a board without PSRAM, build pods with `-Oz` and put long computations in separate functions.

## Memory

### A pod

| Pod | Linear memory | Used on the Cardputer (pod pool) | Used on the devkit |
| --- | ---: | ---: | ---: |
| bme688 driver | 3 KiB | 25,400 bytes | |
| miniscale driver | 3 KiB | 20,072 bytes | |
| rfid driver | 3 KiB | 23,536 bytes | |
| bench | 24 KiB | 42,976 bytes | 55 to 59 KB of PSRAM, 1 KB of SRAM |

A pod uses memory for its linear memory, its compiled code, its value stack and the interpreter's structures. The compiled code is about three bytes for each byte of Wasm code.

On a board without PSRAM, all of this memory comes from an 80 KiB pod pool that the node reserves at boot. The kubelet and the runtime also keep about 1.5 KB for each pod in the general heap. The Cardputer runs the three drivers above together, with 12.9 KB of the pool free.

### The platform

Free memory in bytes at each stage of boot:

| Stage | Cardputer (SRAM) | devkit (SRAM) | devkit (PSRAM) |
| --- | ---: | ---: | ---: |
| After peripherals, pod pool and interpreter stack reserved | 161,028 | 269,811 | 673,864 |
| After Wi-Fi | 124,192 | 242,003 | 669,264 |
| Cluster services up | 59,396 | 10,531 to 74,935 | 434,128 |
| Steady, with the workloads above | 22,700 to 26,000 | 18,071 | 311,332 |

On the worker, the kubelet, the pod HTTP server, event delivery and discovery use about 65 KB of SRAM. The node reserves the 80 KiB pool and the 28 KiB interpreter stack before these services start.

On the control plane, the API server, the object store, the controllers and their buffers use about 235 KB of PSRAM. The devkit's SRAM value at "cluster services up" changes from boot to boot. It depends on the requests that the API server is serving at that time.

## CPU

This table gives the busy share of the two cores over 60 s. The values come from the FreeRTOS run-time counters (`K32_CMD:TASKS`).

| Node | Busy | Largest users |
| --- | ---: | --- |
| devkit, control plane with one Deployment and three Devices | 20.8 % | Deployment controller 10.0 % of a core, TCP/IP 7.1 %, Wi-Fi 6.4 %, Device controller 4.3 % |
| Cardputer, three drivers | 18.7 % | Wi-Fi 11.2 %, pod ticks 11.1 % (rfid polls five times a second), TCP/IP 7.1 % |
| Cardputer, no pods | 8.1 % | Wi-Fi 6.5 %, TCP/IP 4.1 %, kubelet 1.5 % |

The Deployment controller reconciles every 1 s and the Device controller every 2 s. On each pass, they read the objects that they manage. This polling causes most of the control plane's idle load.

## Latency

A laptop on the same Wi-Fi network sent 15 to 30 requests of each type. The table gives the median, with the 90th percentile in brackets.

| Request | Time |
| --- | ---: |
| `GET /api/v1/pods` to the control plane | 148 ms (245 ms) |
| `kubectl get pods -A`, process start included | 211 ms (313 ms) |
| `kubectl create configmap` then `kubectl delete configmap` | 380 ms (572 ms) |
| A request to a pod and back, through the node's pod HTTP server | 37 ms (54 ms) on the devkit, 76 ms (118 ms) on the Cardputer |
| A pod from `kubectl apply` to scheduled, on the devkit | 1.1 s |
| A pod from `kubectl apply` to `Running`, on the devkit | 2.8 s |

## On a computer: kube32-sim and k3s

`kube32-sim` is the same control plane, kubelet and runtime, built for macOS or Linux. These measurements used ARM64 Linux containers (Ubuntu 24.04, 4 CPUs each) on an Apple M3 Pro. Each cluster stayed idle for three minutes before the first measurement. The second measurement ran with ten pods.

The memory value is the sum of the resident sets of the container's processes. The value in brackets is the cgroup's value. The CPU value is the share of one core over 60 s.

| | Memory | CPU |
| --- | ---: | ---: |
| kube32-sim, control plane and one worker | 6.4 MiB (6.5 MiB) | 0.33 % |
| kube32-sim, ten Wasm pods | 10.3 MiB (15.7 MiB) | 0.33 % |
| k3s v1.33.1, default install | 918 MiB (910 MiB) | 3.21 % |
| k3s v1.33.1, without Traefik, ServiceLB, metrics-server, local storage, Helm controller | 574 MiB (407 MiB) | 2.17 % |
| the same k3s with ten pause containers | 764 MiB (506 MiB) | 2.29 % |

The two systems do different work. k3s runs containers through containerd and implements all of Kubernetes. kube32 runs Wasm modules in one process and implements the subset in [the API reference](api-reference.md). The comparison shows the cost of a Kubernetes-style control plane that kube32 builds for kilobytes of memory. Natively on macOS, the simulator idles at 9.6 MiB and 0.17 % of a core.

## Repeating the measurements

To run the native kernels, send these commands on a node's serial console:

```
K32_CMD:BENCH:crc32:4:5         -> K32_BENCH kernel=crc32 n=4 runs=5 us_min=... us_total=... sum=...
K32_CMD:BENCH:hostcall:100000   -> K32_BENCH hostcall calls=100000 us=...
```

The kernels and their sizes:

| Kernel | n |
| --- | --- |
| `fib` | recursion depth |
| `crc32` | KiB of data |
| `sieve` | upper limit, at most 4096 |
| `sort` | words, at most 1024 |
| `mandel` | side of the grid |
| `json` | records |

To run the same kernels in a pod:

```sh
make -C apps/bench
tools/kube32-cli deploy images/sd/bench.wasm --name bench --node <node> --memory 24Ki --hardware none
curl -H 'X-Kube32-Pod: default/bench' 'http://<node address>:7000/run?k=crc32&n=4&r=5'
curl -H 'X-Kube32-Pod: default/bench' 'http://<node address>:7000/hostcall?n=100000'
```

For an unmetered pod, do these steps:

1. Add `--dry-run` to the `kube32-cli deploy` command and keep the manifest that it prints.
2. Add `kube32.io/execution-budget: "off"` to the pod's annotations.
3. Apply the manifest with kubectl.

To get the CPU shares, take two `K32_CMD:TASKS` listings 60 s apart and compare the `run_ms` value of each task.
