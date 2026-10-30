# Changelog

All notable changes to this project will be documented in this file. The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- Kubernetes API server on port 6443 serving Pod, Node, Namespace, Service, Endpoints, EndpointSlice, ConfigMap, Secret, Event, Deployment, DaemonSet, Job, Lease, Ingress, NetworkPolicy, NodeMetrics, PodMetrics, and subject access reviews.
- Pod egress: `k32_http_call` reaches HTTP hosts outside the cluster by IPv4 address or DNS name, resolving Service names as Kubernetes DNS does; NetworkPolicy egress rules (`ipBlock` with `except`, pod and namespace selectors, numbered, named and ranged ports) restrict Service and outside calls on the calling node; the control plane routes its cluster access point to its station uplink with NAT and offers the uplink's name server.
- Watch streams with bookmarks and `sendInitialEvents`, Table output, OpenAPI v3 index, `fieldValidation=Strict`, `dryRun=All`, pagination, merge and strategic-merge patches, and protobuf request bodies for ConfigMap, Secret, Namespace, Service, Deployment, and Job.
- Pod scheduler filtering by node state, `nodeName`, `nodeSelector`, memory, `esp32.io/psram`, and peripheral counts (`esp32.io/gpio`, `esp32.io/i2c`, `esp32.io/adc`, `esp32.io/uart`).
- Deployment, DaemonSet, and Job controllers, plus node Lease tracking with automatic tainting and pod eviction.
- wasm3 container runtime supporting request pods (`k32_alloc`, `k32_handle`, `k32_init`, `k32_tick`, `k32_event`), static asset serving with gzip negotiation (`k32_resp_asset`), and entry-point pods (`_start`, `app_main`, `main`).
- Per-pod linear memory limits across internal RAM and PSRAM (including sub-page limits below 64 KiB), configurable wasm3 value stacks (`kube32.io/wasm-stack`) and gas execution budgets (`kube32.io/execution-budget`), pool-first allocation on boards without PSRAM, one shared interpreter stack for all pod calls, trap-specific exit codes, and `Always`, `OnFailure`, and `Never` restart policies with exponential back-off.
- Pod environment variable injection from ConfigMaps, Secrets, `fieldRef`, and service links, with `$(VAR)` expansion.
- Kubelet memory admission: a pod that does not fit yet waits `Pending` with reason `InsufficientMemory`, and one larger than the node can hold fails with `OutOfmemory`, each with an Event. Pod allocations never take the system reserve of the internal heap (24 KiB on boards without PSRAM).
- Port 80 Ingress controller with host and path matching, Service backend routing across nodes, `StaticBundle` asset serving, and Server-Sent Events streaming (`kube32.io/event-stream`).
- Inter-pod event pub/sub (`k32_event_emit`, `kube32.io/event-topics`) over ESP-NOW and UDP port 7400, plus synchronous pod-to-pod HTTP calls (`k32_http_call`) gated by the `SERVICE_CALL` capability.
- Single firmware image with automatic control-plane and worker role detection from PSRAM presence, configurable over the serial console (`K32_CMD:*`) and persisted in NVS.
- Log-structured, CRC-checked object store journal on a raw flash partition or microSD card with pre-write capacity checks, object-store-full errors, and API writes committed before the response, persistent volume claims via the `k32_fs_*` ABI, rotated container logs, and network storage access (`GET`, `PUT`, `DELETE`) via the node proxy subresource (`/api/v1/nodes/<node>/proxy/storage/<path>`).
- Optional bearer token authentication (`K32_CMD:TOKEN`) and self-signed ECDSA P-256 TLS (`K32_CMD:CERT`) with HTTP/1.1 keep-alive multiplexed with plain HTTP on port 6443.
- Capability firewall and `generic` and `cardputer` board profiles governing GPIO, ADC, I2C, UART, display, keypad, speaker, IPC, and microSD resources.
- Rust guest SDK (`sdk/rust`), `tools/kube32-cli` (`build`, `check`, `deploy`, `embed`, `sd-push`, and recursive file/directory `push`), USB serial HTTP gateway with concurrent streamed requests (`tools/kube32_serial_gateway.py`), flashing script (`scripts/flash.sh`), and host simulator (`kube32-sim`, supporting `--psram 0` for boards without PSRAM, `--tls`, and `--help`).
- Example workloads and manifests including the four-pod Tessera issue tracker (`apps/tessera`), `echo`, and `i2cscan`.
- Infrared for pods: RMT transmit and receive channels (`esp32.io/rmt-tx`, `esp32.io/rmt-rx`) with pins and carrier from `kube32.io/rmt-tx` and `kube32.io/rmt-rx` annotations, `esp32_rmt_send` and the non-blocking `esp32_rmt_receive` with a per-channel frame queue, node capacity and scheduling, the Cardputer's IR LED as the default TX pin, Rust SDK bindings, and the `irremote` example that decodes and sends NEC codes.
- I2C device discovery: nodes label themselves `i2c.kube32.io/0x<address>: bus-<n>` for devices detected on their I2C pins so `nodeSelector` can place pods on nodes with matching hardware, and the `rfid` example reads ISO 14443-A tags through an MFRC522-compatible reader.
- `kube32.io/tick-interval` configures a pod's `k32_tick` period.
- Firmware rollouts through the cluster: an A/B partition table with bootloader rollback, `kube32-cli firmware push` staging an image on the control plane in resumable ranges (over Wi-Fi or the USB serial gateway), and `kube32.io/v1` `FirmwareRollout` updating workers one at a time and the control plane last. Each node downloads the image in small ranges, checks its SHA-256, reboots into it, and confirms it once healthy. An image that resets or fails to report Ready rolls back, and the rollout stops at the first failure. Nodes report their firmware version in `kubectl get nodes -o wide`, firmware annotations, a `FirmwareUpdate` condition, and Events.
- Each control plane generates a cluster access point password, and `K32_CMD:APPASS` configures it on workers.
- Python integration test suites (`tests/integration/runner.py`) for API, validation, resources, workloads, cluster, and federation in CI.

### Changed

- Pods use less of the pod pool on boards without PSRAM. Without `kube32.io/wasm-stack`, the wasm3 value stack sizes to what the module's call chains need, up to the 4 KiB or 16 KiB default. The shared interpreter stack lends the compiler its scratch space while a module loads. Function constants follow their code on its code pages, and code pages shrink to fit the pool block's free pieces. Metering skips zero-cost charges, and the loader drops function names from the name section.
- Memory admission estimates a pod's size from its module's sections and call chains using wasm3's ESP32-S3 structure sizes in both the simulator and the board. The `[POD]` load line reports the bytes a pod took from the pool block, the block's free bytes, and its largest free piece.

### Fixed

- Stop the ESP-IDF I2C master driver from logging every failed transfer on a pod's I2C bus. The HAL reports the first three failures and every 100th.
- Modules that run out of memory while wasm3 grows its function, global, tag, memory, or table arrays now fail to load instead of crashing the node.
- Pods leave a PSRAM reserve for the system (128 KiB on a control plane, 64 KiB on a worker with PSRAM). Without this reserve, pod code and structures can drop free PSRAM to 16 KiB, causing API requests to fail with 503 and firmware staging to run the control plane out of memory.
- The USB console survives software resets: on any non-power-on reset, the board disconnects from the USB bus for 100 ms so the host enumerates the port again, and it flushes any partial line from the early console before its driver starts. Previously, a reboot could leave the port silent until the board is unplugged.
- Fix loading inline and SD card images placed in PSRAM: the ESP32-S3 maps flash and PSRAM into a single address range, which caused the loader to treat the pod's PSRAM copy as flash and free it.
- Include object metadata labels in Table rows so `kubectl get --show-labels` and `-L` display them.
- Clear the I2C bus after a failed transfer.
