# kube32

[![CI](https://github.com/arterialist/kube32/actions/workflows/ci.yml/badge.svg)](https://github.com/arterialist/kube32/actions/workflows/ci.yml)
[![Docs](https://github.com/arterialist/kube32/actions/workflows/docs.yaml/badge.svg)](https://github.com/arterialist/kube32/actions/workflows/docs.yaml)

kube32 is a Kubernetes-compatible control plane and WebAssembly pod runtime for ESP32-S3 microcontrollers.

`kubectl` and k9s manage a cluster of boards over Wi-Fi or USB serial. Pods are WebAssembly modules executed by the [wasm3](https://github.com/wasm3/wasm3) interpreter. One firmware image runs on every board. A board with PSRAM acts as the control plane and runs a local kubelet, while boards without PSRAM join as workers. The same sources build `kube32-sim`, a host simulator for macOS and Linux.

## Features

- Kubernetes API serving Pod, Node, Namespace, Service, Endpoints, EndpointSlice, Ingress, Deployment, DaemonSet, Job, ConfigMap, Secret, Lease, Event, and metrics for `kubectl` and k9s, including watch streams, Table output, OpenAPI v3 validation, and log tailing.
- WebAssembly pods in [wasm3](https://github.com/wasm3/wasm3) with SDKs and templates for Rust, C, and AssemblyScript, using either a request-handler ABI with `k32_handle` or an entry-point ABI with `app_main`, plus configurable memory limits and per-call gas budgets.
- Service routing, HTTP Ingress with host and prefix rules, Server-Sent Events streams, embedded static asset bundles via `k32_resp_asset` and `StaticBundle`, and synchronous pod-to-pod HTTP calls via `k32_http_call`.
- Persistent object store backed by a CRC-checked journal on a raw flash partition or the microSD card, plus `persistentVolumeClaim` pod volumes via `k32_fs_*` and rotated container logs for `kubectl logs --previous`.
- Peripheral access gated per pod by a capability firewall covering GPIO pins, ADC channels, I2C and UART buses, ST7789 display, key matrix, I2S speaker, and microSD storage.
- Devices: sensors and switches as API objects. Nodes find and identify I2C parts, also behind a PaHub multiplexer. The control plane runs each part's driver on the node that has the part. Apps on any node read the part through a Service, also from a laptop that runs `kube32-sim`.
- AI agents: `kube32 mcp` is a Model Context Protocol server. Claude Code and other agents list, read and describe Devices, send them actions within the limits their owner declares, and build and deploy the pods they write. See [docs/mcp.md](docs/mcp.md).
- Pod migration: a running pod moves to another node with its memory and clock, on `kube32 move`, `kubectl drain`, memory pressure or a faster node, and restores from a periodic checkpoint when its node is lost. See [docs/migration.md](docs/migration.md).
- Container lifecycle management with `restartPolicy` set to `Always`, `OnFailure`, or `Never`, exponential CrashLoopBackOff, and distinct exit codes for Wasm traps, gas exhaustion, and `OOMKilled`.
- Optional bearer-token authentication and automatic self-signed ECDSA P-256 TLS on port 6443.
- Firmware rollouts through the cluster: one node at a time, SHA-256 checked, with bootloader rollback when an image does not come up healthy.
- Pub/sub events via `k32_event_emit` and `kube32.io/event-topics`, bridged across nodes over ESP-NOW and UDP port 7400.
- Multi-node host simulator `kube32-sim`, and the `kube32` command for flashing boards, running the simulator, and creating, building, checking, and deploying pods over the network or to node storage.

## Hardware

| Role | Requirements | Reference board |
| --- | --- | --- |
| Control plane | ESP32-S3, 8 MB flash, 2 MB or more quad SPI PSRAM | Generic ESP32-S3 devkit with 2 MB PSRAM |
| Worker | Any ESP32-S3 with 8 MB flash | M5Stack Cardputer with ST7789 display, keypad, speaker, and microSD |

See [docs/hardware-spec.md](docs/hardware-spec.md) for pin assignments and memory budgets.

## Performance

Measured on an ESP32-S3 devkit (control plane, 2 MB PSRAM) and an M5Stack Cardputer (worker, no PSRAM), both at 240 MHz. [docs/benchmarks.md](docs/benchmarks.md) has the details and the commands to repeat them.

| Measurement | Result |
| --- | --- |
| Compute in a pod against the same C code run natively | 32 to 95 times slower (wasm3 interpreter) |
| A call from a pod into a host function | about 1.5 µs |
| A sensor driver pod on the worker | 20 to 25 KB of its 80 KiB pod pool |
| The worker's platform services | about 65 KB of SRAM |
| `GET /api/v1/pods` over Wi-Fi | 148 ms median |
| A pod from `kubectl apply` to `Running` | 2.8 s |
| `kube32-sim` with a worker on ARM64 Linux, idle | 6.4 MiB, 0.33 % of a core (k3s v1.33.1: 574 to 918 MiB, 2.2 to 3.2 %) |

Use pods for control logic, I/O and glue code. Put heavy computation in native host functions.

## Install

- **Boards**: install the latest release from the browser at [arterialist.github.io/kube32/flash](https://arterialist.github.io/kube32/flash/), which also sets up the board's Wi-Fi.
- **Simulator**: run the container image, then point `kubectl` at `http://127.0.0.1:6443`:

  ```sh
  docker run --rm -p 6443:6443 -p 8080:8080 ghcr.io/arterialist/kube32-sim
  ```

  Each [release](https://github.com/arterialist/kube32/releases) also has `kube32-sim` for macOS and Linux.
- **SDKs**: `cargo add kube32-sdk` for Rust pods, and the `kube32-c-sdk` archive of each release for C pods.

See [docs/releases.md](docs/releases.md) for every release asset.

## Quick start

Install the `kube32` command with [uv](https://docs.astral.sh/uv/) ([Install](docs/install.md)). It flashes boards, runs the simulator, and builds and deploys pods. `kubectl` manages the cluster.

```sh
uv tool install kube32
```

### Simulator

Start a two-node cluster on macOS or Linux:

```sh
kube32 sim --ingress-port 8080 --pod-port 17000
```

In a second terminal, configure `kubectl`:

```sh
kubectl config set-cluster kube32-sim --server=http://127.0.0.1:6443
kubectl config set-context kube32-sim --cluster=kube32-sim
kubectl config use-context kube32-sim
kubectl get nodes
```

Create an app, build it, deploy it, and call it through the pod port:

```sh
kube32 new hello --lang c
kube32 build hello
kube32 deploy hello/build/hello.wasm
curl -H 'X-Kube32-Pod: default/hello' http://127.0.0.1:17000/
```

### Boards

Flash each board over USB, then open its serial console:

```sh
kube32 flash
kube32 console
```

Provision Wi-Fi and the board profile on the console:

```text
K32_CMD:BOARD:cardputer          # Cardputer worker only
K32_CMD:WIFI:<ssid>:<password>   # both boards
K32_CMD:AP:off                   # control plane
K32_CMD:API:<control-plane-ip>   # worker
```

Point `kubectl` at the control plane IP:

```sh
kubectl config set-cluster kube32 --server=http://<control-plane-ip>:6443
kubectl config set-context kube32 --cluster=kube32
kubectl config use-context kube32
kubectl get nodes
```

See [docs/quickstart.md](docs/quickstart.md) for full setup instructions and TLS configuration.

### From source

Build the simulator with CMake and the firmware with PlatformIO:

```sh
cmake -S . -B build && cmake --build build -j8
./build/kube32-sim --ingress-port 8080 --pod-port 17000
pio run -e esp32s3 && bash scripts/flash.sh <serial-port> 921600
```

In a checkout, `tools/kube32-cli` runs the `kube32` commands. See [CONTRIBUTING.md](CONTRIBUTING.md).

## Example: Tessera

Tessera is an issue tracker made of four WebAssembly pods routed by an Ingress. `tessera-web` and `tessera-api` run on the control plane to serve the web UI and in-memory search index. `tessera-db` and `tessera-hud` run on the Cardputer worker to persist records to an append-only log on the microSD card and drive the LCD and keypad (`tessera-hud` is silent by default; sound is opt-in via `HUD_SOUND=1` and the speaker capability).

Generate the sample dataset, upload it to the worker's microSD card, and deploy:

```sh
make -C apps/tessera dataset
kube32 push apps/tessera/build/card/kube32/volumes/tessera-db --node <worker-node> --path volumes/tessera-db
kubectl apply -f examples/tessera.yaml
```

Open `http://<control-plane-ip>/` or `http://127.0.0.1:8080/` on the simulator, and sign in as `mara` with password `demo`.

## Documentation

Full documentation is published at [https://arterialist.github.io/kube32/](https://arterialist.github.io/kube32/).

- [Install](docs/install.md)
- [Quick start](docs/quickstart.md)
- [Install from the browser](docs/flash.md)
- [Hardware specification](docs/hardware-spec.md)
- [Flashing and the serial console](docs/flashing-troubleshooting.md)
- [Writing app pods](docs/app-development.md)
- [Cluster access and authentication](docs/cluster-access.md)
- [Services, Ingress and pod-to-pod calls](docs/networking.md)
- [Storage and volumes](docs/storage.md)
- [Peripherals](docs/peripherals.md)
- [Devices](docs/devices.md)
- [MCP server for agents](docs/mcp.md)
- [Pod migration](docs/migration.md)
- [Benchmarks](docs/benchmarks.md)
- [Example: Tessera](docs/examples/tessera.md)
- [Architecture overview](docs/architecture/index.md)
- [Kubernetes API coverage](docs/api-reference.md)
- [CLI and tools reference](docs/cli-reference.md)
- [Releases](docs/releases.md)

## Contributing

Contributions are welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md) for build instructions, test suites, and coding guidelines, or open an issue on [GitHub](https://github.com/arterialist/kube32).

## Security

See [SECURITY.md](SECURITY.md) for the deployment security model and instructions on reporting vulnerabilities privately.

## License

Apache License 2.0. See [LICENSE](LICENSE). `third_party/wasm3` is licensed under the MIT License in `third_party/wasm3/LICENSE`. The BME688 compensation in `apps/drivers/bme688` comes from the Bosch Sensortec BME68x Sensor API under the BSD-3-Clause license. The notice is in its source file.
