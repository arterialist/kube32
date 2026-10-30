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
- Python integration test suites (`tests/integration/runner.py`) for API, validation, resources, workloads, cluster, and federation in CI.
