# Changelog

All notable changes to this project are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/). The project has no
tagged release yet, so everything is under Unreleased. This section covers
the work since commit `2e3bbd1` ("Deploy 3 enterprise workloads"), the point
after which the demo code was replaced by things that run on the boards.
Commit hashes are given so each entry can be checked.

## [Unreleased]

### Added

- **Tessera**, an issue tracker that runs as Wasm pods: a freestanding C core
  with its own index and arena allocator, a display and keypad HUD pod, and a
  gzipped web client served from flash (`55bb012`, `6b29a7c`, `4a8ca5b`).
- **The app ABI** (`include/runtime/k32_app_abi.h`): request pods with
  `k32_handle`, `k32_init`, `k32_tick` and `k32_event`, imports for
  responses, events, environment, display, keypad and tone, per-pod
  serialization and a gas budget per call (`55bb012`).
- **The ingress on port 80**: static assets, `/api/*` forwarding to the
  app's core pod, and server-sent events. Also an embedded image registry
  generated at build time (`55bb012`).
- **Watch streams with bookmarks and the WatchList protocol**, pod logs,
  core and `events.k8s.io` events, `metrics.k8s.io` node and pod metrics,
  and Deployments with a controller (`bf12ce9`, `022d910`).
- **k9s works**: protobuf-encoded access reviews are accepted (`83a323b`),
  and watches honour `sendInitialEvents` (`022d910`).
- **A UDP channel transport beside ESP-NOW**, with peers seeded from node
  addresses, so events cross access points (`464c402`).
- **Console commands**:
  - `K32_CMD:TASKS` (`766a7c9`), `K32_CMD:KEY` (`bf12ce9`) and
    `K32_CMD:KEYPOP` (`022d910`).
  - `K32_CMD:ACCESSLOG`, `K32_CMD:BOOTLOADER`, `K32_CMD:TASKBT` and
    `K32_CMD:TONE` (`464c402`).
  - `K32_CMD:SD` (`0565b3f`).
- **The microSD card on the Cardputer**: mount, images pulled with
  `wasm://sd/<name>`, pod volumes through `k32_fs_*`, and the `sdnote`
  example pod (`0565b3f`).
- **Tooling**: the Rust SDK (`sdk/rust`), `tools/kube32-cli` (build, check,
  deploy, embed, `sd-push`) and `docs/app-development.md` (`f03617a`).
- **Validation for `kubectl apply` and `create`.** A minimal OpenAPI v3 index
  lets them work without `--validate=false`, and `fieldValidation=Strict` is
  enforced for top-level and `metadata` fields (`f737ab6`).
- **Completed pods**: a pod whose entry point returns is reported
  `Succeeded`. A single-object `kubectl get` of a pod, node or deployment
  returns a Table (`ff03c3b`).
- **Project files**: CI (`.github/workflows/ci.yml`), `CONTRIBUTING.md`, this
  changelog, and the docs pages `docs/tessera.md` and
  `docs/workloads/gpio-blinker.md`, now about peripherals in general.

### Changed

- **Roles and board settings**: the role comes from PSRAM presence and the
  board profile from NVS, set over the console. Both used to come from
  hard-coded MAC addresses (`55bb012`).
- **Wi-Fi**: boards join an existing network as stations, and the cluster
  access point can be turned off (`55bb012`).
- **Node figures are measured**: heap, PSRAM, versions, CPU from FreeRTOS
  run-time statistics. The pod IP is now the node IP; the `10.244.0.x`
  addresses were made up (`bf12ce9`). Some constants remain; see
  `docs/api-reference.md`.
- **PodMetrics memory** now counts internal RAM governed by the pod's request.
  PSRAM is reported separately as `esp32.io/psram` (`022d910`).
- **`tools/kube32_serial_gateway.py`** is now a transparent HTTP-over-USB
  relay. It used to serve an in-memory imitation of the Kubernetes API
  (`55bb012`).
- **`scripts/flash.sh`**:
  - It paces writes only on CH340 bridges. A native USB flash takes 26 s
    instead of 192 s (`022d910`).
  - It resets native USB boards through the watchdog (`f54968a`).
- **Socket limits**: lwIP sockets and active TCP connections raised from 16
  to 32 (`8eaf359`).
- **Tessera copy and seed data** no longer mention the boards, the runtime or
  Kubernetes (`4a8ca5b`, `e9e80f7`).
- **Documentation rewritten to match the code.** README, quick start,
  hardware, flashing and console, architecture, API coverage and tools were
  rewritten. Statements the code does not support were removed; the
  "Removed" list below names them.

### Fixed

- **USB console stops answering**: console output now goes through the
  USB-Serial-JTAG driver. Before, `printf` raced the driver and the port went
  silent until a power cycle (`f54968a`).
- **SD mount cost**: FatFs sector buffers are sized for 512-byte sectors. The
  mount had cost 28 KB of internal RAM on the Cardputer (`f54968a`).
- **Control-plane reboot on allocation failure**: telemetry parses pods once,
  and the housekeeping loop survives a failed allocation (`f54968a`).
- **Chunked responses stalled on Nagle's algorithm**: the board view (50 KB)
  took 1.1 s (`766a7c9`).
- **Native stack guard trips** while seeding (`766a7c9`).
- **A worker that registered before DHCP** advertised `0.0.0.0` (`8eaf359`).
- **New connections dropped without a reset** once 16 sockets were in use
  (`8eaf359`).
- **HUD shown `Running` without a thread.** It now gets a stack reserved at
  boot, and a pod whose thread cannot start is reported `Failed` (`83a323b`).
- **k9s showed "No resources found"** without the WatchList bookmark, and
  2700 % pod memory (`022d910`).
- **The I2S speaker hummed endlessly** after a tone (`55bb012`).

### Removed

- **The scripted "enterprise workloads" (`55bb012`).** These were "Vortex DAW
  Synth", "Aether Workspace" and "Aegis Fleet Operations",
  `tools/kube32_workbench_app.py`, and the `web/` dashboards. They ran in
  Python on the developer's machine behind an imitation API, not on the
  boards.
- **Fakes in the firmware and tools (`55bb012`).** The kubelet deployed an
  8-byte Wasm header instead of a module, and the firmware printed scripted
  "pipeline" and "inference" lines. Also removed: the MAC-keyed workbench
  manifests, `tools/kube32_mesh_controller.py`,
  `tools/rpi_edge_telemetry.py` and `tools/autoflash_node.py`.
- **Bounded diagnostic probes** (`[STACK]`, `[TRACE]`, `[DIAG]`) once the
  keypad path was verified (`c7f0139`).
- **Example manifests that named images which were never built** (this
  change):
  - `examples/workloads/` (ten "mission-critical industrial" pods);
  - `examples/gpio-blinker.yaml`;
  - `examples/distributed-pipeline.yaml`.

  `examples/oversized-pod.yaml` now uses the embedded echo image.
- **Docs pages describing behaviour the code does not have** (this change):
  `docs/workloads/distributed-pipeline.md` (a "TinyML wake word" pipeline
  with invented log output and MAC address) and
  `docs/workloads/resource-limits.md`. Removed claims from other pages:
  - "47 KB RAM", "1.59 MB firmware", "under 15 ms cold start" and "0 heap
    allocations" metrics;
  - an ESP-NOW latency under 3 ms;
  - CRC32 journaling to LittleFS on the boards;
  - `GPIO_WRITE`/`ADC_READ` capabilities;
  - pin admission failures reported as `CrashLoopBackOff` (they are
    `CapabilityAdmissionFailed`);
  - CLI output from a CLI that no longer worked.
- **Dead tools and scripts** (this change):
  - `cli/kube32-cli`: it spoke a `K32_REQ` console protocol the firmware no
    longer has.
  - `tools/kube32_port_forward.py`: it imported the deleted workbench module.
  - `tools/openapi_*.json` and `tools/spdy_dict.bin`: data for the old
    imitation gateway.
  - `scripts/benchmark_suite.py`: it measured a `/pipeline/transmit`
    endpoint that no longer exists.
  - `scripts/run_live_stress_suite.py`: it "migrated" pods by rewriting
    `nodeName` and made-up pod IPs.
  - `scripts/deploy_all_workloads.py`, `scripts/visual_review.py`,
    `scripts/flash_dual_boards.py`, `scripts/flash_hardware.sh` and
    `scripts/wait_for_boards.py`.
  - `tests/test_hw_serial_empirical_challenge.py`: it asserted log lines the
    firmware never prints, among them a "WAKE_WORD_DETECTED (confidence:
    99.1%)" result.
